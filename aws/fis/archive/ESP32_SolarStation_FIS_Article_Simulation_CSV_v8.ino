/*
  ESP32 Solar Charging Station - Article Simulation Logger
  Revised version: high-demand profile, three-output utilization, anti-chattering, and restriction/fault-state logging
  -------------------------------------------------------
  Purpose:
  - Run a functional daily simulation on ESP32.
  - Evaluate Weather FIS and Main Decision FIS.
  - Print CSV logs for plotting article results.

  Weather FIS inputs:
  - shortwave_radiation
  - cloud_cover
  - precipitation_probability

  Weather FIS output:
  - weather_index

  Main FIS inputs:
  - battery_soc
  - net_battery_power
  - local_irradiance
  - weather_index
  - demand_index

  Main FIS output:
  - operating_mode M0-M5

  Serial commands:
  1 -> clear_day
  2 -> cloudy_day
  3 -> low_battery
  4 -> fault_event
  r -> reset current scenario
  h -> print CSV header
*/

#include <Arduino.h>
#include <math.h>

// =========================================================
// User configuration
// =========================================================

#define CSV_OUTPUT true

const uint32_t SERIAL_BAUD = 115200;

// Simulation step.
// 5 min gives 288 samples per simulated day.
const int SIM_STEP_MIN = 5;
const int MINUTES_PER_DAY = 1440;

// Real delay between simulated steps.
// Reduce for faster logging, increase if Serial Monitor misses lines.
const uint32_t SIM_STEP_DELAY_MS = 150;

// Energy model
const float PV_NOMINAL_POWER_W = 450.0f;     // 3 x 150 W panels
const float PV_EFFICIENCY = 0.85f;           // simplified system efficiency
const float ELECTRONICS_POWER_W = 5.0f;      // measured idle electronics baseline
const float SCOOTER_POWER_W = 71.0f;         // useful scooter charging power
const float BOOST_EFFICIENCY = 0.88f;        // assumed DC/DC boost efficiency

// Battery model.
// Adjust this according to the battery bank used in the article.
// Example: 12 V * 200 Ah = 2400 Wh, or 12 V * 300 Ah = 3600 Wh.
const float BATTERY_ENERGY_WH = 2400.0f;

// Main FIS output mode
enum OperatingMode {
  M0_BASIC_ONLY = 0,
  M1_TELEMETRY = 1,
  M2_TRACKING_ALLOWED = 2,
  M3_ONE_OUTPUT = 3,
  M4_TWO_OUTPUTS = 4,
  M5_THREE_OUTPUTS = 5
};

// Scenario selection
enum ScenarioType {
  SCENARIO_CLEAR_DAY = 1,
  SCENARIO_CLOUDY_DAY = 2,
  SCENARIO_LOW_BATTERY = 3,
  SCENARIO_FAULT_EVENT = 4
};

ScenarioType currentScenario = SCENARIO_CLEAR_DAY;

// Simulation state
int simMinute = 0;
float batterySOC = 75.0f;
int deCount = 0;

// Last computed variables
float shortwaveRadiation = 0.0f;
float cloudCover = 0.0f;
float precipitationProbability = 0.0f;
float weatherIndex = 0.0f;
float localIrradiance = 0.0f;
float pvPower = 0.0f;
float loadPower = 0.0f;
float netBatteryPower = 0.0f;
float demandIndex = 0.0f;
float mainCentroid = 0.0f;
int operatingMode = 0;     // applied mode after anti-chattering logic
int fisMode = 0;           // raw Main FIS recommendation
int requestedMode = 0;     // mode after deterministic restrictions, before stabilization
int appliedMode = 0;       // mode applied to outputs
int candidateMode = 0;     // pending mode candidate
int candidateCount = 0;    // consecutive confirmations of candidate mode
int activeOutputs = 0;

// Fault/restriction state for article plots.
// 0 = normal operation
// 1 = non-critical restriction / functions blocked
// 2 = sensor or data fault, charging outputs blocked
// 3 = critical safety fault / lockout
int faultState = 0;

int dataFaultActive = 0;       // 1 while the simulated data fault is injected
int criticalFaultActive = 0;   // 1 while a simulated critical safety fault is injected
int functionsBlocked = 0;      // 1 if any deterministic restriction blocks functions
int trackingBlocked = 0;       // 1 if solar tracking is blocked by restrictions
int outputsBlocked = 0;        // 1 if charging outputs are blocked by restrictions

