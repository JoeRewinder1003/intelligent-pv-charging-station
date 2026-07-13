# Thesis Cloud FIS Scope and Decision Record

## 1. Purpose

This document separates the fuzzy-logic reference used in the article
from the definitive cloud decision system required by the thesis.

The article implementation is a functional reference. It is not the
complete definition of the thesis cloud system.

The definitive thesis design may include additional cloud information,
historical analysis, diagnostic indicators, and orchestration rules.
However, the ESP32 remains responsible for final local safety
authorization.

---

## 2. Current reference structure

### Weather FIS

Reference inputs:

- Shortwave radiation
- Cloud cover
- Precipitation probability

Reference output:

```text
Weather Index in [0, 1]
```

Reference method:

- Mamdani inference
- Triangular and trapezoidal membership functions
- Centroid defuzzification

### Main FIS

Reference inputs:

- Battery SOC
- Net battery power
- Local irradiance
- Weather Index
- Demand Index

Reference output:

```text
Operating-mode recommendation: M0, M1, M2, M3, M4, or M5
```

The current `fis_processor` Lambda implements a preliminary version of
this structure. Its membership functions, rule base, thresholds, and
final thesis role are not yet definitive.

---

## 3. Operating-mode semantics

The current reference interpretation is:

| Mode | Reference meaning |
| --- | --- |
| `M0` | Critical protection / lockout |
| `M1` | Basic operation or telemetry-only state |
| `M2` | Tracking allowed, charging outputs disabled |
| `M3` | One charging output allowed |
| `M4` | Two charging outputs allowed |
| `M5` | Three charging outputs allowed |

These meanings must be reviewed before the definitive cloud FIS is
connected automatically to `command_dispatcher`.

---

## 4. Data groups for the thesis

### 4.1 Direct FIS inputs

The following are current candidate direct inputs:

- Battery SOC
- Net battery power
- Local irradiance
- Weather Index
- Demand Index

### 4.2 Deterministic validation inputs

The following should initially remain outside the fuzzy inference and be
used as deterministic restrictions:

- Critical fault state
- Invalid or missing sensor data
- Stale telemetry
- Technician lockout
- Manual operating restrictions
- MQTT or cloud connectivity state
- Local hardware protection state
- Output overcurrent state
- Actuator fault state

### 4.3 Cloud-derived supervisory indicators

The thesis may later add:

- Battery SOH estimate
- Battery-health confidence
- Actuator lifetime or maintenance state
- Adaptive demand estimate
- Historical energy availability
- Historical charging-session behavior
- Forecast reliability
- Data-quality score

Each indicator must be assigned explicitly to one of these roles:

1. Direct fuzzy input
2. Deterministic restriction
3. Scheduling or maintenance information only
4. Stored diagnostic information with no immediate control effect

No indicator should be added to the FIS only because it is available.

---

## 5. Net battery power convention

The preliminary convention is:

```text
Positive battery power = battery charging
Negative battery power = battery discharging
```

This matches the intended SmartShunt sign interpretation, but it must be
confirmed during physical VE.Direct validation.

All simulation, test events, DynamoDB records, membership functions, and
documentation must use the same convention.

---

## 6. Demand Index status

Current status:

```text
Pending physical and methodological definition
```

Possible demand-event detection methods include:

- User confirmation through a physical button
- Automatic detection using charging-output current
- Hybrid user confirmation plus electrical verification

Until the method is defined and physically validated:

- The Demand Index may remain fixed or externally supplied.
- `demand_estimator` remains pending.
- Adaptive updates to `DemandProfile` remain disabled.

---

## 7. Battery SOH status

Current status:

```text
Pending SmartShunt physical validation and algorithm definition
```

Battery SOH must not be treated as a reliable FIS input until:

- Real VE.Direct measurements are validated.
- The historical data window is defined.
- The estimation method is documented.
- An uncertainty or confidence value is available.

A preliminary SOH estimate may initially be used only for monitoring and
maintenance, not for automatic mode changes.

