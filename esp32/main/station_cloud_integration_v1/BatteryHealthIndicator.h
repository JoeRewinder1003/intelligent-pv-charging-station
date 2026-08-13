#pragma once

#include <Arduino.h>

struct BatteryHealthSample {
  float voltageV = 0.0f;
  float currentA = 0.0f;
  float socPercent = 0.0f;
};

struct BatteryHealthEvent {
  bool valid = false;

  float voltageBeforeV = 0.0f;
  float voltageAfterV = 0.0f;

  float currentBeforeA = 0.0f;
  float currentAfterA = 0.0f;

  float socBeforePercent = 0.0f;
  float socAfterPercent = 0.0f;

  float deltaVoltageV = 0.0f;
  float deltaCurrentA = 0.0f;
};

struct BatteryHealthConfig {
  // Preliminary threshold for local emulation validation.
  // It can be adjusted after reviewing experimental behavior.
  float minimumCurrentStepA = 10.0f;
};

class BatteryHealthIndicator {
 public:
  explicit BatteryHealthIndicator(
      const BatteryHealthConfig& config = BatteryHealthConfig()
  );

  void reset();

  bool evaluateStep(
      const BatteryHealthSample& before,
      const BatteryHealthSample& after
  );

  const BatteryHealthEvent& lastEvent() const;

 private:
  BatteryHealthConfig config_;
  BatteryHealthEvent lastEvent_;

  bool sampleIsValid(const BatteryHealthSample& sample) const;
};
