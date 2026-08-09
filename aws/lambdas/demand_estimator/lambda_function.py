import json
import os
from datetime import datetime, timezone
from decimal import Decimal
from typing import Any, Dict, Optional
from zoneinfo import ZoneInfo

import boto3


dynamodb = boto3.resource("dynamodb")

DEMAND_PROFILE_TABLE_NAME = os.environ.get(
    "DEMAND_PROFILE_TABLE_NAME",
    "DemandProfile",
)

STATION_TIMEZONE = os.environ.get(
    "STATION_TIMEZONE",
    "America/Mexico_City",
)

demand_profile_table = dynamodb.Table(DEMAND_PROFILE_TABLE_NAME)


def lambda_handler(event: Dict[str, Any], context: Any) -> Dict[str, Any]:
    """
    Resolves the Demand Index for the current station time slot.

    V1 behavior:
    - Resolves the current evaluation time independently from the telemetry
      measurement timestamp.
    - Converts the evaluation time to the station local timezone.
    - Resolves the weekly half-hour DemandProfile slot.
    - Reads default_demand_index from DynamoDB.
    - Falls back to the externally supplied demand_index when the profile
      item or value is unavailable.

    Adaptive demand updates are intentionally disabled in V1.
    """

    try:
        station_id = event.get("station_id")

        if not isinstance(station_id, str) or not station_id.strip():
            return response(
                400,
                {
                    "message": "station_id is required",
                },
            )

        evaluation_time = resolve_evaluation_time(event)

        try:
            station_timezone = ZoneInfo(STATION_TIMEZONE)
        except Exception as exc:
            return response(
                500,
                {
                    "message": "Invalid station timezone configuration",
                    "timezone": STATION_TIMEZONE,
                    "error": str(exc),
                },
            )

        local_time = evaluation_time.astimezone(station_timezone)

        day_index = local_time.weekday()
        slot_index = local_time.hour * 2 + local_time.minute // 30
        slot_id = f"day_{day_index}_slot_{slot_index}"

        fallback_demand_index = extract_fallback_demand_index(event)

        profile_item = get_demand_profile_item(
            station_id=station_id,
            slot_id=slot_id,
        )

        profile_demand_index = extract_profile_demand_index(
            profile_item
        )

        if profile_demand_index is not None:
            demand_index = profile_demand_index
            source = "default_demand_profile"

        elif fallback_demand_index is not None:
            demand_index = fallback_demand_index
            source = "telemetry_fallback"

        else:
            return response(
                404,
                {
                    "message": (
                        "Demand Index could not be resolved because the "
                        "DemandProfile slot is unavailable and no valid "
                        "fallback demand_index was provided."
                    ),
                    "station_id": station_id,
                    "slot_id": slot_id,
                    "timezone": STATION_TIMEZONE,
                },
            )

        result = {
            "station_id": station_id,
            "demand_index": round(demand_index, 4),
            "source": source,
            "slot_id": slot_id,
            "day_index": day_index,
            "slot_index": slot_index,
            "local_time": local_time.isoformat(),
            "timezone": STATION_TIMEZONE,
            "adaptive": False,
        }

        print(
            json.dumps(
                {
                    "event": "demand_index_resolved",
                    **result,
                }
            )
        )

        return response(
            200,
            {
                "message": "Demand Index resolved successfully",
                **result,
            },
        )

    except Exception as exc:
        print(
            json.dumps(
                {
                    "event": "demand_estimator_error",
                    "error": str(exc),
                }
            )
        )

        return response(
            500,
            {
                "message": "Demand estimator failed",
                "error": str(exc),
            },
        )


def resolve_evaluation_time(event: Dict[str, Any]) -> datetime:
    """
    Resolves the time used to select the DemandProfile slot.

    Priority:
    1. Explicit evaluation_timestamp for deterministic tests.
    2. AWS IoT reception timestamp stored in _aws.received_at.
    3. Current UTC Lambda execution time.

    The telemetry measurement timestamp is intentionally not used because
    stale telemetry may contain an old measurement time.
    """

    explicit_time = parse_timestamp(
        event.get("evaluation_timestamp")
    )

    if explicit_time is not None:
        return explicit_time

    aws_metadata = event.get("_aws")

    if isinstance(aws_metadata, dict):
        received_at = parse_timestamp(
            aws_metadata.get("received_at")
        )

        if received_at is not None:
            return received_at

    return datetime.now(timezone.utc)


def parse_timestamp(value: Any) -> Optional[datetime]:
    """
    Parses an ISO 8601 timestamp and normalizes it to UTC.

    Naive timestamps without timezone information are rejected.
    """

    if not isinstance(value, str):
        return None

    try:
        parsed = datetime.fromisoformat(
            value.replace("Z", "+00:00")
        )
    except ValueError:
        return None

    if parsed.tzinfo is None:
        return None

    return parsed.astimezone(timezone.utc)


def extract_fallback_demand_index(
    event: Dict[str, Any],
) -> Optional[float]:
    """
    Retrieves a fallback Demand Index from the event.

    Accepted locations:
    - event["demand_index"]
    - event["decision"]["demand_index"]
    """

    value = event.get("demand_index")

    if value is None:
        decision = event.get("decision")

        if isinstance(decision, dict):
            value = decision.get("demand_index")

    return normalize_demand_index(value)


def get_demand_profile_item(
    station_id: str,
    slot_id: str,
) -> Optional[Dict[str, Any]]:
    """
    Reads the DemandProfile item corresponding to the station and slot.
    """

    result = demand_profile_table.get_item(
        Key={
            "station_id": station_id,
            "slot_id": slot_id,
        }
    )

    item = result.get("Item")

    if not isinstance(item, dict):
        return None

    return item


def extract_profile_demand_index(
    item: Optional[Dict[str, Any]],
) -> Optional[float]:
    """
    Retrieves the fixed default Demand Index from a DemandProfile item.

    adaptive_demand_index is intentionally ignored in V1.
    """

    if not item:
        return None

    return normalize_demand_index(
        item.get("default_demand_index")
    )


def normalize_demand_index(value: Any) -> Optional[float]:
    """
    Validates and converts a Demand Index to float.

    Valid range:
    0.0 <= demand_index <= 1.0
    """

    if isinstance(value, bool):
        return None

    if isinstance(value, Decimal):
        value = float(value)

    if not isinstance(value, (int, float)):
        return None

    numeric_value = float(value)

    if numeric_value < 0.0 or numeric_value > 1.0:
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