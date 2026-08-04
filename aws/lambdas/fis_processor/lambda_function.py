import json
import math
import os
from datetime import datetime, timezone
from decimal import Decimal
from typing import Any, Dict, List

import boto3

dynamodb = boto3.resource("dynamodb")

FIS_DECISION_TABLE_NAME = os.environ.get(
    "FIS_DECISION_TABLE_NAME",
    "FISDecisionHistory",
)
STATUS_TABLE_NAME = os.environ.get(
    "STATUS_TABLE_NAME",
    "StationStatus",
)

fis_decision_table = dynamodb.Table(FIS_DECISION_TABLE_NAME)
status_table = dynamodb.Table(STATUS_TABLE_NAME)

FIS_IMPLEMENTATION_VERSION = "article_v9_dispatch_v1"

COMMAND_DISPATCHER_FUNCTION_NAME = os.environ.get(
    "COMMAND_DISPATCHER_FUNCTION_NAME",
    "command_dispatcher",
)

MODE_CONFIRMATION_SECONDS = int(
    os.environ.get("MODE_CONFIRMATION_SECONDS", "600")
)
MIN_MODE_DWELL_SECONDS = int(
    os.environ.get("MIN_MODE_DWELL_SECONDS", "900")
)

OPERATING_MODES = {
    0: "M0",
    1: "M1",
    2: "M2",
    3: "M3",
    4: "M4",
    5: "M5",
}


def lambda_handler(event: Dict[str, Any], context: Any) -> Dict[str, Any]:
    """
    Evaluates the cloud-side fuzzy decision system and conditionally invokes
    command_dispatcher.

    A command is dispatched only when the stabilized operating mode differs
    from the last mode successfully sent to the station. If dispatch fails,
    the state keeps the previous last-dispatched mode so the next evaluation
    retries the command instead of silently losing it.
    """

    try:
        payload = parse_event(event)
        errors = validate_input(payload)

        if errors:
            return response(
                400,
                {
                    "message": "Invalid FIS input payload",
                    "errors": errors,
                },
            )

        station_id = payload["station_id"]
        previous_state = load_fis_state(station_id)
        evaluation_time = current_utc_timestamp()

        fis_result = evaluate_fis(
            payload,
            previous_state=previous_state,
            evaluation_time=evaluation_time,
        )
        command_request = build_command_request(payload, fis_result)

        dispatch_result = {
            "attempted": False,
            "status": "not_required",
        }
        stabilization_to_save = dict(fis_result["mode_stabilization"])
        dispatch_error = None

        if fis_result["final_decision"]["command_required"]:
            try:
                dispatcher_response = invoke_command_dispatcher(
                    command_request
                )
                dispatch_result = {
                    "attempted": True,
                    "status": "sent",
                    "response": dispatcher_response,
                }
                stabilization_to_save = mark_command_dispatched(
                    stabilization=stabilization_to_save,
                    dispatch_timestamp=evaluation_time,
                )
            except Exception as exc:
                dispatch_error = exc
                dispatch_result = {
                    "attempted": True,
                    "status": "failed",
                    "error": str(exc),
                }

        save_fis_state(
            station_id=station_id,
            stabilization=stabilization_to_save,
        )
        save_fis_decision(
            payload=payload,
            fis_result=fis_result,
            command_request=command_request,
            dispatch_result=dispatch_result,
        )

        if dispatch_error is not None:
            return response(
                502,
                {
                    "message": "FIS decision evaluated but command dispatch failed",
                    "fis_result": fis_result,
                    "command_request": command_request,
                    "dispatch_result": dispatch_result,
                },
            )

        return response(
            200,
            {
                "message": "FIS decision evaluated successfully",
                "fis_result": fis_result,
                "command_request": command_request,
                "dispatch_result": dispatch_result,
            },
        )

    except Exception as exc:
        return response(
            500,
            {
                "message": "Internal error while evaluating FIS decision",
                "error": str(exc),
            },
        )


def parse_event(event: Dict[str, Any]) -> Dict[str, Any]:
    if "body" in event:
        body = event["body"]

        if isinstance(body, str):
            return json.loads(body)

        if isinstance(body, dict):
            return body

    return event


def validate_input(payload: Dict[str, Any]) -> List[str]:
    errors = []

    if not isinstance(payload.get("station_id"), str):
        errors.append("station_id must be a string")

    battery = payload.get("battery", {})
    pv = payload.get("pv", {})

    if not isinstance(battery, dict):
        errors.append("battery must be an object")
        return errors

    if not isinstance(pv, dict):
        errors.append("pv must be an object")
        return errors

    validate_numeric(battery, "soc_percent", errors, 0, 100)
    validate_numeric(battery, "power_w", errors, None, None)
    validate_numeric(pv, "local_irradiance_wm2", errors, 0, None)

    decision = payload.get("decision", {})
    if isinstance(decision, dict) and "demand_index" in decision:
        validate_numeric(decision, "demand_index", errors, 0, 1)

    fault_state = payload.get("fault_state", "normal")
    allowed_fault_states = {
        "normal",
        "non_critical_restriction",
        "data_or_sensor_fault",
        "critical_lockout",
    }
    if not isinstance(fault_state, str):
        errors.append("fault_state must be a string")
    elif fault_state not in allowed_fault_states:
        errors.append(
            "fault_state must be one of: "
            + ", ".join(sorted(allowed_fault_states))
        )

    return errors


