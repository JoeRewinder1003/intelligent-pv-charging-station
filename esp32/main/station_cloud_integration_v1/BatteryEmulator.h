#pragma once

#include <Arduino.h>

enum class BatteryProtectionState : uint8_t
{
  NORMAL = 0,
  RESTRICTED = 1,
  CRITICAL = 2,
};

struct BatteryConfig
{
  // Confirmed bank configuration.
  uint8_t batteryCount = 3;
  float capacityAhPerBattery = 100.0f;
  float nominalVoltageV = 12.0f;
  float initialSocPercent = 95.0f;
  // Simulation-only capacity retention used as SOH ground truth.
  // This value is not reported to the cloud as an estimated SOH.
  float emulatedCapacityRetentionPercent = 100.0f;
  // Configurable energy-efficiency assumptions used by the emulator.
  // These values are simulation parameters and are not measured
  // efficiencies of the installed Felicity Solar batteries.
  float chargeEfficiency = 0.90f;
  float dischargeEfficiency = 0.95f;

  // Manufacturer value: approximately 5 mOhm per battery.
  float internalResistancePerBatteryOhm = 0.005f;

  // Experimental parameter. It remains zero until measured.
  float wiringResistanceOhm = 0.0f;

  // Expected operating ranges and absolute protection thresholds.
  float maxNormalChargeCurrentA = 32.0f;
  float absoluteChargeCurrentA = 50.0f;
  float maxNormalDischargeCurrentA = 30.0f;
  float overcurrentDischargeA = 35.0f;

  // Supervisory SOC thresholds with hysteresis.
  float restrictedSocPercent = 50.0f;
  float criticalSocPercent = 20.0f;
  float normalRecoverySocPercent = 55.0f;
  float criticalRecoverySocPercent = 25.0f;

  // Manufacturer final-discharge voltage for I <= 0.2C.
  float minimumTerminalVoltageV = 10.5f;
};

struct BatteryState
{
  float socPercent = 0.0f;
  float openCircuitVoltageV = 0.0f;
  float terminalVoltageV = 0.0f;
  float currentA = 0.0f;
  float powerW = 0.0f;

  float storedEnergyWh = 0.0f;
  float chargedEnergyWh = 0.0f;
  float dischargedEnergyWh = 0.0f;
  float consumedCapacityAh = 0.0f;

  bool charging = false;
  bool discharging = false;
  bool normalCurrentExceeded = false;
  bool overcurrent = false;
  bool undervoltage = false;
  bool powerModelLimited = false;

  BatteryProtectionState protectionState =
      BatteryProtectionState::NORMAL;
};

class BatteryEmulator
{
public:
  explicit BatteryEmulator(const BatteryConfig &config = BatteryConfig());

  void begin();
  void update(float requestedBatteryPowerW, float deltaTimeSeconds);
  void setSocPercent(float socPercent);

  const BatteryConfig &config() const;
  const BatteryState &state() const;

  float bankCapacityAh() const;
  float nominalEnergyWh() const;
  float effectiveCapacityAh() const;
  float effectiveEnergyWh() const;
  float equivalentResistanceOhm() const;

  const char *protectionStateText() const;
  bool isCritical() const;
  bool isRestricted() const;

private:
  BatteryConfig config_;
  BatteryState state_;

  float interpolateOcv(float socPercent) const;
  void solveElectricalState(float requestedBatteryPowerW);
  void updateProtectionState();
  void sanitizeConfiguration();
};
