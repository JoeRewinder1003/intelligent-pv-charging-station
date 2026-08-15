#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <string.h>
#include <math.h>

#include "secrets.h"
#include "BatteryEmulator.h"
#include "BatteryHealthIndicator.h"
#include "BatteryCapacityTest.h"
#include "ActuatorEmulator.h"
#include "ActuatorSynchronizer.h"
#include "ActuatorStallMonitor.h"
#include "TrackingSafetyMonitor.h"
#include "MaintenanceController.h"
#include "ScenarioManager.h"
#include "PVSimulator.h"
#include "TrackingGeometry.h"
#include "TrackingController.h"
#include "SunSensorEmulator.h"
#include "ActuatorUsageMonitor.h"

/*
 * AWS IoT connectivity and local command-validation test.
 *
 * This firmware:
 * - Connects the ESP32 to Wi-Fi and AWS IoT Core.
 * - Synchronizes UTC time with NTP.
 * - Runs a functional station simulation through reproducible scenarios.
 * - Simulates photovoltaic generation with a reduced-order model.
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

static constexpr char BATTERY_DIAGNOSTICS_TOPIC[] =
    "station/station_001/battery_diagnostics";

static constexpr char ACTUATOR_DIAGNOSTICS_TOPIC[] =
    "station/station_001/actuator_diagnostics";

// ============================================================
// Timing and test configuration
// ============================================================

static constexpr uint16_t MQTT_PORT = 8883;
static constexpr unsigned long TELEMETRY_INTERVAL_MS = 15000;
static constexpr unsigned long BATTERY_UPDATE_INTERVAL_MS = 1000;
static constexpr unsigned long ACTUATOR_UPDATE_INTERVAL_MS = 100;
static constexpr unsigned long MQTT_RETRY_INTERVAL_MS = 5000;
static constexpr unsigned long ACK_RETRY_INTERVAL_MS = 2000;
static constexpr unsigned long ACTUATOR_DIAGNOSTICS_RETRY_INTERVAL_MS = 2000;
static constexpr time_t MIN_VALID_EPOCH = 1767225600;  // 2026-01-01 UTC
static constexpr time_t STALE_DATA_OFFSET_SECONDS = 60;

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
unsigned long lastActuatorUpdateMs = 0;
unsigned long lastMqttAttemptMs = 0;
unsigned long lastActuatorDiagnosticsAttemptMs = 0;
unsigned long lastAckAttemptMs = 0;
uint32_t telemetryCounter = 0;
uint32_t lastPublishedActuatorDiagnosticsSequence = 0;

BatteryConfig batteryConfig;
BatteryEmulator batteryEmulator(batteryConfig);
BatteryHealthIndicator batteryHealthIndicator;

BatteryCapacityTestPlant capacityTestPlant;
BatteryCapacityTestMonitor capacityTestMonitor;

ActuatorEmulator masterActuator;
ActuatorEmulator slaveActuator;
ActuatorUsageMonitor masterActuatorUsageMonitor;
ActuatorUsageMonitor slaveActuatorUsageMonitor;
TrackingSafetyMonitor trackingSafetyMonitor;
TrackingGeometry trackingGeometry;
SunSensorEmulator sunSensorEmulator;
TrackingController trackingController;
ActuatorSynchronizer actuatorSynchronizer;
ActuatorStallMonitor actuatorStallMonitor;
MaintenanceController maintenanceController;
ActuatorCommand requestedActuatorCommand =
    ActuatorCommand::STOP;

ScenarioManager scenarioManager;
PVConfig pvConfig;
PVSimulator pvSimulator(pvConfig);

bool manualBatteryPowerOverride = false;
float manualBatteryPowerW = 0.0f;
float batteryTimeScale = 1.0f;
constexpr float CAPACITY_TEST_TIME_SCALE = 600.0f;
BatteryHealthSample previousBatteryHealthSample;
uint8_t previousBatteryHealthOutputCount = 0;
bool previousBatteryHealthSampleAvailable = false;

/*
 * Simulated safety state used only for this validation test.
 * It does not drive any physical output.
 */
bool simulatedCriticalLockout = false;

/*
 * Cloud operating-mode override.
 *
 * Local safety always has higher priority.
 * When inactive, the functional simulation uses the
 * nominal operating mode defined by the active scenario.
 */
bool cloudOperatingModeActive = false;
char cloudOperatingMode[8] = "";

struct AppliedOutputState {
  bool output1Active = false;
  bool output2Active = false;
  bool output3Active = false;
  uint8_t activeCount = 0;
};

