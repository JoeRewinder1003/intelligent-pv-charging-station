# Create Lambda Functions

This document describes the initial manual procedure for creating the AWS Lambda functions required by the cloud backend of the ESP32-based solar charging station.

The purpose of this step is to deploy the cloud processing layer after the DynamoDB tables have been created.

## Lambda Functions to Create

The first prototype uses the following Lambda functions:

```text
telemetry_processor
diagnostics
command_dispatcher
fis_processor
```

## Recommended Runtime

Use a supported Python 3 runtime, preferably:

```text
Python 3.12
```

If a newer supported Python runtime is available in the AWS console, it can also be used, but all functions should use the same runtime version for consistency.

## General Creation Settings

For each Lambda function:

```text
Author from scratch
Runtime: Python 3.x
Architecture: x86_64
Permissions: Use an existing role or create a role according to iam_policies.md
Timeout: 10 seconds initially
Memory: 128 MB initially
```

The initial functions are lightweight, so 128 MB and 10 seconds are sufficient for early testing.

---

## 1. telemetry_processor

### Source Folder

```text
aws/lambdas/telemetry_processor/
```

### Main File

```text
lambda_function.py
```

### Handler

```text
lambda_function.lambda_handler
```

### Environment Variables

```text
TELEMETRY_TABLE_NAME=TelemetryHistory
STATUS_TABLE_NAME=StationStatus
```

### Required IAM Access

```text
dynamodb:PutItem → TelemetryHistory
dynamodb:PutItem / dynamodb:UpdateItem → StationStatus
CloudWatch Logs access
```

### Manual Test Event

Use:

```text
aws/lambdas/telemetry_processor/test_event.json
```

Expected result:

```text
Telemetry processed successfully
```

---

## 2. diagnostics

### Source Folder

```text
aws/lambdas/diagnostics/
```

### Main File

```text
lambda_function.py
```

### Handler

```text
lambda_function.lambda_handler
```

### Environment Variables

```text
FAULT_EVENTS_TABLE_NAME=FaultEvents
COMMAND_LOG_TABLE_NAME=CommandLog
STATUS_TABLE_NAME=StationStatus
```

### Required IAM Access

```text
dynamodb:PutItem → FaultEvents
dynamodb:GetItem / dynamodb:UpdateItem → CommandLog
dynamodb:GetItem / dynamodb:UpdateItem → StationStatus
CloudWatch Logs access
```

### Manual Test Events

Use:

```text
aws/lambdas/diagnostics/test_event_fault.json
aws/lambdas/diagnostics/test_event_ack.json
aws/lambdas/diagnostics/test_event_status.json
```

Expected result:

```text
Diagnostics message processed successfully
```

---

## 3. command_dispatcher

### Source Folder

```text
aws/lambdas/command_dispatcher/
```

### Main File

```text
lambda_function.py
```

### Handler

```text
lambda_function.lambda_handler
```

### Environment Variables

```text
COMMAND_LOG_TABLE_NAME=CommandLog
AWS_IOT_DATA_ENDPOINT=<your-iot-data-endpoint>
```

### Required IAM Access

```text
dynamodb:PutItem / dynamodb:UpdateItem → CommandLog
iot:Publish → station/*/commands
CloudWatch Logs access
```

### Manual Test Event

Use:

```text
aws/lambdas/command_dispatcher/test_event.json
```

Expected result:

```text
Command published successfully
```

### Important Note

This function requires the AWS IoT Data endpoint. The endpoint can be obtained later from AWS IoT Core.

Until the endpoint and permissions are configured, this function can be tested only locally using:

```text
local_test.py
```

---

## 4. fis_processor

### Source Folder

```text
aws/lambdas/fis_processor/
```

### Main File

```text
lambda_function.py
```

### Handler

```text
lambda_function.lambda_handler
```

### Environment Variables

The current preliminary draft does not require environment variables.

