#pragma once

#include <cstdint>

#include "relay_backend.h"
#include "resistance_model.h"

namespace smart_resistance {

class ResistanceController {
public:
    explicit ResistanceController(RelayBackend& backend);

    // Clears the whole shift chain, enables the 74HC595 outputs and powers the coil rail.
    void begin();
    bool stage(std::uint32_t targetOhms, bool connectAfterSwitching);
    // Same sequencing as stage(), for a plan the caller already computed (calibrated solver).
    bool stagePlan(const RelayPlan& plan, bool connectAfterSwitching);
    // Relay test only: writes an arbitrary 40-bit pattern, no plan validation.
    void stageRaw(std::uint64_t shiftWord, bool connectAfterSwitching);
    void emergencyDisconnect();

    const RelayPlan& activePlan() const;

private:
    RelayBackend& backend_;
    RelayPlan activePlan_{};
};

}  // namespace smart_resistance

