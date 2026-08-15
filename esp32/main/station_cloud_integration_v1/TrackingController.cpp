#include "TrackingController.h"

#include <math.h>

TrackingController::TrackingController(
    const TrackingControllerConfig& config
) : config_(config) {
  if (!isfinite(config_.startThresholdDeg) ||
      config_.startThresholdDeg <= 0.0f) {
    config_.startThresholdDeg = 1.0f;
  }

  if (!isfinite(config_.stopThresholdDeg) ||
      config_.stopThresholdDeg < 0.0f ||
      config_.stopThresholdDeg >
          config_.startThresholdDeg) {
    config_.stopThresholdDeg = 0.5f;
  }

  reset();
}

void TrackingController::reset() {
  state_ = TrackingControllerState();
}

void TrackingController::update(
    float angleYDeg,
    bool measurementValid
) {
  state_.angleYDeg = angleYDeg;

  if (!measurementValid ||
      !isfinite(angleYDeg)) {
    state_.command = ActuatorCommand::STOP;
    state_.correctionActive = false;
    return;
  }

  const float absoluteErrorDeg =
      fabsf(angleYDeg);

  // Start a new correction only outside the start threshold.
  if (state_.command == ActuatorCommand::STOP) {
    if (angleYDeg >= config_.startThresholdDeg) {
      state_.command = ActuatorCommand::EXTEND;
      state_.correctionActive = true;
    } else if (
        angleYDeg <= -config_.startThresholdDeg
    ) {
      state_.command = ActuatorCommand::RETRACT;
      state_.correctionActive = true;
    }

    return;
  }

  // Stop once the alignment error enters the inner threshold.
  if (absoluteErrorDeg <=
      config_.stopThresholdDeg) {
    state_.command = ActuatorCommand::STOP;
    state_.correctionActive = false;
    return;
  }

  // Reverse direction if the error crosses the opposite
  // start threshold.
  if (state_.command == ActuatorCommand::EXTEND &&
      angleYDeg <= -config_.startThresholdDeg) {
    state_.command = ActuatorCommand::RETRACT;
    state_.correctionActive = true;
    return;
  }

  if (state_.command == ActuatorCommand::RETRACT &&
      angleYDeg >= config_.startThresholdDeg) {
    state_.command = ActuatorCommand::EXTEND;
    state_.correctionActive = true;
    return;
  }

  state_.correctionActive = true;
}

const TrackingControllerState&
TrackingController::state() const {
  return state_;
}

