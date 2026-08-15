#include "ActuatorEmulator.h"

namespace
{

  float clampFloat(
      float value,
      float minimumValue,
      float maximumValue)
  {
    if (value < minimumValue)
    {
      return minimumValue;
    }

    if (value > maximumValue)
    {
      return maximumValue;
    }

    return value;
  }

} // namespace

ActuatorEmulator::ActuatorEmulator(
    const ActuatorConfig &config)
    : config_(config)
{
  if (config_.minimumPositionMm > config_.maximumPositionMm)
  {
    const float temporaryValue = config_.minimumPositionMm;
    config_.minimumPositionMm = config_.maximumPositionMm;
    config_.maximumPositionMm = temporaryValue;
  }

  if (!isfinite(config_.speedMmPerSecond) ||
      config_.speedMmPerSecond <= 0.0f)
  {
    config_.speedMmPerSecond = 5.08f;
  }

  reset(config_.minimumPositionMm);
}

void ActuatorEmulator::reset(float initialPositionMm)
{
  state_.positionMm = clampFloat(
      initialPositionMm,
      config_.minimumPositionMm,
      config_.maximumPositionMm);

  state_.command = ActuatorCommand::STOP;
  state_.moving = false;

  state_.atMinimumLimit =
      state_.positionMm <= config_.minimumPositionMm;

  state_.atMaximumLimit =
      state_.positionMm >= config_.maximumPositionMm;
}

void ActuatorEmulator::setCommand(
    ActuatorCommand command)
{
  state_.command = command;
}

void ActuatorEmulator::setStalled(bool stalled) {
  stalled_ = stalled;

  if (stalled_) {
    state_.moving = false;
  }
}

void ActuatorEmulator::update(float deltaTimeSeconds)
{
  if (!isfinite(deltaTimeSeconds) ||
      deltaTimeSeconds <= 0.0f)
  {
    return;
  }

  if (stalled_) {
    state_.moving = false;
    return;
  }

  state_.moving = false;

  if (state_.command == ActuatorCommand::EXTEND)
  {
    if (state_.positionMm < config_.maximumPositionMm)
    {
      state_.positionMm +=
          config_.speedMmPerSecond * deltaTimeSeconds;

      state_.positionMm = clampFloat(
          state_.positionMm,
          config_.minimumPositionMm,
          config_.maximumPositionMm);

      state_.moving = true;
    }
  }
  else if (
      state_.command == ActuatorCommand::RETRACT)
  {
    if (state_.positionMm > config_.minimumPositionMm)
    {
      state_.positionMm -=
          config_.speedMmPerSecond * deltaTimeSeconds;

      state_.positionMm = clampFloat(
          state_.positionMm,
          config_.minimumPositionMm,
          config_.maximumPositionMm);

      state_.moving = true;
    }
  }

  state_.atMinimumLimit =
      state_.positionMm <= config_.minimumPositionMm;

  state_.atMaximumLimit =
      state_.positionMm >= config_.maximumPositionMm;

  if (state_.atMinimumLimit &&
      state_.command == ActuatorCommand::RETRACT)
  {
    state_.moving = false;
  }

  if (state_.atMaximumLimit &&
      state_.command == ActuatorCommand::EXTEND)
  {
    state_.moving = false;
  }
}

const ActuatorState &ActuatorEmulator::state() const
{
  return state_;
}

bool ActuatorEmulator::stalled() const {
  return stalled_;
}
