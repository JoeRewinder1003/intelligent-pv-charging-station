# Architecture Notes — station_cloud_v1

## 1. System purpose

`station_cloud_v1` is the AWS cloud backend for an ESP32-based solar
charging station for low-power electric vehicles, mainly electric
scooters.

The ESP32 performs local acquisition, hardware control, and immediate
safety actions. AWS receives telemetry and events, stores historical and
current state, evaluates cloud-side decision logic, records commands, and
supports future diagnostic and maintenance functions.

The thesis scope is broader than the fuzzy-logic implementation described
in the article. The article FIS is a reference, not the complete final
definition of the thesis system.

---

## 2. Current architecture

```text
ESP32
  |
  | MQTT over TLS
  v
AWS IoT Core
  |
  +--> telemetry rule --> telemetry_processor
  |                         |
  |                         +--> TelemetryHistory
  |                         +--> partial update of StationStatus
  |
  +--> status rule ------> diagnostics
  |                         |
  |                         +--> partial update of StationStatus
  |
  +--> faults rule ------> diagnostics
  |                         |
  |                         +--> FaultEvents
  |                         +--> partial update of StationStatus
  |
  +--> acknowledgements -> diagnostics
  |                         |
  |                         +--> update existing CommandLog item
  |
  +<-- command_dispatcher publishes MQTT commands
                            |
                            +--> CommandLog
```

A separate test-only rule was used to invoke `fis_processor` from
telemetry. That rule is disabled after validation because the current FIS
implementation is preliminary.

---

## 3. MQTT topics

### Active topics

```text
station/{station_id}/telemetry
station/{station_id}/status
station/{station_id}/faults
station/{station_id}/acks
station/{station_id}/commands
```

### Planned topic

```text
station/{station_id}/battery_diagnostics
```

The planned battery-diagnostics topic is reserved for low-frequency
SmartShunt data and is not implemented yet.

---

## 4. Implemented Lambda functions

### telemetry_processor

Triggered by:

```text
station/+/telemetry
```

Responsibilities:

1. Parse and validate incoming telemetry.
2. Store each valid sample in `TelemetryHistory`.
3. Partially update the latest state in `StationStatus`.
4. Preserve attributes written by other Lambda functions.

This function does not run the cloud FIS and does not publish commands.

### diagnostics

Triggered by:

```text
station/+/status
station/+/faults
station/+/acks
```

Responsibilities:

1. Process station-status messages.
2. Store fault events in `FaultEvents`.
3. Partially update `StationStatus`.
4. Update an existing `CommandLog` item when an ESP32 acknowledgement is
   received.
5. Reject acknowledgements for command identifiers that do not exist.

### command_dispatcher

Triggered manually during the current validation stage.

Responsibilities:

1. Validate a cloud command request.
2. Create a `CommandLog` item with an initial lifecycle state.
3. Publish the command to:

```text
station/{station_id}/commands
```

4. Update the command state after MQTT publication.
5. Allow the ESP32 acknowledgement to complete the lifecycle through
   `diagnostics`.

Current command lifecycle:

```text
pending -> sent -> accepted / rejected / blocked_by_safety /
invalid_command
```

### fis_processor

Current status:

```text
Preliminary cloud implementation
```

Responsibilities:

1. Validate the required FIS inputs.
2. Evaluate the preliminary Weather FIS.
3. Evaluate the preliminary Main FIS.
4. Apply cloud-side deterministic restrictions.
5. Store the result in `FISDecisionHistory`.
6. Return a command request without publishing it.

The current `fis_processor` is not connected automatically to
`command_dispatcher`.

Its membership functions, rule base, thresholds, and final role within the
thesis architecture must still be aligned with the definitive cloud FIS.

---

## 5. Pending Lambda functions

### demand_estimator

Status:

```text
Pending physical and methodological definition
```

The adaptive demand mechanism is not implemented because the method for
detecting scooter-connection events has not been selected.

Possible future approaches include:

- User confirmation through a physical button
- Automatic detection using charging-output current
- A hybrid method combining user input and electrical confirmation

The decision depends on what physical modifications remain viable for the
station.

Until this is defined, the system may use a fixed or externally supplied
Demand Index.

### soh_estimator

Status:

```text
Pending SmartShunt physical validation and algorithm definition
```

Planned inputs include frequent battery telemetry and selected
low-frequency VE.Direct diagnostics.

The estimator must avoid increasing every MQTT telemetry payload
unnecessarily. Values that can be derived from history should be
calculated in AWS.

