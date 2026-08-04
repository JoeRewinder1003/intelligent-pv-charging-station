#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <string.h>

#include "secrets.h"

/*
 * AWS IoT connectivity and local command-validation test.
 *
 * This firmware:
 * - Connects the ESP32 to Wi-Fi and AWS IoT Core.
 * - Synchronizes UTC time with NTP.
 * - Publishes test telemetry every 15 seconds.
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
static constexpr unsigned long MQTT_RETRY_INTERVAL_MS = 5000;
static constexpr unsigned long ACK_RETRY_INTERVAL_MS = 2000;
static constexpr time_t MIN_VALID_EPOCH = 1704067200;  // 2024-01-01 UTC

static constexpr char NORMAL_TEST_MODE[] = "M4";
static constexpr char LOCKOUT_TEST_MODE[] = "M0";

// ============================================================
// MQTT and TLS clients
// ============================================================

WiFiClientSecure secureClient;
PubSubClient mqttClient(secureClient);

// ============================================================
// Runtime state
// ============================================================

unsigned long lastTelemetryMs = 0;
unsigned long lastMqttAttemptMs = 0;
unsigned long lastAckAttemptMs = 0;
uint32_t telemetryCounter = 0;

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
void printSimulatedSafetyState();

bool getUtcTimestamp(char* buffer, size_t bufferSize);
bool isAllowedCommand(const char* command);
bool isSafetyReducingCommand(const char* command);
bool copyText(char* destination, size_t destinationSize, const char* source);
const char* currentTestOperatingMode();

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
  Serial.println("AWS IoT local command-validation test");
  Serial.println("No physical outputs will be activated.");
  Serial.println("Serial commands: SAFETY ON, SAFETY OFF, STATUS");
  Serial.println("========================================");

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
    return;
  }

  Serial.println("Unknown serial command.");
  Serial.println("Use: SAFETY ON, SAFETY OFF, or STATUS");
}

void printSimulatedSafetyState() {
  Serial.print("Simulated safety state: ");
  Serial.println(
      simulatedCriticalLockout ? "CRITICAL_LOCKOUT" : "NORMAL"
  );
  Serial.print("Test operating mode: ");
  Serial.println(currentTestOperatingMode());
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
  return simulatedCriticalLockout
      ? LOCKOUT_TEST_MODE
      : NORMAL_TEST_MODE;
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

  const bool trackingEnabled = !simulatedCriticalLockout;
  const bool output1Active = !simulatedCriticalLockout;
  const bool output2Active = !simulatedCriticalLockout;
  const bool output3Active = false;
  const char* faultState = simulatedCriticalLockout
      ? "critical_lockout"
      : "normal";
  const char* operatingMode = currentTestOperatingMode();

  char payload[1800];

  const int written = snprintf(
      payload,
      sizeof(payload),
      "{"
        "\"station_id\":\"%s\","
        "\"timestamp\":\"%s\","
        "\"battery\":{"
          "\"voltage_v\":12.7,"
          "\"current_a\":-5.2,"
          "\"power_w\":-66.0,"
          "\"soc_percent\":92.5"
        "},"
        "\"pv\":{"
          "\"voltage_v\":19.8,"
          "\"current_a\":12.4,"
          "\"power_w\":245.5,"
          "\"local_irradiance_wm2\":720.0"
        "},"
        "\"environment\":{"
          "\"ambient_temperature_c\":28.4,"
          "\"relative_humidity_percent\":46.0,"
          "\"panel_temperature_c\":45.3"
        "},"
        "\"outputs\":{"
          "\"output_1_active\":%s,"
          "\"output_2_active\":%s,"
          "\"output_3_active\":%s,"
          "\"output_1_current_a\":%.2f,"
          "\"output_2_current_a\":%.2f,"
          "\"output_3_current_a\":0.0"
        "},"
        "\"tracking\":{"
          "\"enabled\":%s,"
          "\"angle_deg\":28.5,"
          "\"target_angle_deg\":30.0,"
          "\"master_position_raw\":2040,"
          "\"slave_position_raw\":2025"
        "},"
        "\"decision\":{"
          "\"weather_index\":0.78,"
          "\"demand_index\":0.62,"
          "\"fis_mode\":\"M4\","
          "\"requested_mode\":\"M4\","
          "\"operating_mode\":\"%s\""
        "},"
        "\"fault_state\":\"%s\","
        "\"test_counter\":%lu,"
        "\"source\":\"esp32_local_command_validation_test\""
      "}",
      AWS_IOT_CLIENT_ID,
      timestamp,
      output1Active ? "true" : "false",
      output2Active ? "true" : "false",
      output3Active ? "true" : "false",
      output1Active ? 1.62 : 0.0,
      output2Active ? 1.58 : 0.0,
      trackingEnabled ? "true" : "false",
      operatingMode,
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

  if (simulatedCriticalLockout && !isSafetyReducingCommand(command)) {
    prepareCommandAck(
        commandId,
        command,
        "blocked_by_safety",
        "Command blocked by simulated local critical lockout; no physical action executed."
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