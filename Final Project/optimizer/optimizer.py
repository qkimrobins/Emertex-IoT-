#!/usr/bin/env python3
"""
optimizer.py
============
Python reference implementation of the 3-bay EV charging station simulator and
the Edge-AI driven load-management optimizer.

This module mirrors, line-for-line in behaviour, the C++ code that runs on the
ESP32 (firmware/smart_ev_charger). It is used by:

  * scripts/live_demo.py   - end-to-end MQTT demo without any hardware,
  * scripts/run_tests.py   - invariant checks used during the viva,
  * the training pipeline  - consistency checks against the exported models.

Decision policy (identical on the device and here):
  ALLOW   - charge at the requested current,
  THROTTLE- reduced current (thermal / station-load / predictive reserve),
  DEFER   - charging paused (temperature critical, station overload, disabled).

Safety thresholds (IEC 61851-inspired, demo values):
  TEMP_WARN = 55 C   -> THROTTLE to 75 %
  TEMP_HIGH = 65 C   -> THROTTLE to 40 %
  TEMP_CRIT = 80 C   -> DEFER (pause) + critical alarm
  MIN_CHARGE_CURRENT = 6 A  (below this a session is not worth running)
"""
from __future__ import annotations

import math
import random
from dataclasses import dataclass, field
from typing import Optional

# --------------------------------------------------------------------------- #
# Constants (shared with firmware/config.h)
# --------------------------------------------------------------------------- #
V_NOM = 230.0          # nominal AC voltage (V)
PF = 0.98              # power factor
ETA = 0.92             # charger efficiency
STATION_MAX_CURRENT_DEFAULT = 64.0   # A
BAY_LIMIT_DEFAULT = 32.0             # A (7.4 kW per bay)
MIN_CHARGE_CURRENT = 6.0             # A
TEMP_WARN, TEMP_HIGH, TEMP_CRIT = 55.0, 65.0, 80.0   # connector temp (C)
SOFT_LIMIT_PCT = 90.0                # station load alarm threshold (%)
SIM_TIME_ACCEL = 60                  # sim minutes per real second

VEHICLES: dict[str, dict] = {
    "Tesla Model 3":      {"capacity": 60.0, "soc0": (15, 45), "socT": 90, "current": 32.0},
    "Tesla Model Y":      {"capacity": 75.0, "soc0": (20, 50), "socT": 90, "current": 32.0},
    "Hyundai IONIQ 5":    {"capacity": 77.0, "soc0": (25, 55), "socT": 95, "current": 26.0},
    "Nissan Leaf":        {"capacity": 40.0, "soc0": (20, 60), "socT": 100, "current": 32.0},
    "VW ID.4":            {"capacity": 82.0, "soc0": (10, 50), "socT": 90, "current": 30.0},
    "Porsche Taycan":     {"capacity": 93.0, "soc0": (15, 45), "socT": 85, "current": 32.0},
    "BYD Atto 3":         {"capacity": 60.0, "soc0": (20, 50), "socT": 95, "current": 32.0},
    "Audi e-tron GT":     {"capacity": 85.0, "soc0": (10, 40), "socT": 90, "current": 32.0},
}

DECISION_ALLOW, DECISION_THROTTLE, DECISION_DEFER = "ALLOW", "THROTTLE", "DEFER"

STATE_IDLE, STATE_CHARGING, STATE_DONE, STATE_FAULT = "IDLE", "CHARGING", "DONE", "FAULT"

ALARM_TEMP_HIGH = "TEMP_HIGH"
ALARM_TEMP_CRIT = "TEMP_CRIT"
ALARM_OVERLOAD = "OVERLOAD"
ALARM_VOLTAGE = "VOLTAGE"
ALARM_COM_LOSS = "COM_LOSS"


