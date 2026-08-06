#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <string.h>
#include <math.h>

#include "secrets.h"
#include "BatteryEmulator.h"
#include "ScenarioManager.h"

/*
 * AWS IoT connectivity and local command-validation test.
 *
 * This firmware:
 * - Connects the ESP32 to Wi-Fi and AWS IoT Core.
 * - Synchronizes UTC time with NTP.
 * - Runs a functional station simulation through reproducible scenarios.
 * - Emulates a configurable GEL lead-acid battery bank.
 * - Publishes dynamic test telemetry every 15 seconds.
 * - Receives cloud commands.
 * - Classifies commands as accepted, invalid_command, or
 *   blocked_by_safety.
 * - Publishes an acknowledgement to AWS.
 *
 * This firmware DOES NOT activate relays, charging outputs,
 * tracking motors, or linear actuators. Every acknowledgement
 * is sent with applied=false.
 */

// ============================================================
// MQTT topics
// ============================================================

static constexpr char TELEMETRY_TOPIC[] =
    "station/station_001/telemetry";

static constexpr char COMMAND_TOPIC[] =
    "station/station_001/commands";

static constexpr char ACK_TOPIC[] =
    "station/station_001/acks";

// ============================================================
// Timing and test configuration
// ============================================================

static constexpr uint16_t MQTT_PORT = 8883;
static constexpr unsigned long TELEMETRY_INTERVAL_MS = 15000;
static constexpr unsigned long BATTERY_UPDATE_INTERVAL_MS = 1000;
static constexpr unsigned long MQTT_RETRY_INTERVAL_MS = 5000;
static constexpr unsigned long ACK_RETRY_INTERVAL_MS = 2000;
static constexpr time_t MIN_VALID_EPOCH = 1704067200;  // 2024-01-01 UTC

static constexpr char RESTRICTED_TEST_MODE[] = "M2";
static constexpr char LOCKOUT_TEST_MODE[] = "M0";

// Simplified functional-simulation constants.
static constexpr float BASE_LOAD_POWER_W = 5.0f;
static constexpr float SCOOTER_USEFUL_POWER_W = 71.0f;
static constexpr float BOOST_EFFICIENCY = 0.88f;

// ============================================================
// MQTT and TLS clients
// ============================================================

WiFiClientSecure secureClient;
PubSubClient mqttClient(secureClient);

// ============================================================
// Runtime state
// ============================================================

unsigned long lastTelemetryMs = 0;
unsigned long lastBatteryUpdateMs = 0;
unsigned long lastMqttAttemptMs = 0;
unsigned long lastAckAttemptMs = 0;
uint32_t telemetryCounter = 0;

BatteryConfig batteryConfig;
BatteryEmulator batteryEmulator(batteryConfig);
ScenarioManager scenarioManager;

bool manualBatteryPowerOverride = false;
float manualBatteryPowerW = 0.0f;
float batteryTimeScale = 1.0f;

/*
 * Simulated safety state used only for this validation test.
 * It does not drive any physical output.
 */
bool simulatedCriticalLockout = false;

struct PendingCommandAck {
  bool pending = false;
  char commandId[80] = "";
  char command[48] = "";
  char status[32] = "";
  char message[180] = "";
  char resultingOperatingMode[8] = "";
};

PendingCommandAck pendingAck;

// ============================================================
// Allowed commands
// Keep this list aligned with command_dispatcher.
// ============================================================

static const char* const ALLOWED_COMMANDS[] = {
    "AUTO",
    "STOP",
    "NEUTRAL",
    "ENABLE_TRACKING",
    "DISABLE_TRACKING",
    "ENABLE_OUTPUT_1",
    "ENABLE_OUTPUT_2",
    "ENABLE_OUTPUT_3",
    "DISABLE_OUTPUTS",
    "LOCKOUT",
    "CLEAR_LOCKOUT",
};

static constexpr size_t ALLOWED_COMMAND_COUNT =
    sizeof(ALLOWED_COMMANDS) / sizeof(ALLOWED_COMMANDS[0]);

// ============================================================
// Function declarations
// ============================================================

