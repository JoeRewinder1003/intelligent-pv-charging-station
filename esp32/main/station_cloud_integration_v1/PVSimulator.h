#pragma once

#include <Arduino.h>

struct PVConfig {
  // Confirmed photovoltaic-array configuration.
  uint8_t panelCount = 3;
  float ratedPowerPerPanelW = 150.0f;
  float vmpAtStcV = 17.49f;
  float impAtStcA = 8.58f;

  // Reference conditions and configurable functional-simulation assumptions.
  float referenceIrradianceWm2 = 1000.0f;
  float referenceCellTemperatureC = 25.0f;
  float powerTemperatureCoefficientPerC = -0.0040f;
  float voltageTemperatureCoefficientPerC = -0.0031f;
  float deliveryEfficiency = 0.90f;
  float minimumOperatingIrradianceWm2 = 5.0f;
};

struct PVState {
  float irradianceWm2 = 0.0f;
  float panelTemperatureC = 25.0f;
  uint8_t availablePanelCount = 0;
  float availabilityFactor = 0.0f;

  float temperaturePowerFactor = 1.0f;
  float voltageV = 0.0f;
  float panelCurrentA[3] = {0.0f, 0.0f, 0.0f};
  float totalCurrentA = 0.0f;
  float rawPowerW = 0.0f;
  float deliveredPowerW = 0.0f;

  bool generationAvailable = false;
  bool inputLimited = false;
};

class PVSimulator {
 public:
  explicit PVSimulator(const PVConfig& config = PVConfig());

  void begin();
  void update(
      float irradianceWm2,
      float panelTemperatureC,
      uint8_t availablePanelCount,
      float availabilityFactor
  );

  const PVConfig& config() const;
  const PVState& state() const;
  float ratedArrayPowerW() const;

 private:
  PVConfig config_;
  PVState state_;

  void sanitizeConfiguration();
};
