#include "app.h"

#include <Preferences.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <nvs.h>

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "ancs.h"
#include "application_controller.h"
#include "board_pins.h"
#include "comm.h"
#include "desk.h"
#include "game.h"
#include "messages.h"
#include "pbank.h"
#include "relay_backend.h"
#include "resistance_controller.h"
#include "ui.h"

namespace app {
namespace {

using smart_resistance::AppEventType;
using smart_resistance::Calibration;
using smart_resistance::kNtcPresetCount;
using smart_resistance::kNtcPresets;

class ArduinoRelayBackend final : public smart_resistance::RelayBackend {
public:
    void setOutputConnected(bool connected) override {
        digitalWrite(board::kOutputRelayEnable, connected ? HIGH : LOW);
    }

    void setShiftOutputsEnabled(bool enabled) override {
        digitalWrite(board::kRelayOutputEnableN, enabled ? LOW : HIGH);
    }

    void writeShiftWord(std::uint64_t word) override {
        digitalWrite(board::kRelayLatch, LOW);
        for (int bit = smart_resistance::kRelayChainBits - 1; bit >= 0; --bit) {
            digitalWrite(board::kRelayClock, LOW);
            digitalWrite(board::kRelaySerial, (word & (1ULL << bit)) != 0 ? HIGH : LOW);
            digitalWrite(board::kRelayClock, HIGH);
        }
        digitalWrite(board::kRelayClock, LOW);
        digitalWrite(board::kRelayLatch, HIGH);
    }

    void setRelayPowerEnabled(bool enabled) override {
        digitalWrite(board::kRelayPowerEnable, enabled ? HIGH : LOW);
    }

