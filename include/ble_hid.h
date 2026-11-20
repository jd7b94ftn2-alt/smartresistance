#pragma once

#include <Arduino.h>

class NimBLEServer;

// Bluetooth keyboard (BLE HID: keyboard report 1 + consumer-control report 2) on the comm server.
// Kept in its own translation unit: NimBLE's HIDTypes.h defines macros (HID_SUBCLASS_NONE, ...)
// that clash with TinyUSB's hid.h, which comm.cpp needs for the USB keyboard.
namespace blehid {

void begin(NimBLEServer* server);         // creates the HID services; call before advertising starts
void connectionClosed(std::uint16_t conn);
int hosts();                               // connected hosts subscribed to the reports
void key(std::uint8_t modifiers, std::uint8_t usage);   // press + release, HID keyboard usage id
void consumer(std::uint16_t usage);                     // press + release, consumer page usage id

}  // namespace blehid
