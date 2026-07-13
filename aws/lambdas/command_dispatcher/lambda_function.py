import json
import os
import uuid
from datetime import datetime, timezone
from decimal import Decimal
from typing import Any, Dict, List

import boto3


COMMAND_LOG_TABLE_NAME = os.environ.get("COMMAND_LOG_TABLE_NAME", "CommandLog")
AWS_IOT_DATA_ENDPOINT = os.environ.get("AWS_IOT_DATA_ENDPOINT", "")


ALLOWED_COMMANDS = {
    "AUTO",
    "STOP",
    "NEUTRAL",
    "ENABLE_TRACKING",
    "DISABLE_TRACKING",
    "ENABLE_OUTPUT_1",
    "ENABLE_OUTPUT_2",
    "ENABLE_OUTPUT_3",
    "DISABLE_OUTPUTS",
    "LOCKOUT",
    "CLEAR_LOCKOUT",
}


def lambda_handler(event: Dict[str, Any], context: Any) -> Dict[str, Any]:
    """
    Publishes a cloud-generated command to the ESP32 through AWS IoT Core.

    Expected event:
    {
      "station_id": "station_001",
      "command": "ENABLE_OUTPUT_2",
      "source": "cloud_fis",
      "parameters": {
        "requested_mode": "M4",
        "max_outputs": 2,
        "tracking_allowed": true
      }
    }
    """

    try:
        payload = parse_event(event)
        errors = validate_command(payload)

        if errors:
            return response(
                status_code=400,
                body={
                    "message": "Invalid command payload",
                    "errors": errors,
                },
            )

        command_payload = build_command_payload(payload)
        topic = f"station/{command_payload['station_id']}/commands"

        save_command_to_dynamodb(command_payload)

        mqtt_payload = dict(command_payload)
        mqtt_payload["status"] = "sent"

        try:
            publish_command_to_iot(topic, mqtt_payload)

        except Exception as publish_error:
            update_command_status(
                command_payload=command_payload,
                status="failed",
                error_message=str(publish_error),
            )
            raise

        update_command_status(
            command_payload=command_payload,
            status="sent",
        )

        return response(
            status_code=200,
            body={
                "message": "Command published successfully",
                "station_id": command_payload["station_id"],
                "command_id": command_payload["command_id"],
                "command": command_payload["command"],
                "topic": topic,
                "status": "sent",
            },
        )

    except Exception as exc:
        return response(
            status_code=500,
            body={
                "message": "Internal error while dispatching command",
                "error": str(exc),
            },
        )


def parse_event(event: Dict[str, Any]) -> Dict[str, Any]:
    if "body" in event:
        body = event["body"]

        if isinstance(body, str):
            return json.loads(body)

        if isinstance(body, dict):
            return body

    return event


def validate_command(payload: Dict[str, Any]) -> List[str]:
    errors = []

    station_id = payload.get("station_id")
    command = payload.get("command")

    if not isinstance(station_id, str) or not station_id:
        errors.append("station_id must be a non-empty string")

    if not isinstance(command, str) or not command:
        errors.append("command must be a non-empty string")
    elif command not in ALLOWED_COMMANDS:
        errors.append(f"command must be one of: {sorted(ALLOWED_COMMANDS)}")

    parameters = payload.get("parameters", {})
    if parameters is not None and not isinstance(parameters, dict):
        errors.append("parameters must be an object")

    source = payload.get("source", "manual_cloud")
    if not isinstance(source, str) or not source:
        errors.append("source must be a non-empty string")

    return errors


def build_command_payload(payload: Dict[str, Any]) -> Dict[str, Any]:
    now_utc = datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")

    command_id = payload.get("command_id")
    if not command_id:
        command_id = f"cmd-{uuid.uuid4()}"

    return {
        "station_id": payload["station_id"],
        "timestamp": payload.get("timestamp", now_utc),
        "command_id": command_id,
        "command": payload["command"],
        "source": payload.get("source", "manual_cloud"),
        "parameters": payload.get("parameters", {}),
        "status": "pending",
        "created_at": now_utc,
    }


def save_command_to_dynamodb(command_payload: Dict[str, Any]) -> None:
    dynamodb = boto3.resource("dynamodb")
    table = dynamodb.Table(COMMAND_LOG_TABLE_NAME)

    item = convert_floats_to_decimal(command_payload)
    table.put_item(Item=item)

def update_command_status(
    command_payload: Dict[str, Any],
    status: str,
    error_message: str | None = None,
) -> None:
    dynamodb = boto3.resource("dynamodb")
    table = dynamodb.Table(COMMAND_LOG_TABLE_NAME)

    now_utc = datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")

    update_expression = (
        "SET #status = :status, "
        "updated_at = :updated_at"
    )

    expression_names = {
        "#status": "status",
    }

    expression_values = {
        ":status": status,
        ":updated_at": now_utc,
    }

    if error_message is not None:
        update_expression += ", error_message = :error_message"
        expression_values[":error_message"] = error_message

    table.update_item(
        Key={
            "station_id": command_payload["station_id"],
            "command_id": command_payload["command_id"],
        },
        UpdateExpression=update_expression,
        ExpressionAttributeNames=expression_names,
        ExpressionAttributeValues=convert_floats_to_decimal(
            expression_values
        ),
    )

def publish_command_to_iot(topic: str, command_payload: Dict[str, Any]) -> None:
    if not AWS_IOT_DATA_ENDPOINT:
        raise ValueError("AWS_IOT_DATA_ENDPOINT environment variable is required")

    iot_client = boto3.client(
        "iot-data",
        endpoint_url=f"https://{AWS_IOT_DATA_ENDPOINT}",
    )

    iot_client.publish(
        topic=topic,
        qos=1,
        payload=json.dumps(command_payload).encode("utf-8"),
    )


def convert_floats_to_decimal(data: Any) -> Any:
    if isinstance(data, list):
        return [convert_floats_to_decimal(item) for item in data]

    if isinstance(data, dict):
        return {
            key: convert_floats_to_decimal(value)
            for key, value in data.items()
        }

    if isinstance(data, float):
        return Decimal(str(data))

    return data


def response(status_code: int, body: Dict[str, Any]) -> Dict[str, Any]:
    return {
        "statusCode": status_code,
        "body": json.dumps(body),
    }