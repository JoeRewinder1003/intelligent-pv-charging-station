# Manual AWS Tests

This document defines the manual test sequence for validating the AWS cloud backend before connecting the physical ESP32-based solar charging station.

The goal is to verify that DynamoDB tables, Lambda functions, IAM permissions, AWS IoT Core rules, and MQTT messages work correctly using controlled test events.

## Test Principle

The ESP32 should not be connected until the cloud backend has been validated with manual tests.

Recommended validation sequence:

```text
1. Test Lambda functions manually
2. Confirm DynamoDB writes and updates
3. Test AWS IoT Core MQTT rules
4. Test command publishing
5. Connect ESP32 only after cloud tests pass
```

## Important Note About `fis_processor`

The current `fis_processor` Lambda is a preliminary functional draft. It is useful for testing the cloud architecture and data flow, but its membership functions and rule base must still be aligned with the final ESP32/article FIS before it is considered definitive.

Therefore, early tests should validate:

```text
Data flow
Payload structure
Command generation
Lambda integration
Database writes
```

They should not yet be interpreted as final control-performance validation.

---

## 1. DynamoDB Table Check

Before testing Lambdas, confirm that these tables exist:

```text
TelemetryHistory
StationStatus
FaultEvents
CommandLog
DemandProfile
FISDecisionHistory
```

Optional later tables:

```text
BatterySOHHistory
ActuatorLifeHistory
```

Check that all required key names and types match the project documentation.

Minimum required key configuration:

| Table                | Partition Key | Sort Key     |
| -------------------- | ------------- | ------------ |
| `TelemetryHistory`   | `station_id`  | `timestamp`  |
| `StationStatus`      | `station_id`  | None         |
| `FaultEvents`        | `station_id`  | `timestamp`  |
| `CommandLog`         | `station_id`  | `command_id` |
| `DemandProfile`      | `station_id`  | `slot_id`    |
| `FISDecisionHistory` | `station_id`  | `timestamp`  |

---

## 2. Lambda Manual Test: `telemetry_processor`

### Test Event

Use the JSON file:

```text
aws/lambdas/telemetry_processor/test_event.json
```

### Expected Result

The Lambda should return:

```text
Telemetry processed successfully
```

### Verify in DynamoDB

Check that:

```text
TelemetryHistory contains a new telemetry sample
StationStatus contains or updates the station_001 record
```

### Common Errors

| Error           | Possible Cause                              |
| --------------- | ------------------------------------------- |
| Access denied   | IAM role does not allow DynamoDB write      |
| Table not found | Table name or environment variable mismatch |
| Invalid payload | Missing required field in test event        |

---

## 3. Lambda Manual Test: `diagnostics`

Test the three supported diagnostic message types.

### Fault Event

Use:

```text
aws/lambdas/diagnostics/test_event_fault.json
```

Expected result:

```text
Diagnostics message processed successfully
```

Verify:

```text
FaultEvents receives a new item
StationStatus is updated with latest fault state
```

### Acknowledgement Event

Use:

```text
aws/lambdas/diagnostics/test_event_ack.json
```

Expected result:

```text
Diagnostics message processed successfully
```

Verify:

```text
CommandLog is updated with acknowledgement data
```

Note: this test may require an existing command record with the same `station_id` and `command_id`.

### Status Event

Use:

```text
aws/lambdas/diagnostics/test_event_status.json
```

Expected result:

```text
Diagnostics message processed successfully
```

Verify:

```text
StationStatus is updated with the latest operating mode and fault state
```

---

## 4. Lambda Manual Test: `fis_processor`

### Test Event

Use:

```text
aws/lambdas/fis_processor/test_event.json
```

### Expected Result

The Lambda should return:

```text
FIS decision evaluated successfully
```

The output should include:

```text
fis_result
command_request
```

The command request should include:

```text
station_id
command
source
parameters
```

Example command values:

```text
ENABLE_TRACKING
ENABLE_OUTPUT_1
ENABLE_OUTPUT_2
ENABLE_OUTPUT_3
STOP
LOCKOUT
```

### Note

The exact operating mode may vary because the current FIS implementation is preliminary.

---

## 5. Lambda Manual Test: `command_dispatcher`

### Test Event

Use:

```text
aws/lambdas/command_dispatcher/test_event.json
```

### Required Environment Variable

Before testing real MQTT publishing, configure:

```text
AWS_IOT_DATA_ENDPOINT=<your-iot-data-endpoint>
COMMAND_LOG_TABLE_NAME=CommandLog
```

### Expected Result

The Lambda should return:

```text
Command published successfully
```

### Verify

Check that:

```text
CommandLog contains a new command record
AWS IoT MQTT test client receives the command on station/station_001/commands
```

