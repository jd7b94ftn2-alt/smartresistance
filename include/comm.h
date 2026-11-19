#pragma once

#include <Arduino.h>

// Transports: USB CDC serial, BLE (Nordic UART service), Wi-Fi (HTTP page + API, TCP command
// port 5025, telnet serial bridge on port 23), UART bridge on J_DBG and the USB HID knob.
namespace comm {

constexpr std::uint16_t kCommandPort = 5025;
constexpr std::uint16_t kBridgePort = 23;
constexpr int kBridgeLineCount = 26;
constexpr int kBridgeLineLength = 53;

enum class KnobMode : std::uint8_t { Volume, Scroll, Media, Arrows, Count };

void begin();
void loop();

// ---- Wi-Fi ----
void restartWifi();                  // applies app::settings() credentials (empty SSID = AP only)
bool wifiConnected();
bool accessPointActive();
String ipAddress();
String accessPointName();
bool timeValid();
String weatherLine();                // last fetched text, empty until the first success
void requestWeather();               // fetched in loop() while connected, at most every 15 min

// ---- MQTT (internet remote control) ----
void restartMqtt();                  // applies app::settings() broker settings
bool mqttConnected();
String mqttStatus();                 // "off", "connecting", "connected", "no wifi", "error <n>"
String mqttTopicBase();              // smartr/<mac>; topics <base>/cmd, /reply, /state, /online

// ---- BLE ----
bool bleConnected();
String bleName();
void setBleEnabled(bool enabled);    // off: stop advertising and drop the current link
bool bleEnabled();
void setBleBridge(bool bridge);      // true: BLE <-> J_DBG UART bridge, false: text commands
bool bleBridge();
String blePeer();                    // address of the connected phone, empty when none

// ---- UART bridge (J_DBG TX/RX <-> telnet) ----
void setBridgeBaud(std::uint32_t baud);
std::uint32_t bridgeRevision();      // increments whenever the on-screen log changes
const char* bridgeLine(int row);     // row 0 = oldest visible line
void setBridgePaused(bool paused);
bool bridgePaused();
void clearBridgeLog();

// ---- USB HID knob ----
const char* knobModeName(KnobMode mode);
void knobRotate(KnobMode mode, int steps);
void knobClick(KnobMode mode);

}  // namespace comm