# --------------------------------------------------------------------------- #
@dataclass
class Bay:
    index: int
    enabled: bool = True
    present: bool = False                 # EV physically plugged in
    vehicle: str = ""
    state: str = STATE_IDLE
    soc: float = 0.0
    target_soc: float = 90.0
    capacity_kwh: float = 60.0
    voltage_v: float = V_NOM
    current_a: float = 0.0
    temp_c: float = 25.0
    power_kw: float = 0.0
    energy_kwh: float = 0.0               # session energy
    current_limit_a: float = BAY_LIMIT_DEFAULT
    priority: int = 2                     # 1 = highest
    # --- optimizer output ---
    allowed_a: float = 0.0
    decision: str = DECISION_ALLOW
    reason: str = "normal"
    # --- AI output ---
    predicted_min: float = 0.0
    time_remaining_min: float = 0.0

    @property
    def charging(self) -> bool:
        return self.state == STATE_CHARGING


@dataclass
class Station:
    bays: list[Bay] = field(default_factory=list)
    max_station_current: float = STATION_MAX_CURRENT_DEFAULT
    optimizer_mode: str = "AUTO"          # AUTO | MANUAL
    busy_rate: float = 1.0                # arrival-rate multiplier (shared attr)
    auto_events: bool = True              # simulator plugs EVs in/out itself
    ambient_c: float = 22.0
    ambient_base: float = 22.0
    sim_day: int = 2                      # day counter (0 = Monday, 2 = Wednesday)
    sim_minute_of_day: float = 8.0 * 60   # simulated clock
    station_energy_kwh: float = 0.0
    last_arrival_prob: float = 0.0
    predicted_arrivals: float = 0.0
    mqtt_ok: bool = True
    rng: random.Random = field(default_factory=lambda: random.Random(7))
    alarms: set[str] = field(default_factory=set)
    # queued simulator events (sim minutes)
    _next_auto_event: float = field(default_factory=lambda: 5.0 + random.random() * 10)

    def __post_init__(self) -> None:
        if not self.bays:
            self.bays = [Bay(index=1, priority=1), Bay(index=2, priority=2), Bay(index=3, priority=3)]

    # --------------------------------------------------------------- #
    def totals(self) -> dict:
        total_a = sum(b.current_a for b in self.bays)
        total_kw = sum(b.power_kw for b in self.bays)
        active = sum(1 for b in self.bays if b.charging)
        return {
            "total_a": total_a,
            "total_kw": total_kw,
            "load_pct": 100.0 * total_a / self.max_station_current if self.max_station_current else 0.0,
            "active": active,
            "energy_kwh": self.station_energy_kwh,
        }

    # --------------------------------------------------------------- #
    # Simulation stepping (dt = sim minutes)
    # --------------------------------------------------------------- #
    def step(self, dt_min: float) -> None:
        self._advance_clock(dt_min)
        self._update_ambient()
        self._thermal(dt_min)
        for bay in self.bays:
            if not bay.present or bay.state in (STATE_IDLE, STATE_FAULT):
                bay.current_a, bay.power_kw = 0.0, 0.0
                if bay.state == STATE_IDLE:
                    bay.voltage_v = self._mains_voltage()
                continue
            if bay.state == STATE_CHARGING:
                self._charge_step(bay, dt_min)
            elif bay.state == STATE_DONE:
                bay.current_a, bay.power_kw = 0.0, 0.0
                bay.voltage_v = self._mains_voltage()
        self._auto_events(dt_min)

    def _advance_clock(self, dt_min: float) -> None:
        self.sim_minute_of_day += dt_min
        if self.sim_minute_of_day >= 1440:
            self.sim_minute_of_day -= 1440
            self.sim_day = (self.sim_day + 1) % 7

    def _update_ambient(self) -> None:
        hour = self.sim_minute_of_day / 60.0
        # gentle 24 h temperature cycle around the base
        self.ambient_c = self.ambient_base + 4.0 * math.sin((hour - 9.0) / 24.0 * 2 * math.pi)

    def _mains_voltage(self) -> float:
        t = self.totals()["total_a"]
        return V_NOM - 0.05 * t + self.rng.uniform(-1.0, 1.0)

    def _thermal(self, dt_min: float) -> None:
        """Connector/cable temperature first-order model: T -> T_eq(I^2)."""
        tau_min = 1.5  # ~90 s time constant
        for bay in self.bays:
            if bay.present and bay.charging:
                teq = self.ambient_c + 5.0 + 0.020 * bay.current_a ** 2
                bay.temp_c += (teq - bay.temp_c) * min(1.0, dt_min / tau_min)
            elif bay.present:
                teq = self.ambient_c + 3.0
                bay.temp_c += (teq - bay.temp_c) * min(1.0, dt_min / tau_min)
            else:
                bay.temp_c = self.ambient_c

    def _charge_step(self, bay: Bay, dt_min: float) -> None:
        # current taper (CC -> CV) as the pack approaches full
        if bay.soc >= 80.0:
            taper = max(0.05, (100.0 - bay.soc) / 20.0)
        else:
            taper = 1.0
        bay.current_a = bay.allowed_a * taper
        if bay.current_a < 0.05:
            bay.current_a = 0.0
        bay.voltage_v = self._mains_voltage()
        p_w = bay.voltage_v * bay.current_a * PF * ETA
        bay.power_kw = p_w / 1000.0
        d_wh = p_w * dt_min / 60.0
        bay.energy_kwh += d_wh / 1000.0
        self.station_energy_kwh += d_wh / 1000.0
        bay.soc += d_wh / (bay.capacity_kwh * 1000.0) * 100.0
        if bay.soc >= bay.target_soc:
            bay.soc = bay.target_soc
            bay.state = STATE_DONE
            bay.current_a, bay.power_kw = 0.0, 0.0
        # remaining time = energy needed [Wh] / actual power [W] * 60
        needed_wh = bay.capacity_kwh * 1000.0 * (bay.target_soc - bay.soc) / 100.0
        bay.time_remaining_min = needed_wh / max(p_w, 1.0) * 60.0 if p_w > 1.0 else 0.0

    # --------------------------------------------------------------- #
    # Automatic plug-in / plug-out events (driven by the Edge-AI model)
    # --------------------------------------------------------------- #
    def _auto_events(self, dt_min: float) -> None:
        if not self.auto_events:
            return
        self._next_auto_event -= dt_min
        if self._next_auto_event > 0:
            return
        free = [b for b in self.bays if not b.present]
        # Edge-AI arrival probability decides whether a driver shows up
        p_arrive = self.last_arrival_prob
        if free and self.rng.random() < p_arrive:
            bay = self.rng.choice(free)
            self.plug_in(bay.index, vehicle=self.rng.choice(list(VEHICLES)))
            self._next_auto_event = 4.0 + self.rng.random() * 9.0
        else:
            self._next_auto_event = 1.5 + self.rng.random() * 4.0

    # --------------------------------------------------------------- #
    # Public control API (same commands the dashboard / RPC send)
    # --------------------------------------------------------------- #
    def plug_in(self, bay_idx: int, vehicle: Optional[str] = None,
                capacity: Optional[float] = None, soc0: Optional[float] = None,
                socT: Optional[float] = None, priority: Optional[int] = None) -> bool:
        bay = self.bays[bay_idx - 1]
        if bay.present:
            return False
        spec = VEHICLES.get(vehicle, VEHICLES["Tesla Model 3"])
        bay.vehicle = vehicle or "Tesla Model 3"
        bay.capacity_kwh = capacity or spec["capacity"]
        lo, hi = spec["soc0"]
        bay.soc = round((soc0 if soc0 is not None else self.rng.uniform(lo, hi)), 1)
        bay.target_soc = round(socT or spec["socT"], 0)
        bay.current_limit_a = spec["current"]
        bay.priority = priority or self.rng.randint(1, 3)
        bay.present = True
        bay.state = STATE_CHARGING
        bay.energy_kwh = 0.0
        bay.temp_c = self.ambient_c + 2.0
        bay.predicted_min = 0.0
        bay.time_remaining_min = 0.0
        return True

    def plug_out(self, bay_idx: int) -> bool:
        bay = self.bays[bay_idx - 1]
        if not bay.present:
            return False
        bay.present = False
        bay.state = STATE_IDLE
        bay.soc = 0.0
        bay.current_a = bay.power_kw = 0.0
        bay.allowed_a = 0.0
        bay.decision = DECISION_ALLOW
        bay.reason = "idle"
        return True

    def simulate_fault(self, bay_idx: int) -> bool:
        bay = self.bays[bay_idx - 1]
        bay.state = STATE_FAULT
        bay.reason = "simulated_fault"
        return True

    def clear_fault(self, bay_idx: int) -> bool:
        bay = self.bays[bay_idx - 1]
        if bay.state != STATE_FAULT:
            return False
        bay.state = STATE_CHARGING if bay.present else STATE_IDLE
        bay.reason = "normal"
        return True