void connectWiFi();
void configureTls();
void synchronizeTime();
void connectMqtt();
void processSerialCommands();
void printSerialHelp();
void printSimulatedSafetyState();
void printBatteryStatus();
void printScenarioStatus();
void applyScenarioInitialConditions();
void updateBatteryEmulator();

bool getUtcTimestamp(char* buffer, size_t bufferSize);
bool isAllowedCommand(const char* command);
bool isSafetyReducingCommand(const char* command);
bool copyText(char* destination, size_t destinationSize, const char* source);
bool isCriticalSafetyActive();
bool batteryOutputsAllowed();
float calculateAutomaticBatteryPowerW();
const char* currentTestOperatingMode();
const char* currentFaultState();

void publishTelemetry();
void publishPendingCommandAck();
void prepareCommandAck(
    const char* commandId,
    const char* command,
    const char* status,
    const char* message
);

void mqttCallback(char* topic, byte* payload, unsigned int length);

// ============================================================
// Setup
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("========================================");
  Serial.println("AWS IoT + functional simulation validation");
  Serial.println("No physical outputs will be activated.");
  Serial.println("========================================");
  printSerialHelp();

  batteryEmulator.begin();
  scenarioManager.begin(ScenarioType::CLEAR_DAY);
  applyScenarioInitialConditions();
  lastBatteryUpdateMs = millis();
  printScenarioStatus();
  printBatteryStatus();

  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    synchronizeTime();
  }

  configureTls();

  mqttClient.setServer(AWS_IOT_ENDPOINT, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  if (!mqttClient.setBufferSize(2048)) {
    Serial.println("Warning: MQTT buffer size could not be changed.");
  }

  connectMqtt();
  printSimulatedSafetyState();
}

// ============================================================
// Main loop
// ============================================================

void loop() {
  processSerialCommands();
  updateBatteryEmulator();

  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();

    if (WiFi.status() == WL_CONNECTED &&
        time(nullptr) < MIN_VALID_EPOCH) {
      synchronizeTime();
    }
  }

  if (!mqttClient.connected()) {
    const unsigned long now = millis();

    if (now - lastMqttAttemptMs >= MQTT_RETRY_INTERVAL_MS) {
      lastMqttAttemptMs = now;
      connectMqtt();
    }
  } else {
    mqttClient.loop();

    const unsigned long now = millis();

    if (pendingAck.pending &&
        now - lastAckAttemptMs >= ACK_RETRY_INTERVAL_MS) {
      lastAckAttemptMs = now;
      publishPendingCommandAck();
    }

    if (now - lastTelemetryMs >= TELEMETRY_INTERVAL_MS) {
      lastTelemetryMs = now;
      publishTelemetry();
    }
  }

  delay(10);
}

// ============================================================
// Wi-Fi
// ============================================================

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  Serial.printf("Connecting to Wi-Fi: %s\n", WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const unsigned long startMs = millis();
  constexpr unsigned long timeoutMs = 30000;

  while (WiFi.status() != WL_CONNECTED &&
         millis() - startMs < timeoutMs) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi connected.");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    Serial.print("Signal strength: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println("Wi-Fi connection timeout.");
  }
}

// ============================================================
// TLS
// ============================================================

void configureTls() {
  secureClient.setCACert(AWS_ROOT_CA);
  secureClient.setCertificate(AWS_DEVICE_CERT);
  secureClient.setPrivateKey(AWS_PRIVATE_KEY);
  Serial.println("TLS certificates configured.");
}

// ============================================================
// NTP
// ============================================================

void synchronizeTime() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Cannot synchronize time without Wi-Fi.");
    return;
  }

  Serial.println("Synchronizing UTC time with NTP...");

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  const unsigned long startMs = millis();
  constexpr unsigned long timeoutMs = 20000;

  while (time(nullptr) < MIN_VALID_EPOCH &&
         millis() - startMs < timeoutMs) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (time(nullptr) >= MIN_VALID_EPOCH) {
    char timestamp[25];

    if (getUtcTimestamp(timestamp, sizeof(timestamp))) {
      Serial.print("UTC time synchronized: ");
      Serial.println(timestamp);
    } else {
      Serial.println("UTC timestamp formatting failed.");
    }
  } else {
    Serial.println("NTP synchronization timeout.");
  }
}

