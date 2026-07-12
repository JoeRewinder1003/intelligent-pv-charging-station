# station_cloud_v1

Cloud backend prototype for the ESP32-based solar charging station.

This project defines the initial AWS cloud architecture for receiving telemetry from the station, storing operational data, evaluating cloud-side decision logic, and sending command requests back to the ESP32 through MQTT.

## Project Objective

The objective is to migrate part of the supervisory processing of the solar charging station to AWS while keeping the ESP32 local deterministic layer responsible for final physical safety validation.

The cloud backend is intended to support:

```text
Telemetry processing
Historical data storage
Cloud-side FIS evaluation
MQTT command dispatching
Fault and diagnostic processing
Battery SOH estimation
Actuator lifetime estimation
Adaptive demand profile updates
```

## Current Status

The following cloud components have been drafted locally:

```text
telemetry_processor
diagnostics
command_dispatcher
fis_processor
```

The following local tests have been created:

```text
telemetry_processor local payload validation
command_dispatcher local command validation
diagnostics local payload validation
local cloud flow integration test
```

The local cloud flow test validates:

```text
telemetry payload → fis_processor → command request → command_dispatcher validation
```

## Important Note About `fis_processor`

The current `fis_processor` Lambda is a preliminary functional draft.

It follows the intended structure:

```text
Weather FIS → Main FIS → cloud-side validation → command request
```

However, its rule base and membership functions must still be aligned with the final ESP32/article version before it is considered the definitive thesis implementation.

Therefore, the current `fis_processor` should be used for architecture and data-flow testing, not as final control validation.

## Project Structure

```text
station_cloud_v1/
├── aws/
│   ├── deployment/
│   │   ├── create_dynamodb_tables.md
│   │   ├── create_lambda_functions.md
│   │   ├── configure_iot_core.md
│   │   └── manual_aws_tests.md
│   ├── fis/
│   ├── infrastructure/
│   │   ├── dynamodb_tables.md
│   │   ├── iam_policies.md
│   │   ├── iot_rules.sql
│   │   └── mqtt_topics.md
│   └── lambdas/
│       ├── command_dispatcher/
│       ├── diagnostics/
│       ├── fis_processor/
│       ├── telemetry_processor/
│       ├── demand_estimator/
│       ├── soh_estimator/
│       ├── actuator_life_estimator/
│       └── firmware_update_manager/
├── docs/
│   ├── architecture_notes.md
│   ├── aws_deployment_plan.md
│   ├── cloud_flow.md
│   └── mqtt_payloads.md
├── esp32/
├── tests/
│   └── test_local_cloud_flow.py
├── .gitignore
└── README.md
```

## Main AWS Components

## AWS IoT Core

AWS IoT Core is used as the MQTT broker between the ESP32 and AWS.

Main topics:

```text
station/{station_id}/telemetry
station/{station_id}/status
station/{station_id}/faults
station/{station_id}/acks
station/{station_id}/commands
station/{station_id}/config
```

## DynamoDB

Minimum required tables:

```text
TelemetryHistory
StationStatus
FaultEvents
CommandLog
DemandProfile
FISDecisionHistory
```

Optional future tables:

```text
BatterySOHHistory
ActuatorLifeHistory
```

## Lambda Functions

| Lambda                    | Purpose                                                                   |
| ------------------------- | ------------------------------------------------------------------------- |
| `telemetry_processor`     | Processes telemetry messages and stores telemetry/status data             |
| `diagnostics`             | Processes faults, status messages, and command acknowledgements           |
| `command_dispatcher`      | Publishes command requests to the ESP32 through AWS IoT Core              |
| `fis_processor`           | Evaluates preliminary cloud-side FIS logic and generates command requests |
| `demand_estimator`        | Future adaptive demand profile estimation                                 |
| `soh_estimator`           | Future battery state-of-health estimation                                 |
| `actuator_life_estimator` | Future actuator lifetime estimation                                       |

## Local Tests

Run the local telemetry processor test:

```bash
cd aws/lambdas/telemetry_processor
python local_test.py
```

Run the local command dispatcher test:

```bash
cd aws/lambdas/command_dispatcher
python local_test.py
```

Run the local diagnostics test:

```bash
cd aws/lambdas/diagnostics
python local_test.py
```

Run the local cloud flow integration test from the project root:

```bash
python tests/test_local_cloud_flow.py
```

## Deployment Documentation

Deployment guides are located in:

```text
aws/deployment/
```

Recommended order:

```text
1. Create DynamoDB tables
2. Create Lambda functions
3. Configure AWS IoT Core
4. Configure IAM roles and policies
5. Run manual AWS tests
6. Connect ESP32 only after cloud-side tests pass
```

## Safety Rule

The cloud backend may evaluate the system state and publish command requests, but the ESP32 must remain responsible for final local authorization.

```text
Cloud FIS = recommendation
Command Dispatcher = MQTT command request
ESP32 deterministic layer = final physical authorization
```

This prevents cloud-side errors, delayed messages, invalid payloads, or communication issues from directly activating unsafe hardware states.

## Cost-Control Notes

The first prototype should use a cost-aware configuration:

```text
Moderate telemetry interval
No unnecessary DynamoDB secondary indexes
Limited CloudWatch log verbosity
No provisioned Lambda concurrency
No unnecessary DynamoDB streams or backups
Manual MQTT tests before continuous ESP32 telemetry
```

Recommended initial telemetry interval:

```text
15 to 60 seconds
```

## Next Development Steps

Recommended next steps:

```text
1. Review and clean project documentation
2. Create AWS resources manually following deployment guides
3. Test Lambdas manually in AWS
4. Test AWS IoT MQTT rules with the MQTT test client
5. Align fis_processor with the final ESP32/article FIS
6. Connect ESP32 to AWS IoT Core
7. Add demand_estimator, soh_estimator, and actuator_life_estimator
```
