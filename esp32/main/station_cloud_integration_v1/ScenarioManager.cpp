#include "ScenarioManager.h"

#include <math.h>
#include <string.h>

ScenarioManager::ScenarioManager() = default;

void ScenarioManager::begin(ScenarioType initialScenario) {
  setScenario(initialScenario);
}

void ScenarioManager::update(float simulatedDeltaTimeSeconds) {
  if (!isfinite(simulatedDeltaTimeSeconds) ||
      simulatedDeltaTimeSeconds <= 0.0f) {
    return;
  }

  elapsedSimulatedSeconds_ += simulatedDeltaTimeSeconds;
}

void ScenarioManager::setScenario(ScenarioType scenario) {
  scenario_ = scenario;
  loadProfile(scenario_);
  resetElapsedTime();
}

bool ScenarioManager::setScenarioByName(const char* scenarioNameValue) {
  if (scenarioNameValue == nullptr || scenarioNameValue[0] == '\0') {
    return false;
  }

  if (strcmp(scenarioNameValue, "CLEAR_DAY") == 0) {
    setScenario(ScenarioType::CLEAR_DAY);
  } else if (strcmp(scenarioNameValue, "CLOUDY_DAY") == 0) {
    setScenario(ScenarioType::CLOUDY_DAY);
  } else if (strcmp(scenarioNameValue, "LOW_BATTERY") == 0) {
    setScenario(ScenarioType::LOW_BATTERY);
  } else if (strcmp(scenarioNameValue, "HIGH_DEMAND") == 0) {
    setScenario(ScenarioType::HIGH_DEMAND);
  } else if (strcmp(scenarioNameValue, "FAULT_EVENT") == 0) {
    setScenario(ScenarioType::FAULT_EVENT);
  } else if (strcmp(scenarioNameValue, "TRACKING_FAULT") == 0) {
    setScenario(ScenarioType::TRACKING_FAULT);
  } else if (strcmp(scenarioNameValue, "STALE_DATA") == 0) {
    setScenario(ScenarioType::STALE_DATA);
  } else {
    return false;
  }

  return true;
}

ScenarioType ScenarioManager::scenario() const {
  return scenario_;
}

const ScenarioProfile& ScenarioManager::profile() const {
  return profile_;
}

const char* ScenarioManager::scenarioName() const {
  switch (scenario_) {
    case ScenarioType::CLOUDY_DAY:
      return "CLOUDY_DAY";
    case ScenarioType::LOW_BATTERY:
      return "LOW_BATTERY";
    case ScenarioType::HIGH_DEMAND:
      return "HIGH_DEMAND";
    case ScenarioType::FAULT_EVENT:
      return "FAULT_EVENT";
    case ScenarioType::TRACKING_FAULT:
      return "TRACKING_FAULT";
    case ScenarioType::STALE_DATA:
      return "STALE_DATA";
    case ScenarioType::CLEAR_DAY:
    default:
      return "CLEAR_DAY";
  }
}

const char* ScenarioManager::nominalOperatingMode() const {
  return profile_.nominalOperatingMode;
}

const char* ScenarioManager::faultStateText() const {
  if (profile_.criticalFault) {
    return "critical_fault";
  }

  if (profile_.trackingFault) {
    return "tracking_fault";
  }

  if (profile_.staleData) {
    return "stale_data";
  }

  return "normal";
}

float ScenarioManager::elapsedSimulatedSeconds() const {
  return elapsedSimulatedSeconds_;
}

void ScenarioManager::resetElapsedTime() {
  elapsedSimulatedSeconds_ = 0.0f;
}

bool ScenarioManager::isCriticalFaultActive() const {
  return profile_.criticalFault;
}

bool ScenarioManager::isTrackingFaultActive() const {
  return profile_.trackingFault;
}

bool ScenarioManager::isStaleDataRequested() const {
  return profile_.staleData;
}

