# AWS Deployment Plan

This document defines the recommended deployment sequence for the cloud backend of the ESP32-based solar charging station.

The objective is to deploy the AWS components in a controlled order, validate each step independently, and avoid connecting the physical ESP32 system until the cloud backend has been tested with manual events and MQTT test messages.

## Current Project Status

The following components have already been drafted locally:

```text
telemetry_processor
diagnostics
command_dispatcher
fis_processor
```

The following design documents have also been created:

```text
MQTT topic structure
MQTT payload definitions
DynamoDB table design
Lambda function design
AWS IoT rule SQL statements
IAM policy design
Local cloud flow test
```

## Important Note About the FIS Processor

The current `fis_processor` Lambda is a preliminary functional draft.

It follows the intended architecture:

```text
Weather FIS → Main FIS → cloud-side validation → command request
```

However, its rule base and membership functions must still be aligned with the final ESP32/article version before it is considered the definitive thesis implementation.

For this reason, the first AWS deployment should focus on validating the cloud architecture and data flow, not on treating the current FIS output as final.

## Deployment Sequence

The recommended deployment order is:

```text
1. Create DynamoDB tables
2. Create Lambda functions
3. Configure Lambda environment variables
4. Create IAM roles and policies
5. Create AWS IoT Core thing and certificate
6. Attach IoT policy to device certificate
7. Create AWS IoT Core rules
8. Test Lambdas manually
9. Test AWS IoT MQTT messages manually
10. Connect ESP32 only after cloud-side tests pass
```

## Step 1: Create DynamoDB Tables

Create the minimum required tables first:

```text
TelemetryHistory
StationStatus
FaultEvents
CommandLog
DemandProfile
FISDecisionHistory
```

Optional tables for later stages:

```text
BatterySOHHistory
ActuatorLifeHistory
```

Recommended initial configuration:

```text
Billing mode: provisioned or on-demand with low usage
Secondary indexes: none initially
Streams: disabled initially
Point-in-time recovery: disabled initially
Backups: manual only if needed
```

## Step 2: Create Lambda Functions

Create the following Lambda functions:

```text
telemetry_processor
diagnostics
command_dispatcher
fis_processor
```

Recommended runtime:

```text
Python 3.12
```

Each function should initially be deployed from its corresponding folder:

```text
aws/lambdas/telemetry_processor/
aws/lambdas/diagnostics/
aws/lambdas/command_dispatcher/
aws/lambdas/fis_processor/
```

## Step 3: Configure Lambda Environment Variables

### telemetry_processor

```text
TELEMETRY_TABLE_NAME=TelemetryHistory
STATUS_TABLE_NAME=StationStatus
```

### diagnostics

```text
FAULT_EVENTS_TABLE_NAME=FaultEvents
COMMAND_LOG_TABLE_NAME=CommandLog
STATUS_TABLE_NAME=StationStatus
```

### command_dispatcher

```text
COMMAND_LOG_TABLE_NAME=CommandLog
AWS_IOT_DATA_ENDPOINT=<your-iot-data-endpoint>
```

### fis_processor

Initial local draft does not require environment variables.

Later, when integrated with DynamoDB and command dispatching, it may require:

```text
FIS_DECISION_TABLE_NAME=FISDecisionHistory
DEMAND_PROFILE_TABLE_NAME=DemandProfile
COMMAND_DISPATCHER_FUNCTION_NAME=command_dispatcher
```

## Step 4: Create IAM Roles

Each Lambda should have its own execution role.

Minimum roles:

```text
lambda_telemetry_processor_role
lambda_diagnostics_role
lambda_command_dispatcher_role
lambda_fis_processor_role
```

Each role should follow least-privilege access.

For example:

```text
telemetry_processor → write telemetry and station status only
diagnostics → write faults and update command acknowledgements
command_dispatcher → write command log and publish MQTT commands
fis_processor → read decision inputs and write decision history
```

## Step 5: Create AWS IoT Core Thing

Create one thing for the first prototype:

```text
station_001
```

The ESP32 device certificate should be associated with this thing.

The certificate should allow the device to:

```text
Connect as station_001
Publish telemetry, status, faults, and acknowledgements
Subscribe to commands and configuration topics
Receive commands and configuration messages
```

## Step 6: Create AWS IoT Rules

Create rules for these MQTT topic filters:

```text
station/+/telemetry
station/+/status
station/+/faults
station/+/acks
```

Recommended routing:

```text
station/+/telemetry → telemetry_processor
station/+/status → diagnostics
station/+/faults → diagnostics
station/+/acks → diagnostics
```

The SQL statements are documented in:

```text
aws/infrastructure/iot_rules.sql
```

## Step 7: Manual Lambda Tests

Before using MQTT, test each Lambda manually from the AWS Lambda console.

Recommended test order:

```text
1. telemetry_processor
2. diagnostics
3. command_dispatcher
4. fis_processor
```

The `command_dispatcher` should not be tested with real MQTT publishing until the IoT endpoint and permissions are configured.

## Step 8: Manual MQTT Tests

Before connecting the ESP32, publish test messages from the AWS IoT MQTT test client.

Test topics:

```text
station/station_001/telemetry
station/station_001/status
station/station_001/faults
station/station_001/acks
```

Confirm that:

```text
Telemetry is stored in DynamoDB
Station status is updated
Fault events are stored
Command acknowledgements update CommandLog
CloudWatch logs show successful processing
```

## Step 9: Command Publishing Test

After `command_dispatcher` is configured, test publishing to:

```text
station/station_001/commands
```

The expected command payload should include:

```text
station_id
timestamp
command_id
command
source
parameters
status
created_at
```

The ESP32 should eventually subscribe to this topic, but for initial testing the topic can be monitored from the AWS IoT MQTT test client.

## Step 10: ESP32 Connection

The ESP32 should only be connected after the cloud backend passes manual testing.

Initial ESP32 cloud test should verify:

```text
Wi-Fi connection
AWS IoT TLS connection
MQTT publish to telemetry topic
MQTT subscribe to commands topic
Command acknowledgement publish
Local safety validation before applying commands
```

## Safety Rule

The AWS cloud backend must never directly bypass the ESP32 deterministic safety layer.

Therefore:

```text
Cloud FIS = recommendation
Command Dispatcher = MQTT command request
ESP32 deterministic layer = final physical authorization
```

This ensures that communication errors, cloud-side bugs, invalid commands, or delayed messages cannot directly activate unsafe hardware states.

## Cost-Control Notes

For the first prototype:

```text
Use moderate telemetry intervals
Avoid unnecessary DynamoDB indexes
Limit CloudWatch log verbosity
Use short log retention periods
Avoid provisioned concurrency
Avoid unnecessary backups, streams, or global tables
```

Recommended telemetry interval for early testing:

```text
15 to 60 seconds
```

For early AWS testing, manual MQTT messages should be used before continuous ESP32 telemetry.


---

## Pending Demand Estimation Definition

The adaptive `demand_estimator` remains pending because the physical
method for detecting scooter connection events has not been defined.

Possible approaches include:

- User-confirmed connection through a physical button.
- Automatic session detection using charging-output current measurements.
- A hybrid method combining user input and electrical confirmation.

Implementation depends on the modifications that remain physically viable
for the charging station. Until this mechanism is defined and validated,
the system will use a fixed or externally supplied Demand Index.