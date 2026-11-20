#include "application_controller.h"

#include <algorithm>
#include <array>

namespace smart_resistance {
namespace {

constexpr std::array<std::uint32_t, 6> kDigitMultipliers = {
    1, 10, 100, 1000, 10000, 100000,
};

}  // namespace

ApplicationController::ApplicationController(ResistanceController& resistanceController)
    : resistanceController_(resistanceController) {}

bool ApplicationController::initialize(std::uint32_t initialResistanceOhms) {
    const Calibration keptCalibration = calibration_;
    state_ = AppState{};
    calibration_ = keptCalibration;
    state_.targetOhms = std::min(initialResistanceOhms, kMaximumResistanceOhms);

    // Stage the restored resistance, but require an explicit user command to connect output.
    state_.outputEnabled = false;
    const bool success = applyTarget(state_.targetOhms);
    state_.fault = !success;
    return success;
}

bool ApplicationController::process(const AppEvent& event) {
    if (state_.fault && event.type != AppEventType::ClearFault) {
        resistanceController_.emergencyDisconnect();
        state_.outputEnabled = false;
        return false;
    }

    switch (event.type) {
        case AppEventType::SetResistance:
            if (state_.locked || event.value < 0) {
                return false;
            }
            return applyTarget(static_cast<std::uint32_t>(event.value));

        case AppEventType::AdjustSelectedDigit:
            return !state_.locked && adjustSelectedDigit(event.value);

        case AppEventType::SelectDigit:
            if (event.value < 0 || event.value >= static_cast<std::int32_t>(kDigitMultipliers.size())) {
                return false;
            }
            state_.selectedDigit = static_cast<std::uint8_t>(event.value);
            return true;

        case AppEventType::ToggleOutput:
            return setOutput(!state_.outputEnabled);

        case AppEventType::SetOutput:
            return setOutput(event.value != 0);

        case AppEventType::SetLocked:
            state_.locked = event.value != 0;
            return true;

        case AppEventType::RaiseFault:
            state_.fault = true;
            state_.outputEnabled = false;
            resistanceController_.emergencyDisconnect();
            return true;

        case AppEventType::ClearFault:
            resistanceController_.emergencyDisconnect();
            state_.outputEnabled = false;
            state_.fault = false;
            return applyTarget(state_.targetOhms);
    }

    return false;
}

const AppState& ApplicationController::state() const {
    return state_;
}

Calibration& ApplicationController::calibration() {
    return calibration_;
}

void ApplicationController::setSolverEnabled(bool enabled) {
    solverEnabled_ = enabled;
}

bool ApplicationController::solverEnabled() const {
    return solverEnabled_;
}

bool ApplicationController::recompute() {
    if (state_.fault) {
        return false;
    }
    return applyTarget(state_.targetOhms);
}

void ApplicationController::setRaw(std::uint64_t shiftWord, bool outputRelay) {
    rawMode_ = true;
    state_.outputEnabled = outputRelay;
    resistanceController_.stageRaw(shiftWord, outputRelay);
    state_.relayPlan = resistanceController_.activePlan();
}

bool ApplicationController::rawMode() const {
    return rawMode_;
}

bool ApplicationController::applyTarget(std::uint32_t targetOhms) {
    const std::uint32_t clamped = std::min(targetOhms, kMaximumResistanceOhms);
    rawMode_ = false;
    const bool useSolver = solverEnabled_ && !isNominalCalibration(calibration_);
    const RelayPlan plan = useSolver
        ? makeCalibratedRelayPlan(clamped, calibration_)
        : makeNominalRelayPlan(clamped);
    if (!resistanceController_.stagePlan(plan, state_.outputEnabled)) {
        state_.fault = true;
        state_.outputEnabled = false;
        return false;
    }

    state_.targetOhms = clamped;
    state_.relayPlan = resistanceController_.activePlan();
    state_.estimatedOhms = estimateResistanceOhms(state_.relayPlan, calibration_);
    return true;
}

bool ApplicationController::setOutput(bool enabled) {
    if (!enabled) {
        resistanceController_.emergencyDisconnect();
        state_.outputEnabled = false;
        return true;
    }

    if (state_.fault) {
        state_.outputEnabled = false;
        return false;
    }

    // Re-stage with the output flag set so K_OUT closes only after the network has settled.
    state_.outputEnabled = true;
    if (!applyTarget(state_.targetOhms)) {
        state_.outputEnabled = false;
        return false;
    }
    return true;
}

// Adds delta x (selected digit's weight) with carry / borrow into the higher digits
// (999 + 1 -> 1000, 1000 - 1 -> 999), clamped to 0 .. 999999 instead of wrapping.
bool ApplicationController::adjustSelectedDigit(std::int32_t delta) {
    const std::uint32_t multiplier = kDigitMultipliers[state_.selectedDigit];
    std::int64_t candidate = static_cast<std::int64_t>(state_.targetOhms)
        + static_cast<std::int64_t>(delta) * multiplier;
    if (candidate < 0) {
        candidate = 0;
    } else if (candidate > kMaximumResistanceOhms) {
        candidate = kMaximumResistanceOhms;
    }
    if (static_cast<std::uint32_t>(candidate) == state_.targetOhms) {
        return false;          // already at the limit
    }
    return applyTarget(static_cast<std::uint32_t>(candidate));
}

}  // namespace smart_resistance