def validate_numeric(
    parent: Dict[str, Any],
    field: str,
    errors: List[str],
    min_value: float | None,
    max_value: float | None,
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


def evaluate_fis(
    payload: Dict[str, Any],
    previous_state: Dict[str, Any] | None = None,
    evaluation_time: str | None = None,
) -> Dict[str, Any]:
    inputs = extract_fis_inputs(payload)

    weather_index = evaluate_weather_fis(
        shortwave_radiation_wm2=inputs["shortwave_radiation_wm2"],
        cloud_cover_percent=inputs["cloud_cover_percent"],
        precipitation_probability_percent=inputs["precipitation_probability_percent"],
    )

    main_result = evaluate_main_fis(
        soc_percent=inputs["soc_percent"],
        p_net_w=inputs["p_net_w"],
        local_irradiance_wm2=inputs["local_irradiance_wm2"],
        weather_index=weather_index,
        demand_index=inputs["demand_index"],
    )

    deterministic = evaluate_deterministic_layer(
        fis_mode=main_result["fis_mode"],
        soc_percent=inputs["soc_percent"],
        fault_state=payload.get("fault_state", "normal"),
        local_irradiance_wm2=inputs["local_irradiance_wm2"],
        weather_index=weather_index,
    )

    timestamp = evaluation_time or current_utc_timestamp()
    stabilization = stabilize_operating_mode(
        requested_mode=deterministic["requested_mode"],
        deterministic=deterministic,
        previous_state=previous_state,
        evaluation_time=timestamp,
    )

    applied_mode = stabilization["applied_mode"]
    outputs_active = mode_to_outputs(applied_mode)

    if deterministic["outputs_blocked"] or deterministic["fault_state_level"] >= 2:
        outputs_active = 0

    return {
        "timestamp": timestamp,
        "inputs": {
            **inputs,
            "weather_index": round(weather_index, 4),
        },
        "weather_fis_output": {
            "weather_index": round(weather_index, 4),
        },
        "main_fis_output": {
            "centroid": round(main_result["centroid"], 4),
            "fis_mode": main_result["fis_mode"],
        },
        "deterministic_validation": deterministic,
        "mode_stabilization": stabilization,
        "final_decision": {
            "requested_mode": deterministic["requested_mode"],
            "operating_mode": applied_mode,
            "outputs_active": outputs_active,
            "command_required": stabilization["command_required"],
        },
    }


def extract_fis_inputs(payload: Dict[str, Any]) -> Dict[str, float]:
    battery = payload.get("battery", {})
    pv = payload.get("pv", {})
    weather = payload.get("weather", {})
    decision = payload.get("decision", {})

    local_irradiance = float(pv.get("local_irradiance_wm2", 0.0))

    return {
        "soc_percent": float(battery.get("soc_percent", 0.0)),
        "p_net_w": float(battery.get("power_w", 0.0)),
        "local_irradiance_wm2": local_irradiance,
        "shortwave_radiation_wm2": float(
            weather.get("shortwave_radiation_wm2", local_irradiance)
        ),
        "cloud_cover_percent": float(weather.get("cloud_cover_percent", 30.0)),
        "precipitation_probability_percent": float(
            weather.get("precipitation_probability_percent", 0.0)
        ),
        "demand_index": float(decision.get("demand_index", payload.get("demand_index", 0.5))),
    }


def evaluate_weather_fis(
    shortwave_radiation_wm2: float,
    cloud_cover_percent: float,
    precipitation_probability_percent: float,
) -> float:
    """Evaluate the Weather FIS using the final ESP32 article v9 definition."""

    rad_low = trapmf(shortwave_radiation_wm2, -50.0, 0.0, 150.0, 350.0)
    rad_med = trimf(shortwave_radiation_wm2, 200.0, 500.0, 800.0)
    rad_high = trapmf(shortwave_radiation_wm2, 650.0, 850.0, 1000.0, 1100.0)

    cloud_low = trapmf(cloud_cover_percent, -5.0, 0.0, 20.0, 40.0)
    cloud_med = trimf(cloud_cover_percent, 25.0, 50.0, 75.0)
    cloud_high = trapmf(cloud_cover_percent, 60.0, 80.0, 100.0, 105.0)

    precip_low = trapmf(
        precipitation_probability_percent,
        -5.0,
        0.0,
        15.0,
        35.0,
    )
    precip_med = trimf(
        precipitation_probability_percent,
        20.0,
        50.0,
        80.0,
    )
    precip_high = trapmf(
        precipitation_probability_percent,
        65.0,
        85.0,
        100.0,
        105.0,
    )

    out_poor = 0.0
    out_moderate = 0.0
    out_favorable = 0.0

    # Favorable weather rules.
    out_favorable = max(out_favorable, min(rad_high, cloud_low, precip_low))
    out_favorable = max(out_favorable, min(rad_high, cloud_med, precip_low))
    out_favorable = max(out_favorable, min(rad_med, cloud_low, precip_low))

    # Moderate weather rules.
    out_moderate = max(out_moderate, min(rad_med, cloud_med, precip_low))
    out_moderate = max(out_moderate, min(rad_med, cloud_low, precip_med))
    out_moderate = max(out_moderate, min(rad_high, cloud_high, precip_low))
    out_moderate = max(out_moderate, min(rad_high, cloud_med, precip_med))
    out_moderate = max(out_moderate, min(rad_low, cloud_low, precip_low))

    # Poor weather rules.
    out_poor = max(out_poor, rad_low)
    out_poor = max(out_poor, cloud_high)
    out_poor = max(out_poor, precip_high)
    out_poor = max(out_poor, min(cloud_med, precip_med))

    numerator = 0.0
    denominator = 0.0

    for i in range(101):
        x = i / 100.0

        poor_mf = trapmf(x, -0.10, 0.00, 0.20, 0.45)
        moderate_mf = trimf(x, 0.25, 0.50, 0.75)
        favorable_mf = trapmf(x, 0.55, 0.80, 1.00, 1.10)

        mu_poor = min(out_poor, poor_mf)
        mu_moderate = min(out_moderate, moderate_mf)
        mu_favorable = min(out_favorable, favorable_mf)
        mu_aggregated = max(mu_poor, mu_moderate, mu_favorable)

        numerator += x * mu_aggregated
        denominator += mu_aggregated

    if denominator <= 0.0001:
        return 0.0

    return numerator / denominator

def evaluate_main_fis(
    soc_percent: float,
    p_net_w: float,
    local_irradiance_wm2: float,
    weather_index: float,
    demand_index: float,
) -> Dict[str, Any]:
    """Evaluate the Main FIS using the final ESP32 article v9 definition."""

    soc_critical = trapmf(soc_percent, -5.0, 0.0, 15.0, 25.0)
    soc_low = trimf(soc_percent, 15.0, 30.0, 45.0)
    soc_medium = trimf(soc_percent, 35.0, 55.0, 75.0)
    soc_high = trapmf(soc_percent, 65.0, 80.0, 100.0, 105.0)
    soc_full = trapmf(soc_percent, 85.0, 92.0, 100.0, 105.0)

    p_negative = trapmf(p_net_w, -400.0, -300.0, -60.0, 0.0)
    p_slight_negative = trapmf(p_net_w, -180.0, -120.0, -20.0, 20.0)
    p_strong_negative = trapmf(p_net_w, -450.0, -350.0, -220.0, -120.0)
    p_balanced = trimf(p_net_w, -80.0, 0.0, 80.0)
    p_positive = trapmf(p_net_w, 0.0, 60.0, 300.0, 400.0)

    irr_low = trapmf(local_irradiance_wm2, -50.0, 0.0, 150.0, 350.0)
    irr_med = trimf(local_irradiance_wm2, 250.0, 500.0, 750.0)
    irr_high = trapmf(local_irradiance_wm2, 650.0, 850.0, 1000.0, 1100.0)

    w_poor = trapmf(weather_index, -0.10, 0.00, 0.20, 0.45)
    w_moderate = trimf(weather_index, 0.25, 0.50, 0.75)
    w_favorable = trapmf(weather_index, 0.55, 0.80, 1.00, 1.10)

    d_low = trapmf(demand_index, -0.10, 0.00, 0.20, 0.45)
    d_medium = trimf(demand_index, 0.25, 0.50, 0.75)
    d_high = trapmf(demand_index, 0.55, 0.80, 1.00, 1.10)

    energy_ok = max(p_balanced, p_positive)
    solar_ok = max(irr_med, irr_high)
    weather_ok = max(w_moderate, w_favorable)
    demand_active = max(d_medium, d_high)

    battery_service_available = min(soc_high, demand_active)
    high_energy_service = min(
        soc_full,
        min(d_high, max(p_positive, min(p_balanced, solar_ok))),
    )

    output_activation = [0.0] * 6

    # Dominant safety and low-energy rules.
    output_activation[0] = max(output_activation[0], soc_critical)
    output_activation[0] = max(
        output_activation[0],
        min(soc_low, p_negative),
    )
    output_activation[1] = max(
        output_activation[1],
        min(soc_low, p_balanced),
    )
    output_activation[1] = max(
        output_activation[1],
        min(soc_low, irr_low),
    )
    output_activation[1] = max(
        output_activation[1],
        min(soc_high, min(irr_low, w_poor)),
    )

    # Basic operational availability.
    output_activation[2] = max(
        output_activation[2],
        min(soc_medium, p_balanced),
    )
    output_activation[2] = max(
        output_activation[2],
        min(soc_medium, min(irr_med, w_moderate)),
    )
    output_activation[2] = max(
        output_activation[2],
        min(soc_high, min(energy_ok, min(solar_ok, w_poor))),
    )

    # One-output rules.
    output_activation[3] = max(
        output_activation[3],
        min(soc_medium, min(p_positive, min(irr_med, w_moderate))),
    )
    output_activation[3] = max(
        output_activation[3],
        min(soc_high, min(p_balanced, min(solar_ok, weather_ok))),
    )
    output_activation[3] = max(
        output_activation[3],
        min(soc_high, min(p_positive, min(solar_ok, d_low))),
    )
    output_activation[3] = max(
        output_activation[3],
        min(soc_high, min(p_positive, min(solar_ok, demand_active))),
    )
    output_activation[3] = max(
        output_activation[3],
        min(soc_full, min(p_positive, demand_active)),
    )
    output_activation[3] = max(
        output_activation[3],
        min(battery_service_available, p_balanced),
    )
    output_activation[3] = max(
        output_activation[3],
        min(battery_service_available, p_slight_negative),
    )
    output_activation[3] = max(
        output_activation[3],
        min(battery_service_available, min(w_poor, irr_low)),
    )

    # Two-output rules.
    output_activation[4] = max(
        output_activation[4],
        min(soc_full, min(d_high, max(p_balanced, p_slight_negative))),
    )
    output_activation[4] = max(
        output_activation[4],
        min(
            soc_high,
            min(p_positive, min(irr_high, min(w_favorable, d_medium))),
        ),
    )
    output_activation[4] = max(
        output_activation[4],
        min(
            soc_high,
            min(p_positive, min(irr_high, min(w_moderate, d_high))),
        ),
    )
    output_activation[4] = max(
        output_activation[4],
        min(
            soc_medium,
            min(p_positive, min(irr_high, min(w_favorable, d_high))),
        ),
    )
    output_activation[4] = max(
        output_activation[4],
        min(
            soc_full,
            min(p_positive, min(solar_ok, min(weather_ok, d_high))),
        ),
    )
    output_activation[4] = max(
        output_activation[4],
        min(soc_full, min(p_positive, demand_active)),
    )

    # Three-output rules.
    output_activation[5] = max(
        output_activation[5],
        min(soc_full, min(p_positive, min(irr_high, d_high))),
    )
    output_activation[5] = max(
        output_activation[5],
        min(soc_full, min(p_positive, min(solar_ok, d_high))),
    )
    output_activation[5] = max(
        output_activation[5],
        min(soc_full, min(energy_ok, min(irr_high, d_high))),
    )
    output_activation[5] = max(
        output_activation[5],
        min(high_energy_service, weather_ok),
    )
    output_activation[5] = max(
        output_activation[5],
        min(
            soc_high,
            min(p_positive, min(irr_high, min(w_favorable, d_high))),
        ),
    )

    # Conservative complementary rules.
    output_activation[1] = max(
        output_activation[1],
        min(p_strong_negative, max(d_medium, d_high)),
    )
    output_activation[1] = max(
        output_activation[1],
        min(soc_low, min(p_negative, max(d_medium, d_high))),
    )
    output_activation[2] = max(
        output_activation[2],
        min(w_poor, min(soc_medium, energy_ok)),
    )

    numerator = 0.0
    denominator = 0.0

    for i in range(501):
        x = i / 100.0
        mu_aggregated = 0.0

        for mode in range(6):
            if mode == 0:
                mode_membership = trapmf(x, -0.5, 0.0, 0.35, 0.85)
            elif mode == 1:
                mode_membership = trimf(x, 0.3, 1.0, 1.7)
            elif mode == 2:
                mode_membership = trimf(x, 1.3, 2.0, 2.7)
            elif mode == 3:
                mode_membership = trimf(x, 2.3, 3.0, 3.7)
            elif mode == 4:
                mode_membership = trimf(x, 3.3, 4.0, 4.7)
            else:
                mode_membership = trapmf(x, 4.15, 4.65, 5.0, 5.5)

            mu_aggregated = max(
                mu_aggregated,
                min(output_activation[mode], mode_membership),
            )

        numerator += x * mu_aggregated
        denominator += mu_aggregated

    if denominator <= 0.0001:
        crisp_value = 0.0
        mode_number = 0
    else:
        crisp_value = numerator / denominator
        # Arduino roundf() rounds positive half-values away from zero.
        mode_number = int(math.floor(crisp_value + 0.5))
        mode_number = max(0, min(5, mode_number))

    return {
        "centroid": crisp_value,
        "fis_mode": OPERATING_MODES[mode_number],
    }


def mode_number(mode: str) -> int:
    if mode not in {"M0", "M1", "M2", "M3", "M4", "M5"}:
        raise ValueError(f"Unsupported operating mode: {mode}")
    return int(mode[1])


def evaluate_deterministic_layer(
    fis_mode: str,
    soc_percent: float,
    fault_state: str,
    local_irradiance_wm2: float,
    weather_index: float,
) -> Dict[str, Any]:
    """
    Apply the deterministic restrictions represented in the ESP32 article v9.

    The ESP32 simulation's persistence counter is represented in the cloud by
    the incoming ``data_or_sensor_fault`` state. Dwell-time stabilization is
    intentionally not implemented in this stage because a Lambda invocation is
    stateless; it will be added later using persistent station state.
    """
    requested_mode = fis_mode
    fault_state_level = 0
    functions_blocked = False
    tracking_blocked = False
    outputs_blocked = False
    blocked_reasons: List[str] = []

    critical_energy_fault = soc_percent <= 15.0
    low_battery_restriction = 15.0 < soc_percent <= 25.0

    if fault_state == "critical_lockout" or critical_energy_fault:
        requested_mode = "M0"
        fault_state_level = 3
        functions_blocked = True
        tracking_blocked = True
        outputs_blocked = True
        blocked_reasons.append(
            "critical_lockout"
            if fault_state == "critical_lockout"
            else "critical_soc"
        )
    else:
        if fault_state == "data_or_sensor_fault":
            if mode_number(requested_mode) > 1:
                requested_mode = "M1"
            fault_state_level = max(fault_state_level, 2)
            functions_blocked = True
            tracking_blocked = True
            outputs_blocked = True
            blocked_reasons.append("data_or_sensor_fault")

        if low_battery_restriction:
            if mode_number(requested_mode) > 1:
                requested_mode = "M1"
            fault_state_level = max(fault_state_level, 1)
            functions_blocked = True
            tracking_blocked = True
            outputs_blocked = True
            blocked_reasons.append("low_battery_restriction")

        if fault_state == "non_critical_restriction":
            fault_state_level = max(fault_state_level, 1)
            functions_blocked = True
            blocked_reasons.append("non_critical_restriction")

        poor_tracking_condition = (
            local_irradiance_wm2 < 120.0 or weather_index < 0.20
        )
        if poor_tracking_condition and mode_number(requested_mode) >= 2:
            if requested_mode == "M2":
                requested_mode = "M1"
            tracking_blocked = True
            functions_blocked = True
            fault_state_level = max(fault_state_level, 1)
            blocked_reasons.append("poor_tracking_condition")

        if mode_number(fis_mode) >= 3 and mode_number(requested_mode) < 3:
            outputs_blocked = True
            functions_blocked = True
            fault_state_level = max(fault_state_level, 1)
            if "charging_outputs_blocked" not in blocked_reasons:
                blocked_reasons.append("charging_outputs_blocked")

    outputs_active = mode_to_outputs(requested_mode)
    if outputs_blocked or fault_state_level >= 2:
        outputs_active = 0

    if mode_number(fis_mode) >= 3 and outputs_active == 0:
        outputs_blocked = True
        functions_blocked = True
        fault_state_level = max(fault_state_level, 1)
        if "charging_outputs_blocked" not in blocked_reasons:
            blocked_reasons.append("charging_outputs_blocked")

    tracking_allowed = (
        not tracking_blocked and mode_number(requested_mode) >= 2
    )
    charging_allowed = not outputs_blocked and outputs_active > 0

    return {
        "requested_mode": requested_mode,
        "fault_state_level": fault_state_level,
        "functions_blocked": functions_blocked,
        "tracking_blocked": tracking_blocked,
        "outputs_blocked": outputs_blocked,
        "tracking_allowed": tracking_allowed,
        "charging_allowed": charging_allowed,
        "outputs_active": outputs_active,
        "blocked_reason": blocked_reasons[0] if blocked_reasons else None,
        "blocked_reasons": blocked_reasons,
    }



def parse_utc_timestamp(value: str | None) -> datetime | None:
    if not isinstance(value, str) or not value:
        return None

    normalized = value
    if normalized.endswith("Z"):
        normalized = normalized[:-1] + "+00:00"

    try:
        parsed = datetime.fromisoformat(normalized)
    except ValueError:
        return None

    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)

    return parsed.astimezone(timezone.utc)


