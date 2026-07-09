# MQTT Payload Definitions

This document defines the initial JSON payloads exchanged between the ESP32 solar charging station and the AWS cloud backend.

## 1. Telemetry Payload

Topic:

```text
station/{station_id}/telemetry
```

Direction:

```text
ESP32 → AWS
```

Example:

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
    "target_angle_deg": 30.0,
    "master_position_raw": 2040,
    "slave_position_raw": 2025
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

## 2. Status Payload

Topic:

```text
station/{station_id}/status
```

Direction:

```text
ESP32 → AWS
```

Example:

```json
{
  "station_id": "station_001",
  "timestamp": "2026-07-09T21:00:00Z",
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

## 3. Fault Payload

Topic:

```text
station/{station_id}/faults
```

Direction:

```text
ESP32 → AWS
```

Example:

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

Suggested fault severity levels:

```text
info
warning
critical
```

Suggested fault states:

```text
normal
non_critical_restriction
data_or_sensor_fault
critical_lockout
```

## 4. Command Payload

Topic:

```text
station/{station_id}/commands
```

Direction:

```text
AWS → ESP32
```

Example:

```json
{
  "station_id": "station_001",
  "timestamp": "2026-07-09T21:00:00Z",
  "command_id": "cmd-20260709-0001",
  "command": "ENABLE_OUTPUT_2",
  "source": "cloud_fis",
  "parameters": {
    "requested_mode": "M4",
    "max_outputs": 2,
    "tracking_allowed": true
  }
}
```

The ESP32 must validate every command locally before applying it. A command can be rejected if it violates local safety conditions.

## 5. Acknowledgement Payload

Topic:

```text
station/{station_id}/acks
```

Direction:

```text
ESP32 → AWS
```

Example:

```json
{
  "station_id": "station_001",
  "timestamp": "2026-07-09T21:00:00Z",
  "command_id": "cmd-20260709-0001",
  "command": "ENABLE_OUTPUT_2",
  "status": "accepted",
  "applied": true,
  "resulting_operating_mode": "M4",
  "message": "Command accepted and applied."
}
```

Possible acknowledgement statuses:

```text
received
accepted
rejected
blocked_by_safety
invalid_command
```

## 6. Design Rule

The cloud backend can recommend or request an operating state, but the ESP32 local deterministic safety layer has final authority over the physical system.

Therefore:

```text
Cloud decision = recommendation or request
ESP32 deterministic layer = final authorization
```
