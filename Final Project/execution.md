# execution.md — How to Execute the Project

Step-by-step run guide for the **Smart Edge-AI EV Charging Station Optimizer**.

You can execute the project in **two ways** (or combine them):

| Path | What runs | Needs |
|---|---|---|
| **A. Python demo** (recommended first) | Full AI + optimizer + MQTT + dashboard, no hardware | Python venv only |
| **B. Wokwi firmware** | The ESP32 simulation: OLED, LEDs, real MQTT — in VS Code | Wokwi VS Code extension (prebuilt binary included) |
| **C. ThingsBoard uplink** (optional add-on) | Same telemetry + commands mirrored to ThingsBoard | A ThingsBoard device access token |

All commands below run from the **project root** (`…/Final Project`).

---

## 0. Prerequisites

- **Python 3.10+** (3.11/3.12 recommended)
- **VS Code** with the **Wokwi for VS Code** extension — only for Path B
- **Node.js** — optional, only to syntax-check dashboard JS
- **arduino-cli** + `esp32:esp32` core + 4 libraries — optional, only to *rebuild*
  the firmware (a prebuilt binary is already committed)

---

## 1. One-time setup (first time only)

```bash
cd "Final Project"

# 1. create + activate the Python environment
python3 -m venv .venv

# 2. install dependencies (scikit-learn, numpy, pandas, paho-mqtt)
.venv/bin/pip install -r requirements.txt

# 3. regenerate the trained model artifacts (.joblib files are git-ignored,
#    so they are NOT in the repository; this rebuilds them from ai/data/*.csv)
.venv/bin/python ai/train_models.py

# 4. verify the whole system
.venv/bin/python scripts/run_tests.py      # -> Result: 14 passed, 0 failed
```

> If `ai/train_models.py` complains that `ai/data/*.csv` is missing, generate
> the datasets first:
> ```bash
> .venv/bin/python ai/generate_data.py
> .venv/bin/python ai/train_models.py
> ```

> On Windows use `.venv\Scripts\python` / `.venv\Scripts\pip` instead of
> `.venv/bin/python` / `.venv/bin/pip`. On Linux/macOS the commands below work
> unchanged.

---

## Path A — Run everything with Python (no hardware)

### A.1 Verify the whole system (always do this first)

```bash
.venv/bin/python scripts/run_tests.py
```

**Expected:** `Result: 14 passed, 0 failed`
(checks: C-export equals sklearn, arrival curve, hard-limit invariant,
safety defer, command API, telemetry schema.)

### A.2 Start the live demo (publishes MQTT + prints console)

```bash
.venv/bin/python scripts/live_demo.py
```

**What you see** — one line every 2 s in the terminal:

```
t=   42 Day 2 14:49  load  57.8% P=  8.54kW E= 12.31kWh AI= 33.1% | B1:IDLE 0.0A … B2:CHRG 17.3A 63.2% THROTTLE predictive_reserve …
```

Variations:

```bash
.venv/bin/python scripts/live_demo.py --no-mqtt          # console only, no broker
.venv/bin/python scripts/live_demo.py --tb-token <TOKEN> # also uplink to ThingsBoard
```

### A.3 Open the dashboard (in a second terminal or another tab)

```bash
.venv/bin/python dashboard/serve.py
```

Then open **http://localhost:8080** in a browser.
*(Alternative: just double-click `dashboard/index.html` — no server needed.)*

**What you see:**

- Top-right badge **LIVE · MQTT** within ~2 s (data arriving from the broker).
- Bay cards with SoC rings, live V/A/kW/temp, ALLOW/THROTTLE/DEFER chips + reasons.
- Load / power / arrival gauges and charts update live.
- **Buttons actually work**: click *Plug in* → the terminal shows
  `[CMD] plugIn …` and the bay card activates; *Fault* → alarm banner/red.

> **Offline fallback:** if nothing is publishing, the dashboard switches to
> **DEMO FEED** after ~12 s and animates on its own with the same schema —
> good for a viva without a network.

### A.4 (Re)train the AI models — optional

```bash
.venv/bin/python ai/generate_data.py    # regenerates ai/data/*.csv
.venv/bin/python ai/train_models.py     # retrains + exports models.h
```

`train_models.py` overwrites `firmware/smart_ev_charger/models.h` —
rebuild the firmware (Path B.3) after retraining if you run Wokwi.

---

## Path B — Run the ESP32 firmware in Wokwi (VS Code)

> ℹ️ Wokwi does **not** compile sketches. `wokwi.toml` already points at the
> prebuilt `build/smart_ev_charger.ino.elf`, so this runs out of the box.

