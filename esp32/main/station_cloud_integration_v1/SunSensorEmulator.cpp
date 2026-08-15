#include "SunSensorEmulator.h"

#include <math.h>

namespace {

// Manufacturer threshold for valid radiation.
constexpr float MIN_VALID_RADIATION_WM2 = 300.0f;

// ISS-T60 field of view is 120 degrees.
// For the simplified one-axis model, +/-60 degrees
// is used around the sensor normal.
constexpr float HALF_FIELD_OF_VIEW_DEG = 60.0f;

// ISS-T60 angular resolution.
constexpr float ANGLE_RESOLUTION_DEG = 0.01f;

}  // namespace

SunSensorEmulator::SunSensorEmulator() {
  state_ = SunSensorState();
}

void SunSensorEmulator::update(
    float sensorRadiationWm2,
    float sunReferenceAngleDeg,
    float panelAngleDeg
) {
  state_ = SunSensorState();

  if (!isfinite(sensorRadiationWm2) ||
      !isfinite(sunReferenceAngleDeg) ||
      !isfinite(panelAngleDeg) ||
      sensorRadiationWm2 < 0.0f) {
    return;
  }

  state_.radiationWm2 = sensorRadiationWm2;

  if (sensorRadiationWm2 < MIN_VALID_RADIATION_WM2) {
    // ISS-TX reports zero angles when radiation
    // is insufficient.
    state_.angleYDeg = 0.0f;
    state_.radiationEnough = false;
    state_.sunInFieldOfView = false;
    state_.measurementValid = false;
    return;
  }

  state_.radiationEnough = true;

  // Simulation sign convention only.
  // The physical sign must be confirmed according
  // to the final sensor mounting orientation.
  const float rawAngleYDeg =
      sunReferenceAngleDeg - panelAngleDeg;

  if (fabsf(rawAngleYDeg) > HALF_FIELD_OF_VIEW_DEG) {
    // ISS-TX reports zero angles when the Sun
    // is outside its field of view.
    state_.angleYDeg = 0.0f;
    state_.sunInFieldOfView = false;
    state_.measurementValid = false;
    return;
  }

  state_.angleYDeg =
      roundf(
          rawAngleYDeg / ANGLE_RESOLUTION_DEG
      ) * ANGLE_RESOLUTION_DEG;

  state_.sunInFieldOfView = true;
  state_.measurementValid = true;
}

const SunSensorState&
SunSensorEmulator::state() const {
  return state_;
}

