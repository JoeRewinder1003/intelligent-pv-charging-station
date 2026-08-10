#include "BatteryEmulator.h"

#include <math.h>

namespace {

constexpr float MIN_RESISTANCE_OHM = 1.0e-6f;
constexpr float MIN_EFFICIENCY = 0.01f;
constexpr float CURRENT_DEADBAND_A = 0.01f;

// Generic 12 V VRLA/GEL OCV-SOC approximation at 25 degC.
// These points are an emulation assumption, not a measured Felicity Solar
// G12V100AH curve. Linear interpolation is used between points.
constexpr size_t OCV_POINT_COUNT = 11;
constexpr float OCV_SOC_POINTS[OCV_POINT_COUNT] = {
    0.0f, 10.0f, 20.0f, 30.0f, 40.0f, 50.0f,
    60.0f, 70.0f, 80.0f, 90.0f, 100.0f,
};
constexpr float OCV_VOLTAGE_POINTS[OCV_POINT_COUNT] = {
    11.90f, 11.98f, 12.06f, 12.14f, 12.22f, 12.30f,
    12.38f, 12.46f, 12.55f, 12.68f, 12.84f,
};

float clampFloat(float value, float minimum, float maximum) {
  if (value < minimum) {
    return minimum;
  }

  if (value > maximum) {
    return maximum;
  }

  return value;
}

}  // namespace

BatteryEmulator::BatteryEmulator(const BatteryConfig& config)
    : config_(config) {
  sanitizeConfiguration();
}

void BatteryEmulator::sanitizeConfiguration() {
  if (config_.batteryCount == 0) {
    config_.batteryCount = 1;
  }

  if (config_.capacityAhPerBattery <= 0.0f) {
    config_.capacityAhPerBattery = 100.0f;
  }

  if (config_.nominalVoltageV <= 0.0f) {
    config_.nominalVoltageV = 12.0f;
  }

  config_.initialSocPercent =
      clampFloat(config_.initialSocPercent, 0.0f, 100.0f);
  config_.emulatedCapacityRetentionPercent =
    clampFloat(
        config_.emulatedCapacityRetentionPercent,
        1.0f,
        100.0f
    );
  config_.chargeEfficiency =
      clampFloat(config_.chargeEfficiency, MIN_EFFICIENCY, 1.0f);
  config_.dischargeEfficiency =
      clampFloat(config_.dischargeEfficiency, MIN_EFFICIENCY, 1.0f);

  if (config_.internalResistancePerBatteryOhm < 0.0f) {
    config_.internalResistancePerBatteryOhm = 0.0f;
  }

  if (config_.wiringResistanceOhm < 0.0f) {
    config_.wiringResistanceOhm = 0.0f;
  }
}

void BatteryEmulator::begin() {
  state_ = BatteryState();
  state_.protectionState = BatteryProtectionState::NORMAL;
  setSocPercent(config_.initialSocPercent);
  solveElectricalState(0.0f);
  updateProtectionState();
}

void BatteryEmulator::update(
    float requestedBatteryPowerW,
    float deltaTimeSeconds
) {
  if (!isfinite(requestedBatteryPowerW) ||
      !isfinite(deltaTimeSeconds) ||
      deltaTimeSeconds <= 0.0f) {
    return;
  }

  solveElectricalState(requestedBatteryPowerW);

  const float deltaTimeHours = deltaTimeSeconds / 3600.0f;
  float storedEnergyDeltaWh = 0.0f;

  if (state_.powerW >= 0.0f) {
    storedEnergyDeltaWh =
        config_.chargeEfficiency * state_.powerW * deltaTimeHours;
  } else {
    storedEnergyDeltaWh =
        (state_.powerW / config_.dischargeEfficiency) * deltaTimeHours;
  }

  const float previousStoredEnergyWh = state_.storedEnergyWh;
  state_.storedEnergyWh = clampFloat(
    state_.storedEnergyWh + storedEnergyDeltaWh,
    0.0f,
    effectiveEnergyWh()
  );

  const float appliedStoredEnergyDeltaWh =
      state_.storedEnergyWh - previousStoredEnergyWh;

  if (appliedStoredEnergyDeltaWh >= 0.0f) {
    state_.chargedEnergyWh += appliedStoredEnergyDeltaWh;
  } else {
    state_.dischargedEnergyWh += -appliedStoredEnergyDeltaWh;
  }

  state_.socPercent =
    100.0f * state_.storedEnergyWh / effectiveEnergyWh();

  state_.consumedCapacityAh =
    effectiveCapacityAh() *
    (100.0f - state_.socPercent) / 100.0f;

  // Recalculate OCV and terminal voltage using the updated SOC while
  // maintaining the same requested external power.
  solveElectricalState(requestedBatteryPowerW);
  updateProtectionState();
}

void BatteryEmulator::setSocPercent(float socPercent) {
  state_.socPercent = clampFloat(socPercent, 0.0f, 100.0f);
  state_.storedEnergyWh =
    effectiveEnergyWh() * state_.socPercent / 100.0f;

  state_.consumedCapacityAh =
    effectiveCapacityAh() *
    (100.0f - state_.socPercent) / 100.0f;
  solveElectricalState(0.0f);
  updateProtectionState();
}