# --------------------------------------------------------------------------- #
# The optimizer itself
# --------------------------------------------------------------------------- #
def run_optimizer(st: Station) -> None:
    """Computes the per-bay ALLOW / THROTTLE / DEFER decision for the next tick.

    Runs safety checks first (always active), then the load/energy management
    layer (AUTO mode) which includes a predictive head-room reserve driven by
    the Edge-AI arrival probability.
    """
    st.alarms.discard(ALARM_OVERLOAD)
    st.alarms.discard(ALARM_TEMP_CRIT)
    st.alarms.discard(ALARM_TEMP_HIGH)

    for bay in st.bays:
        if not bay.present or bay.state == STATE_IDLE:
            bay.allowed_a = 0.0
            bay.decision = DECISION_ALLOW
            bay.reason = "idle"
            continue

        bay.allowed_a = bay.current_limit_a
        bay.decision = DECISION_ALLOW
        bay.reason = "normal"

        # ---- safety layer (hard, always on) ----
        if not bay.enabled:
            bay.allowed_a, bay.decision, bay.reason = 0.0, DECISION_DEFER, "disabled"
            continue
        if bay.state == STATE_FAULT:
            bay.allowed_a, bay.decision, bay.reason = 0.0, DECISION_DEFER, "fault_locked"
            continue
        if bay.state == STATE_DONE:
            bay.allowed_a, bay.decision, bay.reason = 0.0, DECISION_ALLOW, "session_complete"
            continue
        if bay.temp_c >= TEMP_CRIT:
            bay.allowed_a, bay.decision, bay.reason = 0.0, DECISION_DEFER, "temp_critical"
            st.alarms.add(ALARM_TEMP_CRIT)
            continue
        if bay.temp_c >= TEMP_HIGH:
            bay.allowed_a, bay.decision, bay.reason = bay.allowed_a * 0.40, DECISION_THROTTLE, "temp_high"
            st.alarms.add(ALARM_TEMP_HIGH)
            continue
        if bay.temp_c >= TEMP_WARN:
            bay.allowed_a, bay.decision, bay.reason = bay.allowed_a * 0.75, DECISION_THROTTLE, "temp_warm"

    if st.optimizer_mode != "AUTO":
        _enforce_hard_limit(st)
        _refresh_alarms(st)
        return

    # ---- predictive load-management (AUTO) ----
    # expected current of arrivals in the next 15 min (Edge-AI forecast)
    free_bays = sum(1 for b in st.bays if not b.present)
    st.predicted_arrivals = st.last_arrival_prob * free_bays
    reserve_a = st.predicted_arrivals * 16.0  # ~3.7 kW per expected session

    running = [b for b in st.bays if b.charging]
    if running:
        requested = sum(b.allowed_a for b in running)
        avail = st.max_station_current - reserve_a
        if requested > avail:
            _proportional_allocate(running, avail, st.max_station_current)

    _enforce_hard_limit(st)
    _refresh_alarms(st)


