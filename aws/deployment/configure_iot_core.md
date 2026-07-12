# Configure AWS IoT Core

This document describes the initial manual procedure for configuring AWS IoT Core for the ESP32-based solar charging station.

AWS IoT Core will be used as the MQTT broker between the ESP32 station and the AWS cloud backend.

## Purpose

AWS IoT Core is responsible for:

```text
Receiving telemetry from the ESP32
Routing MQTT messages to Lambda functions
Publishing cloud-generated commands to the ESP32
Managing device certificates and IoT policies
```

## Initial Device

For the first prototype, create one IoT thing:

```text
station_001
```

This represents the first ESP32-based solar charging station.

---

## 1. Create IoT Thing

Create a thing named:

```text
station_001
```

Recommended thing type:

```text
SolarChargingStation
```

Optional attributes:

| Attribute         | Value         |
| ----------------- | ------------- |
| `station_id`      | `station_001` |
| `location`        | `UPIITA`      |
| `controller`      | `ESP32`       |
| `prototype_stage` | `cloud_v1`    |

---

## 2. Create Device Certificate

Create a new certificate for the ESP32.

Download and store the following files securely:

```text
Device certificate
Private key
Public key
Amazon Root CA certificate
```

Important:

```text
Never commit certificates, private keys, or secrets to Git.
```

These files should remain excluded by `.gitignore`.

Recommended local folder, outside Git or ignored by Git:

```text
aws/certs/
```

The private key must be protected because it allows the device to authenticate with AWS IoT Core.

---

## 3. Attach Certificate to Thing

Attach the generated certificate to:

```text
station_001
```

This links the device identity to the IoT thing.

---

## 4. Create IoT Policy

Create an IoT policy that allows the ESP32 to connect, publish telemetry, and subscribe to commands.

Recommended policy name:

```text
station_001_iot_policy
```

Policy draft:

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

Replace the following values before using the policy:

```text
{region}
{account_id}
```

Example region:

```text
us-east-2
```

---

## 5. Attach IoT Policy to Certificate

Attach the policy:

```text
station_001_iot_policy
```

to the device certificate.

Without this step, the ESP32 certificate may exist, but the device will not have permission to connect, publish, or subscribe.

---

## 6. Get AWS IoT Data Endpoint

The ESP32 and the `command_dispatcher` Lambda need the AWS IoT Data endpoint.

It usually has a format similar to:

```text
xxxxxxxxxxxxx-ats.iot.{region}.amazonaws.com
```

Store this endpoint for later use.

It will be needed in:

```text
ESP32 firmware
command_dispatcher Lambda environment variable
```

Lambda environment variable:

```text
AWS_IOT_DATA_ENDPOINT=<your-iot-data-endpoint>
```

---

## 7. Create IoT Rules

Create the AWS IoT Core rules documented in:

```text
aws/infrastructure/iot_rules.sql
```

Minimum rules:

```text
station_telemetry_to_lambda
station_status_to_lambda
station_faults_to_lambda
station_acks_to_lambda
```

Recommended routing:

| MQTT Topic Filter     | Target Lambda         |
| --------------------- | --------------------- |
| `station/+/telemetry` | `telemetry_processor` |
| `station/+/status`    | `diagnostics`         |
| `station/+/faults`    | `diagnostics`         |
| `station/+/acks`      | `diagnostics`         |

Each IoT rule should use the SQL version:

```text
2016-03-23
```

---

## 8. Allow IoT Rules to Invoke Lambda

Each IoT rule that targets a Lambda function needs permission to invoke that Lambda.

Conceptually:

```text
AWS IoT Core rule → Lambda invoke permission → target Lambda
```

The target Lambdas are:

```text
telemetry_processor
diagnostics
```

If this permission is missing, the MQTT message may reach AWS IoT Core but fail to trigger the Lambda.

---

## 9. Test with MQTT Test Client

Before connecting the ESP32, use the AWS IoT MQTT test client.

Subscribe to:

```text
station/station_001/telemetry
station/station_001/status
station/station_001/faults
station/station_001/acks
station/station_001/commands
```

Then publish test messages to:

```text
station/station_001/telemetry
station/station_001/status
station/station_001/faults
station/station_001/acks
```

Confirm that:

```text
Telemetry messages trigger telemetry_processor
Status messages trigger diagnostics
Fault messages trigger diagnostics
Acknowledgement messages trigger diagnostics
DynamoDB tables are updated
CloudWatch logs show successful processing
```

---

## 10. Command Topic

The cloud backend publishes commands to:

```text
station/station_001/commands
```

The ESP32 should subscribe to this topic.

Command publishing is performed by:

```text
command_dispatcher
```

The ESP32 must validate every command locally before applying it.

---

## 11. Safety Rule

AWS IoT Core only transports messages. It does not authorize physical actions.

Therefore:

```text
AWS IoT Core = MQTT communication and routing
Lambda = cloud-side processing
ESP32 deterministic layer = final physical authorization
```

Cloud-generated commands must never bypass the local safety logic of the ESP32.

---


## Created IoT Rules

The following AWS IoT Core rules were created in region `us-east-2`:

| Rule | MQTT topic filter | Target Lambda |
|---|---|---|
| `station_telemetry_to_lambda` | `station/+/telemetry` | `telemetry_processor` |
| `station_status_to_lambda` | `station/+/status` | `diagnostics` |
| `station_faults_to_lambda` | `station/+/faults` | `diagnostics` |
| `station_acks_to_lambda` | `station/+/acks` | `diagnostics` |

The optional combined diagnostics rule was not created to avoid duplicate Lambda invocations.
## Certificate Validation

The certificate and IoT policy assigned to `station_001` were successfully validated using an external MQTT client.

The following permissions were confirmed:

- `iot:Connect`
- `iot:Publish`
- `iot:Subscribe`
- `iot:Receive`

Both telemetry publishing and command reception were tested successfully using QoS 1.