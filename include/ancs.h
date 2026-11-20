#pragma once

#include <Arduino.h>

// iPhone notifications on the board screen (Apple Notification Center Service).
//
// Once an iPhone has paired with the board's Bluetooth keyboard (ble_hid.cpp) the link is encrypted,
// and the board, acting as GATT client on that same link, subscribes to ANCS. Every new notification
// (not the ones already on the phone) is fetched - app, title, message, time - rendered with the
// text font into a message card and shown like the PC-forwarded ones (banner + Messages page).
// Other peers (PC, Android) simply have no ANCS service and are left alone.
namespace ancs {

void begin();            // after NimBLEDevice::init(): registers the GAP event listener
void loop();             // loop task: turns fetched notifications into cards
bool connected();        // an iPhone's ANCS is subscribed
std::uint32_t received();
String status();         // ANCS? reply

}  // namespace ancs
