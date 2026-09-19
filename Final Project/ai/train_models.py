#!/usr/bin/env python3
"""
train_models.py
===============
Trains the two Edge-AI models with scikit-learn and exports them:

  * arrival_probability  : LogisticRegression  ->  weights + bias array
  * charging_duration    : DecisionTreeRegressor  ->  node arrays (feature/threshold/children/value)

Exports:
  ai/models/models.h     : C header consumed by the ESP32 firmware (also copied to
                           firmware/smart_ev_charger/models.h)
  ai/models/*.joblib     : pickled models for the Python simulator / live demo
  ai/models/metrics.json : evaluation metrics for the handbook / viva
  ai/models/plots/*.png  : diagnostic plots

Usage:
    python ai/train_models.py
"""
from __future__ import annotations

import json
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

import joblib
import numpy as np
import pandas as pd
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import accuracy_score, confusion_matrix, mean_absolute_error, r2_score, roc_auc_score
from sklearn.model_selection import train_test_split
from sklearn.tree import DecisionTreeRegressor, export_text

ROOT = Path(__file__).resolve().parent.parent
DATA_DIR = ROOT / "ai" / "data"
MODEL_DIR = ROOT / "ai" / "models"
PLOTS_DIR = MODEL_DIR / "plots"
FIRMWARE_DIR = ROOT / "firmware" / "smart_ev_charger"

ARRIVAL_FEATURES = ["hour_sin", "hour_cos", "weekday", "is_weekend", "is_peak", "busy_rate"]
DURATION_FEATURES = [
    "capacity_kwh", "soc0_pct", "socT_pct", "ambient_c", "current_a", "connector_temp_c",
]


# --------------------------------------------------------------------------- #
# 1) Arrival probability  (LogisticRegression)
# --------------------------------------------------------------------------- #
def train_arrival_model(df: pd.DataFrame) -> dict:
    X = df[ARRIVAL_FEATURES].to_numpy(float)
    y = df["arrival"].to_numpy(int)
    Xtr, Xte, ytr, yte = train_test_split(X, y, test_size=0.2, random_state=7, stratify=y)

    model = LogisticRegression(C=1.0, max_iter=2000, random_state=7)
    model.fit(Xtr, ytr)

    y_prob = model.predict_proba(Xte)[:, 1]
    y_pred = (y_prob >= 0.5).astype(int)
    tn, fp, fn, tp = confusion_matrix(yte, y_pred).ravel()

    metrics = {
        "accuracy": round(float(accuracy_score(yte, y_pred)), 4),
        "roc_auc": round(float(roc_auc_score(yte, y_prob)), 4),
        "true_positive_rate": round(float(tp / (tp + fn)), 4),
        "false_positive_rate": round(float(fp / (fp + tn)), 4),
        "positive_rate": round(float(float(y_prob.mean())), 4),
    }

    export = {
        "features": ARRIVAL_FEATURES,
        "coefficients": [round(float(c), 6) for c in model.coef_[0]],
        "intercept": round(float(model.intercept_[0]), 6),
    }
    joblib.dump(model, MODEL_DIR / "arrival_model.joblib")
    joblib.dump(export, MODEL_DIR / "arrival_export.joblib")
    print("Arrival model (LogisticRegression)")
    print(f"  features        : {ARRIVAL_FEATURES}")
    print(f"  coefficients    : {export['coefficients']}")
    print(f"  intercept       : {export['intercept']}")
    print(f"  metrics         : {metrics}")
    return export


# --------------------------------------------------------------------------- #
# 2) Charging duration  (DecisionTreeRegressor)
# --------------------------------------------------------------------------- #
def train_duration_model(df: pd.DataFrame) -> dict:
    X = df[DURATION_FEATURES].to_numpy(float)
    y = df["duration_min"].to_numpy(float)
    Xtr, Xte, ytr, yte = train_test_split(X, y, test_size=0.2, random_state=11)

    model = DecisionTreeRegressor(
        max_depth=8, min_samples_leaf=4, min_samples_split=10, random_state=11
    )
    model.fit(Xtr, ytr)

    y_pred = model.predict(Xte)
    metrics = {
        "mae_min": round(float(mean_absolute_error(yte, y_pred)), 2),
        "r2": round(float(r2_score(yte, y_pred)), 4),
        "rmse_min": round(float(np.sqrt(np.mean((yte - y_pred) ** 2))), 2),
        "max_depth": int(model.get_depth()),
        "n_leaves": int(model.get_n_leaves()),
    }

    tree = model.tree_
    export = {
        "features": DURATION_FEATURES,
        "max_depth": int(tree.max_depth),
        "n_nodes": int(tree.node_count),
        "feature": tree.feature.tolist(),      # -2 == leaf
        "threshold": np.round(tree.threshold, 4).tolist(),
        "children_left": tree.children_left.tolist(),
        "children_right": tree.children_right.tolist(),
        "value": np.round(tree.value.ravel(), 4).tolist(),
    }
    joblib.dump(model, MODEL_DIR / "duration_model.joblib")
    joblib.dump(export, MODEL_DIR / "duration_export.joblib")

    print("\nDuration model (DecisionTreeRegressor, max_depth=8)")
    print(f"  metrics         : {metrics}")
    print("\n  tree (text view):")
    print(export_text(model, feature_names=DURATION_FEATURES, max_depth=3))
    return export


