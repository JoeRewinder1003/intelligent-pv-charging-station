#pragma once

#include <Arduino.h>

#include "ActuatorEmulator.h"

struct ActuatorSynchronizationConfig {
  // Provisional design thresholds for functional validation.
  float correctionStartDifferenceMm = 10.0f;
  float correctionStopDifferenceMm = 5.0f;
};

struct ActuatorSynchronizationState {
  bool correctionActive = false;
  float positionDifferenceMm = 0.0f;

  ActuatorCommand masterCommand = ActuatorCommand::STOP;
  ActuatorCommand slaveCommand = ActuatorCommand::STOP;
};

class ActuatorSynchronizer {
 public:
  explicit ActuatorSynchronizer(
      const ActuatorSynchronizationConfig& config =
          ActuatorSynchronizationConfig()
  );

  void reset();

  void update(
      ActuatorCommand requestedCommand,
      float masterPositionMm,
      float slavePositionMm
  );

  const ActuatorSynchronizationState& state() const;

 private:
  ActuatorSynchronizationConfig config_;
  ActuatorSynchronizationState state_;
};

