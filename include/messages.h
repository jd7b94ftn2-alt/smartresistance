#pragma once

#include <Arduino.h>

#include <cstdint>

// Message cards pushed by the PC host (Feishu / WeChat notifications). The host renders each card
// as a small 2-bit grey bitmap (any font, emoji, ...), so the firmware needs no CJK font for chat
// text. Cards live in PSRAM; the newest kMaxMessages are kept.
//
//   MSGNEW <id> <app> <w> <h>      start a card   (app 0 = Feishu, 1 = WeChat, 2 = other)
//   MSGDAT <id> <offset> <base64>  card bytes     (no reply on success)
//   MSGEND <id>                    commit         -> "OK N=<stored> UNREAD=<unread>"
//   MSG? | MSGREAD | MSGCLR        status / mark all read / delete all
namespace msg {

constexpr int kMaxMessages = 30;
constexpr int kMaxWidth = 288;
constexpr int kMaxHeight = 50;

enum class App : std::uint8_t { Feishu = 0, WeChat = 1, Other = 2 };

struct Message {
    std::uint16_t id = 0;
    App app = App::Other;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint32_t receivedMs = 0;
    // 2 bits per pixel (0 = background .. 3 = full text colour), 4 pixels per byte, MSB first,
    // row stride (width + 3) / 4 bytes.
    const std::uint8_t* pixels = nullptr;
};

// Fixed image slots for other pages (slot 0 = Desk page "now playing"), same 2-bit format:
//   IMGNEW <slot> <w> <h> | IMGDAT <slot> <offset> <base64> | IMGEND <slot> | IMGCLR <slot>
constexpr int kImageSlots = 2;

void begin();
// Handles every command starting with "MSG" or "IMG"; `cmd` is tokens[0] upper-cased.
String handleCommand(const String& cmd, const String* tokens, int count);
bool image(int slot, Message& out);    // false when the slot is empty
std::uint32_t imageRevision();

int count();
int unread();
bool get(int index, Message& out);     // 0 = newest
void markAllRead();
void clear();
std::uint32_t revision();              // changes whenever a card is added or removed

}  // namespace msg
