#include "pbank.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <usb/usb_host.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

// Reference implementations of the link (same byte layout, verified against the bank firmware):
//   Android  app/src/main/java/com/arun/batterymonitor/{UsbCh9347Link,Ch9347Sysex,TlvProtocol}.java
//   Windows  Nvtmonitor battery_monitor/protocol/tlv.py
namespace pbank {
namespace {

// ------------------------------------------------------------ constants
constexpr std::uint16_t kVendorWch = 0x1A86;
constexpr std::uint8_t kClassAudio = 0x01;
constexpr std::uint8_t kSubclassMidiStreaming = 0x03;
constexpr std::uint8_t kClassHid = 0x03;

// CH9347X SysEx wrapper: F0 00 22 21 <cmd> <N> <ceil(N/2) groups of 3> F7, N <= 32 raw bytes.
constexpr std::uint8_t kSysexData = 0x00;
constexpr std::uint8_t kSysexControl = 0x55;
constexpr std::uint8_t kSysexIdentifyReply = 0x2A;
constexpr int kSysexMaxRaw = 32;

// TLV frame: head(1) length(1) type(1) len(1) value(n) crc8(1), length = n + 3.
constexpr std::uint8_t kHeadRequest = 0xAA;
constexpr std::uint8_t kHeadResponse = 0x55;
constexpr int kFrameMax = 46;
constexpr std::uint8_t kCmdQuery = 0x80;
constexpr std::uint8_t kQueryRealtime = 0x03;
constexpr std::uint8_t kQueryDeviceInfo = 0x04;
constexpr std::uint8_t kTagCells = 0x01;
constexpr std::uint8_t kTagTemperature = 0x02;
constexpr std::uint8_t kTagInput = 0x03;
constexpr std::uint8_t kTagOutput = 0x04;
constexpr std::uint8_t kTagPower = 0x05;
constexpr std::uint8_t kTagSoc = 0x07;
constexpr std::uint8_t kTagSoh = 0x08;
constexpr std::uint8_t kTagManufacturer = 0x21;
constexpr std::uint8_t kTagModel = 0x22;
constexpr std::uint8_t kTagSerial = 0x23;
constexpr std::uint8_t kTagProdDate = 0x24;
constexpr std::uint8_t kTagCycles = 0x28;
constexpr std::uint8_t kTagProtocol = 0x41;
constexpr std::uint8_t kTagSendComplete = 0x90;

constexpr std::uint32_t kSettleMs = 300;          // after claiming, before the first byte goes out
constexpr std::uint32_t kIdentifyWaitMs = 400;
constexpr std::uint32_t kReadyWaitMs = 2500;      // the bank ignores the bridge for ~2 s after EN
constexpr std::uint32_t kRealtimeMs = 500;
constexpr std::uint32_t kDeviceInfoMs = 30000;
constexpr std::uint32_t kInfoRetryMs = 5000;
constexpr std::uint32_t kQueryTimeoutMs = 3000;
constexpr int kNoReplyTimeouts = 3;

constexpr int kBufferSize = 64;                   // full-speed bulk / interrupt packet

// ------------------------------------------------------------ shared state (loop task <-> USB task)
SemaphoreHandle_t lock = nullptr;
Data shared;
std::uint32_t sharedRealtimeMs = 0;
volatile Link linkState = Link::Off;
bool started = false;

constexpr int kLogLines = 20;
constexpr int kLogWidth = 80;
char logRing[kLogLines][kLogWidth];
int logNext = 0;
int logUsed = 0;

void trace(const char* format, ...) {
    char line[kLogWidth];
    const std::uint32_t ms = millis();
    int used = snprintf(line, sizeof(line), "%lu.%lu ", static_cast<unsigned long>(ms / 1000),
        static_cast<unsigned long>((ms / 100) % 10));
    if (used < 0 || used >= static_cast<int>(sizeof(line))) {
        used = 0;
    }
    va_list args;
    va_start(args, format);
    vsnprintf(line + used, sizeof(line) - used, format, args);
    va_end(args);
    if (lock == nullptr) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    memcpy(logRing[logNext], line, sizeof(line));
    logNext = (logNext + 1) % kLogLines;
    if (logUsed < kLogLines) {
        ++logUsed;
    }
    xSemaphoreGive(lock);
}

// ------------------------------------------------------------ USB host state (USB task only)
enum class Mode : std::uint8_t { None, Midi, Hid };

usb_host_client_handle_t client = nullptr;
usb_device_handle_t device = nullptr;
volatile std::uint8_t newAddress = 0;     // set by the client event callback
volatile bool deviceGone = false;
Mode mode = Mode::None;
bool claimed = false;
bool closing = false;
std::uint8_t interfaceNumber = 0;
std::uint8_t epIn = 0;
std::uint8_t epOut = 0;                   // 0: HID without an OUT endpoint, reports go by SET_REPORT
std::uint16_t mpsIn = kBufferSize;
std::uint16_t mpsOut = kBufferSize;
usb_transfer_t* xferIn = nullptr;
usb_transfer_t* xferOut = nullptr;
usb_transfer_t* xferCtrl = nullptr;
volatile bool inBusy = false;
volatile bool outBusy = false;
volatile bool ctrlBusy = false;
int inErrors = 0;

struct Session {
    std::uint32_t openedMs = 0;
    bool identifySent = false;
    std::uint32_t identifyMs = 0;
    bool identified = false;
    bool firmwareReady = false;   // any byte from the bank, including its 0x00 keep-alive
    bool polling = false;
    std::uint8_t inFlight = 0;    // query waiting for its 0x90, 0 = idle
    std::uint32_t deadline = 0;
    std::uint32_t lastRealtime = 0;
    std::uint32_t lastInfo = 0;
    bool infoDone = false;
    int timeoutsInRow = 0;
};
Session session;
Data work;                        // filled frame by frame, published when a query completes

// SysEx assembler
std::uint8_t sysex[96];
int sysexLength = 0;
bool inSysex = false;

// TLV stream parser
std::uint8_t frame[kFrameMax];
int frameLength = 0;
int frameExpect = 0;
int parseState = 0;               // 0 head, 1 length, 2 body

// string fragments: value = [index, total, data...], used by the 40-byte string fields
std::uint8_t fragTag = 0;
int fragTotal = 0;
int fragNext = 0;
char fragText[48];
int fragLength = 0;

void publish(bool realtimeDone) {
    xSemaphoreTake(lock, portMAX_DELAY);
    shared = work;
    if (realtimeDone) {
        sharedRealtimeMs = millis();
    }
    xSemaphoreGive(lock);
}

// ------------------------------------------------------------ helpers
std::uint8_t crc8(const std::uint8_t* data, int length) {
    // Polynomial 0x31, init 0, MSB first, no reflection (NOT CRC-8/MAXIM). AA 04 FF 01 01 -> 0x62.
    std::uint8_t crc = 0;
    for (int i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) ? static_cast<std::uint8_t>((crc << 1) ^ 0x31) : static_cast<std::uint8_t>(crc << 1);
        }
    }
    return crc;
}

