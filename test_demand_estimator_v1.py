import importlib.util
import json
from decimal import Decimal
from pathlib import Path
from typing import Any, Dict


MODULE_PATH = (
    Path(__file__).parent
    / "aws"
    / "lambdas"
    / "demand_estimator"
    / "lambda_function.py"
)


def load_demand_estimator():
    spec = importlib.util.spec_from_file_location(
        "demand_estimator_lambda",
        MODULE_PATH,
    )

    if spec is None or spec.loader is None:
        raise RuntimeError("Could not load demand_estimator module.")

    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    return module


demand_estimator = load_demand_estimator()


class FakeDemandProfileTable:
    """
    In-memory replacement for the real DynamoDB DemandProfile table.
    No AWS requests are performed by get_item().
    """

    def __init__(self, items: Dict[Any, Dict[str, Any]]):
        self.items = items
        self.requested_keys = []

    def get_item(self, Key: Dict[str, str]) -> Dict[str, Any]:
        self.requested_keys.append(dict(Key))

        item_key = (
            Key["station_id"],
            Key["slot_id"],
        )

        item = self.items.get(item_key)

        if item is None:
            return {}

        return {
            "Item": item,
        }


def invoke(event, items):
    fake_table = FakeDemandProfileTable(items)

    demand_estimator.demand_profile_table = fake_table

    result = demand_estimator.lambda_handler(
        event,
        None,
    )

    body = json.loads(result["body"])

    return result, body, fake_table


def assert_equal(actual, expected, label):
    if actual != expected:
        raise AssertionError(
            f"{label}: expected {expected!r}, got {actual!r}"
        )


def test_default_profile_lookup():
    """
    Monday 2026-08-10 at 15:15 UTC corresponds to approximately
    09:15 local station time in America/Mexico_City.

    Expected slot:
    day_0_slot_18
    """

    event = {
        "station_id": "station_001",
        "timestamp": "2026-08-10T01:00:00Z",
        "evaluation_timestamp": "2026-08-10T15:15:00Z",
        "decision": {
            "demand_index": 0.65,
        },
    }

    items = {
        (
            "station_001",
            "day_0_slot_18",
        ): {
            "station_id": "station_001",
            "slot_id": "day_0_slot_18",
            "default_demand_index": Decimal("0.60"),
            "adaptive_demand_index": Decimal("0.95"),
        },
    }

    result, body, fake_table = invoke(
        event,
        items,
    )

    assert_equal(
        result["statusCode"],
        200,
        "statusCode",
    )

    assert_equal(
        body["slot_id"],
        "day_0_slot_18",
        "slot_id",
    )

    assert_equal(
        body["demand_index"],
        0.60,
        "demand_index",
    )

    assert_equal(
        body["source"],
        "default_demand_profile",
        "source",
    )

    assert_equal(
        body["adaptive"],
        False,
        "adaptive",
    )

    assert_equal(
        fake_table.requested_keys[0],
        {
            "station_id": "station_001",
            "slot_id": "day_0_slot_18",
        },
        "DynamoDB key",
    )

    print(
        "PASS: default DemandProfile value was resolved correctly."
    )


def test_telemetry_fallback():
    """
    When the DemandProfile item does not exist, V1 must preserve the
    externally supplied Demand Index as a fallback.
    """

    event = {
        "station_id": "station_001",
        "timestamp": "2026-08-10T01:00:00Z",
        "_aws": {
            "received_at": "2026-08-10T15:45:00Z",
        },
        "decision": {
            "demand_index": 0.65,
        },
    }

    result, body, _ = invoke(
        event,
        {},
    )

    assert_equal(
        result["statusCode"],
        200,
        "statusCode",
    )

    assert_equal(
        body["slot_id"],
        "day_0_slot_19",
        "slot_id",
    )

    assert_equal(
        body["demand_index"],
        0.65,
        "demand_index",
    )

    assert_equal(
        body["source"],
        "telemetry_fallback",
        "source",
    )

    print(
        "PASS: telemetry Demand Index fallback works correctly."
    )


def test_adaptive_value_is_ignored():
    """
    V1 must use default_demand_index even when an adaptive value exists.
    """

    event = {
        "station_id": "station_001",
        "evaluation_timestamp": "2026-08-10T15:15:00Z",
        "decision": {
            "demand_index": 0.40,
        },
    }

    items = {
        (
            "station_001",
            "day_0_slot_18",
        ): {
            "default_demand_index": Decimal("0.55"),
            "adaptive_demand_index": Decimal("0.99"),
            "sample_count": 50,
        },
    }

    result, body, _ = invoke(
        event,
        items,
    )

    assert_equal(
        result["statusCode"],
        200,
        "statusCode",
    )

    assert_equal(
        body["demand_index"],
        0.55,
        "demand_index",
    )

    assert_equal(
        body["adaptive"],
        False,
        "adaptive",
    )

    print(
        "PASS: adaptive_demand_index is intentionally ignored in V1."
    )


def test_missing_profile_and_fallback():
    """
    If neither the profile nor a valid fallback exists, the function
    must fail explicitly instead of inventing a Demand Index.
    """

    event = {
        "station_id": "station_001",
        "evaluation_timestamp": "2026-08-10T15:15:00Z",
    }

    result, body, _ = invoke(
        event,
        {},
    )

    assert_equal(
        result["statusCode"],
        404,
        "statusCode",
    )

    assert_equal(
        body["slot_id"],
        "day_0_slot_18",
        "slot_id",
    )

    print(
        "PASS: missing Demand Index is reported correctly."
    )


def test_missing_station_id():
    event = {
        "evaluation_timestamp": "2026-08-10T15:15:00Z",
        "demand_index": 0.50,
    }

    result, _, _ = invoke(
        event,
        {},
    )

    assert_equal(
        result["statusCode"],
        400,
        "statusCode",
    )

    print(
        "PASS: missing station_id is rejected correctly."
    )


def main():
    print()
    print("Demand Estimator V1 local validation")
    print("====================================")
    print()

    test_default_profile_lookup()
    test_telemetry_fallback()
    test_adaptive_value_is_ignored()
    test_missing_profile_and_fallback()
    test_missing_station_id()

    print()
    print("ALL LOCAL DEMAND ESTIMATOR V1 TESTS PASSED")
    print()


if __name__ == "__main__":
    main()