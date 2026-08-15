#pragma once

#include <Arduino.h>

enum class ActuatorCommand
{
  STOP,
  EXTEND,
  RETRACT
};

struct ActuatorConfig
{
  float minimumPositionMm = 0.0f;
  float maximumPositionMm = 300.0f;

  // Representative actuator speed used in the functional simulation.
  float speedMmPerSecond = 5.08f;
};

struct ActuatorState
{
  float positionMm = 0.0f;

  ActuatorCommand command = ActuatorCommand::STOP;

  bool moving = false;
  bool atMinimumLimit = true;
  bool atMaximumLimit = false;
};

class ActuatorEmulator
{
public:
  explicit ActuatorEmulator(
      const ActuatorConfig &config = ActuatorConfig());

  void reset(float initialPositionMm);
  void setCommand(ActuatorCommand command);
  void setStalled(bool stalled);

  void update(float deltaTimeSeconds);

  const ActuatorState &state() const;
  bool stalled() const;

private:
  ActuatorConfig config_;
  ActuatorState state_;
  bool stalled_ = false;
};

