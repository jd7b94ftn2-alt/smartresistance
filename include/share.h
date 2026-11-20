#pragma once

#include <Arduino.h>

#include <cstdint>

// Pictures and text files shared to the screen ("分享" page), from the PC host app or the phone web
// page. Senders convert first: pictures to a baseline JPEG of at most 320 x 240, text to UTF-8.
// Files live in LittleFS on the otherwise unused 3.4 MB "spiffs" partition, so they survive a
// reboot; the newest kMaxItems are kept (older ones go when space or slots run out).
//   FILENEW <IMG|TXT> <bytes> <name...>   start (IMG <= 400 KB, TXT <= 1 MB)
//   FILEDAT <offset> <base64>              data in order, <= 120 bytes per line (no reply;
//                                          a chunk that is not the next one is ignored)
//   FILEPOS?                               -> FILEPOS <received> <total>  (resume point after a drop)
//   FILEEND <crc32 hex>                    -> OK ID=<id>; the board shows the file at once
//   FILES?  |  FILEOPEN <id>  |  FILEDEL <id|ALL>
// Wi-Fi: POST /api/upload?kind=IMG|TXT&size=<bytes>&name=<name> with a multipart file field.
namespace share {

constexpr int kMaxItems = 30;
constexpr std::uint32_t kMaxImageBytes = 400UL * 1024UL;
constexpr std::uint32_t kMaxTextBytes = 1024UL * 1024UL;

enum class Kind : std::uint8_t { Image = 0, Text = 1 };

struct Item {
    std::uint16_t id = 0;
    Kind kind = Kind::Image;
    std::uint32_t size = 0;
    String name;                       // UTF-8
};

void begin();                          // mounts the file system (formats it on first use)
String handleCommand(const String& cmd, const String* tokens, int count);

// Streaming upload (HTTP); the line commands use the same path. size 0 = not known in advance (iPhone
// Shortcuts): room for the largest file is reserved and the real size is taken at the end.
bool beginUpload(Kind kind, std::uint32_t size, const String& name, String& error);
bool writeUpload(const std::uint8_t* data, std::size_t length);
void renameUpload(const String& name);
int finishUpload(String& error);       // new id, or -1 (pictures must be JPEG)
void abortUpload();

int count();
bool item(int index, Item& out);       // 0 = newest
bool find(std::uint16_t id, Item& out);
int indexOf(std::uint16_t id);         // -1 when gone
// Reads a file into a PSRAM buffer owned by this module, valid until the next load / upload.
bool load(std::uint16_t id, const std::uint8_t*& data, std::size_t& size);
bool remove(std::uint16_t id);
int takeArrived();                     // id of a file that just arrived (show it), -1 when none
void requestOpen(std::uint16_t id);    // FILEOPEN: same, for an existing file
std::uint32_t revision();              // changes when the list changes
bool ready();                          // file system mounted
std::uint32_t freeBytes();
std::uint32_t usedBytes();

}  // namespace share