// Data-error fault threshold.
// A threshold of 3 means the data fault must persist for approximately
// 15 simulated minutes before it is treated as a fault.
const int DATA_ERROR_FAULT_THRESHOLD = 3;

// =========================================================
// Anti-chattering / dwell-time configuration
// =========================================================
//
// The FIS can change its recommendation every simulation step.
// In a real system, charging outputs should not switch ON/OFF rapidly.
// Therefore, the requested mode must remain stable for several steps and
// the current applied mode must satisfy a minimum dwell time before changing.
// Critical restrictions bypass this filter and are applied immediately.

const int MODE_CONFIRM_STEPS = 2;      // 2 steps x 5 min = 10 min confirmation
const int MIN_MODE_DWELL_MIN = 15;     // minimum time before changing applied mode
int lastModeChangeMin = 0;

// =========================================================
// Utility functions
// =========================================================

float clampFloat(float x, float xmin, float xmax) {
  if (x < xmin) return xmin;
  if (x > xmax) return xmax;
  return x;
}

float trimf(float x, float a, float b, float c) {
  if (x <= a || x >= c) return 0.0f;
  if (x == b) return 1.0f;
  if (x < b) return (x - a) / (b - a);
  return (c - x) / (c - b);
}

float trapmf(float x, float a, float b, float c, float d) {
  if (x <= a || x >= d) return 0.0f;
  if (x >= b && x <= c) return 1.0f;
  if (x > a && x < b) return (x - a) / (b - a);
  return (d - x) / (d - c);
}

// Output membership functions for operating mode universe [0, 5]
float modeMF(float x, int mode) {
  switch (mode) {
    case 0: return trapmf(x, -0.5f, 0.0f, 0.35f, 0.85f);
    case 1: return trimf(x, 0.3f, 1.0f, 1.7f);
    case 2: return trimf(x, 1.3f, 2.0f, 2.7f);
    case 3: return trimf(x, 2.3f, 3.0f, 3.7f);
    case 4: return trimf(x, 3.3f, 4.0f, 4.7f);
    case 5: return trapmf(x, 4.15f, 4.65f, 5.0f, 5.5f);
    default: return 0.0f;
  }
}

const char* scenarioName() {
  switch (currentScenario) {
    case SCENARIO_CLEAR_DAY: return "clear_day";
    case SCENARIO_CLOUDY_DAY: return "cloudy_day";
    case SCENARIO_LOW_BATTERY: return "low_battery";
    case SCENARIO_FAULT_EVENT: return "fault_event";
    default: return "unknown";
  }
}

// =========================================================
// Simulated profiles
// =========================================================

float solarProfileClearSky(int minute) {
  // Smooth daylight profile between 06:00 and 18:00.
  // Maximum near noon.
  const int sunrise = 360;
  const int sunset = 1080;

  if (minute < sunrise || minute > sunset) return 0.0f;

  float x = (float)(minute - sunrise) / (float)(sunset - sunrise);
  float s = sinf(PI * x);
  if (s < 0.0f) s = 0.0f;

  // Peak around 950 W/m2
  return 950.0f * powf(s, 1.20f);
}

float demandProfile(int minute) {
  // High-demand daily profile for article simulations.
  // The purpose is to show service maximization when energy is available,
  // and battery-supported service when stored energy is high.
  float hour = minute / 60.0f;

  if (hour < 5.5f) return 0.20f;   // night standby demand
  if (hour < 7.0f) return 0.70f;   // early arrivals
  if (hour < 10.0f) return 1.00f;  // morning peak
  if (hour < 13.0f) return 0.95f;  // sustained demand
  if (hour < 17.0f) return 1.00f;  // midday/afternoon high demand
  if (hour < 20.0f) return 1.00f;  // departure peak
  if (hour < 23.0f) return 0.75f;  // evening demand
  return 0.35f;
}

float cloudCoverProfile(int minute) {
  float hour = minute / 60.0f;

  switch (currentScenario) {
    case SCENARIO_CLEAR_DAY:
      return 10.0f + 5.0f * sinf(2.0f * PI * hour / 24.0f);

    case SCENARIO_CLOUDY_DAY:
      // Heavy cloud period around midday
      if (hour >= 10.0f && hour <= 15.5f) return 75.0f;
      return 45.0f;

    case SCENARIO_LOW_BATTERY:
      return 35.0f;

    case SCENARIO_FAULT_EVENT:
      return 20.0f;

    default:
      return 30.0f;
  }
}

