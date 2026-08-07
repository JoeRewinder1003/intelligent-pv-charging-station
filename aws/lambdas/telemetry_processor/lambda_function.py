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

FIS_PROCESSOR_FUNCTION_NAME = os.environ.get(
    "FIS_PROCESSOR_FUNCTION_NAME",
    "fis_processor",
)
STALE_DATA_THRESHOLD_SECONDS = float(
    os.environ.get("STALE_DATA_THRESHOLD_SECONDS", "30")
)


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

    The function validates and stores the station telemetry, evaluates the
    measurement age using the station timestamp and the AWS IoT reception
    timestamp, updates the latest station status, and then invokes the
    cloud-side FIS processor with the validated payload.

    Stale-data detection is based only on timestamps. Simulation flags such as
    ``stale_data_requested`` are stored for traceability but are not used to
    decide whether the message is stale.
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

        temporal_validation = evaluate_temporal_validation(payload)
        station_reported_fault_state = payload.get("fault_state", "normal")
        effective_fault_state = derive_effective_fault_state(
            station_reported_fault_state=station_reported_fault_state,
            temporal_validation=temporal_validation,
        )

        payload["cloud_validation"] = temporal_validation
        payload["station_reported_fault_state"] = station_reported_fault_state
        payload["cloud_effective_fault_state"] = effective_fault_state

        item = convert_floats_to_decimal(payload)
        telemetry_table.put_item(Item=item)

        update_station_status(
            payload=payload,
            effective_fault_state=effective_fault_state,
        )

        fis_payload = build_fis_payload(
            payload=payload,
            effective_fault_state=effective_fault_state,
        )
        fis_result = invoke_fis_processor(fis_payload)

        print(
            json.dumps(
                {
                    "event": "telemetry_processed",
                    "station_id": payload["station_id"],
                    "timestamp": payload["timestamp"],
                    "data_age_s": temporal_validation["data_age_s"],
                    "stale_detected": temporal_validation["stale_detected"],
                    "station_reported_fault_state": station_reported_fault_state,
                    "cloud_effective_fault_state": effective_fault_state,
                    "fis_status_code": fis_result.get("status_code"),
                }
            )
        )

        return response(
            status_code=200,
            body={
                "message": "Telemetry processed successfully",
                "station_id": payload["station_id"],
                "timestamp": payload["timestamp"],
                "cloud_validation": temporal_validation,
                "cloud_effective_fault_state": effective_fault_state,
                "fis_result": fis_result,
            },
        )

    except Exception as exc:
        print(
            json.dumps(
                {
                    "event": "telemetry_processing_error",
                    "error": str(exc),
                }
            )
        )
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


def parse_utc_timestamp(value: str) -> datetime:
    normalized = value[:-1] + "+00:00" if value.endswith("Z") else value
    parsed = datetime.fromisoformat(normalized)

    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)

    return parsed.astimezone(timezone.utc)


def resolve_received_at_epoch_ms(payload: Dict[str, Any]) -> int:
    value = payload.get("received_at_epoch_ms")

    if isinstance(value, bool):
        value = None

    if isinstance(value, (int, float)):
        return int(value)

    return int(datetime.now(timezone.utc).timestamp() * 1000)


def evaluate_temporal_validation(payload: Dict[str, Any]) -> Dict[str, Any]:
    """
    Calculate telemetry age using the station timestamp and AWS reception time.

    ``received_at_epoch_ms`` is added by the AWS IoT SQL rule. Local tests may
    omit it; in that case the Lambda invocation time is used.
    """

    measurement_time = parse_utc_timestamp(payload["timestamp"])
    received_at_epoch_ms = resolve_received_at_epoch_ms(payload)
    received_time = datetime.fromtimestamp(
        received_at_epoch_ms / 1000.0,
        tz=timezone.utc,
    )

    signed_age_seconds = (
        received_time - measurement_time
    ).total_seconds()
    data_age_seconds = max(0.0, signed_age_seconds)
    stale_detected = data_age_seconds > STALE_DATA_THRESHOLD_SECONDS

    return {
        "measurement_timestamp": payload["timestamp"],
        "received_at_epoch_ms": received_at_epoch_ms,
        "data_age_s": round(data_age_seconds, 3),
        "stale_threshold_s": STALE_DATA_THRESHOLD_SECONDS,
        "stale_detected": stale_detected,
    }


