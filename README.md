# station_cloud_v1

Solar PV charging station — AWS cloud architecture v1.

## Structure

```
esp32/          ESP32 firmware (Arduino)
aws/
  lambdas/      Lambda functions
  fis/          Fuzzy logic (Weather FIS + Main Decision FIS)
  infrastructure/  IoT Rules, DynamoDB schema, IAM policies
docs/           Architecture diagrams and notes
```

## Requirements

- AWS account with Free Tier
- Arduino IDE with ESP32 board support
- Python 3.11 (for Lambda functions)

## Deployment order

1. Create DynamoDB tables (infrastructure/dynamodb_tables.json)
2. Create IAM roles (infrastructure/iam_policies/)
3. Register Thing in AWS IoT Core and download certificates
4. Create IoT Rules (infrastructure/iot_rules.json)
5. Deploy Lambda functions (aws/lambdas/)
6. Flash ESP32 firmware (esp32/main/)
