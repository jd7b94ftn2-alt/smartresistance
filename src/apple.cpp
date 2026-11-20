#include "apple.h"

#include <NimBLEDevice.h>
#include "nimble/nimble/host/include/host/ble_gatt.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "desk.h"
#include "messages.h"
#include "textfont.h"

// AMS (developer.apple.com "Apple Media Service Reference") and the Bluetooth SIG Current Time
// Service, as a GATT client on the iPhone's link. Set-up runs on the NimBLE host task one procedure
// at a time (AMS discovery -> subscribe -> register track / player attributes -> CTS discovery ->
// read time zone -> read time); results go to the loop task.
namespace apple {
namespace {

const ble_uuid128_t kAms = BLE_UUID128_INIT(0xDC, 0xF8, 0x55, 0xAD, 0x02, 0xC5, 0xF4, 0x8E, 0x3A, 0x43, 0x36, 0x0F, 0x2B,
    0x50, 0xD3, 0x89);
const ble_uuid128_t kRemoteCommand = BLE_UUID128_INIT(0xC2, 0x51, 0xCA, 0xF7, 0x56, 0x0E, 0xDF, 0xB8, 0x8A, 0x4A, 0xB1,
    0x57, 0xD8, 0x81, 0x3C, 0x9B);
const ble_uuid128_t kEntityUpdate = BLE_UUID128_INIT(0x02, 0xC1, 0x96, 0xBA, 0x92, 0xBB, 0x0C, 0x9A, 0x1F, 0x41, 0x8D,
    0x80, 0xCE, 0xAB, 0x7C, 0x2F);
const ble_uuid16_t kCts = BLE_UUID16_INIT(0x1805);
const ble_uuid16_t kCurrentTime = BLE_UUID16_INIT(0x2A2B);
const ble_uuid16_t kLocalTimeInfo = BLE_UUID16_INIT(0x2A0F);
const ble_uuid16_t kCccd = BLE_UUID16_INIT(0x2902);

constexpr std::uint16_t kNone = 0xFFFF;
constexpr std::uint8_t kEntityPlayer = 0;
constexpr std::uint8_t kEntityTrack = 2;
constexpr std::uint8_t kPlayerName = 0;
constexpr std::uint8_t kPlayerPlayback = 1;      // "state,rate,elapsed"; state 0 paused, 1 playing
constexpr std::uint8_t kTrackArtist = 0;
constexpr std::uint8_t kTrackTitle = 2;
constexpr std::uint8_t kFlagTruncated = 0x01;

struct Update {                        // one Entity Update notification, host task -> loop task
    std::uint8_t entity;
    std::uint8_t attribute;
    std::uint8_t flags;
    char value[124];
};
QueueHandle_t updates = nullptr;

// ---- host task ----
struct Link {
    std::uint16_t conn = kNone;
    std::uint16_t amsStart = 0;
    std::uint16_t amsEnd = 0;
    std::uint16_t rc = 0;
    std::uint16_t eu = 0;
    std::uint16_t euCccd = 0;
    std::uint16_t ctsStart = 0;
    std::uint16_t ctsEnd = 0;
    std::uint16_t currentTime = 0;
    std::uint16_t localInfo = 0;
    std::int8_t zoneQuarters = -128;   // Local Time Information: UTC offset in 15 min steps, -128 unknown
    std::uint8_t dstQuarters = 0;
};
Link link;
void (*whenDone)() = nullptr;
volatile bool settingUp = false;
volatile std::uint32_t setupStartMs = 0;
volatile bool amsReady = false;
volatile bool ctsRead = false;
char lastError[40] = "";

volatile bool timePending = false;     // phone time, host task -> loop task
std::uint8_t timeRaw[10];
std::int8_t timeZone = -128;
std::uint8_t timeDst = 0;

void setError(const char* text) {
    snprintf(lastError, sizeof(lastError), "%s", text);
}

void finishSetup() {
    settingUp = false;
    if (whenDone != nullptr) {
        whenDone();
    }
}

// ---- CTS ----
int onCurrentTime(std::uint16_t conn, const ble_gatt_error* error, ble_gatt_attr* attr, void* arg) {
    (void)arg;
    if (conn != link.conn) {
        return 0;
    }
    if (error->status == 0 && attr != nullptr && attr->om != nullptr) {
        std::uint8_t raw[10] = {};
        const int n = std::min<int>(OS_MBUF_PKTLEN(attr->om), sizeof(raw));
        if (n >= 7 && os_mbuf_copydata(attr->om, 0, n, raw) == 0) {
            memcpy(timeRaw, raw, sizeof(raw));
            timeZone = link.zoneQuarters;
            timeDst = link.dstQuarters;
            timePending = true;
            ctsRead = true;
        }
    } else {
        setError("CTS read failed");
    }
    finishSetup();
    return 0;
}

void readCurrentTime() {
    if (link.currentTime == 0 || ble_gattc_read(link.conn, link.currentTime, onCurrentTime, nullptr) != 0) {
        if (link.currentTime == 0) {
            setError("no CTS time");
        }
        finishSetup();
    }
}

int onLocalInfo(std::uint16_t conn, const ble_gatt_error* error, ble_gatt_attr* attr, void* arg) {
    (void)arg;
    if (conn != link.conn) {
        return 0;
    }
    if (error->status == 0 && attr != nullptr && attr->om != nullptr && OS_MBUF_PKTLEN(attr->om) >= 2) {
        std::uint8_t raw[2];
        if (os_mbuf_copydata(attr->om, 0, 2, raw) == 0) {
            link.zoneQuarters = static_cast<std::int8_t>(raw[0]);
            link.dstQuarters = raw[1] == 255 ? 0 : raw[1];
        }
    }
    readCurrentTime();
    return 0;
}

int onCtsCharacteristic(std::uint16_t conn, const ble_gatt_error* error, const ble_gatt_chr* chr, void* arg) {
    (void)arg;
    if (conn != link.conn) {
        return 0;
    }
    if (error->status == 0 && chr != nullptr) {
        if (ble_uuid_cmp(&chr->uuid.u, &kCurrentTime.u) == 0) {
            link.currentTime = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &kLocalTimeInfo.u) == 0) {
            link.localInfo = chr->val_handle;
        }
        return 0;
    }
    if (link.localInfo != 0 && ble_gattc_read(link.conn, link.localInfo, onLocalInfo, nullptr) == 0) {
        return 0;
    }
    readCurrentTime();
    return 0;
}

int onCtsService(std::uint16_t conn, const ble_gatt_error* error, const ble_gatt_svc* service, void* arg) {
    (void)arg;
    if (conn != link.conn) {
        return 0;
    }
    if (error->status == 0 && service != nullptr) {
        link.ctsStart = service->start_handle;
        link.ctsEnd = service->end_handle;
        return 0;
    }
    if (link.ctsStart != 0
        && ble_gattc_disc_all_chrs(link.conn, link.ctsStart, link.ctsEnd, onCtsCharacteristic, nullptr) == 0) {
        return 0;
    }
    setError(link.ctsStart != 0 ? "CTS discovery failed" : "no CTS on phone");
    finishSetup();
    return 0;
}

void startCts() {
    if (ble_gattc_disc_svc_by_uuid(link.conn, &kCts.u, onCtsService, nullptr) != 0) {
        setError("CTS discovery busy");
        finishSetup();
    }
}

// ---- AMS ----
int onPlayerRegistered(std::uint16_t conn, const ble_gatt_error* error, ble_gatt_attr* attr, void* arg) {
    (void)attr;
    (void)arg;
    if (conn != link.conn) {
        return 0;
    }
    amsReady = error->status == 0;
    if (!amsReady) {
        setError("AMS player register failed");
    }
    startCts();
    return 0;
}

int onTrackRegistered(std::uint16_t conn, const ble_gatt_error* error, ble_gatt_attr* attr, void* arg) {
    (void)attr;
    (void)arg;
    if (conn != link.conn) {
        return 0;
    }
    static const std::uint8_t kPlayerAttributes[] = {kEntityPlayer, kPlayerName, kPlayerPlayback};
    if (error->status != 0
        || ble_gattc_write_flat(link.conn, link.eu, kPlayerAttributes, sizeof(kPlayerAttributes), onPlayerRegistered,
               nullptr) != 0) {
        setError("AMS track register failed");
        startCts();
    }
    return 0;
}

int onEntityUpdateSubscribed(std::uint16_t conn, const ble_gatt_error* error, ble_gatt_attr* attr, void* arg) {
    (void)attr;
    (void)arg;
    if (conn != link.conn) {
        return 0;
    }
    static const std::uint8_t kTrackAttributes[] = {kEntityTrack, kTrackArtist, kTrackTitle};
    if (error->status != 0
        || ble_gattc_write_flat(link.conn, link.eu, kTrackAttributes, sizeof(kTrackAttributes), onTrackRegistered,
               nullptr) != 0) {
        setError(error->status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN) ? "AMS not authorised" : "AMS subscribe failed");
        startCts();
    }
    return 0;
}

