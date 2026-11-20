#include "ancs.h"

#include <NimBLEDevice.h>
#include "nimble/nimble/host/include/host/ble_gatt.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "messages.h"
#include "textfont.h"

// Apple Notification Center Service, client side on the iPhone's (incoming, encrypted) link.
// Spec: developer.apple.com "Apple Notification Center Service (ANCS) Specification".
// GAP / GATT callbacks run on the NimBLE host task; finished notifications go to the loop task
// through a queue and are rendered there.
namespace ancs {
namespace {

// 7905F431-B5CE-4E99-A40F-4B1E122D00D0 and its characteristics (bytes little-endian)
const ble_uuid128_t kService = BLE_UUID128_INIT(0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4, 0x99, 0x4E, 0xCE, 0xB5,
    0x31, 0xF4, 0x05, 0x79);
const ble_uuid128_t kNotificationSource = BLE_UUID128_INIT(0xBD, 0x1D, 0xA2, 0x99, 0xE6, 0x25, 0x58, 0x8C, 0xD9, 0x42,
    0x01, 0x63, 0x0D, 0x12, 0xBF, 0x9F);
const ble_uuid128_t kControlPoint = BLE_UUID128_INIT(0xD9, 0xD9, 0xAA, 0xFD, 0xBD, 0x9B, 0x21, 0x98, 0xA8, 0x49, 0xE1,
    0x45, 0xF3, 0xD8, 0xD1, 0x69);
const ble_uuid128_t kDataSource = BLE_UUID128_INIT(0xFB, 0x7B, 0x7C, 0xCE, 0x6A, 0xB3, 0x44, 0xBE, 0xB5, 0x4B, 0xD6, 0x24,
    0xE9, 0xC6, 0xEA, 0x22);
const ble_uuid16_t kCccd = BLE_UUID16_INIT(0x2902);

constexpr std::uint16_t kNone = 0xFFFF;
constexpr std::uint8_t kCmdNotificationAttributes = 0;
constexpr std::uint8_t kCmdAppAttributes = 1;
constexpr std::uint8_t kAttrAppId = 0;
constexpr std::uint8_t kAttrTitle = 1;
constexpr std::uint8_t kAttrMessage = 3;
constexpr std::uint8_t kAttrDate = 5;
constexpr std::uint8_t kFlagPreExisting = 0x04;

struct Item {                          // one finished notification, host task -> loop task
    char app[40];
    char appId[64];
    char title[72];
    char message[140];
    char time[6];
    std::uint8_t category;
};
QueueHandle_t items = nullptr;

// ---- link state (host task) ----
struct Peer {
    std::uint16_t conn = kNone;
    std::uint16_t start = 0;
    std::uint16_t end = 0;
    std::uint16_t ns = 0;              // value handles
    std::uint16_t cp = 0;
    std::uint16_t ds = 0;
    std::uint16_t nsDef = 0;           // declaration handles (to bound descriptor discovery)
    std::uint16_t dsDef = 0;
    std::uint16_t nextDef[2] = {0, 0}; // first declaration after ns / ds
    std::uint16_t nsCccd = 0;
    std::uint16_t dsCccd = 0;
    bool subscribed = false;
};
Peer peer;
volatile bool linkReady = false;
volatile std::uint32_t receivedCount = 0;
char lastError[48] = "";

// ---- request queue (host task) ----
constexpr int kUidQueue = 8;
std::uint32_t uidQueue[kUidQueue];
std::uint8_t categoryQueue[kUidQueue];
int uidHead = 0;
int uidCount = 0;
volatile bool busy = false;
volatile std::uint32_t busySinceMs = 0;
std::uint8_t pendingCategory = 0;

// Data Source reassembly
std::uint8_t response[700];
int responseLength = 0;
Item current;                          // notification attributes waiting for the app name

constexpr int kAppCache = 8;
struct AppName {
    char id[64];
    char name[40];
};
AppName appCache[kAppCache];
int appCacheNext = 0;

void setError(const char* text) {
    snprintf(lastError, sizeof(lastError), "%s", text);
}

void resetPeer() {
    peer = Peer();
    linkReady = false;
    busy = false;
    uidCount = 0;
    responseLength = 0;
}

const char* cachedName(const char* id) {
    for (int i = 0; i < kAppCache; ++i) {
        if (appCache[i].id[0] != '\0' && strcmp(appCache[i].id, id) == 0) {
            return appCache[i].name;
        }
    }
    return nullptr;
}

void cacheName(const char* id, const char* name) {
    AppName& slot = appCache[appCacheNext];
    appCacheNext = (appCacheNext + 1) % kAppCache;
    snprintf(slot.id, sizeof(slot.id), "%s", id);
    snprintf(slot.name, sizeof(slot.name), "%s", name);
}

void finish() {
    if (items != nullptr) {
        xQueueSend(items, &current, 0);
    }
    ++receivedCount;
    busy = false;
}

void requestNext();

int onWritten(std::uint16_t conn, const ble_gatt_error* error, ble_gatt_attr* attr, void* arg) {
    (void)conn;
    (void)attr;
    (void)arg;
    if (error->status != 0) {
        setError("control point write failed");
        busy = false;
    }
    return 0;
}

// Asks for title / message / date of the oldest queued notification.
void requestNext() {
    if (busy || uidCount == 0 || !linkReady) {
        return;
    }
    const std::uint32_t uid = uidQueue[uidHead];
    pendingCategory = categoryQueue[uidHead];
    uidHead = (uidHead + 1) % kUidQueue;
    --uidCount;
    const std::uint8_t request[] = {
        kCmdNotificationAttributes,
        static_cast<std::uint8_t>(uid), static_cast<std::uint8_t>(uid >> 8),
        static_cast<std::uint8_t>(uid >> 16), static_cast<std::uint8_t>(uid >> 24),
        kAttrAppId,
        kAttrTitle, 64, 0,
        kAttrMessage, 128, 0,
        kAttrDate,
    };
    busy = true;
    busySinceMs = millis();
    responseLength = 0;
    if (ble_gattc_write_flat(peer.conn, peer.cp, request, sizeof(request), onWritten, nullptr) != 0) {
        busy = false;
        setError("control point busy");
    }
}

void requestAppName(const char* appId) {
    std::uint8_t request[72];
    const std::size_t idLength = strnlen(appId, sizeof(current.appId) - 1);
    request[0] = kCmdAppAttributes;
    memcpy(request + 1, appId, idLength);
    request[1 + idLength] = 0;
    request[2 + idLength] = 0;          // AppAttributeIDDisplayName
    responseLength = 0;
    busySinceMs = millis();
    if (ble_gattc_write_flat(peer.conn, peer.cp, request, static_cast<std::uint16_t>(idLength + 3), onWritten, nullptr) != 0) {
        snprintf(current.app, sizeof(current.app), "%s", appId);
        finish();
    }
}

void copyField(char* out, std::size_t size, const std::uint8_t* data, int length) {
    const std::size_t n = std::min(static_cast<std::size_t>(length), size - 1);
    memcpy(out, data, n);
    out[n] = '\0';
}

// Tries to parse the Data Source bytes received so far; returns true when a full response was used.
bool parseResponse() {
    if (responseLength < 1) {
        return false;
    }
    if (response[0] == kCmdNotificationAttributes) {
        if (responseLength < 5) {
            return false;
        }
        int p = 5;
        Item item;
        memset(&item, 0, sizeof(item));
        int found = 0;
        while (found < 4) {
            if (p + 3 > responseLength) {
                return false;                 // wait for more notifications
            }
            const std::uint8_t id = response[p];
            const int length = response[p + 1] | (response[p + 2] << 8);
            if (p + 3 + length > responseLength) {
                return false;
            }
            const std::uint8_t* data = response + p + 3;
            if (id == kAttrAppId) {
                copyField(item.appId, sizeof(item.appId), data, length);
            } else if (id == kAttrTitle) {
                copyField(item.title, sizeof(item.title), data, length);
            } else if (id == kAttrMessage) {
                copyField(item.message, sizeof(item.message), data, length);
            } else if (id == kAttrDate && length >= 13) {   // yyyyMMdd'T'HHmmSS
                item.time[0] = static_cast<char>(data[9]);
                item.time[1] = static_cast<char>(data[10]);
                item.time[2] = ':';
                item.time[3] = static_cast<char>(data[11]);
                item.time[4] = static_cast<char>(data[12]);
                item.time[5] = '\0';
            }
            p += 3 + length;
            ++found;
        }
        item.category = pendingCategory;
        current = item;
        responseLength = 0;
        const char* name = cachedName(current.appId);
        if (name != nullptr) {
            snprintf(current.app, sizeof(current.app), "%s", name);
            finish();
        } else {
            requestAppName(current.appId);
        }
        return true;
    }
    if (response[0] == kCmdAppAttributes) {
        int p = 1;
        while (p < responseLength && response[p] != 0) {
            ++p;
        }
        if (p + 4 > responseLength) {
            return false;
        }
        const int length = response[p + 2] | (response[p + 3] << 8);
        if (p + 4 + length > responseLength) {
            return false;
        }
        char name[40];
        copyField(name, sizeof(name), response + p + 4, length);
        if (name[0] == '\0') {
            snprintf(name, sizeof(name), "%s", current.appId);
        }
        cacheName(current.appId, name);
        snprintf(current.app, sizeof(current.app), "%s", name);
        responseLength = 0;
        finish();
        return true;
    }
    responseLength = 0;                       // unknown command id: drop
    return false;
}

// ---- discovery ----
int onCccdWritten(std::uint16_t conn, const ble_gatt_error* error, ble_gatt_attr* attr, void* arg);

void subscribe(std::uint16_t cccd) {
    static const std::uint8_t kNotify[2] = {0x01, 0x00};
    if (ble_gattc_write_flat(peer.conn, cccd, kNotify, sizeof(kNotify), onCccdWritten, nullptr) != 0) {
        setError("subscribe failed");
    }
}

// Data Source first (so no response is lost), then Notification Source.
int onCccdWritten(std::uint16_t conn, const ble_gatt_error* error, ble_gatt_attr* attr, void* arg) {
    (void)arg;
    if (conn != peer.conn) {
        return 0;
    }
    if (error->status != 0) {
        setError(error->status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN) ? "not authorised on iPhone"
                                                                                : "subscribe rejected");
        return 0;
    }
    if (attr != nullptr && attr->handle == peer.dsCccd) {
        subscribe(peer.nsCccd);
    } else {
        peer.subscribed = true;
        linkReady = true;
        setError("");
    }
    return 0;
}

int onDescriptor(std::uint16_t conn, const ble_gatt_error* error, std::uint16_t chrValHandle, const ble_gatt_dsc* dsc,
    void* arg) {
    const int which = static_cast<int>(reinterpret_cast<std::intptr_t>(arg));   // 0 ds, 1 ns
    if (conn != peer.conn) {
        return 0;
    }
    if (error->status == 0 && dsc != nullptr) {
        if (ble_uuid_cmp(&dsc->uuid.u, &kCccd.u) == 0) {
            (which == 0 ? peer.dsCccd : peer.nsCccd) = dsc->handle;
        }
        return 0;
    }
    if (error->status != BLE_HS_EDONE) {
        setError("descriptor discovery failed");
        return 0;
    }
    if (which == 0) {
        const std::uint16_t end = peer.nextDef[1] ? peer.nextDef[1] - 1 : peer.end;
        ble_gattc_disc_all_dscs(peer.conn, peer.ns, end, onDescriptor, reinterpret_cast<void*>(1));
    } else {
        if (peer.dsCccd == 0) {
            peer.dsCccd = static_cast<std::uint16_t>(peer.ds + 1);       // usual layout
        }
        if (peer.nsCccd == 0) {
            peer.nsCccd = static_cast<std::uint16_t>(peer.ns + 1);
        }
        subscribe(peer.dsCccd);
    }
    (void)chrValHandle;
    return 0;
}

int onCharacteristic(std::uint16_t conn, const ble_gatt_error* error, const ble_gatt_chr* chr, void* arg) {
    (void)arg;
    if (conn != peer.conn) {
        return 0;
    }
    if (error->status == 0 && chr != nullptr) {
        // the first declaration after ns / ds bounds their descriptor ranges
        if (peer.dsDef && !peer.nextDef[0] && chr->def_handle > peer.dsDef) {
            peer.nextDef[0] = chr->def_handle;
        }
        if (peer.nsDef && !peer.nextDef[1] && chr->def_handle > peer.nsDef) {
            peer.nextDef[1] = chr->def_handle;
        }
        if (ble_uuid_cmp(&chr->uuid.u, &kNotificationSource.u) == 0) {
            peer.ns = chr->val_handle;
            peer.nsDef = chr->def_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &kControlPoint.u) == 0) {
            peer.cp = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &kDataSource.u) == 0) {
            peer.ds = chr->val_handle;
            peer.dsDef = chr->def_handle;
        }
        return 0;
    }
    if (error->status != BLE_HS_EDONE) {
        setError("characteristic discovery failed");
        return 0;
    }
    if (peer.ns == 0 || peer.cp == 0 || peer.ds == 0) {
        setError("ANCS characteristics missing");
        return 0;
    }
    const std::uint16_t end = peer.nextDef[0] ? peer.nextDef[0] - 1 : peer.end;
    ble_gattc_disc_all_dscs(peer.conn, peer.ds, end, onDescriptor, reinterpret_cast<void*>(0));
    return 0;
}

