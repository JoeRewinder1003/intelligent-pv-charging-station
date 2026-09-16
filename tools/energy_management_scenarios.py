"""Scenario extension for the frozen energy-management pilot.

This script reuses the validated pilot implementation in
``tools/energy_management_comparison.py`` without changing the FIS rules or the
threshold baseline.  It adds controlled scenarios that can be selected from the
command line.

Examples (run from project root):
    python tools/energy_management_scenarios.py --scenario favorable
    python tools/energy_management_scenarios.py --scenario restrictive
    python tools/energy_management_scenarios.py --scenario persistent-low-generation

The persistent-low-generation case is a *synthetic stress test*, not measured
meteorological data.  It repeats five consecutive low-generation days using
50 % of the clear-sky shortwave profile, 85 % cloud cover, and 70 % precipitation
probability while preserving the same daily demand profile.  Its purpose is to
evaluate cumulative stored-energy management when poor solar availability
persists for several days.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from dataclasses import dataclass
from datetime import timedelta
from pathlib import Path
from typing import Any, Callable

import energy_management_comparison as pilot


@dataclass(frozen=True)
class ScenarioConfig:
    key: str
    title: str
    description: str
    days: int
    initial_soc_percent: float
    panel_temperature_c: float
    cloud_cover_fn: Callable[[int, int], float]
    precipitation_fn: Callable[[int, int], float]
    shortwave_scale: float = 1.0


def favorable_cloud_cover(_day: int, minute: int) -> float:
    """CLEAR_DAY cloud-cover profile from the prior article simulator."""
    hour = minute / 60.0
    return 10.0 + 5.0 * math.sin(2.0 * math.pi * hour / 24.0)


def favorable_precipitation(_day: int, _minute: int) -> float:
    return 5.0


def cloudy_cloud_cover(_day: int, minute: int) -> float:
    return pilot.cloudy_day_cloud_cover(minute)


def cloudy_precipitation(_day: int, minute: int) -> float:
    return pilot.cloudy_day_precipitation(minute)


def restrictive_cloud_cover(_day: int, _minute: int) -> float:
    # LOW_BATTERY profile from the prior article simulator.
    return 35.0


def restrictive_precipitation(_day: int, _minute: int) -> float:
    return 10.0


def persistent_low_generation_cloud_cover(_day: int, _minute: int) -> float:
    # Controlled stress-test value; intentionally constant across the five days.
    return 85.0


def persistent_low_generation_precipitation(_day: int, _minute: int) -> float:
    return 70.0


SCENARIOS: dict[str, ScenarioConfig] = {
    "favorable": ScenarioConfig(
        key="favorable",
        title="FAVORABLE_CLEAR_DAY",
        description=(
            "Prior-article CLEAR_DAY weather profile + current station energy model; "
            "initial SOC and panel temperature taken from current CLEAR_DAY ScenarioManager."
        ),
        days=1,
        initial_soc_percent=95.0,
        panel_temperature_c=45.0,
        cloud_cover_fn=favorable_cloud_cover,
        precipitation_fn=favorable_precipitation,
    ),
    "cloudy": ScenarioConfig(
        key="cloudy",
        title="CLOUDY_DAY",
        description="Validated pilot CLOUDY_DAY profile.",
        days=1,
        initial_soc_percent=80.0,
        panel_temperature_c=29.0,
        cloud_cover_fn=cloudy_cloud_cover,
        precipitation_fn=cloudy_precipitation,
    ),
    "restrictive": ScenarioConfig(
        key="restrictive",
        title="LOW_BATTERY",
        description=(
            "Prior-article LOW_BATTERY weather profile + current LOW_BATTERY "
            "ScenarioManager initial SOC/panel temperature."
        ),
        days=1,
        initial_soc_percent=30.0,
        panel_temperature_c=34.0,
        cloud_cover_fn=restrictive_cloud_cover,
        precipitation_fn=restrictive_precipitation,
    ),
    "persistent-low-generation": ScenarioConfig(
        key="persistent-low-generation",
        title="PERSISTENT_LOW_GENERATION_5D",
        description=(
            "Synthetic five-day stress test with 50% of the clear-sky shortwave profile, "
            "persistent 85% cloud cover, and 70% precipitation probability; same daily "
            "demand profile repeated each day."
        ),
        days=5,
        initial_soc_percent=80.0,
        panel_temperature_c=29.0,
        cloud_cover_fn=persistent_low_generation_cloud_cover,
        precipitation_fn=persistent_low_generation_precipitation,
        shortwave_scale=0.50,
    ),
}


def pv_delivered_power(local_irradiance_wm2: float, panel_temperature_c: float) -> tuple[float, float]:
    """Same PV equations as the validated pilot, with scenario panel temperature."""
    temperature_factor = pilot.clamp(
        1.0
        + pilot.PV_POWER_TEMP_COEFF_PER_C
        * (panel_temperature_c - pilot.PV_REFERENCE_TEMP_C),
        0.0,
        1.25,
    )
    raw = (
        pilot.PV_RATED_PER_PANEL_W
        * pilot.PV_PANEL_COUNT
        * (max(0.0, local_irradiance_wm2) / pilot.PV_REFERENCE_IRRADIANCE_WM2)
        * temperature_factor
    )
    delivered = raw * pilot.PV_DELIVERY_EFFICIENCY
    return raw, delivered


def make_battery(initial_soc_percent: float) -> pilot.BatteryState:
    battery = pilot.BatteryState(
        soc_percent=initial_soc_percent,
        stored_energy_wh=pilot.BATTERY_ENERGY_WH * initial_soc_percent / 100.0,
        protection_state="NORMAL",
        charged_energy_wh=0.0,
        discharged_energy_wh=0.0,
    )
    battery.update_protection()
    return battery


def run_strategy(
    strategy: str,
    fis: Any,
    scenario: ScenarioConfig,
) -> tuple[list[dict[str, Any]], dict[str, float]]:
    if strategy not in {"fis", "threshold"}:
        raise ValueError(strategy)

    battery = make_battery(scenario.initial_soc_percent)
    previous_stabilization: dict[str, Any] | None = None
    previous_outputs = 0
    rows: list[dict[str, Any]] = []

    totals = {
        "e_pv_potential_wh": 0.0,
        "e_pv_curtailed_wh": 0.0,
        "e_ev_useful_wh": 0.0,
        "e_ev_during_battery_discharge_wh": 0.0,
        "e_ev_when_pv_zero_wh": 0.0,
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
        "time_restricted_h": 0.0,
        "time_critical_h": 0.0,
    }

    initial_soc = battery.soc_percent
    min_soc = battery.soc_percent
    previous_applied_mode: str | None = None
    total_steps = scenario.days * pilot.STEPS_PER_DAY

    for step in range(total_steps):
        elapsed_min = step * pilot.STEP_MIN
        day_index = elapsed_min // (24 * 60)
        minute_of_day = elapsed_min % (24 * 60)
        timestamp_dt = pilot.START_TIME + timedelta(minutes=elapsed_min)
        timestamp = timestamp_dt.isoformat().replace("+00:00", "Z")

        shortwave = pilot.solar_profile_clear_sky(minute_of_day) * scenario.shortwave_scale
        cloud = scenario.cloud_cover_fn(day_index, minute_of_day)
        precip = scenario.precipitation_fn(day_index, minute_of_day)
        local_irradiance = pilot.local_irradiance_from_weather(shortwave, cloud)
        raw_pv_w, pv_delivered_w = pv_delivered_power(
            local_irradiance,
            scenario.panel_temperature_c,
        )
        demand = pilot.demand_profile(minute_of_day)

        predecision_load_w = pilot.BASE_LOAD_POWER_W + previous_outputs * pilot.SCOOTER_INPUT_POWER_W
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
            # Imported unchanged from the frozen/validated pilot baseline.
            raw_mode = pilot.threshold_raw_mode(
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

        final_load_w = pilot.BASE_LOAD_POWER_W + outputs * pilot.SCOOTER_INPUT_POWER_W
        requested_battery_power_w = pv_delivered_w - final_load_w

        soc_before = battery.soc_percent
        protection_before = battery.protection_state
        battery_step = battery.apply_power(requested_battery_power_w, pilot.STEP_H)
        soc_after = battery.soc_percent
        min_soc = min(min_soc, soc_after)

        requested_outputs = pilot.demand_requested_outputs(demand)
        served_outputs = min(outputs, requested_outputs)
        ev_step_wh = outputs * pilot.SCOOTER_USEFUL_POWER_W * pilot.STEP_H

        totals["e_pv_potential_wh"] += pv_delivered_w * pilot.STEP_H
        totals["e_pv_curtailed_wh"] += battery_step["rejected_surplus_wh"]
        totals["e_ev_useful_wh"] += ev_step_wh
        if battery_step["external_discharge_wh"] > 1e-12:
            totals["e_ev_during_battery_discharge_wh"] += ev_step_wh
        if pv_delivered_w <= 1e-9:
            totals["e_ev_when_pv_zero_wh"] += ev_step_wh
        totals["e_station_base_wh"] += pilot.BASE_LOAD_POWER_W * pilot.STEP_H
        totals["e_battery_external_charge_wh"] += battery_step["external_charge_wh"]
        totals["e_battery_external_discharge_wh"] += battery_step["external_discharge_wh"]
        totals["unserved_station_energy_wh"] += battery_step["unserved_deficit_wh"]
        totals["demand_weighted_service_numerator"] += demand * (outputs / 3.0) * pilot.STEP_H
        totals["demand_weighted_service_denominator"] += demand * pilot.STEP_H
        totals["requested_output_hours"] += requested_outputs * pilot.STEP_H
        totals["served_output_hours"] += served_outputs * pilot.STEP_H
        totals["unserved_output_hours"] += max(0, requested_outputs - outputs) * pilot.STEP_H
        if protection_before == "RESTRICTED":
            totals["time_restricted_h"] += pilot.STEP_H
        elif protection_before == "CRITICAL":
            totals["time_critical_h"] += pilot.STEP_H

        rows.append(
            {
                "strategy": strategy,
                "scenario": scenario.key,
                "day": day_index + 1,
                "time_min": elapsed_min,
                "time_h": elapsed_min / 60.0,
                "time_of_day_h": minute_of_day / 60.0,
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
                "battery_external_discharge_wh_step": battery_step["external_discharge_wh"],
                "ev_useful_wh_step": ev_step_wh,
                "pv_curtailed_wh_step": battery_step["rejected_surplus_wh"],
                "blocked_reasons": ";".join(deterministic["blocked_reasons"]),
                "stabilization_reason": stabilization["decision_reason"],
            }
        )

        previous_stabilization = pilot.stabilization_state_from_result(stabilization)
        previous_applied_mode = applied_mode
        previous_outputs = outputs

    totals["e_battery_stored_charge_wh"] = battery.charged_energy_wh
    totals["e_battery_stored_discharge_wh"] = battery.discharged_energy_wh
    totals["soc_initial_percent"] = initial_soc
    totals["soc_min_percent"] = min_soc
    totals["soc_final_percent"] = battery.soc_percent
    totals["soc_change_percentage_points"] = battery.soc_percent - initial_soc
    totals["pv_utilization_percent"] = (
        100.0
        * (totals["e_pv_potential_wh"] - totals["e_pv_curtailed_wh"])
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
    totals["ev_during_battery_discharge_percent"] = (
        100.0 * totals["e_ev_during_battery_discharge_wh"] / totals["e_ev_useful_wh"]
        if totals["e_ev_useful_wh"] > 0.0
        else 0.0
    )
    totals["ev_when_pv_zero_percent"] = (
        100.0 * totals["e_ev_when_pv_zero_wh"] / totals["e_ev_useful_wh"]
        if totals["e_ev_useful_wh"] > 0.0
        else 0.0
    )
    totals["duration_days"] = float(scenario.days)

    return rows, totals


def write_daily_summary(
    path: Path,
    all_rows: dict[str, list[dict[str, Any]]],
    scenario: ScenarioConfig,
) -> None:
    fieldnames = [
        "strategy",
        "day",
        "soc_start_percent",
        "soc_min_percent",
        "soc_end_percent",
        "e_pv_wh",
        "e_ev_useful_wh",
        "e_ev_during_battery_discharge_wh",
        "requested_output_service_percent",
    ]
    output: list[dict[str, Any]] = []

    for strategy, rows in all_rows.items():
        for day in range(1, scenario.days + 1):
            day_rows = [r for r in rows if r["day"] == day]
            if not day_rows:
                continue
            requested_h = sum(
                r["demand_requested_outputs"] * pilot.STEP_H for r in day_rows
            )
            served_h = sum(
                min(r["active_outputs"], r["demand_requested_outputs"]) * pilot.STEP_H
                for r in day_rows
            )
            output.append(
                {
                    "strategy": strategy,
                    "day": day,
                    "soc_start_percent": day_rows[0]["soc_before_percent"],
                    "soc_min_percent": min(r["soc_after_percent"] for r in day_rows),
                    "soc_end_percent": day_rows[-1]["soc_after_percent"],
                    "e_pv_wh": sum(r["pv_delivered_power_w"] * pilot.STEP_H for r in day_rows),
                    "e_ev_useful_wh": sum(r["ev_useful_wh_step"] for r in day_rows),
                    "e_ev_during_battery_discharge_wh": sum(
                        r["ev_useful_wh_step"]
                        for r in day_rows
                        if r["battery_external_discharge_wh_step"] > 1e-12
                    ),
                    "requested_output_service_percent": (
                        100.0 * served_h / requested_h if requested_h > 0.0 else 0.0
                    ),
                }
            )

    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(output)


def save_plots(
    results_dir: Path,
    scenario: ScenarioConfig,
    all_rows: dict[str, list[dict[str, Any]]],
) -> list[Path]:
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        return []

    generated: list[Path] = []

    fig = plt.figure(figsize=(9, 4.8))
    for name, rows in all_rows.items():
        plt.plot(
            [r["time_h"] for r in rows],
            [r["soc_after_percent"] for r in rows],
            label=name,
        )
    if scenario.days > 1:
        for day in range(1, scenario.days):
            plt.axvline(day * 24.0, linewidth=0.8, alpha=0.25)
    plt.xlabel("Time [h]")
    plt.ylabel("Battery SOC [%]")
    plt.title(f"{scenario.title}: battery SOC")
    plt.grid(True, alpha=0.25)
    plt.legend()
    plt.tight_layout()
    path = results_dir / "soc_comparison.png"
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
    if scenario.days > 1:
        for day in range(1, scenario.days):
            plt.axvline(day * 24.0, linewidth=0.8, alpha=0.25)
    plt.xlabel("Time [h]")
    plt.ylabel("Active charging outputs")
    plt.yticks([0, 1, 2, 3])
    plt.title(f"{scenario.title}: charging service")
    plt.grid(True, alpha=0.25)
    plt.legend()
    plt.tight_layout()
    path = results_dir / "outputs_comparison.png"
    fig.savefig(path, dpi=180)
    plt.close(fig)
    generated.append(path)

    return generated


def print_summary(
    scenario: ScenarioConfig,
    summaries: dict[str, dict[str, float]],
) -> None:
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
        "e_ev_during_battery_discharge_wh",
        "ev_during_battery_discharge_percent",
        "e_ev_when_pv_zero_wh",
        "ev_when_pv_zero_percent",
        "time_restricted_h",
        "time_critical_h",
        "mode_changes",
    ]

    print("\n=== Energy-management scenario comparison ===")
    print(f"Scenario: {scenario.title}")
    print(f"Definition: {scenario.description}")
    print(
        f"Duration: {scenario.days} day(s) | Initial SOC: {scenario.initial_soc_percent:.1f}% "
        f"| Step: {pilot.STEP_MIN} min | Battery: {pilot.BATTERY_ENERGY_WH:.0f} Wh"
    )
    print()
    print(f"{'Metric':42s} {'FIS':>14s} {'Threshold':>14s}")
    print("-" * 74)
    for metric in metrics:
        print(
            f"{metric:42s} "
            f"{summaries['fis'][metric]:14.4f} "
            f"{summaries['threshold'][metric]:14.4f}"
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--scenario",
        choices=sorted(SCENARIOS),
        default="favorable",
        help="Scenario to execute (default: favorable).",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    scenario = SCENARIOS[args.scenario]
    fis = pilot.load_fis_module()

    results_dir = pilot.PROJECT_ROOT / "results" / f"energy_management_{scenario.key.replace('-', '_')}"
    results_dir.mkdir(parents=True, exist_ok=True)

    all_rows: dict[str, list[dict[str, Any]]] = {}
    summaries: dict[str, dict[str, float]] = {}

    for strategy in ("fis", "threshold"):
        rows, summary = run_strategy(strategy, fis, scenario)
        all_rows[strategy] = rows
        summaries[strategy] = summary
        pilot.write_rows(results_dir / f"{scenario.key}_{strategy}.csv", rows)

    pilot.write_summary_csv(results_dir / "summary.csv", summaries)
    with (results_dir / "summary.json").open("w", encoding="utf-8") as handle:
        json.dump(summaries, handle, indent=2)
    write_daily_summary(results_dir / "daily_summary.csv", all_rows, scenario)

    plot_paths = save_plots(results_dir, scenario, all_rows)
    print_summary(scenario, summaries)
    print(f"\nResults written to: {results_dir}")
    if plot_paths:
        print("Plots:")
        for path in plot_paths:
            print(f"  - {path.name}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
