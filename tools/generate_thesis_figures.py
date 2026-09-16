from pathlib import Path
from datetime import datetime
import csv
import importlib.util
import os
import sys

import matplotlib.pyplot as plt
import numpy as np


PROJECT_ROOT = Path(__file__).resolve().parents[1]
OUTPUT_DIR = PROJECT_ROOT / "results" / "thesis_figures"
OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

FIS_PATH = (
    PROJECT_ROOT
    / "aws"
    / "lambdas"
    / "fis_processor"
    / "lambda_function.py"
)

os.environ.setdefault("AWS_DEFAULT_REGION", "us-east-2")
os.environ.setdefault("AWS_EC2_METADATA_DISABLED", "true")


def load_fis():
    spec = importlib.util.spec_from_file_location(
        "current_fis",
        FIS_PATH,
    )
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


fis = load_fis()


# ============================================================
# E1 — End-to-end closed-loop timeline
# ============================================================

events = [
    (
        "2026-08-24T23:26:29Z",
        "ESP32 telemetry\nM4, 2 outputs",
    ),
    (
        "2026-08-24T23:26:33.857919Z",
        "Cloud reconciliation\nLOCKOUT",
    ),
    (
        "2026-08-24T23:26:34Z",
        "ESP32 ACK\naccepted, applied=true",
    ),
    (
        "2026-08-24T23:26:44Z",
        "ESP32 telemetry\nM0, 0 outputs",
    ),
]

times = [
    datetime.fromisoformat(
        timestamp.replace("Z", "+00:00")
    )
    for timestamp, _ in events
]

t0 = times[0]

elapsed = np.array([
    (time - t0).total_seconds()
    for time in times
])

labels = [label for _, label in events]

with open(
    OUTPUT_DIR / "e1_closed_loop_events.csv",
    "w",
    newline="",
    encoding="utf-8",
) as file:
    writer = csv.writer(file)
    writer.writerow([
        "timestamp_utc",
        "elapsed_s",
        "event",
    ])

    for (timestamp, label), seconds in zip(
        events,
        elapsed,
    ):
        writer.writerow([
            timestamp,
            seconds,
            label.replace("\n", " - "),
        ])


fig, ax = plt.subplots(figsize=(10, 4.5))

ax.axhline(0, linewidth=1)
ax.scatter(
    elapsed,
    np.zeros_like(elapsed),
    s=70,
    zorder=3,
)

annotation_y = [0.42, -0.48, 0.55, -0.55]

for x, label, y in zip(
    elapsed,
    labels,
    annotation_y,
):
    ax.vlines(
        x,
        0,
        y * 0.72,
        linewidth=1,
    )

    ax.annotate(
        label,
        xy=(x, 0),
        xytext=(x, y),
        ha="center",
        va="center",
        fontsize=10,
    )

ax.set_xlabel(
    "Elapsed time from initial telemetry (s)"
)
ax.set_title(
    "End-to-end cloud–station mode reconciliation"
)

ax.set_yticks([])
ax.set_ylim(-0.8, 0.8)
ax.set_xlim(-1, elapsed[-1] + 1)

fig.tight_layout()

fig.savefig(
    OUTPUT_DIR / "e1_closed_loop_timeline.png",
    dpi=300,
    bbox_inches="tight",
)

plt.close(fig)


# ============================================================
# E2 — Main FIS decision maps
# ============================================================

soc_values = np.linspace(0, 100, 101)
pnet_values = np.linspace(-350, 300, 131)


def generate_fis_map(
    filename,
    csv_filename,
    irradiance,
    weather_index,
    demand_index,
    title,
):
    mode_grid = np.zeros(
        (
            len(soc_values),
            len(pnet_values),
        )
    )

    with open(
        OUTPUT_DIR / csv_filename,
        "w",
        newline="",
        encoding="utf-8",
    ) as file:

        writer = csv.writer(file)

        writer.writerow([
            "soc_percent",
            "p_net_w",
            "irradiance_wm2",
            "weather_index",
            "demand_index",
            "centroid",
            "fis_mode",
        ])

        for i, soc in enumerate(soc_values):
            for j, pnet in enumerate(pnet_values):

                result = fis.evaluate_main_fis(
                    soc_percent=float(soc),
                    p_net_w=float(pnet),
                    local_irradiance_wm2=float(
                        irradiance
                    ),
                    weather_index=float(
                        weather_index
                    ),
                    demand_index=float(
                        demand_index
                    ),
                )

                mode_number = int(
                    result["fis_mode"][1]
                )

                mode_grid[i, j] = mode_number

                writer.writerow([
                    round(float(soc), 3),
                    round(float(pnet), 3),
                    irradiance,
                    weather_index,
                    demand_index,
                    round(
                        float(result["centroid"]),
                        4,
                    ),
                    result["fis_mode"],
                ])

    fig, ax = plt.subplots(
        figsize=(9, 6)
    )

    image = ax.imshow(
        mode_grid,
        origin="lower",
        aspect="auto",
        extent=[
            pnet_values.min(),
            pnet_values.max(),
            soc_values.min(),
            soc_values.max(),
        ],
        interpolation="nearest",
        vmin=-0.5,
        vmax=5.5,
    )

    colorbar = fig.colorbar(
        image,
        ax=ax,
        ticks=range(6),
    )

    colorbar.ax.set_yticklabels([
        "M0",
        "M1",
        "M2",
        "M3",
        "M4",
        "M5",
    ])

    colorbar.set_label(
        "FIS operating mode"
    )

    ax.set_xlabel(
        r"Net power, $P_{net}$ (W)"
    )

    ax.set_ylabel(
        "Battery state of charge (%)"
    )

    ax.set_title(title)

    fig.tight_layout()

    fig.savefig(
        OUTPUT_DIR / filename,
        dpi=300,
        bbox_inches="tight",
    )

    plt.close(fig)


generate_fis_map(
    filename="e2_fis_map_favorable.png",
    csv_filename="e2_fis_map_favorable.csv",
    irradiance=850.0,
    weather_index=0.85,
    demand_index=0.85,
    title=(
        "Main FIS decision map — favorable conditions\n"
        r"$G=850$ W/m², $WI=0.85$, $DI=0.85$"
    ),
)


generate_fis_map(
    filename="e2_fis_map_poor.png",
    csv_filename="e2_fis_map_poor.csv",
    irradiance=150.0,
    weather_index=0.17,
    demand_index=0.85,
    title=(
        "Main FIS decision map — unfavorable solar conditions\n"
        r"$G=150$ W/m², $WI=0.17$, $DI=0.85$"
    ),
)


print()
print("Figures generated successfully:")
print(
    OUTPUT_DIR
    / "e1_closed_loop_timeline.png"
)
print(
    OUTPUT_DIR
    / "e2_fis_map_favorable.png"
)
print(
    OUTPUT_DIR
    / "e2_fis_map_poor.png"
)
print()
print(
    "CSV data were also saved in the same folder."
)

