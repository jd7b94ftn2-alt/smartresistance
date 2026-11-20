#include "ble_hid.h"

#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>

namespace blehid {
namespace {

NimBLEHIDDevice* device = nullptr;
NimBLECharacteristic* keyboardIn = nullptr;
NimBLECharacteristic* consumerIn = nullptr;
constexpr int kMaxHosts = 3;
volatile std::uint16_t subscribed[kMaxHosts] = {0xFFFF, 0xFFFF, 0xFFFF};   // conn handles

const std::uint8_t kReportMap[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,             // keyboard, report 1
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,                         //   8 modifier bits
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,                         //   reserved byte
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,   // LED output
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x02,             // consumer control, report 2
    0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xC0,
};

void setSubscribed(std::uint16_t conn, bool on) {
    for (int i = 0; i < kMaxHosts; ++i) {
        if (subscribed[i] == conn) {
            if (!on) {
                subscribed[i] = 0xFFFF;
            }
            return;
        }
    }
    if (!on) {
        return;
    }
    for (int i = 0; i < kMaxHosts; ++i) {
        if (subscribed[i] == 0xFFFF) {
            subscribed[i] = conn;
            return;
        }
    }
}

class ReportCallbacks final : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic* characteristic, ble_gap_conn_desc* desc, std::uint16_t subValue) override {
        (void)characteristic;
        setSubscribed(desc->conn_handle, subValue != 0);
    }
};
ReportCallbacks reportCallbacks;

}  // namespace

void begin(NimBLEServer* server) {
    device = new NimBLEHIDDevice(server);
    keyboardIn = device->inputReport(1);
    device->outputReport(1);                     // keyboard LEDs; hosts write it, ignored
    consumerIn = device->inputReport(2);
    keyboardIn->setCallbacks(&reportCallbacks);
    consumerIn->setCallbacks(&reportCallbacks);
    device->manufacturer()->setValue("SmartR");
    device->pnp(0x02, 0x303A, 0x8100, 0x0100);
    device->hidInfo(0x00, 0x01);
    device->reportMap(const_cast<std::uint8_t*>(kReportMap), sizeof(kReportMap));
    device->startServices();
    device->setBatteryLevel(100);
}

void connectionClosed(std::uint16_t conn) {
    setSubscribed(conn, false);
}

int hosts() {
    int n = 0;
    for (int i = 0; i < kMaxHosts; ++i) {
        n += subscribed[i] != 0xFFFF ? 1 : 0;
    }
    return n;
}

void key(std::uint8_t modifiers, std::uint8_t usage) {
    if (keyboardIn == nullptr || hosts() == 0) {
        return;
    }
    std::uint8_t report[8] = {modifiers, 0, usage, 0, 0, 0, 0, 0};
    keyboardIn->setValue(report, sizeof(report));
    keyboardIn->notify();
    delay(15);
    std::uint8_t released[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    keyboardIn->setValue(released, sizeof(released));
    keyboardIn->notify();
}

void consumer(std::uint16_t usage) {
    if (consumerIn == nullptr || hosts() == 0) {
        return;
    }
    std::uint8_t report[2] = {static_cast<std::uint8_t>(usage & 0xFF), static_cast<std::uint8_t>(usage >> 8)};
    consumerIn->setValue(report, sizeof(report));
    consumerIn->notify();
    delay(15);
    std::uint8_t released[2] = {0, 0};
    consumerIn->setValue(released, sizeof(released));
    consumerIn->notify();
}

}  // namespace blehid