void ScenarioManager::loadProfile(ScenarioType scenario) {
  profile_ = ScenarioProfile();

  switch (scenario) {
    case ScenarioType::CLOUDY_DAY:
      profile_.initialSocPercent = 80.0f;
      profile_.irradianceWm2 = 350.0f;
      profile_.pvVoltageV = 17.20f;
      profile_.pvCurrentA = 7.56f;
      profile_.pvPowerW = 130.0f;
      profile_.ambientTemperatureC = 23.0f;
      profile_.relativeHumidityPercent = 70.0f;
      profile_.panelTemperatureC = 29.0f;
      profile_.weatherIndex = 0.35f;
      profile_.demandIndex = 0.50f;
      profile_.requestedOutputCount = 1;
      profile_.trackingAngleDeg = 25.0f;
      profile_.trackingTargetAngleDeg = 28.0f;
      profile_.masterPositionRaw = 1850;
      profile_.slavePositionRaw = 1842;
      profile_.nominalOperatingMode = "M2";
      break;

    case ScenarioType::LOW_BATTERY:
      profile_.initialSocPercent = 30.0f;
      profile_.irradianceWm2 = 250.0f;
      profile_.pvVoltageV = 17.00f;
      profile_.pvCurrentA = 5.59f;
      profile_.pvPowerW = 95.0f;
      profile_.ambientTemperatureC = 26.0f;
      profile_.relativeHumidityPercent = 52.0f;
      profile_.panelTemperatureC = 34.0f;
      profile_.weatherIndex = 0.45f;
      profile_.demandIndex = 0.55f;
      profile_.requestedOutputCount = 2;
      profile_.trackingAngleDeg = 27.0f;
      profile_.trackingTargetAngleDeg = 30.0f;
      profile_.masterPositionRaw = 1950;
      profile_.slavePositionRaw = 1944;
      profile_.nominalOperatingMode = "M2";
      break;

    case ScenarioType::HIGH_DEMAND:
      profile_.initialSocPercent = 85.0f;
      profile_.irradianceWm2 = 700.0f;
      profile_.pvVoltageV = 17.60f;
      profile_.pvCurrentA = 17.90f;
      profile_.pvPowerW = 315.0f;
      profile_.ambientTemperatureC = 30.0f;
      profile_.relativeHumidityPercent = 40.0f;
      profile_.panelTemperatureC = 48.0f;
      profile_.weatherIndex = 0.75f;
      profile_.demandIndex = 0.95f;
      profile_.requestedOutputCount = 3;
      profile_.trackingAngleDeg = 31.0f;
      profile_.trackingTargetAngleDeg = 32.0f;
      profile_.masterPositionRaw = 2200;
      profile_.slavePositionRaw = 2194;
      profile_.nominalOperatingMode = "M5";
      break;

    case ScenarioType::FAULT_EVENT:
      profile_.initialSocPercent = 80.0f;
      profile_.irradianceWm2 = 700.0f;
      profile_.pvVoltageV = 17.50f;
      profile_.pvCurrentA = 17.14f;
      profile_.pvPowerW = 300.0f;
      profile_.ambientTemperatureC = 29.0f;
      profile_.relativeHumidityPercent = 43.0f;
      profile_.panelTemperatureC = 46.0f;
      profile_.weatherIndex = 0.74f;
      profile_.demandIndex = 0.60f;
      profile_.requestedOutputCount = 0;
      profile_.nominalOperatingMode = "M0";
      profile_.criticalFault = true;
      break;

    case ScenarioType::TRACKING_FAULT:
      profile_.initialSocPercent = 80.0f;
      profile_.irradianceWm2 = 750.0f;
      profile_.pvVoltageV = 17.40f;
      profile_.pvCurrentA = 14.37f;
      profile_.pvPowerW = 250.0f;
      profile_.ambientTemperatureC = 29.0f;
      profile_.relativeHumidityPercent = 42.0f;
      profile_.panelTemperatureC = 44.0f;
      profile_.weatherIndex = 0.78f;
      profile_.demandIndex = 0.62f;
      profile_.requestedOutputCount = 1;
      profile_.trackingAngleDeg = 12.0f;
      profile_.trackingTargetAngleDeg = 30.0f;
      profile_.masterPositionRaw = 1700;
      profile_.slavePositionRaw = 1300;
      profile_.nominalOperatingMode = "M3";
      profile_.trackingFault = true;
      break;

    case ScenarioType::STALE_DATA:
      profile_.initialSocPercent = 80.0f;
      profile_.irradianceWm2 = 600.0f;
      profile_.pvVoltageV = 17.30f;
      profile_.pvCurrentA = 12.72f;
      profile_.pvPowerW = 220.0f;
      profile_.ambientTemperatureC = 27.0f;
      profile_.relativeHumidityPercent = 48.0f;
      profile_.panelTemperatureC = 40.0f;
      profile_.weatherIndex = 0.60f;
      profile_.demandIndex = 0.58f;
      profile_.requestedOutputCount = 0;
      profile_.nominalOperatingMode = "M0";
      profile_.staleData = true;
      break;

    case ScenarioType::CLEAR_DAY:
    default:
      profile_.initialSocPercent = 95.0f;
      profile_.irradianceWm2 = 850.0f;
      profile_.pvVoltageV = 17.49f;
      profile_.pvCurrentA = 21.87f;
      profile_.pvPowerW = 382.5f;
      profile_.ambientTemperatureC = 28.0f;
      profile_.relativeHumidityPercent = 45.0f;
      profile_.panelTemperatureC = 45.0f;
      profile_.weatherIndex = 0.85f;
      profile_.demandIndex = 0.65f;
      profile_.requestedOutputCount = 2;
      profile_.trackingAngleDeg = 28.5f;
      profile_.trackingTargetAngleDeg = 30.0f;
      profile_.masterPositionRaw = 2040;
      profile_.slavePositionRaw = 2025;
      profile_.nominalOperatingMode = "M4";
      break;
  }
}