def default_fis_state(
    requested_mode: str,
    evaluation_time: str,
) -> Dict[str, Any]:
    return {
        "applied_mode": requested_mode,
        "candidate_mode": requested_mode,
        "candidate_since": None,
        "last_mode_change_at": evaluation_time,
        "last_evaluation_at": evaluation_time,
        "last_dispatched_mode": None,
        "last_dispatch_at": None,
        "initialized": True,
    }


def normalize_fis_state(
    previous_state: Dict[str, Any] | None,
    requested_mode: str,
    evaluation_time: str,
) -> Dict[str, Any]:
    if not previous_state:
        return default_fis_state(requested_mode, evaluation_time)

    applied_mode = previous_state.get("applied_mode")
    candidate_mode = previous_state.get("candidate_mode")

    if applied_mode not in OPERATING_MODES.values():
        applied_mode = requested_mode

    if candidate_mode not in OPERATING_MODES.values():
        candidate_mode = applied_mode

    last_mode_change_at = previous_state.get("last_mode_change_at")
    if parse_utc_timestamp(last_mode_change_at) is None:
        last_mode_change_at = evaluation_time

    candidate_since = previous_state.get("candidate_since")
    if parse_utc_timestamp(candidate_since) is None:
        candidate_since = None

    last_dispatched_mode = previous_state.get("last_dispatched_mode")
    if last_dispatched_mode not in OPERATING_MODES.values():
        last_dispatched_mode = None

    last_dispatch_at = previous_state.get("last_dispatch_at")
    if parse_utc_timestamp(last_dispatch_at) is None:
        last_dispatch_at = None

    return {
        "applied_mode": applied_mode,
        "candidate_mode": candidate_mode,
        "candidate_since": candidate_since,
        "last_mode_change_at": last_mode_change_at,
        "last_evaluation_at": previous_state.get(
            "last_evaluation_at",
            evaluation_time,
        ),
        "last_dispatched_mode": last_dispatched_mode,
        "last_dispatch_at": last_dispatch_at,
        "initialized": False,
    }


