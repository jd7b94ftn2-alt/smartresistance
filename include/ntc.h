#pragma once

#include <cstddef>
#include <cstdint>

namespace smart_resistance {

struct NtcType {
    const char* name;
    double r25Ohms;
    double beta;
};

constexpr double kNtcMinTempC = -40.0;
constexpr double kNtcMaxTempC = 150.0;

// Built-in types; index kNtcPresetCount is the user-defined slot (NTCCFG command).
extern const NtcType kNtcPresets[];
extern const std::size_t kNtcPresetCount;

// Beta model: R(T) = R25 * exp(B * (1/T - 1/298.15)), T in kelvin.
double ntcResistanceOhms(double r25Ohms, double beta, double tempC);
double ntcTemperatureC(double r25Ohms, double beta, double resistanceOhms);

}  // namespace smart_resistance