bool getUtcTimestamp(char* buffer, size_t bufferSize) {
  if (buffer == nullptr || bufferSize == 0) {
    return false;
  }

  const time_t now = time(nullptr);

  if (now < MIN_VALID_EPOCH) {
    return false;
  }

  struct tm utcTime;

  if (gmtime_r(&now, &utcTime) == nullptr) {
    return false;
  }

  return strftime(
      buffer,
      bufferSize,
      "%Y-%m-%dT%H:%M:%SZ",
      &utcTime
  ) > 0;
}

// ============================================================
// AWS IoT MQTT connection
// ============================================================

void connectMqtt() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("MQTT connection skipped: Wi-Fi is disconnected.");
    return;
  }

  if (mqttClient.connected()) {
    return;
  }

  Serial.printf(
      "Connecting to AWS IoT as client %s...\n",
      AWS_IOT_CLIENT_ID
  );

  if (!mqttClient.connect(AWS_IOT_CLIENT_ID)) {
    Serial.print("MQTT connection failed. State: ");
    Serial.println(mqttClient.state());
    return;
  }

  Serial.println("AWS IoT MQTT connected.");

  if (mqttClient.subscribe(COMMAND_TOPIC, 1)) {
    Serial.printf("Subscribed to: %s\n", COMMAND_TOPIC);
  } else {
    Serial.println("Failed to subscribe to command topic.");
  }
}

// ============================================================
// Serial test controls
// ============================================================

void processSerialCommands() {
  if (!Serial.available()) {
    return;
  }

  String serialCommand = Serial.readStringUntil('\n');
  serialCommand.trim();
  serialCommand.toUpperCase();

  if (serialCommand.length() == 0) {
    return;
  }

  if (serialCommand == "SCENARIO STATUS") {
    printScenarioStatus();
    return;
  }

  if (serialCommand.startsWith("SCENARIO ")) {
    const String scenarioName =
        serialCommand.substring(strlen("SCENARIO "));

    if (!scenarioManager.setScenarioByName(scenarioName.c_str())) {
      Serial.println("Invalid scenario name.");
      Serial.println(
          "Allowed scenarios: CLEAR_DAY, CLOUDY_DAY, LOW_BATTERY, "
          "HIGH_DEMAND, FAULT_EVENT, TRACKING_FAULT, STALE_DATA."
      );
      return;
    }

    applyScenarioInitialConditions();
    Serial.printf(
        "Scenario changed to %s.\n",
        scenarioManager.scenarioName()
    );
    printScenarioStatus();
    printBatteryStatus();
    return;
  }

  if (serialCommand == "SAFETY ON") {
    simulatedCriticalLockout = true;
    Serial.println("Simulated critical safety lockout ENABLED.");
    printSimulatedSafetyState();
    return;
  }

  if (serialCommand == "SAFETY OFF") {
    simulatedCriticalLockout = false;
    Serial.println("Simulated critical safety lockout DISABLED.");
    printSimulatedSafetyState();
    return;
  }

  if (serialCommand == "STATUS") {
    printSimulatedSafetyState();
    printScenarioStatus();
    printBatteryStatus();
    return;
  }

  if (serialCommand == "HELP") {
    printSerialHelp();
    return;
  }

  if (serialCommand == "BATTERY STATUS") {
    printBatteryStatus();
    return;
  }

  if (serialCommand == "BATTERY AUTO") {
    manualBatteryPowerOverride = false;
    Serial.println("Battery power returned to automatic station balance.");
    printBatteryStatus();
    return;
  }

  if (serialCommand.startsWith("BATTERY POWER ")) {
    const float requestedPowerW =
        serialCommand.substring(strlen("BATTERY POWER ")).toFloat();

    if (!isfinite(requestedPowerW) ||
        requestedPowerW < -1000.0f ||
        requestedPowerW > 1000.0f) {
      Serial.println("Invalid battery power. Allowed range: -1000 to 1000 W.");
      return;
    }

    manualBatteryPowerW = requestedPowerW;
    manualBatteryPowerOverride = true;

    Serial.printf(
        "Manual battery power set to %.2f W (%s).\n",
        manualBatteryPowerW,
        manualBatteryPowerW >= 0.0f ? "charging" : "discharging"
    );
    return;
  }

  if (serialCommand.startsWith("BATTERY SOC ")) {
    const float requestedSoc =
        serialCommand.substring(strlen("BATTERY SOC ")).toFloat();

    if (!isfinite(requestedSoc) ||
        requestedSoc < 0.0f ||
        requestedSoc > 100.0f) {
      Serial.println("Invalid SOC. Allowed range: 0 to 100 percent.");
      return;
    }

    batteryEmulator.setSocPercent(requestedSoc);
    Serial.printf("Battery SOC set to %.2f%%.\n", requestedSoc);
    printBatteryStatus();
    return;
  }

  if (serialCommand.startsWith("BATTERY SCALE ")) {
    const float requestedScale =
        serialCommand.substring(strlen("BATTERY SCALE ")).toFloat();

    if (!isfinite(requestedScale) ||
        requestedScale < 1.0f ||
        requestedScale > 3600.0f) {
      Serial.println("Invalid time scale. Allowed range: 1 to 3600.");
      return;
    }

    batteryTimeScale = requestedScale;
    Serial.printf("Battery emulation time scale set to x%.1f.\n", batteryTimeScale);
    return;
  }

  Serial.println("Unknown serial command.");
  printSerialHelp();
}