def _proportional_allocate(running: list[Bay], avail: float, hard_max: float) -> None:
    """Scale every active bay, deferring the lowest-priority bay that falls
    below the minimum useful current, then re-distribute the freed head-room."""
    for _ in range(4):
        requested = sum(b.allowed_a for b in running)
        if requested <= avail + 0.01:
            break
        scale = avail / max(requested, 0.01)
        for b in running:
            b.allowed_a = b.allowed_a * scale
            b.decision = DECISION_THROTTLE
            b.reason = "station_load"
        # defer the lowest-priority bay that dropped below the floor
        below = [b for b in running if b.allowed_a < MIN_CHARGE_CURRENT]
        if below:
            victim = max(below, key=lambda b: (b.priority, -b.index))
            victim.allowed_a = 0.0
            victim.decision = DECISION_DEFER
            victim.reason = "station_load_low_priority"
            running.remove(victim)
            avail = hard_max  # freed soft reserve is reusable once a bay is deferred
        else:
            break


def _enforce_hard_limit(st: Station) -> None:
    """Fuse: never exceed the station's rated current, even in MANUAL mode."""
    for _ in range(4):
        total = sum(b.allowed_a for b in st.bays if b.charging)
        if total <= st.max_station_current + 0.01:
            break
        excess = total - st.max_station_current
        charging = [b for b in st.bays if b.charging]
        for b in charging:
            cut = min(b.allowed_a, excess * b.allowed_a / max(total, 0.01))
            b.allowed_a -= cut
            if b.allowed_a < MIN_CHARGE_CURRENT:
                b.allowed_a = 0.0
                b.decision = DECISION_DEFER
                b.reason = "hard_limit"
                break
        if any(b.allowed_a < MIN_CHARGE_CURRENT for b in charging):
            break


