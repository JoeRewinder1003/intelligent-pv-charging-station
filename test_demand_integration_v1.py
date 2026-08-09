import importlib.util
import json
from datetime import datetime, timezone
from decimal import Decimal
from pathlib import Path
from unittest.mock import patch


MODULE_PATH = (
    Path(__file__).parent
    / "aws"
    / "lambdas"
    / "telemetry_processor"
    / "lambda_function.py"
)

TEST_EVENT_PATH = (
    Path(__file__).parent
    / "aws"
    / "lambdas"
    / "telemetry_processor"
    / "test_event.json"
)


def load_telemetry_processor():
    spec = importlib.util.spec_from_file_location(
        "telemetry_processor_lambda",
        MODULE_PATH,
    )

    if spec is None or spec.loader is None:
        raise RuntimeError(
            "Could not load telemetry_processor module."
        )

    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    return module


def load_test_event():
    with TEST_EVENT_PATH.open(
        "r",
        encoding="utf-8",
    ) as file:
        return json.load(file)


def parse_body(result):
    return json.loads(
        result["body"]
    )


def build_received_at_epoch_ms():
    evaluation_time = datetime(
        2026,
        8,
        10,
        15,
        15,
        0,
        tzinfo=timezone.utc,
    )

    return int(
        evaluation_time.timestamp()
        * 1000
    )


telemetry = load_telemetry_processor()


def test_cloud_demand_replaces_only_fis_copy():
    """
    The station sends demand_index = 0.65.

    demand_estimator resolves 0.60 from DemandProfile.

    Expected behavior:
    - TelemetryHistory keeps the original 0.65.
    - demand_estimator receives the original 0.65 as fallback.
    - fis_processor receives the cloud value 0.60.
    """

    payload = load_test_event()

    payload["decision"]["demand_index"] = 0.65
    payload[
        "received_at_epoch_ms"
    ] = build_received_at_epoch_ms()

    demand_response = {
        "demand_index": 0.60,
        "source": "default_demand_profile",
        "slot_id": "day_0_slot_18",
        "day_index": 0,
        "slot_index": 18,
        "local_time":
            "2026-08-10T09:15:00-06:00",
        "timezone":
            "America/Mexico_City",
        "adaptive": False,
    }

    with (
        patch.object(
            telemetry.telemetry_table,
            "put_item",
        ) as put_item,
        patch.object(
            telemetry.status_table,
            "update_item",
        ) as update_item,
        patch.object(
            telemetry,
            "invoke_demand_estimator",
            return_value=demand_response,
        ) as invoke_demand,
        patch.object(
            telemetry,
            "invoke_fis_processor",
            return_value={
                "status_code": 200,
                "body": {
                    "message":
                        "FIS processed in integration test"
                },
            },
        ) as invoke_fis,
    ):
        result = telemetry.lambda_handler(
            payload,
            None,
        )

    body = parse_body(
        result
    )

    if result["statusCode"] != 200:
        raise AssertionError(
            f"Expected statusCode 200, "
            f"got {result['statusCode']}: {body}"
        )

    invoke_demand.assert_called_once()
    invoke_fis.assert_called_once()
    put_item.assert_called_once()
    update_item.assert_called_once()

    demand_event = (
        invoke_demand.call_args.args[0]
    )

    if (
        demand_event["decision"]["demand_index"]
        != 0.65
    ):
        raise AssertionError(
            "demand_estimator did not receive "
            "the station Demand Index as fallback."
        )

    if (
        demand_event["evaluation_timestamp"]
        != "2026-08-10T15:15:00Z"
    ):
        raise AssertionError(
            "Demand evaluation time was not derived "
            "from received_at_epoch_ms."
        )

    fis_payload = (
        invoke_fis.call_args.args[0]
    )

    if (
        fis_payload["decision"]["demand_index"]
        != 0.60
    ):
        raise AssertionError(
            "fis_processor did not receive "
            "the cloud-resolved Demand Index."
        )

    if (
        fis_payload["cloud_demand"]["source"]
        != "default_demand_profile"
    ):
        raise AssertionError(
            "FIS payload does not contain "
            "Demand Profile traceability."
        )

    stored_item = (
        put_item.call_args.kwargs["Item"]
    )

    stored_demand_index = (
        stored_item["decision"]["demand_index"]
    )

    if (
        stored_demand_index
        != Decimal("0.65")
    ):
        raise AssertionError(
            "TelemetryHistory did not preserve "
            "the original station Demand Index."
        )

    if (
        payload["decision"]["demand_index"]
        != 0.65
    ):
        raise AssertionError(
            "Original telemetry payload was modified."
        )

    if (
        body["demand_result"]["demand_index"]
        != 0.60
    ):
        raise AssertionError(
            "Lambda response does not report "
            "the cloud Demand Index."
        )

    print(
        "PASS: cloud Demand Index replaced "
        "only the FIS copy."
    )


def test_estimator_failure_uses_station_fallback():
    """
    demand_estimator fails.

    Expected behavior:
    - telemetry_processor continues normally.
    - Original station demand_index = 0.65 is sent to the FIS.
    - Fallback is explicitly reported.
    """

    payload = load_test_event()

    payload["decision"]["demand_index"] = 0.65
    payload[
        "received_at_epoch_ms"
    ] = build_received_at_epoch_ms()

    with (
        patch.object(
            telemetry.telemetry_table,
            "put_item",
        ),
        patch.object(
            telemetry.status_table,
            "update_item",
        ),
        patch.object(
            telemetry,
            "invoke_demand_estimator",
            side_effect=RuntimeError(
                "simulated demand estimator failure"
            ),
        ) as invoke_demand,
        patch.object(
            telemetry,
            "invoke_fis_processor",
            return_value={
                "status_code": 200,
                "body": {
                    "message":
                        "FIS processed using fallback"
                },
            },
        ) as invoke_fis,
    ):
        result = telemetry.lambda_handler(
            payload,
            None,
        )

    body = parse_body(
        result
    )

    if result["statusCode"] != 200:
        raise AssertionError(
            f"Expected statusCode 200, "
            f"got {result['statusCode']}: {body}"
        )

    invoke_demand.assert_called_once()
    invoke_fis.assert_called_once()

    fis_payload = (
        invoke_fis.call_args.args[0]
    )

    if (
        fis_payload["decision"]["demand_index"]
        != 0.65
    ):
        raise AssertionError(
            "Station Demand Index was not preserved "
            "after estimator failure."
        )

    cloud_demand = (
        fis_payload["cloud_demand"]
    )

    if (
        cloud_demand["source"]
        !=
        "telemetry_fallback_after_estimator_error"
    ):
        raise AssertionError(
            "Unexpected Demand Index fallback source."
        )

    if (
        cloud_demand["fallback_used"]
        is not True
    ):
        raise AssertionError(
            "Fallback was not explicitly reported."
        )

    if (
        body["demand_result"]["demand_index"]
        != 0.65
    ):
        raise AssertionError(
            "Lambda response does not report "
            "the fallback Demand Index."
        )

    print(
        "PASS: estimator failure preserved "
        "the station Demand Index."
    )


def main():
    print()
    print(
        "Demand Profile -> FIS local integration validation"
    )
    print(
        "==============================================="
    )
    print()

    test_cloud_demand_replaces_only_fis_copy()
    test_estimator_failure_uses_station_fallback()

    print()
    print(
        "ALL LOCAL DEMAND INTEGRATION V1 TESTS PASSED"
    )
    print()


if __name__ == "__main__":
    main()