void printSerialHelp() {
  Serial.println("Serial commands:");
  Serial.println("  STATUS");
  Serial.println("  SCENARIO STATUS");
  Serial.println("  SCENARIO CLEAR_DAY");
  Serial.println("  SCENARIO CLOUDY_DAY");
  Serial.println("  SCENARIO LOW_BATTERY");
  Serial.println("  SCENARIO HIGH_DEMAND");
  Serial.println("  SCENARIO FAULT_EVENT");
  Serial.println("  SCENARIO TRACKING_FAULT");
  Serial.println("  SCENARIO STALE_DATA");
  Serial.println("  SAFETY ON");
  Serial.println("  SAFETY OFF");
  Serial.println("  BATTERY STATUS");
  Serial.println("  BATTERY AUTO");
  Serial.println("  BATTERY POWER <watts>");
  Serial.println("  BATTERY SOC <0-100>");
  Serial.println("  BATTERY SCALE <1-3600>");
  Serial.println("  HELP");
}

void printSimulatedSafetyState() {
  Serial.print("Manual simulated safety state: ");
  Serial.println(
      simulatedCriticalLockout ? "CRITICAL_LOCKOUT" : "NORMAL"
  );
  Serial.print("Combined local safety state: ");
  Serial.println(isCriticalSafetyActive() ? "CRITICAL" : "NON_CRITICAL");
  Serial.print("Test operating mode: ");
  Serial.println(currentTestOperatingMode());
}

void printBatteryStatus() {
  const BatteryState& battery = batteryEmulator.state();

  Serial.println();
  Serial.println("Battery emulator status:");
  Serial.printf(
      "  Bank: %u x %.0f Ah, %.0f Wh nominal\n",
      batteryEmulator.config().batteryCount,
      batteryEmulator.config().capacityAhPerBattery,
      batteryEmulator.nominalEnergyWh()
  );
  Serial.printf("  SOC: %.3f%%\n", battery.socPercent);
  Serial.printf("  OCV: %.3f V\n", battery.openCircuitVoltageV);
  Serial.printf("  Terminal voltage: %.3f V\n", battery.terminalVoltageV);
  Serial.printf("  Current: %.3f A\n", battery.currentA);
  Serial.printf("  Power: %.3f W\n", battery.powerW);
  Serial.printf(
      "  Protection: %s\n",
      batteryEmulator.protectionStateText()
  );
  Serial.printf(
      "  Power source: %s\n",
      manualBatteryPowerOverride ? "MANUAL" : "AUTOMATIC_BALANCE"
  );
  Serial.printf("  Time scale: x%.1f\n", batteryTimeScale);
  Serial.printf(
      "  Flags: normal_limit=%s, overcurrent=%s, undervoltage=%s\n",
      battery.normalCurrentExceeded ? "true" : "false",
      battery.overcurrent ? "true" : "false",
      battery.undervoltage ? "true" : "false"
  );
  Serial.println();
}

