from pathlib import Path
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
# E1 - Closed-loop sequence diagram
# ============================================================

fig, ax = plt.subplots(figsize=(9, 6))

x_esp32 = 0
x_cloud = 1

ax.plot([x_esp32, x_esp32], [0, 5], linewidth=1)
ax.plot([x_cloud, x_cloud], [0, 5], linewidth=1)

ax.text(x_esp32, 5.25, "ESP32", ha="center", fontsize=13)
ax.text(x_cloud, 5.25, "Cloud system", ha="center", fontsize=13)

events = [
    (4.4, x_esp32, x_cloud,
     "Telemetry: M4, 2 outputs\n23:26:29 UTC"),
    (3.3, x_cloud, x_esp32,
     "LOCKOUT command\n23:26:33.858 UTC"),
    (2.2, x_esp32, x_cloud,
     "ACK: accepted, applied=true\n23:26:34 UTC"),
    (1.1, x_esp32, x_cloud,
     "Telemetry: M0, 0 outputs\n23:26:44 UTC"),
]

for y, x1, x2, label in events:
    ax.annotate(
        "",
        xy=(x2, y),
        xytext=(x1, y),
        arrowprops=dict(arrowstyle="->", linewidth=1.5),
    )

    ax.text(
        0.5,
        y + 0.14,
        label,
        ha="center",
        va="bottom",
        fontsize=10,
    )

ax.set_xlim(-0.35, 1.35)
ax.set_ylim(0.5, 5.6)
ax.axis("off")

ax.set_title(
    "End-to-end cloud-station closed-loop validation",
    fontsize=14,
)

fig.tight_layout()

fig.savefig(
    OUTPUT_DIR / "e1_closed_loop_sequence.png",
    dpi=300,
    bbox_inches="tight",
)

plt.close(fig)


# ============================================================
# E2 - Operating mode versus SOC
# ============================================================

soc_values = np.linspace(0, 100, 101)

pnet_cases = [
    (-100.0, r"$P_{net}=-100$ W"),
    (0.0, r"$P_{net}=0$ W"),
    (150.0, r"$P_{net}=150$ W"),
]


def make_mode_plot(
    filename,
    irradiance,
    weather_index,
    demand_index,
    title,
):

    fig, ax = plt.subplots(figsize=(9, 5.5))

    for pnet, label in pnet_cases:

        modes = []

        for soc in soc_values:

            result = fis.evaluate_main_fis(
                soc_percent=float(soc),
                p_net_w=pnet,
                local_irradiance_wm2=irradiance,
                weather_index=weather_index,
                demand_index=demand_index,
            )

            modes.append(
                int(result["fis_mode"][1])
            )

        ax.step(
            soc_values,
            modes,
            where="post",
            linewidth=2,
            label=label,
        )

    ax.set_xlabel(
        "Battery state of charge (%)"
    )

    ax.set_ylabel(
        "FIS operating mode"
    )

    ax.set_yticks(
        range(6),
        ["M0", "M1", "M2", "M3", "M4", "M5"],
    )

    ax.set_xlim(0, 100)
    ax.set_ylim(-0.25, 5.25)

    ax.grid(
        True,
        alpha=0.25,
    )

    ax.legend(
        title="Net-power condition",
    )

    ax.set_title(title)

    fig.tight_layout()

    fig.savefig(
        OUTPUT_DIR / filename,
        dpi=300,
        bbox_inches="tight",
    )

    plt.close(fig)


make_mode_plot(
    filename="e2_mode_vs_soc_favorable.png",
    irradiance=850.0,
    weather_index=0.85,
    demand_index=0.85,
    title=(
        "Main FIS response under favorable conditions\n"
        r"$G=850$ W/m², $WI=0.85$, $DI=0.85$"
    ),
)


make_mode_plot(
    filename="e2_mode_vs_soc_unfavorable.png",
    irradiance=150.0,
    weather_index=0.17,
    demand_index=0.85,
    title=(
        "Main FIS response under unfavorable solar conditions\n"
        r"$G=150$ W/m², $WI=0.17$, $DI=0.85$"
    ),
)


print()
print("New thesis figures generated:")
print(
    OUTPUT_DIR / "e1_closed_loop_sequence.png"
)
print(
    OUTPUT_DIR / "e2_mode_vs_soc_favorable.png"
)
print(
    OUTPUT_DIR / "e2_mode_vs_soc_unfavorable.png"
)
