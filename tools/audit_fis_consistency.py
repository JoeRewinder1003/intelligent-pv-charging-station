from pathlib import Path
import importlib.util
import math
import os
import sys

PROJECT_ROOT = Path(__file__).resolve().parents[1]
FIS_PATH = PROJECT_ROOT / "aws" / "lambdas" / "fis_processor" / "lambda_function.py"

os.environ.setdefault("AWS_DEFAULT_REGION", "us-east-2")
os.environ.setdefault("AWS_EC2_METADATA_DISABLED", "true")


def load_fis():
    spec = importlib.util.spec_from_file_location("fis_module", FIS_PATH)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


fis = load_fis()
trimf = fis.trimf
trapmf = fis.trapmf


def weather_activations(radiation, cloud, precip):
    rad_low = trapmf(radiation, -50.0, 0.0, 150.0, 350.0)
    rad_med = trimf(radiation, 200.0, 500.0, 800.0)
    rad_high = trapmf(radiation, 650.0, 850.0, 1000.0, 1250.0)

    cloud_low = trapmf(cloud, -5.0, 0.0, 20.0, 40.0)
    cloud_med = trimf(cloud, 25.0, 50.0, 75.0)
    cloud_high = trapmf(cloud, 60.0, 80.0, 100.0, 105.0)

    precip_low = trapmf(precip, -5.0, 0.0, 15.0, 35.0)
    precip_med = trimf(precip, 20.0, 50.0, 80.0)
    precip_high = trapmf(precip, 65.0, 85.0, 100.0, 105.0)

    favorable = 0.0
    moderate = 0.0
    poor = 0.0

    favorable = max(favorable, min(rad_high, cloud_low, precip_low))
    favorable = max(favorable, min(rad_high, cloud_med, precip_low))
    favorable = max(favorable, min(rad_med, cloud_low, precip_low))

    moderate = max(moderate, min(rad_med, cloud_med, precip_low))
    moderate = max(moderate, min(rad_med, cloud_low, precip_med))
    moderate = max(moderate, min(rad_high, cloud_high, precip_low))
    moderate = max(moderate, min(rad_high, cloud_med, precip_med))
    moderate = max(moderate, min(rad_high, cloud_low, precip_med))
    moderate = max(moderate, min(rad_low, cloud_low, precip_low))
    

    poor = max(poor, rad_low)
    poor = max(poor, cloud_high)
    poor = max(poor, precip_high)
    poor = max(poor, min(cloud_med, precip_med))

    return poor, moderate, favorable


def main_activations(soc, pnet, irradiance, weather, demand):
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

    w_poor = trapmf(weather, -0.10, 0.00, 0.20, 0.45)
    w_moderate = trimf(weather, 0.25, 0.50, 0.75)
    w_favorable = trapmf(weather, 0.55, 0.80, 1.00, 1.10)

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
    out[2] = max(
        out[2],
        min(soc_high, min(energy_ok, min(solar_ok, w_poor))),
    )

    out[3] = max(
        out[3],
        min(soc_medium, min(p_positive, min(irr_med, w_moderate))),
    )
    out[3] = max(
        out[3],
        min(soc_high, min(p_balanced, min(solar_ok, weather_ok))),
    )
    out[3] = max(
        out[3],
        min(soc_high, min(p_positive, min(solar_ok, d_low))),
    )
    out[3] = max(
        out[3],
        min(soc_high, min(p_positive, min(solar_ok, demand_active))),
    )
    out[3] = max(
        out[3],
        min(soc_full, min(p_positive, demand_active)),
    )
    out[3] = max(out[3], min(battery_service_available, p_balanced))
    out[3] = max(out[3], min(battery_service_available, p_slight_negative))
    out[3] = max(
        out[3],
        min(battery_service_available, min(w_poor, irr_low)),
    )

    out[4] = max(
        out[4],
        min(soc_full, min(d_high, max(p_balanced, p_slight_negative))),
    )
    out[4] = max(
        out[4],
        min(soc_high, min(p_positive, min(irr_high, min(w_favorable, d_medium)))),
    )
    out[4] = max(
        out[4],
        min(soc_high, min(p_positive, min(irr_high, min(w_moderate, d_high)))),
    )
    out[4] = max(
        out[4],
        min(soc_medium, min(p_positive, min(irr_high, min(w_favorable, d_high)))),
    )
    out[4] = max(
        out[4],
        min(soc_full, min(p_positive, min(solar_ok, min(weather_ok, d_high)))),
    )
    out[4] = max(
        out[4],
        min(soc_full, min(p_positive, demand_active)),
    )

    out[5] = max(
        out[5],
        min(soc_full, min(p_positive, min(irr_high, d_high))),
    )
    out[5] = max(
        out[5],
        min(soc_full, min(p_positive, min(solar_ok, d_high))),
    )
    out[5] = max(
        out[5],
        min(soc_full, min(energy_ok, min(irr_high, d_high))),
    )
    out[5] = max(out[5], min(high_energy_service, weather_ok))
    out[5] = max(
        out[5],
        min(soc_high, min(p_positive, min(irr_high, min(w_favorable, d_high)))),
    )

    out[1] = max(
        out[1],
        min(p_strong_negative, max(d_medium, d_high)),
    )
    out[1] = max(
        out[1],
        min(soc_low, min(p_negative, max(d_medium, d_high))),
    )
    out[2] = max(
        out[2],
        min(w_poor, min(soc_medium, energy_ok)),
    )

    return out


