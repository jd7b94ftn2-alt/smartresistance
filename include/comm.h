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

// False in power-bank mode: the USB port is a host for the power bank (pbank.h), so there is no
// USB serial and no USB HID (knob, shortcuts, media keys) until the board is switched back to PC.
bool usbDeviceActive();
bool usbHidConnected();              // PC mode and a PC has enumerated the USB port

// One-line JSON status (also /api/state, MQTT state and the STATE? command used by the web page).
String stateJson();

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

// ---- USB HID shortcut actions (Macro / Desk pages) ----
enum class Action : std::uint8_t {
    Screenshot, Lock, ShowDesktop, TaskManager, Copy, Paste, Undo, SwitchWindow,
    Mute, PlayPause, PrevTrack, NextTrack, FeishuMic, FeishuCamera, VolumeUp, VolumeDown, Count,
};
// Shortcuts go to USB HID when a PC has enumerated the board, otherwise to the Bluetooth keyboard.
void sendAction(Action action);

// ---- Bluetooth keyboard (BLE HID): phone remote, and shortcuts without the USB cable ----
// Phones / PCs pair once in their Bluetooth settings ("SmartR-XXXX", no PIN) and reconnect by themselves.
enum class Remote : std::uint8_t { VolumeUp, VolumeDown, PlayPause, Next, Prev, Shutter, Mute, PageUp, PageDown, Home, Count };
void sendRemote(Remote remote);      // always over Bluetooth; Shutter = volume up (iPhone camera)
const char* remoteName(Remote remote);
bool bleKeyboardActive();            // at least one paired host is connected and listening
int bleKeyboardHosts();
int bleBondCount();
void clearBlePairings();             // forget every paired phone / PC and drop their links

}  // namespace comm
