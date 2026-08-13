#include "BatteryHealthIndicator.h"

#include <math.h>

BatteryHealthIndicator::BatteryHealthIndicator(
    const BatteryHealthConfig& config
)
    : config_(config) {
  reset();
}

void BatteryHealthIndicator::reset() {
  lastEvent_ = BatteryHealthEvent();
}

bool BatteryHealthIndicator::sampleIsValid(
    const BatteryHealthSample& sample
) const {
  return isfinite(sample.voltageV) &&
         isfinite(sample.currentA) &&
         isfinite(sample.socPercent) &&
         sample.voltageV > 0.0f &&
         sample.socPercent >= 0.0f &&
         sample.socPercent <= 100.0f;
}

bool BatteryHealthIndicator::evaluateStep(
    const BatteryHealthSample& before,
    const BatteryHealthSample& after
) {
  reset();

  if (!sampleIsValid(before) || !sampleIsValid(after)) {
    return false;
  }

  const float deltaCurrentA =
      after.currentA - before.currentA;

  if (fabsf(deltaCurrentA) < config_.minimumCurrentStepA) {
    return false;
  }

  lastEvent_.valid = true;

  lastEvent_.voltageBeforeV = before.voltageV;
  lastEvent_.voltageAfterV = after.voltageV;

  lastEvent_.currentBeforeA = before.currentA;
  lastEvent_.currentAfterA = after.currentA;

  lastEvent_.socBeforePercent = before.socPercent;
  lastEvent_.socAfterPercent = after.socPercent;

  lastEvent_.deltaVoltageV =
      after.voltageV - before.voltageV;

  lastEvent_.deltaCurrentA = deltaCurrentA;

  return true;
}

const BatteryHealthEvent&
BatteryHealthIndicator::lastEvent() const {
  return lastEvent_;
}
