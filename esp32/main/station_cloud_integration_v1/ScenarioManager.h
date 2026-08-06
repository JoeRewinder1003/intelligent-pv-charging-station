#pragma once

#include <Arduino.h>

enum class ScenarioType : uint8_t {
  CLEAR_DAY = 0,
  CLOUDY_DAY,
  LOW_BATTERY,
  HIGH_DEMAND,
  FAULT_EVENT,
  TRACKING_FAULT,
  STALE_DATA,
};

struct ScenarioProfile {
  float initialSocPercent = 95.0f;

  float irradianceWm2 = 850.0f;
  float pvVoltageV = 17.49f;
  float pvCurrentA = 21.87f;
  float pvPowerW = 382.5f;

  float ambientTemperatureC = 28.0f;
  float relativeHumidityPercent = 45.0f;
  float panelTemperatureC = 45.0f;

  float weatherIndex = 0.85f;
  float demandIndex = 0.65f;
  uint8_t requestedOutputCount = 2;

  float trackingAngleDeg = 28.5f;
  float trackingTargetAngleDeg = 30.0f;
  uint16_t masterPositionRaw = 2040;
  uint16_t slavePositionRaw = 2025;

  const char* nominalOperatingMode = "M4";

  bool criticalFault = false;
  bool trackingFault = false;
  bool staleData = false;
};

class ScenarioManager {
 public:
  ScenarioManager();

  void begin(ScenarioType initialScenario = ScenarioType::CLEAR_DAY);
  void update(float simulatedDeltaTimeSeconds);

  void setScenario(ScenarioType scenario);
  bool setScenarioByName(const char* scenarioName);

  ScenarioType scenario() const;
  const ScenarioProfile& profile() const;
  const char* scenarioName() const;
  const char* nominalOperatingMode() const;
  const char* faultStateText() const;

  float elapsedSimulatedSeconds() const;
  void resetElapsedTime();

  bool isCriticalFaultActive() const;
  bool isTrackingFaultActive() const;
  bool isStaleDataRequested() const;

 private:
  ScenarioType scenario_ = ScenarioType::CLEAR_DAY;
  ScenarioProfile profile_;
  float elapsedSimulatedSeconds_ = 0.0f;

  void loadProfile(ScenarioType scenario);
};
