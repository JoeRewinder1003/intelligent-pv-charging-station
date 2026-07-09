# MQTT Topic Structure

## Overview

Two types of topics are used in this architecture:

- **Basic Ingest topics** (`$aws/rules/...`): used by the ESP32 to publish
  telemetry and events toward AWS. Messages go directly to the IoT Rule engine
  without passing through the standard MQTT broker, which reduces messaging cost.

- **Standard MQTT topics** (`station/...`): used by AWS to publish commands
  back to the ESP32. The ESP32 subscribes to these topics at startup.

---

## Topic Table

| Topic | Direction | Type | Publisher | Subscriber |
|---|---|---|---|---|
| `$aws/rules/TelemetryRule/station/{station_id}/telemetry/raw` | ESP32 → AWS | Basic Ingest | ESP32 | IoT Rule |
| `$aws/rules/FaultRule/station/{station_id}/events/fault` | ESP32 → AWS | Basic Ingest | ESP32 | IoT Rule |
| `$aws/rules/ChargingRule/station/{station_id}/events/charging` | ESP32 → AWS | Basic Ingest | ESP32 | IoT Rule |
| `$aws/rules/HeartbeatRule/station/{station_id}/status/heartbeat` | ESP32 → AWS | Basic Ingest | ESP32 | IoT Rule |
| `$aws/rules/AckRule/station/{station_id}/ack/command` | ESP32 → AWS | Basic Ingest | ESP32 | IoT Rule |
| `station/{station_id}/commands/mode` | AWS → ESP32 | Standard MQTT | Lambda | ESP32 |
| `station/{station_id}/commands/outputs` | AWS → ESP32 | Standard MQTT | Lambda | ESP32 |
| `station/{station_id}/commands/tracking` | AWS → ESP32 | Standard MQTT | Lambda | ESP32 |
| `station/{station_id}/commands/safety` | AWS → ESP32 | Standard MQTT | Lambda | ESP32 |
| `station/{station_id}/commands/config` | AWS → ESP32 | Standard MQTT | Lambda | ESP32 |
| `station/{station_id}/commands/ota` | AWS → ESP32 | Standard MQTT | Lambda | ESP32 |

---

## Notes

- `{station_id}` is a fixed string per physical station, e.g. `solar_station_01`.
- Basic Ingest topics are not visible to other MQTT clients — they go directly
  to the rule engine. This is intentional and saves cost.
- The ESP32 subscribes to `station/{station_id}/commands/#` using a wildcard
  so it receives all command subtopics with a single subscription.
- OTA commands are delivered through `commands/ota` and contain a pre-signed
  S3 URL. The ESP32 downloads the firmware binary directly from S3 over HTTPS.