# --------------------------------------------------------------------------- #
# Export to C header
# --------------------------------------------------------------------------- #
def export_to_c(arrival: dict, duration: dict) -> Path:
    MODEL_DIR.mkdir(parents=True, exist_ok=True)
    lines = []
    w = lines.append
    w("/*")
    w(" * models.h  —  AUTO-GENERATED by ai/train_models.py — DO NOT EDIT BY HAND.")
    w(" *")
    w(" * Edge-AI models exported from Python/scikit-learn for on-device inference")
    w(" * on the ESP32 (no ML library needed on the target).")
    w(" *")
    w(" * 1) ARRIVAL PROBABILITY (LogisticRegression)")
    w(f" *    features : {', '.join(arrival['features'])}")
    w(f" *    p(arrival) = sigmoid( sum(coef_i * x_i) + intercept )")
    w(" *")
    w(" * 2) CHARGING DURATION (DecisionTreeRegressor)")
    w(f" *    features : {', '.join(duration['features'])}")
    w(" *    Predicts: session duration in minutes.")
    w(" *    Tree encoded as parallel arrays (CSR-style). Node index 0 is the root.")
    w(" *    feature[idx]==-2  ->  leaf, value[] holds the prediction.")
    w(" */")
    w("#pragma once")
    w("")
    w("// ------------------------------------------------------------------ //")
    w("// Arrival probability: sigmoid(w . x + b)")
    w("// ------------------------------------------------------------------ //")
    w(f"#define ARRIVAL_N_FEATURES {len(arrival['coefficients'])}U")
    w("static const float ARRIVAL_COEF[ARRIVAL_N_FEATURES] = {")
    w("    " + ", ".join(f"{c:.6f}f" for c in arrival["coefficients"]) + "};")
    w(f"static const float ARRIVAL_BIAS = {arrival['intercept']:.6f}f;")
    w("// feature order: hour_sin, hour_cos, weekday, is_weekend, is_peak, busy_rate")
    w("")
    w("// ------------------------------------------------------------------ //")
    w("// Charging duration: decision tree")
    w("// ------------------------------------------------------------------ //")
    w(f"#define DURATION_N_FEATURES {len(duration['features'])}U")
    trees = {
        "feature": duration["feature"],
        "threshold": duration["threshold"],
        "left": duration["children_left"],
        "right": duration["children_right"],
        "value": duration["value"],
    }
    for name, arr in trees.items():
        if name == "feature":
            w(f"static const int16_t DT_{name.upper()}[{len(arr)}] = {{")
            w("    " + ", ".join(str(int(v)) for v in arr) + "};")
        elif name == "threshold" or name == "value":
            w(f"static const float DT_{name.upper()}[{len(arr)}] = {{")
            w("    " + ", ".join(f"{float(v):.4f}f" for v in arr) + "};")
        else:
            w(f"static const int32_t DT_{name.upper()}[{len(arr)}] = {{")
            w("    " + ", ".join(str(int(v)) for v in arr) + "};")
    w("// feature order: capacity_kwh, soc0_pct, socT_pct, ambient_c, current_a, connector_temp_c")
    w("")

    header = "\n".join(lines) + "\n"
    header_path = MODEL_DIR / "models.h"
    header_path.write_text(header, encoding="utf-8")
    FIRMWARE_DIR.mkdir(parents=True, exist_ok=True)
    shutil.copy2(header_path, FIRMWARE_DIR / "models.h")
    return header_path


# --------------------------------------------------------------------------- #
def main() -> None:
    MODEL_DIR.mkdir(parents=True, exist_ok=True)
    PLOTS_DIR.mkdir(parents=True, exist_ok=True)

    arr_df = pd.read_csv(DATA_DIR / "arrivals.csv")
    dur_df = pd.read_csv(DATA_DIR / "durations.csv")
    print(f"Loaded {len(arr_df):,} arrival rows and {len(dur_df):,} duration rows\n")

    arrival = train_arrival_model(arr_df)
    duration = train_duration_model(dur_df)

    header_path = export_to_c(arrival, duration)

    summary = {
        "arrival_probability": {
            "algorithm": "LogisticRegression",
            "features": ARRIVAL_FEATURES,
            "coefficients": arrival["coefficients"],
            "intercept": arrival["intercept"],
        },
        "charging_duration": {
            "algorithm": "DecisionTreeRegressor",
            "features": DURATION_FEATURES,
            "max_depth": duration.get("max_depth"),
            "n_nodes": duration.get("n_nodes"),
        },
        "needs_refit": False,
    }
    (MODEL_DIR / "model_metadata.json").write_text(
        json.dumps(summary, indent=2), encoding="utf-8"
    )

    print(f"\nExported C header -> {header_path}")
    print(f"Exported metadata -> {MODEL_DIR / 'model_metadata.json'}")
    print("Done. Firmware copy: firmware/smart_ev_charger/models.h")

    # quick spot check of the tree inference vs sklearn
    _self_test(duration, dur_df)


def _self_test(duration_exp: dict, df: pd.DataFrame) -> None:
    """Verify the C-exported decision-tree encoding by walking it in Python."""
    from ai.edge_ai import DurationPredictorC

    pred = DurationPredictorC(duration_exp)
    sample = df.sample(50, random_state=3)
    X = sample[DURATION_FEATURES].to_numpy(float)
    manual = np.array([pred.predict(x) for x in X])
    sklearn_tree = joblib.load(MODEL_DIR / "duration_model.joblib")
    ref = sklearn_tree.predict(X)
    ok = np.allclose(manual, ref, atol=0.05)
    print(f"\nSelf-test: C-exported tree matches sklearn on 50 samples -> {'PASS' if ok else 'FAIL'}")
    if not ok:
        raise SystemExit("self-test failed — exported tree inconsistent with sklearn!")


if __name__ == "__main__":
    main()