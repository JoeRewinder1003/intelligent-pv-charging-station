#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <string.h>

#include "secrets.h"

/*
 * AWS IoT connectivity and acknowledgement test.
 *
 * This firmware:
 * - Connects the ESP32 to Wi-Fi.
 * - Synchronizes UTC time using NTP.
 * - Connects securely to AWS IoT Core.
 * - Publishes test telemetry every 15 seconds.
 * - Receives cloud commands.
 * - Publishes an ACK with status "received".
 *
 * This firmware DOES NOT:
 * - Activate charging outputs.
 * - Activate relays.
 * - Move linear actuators.
 * - Apply tracking commands.
 * - Authorize any physical cloud command.
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
// Test configuration
// ============================================================

static constexpr uint16_t MQTT_PORT = 8883;

static constexpr unsigned long TELEMETRY_INTERVAL_MS = 15000;
static constexpr unsigned long MQTT_RETRY_INTERVAL_MS = 5000;
static constexpr unsigned long ACK_RETRY_INTERVAL_MS = 2000;

static constexpr time_t MIN_VALID_EPOCH =
    1704067200;  // 2024-01-01 00:00:00 UTC

/*
 * Fixed connectivity-test operating mode.
 *
 * This is not a real physical state and is not the result of the
 * final fuzzy inference system.
 */
static constexpr char TEST_OPERATING_MODE[] = "M4";

// ============================================================
// MQTT and TLS clients
// ============================================================

WiFiClientSecure secureClient;
PubSubClient mqttClient(secureClient);

// ============================================================
// Runtime variables
// ============================================================

unsigned long lastTelemetryMs = 0;
unsigned long lastMqttAttemptMs = 0;
unsigned long lastAckAttemptMs = 0;

uint32_t telemetryCounter = 0;

// ============================================================
// Pending acknowledgement
// ============================================================

struct PendingCommandAck {
  bool pending = false;
  char commandId[80] = "";
  char command[48] = "";
};

PendingCommandAck pendingAck;

// ============================================================
// Function declarations
// ============================================================

void connectWiFi();
void configureTls();
void synchronizeTime();
void connectMqtt();

bool getUtcTimestamp(
    char* buffer,
    size_t bufferSize
);

void publishTelemetry();
void publishPendingCommandAck();

void mqttCallback(
    char* topic,
    byte* payload,
    unsigned int length
);

// ============================================================
// Setup
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("========================================");
  Serial.println("AWS IoT connectivity and ACK test");
  Serial.println("No physical outputs will be activated.");
  Serial.println("========================================");

  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    synchronizeTime();
  }

  configureTls();

  mqttClient.setServer(
      AWS_IOT_ENDPOINT,
      MQTT_PORT
  );

  mqttClient.setCallback(mqttCallback);

  /*
   * The default PubSubClient buffer is too small for the complete
   * nested telemetry JSON and AWS command payload.
   */
  if (!mqttClient.setBufferSize(2048)) {
    Serial.println(
        "Warning: MQTT buffer size could not be changed."
    );
  }

  connectMqtt();
}

// ============================================================
// Main loop
// ============================================================

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();

    if (WiFi.status() == WL_CONNECTED &&
        time(nullptr) < MIN_VALID_EPOCH) {
      synchronizeTime();
    }
  }

  if (!mqttClient.connected()) {
    const unsigned long now = millis();

    if (now - lastMqttAttemptMs >=
        MQTT_RETRY_INTERVAL_MS) {
      lastMqttAttemptMs = now;
      connectMqtt();
    }
  } else {
    /*
     * mqttClient.loop() must run frequently to receive commands,
     * maintain the MQTT connection, and process acknowledgements.
     */
    mqttClient.loop();

    const unsigned long now = millis();

    if (pendingAck.pending &&
        now - lastAckAttemptMs >= ACK_RETRY_INTERVAL_MS) {
      lastAckAttemptMs = now;
      publishPendingCommandAck();
    }

    if (now - lastTelemetryMs >=
        TELEMETRY_INTERVAL_MS) {
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

  Serial.printf(
      "Connecting to Wi-Fi: %s\n",
      WIFI_SSID
  );

  WiFi.mode(WIFI_STA);
  WiFi.begin(
      WIFI_SSID,
      WIFI_PASSWORD
  );

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
    Serial.println(
        "Cannot synchronize time without Wi-Fi."
    );
    return;
  }

  Serial.println(
      "Synchronizing UTC time with NTP..."
  );

  configTime(
      0,
      0,
      "pool.ntp.org",
      "time.nist.gov"
  );

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

    if (getUtcTimestamp(
            timestamp,
            sizeof(timestamp))) {
      Serial.print("UTC time synchronized: ");
      Serial.println(timestamp);
    } else {
      Serial.println(
          "UTC timestamp formatting failed."
      );
    }
  } else {
    Serial.println("NTP synchronization timeout.");
  }
}

