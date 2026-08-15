import importlib.util
import json
from decimal import Decimal
from pathlib import Path
from typing import Any, Dict


MODULE_PATH = (
    Path(__file__).parent
    / "aws"
    / "lambdas"
    / "actuator_life_estimator"
    / "lambda_function.py"
)


def load_actuator_lifetime_estimator():
    spec = importlib.util.spec_from_file_location(
        "actuator_life_estimator_lambda",
        MODULE_PATH,
    )

    if spec is None or spec.loader is None:
        raise RuntimeError(
            "Could not load actuator_lifetime_estimator module."
        )

    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    return module


actuator_lifetime_estimator = (
    load_actuator_lifetime_estimator()
)


class FakeActuatorUsageTable:
    """
    In-memory replacement for the real DynamoDB
    ActuatorUsageHistory table.

    No AWS requests are performed by put_item().
    """

    def __init__(self):
        self.items = []

    def put_item(
        self,
        Item: Dict[str, Any],
    ) -> Dict[str, Any]:
        self.items.append(Item)

        return {}


def invoke(event):
    fake_table = FakeActuatorUsageTable()

    actuator_lifetime_estimator.actuator_usage_table = (
        fake_table
    )

    result = actuator_lifetime_estimator.lambda_handler(
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


def valid_event():
    """
    Returns the actuator diagnostic event obtained from the
    validated ESP32 CLEAR_DAY test.
    """

    actuator = {
        "window_sequence": 1,
        "operating_time_s": 40.68,
        "total_travel_mm": 206.68,
        "movement_starts": 1,
        "equivalent_full_stroke_cycles": 0.3445,
        "duty_cycle_percent": 67.70,
        "duty_cycle_window_available": True,
        "duty_cycle_exceeded": True,
    }

    return {
        "station_id": "station_001",
        "timestamp": "2026-08-15T18:25:55Z",
        "master": dict(actuator),
        "slave": dict(actuator),
    }


def test_valid_actuator_usage():
    """
    A validated ESP32 actuator diagnostic window must be
    accepted and stored.
    """

    result, body, fake_table = invoke(
        valid_event()
    )

    assert_equal(
        result["statusCode"],
        200,
        "statusCode",
    )

    assert_equal(
        body["window_sequence"],
        1,
        "window_sequence",
    )

    assert_equal(
        body["duty_cycle_warning"],
        True,
        "duty_cycle_warning",
    )

    assert_equal(
        body["operating_time_difference_s"],
        0.0,
        "operating_time_difference_s",
    )

    assert_equal(
        body["travel_difference_mm"],
        0.0,
        "travel_difference_mm",
    )

    assert_equal(
        len(fake_table.items),
        1,
        "stored item count",
    )

    stored_item = fake_table.items[0]

    assert_equal(
        stored_item["station_id"],
        "station_001",
        "stored station_id",
    )

    if not isinstance(
        stored_item["master"]["total_travel_mm"],
        Decimal,
    ):
        raise AssertionError(
            "DynamoDB numeric values were not converted "
            "to Decimal."
        )

    print(
        "PASS: valid actuator usage event was processed "
        "and stored correctly."
    )


def test_invalid_equivalent_cycles():
    """
    AWS must reject an equivalent-cycle value that is
    inconsistent with total travel.
    """

    event = valid_event()

    event["master"][
        "equivalent_full_stroke_cycles"
    ] = 0.5000

    result, _, fake_table = invoke(
        event
    )

    assert_equal(
        result["statusCode"],
        400,
        "statusCode",
    )

    assert_equal(
        len(fake_table.items),
        0,
        "stored item count",
    )

    print(
        "PASS: inconsistent equivalent-cycle value was "
        "rejected correctly."
    )


def test_invalid_duty_cycle_warning():
    """
    AWS must reject a duty-cycle warning that does not
    agree with the configured 25 percent limit.
    """

    event = valid_event()

    event["master"]["duty_cycle_percent"] = 10.0
    event["master"]["duty_cycle_exceeded"] = True

    result, _, fake_table = invoke(
        event
    )

    assert_equal(
        result["statusCode"],
        400,
        "statusCode",
    )

    assert_equal(
        len(fake_table.items),
        0,
        "stored item count",
    )

    print(
        "PASS: inconsistent duty-cycle warning was "
        "rejected correctly."
    )


def test_mismatched_window_sequence():
    """
    Master and slave diagnostic windows must correspond
    to the same observation window.
    """

    event = valid_event()

    event["slave"]["window_sequence"] = 2

    result, _, fake_table = invoke(
        event
    )

    assert_equal(
        result["statusCode"],
        400,
        "statusCode",
    )

    assert_equal(
        len(fake_table.items),
        0,
        "stored item count",
    )

    print(
        "PASS: mismatched actuator window sequences were "
        "rejected correctly."
    )


def test_manual_window_zero():
    """
    A manual diagnostic before the first completed duty-cycle
    window is valid when window availability is false.
    """

    event = valid_event()

    for actuator_name in ("master", "slave"):
        event[actuator_name]["window_sequence"] = 0
        event[actuator_name]["operating_time_s"] = 21.08
        event[actuator_name]["total_travel_mm"] = 107.10
        event[actuator_name][
            "equivalent_full_stroke_cycles"
        ] = 0.1785
        event[actuator_name]["duty_cycle_percent"] = 0.0
        event[actuator_name][
            "duty_cycle_window_available"
        ] = False
        event[actuator_name][
            "duty_cycle_exceeded"
        ] = False

    result, body, fake_table = invoke(
        event
    )

    assert_equal(
        result["statusCode"],
        200,
        "statusCode",
    )

    assert_equal(
        body["window_sequence"],
        0,
        "window_sequence",
    )

    assert_equal(
        body["duty_cycle_warning"],
        False,
        "duty_cycle_warning",
    )

    assert_equal(
        len(fake_table.items),
        1,
        "stored item count",
    )

    print(
        "PASS: manual pre-window actuator diagnostic was "
        "accepted correctly."
    )


def test_missing_station_id():
    event = valid_event()
    event.pop("station_id")

    result, _, fake_table = invoke(
        event
    )

    assert_equal(
        result["statusCode"],
        400,
        "statusCode",
    )

    assert_equal(
        len(fake_table.items),
        0,
        "stored item count",
    )

    print(
        "PASS: missing station_id was rejected correctly."
    )


def main():
    print()
    print("Actuator Lifetime Estimator V1 local validation")
    print("===============================================")
    print()

    test_valid_actuator_usage()
    test_invalid_equivalent_cycles()
    test_invalid_duty_cycle_warning()
    test_mismatched_window_sequence()
    test_manual_window_zero()
    test_missing_station_id()

    print()
    print(
        "ALL LOCAL ACTUATOR LIFETIME ESTIMATOR V1 "
        "TESTS PASSED"
    )
    print()


if __name__ == "__main__":
    main()