int onAmsDescriptor(std::uint16_t conn, const ble_gatt_error* error, std::uint16_t chrValHandle, const ble_gatt_dsc* dsc,
    void* arg) {
    (void)chrValHandle;
    (void)arg;
    if (conn != link.conn) {
        return 0;
    }
    if (error->status == 0 && dsc != nullptr) {
        if (link.euCccd == 0 && dsc->handle > link.eu && ble_uuid_cmp(&dsc->uuid.u, &kCccd.u) == 0) {
            link.euCccd = dsc->handle;
        }
        return 0;
    }
    if (link.euCccd == 0) {
        link.euCccd = static_cast<std::uint16_t>(link.eu + 1);   // usual layout
    }
    static const std::uint8_t kNotify[2] = {0x01, 0x00};
    if (ble_gattc_write_flat(link.conn, link.euCccd, kNotify, sizeof(kNotify), onEntityUpdateSubscribed, nullptr) != 0) {
        setError("AMS subscribe busy");
        startCts();
    }
    return 0;
}

int onAmsCharacteristic(std::uint16_t conn, const ble_gatt_error* error, const ble_gatt_chr* chr, void* arg) {
    (void)arg;
    if (conn != link.conn) {
        return 0;
    }
    if (error->status == 0 && chr != nullptr) {
        if (ble_uuid_cmp(&chr->uuid.u, &kEntityUpdate.u) == 0) {
            link.eu = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &kRemoteCommand.u) == 0) {
            link.rc = chr->val_handle;
        }
        return 0;
    }
    if (link.eu == 0 || ble_gattc_disc_all_dscs(link.conn, link.eu, link.amsEnd, onAmsDescriptor, nullptr) != 0) {
        setError("AMS characteristics missing");
        startCts();
    }
    return 0;
}