struct PendingCommandAck {
  bool pending = false;
  bool applied = false;
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
void printPvStatus();
void printScenarioStatus();
void printCapacityTestStatus();
const char* capacityTestStateText(BatteryCapacityTestState state);
void applyScenarioInitialConditions();
void updateBatteryEmulator();
void updateActuatorEmulators();
void printActuatorStatus();
void updateTrackingSafety();
void printTrackingSafetyStatus();
void printActuatorSynchronizationStatus();
void printActuatorStallStatus();
void printMaintenanceStatus();
void updateSunSensorEmulator();
void printSunSensorStatus();
void updateAutomaticTrackingController();
void printActuatorUsageStatus();

bool publishActuatorDiagnostics();
bool getUtcTimestamp(char* buffer, size_t bufferSize);
bool getTelemetryTimestamp(char* buffer, size_t bufferSize);
bool modeAllowsTracking(const char* operatingMode);
bool isAllowedCommand(const char* command);
bool isSafetyReducingCommand(const char* command);
bool applyCloudOperatingCommand(const char* command);
bool copyText(char* destination, size_t destinationSize, const char* source);
bool isCriticalSafetyActive();
bool batteryOutputsAllowed();
uint8_t maximumOutputCountForMode(const char* operatingMode);
AppliedOutputState calculateAppliedOutputState();
uint8_t currentActiveOutputCount();
float calculateAutomaticBatteryPowerW();
const char* currentTestOperatingMode();
const char* currentFaultState();
const char* trackingSafetyFaultToString(TrackingSafetyFault fault);

void publishTelemetry();
void publishPendingCommandAck();
void publishBatteryResistanceStep(
    const BatteryHealthEvent& event
);

void publishBatteryCapacityTest(
    const BatteryCapacityTestResult& result,
    float finalVoltageV
);

void prepareCommandAck(
    const char* commandId,
    const char* command,
    const char* status,
    const char* message,
    bool applied
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
  pvSimulator.begin();
  scenarioManager.begin(ScenarioType::CLEAR_DAY);
  applyScenarioInitialConditions();
  lastBatteryUpdateMs = millis();
  printScenarioStatus();
  printPvStatus();
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

  masterActuatorUsageMonitor.reset(
      masterActuator.state().positionMm
  );

  slaveActuatorUsageMonitor.reset(
      slaveActuator.state().positionMm
  );

  lastActuatorUpdateMs = millis();
}

// ============================================================
// Main loop
// ============================================================

void loop() {
  processSerialCommands();
  updateBatteryEmulator();

  updateSunSensorEmulator();
  updateAutomaticTrackingController();
  updateActuatorEmulators();
  updateTrackingSafety();

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
    const ActuatorUsageState& masterUsage =
        masterActuatorUsageMonitor.state();
    const ActuatorUsageState& slaveUsage =
        slaveActuatorUsageMonitor.state();

    const uint32_t masterSequence =
        masterUsage.dutyCycleWindowSequence;

    const uint32_t slaveSequence =
        slaveUsage.dutyCycleWindowSequence;

    if (masterSequence == slaveSequence &&
        masterSequence >
            lastPublishedActuatorDiagnosticsSequence &&
        now - lastActuatorDiagnosticsAttemptMs >=
            ACTUATOR_DIAGNOSTICS_RETRY_INTERVAL_MS) {

      lastActuatorDiagnosticsAttemptMs = now;

      if (publishActuatorDiagnostics()) {
        lastPublishedActuatorDiagnosticsSequence =
            masterSequence;
      }
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

bool getTelemetryTimestamp(char* buffer, size_t bufferSize) {
  if (buffer == nullptr || bufferSize == 0) {
    return false;
  }

  time_t telemetryTime = time(nullptr);

  if (telemetryTime < MIN_VALID_EPOCH) {
    return false;
  }

  // STALE_DATA injects an old measurement timestamp without changing the
  // ESP32 NTP-synchronized system clock. This is a functional test input.
  if (scenarioManager.isStaleDataRequested()) {
    telemetryTime -= STALE_DATA_OFFSET_SECONDS;
  }

  struct tm utcTime;

  if (gmtime_r(&telemetryTime, &utcTime) == nullptr) {
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

  if (serialCommand ==
      "ACTUATOR DIAGNOSTICS PUBLISH") {

    lastActuatorDiagnosticsAttemptMs = millis();

    const uint32_t masterSequence =
        masterActuatorUsageMonitor.state()
            .dutyCycleWindowSequence;

    const uint32_t slaveSequence =
        slaveActuatorUsageMonitor.state()
            .dutyCycleWindowSequence;

    if (publishActuatorDiagnostics()) {
      if (masterSequence == slaveSequence &&
          masterSequence >
              lastPublishedActuatorDiagnosticsSequence) {

        lastPublishedActuatorDiagnosticsSequence =
            masterSequence;
      }
    }

    return;
  }

  if (serialCommand == "ACTUATOR EXTEND") {
    if (!maintenanceController.maintenanceModeActive()) {
      Serial.println(
        "Manual actuator control requires maintenance mode."
      );
      return;
    }

    if (maintenanceController.state().actuatorMovementLocked) {
      Serial.println(
        "Actuator movement blocked by technician lock."
      );
      return;
    }
    if (!trackingSafetyMonitor.movementAllowed()) {
      Serial.println(
          "Actuator movement blocked by tracking safety interlock."
      );
      return;
    }

    requestedActuatorCommand =
      ActuatorCommand::EXTEND;

    Serial.println("Actuators commanded to EXTEND.");
    return;
  }

  if (serialCommand == "ACTUATOR RETRACT") {
    if (!maintenanceController.maintenanceModeActive()) {
      Serial.println(
        "Manual actuator control requires maintenance mode."
      );
      return;
    }

    if (maintenanceController.state().actuatorMovementLocked) {
      Serial.println(
        "Actuator movement blocked by technician lock."
      );
      return;
    }
    if (!trackingSafetyMonitor.movementAllowed()) {
      Serial.println(
          "Actuator movement blocked by tracking safety interlock."
      );
      return;
    }

    requestedActuatorCommand =
      ActuatorCommand::RETRACT;

    Serial.println("Actuators commanded to RETRACT.");
    return;
  }

  if (serialCommand == "ACTUATOR STOP") {
    requestedActuatorCommand =
      ActuatorCommand::STOP;

    Serial.println("Actuators commanded to STOP.");
    return;
  }

  if (serialCommand == "ACTUATOR STATUS") {
    printActuatorStatus();
    return;
  }

  if (serialCommand == "ACTUATOR TEST DESYNC") {
    requestedActuatorCommand = ActuatorCommand::STOP;

    masterActuator.reset(60.0f);
    slaveActuator.reset(45.0f);

    actuatorSynchronizer.reset();

    Serial.println(
        "Actuator desynchronization test prepared: "
        "master = 60 mm, slave = 45 mm."
    );

    printActuatorStatus();
    return;
  }

  if (serialCommand == "ACTUATOR SYNC STATUS") {
    printActuatorSynchronizationStatus();
    return;
  }

  if (serialCommand == "ACTUATOR STALL STATUS") {
    printActuatorStallStatus();
    return;
  }

  if (serialCommand == "ACTUATOR TEST STALL SLAVE") {
    requestedActuatorCommand = ActuatorCommand::STOP;

    masterActuator.reset(50.0f);
    slaveActuator.reset(50.0f);

    masterActuator.setStalled(false);
    slaveActuator.setStalled(true);

    actuatorSynchronizer.reset();
    actuatorStallMonitor.reset();

    Serial.println(
      "Actuator stall test prepared: slave stalled at 50 mm."
    );

    printActuatorStatus();
    return;
  }

  if (serialCommand == "ACTUATOR USAGE STATUS") {
    printActuatorUsageStatus();
    return;
  }

  if (serialCommand == "TRACKING SAFETY STATUS") {
    printTrackingSafetyStatus();
    return;
  }

  if (serialCommand == "TRACKING SAFETY RESET") {
    if (!maintenanceController.maintenanceModeActive()) {
      Serial.println(
          "Tracking safety reset requires maintenance mode."
      );
      return;
    }

    requestedActuatorCommand = ActuatorCommand::STOP;

    masterActuator.setCommand(
        ActuatorCommand::STOP
    );

    slaveActuator.setCommand(
        ActuatorCommand::STOP
    );

    const ActuatorState& masterState =
        masterActuator.state();

    const ActuatorState& slaveState =
        slaveActuator.state();

    actuatorSynchronizer.reset();
    actuatorStallMonitor.reset();

    const bool resetSuccessful =
        trackingSafetyMonitor.resetIfSafe(
            masterState.positionMm,
            true,
            slaveState.positionMm,
            true
        );

    if (resetSuccessful) {
      Serial.println(
          "Tracking safety fault reset by technician."
      );
    } else {
      Serial.println(
          "Tracking safety reset rejected: "
          "actuator feedback is not safe."
      );
    }

    printTrackingSafetyStatus();
    return;
  }

  if (serialCommand == "TRACKING SAFETY TEST MISMATCH") {
    const ActuatorState& masterState = masterActuator.state();
    const ActuatorState& slaveState = slaveActuator.state();

    trackingSafetyMonitor.update(
        masterState.positionMm,
        true,
        slaveState.positionMm + 15.0f,
        true
    );

    Serial.println(
        "Tracking safety mismatch test injected: +15 mm on slave reading."
    );

    printTrackingSafetyStatus();
    return;
  }

  if (serialCommand == "TRACKING GEOMETRY STATUS") {
    const ActuatorState& masterState =
        masterActuator.state();

    const ActuatorState& slaveState =
        slaveActuator.state();

    const float averagePositionMm =
        (masterState.positionMm +
        slaveState.positionMm) / 2.0f;

    const float panelAngleDeg =
        trackingGeometry.positionToAngleDeg(
            averagePositionMm
        );

    Serial.println();
    Serial.println("Tracking geometry status:");

    Serial.printf(
        "  Master position: %.2f mm\n",
        masterState.positionMm
    );

    Serial.printf(
        "  Slave position: %.2f mm\n",
        slaveState.positionMm
    );

    Serial.printf(
        "  Average position: %.2f mm\n",
        averagePositionMm
    );

    if (isfinite(panelAngleDeg)) {
      Serial.printf(
          "  Panel angle: %.2f deg\n",
          panelAngleDeg
      );
    } else {
      Serial.println(
          "  Panel angle: INVALID"
      );
    }

    Serial.println();
    return;
  }

  if (serialCommand == "TRACKING GEOMETRY TEST") {
    const float positionsMm[] = {
        0.0f,
        150.0f,
        300.0f
    };

    Serial.println();
    Serial.println("Tracking geometry test:");

    for (const float positionMm : positionsMm) {
      const float angleDeg =
          trackingGeometry.positionToAngleDeg(
              positionMm
          );

      Serial.printf(
          "  x = %.1f mm -> alpha = %.2f deg\n",
          positionMm,
          angleDeg
      );
    }

    Serial.println();
    return;
  }

  if (serialCommand == "TRACKING CONTROLLER TEST") {
    Serial.println();
    Serial.println("Tracking controller test:");

    trackingController.reset();

    trackingController.update(10.0f, true);

    Serial.println("  Case 1 - Positive error:");
    Serial.printf(
        "    Angle Y: %.2f deg\n",
        trackingController.state().angleYDeg
    );
    Serial.printf(
        "    Command: %s\n",
        trackingController.state().command ==
                ActuatorCommand::EXTEND
            ? "EXTEND"
            : trackingController.state().command ==
                      ActuatorCommand::RETRACT
                  ? "RETRACT"
                  : "STOP"
    );

    trackingController.update(0.3f, true);

    Serial.println("  Case 2 - Inside stop threshold:");
    Serial.printf(
        "    Angle Y: %.2f deg\n",
        trackingController.state().angleYDeg
    );
    Serial.printf(
        "    Command: %s\n",
        trackingController.state().command ==
                ActuatorCommand::EXTEND
            ? "EXTEND"
            : trackingController.state().command ==
                      ActuatorCommand::RETRACT
                  ? "RETRACT"
                  : "STOP"
    );

    trackingController.update(-10.0f, true);

    Serial.println("  Case 3 - Negative error:");
    Serial.printf(
        "    Angle Y: %.2f deg\n",
        trackingController.state().angleYDeg
    );
    Serial.printf(
        "    Command: %s\n",
        trackingController.state().command ==
                ActuatorCommand::EXTEND
            ? "EXTEND"
            : trackingController.state().command ==
                      ActuatorCommand::RETRACT
                  ? "RETRACT"
                  : "STOP"
    );

    trackingController.update(0.0f, false);

    Serial.println("  Case 4 - Invalid measurement:");
    Serial.printf(
        "    Command: %s\n",
        trackingController.state().command ==
                ActuatorCommand::EXTEND
            ? "EXTEND"
            : trackingController.state().command ==
                      ActuatorCommand::RETRACT
                  ? "RETRACT"
                  : "STOP"
    );

    Serial.println();
    return;
  }

  if (serialCommand == "SUN SENSOR TEST") {
    Serial.println();
    Serial.println("Sun sensor emulator test:");

    // Case 1: sufficient radiation and Sun inside FOV.
    sunSensorEmulator.update(
        500.0f,
        30.0f,
        25.0f
    );

    const SunSensorState validState =
        sunSensorEmulator.state();

    Serial.println("  Case 1 - Valid measurement:");
    Serial.printf(
        "    Radiation: %.1f W/m2\n",
        validState.radiationWm2
    );
    Serial.printf(
        "    Angle Y: %.2f deg\n",
        validState.angleYDeg
    );
    Serial.printf(
        "    Radiation enough: %s\n",
        validState.radiationEnough ? "true" : "false"
    );
    Serial.printf(
        "    Sun in FOV: %s\n",
        validState.sunInFieldOfView ? "true" : "false"
    );
    Serial.printf(
        "    Measurement valid: %s\n",
        validState.measurementValid ? "true" : "false"
    );

    // Case 2: insufficient radiation.
    sunSensorEmulator.update(
        250.0f,
        30.0f,
        25.0f
    );

    const SunSensorState lowRadiationState =
        sunSensorEmulator.state();

    Serial.println("  Case 2 - Insufficient radiation:");
    Serial.printf(
        "    Radiation: %.1f W/m2\n",
        lowRadiationState.radiationWm2
    );
    Serial.printf(
        "    Angle Y: %.2f deg\n",
        lowRadiationState.angleYDeg
    );
    Serial.printf(
        "    Radiation enough: %s\n",
        lowRadiationState.radiationEnough ? "true" : "false"
    );
    Serial.printf(
        "    Measurement valid: %s\n",
        lowRadiationState.measurementValid ? "true" : "false"
    );

    // Case 3: sufficient radiation but Sun outside FOV.
    sunSensorEmulator.update(
        500.0f,
        90.0f,
        20.0f
    );

    const SunSensorState outOfFovState =
        sunSensorEmulator.state();

    Serial.println("  Case 3 - Sun outside FOV:");
    Serial.printf(
        "    Radiation: %.1f W/m2\n",
        outOfFovState.radiationWm2
    );
    Serial.printf(
        "    Angle Y: %.2f deg\n",
        outOfFovState.angleYDeg
    );
    Serial.printf(
        "    Sun in FOV: %s\n",
        outOfFovState.sunInFieldOfView ? "true" : "false"
    );
    Serial.printf(
        "    Measurement valid: %s\n",
        outOfFovState.measurementValid ? "true" : "false"
    );

    Serial.println();
    return;
  }

  if (serialCommand == "SUN SENSOR STATUS") {
    printSunSensorStatus();
    return;
  }

  if (serialCommand == "MAINTENANCE ENTER") {
    requestedActuatorCommand = ActuatorCommand::STOP;
    maintenanceController.enterMaintenance();

    Serial.println("Maintenance mode entered.");
    printMaintenanceStatus();
    return;
  }

  if (serialCommand == "MAINTENANCE EXIT") {
    requestedActuatorCommand = ActuatorCommand::STOP;
    maintenanceController.exitMaintenance();

    Serial.println("Maintenance mode exited.");
    printMaintenanceStatus();
    return;
  }

  if (serialCommand == "MAINTENANCE STATUS") {
    printMaintenanceStatus();
    return;
  }

  if (serialCommand == "MAINTENANCE LOCK ACTUATORS") {
    if (!maintenanceController.maintenanceModeActive()) {
      Serial.println(
        "Actuator lock control requires maintenance mode."
      );
      return;
    }

    requestedActuatorCommand = ActuatorCommand::STOP;
    maintenanceController.lockActuatorMovement();

    Serial.println(
      "Actuator movement locked by technician."
    );

    printMaintenanceStatus();
    return;
  }

  if (serialCommand == "MAINTENANCE UNLOCK ACTUATORS") {
    if (!maintenanceController.maintenanceModeActive()) {
      Serial.println(
        "Actuator lock control requires maintenance mode."
      );
      return;
    }

    maintenanceController.unlockActuatorMovement();

    Serial.println(
      "Actuator movement unlocked by technician."
    );

    printMaintenanceStatus();
    return;
  }

  if (serialCommand == "MAINTENANCE LOCK OUTPUT 1") {
    if (!maintenanceController.maintenanceModeActive()) {
      Serial.println(
        "Output lock control requires maintenance mode."
      );
      return;
    }

    maintenanceController.lockOutput(1);

    Serial.println(
      "Output 1 locked by technician."
    );

    printMaintenanceStatus();
    return;
  }

  if (serialCommand == "MAINTENANCE UNLOCK OUTPUT 1") {
    if (!maintenanceController.maintenanceModeActive()) {
      Serial.println(
        "Output lock control requires maintenance mode."
      );
      return;
    }

    maintenanceController.unlockOutput(1);

    Serial.println(
      "Output 1 unlocked by technician."
    );

    printMaintenanceStatus();
    return;
  }

  if (serialCommand == "MAINTENANCE LOCK OUTPUT 2") {
    if (!maintenanceController.maintenanceModeActive()) {
      Serial.println(
        "Output lock control requires maintenance mode."
      );
      return;
    }

    maintenanceController.lockOutput(2);

    Serial.println(
      "Output 2 locked by technician."
    );

    printMaintenanceStatus();
    return;
  }

  if (serialCommand == "MAINTENANCE UNLOCK OUTPUT 2") {
    if (!maintenanceController.maintenanceModeActive()) {
      Serial.println(
        "Output lock control requires maintenance mode."
      );
      return;
    }

    maintenanceController.unlockOutput(2);

    Serial.println(
      "Output 2 unlocked by technician."
    );

    printMaintenanceStatus();
    return;
  }

  if (serialCommand == "MAINTENANCE LOCK OUTPUT 3") {
    if (!maintenanceController.maintenanceModeActive()) {
      Serial.println(
        "Output lock control requires maintenance mode."
      );
      return;
    }

    maintenanceController.lockOutput(3);

    Serial.println(
      "Output 3 locked by technician."
    );

    printMaintenanceStatus();
    return;
  }

  if (serialCommand == "MAINTENANCE UNLOCK OUTPUT 3") {
    if (!maintenanceController.maintenanceModeActive()) {
      Serial.println(
        "Output lock control requires maintenance mode."
      );
      return;
    }

    maintenanceController.unlockOutput(3);

    Serial.println(
      "Output 3 unlocked by technician."
    );

    printMaintenanceStatus();
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
    printPvStatus();
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
    printPvStatus();
    printBatteryStatus();
    return;
  }

  if (serialCommand == "HELP") {
    printSerialHelp();
    return;
  }

  if (serialCommand == "PV STATUS") {
    printPvStatus();
    return;
  }

  if (serialCommand == "BATTERY STATUS") {
    printBatteryStatus();
    return;
  }

  if (serialCommand == "CAPACITY TEST START") {
    capacityTestPlant.start();
    capacityTestMonitor.start();

    Serial.println("Battery capacity test started.");
    printCapacityTestStatus();
    return;
  }

  if (serialCommand == "CAPACITY TEST STATUS") {
    printCapacityTestStatus();
    return;
  }

  if (serialCommand == "CAPACITY TEST ABORT") {
    capacityTestPlant.abort();
    capacityTestMonitor.abort();

    Serial.println("Battery capacity test aborted.");
    printCapacityTestStatus();
    return;
  }

  if (serialCommand.startsWith("CAPACITY TEST GROUND TRUTH ")) {
    const float requestedRetention =
        serialCommand.substring(
            strlen("CAPACITY TEST GROUND TRUTH ")
        ).toFloat();

    if (!isfinite(requestedRetention) ||
        requestedRetention <= 0.0f ||
        requestedRetention > 100.0f) {
      Serial.println(
          "Invalid capacity-test ground truth. Allowed range: >0 to 100 percent."
      );
      return;
    }

    if (!capacityTestPlant.setCapacityRetentionPercent(
            requestedRetention)) {
      Serial.println(
          "Cannot change capacity-test ground truth while the test is running."
      );
      return;
    }

    Serial.printf(
        "Capacity-test simulation ground truth set to %.1f%%.\n",
        requestedRetention
    );
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
    Serial.printf(
        "Battery emulation time scale set to x%.1f.\n",
        batteryTimeScale
    );
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
  Serial.println("  CAPACITY TEST START");
  Serial.println("  CAPACITY TEST STATUS");
  Serial.println("  CAPACITY TEST ABORT");
  Serial.println("  CAPACITY TEST GROUND TRUTH <percent>");
  Serial.println("  MAINTENANCE ENTER");
  Serial.println("  MAINTENANCE EXIT");
  Serial.println("  MAINTENANCE STATUS");
  Serial.println("  MAINTENANCE LOCK ACTUATORS");
  Serial.println("  MAINTENANCE UNLOCK ACTUATORS");
  Serial.println("  MAINTENANCE LOCK OUTPUT 1");
  Serial.println("  MAINTENANCE UNLOCK OUTPUT 1");
  Serial.println("  MAINTENANCE LOCK OUTPUT 2");
  Serial.println("  MAINTENANCE UNLOCK OUTPUT 2");
  Serial.println("  MAINTENANCE LOCK OUTPUT 3");
  Serial.println("  MAINTENANCE UNLOCK OUTPUT 3");
  Serial.println("  SAFETY ON");
  Serial.println("  SAFETY OFF");
  Serial.println("  PV STATUS");
  Serial.println("  BATTERY STATUS");
  Serial.println("  BATTERY AUTO");
  Serial.println("  BATTERY POWER <watts>");
  Serial.println("  BATTERY SOC <0-100>");
  Serial.println("  BATTERY SCALE <1-3600>");
  Serial.println("  ACTUATOR EXTEND");
  Serial.println("  ACTUATOR RETRACT");
  Serial.println("  ACTUATOR STOP");
  Serial.println("  ACTUATOR STATUS");
  Serial.println("  ACTUATOR TEST DESYNC");
  Serial.println("  ACTUATOR SYNC STATUS");
  Serial.println("  ACTUATOR STALL STATUS");
  Serial.println("  ACTUATOR TEST STALL SLAVE");
  Serial.println("  ACTUATOR USAGE STATUS");
  Serial.println("  ACTUATOR DIAGNOSTICS PUBLISH");
  Serial.println("  TRACKING SAFETY STATUS");
  Serial.println("  TRACKING SAFETY TEST MISMATCH");
  Serial.println("  TRACKING SAFETY RESET");
  Serial.println("  TRACKING GEOMETRY STATUS");
  Serial.println("  TRACKING GEOMETRY TEST");
  Serial.println("  TRACKING CONTROLLER TEST");
  Serial.println("  SUN SENSOR TEST");
  Serial.println("  SUN SENSOR STATUS");
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
  Serial.printf(
    "  Effective capacity: %.1f Ah\n",
    batteryEmulator.effectiveCapacityAh()
  );
  Serial.printf(
    "  Capacity retention: %.1f%%\n",
    batteryEmulator.config().emulatedCapacityRetentionPercent
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

void printActuatorStatus() {
  const ActuatorState& masterState = masterActuator.state();
  const ActuatorState& slaveState = slaveActuator.state();

  Serial.println();
  Serial.println("Actuator emulator status:");

  Serial.printf(
      "  Master position: %.2f mm\n",
      masterState.positionMm
  );

  Serial.printf(
      "  Master moving: %s\n",
      masterState.moving ? "true" : "false"
  );

  Serial.printf(
      "  Slave position: %.2f mm\n",
      slaveState.positionMm
  );

  Serial.printf(
      "  Slave moving: %s\n",
      slaveState.moving ? "true" : "false"
  );

  Serial.println();
}

void printTrackingSafetyStatus() {
  const TrackingSafetyState& safetyState =
      trackingSafetyMonitor.state();

  Serial.println();
  Serial.println("Tracking safety status:");

  Serial.printf(
      "  Fault latched: %s\n",
      safetyState.faultLatched ? "true" : "false"
  );

  Serial.printf(
    "  Fault: %s\n",
    trackingSafetyFaultToString(safetyState.fault)
  );

  Serial.printf(
      "  Position difference: %.2f mm\n",
      safetyState.positionDifferenceMm
  );

  Serial.printf(
      "  Movement allowed: %s\n",
      trackingSafetyMonitor.movementAllowed()
          ? "true"
          : "false"
  );

  Serial.println();
}

void printPvStatus() {
  const PVState& pv = pvSimulator.state();

  Serial.println();
  Serial.println("PV simulator status:");
  Serial.printf("  Rated array power: %.1f W\n", pvSimulator.ratedArrayPowerW());
  Serial.printf("  Irradiance: %.1f W/m2\n", pv.irradianceWm2);
  Serial.printf("  Panel temperature: %.1f degC\n", pv.panelTemperatureC);
  Serial.printf(
      "  Available panels: %u of %u\n",
      static_cast<unsigned int>(pv.availablePanelCount),
      static_cast<unsigned int>(pvSimulator.config().panelCount)
  );
  Serial.printf("  Availability factor: %.3f\n", pv.availabilityFactor);
  Serial.printf("  Temperature factor: %.3f\n", pv.temperaturePowerFactor);
  Serial.printf("  Array voltage: %.3f V\n", pv.voltageV);
  Serial.printf("  Array current: %.3f A\n", pv.totalCurrentA);
  Serial.printf(
      "  Panel currents: %.3f A, %.3f A, %.3f A\n",
      pv.panelCurrentA[0],
      pv.panelCurrentA[1],
      pv.panelCurrentA[2]
  );
  Serial.printf("  Raw PV power: %.3f W\n", pv.rawPowerW);
  Serial.printf("  Delivered PV power: %.3f W\n", pv.deliveredPowerW);
  Serial.printf(
      "  Flags: generation_available=%s, input_limited=%s\n",
      pv.generationAvailable ? "true" : "false",
      pv.inputLimited ? "true" : "false"
  );
  Serial.println();
}

const char* capacityTestStateText(BatteryCapacityTestState state) {
  switch (state) {
    case BatteryCapacityTestState::IDLE:
      return "IDLE";

    case BatteryCapacityTestState::RUNNING:
      return "RUNNING";

    case BatteryCapacityTestState::COMPLETED:
      return "COMPLETED";

    case BatteryCapacityTestState::ABORTED:
      return "ABORTED";

    default:
      return "UNKNOWN";
  }
}

const char* trackingSafetyFaultToString(
  TrackingSafetyFault fault
) {
  switch (fault) {
    case TrackingSafetyFault::NONE:
      return "NONE";

    case TrackingSafetyFault::MASTER_POSITION_INVALID:
      return "MASTER_POSITION_INVALID";

    case TrackingSafetyFault::SLAVE_POSITION_INVALID:
      return "SLAVE_POSITION_INVALID";

    case TrackingSafetyFault::MASTER_POSITION_OUT_OF_RANGE:
      return "MASTER_POSITION_OUT_OF_RANGE";

    case TrackingSafetyFault::SLAVE_POSITION_OUT_OF_RANGE:
      return "SLAVE_POSITION_OUT_OF_RANGE";

    case TrackingSafetyFault::POSITION_MISMATCH:
      return "POSITION_MISMATCH";

    case TrackingSafetyFault::MASTER_ACTUATOR_STALL:
      return "MASTER_ACTUATOR_STALL";

    case TrackingSafetyFault::SLAVE_ACTUATOR_STALL:
      return "SLAVE_ACTUATOR_STALL";

    default:
      return "UNKNOWN";
  }
}

void printCapacityTestStatus() {
  const BatteryCapacityTestSample& sample =
      capacityTestPlant.sample();

  const BatteryCapacityTestResult& result =
      capacityTestMonitor.result();

  Serial.println();
  Serial.println("Battery capacity test:");

  Serial.printf(
      "  Simulation ground truth: %.1f%%\n",
      capacityTestPlant.capacityRetentionPercent()
  );

  Serial.printf(
      "  Plant state: %s\n",
      capacityTestStateText(capacityTestPlant.state())
  );

  Serial.printf(
      "  Monitor state: %s\n",
      capacityTestStateText(capacityTestMonitor.state())
  );

  Serial.printf(
      "  Test voltage: %.3f V\n",
      sample.voltageV
  );

  Serial.printf(
      "  Test current: %.3f A\n",
      sample.currentA
  );

  Serial.printf(
      "  Measured capacity: %.3f Ah\n",
      result.measuredCapacityAh
  );

  Serial.printf(
      "  Elapsed simulated time: %.1f s\n",
      result.elapsedSimulatedSeconds
  );

  Serial.println();
}

void printActuatorSynchronizationStatus() {
  const ActuatorSynchronizationState& syncState =
      actuatorSynchronizer.state();

  Serial.println();
  Serial.println("Actuator synchronization status:");

  Serial.printf(
      "  Correction active: %s\n",
      syncState.correctionActive ? "true" : "false"
  );

  Serial.printf(
      "  Position difference: %.2f mm\n",
      syncState.positionDifferenceMm
  );

  const char* masterCommandText = "STOP";
  const char* slaveCommandText = "STOP";

  if (syncState.masterCommand == ActuatorCommand::EXTEND) {
    masterCommandText = "EXTEND";
  } else if (
      syncState.masterCommand == ActuatorCommand::RETRACT
  ) {
    masterCommandText = "RETRACT";
  }

  if (syncState.slaveCommand == ActuatorCommand::EXTEND) {
    slaveCommandText = "EXTEND";
  } else if (
      syncState.slaveCommand == ActuatorCommand::RETRACT
  ) {
    slaveCommandText = "RETRACT";
  }

  Serial.printf(
      "  Master command: %s\n",
      masterCommandText
  );

  Serial.printf(
      "  Slave command: %s\n",
      slaveCommandText
  );

  Serial.println();
}

void printActuatorStallStatus() {
  const ActuatorStallState& stallState =
      actuatorStallMonitor.state();

  Serial.println();
  Serial.println("Actuator stall status:");

  Serial.printf(
      "  Master stall detected: %s\n",
      stallState.masterStallDetected ? "true" : "false"
  );

  Serial.printf(
      "  Master observed change: %.2f mm\n",
      stallState.masterObservedChangeMm
  );

  Serial.printf(
      "  Slave stall detected: %s\n",
      stallState.slaveStallDetected ? "true" : "false"
  );

  Serial.printf(
      "  Slave observed change: %.2f mm\n",
      stallState.slaveObservedChangeMm
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
  Serial.printf("  Irradiance input: %.1f W/m2\n", profile.irradianceWm2);
  Serial.printf(
      "  Available panels: %u of %u\n",
      static_cast<unsigned int>(profile.availablePanelCount),
      static_cast<unsigned int>(pvSimulator.config().panelCount)
  );
  Serial.printf(
      "  PV availability factor: %.2f\n",
      profile.pvAvailabilityFactor
  );
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

void printMaintenanceStatus() {
  const MaintenanceState& maintenanceState =
    maintenanceController.state();

  Serial.println();
  Serial.println("Maintenance status:");

  Serial.printf(
    "  Control mode: %s\n",
    maintenanceState.controlMode == StationControlMode::MAINTENANCE
      ? "MAINTENANCE"
      : "AUTOMATIC"
  );

  Serial.printf(
    "  Actuator movement locked: %s\n",
    maintenanceState.actuatorMovementLocked ? "true" : "false"
  );

  Serial.printf(
    "  Output 1 locked: %s\n",
    maintenanceState.output1Locked ? "true" : "false"
  );

  Serial.printf(
    "  Output 2 locked: %s\n",
    maintenanceState.output2Locked ? "true" : "false"
  );

  Serial.printf(
    "  Output 3 locked: %s\n",
    maintenanceState.output3Locked ? "true" : "false"
  );

  Serial.printf(
    "  Manual actuator control allowed: %s\n",
    maintenanceController.manualActuatorControlAllowed()
      ? "true"
      : "false"
  );

  Serial.printf(
    "  Automatic tracking allowed: %s\n",
    maintenanceController.automaticTrackingAllowed()
      ? "true"
      : "false"
  );

  Serial.println();
}

void printActuatorUsageStatus() {
  const ActuatorUsageState& masterUsage =
      masterActuatorUsageMonitor.state();

  const ActuatorUsageState& slaveUsage =
      slaveActuatorUsageMonitor.state();

  Serial.println();
  Serial.println("Actuator usage status:");

  Serial.println("  Master actuator:");
  Serial.printf(
      "    Operating time: %.2f s\n",
      masterUsage.operatingTimeSeconds
  );
  Serial.printf(
      "    Total travel: %.2f mm\n",
      masterUsage.totalTravelMm
  );
  Serial.printf(
      "    Movement starts: %lu\n",
      static_cast<unsigned long>(
          masterUsage.movementStarts
      )
  );
  Serial.printf(
      "    Equivalent full-stroke cycles: %.4f\n",
      masterUsage.equivalentFullStrokeCycles
  );
  Serial.printf(
      "    Currently moving: %s\n",
      masterUsage.currentlyMoving
          ? "true"
          : "false"
  );

  Serial.printf(
      "    Last duty cycle: %.2f %%\n",
      masterUsage.lastDutyCyclePercent
  );

  Serial.printf(
      "    Duty cycle window available: %s\n",
      masterUsage.dutyCycleWindowAvailable
          ? "true"
          : "false"
  );

  Serial.printf(
      "    Duty cycle exceeded: %s\n",
      masterUsage.dutyCycleExceeded
          ? "true"
          : "false"
  );

  Serial.println("  Slave actuator:");
  Serial.printf(
      "    Operating time: %.2f s\n",
      slaveUsage.operatingTimeSeconds
  );
  Serial.printf(
      "    Total travel: %.2f mm\n",
      slaveUsage.totalTravelMm
  );
  Serial.printf(
      "    Movement starts: %lu\n",
      static_cast<unsigned long>(
          slaveUsage.movementStarts
      )
  );
  Serial.printf(
      "    Equivalent full-stroke cycles: %.4f\n",
      slaveUsage.equivalentFullStrokeCycles
  );
  Serial.printf(
      "    Currently moving: %s\n",
      slaveUsage.currentlyMoving
          ? "true"
          : "false"
  );

  Serial.printf(
      "    Last duty cycle: %.2f %%\n",
      slaveUsage.lastDutyCyclePercent
  );

  Serial.printf(
      "    Duty cycle window available: %s\n",
      slaveUsage.dutyCycleWindowAvailable
          ? "true"
          : "false"
  );

  Serial.printf(
      "    Duty cycle exceeded: %s\n",
      slaveUsage.dutyCycleExceeded
          ? "true"
          : "false"
  );

  Serial.println();
}

void printSunSensorStatus() {
  const SunSensorState& sensorState =
      sunSensorEmulator.state();

  Serial.println();
  Serial.println("Sun sensor emulator status:");

  Serial.printf(
      "  Radiation: %.1f W/m2\n",
      sensorState.radiationWm2
  );

  Serial.printf(
      "  Angle Y: %.2f deg\n",
      sensorState.angleYDeg
  );

  Serial.printf(
      "  Radiation enough: %s\n",
      sensorState.radiationEnough
          ? "true"
          : "false"
  );

  Serial.printf(
      "  Sun in FOV: %s\n",
      sensorState.sunInFieldOfView
          ? "true"
          : "false"
  );

  Serial.printf(
      "  Measurement valid: %s\n",
      sensorState.measurementValid
          ? "true"
          : "false"
  );

  Serial.println();
}

void applyScenarioInitialConditions() {
  const ScenarioProfile& scenario = scenarioManager.profile();

  batteryEmulator.setSocPercent(scenario.initialSocPercent);
  pvSimulator.update(
      scenario.irradianceWm2,
      scenario.panelTemperatureC,
      scenario.availablePanelCount,
      scenario.pvAvailabilityFactor
  );
  manualBatteryPowerOverride = false;

  batteryHealthIndicator.reset();
  previousBatteryHealthSampleAvailable = false;
}

void updateBatteryEmulator() {
  const unsigned long now = millis();
  const unsigned long elapsedMs = now - lastBatteryUpdateMs;

  if (elapsedMs < BATTERY_UPDATE_INTERVAL_MS) {
    return;
  }

  lastBatteryUpdateMs = now;

  const float realDeltaTimeSeconds =
      static_cast<float>(elapsedMs) / 1000.0f;

  const float simulatedDeltaTimeSeconds =
      realDeltaTimeSeconds * batteryTimeScale;

  const float capacityTestDeltaTimeSeconds =
      realDeltaTimeSeconds * CAPACITY_TEST_TIME_SCALE;

  // Run the independent battery capacity-test simulation when active.
  if (capacityTestPlant.state() == BatteryCapacityTestState::RUNNING &&
      capacityTestMonitor.state() == BatteryCapacityTestState::RUNNING) {

    capacityTestPlant.update(capacityTestDeltaTimeSeconds);

    const BatteryCapacityTestSample& capacitySample =
        capacityTestPlant.sample();

    capacityTestMonitor.update(
        capacitySample.voltageV,
        capacitySample.currentA,
        capacityTestDeltaTimeSeconds
    );

    if (capacityTestMonitor.state() == BatteryCapacityTestState::COMPLETED) {
      Serial.println();
      Serial.println("Battery capacity test completed.");
      printCapacityTestStatus();

      publishBatteryCapacityTest(
        capacityTestMonitor.result(),
        capacityTestPlant.sample().voltageV
      );
    }
  }

  scenarioManager.update(simulatedDeltaTimeSeconds);

  const ScenarioProfile& scenario = scenarioManager.profile();

  pvSimulator.update(
      scenario.irradianceWm2,
      scenario.panelTemperatureC,
      scenario.availablePanelCount,
      scenario.pvAvailabilityFactor
  );

  const float batteryPowerW = manualBatteryPowerOverride
      ? manualBatteryPowerW
      : calculateAutomaticBatteryPowerW();

  batteryEmulator.update(
      batteryPowerW,
      simulatedDeltaTimeSeconds
  );

  const BatteryState& battery = batteryEmulator.state();

  BatteryHealthSample currentSample;
  currentSample.voltageV = battery.terminalVoltageV;
  currentSample.currentA = battery.currentA;
  currentSample.socPercent = battery.socPercent;

  const uint8_t currentOutputCount = currentActiveOutputCount();

  if (previousBatteryHealthSampleAvailable &&
      currentOutputCount != previousBatteryHealthOutputCount) {

    const float deltaCurrentA =
        currentSample.currentA -
        previousBatteryHealthSample.currentA;

    const float deltaVoltageV =
        currentSample.voltageV -
        previousBatteryHealthSample.voltageV;

    Serial.println();
    Serial.println("Battery health load-step detected:");

    Serial.printf(
        "  Outputs: %u -> %u\n",
        static_cast<unsigned int>(
            previousBatteryHealthOutputCount
        ),
        static_cast<unsigned int>(currentOutputCount)
    );

    Serial.printf(
        "  Before: V=%.3f V, I=%.3f A, SOC=%.3f%%\n",
        previousBatteryHealthSample.voltageV,
        previousBatteryHealthSample.currentA,
        previousBatteryHealthSample.socPercent
    );

    Serial.printf(
        "  After:  V=%.3f V, I=%.3f A, SOC=%.3f%%\n",
        currentSample.voltageV,
        currentSample.currentA,
        currentSample.socPercent
    );

    Serial.printf(
        "  dV=%.4f V, dI=%.3f A\n",
        deltaVoltageV,
        deltaCurrentA
    );

    if (batteryHealthIndicator.evaluateStep(
            previousBatteryHealthSample,
            currentSample
        )) {
      Serial.println(
          "  Result: valid Battery Health Indicator sample."
      );
      publishBatteryResistanceStep(
        batteryHealthIndicator.lastEvent()
      );
    } else {
      Serial.println(
          "  Result: sample rejected by validation conditions."
      );
    }
  }

  previousBatteryHealthSample = currentSample;
  previousBatteryHealthOutputCount = currentOutputCount;
  previousBatteryHealthSampleAvailable = true;
}

void updateActuatorEmulators() {
  const unsigned long now = millis();

  if (now - lastActuatorUpdateMs <
      ACTUATOR_UPDATE_INTERVAL_MS) {
    return;
  }

  const unsigned long elapsedMs =
      now - lastActuatorUpdateMs;

  lastActuatorUpdateMs = now;

  const float deltaTimeSeconds =
      static_cast<float>(elapsedMs) / 1000.0f;

  const ActuatorState& masterState =
      masterActuator.state();

  const ActuatorState& slaveState =
      slaveActuator.state();

  ActuatorCommand appliedMasterCommand =
      ActuatorCommand::STOP;

  ActuatorCommand appliedSlaveCommand =
      ActuatorCommand::STOP;

  if (!trackingSafetyMonitor.movementAllowed()) {
    requestedActuatorCommand =
        ActuatorCommand::STOP;

    masterActuator.setCommand(
        ActuatorCommand::STOP
    );

    slaveActuator.setCommand(
        ActuatorCommand::STOP
    );
  } else {
    actuatorSynchronizer.update(
        requestedActuatorCommand,
        masterState.positionMm,
        slaveState.positionMm
    );

    const ActuatorSynchronizationState& syncState =
        actuatorSynchronizer.state();

    appliedMasterCommand =
        syncState.masterCommand;

    appliedSlaveCommand =
        syncState.slaveCommand;

    masterActuator.setCommand(
        appliedMasterCommand
    );

    slaveActuator.setCommand(
        appliedSlaveCommand
    );
  }

  masterActuator.update(deltaTimeSeconds);
  slaveActuator.update(deltaTimeSeconds);

  const ActuatorState& updatedMasterState =
      masterActuator.state();

  const ActuatorState& updatedSlaveState =
      slaveActuator.state();

  masterActuatorUsageMonitor.update(
      deltaTimeSeconds,
      appliedMasterCommand,
      updatedMasterState.positionMm
  );

  slaveActuatorUsageMonitor.update(
      deltaTimeSeconds,
      appliedSlaveCommand,
      updatedSlaveState.positionMm
  );

  actuatorStallMonitor.update(
      deltaTimeSeconds,
      appliedMasterCommand,
      updatedMasterState.positionMm,
      updatedMasterState.atMinimumLimit,
      updatedMasterState.atMaximumLimit,
      appliedSlaveCommand,
      updatedSlaveState.positionMm,
      updatedSlaveState.atMinimumLimit,
      updatedSlaveState.atMaximumLimit
  );

  const ActuatorStallState& stallState =
      actuatorStallMonitor.state();

  trackingSafetyMonitor.reportActuatorStall(
      stallState.masterStallDetected,
      stallState.slaveStallDetected
  );

  if (!trackingSafetyMonitor.movementAllowed()) {
    requestedActuatorCommand =
        ActuatorCommand::STOP;

    masterActuator.setCommand(
        ActuatorCommand::STOP
    );

    slaveActuator.setCommand(
        ActuatorCommand::STOP
    );
  }
}

void updateTrackingSafety() {
  const ActuatorState& masterState = masterActuator.state();
  const ActuatorState& slaveState = slaveActuator.state();

  trackingSafetyMonitor.update(
      masterState.positionMm,
      true,
      slaveState.positionMm,
      true
  );

  if (!trackingSafetyMonitor.movementAllowed()) {
    masterActuator.setCommand(ActuatorCommand::STOP);
    slaveActuator.setCommand(ActuatorCommand::STOP);
  }
}

void updateAutomaticTrackingController() {
    // Maintenance mode gives actuator control to the technician.
    // Do not overwrite manual actuator commands.
    if (maintenanceController.maintenanceModeActive()) {
      trackingController.reset();
      return;
    }

    const char* operatingMode =
        currentTestOperatingMode();

    const bool automaticTrackingPermitted =
        modeAllowsTracking(operatingMode) &&
        maintenanceController.automaticTrackingAllowed() &&
        trackingSafetyMonitor.movementAllowed() &&
        !isCriticalSafetyActive() &&
        !scenarioManager.isTrackingFaultActive();

    if (!automaticTrackingPermitted) {
      trackingController.reset();
      requestedActuatorCommand =
          ActuatorCommand::STOP;
      return;
    }

    const SunSensorState& sensorState =
        sunSensorEmulator.state();

    trackingController.update(
        sensorState.angleYDeg,
        sensorState.measurementValid
    );

    requestedActuatorCommand =
        trackingController.state().command;
  }

void updateSunSensorEmulator() {
  const ScenarioProfile& scenario =
      scenarioManager.profile();

  const ActuatorState& masterState =
      masterActuator.state();

  const ActuatorState& slaveState =
      slaveActuator.state();

  const float averagePositionMm =
      (masterState.positionMm +
       slaveState.positionMm) / 2.0f;

  const float panelAngleDeg =
      trackingGeometry.positionToAngleDeg(
          averagePositionMm
      );

  sunSensorEmulator.update(
      scenario.sunSensorRadiationWm2,
      scenario.sunReferenceAngleDeg,
      panelAngleDeg
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

uint8_t maximumOutputCountForMode(const char* operatingMode) {
  if (operatingMode == nullptr) {
    return 0;
  }

  if (strcmp(operatingMode, "M5") == 0) {
    return 3;
  }

  if (strcmp(operatingMode, "M4") == 0) {
    return 2;
  }

  if (strcmp(operatingMode, "M3") == 0) {
    return 1;
  }

  return 0;
}

bool modeAllowsTracking(const char* operatingMode) {
  if (operatingMode == nullptr) {
    return false;
  }

  return strcmp(operatingMode, "M2") == 0 ||
         strcmp(operatingMode, "M3") == 0 ||
         strcmp(operatingMode, "M4") == 0 ||
         strcmp(operatingMode, "M5") == 0;
}

AppliedOutputState calculateAppliedOutputState() {
  AppliedOutputState appliedState;

  if (!batteryOutputsAllowed()) {
    return appliedState;
  }

  const uint8_t requestedCount =
      cloudOperatingModeActive
        ? maximumOutputCountForMode(cloudOperatingMode)
        : scenarioManager.profile().requestedOutputCount;

  const uint8_t modeLimit =
      maximumOutputCountForMode(currentTestOperatingMode());

  const uint8_t targetCount =
      requestedCount < modeLimit
          ? requestedCount
          : modeLimit;

  if (targetCount == 0) {
    return appliedState;
  }

  if (!maintenanceController.outputLocked(1)) {
    appliedState.output1Active = true;
    appliedState.activeCount++;
  }

  if (appliedState.activeCount < targetCount &&
      !maintenanceController.outputLocked(2)) {
    appliedState.output2Active = true;
    appliedState.activeCount++;
  }

  if (appliedState.activeCount < targetCount &&
      !maintenanceController.outputLocked(3)) {
    appliedState.output3Active = true;
    appliedState.activeCount++;
  }

  return appliedState;
}

uint8_t currentActiveOutputCount() {
  return calculateAppliedOutputState().activeCount;
}

float calculateAutomaticBatteryPowerW() {
  const uint8_t activeOutputCount = currentActiveOutputCount();

  const float outputInputPowerW =
      activeOutputCount *
      (SCOOTER_USEFUL_POWER_W / BOOST_EFFICIENCY);

  // Positive battery power means charging; negative means discharging.
  return pvSimulator.state().deliveredPowerW -
         BASE_LOAD_POWER_W -
         outputInputPowerW;
}

// ============================================================
// Command validation helpers
// ============================================================

bool applyCloudOperatingCommand(const char* command) {
  if (command == nullptr || command[0] == '\0') {
    return false;
  }

  /*
   * AUTO and CLEAR_LOCKOUT release the cloud override.
   * They do not bypass or clear local safety conditions.
   */
  if (strcmp(command, "AUTO") == 0 ||
      strcmp(command, "CLEAR_LOCKOUT") == 0) {
    cloudOperatingModeActive = false;
    cloudOperatingMode[0] = '\0';
    return true;
  }

  const char* targetMode = nullptr;

  if (strcmp(command, "LOCKOUT") == 0) {
    targetMode = "M0";
  } else if (strcmp(command, "STOP") == 0) {
    targetMode = "M1";
  } else if (strcmp(command, "ENABLE_TRACKING") == 0) {
    targetMode = "M2";
  } else if (strcmp(command, "ENABLE_OUTPUT_1") == 0) {
    targetMode = "M3";
  } else if (strcmp(command, "ENABLE_OUTPUT_2") == 0) {
    targetMode = "M4";
  } else if (strcmp(command, "ENABLE_OUTPUT_3") == 0) {
    targetMode = "M5";
  } else {
    return false;
  }

  if (!copyText(
          cloudOperatingMode,
          sizeof(cloudOperatingMode),
          targetMode)) {
    return false;
  }

  cloudOperatingModeActive = true;
  return true;
}

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

  if (cloudOperatingModeActive) {
    return cloudOperatingMode;
  }

  return scenarioManager.nominalOperatingMode();
}

const char* currentFaultState() {
  if (isCriticalSafetyActive()) {
    return "critical_lockout";
  }

  if (!trackingSafetyMonitor.movementAllowed()) {
    return "tracking_fault";
  }

  if (scenarioManager.isStaleDataRequested()) {
    return "normal";
  }

  return scenarioManager.faultStateText();
}

// ============================================================
// Telemetry
// ============================================================
void publishBatteryResistanceStep(
    const BatteryHealthEvent& event
) {
  if (!event.valid) {
    return;
  }

  if (!mqttClient.connected()) {
    Serial.println(
        "Battery diagnostic not published: MQTT is disconnected."
    );
    return;
  }

  char timestamp[25];

  if (!getUtcTimestamp(timestamp, sizeof(timestamp))) {
    Serial.println(
        "Battery diagnostic not published: UTC time is not synchronized."
    );
    return;
  }

  const float socPercent =
      (event.socBeforePercent + event.socAfterPercent) / 2.0f;

  char payload[512];

  const int written = snprintf(
      payload,
      sizeof(payload),
      "{"
        "\"station_id\":\"%s\","
        "\"timestamp\":\"%s\","
        "\"event_type\":\"resistance_step\","
        "\"voltage_before_v\":%.4f,"
        "\"voltage_after_v\":%.4f,"
        "\"current_before_a\":%.4f,"
        "\"current_after_a\":%.4f,"
        "\"soc_percent\":%.3f"
      "}",
      AWS_IOT_CLIENT_ID,
      timestamp,
      event.voltageBeforeV,
      event.voltageAfterV,
      event.currentBeforeA,
      event.currentAfterA,
      socPercent
  );

  if (written < 0 ||
      written >= static_cast<int>(sizeof(payload))) {
    Serial.println(
        "Battery diagnostic not published: payload buffer too small."
    );
    return;
  }

  if (!mqttClient.publish(
          BATTERY_DIAGNOSTICS_TOPIC,
          payload
      )) {
    Serial.println(
        "Battery diagnostic MQTT publish failed."
    );
    return;
  }

  Serial.printf(
      "Battery diagnostic published to %s (%d bytes):\n",
      BATTERY_DIAGNOSTICS_TOPIC,
      written
  );
  Serial.println(payload);
}

void publishBatteryCapacityTest(
    const BatteryCapacityTestResult& result,
    float finalVoltageV
) {
  if (!isfinite(result.measuredCapacityAh) ||
      result.measuredCapacityAh <= 0.0f ||
      !isfinite(result.elapsedSimulatedSeconds) ||
      result.elapsedSimulatedSeconds <= 0.0f ||
      !isfinite(finalVoltageV)) {
    Serial.println(
        "Battery capacity diagnostic not published: invalid test result."
    );
    return;
  }

  if (!mqttClient.connected()) {
    Serial.println(
        "Battery capacity diagnostic not published: MQTT is disconnected."
    );
    return;
  }

  char timestamp[25];

  if (!getUtcTimestamp(timestamp, sizeof(timestamp))) {
    Serial.println(
        "Battery capacity diagnostic not published: UTC time is not synchronized."
    );
    return;
  }

  char payload[256];

  const int written = snprintf(
      payload,
      sizeof(payload),
      "{"
        "\"station_id\":\"%s\","
        "\"timestamp\":\"%s\","
        "\"event_type\":\"capacity_test\","
        "\"measured_capacity_ah\":%.3f,"
        "\"final_voltage_v\":%.3f,"
        "\"elapsed_test_s\":%.1f"
      "}",
      AWS_IOT_CLIENT_ID,
      timestamp,
      result.measuredCapacityAh,
      finalVoltageV,
      result.elapsedSimulatedSeconds
  );

  if (written < 0 ||
      written >= static_cast<int>(sizeof(payload))) {
    Serial.println(
        "Battery capacity diagnostic not published: payload buffer too small."
    );
    return;
  }

  if (!mqttClient.publish(
          BATTERY_DIAGNOSTICS_TOPIC,
          payload
      )) {
    Serial.println(
        "Battery capacity diagnostic MQTT publish failed."
    );
    return;
  }

  Serial.printf(
      "Battery capacity diagnostic published to %s (%d bytes):\n",
      BATTERY_DIAGNOSTICS_TOPIC,
      written
  );
  Serial.println(payload);
}

bool publishActuatorDiagnostics() {
  if (!mqttClient.connected()) {
    Serial.println(
        "Actuator diagnostics not published: MQTT is disconnected."
    );
    return false;
  }

  char timestamp[25];

  if (!getUtcTimestamp(
          timestamp,
          sizeof(timestamp)
      )) {
    Serial.println(
        "Actuator diagnostics not published: UTC time is not synchronized."
    );
    return false;
  }

  const ActuatorUsageState& masterUsage =
      masterActuatorUsageMonitor.state();

  const ActuatorUsageState& slaveUsage =
      slaveActuatorUsageMonitor.state();

  char payload[1024];

  const int written = snprintf(
      payload,
      sizeof(payload),
      "{"
        "\"station_id\":\"%s\","
        "\"timestamp\":\"%s\","
        "\"master\":{"
          "\"window_sequence\":%lu,"
          "\"operating_time_s\":%.2f,"
          "\"total_travel_mm\":%.2f,"
          "\"movement_starts\":%lu,"
          "\"equivalent_full_stroke_cycles\":%.4f,"
          "\"duty_cycle_percent\":%.2f,"
          "\"duty_cycle_window_available\":%s,"
          "\"duty_cycle_exceeded\":%s"
        "},"
        "\"slave\":{"
          "\"window_sequence\":%lu,"
          "\"operating_time_s\":%.2f,"
          "\"total_travel_mm\":%.2f,"
          "\"movement_starts\":%lu,"
          "\"equivalent_full_stroke_cycles\":%.4f,"
          "\"duty_cycle_percent\":%.2f,"
          "\"duty_cycle_window_available\":%s,"
          "\"duty_cycle_exceeded\":%s"
        "}"
      "}",
      AWS_IOT_CLIENT_ID,
      timestamp,

      static_cast<unsigned long>(
          masterUsage.dutyCycleWindowSequence
      ),
      masterUsage.operatingTimeSeconds,
      masterUsage.totalTravelMm,
      static_cast<unsigned long>(
          masterUsage.movementStarts
      ),
      masterUsage.equivalentFullStrokeCycles,
      masterUsage.lastDutyCyclePercent,
      masterUsage.dutyCycleWindowAvailable
          ? "true"
          : "false",
      masterUsage.dutyCycleExceeded
          ? "true"
          : "false",

      static_cast<unsigned long>(
          slaveUsage.dutyCycleWindowSequence
      ),
      slaveUsage.operatingTimeSeconds,
      slaveUsage.totalTravelMm,
      static_cast<unsigned long>(
          slaveUsage.movementStarts
      ),
      slaveUsage.equivalentFullStrokeCycles,
      slaveUsage.lastDutyCyclePercent,
      slaveUsage.dutyCycleWindowAvailable
          ? "true"
          : "false",
      slaveUsage.dutyCycleExceeded
          ? "true"
          : "false"
  );

  if (written < 0) {
    Serial.println(
        "Actuator diagnostics JSON formatting error."
    );
    return false;
  }

  if (static_cast<size_t>(written) >=
      sizeof(payload)) {
    Serial.printf(
        "Actuator diagnostics payload too large. "
        "Required: %d bytes, available: %u bytes.\n",
        written + 1,
        static_cast<unsigned int>(sizeof(payload))
    );
    return false;
  }

  const bool published = mqttClient.publish(
      ACTUATOR_DIAGNOSTICS_TOPIC,
      reinterpret_cast<const uint8_t*>(payload),
      static_cast<unsigned int>(written),
      false
  );

  if (!published) {
    Serial.print(
        "Actuator diagnostics publication failed. "
        "MQTT state: "
    );
    Serial.println(mqttClient.state());
    return false;
  }

  Serial.printf(
      "Actuator diagnostics published to %s "
      "(%d bytes):\n%s\n",
      ACTUATOR_DIAGNOSTICS_TOPIC,
      written,
      payload
  );

  return true;
}

void publishTelemetry() {
  if (!mqttClient.connected()) {
    Serial.println("Telemetry not published: MQTT is disconnected.");
    return;
  }

  char timestamp[25];

  if (!getTelemetryTimestamp(timestamp, sizeof(timestamp))) {
    Serial.println("Telemetry not published: UTC time is not synchronized.");
    return;
  }

  telemetryCounter++;

  const BatteryState& battery = batteryEmulator.state();
  const PVState& pv = pvSimulator.state();
  const ScenarioProfile& scenario = scenarioManager.profile();
  const ActuatorState& masterActuatorState =
      masterActuator.state();

  const ActuatorState& slaveActuatorState =
      slaveActuator.state();

  const float averageActuatorPositionMm =
      (masterActuatorState.positionMm +
      slaveActuatorState.positionMm) / 2.0f;

  const float trackingAngleDeg =
      trackingGeometry.positionToAngleDeg(
          averageActuatorPositionMm
      );

  const char* operatingMode = currentTestOperatingMode();
  const TrackingSafetyState& trackingSafetyState =
      trackingSafetyMonitor.state();
  const SunSensorState& sunSensorState =
      sunSensorEmulator.state();

  const char* trackingSafetyFault =
      trackingSafetyFaultToString(trackingSafetyState.fault);
  const bool trackingEnabled =
      modeAllowsTracking(operatingMode) &&
      maintenanceController.automaticTrackingAllowed() &&
      trackingSafetyMonitor.movementAllowed() &&
      sunSensorState.measurementValid &&
      !isCriticalSafetyActive() &&
      !scenarioManager.isTrackingFaultActive();
  const AppliedOutputState appliedOutputs =
      calculateAppliedOutputState();

  const bool output1Active =
      appliedOutputs.output1Active;

  const bool output2Active =
      appliedOutputs.output2Active;

  const bool output3Active =
      appliedOutputs.output3Active;
  const char* faultState = currentFaultState();
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
          "\"delivered_power_w\":%.3f,"
          "\"panel_1_current_a\":%.3f,"
          "\"panel_2_current_a\":%.3f,"
          "\"panel_3_current_a\":%.3f,"
          "\"available_panel_count\":%u,"
          "\"availability_factor\":%.3f,"
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
          "\"safety_fault\":\"%s\","
          "\"angle_deg\":%.2f,"
          "\"sun_sensor_angle_y_deg\":%.2f,"
          "\"sun_sensor_radiation_wm2\":%.1f,"
          "\"sun_sensor_radiation_enough\":%s,"
          "\"sun_sensor_in_fov\":%s,"
          "\"sun_sensor_measurement_valid\":%s,"
          "\"master_position_mm\":%.2f,"
          "\"slave_position_mm\":%.2f"
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
          "\"stale_data_requested\":%s,"
          "\"stale_offset_s\":%ld"
        "},"
        "\"fault_state\":\"%s\","
        "\"test_counter\":%lu,"
        "\"source\":\"esp32_functional_simulation_pv_v1\""
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
      pv.voltageV,
      pv.totalCurrentA,
      pv.rawPowerW,
      pv.deliveredPowerW,
      pv.panelCurrentA[0],
      pv.panelCurrentA[1],
      pv.panelCurrentA[2],
      static_cast<unsigned int>(pv.availablePanelCount),
      pv.availabilityFactor,
      pv.irradianceWm2,
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
      trackingSafetyFault,
      trackingAngleDeg,
      sunSensorState.angleYDeg,
      sunSensorState.radiationWm2,
      sunSensorState.radiationEnough ? "true" : "false",
      sunSensorState.sunInFieldOfView ? "true" : "false",
      sunSensorState.measurementValid ? "true" : "false",
      masterActuatorState.positionMm,
      slaveActuatorState.positionMm,
      scenario.weatherIndex,
      scenario.demandIndex,
      nominalMode,
      nominalMode,
      operatingMode,
      scenarioManager.scenarioName(),
      scenarioManager.elapsedSimulatedSeconds(),
      batteryTimeScale,
      scenarioManager.isStaleDataRequested() ? "true" : "false",
      static_cast<long>(
          scenarioManager.isStaleDataRequested()
              ? STALE_DATA_OFFSET_SECONDS
              : 0
      ),
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
        "Command name is missing; no simulated action was applied.",
        false
    );
    return;
  }

  if (!isAllowedCommand(command)) {
    prepareCommandAck(
        commandId,
        command,
        "invalid_command",
        "Command is not in the local allowed-command list; no simulated action was applied.",
        false
    );
    return;
  }

  if (isCriticalSafetyActive() && !isSafetyReducingCommand(command)) {
    prepareCommandAck(
        commandId,
        command,
        "blocked_by_safety",
        "Command blocked by a local critical safety condition; no simulated action was applied.",
        false
    );
    return;
  }

  const bool applied =
      applyCloudOperatingCommand(command);

  if (!applied) {
    prepareCommandAck(
        commandId,
        command,
        "accepted",
        "Command recognized but not mapped to an operating-mode action.",
        false
    );
    return;
  }

  prepareCommandAck(
      commandId,
      command,
      "accepted",
      "Command applied to the functional simulation.",
      true
  );
}

void prepareCommandAck(
    const char* commandId,
    const char* command,
    const char* status,
    const char* message,
    bool applied
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

  pendingAck.applied = applied;
  pendingAck.pending = true;
  lastAckAttemptMs = millis() - ACK_RETRY_INTERVAL_MS;

  Serial.println("Command classified by local validation.");
  Serial.printf("Command ID: %s\n", pendingAck.commandId);
  Serial.printf("Command: %s\n", pendingAck.command);
  Serial.printf("ACK status: %s\n", pendingAck.status);
  Serial.printf("Applied: %s\n", pendingAck.applied ? "true" : "false");
  Serial.println(pendingAck.message);
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
  ackDocument["applied"] = pendingAck.applied;
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
  pendingAck.applied = false;
  pendingAck.commandId[0] = '\0';
  pendingAck.command[0] = '\0';
  pendingAck.status[0] = '\0';
  pendingAck.message[0] = '\0';
  pendingAck.resultingOperatingMode[0] = '\0';
}