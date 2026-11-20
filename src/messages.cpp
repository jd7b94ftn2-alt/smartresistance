#include "messages.h"

#include <esp_heap_caps.h>
#include <mbedtls/base64.h>

#include <algorithm>
#include <cstring>

namespace msg {
namespace {

constexpr std::size_t kSlotBytes = static_cast<std::size_t>((kMaxWidth + 3) / 4) * kMaxHeight;
constexpr int kListTarget = -1;

struct Slot {
    Message info;
    std::uint8_t* data = nullptr;
};

Slot slots[kMaxMessages];
Slot images[kImageSlots];
bool imageValid[kImageSlots] = {};
int newest = kMaxMessages - 1;      // the first commit goes to slot 0
int stored = 0;
int unreadCount = 0;
std::uint32_t rev = 1;
std::uint32_t imgRev = 1;

// The card / image currently being received.
std::uint8_t* pending = nullptr;
bool pendingActive = false;
int pendingTarget = kListTarget;     // kListTarget or an image slot
std::uint16_t pendingId = 0;
App pendingApp = App::Other;
std::uint16_t pendingWidth = 0;
std::uint16_t pendingHeight = 0;
std::size_t pendingBytes = 0;
std::size_t pendingReceived = 0;

std::uint8_t* allocate(std::size_t bytes) {
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == nullptr) {
        p = malloc(bytes);
    }
    return static_cast<std::uint8_t*>(p);
}

bool parseUint(const String& text, std::uint32_t& value) {
    if (text.length() == 0) {
        return false;
    }
    char* end = nullptr;
    const unsigned long v = strtoul(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') {
        return false;
    }
    value = static_cast<std::uint32_t>(v);
    return true;
}

String statusText() {
    return "N=" + String(stored) + " UNREAD=" + String(unreadCount);
}

String startPending(int target, std::uint32_t id, std::uint32_t app, std::uint32_t width, std::uint32_t height) {
    if (width < 8 || width > kMaxWidth || height < 8 || height > kMaxHeight || app > 2) {
        return "ERR size: w 8..288, h 8..50";
    }
    begin();
    if (pending == nullptr) {
        return "ERR no memory";
    }
    pendingActive = true;
    pendingTarget = target;
    pendingId = static_cast<std::uint16_t>(id);
    pendingApp = static_cast<App>(app);
    pendingWidth = static_cast<std::uint16_t>(width);
    pendingHeight = static_cast<std::uint16_t>(height);
    pendingBytes = static_cast<std::size_t>((width + 3) / 4) * height;
    pendingReceived = 0;
    memset(pending, 0, kSlotBytes);
    return "OK";
}

String addPendingData(int target, std::uint32_t id, const String& offsetText, const String& data) {
    std::uint32_t offset = 0;
    if (!pendingActive || pendingTarget != target || pendingId != id) {
        return "ERR nothing in progress for " + String(id);
    }
    if (!parseUint(offsetText, offset) || offset >= pendingBytes) {
        return "ERR offset";
    }
    std::size_t written = 0;
    const int result = mbedtls_base64_decode(pending + offset, pendingBytes - offset, &written,
        reinterpret_cast<const unsigned char*>(data.c_str()), data.length());
    if (result != 0) {
        return "ERR base64 " + String(result);
    }
    pendingReceived = std::max(pendingReceived, static_cast<std::size_t>(offset) + written);
    return "";      // silent on success: the host streams these without waiting
}

// Copies the pending bitmap into `slot`; false when out of memory.
bool commitInto(Slot& slot) {
    if (slot.data == nullptr) {
        slot.data = allocate(kSlotBytes);
        if (slot.data == nullptr) {
            return false;
        }
    }
    memcpy(slot.data, pending, pendingBytes);
    slot.info.id = pendingId;
    slot.info.app = pendingApp;
    slot.info.width = pendingWidth;
    slot.info.height = pendingHeight;
    slot.info.receivedMs = millis();
    slot.info.pixels = slot.data;
    pendingActive = false;
    return true;
}

}  // namespace

void begin() {
    if (pending == nullptr) {
        pending = allocate(kSlotBytes);
    }
}

