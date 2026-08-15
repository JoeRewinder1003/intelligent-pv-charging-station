#include "TrackingGeometry.h"

#include <math.h>

namespace {

constexpr float LINK_L_MM = 450.0f;
constexpr float LINK_S_MM = 1450.0f;
constexpr float LINK_B_MM = 1250.0f;

constexpr float ANGLE_OFFSET_DEG = 2.65349f;

constexpr float MIN_POSITION_MM = 0.0f;
constexpr float MAX_POSITION_MM = 300.0f;

}  // namespace

float TrackingGeometry::positionToAngleDeg(
    float positionMm
) const {
  if (!isfinite(positionMm)) {
    return NAN;
  }

  if (positionMm < MIN_POSITION_MM ||
      positionMm > MAX_POSITION_MM) {
    return NAN;
  }

  const float numerator =
      -(positionMm + LINK_L_MM) *
      (positionMm + LINK_L_MM) +
      LINK_S_MM * LINK_S_MM +
      LINK_B_MM * LINK_B_MM;

  const float denominator =
      2.0f * LINK_S_MM * LINK_B_MM;

  const float cosineArgument =
      numerator / denominator;

  if (cosineArgument < -1.0f ||
      cosineArgument > 1.0f) {
    return NAN;
  }

  const float angleRad =
      acosf(cosineArgument);

  const float angleDeg =
      angleRad * 180.0f / PI;

  return angleDeg + ANGLE_OFFSET_DEG;
}
