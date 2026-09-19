#!/usr/bin/env python3
"""
mqtt_probe.py
=============
Debug helper: subscribe to the demo topics and print every message.
Useful to verify that the firmware (Wokwi) or live_demo.py is actually
publishing before you debug the dashboard.

Run:
    .venv/bin/python scripts/mqtt_probe.py [--broker broker.emqx.io]
"""
from __future__ import annotations

import argparse
import json

import paho.mqtt.client as mqtt

TOPICS = [
    "emx/ev/telemetry",
    "emx/ev/cmd",
    "emx/ev/attr/state",
    "emx/ev/status",
    "emx/ev/#",
]


def on_message(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode())
        pretty = json.dumps(payload)[:400]
    except Exception:  # noqa: BLE001
        pretty = msg.payload.decode(errors="replace")[:400]
    print(f"[{msg.topic}] {pretty}")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--broker", default="broker.emqx.io")
    ap.add_argument("--port", type=int, default=1883)
    args = ap.parse_args()

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_message = on_message
    client.connect(args.broker, args.port, keepalive=30)
    for t in TOPICS:
        client.subscribe(t)
    print(f"Listening on {args.broker}:{args.port} -> {TOPICS}")
    client.loop_forever()


if __name__ == "__main__":
    main()