std::uint16_t u16(const std::uint8_t* v) {
    return static_cast<std::uint16_t>(v[0] | (v[1] << 8));
}

std::uint32_t u32(const std::uint8_t* v) {
    return static_cast<std::uint32_t>(v[0]) | (static_cast<std::uint32_t>(v[1]) << 8)
        | (static_cast<std::uint32_t>(v[2]) << 16) | (static_cast<std::uint32_t>(v[3]) << 24);
}

// Copies bytes up to the first 0x00 / 0xFF, dropping control characters.
void copyText(char* out, std::size_t size, const std::uint8_t* v, int length) {
    std::size_t used = 0;
    for (int i = 0; i < length && used + 1 < size; ++i) {
        if (v[i] == 0x00 || v[i] == 0xFF) {
            break;
        }
        if (v[i] >= 0x20) {
            out[used++] = static_cast<char>(v[i]);
        }
    }
    while (used > 0 && out[used - 1] == ' ') {
        --used;
    }
    out[used] = '\0';
}

const char* chipName(std::uint8_t id) {
    switch (id) {
        case 0x00: return "CH9347D";
        case 0x01: return "CH9347J";
        case 0x02: return "CH9347X";
        case 0x03: return "CH9347A";
        default: return "CH9347?";
    }
}

// ------------------------------------------------------------ USB transfers
void onInDone(usb_transfer_t* transfer);
void onOutDone(usb_transfer_t* transfer);
void onCtrlDone(usb_transfer_t* transfer);

void submitIn() {
    if (!claimed || closing || deviceGone || xferIn == nullptr || inBusy) {
        return;
    }
    xferIn->device_handle = device;
    xferIn->bEndpointAddress = epIn;
    xferIn->callback = onInDone;
    xferIn->context = nullptr;
    xferIn->num_bytes = mpsIn;
    if (usb_host_transfer_submit(xferIn) == ESP_OK) {
        inBusy = true;
    } else {
        trace("IN submit failed");
    }
}

bool submitOut(int length) {
    xferOut->device_handle = device;
    xferOut->bEndpointAddress = epOut;
    xferOut->callback = onOutDone;
    xferOut->context = nullptr;
    xferOut->num_bytes = length;
    if (usb_host_transfer_submit(xferOut) != ESP_OK) {
        trace("OUT submit failed");
        return false;
    }
    outBusy = true;
    return true;
}

