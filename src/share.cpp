#include "share.h"

#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace share {
namespace {

constexpr const char* kDir = "/share";
constexpr const char* kIndex = "/share/index";
constexpr std::uint32_t kReserveBytes = 16UL * 1024UL;   // keep LittleFS some room for its metadata

bool mounted = false;
Item items[kMaxItems];                 // newest first
int itemCount = 0;
std::uint16_t lastId = 0;
std::uint32_t rev = 1;
int arrived = -1;

// upload in progress
std::uint8_t* rx = nullptr;
std::uint32_t rxSize = 0;
std::uint32_t rxReceived = 0;
Kind rxKind = Kind::Image;
String rxName;
bool rxActive = false;
bool rxOpenSize = false;               // size was not given: rxSize is the limit until the end

// last loaded file
std::uint8_t* view = nullptr;
std::size_t viewSize = 0;
std::uint16_t viewId = 0;

std::uint8_t* allocate(std::size_t bytes) {
    void* p = heap_caps_malloc(std::max<std::size_t>(bytes, 1), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return static_cast<std::uint8_t*>(p);
}

String pathOf(const Item& it) {
    return String(kDir) + "/" + it.id + (it.kind == Kind::Image ? ".jpg" : ".txt");
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t length) {
    std::uint32_t crc = 0xFFFFFFFFUL;
    for (std::size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0xEDB88320UL & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

void saveIndex() {
    File f = LittleFS.open(kIndex, "w");
    if (!f) {
        return;
    }
    for (int i = 0; i < itemCount; ++i) {
        f.printf("%u %u %lu %s\n", items[i].id, static_cast<unsigned>(items[i].kind),
            static_cast<unsigned long>(items[i].size), items[i].name.c_str());
    }
    f.close();
}

void loadIndex() {
    itemCount = 0;
    File f = LittleFS.open(kIndex, "r");
    if (!f) {
        return;
    }
    while (f.available() && itemCount < kMaxItems) {
        const String line = f.readStringUntil('\n');
        char name[96] = "";
        unsigned id = 0;
        unsigned kind = 0;
        unsigned long size = 0;
        if (sscanf(line.c_str(), "%u %u %lu %95[^\n]", &id, &kind, &size, name) < 3 || id == 0 || kind > 1) {
            continue;
        }
        Item& it = items[itemCount];
        it.id = static_cast<std::uint16_t>(id);
        it.kind = static_cast<Kind>(kind);
        it.size = static_cast<std::uint32_t>(size);
        it.name = name;
        if (LittleFS.exists(pathOf(it))) {
            ++itemCount;
            lastId = std::max<std::uint16_t>(lastId, it.id);
        }
    }
    f.close();
}

void dropAt(int index) {
    LittleFS.remove(pathOf(items[index]));
    if (viewId == items[index].id) {
        viewId = 0;
    }
    for (int i = index; i + 1 < itemCount; ++i) {
        items[i] = items[i + 1];
    }
    --itemCount;
    ++rev;
}

void releaseRx() {
    if (rx != nullptr && rx != view) {
        free(rx);
    }
    rx = nullptr;
    rxActive = false;
    rxOpenSize = false;
    rxReceived = 0;
    rxSize = 0;
}

Kind parseKind(const String& text, bool& ok) {
    ok = true;
    if (text.equalsIgnoreCase("IMG") || text.equalsIgnoreCase("JPG")) {
        return Kind::Image;
    }
    if (text.equalsIgnoreCase("TXT")) {
        return Kind::Text;
    }
    ok = false;
    return Kind::Image;
}

String listText() {
    String out = String("FILES N=") + itemCount + " FREE=" + freeBytes();
    for (int i = 0; i < itemCount; ++i) {
        out += String("\n") + items[i].id + " " + (items[i].kind == Kind::Image ? "IMG" : "TXT") + " " + items[i].size + " "
            + items[i].name;
    }
    return out;
}

}  // namespace

void begin() {
    // "spiffs" is the data partition of default_16MB.csv (3.4 MB, unused before 1.8.0).
    mounted = LittleFS.begin(true, "/littlefs", 4, "spiffs");
    if (!mounted) {
        return;
    }
    if (!LittleFS.exists(kDir)) {
        LittleFS.mkdir(kDir);
    }
    loadIndex();
}

bool beginUpload(Kind kind, std::uint32_t size, const String& name, String& error) {
    abortUpload();
    if (!mounted) {
        error = "no file system";
        return false;
    }
    const std::uint32_t limit = kind == Kind::Image ? kMaxImageBytes : kMaxTextBytes;
    rxOpenSize = size == 0;
    if (rxOpenSize) {
        size = limit;
    }
    if (size > limit) {
        error = String("size 1..") + limit;
        return false;
    }
    if (size + kReserveBytes > LittleFS.totalBytes()) {
        error = "too big for the storage";
        return false;
    }
    rx = allocate(size);
    if (rx == nullptr) {
        error = "no memory";
        return false;
    }
    rxSize = size;
    rxReceived = 0;
    rxKind = kind;
    rxName = name.length() > 0 ? name : String(kind == Kind::Image ? "图片" : "文本");
    rxName.replace("\n", " ");
    if (rxName.length() > 80) {
        rxName = rxName.substring(0, 80);
    }
    rxActive = true;
    return true;
}

bool writeUpload(const std::uint8_t* data, std::size_t length) {
    if (!rxActive || rxReceived + length > rxSize) {
        return false;
    }
    memcpy(rx + rxReceived, data, length);
    rxReceived += static_cast<std::uint32_t>(length);
    return true;
}

void renameUpload(const String& name) {
    if (rxActive && name.length() > 0) {
        rxName = name.length() > 80 ? name.substring(0, 80) : name;
        rxName.replace("\n", " ");
    }
}

int finishUpload(String& error) {
    if (!rxActive) {
        error = "no upload in progress";
        return -1;
    }
    if (rxOpenSize) {                      // size not given: take what arrived, give the rest back
        rxOpenSize = false;
        if (rxReceived == 0) {
            error = "empty file";
            abortUpload();
            return -1;
        }
        rxSize = rxReceived;
        void* smaller = heap_caps_realloc(rx, rxSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (smaller != nullptr) {
            rx = static_cast<std::uint8_t*>(smaller);
        }
    }
    if (rxReceived != rxSize) {
        error = String("incomplete ") + rxReceived + "/" + rxSize;
        abortUpload();
        return -1;
    }
    if (rxKind == Kind::Image && !(rxSize > 3 && rx[0] == 0xFF && rx[1] == 0xD8 && rx[2] == 0xFF)) {
        error = "not a JPEG (Shortcuts: add 'Convert Image' to JPEG first)";
        abortUpload();
        return -1;
    }
    // Room: drop the oldest files until the new one fits and a slot is free.
    while (itemCount > 0
        && (itemCount >= kMaxItems || LittleFS.totalBytes() - LittleFS.usedBytes() < rxSize + kReserveBytes)) {
        dropAt(itemCount - 1);
    }
    Item it;
    it.id = static_cast<std::uint16_t>(lastId >= 65000 ? 1 : lastId + 1);
    it.kind = rxKind;
    it.size = rxSize;
    it.name = rxName;
    File f = LittleFS.open(pathOf(it), "w");
    if (!f) {
        error = "cannot write";
        abortUpload();
        return -1;
    }
    const std::size_t written = f.write(rx, rxSize);
    f.close();
    if (written != rxSize) {
        LittleFS.remove(pathOf(it));
        error = "storage full";
        abortUpload();
        return -1;
    }
    lastId = it.id;
    for (int i = std::min(itemCount, kMaxItems - 1); i > 0; --i) {
        items[i] = items[i - 1];
    }
    items[0] = it;
    itemCount = std::min(itemCount + 1, kMaxItems);
    saveIndex();
    // The received bytes become the loaded file: the screen shows it without reading flash back.
    if (view != nullptr && view != rx) {
        free(view);
    }
    view = rx;
    viewSize = rxSize;
    viewId = it.id;
    rx = nullptr;
    rxActive = false;
    arrived = it.id;
    ++rev;
    return it.id;
}

void abortUpload() {
    releaseRx();
}

String handleCommand(const String& cmd, const String* tokens, int count) {
    if (cmd == "FILEDAT") {
        if (!rxActive || count < 3) {
            return "";
        }
        const std::uint32_t offset = static_cast<std::uint32_t>(strtoul(tokens[1].c_str(), nullptr, 10));
        if (offset != rxReceived) {
            return "";                   // a chunk was lost: the sender resends from FILEPOS?
        }
        std::uint8_t chunk[192];
        std::size_t length = 0;
        if (mbedtls_base64_decode(chunk, sizeof(chunk), &length, reinterpret_cast<const unsigned char*>(tokens[2].c_str()),
                tokens[2].length()) != 0) {
            return "";
        }
        writeUpload(chunk, length);
        return "";
    }
    if (cmd == "FILEPOS?") {
        return String("FILEPOS ") + rxReceived + " " + rxSize;
    }
    if (cmd == "FILENEW") {
        bool ok = false;
        const Kind kind = count >= 2 ? parseKind(tokens[1], ok) : Kind::Image;
        if (!ok || count < 3) {
            return "ERR FILENEW <IMG|TXT> <bytes> <name>";
        }
        String name;
        for (int i = 3; i < count; ++i) {
            if (i > 3) {
                name += " ";
            }
            name += tokens[i];
        }
        String error;
        if (!beginUpload(kind, static_cast<std::uint32_t>(strtoul(tokens[2].c_str(), nullptr, 10)), name, error)) {
            return String("ERR ") + error;
        }
        return "OK";
    }
    if (cmd == "FILEEND") {
        if (!rxActive) {
            return "ERR no upload in progress";
        }
        if (rxReceived != rxSize) {
            return String("ERR incomplete ") + rxReceived + "/" + rxSize;
        }
        if (count >= 2) {
            const std::uint32_t want = static_cast<std::uint32_t>(strtoul(tokens[1].c_str(), nullptr, 16));
            const std::uint32_t got = crc32(rx, rxSize);
            if (want != got) {
                abortUpload();
                return "ERR crc";
            }
        }
        String error;
        const int id = finishUpload(error);
        if (id < 0) {
            return String("ERR ") + error;
        }
        return String("OK ID=") + id;
    }
    if (cmd == "FILES?") {
        return listText();
    }
    if (cmd == "FILEOPEN") {
        const std::uint16_t id = count >= 2 ? static_cast<std::uint16_t>(tokens[1].toInt()) : 0;
        if (indexOf(id) < 0) {
            return String("ERR no file ") + id;
        }
        requestOpen(id);
        return "OK";
    }
    if (cmd == "FILEDEL") {
        if (count >= 2 && tokens[1].equalsIgnoreCase("ALL")) {
            while (itemCount > 0) {
                dropAt(0);
            }
            saveIndex();
            return "OK";
        }
        const std::uint16_t id = count >= 2 ? static_cast<std::uint16_t>(tokens[1].toInt()) : 0;
        if (!remove(id)) {
            return String("ERR no file ") + id;
        }
        return "OK";
    }
    return "ERR unknown FILE command";
}

int count() {
    return itemCount;
}

bool item(int index, Item& out) {
    if (index < 0 || index >= itemCount) {
        return false;
    }
    out = items[index];
    return true;
}

int indexOf(std::uint16_t id) {
    for (int i = 0; i < itemCount; ++i) {
        if (items[i].id == id) {
            return i;
        }
    }
    return -1;
}

bool find(std::uint16_t id, Item& out) {
    const int i = indexOf(id);
    return i >= 0 && item(i, out);
}

bool load(std::uint16_t id, const std::uint8_t*& data, std::size_t& size) {
    if (view != nullptr && viewId == id) {
        data = view;
        size = viewSize;
        return true;
    }
    Item it;
    if (!find(id, it)) {
        return false;
    }
    File f = LittleFS.open(pathOf(it), "r");
    if (!f) {
        return false;
    }
    const std::size_t n = f.size();
    std::uint8_t* buffer = allocate(n);
    if (buffer == nullptr) {
        f.close();
        return false;
    }
    const std::size_t got = f.read(buffer, n);
    f.close();
    if (got != n) {
        free(buffer);
        return false;
    }
    if (view != nullptr && view != rx) {
        free(view);
    }
    view = buffer;
    viewSize = n;
    viewId = id;
    data = view;
    size = viewSize;
    return true;
}

bool remove(std::uint16_t id) {
    const int i = indexOf(id);
    if (i < 0) {
        return false;
    }
    dropAt(i);
    saveIndex();
    return true;
}

int takeArrived() {
    const int id = arrived;
    arrived = -1;
    return id;
}

void requestOpen(std::uint16_t id) {
    arrived = id;
}

std::uint32_t revision() {
    return rev;
}

bool ready() {
    return mounted;
}

std::uint32_t freeBytes() {
    return mounted ? static_cast<std::uint32_t>(LittleFS.totalBytes() - LittleFS.usedBytes()) : 0;
}

std::uint32_t usedBytes() {
    return mounted ? static_cast<std::uint32_t>(LittleFS.usedBytes()) : 0;
}

}  // namespace share
