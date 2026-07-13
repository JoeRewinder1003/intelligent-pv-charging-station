#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

#include "secrets.h"

static constexpr char TELEMETRY_TOPIC[] =
    "station/station_001/telemetry";

static constexpr char COMMAND_TOPIC[] =
    "station/station_001/commands";

static constexpr uint16_t MQTT_PORT = 8883;
static constexpr unsigned long TELEMETRY_INTERVAL_MS = 15000;
static constexpr unsigned long MQTT_RETRY_INTERVAL_MS = 5000;

WiFiClientSecure secureClient;
PubSubClient mqttClient(secureClient);

unsigned long lastTelemetryMs = 0;
unsigned long lastMqttAttemptMs = 0;
uint32_t telemetryCounter = 0;

void connectWiFi();
void configureTls();
void connectMqtt();
void publishTelemetry();
void mqttCallback(char* topic, byte* payload, unsigned int length);

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("AWS IoT connectivity test");
  Serial.println("No physical outputs will be activated.");

  connectWiFi();
  configureTls();

  mqttClient.setServer(AWS_IOT_ENDPOINT, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(2048);

  connectMqtt();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
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

    if (now - lastTelemetryMs >= TELEMETRY_INTERVAL_MS) {
      lastTelemetryMs = now;
      publishTelemetry();
    }
  }

  delay(10);
}

void connectWiFi() {
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
  } else {
    Serial.println("Wi-Fi connection timeout.");
  }
}

void configureTls() {
  secureClient.setCACert(AWS_ROOT_CA);
  secureClient.setCertificate(AWS_DEVICE_CERT);
  secureClient.setPrivateKey(AWS_PRIVATE_KEY);
}

void connectMqtt() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  Serial.printf(
      "Connecting to AWS IoT as client %s...\n",
      AWS_IOT_CLIENT_ID
  );

  if (mqttClient.connect(AWS_IOT_CLIENT_ID)) {
    Serial.println("AWS IoT MQTT connected.");

    if (mqttClient.subscribe(COMMAND_TOPIC, 1)) {
      Serial.printf("Subscribed to: %s\n", COMMAND_TOPIC);
    } else {
      Serial.println("Failed to subscribe to command topic.");
    }
  } else {
    Serial.print("MQTT connection failed. State: ");
    Serial.println(mqttClient.state());
  }
}

void publishTelemetry() {
  telemetryCounter++;

  char payload[768];

  const int written = snprintf(
      payload,
      sizeof(payload),
      "{"
        "\"station_id\":\"station_001\","
        "\"timestamp\":\"2026-07-12T00:00:00Z\","
        "\"soc_percent\":90.0,"
        "\"battery_voltage_v\":12.8,"
        "\"battery_current_a\":-2.5,"
        "\"battery_power_w\":-32.0,"
        "\"local_irradiance_wm2\":450.0,"
        "\"test_counter\":%lu,"
        "\"source\":\"esp32_connectivity_test\""
      "}",
      static_cast<unsigned long>(telemetryCounter)
  );

  if (written < 0 ||
      static_cast<size_t>(written) >= sizeof(payload)) {
    Serial.println("Telemetry payload buffer error.");
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
        "Telemetry published to %s:\n%s\n",
        TELEMETRY_TOPIC,
        payload
    );
  } else {
    Serial.println("Telemetry publication failed.");
  }
}

void mqttCallback(
    char* topic,
    byte* payload,
    unsigned int length
) {
  Serial.println();
  Serial.printf("MQTT message received on: %s\n", topic);

  Serial.print("Payload: ");
  for (unsigned int i = 0; i < length; i++) {
    Serial.write(payload[i]);
  }
  Serial.println();

  /*
   * Connectivity test only.
   *
   * Do not activate relays, actuators, tracking, or charging outputs here.
   * Commands will later be parsed and sent through the local deterministic
   * safety-validation layer before any physical action is authorized.
   */
}