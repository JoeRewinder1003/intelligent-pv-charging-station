# AWS Deployment

## Deployment order

1. `infrastructure/dynamodb_tables.json`   → create tables in DynamoDB
2. `infrastructure/iam_policies/`          → create roles in IAM
3. AWS IoT Core console                    → register Thing, download certs
4. `infrastructure/iot_rules.json`         → create IoT Rules
5. `lambdas/telemetry_processor/`          → deploy Lambda (zip + upload)
6. `lambdas/diagnostics/`                  → deploy Lambda (zip + upload)

## Lambda runtime

- Python 3.11
- Architecture: x86_64
- Memory: 128 MB (sufficient for FIS with numpy)
- Timeout: 10 seconds

## Environment variables (set in Lambda console)

| Variable | Example value |
|---|---|
| STATION_TABLE | StationState |
| TELEMETRY_TABLE | StationTelemetry |
| COMMANDS_TABLE | StationCommands |
| IOT_ENDPOINT | xxxxxx-ats.iot.us-east-1.amazonaws.com |
| AWS_REGION | us-east-1 |
