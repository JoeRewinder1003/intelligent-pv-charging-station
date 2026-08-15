#pragma once

#include <Arduino.h>

enum class TrackingSafetyFault
{
  NONE,
  MASTER_POSITION_INVALID,
  SLAVE_POSITION_INVALID,
  MASTER_POSITION_OUT_OF_RANGE,
  SLAVE_POSITION_OUT_OF_RANGE,
  POSITION_MISMATCH,
  MASTER_ACTUATOR_STALL,
  SLAVE_ACTUATOR_STALL
};

struct TrackingSafetyConfig
{
  float minimumPositionMm = 0.0f;
  float maximumPositionMm = 300.0f;

  // Configurable synchronization limit.
  // The final physical safety threshold must be validated separately.
  float maximumPositionDifferenceMm = 10.0f;
};

struct TrackingSafetyState
{
  bool faultLatched = false;

  TrackingSafetyFault fault =
      TrackingSafetyFault::NONE;

  float positionDifferenceMm = 0.0f;
};

class TrackingSafetyMonitor
{
public:
  explicit TrackingSafetyMonitor(
      const TrackingSafetyConfig &config =
          TrackingSafetyConfig());

  void reset();

  bool resetIfSafe(
      float masterPositionMm,
      bool masterPositionValid,
      float slavePositionMm,
      bool slavePositionValid);

  void update(
      float masterPositionMm,
      bool masterPositionValid,
      float slavePositionMm,
      bool slavePositionValid);

  void reportActuatorStall(
      bool masterStallDetected,
      bool slaveStallDetected);

  const TrackingSafetyState &state() const;

  bool movementAllowed() const;

private:
  void latchFault(TrackingSafetyFault fault);

  TrackingSafetyConfig config_;
  TrackingSafetyState state_;
};