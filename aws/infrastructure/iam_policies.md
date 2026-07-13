# IAM Policy Design

This document defines the initial IAM permission model for the AWS cloud backend of the ESP32-based solar charging station.

The objective is to use least-privilege permissions for each Lambda function. Each function should only access the DynamoDB tables, IoT topics, and AWS services required for its specific responsibility.

---

## 1. General Principle

The cloud backend is divided into independent Lambda functions. Each Lambda should have its own IAM execution role.

Role naming convention:

```text
lambda_{function_name}_role
```

Example:

```text
lambda_telemetry_processor_role
```

---

## 2. telemetry_processor Role

### Role Name

```text
lambda_telemetry_processor_role
```

### Purpose

Allows the `telemetry_processor` Lambda to write telemetry data and update the latest station status.

### Required Access

| Service         | Resource           | Permission              |
| --------------- | ------------------ | ----------------------- |
| DynamoDB        | `TelemetryHistory` | `PutItem`               |
| DynamoDB        | `StationStatus`    | `PutItem`, `UpdateItem` |
| CloudWatch Logs | Lambda log group   | Create/write logs       |

### Policy Draft

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Sid": "TelemetryHistoryWriteAccess",
      "Effect": "Allow",
      "Action": [
        "dynamodb:PutItem"
      ],
      "Resource": "arn:aws:dynamodb:{region}:{account_id}:table/TelemetryHistory"
    },
    {
      "Sid": "StationStatusWriteAccess",
      "Effect": "Allow",
      "Action": [
        "dynamodb:PutItem",
        "dynamodb:UpdateItem"
      ],
      "Resource": "arn:aws:dynamodb:{region}:{account_id}:table/StationStatus"
    },
    {
      "Sid": "CloudWatchLogsAccess",
      "Effect": "Allow",
      "Action": [
        "logs:CreateLogGroup",
        "logs:CreateLogStream",
        "logs:PutLogEvents"
      ],
      "Resource": "*"
    }
  ]
}
```

---

## 3. fis_processor Role

### Role Name

```text
lambda_fis_processor_role
```

### Purpose

Allows the `fis_processor` Lambda to read telemetry, read demand data, store fuzzy decision results, and invoke or trigger the command dispatch process.

### Required Access

| Service         | Resource             | Permission              |
| --------------- | -------------------- | ----------------------- |
| DynamoDB        | `TelemetryHistory`   | `GetItem`, `Query`      |
| DynamoDB        | `DemandProfile`      | `GetItem`, `Query`      |
| DynamoDB        | `FISDecisionHistory` | `PutItem`               |
| DynamoDB        | `StationStatus`      | `GetItem`, `UpdateItem` |
| Lambda          | `command_dispatcher` | `InvokeFunction`        |
| CloudWatch Logs | Lambda log group     | Create/write logs       |

### Policy Draft

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Sid": "ReadTelemetryHistory",
      "Effect": "Allow",
      "Action": [
        "dynamodb:GetItem",
        "dynamodb:Query"
      ],
      "Resource": "arn:aws:dynamodb:{region}:{account_id}:table/TelemetryHistory"
    },
    {
      "Sid": "ReadDemandProfile",
      "Effect": "Allow",
      "Action": [
        "dynamodb:GetItem",
        "dynamodb:Query"
      ],
      "Resource": "arn:aws:dynamodb:{region}:{account_id}:table/DemandProfile"
    },
    {
      "Sid": "WriteFISDecisionHistory",
      "Effect": "Allow",
      "Action": [
        "dynamodb:PutItem"
      ],
      "Resource": "arn:aws:dynamodb:{region}:{account_id}:table/FISDecisionHistory"
    },
    {
      "Sid": "ReadUpdateStationStatus",
      "Effect": "Allow",
      "Action": [
        "dynamodb:GetItem",
        "dynamodb:UpdateItem"
      ],
      "Resource": "arn:aws:dynamodb:{region}:{account_id}:table/StationStatus"
    },
    {
      "Sid": "InvokeCommandDispatcher",
      "Effect": "Allow",
      "Action": [
        "lambda:InvokeFunction"
      ],
      "Resource": "arn:aws:lambda:{region}:{account_id}:function:command_dispatcher"
    },
    {
      "Sid": "CloudWatchLogsAccess",
      "Effect": "Allow",
      "Action": [
        "logs:CreateLogGroup",
        "logs:CreateLogStream",
        "logs:PutLogEvents"
      ],
      "Resource": "*"
    }
  ]
}
```

