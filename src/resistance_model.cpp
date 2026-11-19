#include "resistance_model.h"

#include <algorithm>

namespace smart_resistance {
namespace {

constexpr std::uint64_t kUnitRelayMask = 0x3FFULL << kUnitFirstBit;
constexpr std::uint64_t kHighRelayMask = 0xFFFFFULL;
constexpr std::uint64_t kRangeBypassMask = 0x1FULL << kRangeBypassFirstBit;
constexpr std::uint64_t kUsedChainMask = (1ULL << (kShortBit + 1)) - 1;

std::uint64_t unitPathBit(std::uint8_t units) {
    return units == 0 ? (1ULL << (kUnitFirstBit + 9)) : (1ULL << (kUnitFirstBit + units - 1));
}

std::uint32_t popcount(std::uint64_t value) {
    std::uint32_t count = 0;
    while (value != 0) {
        value &= value - 1;
        ++count;
    }
    return count;
}

std::uint32_t decimalDigits(std::uint32_t value) {
    std::uint32_t digits = 1;
    while (value >= 10) {
        value /= 10;
        ++digits;
    }
    return digits;
}

// Weights at index >= 4 * bypass are shorted by the closed range bypass and must stay unused.
std::uint32_t weightsBypassedBy(std::int8_t rangeBypass) {
    if (rangeBypass == kNoRangeBypass) {
        return 0;
    }
    return static_cast<std::uint32_t>(kHighRelayMask) & ~((1UL << (4 * rangeBypass)) - 1UL);
}

}  // namespace

RelayPlan makeNominalRelayPlan(std::uint32_t targetOhms) {
    RelayPlan plan;
    plan.targetOhms = std::min(targetOhms, kMaximumResistanceOhms);

    if (plan.targetOhms == 0) {
        plan.shortMode = true;
        plan.shiftWord = 1ULL << kShortBit;
        return plan;
    }

    plan.shortMode = false;
    plan.unitsPath = static_cast<std::uint8_t>(plan.targetOhms % 10UL);
    plan.shiftWord = unitPathBit(plan.unitsPath);

    std::uint32_t remaining = plan.targetOhms / 10UL;
    for (std::uint32_t decade = 0; decade < 5; ++decade) {
        const std::uint32_t digit = remaining % 10UL;
        remaining /= 10UL;
        plan.highInsertMask |= digit << (decade * 4UL);
    }
    plan.shiftWord |= static_cast<std::uint64_t>(plan.highInsertMask) << kHighFirstBit;

    // 1-digit targets close K_BYP_GE10, 2-digit K_BYP_GE100, ... 5-digit K_BYP_GE100K.
    const std::uint32_t digits = decimalDigits(plan.targetOhms);
    if (digits <= kRangeBypassCount) {
        plan.rangeBypass = static_cast<std::int8_t>(digits - 1);
        plan.shiftWord |= 1ULL << (kRangeBypassFirstBit + plan.rangeBypass);
    }

    return plan;
}

bool isRelayPlanValid(const RelayPlan& plan) {
    if (plan.targetOhms > kMaximumResistanceOhms || (plan.shiftWord & ~kUsedChainMask) != 0) {
        return false;
    }

    const bool shortBitSet = (plan.shiftWord & (1ULL << kShortBit)) != 0;
    const std::uint64_t unitBits = plan.shiftWord & kUnitRelayMask;
    const std::uint64_t bypassBits = plan.shiftWord & kRangeBypassMask;
    const std::uint32_t encodedHighMask =
        static_cast<std::uint32_t>((plan.shiftWord >> kHighFirstBit) & kHighRelayMask);

    if (plan.targetOhms == 0) {
        return plan.shortMode && plan.shiftWord == (1ULL << kShortBit);
    }

    if (plan.shortMode || shortBitSet || popcount(unitBits) != 1) {
        return false;
    }

    if (plan.unitsPath > 9 || unitBits != unitPathBit(plan.unitsPath)) {
        return false;
    }

    if (plan.highInsertMask != encodedHighMask || popcount(bypassBits) > 1) {
        return false;
    }

    if (plan.rangeBypass == kNoRangeBypass) {
        return bypassBits == 0;
    }

    if (plan.rangeBypass < 0 || plan.rangeBypass >= static_cast<std::int8_t>(kRangeBypassCount)
        || bypassBits != (1ULL << (kRangeBypassFirstBit + plan.rangeBypass))) {
        return false;
    }

    // An inserted weight behind a closed bypass would silently drop out of the result.
    return (plan.highInsertMask & weightsBypassedBy(plan.rangeBypass)) == 0;
}

double estimateResistanceOhms(const RelayPlan& plan, const Calibration& calibration) {
    if (plan.shortMode) {
        return calibration.outputRelayOhms + calibration.shortPathOhms;
    }

    const std::size_t baseIndex = plan.rangeBypass == kNoRangeBypass
        ? kRangeBypassCount
        : static_cast<std::size_t>(plan.rangeBypass);
    double result = calibration.outputRelayOhms
        + calibration.unitPathOhms[plan.unitsPath]
        + calibration.highPathBaseOhms[baseIndex];
    for (std::size_t index = 0; index < kHighWeightCount; ++index) {
        if ((plan.highInsertMask & (1UL << index)) != 0) {
            result += calibration.highIncrementOhms[index];
        }
    }
    return result;
}

bool isNominalCalibration(const Calibration& calibration) {
    const Calibration nominal{};
    return calibration.unitPathOhms == nominal.unitPathOhms
        && calibration.highIncrementOhms == nominal.highIncrementOhms
        && calibration.highPathBaseOhms == nominal.highPathBaseOhms
        && calibration.shortPathOhms == nominal.shortPathOhms
        && calibration.outputRelayOhms == nominal.outputRelayOhms;
}

RelayPlan makeCalibratedRelayPlan(std::uint32_t targetOhms, const Calibration& calibration) {
    const std::uint32_t target = std::min(targetOhms, kMaximumResistanceOhms);
    if (target == 0) {
        return makeNominalRelayPlan(0);
    }

    struct Sum {
        double ohms;
        std::uint16_t mask;
    };
    static std::array<Sum, 1024> highSums;  // loop task only; too large for the stack

    const RelayPlan nominal = makeNominalRelayPlan(target);
    const double wanted = static_cast<double>(target);
    const std::int32_t noBypassLevel = static_cast<std::int32_t>(kRangeBypassCount);
    double bestError = 1e18;
    RelayPlan best = nominal;

    // Start at the nominal range bypass and also try wider ranges: a wider range adds contacts
    // but lets the solver reach targets the narrower one cannot hit.
    const std::int32_t firstLevel =
        nominal.rangeBypass == kNoRangeBypass ? noBypassLevel : nominal.rangeBypass;
    for (std::int32_t level = firstLevel; level <= noBypassLevel; ++level) {
        const std::size_t weightCount =
            level == noBypassLevel ? kHighWeightCount : static_cast<std::size_t>(4 * level);
        const std::size_t lowCount = std::min<std::size_t>(weightCount, 10);
        const std::size_t highCount = weightCount - lowCount;
        const std::size_t highSize = static_cast<std::size_t>(1) << highCount;

        for (std::size_t mask = 0; mask < highSize; ++mask) {
            double sum = 0.0;
            for (std::size_t bit = 0; bit < highCount; ++bit) {
                if ((mask & (static_cast<std::size_t>(1) << bit)) != 0) {
                    sum += calibration.highIncrementOhms[lowCount + bit];
                }
            }
            highSums[mask] = {sum, static_cast<std::uint16_t>(mask)};
        }
        std::sort(highSums.begin(), highSums.begin() + highSize,
            [](const Sum& a, const Sum& b) { return a.ohms < b.ohms; });

        const double base = calibration.outputRelayOhms + calibration.highPathBaseOhms[level];
        const std::size_t lowSize = static_cast<std::size_t>(1) << lowCount;
        const Sum* const begin = highSums.data();
        const Sum* const end = begin + highSize;
        double levelBestError = 1e18;
        std::uint32_t levelBestMask = 0;
        std::uint8_t levelBestUnits = 0;
        for (std::uint8_t units = 0; units < 10; ++units) {
            for (std::size_t lowMask = 0; lowMask < lowSize; ++lowMask) {
                double lowSum = base + calibration.unitPathOhms[units];
                for (std::size_t bit = 0; bit < lowCount; ++bit) {
                    if ((lowMask & (static_cast<std::size_t>(1) << bit)) != 0) {
                        lowSum += calibration.highIncrementOhms[bit];
                    }
                }
                const double remainder = wanted - lowSum;
                const Sum* const upper = std::lower_bound(begin, end, remainder,
                    [](const Sum& entry, double value) { return entry.ohms < value; });
                const Sum* const lower = upper == begin ? upper : upper - 1;
                const Sum* const candidates[2] = {upper, lower};
                for (const Sum* candidate : candidates) {
                    if (candidate == end) {
                        continue;
                    }
                    const double error = lowSum + candidate->ohms - wanted;
                    const double magnitude = error < 0 ? -error : error;
                    if (magnitude < levelBestError) {
                        levelBestError = magnitude;
                        levelBestMask = static_cast<std::uint32_t>(lowMask)
                            | (static_cast<std::uint32_t>(candidate->mask) << lowCount);
                        levelBestUnits = units;
                    }
                }
            }
        }

        // Prefer the narrower range unless a wider one is clearly (>20 milliohm) closer.
        if (levelBestError + 0.02 < bestError) {
            bestError = levelBestError;
            RelayPlan plan;
            plan.targetOhms = target;
            plan.shortMode = false;
            plan.unitsPath = levelBestUnits;
            plan.highInsertMask = levelBestMask;
            plan.rangeBypass =
                level == noBypassLevel ? kNoRangeBypass : static_cast<std::int8_t>(level);
            plan.shiftWord = unitPathBit(levelBestUnits)
                | (static_cast<std::uint64_t>(levelBestMask) << kHighFirstBit);
            if (plan.rangeBypass != kNoRangeBypass) {
                plan.shiftWord |= 1ULL << (kRangeBypassFirstBit + plan.rangeBypass);
            }
            best = plan;
        }
    }

    return isRelayPlanValid(best) ? best : nominal;
}

}  // namespace smart_resistance