def audit_weather():
    radiation_values = range(0, 1201, 50)
    cloud_values = range(0, 101, 5)
    precip_values = range(0, 101, 5)

    total = 0
    uncovered = []
    min_wi = float("inf")
    max_wi = float("-inf")

    for r in radiation_values:
        for c in cloud_values:
            for p in precip_values:
                total += 1

                activations = weather_activations(r, c, p)

                if max(activations) <= 1e-9:
                    uncovered.append((r, c, p))

                wi = fis.evaluate_weather_fis(r, c, p)

                min_wi = min(min_wi, wi)
                max_wi = max(max_wi, wi)

    print("\n=== WEATHER FIS AUDIT ===")
    print(f"Grid combinations:        {total}")
    print(f"Uncovered combinations:   {len(uncovered)}")
    print(f"Uncovered percentage:     {100 * len(uncovered) / total:.2f}%")
    print(f"Weather Index minimum:    {min_wi:.4f}")
    print(f"Weather Index maximum:    {max_wi:.4f}")

    print("\nFirst uncovered examples:")
    for case in uncovered[:20]:
        print(
            f"  radiation={case[0]:4d} W/m2, "
            f"cloud={case[1]:3d} %, precip={case[2]:3d} %"
        )

    target = (900, 10, 50)
    print("\nSpecific suspected gap:")
    print(
        f"  {target}: activation={weather_activations(*target)}, "
        f"WI={fis.evaluate_weather_fis(*target):.4f}"
    )


def audit_main():
    soc_values = range(0, 101, 5)
    pnet_values = range(-300, 301, 50)
    irradiance_values = range(0, 1001, 100)
    weather_values = [0.1, 0.3, 0.5, 0.7, 0.9]
    demand_values = [0.1, 0.3, 0.5, 0.7, 0.9]

    total = 0
    uncovered = []
    modes = set()

    min_centroid = float("inf")
    max_centroid = float("-inf")
    max_case = None

    for soc in soc_values:
        for pnet in pnet_values:
            for irr in irradiance_values:
                for weather in weather_values:
                    for demand in demand_values:
                        total += 1

                        activations = main_activations(
                            soc, pnet, irr, weather, demand
                        )

                        if max(activations) <= 1e-9:
                            uncovered.append(
                                (soc, pnet, irr, weather, demand)
                            )

                        result = fis.evaluate_main_fis(
                            soc, pnet, irr, weather, demand
                        )

                        centroid = result["centroid"]
                        mode = result["fis_mode"]

                        modes.add(mode)

                        if centroid < min_centroid:
                            min_centroid = centroid

                        if centroid > max_centroid:
                            max_centroid = centroid
                            max_case = (
                                soc,
                                pnet,
                                irr,
                                weather,
                                demand,
                                mode,
                            )

    print("\n=== MAIN FIS AUDIT ===")
    print(f"Grid combinations:        {total}")
    print(f"Uncovered combinations:   {len(uncovered)}")
    print(f"Uncovered percentage:     {100 * len(uncovered) / total:.2f}%")
    print(f"Reachable modes:          {sorted(modes)}")
    print(f"Minimum centroid:         {min_centroid:.4f}")
    print(f"Maximum centroid:         {max_centroid:.4f}")
    print(f"Case with max centroid:   {max_case}")

    print("\nFirst uncovered examples:")
    for case in uncovered[:20]:
        print(
            f"  SOC={case[0]:3d} %, "
            f"Pnet={case[1]:4d} W, "
            f"Irr={case[2]:4d} W/m2, "
            f"WI={case[3]:.1f}, "
            f"DI={case[4]:.1f}"
        )


