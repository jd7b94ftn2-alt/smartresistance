#pragma once

#include <cstdint>

namespace board {

constexpr std::uint8_t kTftSclk = 12;
constexpr std::uint8_t kTftMosi = 11;
constexpr std::uint8_t kTftMiso = 13;
constexpr std::uint8_t kTftCs = 10;
constexpr std::uint8_t kTftDc = 9;
constexpr std::uint8_t kTftReset = 14;
constexpr std::uint8_t kTftBacklight = 21;
constexpr std::uint8_t kTouchCs = 8;
constexpr std::uint8_t kTouchIrq = 47;

constexpr std::uint8_t kEncoderA = 4;
constexpr std::uint8_t kEncoderB = 5;
constexpr std::uint8_t kEncoderSwitch = 6;

constexpr std::uint8_t kRelaySerial = 15;
constexpr std::uint8_t kRelayClock = 16;
constexpr std::uint8_t kRelayLatch = 17;
constexpr std::uint8_t kRelayOutputEnableN = 18;
constexpr std::uint8_t kRelayPowerEnable = 39;
constexpr std::uint8_t kOutputRelayEnable = 40;

// USB-C CC voltage through 100k/100k dividers (ADC1); reads half the CC voltage.
constexpr std::uint8_t kUsbCc1Sense = 3;
constexpr std::uint8_t kUsbCc2Sense = 7;

constexpr std::uint8_t kI2cSda = 1;
constexpr std::uint8_t kI2cScl = 2;

// J_DBG header: UART0 pins, used as the serial bridge to a device under test.
constexpr std::uint8_t kUartTx = 43;
constexpr std::uint8_t kUartRx = 44;

}  // namespace board
