import json
import os
from datetime import datetime, timezone
from decimal import Decimal
from typing import Any, Dict, List, Tuple

import boto3


dynamodb = boto3.resource("dynamodb")

TELEMETRY_TABLE_NAME = os.environ.get("TELEMETRY_TABLE_NAME", "TelemetryHistory")
STATUS_TABLE_NAME = os.environ.get("STATUS_TABLE_NAME", "StationStatus")

telemetry_table = dynamodb.Table(TELEMETRY_TABLE_NAME)
status_table = dynamodb.Table(STATUS_TABLE_NAME)


REQUIRED_TOP_LEVEL_FIELDS = [
    "station_id",
    "timestamp",
    "battery",
    "pv",
    "decision",
    "fault_state",
]


def lambda_handler(event: Dict[str, Any], context: Any) -> Dict[str, Any]:
    """
    Processes telemetry messages received from AWS IoT Core.

    Expected input:
    {
      "station_id": "station_001",
      "timestamp": "2026-07-09T21:00:00Z",
      "battery": {...},
      "pv": {...},
      "environment": {...},
      "outputs": {...},
      "tracking": {...},
      "decision": {...},
      "fault_state": "normal"
    }
    """

    try:
        payload = parse_event(event)
        errors = validate_payload(payload)

        if errors:
            return response(
                status_code=400,
                body={
                    "message": "Invalid telemetry payload",
                    "errors": errors,
                },
            )

        item = convert_floats_to_decimal(payload)

        telemetry_table.put_item(Item=item)

        update_station_status(payload)

        return response(
            status_code=200,
            body={
                "message": "Telemetry processed successfully",
                "station_id": payload["station_id"],
                "timestamp": payload["timestamp"],
            },
        )

    except Exception as exc:
        return response(
            status_code=500,
            body={
                "message": "Internal error while processing telemetry",
                "error": str(exc),
            },
        )


def parse_event(event: Dict[str, Any]) -> Dict[str, Any]:
    """
    Normalizes the Lambda input event.

    AWS IoT Rules may pass the MQTT payload directly as a JSON object.
    In local tests, the event may also contain a JSON string under 'body'.
    """

    if "body" in event:
        body = event["body"]

        if isinstance(body, str):
            return json.loads(body)

        if isinstance(body, dict):
            return body

    return event


def validate_payload(payload: Dict[str, Any]) -> List[str]:
    errors = []

    for field in REQUIRED_TOP_LEVEL_FIELDS:
        if field not in payload:
            errors.append(f"Missing required field: {field}")

    if errors:
        return errors

    if not isinstance(payload["station_id"], str) or not payload["station_id"]:
        errors.append("station_id must be a non-empty string")

    if not is_valid_utc_timestamp(payload["timestamp"]):
        errors.append("timestamp must use UTC format, for example 2026-07-09T21:00:00Z")

    battery = payload.get("battery", {})
    pv = payload.get("pv", {})
    decision = payload.get("decision", {})

    validate_numeric_field(battery, "soc_percent", errors, min_value=0, max_value=100)
    validate_numeric_field(battery, "voltage_v", errors, min_value=0)
    validate_numeric_field(battery, "current_a", errors)
    validate_numeric_field(battery, "power_w", errors)

    validate_numeric_field(pv, "local_irradiance_wm2", errors, min_value=0)
    validate_numeric_field(pv, "voltage_v", errors, min_value=0)
    validate_numeric_field(pv, "current_a", errors, min_value=0)
    validate_numeric_field(pv, "power_w", errors, min_value=0)

    validate_numeric_field(decision, "weather_index", errors, min_value=0, max_value=1)
    validate_numeric_field(decision, "demand_index", errors, min_value=0, max_value=1)

    if "operating_mode" not in decision:
        errors.append("decision.operating_mode is required")

    return errors


def validate_numeric_field(
    parent: Dict[str, Any],
    field: str,
    errors: List[str],
    min_value: float | None = None,
    max_value: float | None = None,
) -> None:
    value = parent.get(field)

    if value is None:
        errors.append(f"Missing numeric field: {field}")
        return

    if not isinstance(value, (int, float)):
        errors.append(f"{field} must be numeric")
        return

    if min_value is not None and value < min_value:
        errors.append(f"{field} must be >= {min_value}")

    if max_value is not None and value > max_value:
        errors.append(f"{field} must be <= {max_value}")


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


def update_station_status(payload: Dict[str, Any]) -> None:
    """
    Partially updates the latest station state without deleting
    attributes written by other Lambda functions.
    """

    decision = payload.get("decision", {})
    outputs = payload.get("outputs", {})
    tracking = payload.get("tracking", {})

    updates: Dict[str, Any] = {
        "last_update": payload["timestamp"],
        "operating_mode": decision["operating_mode"],
        "fault_state": payload["fault_state"],
        "updated_at": datetime.now(timezone.utc)
        .isoformat()
        .replace("+00:00", "Z"),
    }

    if "fis_mode" in decision:
        updates["fis_mode"] = decision["fis_mode"]

    if "requested_mode" in decision:
        updates["requested_mode"] = decision["requested_mode"]

    if "outputs" in payload:
        updates["outputs_active"] = sum(
            [
                bool(outputs.get("output_1_active", False)),
                bool(outputs.get("output_2_active", False)),
                bool(outputs.get("output_3_active", False)),
            ]
        )

    if "enabled" in tracking:
        updates["tracking_allowed"] = bool(tracking["enabled"])

    update_station_status_fields(
        station_id=payload["station_id"],
        updates=updates,
    )


def update_station_status_fields(
    station_id: str,
    updates: Dict[str, Any],
) -> None:
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

    status_table.update_item(
        Key={
            "station_id": station_id,
        },
        UpdateExpression="SET " + ", ".join(set_expressions),
        ExpressionAttributeNames=expression_names,
        ExpressionAttributeValues=convert_floats_to_decimal(
            expression_values
        ),
    )


def convert_floats_to_decimal(data: Any) -> Any:
    """
    DynamoDB does not accept Python float values directly.
    This function converts floats to Decimal recursively.
    """

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