    void delayMilliseconds(std::uint32_t milliseconds) override {
        delay(milliseconds);
    }
};

ArduinoRelayBackend relayBackend;
smart_resistance::ResistanceController resistanceController(relayBackend);
smart_resistance::ApplicationController controller(resistanceController);

Preferences prefs;
Settings config;

// ---- NTC / sweep ----
bool ntcMode = false;
double ntcTempC = 25.0;

struct Sweep {
    bool running = false;
    double current = 0.0;
    double target = 0.0;
    double step = 0.0;
    std::uint32_t dwellMs = 0;
    std::uint32_t nextMs = 0;
} sweep;

// ---- relay test ----
std::uint64_t rawWord = 0;
bool rawOutput = false;

struct RelayCycle {
    bool running = false;
    int relay = 0;
    std::uint32_t onMs = 0;
    std::uint32_t offMs = 0;
    std::uint32_t cycles = 0;   // 0 = until stopped
    std::uint32_t count = 0;
    bool phaseOn = false;
    std::uint32_t nextMs = 0;
} cycle;

// ---- TMP117 ----
constexpr std::uint8_t kTmp117Address = 0x48;
bool tempOk = false;
float tempC = NAN;
std::uint32_t nextTempReadMs = 0;
std::uint32_t nextTempLogMs = 0;
float tempLog[kTempLogSize];
int tempLogHead = 0;
int tempLogUsed = 0;

constexpr std::uint32_t kCalibrationMagic = 0x53524331;  // "SRC1"

bool rebootPending = false;
std::uint32_t rebootAtMs = 0;
bool hostFallback = false;

void loadSettings() {
    prefs.begin("smartr", false);
    config.wifiSsid = prefs.getString("ssid", "");
    config.wifiPass = prefs.getString("pass", "");
    config.ntcPreset = prefs.getInt("ntcp", 0);
    config.userR25 = prefs.getDouble("ur25", 10000.0);
    config.userBeta = prefs.getDouble("ubeta", 3435.0);
    config.lastOhms = prefs.getUInt("lastr", 1000);
    config.solver = prefs.getBool("solver", true);
    config.bridgeBaud = prefs.getUInt("baud", 115200);
    config.rotation = prefs.getUChar("rot", 1);
    config.backlight = prefs.getUChar("bl", 100);
    config.knobMode = prefs.getUChar("knob", 0);
    config.logIntervalS = prefs.getUShort("logint", 1);
    config.language = prefs.getUChar("lang", 0);
    config.bleEnabled = prefs.getBool("ble", true);
    config.mqttEnabled = prefs.getBool("mq_en", false);
    config.mqttHost = prefs.getString("mq_host", "");
    config.mqttPort = prefs.getUShort("mq_port", 8883);
    config.mqttUser = prefs.getString("mq_user", "");
    config.mqttPass = prefs.getString("mq_pass", "");
    config.bleBridge = prefs.getBool("blebr", false);
    config.usbHost = prefs.getBool("usbh", false);
    config.touchValid = prefs.getBool("tvalid", false);
    config.touchSkipped = prefs.getBool("tskip", false);
    config.touchRotation = prefs.getUChar("trot", 1);
    if (prefs.getBytesLength("tcal") == sizeof(config.touchCal)) {
        prefs.getBytes("tcal", config.touchCal, sizeof(config.touchCal));
    } else {
        config.touchValid = false;
    }
    if (config.ntcPreset < 0 || config.ntcPreset > static_cast<int>(kNtcPresetCount)) {
        config.ntcPreset = 0;
    }
    if (config.logIntervalS == 0) {
        config.logIntervalS = 1;
    }

    if (prefs.getUInt("calmagic", 0) == kCalibrationMagic
        && prefs.getBytesLength("cal") == sizeof(Calibration)) {
        Calibration stored;
        prefs.getBytes("cal", &stored, sizeof(stored));
        controller.calibration() = stored;
    }
}

void saveCalibration() {
    prefs.putUInt("calmagic", kCalibrationMagic);
    prefs.putBytes("cal", &controller.calibration(), sizeof(Calibration));
}

void readTemperature() {
    Wire.beginTransmission(kTmp117Address);
    Wire.write(static_cast<std::uint8_t>(0x00));
    if (Wire.endTransmission(false) != 0 || Wire.requestFrom(kTmp117Address, static_cast<std::uint8_t>(2)) != 2) {
        tempOk = false;
        return;
    }
    const std::int16_t raw = static_cast<std::int16_t>((Wire.read() << 8) | Wire.read());
    tempC = raw * 0.0078125f;
    tempOk = true;
}

void applyRaw() {
    controller.setRaw(rawWord, rawOutput);
}

void serviceSweep(std::uint32_t now) {
    if (!sweep.running || static_cast<std::int32_t>(now - sweep.nextMs) < 0) {
        return;
    }
    const bool rising = sweep.step > 0;
    const bool finished = rising ? sweep.current > sweep.target + 1e-9 : sweep.current < sweep.target - 1e-9;
    if (finished) {
        sweep.running = false;
        return;
    }
    setNtcTemperature(sweep.current);
    sweep.current += sweep.step;
    sweep.nextMs = now + sweep.dwellMs;
    sweep.running = true;   // setNtcTemperature() does not clear it, but keep this explicit
}

void serviceRelayCycle(std::uint32_t now) {
    if (!cycle.running || static_cast<std::int32_t>(now - cycle.nextMs) < 0) {
        return;
    }
    if (cycle.phaseOn) {
        setRelayRaw(cycle.relay, false);
        cycle.phaseOn = false;
        ++cycle.count;
        if (cycle.cycles != 0 && cycle.count >= cycle.cycles) {
            cycle.running = false;
            return;
        }
        cycle.nextMs = now + cycle.offMs;
    } else {
        setRelayRaw(cycle.relay, true);
        cycle.phaseOn = true;
        cycle.nextMs = now + cycle.onMs;
    }
}

// ---- command helpers ----
int splitTokens(const String& line, String* tokens, int maxTokens) {
    int count = 0;
    int index = 0;
    const int length = line.length();
    while (index < length && count < maxTokens) {
        while (index < length && isspace(static_cast<unsigned char>(line[index]))) {
            ++index;
        }
        if (index >= length) {
            break;
        }
        int end = index;
        while (end < length && !isspace(static_cast<unsigned char>(line[end]))) {
            ++end;
        }
        tokens[count++] = line.substring(index, end);
        index = end;
    }
    return count;
}

bool parseNumber(const String& text, double& value) {
    if (text.length() == 0) {
        return false;
    }
    char* end = nullptr;
    value = strtod(text.c_str(), &end);
    return end != text.c_str() && *end == '\0';
}

// Accepts 4700, 4.7k, 1M, 10R.
bool parseOhms(const String& text, double& ohms) {
    String body = text;
    double scale = 1.0;
    if (body.length() == 0) {
        return false;
    }
    const char suffix = body[body.length() - 1];
    if (suffix == 'k' || suffix == 'K') {
        scale = 1e3;
        body.remove(body.length() - 1);
    } else if (suffix == 'M') {
        scale = 1e6;
        body.remove(body.length() - 1);
    } else if (suffix == 'r' || suffix == 'R') {
        body.remove(body.length() - 1);
    }
    double value = 0.0;
    if (!parseNumber(body, value) || value < 0.0) {
        return false;
    }
    ohms = value * scale;
    return true;
}

bool parseOnOff(const String& text, bool& on) {
    String upper = text;
    upper.toUpperCase();
    if (upper == "1" || upper == "ON") {
        on = true;
        return true;
    }
    if (upper == "0" || upper == "OFF") {
        on = false;
        return true;
    }
    return false;
}

String statusLine() {
    const auto& st = controller.state();
    String out = "R=" + String(st.targetOhms);
    out += " EST=" + String(st.estimatedOhms, 3);
    out += " OUT=" + String(st.outputEnabled ? 1 : 0);
    out += " FAULT=" + String(st.fault ? 1 : 0);
    out += " MODE=" + String(controller.rawMode() ? "RAW" : (ntcMode ? "NTC" : "R"));
    if (ntcMode) {
        out += " T=" + String(ntcTempC, 2);
    }
    if (tempOk) {
        out += " BOARD=" + String(tempC, 2);
    }
    char raw[20];
    snprintf(raw, sizeof(raw), "%010llx", static_cast<unsigned long long>(st.relayPlan.shiftWord));
    out += String(" RAW=") + raw;
    return out;
}

String calibrationDump() {
    const Calibration& cal = controller.calibration();
    String out = "SOLVER=" + String(controller.solverEnabled() ? 1 : 0);
    out += "\nOUT=" + String(cal.outputRelayOhms, 4) + " SHORT=" + String(cal.shortPathOhms, 4);
    out += "\nU:";
    for (double value : cal.unitPathOhms) {
        out += " " + String(value, 4);
    }
    out += "\nW:";
    for (double value : cal.highIncrementOhms) {
        out += " " + String(value, 3);
    }
    out += "\nBASE:";
    for (double value : cal.highPathBaseOhms) {
        out += " " + String(value, 4);
    }
    return out;
}

const char* const kHelpText =
    "R <ohms|4.7k|1M> | R? | OUT <0|1> | STAT?\n"
    "NTC <tempC> | NTC? | NTCSEL <index> | NTCCFG <r25> <beta> | NTCLIST?\n"
    "SWEEP <fromC> <toC> <stepC> <dwell_ms> | SWEEP STOP\n"
    "TEMP? | TEMPLOG? | LOGINT <s>\n"
    "RELAY <0..40> <0|1> | RAW <hex> [out] | CYCLE <relay> <on_ms> <off_ms> <count> | CYCLE STOP | NORMAL\n"
    "CAL? | CAL U <0..9> <ohms> | CAL W <0..19> <ohms> | CAL BASE <0..5> <ohms>\n"
    "CAL OUT <ohms> | CAL SHORT <ohms> | CAL SOLVER <0|1> | CAL RESET\n"
    "WIFI <ssid> [password] | WIFI OFF | WIFI? | BAUD <n> | KNOB <0..3>\n"
    "MQTT <host> [port] [user] [password] | MQTT OFF | MQTT?\n"
    "BLE <0|1> | BLE? | BLEMODE <0=cmd|1=uart bridge> | LANG <0=zh|1=en> | TOUCHCAL | TOUCH?\n"
    "MSGNEW <id> <app> <w> <h> | MSGDAT <id> <off> <b64> | MSGEND <id> | MSG? | MSGREAD | MSGCLR\n"
    "IMGNEW <slot> <w> <h> | IMGDAT <slot> <off> <b64> | IMGEND <slot> | IMGCLR <slot>\n"
    "SYS <cpu> <ram> <down> <up> | PLAY <0|1> | CI <0..3> [HH:MM] | MACRO <0..5> <action> | MACRO?\n"
    "USBMODE <PC|PBANK> | USBMODE? | PB? | PBLOG? | SHOT [row] [rows] | STATE?\n"
    "HID? | ANCS? | MEM? | CLEAN | REMOTE <VOLUP|VOLDOWN|PLAY|NEXT|PREV|SHUTTER|MUTE|UP|DOWN|HOME> | UNPAIR\n"
    "GAME? | GAME <NEW|LEFT|RIGHT|ROT|DROP|PAUSE|RESUME>\n"
    "PAGE <0..19> | ROT <1|3> | BL <0..100> | LOCK <0|1> | CLEAR | *IDN? | REBOOT";

String handleCalibration(String* tokens, int count) {
    Calibration& cal = controller.calibration();
    String sub = tokens[1];
    sub.toUpperCase();
    double value = 0.0;

    if (sub == "RESET") {
        cal = Calibration{};
    } else if (sub == "SOLVER") {
        bool on = false;
        if (count < 3 || !parseOnOff(tokens[2], on)) {
            return "ERR CAL SOLVER <0|1>";
        }
        controller.setSolverEnabled(on);
        config.solver = on;
        saveSettings();
    } else if (sub == "OUT" || sub == "SHORT") {
        if (count < 3 || !parseNumber(tokens[2], value) || value < 0) {
            return "ERR value";
        }
        (sub == "OUT" ? cal.outputRelayOhms : cal.shortPathOhms) = value;
    } else if (sub == "U" || sub == "W" || sub == "BASE") {
        double index = 0.0;
        if (count < 4 || !parseNumber(tokens[2], index) || !parseNumber(tokens[3], value) || value < 0) {
            return String("ERR CAL ") + sub + " <index> <ohms>";
        }
        const int i = static_cast<int>(index);
        if (sub == "U" && i >= 0 && i < 10) {
            cal.unitPathOhms[i] = value;
        } else if (sub == "W" && i >= 0 && i < static_cast<int>(smart_resistance::kHighWeightCount)) {
            cal.highIncrementOhms[i] = value;
        } else if (sub == "BASE" && i >= 0 && i <= static_cast<int>(smart_resistance::kRangeBypassCount)) {
            cal.highPathBaseOhms[i] = value;
        } else {
            return "ERR index";
        }
    } else {
        return "ERR CAL subcommand";
    }

    saveCalibration();
    if (!controller.rawMode()) {
        controller.recompute();
    }
    return "OK";
}

// ---- MEM? diagnostics ----
TaskHandle_t loopTaskHandle = nullptr;
std::uint32_t lastLoopUs = 0;
std::uint32_t loopWindowStartMs = 0;
std::uint32_t loopWindowCount = 0;
std::uint32_t loopRateHz = 0;
std::uint32_t loopMaxUs = 0;   // longest loop iteration since the previous MEM?

void trackLoop(std::uint32_t nowMs) {
    const std::uint32_t us = micros();
    if (lastLoopUs != 0 && us - lastLoopUs > loopMaxUs) {
        loopMaxUs = us - lastLoopUs;
    }
    lastLoopUs = us;
    ++loopWindowCount;
    if (nowMs - loopWindowStartMs >= 1000) {
        loopRateHz = loopWindowCount * 1000UL / (nowMs - loopWindowStartMs);
        loopWindowCount = 0;
        loopWindowStartMs = nowMs;
    }
}

String kib(std::size_t bytes) {
    return String(static_cast<unsigned>((bytes + 512) / 1024)) + "K";
}

std::uint32_t appImageBytes() {
    static std::uint32_t bytes = 0;
    if (bytes == 0) {
        bytes = ESP.getSketchSize();           // verifies the whole image (~0.2 s), so only once
    }
    return bytes;
}

void appendPartitions(String& out, esp_partition_type_t type) {
    for (esp_partition_iterator_t it = esp_partition_find(type, ESP_PARTITION_SUBTYPE_ANY, nullptr); it != nullptr;
         it = esp_partition_next(it)) {
        const esp_partition_t* p = esp_partition_get(it);
        out += String(" ") + p->label + "=" + kib(p->size);
    }
}

String heapLine(const char* name, std::uint32_t caps) {
    multi_heap_info_t info = {};
    heap_caps_get_info(&info, caps);
    return String(name) + "=" + kib(info.total_free_bytes) + "/" + kib(info.total_free_bytes + info.total_allocated_bytes)
        + " MIN=" + kib(info.minimum_free_bytes) + " BLOCK=" + kib(info.largest_free_block);
}

String memoryReport() {
    // HEAP = internal RAM (free/total), PSRAM = external 8 MB; MIN = lowest free since boot, BLOCK = largest free piece
    String out = String("MEM ") + heapLine("HEAP", MALLOC_CAP_INTERNAL) + " " + heapLine("PSRAM", MALLOC_CAP_SPIRAM);

    const esp_partition_t* running = esp_ota_get_running_partition();
    out += String("\nAPP=") + kib(appImageBytes()) + "/" + kib(running ? running->size : 0);
    nvs_stats_t nvs = {};
    if (nvs_get_stats(nullptr, &nvs) == ESP_OK) {
        out += String(" NVS=") + static_cast<unsigned>(nvs.used_entries) + "/" + static_cast<unsigned>(nvs.total_entries)
            + " NS=" + static_cast<unsigned>(nvs.namespace_count);
    }
    out += String(" UP=") + static_cast<unsigned>(millis() / 1000UL) + " LOOP=" + static_cast<unsigned>(loopRateHz)
        + "Hz MAXLOOP=" + String(loopMaxUs / 1000.0f, 1) + "ms";
    loopMaxUs = 0;
    lastLoopUs = 0;                            // the iteration running this report is not counted

    out += "\nPART";
    appendPartitions(out, ESP_PARTITION_TYPE_APP);
    appendPartitions(out, ESP_PARTITION_TYPE_DATA);

    // Free stack per task (bytes never used since boot), plus CPU load when the core keeps run-time stats.
    out += "\nTASKS";
#if configUSE_TRACE_FACILITY
    const UBaseType_t capacity = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t* tasks = static_cast<TaskStatus_t*>(malloc(sizeof(TaskStatus_t) * capacity));
    if (tasks != nullptr) {
        std::uint32_t total = 0;
        const UBaseType_t got = uxTaskGetSystemState(tasks, capacity, &total);
        for (UBaseType_t i = 0; i < got; ++i) {
            out += String(" ") + tasks[i].pcTaskName + "=" + String(static_cast<unsigned>(tasks[i].usStackHighWaterMark));
        }
#if configGENERATE_RUN_TIME_STATS
        static std::uint32_t prevTotal = 0;
        static std::uint32_t prevIdle[2] = {0, 0};
        std::uint32_t idle[2] = {0, 0};
        for (UBaseType_t i = 0; i < got; ++i) {
            for (int core = 0; core < 2; ++core) {
                if (tasks[i].xHandle == xTaskGetIdleTaskHandleForCPU(core)) {
                    idle[core] = tasks[i].ulRunTimeCounter;
                }
            }
        }
        const std::uint32_t span = total - prevTotal;
        if (span > 0) {
            out += "\nCPU";
            for (int core = 0; core < 2; ++core) {
                const float busy = 100.0f - 100.0f * static_cast<float>(idle[core] - prevIdle[core]) / span;
                out += String(" core") + core + "=" + String(busy < 0 ? 0.0f : busy, 1) + "%";
                prevIdle[core] = idle[core];
            }
            prevTotal = total;
        }
#endif
        free(tasks);
    }
#else
    out += String(" loopTask=") + static_cast<unsigned>(uxTaskGetStackHighWaterMark(loopTaskHandle));
#endif
    return out;
}

}  // namespace

void begin() {
    pinMode(board::kRelaySerial, OUTPUT);
    pinMode(board::kRelayClock, OUTPUT);
    pinMode(board::kRelayLatch, OUTPUT);
    pinMode(board::kRelayOutputEnableN, OUTPUT);
    pinMode(board::kOutputRelayEnable, OUTPUT);
    pinMode(board::kRelayPowerEnable, OUTPUT);

    digitalWrite(board::kRelaySerial, LOW);
    digitalWrite(board::kRelayClock, LOW);
    digitalWrite(board::kRelayLatch, LOW);
    digitalWrite(board::kRelayOutputEnableN, HIGH);
    digitalWrite(board::kRelayPowerEnable, LOW);
    relayBackend.setOutputConnected(false);

    loopTaskHandle = xTaskGetCurrentTaskHandle();
    loadSettings();
    controller.setSolverEnabled(config.solver);

    // Power-bank mode takes the USB port away from the PC, and the board has no BOOT button, so
    // there are two ways back to PC mode that work even if that mode misbehaves: a crash reset
    // (panic / watchdog) while in it, or holding the knob button at power-up.
    if (config.usbHost) {
        pinMode(board::kEncoderSwitch, INPUT_PULLUP);
        delay(5);
        const esp_reset_reason_t reason = esp_reset_reason();
        const bool crashed = reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT
            || reason == ESP_RST_WDT;
        if (crashed || digitalRead(board::kEncoderSwitch) == LOW) {
            config.usbHost = false;
            prefs.putBool("usbh", false);
            hostFallback = true;
        }
    }

    Wire.begin(board::kI2cSda, board::kI2cScl, 400000);
    Wire.setTimeOut(20);

    resistanceController.begin();
    // Stage the last resistance but leave K_OUT open until an explicit command.
    controller.initialize(config.lastOhms);
}

void loop() {
    const std::uint32_t now = millis();
    trackLoop(now);
    if (rebootPending && static_cast<std::int32_t>(now - rebootAtMs) >= 0) {
        controller.process({AppEventType::SetOutput, 0});
        saveSettings();
        delay(50);
        ESP.restart();
    }
    serviceSweep(now);
    serviceRelayCycle(now);

    if (static_cast<std::int32_t>(now - nextTempReadMs) >= 0) {
        nextTempReadMs = now + 500;
        readTemperature();
    }
    if (static_cast<std::int32_t>(now - nextTempLogMs) >= 0) {
        nextTempLogMs = now + 1000UL * config.logIntervalS;
        if (tempOk) {
            tempLog[tempLogHead] = tempC;
            tempLogHead = (tempLogHead + 1) % kTempLogSize;
            if (tempLogUsed < kTempLogSize) {
                ++tempLogUsed;
            }
        }
    }
}

Settings& settings() {
    return config;
}

Resources resources() {
    Resources r;
    multi_heap_info_t info = {};
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);
    r.heapFree = info.total_free_bytes;
    r.heapTotal = info.total_free_bytes + info.total_allocated_bytes;
    r.heapMin = info.minimum_free_bytes;
    r.heapBlock = info.largest_free_block;
    info = {};
    heap_caps_get_info(&info, MALLOC_CAP_SPIRAM);
    r.psramFree = info.total_free_bytes;
    r.psramTotal = info.total_free_bytes + info.total_allocated_bytes;
    r.psramMin = info.minimum_free_bytes;
    const esp_partition_t* running = esp_ota_get_running_partition();
    r.appUsed = appImageBytes();
    r.appTotal = running ? running->size : 0;
    nvs_stats_t nvs = {};
    if (nvs_get_stats(nullptr, &nvs) == ESP_OK) {
        r.nvsUsed = nvs.used_entries;
        r.nvsTotal = nvs.total_entries;
    }
    r.loopHz = loopRateHz;
    r.uptimeS = millis() / 1000UL;
    return r;
}

