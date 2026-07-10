# DynamoDB Table Design

This document defines the initial DynamoDB table structure for the AWS cloud backend of the ESP32-based solar charging station.

The database is designed to store telemetry, system status, fault events, cloud commands, demand profiles, and long-term diagnostic data. The first prototype uses a single station identified as:

```text
station_001
```

However, the table structure allows future expansion to multiple charging stations.

---

## 1. TelemetryHistory Table

### Purpose

Stores periodic telemetry sent by the ESP32, including battery data, PV measurements, environmental variables, output currents, tracking data, and decision variables.

### Table Name

```text
TelemetryHistory
```

### Keys

| Key          | Type          | Description                                |
| ------------ | ------------- | ------------------------------------------ |
| `station_id` | Partition key | Unique identifier of the charging station  |
| `timestamp`  | Sort key      | ISO 8601 timestamp of the telemetry sample |

### Example Item

```json
{
  "station_id": "station_001",
  "timestamp": "2026-07-09T21:00:00Z",
  "battery": {
    "voltage_v": 12.7,
    "current_a": -5.2,
    "power_w": -66.0,
    "soc_percent": 92.5
  },
  "pv": {
    "voltage_v": 19.8,
    "current_a": 12.4,
    "power_w": 245.5,
    "local_irradiance_wm2": 720.0
  },
  "environment": {
    "ambient_temperature_c": 28.4,
    "relative_humidity_percent": 46.0,
    "panel_temperature_c": 45.3
  },
  "outputs": {
    "output_1_active": true,
    "output_2_active": true,
    "output_3_active": false,
    "output_1_current_a": 1.62,
    "output_2_current_a": 1.58,
    "output_3_current_a": 0.0
  },
  "tracking": {
    "enabled": true,
    "angle_deg": 28.5,
    "target_angle_deg": 30.0
  },
  "decision": {
    "weather_index": 0.78,
    "demand_index": 0.62,
    "fis_mode": "M4",
    "requested_mode": "M4",
    "operating_mode": "M4"
  },
  "fault_state": "normal"
}
```

---

## 2. StationStatus Table

### Purpose

Stores the most recent known state of each charging station. Unlike `TelemetryHistory`, this table keeps only the latest status per station.

### Table Name

```text
StationStatus
```

### Keys

| Key          | Type          | Description                               |
| ------------ | ------------- | ----------------------------------------- |
| `station_id` | Partition key | Unique identifier of the charging station |

### Example Item

```json
{
  "station_id": "station_001",
  "last_update": "2026-07-09T21:00:00Z",
  "system_state": "AUTO",
  "fis_mode": "M4",
  "requested_mode": "M4",
  "operating_mode": "M4",
  "outputs_active": 2,
  "tracking_allowed": true,
  "charging_allowed": true,
  "manual_lock": false,
  "cloud_connected": true,
  "fault_state": "normal"
}
```

---

## 3. FaultEvents Table

### Purpose

Stores fault events, restrictions, safety lockouts, invalid data conditions, and other diagnostic events.

### Table Name

```text
FaultEvents
```

### Keys

| Key          | Type          | Description                               |
| ------------ | ------------- | ----------------------------------------- |
| `station_id` | Partition key | Unique identifier of the charging station |
| `timestamp`  | Sort key      | ISO 8601 timestamp of the fault event     |

### Example Item

```json
{
  "station_id": "station_001",
  "timestamp": "2026-07-09T21:00:00Z",
  "fault_state": "non_critical_restriction",
  "fault_code": "LOW_SOC_TRACKING_INHIBIT",
  "severity": "warning",
  "description": "Tracking was inhibited because the battery SOC is below the configured threshold.",
  "affected_functions": {
    "tracking": true,
    "charging_outputs": false,
    "cloud_commands": false
  },
  "measurements": {
    "soc_percent": 84.7,
    "battery_power_w": -55.0,
    "local_irradiance_wm2": 390.0
  }
}
```

### Suggested Severity Levels

```text
info
warning
critical
```

### Suggested Fault States

```text
normal
non_critical_restriction
data_or_sensor_fault
critical_lockout
```

---

## 4. CommandLog Table

### Purpose

Stores cloud-generated commands and ESP32 acknowledgements. This table allows the system to verify whether a command was sent, received, accepted, rejected, or blocked by the local safety layer.

### Table Name

```text
CommandLog
```

### Keys

| Key          | Type          | Description                               |
| ------------ | ------------- | ----------------------------------------- |
| `station_id` | Partition key | Unique identifier of the charging station |
| `command_id` | Sort key      | Unique identifier of the command          |

### Example Item

```json
{
  "station_id": "station_001",
  "command_id": "cmd-20260709-0001",
  "created_at": "2026-07-09T21:00:00Z",
  "command": "ENABLE_OUTPUT_2",
  "source": "cloud_fis",
  "parameters": {
    "requested_mode": "M4",
    "max_outputs": 2,
    "tracking_allowed": true
  },
  "status": "accepted",
  "applied": true,
  "ack_timestamp": "2026-07-09T21:00:02Z",
  "resulting_operating_mode": "M4",
  "message": "Command accepted and applied."
}
```

### Suggested Command Status Values

```text
created
sent
received
accepted
rejected
blocked_by_safety
invalid_command
timeout
```

---

## 5. DemandProfile Table

### Purpose

Stores the default and adaptive demand table used to estimate the Demand Index. The initial version can use a fixed demand table, while future versions may update the table based on historical charging events.

### Table Name

```text
DemandProfile
```

### Keys

| Key          | Type          | Description                               |
| ------------ | ------------- | ----------------------------------------- |
| `station_id` | Partition key | Unique identifier of the charging station |
| `slot_id`    | Sort key      | Time slot identifier                      |

