# MQTT Payload Reference

## 1. Telemetry payload (ESP32 → AWS)

Published every 15–30 seconds to:
`$aws/rules/TelemetryRule/station/{station_id}/telemetry/raw`

```json
{
  "station_id": "solar_station_01",
  "timestamp": "2026-07-04T17:30:00-06:00",
  "battery_soc": 91.4,
  "battery_voltage": 12.8,
  "battery_current": -8.2,
  "p_net": -105.0,
  "local_irradiance": 620.0,
  "shortwave_radiation": 730.0,
  "cloud_cover": 25.0,
  "precipitation_probability": 10.0,
  "demand_index": 0.65,
  "active_outputs": 2,
  "tracking_angle": 34.5,
  "fault_state": "normal"
}
```

Field notes:
- `p_net` is negative when load exceeds PV generation (battery discharging).
- `local_irradiance` is measured by the local pyranometer on the ESP32.
- `shortwave_radiation`, `cloud_cover`, `precipitation_probability` are
  fetched from Open-Meteo API and cached on the ESP32 (updated hourly).
- `demand_index` is computed locally on the ESP32 from charging session history.
- `fault_state`: `"normal"`, `"warning"`, or `"critical"`.

---

## 2. Heartbeat payload (ESP32 → AWS)

Published every 60 seconds to:
`$aws/rules/HeartbeatRule/station/{station_id}/status/heartbeat`

```json
{
  "station_id": "solar_station_01",
  "timestamp": "2026-07-04T17:30:00-06:00",
  "firmware_version": "1.0.0",
  "uptime_seconds": 3600,
  "wifi_rssi": -62,
  "free_heap_bytes": 142000
}
```

---

## 3. Fault event payload (ESP32 → AWS)

Published on fault detection (event-driven) to:
`$aws/rules/FaultRule/station/{station_id}/events/fault`

```json
{
  "station_id": "solar_station_01",
  "timestamp": "2026-07-04T17:35:10-06:00",
  "fault_type": "overcurrent",
  "severity": "critical",
  "description": "Output 2 current exceeded 10A limit",
  "battery_soc": 88.1
}
```

Possible `fault_type` values:
- `sensor_failure` — a sensor returned an out-of-range or NaN value
- `overcurrent` — charging output exceeded current limit
- `low_battery` — SOC dropped below critical threshold
- `comms_lost` — MQTT connection lost for more than N minutes
- `actuator_fault` — linear actuator did not reach target position
- `overvoltage` — PV or battery voltage exceeded safe limit

---

## 4. Charging event payload (ESP32 → AWS)

Published on charging session start or end (event-driven) to:
`$aws/rules/ChargingRule/station/{station_id}/events/charging`

```json
{
  "station_id": "solar_station_01",
  "timestamp": "2026-07-04T17:40:00-06:00",
  "event_type": "session_end",
  "output_id": 2,
  "duration_minutes": 45,
  "energy_wh": 87.3
}
```

`event_type`: `"session_start"` or `"session_end"`.

---

## 5. Command ACK payload (ESP32 → AWS)

Published after the ESP32 applies a received command (event-driven) to:
`$aws/rules/AckRule/station/{station_id}/ack/command`

```json
{
  "station_id": "solar_station_01",
  "command_id": "cmd_001",
  "status": "applied",
  "timestamp": "2026-07-04T17:30:15-06:00"
}
```

`status`: `"applied"` or `"rejected"` (rejected if local safety blocked it).

---

## 6. Mode command payload (AWS → ESP32)

Published by CommandDispatcherLambda to:
`station/{station_id}/commands/mode`

```json
{
  "command_id": "cmd_001",
  "station_id": "solar_station_01",
  "operating_mode": "M3",
  "active_outputs": 1,
  "tracking_allowed": true,
  "safety_lockout": false,
  "reason": "Adequate SOC and favorable weather"
}
```

Operating modes:
- `M0` — Basic / safe mode (no outputs, no tracking)
- `M1` — Telemetry only
- `M2` — Solar tracking allowed, no charging outputs
- `M3` — One charging output active
- `M4` — Two charging outputs active
- `M5` — Three charging outputs active

---

## 7. OTA firmware update command payload (AWS → ESP32)

Published by FirmwareUpdateManagerLambda to:
`station/{station_id}/commands/ota`

```json
{
  "command_id": "ota_007",
  "station_id": "solar_station_01",
  "firmware_version": "1.1.0",
  "download_url": "https://station-firmware-bucket.s3.amazonaws.com/firmware_v1.1.0.bin?X-Amz-Signature=...",
  "file_size_bytes": 892416,
  "md5_checksum": "a3f1c2d4e5b6...",
  "timestamp": "2026-07-04T18:00:00-06:00"
}
```

Notes:
- `download_url` is a pre-signed S3 URL valid for a limited time (e.g. 1 hour).
- The ESP32 verifies `md5_checksum` after download before applying the update.
- The ESP32 publishes a fault event if the download or verification fails.
- OTA updates should only be triggered when `battery_soc` > 50% and
  `fault_state` is `"normal"`.