// raw MIDI byte stream (complete SysEx messages) -> USB-MIDI event packets, cable 0
int toUsbMidi(const std::uint8_t* stream, int length, std::uint8_t* out, int capacity) {
    int used = 0;
    int start = 0;
    for (int i = 0; i < length; ++i) {
        if (stream[i] != 0xF7) {
            continue;
        }
        int index = start;
        const int end = i + 1;
        while (end - index > 3) {
            if (used + 4 > capacity) {
                return -1;
            }
            out[used++] = 0x04;                       // SysEx starts or continues
            out[used++] = stream[index];
            out[used++] = stream[index + 1];
            out[used++] = stream[index + 2];
            index += 3;
        }
        const int rest = end - index;                 // 1..3, the last one is F7
        if (used + 4 > capacity) {
            return -1;
        }
        out[used++] = static_cast<std::uint8_t>(0x04 + rest);   // 5 / 6 / 7: SysEx ends
        for (int k = 0; k < 3; ++k) {
            out[used++] = k < rest ? stream[index + k] : 0;
        }
        start = end;
    }
    return used;
}

int encodeSysex(std::uint8_t cmd, const std::uint8_t* raw, int length, std::uint8_t* out) {
    out[0] = 0xF0;
    out[1] = 0x00;
    out[2] = 0x22;
    out[3] = 0x21;
    out[4] = static_cast<std::uint8_t>(cmd & 0x7F);
    out[5] = static_cast<std::uint8_t>(length & 0x7F);
    int p = 6;
    for (int i = 0; i < length; i += 2) {
        const std::uint8_t r0 = raw[i];
        const std::uint8_t r1 = i + 1 < length ? raw[i + 1] : 0;
        out[p++] = static_cast<std::uint8_t>(r0 & 0x7F);
        out[p++] = static_cast<std::uint8_t>(((r0 >> 7) & 1) | (((r1 >> 7) & 1) << 1));
        out[p++] = static_cast<std::uint8_t>(r1 & 0x7F);
    }
    out[p++] = 0xF7;
    return p;
}

bool sendSysex(std::uint8_t cmd, const std::uint8_t* raw, int length) {
    if (!claimed || outBusy || epOut == 0 || length > kSysexMaxRaw) {
        return false;
    }
    std::uint8_t message[8 + kSysexMaxRaw * 3 / 2 + 4];
    const int size = encodeSysex(cmd, raw, length, message);
    const int packets = toUsbMidi(message, size, xferOut->data_buffer, static_cast<int>(xferOut->data_buffer_size));
    if (packets <= 0) {
        return false;
    }
    return submitOut(packets);
}

// One TLV frame to the power bank.
bool sendRaw(const std::uint8_t* raw, int length) {
    if (!claimed || length <= 0 || length > kBufferSize - 1) {
        return false;
    }
    if (mode == Mode::Midi) {
        return sendSysex(kSysexData, raw, length);
    }
    // HID: 64-byte report [len][data...]
    if (epOut != 0) {
        if (outBusy) {
            return false;
        }
        memset(xferOut->data_buffer, 0, kBufferSize);
        xferOut->data_buffer[0] = static_cast<std::uint8_t>(length);
        memcpy(xferOut->data_buffer + 1, raw, length);
        return submitOut(mpsOut);
    }
    if (ctrlBusy || xferCtrl == nullptr) {
        return false;
    }
    std::uint8_t* buffer = xferCtrl->data_buffer;
    usb_setup_packet_t* setup = reinterpret_cast<usb_setup_packet_t*>(buffer);
    setup->bmRequestType = 0x21;          // class, interface, host -> device
    setup->bRequest = 0x09;               // SET_REPORT
    setup->wValue = 0x0200;               // output report, id 0
    setup->wIndex = interfaceNumber;
    setup->wLength = kBufferSize;
    memset(buffer + sizeof(usb_setup_packet_t), 0, kBufferSize);
    buffer[sizeof(usb_setup_packet_t)] = static_cast<std::uint8_t>(length);
    memcpy(buffer + sizeof(usb_setup_packet_t) + 1, raw, length);
    xferCtrl->device_handle = device;
    xferCtrl->bEndpointAddress = 0;
    xferCtrl->callback = onCtrlDone;
    xferCtrl->context = nullptr;
    xferCtrl->num_bytes = sizeof(usb_setup_packet_t) + kBufferSize;
    if (usb_host_transfer_submit_control(client, xferCtrl) != ESP_OK) {
        trace("SET_REPORT submit failed");
        return false;
    }
    ctrlBusy = true;
    return true;
}

// ------------------------------------------------------------ TLV session
void endQuery(bool completed) {
    const std::uint8_t query = session.inFlight;
    session.inFlight = 0;
    if (!completed) {
        return;
    }
    session.timeoutsInRow = 0;
    if (query == kQueryRealtime) {
        work.valid = true;
        ++work.rounds;
    } else if (query == kQueryDeviceInfo) {
        session.infoDone = true;
    }
    if (linkState != Link::Online) {
        linkState = Link::Online;
        trace("power bank online");
    }
    publish(query == kQueryRealtime);
}