void printScenarioStatus() {
  const ScenarioProfile& profile = scenarioManager.profile();

  Serial.println();
  Serial.println("Functional simulation scenario:");
  Serial.printf("  Name: %s\n", scenarioManager.scenarioName());
  Serial.printf(
      "  Elapsed simulated time: %.1f s\n",
      scenarioManager.elapsedSimulatedSeconds()
  );
  Serial.printf("  Initial SOC: %.1f%%\n", profile.initialSocPercent);
  Serial.printf("  Irradiance: %.1f W/m2\n", profile.irradianceWm2);
  Serial.printf("  PV power: %.1f W\n", profile.pvPowerW);
  Serial.printf("  Demand index: %.2f\n", profile.demandIndex);
  Serial.printf("  Weather index: %.2f\n", profile.weatherIndex);
  Serial.printf(
      "  Requested outputs: %u\n",
      static_cast<unsigned int>(profile.requestedOutputCount)
  );
  Serial.printf(
      "  Nominal operating mode: %s\n",
      profile.nominalOperatingMode
  );
  Serial.printf(
      "  Scenario flags: critical_fault=%s, tracking_fault=%s, "
      "stale_data=%s\n",
      profile.criticalFault ? "true" : "false",
      profile.trackingFault ? "true" : "false",
      profile.staleData ? "true" : "false"
  );
  Serial.println();
}

void applyScenarioInitialConditions() {
  batteryEmulator.setSocPercent(
      scenarioManager.profile().initialSocPercent
  );
  manualBatteryPowerOverride = false;
}

void updateBatteryEmulator() {
  const unsigned long now = millis();
  const unsigned long elapsedMs = now - lastBatteryUpdateMs;

  if (elapsedMs < BATTERY_UPDATE_INTERVAL_MS) {
    return;
  }

  lastBatteryUpdateMs = now;

  const float simulatedDeltaTimeSeconds =
      (static_cast<float>(elapsedMs) / 1000.0f) * batteryTimeScale;

  scenarioManager.update(simulatedDeltaTimeSeconds);

  const float batteryPowerW = manualBatteryPowerOverride
      ? manualBatteryPowerW
      : calculateAutomaticBatteryPowerW();

  batteryEmulator.update(
      batteryPowerW,
      simulatedDeltaTimeSeconds
  );
}

bool isCriticalSafetyActive() {
  return simulatedCriticalLockout ||
         batteryEmulator.isCritical() ||
         scenarioManager.isCriticalFaultActive();
}

bool batteryOutputsAllowed() {
  return !isCriticalSafetyActive() &&
         !batteryEmulator.isRestricted();
}

float calculateAutomaticBatteryPowerW() {
  const bool outputsEnabled = batteryOutputsAllowed();
  const uint8_t activeOutputCount = outputsEnabled
      ? scenarioManager.profile().requestedOutputCount
      : 0;

  const float outputInputPowerW =
      activeOutputCount *
      (SCOOTER_USEFUL_POWER_W / BOOST_EFFICIENCY);

  // Positive battery power means charging; negative means discharging.
  return scenarioManager.profile().pvPowerW -
         BASE_LOAD_POWER_W -
         outputInputPowerW;
}

// ============================================================
// Command validation helpers
// ============================================================

bool isAllowedCommand(const char* command) {
  if (command == nullptr || command[0] == '\0') {
    return false;
  }

  for (size_t index = 0; index < ALLOWED_COMMAND_COUNT; index++) {
    if (strcmp(command, ALLOWED_COMMANDS[index]) == 0) {
      return true;
    }
  }

  return false;
}

bool isSafetyReducingCommand(const char* command) {
  if (command == nullptr) {
    return false;
  }

  return strcmp(command, "STOP") == 0 ||
         strcmp(command, "DISABLE_OUTPUTS") == 0 ||
         strcmp(command, "DISABLE_TRACKING") == 0 ||
         strcmp(command, "LOCKOUT") == 0;
}

bool copyText(
    char* destination,
    size_t destinationSize,
    const char* source
) {
  if (destination == nullptr || destinationSize == 0 || source == nullptr) {
    return false;
  }

  const size_t sourceLength = strlen(source);

  if (sourceLength >= destinationSize) {
    return false;
  }

  memcpy(destination, source, sourceLength + 1);
  return true;
}

