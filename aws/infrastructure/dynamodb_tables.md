# DynamoDB Tables

## Design principles

- DynamoDB is the only database used in v1.
- All tables use on-demand billing (pay-per-request) to stay within Free Tier
  for a single-station prototype.
- No Timestream, no RDS, no ElasticSearch in this version.
- TTL (Time To Live) is enabled on telemetry records to automatically delete
  old data and control storage costs.

---

## Table 1: StationTelemetry

Stores every telemetry record received from the ESP32.

| Attribute | Type | Role |
|---|---|---|
| `station_id` | String | Partition key |
| `timestamp` | String (ISO 8601) | Sort key |
| `battery_soc` | Number | |
| `battery_voltage` | Number | |
| `battery_current` | Number | |
| `p_net` | Number | |
| `local_irradiance` | Number | |
| `shortwave_radiation` | Number | |
| `cloud_cover` | Number | |
| `precipitation_probability` | Number | |
| `demand_index` | Number | |
| `active_outputs` | Number | |
| `tracking_angle` | Number | |
| `fault_state` | String | |
| `weather_index` | Number | Computed by Lambda |
| `operating_mode` | String | Computed by Lambda |
| `ttl` | Number | Unix epoch — auto-delete after 90 days |

Access pattern: query by `station_id`, sorted by `timestamp` descending.

---

## Table 2: StationState

Stores the latest known state of each station. One item per station.
This table is always overwritten (PutItem), never appended.

| Attribute | Type | Role |
|---|---|---|
| `station_id` | String | Partition key |
| `last_seen` | String (ISO 8601) | |
| `battery_soc` | Number | |
| `operating_mode` | String | |
| `active_outputs` | Number | |
| `fault_state` | String | |
| `weather_index` | Number | |
| `last_command_id` | String | |
| `last_command_status` | String | `pending`, `applied`, `timeout` |
| `firmware_version` | String | |
| `connection_status` | String | `online`, `offline` |

Access pattern: GetItem by `station_id` (single lookup, very cheap).

---

## Table 3: StationCommands

Stores every command dispatched to the ESP32 and its acknowledgment status.

| Attribute | Type | Role |
|---|---|---|
| `station_id` | String | Partition key |
| `command_id` | String | Sort key |
| `timestamp` | String (ISO 8601) | |
| `operating_mode` | String | |
| `active_outputs` | Number | |
| `tracking_allowed` | Boolean | |
| `safety_lockout` | Boolean | |
| `reason` | String | |
| `ack_status` | String | `pending`, `applied`, `timeout` |
| `ack_timestamp` | String | Filled when ACK received |
| `ttl` | Number | Unix epoch — auto-delete after 30 days |

Access pattern: query by `station_id`, sorted by `command_id` or `timestamp`.

---

## Table 4: StationFaults

Stores fault and alert events reported by the ESP32.

| Attribute | Type | Role |
|---|---|---|
| `station_id` | String | Partition key |
| `fault_id` | String | Sort key (UUID or timestamp-based) |
| `timestamp` | String (ISO 8601) | |
| `fault_type` | String | e.g. `sensor_failure`, `overcurrent`, `comms_lost` |
| `severity` | String | `warning`, `critical` |
| `description` | String | |
| `resolved` | Boolean | |
| `resolved_timestamp` | String | |
| `ttl` | Number | Unix epoch — auto-delete after 180 days |

Access pattern: query by `station_id`, filter by `resolved = false` for active faults.

---

## Table 5: StationFirmware

Stores firmware version history and OTA update records.

| Attribute | Type | Role |
|---|---|---|
| `station_id` | String | Partition key |
| `firmware_version` | String | Sort key |
| `timestamp` | String (ISO 8601) | |
| `s3_key` | String | S3 object key of the binary |
| `status` | String | `available`, `deployed`, `failed` |
| `deployed_timestamp` | String | |

Access pattern: query by `station_id` to get version history.

---

## TTL Summary

| Table | TTL field | Retention |
|---|---|---|
| StationTelemetry | `ttl` | 90 days |
| StationCommands | `ttl` | 30 days |
| StationFaults | `ttl` | 180 days |
| StationState | none | permanent (1 item per station) |
| StationFirmware | none | permanent (small table) |

---

## Free Tier estimate

At 30-second telemetry intervals from one station:
- ~2,880 writes/day to StationTelemetry
- ~86,400 writes/month
- DynamoDB Free Tier includes 25 WCU and 25 RCU (provisioned) or
  1 million write request units/month (on-demand).
- Estimated storage after 90 days with TTL: < 50 MB.
- **Estimated cost: $0/month within Free Tier.**