String handleCommand(const String& cmd, const String* tokens, int count) {
    if (cmd == "MSG?") {
        return statusText();
    }
    if (cmd == "MSGREAD") {
        markAllRead();
        return "OK " + statusText();
    }
    if (cmd == "MSGCLR") {
        clear();
        return "OK";
    }

    std::uint32_t id = 0;
    if (count < 2 || !parseUint(tokens[1], id) || id > 0xFFFF) {
        return "ERR " + cmd + " <id|slot> ...";
    }

    // ---- image slots ----
    if (cmd.startsWith("IMG")) {
        if (id >= static_cast<std::uint32_t>(kImageSlots)) {
            return "ERR slot 0.." + String(kImageSlots - 1);
        }
        const int slot = static_cast<int>(id);
        if (cmd == "IMGNEW") {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            if (count < 4 || !parseUint(tokens[2], width) || !parseUint(tokens[3], height)) {
                return "ERR IMGNEW <slot> <w> <h>";
            }
            return startPending(slot, id, static_cast<std::uint32_t>(App::Other), width, height);
        }
        if (cmd == "IMGDAT") {
            return count < 4 ? String("ERR IMGDAT <slot> <offset> <base64>") : addPendingData(slot, id, tokens[2], tokens[3]);
        }
        if (cmd == "IMGEND") {
            if (!pendingActive || pendingTarget != slot || pendingReceived < pendingBytes) {
                return "ERR incomplete";
            }
            if (!commitInto(images[slot])) {
                return "ERR no memory";
            }
            imageValid[slot] = true;
            ++imgRev;
            return "OK";
        }
        if (cmd == "IMGCLR") {
            imageValid[slot] = false;
            ++imgRev;
            return "OK";
        }
        return "ERR unknown IMG command";
    }

    // ---- message cards ----
    if (cmd == "MSGNEW") {
        std::uint32_t app = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        if (count < 5 || !parseUint(tokens[2], app) || !parseUint(tokens[3], width) || !parseUint(tokens[4], height)) {
            return "ERR MSGNEW <id> <app 0..2> <w 8..288> <h 8..50>";
        }
        return startPending(kListTarget, id, app, width, height);
    }
    if (cmd == "MSGDAT") {
        return count < 4 ? String("ERR MSGDAT <id> <offset> <base64>") : addPendingData(kListTarget, id, tokens[2], tokens[3]);
    }
    if (cmd == "MSGEND") {
        if (!pendingActive || pendingTarget != kListTarget || pendingId != id) {
            return "ERR no card " + String(id) + " in progress";
        }
        if (pendingReceived < pendingBytes) {
            return "ERR incomplete " + String(pendingReceived) + "/" + String(pendingBytes);
        }
        const int index = (newest + 1) % kMaxMessages;
        if (!commitInto(slots[index])) {
            return "ERR no memory";
        }
        newest = index;
        stored = std::min(stored + 1, kMaxMessages);
        unreadCount = std::min(unreadCount + 1, kMaxMessages);
        ++rev;
        return "OK " + statusText();
    }
    return "ERR unknown MSG command";
}

bool addCard(App app, int width, int height, const std::uint8_t* pixels) {
    if (width < 8 || width > kMaxWidth || height < 8 || height > kMaxHeight || pixels == nullptr) {
        return false;
    }
    const int index = (newest + 1) % kMaxMessages;
    Slot& slot = slots[index];
    if (slot.data == nullptr) {
        slot.data = allocate(kSlotBytes);
        if (slot.data == nullptr) {
            return false;
        }
    }
    memcpy(slot.data, pixels, static_cast<std::size_t>((width + 3) / 4) * height);
    slot.info.id = 0xFFFF;
    slot.info.app = app;
    slot.info.width = static_cast<std::uint16_t>(width);
    slot.info.height = static_cast<std::uint16_t>(height);
    slot.info.receivedMs = millis();
    slot.info.pixels = slot.data;
    newest = index;
    stored = std::min(stored + 1, kMaxMessages);
    unreadCount = std::min(unreadCount + 1, kMaxMessages);
    ++rev;
    return true;
}

int count() {
    return stored;
}

int unread() {
    return unreadCount;
}

bool get(int index, Message& out) {
    if (index < 0 || index >= stored) {
        return false;
    }
    out = slots[(newest - index + kMaxMessages) % kMaxMessages].info;
    return out.pixels != nullptr;
}

bool image(int slot, Message& out) {
    if (slot < 0 || slot >= kImageSlots || !imageValid[slot]) {
        return false;
    }
    out = images[slot].info;
    return out.pixels != nullptr;
}

std::uint32_t imageRevision() {
    return imgRev;
}

void markAllRead() {
    if (unreadCount != 0) {
        unreadCount = 0;
        ++rev;
    }
}

void clear() {
    stored = 0;
    unreadCount = 0;
    newest = kMaxMessages - 1;
    if (pendingTarget == kListTarget) {
        pendingActive = false;
    }
    ++rev;
}

std::uint32_t revision() {
    return rev;
}

}  // namespace msg