int onAmsService(std::uint16_t conn, const ble_gatt_error* error, const ble_gatt_svc* service, void* arg) {
    (void)arg;
    if (conn != link.conn) {
        return 0;
    }
    if (error->status == 0 && service != nullptr) {
        link.amsStart = service->start_handle;
        link.amsEnd = service->end_handle;
        return 0;
    }
    if (link.amsStart == 0
        || ble_gattc_disc_all_chrs(link.conn, link.amsStart, link.amsEnd, onAmsCharacteristic, nullptr) != 0) {
        setError("no AMS on phone");
        startCts();
    }
    return 0;
}

// Drops a UTF-8 sequence that was cut off at the end of `text`.
void trimUtf8(char* text) {
    int n = static_cast<int>(strlen(text));
    int i = n - 1;
    while (i >= 0 && (static_cast<std::uint8_t>(text[i]) & 0xC0) == 0x80) {
        --i;
    }
    if (i < 0) {
        return;
    }
    const std::uint8_t lead = static_cast<std::uint8_t>(text[i]);
    const int need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (n - i < need) {
        text[i] = '\0';
    }
}

// ---- loop task ----
struct Track {
    String title;
    String artist;
    String player;
    int state = 0;
};
Track track;
bool cardDirty = false;
bool wasReady = false;
std::uint32_t ownStamp = 0;            // msg::imageStamp(0) right after this module drew the card

long daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2 ? 1 : 0;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long>(doe) - 719468;
}

void applyTime() {
    if (!timePending) {
        return;
    }
    timePending = false;
    const int year = timeRaw[0] | (timeRaw[1] << 8);
    const unsigned month = timeRaw[2];
    const unsigned day = timeRaw[3];
    if (year < 2020 || month < 1 || month > 12 || day < 1 || day > 31 || timeRaw[4] > 23 || timeRaw[5] > 59
        || timeRaw[6] > 59) {
        return;
    }
    const long long local = static_cast<long long>(daysFromCivil(year, month, day)) * 86400LL + timeRaw[4] * 3600L
        + timeRaw[5] * 60L + timeRaw[6];
    const long offset = timeZone != -128 ? (static_cast<long>(timeZone) + timeDst) * 15L : desk::utcOffsetMinutes();
    desk::setTime(local - offset * 60LL, offset);
}

void showCard() {
    cardDirty = false;
    if (track.title.length() == 0 && track.artist.length() == 0) {
        if (mediaShown()) {
            msg::clearImage(0);
        }
        ownStamp = 0;
        return;
    }
    if (!textfont::ready()) {
        return;
    }
    constexpr int kW = msg::kMaxWidth;
    constexpr int kH = 38;                     // two 16 px text lines
    static std::uint8_t bitmap[((kW + 3) / 4) * kH];
    memset(bitmap, 0, sizeof(bitmap));
    const String first = track.title.length() > 0 ? track.title : track.artist;
    String second = track.title.length() > 0 ? track.artist : String();
    if (track.player.length() > 0) {
        if (second.length() > 0) {
            second += " · ";
        }
        second += track.player;
    }
    textfont::draw2bpp(bitmap, kW, kH, 0, 0, textfont::fit(first.c_str(), kW).c_str(), 3, kW);
    textfont::draw2bpp(bitmap, kW, kH, 0, 19, textfont::fit(second.c_str(), kW).c_str(), 2, kW);
    if (msg::setImage(0, kW, kH, bitmap)) {
        ownStamp = msg::imageStamp(0);
    }
}