def _refresh_alarms(st: Station) -> None:
    t = st.totals()
    if t["load_pct"] >= SOFT_LIMIT_PCT:
        st.alarms.add(ALARM_OVERLOAD)
    for bay in st.bays:
        if bay.present and bay.voltage_v < 210.0:
            st.alarms.add(ALARM_VOLTAGE)


# --------------------------------------------------------------------------- #
# Edge-AI predictions (delegates to ai.edge_ai when available)
# --------------------------------------------------------------------------- #
def refresh_edge_ai(st: Station, ai) -> None:
    """Recompute the Edge-AI outputs each optimizer tick."""
    hour = st.sim_minute_of_day / 60.0
    st.last_arrival_prob = ai.arrival_probability(hour, st.sim_day, st.busy_rate)
    for bay in st.bays:
        if bay.charging and bay.predicted_min <= 0.0:
            stub = {"Tesla Model 3": 60, "Tesla Model Y": 75, "Hyundai IONIQ 5": 77,
                    "Nissan Leaf": 40, "VW ID.4": 82, "Porsche Taycan": 93,
                    "BYD Atto 3": 60, "Audi e-tron GT": 85}.get(bay.vehicle, 60)
            bay.predicted_min = ai.charge_minutes(
                bay.capacity_kwh, bay.soc, bay.target_soc,
                st.ambient_c, bay.current_limit_a, bay.temp_c,
            )


