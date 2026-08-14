#include "BatteryCapacityTest.h"

namespace {

constexpr float CAPACITY_TEST_END_VOLTAGE_V = 10.8f;

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


// ============================================================
// Battery capacity test plant
// ============================================================

BatteryCapacityTestPlant::BatteryCapacityTestPlant(
    const BatteryCapacityTestPlantConfig& config
)
    : config_(config) {

  if (config_.nominalCapacityAh <= 0.0f) {
    config_.nominalCapacityAh = 300.0f;
  }

  config_.capacityRetentionPercent =
      clampFloat(config_.capacityRetentionPercent, 0.0f, 100.0f);

  if (config_.dischargeCurrentA >= 0.0f) {
    config_.dischargeCurrentA = -30.0f;
  }

  if (config_.startVoltageV <= config_.endVoltageV) {
    config_.startVoltageV = 12.8f;
    config_.endVoltageV = 10.8f;
  }
}

void BatteryCapacityTestPlant::start() {
  dischargedCapacityAh_ = 0.0f;

  sample_.voltageV = config_.startVoltageV;
  sample_.currentA = config_.dischargeCurrentA;

  state_ = BatteryCapacityTestState::RUNNING;
}

void BatteryCapacityTestPlant::update(float deltaTimeSeconds) {
  if (state_ != BatteryCapacityTestState::RUNNING) {
    return;
  }

  if (!isfinite(deltaTimeSeconds) || deltaTimeSeconds <= 0.0f) {
    return;
  }

  const float availableCapacityAh =
      config_.nominalCapacityAh *
      config_.capacityRetentionPercent /
      100.0f;

  if (availableCapacityAh <= 0.0f) {
    sample_.voltageV = config_.endVoltageV;
    sample_.currentA = 0.0f;
    state_ = BatteryCapacityTestState::COMPLETED;
    return;
  }

  const float requestedDischargeAh =
      (-config_.dischargeCurrentA) *
      deltaTimeSeconds /
      3600.0f;

  const float remainingCapacityAh =
      availableCapacityAh - dischargedCapacityAh_;

  const float actualDischargeAh =
      requestedDischargeAh < remainingCapacityAh
          ? requestedDischargeAh
          : remainingCapacityAh;

  dischargedCapacityAh_ += actualDischargeAh;

  sample_.currentA =
      -(actualDischargeAh * 3600.0f / deltaTimeSeconds);

  const float dischargeFraction =
      clampFloat(
          dischargedCapacityAh_ / availableCapacityAh,
          0.0f,
          1.0f
      );

  // Simplified representative discharge-voltage curve.
  sample_.voltageV =
      config_.startVoltageV +
      (config_.endVoltageV - config_.startVoltageV) *
      dischargeFraction;

  if (dischargedCapacityAh_ >= availableCapacityAh) {
    sample_.voltageV = config_.endVoltageV;
    state_ = BatteryCapacityTestState::COMPLETED;
  }
}

void BatteryCapacityTestPlant::abort() {
  if (state_ == BatteryCapacityTestState::RUNNING) {
    state_ = BatteryCapacityTestState::ABORTED;
    sample_.currentA = 0.0f;
  }
}

bool BatteryCapacityTestPlant::setCapacityRetentionPercent(
    float retentionPercent
) {
  if (state_ == BatteryCapacityTestState::RUNNING) {
    return false;
  }

  if (!isfinite(retentionPercent) ||
      retentionPercent < 0.0f ||
      retentionPercent > 100.0f) {
    return false;
  }

  config_.capacityRetentionPercent = retentionPercent;
  return true;
}

float BatteryCapacityTestPlant::capacityRetentionPercent() const {
  return config_.capacityRetentionPercent;
}

BatteryCapacityTestState BatteryCapacityTestPlant::state() const {
  return state_;
}

const BatteryCapacityTestSample&
BatteryCapacityTestPlant::sample() const {
  return sample_;
}


// ============================================================
// Battery capacity test monitor
// ============================================================

void BatteryCapacityTestMonitor::start() {
  result_.measuredCapacityAh = 0.0f;
  result_.elapsedSimulatedSeconds = 0.0f;

  state_ = BatteryCapacityTestState::RUNNING;
}

void BatteryCapacityTestMonitor::update(
    float voltageV,
    float currentA,
    float deltaTimeSeconds
) {
  if (state_ != BatteryCapacityTestState::RUNNING) {
    return;
  }

  if (!isfinite(voltageV) ||
      !isfinite(currentA) ||
      !isfinite(deltaTimeSeconds) ||
      deltaTimeSeconds <= 0.0f) {
    return;
  }

  result_.elapsedSimulatedSeconds += deltaTimeSeconds;

  if (currentA < 0.0f) {
    result_.measuredCapacityAh +=
        (-currentA) *
        deltaTimeSeconds /
        3600.0f;
  }

  if (voltageV <= CAPACITY_TEST_END_VOLTAGE_V) {
    state_ = BatteryCapacityTestState::COMPLETED;
  }
}

void BatteryCapacityTestMonitor::abort() {
  if (state_ == BatteryCapacityTestState::RUNNING) {
    state_ = BatteryCapacityTestState::ABORTED;
  }
}

BatteryCapacityTestState BatteryCapacityTestMonitor::state() const {
  return state_;
}

const BatteryCapacityTestResult&
BatteryCapacityTestMonitor::result() const {
  return result_;
}

