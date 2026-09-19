#!/usr/bin/env python3
"""
run_tests.py
============
Quick invariant / behaviour tests for the optimizer and the exported Edge-AI
models. Run before the viva to demonstrate that the system is correct:

    .venv/bin/python scripts/run_tests.py

Checks
  1. The C-exported decision tree matches the sklearn model.
  2. The arrival probability model reacts plausibly (peak > night).
  3. Station current never exceeds the hard rating.
  4. Safety: a bay at critical temperature is deferred (paused).
  5. Commands (plugIn / fault / setMaxCurrent) behave as documented.
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import numpy as np
import joblib

from ai.edge_ai import DurationPredictorC, EdgeAIModels
from optimizer.optimizer import (
    Station, run_optimizer, build_telemetry, apply_command,
    refresh_edge_ai, TEMP_CRIT, STATION_MAX_CURRENT_DEFAULT,
)

PASS, FAIL = 0, 0


def check(name: str, cond: bool, detail: str = "") -> None:
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  [PASS] {name} {detail}")
    else:
        FAIL += 1
        print(f"  [FAIL] {name} {detail}")


def test_tree_export() -> None:
    print("\n1) C-exported decision tree == sklearn model")
    exp = joblib.load(ROOT / "ai/models/duration_export.joblib")
    model = joblib.load(ROOT / "ai/models/duration_model.joblib")
    pred = DurationPredictorC(exp)
    rng = np.random.default_rng(0)
    X = rng.uniform([[20, 5, 80, -5, 6, 5]], [[100, 80, 100, 45, 32, 95]], (200, 6))
    manual = np.array([pred.predict(x) for x in X])
    ref = model.predict(X)
    check("tree export matches sklearn", np.allclose(manual, ref, atol=0.05),
          f"(max err {np.max(np.abs(manual - ref)):.4f} min)")


def test_arrival_curve() -> None:
    print("\n2) Arrival probability curve is sane")
    ai = EdgeAIModels()
    night = ai.arrival_probability(3.0, 0, 1.0)
    peak = ai.arrival_probability(17.5, 2, 1.0)
    weekend = ai.arrival_probability(13.0, 6, 1.0)
    busy = ai.arrival_probability(17.5, 2, 1.5)
    check("peak > night", peak > night, f"(peak {peak:.2f} vs night {night:.2f})")
    check("probability in [0,1]", 0 <= weekend <= 1 and 0 <= busy <= 1)
    check("busy rate increases probability", busy > peak,
          f"({busy:.2f} > {peak:.2f})")


def test_hard_limit() -> None:
    print("\n3) Station current never exceeds rating (predictive + hard limit)")
    ai = EdgeAIModels()
    st = Station()
    st.plug_in(1, vehicle="Tesla Model 3"); st.plug_in(2, vehicle="Nissan Leaf")
    st.plug_in(3, vehicle="VW ID.4")
    worst = 0.0
    saw_non_allow = False
    for _ in range(600):
        refresh_edge_ai(st, ai)
        run_optimizer(st)
        st.step(1.0)
        worst = max(worst, st.totals()["total_a"])
        if any(b.decision != "ALLOW" for b in st.bays):
            saw_non_allow = True
    check("hard limit respected", worst <= STATION_MAX_CURRENT_DEFAULT + 0.1,
          f"(worst {worst:.1f} A vs limit {STATION_MAX_CURRENT_DEFAULT:.0f} A)")
    check("optimizer made decisions", saw_non_allow)


def test_safety_defer() -> None:
    print("\n4) Critical temperature -> bay paused (DEFER)")
    st = Station()
    st.plug_in(1, vehicle="Tesla Model 3")
    st.bays[0].temp_c = TEMP_CRIT + 5
    run_optimizer(st)
    b = st.bays[0]
    check("critical temp defers bay", b.decision == "DEFER" and b.allowed_a == 0.0,
          f"({b.decision}, {b.allowed_a} A)")
    check("critical alarm raised", "TEMP_CRIT" in st.alarms)


def test_commands() -> None:
    print("\n5) Command API")
    st = Station()
    check("plugIn ok", apply_command(st, "plugIn", {"bay": 1, "vehicle": "Nissan Leaf"})["ok"])
    check("plugIn on occupied bay fails",
          not apply_command(st, "plugIn", {"bay": 1, "vehicle": "VW ID.4"})["ok"])
    apply_command(st, "setMaxCurrent", {"amps": 48})
    check("setMaxCurrent applied", st.max_station_current == 48.0)
    apply_command(st, "fault", {"bay": 1})
    run_optimizer(st)
    check("fault locks bay", st.bays[0].state == "FAULT" and st.bays[0].decision == "DEFER")
    apply_command(st, "clearFault", {"bay": 1})
    run_optimizer(st)
    check("clearFault resumes", st.bays[0].state == "CHARGING")
    tele = build_telemetry(st)
    check("telemetry schema complete", all(k in tele for k in (
        "station_load_pct", "arrival_probability_pct", "bay1_decision", "bay1_soc")))


if __name__ == "__main__":
    print("=" * 64)
    print("Smart Edge-AI EV Charging Station Optimizer — test suite")
    print("=" * 64)
    test_tree_export()
    test_arrival_curve()
    test_hard_limit()
    test_safety_defer()
    test_commands()
    print("\n" + "=" * 64)
    print(f"Result: {PASS} passed, {FAIL} failed")
    raise SystemExit(1 if FAIL else 0)