### Common Errors

| Error                                                  | Possible Cause                          |
| ------------------------------------------------------ | --------------------------------------- |
| AWS_IOT_DATA_ENDPOINT environment variable is required | Endpoint not configured                 |
| Access denied for iot:Publish                          | IAM role missing IoT publish permission |
| Access denied for DynamoDB                             | IAM role missing CommandLog permissions |

---

## 6. AWS IoT MQTT Rule Tests

Use the AWS IoT MQTT test client before connecting the ESP32.

### Subscribe to Test Topics

Subscribe to:

```text
station/station_001/telemetry
station/station_001/status
station/station_001/faults
station/station_001/acks
station/station_001/commands
```

### Publish Telemetry Test

Publish to:

```text
station/station_001/telemetry
```

Use the telemetry payload from:

```text
aws/lambdas/telemetry_processor/test_event.json
```

Expected result:

```text
telemetry_processor is triggered
TelemetryHistory is updated
StationStatus is updated
```

### Publish Status Test

Publish to:

```text
station/station_001/status
```

Use:

```text
aws/lambdas/diagnostics/test_event_status.json
```

Expected result:

```text
diagnostics is triggered
StationStatus is updated
```

### Publish Fault Test

Publish to:

```text
station/station_001/faults
```

Use:

```text
aws/lambdas/diagnostics/test_event_fault.json
```

Expected result:

```text
diagnostics is triggered
FaultEvents is updated
StationStatus is updated with fault information
```

### Publish Acknowledgement Test

Publish to:

```text
station/station_001/acks
```

Use:

```text
aws/lambdas/diagnostics/test_event_ack.json
```

Expected result:

```text
diagnostics is triggered
CommandLog is updated
```

---

## 7. Command Topic Test

Subscribe in the MQTT test client to:

```text
station/station_001/commands
```

Then manually test the `command_dispatcher` Lambda.

Expected MQTT message:

```json
{
  "station_id": "station_001",
  "timestamp": "2026-07-09T21:00:00Z",
  "command_id": "cmd-...",
  "command": "ENABLE_OUTPUT_2",
  "source": "cloud_fis",
  "parameters": {
    "requested_mode": "M4",
    "max_outputs": 2,
    "tracking_allowed": true
  },
  "status": "sent",
  "created_at": "2026-07-09T21:00:00Z"
}
```

If the message appears in the MQTT test client, the cloud-to-device command path is working.

---

## 8. CloudWatch Log Check

For every Lambda test, check CloudWatch Logs.

Confirm that:

```text
No permission errors appear
No table-not-found errors appear
No invalid-payload errors appear
No unhandled exceptions appear
```

For cost control, avoid printing full telemetry payloads continuously during normal operation.

---

## 9. Pass/Fail Checklist

The AWS backend is ready for ESP32 connection only if:

```text
telemetry_processor test passes
diagnostics tests pass
fis_processor test passes
command_dispatcher test passes
MQTT telemetry rule triggers telemetry_processor
MQTT status rule triggers diagnostics
MQTT faults rule triggers diagnostics
MQTT acks rule triggers diagnostics
Command topic receives messages from command_dispatcher
DynamoDB tables are updated correctly
CloudWatch logs do not show permission errors
```

---

## 10. Safety Rule

Even after successful AWS tests, the ESP32 must validate every received command locally before applying it.

Therefore:

```text
Cloud backend = recommendation and command request
AWS IoT Core = MQTT transport
ESP32 deterministic layer = final physical authorization
```

Cloud success does not imply direct hardware authorization.

---

## telemetry_processor

The telemetry_processor Lambda function was tested manually from the AWS Lambda console.

### Verified results

- Lambda execution completed successfully.
- A telemetry record was stored in TelemetryHistory.
- The latest station state was created or updated in StationStatus.
- Execution logs were generated successfully in Amazon CloudWatch.

### Test event

aws/lambdas/telemetry_processor/test_event.json

---


## command_dispatcher

The command_dispatcher Lambda function was tested manually from the AWS Lambda console.

### Test event

- File: aws/lambdas/command_dispatcher/test_event.json
- Station ID: station_001
- Command: ENABLE_OUTPUT_2
- Source: cloud_fis

### Verified results

- The Lambda returned a successful response.
- A new command record was stored in the CommandLog DynamoDB table.
- The MQTT message was received through the AWS IoT Core MQTT test client.
- The message was published to `station/station_001/commands`.
- The invocation was registered in Amazon CloudWatch Logs.

### Result

The command_dispatcher infrastructure flow was successfully validated:

Lambda → DynamoDB CommandLog → AWS IoT Core MQTT