Cleanup cleanup() {
    Cleanup result;
    const std::size_t before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    result.messages = msg::releaseRead();
    const std::size_t after = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    result.freedBytes = after > before ? static_cast<std::uint32_t>(after - before) : 0;
    return result;
}

void scheduleReboot(std::uint32_t delayMs) {
    rebootPending = true;
    rebootAtMs = millis() + delayMs;
}

bool setUsbHostMode(bool host) {
    if (host == config.usbHost) {
        return false;
    }
    config.usbHost = host;
    saveSettings();
    scheduleReboot(800);
    return true;
}

bool usbHostFallback() {
    return hostFallback;
}

void saveSettings() {
    prefs.putString("ssid", config.wifiSsid);
    prefs.putString("pass", config.wifiPass);
    prefs.putInt("ntcp", config.ntcPreset);
    prefs.putDouble("ur25", config.userR25);
    prefs.putDouble("ubeta", config.userBeta);
    prefs.putUInt("lastr", config.lastOhms);
    prefs.putBool("solver", config.solver);
    prefs.putUInt("baud", config.bridgeBaud);
    prefs.putUChar("rot", config.rotation);
    prefs.putUChar("bl", config.backlight);
    prefs.putUChar("knob", config.knobMode);
    prefs.putUShort("logint", config.logIntervalS);
    prefs.putUChar("lang", config.language);
    prefs.putBool("ble", config.bleEnabled);
    prefs.putBool("mq_en", config.mqttEnabled);
    prefs.putString("mq_host", config.mqttHost);
    prefs.putUShort("mq_port", config.mqttPort);
    prefs.putString("mq_user", config.mqttUser);
    prefs.putString("mq_pass", config.mqttPass);
    prefs.putBool("blebr", config.bleBridge);
    prefs.putBool("usbh", config.usbHost);
    prefs.putBool("tvalid", config.touchValid);
    prefs.putBool("tskip", config.touchSkipped);
    prefs.putUChar("trot", config.touchRotation);
    prefs.putBytes("tcal", config.touchCal, sizeof(config.touchCal));
}

