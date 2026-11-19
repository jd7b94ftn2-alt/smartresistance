#include "messages.h"

#include <esp_heap_caps.h>
#include <mbedtls/base64.h>

#include <algorithm>
#include <cstring>

namespace msg {
namespace {

constexpr std::size_t kSlotBytes = static_cast<std::size_t>((kMaxWidth + 3) / 4) * kMaxHeight;

struct Slot {
    Message info;
    std::uint8_t* data = nullptr;
};

Slot slots[kMaxMessages];
int newest = kMaxMessages - 1;      // the first commit goes to slot 0
int stored = 0;
int unreadCount = 0;
std::uint32_t rev = 1;

// The card currently being received.
std::uint8_t* pending = nullptr;
bool pendingActive = false;
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
        return "ERR " + cmd + " <id> ...";
    }

    if (cmd == "MSGNEW") {
        std::uint32_t app = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        if (count < 5 || !parseUint(tokens[2], app) || !parseUint(tokens[3], width) || !parseUint(tokens[4], height)
            || app > 2 || width < 8 || width > kMaxWidth || height < 8 || height > kMaxHeight) {
            return "ERR MSGNEW <id> <app 0..2> <w 8..288> <h 8..50>";
        }
        begin();
        if (pending == nullptr) {
            return "ERR no memory";
        }
        pendingActive = true;
        pendingId = static_cast<std::uint16_t>(id);
        pendingApp = static_cast<App>(app);
        pendingWidth = static_cast<std::uint16_t>(width);
        pendingHeight = static_cast<std::uint16_t>(height);
        pendingBytes = static_cast<std::size_t>((width + 3) / 4) * height;
        pendingReceived = 0;
        memset(pending, 0, kSlotBytes);
        return "OK";
    }

    if (!pendingActive || pendingId != id) {
        return "ERR no card " + String(id) + " in progress";
    }

    if (cmd == "MSGDAT") {
        std::uint32_t offset = 0;
        if (count < 4 || !parseUint(tokens[2], offset) || offset >= pendingBytes) {
            return "ERR MSGDAT <id> <offset> <base64>";
        }
        std::size_t written = 0;
        const int result = mbedtls_base64_decode(pending + offset, pendingBytes - offset, &written,
            reinterpret_cast<const unsigned char*>(tokens[3].c_str()), tokens[3].length());
        if (result != 0) {
            return "ERR base64 " + String(result);
        }
        pendingReceived = std::max(pendingReceived, static_cast<std::size_t>(offset) + written);
        return "";      // silent on success: the host streams these without waiting
    }

    if (cmd == "MSGEND") {
        if (pendingReceived < pendingBytes) {
            return "ERR incomplete " + String(pendingReceived) + "/" + String(pendingBytes);
        }
        const int index = (newest + 1) % kMaxMessages;
        Slot& slot = slots[index];
        if (slot.data == nullptr) {
            slot.data = allocate(kSlotBytes);
            if (slot.data == nullptr) {
                return "ERR no memory";
            }
        }
        memcpy(slot.data, pending, pendingBytes);
        slot.info.id = pendingId;
        slot.info.app = pendingApp;
        slot.info.width = pendingWidth;
        slot.info.height = pendingHeight;
        slot.info.receivedMs = millis();
        slot.info.pixels = slot.data;
        newest = index;
        stored = std::min(stored + 1, kMaxMessages);
        unreadCount = std::min(unreadCount + 1, kMaxMessages);
        pendingActive = false;
        ++rev;
        return "OK " + statusText();
    }

    return "ERR unknown MSG command";
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
    pendingActive = false;
    ++rev;
}

std::uint32_t revision() {
    return rev;
}

}  // namespace msg
