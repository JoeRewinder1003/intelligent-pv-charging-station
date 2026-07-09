# Architecture Notes — station_cloud_v1

## System overview

A solar photovoltaic charging station for low-power electric vehicles
(primarily electric scooters) controlled locally by an ESP32 and supervised
by an AWS cloud backend.

Communication between the ESP32 and AWS uses MQTT over TLS through
AWS IoT Core. The ESP32 publishes telemetry and events; AWS processes
the data, runs fuzzy logic decision systems, validates safety, and
publishes commands back to the ESP32.

---

## Architecture diagram (text)

```
ESP32 (local controller)
  │
  │  Publishes every 15-30s (Basic Ingest)
  ├──────────────────────────────────────────► AWS IoT Core
  │  telemetry/raw                                   │
  │  events/fault                              IoT Rules (SQL)
  │  events/charging                                 │
  │  status/heartbeat                    ┌───────────┴───────────┐
  │  ack/command                         ▼                       ▼
  │                          StationTelemetryProcessor   StationDiagnostics
  │                                Lambda                    Lambda
  │                                    │                       │
  │                          ┌─────────┼──────────┐           │
  │                          ▼         ▼          ▼           ▼
  │                     Weather FIS  Main FIS  Safety    DynamoDB
  │                          │         │       Check    (Faults,
  │                          └────┬────┘         │      Commands,
  │                               ▼              │      State)
  │                        CommandDispatcher ◄───┘
  │                            Lambda
  │                               │
  │  Subscribes to commands/#     │  Publishes command
  └───────────────────────────────┘
     station/{id}/commands/mode
     station/{id}/commands/ota
     ...
```

---

## Lambda function responsibilities

### StationTelemetryProcessorLambda

Triggered by: TelemetryRule (every telemetry message)

Responsibilities:
1. Validate incoming payload (required fields, value ranges, stale timestamp).
2. Run Weather FIS → compute `weather_index`.
3. Run Main Decision FIS → compute recommended `operating_mode`.
4. Run deterministic safety validation → may override FIS recommendation.
5. Write telemetry record to StationTelemetry table (with TTL).
6. Update current state in StationState table.
7. Invoke CommandDispatcherLambda if the mode has changed or a new
   command needs to be sent.

### StationCommandDispatcherLambda

Triggered by: StationTelemetryProcessorLambda (internal invoke) or manually.

Responsibilities:
1. Receive the validated and safety-checked operating mode.
2. Build the command JSON payload.
3. Publish to `station/{station_id}/commands/mode` via IoT Core SDK.
4. Write command record to StationCommands table with `ack_status = pending`.

### StationDiagnosticsLambda

Triggered by: FaultRule, ChargingRule, HeartbeatRule, AckRule.

Responsibilities:
1. Fault events → write to StationFaults table, update StationState.
2. Charging events → update demand profile in StationState.
3. Heartbeat → update `last_seen` and `connection_status` in StationState.
4. ACK events → update `ack_status` in StationCommands table.

### StationFirmwareUpdateManagerLambda

Triggered by: manually or on a schedule (e.g. EventBridge once per day).

Responsibilities:
1. Check StationState for current firmware version.
2. Check StationFirmware table for available updates.
3. Generate a pre-signed S3 URL for the firmware binary.
4. Publish OTA command to `station/{station_id}/commands/ota`.
5. Record the update attempt in StationFirmware table.

---

## Fuzzy logic systems

### Weather FIS (already implemented in simulation/)

- Inputs: `shortwave_radiation`, `cloud_cover`, `precipitation_probability`
- Output: `weather_index` ∈ [0, 1]
- Classification: BAD (< 0.35), REGULAR (0.35–0.65), GOOD (> 0.65)
- Runs inside StationTelemetryProcessorLambda on every telemetry message.
- Uses the pure-numpy implementation (Weather_index.py) — no scikit-fuzzy
  dependency in Lambda to keep the deployment package small.

