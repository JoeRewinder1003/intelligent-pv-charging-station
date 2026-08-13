import boto3
import json
import math
import os
from decimal import Decimal
from typing import Any, Dict, Optional


MIN_CURRENT_STEP_A = float(
    os.environ.get(
        "MIN_CURRENT_STEP_A",
        "10.0",
    )
)

BATTERY_HEALTH_TABLE_NAME = "BatteryHealthHistory"

dynamodb = boto3.resource("dynamodb")
battery_health_table = dynamodb.Table(BATTERY_HEALTH_TABLE_NAME)

def lambda_handler(
    event: Dict[str, Any],
    context: Any,
) -> Dict[str, Any]:
    """
    Processes battery diagnostic events.

    V1 behavior:
    - Supports resistance_step events.
    - Validates SmartShunt-equivalent voltage and current measurements.
    - Calculates apparent battery-bank resistance in AWS.
    - Does not calculate battery SOH percentage from resistance.
    - Stores validated resistance-step results in BatteryHealthHistory.

    Capacity-based SOH assessment will be added separately.
    """

    try:
        station_id = event.get("station_id")
        timestamp = event.get("timestamp")
        event_type = event.get("event_type")

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

        if event_type != "resistance_step":
            return response(
                400,
                {
                    "message": "Unsupported battery diagnostic event",
                    "event_type": event_type,
                },
            )

        return process_resistance_step(
            event=event,
            station_id=station_id,
            timestamp=timestamp,
        )

    except Exception as exc:
        print(
            json.dumps(
                {
                    "event": "soh_estimator_error",
                    "error": str(exc),
                }
            )
        )

        return response(
            500,
            {
                "message": "SOH estimator failed",
                "error": str(exc),
            },
        )


def process_resistance_step(
    event: Dict[str, Any],
    station_id: str,
    timestamp: str,
) -> Dict[str, Any]:
    """
    Calculates apparent battery-bank resistance from a load-step event.

    Only SmartShunt-equivalent measurements are used.
    """

    voltage_before_v = normalize_number(
        event.get("voltage_before_v")
    )
    voltage_after_v = normalize_number(
        event.get("voltage_after_v")
    )
    current_before_a = normalize_number(
        event.get("current_before_a")
    )
    current_after_a = normalize_number(
        event.get("current_after_a")
    )
    soc_percent = normalize_soc(
        event.get("soc_percent")
    )

    required_values = {
        "voltage_before_v": voltage_before_v,
        "voltage_after_v": voltage_after_v,
        "current_before_a": current_before_a,
        "current_after_a": current_after_a,
        "soc_percent": soc_percent,
    }

    missing_or_invalid = [
        name
        for name, value in required_values.items()
        if value is None
    ]

    if missing_or_invalid:
        return response(
            400,
            {
                "message": (
                    "Invalid or missing resistance-step measurements"
                ),
                "fields": missing_or_invalid,
            },
        )

    if voltage_before_v <= 0.0 or voltage_after_v <= 0.0:
        return response(
            400,
            {
                "message": "Battery voltage must be greater than zero",
            },
        )

    delta_voltage_v = (
        voltage_after_v - voltage_before_v
    )
    delta_current_a = (
        current_after_a - current_before_a
    )

    if abs(delta_current_a) < MIN_CURRENT_STEP_A:
        return response(
            400,
            {
                "message": (
                    "Current step is below the configured "
                    "validation threshold"
                ),
                "delta_current_a": round(delta_current_a, 4),
                "minimum_current_step_a": MIN_CURRENT_STEP_A,
            },
        )

    apparent_resistance_ohm = abs(
        delta_voltage_v / delta_current_a
    )

    apparent_resistance_mohm = (
        apparent_resistance_ohm * 1000.0
    )

    result = {
        "station_id": station_id,
        "timestamp": timestamp,
        "event_type": "resistance_step",
        "voltage_before_v": round(voltage_before_v, 4),
        "voltage_after_v": round(voltage_after_v, 4),
        "current_before_a": round(current_before_a, 4),
        "current_after_a": round(current_after_a, 4),
        "soc_percent": round(soc_percent, 3),
        "delta_voltage_v": round(delta_voltage_v, 6),
        "delta_current_a": round(delta_current_a, 4),
        "apparent_resistance_ohm": round(
            apparent_resistance_ohm,
            8,
        ),
        "apparent_resistance_mohm": round(
            apparent_resistance_mohm,
            4,
        ),
        "minimum_current_step_a": MIN_CURRENT_STEP_A,
    }
    history_item = {
        "station_id": station_id,
        "timestamp": timestamp,
        "event_type": "resistance_step",
        "voltage_before_v": Decimal(str(result["voltage_before_v"])),
        "voltage_after_v": Decimal(str(result["voltage_after_v"])),
        "current_before_a": Decimal(str(result["current_before_a"])),
        "current_after_a": Decimal(str(result["current_after_a"])),
        "soc_percent": Decimal(str(result["soc_percent"])),
        "delta_current_a": Decimal(str(result["delta_current_a"])),
        "apparent_resistance_mohm": Decimal(
            str(result["apparent_resistance_mohm"])
        ),
    }

    battery_health_table.put_item(
        Item=history_item
    )
    
    print(
        json.dumps(
            {
                "event": "battery_resistance_step_processed",
                **result,
            }
        )
    )

    return response(
        200,
        {
            "message": (
                "Battery resistance-step event processed successfully"
            ),
            **result,
        },
    )


def normalize_number(value: Any) -> Optional[float]:
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


def normalize_soc(value: Any) -> Optional[float]:
    """
    Validates battery SOC.

    Valid range:
    0.0 <= soc_percent <= 100.0
    """

    numeric_value = normalize_number(value)

    if numeric_value is None:
        return None

    if numeric_value < 0.0 or numeric_value > 100.0:
        return None

    return numeric_value


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
