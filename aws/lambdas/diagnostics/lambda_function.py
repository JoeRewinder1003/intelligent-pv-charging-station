import json
import os
from datetime import datetime, timezone
from decimal import Decimal
from typing import Any, Dict, List
from botocore.exceptions import ClientError

import boto3


FAULT_EVENTS_TABLE_NAME = os.environ.get("FAULT_EVENTS_TABLE_NAME", "FaultEvents")
COMMAND_LOG_TABLE_NAME = os.environ.get("COMMAND_LOG_TABLE_NAME", "CommandLog")
STATUS_TABLE_NAME = os.environ.get("STATUS_TABLE_NAME", "StationStatus")


VALID_MESSAGE_TYPES = {"status", "faults", "acks"}

class CommandNotFoundError(Exception):
    """Raised when an ACK references a command that does not exist."""

def lambda_handler(event: Dict[str, Any], context: Any) -> Dict[str, Any]:
    """
    Processes diagnostic messages from AWS IoT Core.

    Supported MQTT topics:
    - station/{station_id}/status
    - station/{station_id}/faults
    - station/{station_id}/acks
    """

    try:
        payload = parse_event(event)
        message_type = detect_message_type(payload)

        errors = validate_diagnostics_payload(payload, message_type)

        if errors:
            return response(
                status_code=400,
                body={
                    "message": "Invalid diagnostics payload",
                    "message_type": message_type,
                    "errors": errors,
                },
            )

        if message_type == "faults":
            save_fault_event(payload)
            update_station_status_from_fault(payload)

        elif message_type == "acks":
            update_command_acknowledgement(payload)

        elif message_type == "status":
            update_station_status(payload)

        else:
            return response(
                status_code=400,
                body={
                    "message": "Unsupported diagnostics message type",
                    "message_type": message_type,
                },
            )

        return response(
            status_code=200,
            body={
                "message": "Diagnostics message processed successfully",
                "message_type": message_type,
                "station_id": payload.get("station_id"),
                "timestamp": payload.get("timestamp"),
            },
        )
    except CommandNotFoundError as exc:
        return response(
            status_code=404,
            body={
                "message": "Command acknowledgement could not be applied",
                "error": str(exc),
            },
        )
    
    except Exception as exc:
        return response(
            status_code=500,
            body={
                "message": "Internal error while processing diagnostics message",
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


def detect_message_type(payload: Dict[str, Any]) -> str:
    """
    Detects whether the payload is a status, fault, or acknowledgement message.

    Priority:
    1. Explicit message_type field from IoT Rule
    2. MQTT topic suffix
    3. Payload structure
    """

    explicit_type = payload.get("message_type")
    if explicit_type in VALID_MESSAGE_TYPES:
        return explicit_type

    mqtt_topic = payload.get("mqtt_topic", "")
    if isinstance(mqtt_topic, str):
        topic_parts = mqtt_topic.split("/")
        if len(topic_parts) >= 3 and topic_parts[-1] in VALID_MESSAGE_TYPES:
            return topic_parts[-1]

    if "command_id" in payload and "command" in payload and "status" in payload:
        return "acks"

    if "fault_code" in payload or "severity" in payload:
        return "faults"

    if "system_state" in payload or "operating_mode" in payload:
        return "status"

    return "unknown"


def validate_diagnostics_payload(payload: Dict[str, Any], message_type: str) -> List[str]:
    errors = []

    station_id = payload.get("station_id")
    timestamp = payload.get("timestamp")

    if not isinstance(station_id, str) or not station_id:
        errors.append("station_id must be a non-empty string")

    if not is_valid_utc_timestamp(timestamp):
        errors.append("timestamp must use UTC format, for example 2026-07-09T21:00:00Z")

    if message_type not in VALID_MESSAGE_TYPES:
        errors.append("message_type must be one of: status, faults, acks")
        return errors

    if message_type == "faults":
        validate_fault_payload(payload, errors)

    elif message_type == "acks":
        validate_ack_payload(payload, errors)

    elif message_type == "status":
        validate_status_payload(payload, errors)

    return errors


def validate_fault_payload(payload: Dict[str, Any], errors: List[str]) -> None:
    if not isinstance(payload.get("fault_state"), str):
        errors.append("fault_state must be a string")

    if not isinstance(payload.get("fault_code"), str):
        errors.append("fault_code must be a string")

    if payload.get("severity") not in {"info", "warning", "critical"}:
        errors.append("severity must be one of: info, warning, critical")


def validate_ack_payload(payload: Dict[str, Any], errors: List[str]) -> None:
    if not isinstance(payload.get("command_id"), str):
        errors.append("command_id must be a string")

    if not isinstance(payload.get("command"), str):
        errors.append("command must be a string")

    if payload.get("status") not in {
        "received",
        "accepted",
        "rejected",
        "blocked_by_safety",
        "invalid_command",
    }:
        errors.append(
            "status must be one of: received, accepted, rejected, "
            "blocked_by_safety, invalid_command"
        )


def validate_status_payload(payload: Dict[str, Any], errors: List[str]) -> None:
    if "operating_mode" not in payload:
        errors.append("operating_mode is required for status messages")

    if "fault_state" not in payload:
        errors.append("fault_state is required for status messages")


def save_fault_event(payload: Dict[str, Any]) -> None:
    dynamodb = boto3.resource("dynamodb")
    table = dynamodb.Table(FAULT_EVENTS_TABLE_NAME)

    item = {
        "station_id": payload["station_id"],
        "timestamp": payload["timestamp"],
        "fault_state": payload.get("fault_state", "unknown"),
        "fault_code": payload.get("fault_code", "UNKNOWN_FAULT"),
        "severity": payload.get("severity", "info"),
        "description": payload.get("description", ""),
        "affected_functions": payload.get("affected_functions", {}),
        "measurements": payload.get("measurements", {}),
        "mqtt_topic": payload.get("mqtt_topic"),
        "received_at_epoch_ms": payload.get("received_at_epoch_ms"),
    }

    table.put_item(Item=convert_floats_to_decimal(item))


def update_command_acknowledgement(payload: Dict[str, Any]) -> None:
    dynamodb = boto3.resource("dynamodb")
    table = dynamodb.Table(COMMAND_LOG_TABLE_NAME)

    try:
        table.update_item(
            Key={
                "station_id": payload["station_id"],
                "command_id": payload["command_id"],
            },
            UpdateExpression=(
                "SET #status = :status, "
                "applied = :applied, "
                "ack_timestamp = :ack_timestamp, "
                "resulting_operating_mode = :resulting_operating_mode, "
                "#message = :message"
            ),
            ConditionExpression=(
                "attribute_exists(station_id) "
                "AND attribute_exists(command_id)"
            ),
            ExpressionAttributeNames={
                "#status": "status",
                "#message": "message",
            },
            ExpressionAttributeValues=convert_floats_to_decimal(
                {
                    ":status": payload["status"],
                    ":applied": bool(payload.get("applied", False)),
                    ":ack_timestamp": payload["timestamp"],
                    ":resulting_operating_mode": payload.get(
                        "resulting_operating_mode"
                    ),
                    ":message": payload.get("message", ""),
                }
            ),
        )

    except ClientError as exc:
        error_code = exc.response.get("Error", {}).get("Code", "")

        if error_code == "ConditionalCheckFailedException":
            raise CommandNotFoundError(
                "No existing CommandLog item matches "
                f"station_id={payload['station_id']} and "
                f"command_id={payload['command_id']}"
            ) from exc

        raise


def update_station_status(payload: Dict[str, Any]) -> None:
    """
    Partially updates StationStatus without deleting telemetry
    or fault attributes written by other message flows.
    """

    dynamodb = boto3.resource("dynamodb")
    table = dynamodb.Table(STATUS_TABLE_NAME)

    updates: Dict[str, Any] = {
        "last_update": payload["timestamp"],
        "operating_mode": payload["operating_mode"],
        "fault_state": payload["fault_state"],
        "updated_at": current_utc_timestamp(),
    }

    optional_fields = [
        "system_state",
        "fis_mode",
        "requested_mode",
        "outputs_active",
        "tracking_allowed",
        "charging_allowed",
        "manual_lock",
        "cloud_connected",
    ]

    for field in optional_fields:
        if field in payload:
            updates[field] = payload[field]

    expression_names: Dict[str, str] = {}
    expression_values: Dict[str, Any] = {}
    set_expressions = []

    for index, (field, value) in enumerate(updates.items()):
        name_placeholder = f"#field_{index}"
        value_placeholder = f":value_{index}"

        expression_names[name_placeholder] = field
        expression_values[value_placeholder] = value

        set_expressions.append(
            f"{name_placeholder} = {value_placeholder}"
        )

    table.update_item(
        Key={
            "station_id": payload["station_id"],
        },
        UpdateExpression="SET " + ", ".join(set_expressions),
        ExpressionAttributeNames=expression_names,
        ExpressionAttributeValues=convert_floats_to_decimal(
            expression_values
        ),
    )


def update_station_status_from_fault(payload: Dict[str, Any]) -> None:
    dynamodb = boto3.resource("dynamodb")
    table = dynamodb.Table(STATUS_TABLE_NAME)

    table.update_item(
        Key={
            "station_id": payload["station_id"],
        },
        UpdateExpression=(
            "SET last_update = :last_update, "
            "fault_state = :fault_state, "
            "last_fault_code = :last_fault_code, "
            "last_fault_severity = :last_fault_severity, "
            "updated_at = :updated_at"
        ),
        ExpressionAttributeValues=convert_floats_to_decimal(
            {
                ":last_update": payload["timestamp"],
                ":fault_state": payload.get("fault_state", "unknown"),
                ":last_fault_code": payload.get("fault_code", "UNKNOWN_FAULT"),
                ":last_fault_severity": payload.get("severity", "info"),
                ":updated_at": current_utc_timestamp(),
            }
        ),
    )


def is_valid_utc_timestamp(value: Any) -> bool:
    if not isinstance(value, str):
        return False

    try:
        if not value.endswith("Z"):
            return False

        datetime.fromisoformat(value.replace("Z", "+00:00"))
        return True

    except ValueError:
        return False


def current_utc_timestamp() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


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