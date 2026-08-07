#include "PVSimulator.h"

#include <math.h>

namespace {

float clampFloat(float value, float minimum, float maximum) {
  if (value < minimum) {
    return minimum;
  }

  if (value > maximum) {
    return maximum;
  }

  return value;
}

}  // namespace

PVSimulator::PVSimulator(const PVConfig& config)
    : config_(config) {
  sanitizeConfiguration();
}

void PVSimulator::sanitizeConfiguration() {
  if (config_.panelCount == 0) {
    config_.panelCount = 1;
  }

  // The current telemetry schema explicitly exposes three panel currents.
  if (config_.panelCount > 3) {
    config_.panelCount = 3;
  }

  if (config_.ratedPowerPerPanelW <= 0.0f) {
    config_.ratedPowerPerPanelW = 150.0f;
  }

  if (config_.vmpAtStcV <= 0.0f) {
    config_.vmpAtStcV = 17.49f;
  }

  if (config_.impAtStcA <= 0.0f) {
    config_.impAtStcA = 8.58f;
  }

  if (config_.referenceIrradianceWm2 <= 0.0f) {
    config_.referenceIrradianceWm2 = 1000.0f;
  }

  config_.deliveryEfficiency =
      clampFloat(config_.deliveryEfficiency, 0.0f, 1.0f);

  if (config_.minimumOperatingIrradianceWm2 < 0.0f) {
    config_.minimumOperatingIrradianceWm2 = 0.0f;
  }
}

void PVSimulator::begin() {
  state_ = PVState();
}

void PVSimulator::update(
    float irradianceWm2,
    float panelTemperatureC,
    uint8_t availablePanelCount,
    float availabilityFactor
) {
  if (!isfinite(irradianceWm2) ||
      !isfinite(panelTemperatureC) ||
      !isfinite(availabilityFactor)) {
    return;
  }

  state_.inputLimited = false;

  state_.irradianceWm2 = irradianceWm2;
  if (state_.irradianceWm2 < 0.0f) {
    state_.irradianceWm2 = 0.0f;
    state_.inputLimited = true;
  } else if (state_.irradianceWm2 > 1200.0f) {
    state_.irradianceWm2 = 1200.0f;
    state_.inputLimited = true;
  }

  state_.panelTemperatureC = panelTemperatureC;
  if (state_.panelTemperatureC < -40.0f) {
    state_.panelTemperatureC = -40.0f;
    state_.inputLimited = true;
  } else if (state_.panelTemperatureC > 85.0f) {
    state_.panelTemperatureC = 85.0f;
    state_.inputLimited = true;
  }

  state_.availablePanelCount = availablePanelCount;
  if (state_.availablePanelCount > config_.panelCount) {
    state_.availablePanelCount = config_.panelCount;
    state_.inputLimited = true;
  }

  state_.availabilityFactor =
      clampFloat(availabilityFactor, 0.0f, 1.0f);
  if (state_.availabilityFactor != availabilityFactor) {
    state_.inputLimited = true;
  }

  for (size_t index = 0; index < 3; index++) {
    state_.panelCurrentA[index] = 0.0f;
  }

  state_.temperaturePowerFactor = 1.0f;
  state_.voltageV = 0.0f;
  state_.totalCurrentA = 0.0f;
  state_.rawPowerW = 0.0f;
  state_.deliveredPowerW = 0.0f;
  state_.generationAvailable = false;

  if (state_.availablePanelCount == 0 ||
      state_.availabilityFactor <= 0.0f ||
      state_.irradianceWm2 < config_.minimumOperatingIrradianceWm2) {
    return;
  }

  const float temperatureDifferenceC =
      state_.panelTemperatureC - config_.referenceCellTemperatureC;

  state_.temperaturePowerFactor = clampFloat(
      1.0f +
          config_.powerTemperatureCoefficientPerC * temperatureDifferenceC,
      0.0f,
      1.25f
  );

  const float irradianceFactor =
      state_.irradianceWm2 / config_.referenceIrradianceWm2;

  state_.rawPowerW =
      config_.ratedPowerPerPanelW *
      static_cast<float>(state_.availablePanelCount) *
      irradianceFactor *
      state_.temperaturePowerFactor *
      state_.availabilityFactor;

  const float voltageTemperatureFactor = clampFloat(
      1.0f +
          config_.voltageTemperatureCoefficientPerC *
          temperatureDifferenceC,
      0.50f,
      1.20f
  );

  state_.voltageV = config_.vmpAtStcV * voltageTemperatureFactor;

  if (state_.voltageV <= 0.0f) {
    state_.inputLimited = true;
    state_.rawPowerW = 0.0f;
    return;
  }

  state_.totalCurrentA = state_.rawPowerW / state_.voltageV;

  const float currentPerAvailablePanelA =
      state_.totalCurrentA /
      static_cast<float>(state_.availablePanelCount);

  for (size_t index = 0; index < 3; index++) {
    if (index < state_.availablePanelCount) {
      state_.panelCurrentA[index] = currentPerAvailablePanelA;
    }
  }

  state_.deliveredPowerW =
      state_.rawPowerW * config_.deliveryEfficiency;
  state_.generationAvailable = state_.deliveredPowerW > 0.0f;
}

const PVConfig& PVSimulator::config() const {
  return config_;
}

const PVState& PVSimulator::state() const {
  return state_;
}

float PVSimulator::ratedArrayPowerW() const {
  return config_.ratedPowerPerPanelW * config_.panelCount;
}
