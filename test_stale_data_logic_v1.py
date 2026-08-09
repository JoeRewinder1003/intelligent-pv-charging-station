import importlib.util
import os
from datetime import datetime
from pathlib import Path

# Prevent boto3 from attempting EC2 metadata lookup during local tests.
os.environ.setdefault("AWS_EC2_METADATA_DISABLED", "true")
os.environ.setdefault("AWS_DEFAULT_REGION", "us-east-2")

ROOT = Path.cwd()

TELEMETRY_PATH = (
    ROOT
    / "aws"
    / "lambdas"
    / "telemetry_processor"
    / "lambda_function.py"
)

FIS_PATH = (
    ROOT
    / "aws"
    / "lambdas"
    / "fis_processor"
    / "lambda_function.py"
)


def load_module(name: str, path: Path):
    if not path.exists():
        raise FileNotFoundError(
            f"File not found: {path}"
        )

    spec = importlib.util.spec_from_file_location(
        name,
        path,
    )

    module = importlib.util.module_from_spec(spec)

    assert spec.loader is not None
    spec.loader.exec_module(module)

    return module


def epoch_ms(timestamp: str) -> int:
    dt = datetime.fromisoformat(
        timestamp.replace(
            "Z",
            "+00:00",
        )
    )

    return int(
        dt.timestamp() * 1000
    )


telemetry = load_module(
    "telemetry_processor_local",
    TELEMETRY_PATH,
)

fis = load_module(
    "fis_processor_local",
    FIS_PATH,
)


BASE_PAYLOAD = {
    "station_id": "station_001",
    "timestamp": "2026-08-07T04:25:23Z",
    "received_at_epoch_ms": epoch_ms(
        "2026-08-07T04:26:23.397Z"
    ),
    "battery": {
        "voltage_v": 12.58,
        "current_a": 17.76,
        "power_w": 223.42,
        "soc_percent": 80.0,
    },
    "pv": {
        "voltage_v": 16.677,
        "current_a": 15.219,
        "power_w": 253.8,
        "local_irradiance_wm2": 600.0,
    },
    "decision": {
        "weather_index": 0.60,
        "demand_index": 0.58,
        "operating_mode": "M0",
    },
    "fault_state": "normal",
    "simulation": {
        "scenario": "STALE_DATA",
        "stale_data_requested": True,
        "stale_offset_s": 60,
    },
}


# Fixed demand result used only by this stale-data unit test.
# Demand Profile behavior is validated separately.
TEST_DEMAND_RESULT = {
    "demand_index": 0.58,
    "source": "telemetry_fallback",
    "slot_id": None,
    "day_index": None,
    "slot_index": None,
    "local_time": None,
    "timezone": None,
    "adaptive": False,
    "fallback_used": True,
}


print("=== STALE MESSAGE TEST ===")

telemetry_validation = (
    telemetry.evaluate_temporal_validation(
        dict(BASE_PAYLOAD)
    )
)

print(
    "telemetry_processor temporal validation:",
    telemetry_validation,
)

assert telemetry_validation[
    "stale_detected"
] is True

assert (
    60.0
    <= telemetry_validation["data_age_s"]
    < 61.0
)


telemetry_effective_fault = (
    telemetry.derive_effective_fault_state(
        "normal",
        telemetry_validation,
    )
)

print(
    "telemetry_processor effective fault:",
    telemetry_effective_fault,
)

assert (
    telemetry_effective_fault
    == "data_or_sensor_fault"
)


fis_payload = telemetry.build_fis_payload(
    payload=dict(BASE_PAYLOAD),
    effective_fault_state=telemetry_effective_fault,
    demand_result=TEST_DEMAND_RESULT,
)

fis_payload[
    "station_reported_fault_state"
] = "normal"


# Verify that Demand Profile integration does not alter
# the stale-data test input.
assert (
    fis_payload["decision"]["demand_index"]
    == 0.58
)


fis_validation = (
    fis.evaluate_temporal_validation(
        fis_payload
    )
)

print(
    "fis_processor temporal validation:",
    fis_validation,
)

assert fis_validation[
    "stale_detected"
] is True


fis_evaluation_payload = (
    fis.apply_temporal_fault_override(
        fis_payload,
        fis_validation,
    )
)

print(
    "fis_processor effective fault:",
    fis_evaluation_payload[
        "fault_state"
    ],
)

assert (
    fis_evaluation_payload["fault_state"]
    == "data_or_sensor_fault"
)


deterministic = (
    fis.evaluate_deterministic_layer(
        fis_mode="M4",
        soc_percent=80.0,
        fault_state=fis_evaluation_payload[
            "fault_state"
        ],
        local_irradiance_wm2=600.0,
        weather_index=0.60,
    )
)

print(
    "deterministic requested mode:",
    deterministic["requested_mode"],
)

print(
    "tracking blocked:",
    deterministic["tracking_blocked"],
)

print(
    "outputs blocked:",
    deterministic["outputs_blocked"],
)

print(
    "outputs active:",
    deterministic["outputs_active"],
)

print(
    "blocked reasons:",
    deterministic["blocked_reasons"],
)


assert (
    deterministic["requested_mode"]
    == "M1"
)

assert (
    deterministic["tracking_blocked"]
    is True
)

assert (
    deterministic["outputs_blocked"]
    is True
)

assert (
    deterministic["outputs_active"]
    == 0
)

assert (
    "data_or_sensor_fault"
    in deterministic["blocked_reasons"]
)


print(
    "\n=== FRESH MESSAGE CONTROL TEST ==="
)

fresh_payload = dict(
    BASE_PAYLOAD
)

fresh_payload[
    "timestamp"
] = "2026-08-07T04:26:13.397Z"

fresh_validation = (
    telemetry.evaluate_temporal_validation(
        fresh_payload
    )
)

print(
    "telemetry_processor temporal validation:",
    fresh_validation,
)

assert fresh_validation[
    "stale_detected"
] is False

assert (
    9.9
    <= fresh_validation["data_age_s"]
    <= 10.1
)


fresh_effective_fault = (
    telemetry.derive_effective_fault_state(
        "normal",
        fresh_validation,
    )
)

print(
    "telemetry_processor effective fault:",
    fresh_effective_fault,
)

assert (
    fresh_effective_fault
    == "normal"
)


print(
    "\nALL LOCAL STALE-DATA LOGIC TESTS PASSED"
)