#include "ntc.h"

#include <cmath>

namespace smart_resistance {

const NtcType kNtcPresets[] = {
    {"10k B3435", 10000.0, 3435.0},
    {"10k B3380", 10000.0, 3380.0},
    {"10k B3950", 10000.0, 3950.0},
    {"10k B3977", 10000.0, 3977.0},
    {"47k B4050", 47000.0, 4050.0},
    {"50k B3950", 50000.0, 3950.0},
    {"100k B3950", 100000.0, 3950.0},
    {"100k B4250", 100000.0, 4250.0},
    {"5k B3470", 5000.0, 3470.0},
    {"2.2k B3500", 2200.0, 3500.0},
};
const std::size_t kNtcPresetCount = sizeof(kNtcPresets) / sizeof(kNtcPresets[0]);

namespace {
constexpr double kKelvinOffset = 273.15;
constexpr double kT25Kelvin = 298.15;
}  // namespace

double ntcResistanceOhms(double r25Ohms, double beta, double tempC) {
    const double kelvin = tempC + kKelvinOffset;
    return r25Ohms * std::exp(beta * (1.0 / kelvin - 1.0 / kT25Kelvin));
}

double ntcTemperatureC(double r25Ohms, double beta, double resistanceOhms) {
    if (resistanceOhms <= 0.0 || r25Ohms <= 0.0 || beta <= 0.0) {
        return NAN;
    }
    const double inverse = 1.0 / kT25Kelvin + std::log(resistanceOhms / r25Ohms) / beta;
    return 1.0 / inverse - kKelvinOffset;
}

}  // namespace smart_resistance
