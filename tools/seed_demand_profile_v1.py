import argparse
from datetime import datetime, timezone
from decimal import Decimal

import boto3


AWS_REGION = "us-east-2"
TABLE_NAME = "DemandProfile"
DEFAULT_STATION_ID = "station_001"

DAY_NAMES = [
    "Monday",
    "Tuesday",
    "Wednesday",
    "Thursday",
    "Friday",
    "Saturday",
    "Sunday",
]


def demand_index_for_slot(slot_index):
    """
    Returns the preliminary fixed Demand Index used by V1.

    This profile is synthetic and intended for initial cloud-system
    validation. It is not learned from real station usage.
    """

    start_minutes = slot_index * 30

    if start_minutes < 5 * 60 + 30:
        return Decimal("0.20")

    if start_minutes < 7 * 60:
        return Decimal("0.70")

    if start_minutes < 10 * 60:
        return Decimal("1.00")

    if start_minutes < 13 * 60:
        return Decimal("0.95")

    if start_minutes < 20 * 60:
        return Decimal("1.00")

    if start_minutes < 23 * 60:
        return Decimal("0.75")

    return Decimal("0.35")


def format_time(total_minutes):
    total_minutes %= 24 * 60

    hour = total_minutes // 60
    minute = total_minutes % 60

    return f"{hour:02d}:{minute:02d}"


def build_items(station_id):
    updated_at = (
        datetime.now(timezone.utc)
        .isoformat()
        .replace("+00:00", "Z")
    )

    items = []

    for day_index, day_name in enumerate(DAY_NAMES):
        for slot_index in range(48):
            start_minutes = slot_index * 30
            end_minutes = start_minutes + 30

            demand_index = demand_index_for_slot(
                slot_index
            )

            items.append(
                {
                    "station_id": station_id,
                    "slot_id": (
                        f"day_{day_index}_slot_{slot_index}"
                    ),
                    "day": day_name,
                    "slot_index": slot_index,
                    "start_time": format_time(
                        start_minutes
                    ),
                    "end_time": format_time(
                        end_minutes
                    ),
                    "default_demand_index":
                        demand_index,
                    "adaptive_demand_index":
                        demand_index,
                    "sample_count": 0,
                    "last_updated": updated_at,
                }
            )

    return items


def print_summary(items):
    print()
    print("Demand Profile V1 seed preview")
    print("==============================")
    print()

    print(f"Items: {len(items)}")
    print()

    sample_slots = [
        "day_0_slot_0",
        "day_0_slot_11",
        "day_0_slot_14",
        "day_0_slot_18",
        "day_0_slot_20",
        "day_0_slot_40",
        "day_0_slot_46",
    ]

    for slot_id in sample_slots:
        item = next(
            item
            for item in items
            if item["slot_id"] == slot_id
        )

        print(
            f"{item['slot_id']}: "
            f"{item['start_time']}-"
            f"{item['end_time']} -> "
            f"{item['default_demand_index']}"
        )

    print()


def write_items(items):
    dynamodb = boto3.resource(
        "dynamodb",
        region_name=AWS_REGION,
    )

    table = dynamodb.Table(
        TABLE_NAME
    )

    with table.batch_writer(
        overwrite_by_pkeys=[
            "station_id",
            "slot_id",
        ]
    ) as batch:
        for item in items:
            batch.put_item(
                Item=item
            )


def main():
    parser = argparse.ArgumentParser(
        description=(
            "Create the preliminary weekly "
            "DemandProfile V1."
        )
    )

    parser.add_argument(
        "--station-id",
        default=DEFAULT_STATION_ID,
    )

    parser.add_argument(
        "--write",
        action="store_true",
        help=(
            "Write the generated profile "
            "to DynamoDB."
        ),
    )

    args = parser.parse_args()

    items = build_items(
        args.station_id
    )

    print_summary(
        items
    )

    if not args.write:
        print(
            "DRY RUN ONLY. "
            "No DynamoDB items were written."
        )
        print(
            "Run again with --write "
            "to upload the profile."
        )
        return

    write_items(
        items
    )

    print(
        f"SUCCESS: {len(items)} DemandProfile "
        "items written to DynamoDB."
    )


if __name__ == "__main__":
    main()