float precipitationProfile(int minute) {
  float hour = minute / 60.0f;

  switch (currentScenario) {
    case SCENARIO_CLEAR_DAY:
      return 5.0f;

    case SCENARIO_CLOUDY_DAY:
      if (hour >= 11.0f && hour <= 15.0f) return 55.0f;
      return 25.0f;

    case SCENARIO_LOW_BATTERY:
      return 10.0f;

    case SCENARIO_FAULT_EVENT:
      if (hour >= 13.0f && hour <= 13.5f) return 20.0f;
      return 8.0f;

    default:
      return 10.0f;
  }
}

bool simulatedDataFault(int minute) {
  // A data fault event is injected between 13:00 and 13:45.
  // With 5-min simulation steps, this gives enough consecutive samples
  // to trigger faultState and make the event visible in the result plot.
  if (currentScenario == SCENARIO_FAULT_EVENT) {
    return (minute >= 780 && minute <= 825);
  }
  return false;
}

bool simulatedCriticalFault(int minute) {
  // A critical safety fault is injected between 14:30 and 14:50.
  // This represents a condition such as overtemperature, emergency stop,
  // severe electrical fault, or another local safety lockout.
  if (currentScenario == SCENARIO_FAULT_EVENT) {
    return (minute >= 870 && minute <= 890);
  }
  return false;
}

// =========================================================
// Weather FIS
// Mamdani-type compact implementation with centroid defuzzification.
// No wind speed is used.
// =========================================================

float weatherOutputMF(float x, int term) {
  // Universe [0, 1]
  // 0 = poor, 1 = moderate, 2 = favorable
  switch (term) {
    case 0: return trapmf(x, -0.10f, 0.00f, 0.20f, 0.45f); // Poor
    case 1: return trimf(x, 0.25f, 0.50f, 0.75f);          // Moderate
    case 2: return trapmf(x, 0.55f, 0.80f, 1.00f, 1.10f);  // Favorable
    default: return 0.0f;
  }
}

float evaluateWeatherFIS(float swrad, float cloud, float precip) {
  // Input membership functions
  float radLow  = trapmf(swrad, -50.0f, 0.0f, 150.0f, 350.0f);
  float radMed  = trimf(swrad, 200.0f, 500.0f, 800.0f);
  float radHigh = trapmf(swrad, 650.0f, 850.0f, 1000.0f, 1100.0f);

  float cloudLow  = trapmf(cloud, -5.0f, 0.0f, 20.0f, 40.0f);
  float cloudMed  = trimf(cloud, 25.0f, 50.0f, 75.0f);
  float cloudHigh = trapmf(cloud, 60.0f, 80.0f, 100.0f, 105.0f);

  float prLow  = trapmf(precip, -5.0f, 0.0f, 15.0f, 35.0f);
  float prMed  = trimf(precip, 20.0f, 50.0f, 80.0f);
  float prHigh = trapmf(precip, 65.0f, 85.0f, 100.0f, 105.0f);

  // Rule activation levels for output terms
  float outPoor = 0.0f;
  float outModerate = 0.0f;
  float outFavorable = 0.0f;

  // Rules for favorable weather
  outFavorable = max(outFavorable, min(radHigh, min(cloudLow, prLow)));
  outFavorable = max(outFavorable, min(radHigh, min(cloudMed, prLow)));
  outFavorable = max(outFavorable, min(radMed, min(cloudLow, prLow)));

  // Rules for moderate weather
  outModerate = max(outModerate, min(radMed, min(cloudMed, prLow)));
  outModerate = max(outModerate, min(radMed, min(cloudLow, prMed)));
  outModerate = max(outModerate, min(radHigh, min(cloudHigh, prLow)));
  outModerate = max(outModerate, min(radHigh, min(cloudMed, prMed)));
  outModerate = max(outModerate, min(radLow, min(cloudLow, prLow)));

  // Rules for poor weather
  outPoor = max(outPoor, radLow);
  outPoor = max(outPoor, cloudHigh);
  outPoor = max(outPoor, prHigh);
  outPoor = max(outPoor, min(cloudMed, prMed));

  // Centroid defuzzification
  float numerator = 0.0f;
  float denominator = 0.0f;

  for (int i = 0; i <= 100; i++) {
    float x = i / 100.0f;

    float muPoor = min(outPoor, weatherOutputMF(x, 0));
    float muModerate = min(outModerate, weatherOutputMF(x, 1));
    float muFavorable = min(outFavorable, weatherOutputMF(x, 2));

    float muAgg = max(muPoor, max(muModerate, muFavorable));

    numerator += x * muAgg;
    denominator += muAgg;
  }

  if (denominator <= 0.0001f) return 0.0f;
  return numerator / denominator;
}