### Main Decision FIS (pending implementation)

- Inputs: `battery_soc`, `p_net`, `local_irradiance`, `weather_index`, `demand_index`
- Output: `operating_mode` ∈ {M0, M1, M2, M3, M4, M5}
- Runs inside StationTelemetryProcessorLambda after Weather FIS.

---

## Deterministic safety validation layer

The FIS recommendation is never applied directly. It passes through a
deterministic safety check inside StationTelemetryProcessorLambda:

| Condition | Override action |
|---|---|
| `battery_soc` < 20% | Force M0 regardless of FIS output |
| `battery_soc` < 30% | Cap at M1 (no outputs, no tracking) |
| `fault_state` == `"critical"` | Force M0, set `safety_lockout = true` |
| Any required field missing or NaN | Reject payload, do not dispatch command |
| Timestamp older than 120 seconds | Treat as stale, do not dispatch command |
| Actuator fault active | Set `tracking_allowed = false` |

---

## ESP32 local safety protections

**The ESP32 must never rely exclusively on the cloud for safety.**

The following protections run locally on the ESP32 at all times,
independent of MQTT connectivity:

- If MQTT connection is lost for more than 3 minutes:
  → Apply local safe mode (M1 or M0 depending on SOC).
  → Disable all charging outputs.
  → Hold solar tracker at last known safe position.

- If local SOC reading drops below 20%:
  → Force M0 immediately, regardless of last received cloud command.
  → Publish fault event when connection is restored.

- If any critical sensor returns an out-of-range or NaN value:
  → Disable the affected output or subsystem.
  → Publish fault event.

- If a charging output exceeds its current limit:
  → Open the relay immediately (hardware interrupt or fast polling loop).
  → Publish fault event.

These local protections are implemented in `esp32/main/safety.h` and
run in the main loop independently of the MQTT task.

---

## Database strategy

- DynamoDB only — no Timestream, no RDS in v1.
- On-demand billing mode to stay within Free Tier.
- TTL enabled on high-volume tables to control storage cost.
- StationState is a single-item-per-station table for fast current-state lookup.

---

## Storage strategy

- S3 is used only for OTA firmware binaries.
- No raw telemetry is archived to S3 in v1 (DynamoDB TTL handles retention).
- S3 bucket has versioning disabled and a lifecycle rule to delete objects
  older than 180 days.

---

## CloudWatch Logs retention

| Log group | Retention |
|---|---|
| `/aws/lambda/StationTelemetryProcessorLambda` | 7 days |
| `/aws/lambda/StationCommandDispatcherLambda` | 7 days |
| `/aws/lambda/StationDiagnosticsLambda` | 7 days |
| `/aws/lambda/StationFirmwareUpdateManagerLambda` | 7 days |
| IoT Core rule error logs | 3 days |

7-day retention keeps logs available for debugging while staying within
the CloudWatch Free Tier (5 GB ingestion/month, 5 GB storage/month).

---

## AWS Budgets alerts

Three budget alerts should be configured in AWS Budgets:

| Alert threshold | Notification |
|---|---|
| $5.00 actual spend | Email |
| $15.00 actual spend | Email |
| $25.00 actual spend | Email + review architecture |

Monthly budget ceiling: $30.00.
Expected normal cost for a single-station prototype: $0–$2/month.

---

## Services explicitly excluded from v1

| Service | Reason |
|---|---|
| EC2 | Not needed — Lambda handles all compute |
| RDS | Not needed — DynamoDB is sufficient |
| API Gateway | Not needed — no HTTP API required |
| Step Functions | Adds cost and complexity — Lambda is sufficient |
| Kinesis | Overkill for a single station at 30s intervals |
| Timestream | Not needed in v1 — DynamoDB with TTL is sufficient |
| Managed Grafana | Not needed — CloudWatch dashboards are free |
| SNS | Optional in v1 — fault alerts can be added later |
