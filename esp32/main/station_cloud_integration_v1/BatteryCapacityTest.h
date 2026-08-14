#pragma once

#include <Arduino.h>

enum class BatteryCapacityTestState {
  IDLE,
  RUNNING,
  COMPLETED,
  ABORTED
};

struct BatteryCapacityTestSample {
  float voltageV = 0.0f;
  float currentA = 0.0f;
};

struct BatteryCapacityTestResult {
  float measuredCapacityAh = 0.0f;
  float elapsedSimulatedSeconds = 0.0f;
};

struct BatteryCapacityTestPlantConfig {
  float nominalCapacityAh = 300.0f;
  float capacityRetentionPercent = 100.0f;

  float dischargeCurrentA = -30.0f;

  float startVoltageV = 12.8f;
  float endVoltageV = 10.8f;
};

class BatteryCapacityTestPlant {
 public:
  explicit BatteryCapacityTestPlant(
      const BatteryCapacityTestPlantConfig& config =
          BatteryCapacityTestPlantConfig()
  );

  void start();
  void update(float deltaTimeSeconds);
  void abort();

  bool setCapacityRetentionPercent(float retentionPercent);
  float capacityRetentionPercent() const;

  BatteryCapacityTestState state() const;
  const BatteryCapacityTestSample& sample() const;

 private:
  BatteryCapacityTestPlantConfig config_;
  BatteryCapacityTestState state_ = BatteryCapacityTestState::IDLE;
  BatteryCapacityTestSample sample_;

  float dischargedCapacityAh_ = 0.0f;
};

class BatteryCapacityTestMonitor {
 public:
  void start();
  void update(
      float voltageV,
      float currentA,
      float deltaTimeSeconds
  );
  void abort();

  BatteryCapacityTestState state() const;
  const BatteryCapacityTestResult& result() const;

 private:
  BatteryCapacityTestState state_ = BatteryCapacityTestState::IDLE;
  BatteryCapacityTestResult result_;
};

