"""Offline energy-management pilot: current cloud FIS vs threshold baseline.

Run from the project root:
    python tools/energy_management_comparison.py

The script is deliberately offline: it does not invoke Lambda, DynamoDB, IoT Core,
or MQTT. It imports and reuses the pure decision functions from the current
``aws/lambdas/fis_processor/lambda_function.py`` so the FIS rules, deterministic
restrictions, and time-based stabilization are not reimplemented or tuned here.

Pilot scenario (frozen before looking at comparative results)
-------------------------------------------------------------
- 24 h, 5 min resolution (288 samples).
- Initial SOC: 80 %, matching the current CLOUDY_DAY ScenarioManager profile.
- Time-varying solar/cloud/precipitation/demand profiles are taken from the prior
  article daily simulator (CLOUDY_DAY profile).
- Current station energy model is used: 3 x 150 W PV panels, 90 % PV delivery
  efficiency, 3 x 100 Ah / 12 V battery bank, 90 % charge efficiency, 95 %
  discharge efficiency, 5 W base load, 71 W useful power per scooter output,
  and 88 % boost-converter efficiency.
- Panel temperature is fixed at 29 degC, matching the current CLOUDY_DAY
  ScenarioManager profile.
- Both strategies use the same SOC protection policy (15/25 % with 20/30 %
  recovery hysteresis), deterministic safety layer, and 10/15 min stabilization.

Frozen threshold baseline
-------------------------
The reference controller intentionally uses only present SOC, delivered PV power,
and demand. It does not use the fuzzy weather forecast index in its raw decision.
Critical/restricted battery handling and poor-tracking restrictions are still
applied by the exact same deterministic layer as the FIS.

Demand-to-request mapping:
    demand < 0.35     -> 0 requested outputs
    0.35 .. < 0.60   -> 1
    0.60 .. < 0.85   -> 2
    >= 0.85           -> 3

PV-supported outputs are the number that can be supplied by instantaneous PV
surplus after the 5 W base load. Stored-energy support is capped by SOC:
    SOC < 45 %        -> 0 battery-supported outputs
    45 .. < 65 %      -> up to 1
    65 .. < 85 %      -> up to 2
    >= 85 %           -> up to 3
The raw output target is min(demand request, max(PV-supported, SOC support)).
This specification is kept explicit so it is not tuned after observing results.
"""

from __future__ import annotations

import csv
import importlib.util
import json
import math
import os
import sys
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any


PROJECT_ROOT = Path(__file__).resolve().parents[1]
FIS_PATH = PROJECT_ROOT / "aws" / "lambdas" / "fis_processor" / "lambda_function.py"
RESULTS_DIR = PROJECT_ROOT / "results" / "energy_management_pilot"

# Keep the imported Lambda module offline.
os.environ.setdefault("AWS_DEFAULT_REGION", "us-east-2")
os.environ.setdefault("AWS_EC2_METADATA_DISABLED", "true")

STEP_MIN = 5
STEP_H = STEP_MIN / 60.0
STEPS_PER_DAY = 24 * 60 // STEP_MIN
START_TIME = datetime(2026, 1, 1, tzinfo=timezone.utc)

# Current station model constants.
PV_PANEL_COUNT = 3
PV_RATED_PER_PANEL_W = 150.0
PV_REFERENCE_IRRADIANCE_WM2 = 1000.0
PV_REFERENCE_TEMP_C = 25.0
PV_POWER_TEMP_COEFF_PER_C = -0.0040
PV_DELIVERY_EFFICIENCY = 0.90
PANEL_TEMPERATURE_C = 29.0  # current CLOUDY_DAY ScenarioManager value

BATTERY_NOMINAL_VOLTAGE_V = 12.0
BATTERY_COUNT = 3
BATTERY_CAPACITY_AH_EACH = 100.0
BATTERY_ENERGY_WH = BATTERY_NOMINAL_VOLTAGE_V * BATTERY_COUNT * BATTERY_CAPACITY_AH_EACH
BATTERY_CHARGE_EFFICIENCY = 0.90
BATTERY_DISCHARGE_EFFICIENCY = 0.95
INITIAL_SOC_PERCENT = 80.0