bool getUtcTimestamp(
    char* buffer,
    size_t bufferSize
) {
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
    Serial.println(
        "MQTT connection skipped: Wi-Fi is disconnected."
    );
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
    Serial.print(
        "MQTT connection failed. State: "
    );
    Serial.println(mqttClient.state());
    return;
  }

  Serial.println("AWS IoT MQTT connected.");

  if (mqttClient.subscribe(COMMAND_TOPIC, 1)) {
    Serial.printf(
        "Subscribed to: %s\n",
        COMMAND_TOPIC
    );
  } else {
    Serial.println(
        "Failed to subscribe to command topic."
    );
  }
}

// ============================================================
// Telemetry
// ============================================================

void publishTelemetry() {
  if (!mqttClient.connected()) {
    Serial.println(
        "Telemetry not published: MQTT is disconnected."
    );
    return;
  }

  char timestamp[25];

  if (!getUtcTimestamp(
          timestamp,
          sizeof(timestamp))) {
    Serial.println(
        "Telemetry not published: UTC time is not synchronized."
    );
    return;
  }

  telemetryCounter++;

  /*
   * These are fixed test values.
   *
   * They confirm the telemetry format and cloud flow but do not
   * represent current sensor measurements or the final FIS.
   */
  char payload[1600];

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
          "\"output_1_active\":true,"
          "\"output_2_active\":true,"
          "\"output_3_active\":false,"
          "\"output_1_current_a\":1.62,"
          "\"output_2_current_a\":1.58,"
          "\"output_3_current_a\":0.0"
        "},"

        "\"tracking\":{"
          "\"enabled\":true,"
          "\"angle_deg\":28.5,"
          "\"target_angle_deg\":30.0,"
          "\"master_position_raw\":2040,"
          "\"slave_position_raw\":2025"
        "},"

        "\"decision\":{"
          "\"weather_index\":0.78,"
          "\"demand_index\":0.62,"
          "\"fis_mode\":\"%s\","
          "\"requested_mode\":\"%s\","
          "\"operating_mode\":\"%s\""
        "},"

        "\"fault_state\":\"normal\","
        "\"test_counter\":%lu,"
        "\"source\":\"esp32_connectivity_test\""
      "}",
      AWS_IOT_CLIENT_ID,
      timestamp,
      TEST_OPERATING_MODE,
      TEST_OPERATING_MODE,
      TEST_OPERATING_MODE,
      static_cast<unsigned long>(telemetryCounter)
  );

  if (written < 0) {
    Serial.println(
        "Telemetry JSON formatting error."
    );
    return;
  }

  if (static_cast<size_t>(written) >=
      sizeof(payload)) {
    Serial.printf(
        "Telemetry payload too large. "
        "Required: %d bytes, available: %u bytes.\n",
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
        "Telemetry published to %s (%d bytes):\n",
        TELEMETRY_TOPIC,
        written
    );

    Serial.println(payload);
  } else {
    Serial.print(
        "Telemetry publication failed. MQTT state: "
    );
    Serial.println(mqttClient.state());
  }
}

// ============================================================
// MQTT command reception
// ============================================================

