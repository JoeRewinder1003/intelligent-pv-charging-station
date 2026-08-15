#pragma once

#include <Arduino.h>

class TrackingGeometry {
 public:
  float positionToAngleDeg(float positionMm) const;
};