int onService(std::uint16_t conn, const ble_gatt_error* error, const ble_gatt_svc* service, void* arg) {
    (void)arg;
    if (conn != peer.conn) {
        return 0;
    }
    if (error->status == 0 && service != nullptr) {
        peer.start = service->start_handle;
        peer.end = service->end_handle;
        return 0;
    }
    if (error->status == BLE_HS_EDONE && peer.start != 0) {
        ble_gattc_disc_all_chrs(peer.conn, peer.start, peer.end, onCharacteristic, nullptr);
    } else if (peer.start == 0) {
        peer.conn = kNone;           // not an iPhone (or ANCS not offered): free for the next peer
    }
    return 0;
}

void onNotificationSource(const std::uint8_t* data, int length) {
    if (length < 8) {
        return;
    }
    const std::uint8_t event = data[0];
    const std::uint8_t flags = data[1];
    if (event != 0 || (flags & kFlagPreExisting) != 0) {   // added, and not one that was already there
        return;
    }
    const std::uint32_t uid = data[4] | (data[5] << 8) | (static_cast<std::uint32_t>(data[6]) << 16)
        | (static_cast<std::uint32_t>(data[7]) << 24);
    if (uidCount == kUidQueue) {             // drop the oldest
        uidHead = (uidHead + 1) % kUidQueue;
        --uidCount;
    }
    const int slot = (uidHead + uidCount) % kUidQueue;
    uidQueue[slot] = uid;
    categoryQueue[slot] = data[2];
    ++uidCount;
    requestNext();
}

