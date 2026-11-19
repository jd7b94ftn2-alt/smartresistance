#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace smart_resistance {

constexpr std::uint32_t kMaximumResistanceOhms = 999999;
constexpr std::size_t kHighWeightCount = 20;

// Shift chain layout, single source of truth: hardware/relay_map.csv.
// Five 74HC595 = 40 bits; bit39 is shifted first, bit0 last. Bits 36..39 are test-only spares.
constexpr std::uint32_t kRelayChainBits = 40;
constexpr std::uint32_t kUnitFirstBit = 0;          // K_U1..K_U9 = bit0..8, K_U0 = bit9
constexpr std::uint32_t kHighFirstBit = 10;         // K_10R..K_800K = bit10..29
constexpr std::uint32_t kRangeBypassFirstBit = 30;  // K_BYP_GE10..K_BYP_GE100K = bit30..34
constexpr std::uint32_t kShortBit = 35;             // K_SHORT
constexpr std::size_t kRangeBypassCount = 5;
constexpr std::int8_t kNoRangeBypass = -1;

constexpr std::array<std::uint32_t, kHighWeightCount> kNominalHighWeightsOhms = {
    10, 20, 40, 80,
    100, 200, 400, 800,
    1000, 2000, 4000, 8000,
    10000, 20000, 40000, 80000,
    100000, 200000, 400000, 800000,
};

struct RelayPlan {
    std::uint32_t targetOhms = 0;
    std::uint64_t shiftWord = 0;
    std::uint32_t highInsertMask = 0;
    std::uint8_t unitsPath = 0;
    // Index of the closed range bypass: 0=K_BYP_GE10 (bypasses all 20 weights) ... 4=K_BYP_GE100K
    // (bypasses 100K..800K); kNoRangeBypass for six-digit targets.
    std::int8_t rangeBypass = kNoRangeBypass;
    bool shortMode = true;
};

struct Calibration {
    // Absolute resistance of K_U0 through K_U9 paths, excluding K_OUT.
    std::array<double, 10> unitPathOhms = {
        0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0,
    };

    // Increment from a branch's copper-bypass state to inserted-resistor state.
    std::array<double, kHighWeightCount> highIncrementOhms = {
        10.0, 20.0, 40.0, 80.0,
        100.0, 200.0, 400.0, 800.0,
        1000.0, 2000.0, 4000.0, 8000.0,
        10000.0, 20000.0, 40000.0, 80000.0,
        100000.0, 200000.0, 400000.0, 800000.0,
    };

    // HIGH_IN to HIGH_OUT with every weight in copper bypass, per range bypass:
    // index 0..4 = K_BYP_GE10..K_BYP_GE100K closed, index 5 = no range bypass (full network).
    std::array<double, kRangeBypassCount + 1> highPathBaseOhms = {};

    double shortPathOhms = 0.0;
    double outputRelayOhms = 0.0;
    double calibrationTemperatureC = 25.0;
};

RelayPlan makeNominalRelayPlan(std::uint32_t targetOhms);
// Picks the relay combination whose calibrated resistance is closest to targetOhms, using the
// redundant 1-2-4-8 codes (0..15 per decade). Falls back to the nominal plan for target 0.
RelayPlan makeCalibratedRelayPlan(std::uint32_t targetOhms, const Calibration& calibration);
// True when every calibration entry still equals its nominal default.
bool isNominalCalibration(const Calibration& calibration);
bool isRelayPlanValid(const RelayPlan& plan);
double estimateResistanceOhms(const RelayPlan& plan, const Calibration& calibration);

}  // namespace smart_resistance