void handle(const Update& u) {
    String value = u.value;
    if (u.flags & kFlagTruncated) {
        value += "…";
    }
    if (u.entity == kEntityTrack) {
        if (u.attribute == kTrackTitle && value != track.title) {
            track.title = value;
            cardDirty = true;
        } else if (u.attribute == kTrackArtist && value != track.artist) {
            track.artist = value;
            cardDirty = true;
        }
    } else if (u.entity == kEntityPlayer) {
        if (u.attribute == kPlayerName && value != track.player) {
            track.player = value;
            cardDirty = true;
        } else if (u.attribute == kPlayerPlayback) {
            track.state = atoi(u.value);
            desk::setPlaying(track.state == 1);
        }
    }
}

}  // namespace

void begin() {
    if (updates == nullptr) {
        updates = xQueueCreate(8, sizeof(Update));
    }
}

void start(std::uint16_t conn, void (*done)()) {
    if (settingUp && link.conn == conn) {
        return;
    }
    link = Link();
    link.conn = conn;
    whenDone = done;
    setupStartMs = millis();
    settingUp = true;
    amsReady = false;
    ctsRead = false;
    setError("");
    if (ble_gattc_disc_svc_by_uuid(conn, &kAms.u, onAmsService, nullptr) != 0) {
        setError("AMS discovery busy");
        startCts();
    }
}

bool busy() {
    return settingUp;
}

bool onNotify(std::uint16_t conn, std::uint16_t handle, os_mbuf* om) {
    if (conn != link.conn || link.eu == 0 || om == nullptr) {
        return false;
    }
    if (handle == link.rc) {
        return true;                           // list of supported remote commands: not needed
    }
    if (handle != link.eu) {
        return false;
    }
    const int length = OS_MBUF_PKTLEN(om);
    if (length < 3) {
        return true;
    }
    Update u;
    std::uint8_t head[3];
    if (os_mbuf_copydata(om, 0, 3, head) != 0) {
        return true;
    }
    u.entity = head[0];
    u.attribute = head[1];
    u.flags = head[2];
    const int n = std::min<int>(length - 3, sizeof(u.value) - 1);
    if (n > 0 && os_mbuf_copydata(om, 3, n, u.value) != 0) {
        return true;
    }
    u.value[std::max(0, n)] = '\0';
    if (n == static_cast<int>(sizeof(u.value)) - 1) {
        u.flags |= kFlagTruncated;
    }
    trimUtf8(u.value);
    if (updates != nullptr) {
        xQueueSend(updates, &u, 0);
    }
    return true;
}

void disconnected(std::uint16_t conn) {
    if (conn == link.conn) {
        link = Link();
        amsReady = false;
        settingUp = false;
    }
}

void loop() {
    if (settingUp && millis() - setupStartMs > 15000) {   // the phone stopped answering: let ANCS go on
        setError("set-up timed out");
        finishSetup();
    }
    applyTime();
    Update u;
    while (updates != nullptr && xQueueReceive(updates, &u, 0) == pdTRUE) {
        handle(u);
    }
    if (wasReady && !amsReady) {               // phone gone: drop its card unless the PC replaced it
        if (mediaShown()) {
            msg::clearImage(0);
            desk::setPlaying(false);
        }
        ownStamp = 0;
        track = Track();
    }
    wasReady = amsReady;
    if (cardDirty) {
        showCard();
    }
}

bool mediaReady() {
    return amsReady;
}

bool mediaShown() {
    return ownStamp != 0 && msg::imageStamp(0) == ownStamp;
}

bool command(Command cmd) {
    if (!amsReady || link.rc == 0 || link.conn == kNone) {
        return false;
    }
    const std::uint8_t id = static_cast<std::uint8_t>(cmd);
    return ble_gattc_write_flat(link.conn, link.rc, &id, 1, nullptr, nullptr) == 0;
}

String status() {
    String out = String("IPHONE AMS=") + (amsReady ? "1" : "0") + " CTS=" + (ctsRead ? "1" : "0") + " STATE="
        + track.state + " SHOWN=" + (mediaShown() ? "1" : "0");
    if (lastError[0] != '\0') {
        out += String(" ERR=") + lastError;
    }
    if (track.title.length() > 0) {
        out += " TITLE=" + track.title;
    }
    return out;
}

}  // namespace apple
