#pragma once

#include <Arduino.h>

struct SunSensorState {
  float angleYDeg = 0.0f;
  float radiationWm2 = 0.0f;

  bool radiationEnough = false;
  bool sunInFieldOfView = false;
  bool measurementValid = false;
};

class SunSensorEmulator {
 public:
  SunSensorEmulator();

  void update(
      float sensorRadiationWm2,
      float sunReferenceAngleDeg,
      float panelAngleDeg
  );

  const SunSensorState& state() const;

 private:
  SunSensorState state_;
};

