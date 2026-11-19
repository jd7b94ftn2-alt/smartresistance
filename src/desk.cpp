#include "desk.h"

#include <Preferences.h>
#include <sys/time.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>

#include "app.h"

namespace desk {
namespace {

constexpr std::uint32_t kSysTimeoutMs = 8000;
const comm::Action kDefaultMacros[kMacroCount] = {
    comm::Action::Screenshot, comm::Action::Lock, comm::Action::ShowDesktop,
    comm::Action::FeishuMic, comm::Action::PlayPause, comm::Action::Mute,
};

constexpr std::uint32_t kWeatherTimeoutMs = 3UL * 3600UL * 1000UL;

Sys sysData;
std::uint32_t sysMs = 0;
Weather weatherData;
std::uint32_t weatherMs = 0;
bool playingNow = false;
int ci = 0;
String ciAt;
std::uint32_t rev = 1;
comm::Action macros[kMacroCount];

bool toUint(const String& text, std::uint32_t& value) {
    if (text.length() == 0) {
        return false;
    }
    char* end = nullptr;
    value = static_cast<std::uint32_t>(strtoul(text.c_str(), &end, 10));
    return end != nullptr && *end == '\0';
}

void saveMacros() {
    Preferences prefs;
    prefs.begin("desk", false);
    std::uint8_t raw[kMacroCount];
    for (int i = 0; i < kMacroCount; ++i) {
        raw[i] = static_cast<std::uint8_t>(macros[i]);
    }
    prefs.putBytes("macro", raw, sizeof(raw));
    prefs.end();
}

}  // namespace

void begin() {
    for (int i = 0; i < kMacroCount; ++i) {
        macros[i] = kDefaultMacros[i];
    }
    Preferences prefs;
    prefs.begin("desk", true);
    std::uint8_t raw[kMacroCount];
    if (prefs.getBytes("macro", raw, sizeof(raw)) == sizeof(raw)) {
        for (int i = 0; i < kMacroCount; ++i) {
            if (raw[i] < static_cast<std::uint8_t>(comm::Action::Count)) {
                macros[i] = static_cast<comm::Action>(raw[i]);
            }
        }
    }
    prefs.end();
}

String handleCommand(const String& cmd, const String* tokens, int count) {
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t c = 0;
    std::uint32_t d = 0;
    if (cmd == "SYS") {
        if (count < 5 || !toUint(tokens[1], a) || !toUint(tokens[2], b) || !toUint(tokens[3], c) || !toUint(tokens[4], d)) {
            return "ERR SYS <cpu> <ram> <down> <up>";
        }
        sysData.valid = true;
        sysData.cpu = static_cast<std::uint8_t>(std::min<std::uint32_t>(a, 100));
        sysData.ram = static_cast<std::uint8_t>(std::min<std::uint32_t>(b, 100));
        sysData.down = c;
        sysData.up = d;
        sysMs = millis();
        ++rev;
        return "";
    }
    if (cmd == "TIME") {
        if (count < 3 || !toUint(tokens[1], a)) {
            return "ERR TIME <unix> <utc offset minutes>";
        }
        const long offset = strtol(tokens[2].c_str(), nullptr, 10);
        timeval tv = {static_cast<time_t>(a), 0};
        settimeofday(&tv, nullptr);
        char tz[20];
        // POSIX TZ has the sign reversed: UTC+8 is "UTC-08:00".
        snprintf(tz, sizeof(tz), "UTC%c%02ld:%02ld", offset >= 0 ? '-' : '+', labs(offset) / 60, labs(offset) % 60);
        setenv("TZ", tz, 1);
        tzset();
        return "";
    }
    if (cmd == "WX") {
        if (count < 6) {
            return "ERR WX <code> <temp x10> <min> <max> <humidity>";
        }
        weatherData.valid = true;
        weatherData.code = static_cast<int>(strtol(tokens[1].c_str(), nullptr, 10));
        weatherData.temp = strtol(tokens[2].c_str(), nullptr, 10) / 10.0f;
        weatherData.low = static_cast<int>(strtol(tokens[3].c_str(), nullptr, 10));
        weatherData.high = static_cast<int>(strtol(tokens[4].c_str(), nullptr, 10));
        weatherData.humidity = static_cast<int>(strtol(tokens[5].c_str(), nullptr, 10));
        weatherMs = millis();
        ++rev;
        return "";
    }
    if (cmd == "PLAY") {
        if (count < 2 || !toUint(tokens[1], a)) {
            return "ERR PLAY <0|1>";
        }
        if (playingNow != (a != 0)) {
            playingNow = a != 0;
            ++rev;
        }
        return "";
    }
    if (cmd == "CI") {
        if (count < 2 || !toUint(tokens[1], a) || a > 3) {
            return "ERR CI <0..3> [HH:MM]";
        }
        const String at = count > 2 ? tokens[2] : String();
        if (ci != static_cast<int>(a) || ciAt != at) {
            ci = static_cast<int>(a);
            ciAt = at;
            ++rev;
        }
        return "";
    }
    if (cmd == "MACRO?") {
        String out;
        for (int i = 0; i < kMacroCount; ++i) {
            if (i > 0) {
                out += " ";
            }
            out += String(static_cast<int>(macros[i]));
        }
        return out;
    }
    if (cmd == "MACRO") {
        if (count < 3 || !toUint(tokens[1], a) || !toUint(tokens[2], b) || a >= kMacroCount
            || b >= static_cast<std::uint32_t>(comm::Action::Count)) {
            return "ERR MACRO <0..5> <action 0..15>";
        }
        macros[a] = static_cast<comm::Action>(b);
        saveMacros();
        ++rev;
        return "OK";
    }
    return "ERR unknown desk command";
}

Sys sys() {
    Sys s = sysData;
    if (!s.valid || millis() - sysMs > kSysTimeoutMs) {
        s.valid = false;
    }
    return s;
}

Weather weather() {
    Weather w = weatherData;
    if (!w.valid || millis() - weatherMs > kWeatherTimeoutMs) {
        w.valid = false;
    }
    return w;
}

const char* weatherText(int code) {
    const bool en = app::settings().language == 1;
    if (code == 0) return en ? "Clear" : "晴";
    if (code == 1) return en ? "Mostly clear" : "晴间多云";
    if (code == 2) return en ? "Partly cloudy" : "多云";
    if (code == 3) return en ? "Overcast" : "阴";
    if (code == 45 || code == 48) return en ? "Fog" : "雾";
    if (code >= 51 && code <= 57) return en ? "Drizzle" : "毛毛雨";
    if (code == 61) return en ? "Light rain" : "小雨";
    if (code == 63) return en ? "Rain" : "中雨";
    if (code == 65) return en ? "Heavy rain" : "大雨";
    if (code == 66 || code == 67) return en ? "Freezing rain" : "冻雨";
    if (code == 71) return en ? "Light snow" : "小雪";
    if (code == 73) return en ? "Snow" : "中雪";
    if (code == 75 || code == 77) return en ? "Heavy snow" : "大雪";
    if (code >= 80 && code <= 82) return en ? "Showers" : "阵雨";
    if (code == 85 || code == 86) return en ? "Snow showers" : "阵雪";
    if (code == 95) return en ? "Thunderstorm" : "雷阵雨";
    if (code == 96 || code == 99) return en ? "Thunder + hail" : "雷阵雨伴冰雹";
    return en ? "Unknown" : "未知";
}

bool playing() {
    return playingNow;
}

int ciState() {
    return ci;
}

String ciTime() {
    return ciAt;
}

std::uint32_t revision() {
    // SYS going stale also counts as a change (once per timeout period).
    return rev * 2U + ((sysData.valid && millis() - sysMs > kSysTimeoutMs) ? 1U : 0U);
}

comm::Action macro(int slot) {
    return slot >= 0 && slot < kMacroCount ? macros[slot] : comm::Action::Screenshot;
}

const char* actionLabel(comm::Action action) {
    const bool en = app::settings().language == 1;
    switch (action) {
        case comm::Action::Screenshot: return en ? "Screenshot" : "截图";
        case comm::Action::Lock: return en ? "Lock" : "锁屏";
        case comm::Action::ShowDesktop: return en ? "Desktop" : "显示桌面";
        case comm::Action::TaskManager: return en ? "Task Mgr" : "任务管理器";
        case comm::Action::Copy: return en ? "Copy" : "复制";
        case comm::Action::Paste: return en ? "Paste" : "粘贴";
        case comm::Action::Undo: return en ? "Undo" : "撤销";
        case comm::Action::SwitchWindow: return en ? "Alt+Tab" : "切换窗口";
        case comm::Action::Mute: return en ? "Mute" : "静音";
        case comm::Action::PlayPause: return en ? "Play/Pause" : "播放/暂停";
        case comm::Action::PrevTrack: return en ? "Previous" : "上一首";
        case comm::Action::NextTrack: return en ? "Next" : "下一首";
        case comm::Action::FeishuMic: return en ? "Feishu mic" : "飞书麦克风";
        case comm::Action::FeishuCamera: return en ? "Feishu cam" : "飞书摄像头";
        case comm::Action::VolumeUp: return en ? "Volume +" : "音量 +";
        case comm::Action::VolumeDown: return en ? "Volume -" : "音量 -";
        default: return "?";
    }
}

}  // namespace desk
