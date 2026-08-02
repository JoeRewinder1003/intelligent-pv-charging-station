"""Compare the current cloud FIS implementation with the article v9 reference.

Place this file at:
    station_cloud_v1/tools/compare_fis_v9.py

Run from the project root:
    python tools/compare_fis_v9.py

This script does not access AWS, write DynamoDB, or publish MQTT messages.
It compares only the pure Weather FIS and Main FIS calculations.
"""

from __future__ import annotations

import importlib.util
import math
import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable


PROJECT_ROOT = Path(__file__).resolve().parents[1]
CURRENT_FIS_PATH = PROJECT_ROOT / "aws" / "lambdas" / "fis_processor" / "lambda_function.py"

# Avoid boto3 metadata/credential discovery while importing the Lambda module.
os.environ.setdefault("AWS_DEFAULT_REGION", "us-east-2")
os.environ.setdefault("AWS_EC2_METADATA_DISABLED", "true")


def load_current_fis():
    if not CURRENT_FIS_PATH.exists():
        raise FileNotFoundError(f"Current FIS module not found: {CURRENT_FIS_PATH}")

    spec = importlib.util.spec_from_file_location("current_fis", CURRENT_FIS_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError("Could not create import specification for current FIS")

    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def trimf(x: float, a: float, b: float, c: float) -> float:
    # Exact behavior of the v9 ESP32 helper.
    if x <= a or x >= c:
        return 0.0
    if x == b:
        return 1.0
    if x < b:
        return (x - a) / (b - a)
    return (c - x) / (c - b)


def trapmf(x: float, a: float, b: float, c: float, d: float) -> float:
    # Exact behavior of the v9 ESP32 helper.
    if x <= a or x >= d:
        return 0.0
    if b <= x <= c:
        return 1.0
    if a < x < b:
        return (x - a) / (b - a)
    return (d - x) / (d - c)


def weather_output_mf(x: float, term: int) -> float:
    if term == 0:
        return trapmf(x, -0.10, 0.00, 0.20, 0.45)
    if term == 1:
        return trimf(x, 0.25, 0.50, 0.75)
    if term == 2:
        return trapmf(x, 0.55, 0.80, 1.00, 1.10)
    return 0.0


def reference_weather_fis(swrad: float, cloud: float, precip: float) -> float:
    rad_low = trapmf(swrad, -50.0, 0.0, 150.0, 350.0)
    rad_med = trimf(swrad, 200.0, 500.0, 800.0)
    rad_high = trapmf(swrad, 650.0, 850.0, 1000.0, 1100.0)

    cloud_low = trapmf(cloud, -5.0, 0.0, 20.0, 40.0)
    cloud_med = trimf(cloud, 25.0, 50.0, 75.0)
    cloud_high = trapmf(cloud, 60.0, 80.0, 100.0, 105.0)

    pr_low = trapmf(precip, -5.0, 0.0, 15.0, 35.0)
    pr_med = trimf(precip, 20.0, 50.0, 80.0)
    pr_high = trapmf(precip, 65.0, 85.0, 100.0, 105.0)

    out_poor = 0.0
    out_moderate = 0.0
    out_favorable = 0.0

    out_favorable = max(out_favorable, min(rad_high, cloud_low, pr_low))
    out_favorable = max(out_favorable, min(rad_high, cloud_med, pr_low))
    out_favorable = max(out_favorable, min(rad_med, cloud_low, pr_low))

    out_moderate = max(out_moderate, min(rad_med, cloud_med, pr_low))
    out_moderate = max(out_moderate, min(rad_med, cloud_low, pr_med))
    out_moderate = max(out_moderate, min(rad_high, cloud_high, pr_low))
    out_moderate = max(out_moderate, min(rad_high, cloud_med, pr_med))
    out_moderate = max(out_moderate, min(rad_low, cloud_low, pr_low))

    out_poor = max(out_poor, rad_low)
    out_poor = max(out_poor, cloud_high)
    out_poor = max(out_poor, pr_high)
    out_poor = max(out_poor, min(cloud_med, pr_med))

    numerator = 0.0
    denominator = 0.0

    for i in range(101):
        x = i / 100.0
        mu_poor = min(out_poor, weather_output_mf(x, 0))
        mu_moderate = min(out_moderate, weather_output_mf(x, 1))
        mu_favorable = min(out_favorable, weather_output_mf(x, 2))
        mu_agg = max(mu_poor, mu_moderate, mu_favorable)
        numerator += x * mu_agg
        denominator += mu_agg

    if denominator <= 0.0001:
        return 0.0
    return numerator / denominator


def mode_mf(x: float, mode: int) -> float:
    if mode == 0:
        return trapmf(x, -0.5, 0.0, 0.35, 0.85)
    if mode == 1:
        return trimf(x, 0.3, 1.0, 1.7)
    if mode == 2:
        return trimf(x, 1.3, 2.0, 2.7)
    if mode == 3:
        return trimf(x, 2.3, 3.0, 3.7)
    if mode == 4:
        return trimf(x, 3.3, 4.0, 4.7)
    if mode == 5:
        return trapmf(x, 4.15, 4.65, 5.0, 5.5)
    return 0.0


def cxx_round_positive(x: float) -> int:
    return int(math.floor(x + 0.5))


def reference_main_fis(
    soc: float,
    pnet: float,
    irradiance: float,
    w_index: float,
    demand: float,
) -> tuple[float, int]:
    soc_critical = trapmf(soc, -5.0, 0.0, 15.0, 25.0)
    soc_low = trimf(soc, 15.0, 30.0, 45.0)
    soc_medium = trimf(soc, 35.0, 55.0, 75.0)
    soc_high = trapmf(soc, 65.0, 80.0, 100.0, 105.0)
    soc_full = trapmf(soc, 85.0, 92.0, 100.0, 105.0)

    p_negative = trapmf(pnet, -400.0, -300.0, -60.0, 0.0)
    p_slight_negative = trapmf(pnet, -180.0, -120.0, -20.0, 20.0)
    p_strong_negative = trapmf(pnet, -450.0, -350.0, -220.0, -120.0)
    p_balanced = trimf(pnet, -80.0, 0.0, 80.0)
    p_positive = trapmf(pnet, 0.0, 60.0, 300.0, 400.0)

    irr_low = trapmf(irradiance, -50.0, 0.0, 150.0, 350.0)
    irr_med = trimf(irradiance, 250.0, 500.0, 750.0)
    irr_high = trapmf(irradiance, 650.0, 850.0, 1000.0, 1100.0)

    w_poor = trapmf(w_index, -0.10, 0.00, 0.20, 0.45)
    w_moderate = trimf(w_index, 0.25, 0.50, 0.75)
    w_favorable = trapmf(w_index, 0.55, 0.80, 1.00, 1.10)

    d_low = trapmf(demand, -0.10, 0.00, 0.20, 0.45)
    d_medium = trimf(demand, 0.25, 0.50, 0.75)
    d_high = trapmf(demand, 0.55, 0.80, 1.00, 1.10)

    energy_ok = max(p_balanced, p_positive)
    solar_ok = max(irr_med, irr_high)
    weather_ok = max(w_moderate, w_favorable)
    demand_active = max(d_medium, d_high)
    battery_service_available = min(soc_high, demand_active)
    high_energy_service = min(
        soc_full,
        min(d_high, max(p_positive, min(p_balanced, solar_ok))),
    )

    out = [0.0] * 6

    out[0] = max(out[0], soc_critical)
    out[0] = max(out[0], min(soc_low, p_negative))
    out[1] = max(out[1], min(soc_low, p_balanced))
    out[1] = max(out[1], min(soc_low, irr_low))
    out[1] = max(out[1], min(soc_high, min(irr_low, w_poor)))

    out[2] = max(out[2], min(soc_medium, p_balanced))
    out[2] = max(out[2], min(soc_medium, min(irr_med, w_moderate)))
    out[2] = max(out[2], min(soc_high, min(energy_ok, min(solar_ok, w_poor))))

    out[3] = max(out[3], min(soc_medium, min(p_positive, min(irr_med, w_moderate))))
    out[3] = max(out[3], min(soc_high, min(p_balanced, min(solar_ok, weather_ok))))
    out[3] = max(out[3], min(soc_high, min(p_positive, min(solar_ok, d_low))))
    out[3] = max(out[3], min(soc_high, min(p_positive, min(solar_ok, demand_active))))
    out[3] = max(out[3], min(soc_full, min(p_positive, demand_active)))
    out[3] = max(out[3], min(battery_service_available, p_balanced))
    out[3] = max(out[3], min(battery_service_available, p_slight_negative))
    out[3] = max(out[3], min(battery_service_available, min(w_poor, irr_low)))

    out[4] = max(out[4], min(soc_full, min(d_high, max(p_balanced, p_slight_negative))))
    out[4] = max(out[4], min(soc_high, min(p_positive, min(irr_high, min(w_favorable, d_medium)))))
    out[4] = max(out[4], min(soc_high, min(p_positive, min(irr_high, min(w_moderate, d_high)))))
    out[4] = max(out[4], min(soc_medium, min(p_positive, min(irr_high, min(w_favorable, d_high)))))
    out[4] = max(out[4], min(soc_full, min(p_positive, min(solar_ok, min(weather_ok, d_high)))))
    out[4] = max(out[4], min(soc_full, min(p_positive, demand_active)))

    out[5] = max(out[5], min(soc_full, min(p_positive, min(irr_high, d_high))))
    out[5] = max(out[5], min(soc_full, min(p_positive, min(solar_ok, d_high))))
    out[5] = max(out[5], min(soc_full, min(energy_ok, min(irr_high, d_high))))
    out[5] = max(out[5], min(high_energy_service, weather_ok))
    out[5] = max(out[5], min(soc_high, min(p_positive, min(irr_high, min(w_favorable, d_high)))))

    out[1] = max(out[1], min(p_strong_negative, max(d_medium, d_high)))
    out[1] = max(out[1], min(soc_low, min(p_negative, max(d_medium, d_high))))
    out[2] = max(out[2], min(w_poor, min(soc_medium, energy_ok)))

    numerator = 0.0
    denominator = 0.0
    for i in range(501):
        x = i / 100.0
        mu_agg = 0.0
        for mode in range(6):
            mu_agg = max(mu_agg, min(out[mode], mode_mf(x, mode)))
        numerator += x * mu_agg
        denominator += mu_agg

    if denominator <= 0.0001:
        return 0.0, 0

    centroid = numerator / denominator
    mode = max(0, min(5, cxx_round_positive(centroid)))
    return centroid, mode


@dataclass(frozen=True)
class WeatherCase:
    name: str
    swrad: float
    cloud: float
    precip: float


@dataclass(frozen=True)
class MainCase:
    name: str
    soc: float
    pnet: float
    irradiance: float
    demand: float
    swrad: float
    cloud: float
    precip: float


def compare_weather(current_module) -> int:
    cases = [
        WeatherCase("clear_high", 900.0, 10.0, 5.0),
        WeatherCase("article_test_event", 760.0, 18.0, 5.0),
        WeatherCase("moderate", 500.0, 50.0, 25.0),
        WeatherCase("cloudy", 650.0, 80.0, 55.0),
        WeatherCase("rain", 700.0, 20.0, 90.0),
        WeatherCase("night", 0.0, 10.0, 5.0),
    ]

    mismatches = 0
    print("\n=== Weather FIS comparison ===")
    print(f"{'case':<22} {'current':>10} {'v9 ref':>10} {'abs diff':>10}")
    for case in cases:
        current = current_module.evaluate_weather_fis(case.swrad, case.cloud, case.precip)
        reference = reference_weather_fis(case.swrad, case.cloud, case.precip)
        diff = abs(current - reference)
        if diff > 1e-4:
            mismatches += 1
        print(f"{case.name:<22} {current:10.4f} {reference:10.4f} {diff:10.4f}")
    return mismatches


def compare_main(current_module) -> int:
    cases = [
        MainCase("article_test_event", 94.0, 178.0, 720.0, 0.72, 760.0, 18.0, 5.0),
        MainCase("critical_soc", 12.0, 100.0, 800.0, 0.80, 850.0, 10.0, 5.0),
        MainCase("low_soc", 22.0, 20.0, 500.0, 0.60, 550.0, 30.0, 10.0),
        MainCase("night_high_soc", 95.0, -5.0, 0.0, 0.10, 0.0, 10.0, 5.0),
        MainCase("balanced_demand", 90.0, 0.0, 500.0, 0.70, 550.0, 30.0, 10.0),
        MainCase("strong_service", 100.0, 250.0, 900.0, 0.90, 900.0, 10.0, 5.0),
        MainCase("battery_service", 95.0, -40.0, 50.0, 0.85, 50.0, 80.0, 20.0),
    ]

    mismatches = 0
    print("\n=== Main FIS comparison ===")
    print(f"{'case':<22} {'cur cent':>10} {'ref cent':>10} {'cur':>6} {'ref':>6}")
    for case in cases:
        w_ref = reference_weather_fis(case.swrad, case.cloud, case.precip)
        current_result = current_module.evaluate_main_fis(
            soc_percent=case.soc,
            p_net_w=case.pnet,
            local_irradiance_wm2=case.irradiance,
            weather_index=w_ref,
            demand_index=case.demand,
        )
        ref_centroid, ref_mode_num = reference_main_fis(
            case.soc,
            case.pnet,
            case.irradiance,
            w_ref,
            case.demand,
        )
        cur_centroid = float(current_result["centroid"])
        cur_mode = str(current_result["fis_mode"])
        ref_mode = f"M{ref_mode_num}"
        if abs(cur_centroid - ref_centroid) > 1e-4 or cur_mode != ref_mode:
            mismatches += 1
        print(
            f"{case.name:<22} {cur_centroid:10.4f} {ref_centroid:10.4f} "
            f"{cur_mode:>6} {ref_mode:>6}"
        )
    return mismatches


def compare_grid(current_module) -> tuple[int, int]:
    soc_values = [10.0, 20.0, 35.0, 55.0, 75.0, 90.0, 100.0]
    pnet_values = [-300.0, -100.0, -20.0, 0.0, 80.0, 200.0]
    irr_values = [0.0, 150.0, 500.0, 850.0]
    weather_values = [0.10, 0.50, 0.85]
    demand_values = [0.10, 0.50, 0.85]

    total = 0
    mode_mismatches = 0
    for soc in soc_values:
        for pnet in pnet_values:
            for irr in irr_values:
                for weather in weather_values:
                    for demand in demand_values:
                        total += 1
                        current_result = current_module.evaluate_main_fis(
                            soc_percent=soc,
                            p_net_w=pnet,
                            local_irradiance_wm2=irr,
                            weather_index=weather,
                            demand_index=demand,
                        )
                        _, ref_mode_num = reference_main_fis(soc, pnet, irr, weather, demand)
                        if current_result["fis_mode"] != f"M{ref_mode_num}":
                            mode_mismatches += 1
    return total, mode_mismatches


def main() -> int:
    current_module = load_current_fis()

    weather_mismatches = compare_weather(current_module)
    main_mismatches = compare_main(current_module)
    total_grid, grid_mismatches = compare_grid(current_module)

    print("\n=== Summary ===")
    print(f"Weather sample mismatches: {weather_mismatches}")
    print(f"Main sample mismatches:    {main_mismatches}")
    print(f"Grid mode mismatches:      {grid_mismatches}/{total_grid}")
    print("\nThis report is expected to show mismatches before the cloud FIS is aligned with v9.")
    print("No AWS resources were accessed or modified.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