---

## 4. command_dispatcher Role

### Role Name

```text
lambda_command_dispatcher_role
```

### Purpose

Allows the `command_dispatcher` Lambda to store command records and publish MQTT commands to the ESP32 through AWS IoT Core.

### Required Access

| Service         | Resource             | Permission              |
| --------------- | -------------------- | ----------------------- |
| DynamoDB        | `CommandLog`         | `PutItem`, `UpdateItem` |
| DynamoDB        | `StationStatus`      | `GetItem`               |
| AWS IoT Core    | `station/*/commands` | `iot:Publish`           |
| CloudWatch Logs | Lambda log group     | Create/write logs       |

### Policy Draft

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Sid": "CommandLogWriteAccess",
      "Effect": "Allow",
      "Action": [
        "dynamodb:PutItem",
        "dynamodb:UpdateItem"
      ],
      "Resource": "arn:aws:dynamodb:{region}:{account_id}:table/CommandLog"
    },
    {
      "Sid": "ReadStationStatus",
      "Effect": "Allow",
      "Action": [
        "dynamodb:GetItem"
      ],
      "Resource": "arn:aws:dynamodb:{region}:{account_id}:table/StationStatus"
    },
    {
      "Sid": "PublishCommandsToStation",
      "Effect": "Allow",
      "Action": [
        "iot:Publish"
      ],
      "Resource": "arn:aws:iot:{region}:{account_id}:topic/station/*/commands"
    },
    {
      "Sid": "CloudWatchLogsAccess",
      "Effect": "Allow",
      "Action": [
        "logs:CreateLogGroup",
        "logs:CreateLogStream",
        "logs:PutLogEvents"
      ],
      "Resource": "*"
    }
  ]
}
```

---

## 5. diagnostics Role

### Role Name

```text
lambda_diagnostics_role
```

### Purpose

Allows the `diagnostics` Lambda to store fault events, update command acknowledgements, and update the latest station status.

### Required Access

| Service         | Resource             | Permission                |
| --------------- | -------------------- | ------------------------- |
| DynamoDB        | `FaultEvents`        | `PutItem`                 |
| DynamoDB        | `CommandLog`         | `GetItem`, `UpdateItem`   |
| DynamoDB        | `StationStatus`      | `GetItem`, `UpdateItem`   |
| Lambda          | `command_dispatcher` | Optional `InvokeFunction` |
| CloudWatch Logs | Lambda log group     | Create/write logs         |

### Policy Draft

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Sid": "FaultEventsWriteAccess",
      "Effect": "Allow",
      "Action": [
        "dynamodb:PutItem"
      ],
      "Resource": "arn:aws:dynamodb:{region}:{account_id}:table/FaultEvents"
    },
    {
      "Sid": "CommandLogReadUpdateAccess",
      "Effect": "Allow",
      "Action": [
        "dynamodb:GetItem",
        "dynamodb:UpdateItem"
      ],
      "Resource": "arn:aws:dynamodb:{region}:{account_id}:table/CommandLog"
    },
    {
      "Sid": "StationStatusReadUpdateAccess",
      "Effect": "Allow",
      "Action": [
        "dynamodb:GetItem",
        "dynamodb:UpdateItem"
      ],
      "Resource": "arn:aws:dynamodb:{region}:{account_id}:table/StationStatus"
    },
    {
      "Sid": "OptionalInvokeCommandDispatcher",
      "Effect": "Allow",
      "Action": [
        "lambda:InvokeFunction"
      ],
      "Resource": "arn:aws:lambda:{region}:{account_id}:function:command_dispatcher"
    },
    {
      "Sid": "CloudWatchLogsAccess",
      "Effect": "Allow",
      "Action": [
        "logs:CreateLogGroup",
        "logs:CreateLogStream",
        "logs:PutLogEvents"
      ],
      "Resource": "*"
    }
  ]
}
```

