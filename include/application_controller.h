#pragma once

#include "app_events.h"
#include "app_state.h"
#include "resistance_controller.h"

namespace smart_resistance {

class ApplicationController {
public:
    explicit ApplicationController(ResistanceController& resistanceController);

    bool initialize(std::uint32_t initialResistanceOhms);
    bool process(const AppEvent& event);

    const AppState& state() const;

    // Calibration is owned here; call recompute() after editing it.
    Calibration& calibration();
    void setSolverEnabled(bool enabled);
    bool solverEnabled() const;
    bool recompute();
    // Relay test: arbitrary 40-bit pattern plus K_OUT state. Leaves targetOhms untouched;
    // the next SetResistance (or recompute) returns to normal operation.
    void setRaw(std::uint64_t shiftWord, bool outputRelay);
    bool rawMode() const;

private:
    bool applyTarget(std::uint32_t targetOhms);
    bool setOutput(bool enabled);
    bool adjustSelectedDigit(std::int32_t delta);

    ResistanceController& resistanceController_;
    Calibration calibration_{};
    AppState state_{};
    bool solverEnabled_ = true;
    bool rawMode_ = false;
};

}  // namespace smart_resistance