void startQuery(std::uint8_t sub, std::uint32_t now) {
    std::uint8_t request[6] = {kHeadRequest, 4, kCmdQuery, 1, sub, 0};
    request[5] = crc8(request, 5);
    if (!sendRaw(request, sizeof(request))) {
        return;                       // OUT still busy: try again on the next tick
    }
    session.inFlight = sub;
    session.deadline = now + kQueryTimeoutMs;
}

void textField(std::uint8_t tag, const std::uint8_t* v, int n, char* out, std::size_t size) {
    const bool fragment = n >= 3 && v[0] < v[1] && v[1] == 2;   // 40-byte strings come in <= 2 parts
    if (!fragment) {
        fragTotal = 0;
        copyText(out, size, v, n);
        return;
    }
    if (v[0] == 0) {
        fragTag = tag;
        fragTotal = v[1];
        fragNext = 0;
        fragLength = 0;
    }
    if (fragTag != tag || fragTotal != v[1] || fragNext != v[0]) {
        fragTotal = 0;
        return;
    }
    for (int i = 2; i < n && fragLength < static_cast<int>(sizeof(fragText)); ++i) {
        fragText[fragLength++] = static_cast<char>(v[i]);
    }
    if (++fragNext >= fragTotal) {
        copyText(out, size, reinterpret_cast<const std::uint8_t*>(fragText), fragLength);
        fragTotal = 0;
    }
}

void handleFrame() {
    const int n = frame[3];
    if (n != frameExpect - 3 || crc8(frame, frameLength - 1) != frame[frameLength - 1]) {
        trace("bad frame type %02X", frame[2]);
        return;
    }
    if (frame[0] != kHeadResponse) {
        return;
    }
    const std::uint8_t* v = frame + 4;
    switch (frame[2]) {
        case kCmdQuery:                       // ACK: [sub, rc]
            if (n >= 2 && v[0] == session.inFlight && v[1] != 0) {
                trace("query %02X refused rc=%02X", v[0], v[1]);
                endQuery(false);
            }
            break;
        case kTagCells:
            if (n >= 1) {
                work.cells = v[0];
                work.cellMv[0] = (v[0] >= 1 && n >= 3) ? u16(v + 1) : 0;
                work.cellMv[1] = (v[0] >= 2 && n >= 5) ? u16(v + 3) : 0;
            }
            break;
        case kTagTemperature:
            if (n >= 3 && v[0] >= 1) {
                work.tempC = static_cast<std::int16_t>(u16(v + 1)) / 10.0f;
                work.tempValid = true;
            }
            break;
        case kTagInput:
        case kTagOutput: {
            // [N, (port type, mV u16, mA s16) * N]; N = 0: no port in that direction
            const bool present = n >= 6 && v[0] >= 1;
            const std::uint8_t ports = n >= 1 ? v[0] : 0;
            const std::uint8_t type = present ? v[1] : 0;
            const std::uint16_t mv = present ? u16(v + 2) : 0;
            const std::int16_t ma = present ? static_cast<std::int16_t>(u16(v + 4)) : 0;
            if (frame[2] == kTagInput) {
                work.inPorts = ports;
                work.inType = type;
                work.inMv = mv;
                work.inMa = ma;
            } else {
                work.outPorts = ports;
                work.outType = type;
                work.outMv = mv;
                work.outMa = ma;
            }
            break;
        }
        case kTagPower:
            if (n >= 4) {
                work.powerMw = u32(v);
            }
            break;
        case kTagSoc:
            if (n >= 1) {
                work.soc = v[0];
            }
            break;
        case kTagSoh:
            if (n >= 1) {
                work.soh = v[0];
            }
            break;
        case kTagManufacturer:
            textField(frame[2], v, n, work.manufacturer, sizeof(work.manufacturer));
            break;
        case kTagModel:
            copyText(work.model, sizeof(work.model), v, n);
            break;
        case kTagSerial:
            textField(frame[2], v, n, work.serial, sizeof(work.serial));
            break;
        case kTagProdDate:
            if (n == 3) {
                int d[3];
                bool ok = true;
                for (int i = 0; i < 3; ++i) {
                    const int hi = v[i] >> 4;
                    const int lo = v[i] & 0x0F;
                    ok = ok && hi <= 9 && lo <= 9;
                    d[i] = hi * 10 + lo;
                }
                if (ok && d[1] >= 1 && d[1] <= 12 && d[2] >= 1 && d[2] <= 31) {
                    snprintf(work.prodDate, sizeof(work.prodDate), "%04d-%02d-%02d", 2000 + d[0], d[1], d[2]);
                }
            }
            break;
        case kTagCycles:
            if (n >= 4) {
                work.cycles = u32(v) & 0xFFFF;
                work.cyclesValid = true;
            }
            break;
        case kTagProtocol:
            if (n >= 4) {
                snprintf(work.protocol, sizeof(work.protocol), "%u.%u.%u.%u", v[0], v[1], v[2], v[3]);
            }
            break;
        case kTagSendComplete:
            if (session.inFlight != 0) {
                endQuery(true);
            }
            break;
        default:
            break;
    }
}