### Slot Format

The demand table can use 48 half-hour slots per day:

```text
day_{0-6}_slot_{0-47}
```

Where:

```text
day_0 = Monday
day_1 = Tuesday
day_2 = Wednesday
day_3 = Thursday
day_4 = Friday
day_5 = Saturday
day_6 = Sunday
```

### Example Item

```json
{
  "station_id": "station_001",
  "slot_id": "day_0_slot_18",
  "day": "Monday",
  "slot_index": 18,
  "start_time": "09:00",
  "end_time": "09:30",
  "default_demand_index": 0.60,
  "adaptive_demand_index": 0.64,
  "sample_count": 25,
  "last_updated": "2026-07-09T21:00:00Z"
}
```

---

## 6. BatterySOHHistory Table

### Purpose

Stores battery health indicators estimated from SmartShunt measurements and long-term operating data. The Victron SmartShunt does not directly provide a complete SOH value in the standard telemetry fields, so SOH should be estimated using historical voltage, current, SOC, charge/discharge behavior, and energy throughput.

### Table Name

```text
BatterySOHHistory
```

### Keys

| Key          | Type          | Description                               |
| ------------ | ------------- | ----------------------------------------- |
| `station_id` | Partition key | Unique identifier of the charging station |
| `timestamp`  | Sort key      | ISO 8601 timestamp of the SOH estimate    |

### Example Item

```json
{
  "station_id": "station_001",
  "timestamp": "2026-07-09T21:00:00Z",
  "estimated_soh_percent": 96.5,
  "soc_percent": 92.5,
  "battery_voltage_v": 12.7,
  "battery_current_a": -5.2,
  "energy_throughput_wh": 15420.0,
  "cycle_estimate": 18.4,
  "estimation_method": "energy_throughput_and_soc_trend",
  "confidence": "low"
}
```

---

## 7. ActuatorLifeHistory Table

### Purpose

Stores actuator usage statistics, including movement time, estimated duty cycle, number of movements, and accumulated operating time. This table supports future maintenance estimation.

### Table Name

```text
ActuatorLifeHistory
```

### Keys

| Key          | Type          | Description                                     |
| ------------ | ------------- | ----------------------------------------------- |
| `station_id` | Partition key | Unique identifier of the charging station       |
| `timestamp`  | Sort key      | ISO 8601 timestamp of the actuator usage update |

### Example Item

```json
{
  "station_id": "station_001",
  "timestamp": "2026-07-09T21:00:00Z",
  "actuator_group": "elevation_tracking",
  "movement_direction": "extend",
  "movement_time_s": 4.6,
  "daily_movement_time_s": 126.0,
  "accumulated_movement_time_s": 48250.0,
  "movement_count": 12,
  "estimated_duty_cycle_percent": 18.0,
  "maintenance_state": "normal"
}
```

---

## 8. FISDecisionHistory Table

### Purpose

Stores the fuzzy decision results generated by the cloud backend. This allows the thesis to compare input conditions, fuzzy recommendations, deterministic validation, and final operating modes.

### Table Name

```text
FISDecisionHistory
```

### Keys

| Key          | Type          | Description                               |
| ------------ | ------------- | ----------------------------------------- |
| `station_id` | Partition key | Unique identifier of the charging station |
| `timestamp`  | Sort key      | ISO 8601 timestamp of the FIS decision    |

### Example Item

```json
{
  "station_id": "station_001",
  "timestamp": "2026-07-09T21:00:00Z",
  "inputs": {
    "soc_percent": 92.5,
    "p_net_w": 178.0,
    "local_irradiance_wm2": 720.0,
    "weather_index": 0.78,
    "demand_index": 0.62
  },
  "weather_fis_output": {
    "weather_index": 0.78
  },
  "main_fis_output": {
    "fis_mode": "M4"
  },
  "deterministic_validation": {
    "requested_mode": "M4",
    "tracking_allowed": true,
    "charging_allowed": true,
    "blocked_reason": null
  },
  "final_decision": {
    "operating_mode": "M4",
    "outputs_active": 2
  }
}
```

---

## 9. Initial Table Summary

| Table                 | Main Purpose                        | Partition Key | Sort Key     |
| --------------------- | ----------------------------------- | ------------- | ------------ |
| `TelemetryHistory`    | Periodic sensor and energy data     | `station_id`  | `timestamp`  |
| `StationStatus`       | Latest station state                | `station_id`  | None         |
| `FaultEvents`         | Fault and restriction history       | `station_id`  | `timestamp`  |
| `CommandLog`          | Cloud commands and acknowledgements | `station_id`  | `command_id` |
| `DemandProfile`       | Default and adaptive demand table   | `station_id`  | `slot_id`    |
| `BatterySOHHistory`   | Estimated battery health history    | `station_id`  | `timestamp`  |
| `ActuatorLifeHistory` | Actuator usage and lifetime data    | `station_id`  | `timestamp`  |
| `FISDecisionHistory`  | Cloud FIS decision history          | `station_id`  | `timestamp`  |

---

## 10. Design Notes

The database separates raw telemetry, current station status, faults, commands, and long-term analysis data.

The ESP32 remains responsible for local safety validation. The cloud backend stores data, estimates demand, evaluates fuzzy decision logic, sends command requests, and records whether the ESP32 accepted or rejected those commands.

For the first thesis implementation, the minimum required tables are:

```text
TelemetryHistory
StationStatus
FaultEvents
CommandLog
DemandProfile
FISDecisionHistory
```

The following tables can be added during the advanced diagnostic stage:

```text
BatterySOHHistory
ActuatorLifeHistory
```
