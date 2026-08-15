#pragma once

#include <Arduino.h>

#include "ActuatorEmulator.h"

struct TrackingControllerConfig {
  // Provisional design parameters for tracking hysteresis.
  float startThresholdDeg = 1.0f;
  float stopThresholdDeg = 0.5f;
};

struct TrackingControllerState {
  ActuatorCommand command = ActuatorCommand::STOP;
  bool correctionActive = false;
  float angleYDeg = 0.0f;
};

class TrackingController {
 public:
  explicit TrackingController(
      const TrackingControllerConfig& config =
          TrackingControllerConfig()
  );

  void reset();

  void update(
      float angleYDeg,
      bool measurementValid
  );

  const TrackingControllerState& state() const;

 private:
  TrackingControllerConfig config_;
  TrackingControllerState state_;
};