void feedTlv(const std::uint8_t* data, int length) {
    for (int i = 0; i < length; ++i) {
        const std::uint8_t b = data[i];
        if (parseState == 0) {
            // skips everything outside a frame, including the bank's 0x00 keep-alive every ~188 ms
            if (b == kHeadRequest || b == kHeadResponse) {
                frame[0] = b;
                frameLength = 1;
                parseState = 1;
            }
        } else if (parseState == 1) {
            if (b < 3 || b > kFrameMax - 2) {
                parseState = 0;
                continue;
            }
            frameExpect = b;
            frame[1] = b;
            frameLength = 2;
            parseState = 2;
        } else {
            frame[frameLength++] = b;
            if (frameLength >= 2 + frameExpect) {
                handleFrame();
                parseState = 0;
            } else if (frameLength >= kFrameMax) {
                parseState = 0;
            }
        }
    }
}

void onSysex(const std::uint8_t* message, int length) {
    if (length < 7 || message[1] != 0x00 || message[2] != 0x22 || message[3] != 0x21) {
        return;
    }
    const std::uint8_t cmd = message[4] & 0x7F;
    if (cmd == kSysexIdentifyReply) {
        if (length == 10 && message[5] == 0x03) {
            session.identified = true;
            snprintf(work.chip, sizeof(work.chip), "%s", chipName(message[8]));
            trace("bridge %s", work.chip);
        }
        return;
    }
    if (cmd != kSysexData) {
        return;
    }
    const int declared = message[5] & 0x7F;
    std::uint8_t raw[kSysexMaxRaw + 2];
    int used = 0;
    for (int p = 6; p + 2 < length - 1 && used < declared && used < kSysexMaxRaw; p += 3) {
        const std::uint8_t a = message[p] & 0x7F;
        const std::uint8_t b = message[p + 1] & 0x7F;
        const std::uint8_t c = message[p + 2] & 0x7F;
        raw[used++] = static_cast<std::uint8_t>(a | ((b & 1) << 7));
        if (used < declared) {
            raw[used++] = static_cast<std::uint8_t>(c | ((b & 2) << 6));
        }
    }
    if (used > 0) {
        session.firmwareReady = true;
        feedTlv(raw, used);
    }
}

void feedMidiByte(std::uint8_t b) {
    if (b == 0xF0) {
        sysex[0] = b;
        sysexLength = 1;
        inSysex = true;
    } else if (b == 0xF7) {
        if (inSysex && sysexLength < static_cast<int>(sizeof(sysex))) {
            sysex[sysexLength++] = b;
            onSysex(sysex, sysexLength);
        }
        inSysex = false;
        sysexLength = 0;
    } else if (inSysex) {
        if (b < 0x80) {
            if (sysexLength < static_cast<int>(sizeof(sysex))) {
                sysex[sysexLength++] = b;
            } else {
                inSysex = false;          // longer than any CH9347X message
            }
        } else if (b < 0xF8) {
            inSysex = false;              // another status byte broke the SysEx
        }
        // F8..FF real-time bytes may sit inside a SysEx: ignored
    }
}

void onUsbData(const std::uint8_t* data, int length) {
    if (mode == Mode::Midi) {
        for (int p = 0; p + 4 <= length; p += 4) {
            int count = 0;
            switch (data[p] & 0x0F) {             // code index number -> MIDI bytes in the packet
                case 0x3: case 0x4: case 0x7: case 0x8: case 0x9: case 0xA: case 0xB: case 0xE: count = 3; break;
                case 0x2: case 0x6: case 0xC: case 0xD: count = 2; break;
                case 0x5: case 0xF: count = 1; break;
                default: count = 0; break;
            }
            for (int k = 0; k < count; ++k) {
                feedMidiByte(data[p + 1 + k]);
            }
        }
        return;
    }
    // HID report [len][data...] (or [0][len][data...] with a report id)
    int offset = 0;
    if (length == kBufferSize + 1 && data[0] == 0) {
        offset = 1;
    }
    const int count = data[offset];
    if (count > 0 && offset + 1 + count <= length) {
        session.firmwareReady = true;
        feedTlv(data + offset + 1, count);
    }
}

void onInDone(usb_transfer_t* transfer) {
    inBusy = false;
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        inErrors = 0;
        if (transfer->actual_num_bytes > 0) {
            onUsbData(transfer->data_buffer, transfer->actual_num_bytes);
        }
    } else if (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE || transfer->status == USB_TRANSFER_STATUS_CANCELED) {
        return;
    } else if (++inErrors > 20) {
        trace("IN error %d, reading stopped", static_cast<int>(transfer->status));
        return;
    }
    submitIn();
}

void onOutDone(usb_transfer_t* transfer) {
    outBusy = false;
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED && transfer->status != USB_TRANSFER_STATUS_CANCELED
        && transfer->status != USB_TRANSFER_STATUS_NO_DEVICE) {
        trace("OUT error %d", static_cast<int>(transfer->status));
    }
}