def seconds_between(
    newer_timestamp: str,
    older_timestamp: str | None,
) -> float:
    newer = parse_utc_timestamp(newer_timestamp)
    older = parse_utc_timestamp(older_timestamp)

    if newer is None or older is None:
        return 0.0

    return max(0.0, (newer - older).total_seconds())


def is_safety_reduction(
    requested_mode: str,
    applied_mode: str,
    deterministic: Dict[str, Any],
) -> bool:
    return (
        deterministic["fault_state_level"] >= 2
        or deterministic["outputs_blocked"]
        or (
            deterministic["fault_state_level"] > 0
            and mode_number(requested_mode) < mode_number(applied_mode)
        )
    )


def stabilize_operating_mode(
    requested_mode: str,
    deterministic: Dict[str, Any],
    previous_state: Dict[str, Any] | None,
    evaluation_time: str,
) -> Dict[str, Any]:
    """
    Cloud adaptation of the article v9 anti-chattering layer.

    Unlike the ESP32 simulation, which advances in 5-minute steps, the cloud
    receives telemetry more frequently. Therefore confirmation and dwell are
    time-based: 10 minutes of persistent candidacy and 15 minutes since the
    last applied-mode change. Safety reductions bypass both delays.
    """
    state = normalize_fis_state(
        previous_state=previous_state,
        requested_mode=requested_mode,
        evaluation_time=evaluation_time,
    )

    applied_mode = state["applied_mode"]
    candidate_mode = state["candidate_mode"]
    candidate_since = state["candidate_since"]
    last_mode_change_at = state["last_mode_change_at"]
    last_dispatched_mode = state["last_dispatched_mode"]
    last_dispatch_at = state["last_dispatch_at"]
    initialized = state["initialized"]

    safety_reduction = is_safety_reduction(
        requested_mode=requested_mode,
        applied_mode=applied_mode,
        deterministic=deterministic,
    )

    mode_changed = False
    decision_reason = "initialized_from_first_valid_decision"

    if safety_reduction:
        mode_changed = requested_mode != applied_mode
        applied_mode = requested_mode
        candidate_mode = requested_mode
        candidate_since = None
        last_mode_change_at = evaluation_time
        decision_reason = "safety_reduction_applied_immediately"

    elif requested_mode == applied_mode:
        candidate_mode = requested_mode
        candidate_since = None
        if not initialized:
            decision_reason = "requested_mode_matches_applied_mode"

    elif requested_mode != candidate_mode or candidate_since is None:
        candidate_mode = requested_mode
        candidate_since = evaluation_time
        decision_reason = "new_candidate_started"

    else:
        confirmation_elapsed = seconds_between(
            evaluation_time,
            candidate_since,
        )
        dwell_elapsed = seconds_between(
            evaluation_time,
            last_mode_change_at,
        )

        if (
            confirmation_elapsed >= MODE_CONFIRMATION_SECONDS
            and dwell_elapsed >= MIN_MODE_DWELL_SECONDS
        ):
            applied_mode = candidate_mode
            candidate_since = None
            last_mode_change_at = evaluation_time
            mode_changed = True
            decision_reason = "candidate_confirmed_and_dwell_satisfied"
        elif confirmation_elapsed < MODE_CONFIRMATION_SECONDS:
            decision_reason = "waiting_for_candidate_confirmation"
        else:
            decision_reason = "waiting_for_minimum_dwell"

    confirmation_elapsed = (
        seconds_between(evaluation_time, candidate_since)
        if candidate_since
        else 0.0
    )
    dwell_elapsed = seconds_between(
        evaluation_time,
        last_mode_change_at,
    )

    command_required = applied_mode != last_dispatched_mode

    return {
        "requested_mode": requested_mode,
        "applied_mode": applied_mode,
        "candidate_mode": candidate_mode,
        "candidate_since": candidate_since,
        "last_mode_change_at": last_mode_change_at,
        "last_evaluation_at": evaluation_time,
        "last_dispatched_mode": last_dispatched_mode,
        "last_dispatch_at": last_dispatch_at,
        "confirmation_elapsed_seconds": int(confirmation_elapsed),
        "dwell_elapsed_seconds": int(dwell_elapsed),
        "confirmation_required_seconds": MODE_CONFIRMATION_SECONDS,
        "minimum_dwell_seconds": MIN_MODE_DWELL_SECONDS,
        "safety_reduction": safety_reduction,
        "mode_changed": mode_changed,
        "command_required": command_required,
        "decision_reason": decision_reason,
    }


