#pragma once

#include <Arduino.h>

// Power bank link over the USB-C port.
//
// The onecell power bank firmware (CH9347X branches) exposes its battery data through a WCH CH9347X
// bridge once the user clicks the key three times ("keep PD" comm mode: the bank keeps powering
// the port). The bridge enumerates as a USB MIDI device (1A86:E208) or, in HID mode, 1A86:FE30,
// and carries the TLV protocol of Nvtmonitor (battery_monitor/protocol/tlv.py).
//
// The ESP32-S3 has a single USB controller, so the board is either a USB device for the PC
// (CDC + HID, the default) or a USB host for the power bank. The choice is a setting (USBMODE)
// that takes effect after a reboot; in PBANK mode commands reach the board over BLE / Wi-Fi only.
namespace pbank {

enum class Link : std::uint8_t {
    Off,        // PC mode: the USB host stack is not running
    Waiting,    // host running, nothing plugged in
    Foreign,    // a USB device that is not the power bank bridge
    Starting,   // bridge claimed, waiting for the first answer
    Online,     // answering queries
    NoReply,    // bridge present, but the power bank does not answer
    Error,      // the USB host stack failed to start
};

struct Data {
    bool valid = false;               // at least one complete realtime answer
    std::uint32_t ageMs = 0;          // since that answer (set by snapshot())
    std::uint8_t soc = 0;             // %
    std::uint8_t soh = 0;             // %
    std::uint8_t cells = 0;
    std::uint16_t cellMv[2] = {0, 0};
    bool tempValid = false;
    float tempC = 0.0f;
    std::uint8_t inPorts = 0;         // charging input (bank is being charged)
    std::uint8_t inType = 0;
    std::uint16_t inMv = 0;
    std::int16_t inMa = 0;
    std::uint8_t outPorts = 0;        // discharging output (bank powers something, incl. this board)
    std::uint8_t outType = 0;
    std::uint16_t outMv = 0;
    std::int16_t outMa = 0;
    std::uint32_t powerMw = 0;
    bool cyclesValid = false;
    std::uint32_t cycles = 0;
    char model[20] = "";
    char manufacturer[44] = "";
    char serial[36] = "";
    char prodDate[12] = "";
    char protocol[16] = "";
    char chip[10] = "";
    std::uint32_t rounds = 0;         // completed realtime answers
    std::uint32_t timeouts = 0;
};

void begin();                 // PBANK mode only: installs the USB host stack and its tasks
bool active();                // host stack running
Link link();
const char* linkName(Link link);
Data snapshot();
// Battery current seen by the bank: + charging, - discharging (mA).
std::int32_t batteryCurrentMa(const Data& data);
String statusLine();          // reply of PB?
String logText();             // reply of PBLOG?

}  // namespace pbank