void onCtrlDone(usb_transfer_t* transfer) {
    ctrlBusy = false;
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED && transfer->status != USB_TRANSFER_STATUS_CANCELED
        && transfer->status != USB_TRANSFER_STATUS_NO_DEVICE) {
        trace("SET_REPORT error %d", static_cast<int>(transfer->status));
    }
}

void serviceSession(std::uint32_t now) {
    if (!claimed || closing) {
        return;
    }
    if (now - session.openedMs < kSettleMs) {
        return;
    }
    if (mode == Mode::Midi && !session.identifySent) {
        static const std::uint8_t zeros[6] = {0, 0, 0, 0, 0, 0};
        if (sendSysex(kSysexControl, zeros, sizeof(zeros))) {
            session.identifySent = true;
            session.identifyMs = now;
        }
        return;
    }
    if (!session.polling) {
        const bool identifyDone = mode != Mode::Midi || session.identified || now - session.identifyMs >= kIdentifyWaitMs;
        const bool ready = session.firmwareReady || now - session.openedMs >= kSettleMs + kReadyWaitMs;
        if (!identifyDone || !ready) {
            return;
        }
        session.polling = true;
        trace("%s", session.firmwareReady ? "bank alive, polling" : "no keep-alive yet, polling anyway");
    }
    if (session.inFlight != 0) {
        if (static_cast<std::int32_t>(now - session.deadline) >= 0) {
            trace("query %02X timeout", session.inFlight);
            session.inFlight = 0;
            ++work.timeouts;
            if (++session.timeoutsInRow >= kNoReplyTimeouts && linkState != Link::NoReply) {
                linkState = Link::NoReply;
                trace("power bank not answering");
                publish(false);
            }
        }
        return;
    }
    const bool infoDue = session.infoDone ? now - session.lastInfo >= kDeviceInfoMs
                                          : (session.lastInfo == 0 || now - session.lastInfo >= kInfoRetryMs);
    if (infoDue) {
        session.lastInfo = now;
        startQuery(kQueryDeviceInfo, now);
    } else if (now - session.lastRealtime >= kRealtimeMs) {
        session.lastRealtime = now;
        startQuery(kQueryRealtime, now);
    }
}

// ------------------------------------------------------------ device open / close
void freeTransfer(usb_transfer_t*& transfer, bool busy, const char* name) {
    if (transfer == nullptr) {
        return;
    }
    if (busy) {
        trace("%s transfer still busy, kept", name);   // freeing it would corrupt the stack
        transfer = nullptr;
        return;
    }
    usb_host_transfer_free(transfer);
    transfer = nullptr;
}

void resetEndpoint(std::uint8_t address) {
    if (address == 0) {
        return;
    }
    usb_host_endpoint_halt(device, address);
    usb_host_endpoint_flush(device, address);
    usb_host_endpoint_clear(device, address);
}

void closeDevice() {
    closing = true;
    if (claimed) {
        resetEndpoint(epIn);
        resetEndpoint(epOut);
        const std::uint32_t start = millis();
        while ((inBusy || outBusy || ctrlBusy) && millis() - start < 500) {
            usb_host_client_handle_events(client, pdMS_TO_TICKS(10));   // runs the cancelled callbacks
        }
        const esp_err_t err = usb_host_interface_release(client, device, interfaceNumber);
        if (err != ESP_OK) {
            trace("release failed %d", static_cast<int>(err));
        }
        claimed = false;
    }
    freeTransfer(xferIn, inBusy, "IN");
    freeTransfer(xferOut, outBusy, "OUT");
    freeTransfer(xferCtrl, ctrlBusy, "CTRL");
    if (device != nullptr) {
        usb_host_device_close(client, device);
        device = nullptr;
    }
    mode = Mode::None;
    epIn = 0;
    epOut = 0;
    inBusy = outBusy = ctrlBusy = false;
    session = Session();
    work = Data();
    linkState = Link::Waiting;
    publish(false);
    closing = false;
}

struct Candidate {
    bool used = false;
    std::uint8_t number = 0;
    std::uint8_t alt = 0;
    std::uint8_t in = 0;
    std::uint8_t out = 0;
    std::uint16_t mpsIn = 0;
    std::uint16_t mpsOut = 0;
};