def load_fis_state(station_id: str) -> Dict[str, Any] | None:
    result = status_table.get_item(
        Key={"station_id": station_id},
        ConsistentRead=True,
    )
    item = result.get("Item", {})
    state = item.get("fis_state")

    if isinstance(state, dict):
        return state

    return None


def save_fis_state(
    station_id: str,
    stabilization: Dict[str, Any],
) -> None:
    state = {
        "applied_mode": stabilization["applied_mode"],
        "candidate_mode": stabilization["candidate_mode"],
        "candidate_since": stabilization["candidate_since"],
        "last_mode_change_at": stabilization["last_mode_change_at"],
        "last_evaluation_at": stabilization["last_evaluation_at"],
        "last_dispatched_mode": stabilization.get(
            "last_dispatched_mode"
        ),
        "last_dispatch_at": stabilization.get("last_dispatch_at"),
    }

    status_table.update_item(
        Key={"station_id": station_id},
        UpdateExpression=(
            "SET fis_state = :fis_state, "
            "cloud_requested_mode = :requested_mode, "
            "cloud_operating_mode = :applied_mode, "
            "cloud_mode_updated_at = :updated_at"
        ),
        ExpressionAttributeValues=convert_floats_to_decimal(
            {
                ":fis_state": state,
                ":requested_mode": stabilization["requested_mode"],
                ":applied_mode": stabilization["applied_mode"],
                ":updated_at": stabilization["last_evaluation_at"],
            }
        ),
    )



