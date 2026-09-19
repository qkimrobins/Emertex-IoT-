#!/usr/bin/env python3
"""
live_demo.py
============
Full end-to-end demo WITHOUT any hardware or Wokwi.

This script:
  * loads the trained Edge-AI models (ai/edge_ai.py),
  * runs the 3-bay station simulator + optimizer (optimizer/optimizer.py),
  * publishes telemetry to the MQTT broker the dashboard listens on,
  * subscribes to the command topic so the dashboard's manual controls
    actually drive the simulated station,
  * optionally mirrors telemetry to ThingsBoard (topic v1/devices/me/telemetry).

Run:
    .venv/bin/python scripts/live_demo.py                # EMQX public broker
    .venv/bin/python scripts/live_demo.py --no-mqtt      # console-only
    .venv/bin/python scripts/live_demo.py --tb-token YOUR_TOKEN --tb-host host

Then open dashboard/index.html in a browser.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from ai.edge_ai import EdgeAIModels
from optimizer.optimizer import (
    Station, run_optimizer, refresh_edge_ai, build_telemetry,
    apply_command, STATION_MAX_CURRENT_DEFAULT,
)

BROKER = "broker.emqx.io"
PORT = 1883
TOPIC_TELE = "emx/ev/telemetry"
TOPIC_CMD = "emx/ev/cmd"
TOPIC_ATTR = "emx/ev/attr/state"
TOPIC_TB_TELE = "v1/devices/me/telemetry"

TICK_S = 2.0            # publish every 2 real seconds
SIM_DT_MIN = 2.0        # simulation advances 2 minutes per tick (~60x speed)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-mqtt", action="store_true", help="console-only simulation")
    ap.add_argument("--broker", default=BROKER)
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--tb-host", default=None, help="ThingsBoard CE host (optional)")
    ap.add_argument("--tb-token", default=None, help="ThingsBoard device access token")
    args = ap.parse_args()

    ai = EdgeAIModels()
    st = Station()
    # open the demo with one EV already charging so screens are alive instantly
    st.plug_in(2, vehicle="Nissan Leaf")
    pub = None
    if not args.no_mqtt:
        import paho.mqtt.client as mqtt
        pub = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"ev-station-py-{int(time.time())%100000}")
        pub.on_connect = lambda c, u, f, rc, p=None: print(f"[MQTT] connected to {args.broker} rc={rc}")
        try:
            pub.connect(args.broker, args.port, keepalive=30)
            pub.loop_start()
        except Exception as exc:  # noqa: BLE001
            print(f"[MQTT] connect failed ({exc}) — continuing console-only")
            pub = None

    tb = None
    if args.tb_host and args.tb_token:
        import paho.mqtt.client as mqtt
        tb = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="tb-edge-ai")
        try:
            tb.username_pw_set(args.tb_token)
            tb.connect(args.tb_host, 1883, keepalive=30)
            tb.loop_start()
            print(f"[MQTT] ThingsBoard uplink -> {args.tb_host} (token {args.tb_token[:6]}...)")
        except Exception as exc:  # noqa: BLE001
            print(f"[MQTT] ThingsBoard connect failed ({exc})")
            tb = None

    def handle_cmd(client, userdata, msg):
        try:
            payload = json.loads(msg.payload.decode())
            cmd = payload.get("cmd")
            params = payload.get("params", payload)
            resp = apply_command(st, cmd, params)
            print(f"[CMD] {cmd} {params} -> {resp}")
        except Exception as exc:  # noqa: BLE001
            print(f"[CMD] bad message {msg.payload!r}: {exc}")

    mqtt_ok = False
    if pub is not None:
        try:
            pub.subscribe(TOPIC_CMD)
            pub.on_message = handle_cmd
            mqtt_ok = True
        except Exception:  # noqa: BLE001
            pass

    print("=" * 78)
    print("Smart Edge-AI EV Charging Station Optimizer — Python live demo")
    print(f"  telemetry -> {TOPIC_TELE}  commands <- {TOPIC_CMD}")
    if mqtt_ok:
        print("  open dashboard/index.html (broker ws://broker.emqx.io:8083/mqtt)")
    else:
        print("  MQTT disabled — console output only")
    print("=" * 78)

    start = time.time()
    step = 0
    while True:
        refresh_edge_ai(st, ai)
        run_optimizer(st)
        st.step(SIM_DT_MIN)

        tele = build_telemetry(st)
        tele["uptime_s"] = int(time.time() - start)

        if pub is not None:
            pub.publish(TOPIC_TELE, json.dumps(tele), qos=0)
            pub.publish(TOPIC_ATTR, json.dumps({
                "optimizerMode": st.optimizer_mode,
                "maxStationCurrent_a": st.max_station_current,
                "busyRate": st.busy_rate,
                "autoEvents": st.auto_events,
            }), qos=0)
        if tb is not None:
            tb.publish(TOPIC_TB_TELE, json.dumps(tele), qos=0)

        print(station_summary(tele, step))
        step += 1
        time.sleep(TICK_S)


def station_summary(tele: dict, step: int) -> str:
    bays = "  ".join(
        f"B{b}:{tele[f'bay{b}_state'][:4]:<4} {tele[f'bay{b}_current_a']:5.1f}A "
        f"{tele[f'bay{b}_soc']:5.1f}% {tele[f'bay{b}_decision']:<8}{tele[f'bay{b}_reason']:<16}"
        for b in (1, 2, 3)
    )
    return (f"t={step:4d} {tele['sim_time']:<16} load {tele['station_load_pct']:5.1f}% "
            f"P={tele['station_total_power_kw']:6.2f}kW E={tele['station_energy_kwh']:6.2f}kWh "
            f"AI={tele['arrival_probability_pct']:4.1f}% | {bays}")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nDemo stopped.")