#include "ActuatorUsageMonitor.h"

#include <math.h>

namespace {

constexpr float STROKE_LENGTH_MM = 300.0f;

// Design parameter for monitoring only.
// The actuator manufacturer specifies a 25% duty cycle
// but does not define this 60-second observation window.
constexpr float DUTY_CYCLE_WINDOW_SECONDS = 60.0f;

constexpr float MAX_DUTY_CYCLE_PERCENT = 25.0f;

}  // namespace

ActuatorUsageMonitor::ActuatorUsageMonitor() {
  reset();
}

void ActuatorUsageMonitor::reset() {
  state_ = ActuatorUsageState();

  previousPositionMm_ = 0.0f;
  previousPositionAvailable_ = false;

  previousCommand_ = ActuatorCommand::STOP;

  dutyCycleWindowElapsedSeconds_ = 0.0f;
  dutyCycleWindowOperatingSeconds_ = 0.0f;
}

void ActuatorUsageMonitor::reset(
    float initialPositionMm
) {
  reset();

  if (isfinite(initialPositionMm)) {
    previousPositionMm_ = initialPositionMm;
    previousPositionAvailable_ = true;
  }
}

void ActuatorUsageMonitor::update(
    float deltaTimeSeconds,
    ActuatorCommand appliedCommand,
    float positionMm
) {
  if (!isfinite(deltaTimeSeconds) ||
      deltaTimeSeconds <= 0.0f ||
      !isfinite(positionMm)) {
    return;
  }

  const bool movingCommand =
      appliedCommand != ActuatorCommand::STOP;

  state_.currentlyMoving = movingCommand;

  if (movingCommand) {
    state_.operatingTimeSeconds +=
        deltaTimeSeconds;
  }

  if (movingCommand &&
      previousCommand_ == ActuatorCommand::STOP) {
    state_.movementStarts++;
  }

  if (previousPositionAvailable_) {
    const float travelMm =
        fabsf(positionMm - previousPositionMm_);

    state_.totalTravelMm += travelMm;
  }

  state_.equivalentFullStrokeCycles =
      state_.totalTravelMm /
      (2.0f * STROKE_LENGTH_MM);

  dutyCycleWindowElapsedSeconds_ +=
      deltaTimeSeconds;

  if (movingCommand) {
    dutyCycleWindowOperatingSeconds_ +=
        deltaTimeSeconds;
  }

  if (dutyCycleWindowElapsedSeconds_ >=
      DUTY_CYCLE_WINDOW_SECONDS) {

    state_.lastDutyCyclePercent =
        100.0f *
        dutyCycleWindowOperatingSeconds_ /
        dutyCycleWindowElapsedSeconds_;

    state_.dutyCycleWindowAvailable = true;

    state_.dutyCycleExceeded =
        state_.lastDutyCyclePercent >
        MAX_DUTY_CYCLE_PERCENT;

    dutyCycleWindowElapsedSeconds_ = 0.0f;
    dutyCycleWindowOperatingSeconds_ = 0.0f;
  }

  previousPositionMm_ = positionMm;
  previousPositionAvailable_ = true;

  previousCommand_ = appliedCommand;
}

const ActuatorUsageState&
ActuatorUsageMonitor::state() const {
  return state_;
}