void openDevice(std::uint8_t address, std::uint32_t now) {
    if (device != nullptr) {
        trace("second USB device ignored");
        return;
    }
    if (usb_host_device_open(client, address, &device) != ESP_OK) {
        device = nullptr;
        trace("device open failed");
        return;
    }
    const usb_device_desc_t* desc = nullptr;
    const usb_config_desc_t* config = nullptr;
    usb_host_get_device_descriptor(device, &desc);
    usb_host_get_active_config_descriptor(device, &config);
    if (desc == nullptr || config == nullptr) {
        trace("no descriptors");
        linkState = Link::Foreign;
        publish(false);
        return;                         // stays open so DEV_GONE arrives when it is unplugged
    }
    trace("USB %04X:%04X", desc->idVendor, desc->idProduct);
    if (desc->idVendor != kVendorWch) {
        linkState = Link::Foreign;
        publish(false);
        return;
    }

    // Find the MIDI streaming interface (bulk IN + OUT), or a HID interface (interrupt IN).
    Candidate midi;
    Candidate hid;
    const usb_intf_desc_t* intf = nullptr;
    const usb_standard_desc_t* d = reinterpret_cast<const usb_standard_desc_t*>(config);
    int offset = 0;
    while ((d = usb_parse_next_descriptor(d, config->wTotalLength, &offset)) != nullptr) {
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            intf = reinterpret_cast<const usb_intf_desc_t*>(d);
            continue;
        }
        if (d->bDescriptorType != USB_B_DESCRIPTOR_TYPE_ENDPOINT || intf == nullptr) {
            continue;
        }
        const usb_ep_desc_t* ep = reinterpret_cast<const usb_ep_desc_t*>(d);
        const std::uint8_t type = ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK;
        Candidate* c = nullptr;
        if (intf->bInterfaceClass == kClassAudio && intf->bInterfaceSubClass == kSubclassMidiStreaming
            && type == USB_BM_ATTRIBUTES_XFER_BULK) {
            c = &midi;
        } else if (intf->bInterfaceClass == kClassHid && type == USB_BM_ATTRIBUTES_XFER_INT) {
            c = &hid;
        }
        if (c == nullptr || (c->used && c->number != intf->bInterfaceNumber)) {
            continue;
        }
        c->used = true;
        c->number = intf->bInterfaceNumber;
        c->alt = intf->bAlternateSetting;
        const std::uint16_t mps = USB_EP_DESC_GET_MPS(ep);
        if ((ep->bEndpointAddress & 0x80) != 0) {
            if (c->in == 0) {
                c->in = ep->bEndpointAddress;
                c->mpsIn = mps;
            }
        } else if (c->out == 0) {
            c->out = ep->bEndpointAddress;
            c->mpsOut = mps;
        }
    }

    const Candidate* pick = nullptr;
    if (midi.in != 0 && midi.out != 0) {
        pick = &midi;
        mode = Mode::Midi;
    } else if (hid.in != 0) {
        pick = &hid;
        mode = Mode::Hid;
    } else {
        trace("no MIDI / HID interface");
        linkState = Link::Foreign;
        publish(false);
        return;
    }
    if (pick->mpsIn == 0 || pick->mpsIn > kBufferSize || (pick->out != 0 && (pick->mpsOut == 0 || pick->mpsOut > kBufferSize))) {
        trace("unexpected packet size %u/%u", pick->mpsIn, pick->mpsOut);
        mode = Mode::None;
        linkState = Link::Foreign;
        publish(false);
        return;
    }
    const esp_err_t err = usb_host_interface_claim(client, device, pick->number, pick->alt);
    if (err != ESP_OK) {
        trace("claim if%u failed %d", pick->number, static_cast<int>(err));
        mode = Mode::None;
        linkState = Link::Foreign;
        publish(false);
        return;
    }
    claimed = true;
    interfaceNumber = pick->number;
    epIn = pick->in;
    epOut = pick->out;
    mpsIn = pick->mpsIn;
    mpsOut = pick->out != 0 ? pick->mpsOut : kBufferSize;
    usb_host_transfer_alloc(kBufferSize, 0, &xferIn);
    usb_host_transfer_alloc(kBufferSize, 0, &xferOut);
    if (mode == Mode::Hid && epOut == 0) {
        usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + kBufferSize, 0, &xferCtrl);
    }
    if (xferIn == nullptr || xferOut == nullptr || (mode == Mode::Hid && epOut == 0 && xferCtrl == nullptr)) {
        trace("transfer alloc failed");
        closeDevice();
        return;
    }
    trace("%s if%u in=%02X out=%02X", mode == Mode::Midi ? "MIDI" : "HID", interfaceNumber, epIn, epOut);

    session = Session();
    session.openedMs = now;
    work = Data();
    snprintf(work.chip, sizeof(work.chip), "%s", mode == Mode::Midi ? "CH9347" : "CH9347 HID");
    parseState = 0;
    inSysex = false;
    sysexLength = 0;
    fragTotal = 0;
    inErrors = 0;
    linkState = Link::Starting;
    publish(false);
    submitIn();
}

// ------------------------------------------------------------ tasks
void onClientEvent(const usb_host_client_event_msg_t* event, void* arg) {
    (void)arg;
    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        newAddress = event->new_dev.address;
    } else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        deviceGone = true;
    }
}

void libraryTask(void* arg) {
    (void)arg;
    for (;;) {
        std::uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if ((flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) != 0) {
            usb_host_device_free_all();
        }
    }
}