int gapEvent(ble_gap_event* event, void* arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_ENC_CHANGE:
            if (event->enc_change.status == 0 && peer.conn == kNone) {
                resetPeer();
                peer.conn = event->enc_change.conn_handle;
                ble_gattc_disc_svc_by_uuid(peer.conn, &kService.u, onService, nullptr);
            }
            break;
        case BLE_GAP_EVENT_NOTIFY_RX: {
            if (event->notify_rx.conn_handle != peer.conn || event->notify_rx.om == nullptr) {
                break;
            }
            const int length = OS_MBUF_PKTLEN(event->notify_rx.om);
            if (event->notify_rx.attr_handle == peer.ns) {
                std::uint8_t data[8];
                if (length >= 8 && os_mbuf_copydata(event->notify_rx.om, 0, 8, data) == 0) {
                    onNotificationSource(data, length);
                }
            } else if (event->notify_rx.attr_handle == peer.ds) {
                const int room = static_cast<int>(sizeof(response)) - responseLength;
                const int n = std::min(length, room);
                if (n > 0 && os_mbuf_copydata(event->notify_rx.om, 0, n, response + responseLength) == 0) {
                    responseLength += n;
                }
                if (parseResponse() && !busy) {
                    requestNext();
                }
            }
            break;
        }
        case BLE_GAP_EVENT_DISCONNECT:
            if (event->disconnect.conn.conn_handle == peer.conn) {
                resetPeer();
            }
            break;
        default:
            break;
    }
    return 0;
}

