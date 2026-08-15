#include "ActuatorUsageMonitor.h"

#include <math.h>

namespace {

constexpr float STROKE_LENGTH_MM = 300.0f;

// Design parameter for monitoring only.
// The actuator manufacturer specifies a 25% duty cycle,
// while this 60-second observation window is defined
// by the station monitoring implementation.
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
      deltaTimeSeconds <= 0.0f) {
    return;
  }

  const bool movingCommand =
      appliedCommand != ActuatorCommand::STOP;

  state_.currentlyMoving = movingCommand;

  // Operating time depends on the applied command,
  // not on position-feedback validity.
  if (movingCommand) {
    state_.operatingTimeSeconds +=
        deltaTimeSeconds;
  }

  if (movingCommand &&
      previousCommand_ == ActuatorCommand::STOP) {
    state_.movementStarts++;
  }

  // Travel distance requires valid position feedback.
  if (isfinite(positionMm)) {
    if (previousPositionAvailable_) {
      const float travelMm =
          fabsf(positionMm - previousPositionMm_);

      state_.totalTravelMm += travelMm;
    }

    previousPositionMm_ = positionMm;
    previousPositionAvailable_ = true;
  } else {
    // Do not calculate a future distance jump across
    // an interval with invalid position feedback.
    previousPositionAvailable_ = false;
  }

  state_.equivalentFullStrokeCycles =
      state_.totalTravelMm /
      (2.0f * STROKE_LENGTH_MM);

  // Duty-cycle timing remains valid even if position
  // feedback is temporarily unavailable.
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

    state_.dutyCycleWindowSequence++;

    dutyCycleWindowElapsedSeconds_ = 0.0f;
    dutyCycleWindowOperatingSeconds_ = 0.0f;
  }

  previousCommand_ = appliedCommand;
}

const ActuatorUsageState&
ActuatorUsageMonitor::state() const {
  return state_;
}
