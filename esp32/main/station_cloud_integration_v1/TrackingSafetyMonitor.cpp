#include "TrackingSafetyMonitor.h"

TrackingSafetyMonitor::TrackingSafetyMonitor(
    const TrackingSafetyConfig &config)
    : config_(config)
{
  if (config_.minimumPositionMm > config_.maximumPositionMm)
  {
    const float temporaryValue = config_.minimumPositionMm;
    config_.minimumPositionMm = config_.maximumPositionMm;
    config_.maximumPositionMm = temporaryValue;
  }

  if (!isfinite(config_.maximumPositionDifferenceMm) ||
      config_.maximumPositionDifferenceMm <= 0.0f)
  {
    config_.maximumPositionDifferenceMm = 10.0f;
  }

  reset();
}

void TrackingSafetyMonitor::reset()
{
  state_.faultLatched = false;
  state_.fault = TrackingSafetyFault::NONE;
  state_.positionDifferenceMm = 0.0f;
}

bool TrackingSafetyMonitor::resetIfSafe(
  float masterPositionMm,
  bool masterPositionValid,
  float slavePositionMm,
  bool slavePositionValid
) {
  if (!masterPositionValid ||
      !isfinite(masterPositionMm)) {
    return false;
  }

  if (!slavePositionValid ||
      !isfinite(slavePositionMm)) {
    return false;
  }

  if (masterPositionMm < config_.minimumPositionMm ||
      masterPositionMm > config_.maximumPositionMm) {
    return false;
  }

  if (slavePositionMm < config_.minimumPositionMm ||
      slavePositionMm > config_.maximumPositionMm) {
    return false;
  }

  reset();
  return true;
}

void TrackingSafetyMonitor::update(
    float masterPositionMm,
    bool masterPositionValid,
    float slavePositionMm,
    bool slavePositionValid)
{
  if (state_.faultLatched)
  {
    return;
  }

  if (!masterPositionValid ||
      !isfinite(masterPositionMm))
  {
    latchFault(
        TrackingSafetyFault::MASTER_POSITION_INVALID);
    return;
  }

  if (!slavePositionValid ||
      !isfinite(slavePositionMm))
  {
    latchFault(
        TrackingSafetyFault::SLAVE_POSITION_INVALID);
    return;
  }

  if (masterPositionMm < config_.minimumPositionMm ||
      masterPositionMm > config_.maximumPositionMm)
  {
    latchFault(
        TrackingSafetyFault::MASTER_POSITION_OUT_OF_RANGE);
    return;
  }

  if (slavePositionMm < config_.minimumPositionMm ||
      slavePositionMm > config_.maximumPositionMm)
  {
    latchFault(
        TrackingSafetyFault::SLAVE_POSITION_OUT_OF_RANGE);
    return;
  }

  state_.positionDifferenceMm =
      fabsf(masterPositionMm - slavePositionMm);
}

void TrackingSafetyMonitor::reportActuatorStall(
  bool masterStallDetected,
  bool slaveStallDetected
) {
  if (state_.faultLatched) {
    return;
  }

  if (masterStallDetected) {
    latchFault(
      TrackingSafetyFault::MASTER_ACTUATOR_STALL
    );
    return;
  }

  if (slaveStallDetected) {
    latchFault(
      TrackingSafetyFault::SLAVE_ACTUATOR_STALL
    );
  }
}

const TrackingSafetyState &
TrackingSafetyMonitor::state() const
{
  return state_;
}

bool TrackingSafetyMonitor::movementAllowed() const
{
  return !state_.faultLatched;
}

void TrackingSafetyMonitor::latchFault(
    TrackingSafetyFault fault)
{
  state_.faultLatched = true;
  state_.fault = fault;
}