void mqttCallback(
    char* topic,
    byte* payload,
    unsigned int length
) {
  Serial.println();
  Serial.println("----------------------------------------");
  Serial.printf(
      "MQTT message received on: %s\n",
      topic
  );

  Serial.print("Payload: ");

  for (unsigned int i = 0; i < length; i++) {
    Serial.write(payload[i]);
  }

  Serial.println();

  if (strcmp(topic, COMMAND_TOPIC) != 0) {
    Serial.println(
        "Message ignored: unexpected MQTT topic."
    );
    return;
  }

  /*
   * This test firmware only has one ACK slot.
   * A new command is not stored until the previous ACK has been
   * published successfully.
   */
  if (pendingAck.pending) {
    Serial.println(
        "Command not queued: previous ACK is still pending."
    );
    return;
  }

#if ARDUINOJSON_VERSION_MAJOR >= 7
  JsonDocument commandDocument;
#else
  StaticJsonDocument<1024> commandDocument;
#endif

  const DeserializationError error =
      deserializeJson(
          commandDocument,
          payload,
          length
      );

  if (error) {
    Serial.print("Invalid command JSON: ");
    Serial.println(error.c_str());
    return;
  }

  const char* stationId =
      commandDocument["station_id"] | "";

  const char* commandId =
      commandDocument["command_id"] | "";

  const char* command =
      commandDocument["command"] | "";

  if (stationId[0] == '\0') {
    Serial.println(
        "Command rejected: station_id is missing."
    );
    return;
  }

  if (strcmp(
          stationId,
          AWS_IOT_CLIENT_ID) != 0) {
    Serial.println(
        "Command rejected: station_id does not match this device."
    );
    return;
  }

  if (commandId[0] == '\0') {
    Serial.println(
        "Command rejected: command_id is missing."
    );
    return;
  }

  if (command[0] == '\0') {
    Serial.println(
        "Command rejected: command is missing."
    );
    return;
  }

  if (strlen(commandId) >=
      sizeof(pendingAck.commandId)) {
    Serial.println(
        "Command rejected: command_id is too long."
    );
    return;
  }

  if (strlen(command) >=
      sizeof(pendingAck.command)) {
    Serial.println(
        "Command rejected: command name is too long."
    );
    return;
  }

  strncpy(
      pendingAck.commandId,
      commandId,
      sizeof(pendingAck.commandId) - 1
  );

  pendingAck.commandId[
      sizeof(pendingAck.commandId) - 1
  ] = '\0';

  strncpy(
      pendingAck.command,
      command,
      sizeof(pendingAck.command) - 1
  );

  pendingAck.command[
      sizeof(pendingAck.command) - 1
  ] = '\0';

  pendingAck.pending = true;

  /*
   * Permit the first ACK publication attempt immediately.
   */
  lastAckAttemptMs =
      millis() - ACK_RETRY_INTERVAL_MS;

  Serial.println("Command parsed successfully.");

  Serial.printf(
      "Command ID: %s\n",
      pendingAck.commandId
  );

  Serial.printf(
      "Command: %s\n",
      pendingAck.command
  );

  Serial.println(
      "No physical action was executed."
  );

  Serial.println(
      "A received ACK will be published."
  );

  Serial.println("----------------------------------------");
}

// ============================================================
// Command acknowledgement
// ============================================================

void publishPendingCommandAck() {
  if (!pendingAck.pending) {
    return;
  }

  if (!mqttClient.connected()) {
    return;
  }

  char timestamp[25];

  if (!getUtcTimestamp(
          timestamp,
          sizeof(timestamp))) {
    Serial.println(
        "ACK not published: UTC time is not synchronized."
    );
    return;
  }

#if ARDUINOJSON_VERSION_MAJOR >= 7
  JsonDocument ackDocument;
#else
  StaticJsonDocument<512> ackDocument;
#endif

  ackDocument["station_id"] =
      AWS_IOT_CLIENT_ID;

  ackDocument["timestamp"] =
      timestamp;

  ackDocument["message_type"] =
      "acks";

  ackDocument["command_id"] =
      pendingAck.commandId;

  ackDocument["command"] =
      pendingAck.command;

  /*
   * "received" only confirms that the command arrived and its
   * minimum JSON fields were parsed.
   *
   * It does not mean that the command was accepted or applied.
   */
  ackDocument["status"] =
      "received";

  ackDocument["applied"] =
      false;

  /*
   * The operating mode remains the fixed test mode because this
   * connectivity firmware does not apply the received command.
   */
  ackDocument["resulting_operating_mode"] =
      TEST_OPERATING_MODE;

  ackDocument["message"] =
      "Command received by ESP32; no physical action executed.";

  char ackPayload[512];

  const size_t requiredSize =
      measureJson(ackDocument);

  if (requiredSize + 1 >
      sizeof(ackPayload)) {
    Serial.printf(
        "ACK payload buffer is too small. "
        "Required: %u bytes, available: %u bytes.\n",
        static_cast<unsigned int>(requiredSize + 1),
        static_cast<unsigned int>(sizeof(ackPayload))
    );
    return;
  }

  const size_t written =
      serializeJson(
          ackDocument,
          ackPayload,
          sizeof(ackPayload)
      );

  if (written == 0) {
    Serial.println(
        "ACK JSON serialization failed."
    );
    return;
  }

  const bool published =
      mqttClient.publish(
          ACK_TOPIC,
          reinterpret_cast<const uint8_t*>(ackPayload),
          static_cast<unsigned int>(written),
          false
      );

  if (!published) {
    Serial.print(
        "ACK publication failed. MQTT state: "
    );
    Serial.println(mqttClient.state());
    return;
  }

  Serial.println();
  Serial.printf(
      "ACK published to %s (%u bytes):\n",
      ACK_TOPIC,
      static_cast<unsigned int>(written)
  );

  Serial.println(ackPayload);

  /*
   * Clear the pending ACK only after MQTT accepted the
   * publication request.
   */
  pendingAck.pending = false;
  pendingAck.commandId[0] = '\0';
  pendingAck.command[0] = '\0';
}