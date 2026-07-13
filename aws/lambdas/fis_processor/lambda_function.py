import json
from datetime import datetime, timezone
from typing import Any, Dict, List


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
    Evaluates the cloud-side fuzzy decision system.

    This first version:
    - receives a telemetry-like payload,
    - estimates Weather Index,
    - evaluates the Main FIS,
    - returns a cloud-side command request.

    It does not publish MQTT commands yet. That will be handled by
    command_dispatcher in a later integration step.
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

        fis_result = evaluate_fis(payload)
        command_request = build_command_request(payload, fis_result)

        return response(
            200,
            {
                "message": "FIS decision evaluated successfully",
                "fis_result": fis_result,
                "command_request": command_request,
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


def evaluate_fis(payload: Dict[str, Any]) -> Dict[str, Any]:
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

    requested_mode = apply_cloud_side_validation(
        fis_mode=main_result["fis_mode"],
        soc_percent=inputs["soc_percent"],
        fault_state=payload.get("fault_state", "normal"),
    )

    outputs_active = mode_to_outputs(requested_mode)

    return {
        "timestamp": current_utc_timestamp(),
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
        "deterministic_validation": {
            "requested_mode": requested_mode,
            "blocked_reason": get_blocked_reason(
                requested_mode,
                main_result["fis_mode"],
                payload.get("fault_state", "normal"),
                inputs["soc_percent"],
            ),
        },
        "final_decision": {
            "operating_mode_request": requested_mode,
            "outputs_active_request": outputs_active,
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
    sw_low = trapmf(shortwave_radiation_wm2, 0, 0, 200, 400)
    sw_med = trimf(shortwave_radiation_wm2, 250, 500, 750)
    sw_high = trapmf(shortwave_radiation_wm2, 600, 800, 1000, 1000)

    cloud_low = trapmf(cloud_cover_percent, 0, 0, 20, 40)
    cloud_med = trimf(cloud_cover_percent, 25, 50, 75)
    cloud_high = trapmf(cloud_cover_percent, 60, 80, 100, 100)

    prcp_low = trapmf(precipitation_probability_percent, 0, 0, 20, 40)
    prcp_med = trimf(precipitation_probability_percent, 25, 50, 75)
    prcp_high = trapmf(precipitation_probability_percent, 60, 80, 100, 100)

    rules = []

    # Good weather
    rules.append(("high", min(sw_high, cloud_low, prcp_low)))
    rules.append(("high", min(sw_high, cloud_med, prcp_low)))

    # Medium weather
    rules.append(("medium", min(sw_med, prcp_low)))
    rules.append(("medium", min(sw_high, cloud_high)))
    rules.append(("medium", min(sw_med, cloud_med, prcp_med)))

    # Poor weather
    rules.append(("low", sw_low))
    rules.append(("low", cloud_high))
    rules.append(("low", prcp_high))
    rules.append(("low", min(sw_med, prcp_high)))

    universe = [i / 100 for i in range(0, 101)]
    aggregated = []

    for x in universe:
        low_mf = trapmf(x, 0.0, 0.0, 0.25, 0.45)
        med_mf = trimf(x, 0.30, 0.50, 0.70)
        high_mf = trapmf(x, 0.55, 0.75, 1.0, 1.0)

        value = 0.0

        for label, strength in rules:
            if label == "low":
                value = max(value, min(strength, low_mf))
            elif label == "medium":
                value = max(value, min(strength, med_mf))
            elif label == "high":
                value = max(value, min(strength, high_mf))

        aggregated.append(value)

    return centroid(universe, aggregated, default=0.5)


def evaluate_main_fis(
    soc_percent: float,
    p_net_w: float,
    local_irradiance_wm2: float,
    weather_index: float,
    demand_index: float,
) -> Dict[str, Any]:
    soc_low = trapmf(soc_percent, 0, 0, 80, 85)
    soc_med = trimf(soc_percent, 80, 88, 96)
    soc_high = trapmf(soc_percent, 90, 96, 100, 100)

    p_neg = trapmf(p_net_w, -500, -500, -120, -20)
    p_bal = trimf(p_net_w, -80, 0, 80)
    p_pos = trapmf(p_net_w, 20, 120, 500, 500)

    irr_low = trapmf(local_irradiance_wm2, 0, 0, 250, 400)
    irr_med = trimf(local_irradiance_wm2, 300, 550, 800)
    irr_high = trapmf(local_irradiance_wm2, 650, 800, 1000, 1000)

    w_low = trapmf(weather_index, 0.0, 0.0, 0.25, 0.45)
    w_med = trimf(weather_index, 0.30, 0.50, 0.70)
    w_high = trapmf(weather_index, 0.55, 0.75, 1.0, 1.0)

    d_low = trapmf(demand_index, 0.0, 0.0, 0.25, 0.45)
    d_med = trimf(demand_index, 0.30, 0.50, 0.70)
    d_high = trapmf(demand_index, 0.55, 0.75, 1.0, 1.0)

    rules = []

    # Protection and low-energy states
    rules.append((0, soc_low))
    rules.append((1, min(soc_med, p_neg, irr_low)))
    rules.append((2, min(soc_med, p_pos)))

    # Tracking / telemetry states
    rules.append((2, min(soc_high, irr_med, w_med, d_low)))
    rules.append((2, min(soc_high, irr_high, w_high, d_low)))

    # Charging service levels
    rules.append((3, min(soc_high, p_pos, irr_med, w_med, d_med)))
    rules.append((3, min(soc_med, p_pos, irr_high, w_high, d_med)))

    rules.append((4, min(soc_high, p_pos, irr_high, w_high, d_med)))
    rules.append((4, min(soc_high, p_pos, irr_med, w_high, d_high)))

    rules.append((5, min(soc_high, p_pos, irr_high, w_high, d_high)))

    # Conservative fallback when energy balance is not clearly positive
    rules.append((2, min(soc_high, p_bal, d_low)))
    rules.append((3, min(soc_high, p_bal, d_med)))
    rules.append((3, min(soc_high, p_bal, d_high)))

    universe = [i / 20 for i in range(0, 101)]  # 0.00 to 5.00
    aggregated = []

    for x in universe:
        mode_mfs = {
            0: trapmf(x, 0.0, 0.0, 0.3, 0.7),
            1: trimf(x, 0.5, 1.0, 1.5),
            2: trimf(x, 1.5, 2.0, 2.5),
            3: trimf(x, 2.5, 3.0, 3.5),
            4: trimf(x, 3.5, 4.0, 4.5),
            5: trapmf(x, 4.3, 4.7, 5.0, 5.0),
        }

        value = 0.0

        for mode, strength in rules:
            value = max(value, min(strength, mode_mfs[mode]))

        aggregated.append(value)

    crisp_value = centroid(universe, aggregated, default=1.0)
    mode_number = max(0, min(5, int(round(crisp_value))))

    return {
        "centroid": crisp_value,
        "fis_mode": OPERATING_MODES[mode_number],
    }


def apply_cloud_side_validation(
    fis_mode: str,
    soc_percent: float,
    fault_state: str,
) -> str:
    if fault_state == "critical_lockout":
        return "M0"

    if soc_percent < 85:
        if fis_mode in {"M3", "M4", "M5"}:
            return "M2"

    return fis_mode


def get_blocked_reason(
    requested_mode: str,
    fis_mode: str,
    fault_state: str,
    soc_percent: float,
) -> str | None:
    if fault_state == "critical_lockout":
        return "critical_lockout"

    if requested_mode != fis_mode and soc_percent < 85:
        return "low_soc_cloud_side_limit"

    return None


def mode_to_outputs(mode: str) -> int:
    if mode == "M3":
        return 1
    if mode == "M4":
        return 2
    if mode == "M5":
        return 3
    return 0


def build_command_request(payload: Dict[str, Any], fis_result: Dict[str, Any]) -> Dict[str, Any]:
    station_id = payload["station_id"]
    requested_mode = fis_result["deterministic_validation"]["requested_mode"]
    outputs_active = fis_result["final_decision"]["outputs_active_request"]

    if requested_mode == "M0":
        command = "LOCKOUT"
    elif requested_mode == "M1":
        command = "STOP"
    elif requested_mode == "M2":
        command = "ENABLE_TRACKING"
    elif requested_mode == "M3":
        command = "ENABLE_OUTPUT_1"
    elif requested_mode == "M4":
        command = "ENABLE_OUTPUT_2"
    elif requested_mode == "M5":
        command = "ENABLE_OUTPUT_3"
    else:
        command = "STOP"

    return {
        "station_id": station_id,
        "command": command,
        "source": "cloud_fis",
        "parameters": {
            "requested_mode": requested_mode,
            "max_outputs": outputs_active,
            "tracking_allowed": requested_mode in {"M2", "M3", "M4", "M5"},
            "weather_index": fis_result["weather_fis_output"]["weather_index"],
            "fis_mode": fis_result["main_fis_output"]["fis_mode"],
        },
    }


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