Future integrated version may require:

```text
FIS_DECISION_TABLE_NAME=FISDecisionHistory
DEMAND_PROFILE_TABLE_NAME=DemandProfile
COMMAND_DISPATCHER_FUNCTION_NAME=command_dispatcher
```

### Required IAM Access

For the current draft:

```text
CloudWatch Logs access
```

For the future integrated version:

```text
dynamodb:GetItem / dynamodb:Query → DemandProfile
dynamodb:PutItem → FISDecisionHistory
lambda:InvokeFunction → command_dispatcher
CloudWatch Logs access
```

### Manual Test Event

Use:

```text
aws/lambdas/fis_processor/test_event.json
```

Expected result:

```text
FIS decision evaluated successfully
```

### Important Note

The current `fis_processor` is a preliminary functional draft. It follows the intended architecture:

```text
Weather FIS → Main FIS → cloud-side validation → command request
```

However, its rule base and membership functions must still be aligned with the final ESP32/article version before it is treated as the definitive thesis implementation.

---

## Deployment Method

For the first prototype, each Lambda can be deployed manually from the AWS Lambda console.

Recommended manual process:

```text
1. Open AWS Lambda
2. Create function
3. Select Author from scratch
4. Enter the function name
5. Select Python runtime
6. Assign the corresponding IAM execution role
7. Paste or upload the lambda_function.py code
8. Configure environment variables
9. Save and deploy
10. Create a manual test event
11. Run the test
12. Check CloudWatch logs if errors occur
```

## Deployment Package Notes

The current Lambda drafts only require:

```text
Python standard library
boto3
```

In AWS Lambda, `boto3` is normally available in the runtime environment. Therefore, for the first manual deployment, uploading only `lambda_function.py` should be enough.

If additional external libraries are added later, a deployment package or Lambda layer will be required.

## Validation Checklist

After creating each Lambda, confirm that:

```text
The function name matches the project documentation
The handler is lambda_function.lambda_handler
The runtime is Python 3.x
The correct IAM role is attached
The required environment variables are configured
The test event runs successfully
CloudWatch logs do not show permission errors
```

## Recommended Test Order

Test the functions in this order:

```text
1. telemetry_processor
2. diagnostics
3. fis_processor
4. command_dispatcher
```

The `command_dispatcher` should be tested after AWS IoT Core permissions and endpoint are configured.

## Safety Rule

The cloud backend may generate command requests, but the ESP32 local deterministic safety layer must remain responsible for final physical authorization.

Therefore:

```text
Cloud FIS = recommendation
Command Dispatcher = MQTT command request
ESP32 deterministic layer = final authorization
```

## telemetry_processor

- Region: us-east-2
- Function name: telemetry_processor
- Runtime: Python 3.13
- Architecture: x86_64
- Handler: lambda_function.lambda_handler
- Execution role: lambda_telemetry_processor_role

### Environment variables

- TELEMETRY_TABLE_NAME=TelemetryHistory
- STATUS_TABLE_NAME=StationStatus

### Source files

- Lambda code: aws/lambdas/telemetry_processor/lambda_function.py
- Test event: aws/lambdas/telemetry_processor/test_event.json


---


## command_dispatcher

### AWS configuration

- Region: us-east-2
- Function name: command_dispatcher
- Runtime: Python 3.13
- Architecture: x86_64
- Handler: lambda_function.lambda_handler
- Execution role: lambda_command_dispatcher_role
- Memory: 128 MB
- Timeout: 10 seconds

### Environment variables

- COMMAND_LOG_TABLE_NAME=CommandLog
- AWS_IOT_DATA_ENDPOINT= <AWS IoT Data-ATS endpoint>


### Source files

- Lambda code: aws/lambdas/command_dispatcher/lambda_function.py
- Test event: aws/lambdas/command_dispatcher/test_event.json

### MQTT topic

Commands are published to:

`station/{station_id}/commands`