// =========================================================
// Main Decision FIS
// Compact Mamdani-type decision layer.
// =========================================================

void evaluateMainFIS(
  float soc,
  float pnet,
  float irradiance,
  float wIndex,
  float demand,
  float &centroid,
  int &mode
) {
  // Input membership functions
  float socCritical = trapmf(soc, -5.0f, 0.0f, 15.0f, 25.0f);
  float socLow      = trimf(soc, 15.0f, 30.0f, 45.0f);
  float socMedium   = trimf(soc, 35.0f, 55.0f, 75.0f);
  float socHigh     = trapmf(soc, 65.0f, 80.0f, 100.0f, 105.0f);
  float socFull     = trapmf(soc, 85.0f, 92.0f, 100.0f, 105.0f);

  float pNegative = trapmf(pnet, -400.0f, -300.0f, -60.0f, 0.0f);
  float pSlightNegative = trapmf(pnet, -180.0f, -120.0f, -20.0f, 20.0f);
  float pStrongNegative = trapmf(pnet, -450.0f, -350.0f, -220.0f, -120.0f);
  float pBalanced = trimf(pnet, -80.0f, 0.0f, 80.0f);
  float pPositive = trapmf(pnet, 0.0f, 60.0f, 300.0f, 400.0f);

  float irrLow  = trapmf(irradiance, -50.0f, 0.0f, 150.0f, 350.0f);
  float irrMed  = trimf(irradiance, 250.0f, 500.0f, 750.0f);
  float irrHigh = trapmf(irradiance, 650.0f, 850.0f, 1000.0f, 1100.0f);

  float wPoor = trapmf(wIndex, -0.10f, 0.00f, 0.20f, 0.45f);
  float wModerate = trimf(wIndex, 0.25f, 0.50f, 0.75f);
  float wFavorable = trapmf(wIndex, 0.55f, 0.80f, 1.00f, 1.10f);

  float dLow = trapmf(demand, -0.10f, 0.00f, 0.20f, 0.45f);
  float dMedium = trimf(demand, 0.25f, 0.50f, 0.75f);
  float dHigh = trapmf(demand, 0.55f, 0.80f, 1.00f, 1.10f);

  // Useful combined conditions for rule-base coverage.
  // These prevent uncovered combinations from falling to M0 when the system is actually safe.
  float energyOk = max(pBalanced, pPositive);
  float solarOk = max(irrMed, irrHigh);
  float weatherOk = max(wModerate, wFavorable);
  float demandActive = max(dMedium, dHigh);

  // Battery-supported service condition:
  // When SOC is high and demand is active, limited charging can be allowed even if
  // the station is not in a peak-PV condition. This makes the battery useful as
  // an energy buffer, instead of keeping it saturated at 100%.
  float batteryServiceAvailable = min(socHigh, demandActive);
  float highEnergyService = min(socFull, min(dHigh, max(pPositive, min(pBalanced, solarOk))));

  // Output activations for M0-M5
  float out[6] = {0, 0, 0, 0, 0, 0};

  // Dominant safety/energy protection rules
  out[0] = max(out[0], socCritical);
  out[0] = max(out[0], min(socLow, pNegative));
  out[1] = max(out[1], min(socLow, pBalanced));
  out[1] = max(out[1], min(socLow, irrLow));

  // Night or very poor solar conditions: allow telemetry, not tracking.
  // This replaces the previous conservative rule that could produce M2 during nighttime.
  out[1] = max(out[1], min(socHigh, min(irrLow, wPoor)));

  // Basic operational availability
  out[2] = max(out[2], min(socMedium, pBalanced));
  out[2] = max(out[2], min(socMedium, min(irrMed, wModerate)));
  out[2] = max(out[2], min(socHigh, min(energyOk, min(solarOk, wPoor))));

  // One output under moderate or favorable conditions
  out[3] = max(out[3], min(socMedium, min(pPositive, min(irrMed, wModerate))));
  out[3] = max(out[3], min(socHigh, min(pBalanced, min(solarOk, weatherOk))));
  out[3] = max(out[3], min(socHigh, min(pPositive, min(solarOk, dLow))));
  out[3] = max(out[3], min(socHigh, min(pPositive, min(solarOk, demandActive))));

  // Full-battery / curtailment-avoidance rule:
  // If the battery is near full and net power is positive, permit at least one output
  // even when the weather index is low, as long as there is active demand.
  out[3] = max(out[3], min(socFull, min(pPositive, demandActive)));

  // Battery-supported limited service:
  // If demand is active and SOC is high, the station can intentionally use stored
  // energy to serve scooters, even when solar generation is low or slightly negative.
  // This rule is restricted by deterministic SOC protection outside the FIS.
  out[3] = max(out[3], min(batteryServiceAvailable, pBalanced));
  out[3] = max(out[3], min(batteryServiceAvailable, pSlightNegative));
  out[3] = max(out[3], min(batteryServiceAvailable, min(wPoor, irrLow)));

  // Stronger battery-supported service when SOC is full and demand is high.
  // This makes the SOC decrease from 100% under sustained demand, as expected
  // in a station that uses the battery bank as an energy buffer.
  out[4] = max(out[4], min(socFull, min(dHigh, max(pBalanced, pSlightNegative))));

  // Two outputs
  out[4] = max(out[4], min(socHigh, min(pPositive, min(irrHigh, min(wFavorable, dMedium)))));
  out[4] = max(out[4], min(socHigh, min(pPositive, min(irrHigh, min(wModerate, dHigh)))));
  out[4] = max(out[4], min(socMedium, min(pPositive, min(irrHigh, min(wFavorable, dHigh)))));
  out[4] = max(out[4], min(socFull, min(pPositive, min(solarOk, min(weatherOk, dHigh)))));

  // If SOC is full, demand is high, and PV power is positive, allow stronger service.
  out[4] = max(out[4], min(socFull, min(pPositive, demandActive)));

  // Three-output utilization rules:
  // These rules allow the controller to reach M5 under high-energy/high-demand
  // conditions. Weather is still considered, but it is not the only gate,
  // because local irradiance and battery SOC indicate actual available energy.
  out[5] = max(out[5], min(socFull, min(pPositive, min(irrHigh, dHigh))));
  out[5] = max(out[5], min(socFull, min(pPositive, min(solarOk, dHigh))));
  out[5] = max(out[5], min(socFull, min(energyOk, min(irrHigh, dHigh))));
  out[5] = max(out[5], min(highEnergyService, weatherOk));

  // Three outputs under clearly favorable conditions
  out[5] = max(out[5], min(socHigh, min(pPositive, min(irrHigh, min(wFavorable, dHigh)))));

  // Extra conservative rules.
  // Negative net power is restrictive mainly when SOC is not high.
  // With high/full SOC, limited battery-supported service is allowed by the rules above.
  out[1] = max(out[1], min(pStrongNegative, max(dMedium, dHigh)));
  out[1] = max(out[1], min(socLow, min(pNegative, max(dMedium, dHigh))));
  out[2] = max(out[2], min(wPoor, min(socMedium, energyOk)));

  // Centroid defuzzification over [0, 5]
  float numerator = 0.0f;
  float denominator = 0.0f;

  for (int i = 0; i <= 500; i++) {
    float x = i / 100.0f;  // 0.00 to 5.00
    float muAgg = 0.0f;

    for (int m = 0; m <= 5; m++) {
      float muClipped = min(out[m], modeMF(x, m));
      muAgg = max(muAgg, muClipped);
    }

    numerator += x * muAgg;
    denominator += muAgg;
  }

  if (denominator <= 0.0001f) {
    centroid = 0.0f;
    mode = 0;
    return;
  }

  centroid = numerator / denominator;

  // Map centroid to nearest operating mode
  mode = (int)roundf(centroid);
  if (mode < 0) mode = 0;
  if (mode > 5) mode = 5;
}