# --------------------------------------------------------------------------- #
# Telemetry schema (matches firmware + dashboard)
# --------------------------------------------------------------------------- #
def build_telemetry(st: Station) -> dict:
    t = st.totals()
    hour = st.sim_minute_of_day / 60.0
    msg = {
        "uptime_s": 0,
        "station_total_power_kw": round(t["total_kw"], 2),
        "station_total_current_a": round(t["total_a"], 1),
        "station_load_pct": round(t["load_pct"], 1),
        "station_energy_kwh": round(t["energy_kwh"], 2),
        "max_station_current_a": st.max_station_current,
        "ambient_temp_c": round(st.ambient_c, 1),
        "optimizer_mode": st.optimizer_mode,
        "busy_rate": round(st.busy_rate, 2),
        "sim_day": st.sim_day,
        "sim_hour": round(hour, 1),
        "sim_time": f"Day {st.sim_day} {int(hour):02d}:{int((hour % 1) * 60):02d}",
        "arrival_probability_pct": round(100.0 * st.last_arrival_prob, 1),
        "predicted_arrivals": round(st.predicted_arrivals, 2),
        "charging_sessions_active": t["active"],
        "alarms_active": len(st.alarms),
        "alarm_codes": "|".join(sorted(st.alarms)),
        "wifi_rssi_db": -55,
        "firmware_version": "python-1.0.0",
    }
    for b in st.bays:
        msg[f"bay{b.index}_state"] = b.state
        msg[f"bay{b.index}_soc"] = round(b.soc, 1)
        msg[f"bay{b.index}_voltage_v"] = round(b.voltage_v, 1)
        msg[f"bay{b.index}_current_a"] = round(b.current_a, 1)
        msg[f"bay{b.index}_power_kw"] = round(b.power_kw, 2)
        msg[f"bay{b.index}_energy_kwh"] = round(b.energy_kwh, 2)
        msg[f"bay{b.index}_temp_c"] = round(b.temp_c, 1)
        msg[f"bay{b.index}_target_soc"] = b.target_soc
        msg[f"bay{b.index}_predicted_min"] = round(b.predicted_min, 0)
        msg[f"bay{b.index}_time_remaining_min"] = round(b.time_remaining_min, 0)
        msg[f"bay{b.index}_priority"] = b.priority
        msg[f"bay{b.index}_decision"] = b.decision
        msg[f"bay{b.index}_reason"] = b.reason
        msg[f"bay{b.index}_vehicle"] = b.vehicle if b.present else ""
    return msg


def apply_command(st: Station, cmd: str, params: dict) -> dict:
    """Applies a dashboard / RPC command to the station. Returns a response."""
    bay = int(params.get("bay", 1))
    resp = {"cmd": cmd, "ok": True, "bay": bay, "detail": "done"}
    try:
        if cmd == "plugIn":
            resp["ok"] = st.plug_in(bay, vehicle=params.get("vehicle"),
                                    soc0=params.get("soc0"), socT=params.get("socT"),
                                    priority=params.get("priority"))
            resp["detail"] = "plugged in" if resp["ok"] else "bay already occupied"
        elif cmd == "plugOut":
            resp["ok"] = st.plug_out(bay)
            resp["detail"] = "unplugged" if resp["ok"] else "bay already free"
        elif cmd == "setEnabled":
            st.bays[bay - 1].enabled = bool(params.get("enabled", True))
        elif cmd == "setMode":
            mode = str(params.get("mode", "AUTO")).upper()
            st.optimizer_mode = mode if mode in ("AUTO", "MANUAL") else "AUTO"
        elif cmd == "setMaxCurrent":
            st.max_station_current = float(params.get("amps", 64))
        elif cmd == "setBayLimit":
            st.bays[bay - 1].current_limit_a = float(params.get("amps", 16))
        elif cmd == "setPriority":
            st.bays[bay - 1].priority = int(params.get("priority", 2))
        elif cmd == "setBusyRate":
            st.busy_rate = float(params.get("rate", 1.0))
        elif cmd == "setAutoEvents":
            st.auto_events = bool(params.get("on", True))
        elif cmd == "fault":
            resp["ok"] = st.simulate_fault(bay)
        elif cmd == "clearFault":
            resp["ok"] = st.clear_fault(bay)
        elif cmd == "resetEnergy":
            st.station_energy_kwh = 0.0
        else:
            resp = {"cmd": cmd, "ok": False, "detail": f"unknown cmd: {cmd}"}
    except Exception as exc:  # noqa: BLE001
        resp = {"cmd": cmd, "ok": False, "detail": str(exc)}
    return resp


def station_summary_row(st: Station) -> str:
    """One-line console summary used by live_demo.py."""
    t = st.totals()
    bays = "  ".join(
        f"B{b.index}:{b.state[:4]:<4} {b.current_a:5.1f}A {b.soc:5.1f}% "
        f"{b.decision:<8} {b.reason:<18}"
        for b in st.bays
    )
    return (
        f"[{st.sim_time}] load {t['load_pct']:5.1f}%  P={t['total_kw']:6.2f} kW  "
        f"E={t['energy_kwh']:6.2f} kWh  AI-p={st.last_arrival_prob*100:4.1f}%  | {bays}"
    )