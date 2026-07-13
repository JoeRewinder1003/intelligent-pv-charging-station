# SmartShunt Data Plan

## Purpose

This document defines which Victron SmartShunt measurements will be sent
to AWS and how frequently they will be transmitted.

The design prioritizes:

- Low MQTT payload size
- Low ESP32 memory use
- Reliable real-time control
- Sufficient historical data for future battery SOH estimation
- Separation between frequent telemetry and low-frequency diagnostics

## Confirmed VE.Direct fields

| VE.Direct key | Cloud field | Unit |
| --- | --- | --- |
| `V` | `voltage_v` | V |
| `I` | `current_a` | A |
| `P` | `power_w` | W |
| `SOC` | `soc_percent` | % |
| `CE` | `consumed_ah` | Ah |

## Frequent telemetry

The following values remain in the normal telemetry payload:

```json
{
  "battery": {
    "voltage_v": 12.7,
    "current_a": -5.2,
    "power_w": -66.0,
    "soc_percent": 92.5
  }
}
```

Recommended publication interval:

```text
15 to 30 seconds
```

These fields are required for:

- Current station monitoring
- Cloud FIS inputs
- Fault detection
- Energy balance
- Current-state storage

## Low-frequency diagnostics

The accumulated consumed amp-hour value will not initially be included
in every telemetry message.

It will be sent in a separate diagnostic payload:

```json
{
  "station_id": "station_001",
  "timestamp": "2026-07-13T21:00:00Z",
  "battery_diagnostics": {
    "consumed_ah": -24.6
  },
  "source": "smartshunt_vedirect"
}
```

Recommended initial publication interval:

```text
5 minutes
```

It may also be published when the value changes by a configured threshold.

## MQTT topic

```text
station/{station_id}/battery_diagnostics
```

## Cloud-derived values

The following values should be calculated in AWS rather than transmitted
by the ESP32 whenever possible:

- Charged energy
- Discharged energy
- Energy throughput
- Equivalent full cycles
- Time spent in SOC ranges
- Voltage trends under similar loads
- Preliminary capacity estimates

## Payload design rule

New SmartShunt fields must not be added automatically to every telemetry
message.

Before adding a field, confirm:

1. It is available reliably through VE.Direct.
2. It is required by a cloud or local function.
3. It cannot be derived from existing historical data.
4. Its transmission frequency is justified.
5. The ESP32 MQTT buffer and JSON memory remain sufficient.
6. Existing telemetry, command, and acknowledgement functions still work.

## Current implementation status

- Frequent SmartShunt telemetry fields are defined.
- `consumed_ah` is reserved for low-frequency diagnostics.
- The physical VE.Direct integration still needs to be incorporated into
  the AWS firmware.
- The SOH algorithm is not yet implemented.
- `BatterySOHHistory` has not yet been created.