def derive_effective_fault_state(
    station_reported_fault_state: str,
    temporal_validation: Dict[str, Any],
) -> str:
    """
    Convert a stale measurement into the cloud FIS data-fault state.

    A critical lockout reported by the station always has higher priority.
    """

    if station_reported_fault_state == "critical_lockout":
        return "critical_lockout"

    if temporal_validation["stale_detected"]:
        return "data_or_sensor_fault"

    return station_reported_fault_state


def build_fis_payload(
    payload: Dict[str, Any],
    effective_fault_state: str,
) -> Dict[str, Any]:
    fis_payload = dict(payload)
    fis_payload["fault_state"] = effective_fault_state
    return fis_payload


def invoke_fis_processor(payload: Dict[str, Any]) -> Dict[str, Any]:
    lambda_client = boto3.client("lambda")
    invoke_result = lambda_client.invoke(
        FunctionName=FIS_PROCESSOR_FUNCTION_NAME,
        InvocationType="RequestResponse",
        Payload=json.dumps(payload).encode("utf-8"),
    )

    payload_stream = invoke_result.get("Payload")
    raw_payload = payload_stream.read() if payload_stream else b"{}"

    if isinstance(raw_payload, bytes):
        raw_payload = raw_payload.decode("utf-8")

    downstream_result = json.loads(raw_payload or "{}")

    if invoke_result.get("FunctionError"):
        raise RuntimeError(
            "fis_processor Lambda execution failed: "
            + json.dumps(downstream_result)
        )

    status_code = downstream_result.get("statusCode", 500)
    body = downstream_result.get("body")

    if isinstance(body, str):
        try:
            body = json.loads(body)
        except json.JSONDecodeError:
            pass

    if status_code >= 400:
        raise RuntimeError(
            "fis_processor returned an error: "
            + json.dumps(downstream_result)
        )

    return {
        "status_code": status_code,
        "body": body,
    }


def update_station_status(
    payload: Dict[str, Any],
    effective_fault_state: str | None = None,
) -> None:
    """
    Partially updates the latest station state without deleting
    attributes written by other Lambda functions.

    ``effective_fault_state`` is optional to preserve compatibility with
    existing local tests and helper callers. When omitted, the function uses
    the cloud-derived fault state when available and otherwise falls back to
    the station-reported fault state. Temporal-validation fields are added
    only when they are present in the payload.
    """

    decision = payload.get("decision", {})
    outputs = payload.get("outputs", {})
    tracking = payload.get("tracking", {})
    cloud_validation = payload.get("cloud_validation", {})

    station_reported_fault_state = payload.get(
        "station_reported_fault_state",
        payload.get("fault_state", "normal"),
    )

    if effective_fault_state is None:
        effective_fault_state = payload.get(
            "cloud_effective_fault_state",
            station_reported_fault_state,
        )

    updates: Dict[str, Any] = {
        "last_update": payload["timestamp"],
        "operating_mode": decision["operating_mode"],
        "fault_state": effective_fault_state,
        "station_reported_fault_state": station_reported_fault_state,
        "updated_at": datetime.now(timezone.utc)
        .isoformat()
        .replace("+00:00", "Z"),
    }

    if "data_age_s" in cloud_validation:
        updates["telemetry_data_age_s"] = cloud_validation["data_age_s"]

    if "stale_detected" in cloud_validation:
        updates["telemetry_stale_detected"] = cloud_validation["stale_detected"]

    if "received_at_epoch_ms" in cloud_validation:
        updates["last_received_at_epoch_ms"] = cloud_validation[
            "received_at_epoch_ms"
        ]

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