void clientTask(void* arg) {
    (void)arg;
    usb_host_client_config_t config = {};
    config.is_synchronous = false;
    config.max_num_event_msg = 5;
    config.async.client_event_callback = onClientEvent;
    config.async.callback_arg = nullptr;
    if (usb_host_client_register(&config, &client) != ESP_OK) {
        linkState = Link::Error;
        trace("client register failed");
        vTaskDelete(nullptr);
        return;
    }
    for (;;) {
        usb_host_client_handle_events(client, pdMS_TO_TICKS(20));   // runs event + transfer callbacks
        const std::uint32_t now = millis();
        if (deviceGone) {
            deviceGone = false;
            trace("unplugged");
            closeDevice();
        }
        if (newAddress != 0) {
            const std::uint8_t address = newAddress;
            newAddress = 0;
            openDevice(address, now);
        }
        serviceSession(now);
    }
}

}  // namespace

void begin() {
    if (started) {
        return;
    }
    lock = xSemaphoreCreateMutex();
    usb_host_config_t config = {};
    config.skip_phy_setup = false;
    config.intr_flags = ESP_INTR_FLAG_LEVEL1;
    const esp_err_t err = usb_host_install(&config);
    if (err != ESP_OK) {
        linkState = Link::Error;
        trace("usb_host_install failed %d", static_cast<int>(err));
        return;
    }
    started = true;
    linkState = Link::Waiting;
    xTaskCreatePinnedToCore(libraryTask, "usbLib", 4096, nullptr, 3, nullptr, 0);
    xTaskCreatePinnedToCore(clientTask, "usbPbank", 6144, nullptr, 4, nullptr, 0);
    trace("USB host on, waiting for the power bank");
}

bool active() {
    return started;
}

Link link() {
    return linkState;
}

const char* linkName(Link value) {
    switch (value) {
        case Link::Off: return "OFF";
        case Link::Waiting: return "WAITING";
        case Link::Foreign: return "FOREIGN";
        case Link::Starting: return "STARTING";
        case Link::Online: return "ONLINE";
        case Link::NoReply: return "NOREPLY";
        case Link::Error: return "ERROR";
        default: return "?";
    }
}

Data snapshot() {
    Data copy;
    if (lock == nullptr) {
        return copy;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    copy = shared;
    const std::uint32_t at = sharedRealtimeMs;
    xSemaphoreGive(lock);
    copy.ageMs = copy.valid ? millis() - at : 0;
    return copy;
}

std::int32_t batteryCurrentMa(const Data& data) {
    const std::int32_t in = data.inMa < 0 ? -data.inMa : data.inMa;
    const std::int32_t out = data.outMa < 0 ? -data.outMa : data.outMa;
    return in != 0 ? in : -out;
}

namespace {

String token(const char* text) {
    String out;
    for (const char* p = text; *p != '\0'; ++p) {
        out += *p == ' ' ? '_' : *p;
    }
    return out.length() > 0 ? out : String("-");
}

}  // namespace

String statusLine() {
    if (!started) {
        return String("PB MODE=PC LINK=") + linkName(linkState);
    }
    const Data d = snapshot();
    String line = String("PB MODE=PBANK LINK=") + linkName(linkState);
    if (d.chip[0] != '\0') {
        line += String(" CHIP=") + token(d.chip);
    }
    if (!d.valid) {
        return line;
    }
    line += " AGE=" + String(d.ageMs / 1000.0f, 1);
    line += " SOC=" + String(d.soc) + " SOH=" + String(d.soh);
    line += " CELLS=" + String(d.cells) + " CELL1=" + String(d.cellMv[0] / 1000.0f, 3)
        + " CELL2=" + String(d.cellMv[1] / 1000.0f, 3);
    line += " TEMP=" + (d.tempValid ? String(d.tempC, 1) : String("-"));
    line += " IN=" + String(d.inMv) + "/" + String(d.inMa) + " OUT=" + String(d.outMv) + "/" + String(d.outMa);
    line += " I=" + String(batteryCurrentMa(d)) + " P=" + String(d.powerMw / 1000.0f, 3);
    line += " CYC=" + (d.cyclesValid ? String(d.cycles) : String("-"));
    line += " MODEL=" + token(d.model) + " SN=" + token(d.serial) + " DATE=" + token(d.prodDate)
        + " PROTO=" + token(d.protocol);
    line += " ROUNDS=" + String(d.rounds) + " TIMEOUTS=" + String(d.timeouts);
    return line;
}

String logText() {
    if (lock == nullptr) {
        return "PBLOG empty (PC mode)";
    }
    String out;
    xSemaphoreTake(lock, portMAX_DELAY);
    const int first = (logNext - logUsed + kLogLines) % kLogLines;
    for (int i = 0; i < logUsed; ++i) {
        if (i > 0) {
            out += '\n';
        }
        out += logRing[(first + i) % kLogLines];
    }
    xSemaphoreGive(lock);
    return out.length() > 0 ? out : String("PBLOG empty");
}

}  // namespace pbank
