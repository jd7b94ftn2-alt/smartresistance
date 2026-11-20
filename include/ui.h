#pragma once

#include <Arduino.h>

// 320x240 TFT + EC11 user interface. Rotate = change, short press = next field / action,
// long press (0.6 s) = back to the menu.
namespace ui {

enum class Page : std::uint8_t {
    Menu,
    Resistor,
    Ntc,
    Bluetooth,
    Knob,
    Serial,
    TempLog,
    Info,
    RelayTest,
    About,
    TouchCal,
    Messages,
    Desk,
    Macro,
    Pomodoro,
    NtcPick,
    PowerBank,
    Remote,
    Game,
    System,
    Share,
    Count,
};

void begin();
void loop();

void setPage(Page page);
Page page();
void applyDisplaySettings();   // rotation + backlight from app::settings()
void startTouchCalibration();
// Raw XPT2046 reading (z = pressure, 0 when not pressed); for the TOUCH? command.
bool touchRaw(std::uint16_t& x, std::uint16_t& y, std::uint16_t& z);
// SHOT: screen rows firstRow.. as "SHOT 320 240 <y> <n>" + one "<y> <base64>" line per row;
// each row is RLE: [run-1][RGB565 hi][RGB565 lo] repeated. Read from the frame buffer.
String screenshot(int firstRow, int rows);
// 资源 page / CLEAN: closes what keeps running in the background (a paused 色环方块 game, the
// countdown, the stopwatch). Instrument functions (resistance output, NTC sweep) are left alone.
int closeApps();                       // returns how many were closed

}  // namespace ui
