# AWS Lambda Function Design

This document defines the initial AWS Lambda functions used by the cloud backend of the ESP32-based solar charging station.

The cloud backend receives telemetry from AWS IoT Core, stores operational data in DynamoDB, evaluates fuzzy decision logic, generates cloud-side command requests, and records diagnostic information. However, the ESP32 remains responsible for final local safety validation before applying any physical command.

---

## 1. telemetry_processor

### Path

```text
aws/lambdas/telemetry_processor/
```

### Purpose

Processes periodic telemetry messages sent by the ESP32 through AWS IoT Core.

### Input Source

```text
station/{station_id}/telemetry
```

### Main Responsibilities

* Receive telemetry payloads from AWS IoT Core.
* Validate required fields.
* Normalize numeric values.
* Store telemetry samples in `TelemetryHistory`.
* Update the latest station state in `StationStatus`.
* Prepare selected variables for cloud-side FIS evaluation.

### DynamoDB Tables Used

| Table              | Operation                   |
| ------------------ | --------------------------- |
| `TelemetryHistory` | Write telemetry sample      |
| `StationStatus`    | Update latest station state |

### Output

The validated telemetry data can be passed to the `fis_processor` or stored for later processing.

---

## 2. fis_processor

### Path

```text
aws/lambdas/fis_processor/
```

### Purpose

Evaluates the fuzzy decision system in the cloud.

### Input Source

Validated telemetry data from `telemetry_processor`, DynamoDB, or an AWS IoT Rule.

### Main Responsibilities

* Evaluate the Weather FIS.
* Evaluate the Main Decision FIS.
* Use battery SOC, net battery power, local irradiance, Weather Index, and Demand Index as decision inputs.
* Generate the fuzzy recommended operating mode.
* Apply cloud-side deterministic validation.
* Store the decision result in `FISDecisionHistory`.
* Prepare a command request for the `command_dispatcher`.

### FIS Inputs

| Input                  | Description                                      |
| ---------------------- | ------------------------------------------------ |
| `soc_percent`          | Battery state of charge                          |
| `p_net_w`              | Net battery power                                |
| `local_irradiance_wm2` | Measured local solar irradiance                  |
| `weather_index`        | Output of the Weather FIS                        |
| `demand_index`         | Demand estimation from fixed or adaptive profile |

### FIS Output

| Output     | Description                              |
| ---------- | ---------------------------------------- |
| `fis_mode` | Recommended operating mode from M0 to M5 |

### DynamoDB Tables Used

| Table                | Operation                         |
| -------------------- | --------------------------------- |
| `TelemetryHistory`   | Read recent telemetry if needed   |
| `DemandProfile`      | Read current demand slot          |
| `FISDecisionHistory` | Write FIS decision                |
| `StationStatus`      | Read/update latest decision state |

### Output

The function produces a cloud-side operating request such as:

```text
M0, M1, M2, M3, M4, or M5
```

This request is sent to the `command_dispatcher`.

---

## 3. command_dispatcher

### Path

```text
aws/lambdas/command_dispatcher/
```

### Purpose

Publishes cloud-generated commands to the ESP32 using AWS IoT Core MQTT topics.

### Input Source

Command requests from `fis_processor`, diagnostics logic, or a manual cloud interface.

### Main Responsibilities

* Receive a command request.
* Create a unique `command_id`.
* Store the command in `CommandLog`.
* Publish the command to the MQTT command topic.
* Mark the command as sent.

### MQTT Publish Topic

```text
station/{station_id}/commands
```

### DynamoDB Tables Used

| Table           | Operation                             |
| --------------- | ------------------------------------- |
| `CommandLog`    | Write command record                  |
| `StationStatus` | Optional read of latest station state |

### Output

Example command:

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

The ESP32 must validate the command locally before applying it.

---

## 4. diagnostics

### Path

```text
aws/lambdas/diagnostics/
```

### Purpose

Processes fault messages, command acknowledgements, invalid data reports, and station diagnostic events.

### Input Sources

```text
station/{station_id}/faults
station/{station_id}/acks
station/{station_id}/status
```

### Main Responsibilities