const char* currentTestOperatingMode() {
  if (isCriticalSafetyActive()) {
    return LOCKOUT_TEST_MODE;
  }

  if (batteryEmulator.isRestricted()) {
    return RESTRICTED_TEST_MODE;
  }

  return scenarioManager.nominalOperatingMode();
}

const char* currentFaultState() {
  if (isCriticalSafetyActive()) {
    return "critical_lockout";
  }

  return scenarioManager.faultStateText();
}

// ============================================================
// Telemetry
// ============================================================

void publishTelemetry() {
  if (!mqttClient.connected()) {
    Serial.println("Telemetry not published: MQTT is disconnected.");
    return;
  }

  char timestamp[25];

  if (!getUtcTimestamp(timestamp, sizeof(timestamp))) {
    Serial.println("Telemetry not published: UTC time is not synchronized.");
    return;
  }

  telemetryCounter++;

  const BatteryState& battery = batteryEmulator.state();
  const ScenarioProfile& scenario = scenarioManager.profile();

  const bool trackingEnabled =
      !isCriticalSafetyActive() &&
      !scenarioManager.isTrackingFaultActive();
  const bool outputsAllowed = batteryOutputsAllowed();
  const uint8_t activeOutputCount = outputsAllowed
      ? scenario.requestedOutputCount
      : 0;
  const bool output1Active = activeOutputCount >= 1;
  const bool output2Active = activeOutputCount >= 2;
  const bool output3Active = activeOutputCount >= 3;
  const char* faultState = currentFaultState();
  const char* operatingMode = currentTestOperatingMode();
  const char* nominalMode = scenarioManager.nominalOperatingMode();

  char payload[2048];

  const int written = snprintf(
      payload,
      sizeof(payload),
      "{"
        "\"station_id\":\"%s\","
        "\"timestamp\":\"%s\","
        "\"battery\":{"
          "\"voltage_v\":%.3f,"
          "\"current_a\":%.3f,"
          "\"power_w\":%.3f,"
          "\"soc_percent\":%.3f,"
          "\"ocv_v\":%.3f,"
          "\"protection_state\":\"%s\","
          "\"normal_current_exceeded\":%s,"
          "\"overcurrent\":%s,"
          "\"undervoltage\":%s"
        "},"
        "\"pv\":{"
          "\"voltage_v\":%.3f,"
          "\"current_a\":%.3f,"
          "\"power_w\":%.3f,"
          "\"local_irradiance_wm2\":%.1f"
        "},"
        "\"environment\":{"
          "\"ambient_temperature_c\":%.2f,"
          "\"relative_humidity_percent\":%.2f,"
          "\"panel_temperature_c\":%.2f"
        "},"
        "\"outputs\":{"
          "\"output_1_active\":%s,"
          "\"output_2_active\":%s,"
          "\"output_3_active\":%s,"
          "\"output_1_current_a\":%.2f,"
          "\"output_2_current_a\":%.2f,"
          "\"output_3_current_a\":%.2f"
        "},"
        "\"tracking\":{"
          "\"enabled\":%s,"
          "\"angle_deg\":%.2f,"
          "\"target_angle_deg\":%.2f,"
          "\"master_position_raw\":%u,"
          "\"slave_position_raw\":%u"
        "},"
        "\"decision\":{"
          "\"weather_index\":%.3f,"
          "\"demand_index\":%.3f,"
          "\"fis_mode\":\"%s\","
          "\"requested_mode\":\"%s\","
          "\"operating_mode\":\"%s\""
        "},"
        "\"simulation\":{"
          "\"scenario\":\"%s\","
          "\"elapsed_simulated_s\":%.1f,"
          "\"time_scale\":%.1f,"
          "\"stale_data_requested\":%s"
        "},"
        "\"fault_state\":\"%s\","
        "\"test_counter\":%lu,"
        "\"source\":\"esp32_functional_simulation_v1\""
      "}",
      AWS_IOT_CLIENT_ID,
      timestamp,
      battery.terminalVoltageV,
      battery.currentA,
      battery.powerW,
      battery.socPercent,
      battery.openCircuitVoltageV,
      batteryEmulator.protectionStateText(),
      battery.normalCurrentExceeded ? "true" : "false",
      battery.overcurrent ? "true" : "false",
      battery.undervoltage ? "true" : "false",
      scenario.pvVoltageV,
      scenario.pvCurrentA,
      scenario.pvPowerW,
      scenario.irradianceWm2,
      scenario.ambientTemperatureC,
      scenario.relativeHumidityPercent,
      scenario.panelTemperatureC,
      output1Active ? "true" : "false",
      output2Active ? "true" : "false",
      output3Active ? "true" : "false",
      output1Active ? 1.62 : 0.0,
      output2Active ? 1.58 : 0.0,
      output3Active ? 1.60 : 0.0,
      trackingEnabled ? "true" : "false",
      scenario.trackingAngleDeg,
      scenario.trackingTargetAngleDeg,
      static_cast<unsigned int>(scenario.masterPositionRaw),
      static_cast<unsigned int>(scenario.slavePositionRaw),
      scenario.weatherIndex,
      scenario.demandIndex,
      nominalMode,
      nominalMode,
      operatingMode,
      scenarioManager.scenarioName(),
      scenarioManager.elapsedSimulatedSeconds(),
      batteryTimeScale,
      scenarioManager.isStaleDataRequested() ? "true" : "false",
      faultState,
      static_cast<unsigned long>(telemetryCounter)
  );

  if (written < 0) {
    Serial.println("Telemetry JSON formatting error.");
    return;
  }

  if (static_cast<size_t>(written) >= sizeof(payload)) {
    Serial.printf(
        "Telemetry payload too large. Required: %d bytes, available: %u bytes.\n",
        written + 1,
        static_cast<unsigned int>(sizeof(payload))
    );
    return;
  }

  const bool published = mqttClient.publish(
      TELEMETRY_TOPIC,
      reinterpret_cast<const uint8_t*>(payload),
      static_cast<unsigned int>(written),
      false
  );

  if (published) {
    Serial.printf(
        "Telemetry published to %s (%d bytes):\n%s\n",
        TELEMETRY_TOPIC,
        written,
        payload
    );
  } else {
    Serial.print("Telemetry publication failed. MQTT state: ");
    Serial.println(mqttClient.state());
  }
}