RESTRICTED_SOC_PERCENT = 25.0
CRITICAL_SOC_PERCENT = 15.0
NORMAL_RECOVERY_SOC_PERCENT = 30.0
CRITICAL_RECOVERY_SOC_PERCENT = 20.0

BASE_LOAD_POWER_W = 5.0
SCOOTER_USEFUL_POWER_W = 71.0
BOOST_EFFICIENCY = 0.88
SCOOTER_INPUT_POWER_W = SCOOTER_USEFUL_POWER_W / BOOST_EFFICIENCY


@dataclass
class BatteryState:
    soc_percent: float = INITIAL_SOC_PERCENT
    stored_energy_wh: float = BATTERY_ENERGY_WH * INITIAL_SOC_PERCENT / 100.0
    protection_state: str = "NORMAL"
    charged_energy_wh: float = 0.0
    discharged_energy_wh: float = 0.0

    def update_protection(self) -> None:
        soc = self.soc_percent
        state = self.protection_state

        if state == "CRITICAL":
            if soc >= CRITICAL_RECOVERY_SOC_PERCENT:
                self.protection_state = (
                    "NORMAL" if soc >= NORMAL_RECOVERY_SOC_PERCENT else "RESTRICTED"
                )
        elif state == "RESTRICTED":
            if soc <= CRITICAL_SOC_PERCENT:
                self.protection_state = "CRITICAL"
            elif soc >= NORMAL_RECOVERY_SOC_PERCENT:
                self.protection_state = "NORMAL"
        else:
            if soc <= CRITICAL_SOC_PERCENT:
                self.protection_state = "CRITICAL"
            elif soc <= RESTRICTED_SOC_PERCENT:
                self.protection_state = "RESTRICTED"

    def apply_power(self, requested_power_w: float, dt_h: float) -> dict[str, float]:
        """Apply external battery power using the current emulator efficiency semantics.

        Positive power charges the battery. Negative power discharges it.
        Returns external charge/discharge energy and rejected surplus for this step.
        """
        previous = self.stored_energy_wh
        external_charge_wh = 0.0
        external_discharge_wh = 0.0
        rejected_surplus_wh = 0.0
        unserved_deficit_wh = 0.0

        if requested_power_w >= 0.0:
            requested_external_wh = requested_power_w * dt_h
            potential_stored_wh = BATTERY_CHARGE_EFFICIENCY * requested_external_wh
            room_wh = max(0.0, BATTERY_ENERGY_WH - previous)
            stored_delta_wh = min(room_wh, potential_stored_wh)
            self.stored_energy_wh = previous + stored_delta_wh
            external_charge_wh = stored_delta_wh / BATTERY_CHARGE_EFFICIENCY
            rejected_surplus_wh = max(0.0, requested_external_wh - external_charge_wh)
            self.charged_energy_wh += stored_delta_wh
        else:
            requested_external_discharge_wh = (-requested_power_w) * dt_h
            required_stored_wh = requested_external_discharge_wh / BATTERY_DISCHARGE_EFFICIENCY
            available_stored_wh = previous
            applied_stored_wh = min(available_stored_wh, required_stored_wh)
            self.stored_energy_wh = previous - applied_stored_wh
            external_discharge_wh = applied_stored_wh * BATTERY_DISCHARGE_EFFICIENCY
            unserved_deficit_wh = max(
                0.0,
                requested_external_discharge_wh - external_discharge_wh,
            )
            self.discharged_energy_wh += applied_stored_wh

        self.stored_energy_wh = min(max(self.stored_energy_wh, 0.0), BATTERY_ENERGY_WH)
        self.soc_percent = 100.0 * self.stored_energy_wh / BATTERY_ENERGY_WH
        self.update_protection()

        return {
            "external_charge_wh": external_charge_wh,
            "external_discharge_wh": external_discharge_wh,
            "rejected_surplus_wh": rejected_surplus_wh,
            "unserved_deficit_wh": unserved_deficit_wh,
        }


