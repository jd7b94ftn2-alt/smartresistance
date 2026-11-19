#pragma once

#include <Arduino.h>

#include "app_state.h"
#include "ntc.h"

// Device-level API shared by the UI, the command parser and every transport (USB serial,
// BLE, TCP, HTTP). All of it runs on the Arduino loop task.
namespace app {

constexpr const char* kFirmwareVersion = "1.3.0";

// Relay indices for the raw/test interface: 0..35 = shift-chain bits (hardware/relay_map.csv),
// 36..39 = spare driver outputs, 40 = K_OUT.
constexpr int kRelayIndexCount = 41;
constexpr int kOutputRelayIndex = 40;

constexpr int kTempLogSize = 300;

struct Settings {
    String wifiSsid;
    String wifiPass;
    int ntcPreset = 0;              // kNtcPresetCount selects the user-defined type
    double userR25 = 10000.0;
    double userBeta = 3435.0;
    std::uint32_t lastOhms = 1000;
    bool solver = true;
    std::uint32_t bridgeBaud = 115200;
    std::uint8_t rotation = 1;
    std::uint8_t backlight = 100;   // percent
    std::uint8_t knobMode = 0;
    std::uint16_t logIntervalS = 1;
    std::uint8_t language = 0;      // 0 = Chinese, 1 = English (on-screen text only)
    bool bleEnabled = true;
    bool mqttEnabled = false;       // internet remote control through an MQTT broker
    String mqttHost;
    std::uint16_t mqttPort = 8883;  // 8883 = TLS, 1883 = plain
    String mqttUser;
    String mqttPass;
    bool bleBridge = false;         // BLE carries the J_DBG UART instead of commands
    // Touch: screen = A * raw, A = {a, b, c, d, e, f}: x = a*rx + b*ry + c, y = d*rx + e*ry + f,
    // valid for touchRotation; rotation 1 <-> 3 is handled as a 180 degree flip.
    bool touchValid = false;
    bool touchSkipped = false;      // user skipped the first-boot calibration
    std::uint8_t touchRotation = 1;
    float touchCal[6] = {0, 0, 0, 0, 0, 0};
};

void begin();
void loop();

Settings& settings();
void saveSettings();

// ---- resistance ----
const smart_resistance::AppState& state();
bool setResistance(std::uint32_t ohms);       // leaves NTC mode
bool adjustDigit(std::uint8_t digit, int delta);
bool setOutput(bool on);
bool rawMode();

// ---- NTC ----
int ntcTypeCount();                            // presets + user slot
const char* ntcName(int index);
double ntcR25();
double ntcBeta();
void selectNtc(int index);
void setUserNtc(double r25Ohms, double beta);
bool setNtcTemperature(double tempC);          // computes R, applies it, enters NTC mode
bool ntcActive();
double ntcTemperature();
double ntcIdealOhms();                         // un-rounded R for the current temperature

bool startSweep(double fromC, double toC, double stepC, std::uint32_t dwellMs);
void stopSweep();
bool sweepRunning();

// ---- relay test ----
void setRelayRaw(int relayIndex, bool on);
void setRawWord(std::uint64_t shiftWord, bool outputRelay);
bool startRelayCycle(int relayIndex, std::uint32_t onMs, std::uint32_t offMs, std::uint32_t cycles);
void stopRelayCycle();
bool relayCycleRunning();
std::uint32_t relayCycleCount();
void leaveRawMode();                           // back to the staged resistance, output off

// ---- TMP117 ----
bool tempValid();
float boardTempC();
int tempLogCount();
float tempLogAt(int index);                    // 0 = oldest
void clearTempLog();

// ---- commands ----
// Executes one text command line and returns the reply (may span several lines, no trailing
// newline). Unknown commands return "ERR ...".
String handleCommand(const String& line);

}  // namespace app
