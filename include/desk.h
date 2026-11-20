#pragma once

#include <Arduino.h>

#include <cstdint>

#include "comm.h"

// Data for the Desk page pushed by the PC host, and the Macro page button assignment.
//   SYS <cpu%> <ram%> <down B/s> <up B/s>   PC load (no reply)
//   PLAY <0|1>                              media playing (no reply); title image = IMG slot 0
//   CI <0 none|1 running|2 ok|3 failed> <HH:MM>   firmware build status (no reply)
//   MACRO <0..5> <action>  |  MACRO?        Macro page buttons (saved in NVS)
//   TIME <unix seconds> <UTC offset minutes>  set the clock without Wi-Fi/NTP (no reply)
//   WX <WMO code> <temp x10> <min> <max> <humidity%>   weather for the clock page (no reply);
//                                           the city / detail line is IMG slot 1
//   WXLOC <lat> <lon> [city]  |  WXLOC?  |  WXLOC OFF
//                                           saved location: the board then fetches open-meteo itself
//                                           whenever it is on a Wi-Fi with internet (e.g. the phone's
//                                           hotspot), and draws the city line itself
// Without the PC the same data comes from the phone: time from the iPhone (apple.cpp, CTS) or NTP,
// now playing from the iPhone (AMS), weather from the phone web page (WX + WXLOC) or the board's own fetch.
namespace desk {

constexpr int kMacroCount = 6;

struct Weather {
    bool valid = false;            // false until WX arrives, and again after 3 hours without updates
    int code = 0;                  // WMO weather code (open-meteo)
    float temp = 0;
    int low = 0;
    int high = 0;
    int humidity = 0;
};

struct Sys {
    bool valid = false;            // false when nothing arrived for a while
    std::uint8_t cpu = 0;
    std::uint8_t ram = 0;
    std::uint32_t down = 0;        // bytes per second
    std::uint32_t up = 0;
};

struct Location {
    bool valid = false;
    float lat = 0;
    float lon = 0;
    String city;                   // UTF-8, may be empty
};

void begin();
String handleCommand(const String& cmd, const String* tokens, int count);

// Clock: sets the system time and time zone; the UTC offset is saved and also used for NTP.
void setTime(long long unixSeconds, long utcOffsetMinutes);
long utcOffsetMinutes();           // last known offset (TIME, iPhone, weather API), default UTC+8
void setPlaying(bool playing);     // now-playing state from the PC (PLAY) or the iPhone (AMS)

Location location();
bool fetchWeather();               // open-meteo for location(); blocking (needs Wi-Fi + internet)

Sys sys();
Weather weather();
const char* weatherText(int code);              // Chinese / English description of a WMO code
bool playing();
int ciState();                     // 0 none, 1 running, 2 ok, 3 failed
String ciTime();
std::uint32_t revision();          // changes whenever SYS / PLAY / CI data changes

comm::Action macro(int slot);
const char* actionLabel(comm::Action action);   // Chinese / English per app::settings().language

}  // namespace desk