### B.1 Start the simulator

1. In VS Code: **File → Open Folder… → `firmware/smart_ev_charger`**
   (this exact folder must be the workspace root).
2. Press **F1 → "Wokwi: Start Simulator"** and wait ~10 s (first run downloads
   the simulator engine).

### B.2 What you should see immediately

| Element | Expected |
|---|---|
| Serial Monitor (115200 baud) | `Smart Edge-AI EV Charging Station Optimizer` boot banner + station logs |
| OLED (`board-ssd1306`, I²C D21/D22) | `== Smart EV Station ==` summary screen |
| LED1 (Bay 1, GPIO26/D26) | green (a Tesla Model 3 auto-plugs at boot) |
| LED2 / LED3 (D27 / D14) | light as EVs plug in |
| Red alarm LED (D13) | flashes when an alarm/fault is active |

### B.3 Rebuild the firmware (only after editing code)

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 \
  --output-dir firmware/smart_ev_charger/build firmware/smart_ev_charger
```

### B.4 Dashboard + Wokwi together (live MQTT)

The VS Code extension's bundled **IoT Gateway** gives the virtual ESP32 real
Internet access, so it can reach `broker.emqx.io`. While the simulator runs:

```bash
# terminal 2 — serve the dashboard
.venv/bin/python dashboard/serve.py
```

Open http://localhost:8080 → badge **LIVE · MQTT** ← data comes from the Wokwi
ESP32. Dashboard buttons now control the simulated station over `emx/ev/cmd`.

> On **wokwi.com** (web) the free tier has no Internet for WiFi — the OLED/
> LEDs/serial still run, but MQTT won't connect. Use the dashboard's DEMO FEED
> there, or Path A/B.4 for real MQTT.

---

## Path C — ThingsBoard uplink (optional)

1. Create a device in ThingsBoard CE (e.g. `demo.thingsboard.io`) and copy its
   **access token**.
2. Put the token in `firmware/smart_ev_charger/config.h`:
   ```cpp
   #define TB_ACCESS_TOKEN "PASTE_YOUR_DEVICE_ACCESS_TOKEN_HERE"
   ```
3. Rebuild (B.3) and restart the simulator — telemetry now also appears under
   **v1/devices/me/telemetry**.
   *Prefer the Python demo for TB testing:*
   ```bash
   .venv/bin/python scripts/live_demo.py --tb-token <TOKEN> --tb-host demo.thingsboard.io
   ```
4. Optional shared attributes you can set in ThingsBoard: `optimizerMode`
   (`AUTO`/`MANUAL`), `maxStationCurrent_a`, `busyRate`, `autoEvents`,
   `bayN_enabled`, `bayN_limit_a`, `bayN_priority`.

---

## Debugging / tools

```bash
# Raw MQTT stream on emx/ev/# — proves data is flowing, independent of the UI
.venv/bin/python scripts/mqtt_probe.py

# Lint the Wokwi circuit (diagram.json + wokwi.toml)
wokwi-cli lint          # run inside firmware/smart_ev_charger

# Syntax-check dashboard JS
for f in dashboard/js/*.js; do node --check "$f"; done
```

## Typical problems

| Symptom | Fix |
|---|---|
| `FileNotFoundError: …/ai/models/duration_export.joblib` | Run `.venv/bin/python ai/train_models.py` (joblib artifacts are git-ignored, not committed) |
| `module not found: sklearn` | `.venv/bin/pip install -r requirements.txt` |
| Dashboard badge = **DEMO FEED** | Nothing is publishing → start `live_demo.py` / Wokwi, wait 2 s |
| Wokwi black screen / no serial | Open the **`firmware/smart_ev_charger`** folder, not the repo root; check Output → "Wokwi" |
| Wokwi "firmware not found" | `wokwi.toml` must point to an existing `build/*.elf` (rebuild per B.3) |
| Wokwi compiles nothing | By design — Wokwi loads the prebuilt ELF; rebuild after code edits |
| MQTT won't connect on wokwi.com | Free web tier has no Internet WiFi; use VS Code extension or Path A |
| ThingsBoard shows nothing | Access token missing/incorrect in `config.h` |

---

## Suggested demo flow (2 minutes)

1. `scripts/run_tests.py` → **14/14 PASS** (evidence).
2. `scripts/live_demo.py` + dashboard at `localhost:8080` → live cards, click
   *Plug in* / *Fault* / *Prio* and watch decisions change.
3. If asked about the device: start the Wokwi simulator (Path B) → OLED/LEDs
   + same dashboard data.

For deep explanations (architecture, AI, viva Q&A) see **handbook.md**.