def load_fis_module():
    if not FIS_PATH.exists():
        raise FileNotFoundError(f"FIS module not found: {FIS_PATH}")

    spec = importlib.util.spec_from_file_location("energy_eval_fis", FIS_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError("Could not create FIS import specification")

    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def clamp(value: float, low: float, high: float) -> float:
    return max(low, min(high, value))


def solar_profile_clear_sky(minute: int) -> float:
    """Exact daily shortwave profile used by the prior article simulator."""
    sunrise = 360
    sunset = 1080
    if minute < sunrise or minute > sunset:
        return 0.0
    x = (minute - sunrise) / (sunset - sunrise)
    s = max(0.0, math.sin(math.pi * x))
    return 950.0 * (s ** 1.20)


def demand_profile(minute: int) -> float:
    """Exact high-demand daily profile used by the prior article simulator."""
    hour = minute / 60.0
    if hour < 5.5:
        return 0.20
    if hour < 7.0:
        return 0.70
    if hour < 10.0:
        return 1.00
    if hour < 13.0:
        return 0.95
    if hour < 17.0:
        return 1.00
    if hour < 20.0:
        return 1.00
    if hour < 23.0:
        return 0.75
    return 0.35


def cloudy_day_cloud_cover(minute: int) -> float:
    hour = minute / 60.0
    return 75.0 if 10.0 <= hour <= 15.5 else 45.0


def cloudy_day_precipitation(minute: int) -> float:
    hour = minute / 60.0
    return 55.0 if 11.0 <= hour <= 15.0 else 25.0


def local_irradiance_from_weather(shortwave_wm2: float, cloud_cover_percent: float) -> float:
    cloud_factor = 1.0 - 0.75 * (cloud_cover_percent / 100.0)
    cloud_factor = clamp(cloud_factor, 0.15, 1.0)
    return shortwave_wm2 * cloud_factor


def current_pv_delivered_power(local_irradiance_wm2: float) -> tuple[float, float]:
    """Reproduce the current PVSimulator raw/delivered power equations."""
    temperature_factor = clamp(
        1.0 + PV_POWER_TEMP_COEFF_PER_C * (PANEL_TEMPERATURE_C - PV_REFERENCE_TEMP_C),
        0.0,
        1.25,
    )
    raw = (
        PV_RATED_PER_PANEL_W
        * PV_PANEL_COUNT
        * (max(0.0, local_irradiance_wm2) / PV_REFERENCE_IRRADIANCE_WM2)
        * temperature_factor
    )
    delivered = raw * PV_DELIVERY_EFFICIENCY
    return raw, delivered


def demand_requested_outputs(demand_index: float) -> int:
    if demand_index < 0.35:
        return 0
    if demand_index < 0.60:
        return 1
    if demand_index < 0.85:
        return 2
    return 3


def threshold_raw_mode(
    soc_percent: float,
    pv_delivered_w: float,
    demand_index: float,
) -> str:
    requested_outputs = demand_requested_outputs(demand_index)

    pv_surplus_w = max(0.0, pv_delivered_w - BASE_LOAD_POWER_W)
    pv_supported_outputs = min(3, int(pv_surplus_w // SCOOTER_INPUT_POWER_W))

    if soc_percent < 45.0:
        soc_supported_outputs = 0
    elif soc_percent < 65.0:
        soc_supported_outputs = 1
    elif soc_percent < 85.0:
        soc_supported_outputs = 2
    else:
        soc_supported_outputs = 3

    target_outputs = min(
        requested_outputs,
        max(pv_supported_outputs, soc_supported_outputs),
        3,
    )

    return {0: "M2", 1: "M3", 2: "M4", 3: "M5"}[target_outputs]


def stabilization_state_from_result(result: dict[str, Any]) -> dict[str, Any]:
    """Pretend each offline mode is immediately delivered to the station.

    Dispatch/reconciliation fields do not affect mode stabilization itself, but keeping
    them current makes the state equivalent to a successful closed-loop command path.
    """
    state = dict(result)
    state["last_dispatched_mode"] = result["applied_mode"]
    state["last_dispatch_at"] = result["last_evaluation_at"]
    return state


def run_strategy(strategy: str, fis: Any) -> tuple[list[dict[str, Any]], dict[str, float]]:
    if strategy not in {"fis", "threshold"}:
        raise ValueError(strategy)

    battery = BatteryState()
    battery.update_protection()
    previous_stabilization: dict[str, Any] | None = None
    previous_outputs = 0
    rows: list[dict[str, Any]] = []

    totals = {
        "e_pv_potential_wh": 0.0,
        "e_pv_curtailed_wh": 0.0,
        "e_ev_useful_wh": 0.0,
        "e_station_base_wh": 0.0,
        "e_battery_external_charge_wh": 0.0,
        "e_battery_external_discharge_wh": 0.0,
        "e_battery_stored_charge_wh": 0.0,
        "e_battery_stored_discharge_wh": 0.0,
        "unserved_station_energy_wh": 0.0,
        "demand_weighted_service_numerator": 0.0,
        "demand_weighted_service_denominator": 0.0,
        "requested_output_hours": 0.0,
        "served_output_hours": 0.0,
        "unserved_output_hours": 0.0,
        "mode_changes": 0.0,
    }

    initial_soc = battery.soc_percent
    min_soc = battery.soc_percent
    previous_applied_mode: str | None = None

    for step in range(STEPS_PER_DAY):
        minute = step * STEP_MIN
        timestamp_dt = START_TIME + timedelta(minutes=minute)
        timestamp = timestamp_dt.isoformat().replace("+00:00", "Z")

        shortwave = solar_profile_clear_sky(minute)
        cloud = cloudy_day_cloud_cover(minute)
        precip = cloudy_day_precipitation(minute)
        local_irradiance = local_irradiance_from_weather(shortwave, cloud)
        raw_pv_w, pv_delivered_w = current_pv_delivered_power(local_irradiance)
        demand = demand_profile(minute)

        # The FIS sees the present battery balance based on the currently applied outputs,
        # matching the sequential structure of the article/current functional simulation.
        predecision_load_w = BASE_LOAD_POWER_W + previous_outputs * SCOOTER_INPUT_POWER_W
        p_net_predecision_w = pv_delivered_w - predecision_load_w

        weather_index = fis.evaluate_weather_fis(
            shortwave_radiation_wm2=shortwave,
            cloud_cover_percent=cloud,
            precipitation_probability_percent=precip,
        )

        if strategy == "fis":
            main_result = fis.evaluate_main_fis(
                soc_percent=battery.soc_percent,
                p_net_w=p_net_predecision_w,
                local_irradiance_wm2=local_irradiance,
                weather_index=weather_index,
                demand_index=demand,
            )
            raw_mode = main_result["fis_mode"]
            centroid = main_result["centroid"]
        else:
            raw_mode = threshold_raw_mode(
                soc_percent=battery.soc_percent,
                pv_delivered_w=pv_delivered_w,
                demand_index=demand,
            )
            centroid = float("nan")

        deterministic = fis.evaluate_deterministic_layer(
            fis_mode=raw_mode,
            soc_percent=battery.soc_percent,
            fault_state="normal",
            local_irradiance_wm2=local_irradiance,
            weather_index=weather_index,
            battery_protection_state=battery.protection_state,
        )

        stabilization = fis.stabilize_operating_mode(
            requested_mode=deterministic["requested_mode"],
            deterministic=deterministic,
            previous_state=previous_stabilization,
            evaluation_time=timestamp,
            station_reported_mode=(previous_applied_mode if previous_applied_mode else None),
        )
        applied_mode = stabilization["applied_mode"]
        outputs = fis.mode_to_outputs(applied_mode)
        if deterministic["outputs_blocked"] or deterministic["fault_state_level"] >= 2:
            outputs = 0

        if previous_applied_mode is not None and applied_mode != previous_applied_mode:
            totals["mode_changes"] += 1.0

        final_load_w = BASE_LOAD_POWER_W + outputs * SCOOTER_INPUT_POWER_W
        requested_battery_power_w = pv_delivered_w - final_load_w

        soc_before = battery.soc_percent
        protection_before = battery.protection_state
        battery_step = battery.apply_power(requested_battery_power_w, STEP_H)
        soc_after = battery.soc_percent
        min_soc = min(min_soc, soc_after)

        requested_outputs = demand_requested_outputs(demand)
        served_outputs = min(outputs, requested_outputs)

        totals["e_pv_potential_wh"] += pv_delivered_w * STEP_H
        totals["e_pv_curtailed_wh"] += battery_step["rejected_surplus_wh"]
        totals["e_ev_useful_wh"] += outputs * SCOOTER_USEFUL_POWER_W * STEP_H
        totals["e_station_base_wh"] += BASE_LOAD_POWER_W * STEP_H
        totals["e_battery_external_charge_wh"] += battery_step["external_charge_wh"]
        totals["e_battery_external_discharge_wh"] += battery_step["external_discharge_wh"]
        totals["unserved_station_energy_wh"] += battery_step["unserved_deficit_wh"]
        totals["demand_weighted_service_numerator"] += demand * (outputs / 3.0) * STEP_H
        totals["demand_weighted_service_denominator"] += demand * STEP_H
        totals["requested_output_hours"] += requested_outputs * STEP_H
        totals["served_output_hours"] += served_outputs * STEP_H
        totals["unserved_output_hours"] += max(0, requested_outputs - outputs) * STEP_H

        rows.append(
            {
                "strategy": strategy,
                "time_min": minute,
                "time_h": minute / 60.0,
                "timestamp": timestamp,
                "soc_before_percent": soc_before,
                "soc_after_percent": soc_after,
                "battery_protection_before": protection_before,
                "battery_protection_after": battery.protection_state,
                "shortwave_radiation_wm2": shortwave,
                "cloud_cover_percent": cloud,
                "precipitation_probability_percent": precip,
                "weather_index": weather_index,
                "local_irradiance_wm2": local_irradiance,
                "pv_raw_power_w": raw_pv_w,
                "pv_delivered_power_w": pv_delivered_w,
                "demand_index": demand,
                "demand_requested_outputs": requested_outputs,
                "p_net_predecision_w": p_net_predecision_w,
                "raw_mode": raw_mode,
                "centroid": centroid,
                "requested_mode": deterministic["requested_mode"],
                "applied_mode": applied_mode,
                "active_outputs": outputs,
                "final_station_load_w": final_load_w,
                "battery_requested_power_w": requested_battery_power_w,
                "pv_curtailed_wh_step": battery_step["rejected_surplus_wh"],
                "blocked_reasons": ";".join(deterministic["blocked_reasons"]),
                "stabilization_reason": stabilization["decision_reason"],
            }
        )

        previous_stabilization = stabilization_state_from_result(stabilization)
        previous_applied_mode = applied_mode
        previous_outputs = outputs

    totals["e_battery_stored_charge_wh"] = battery.charged_energy_wh
    totals["e_battery_stored_discharge_wh"] = battery.discharged_energy_wh
    totals["soc_initial_percent"] = initial_soc
    totals["soc_min_percent"] = min_soc
    totals["soc_final_percent"] = battery.soc_percent
    totals["soc_change_percentage_points"] = battery.soc_percent - initial_soc
    totals["pv_utilization_percent"] = (
        100.0 * (totals["e_pv_potential_wh"] - totals["e_pv_curtailed_wh"])
        / totals["e_pv_potential_wh"]
        if totals["e_pv_potential_wh"] > 0.0
        else 0.0
    )
    totals["demand_weighted_service_index"] = (
        totals["demand_weighted_service_numerator"]
        / totals["demand_weighted_service_denominator"]
        if totals["demand_weighted_service_denominator"] > 0.0
        else 0.0
    )
    totals["requested_output_service_percent"] = (
        100.0 * totals["served_output_hours"] / totals["requested_output_hours"]
        if totals["requested_output_hours"] > 0.0
        else 0.0
    )

    return rows, totals


def write_rows(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not rows:
        return
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def write_summary_csv(path: Path, summaries: dict[str, dict[str, float]]) -> None:
    keys = sorted({key for summary in summaries.values() for key in summary})
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["metric", *summaries.keys()])
        for key in keys:
            writer.writerow([key, *[summaries[name].get(key, "") for name in summaries]])


def save_plots(all_rows: dict[str, list[dict[str, Any]]]) -> list[Path]:
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        return []

    generated: list[Path] = []

    fig = plt.figure(figsize=(9, 4.8))
    for name, rows in all_rows.items():
        plt.plot([r["time_h"] for r in rows], [r["soc_after_percent"] for r in rows], label=name)
    plt.xlabel("Time [h]")
    plt.ylabel("Battery SOC [%]")
    plt.title("Pilot comparison: battery SOC")
    plt.grid(True, alpha=0.25)
    plt.legend()
    plt.tight_layout()
    path = RESULTS_DIR / "pilot_soc_comparison.png"
    fig.savefig(path, dpi=180)
    plt.close(fig)
    generated.append(path)

    fig = plt.figure(figsize=(9, 4.8))
    for name, rows in all_rows.items():
        plt.step(
            [r["time_h"] for r in rows],
            [r["active_outputs"] for r in rows],
            where="post",
            label=name,
        )
    plt.xlabel("Time [h]")
    plt.ylabel("Active charging outputs")
    plt.yticks([0, 1, 2, 3])
    plt.title("Pilot comparison: charging service")
    plt.grid(True, alpha=0.25)
    plt.legend()
    plt.tight_layout()
    path = RESULTS_DIR / "pilot_outputs_comparison.png"
    fig.savefig(path, dpi=180)
    plt.close(fig)
    generated.append(path)

    return generated


def print_summary(summaries: dict[str, dict[str, float]]) -> None:
    metrics = [
        "e_pv_potential_wh",
        "e_pv_curtailed_wh",
        "pv_utilization_percent",
        "e_ev_useful_wh",
        "soc_min_percent",
        "soc_final_percent",
        "soc_change_percentage_points",
        "demand_weighted_service_index",
        "requested_output_service_percent",
        "e_battery_stored_charge_wh",
        "e_battery_stored_discharge_wh",
        "mode_changes",
    ]

    print("\n=== Energy-management pilot ===")
    print("Scenario: prior article CLOUDY_DAY profile + current station energy model")
    print(f"Initial SOC: {INITIAL_SOC_PERCENT:.1f}% | Step: {STEP_MIN} min | Battery: {BATTERY_ENERGY_WH:.0f} Wh")
    print()
    print(f"{'Metric':38s} {'FIS':>14s} {'Threshold':>14s}")
    print("-" * 70)
    for metric in metrics:
        print(
            f"{metric:38s} "
            f"{summaries['fis'][metric]:14.4f} "
            f"{summaries['threshold'][metric]:14.4f}"
        )


def main() -> int:
    fis = load_fis_module()
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)

    all_rows: dict[str, list[dict[str, Any]]] = {}
    summaries: dict[str, dict[str, float]] = {}

    for strategy in ("fis", "threshold"):
        rows, summary = run_strategy(strategy, fis)
        all_rows[strategy] = rows
        summaries[strategy] = summary
        write_rows(RESULTS_DIR / f"pilot_{strategy}.csv", rows)

    write_summary_csv(RESULTS_DIR / "pilot_summary.csv", summaries)
    with (RESULTS_DIR / "pilot_summary.json").open("w", encoding="utf-8") as handle:
        json.dump(summaries, handle, indent=2)

    plot_paths = save_plots(all_rows)
    print_summary(summaries)
    print(f"\nResults written to: {RESULTS_DIR}")
    if plot_paths:
        print("Plots:")
        for path in plot_paths:
            print(f"  - {path.name}")
    else:
        print("matplotlib not installed: CSV/JSON results were still generated.")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
