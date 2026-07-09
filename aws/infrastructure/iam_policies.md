# IAM Policies

## Design principle: least privilege

Every IAM role and policy in this project grants only the minimum permissions
required for each component to function. No wildcard `*` actions are used
except where AWS requires it (e.g. IoT Core publish does not support
resource-level restrictions on all actions).

---

## 1. ESP32 Device Policy (AWS IoT Core Policy)

Applied to the X.509 certificate attached to the Thing `solar_station_01`.

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": "iot:Connect",
      "Resource": "arn:aws:iot:{region}:{account_id}:client/solar_station_01"
    },
    {
      "Effect": "Allow",
      "Action": "iot:Publish",
      "Resource": [
        "arn:aws:iot:{region}:{account_id}:topic/$aws/rules/TelemetryRule/station/solar_station_01/telemetry/raw",
        "arn:aws:iot:{region}:{account_id}:topic/$aws/rules/FaultRule/station/solar_station_01/events/fault",
        "arn:aws:iot:{region}:{account_id}:topic/$aws/rules/ChargingRule/station/solar_station_01/events/charging",
        "arn:aws:iot:{region}:{account_id}:topic/$aws/rules/HeartbeatRule/station/solar_station_01/status/heartbeat",
        "arn:aws:iot:{region}:{account_id}:topic/$aws/rules/AckRule/station/solar_station_01/ack/command"
      ]
    },
    {
      "Effect": "Allow",
      "Action": "iot:Subscribe",
      "Resource": "arn:aws:iot:{region}:{account_id}:topicfilter/station/solar_station_01/commands/*"
    },
    {
      "Effect": "Allow",
      "Action": "iot:Receive",
      "Resource": "arn:aws:iot:{region}:{account_id}:topic/station/solar_station_01/commands/*"
    }
  ]
}
```

---

## 2. IoT Rule Execution Role

Used by AWS IoT Core Rules to invoke Lambda functions.
One role shared by all rules (simplifies management for a single station).

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": "lambda:InvokeFunction",
      "Resource": [
        "arn:aws:lambda:{region}:{account_id}:function:StationTelemetryProcessorLambda",
        "arn:aws:lambda:{region}:{account_id}:function:StationDiagnosticsLambda"
      ]
    }
  ]
}
```

---

## 3. StationTelemetryProcessorLambda Execution Role

Attached to the Lambda function as its execution role.

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": [
        "dynamodb:PutItem",
        "dynamodb:UpdateItem",
        "dynamodb:GetItem"
      ],
      "Resource": [
        "arn:aws:dynamodb:{region}:{account_id}:table/StationTelemetry",
        "arn:aws:dynamodb:{region}:{account_id}:table/StationState"
      ]
    },
    {
      "Effect": "Allow",
      "Action": "iot:Publish",
      "Resource": "arn:aws:iot:{region}:{account_id}:topic/station/*/commands/*"
    },
    {
      "Effect": "Allow",
      "Action": [
        "logs:CreateLogGroup",
        "logs:CreateLogStream",
        "logs:PutLogEvents"
      ],
      "Resource": "arn:aws:logs:{region}:{account_id}:log-group:/aws/lambda/StationTelemetryProcessorLambda:*"
    }
  ]
}
```

---

## 4. StationDiagnosticsLambda Execution Role

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": [
        "dynamodb:PutItem",
        "dynamodb:UpdateItem",
        "dynamodb:GetItem",
        "dynamodb:Query"
      ],
      "Resource": [
        "arn:aws:dynamodb:{region}:{account_id}:table/StationFaults",
        "arn:aws:dynamodb:{region}:{account_id}:table/StationCommands",
        "arn:aws:dynamodb:{region}:{account_id}:table/StationState"
      ]
    },
    {
      "Effect": "Allow",
      "Action": [
        "logs:CreateLogGroup",
        "logs:CreateLogStream",
        "logs:PutLogEvents"
      ],
      "Resource": "arn:aws:logs:{region}:{account_id}:log-group:/aws/lambda/StationDiagnosticsLambda:*"
    }
  ]
}
```

---

## 5. StationCommandDispatcherLambda Execution Role

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": "iot:Publish",
      "Resource": "arn:aws:iot:{region}:{account_id}:topic/station/*/commands/*"
    },
    {
      "Effect": "Allow",
      "Action": [
        "dynamodb:PutItem",
        "dynamodb:UpdateItem"
      ],
      "Resource": "arn:aws:dynamodb:{region}:{account_id}:table/StationCommands"
    },
    {
      "Effect": "Allow",
      "Action": [
        "logs:CreateLogGroup",
        "logs:CreateLogStream",
        "logs:PutLogEvents"
      ],
      "Resource": "arn:aws:logs:{region}:{account_id}:log-group:/aws/lambda/StationCommandDispatcherLambda:*"
    }
  ]
}
```

---

## 6. StationFirmwareUpdateManagerLambda Execution Role

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": "s3:GetObject",
      "Resource": "arn:aws:s3:::station-firmware-bucket/*"
    },
    {
      "Effect": "Allow",
      "Action": "s3:GeneratePresignedUrl",
      "Resource": "arn:aws:s3:::station-firmware-bucket/*"
    },
    {
      "Effect": "Allow",
      "Action": "iot:Publish",
      "Resource": "arn:aws:iot:{region}:{account_id}:topic/station/*/commands/ota"
    },
    {
      "Effect": "Allow",
      "Action": [
        "dynamodb:PutItem",
        "dynamodb:UpdateItem",
        "dynamodb:GetItem"
      ],
      "Resource": "arn:aws:dynamodb:{region}:{account_id}:table/StationFirmware"
    },
    {
      "Effect": "Allow",
      "Action": [
        "logs:CreateLogGroup",
        "logs:CreateLogStream",
        "logs:PutLogEvents"
      ],
      "Resource": "arn:aws:logs:{region}:{account_id}:log-group:/aws/lambda/StationFirmwareUpdateManagerLambda:*"
    }
  ]
}
```

---

## Placeholders

Replace the following before deploying:

| Placeholder | Example value |
|---|---|
| `{region}` | `us-east-1` |
| `{account_id}` | `123456789012` |
| `station-firmware-bucket` | your actual S3 bucket name |