// =========================================================
// Operating mode to outputs and deterministic restrictions
// =========================================================

int outputsFromOperatingMode(int mode) {
  if (mode <= 2) return 0;
  if (mode == 3) return 1;
  if (mode == 4) return 2;
  return 3;
}

int elapsedMinutesSince(int currentMin, int previousMin) {
  if (currentMin >= previousMin) {
    return currentMin - previousMin;
  }
  return (MINUTES_PER_DAY - previousMin) + currentMin;
}

int stabilizeOperatingMode(int requested, bool immediateChange) {
  // Safety-relevant conditions must be applied immediately.
  if (immediateChange) {
    appliedMode = requested;
    candidateMode = requested;
    candidateCount = 0;
    lastModeChangeMin = simMinute;
    return appliedMode;
  }

  // No change requested.
  if (requested == appliedMode) {
    candidateMode = requested;
    candidateCount = 0;
    return appliedMode;
  }

  // A new candidate mode appears.
  if (requested != candidateMode) {
    candidateMode = requested;
    candidateCount = 1;
    return appliedMode;
  }

  // Same candidate persists.
  candidateCount++;

  int elapsed = elapsedMinutesSince(simMinute, lastModeChangeMin);

  // Apply the new mode only if it persists and the dwell time is satisfied.
  if (candidateCount >= MODE_CONFIRM_STEPS && elapsed >= MIN_MODE_DWELL_MIN) {
    appliedMode = candidateMode;
    candidateCount = 0;
    lastModeChangeMin = simMinute;
  }

  return appliedMode;
}