// ============================================================
// MQTT command reception and classification
// ============================================================

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.println();
  Serial.println("----------------------------------------");
  Serial.printf("MQTT message received on: %s\n", topic);

  Serial.print("Payload: ");
  for (unsigned int index = 0; index < length; index++) {
    Serial.write(payload[index]);
  }
  Serial.println();

  if (strcmp(topic, COMMAND_TOPIC) != 0) {
    Serial.println("Message ignored: unexpected MQTT topic.");
    return;
  }

  if (pendingAck.pending) {
    Serial.println("Command not processed: previous ACK is still pending.");
    return;
  }

#if ARDUINOJSON_VERSION_MAJOR >= 7
  JsonDocument commandDocument;
#else
  StaticJsonDocument<1024> commandDocument;
#endif

  const DeserializationError error =
      deserializeJson(commandDocument, payload, length);

  if (error) {
    Serial.print("Invalid command JSON: ");
    Serial.println(error.c_str());
    return;
  }

  const char* stationId = commandDocument["station_id"] | "";
  const char* commandId = commandDocument["command_id"] | "";
  const char* command = commandDocument["command"] | "";

  if (stationId[0] == '\0') {
    Serial.println("Command rejected: station_id is missing.");
    return;
  }

  if (strcmp(stationId, AWS_IOT_CLIENT_ID) != 0) {
    Serial.println("Command rejected: station_id does not match this device.");
    return;
  }

  if (commandId[0] == '\0') {
    Serial.println("Command rejected: command_id is missing.");
    return;
  }

  if (command[0] == '\0') {
    prepareCommandAck(
        commandId,
        "UNKNOWN",
        "invalid_command",
        "Command name is missing; no physical action executed."
    );
    return;
  }

  if (!isAllowedCommand(command)) {
    prepareCommandAck(
        commandId,
        command,
        "invalid_command",
        "Command is not in the local allowed-command list; no physical action executed."
    );
    return;
  }

  if (isCriticalSafetyActive() && !isSafetyReducingCommand(command)) {
    prepareCommandAck(
        commandId,
        command,
        "blocked_by_safety",
        "Command blocked by a local critical safety condition; no physical action executed."
    );
    return;
  }

  prepareCommandAck(
      commandId,
      command,
      "accepted",
      "Command accepted by local validation; no physical action executed."
  );
}

