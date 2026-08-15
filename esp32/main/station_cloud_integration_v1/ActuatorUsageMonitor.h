#pragma once

#include <Arduino.h>

#include "ActuatorEmulator.h"

struct ActuatorUsageState {
  float operatingTimeSeconds = 0.0f;
  float totalTravelMm = 0.0f;

  uint32_t movementStarts = 0;

  float equivalentFullStrokeCycles = 0.0f;

  bool currentlyMoving = false;

  float lastDutyCyclePercent = 0.0f;
  bool dutyCycleWindowAvailable = false;
  bool dutyCycleExceeded = false;

  uint32_t dutyCycleWindowSequence = 0;
};

class ActuatorUsageMonitor {
 public:
  ActuatorUsageMonitor();

  void reset();
  void reset(float initialPositionMm);

  void update(
      float deltaTimeSeconds,
      ActuatorCommand appliedCommand,
      float positionMm
  );

  const ActuatorUsageState& state() const;

 private:
  ActuatorUsageState state_;

  float previousPositionMm_ = 0.0f;
  bool previousPositionAvailable_ = false;

  ActuatorCommand previousCommand_ =
      ActuatorCommand::STOP;

  float dutyCycleWindowElapsedSeconds_ = 0.0f;
  float dutyCycleWindowOperatingSeconds_ = 0.0f;
};