const smart_resistance::AppState& state() {
    return controller.state();
}

bool rawMode() {
    return controller.rawMode();
}

bool setResistance(std::uint32_t ohms) {
    ntcMode = false;
    sweep.running = false;
    cycle.running = false;
    const bool ok = controller.process({AppEventType::SetResistance, static_cast<std::int32_t>(ohms)});
    if (ok) {
        config.lastOhms = controller.state().targetOhms;
    }
    return ok;
}

bool adjustDigit(std::uint8_t digit, int delta) {
    ntcMode = false;
    sweep.running = false;
    cycle.running = false;
    if (controller.rawMode()) {
        controller.recompute();
    }
    controller.process({AppEventType::SelectDigit, digit});
    const bool ok = controller.process({AppEventType::AdjustSelectedDigit, delta});
    if (ok) {
        config.lastOhms = controller.state().targetOhms;
    }
    return ok;
}

bool setOutput(bool on) {
    if (controller.rawMode()) {
        rawOutput = on;
        applyRaw();
        return true;
    }
    return controller.process({AppEventType::SetOutput, on ? 1 : 0});
}

int ntcTypeCount() {
    return static_cast<int>(kNtcPresetCount) + 1;
}

const char* ntcName(int index) {
    if (index >= 0 && index < static_cast<int>(kNtcPresetCount)) {
        return kNtcPresets[index].name;
    }
    return "User";
}