void updateFaultStateAndRequestedMode() {
  faultState = 0;
  functionsBlocked = 0;
  trackingBlocked = 0;
  outputsBlocked = 0;
  criticalFaultActive = 0;

  // Start from the raw fuzzy recommendation.
  requestedMode = fisMode;

  // ---------------------------------------------------------
  // 1) Data-fault detection
  // ---------------------------------------------------------
  dataFaultActive = simulatedDataFault(simMinute) ? 1 : 0;

  if (dataFaultActive) {
    deCount++;
  } else {
    // Slowly recover counter when the data stream is valid again.
    if (deCount > 0) deCount--;
  }

  bool persistentDataFault = (deCount >= DATA_ERROR_FAULT_THRESHOLD);

  // ---------------------------------------------------------
  // 2) Critical safety fault simulation
  // ---------------------------------------------------------
  criticalFaultActive = simulatedCriticalFault(simMinute) ? 1 : 0;

  bool criticalEnergyFault = (batterySOC <= 15.0f);
  bool lowBatteryRestriction = (batterySOC > 15.0f && batterySOC <= 25.0f);

  // ---------------------------------------------------------
  // 3) Deterministic restrictions and lockouts
  // ---------------------------------------------------------

  // Critical faults override everything and force M0.
  if (criticalEnergyFault || criticalFaultActive) {
    requestedMode = M0_BASIC_ONLY;
    faultState = 3;
    functionsBlocked = 1;
    trackingBlocked = 1;
    outputsBlocked = 1;
    return;
  }

  // Persistent data/sensor faults are not necessarily energy-critical,
  // but they block charging and tracking because decisions are no longer reliable.
  if (persistentDataFault) {
    if (requestedMode > M1_TELEMETRY) requestedMode = M1_TELEMETRY;
    faultState = max(faultState, 2);
    functionsBlocked = 1;
    trackingBlocked = 1;
    outputsBlocked = 1;
  }

  // Low battery is a non-critical restriction: telemetry remains available,
  // but charging and tracking are blocked to protect the battery.
  if (lowBatteryRestriction) {
    if (requestedMode > M1_TELEMETRY) requestedMode = M1_TELEMETRY;
    faultState = max(faultState, 1);
    functionsBlocked = 1;
    trackingBlocked = 1;
    outputsBlocked = 1;
  }

  // Optional deterministic tracking restriction:
  // tracking is blocked when solar conditions are not useful enough.
  // This does not necessarily block charging if battery-supported service is allowed.
  bool poorTrackingCondition = (localIrradiance < 120.0f || weatherIndex < 0.20f);

  if (poorTrackingCondition && requestedMode >= M2_TRACKING_ALLOWED) {
    if (requestedMode == M2_TRACKING_ALLOWED) {
      requestedMode = M1_TELEMETRY;
    }
    trackingBlocked = 1;
    functionsBlocked = 1;
    faultState = max(faultState, 1);
  }

  // If the FIS wanted charging service but deterministic restrictions reduced it,
  // mark the charging outputs as blocked.
  if (fisMode >= M3_ONE_OUTPUT && requestedMode < M3_ONE_OUTPUT) {
    outputsBlocked = 1;
    functionsBlocked = 1;
    faultState = max(faultState, 1);
  }
}