def mark_command_dispatched(
    stabilization: Dict[str, Any],
    dispatch_timestamp: str,
) -> Dict[str, Any]:
    updated = dict(stabilization)
    updated["last_dispatched_mode"] = stabilization["applied_mode"]
    updated["last_dispatch_at"] = dispatch_timestamp
    return updated


def invoke_command_dispatcher(
    command_request: Dict[str, Any],
) -> Dict[str, Any]:
    lambda_client = boto3.client("lambda")
    invoke_result = lambda_client.invoke(
        FunctionName=COMMAND_DISPATCHER_FUNCTION_NAME,
        InvocationType="RequestResponse",
        Payload=json.dumps(command_request).encode("utf-8"),
    )

    payload_stream = invoke_result.get("Payload")
    raw_payload = payload_stream.read() if payload_stream else b"{}"
    if isinstance(raw_payload, bytes):
        raw_payload = raw_payload.decode("utf-8")

    downstream_result = json.loads(raw_payload or "{}")

    if invoke_result.get("FunctionError"):
        raise RuntimeError(
            "command_dispatcher Lambda execution failed: "
            + json.dumps(downstream_result)
        )

    status_code = downstream_result.get("statusCode", 500)
    if status_code >= 400:
        raise RuntimeError(
            "command_dispatcher returned an error: "
            + json.dumps(downstream_result)
        )

    body = downstream_result.get("body")
    if isinstance(body, str):
        try:
            body = json.loads(body)
        except json.JSONDecodeError:
            pass

    return {
        "status_code": status_code,
        "body": body,
    }