---

## 6. demand_estimator Role

### Role Name

```text
lambda_demand_estimator_role
```

### Purpose

Allows the `demand_estimator` Lambda to read historical charging behavior and update the adaptive demand profile.

### Required Access

| Service         | Resource           | Permission                                  |
| --------------- | ------------------ | ------------------------------------------- |
| DynamoDB        | `TelemetryHistory` | `Query`                                     |
| DynamoDB        | `DemandProfile`    | `GetItem`, `PutItem`, `UpdateItem`, `Query` |
| CloudWatch Logs | Lambda log group   | Create/write logs                           |

### Policy Draft

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Sid": "ReadTelemetryHistory",
      "Effect": "Allow",
      "Action": [
        "dynamodb:Query"
      ],
      "Resource": "arn:aws:dynamodb:{region}:{account_id}:table/TelemetryHistory"
    },
    {
      "Sid": "ReadUpdateDemandProfile",
      "Effect": "Allow",
      "Action": [
        "dynamodb:GetItem",
        "dynamodb:PutItem",
        "dynamodb:UpdateItem",
        "dynamodb:Query"
      ],
      "Resource": "arn:aws:dynamodb:{region}:{account_id}:table/DemandProfile"
    },
    {
      "Sid": "CloudWatchLogsAccess",
      "Effect": "Allow",
      "Action": [
        "logs:CreateLogGroup",
        "logs:CreateLogStream",
        "logs:PutLogEvents"
      ],
      "Resource": "*"
    }
  ]
}
```

---

## 7. soh_estimator Role

### Role Name

```text
lambda_soh_estimator_role
```

### Purpose

Allows the `soh_estimator` Lambda to read battery telemetry and store estimated battery state-of-health records.

### Required Access

| Service         | Resource            | Permission        |
| --------------- | ------------------- | ----------------- |
| DynamoDB        | `TelemetryHistory`  | `Query`           |
| DynamoDB        | `BatterySOHHistory` | `PutItem`         |
| CloudWatch Logs | Lambda log group    | Create/write logs |

---

## 8. actuator_life_estimator Role

### Role Name

```text
lambda_actuator_life_estimator_role
```

### Purpose

Allows the `actuator_life_estimator` Lambda to read tracking-related telemetry and store actuator usage indicators.

### Required Access

| Service         | Resource              | Permission        |
| --------------- | --------------------- | ----------------- |
| DynamoDB        | `TelemetryHistory`    | `Query`           |
| DynamoDB        | `ActuatorLifeHistory` | `PutItem`         |
| CloudWatch Logs | Lambda log group      | Create/write logs |

---

## 9. AWS IoT Rule Invocation Permission

AWS IoT Core needs permission to invoke the target Lambda functions.

This is usually added as a Lambda resource-based permission, not as an IAM policy attached to the Lambda execution role.

Conceptually, AWS IoT Core must be allowed to invoke:

```text
telemetry_processor
diagnostics
```

Example permission concept:

```text
Principal: iot.amazonaws.com
Action: lambda:InvokeFunction
Resource: target Lambda function
Source ARN: AWS IoT Rule ARN
```

---

## 10. ESP32 IoT Policy

The ESP32 device certificate should be attached to an AWS IoT policy that allows only the required MQTT operations.

### Device Policy Scope

The ESP32 should be allowed to:

```text
Connect as its assigned client ID
Publish telemetry, status, faults, and acknowledgements
Subscribe to its own command and configuration topics
Receive messages from its own command and configuration topics
```

### Policy Draft

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Sid": "AllowDeviceConnect",
      "Effect": "Allow",
      "Action": [
        "iot:Connect"
      ],
      "Resource": "arn:aws:iot:{region}:{account_id}:client/station_001"
    },
    {
      "Sid": "AllowDevicePublish",
      "Effect": "Allow",
      "Action": [
        "iot:Publish"
      ],
      "Resource": [
        "arn:aws:iot:{region}:{account_id}:topic/station/station_001/telemetry",
        "arn:aws:iot:{region}:{account_id}:topic/station/station_001/status",
        "arn:aws:iot:{region}:{account_id}:topic/station/station_001/faults",
        "arn:aws:iot:{region}:{account_id}:topic/station/station_001/acks"
      ]
    },
    {
      "Sid": "AllowDeviceSubscribe",
      "Effect": "Allow",
      "Action": [
        "iot:Subscribe"
      ],
      "Resource": [
        "arn:aws:iot:{region}:{account_id}:topicfilter/station/station_001/commands",
        "arn:aws:iot:{region}:{account_id}:topicfilter/station/station_001/config"
      ]
    },
    {
      "Sid": "AllowDeviceReceive",
      "Effect": "Allow",
      "Action": [
        "iot:Receive"
      ],
      "Resource": [
        "arn:aws:iot:{region}:{account_id}:topic/station/station_001/commands",
        "arn:aws:iot:{region}:{account_id}:topic/station/station_001/config"
      ]
    }
  ]
}
```