void prepareCommandAck(
    const char* commandId,
    const char* command,
    const char* status,
    const char* message
) {
  if (!copyText(
          pendingAck.commandId,
          sizeof(pendingAck.commandId),
          commandId) ||
      !copyText(
          pendingAck.command,
          sizeof(pendingAck.command),
          command) ||
      !copyText(
          pendingAck.status,
          sizeof(pendingAck.status),
          status) ||
      !copyText(
          pendingAck.message,
          sizeof(pendingAck.message),
          message) ||
      !copyText(
          pendingAck.resultingOperatingMode,
          sizeof(pendingAck.resultingOperatingMode),
          currentTestOperatingMode())) {
    Serial.println("Command ACK could not be prepared: a field is too long.");
    return;
  }

  pendingAck.pending = true;
  lastAckAttemptMs = millis() - ACK_RETRY_INTERVAL_MS;

  Serial.println("Command classified by local validation.");
  Serial.printf("Command ID: %s\n", pendingAck.commandId);
  Serial.printf("Command: %s\n", pendingAck.command);
  Serial.printf("ACK status: %s\n", pendingAck.status);
  Serial.println("Applied: false");
  Serial.println("No physical action was executed.");
  Serial.println("----------------------------------------");
}

// ============================================================
// Command acknowledgement
// ============================================================

void publishPendingCommandAck() {
  if (!pendingAck.pending || !mqttClient.connected()) {
    return;
  }

  char timestamp[25];

  if (!getUtcTimestamp(timestamp, sizeof(timestamp))) {
    Serial.println("ACK not published: UTC time is not synchronized.");
    return;
  }

#if ARDUINOJSON_VERSION_MAJOR >= 7
  JsonDocument ackDocument;
#else
  StaticJsonDocument<768> ackDocument;
#endif

  ackDocument["station_id"] = AWS_IOT_CLIENT_ID;
  ackDocument["timestamp"] = timestamp;
  ackDocument["message_type"] = "acks";
  ackDocument["command_id"] = pendingAck.commandId;
  ackDocument["command"] = pendingAck.command;
  ackDocument["status"] = pendingAck.status;
  ackDocument["applied"] = false;
  ackDocument["resulting_operating_mode"] =
      pendingAck.resultingOperatingMode;
  ackDocument["message"] = pendingAck.message;

  char ackPayload[768];
  const size_t requiredSize = measureJson(ackDocument);

  if (requiredSize + 1 > sizeof(ackPayload)) {
    Serial.printf(
        "ACK payload buffer is too small. Required: %u bytes, available: %u bytes.\n",
        static_cast<unsigned int>(requiredSize + 1),
        static_cast<unsigned int>(sizeof(ackPayload))
    );
    return;
  }

  const size_t written = serializeJson(
      ackDocument,
      ackPayload,
      sizeof(ackPayload)
  );

  if (written == 0) {
    Serial.println("ACK JSON serialization failed.");
    return;
  }

  const bool published = mqttClient.publish(
      ACK_TOPIC,
      reinterpret_cast<const uint8_t*>(ackPayload),
      static_cast<unsigned int>(written),
      false
  );

  if (!published) {
    Serial.print("ACK publication failed. MQTT state: ");
    Serial.println(mqttClient.state());
    return;
  }

  Serial.println();
  Serial.printf(
      "ACK published to %s (%u bytes):\n%s\n",
      ACK_TOPIC,
      static_cast<unsigned int>(written),
      ackPayload
  );

  pendingAck.pending = false;
  pendingAck.commandId[0] = '\0';
  pendingAck.command[0] = '\0';
  pendingAck.status[0] = '\0';
  pendingAck.message[0] = '\0';
  pendingAck.resultingOperatingMode[0] = '\0';
}