### actuator_life_estimator

Status:

```text
Pending implementation
```

Planned indicators include:

- Movement count
- Movement duration
- Accumulated operating time
- Estimated duty cycle
- Maintenance state

Physical validation is required before defining the definitive estimator.

### firmware_update_manager

Status:

```text
Future capability
```

OTA firmware updates are planned so the ESP32 can eventually be
reprogrammed without a wired connection.

OTA is not being implemented in the current stage because the present
focus is cloud processing, data flow, and safe command handling.

---

## 6. DynamoDB tables currently created

Region:

```text
us-east-2
```

Current tables:

| Table | Purpose | Partition key | Sort key |
| --- | --- | --- | --- |
| `TelemetryHistory` | Periodic telemetry history | `station_id` | `timestamp` |
| `StationStatus` | Latest known station state | `station_id` | None |
| `FaultEvents` | Fault and restriction history | `station_id` | `timestamp` |
| `CommandLog` | Commands and acknowledgements | `station_id` | `command_id` |
| `DemandProfile` | Fixed and future adaptive demand profile | `station_id` | `slot_id` |
| `FISDecisionHistory` | Preliminary and future FIS decisions | `station_id` | `timestamp` |

Not yet created:

```text
BatterySOHHistory
ActuatorLifeHistory
```

These tables will be created only when their corresponding estimators are
ready for meaningful testing.

---

## 7. AWS IoT rules currently created

Active rules:

```text
station_telemetry_to_lambda
station_status_to_lambda
station_faults_to_lambda
station_acks_to_lambda
```

Test-only rule:

```text
station_telemetry_to_preliminary_fis
```

The test-only FIS rule remains disabled after infrastructure validation.

---

## 8. Command authority and safety hierarchy

The cloud does not directly authorize physical actuation.

The control hierarchy is:

1. ESP32 local safety protections
2. Technician lockouts
3. Technician manual commands
4. Validated cloud commands
5. Cloud automatic recommendations
6. Local basic routines

Every cloud command must be validated again by the ESP32 before any
physical action is applied.

Therefore:

```text
Cloud result = recommendation or command request
ESP32 deterministic layer = final physical authorization
```

Loss of cloud connectivity must not disable local safety.

---

## 9. Fuzzy decision system

### Weather FIS reference inputs

```text
shortwave radiation
cloud cover
precipitation probability
```

Output:

```text
Weather Index in [0, 1]
```

### Main FIS reference inputs

```text
battery SOC
net battery power
local irradiance
Weather Index
Demand Index
```

Output:

```text
M0, M1, M2, M3, M4, or M5
```

The reference approach uses Mamdani inference and centroid
defuzzification.

The definitive thesis FIS may include additional cloud-derived
information and must be documented separately from the article reference
implementation.

Safety thresholds must not be copied from old drafts without review.
Thresholds used in preliminary tests are not automatically definitive
hardware-protection thresholds.

---

## 10. SmartShunt and payload strategy

Frequent battery telemetry should remain limited to the measurements
needed for current supervision and control:

```text
voltage_v
current_a
power_w
soc_percent
```

Low-frequency diagnostic values, such as consumed amp-hours, should use a
separate payload when physically validated.

Design constraints include:

- MQTT payload size
- ESP32 JSON memory
- PubSubClient buffer size
- ESP32 RAM and flash usage
- Telemetry frequency
- DynamoDB storage volume
- Lambda invocation rate
- Compatibility with commands and acknowledgements

New SmartShunt fields must not be added indiscriminately to every
telemetry message.

---

## 11. Current physical-validation status

At the current stage, only the ESP32 is available.

The following validations remain pending until access to the station
hardware is restored:

- Real SmartShunt VE.Direct acquisition
- Battery-diagnostics publication
- Charging-session detection
- Real output-current measurements
- Actuator usage measurements
- Physical command application
- Final local safety tests

Simulated values may be used for cloud infrastructure tests, but they must
be clearly identified as simulated.

---

## 12. Current development priorities

The current priorities are:

1. Keep the deployed AWS architecture consistent and documented.
2. Strengthen local and cloud tests.
3. Define the definitive thesis FIS and its cloud inputs.
4. Preserve the ESP32 as final safety authority.
5. Keep pending physical interfaces prepared but inactive.
6. Avoid implementing OTA, adaptive demand, SOH, or actuator-life logic
   before their required data and validation methods are available.
