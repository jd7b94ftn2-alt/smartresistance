#pragma once

#include <cstdint>

#include "resistance_model.h"

namespace smart_resistance {

struct AppState {
    std::uint32_t targetOhms = 1000;
    double estimatedOhms = 1000.0;
    std::uint8_t selectedDigit = 0;
    bool outputEnabled = false;
    bool locked = false;
    bool fault = false;
    RelayPlan relayPlan{};
};

}  // namespace smart_resistance

