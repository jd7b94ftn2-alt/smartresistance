#pragma once

#include <Arduino.h>

#include <cstdint>

struct os_mbuf;

// iPhone services on the same encrypted BLE link as ANCS (ancs.cpp drives the GAP events):
//  - AMS, Apple Media Service: now-playing title / artist / player app / play state, plus remote
//    commands (play / pause / next / previous / volume);
//  - CTS, Current Time Service: the phone's local time and time zone, read once per connection.
// So the Desk page's "now playing" card and the clock work with only the phone (no PC host).
namespace apple {

enum class Command : std::uint8_t {
    Play = 0, Pause = 1, TogglePlayPause = 2, Next = 3, Previous = 4, VolumeUp = 5, VolumeDown = 6,
};

void begin();                          // loop task, once
void loop();                           // loop task: applies the phone's time, renders the track card

// ---- host task, called from ancs.cpp ----
void start(std::uint16_t conn, void (*done)());   // after ANCS set-up; done() runs when AMS / CTS are set up
bool busy();                           // set-up in progress (ANCS holds its control-point writes)
bool onNotify(std::uint16_t conn, std::uint16_t handle, os_mbuf* om);   // true = AMS data, consumed
void disconnected(std::uint16_t conn);

// ---- loop task ----
bool mediaReady();                     // AMS subscribed on a connected iPhone
bool mediaShown();                     // the Desk page card (image slot 0) currently shows the phone's track
bool command(Command cmd);             // false when AMS is not available
String status();                       // "IPHONE AMS=1 CTS=1 TITLE=... STATE=..."

}  // namespace apple