double ntcR25() {
    return config.ntcPreset < static_cast<int>(kNtcPresetCount)
        ? kNtcPresets[config.ntcPreset].r25Ohms : config.userR25;
}

double ntcBeta() {
    return config.ntcPreset < static_cast<int>(kNtcPresetCount)
        ? kNtcPresets[config.ntcPreset].beta : config.userBeta;
}

void selectNtc(int index) {
    if (index < 0 || index >= ntcTypeCount()) {
        return;
    }
    config.ntcPreset = index;
    if (ntcMode) {
        setNtcTemperature(ntcTempC);
    }
}

void setUserNtc(double r25Ohms, double beta) {
    config.userR25 = r25Ohms;
    config.userBeta = beta;
    selectNtc(static_cast<int>(kNtcPresetCount));
}

double ntcIdealOhms() {
    return smart_resistance::ntcResistanceOhms(ntcR25(), ntcBeta(), ntcTempC);
}

bool setNtcTemperature(double tempCelsius) {
    if (std::isnan(tempCelsius)) {
        return false;
    }
    tempCelsius = constrain(tempCelsius, smart_resistance::kNtcMinTempC, smart_resistance::kNtcMaxTempC);
    const double ohms = smart_resistance::ntcResistanceOhms(ntcR25(), ntcBeta(), tempCelsius);
    const double clamped = constrain(ohms, 0.0, static_cast<double>(smart_resistance::kMaximumResistanceOhms));
    cycle.running = false;
    const bool ok = controller.process(
        {AppEventType::SetResistance, static_cast<std::int32_t>(lround(clamped))});
    if (ok) {
        ntcMode = true;
        ntcTempC = tempCelsius;
    }
    return ok;
}

bool ntcActive() {
    return ntcMode;
}

double ntcTemperature() {
    return ntcTempC;
}

bool startSweep(double fromC, double toC, double stepC, std::uint32_t dwellMs) {
    if (stepC == 0.0 || dwellMs < 50) {
        return false;
    }
    stepC = std::fabs(stepC) * (toC >= fromC ? 1.0 : -1.0);
    sweep.current = fromC;
    sweep.target = toC;
    sweep.step = stepC;
    sweep.dwellMs = dwellMs;
    sweep.nextMs = millis();
    sweep.running = true;
    return true;
}

void stopSweep() {
    sweep.running = false;
}

bool sweepRunning() {
    return sweep.running;
}

void setRawWord(std::uint64_t shiftWord, bool outputRelay) {
    ntcMode = false;
    sweep.running = false;
    rawWord = shiftWord & ((1ULL << smart_resistance::kRelayChainBits) - 1);
    rawOutput = outputRelay;
    applyRaw();
}

void setRelayRaw(int relayIndex, bool on) {
    if (relayIndex < 0 || relayIndex >= kRelayIndexCount) {
        return;
    }
    if (!controller.rawMode()) {
        rawWord = 0;
        rawOutput = false;
    }
    ntcMode = false;
    sweep.running = false;
    if (relayIndex == kOutputRelayIndex) {
        rawOutput = on;
    } else if (on) {
        rawWord |= 1ULL << relayIndex;
    } else {
        rawWord &= ~(1ULL << relayIndex);
    }
    applyRaw();
}

bool startRelayCycle(int relayIndex, std::uint32_t onMs, std::uint32_t offMs, std::uint32_t cycles) {
    if (relayIndex < 0 || relayIndex >= kRelayIndexCount || onMs < 30 || offMs < 30) {
        return false;
    }
    setRawWord(0, false);
    cycle.relay = relayIndex;
    cycle.onMs = onMs;
    cycle.offMs = offMs;
    cycle.cycles = cycles;
    cycle.count = 0;
    cycle.phaseOn = false;
    cycle.nextMs = millis();
    cycle.running = true;
    return true;
}

void stopRelayCycle() {
    if (cycle.running) {
        cycle.running = false;
        setRawWord(0, false);
    }
}

bool relayCycleRunning() {
    return cycle.running;
}

std::uint32_t relayCycleCount() {
    return cycle.count;
}