---

## 11. Notes for First Prototype

For the first prototype, the minimum required IAM elements are:

```text
lambda_telemetry_processor_role
lambda_diagnostics_role
lambda_command_dispatcher_role
ESP32 IoT policy
AWS IoT permission to invoke Lambda
```

The following roles can be added later:

```text
lambda_fis_processor_role
lambda_demand_estimator_role
lambda_soh_estimator_role
lambda_actuator_life_estimator_role
```

---

## 12. Safety and Security Rule

The cloud backend should only publish command requests. The ESP32 local deterministic safety layer must remain responsible for final physical authorization.

Therefore:

```text
Cloud permission = request and process
ESP32 local logic = validate and authorize physical action
```

---

## Deployment Status

The first IAM role was created manually in AWS.

### AWS Region

```text
us-east-2
```

### Created roles

| Role                              | Status  | Purpose                                                                                       |
| --------------------------------- | ------- | --------------------------------------------------------------------------------------------- |
| `lambda_telemetry_processor_role` | Created | Allows `telemetry_processor` to write telemetry data to DynamoDB and write logs to CloudWatch |

### Attatched permissions
| Role                              | Permission                                           |
| --------------------------------- | ---------------------------------------------------- |
| `lambda_telemetry_processor_role` | `AWSLambdaBasicExecutionRole`                        |
| `lambda_telemetry_processor_role` | Inline policy: `telemetry_processor_dynamodb_policy` |

### DynamoDB Access
| Table              | Permissions                               |
| ------------------ | ----------------------------------------- |
| `TelemetryHistory` | `dynamodb:PutItem`                        |
| `StationStatus`    | `dynamodb:PutItem`, `dynamodb:UpdateItem` |


## lambda_command_dispatcher_role

### Managed policy

- AWSLambdaBasicExecutionRole

### Inline policy

- command_dispatcher_dynamodb_iot_policy

### Permissions

- `dynamodb:PutItem` on `CommandLog`
- `dynamodb:UpdateItem` on `CommandLog`
- `iot:Publish` on `station/*/commands`

---


## lambda_diagnostics_role

### Managed policy

- AWSLambdaBasicExecutionRole

### Inline policy

- diagnostics_dynamodb_policy

### Permissions

- dynamodb:PutItem on FaultEvents
- dynamodb:UpdateItem on CommandLog
- dynamodb:PutItem and dynamodb:UpdateItem on StationStatus

---

## lambda_fis_processor_role

### Managed policy

- AWSLambdaBasicExecutionRole

### Inline policy

- fis_processor_dynamodb_policy

### Permissions

- `dynamodb:PutItem` on `FISDecisionHistory`

The current FIS implementation remains preliminary.