* Store fault events in `FaultEvents`.
* Update command status in `CommandLog`.
* Detect command timeouts.
* Detect stale telemetry.
* Detect repeated warnings or critical lockout conditions.
* Update `StationStatus` with the latest fault state.

### DynamoDB Tables Used

| Table           | Operation                      |
| --------------- | ------------------------------ |
| `FaultEvents`   | Write fault event              |
| `CommandLog`    | Update command acknowledgement |
| `StationStatus` | Update latest diagnostic state |

### Output

The function may trigger alerts, logs, or additional lockout commands through `command_dispatcher`.

---

## 5. demand_estimator

### Path

```text
aws/lambdas/demand_estimator/
```

### Purpose

Updates or estimates the Demand Index used by the Main Decision FIS.

### Main Responsibilities

* Read the current time slot.
* Retrieve the default demand value from `DemandProfile`.
* Analyze historical charging activity.
* Estimate adaptive demand using output current events.
* Update the adaptive demand profile when enough data is available.

### DynamoDB Tables Used

| Table              | Operation                       |
| ------------------ | ------------------------------- |
| `DemandProfile`    | Read/update demand values       |
| `TelemetryHistory` | Read historical output activity |

### Output

The function returns a value:

```text
demand_index ∈ [0, 1]
```

For the first thesis implementation, the system may use a fixed demand table. Adaptive demand estimation can be implemented as a future improvement after enough historical data is collected.

---

## 6. soh_estimator

### Path

```text
aws/lambdas/soh_estimator/
```

### Purpose

Estimates battery state of health using long-term SmartShunt data.

### Main Responsibilities

* Analyze battery voltage, current, SOC, power, and energy throughput.
* Estimate accumulated charge/discharge cycles.
* Detect abnormal SOC behavior.
* Store SOH estimation results.

### DynamoDB Tables Used

| Table               | Operation                    |
| ------------------- | ---------------------------- |
| `TelemetryHistory`  | Read historical battery data |
| `BatterySOHHistory` | Write SOH estimate           |

### Output

Example output:

```json
{
  "estimated_soh_percent": 96.5,
  "cycle_estimate": 18.4,
  "confidence": "low"
}
```

In the first version, the confidence may be low because SOH estimation requires long-term historical data.

---

## 7. actuator_life_estimator

### Path

```text
aws/lambdas/actuator_life_estimator/
```

### Purpose

Estimates actuator usage and maintenance condition based on tracking movement history.

### Main Responsibilities

* Count actuator movements.
* Accumulate movement time.
* Estimate duty cycle.
* Detect excessive usage.
* Store actuator lifetime indicators.

### DynamoDB Tables Used

| Table                 | Operation                           |
| --------------------- | ----------------------------------- |
| `TelemetryHistory`    | Read tracking and actuator activity |
| `ActuatorLifeHistory` | Write actuator usage estimate       |

### Output

Example output:

```json
{
  "actuator_group": "elevation_tracking",
  "daily_movement_time_s": 126.0,
  "accumulated_movement_time_s": 48250.0,
  "estimated_duty_cycle_percent": 18.0,
  "maintenance_state": "normal"
}
```

---

## 8. firmware_update_manager

### Path

```text
aws/lambdas/firmware_update_manager/
```

### Purpose

Reserved for future firmware update management.

### Main Responsibilities

* Register firmware versions.
* Validate available firmware metadata.
* Notify the station of available updates.
* Track update status.

### Status

This function is optional and should not be implemented in the first prototype unless required.

---

## 9. Minimum Lambda Set for First Prototype

The minimum recommended functions for the first cloud prototype are:

```text
telemetry_processor
fis_processor
command_dispatcher
diagnostics
```

The advanced thesis functions are:

```text
demand_estimator
soh_estimator
actuator_life_estimator
```

The optional future function is:

```text
firmware_update_manager
```

---

## 10. Cloud-to-Local Safety Rule

The cloud backend can estimate, recommend, and request operating modes. However, it must not directly bypass the local safety layer.

Therefore:

```text
Cloud FIS = decision recommendation
Command Dispatcher = MQTT command request
ESP32 deterministic layer = final physical authorization
```

This ensures that communication delays, cloud errors, or invalid commands cannot directly activate unsafe hardware states.
