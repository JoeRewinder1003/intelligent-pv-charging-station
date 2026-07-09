# MQTT Topic Structure

This document defines the MQTT topic structure used by the ESP32-based solar charging station and the AWS cloud backend.

## General Naming Convention

All station-specific topics follow this format:

```text
station/{station_id}/{message_type}
```

Example:

```text
station/station_001/telemetry
```

The `{station_id}` field uniquely identifies the charging station. For the first prototype, the default station ID is:

```text
station_001
```

## Main MQTT Topics

| Topic                            | Direction   | Publisher                 | Subscriber                    | Purpose                                               |
| -------------------------------- | ----------- | ------------------------- | ----------------------------- | ----------------------------------------------------- |
| `station/{station_id}/telemetry` | ESP32 → AWS | ESP32                     | AWS IoT Rule / Lambda         | Periodic sensor and energy data                       |
| `station/{station_id}/status`    | ESP32 → AWS | ESP32                     | AWS IoT Rule / Lambda         | Current operating mode and system state               |
| `station/{station_id}/faults`    | ESP32 → AWS | ESP32                     | AWS IoT Rule / Lambda         | Fault events, safety lockouts, and invalid data flags |
| `station/{station_id}/commands`  | AWS → ESP32 | Command Dispatcher Lambda | ESP32                         | Cloud-generated operating commands                    |
| `station/{station_id}/acks`      | ESP32 → AWS | ESP32                     | Diagnostics Lambda / DynamoDB | Acknowledgement of received commands                  |
| `station/{station_id}/config`    | AWS → ESP32 | AWS backend               | ESP32                         | Future configuration updates                          |

## Telemetry Topic

```text
station/{station_id}/telemetry
```

This topic is used for periodic data sent from the ESP32 to AWS. The telemetry payload includes battery data, PV data, environmental measurements, output states, and selected decision variables.

Suggested publishing interval:

```text
15 seconds
```

## Status Topic

```text
station/{station_id}/status
```

This topic reports the current state of the station, including the active operating mode, requested mode, FIS recommendation, active outputs, and control state.

## Faults Topic

```text
station/{station_id}/faults
```

This topic is used when the ESP32 detects a relevant event such as:

* Critical safety lockout
* Invalid sensor data
* Low-battery protection
* Output blocking
* Tracking inhibition
* Stale data
* Manual fault condition

## Commands Topic

```text
station/{station_id}/commands
```

This topic is used by the cloud backend to send commands to the station.

Possible command types include:

```text
AUTO
STOP
NEUTRAL
ENABLE_TRACKING
DISABLE_TRACKING
ENABLE_OUTPUT_1
ENABLE_OUTPUT_2
ENABLE_OUTPUT_3
DISABLE_OUTPUTS
LOCKOUT
CLEAR_LOCKOUT
```

The ESP32 must validate every received command before applying it. Cloud commands should never bypass the local deterministic safety layer.

## Acknowledgement Topic

```text
station/{station_id}/acks
```

This topic is used by the ESP32 to confirm whether a cloud command was received, accepted, rejected, or blocked by local safety logic.

## Configuration Topic

```text
station/{station_id}/config
```

This topic is reserved for future configuration updates, such as:

* SOC thresholds
* Output current limits
* Demand table parameters
* Tracking limits
* Telemetry interval
* FIS parameter updates

Configuration messages should be versioned and validated before being applied.
