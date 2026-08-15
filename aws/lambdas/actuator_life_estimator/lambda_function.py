import boto3
import json
import math
import os
from decimal import Decimal
from typing import Any, Dict, Optional, Tuple


STROKE_LENGTH_MM = float(
    os.environ.get(
        "STROKE_LENGTH_MM",
        "300.0",
    )
)

DUTY_CYCLE_LIMIT_PERCENT = float(
    os.environ.get(
        "DUTY_CYCLE_LIMIT_PERCENT",
        "25.0",
    )
)

EQUIVALENT_CYCLE_TOLERANCE = 0.001

ACTUATOR_USAGE_TABLE_NAME = "ActuatorUsageHistory"

dynamodb = boto3.resource("dynamodb")
actuator_usage_table = dynamodb.Table(
    ACTUATOR_USAGE_TABLE_NAME
)


def lambda_handler(
    event: Dict[str, Any],
    context: Any,
) -> Dict[str, Any]:
    """
    Processes actuator usage diagnostic events.

    V1 behavior:
    - Validates master and slave actuator usage metrics.
    - Verifies equivalent full-stroke cycle calculations.
    - Verifies duty-cycle warning information.
    - Calculates simple master/slave usage differences.
    - Stores validated usage records in ActuatorUsageHistory.

    This function does not estimate remaining actuator life
    or actuator health percentage.
    """

    try:
        station_id = event.get("station_id")
        timestamp = event.get("timestamp")

        if not isinstance(station_id, str) or not station_id.strip():
            return response(
                400,
                {
                    "message": "station_id is required",
                },
            )

        if not isinstance(timestamp, str) or not timestamp.strip():
            return response(
                400,
                {
                    "message": "timestamp is required",
                },
            )

        master, master_error = validate_actuator_usage(
            event.get("master"),
            actuator_name="master",
        )

        if master_error is not None:
            return response(
                400,
                master_error,
            )

        slave, slave_error = validate_actuator_usage(
            event.get("slave"),
            actuator_name="slave",
        )

        if slave_error is not None:
            return response(
                400,
                slave_error,
            )

        if master["window_sequence"] != slave["window_sequence"]:
            return response(
                400,
                {
                    "message": (
                        "Master and slave window_sequence values "
                        "must match"
                    ),
                    "master_window_sequence": (
                        master["window_sequence"]
                    ),
                    "slave_window_sequence": (
                        slave["window_sequence"]
                    ),
                },
            )

        window_sequence = master["window_sequence"]

        operating_time_difference_s = abs(
            master["operating_time_s"]
            - slave["operating_time_s"]
        )

        travel_difference_mm = abs(
            master["total_travel_mm"]
            - slave["total_travel_mm"]
        )

        duty_cycle_warning = (
            master["duty_cycle_exceeded"]
            or slave["duty_cycle_exceeded"]
        )

        result = {
            "station_id": station_id,
            "timestamp": timestamp,
            "event_type": "actuator_usage",
            "window_sequence": window_sequence,
            "master": master,
            "slave": slave,
            "operating_time_difference_s": round(
                operating_time_difference_s,
                3,
            ),
            "travel_difference_mm": round(
                travel_difference_mm,
                3,
            ),
            "duty_cycle_warning": duty_cycle_warning,
            "duty_cycle_limit_percent": (
                DUTY_CYCLE_LIMIT_PERCENT
            ),
            "stroke_length_mm": STROKE_LENGTH_MM,
        }

        history_item = convert_to_decimal(result)

        actuator_usage_table.put_item(
            Item=history_item
        )

        print(
            json.dumps(
                {
                    "event": "actuator_usage_processed",
                    **result,
                }
            )
        )

        return response(
            200,
            {
                "message": (
                    "Actuator usage event processed successfully"
                ),
                **result,
            },
        )

    except Exception as exc:
        print(
            json.dumps(
                {
                    "event": "actuator_lifetime_estimator_error",
                    "error": str(exc),
                }
            )
        )

        return response(
            500,
            {
                "message": (
                    "Actuator lifetime estimator failed"
                ),
                "error": str(exc),
            },
        )