def apply_cloud_side_validation(
    fis_mode: str,
    soc_percent: float,
    fault_state: str,
    local_irradiance_wm2: float = 1000.0,
    weather_index: float = 1.0,
) -> str:
    """Backward-compatible wrapper returning only the requested mode."""
    return evaluate_deterministic_layer(
        fis_mode=fis_mode,
        soc_percent=soc_percent,
        fault_state=fault_state,
        local_irradiance_wm2=local_irradiance_wm2,
        weather_index=weather_index,
    )["requested_mode"]


def get_blocked_reason(
    requested_mode: str,
    fis_mode: str,
    fault_state: str,
    soc_percent: float,
) -> str | None:
    """Compatibility helper for older callers and tests."""
    result = evaluate_deterministic_layer(
        fis_mode=fis_mode,
        soc_percent=soc_percent,
        fault_state=fault_state,
        local_irradiance_wm2=1000.0,
        weather_index=1.0,
    )
    return result["blocked_reason"]

def mode_to_outputs(mode: str) -> int:
    if mode == "M3":
        return 1
    if mode == "M4":
        return 2
    if mode == "M5":
        return 3
    return 0


def build_command_request(
    payload: Dict[str, Any],
    fis_result: Dict[str, Any],
) -> Dict[str, Any]:
    station_id = payload["station_id"]
    deterministic = fis_result["deterministic_validation"]
    final_decision = fis_result["final_decision"]
    applied_mode = final_decision["operating_mode"]
    outputs_active = final_decision["outputs_active"]

    if applied_mode == "M0":
        command = "LOCKOUT"
    elif applied_mode == "M1":
        command = "STOP"
    elif applied_mode == "M2":
        command = "ENABLE_TRACKING"
    elif applied_mode == "M3":
        command = "ENABLE_OUTPUT_1"
    elif applied_mode == "M4":
        command = "ENABLE_OUTPUT_2"
    elif applied_mode == "M5":
        command = "ENABLE_OUTPUT_3"
    else:
        command = "STOP"

    return {
        "station_id": station_id,
        "command": command,
        "source": "cloud_fis",
        "parameters": {
            "requested_mode": final_decision["requested_mode"],
            "applied_mode": applied_mode,
            "max_outputs": outputs_active,
            "tracking_allowed": (
                deterministic["tracking_allowed"]
                and mode_number(applied_mode) >= 2
            ),
            "weather_index": fis_result["weather_fis_output"]["weather_index"],
            "fis_mode": fis_result["main_fis_output"]["fis_mode"],
            "command_required": final_decision["command_required"],
        },
    }