---

## 8. Actuator-life status

Current status:

```text
Pending physical validation and estimator design
```

Candidate indicators include:

- Movement count
- Movement duration
- Accumulated movement time
- Duty-cycle estimate
- Position mismatch events
- Maintenance state

Actuator lifetime should initially influence maintenance reporting or
tracking restrictions rather than charging-output authorization.

---

## 9. Deterministic layer and local authority

The cloud decision process must follow:

```text
Cloud data validation
→ Weather FIS
→ Main FIS
→ cloud-side deterministic restrictions
→ command request
→ command_dispatcher
→ MQTT
→ ESP32 local validation
→ physical authorization or rejection
```

The ESP32 has final authority over:

- Charging-output activation
- Tracking movement
- Safety relay state
- Critical lockout
- Overcurrent response
- Sensor-fault response
- Loss-of-connectivity fallback

A cloud command is never equivalent to physical authorization.

---

## 10. Anti-chattering and command dispatch

The definitive system must avoid repeated or rapid mode changes.

The following decisions remain open:

- Minimum dwell time between operating-mode changes
- Whether dwell time is enforced in AWS, ESP32, or both
- Whether repeated commands with the same requested mode are suppressed
- Maximum command retry count
- ACK timeout
- Recovery behavior after rejected or blocked commands

Until these are defined, the preliminary FIS must not dispatch commands
automatically.

---

## 11. Invocation cadence

The definitive FIS should not necessarily run for every telemetry sample.

Candidate strategies:

- Every telemetry message
- Fixed interval, such as once per minute
- Only after meaningful input changes
- Hybrid interval plus event-based execution

The selected strategy must consider:

- ESP32 telemetry interval
- Lambda invocation volume
- DynamoDB write volume
- Weather-data update frequency
- Demand-profile update frequency
- Anti-chattering requirements
- Command traffic
- Decision latency

---

## 12. Payload and storage constraints

Additional fields must be evaluated for:

- MQTT payload size
- ESP32 JSON memory
- PubSubClient buffer size
- ESP32 RAM and flash usage
- Lambda processing
- DynamoDB item size and write volume
- Network reliability
- Compatibility with commands and acknowledgements

Frequent telemetry should carry only values required for current
supervision and control.

Accumulated or diagnostic values should use lower-frequency payloads
where practical.

---

## 13. Current implementation status

| Component | Status |
| --- | --- |
| `telemetry_processor` | Implemented and deployed |
| `diagnostics` | Implemented and deployed |
| `command_dispatcher` | Implemented and deployed |
| `fis_processor` | Preliminary, deployed, automatic rule disabled |
| `demand_estimator` | Pending |
| `soh_estimator` | Pending |
| `actuator_life_estimator` | Pending |
| `firmware_update_manager` | Future OTA capability |
| Automatic FIS-to-command flow | Disabled |
| ESP32 physical validation | Pending hardware access |

---

## 14. Decisions required before definitive implementation

The following decisions must be resolved and documented:

1. Final meaning of modes `M0` through `M5`
2. Final SOC membership functions and protection thresholds
3. Final net-power universe and sign convention
4. Final irradiance membership functions
5. Final Weather Index functions and rules
6. Final Main FIS rule base
7. Demand Index source during the first physical prototype
8. Role of SOH in control
9. Role of actuator health in control
10. Dwell-time location and duration
11. FIS invocation cadence
12. Conditions that justify publishing a new command
13. ACK timeout and retry behavior
14. Stale-data threshold
15. Safe behavior after cloud disconnection

---

## 15. Immediate next decision block

The next design block should resolve only the core FIS reference:

- Exact meaning of `M0` to `M5`
- Battery-power sign convention
- Required inputs for the first thesis prototype
- Variables that remain deterministic rather than fuzzy
- Whether the first prototype uses a fixed Demand Index

SOH, actuator-life estimation, adaptive demand, and OTA should remain
separate until their data paths and physical validation are available.