def validate_actuator_usage(
    data: Any,
    actuator_name: str,
) -> Tuple[
    Optional[Dict[str, Any]],
    Optional[Dict[str, Any]],
]:
    """
    Validates one actuator usage record.

    Equivalent full-stroke cycles are checked using:

        cycles = total_travel_mm / (2 * stroke_length_mm)

    The calculation is a usage normalization only. It is not
    a degradation or remaining-life model.
    """

    if not isinstance(data, dict):
        return None, {
            "message": (
                f"{actuator_name} actuator data is required"
            ),
        }

    window_sequence = normalize_nonnegative_integer(
        data.get("window_sequence")
    )

    operating_time_s = normalize_nonnegative_number(
        data.get("operating_time_s")
    )

    total_travel_mm = normalize_nonnegative_number(
        data.get("total_travel_mm")
    )

    movement_starts = normalize_nonnegative_integer(
        data.get("movement_starts")
    )

    reported_equivalent_cycles = normalize_nonnegative_number(
        data.get("equivalent_full_stroke_cycles")
    )

    duty_cycle_percent = normalize_percent(
        data.get("duty_cycle_percent")
    )

    duty_cycle_window_available = normalize_boolean(
        data.get("duty_cycle_window_available")
    )

    reported_duty_cycle_exceeded = normalize_boolean(
        data.get("duty_cycle_exceeded")
    )

    required_values = {
        "window_sequence": window_sequence,
        "operating_time_s": operating_time_s,
        "total_travel_mm": total_travel_mm,
        "movement_starts": movement_starts,
        "equivalent_full_stroke_cycles": (
            reported_equivalent_cycles
        ),
        "duty_cycle_percent": duty_cycle_percent,
        "duty_cycle_window_available": (
            duty_cycle_window_available
        ),
        "duty_cycle_exceeded": (
            reported_duty_cycle_exceeded
        ),
    }

    invalid_fields = [
        name
        for name, value in required_values.items()
        if value is None
    ]

    if invalid_fields:
        return None, {
            "message": (
                f"Invalid or missing {actuator_name} "
                "actuator usage fields"
            ),
            "fields": invalid_fields,
        }

    calculated_equivalent_cycles = (
        total_travel_mm /
        (2.0 * STROKE_LENGTH_MM)
    )

    equivalent_cycle_difference = abs(
        reported_equivalent_cycles
        - calculated_equivalent_cycles
    )

    if (
        equivalent_cycle_difference
        > EQUIVALENT_CYCLE_TOLERANCE
    ):
        return None, {
            "message": (
                f"{actuator_name} equivalent full-stroke "
                "cycle value is inconsistent with total travel"
            ),
            "reported_equivalent_full_stroke_cycles": (
                reported_equivalent_cycles
            ),
            "calculated_equivalent_full_stroke_cycles": round(
                calculated_equivalent_cycles,
                6,
            ),
        }

    calculated_duty_cycle_exceeded = False

    if duty_cycle_window_available:
        calculated_duty_cycle_exceeded = (
            duty_cycle_percent
            > DUTY_CYCLE_LIMIT_PERCENT
        )

    if (
        reported_duty_cycle_exceeded
        != calculated_duty_cycle_exceeded
    ):
        return None, {
            "message": (
                f"{actuator_name} duty_cycle_exceeded value "
                "is inconsistent with the configured limit"
            ),
            "reported_duty_cycle_exceeded": (
                reported_duty_cycle_exceeded
            ),
            "calculated_duty_cycle_exceeded": (
                calculated_duty_cycle_exceeded
            ),
            "duty_cycle_percent": duty_cycle_percent,
            "duty_cycle_limit_percent": (
                DUTY_CYCLE_LIMIT_PERCENT
            ),
        }

    result = {
        "window_sequence": window_sequence,
        "operating_time_s": round(
            operating_time_s,
            3,
        ),
        "total_travel_mm": round(
            total_travel_mm,
            3,
        ),
        "movement_starts": movement_starts,
        "equivalent_full_stroke_cycles": round(
            calculated_equivalent_cycles,
            6,
        ),
        "duty_cycle_percent": round(
            duty_cycle_percent,
            2,
        ),
        "duty_cycle_window_available": (
            duty_cycle_window_available
        ),
        "duty_cycle_exceeded": (
            calculated_duty_cycle_exceeded
        ),
    }

    return result, None


def normalize_number(
    value: Any,
) -> Optional[float]:
    """
    Converts a numeric input to a finite float.
    """

    if isinstance(value, bool):
        return None

    if isinstance(value, Decimal):
        value = float(value)

    if not isinstance(value, (int, float)):
        return None

    numeric_value = float(value)

    if not math.isfinite(numeric_value):
        return None

    return numeric_value


def normalize_nonnegative_number(
    value: Any,
) -> Optional[float]:
    """
    Validates a finite number greater than or equal to zero.
    """

    numeric_value = normalize_number(value)

    if numeric_value is None:
        return None

    if numeric_value < 0.0:
        return None

    return numeric_value


def normalize_nonnegative_integer(
    value: Any,
) -> Optional[int]:
    """
    Validates a non-negative integer.
    """

    if isinstance(value, bool):
        return None

    if not isinstance(value, int):
        return None

    if value < 0:
        return None

    return value


def normalize_percent(
    value: Any,
) -> Optional[float]:
    """
    Validates a percentage from 0 to 100.
    """

    numeric_value = normalize_number(value)

    if numeric_value is None:
        return None

    if numeric_value < 0.0 or numeric_value > 100.0:
        return None

    return numeric_value


def normalize_boolean(
    value: Any,
) -> Optional[bool]:
    """
    Validates a boolean input.
    """

    if not isinstance(value, bool):
        return None

    return value


def convert_to_decimal(
    value: Any,
) -> Any:
    """
    Recursively converts numeric values to Decimal for DynamoDB.
    """

    if isinstance(value, bool):
        return value

    if isinstance(value, float):
        return Decimal(str(value))

    if isinstance(value, int):
        return Decimal(str(value))

    if isinstance(value, dict):
        return {
            key: convert_to_decimal(item)
            for key, item in value.items()
        }

    if isinstance(value, list):
        return [
            convert_to_decimal(item)
            for item in value
        ]

    return value


def response(
    status_code: int,
    body: Dict[str, Any],
) -> Dict[str, Any]:
    """
    Builds the standard Lambda response structure.
    """

    return {
        "statusCode": status_code,
        "body": json.dumps(body),
    }