#!/usr/bin/env python3
"""
edge_ai.py
==========
Python-side Edge-AI inference module.

Mirrors the exact inference code that runs on the ESP32 firmware so that the
Python live demo / dashboard backend and the device produce identical results.

Two models:
  * ArrivalProbability — LogisticRegression -> sigmoid(w . x + b)
  * DurationPredictor — DecisionTreeRegressor encoded as parallel C-style arrays

Usage:
    from ai.edge_ai import EdgeAIModels
    ai = EdgeAIModels()                       # loads exported artifacts
    p  = ai.arrival_probability(hour=17, weekday=2, busy_rate=1.2)
    t  = ai.charge_minutes(capacity=60, soc0=30, socT=90, ambient=22, current=32, conn_temp=45)
"""
from __future__ import annotations

import math
from pathlib import Path

import joblib
import numpy as np

MODEL_DIR = Path(__file__).resolve().parent / "models"

ARRIVAL_FEATURES = ["hour_sin", "hour_cos", "weekday", "is_weekend", "is_peak", "busy_rate"]
DURATION_FEATURES = [
    "capacity_kwh", "soc0_pct", "socT_pct", "ambient_c", "current_a", "connector_temp_c",
]


def _sigmoid(x: float) -> float:
    if x >= 0:
        z = math.exp(-x)
        return 1.0 / (1.0 + z)
    z = math.exp(x)
    return z / (1.0 + z)


# --------------------------------------------------------------------------- #
class ArrivalPredictor:
    """Logistic regression. Features must be built exactly as on the ESP32."""

    def __init__(self, export: dict):
        self.coef = np.asarray(export["coefficients"], dtype=float)
        self.bias = float(export["intercept"])

    @staticmethod
    def build_features(hour: float, weekday: int, busy_rate: float = 1.0) -> np.ndarray:
        rad = 2.0 * math.pi * hour / 24.0
        is_weekend = 1 if weekday >= 5 else 0
        is_peak = 1 if (6.5 <= hour <= 9.5) or (16.0 <= hour <= 19.5) else 0
        return np.array(
            [math.sin(rad), math.cos(rad), float(weekday % 7), float(is_weekend),
             float(is_peak), float(busy_rate)],
            dtype=float,
        )

    def predict_proba(self, hour: float, weekday: int, busy_rate: float = 1.0) -> float:
        x = self.build_features(hour, weekday, busy_rate)
        return _sigmoid(float(np.dot(self.coef, x) + self.bias))


# --------------------------------------------------------------------------- #
class DurationPredictorC:
    """Decision-tree regressor walked directly from the exported C arrays."""

    def __init__(self, export: dict):
        self.feature = np.asarray(export["feature"], dtype=int)
        self.threshold = np.asarray(export["threshold"], dtype=float)
        self.left = np.asarray(export["children_left"], dtype=int)
        self.right = np.asarray(export["children_right"], dtype=int)
        self.value = np.asarray(export["value"], dtype=float)

    def predict(self, x: np.ndarray) -> float:
        node = 0
        while self.feature[node] >= 0:
            f = self.feature[node]
            if x[f] <= self.threshold[node]:
                node = self.left[node]
            else:
                node = self.right[node]
        return float(self.value[node])


# --------------------------------------------------------------------------- #
class EdgeAIModels:
    """Convenience facade matching the firmware API."""

    def __init__(self, model_dir: Path = MODEL_DIR):
        arrival_export = joblib.load(model_dir / "arrival_export.joblib")
        duration_export = joblib.load(model_dir / "duration_export.joblib")
        self.arrival = ArrivalPredictor(arrival_export)
        self.duration = DurationPredictorC(duration_export)

    def arrival_probability(self, hour: float, weekday: int, busy_rate: float = 1.0) -> float:
        return self.arrival.predict_proba(hour, weekday, busy_rate)

    def charge_minutes(
        self,
        capacity_kwh: float,
        soc0: float,
        socT: float,
        ambient_c: float,
        current_a: float,
        conn_temp_c: float,
    ) -> float:
        x = np.array(
            [capacity_kwh, soc0, socT, ambient_c, current_a, conn_temp_c], dtype=float
        )
        return self.duration.predict(x)


if __name__ == "__main__":
    ai = EdgeAIModels()
    print("Arrival probability @ Tue 17:30, busy_rate 1.2 : "
          f"{ai.arrival_probability(17.5, 1, 1.2):.3f}")
    print("Duration for 60 kWh, 30->90 %, 32 A, 22 C      : "
          f"{ai.charge_minutes(60, 30, 90, 22, 32, 45):.1f} min")
    print("Duration for 60 kWh, 30->90 %, 32 A, 40 C      : "
          f"{ai.charge_minutes(60, 30, 90, 40, 32, 70):.1f} min")