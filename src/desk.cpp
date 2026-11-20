#include "desk.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <sys/time.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "app.h"
#include "messages.h"
#include "textfont.h"

namespace desk {
namespace {

long offsetMinutes = 480;              // UTC+8 until something tells otherwise
Location place;

void applyZone(long minutes) {
    char tz[20];
    // POSIX TZ has the sign reversed: UTC+8 is "UTC-08:00".
    snprintf(tz, sizeof(tz), "UTC%c%02ld:%02ld", minutes >= 0 ? '-' : '+', labs(minutes) / 60, labs(minutes) % 60);
    setenv("TZ", tz, 1);
    tzset();
}

void saveOffset(long minutes) {
    if (minutes == offsetMinutes) {
        return;
    }
    offsetMinutes = minutes;
    Preferences prefs;
    prefs.begin("desk", false);
    prefs.putInt("tzm", static_cast<std::int32_t>(minutes));
    prefs.end();
}

void savePlace() {
    Preferences prefs;
    prefs.begin("desk", false);
    if (place.valid) {
        prefs.putFloat("wxlat", place.lat);
        prefs.putFloat("wxlon", place.lon);
        prefs.putString("wxcity", place.city);
    } else {
        prefs.remove("wxlat");
        prefs.remove("wxlon");
        prefs.remove("wxcity");
    }
    prefs.end();
}

void renderDetail();

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

// City line under the weather on the clock page (image slot 1), drawn with the board's text font:
// "<city> · <low>~<high>°C · 湿度 <n>%". Used when the weather comes from the phone or the board itself.
void renderDetail() {
    if (!weatherData.valid || !textfont::ready()) {
        return;
    }
    const bool en = app::settings().language == 1;
    String line;
    if (place.valid && place.city.length() > 0) {
        line = place.city + " · ";
    }
    line += String(weatherData.low) + "~" + String(weatherData.high) + "°C · " + (en ? "RH " : "湿度 ")
        + String(weatherData.humidity) + "%";
    constexpr int kW = 224;
    constexpr int kH = 20;
    static std::uint8_t bitmap[((kW + 3) / 4) * kH];
    memset(bitmap, 0, sizeof(bitmap));
    textfont::draw2bpp(bitmap, kW, kH, 0, 0, textfont::fit(line.c_str(), kW).c_str(), 2, kW);
    msg::setImage(1, kW, kH, bitmap);
}

void storeWeather(int code, float temp, int low, int high, int humidity) {
    weatherData.valid = true;
    weatherData.code = code;
    weatherData.temp = temp;
    weatherData.low = low;
    weatherData.high = high;
    weatherData.humidity = humidity;
    weatherMs = millis();
    ++rev;
}

bool numberAfter(const String& body, int from, const char* key, float& out) {
    if (from < 0) {
        return false;
    }
    const int at = body.indexOf(key, from);
    if (at < 0) {
        return false;
    }
    const char* p = body.c_str() + at + strlen(key);
    while (*p == ' ' || *p == ':' || *p == '[') {
        ++p;
    }
    char* end = nullptr;
    out = strtof(p, &end);
    return end != p;
}

}  // namespace

void setTime(long long unixSeconds, long utcOffsetMinutes) {
    timeval tv = {static_cast<time_t>(unixSeconds), 0};
    settimeofday(&tv, nullptr);
    applyZone(utcOffsetMinutes);
    saveOffset(utcOffsetMinutes);
}

long utcOffsetMinutes() {
    return offsetMinutes;
}

void setPlaying(bool playing) {
    if (playingNow != playing) {
        playingNow = playing;
        ++rev;
    }
}

Location location() {
    return place;
}

bool fetchWeather() {
    if (!place.valid) {
        return false;
    }
    char url[300];
    snprintf(url, sizeof(url),
        "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,relative_humidity_2m,weather_code"
        "&daily=temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=1",
        place.lat, place.lon);
    HTTPClient http;
    http.setConnectTimeout(3000);
    http.setTimeout(4000);
    if (!http.begin(url)) {
        return false;
    }
    const int status = http.GET();
    const String body = status == HTTP_CODE_OK ? http.getString() : String();
    http.end();
    const int current = body.indexOf("\"current\":{");
    const int daily = body.indexOf("\"daily\":{");
    float temp = 0;
    float humidity = 0;
    float code = 0;
    float high = 0;
    float low = 0;
    if (!numberAfter(body, current, "\"temperature_2m\"", temp) || !numberAfter(body, current, "\"weather_code\"", code)
        || !numberAfter(body, current, "\"relative_humidity_2m\"", humidity)
        || !numberAfter(body, daily, "\"temperature_2m_max\"", high) || !numberAfter(body, daily, "\"temperature_2m_min\"", low)) {
        return false;
    }
    float offset = 0;
    if (numberAfter(body, 0, "\"utc_offset_seconds\"", offset)) {
        const long minutes = lroundf(offset / 60.0f);
        if (minutes != offsetMinutes) {
            applyZone(minutes);
            saveOffset(minutes);
        }
    }
    storeWeather(static_cast<int>(code), temp, lroundf(low), lroundf(high), lroundf(humidity));
    renderDetail();
    return true;
}

void begin() {
    for (int i = 0; i < kMacroCount; ++i) {
        macros[i] = kDefaultMacros[i];
    }
    Preferences prefs;
    prefs.begin("desk", true);
    offsetMinutes = prefs.getInt("tzm", 480);
    applyZone(offsetMinutes);
    if (prefs.isKey("wxlat")) {
        place.valid = true;
        place.lat = prefs.getFloat("wxlat", 0);
        place.lon = prefs.getFloat("wxlon", 0);
        place.city = prefs.getString("wxcity", "");
    }
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
        setTime(static_cast<long long>(a), strtol(tokens[2].c_str(), nullptr, 10));
        return "";
    }
    if (cmd == "WX") {
        if (count < 6) {
            return "ERR WX <code> <temp x10> <min> <max> <humidity>";
        }
        storeWeather(static_cast<int>(strtol(tokens[1].c_str(), nullptr, 10)), strtol(tokens[2].c_str(), nullptr, 10) / 10.0f,
            static_cast<int>(strtol(tokens[3].c_str(), nullptr, 10)), static_cast<int>(strtol(tokens[4].c_str(), nullptr, 10)),
            static_cast<int>(strtol(tokens[5].c_str(), nullptr, 10)));
        if (place.valid) {
            renderDetail();          // city line; the PC host replaces it with its own IMG slot 1 right after
        }
        return "";
    }
    if (cmd == "WXLOC?") {
        if (!place.valid) {
            return "WXLOC OFF";
        }
        return String("WXLOC ") + String(place.lat, 4) + " " + String(place.lon, 4) + " " + place.city;
    }
    if (cmd == "WXLOC") {
        if (count >= 2 && tokens[1].equalsIgnoreCase("OFF")) {
            place = Location();
            savePlace();
            return "OK";
        }
        if (count < 3) {
            return "ERR WXLOC <lat> <lon> [city] | WXLOC OFF";
        }
        const float lat = tokens[1].toFloat();
        const float lon = tokens[2].toFloat();
        if (lat < -90 || lat > 90 || lon < -180 || lon > 180 || (lat == 0 && lon == 0)) {
            return "ERR lat -90..90, lon -180..180";
        }
        String city;
        for (int i = 3; i < count; ++i) {      // the tokenizer splits on spaces; a city may have some
            if (i > 3) {
                city += " ";
            }
            city += tokens[i];
        }
        if (city.length() > 40) {
            city = city.substring(0, 40);
        }
        const bool same = place.valid && fabsf(place.lat - lat) < 1e-4f && fabsf(place.lon - lon) < 1e-4f && place.city == city;
        if (!same) {                           // the PC / phone resend it often: write NVS only on a change
            place.valid = true;
            place.lat = lat;
            place.lon = lon;
            place.city = city;
            savePlace();
            comm::requestWeatherNow();
        }
        renderDetail();
        return "OK";
    }
    if (cmd == "PLAY") {
        if (count < 2 || !toUint(tokens[1], a)) {
            return "ERR PLAY <0|1>";
        }
        setPlaying(a != 0);
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