void leaveRawMode() {
    cycle.running = false;
    if (controller.rawMode()) {
        controller.process({AppEventType::SetOutput, 0});
        controller.recompute();
    }
}

bool tempValid() {
    return tempOk;
}

float boardTempC() {
    return tempC;
}

int tempLogCount() {
    return tempLogUsed;
}

float tempLogAt(int index) {
    if (index < 0 || index >= tempLogUsed) {
        return NAN;
    }
    const int start = (tempLogHead - tempLogUsed + kTempLogSize) % kTempLogSize;
    return tempLog[(start + index) % kTempLogSize];
}

void clearTempLog() {
    tempLogHead = 0;
    tempLogUsed = 0;
}

String handleCommand(const String& rawLine) {
    String line = rawLine;
    line.trim();
    if (line.length() == 0) {
        return "";
    }

    String tokens[6];
    const int count = splitTokens(line, tokens, 6);
    String cmd = tokens[0];
    cmd.toUpperCase();
    double a = 0.0;
    double b = 0.0;

    if (cmd == "*IDN?") {
        return String("SmartResistance,V1-DVT-HW37,") + comm::bleName() + "," + kFirmwareVersion;
    }
    if (cmd == "HELP" || cmd == "?") {
        return kHelpText;
    }
    if (cmd == "STAT?" || cmd == "R?") {
        return statusLine();
    }
    if (cmd.startsWith("MSG") || cmd.startsWith("IMG")) {
        return msg::handleCommand(cmd, tokens, count);
    }
    if (cmd == "SYS" || cmd == "PLAY" || cmd == "CI" || cmd == "MACRO" || cmd == "MACRO?" || cmd == "TIME"
        || cmd == "WX") {
        return desk::handleCommand(cmd, tokens, count);
    }

    if (cmd == "R") {
        if (count < 2 || !parseOhms(tokens[1], a) || a > smart_resistance::kMaximumResistanceOhms + 0.5) {
            return "ERR R <0..999999>";
        }
        return setResistance(static_cast<std::uint32_t>(lround(a))) ? String("OK ") + statusLine() : String("ERR");
    }

    if (cmd == "OUT" || cmd == "OUT?") {
        if (cmd == "OUT?" || count < 2) {
            return String("OUT=") + (controller.state().outputEnabled ? "1" : "0");
        }
        bool on = false;
        if (!parseOnOff(tokens[1], on)) {
            return "ERR OUT <0|1>";
        }
        return setOutput(on) ? "OK" : "ERR";
    }

    if (cmd == "NTC?" ) {
        return String("NTC=") + ntcName(config.ntcPreset) + " R25=" + String(ntcR25(), 1)
            + " B=" + String(ntcBeta(), 1) + " T=" + String(ntcTempC, 2)
            + " IDEAL=" + String(ntcIdealOhms(), 2) + " ACTIVE=" + String(ntcMode ? 1 : 0);
    }
    if (cmd == "NTC") {
        if (count < 2 || !parseNumber(tokens[1], a)) {
            return "ERR NTC <tempC>";
        }
        if (a < smart_resistance::kNtcMinTempC || a > smart_resistance::kNtcMaxTempC) {
            return "ERR temperature range -40..150";
        }
        const double ideal = smart_resistance::ntcResistanceOhms(ntcR25(), ntcBeta(), a);
        if (ideal > smart_resistance::kMaximumResistanceOhms) {
            return "ERR resistance above 999999 ohm at this temperature";
        }
        sweep.running = false;
        return setNtcTemperature(a) ? String("OK ") + statusLine() : String("ERR");
    }
    if (cmd == "NTCLIST?") {
        String out;
        for (int i = 0; i < ntcTypeCount(); ++i) {
            out += String(i) + (i == config.ntcPreset ? "* " : "  ") + ntcName(i);
            if (i == static_cast<int>(kNtcPresetCount)) {
                out += " R25=" + String(config.userR25, 1) + " B=" + String(config.userBeta, 1);
            }
            if (i + 1 < ntcTypeCount()) {
                out += "\n";
            }
        }
        return out;
    }
    if (cmd == "NTCSEL") {
        if (count < 2 || !parseNumber(tokens[1], a) || a < 0 || a >= ntcTypeCount()) {
            return "ERR NTCSEL <index>";
        }
        selectNtc(static_cast<int>(a));
        saveSettings();
        return String("OK ") + ntcName(config.ntcPreset);
    }
    if (cmd == "NTCCFG") {
        if (count < 3 || !parseOhms(tokens[1], a) || !parseNumber(tokens[2], b)
            || a < 10 || a > 2e6 || b < 500 || b > 10000) {
            return "ERR NTCCFG <r25> <beta>";
        }
        setUserNtc(a, b);
        saveSettings();
        return "OK";
    }

    if (cmd == "SWEEP") {
        String sub = count > 1 ? tokens[1] : String();
        sub.toUpperCase();
        if (sub == "STOP") {
            stopSweep();
            return "OK";
        }
        double step = 0.0;
        double dwell = 0.0;
        if (count < 5 || !parseNumber(tokens[1], a) || !parseNumber(tokens[2], b)
            || !parseNumber(tokens[3], step) || !parseNumber(tokens[4], dwell)) {
            return "ERR SWEEP <fromC> <toC> <stepC> <dwell_ms>";
        }
        return startSweep(a, b, step, static_cast<std::uint32_t>(dwell)) ? "OK" : "ERR";
    }

    if (cmd == "TEMP?") {
        return tempOk ? String(tempC, 3) : String("ERR no TMP117");
    }
    if (cmd == "TEMPLOG?") {
        String out = "N=" + String(tempLogUsed) + " INT=" + String(config.logIntervalS);
        for (int i = 0; i < tempLogUsed; ++i) {
            out += (i % 10 == 0 ? "\n" : " ");
            out += String(tempLogAt(i), 2);
        }
        return out;
    }
    if (cmd == "LOGINT") {
        if (count < 2 || !parseNumber(tokens[1], a) || a < 1 || a > 3600) {
            return "ERR LOGINT <1..3600>";
        }
        config.logIntervalS = static_cast<std::uint16_t>(a);
        saveSettings();
        return "OK";
    }

    if (cmd == "RELAY") {
        bool on = false;
        if (count < 3 || !parseNumber(tokens[1], a) || !parseOnOff(tokens[2], on)
            || a < 0 || a >= kRelayIndexCount) {
            return "ERR RELAY <0..40> <0|1>";
        }
        cycle.running = false;
        setRelayRaw(static_cast<int>(a), on);
        return "OK";
    }
    if (cmd == "RAW") {
        if (count < 2) {
            return "ERR RAW <hex> [out]";
        }
        bool out = false;
        if (count > 2 && !parseOnOff(tokens[2], out)) {
            return "ERR RAW <hex> [out]";
        }
        cycle.running = false;
        setRawWord(strtoull(tokens[1].c_str(), nullptr, 16), out);
        return "OK";
    }
    if (cmd == "CYCLE") {
        String sub = count > 1 ? tokens[1] : String();
        sub.toUpperCase();
        if (sub == "STOP") {
            stopRelayCycle();
            return String("OK COUNT=") + String(cycle.count);
        }
        double onMs = 0.0;
        double offMs = 0.0;
        double cycles = 0.0;
        if (count < 5 || !parseNumber(tokens[1], a) || !parseNumber(tokens[2], onMs)
            || !parseNumber(tokens[3], offMs) || !parseNumber(tokens[4], cycles) || cycles < 0) {
            return "ERR CYCLE <relay> <on_ms> <off_ms> <count>";
        }
        return startRelayCycle(static_cast<int>(a), static_cast<std::uint32_t>(onMs),
            static_cast<std::uint32_t>(offMs), static_cast<std::uint32_t>(cycles)) ? "OK" : "ERR";
    }
    if (cmd == "CYCLE?") {
        return String("RUN=") + String(cycle.running ? 1 : 0) + " COUNT=" + String(cycle.count);
    }
    if (cmd == "NORMAL") {
        leaveRawMode();
        return String("OK ") + statusLine();
    }

    if (cmd == "CAL?") {
        return calibrationDump();
    }
    if (cmd == "CAL") {
        if (count < 2) {
            return "ERR CAL subcommand";
        }
        return handleCalibration(tokens, count);
    }

    if (cmd == "WIFI?") {
        String out = "SSID=" + config.wifiSsid;
        out += String(" CONNECTED=") + (comm::wifiConnected() ? "1" : "0");
        out += " IP=" + comm::ipAddress();
        if (comm::accessPointActive()) {
            out += " AP=" + comm::accessPointName();
        }
        return out;
    }
    if (cmd == "WIFI") {
        if (count < 2) {
            return "ERR WIFI <ssid> [password] | WIFI OFF";
        }
        String sub = tokens[1];
        sub.toUpperCase();
        if (sub == "OFF" && count == 2) {
            config.wifiSsid = "";
            config.wifiPass = "";
        } else {
            // Keep the original spacing of the password: everything after the SSID token.
            config.wifiSsid = tokens[1];
            const int at = line.indexOf(tokens[1]) + tokens[1].length();
            String rest = line.substring(at);
            rest.trim();
            config.wifiPass = rest;
        }
        saveSettings();
        comm::restartWifi();
        return "OK";
    }
    if (cmd == "BAUD") {
        if (count < 2 || !parseNumber(tokens[1], a) || a < 300 || a > 3000000) {
            return "ERR BAUD <300..3000000>";
        }
        config.bridgeBaud = static_cast<std::uint32_t>(a);
        saveSettings();
        comm::setBridgeBaud(config.bridgeBaud);
        return "OK";
    }
    if (cmd == "KNOB") {
        if (count < 2 || !parseNumber(tokens[1], a) || a < 0
            || a >= static_cast<int>(comm::KnobMode::Count)) {
            return "ERR KNOB <0..3>";
        }
        config.knobMode = static_cast<std::uint8_t>(a);
        saveSettings();
        return String("OK ") + comm::knobModeName(static_cast<comm::KnobMode>(config.knobMode));
    }

    if (cmd == "PAGE") {
        if (count < 2 || !parseNumber(tokens[1], a) || a < 0 || a >= static_cast<int>(ui::Page::Count)) {
            return "ERR PAGE <0..19>";
        }
        ui::setPage(static_cast<ui::Page>(static_cast<int>(a)));
        return "OK";
    }
    if (cmd == "MQTT?") {
        return String("MQTT=") + (config.mqttEnabled ? "1" : "0") + " HOST=" + config.mqttHost
            + " PORT=" + String(config.mqttPort) + " USER=" + config.mqttUser
            + " STATUS=" + comm::mqttStatus() + " TOPIC=" + comm::mqttTopicBase();
    }
    if (cmd == "MQTT") {
        if (count < 2) {
            return "ERR MQTT <host> [port] [user] [password] | MQTT OFF";
        }
        String sub = tokens[1];
        sub.toUpperCase();
        if (sub == "OFF" && count == 2) {
            config.mqttEnabled = false;
        } else {
            double port = 8883;
            if (count > 2 && (!parseNumber(tokens[2], port) || port < 1 || port > 65535)) {
                return "ERR port";
            }
            config.mqttHost = tokens[1];
            config.mqttPort = static_cast<std::uint16_t>(port);
            config.mqttUser = count > 3 ? tokens[3] : String();
            config.mqttPass = "";
            if (count > 4) {
                // Everything after the user name, so a password may contain spaces.
                const int at = line.indexOf(tokens[3], line.indexOf(tokens[2]) + tokens[2].length()) + tokens[3].length();
                String rest = line.substring(at);
                rest.trim();
                config.mqttPass = rest;
            }
            config.mqttEnabled = true;
        }
        saveSettings();
        comm::restartMqtt();
        return String("OK TOPIC=") + comm::mqttTopicBase();
    }
    if (cmd == "USBMODE?" || (cmd == "USBMODE" && count < 2)) {
        return String("USBMODE=") + (config.usbHost ? "PBANK" : "PC") + " RUNNING=" + (pbank::active() ? "PBANK" : "PC");
    }
    if (cmd == "USBMODE") {
        String mode = tokens[1];
        mode.toUpperCase();
        bool host = false;
        if (mode == "PBANK" || mode == "HOST" || mode == "1") {
            host = true;
        } else if (mode != "PC" && mode != "DEVICE" && mode != "0") {
            return "ERR USBMODE <PC|PBANK>";
        }
        if (!setUsbHostMode(host)) {
            return String("OK USBMODE=") + (host ? "PBANK" : "PC");
        }
        return String("OK USBMODE=") + (host ? "PBANK" : "PC") + " REBOOTING";
    }
    if (cmd == "PB?") {
        return pbank::statusLine();
    }
    if (cmd == "ANCS?") {
        return ancs::status();
    }
    if (cmd == "MEM?") {
        return memoryReport();
    }
    if (cmd == "CLEAN") {
        const Cleanup done = cleanup();
        return String("OK FREED=") + done.freedBytes + " MSGS=" + done.messages;
    }
    if (cmd == "GAME?" || (cmd == "GAME" && count >= 2)) {
        // 色环方块 remote control (tests / screenshots): GAME NEW|LEFT|RIGHT|ROT|DROP|PAUSE|RESUME
        const std::uint32_t now = millis();
        if (cmd == "GAME") {
            String action = tokens[1];
            action.toUpperCase();
            if (action == "NEW") {
                game::newGame(now);
            } else if (action == "LEFT" || action == "RIGHT") {
                game::move(action == "LEFT" ? -1 : 1, now);
            } else if (action == "ROT") {
                game::rotate(now);
            } else if (action == "DROP") {
                game::drop(now);
            } else if (action == "PAUSE") {
                game::pause(now);
            } else if (action == "RESUME") {
                game::resume(now);
            } else {
                return "ERR GAME NEW|LEFT|RIGHT|ROT|DROP|PAUSE|RESUME";
            }
        }
        game::begin();
        static const char* const kStates[] = {"READY", "PLAYING", "PAUSED", "CLEARING", "OVER"};
        return String("GAME STATE=") + kStates[static_cast<int>(game::state())] + " SCORE=" + game::score()
            + " BEST=" + game::best() + " LINES=" + game::lines() + " LEVEL=" + game::level()
            + " NEXT=" + game::nextType();
    }
    if (cmd == "HID?") {
        return String("HID HOSTS=") + comm::bleKeyboardHosts() + " BONDS=" + comm::bleBondCount();
    }
    if (cmd == "REMOTE") {
        String name = count >= 2 ? tokens[1] : String("");
        name.toUpperCase();
        for (int i = 0; i < static_cast<int>(comm::Remote::Count); ++i) {
            const comm::Remote remote = static_cast<comm::Remote>(i);
            if (name == comm::remoteName(remote)) {
                if (!comm::bleKeyboardActive()) {
                    return "ERR no paired phone connected";
                }
                comm::sendRemote(remote);
                return "OK";
            }
        }
        return "ERR REMOTE <VOLUP|VOLDOWN|PLAY|NEXT|PREV|SHUTTER|MUTE|UP|DOWN|HOME>";
    }
    if (cmd == "UNPAIR") {
        comm::clearBlePairings();
        return "OK pairings cleared";
    }
    if (cmd == "STATE?") {
        return comm::stateJson();
    }
    if (cmd == "SHOT") {
        const int first = count >= 2 ? tokens[1].toInt() : 0;
        const int rows = count >= 3 ? tokens[2].toInt() : 240;
        return ui::screenshot(first, rows);
    }
    if (cmd == "PBLOG?") {
        return pbank::logText();
    }
    if (cmd == "BLE?") {
        return String("BLE=") + (comm::bleEnabled() ? "1" : "0") + " MODE=" + (comm::bleBridge() ? "UART" : "CMD")
            + " NAME=" + comm::bleName() + " CONNECTED=" + (comm::bleConnected() ? String("1 PEER=") + comm::blePeer() : String("0"));
    }
    if (cmd == "BLE") {
        bool on = false;
        if (count < 2 || !parseOnOff(tokens[1], on)) {
            return "ERR BLE <0|1>";
        }
        config.bleEnabled = on;
        saveSettings();
        comm::setBleEnabled(on);
        return "OK";
    }
    if (cmd == "BLEMODE") {
        if (count < 2 || !parseNumber(tokens[1], a) || (a != 0 && a != 1)) {
            return "ERR BLEMODE <0|1>";
        }
        config.bleBridge = a == 1;
        saveSettings();
        comm::setBleBridge(config.bleBridge);
        return config.bleBridge ? "OK UART bridge" : "OK commands";
    }
    if (cmd == "LANG") {
        if (count < 2 || !parseNumber(tokens[1], a) || (a != 0 && a != 1)) {
            return "ERR LANG <0|1>";
        }
        config.language = static_cast<std::uint8_t>(a);
        saveSettings();
        ui::applyDisplaySettings();
        return "OK";
    }
    if (cmd == "TOUCHCAL") {
        ui::startTouchCalibration();
        return "OK touch the four crosses on the screen";
    }
    if (cmd == "TOUCH?") {
        std::uint16_t x = 0;
        std::uint16_t y = 0;
        std::uint16_t z = 0;
        ui::touchRaw(x, y, z);
        return "RAW X=" + String(x) + " Y=" + String(y) + " Z=" + String(z)
            + " CAL=" + String(config.touchValid ? 1 : 0);
    }
    if (cmd == "ROT") {
        if (count < 2 || !parseNumber(tokens[1], a) || a < 0 || a > 3) {
            return "ERR ROT <0..3>";
        }
        config.rotation = static_cast<std::uint8_t>(a);
        saveSettings();
        ui::applyDisplaySettings();
        return "OK";
    }
    if (cmd == "BL") {
        if (count < 2 || !parseNumber(tokens[1], a) || a < 0 || a > 100) {
            return "ERR BL <0..100>";
        }
        config.backlight = static_cast<std::uint8_t>(a);
        saveSettings();
        ui::applyDisplaySettings();
        return "OK";
    }
    if (cmd == "LOCK") {
        bool on = false;
        if (count < 2 || !parseOnOff(tokens[1], on)) {
            return "ERR LOCK <0|1>";
        }
        controller.process({AppEventType::SetLocked, on ? 1 : 0});
        return "OK";
    }
    if (cmd == "CLEAR") {
        return controller.process({AppEventType::ClearFault, 0}) ? "OK" : "ERR";
    }
    if (cmd == "SAVE") {
        saveSettings();
        return "OK";
    }
    if (cmd == "REBOOT") {
        saveSettings();
        controller.process({AppEventType::SetOutput, 0});
        delay(50);
        ESP.restart();
        return "OK";
    }

    return "ERR unknown command, send HELP";
}

}  // namespace app