def audit_monotonicity():
    print("\n=== BASIC MONOTONICITY AUDIT ===")

    violations_soc = []
    violations_pnet = []

    irradiances = [200, 500, 900]
    weather_values = [0.2, 0.5, 0.9]
    demand_values = [0.2, 0.5, 0.9]

    # Increasing SOC should not normally produce a lower FIS mode
    # when all other inputs are fixed.
    for pnet in [-150, 0, 150, 300]:
        for irr in irradiances:
            for weather in weather_values:
                for demand in demand_values:

                    previous_mode = None

                    for soc in range(0, 101, 5):
                        result = fis.evaluate_main_fis(
                            soc, pnet, irr, weather, demand
                        )
                        mode = int(result["fis_mode"][1])

                        if previous_mode is not None and mode < previous_mode:
                            violations_soc.append(
                                (
                                    soc - 5,
                                    soc,
                                    pnet,
                                    irr,
                                    weather,
                                    demand,
                                    previous_mode,
                                    mode,
                                )
                            )

                        previous_mode = mode

    # Increasing Pnet should not normally produce a lower FIS mode
    # under otherwise identical conditions.
    for soc in [25, 40, 55, 70, 90, 100]:
        for irr in irradiances:
            for weather in weather_values:
                for demand in demand_values:

                    previous_mode = None

                    for pnet in range(-300, 301, 25):
                        result = fis.evaluate_main_fis(
                            soc, pnet, irr, weather, demand
                        )
                        mode = int(result["fis_mode"][1])

                        if previous_mode is not None and mode < previous_mode:
                            violations_pnet.append(
                                (
                                    pnet - 25,
                                    pnet,
                                    soc,
                                    irr,
                                    weather,
                                    demand,
                                    previous_mode,
                                    mode,
                                )
                            )

                        previous_mode = mode

    print(f"SOC monotonicity violations:   {len(violations_soc)}")
    print(f"Pnet monotonicity violations:  {len(violations_pnet)}")

    print("\nFirst SOC violations:")
    for v in violations_soc[:10]:
        print(v)

    print("\nFirst Pnet violations:")
    for v in violations_pnet[:10]:
        print(v)


def inspect_main_cases():
    print("\n=== MAIN FIS ACTIVATION INSPECTION ===")

    cases = [
        (
            "strong_service",
            100.0, 250.0, 900.0, 0.9, 0.9
        ),
        (
            "full_good_conditions",
            95.0, 200.0, 900.0, 0.9, 0.9
        ),
        (
            "high_soc_good_conditions",
            80.0, 200.0, 900.0, 0.9, 0.9
        ),
        (
            "reported_maximum",
            40.0, 100.0, 700.0, 0.9, 0.7
        ),
        (
            "uncovered_example",
            25.0, 100.0, 400.0, 0.5, 0.5
        ),
    ]

    for name, soc, pnet, irr, weather, demand in cases:
        activations = main_activations(
            soc,
            pnet,
            irr,
            weather,
            demand,
        )

        result = fis.evaluate_main_fis(
            soc,
            pnet,
            irr,
            weather,
            demand,
        )

        print(f"\n{name}")
        print(
            f"  SOC={soc:.1f}, "
            f"Pnet={pnet:.1f}, "
            f"Irr={irr:.1f}, "
            f"WI={weather:.2f}, "
            f"DI={demand:.2f}"
        )

        for i, activation in enumerate(activations):
            print(f"  M{i} activation = {activation:.4f}")

        print(f"  centroid = {result['centroid']:.4f}")
        print(f"  mode     = {result['fis_mode']}")

if __name__ == "__main__":
    audit_weather()
    audit_main()
    audit_monotonicity()
    inspect_main_cases()