// ---- rendering (loop task) ----
msg::App appKind(const char* appId) {
    if (strcmp(appId, "com.tencent.xin") == 0) {
        return msg::App::WeChat;
    }
    if (strstr(appId, "lark") != nullptr || strstr(appId, "feishu") != nullptr) {
        return msg::App::Feishu;
    }
    return msg::App::Other;
}

void flatten(char* text) {
    for (char* p = text; *p != '\0'; ++p) {
        if (*p == '\n' || *p == '\r' || *p == '\t') {
            *p = ' ';
        }
    }
}

void render(Item& item) {
    constexpr int kWidth = msg::kMaxWidth;
    constexpr int kHeight = 44;              // two 20 px lines + margins
    static std::uint8_t bitmap[((kWidth + 3) / 4) * kHeight];
    memset(bitmap, 0, sizeof(bitmap));
    flatten(item.title);
    flatten(item.message);
    if (item.title[0] == '\0' && item.category == 1) {
        snprintf(item.title, sizeof(item.title), "%s", "来电");
    }
    const int timeWidth = item.time[0] ? textfont::width(item.time) : 0;
    String head = String(item.app);
    if (item.title[0] != '\0' && strcmp(item.title, item.app) != 0) {
        head += " · ";
        head += item.title;
    }
    head = textfont::fit(head.c_str(), kWidth - timeWidth - 10);
    textfont::draw2bpp(bitmap, kWidth, kHeight, 0, 1, head.c_str(), 3, kWidth);
    if (timeWidth) {
        textfont::draw2bpp(bitmap, kWidth, kHeight, kWidth - timeWidth, 1, item.time, 2, kWidth);
    }
    const String body = textfont::fit(item.message[0] ? item.message : "-", kWidth);
    textfont::draw2bpp(bitmap, kWidth, kHeight, 0, 23, body.c_str(), 2, kWidth);
    msg::addCard(appKind(item.appId), kWidth, kHeight, bitmap);
}

}  // namespace

void begin() {
    if (items == nullptr) {
        items = xQueueCreate(4, sizeof(Item));
    }
    NimBLEDevice::setCustomGapHandler(gapEvent);
}

void loop() {
    if (busy && millis() - busySinceMs > 4000) {      // the phone never answered: move on
        busy = false;
        setError("no answer from iPhone");
        requestNext();
    }
    Item item;
    while (items != nullptr && xQueueReceive(items, &item, 0) == pdTRUE) {
        if (textfont::ready()) {
            render(item);
        }
    }
}

bool connected() {
    return linkReady;
}

std::uint32_t received() {
    return receivedCount;
}

String status() {
    return String("ANCS LINK=") + (linkReady ? "1" : "0") + " RX=" + String(receivedCount)
        + (lastError[0] ? String(" ERR=") + lastError : String(""));
}

}  // namespace ancs
