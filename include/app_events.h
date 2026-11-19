#pragma once

#include <cstdint>

namespace smart_resistance {

enum class AppEventType : std::uint8_t {
    SetResistance,
    AdjustSelectedDigit,
    SelectDigit,
    ToggleOutput,
    SetOutput,
    SetLocked,
    RaiseFault,
    ClearFault,
};

struct AppEvent {
    AppEventType type;
    std::int32_t value;
};

}  // namespace smart_resistance