def save_fis_decision(
    payload: Dict[str, Any],
    fis_result: Dict[str, Any],
    command_request: Dict[str, Any],
    dispatch_result: Dict[str, Any] | None = None,
) -> None:
    item = build_fis_history_item(
        payload=payload,
        fis_result=fis_result,
        command_request=command_request,
        dispatch_result=dispatch_result,
    )

    fis_decision_table.put_item(
        Item=convert_floats_to_decimal(item)
    )


def build_fis_history_item(
    payload: Dict[str, Any],
    fis_result: Dict[str, Any],
    command_request: Dict[str, Any],
    dispatch_result: Dict[str, Any] | None = None,
) -> Dict[str, Any]:
    requested_mode = fis_result["final_decision"]["requested_mode"]
    applied_mode = fis_result["final_decision"]["operating_mode"]
    outputs_active = fis_result["final_decision"]["outputs_active"]

    item: Dict[str, Any] = {
        "station_id": payload["station_id"],
        "timestamp": fis_result["timestamp"],
        "fis_implementation_version": FIS_IMPLEMENTATION_VERSION,
        "preliminary": True,
        "inputs": fis_result["inputs"],
        "weather_fis_output": fis_result["weather_fis_output"],
        "main_fis_output": fis_result["main_fis_output"],
        "deterministic_validation": {
            "requested_mode": requested_mode,
            "fault_state_level": fis_result["deterministic_validation"][
                "fault_state_level"
            ],
            "functions_blocked": fis_result["deterministic_validation"][
                "functions_blocked"
            ],
            "tracking_blocked": fis_result["deterministic_validation"][
                "tracking_blocked"
            ],
            "outputs_blocked": fis_result["deterministic_validation"][
                "outputs_blocked"
            ],
            "tracking_allowed": fis_result["deterministic_validation"][
                "tracking_allowed"
            ],
            "charging_allowed": fis_result["deterministic_validation"][
                "charging_allowed"
            ],
            "blocked_reason": fis_result["deterministic_validation"][
                "blocked_reason"
            ],
            "blocked_reasons": fis_result["deterministic_validation"][
                "blocked_reasons"
            ],
        },
        "mode_stabilization": fis_result["mode_stabilization"],
        "final_decision": {
            "requested_mode": requested_mode,
            "operating_mode": applied_mode,
            "outputs_active": outputs_active,
            "command_required": fis_result["final_decision"][
                "command_required"
            ],
        },
        "command_request": command_request,
        "dispatch_result": dispatch_result or {
            "attempted": False,
            "status": "not_available",
        },
    }

    input_timestamp = payload.get("timestamp")

    if isinstance(input_timestamp, str) and input_timestamp:
        item["input_timestamp"] = input_timestamp

    return item


def convert_floats_to_decimal(data: Any) -> Any:
    if isinstance(data, list):
        return [
            convert_floats_to_decimal(item)
            for item in data
        ]

    if isinstance(data, dict):
        return {
            key: convert_floats_to_decimal(value)
            for key, value in data.items()
        }

    if isinstance(data, float):
        return Decimal(str(data))

    return data

def trimf(x: float, a: float, b: float, c: float) -> float:
    if x <= a or x >= c:
        return 0.0
    if x == b:
        return 1.0
    if a < x < b:
        return (x - a) / (b - a)
    if b < x < c:
        return (c - x) / (c - b)
    return 0.0


def trapmf( x: float, a: float, b: float, c: float, d: float,) -> float:

    if x < a or x > d:
        return 0.0

    if b <= x <= c:
        return 1.0

    if a <= x < b:
        if b == a:
            return 1.0

        return (x - a) / (b - a)

    if c < x <= d:
        if d == c:
            return 1.0

        return (d - x) / (d - c)

    return 0.0


def centroid(universe: List[float], membership: List[float], default: float) -> float:
    numerator = sum(x * mu for x, mu in zip(universe, membership))
    denominator = sum(membership)

    if denominator == 0:
        return default

    return numerator / denominator


def current_utc_timestamp() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def response(status_code: int, body: Dict[str, Any]) -> Dict[str, Any]:
    return {
        "statusCode": status_code,
        "body": json.dumps(body),
    }