float BatteryEmulator::interpolateOcv(float socPercent) const {
  const float boundedSoc = clampFloat(socPercent, 0.0f, 100.0f);

  if (boundedSoc <= OCV_SOC_POINTS[0]) {
    return OCV_VOLTAGE_POINTS[0];
  }

  if (boundedSoc >= OCV_SOC_POINTS[OCV_POINT_COUNT - 1]) {
    return OCV_VOLTAGE_POINTS[OCV_POINT_COUNT - 1];
  }

  for (size_t index = 1; index < OCV_POINT_COUNT; index++) {
    if (boundedSoc <= OCV_SOC_POINTS[index]) {
      const float lowerSoc = OCV_SOC_POINTS[index - 1];
      const float upperSoc = OCV_SOC_POINTS[index];
      const float fraction =
          (boundedSoc - lowerSoc) / (upperSoc - lowerSoc);

      return OCV_VOLTAGE_POINTS[index - 1] +
             fraction *
                 (OCV_VOLTAGE_POINTS[index] -
                  OCV_VOLTAGE_POINTS[index - 1]);
    }
  }

  return OCV_VOLTAGE_POINTS[OCV_POINT_COUNT - 1];
}

void BatteryEmulator::solveElectricalState(float requestedBatteryPowerW) {
  state_.openCircuitVoltageV = interpolateOcv(state_.socPercent);
  state_.powerModelLimited = false;

  const float resistanceOhm = equivalentResistanceOhm();
  float appliedPowerW = requestedBatteryPowerW;
  float currentA = 0.0f;

  if (resistanceOhm <= MIN_RESISTANCE_OHM) {
    currentA = appliedPowerW / state_.openCircuitVoltageV;
  } else {
    float discriminant =
        state_.openCircuitVoltageV * state_.openCircuitVoltageV +
        4.0f * resistanceOhm * appliedPowerW;

    if (discriminant < 0.0f) {
      // Thévenin model limit. Preserve a small positive discriminant so
      // the emulation remains numerically valid and flag the condition.
      appliedPowerW =
          -0.999f * state_.openCircuitVoltageV *
          state_.openCircuitVoltageV /
          (4.0f * resistanceOhm);
      discriminant =
          state_.openCircuitVoltageV * state_.openCircuitVoltageV +
          4.0f * resistanceOhm * appliedPowerW;
      state_.powerModelLimited = true;
    }

    const float squareRoot = sqrtf(discriminant);
    const float denominator = state_.openCircuitVoltageV + squareRoot;

    if (fabsf(denominator) > MIN_RESISTANCE_OHM) {
      // Numerically stable form of the quadratic solution.
      currentA = 2.0f * appliedPowerW / denominator;
    }
  }

  state_.currentA = currentA;
  state_.terminalVoltageV =
      state_.openCircuitVoltageV +
      state_.currentA * resistanceOhm;
  state_.powerW = state_.terminalVoltageV * state_.currentA;

  state_.charging = state_.currentA > CURRENT_DEADBAND_A;
  state_.discharging = state_.currentA < -CURRENT_DEADBAND_A;

  state_.normalCurrentExceeded =
      state_.currentA > config_.maxNormalChargeCurrentA ||
      state_.currentA < -config_.maxNormalDischargeCurrentA;

  state_.overcurrent =
      state_.currentA > config_.absoluteChargeCurrentA ||
      state_.currentA < -config_.overcurrentDischargeA;

  state_.undervoltage =
      state_.terminalVoltageV <= config_.minimumTerminalVoltageV;
}

void BatteryEmulator::updateProtectionState() {
  if (state_.undervoltage || state_.overcurrent) {
    state_.protectionState = BatteryProtectionState::CRITICAL;
    return;
  }

  switch (state_.protectionState) {
    case BatteryProtectionState::CRITICAL:
      if (state_.socPercent >= config_.criticalRecoverySocPercent) {
        state_.protectionState =
            state_.socPercent >= config_.normalRecoverySocPercent
                ? BatteryProtectionState::NORMAL
                : BatteryProtectionState::RESTRICTED;
      }
      break;

    case BatteryProtectionState::RESTRICTED:
      if (state_.socPercent <= config_.criticalSocPercent) {
        state_.protectionState = BatteryProtectionState::CRITICAL;
      } else if (state_.socPercent >= config_.normalRecoverySocPercent) {
        state_.protectionState = BatteryProtectionState::NORMAL;
      }
      break;

    case BatteryProtectionState::NORMAL:
    default:
      if (state_.socPercent <= config_.criticalSocPercent) {
        state_.protectionState = BatteryProtectionState::CRITICAL;
      } else if (state_.socPercent <= config_.restrictedSocPercent) {
        state_.protectionState = BatteryProtectionState::RESTRICTED;
      }
      break;
  }
}

const BatteryConfig& BatteryEmulator::config() const {
  return config_;
}

const BatteryState& BatteryEmulator::state() const {
  return state_;
}

float BatteryEmulator::bankCapacityAh() const {
  return config_.capacityAhPerBattery * config_.batteryCount;
}

float BatteryEmulator::nominalEnergyWh() const {
  return config_.nominalVoltageV * bankCapacityAh();
}

float BatteryEmulator::effectiveCapacityAh() const {
  return bankCapacityAh() *
         config_.emulatedCapacityRetentionPercent / 100.0f;
}

float BatteryEmulator::effectiveEnergyWh() const {
  return config_.nominalVoltageV * effectiveCapacityAh();
}

float BatteryEmulator::equivalentResistanceOhm() const {
  return config_.internalResistancePerBatteryOhm /
             static_cast<float>(config_.batteryCount) +
         config_.wiringResistanceOhm;
}

const char* BatteryEmulator::protectionStateText() const {
  switch (state_.protectionState) {
    case BatteryProtectionState::RESTRICTED:
      return "RESTRICTED";
    case BatteryProtectionState::CRITICAL:
      return "CRITICAL";
    case BatteryProtectionState::NORMAL:
    default:
      return "NORMAL";
  }
}

bool BatteryEmulator::isCritical() const {
  return state_.protectionState == BatteryProtectionState::CRITICAL;
}

bool BatteryEmulator::isRestricted() const {
  return state_.protectionState == BatteryProtectionState::RESTRICTED;
}