void applyStableModeToOutputs() {
  // Restrictions and faults bypass the anti-chattering filter
  // when they reduce operation for safety.
  bool immediateChange = (faultState > 0);

  operatingMode = stabilizeOperatingMode(requestedMode, immediateChange);
  activeOutputs = outputsFromOperatingMode(operatingMode);

  // Data faults, low-battery restrictions, and critical faults force charging outputs off.
  if (outputsBlocked || faultState >= 2) {
    activeOutputs = 0;
  }

  // If outputs were expected from the FIS but the final output count is zero,
  // keep outputsBlocked visible in the CSV.
  if (fisMode >= M3_ONE_OUTPUT && activeOutputs == 0) {
    outputsBlocked = 1;
    functionsBlocked = 1;
    if (faultState == 0) faultState = 1;
  }
}

// =========================================================
// Energy model
// =========================================================

void updateSimulatedInputs() {
  shortwaveRadiation = solarProfileClearSky(simMinute);

  cloudCover = clampFloat(cloudCoverProfile(simMinute), 0.0f, 100.0f);
  precipitationProbability = clampFloat(precipitationProfile(simMinute), 0.0f, 100.0f);

  // Convert cloud cover into irradiance reduction
  float cloudFactor = 1.0f - 0.75f * (cloudCover / 100.0f);
  cloudFactor = clampFloat(cloudFactor, 0.15f, 1.0f);

  localIrradiance = shortwaveRadiation * cloudFactor;

  // Scenario-specific low battery initial condition is handled in resetScenario()
  demandIndex = demandProfile(simMinute);

  // Weather FIS
  weatherIndex = evaluateWeatherFIS(shortwaveRadiation, cloudCover, precipitationProbability);

  // Preliminary PV power.
  // In a real implementation, this would come from sensors.
  pvPower = PV_NOMINAL_POWER_W * (localIrradiance / 1000.0f) * PV_EFFICIENCY;
  if (pvPower < 0.0f) pvPower = 0.0f;
}

void updateEnergyBalance() {
  loadPower = ELECTRONICS_POWER_W + activeOutputs * (SCOOTER_POWER_W / BOOST_EFFICIENCY);
  netBatteryPower = pvPower - loadPower;

  float dtHours = SIM_STEP_MIN / 60.0f;
  float deltaSOC = (netBatteryPower * dtHours / BATTERY_ENERGY_WH) * 100.0f;

  batterySOC += deltaSOC;
  batterySOC = clampFloat(batterySOC, 0.0f, 100.0f);
}

// =========================================================
// CSV output
// =========================================================

void printCSVHeader() {
  Serial.println("scenario,time_min,time_h,soc,shortwave_radiation,cloud_cover,precipitation_probability,weather_index,local_irradiance,pv_power,load_power,pnet,demand_index,centroid,fis_mode,requested_mode,operating_mode,active_outputs,data_fault_active,critical_fault_active,functions_blocked,tracking_blocked,outputs_blocked,fault_state,de_count,candidate_mode,candidate_count");
}

void printCSVRow() {
  Serial.print(scenarioName()); Serial.print(",");
  Serial.print(simMinute); Serial.print(",");
  Serial.print(simMinute / 60.0f, 3); Serial.print(",");
  Serial.print(batterySOC, 3); Serial.print(",");
  Serial.print(shortwaveRadiation, 3); Serial.print(",");
  Serial.print(cloudCover, 3); Serial.print(",");
  Serial.print(precipitationProbability, 3); Serial.print(",");
  Serial.print(weatherIndex, 4); Serial.print(",");
  Serial.print(localIrradiance, 3); Serial.print(",");
  Serial.print(pvPower, 3); Serial.print(",");
  Serial.print(loadPower, 3); Serial.print(",");
  Serial.print(netBatteryPower, 3); Serial.print(",");
  Serial.print(demandIndex, 4); Serial.print(",");
  Serial.print(mainCentroid, 4); Serial.print(",");
  Serial.print(fisMode); Serial.print(",");
  Serial.print(requestedMode); Serial.print(",");
  Serial.print(operatingMode); Serial.print(",");
  Serial.print(activeOutputs); Serial.print(",");
  Serial.print(dataFaultActive); Serial.print(",");
  Serial.print(criticalFaultActive); Serial.print(",");
  Serial.print(functionsBlocked); Serial.print(",");
  Serial.print(trackingBlocked); Serial.print(",");
  Serial.print(outputsBlocked); Serial.print(",");
  Serial.print(faultState); Serial.print(",");
  Serial.print(deCount); Serial.print(",");
  Serial.print(candidateMode); Serial.print(",");
  Serial.println(candidateCount);
}

