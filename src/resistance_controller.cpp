#include "resistance_controller.h"

namespace smart_resistance {

namespace {

// Q1 gate soft-start (R8 47k, C7 1uF) plus charging C5 470uF; ~30 ms nominal, doubled for margin.
constexpr std::uint32_t kRelayRailSettleMs = 100;
constexpr std::uint32_t kOutputBreakMs = 5;
constexpr std::uint32_t kRelaySettleMs = 20;

}  // namespace

ResistanceController::ResistanceController(RelayBackend& backend) : backend_(backend) {}

void ResistanceController::begin() {
    backend_.setOutputConnected(false);
    // Power-up 74HC595 contents are random: clear all 40 bits while outputs are still disabled.
    backend_.setShiftOutputsEnabled(false);
    backend_.writeShiftWord(0);
    backend_.setShiftOutputsEnabled(true);
    backend_.setRelayPowerEnabled(true);
    backend_.delayMilliseconds(kRelayRailSettleMs);
}

bool ResistanceController::stage(
    std::uint32_t targetOhms,
    bool connectAfterSwitching
) {
    return stagePlan(makeNominalRelayPlan(targetOhms), connectAfterSwitching);
}

bool ResistanceController::stagePlan(const RelayPlan& nextPlan, bool connectAfterSwitching) {
    if (!isRelayPlanValid(nextPlan)) {
        emergencyDisconnect();
        return false;
    }

    // Break before make: the external circuit never sees intermediate relay states.
    backend_.setOutputConnected(false);
    backend_.delayMilliseconds(kOutputBreakMs);

    // The 74HC595 storage latch holds the old pattern while shifting, so OE stays enabled:
    // toggling it would release and re-operate every relay on each change.
    backend_.writeShiftWord(nextPlan.shiftWord);
    backend_.delayMilliseconds(kRelaySettleMs);

    activePlan_ = nextPlan;
    backend_.setOutputConnected(connectAfterSwitching);
    return true;
}

void ResistanceController::stageRaw(std::uint64_t shiftWord, bool connectAfterSwitching) {
    const std::uint64_t word = shiftWord & ((1ULL << kRelayChainBits) - 1);
    backend_.setOutputConnected(false);
    backend_.delayMilliseconds(kOutputBreakMs);
    backend_.writeShiftWord(word);
    backend_.delayMilliseconds(kRelaySettleMs);
    activePlan_ = RelayPlan{};
    activePlan_.shiftWord = word;
    backend_.setOutputConnected(connectAfterSwitching);
}

void ResistanceController::emergencyDisconnect() {
    backend_.setOutputConnected(false);
}

const RelayPlan& ResistanceController::activePlan() const {
    return activePlan_;
}

}  // namespace smart_resistance
