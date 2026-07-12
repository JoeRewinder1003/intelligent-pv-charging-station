# Local Cloud Flow Test

This document describes the first local integration test of the AWS cloud backend for the ESP32-based solar charging station.

The purpose of this test is to validate the logical flow between the main cloud-side Lambda functions without connecting to real AWS services yet.

## Tested Flow

The local integration test validates the following sequence:

```text
telemetry payload → fis_processor → command request → command_dispatcher validation
```

## Components Involved

| Component                  | Role                                                                                                    |
| -------------------------- | ------------------------------------------------------------------------------------------------------- |
| `fis_processor`            | Receives telemetry-like data, evaluates the preliminary cloud-side FIS, and generates a command request |
| `command_dispatcher`       | Validates the command request and builds the MQTT command payload                                       |
| `test_local_cloud_flow.py` | Local test script that connects both components without using AWS services                              |

## Test Script

The integration test is located at:

```text
tests/test_local_cloud_flow.py
```

## Input Data

The test uses the local FIS test event:

```text
aws/lambdas/fis_processor/test_event.json
```

This event includes representative telemetry values such as:

* Battery SOC
* Net battery power
* Local irradiance
* Weather variables
* Demand Index
* Fault state

## Expected Result

A successful test should show that:

1. The FIS input is valid.
2. The FIS processor generates a valid operating-mode recommendation.
3. A requested operating mode is produced.
4. A command request is generated.
5. The command dispatcher accepts the command request.
6. A final command payload is created.

Example output:

```text
Local cloud flow is valid.

FIS mode:
M4

Requested mode:
M4

Generated command:
ENABLE_OUTPUT_2
```

The exact FIS mode may vary depending on the input values and the current FIS rule base.

## Important Note

The current `fis_processor` is a preliminary functional draft. It follows the agreed architecture:

```text
Weather FIS → Main FIS → cloud-side validation → command request
```

However, the rule base and membership functions must still be aligned with the final ESP32/article version before this Lambda is used as the definitive thesis implementation.

## Safety Consideration

Even when the cloud backend generates a command, the ESP32 local deterministic safety layer must remain responsible for final physical authorization.

Therefore:

```text
Cloud FIS = recommendation
Command Dispatcher = MQTT command request
ESP32 deterministic layer = final authorization
```

This prevents cloud-side errors, communication delays, or invalid commands from directly activating unsafe hardware states.
