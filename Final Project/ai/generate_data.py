#!/usr/bin/env python3
"""
generate_data.py
================
Synthetic data generator for the Smart Edge-AI EV Charging Station Optimizer.

Produces two CSV datasets used to train the two Edge-AI models:

  1) arrival_probability  ->  "will at least one EV plug in during the next 15 min?"
     Features : hour_sin, hour_cos, weekday, is_weekend, is_peak, busy_rate
     Label    : arrival (0/1)

  2) charging_duration    ->  "how many minutes does this charging session take?"
     Features : capacity_kwh, soc0_pct, socT_pct, ambient_c, current_a, connector_temp_c
     Label    : duration_min

The physical model behind the duration dataset uses the same CC/CV taper behaviour
that the ESP32 firmware implements, so the trained model matches on-device behaviour.

Usage:
    python ai/generate_data.py            # writes ai/data/*.csv
"""
from __future__ import annotations

import argparse
import math
import os
from pathlib import Path

import numpy as np
import pandas as pd

RNG_SEED = 42
DATA_DIR = Path(__file__).resolve().parent / "data"


# --------------------------------------------------------------------------- #
# Arrival dataset
# --------------------------------------------------------------------------- #
def time_of_day_features(hour: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Map the hour of day onto a periodic (sin/cos) basis, scaled to [-1, 1]."""
    rad = 2.0 * math.pi * hour / 24.0
    return np.sin(rad), np.cos(rad)


def base_arrival_rate(hour: np.ndarray, weekday: np.ndarray, busy_rate: np.ndarray) -> np.ndarray:
    """
    Shape of a typical urban charging station arrival curve (probability that an
    EV arrives during the next 15 minutes), before the busy-rate multiplier.
    """
    rate = np.full_like(hour, 0.06, dtype=float)                 # base ~6% / 15 min
    # Morning commute peak 07:00 - 09:00
    morning = ((hour >= 6.5) & (hour <= 9.5)).astype(float)
    # Evening peak 16:00 - 19:30
    evening = ((hour >= 16.0) & (hour <= 19.5)).astype(float)
    peak = np.clip(morning + evening, 0.0, 1.0)
    rate += peak * 0.22
    # Midday is a bit quieter, nights very quiet
    night = ((hour >= 23.0) | (hour <= 5.0)).astype(float)
    rate *= (1.0 - night * 0.75)
    # Weekends shift the peaks later / flatter
    weekend = (weekday >= 5).astype(float)
    rate *= (1.0 + weekend * 0.25)
    # User-configurable busy multiplier from the dashboard / shared attributes
    rate *= busy_rate
    return np.clip(rate, 0.005, 0.75)


def generate_arrivals(n: int = 80_000) -> pd.DataFrame:
    rng = np.random.default_rng(RNG_SEED + 1)
    hour = rng.uniform(0.0, 24.0, n)
    weekday = rng.integers(0, 7, n)
    busy_rate = rng.uniform(0.5, 1.5, n)
    hour_sin, hour_cos = time_of_day_features(hour)
    is_weekend = (weekday >= 5).astype(int)
    is_peak = np.where(((6.5 <= hour) & (hour <= 9.5)) | ((16.0 <= hour) & (hour <= 19.5)), 1, 0)

    prob = base_arrival_rate(hour, weekday, busy_rate)
    label = (rng.random(n) < prob).astype(int)

    df = pd.DataFrame(
        {
            "hour_sin": hour_sin.round(5),
            "hour_cos": hour_cos.round(5),
            "weekday": weekday,
            "is_weekend": is_weekend,
            "is_peak": is_peak,
            "busy_rate": busy_rate.round(4),
            "arrival": label,
        }
    )
    return df


# --------------------------------------------------------------------------- #
# Duration dataset
# --------------------------------------------------------------------------- #
def charge_duration_min(
    capacity_kwh: np.ndarray,
    soc0: np.ndarray,
    socT: np.ndarray,
    ambient_c: np.ndarray,
    current_a: np.ndarray,
    connector_temp_c: np.ndarray,
    rng: np.random.Generator,
) -> np.ndarray:
    """
    Physics-inspired CC/CV charging model (also used by the ESP32 simulator):

      * constant-current phase up to ~80 % SoC,
      * linear current taper 80 % -> 100 % (CV-like behaviour),
      * thermal derating when the connector gets hot,
      * small efficiency / noise terms.

    Returns duration in minutes.
    """
    v_ac = 230.0
    pf = 0.98
    eta = 0.92  # charger efficiency

    p_kw = (v_ac * current_a * pf / 1000.0) * eta
    e_needed_kwh = capacity_kwh * (socT - soc0) / 100.0

    # CC phase: 80 % - soc0
    cc_frac = np.clip((80.0 - soc0) / (socT - soc0), 0.0, 1.0)
    # CV phase: socT - 80 %
    cv_frac = 1.0 - cc_frac

    minutes_cc = e_needed_kwh * cc_frac / p_kw * 60.0

    # Taper: average current during CV phase is ~45 % of nominal
    minutes_cv = e_needed_kwh * cv_frac / (p_kw * 0.45) * 60.0
    duration = minutes_cc + minutes_cv

    # Thermal derating above 65 C connector temperature
    hot = np.clip(connector_temp_c - 65.0, 0.0, None)
    duration *= 1.0 + hot * 0.02

    # Cold batteries accept slightly less power
    cold = np.clip(12.0 - ambient_c, 0.0, None)
    duration *= 1.0 + cold * 0.01

    # Charger efficiency variance + measurement noise
    duration *= rng.normal(1.0, 0.06, len(duration))
    return np.clip(np.round(duration, 1), 5.0, 720.0)


def generate_durations(n: int = 60_000) -> pd.DataFrame:
    rng = np.random.default_rng(RNG_SEED + 2)
    capacity_kwh = rng.uniform(24.0, 100.0, n)
    soc0 = rng.uniform(5.0, 80.0, n)
    socT = rng.uniform(80.0, 100.0, n)
    # target must be above initial
    socT = np.where(socT > soc0, socT, np.clip(soc0 + rng.uniform(5, 40, n), 80, 100))
    ambient_c = rng.uniform(-5.0, 45.0, n)
    current_a = rng.uniform(6.0, 32.0, n)
    connector_temp_c = np.clip(ambient_c + rng.normal(25.0, 8.0, n), 5.0, 95.0)

    duration_min = charge_duration_min(
        capacity_kwh, soc0, socT, ambient_c, current_a, connector_temp_c, rng
    )

    df = pd.DataFrame(
        {
            "capacity_kwh": capacity_kwh.round(1),
            "soc0_pct": soc0.round(1),
            "socT_pct": socT.round(1),
            "ambient_c": ambient_c.round(1),
            "current_a": current_a.round(1),
            "connector_temp_c": connector_temp_c.round(1),
            "duration_min": duration_min,
        }
    )
    return df


# --------------------------------------------------------------------------- #
def main() -> None:
    parser = argparse.ArgumentParser(description="Generate the synthetic datasets")
    parser.add_argument("--n-arrivals", type=int, default=80_000)
    parser.add_argument("--n-durations", type=int, default=60_000)
    parser.add_argument("--out", type=Path, default=DATA_DIR)
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    print("Generating arrival dataset ...")
    arrivals = generate_arrivals(args.n_arrivals)
    arrivals.to_csv(args.out / "arrivals.csv", index=False)
    print(f"  -> {args.out / 'arrivals.csv'}  ({len(arrivals):,} rows)")

    print("Generating duration dataset ...")
    durations = generate_durations(args.n_durations)
    durations.to_csv(args.out / "durations.csv", index=False)
    print(f"  -> {args.out / 'durations.csv'}  ({len(durations):,} rows)")

    print("Done.")


if __name__ == "__main__":
    main()