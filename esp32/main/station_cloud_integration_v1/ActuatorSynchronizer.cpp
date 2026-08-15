#include "ActuatorSynchronizer.h"

ActuatorSynchronizer::ActuatorSynchronizer(
    const ActuatorSynchronizationConfig& config
)
    : config_(config) {
  if (!isfinite(config_.correctionStartDifferenceMm) ||
      config_.correctionStartDifferenceMm <= 0.0f) {
    config_.correctionStartDifferenceMm = 10.0f;
  }

  if (!isfinite(config_.correctionStopDifferenceMm) ||
      config_.correctionStopDifferenceMm < 0.0f ||
      config_.correctionStopDifferenceMm >=
          config_.correctionStartDifferenceMm) {
    config_.correctionStopDifferenceMm = 5.0f;
  }

  reset();
}

void ActuatorSynchronizer::reset() {
  state_.correctionActive = false;
  state_.positionDifferenceMm = 0.0f;
  state_.masterCommand = ActuatorCommand::STOP;
  state_.slaveCommand = ActuatorCommand::STOP;
}

void ActuatorSynchronizer::update(
    ActuatorCommand requestedCommand,
    float masterPositionMm,
    float slavePositionMm
) {
  if (!isfinite(masterPositionMm) ||
      !isfinite(slavePositionMm)) {
    state_.correctionActive = false;
    state_.positionDifferenceMm = 0.0f;
    state_.masterCommand = ActuatorCommand::STOP;
    state_.slaveCommand = ActuatorCommand::STOP;
    return;
  }

  state_.positionDifferenceMm =
      fabsf(masterPositionMm - slavePositionMm);

  if (requestedCommand == ActuatorCommand::STOP) {
    state_.correctionActive = false;
    state_.masterCommand = ActuatorCommand::STOP;
    state_.slaveCommand = ActuatorCommand::STOP;
    return;
  }

  if (state_.correctionActive) {
    if (state_.positionDifferenceMm <=
        config_.correctionStopDifferenceMm) {
      state_.correctionActive = false;
    }
  } else {
    if (state_.positionDifferenceMm >
        config_.correctionStartDifferenceMm) {
      state_.correctionActive = true;
    }
  }

  if (!state_.correctionActive) {
    state_.masterCommand = requestedCommand;
    state_.slaveCommand = requestedCommand;
    return;
  }

  if (requestedCommand == ActuatorCommand::EXTEND) {
    if (masterPositionMm > slavePositionMm) {
      // Master is ahead in the extension direction.
      state_.masterCommand = ActuatorCommand::STOP;
      state_.slaveCommand = ActuatorCommand::EXTEND;
    } else {
      // Slave is ahead in the extension direction.
      state_.masterCommand = ActuatorCommand::EXTEND;
      state_.slaveCommand = ActuatorCommand::STOP;
    }

    return;
  }

  if (requestedCommand == ActuatorCommand::RETRACT) {
    if (masterPositionMm < slavePositionMm) {
      // Master is ahead in the retraction direction.
      state_.masterCommand = ActuatorCommand::STOP;
      state_.slaveCommand = ActuatorCommand::RETRACT;
    } else {
      // Slave is ahead in the retraction direction.
      state_.masterCommand = ActuatorCommand::RETRACT;
      state_.slaveCommand = ActuatorCommand::STOP;
    }
  }
}

const ActuatorSynchronizationState&
ActuatorSynchronizer::state() const {
  return state_;
}

