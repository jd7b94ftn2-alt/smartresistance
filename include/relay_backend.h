#pragma once

#include <cstdint>

namespace smart_resistance {

class RelayBackend {
public:
    virtual ~RelayBackend() = default;

    virtual void setOutputConnected(bool connected) = 0;
    virtual void setShiftOutputsEnabled(bool enabled) = 0;
    // Shifts all kRelayChainBits bits (bit39 first) and latches them.
    virtual void writeShiftWord(std::uint64_t word) = 0;
    // High-side switch for the +5V_RELAY coil rail (GPIO39); hardware default is off.
    virtual void setRelayPowerEnabled(bool enabled) = 0;
    virtual void delayMilliseconds(std::uint32_t milliseconds) = 0;
};

}  // namespace smart_resistance