// =========================================================
// Scenario control
// =========================================================

void resetScenario() {
  simMinute = 0;
  deCount = 0;
  faultState = 0;
  dataFaultActive = 0;
  criticalFaultActive = 0;
  functionsBlocked = 0;
  trackingBlocked = 0;
  outputsBlocked = 0;
  activeOutputs = 0;
  operatingMode = 0;
  fisMode = 0;
  requestedMode = 0;
  appliedMode = 0;
  candidateMode = 0;
  candidateCount = 0;
  lastModeChangeMin = 0;
  mainCentroid = 0.0f;

  switch (currentScenario) {
    case SCENARIO_CLEAR_DAY:
      batterySOC = 90.0f;
      break;

    case SCENARIO_CLOUDY_DAY:
      // Full-battery cloudy scenario:
      // useful to show limited battery-supported service under reduced solar availability.
      batterySOC = 100.0f;
      break;

    case SCENARIO_LOW_BATTERY:
      batterySOC = 24.0f;
      break;

    case SCENARIO_FAULT_EVENT:
      batterySOC = 90.0f;
      break;

    default:
      batterySOC = 75.0f;
      break;
  }

  Serial.println();
  Serial.print("# Scenario reset: ");
  Serial.println(scenarioName());
  if (CSV_OUTPUT) printCSVHeader();
}

void handleSerialCommands() {
  if (!Serial.available()) return;

  char c = Serial.read();

  if (c == '1') {
    currentScenario = SCENARIO_CLEAR_DAY;
    resetScenario();
  } else if (c == '2') {
    currentScenario = SCENARIO_CLOUDY_DAY;
    resetScenario();
  } else if (c == '3') {
    currentScenario = SCENARIO_LOW_BATTERY;
    resetScenario();
  } else if (c == '4') {
    currentScenario = SCENARIO_FAULT_EVENT;
    resetScenario();
  } else if (c == 'r' || c == 'R') {
    resetScenario();
  } else if (c == 'h' || c == 'H') {
    printCSVHeader();
  }
}

// =========================================================
// Arduino setup and loop
// =========================================================

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(1000);

  Serial.println("# ESP32 Solar Charging Station - FIS Article Simulation Logger");
  Serial.println("# Commands: 1=clear_day, 2=cloudy_day, 3=low_battery, 4=fault_event, r=reset, h=header");
  resetScenario();
}

void loop() {
  handleSerialCommands();

  // 1) Generate simulated inputs
  updateSimulatedInputs();

  // 2) Estimate the current load using previous active output state
  loadPower = ELECTRONICS_POWER_W + activeOutputs * (SCOOTER_POWER_W / BOOST_EFFICIENCY);
  netBatteryPower = pvPower - loadPower;

  // 3) Evaluate Main FIS.
  // fisMode is the raw fuzzy recommendation.
  evaluateMainFIS(
    batterySOC,
    netBatteryPower,
    localIrradiance,
    weatherIndex,
    demandIndex,
    mainCentroid,
    fisMode
  );

  // 4) Apply deterministic safety restrictions before stabilization.
  // This generates requestedMode.
  updateFaultStateAndRequestedMode();

  // 5) Apply anti-chattering / dwell-time logic.
  // This generates operatingMode and activeOutputs.
  applyStableModeToOutputs();

  // 6) Recalculate load and energy balance using final active outputs.
  updateEnergyBalance();

  // 7) Print CSV row
  if (CSV_OUTPUT) {
    printCSVRow();
  }

  // 8) Advance simulated time
  simMinute += SIM_STEP_MIN;
  if (simMinute >= MINUTES_PER_DAY) {
    simMinute = 0;

    // Keep scenario running for multiple days,
    // but do not reset SOC automatically.
    Serial.println("# End of simulated day");
    if (CSV_OUTPUT) printCSVHeader();
  }

  delay(SIM_STEP_DELAY_MS);
}
