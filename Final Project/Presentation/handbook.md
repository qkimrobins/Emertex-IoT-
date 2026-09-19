# Smart Edge-AI EV Charging Station Optimizer — Viva Handbook

> Complete walkthrough of the project: what it does, how it works, how to run
> everything from **VS Code**, and what results to show during the demo/viva.

---

## 1. Project at a glance

| | |
|---|---|
| **Title** | Smart Edge-AI EV Charging Station Optimizer |
| **Core idea** | A 3-bay EV charging station (simulated on an ESP32 in Wokwi, or in Python) that uses **on-device machine learning** to predict arrivals and charging durations, and a rule-based **optimizer** that decides per bay: **ALLOW / THROTTLE / DEFER**. |
| **Hardware (virtual)** | ESP32 (Wokwi) + SSD1306 OLED + bay status LEDs. |
| **Cloud** | MQTT telemetry → ThingsBoard (device API) + a public mirror broker that feeds the browser dashboard. |
| **Edge AI** | `LogisticRegression` (arrival probability) + `DecisionTreeRegressor` (charging duration), trained in Python with scikit-learn and exported as **C arrays** into the firmware. |
| **Dashboard** | Modern dark IoT web dashboard — gauges, charts, bay cards, alarms, manual controls. Works even with **no MQTT** via an offline demo feed. |

---

## 2. Problem statement

EV charging points at a site share one feeder/transformer (~64 A in this demo).
If all bays charge at full current simultaneously:

- the feeder **overloads** (breaker trip / fire risk),
- connector temperature **rises** (cable/plug damage),
- a new EV may plug in and have **no head-room left**.

We need a smart station controller that:

1. **senses** electrical + thermal conditions per bay,
2. **predicts** the near future with edge AI (arrival probability, expected session duration),
3. **decides**, every second, what current each bay may draw —
   **ALLOW** (full), **THROTTLE** (reduced) or **DEFER** (paused) — while always
   respecting hard safety limits,
4. reports everything over **MQTT** to **ThingsBoard** and a web dashboard.

This is a classic **predictive load management** problem, solved here with a
lightweight stack that runs on a $10 microcontroller.

---

## 3. Architecture

![Architecture](docs/images/architecture.svg)

```
┌──────────────── Edge device (ESP32 / Wokwi) ────────────────┐
│                                                             │
│  3-Bay electrical simulator         Edge-AI inference        │
│  voltage · current · temp · SoC    ┌──────────────────┐     │
│  CC/CV charging profile            │ arrival prob (LR)│     │
│  EV plug-in/out events             │ duration (Tree)  │     │
│                                    └──────────────────┘     │
│                     Optimizer                                │
│        ALLOW / THROTTLE / DEFER per bay                       │
│        safety thresholds · predictive reserve                 │
│                                                             │
│   OLED status      MQTT client (PubSubClient)                │
│   bay LEDs           + WiFi (Wokwi-GUEST)                    │
└──────────────┬──────────────────────────────────────────────┘
               │  MQTT: telemetry / RPC / shared attributes
       ┌───────▼──────────┐        ┌─────────────────────┐
       │  ThingsBoard CE  │        │ Public mirror broker │ wss://broker.emqx.io
       │  (dashboard,     │        │  broker.emqx.io:1883 │
       │   device API)    │        └──────────┬──────────┘
       └──────────────────┘                   │ telemetry / commands
                                     ┌────────▼─────────┐
                                     │  Web dashboard    │
                                     │  index.html       │
                                     └──────────────────┘

Python (offline) side:
  ai/generate_data.py ─► train_models.py ─► models.h (C arrays) ─► firmware
  optimizer/optimizer.py : reference station + optimizer (mirrors firmware)
  scripts/live_demo.py   : full demo without hardware (publishes the same MQTT)
```

**One important design choice** — the firmware keeps *two* MQTT connections:

| Connection | Broker | Purpose |
|---|---|---|
| ThingsBoard | `demo.thingsboard.io` (or your CE) | `v1/devices/me/telemetry`, RPC commands, shared attributes — the official cloud demo |
| Mirror | `broker.emqx.io` (public) | same telemetry re-published so the browser dashboard works **even without ThingsBoard credentials** |

The browser dashboard talks to the mirror broker over **WebSockets** (`wss://broker.emqx.io:8084/mqtt`).

---

## 4. Components in detail

### 4.1 Firmware — `firmware/smart_ev_charger/` (ESP32, C++)

| File | Responsibility |
|---|---|
| `smart_ev_charger.ino` | Setup/loop, station + optimizer glue, telemetry publishing, command dispatcher |
| `config.h` | WiFi / ThingsBoard token / broker + **all safety thresholds** (single source of truth) |
| `ev_station.h/.cpp` | 3-bay electrical simulator: voltage sag, CC/CV charging curve, temperature model (first-order `T_eq = f(I²)`), energy integration, plug-in/out events, faults |
| `ai_edge.h` | On-device inference from exported arrays (`predictArrivalProbability`, `predictChargeMinutes`) |
| `optimizer.h/.cpp` | ALLOW/THROTTLE/DEFER decision engine + hard-limit enforcement |
| `tb_mqtt.h/.cpp` | Two MQTT clients: ThingsBoard API + mirror broker; RPC & shared-attribute handlers |
| `display.h/.cpp` | SSD1306 OLED status screen |
| `models.h` | **Generated** C arrays of the trained models (do not edit — see §6.4) |
| `wokwi.toml`, `diagram.json` | Wokwi simulator config: ESP32 + OLED (I²C 21/22) + 3 green bay LEDs (26/27/14) + red alarm LED (13) |

Simulated sensing (what a real station would have with CTs + NTC sensors):

- `voltageV` — mains sag with load: `V_NOM - 0.05·I + noise`
- `currentA` — follows the optimizer's `allowedA`, tapering on the CV segment
- `tempC` — connector temp: `T_eq = ambient + 5 + 0.020·I²`, approaches exponentially (`τ ≈ 1.5 min`)
- `soc`, `energyKwh` — integrated via a CC→CV charging profile

### 4.2 Edge-AI models — `ai/`

**Model A — arrival probability** (`LogisticRegression`)

> Will at least one EV plug in during the next 15 minutes?

- Features (6): `hour_sin`, `hour_cos`, `weekday`, `is_weekend`, `is_peak`, `busy_rate`
- Trained on **80 000** synthetic samples. Export = 6 coefficients + bias.
- On device: `p = 1 / (1 + exp(-(w·x + b)))` — a few multiply/adds, microseconds.

**Model B — charging duration** (`DecisionTreeRegressor`, max depth 8, 511 nodes)

> How many minutes will this charging session take?

- Features (6): `capacity_kwh`, `soc0_pct`, `socT_pct`, `ambient_c`, `current_a`, `connector_temp_c`
- Trained on **60 000** synthetic samples. Export = 4 node arrays
  (`feature`, `threshold`, `children_left/right`, `value`).
- On device: walk the tree with comparisons.

**Metrics (test split):**

| Model | Metric | Value |
|---|---|---|
| Arrival | ROC-AUC | **0.79** |
| Duration | MAE | **~43 min** |
| Duration | R² | **0.91** |

The training data is generated with the *same* CC/CV physical model the firmware
uses, so the exported model matches real on-device behaviour.

### 4.3 Optimizer — ALLOW / THROTTLE / DEFER

Runs every control tick (1 s firmware / 2 s Python). Per bay:

1. **Defaults**: charging bay → `ALLOW` at its socket limit.
2. **Disabled / complete / faulted** bays → `allowedA = 0` (reason: `disabled`, `session_complete`, `fault`).
3. **Thermal ladder** (connector temp):
   - `≥ 55 °C` → THROTTLE to **75%**
   - `≥ 65 °C` → THROTTLE to **40%**
   - `≥ 80 °C` → DEFER (0 A) + `TEMP_CRIT` alarm
   - Any current below the useful floor (**6 A**) pauses the session (DEFER).
4. **Predictive reserve**: `reserve = arrival_probability × free_bays × 16 A` —
   head-room is kept for EVs predicted to arrive soon.
5. **Proportional current ratioing** (AUTO mode): if requested current exceeds
   `station_rating − reserve`, scale every bay proportionally; then repeatedly
   drop the *lowest-priority* bay that fell under 6 A and re-divide the freed
   head-room.
6. **Hard limit (always enforced)**: `Σ bay currents ≤ station rating` (64 A
   default), even in MANUAL mode.

Shared thresholds table (kept identical in `config.h`, `optimizer/optimizer.py`, `dashboard/js/demo.js`):

| Constant | Value | Effect |
|---|---|---|
| `STATION_MAX_CURRENT_DEFAULT` | 64 A | feeder rating |
| `BAY_CURRENT_LIMIT_DEFAULT` | 32 A | per socket (7.4 kW @ 230 V) |
| `MIN_CHARGE_CURRENT_A` | 6 A | below this → pause session |
| `TEMP_WARN/C_HIGH/C_CRIT` | 55 / 65 / 80 °C | 75% → 40% → DEFER |

### 4.4 MQTT + ThingsBoard

**Topics**

| Topic | Direction | Content |
|---|---|---|
| `v1/devices/me/telemetry` (TB) | FW→TB | telemetry JSON |
| `v1/devices/me/attributes` (TB) | FW↔TB | client + shared attributes |
| `v1/devices/me/rpc/request/+` (TB) | TB→FW | RPC commands |
| `emx/ev/telemetry` (mirror) | FW→dash | telemetry JSON |
| `emx/ev/cmd` (mirror) | dash→FW | `{"cmd": "...", "params": {...}}` |
| `emx/ev/cmd/ack` (mirror) | FW→dash | command acknowledgement |
| `emx/ev/attr/state` (mirror) | FW→dash | config state (`optimizerMode`, `maxStationCurrent_a`, `busyRate`, `autoEvents`) |
| `emx/ev/status` (mirror) | FW→dash | connectivity heartbeat |

**Command API** (identical for RPC and mirror commands):

`plugIn, plugOut, setEnabled, setMode, setMaxCurrent, setBayLimit, setPriority,
setBusyRate, setAutoEvents, fault, clearFault, resetEnergy, getStatus`

### 4.5 Web dashboard — `dashboard/`

- `index.html` + `css/style.css` — dark IoT theme.
- `js/gauges.js` — SVG gauges (load %, arrival %), SoC rings.
- `js/charts.js` — canvas area/multi-line charts (station load, power, AI arrival).
- `js/app.js` — MQTT.js client + rendering; **demo-feed fallback**.
- `js/demo.js` — a JavaScript port of the station + optimizer + AI inference,
  used as an offline feed: if no MQTT message arrives for **12 s**, the dashboard
  switches to DEMO FEED mode and keeps animating (perfect for an offline viva).
- `serve.py` — zero-dependency static server (port 8080).

Dashboard widgets: 3 bay cards (SoC ring, V/A/kW/temp, state+decision chips,
AI estimated duration vs time left, Plug-in / Unplug / Prio / Fault buttons),
station gauges + charts, alarm banner, sim clock, LIVE/DEMO badge.

---

## 5. End-to-end workflow

```
generate_data.py ─► train_models.py ─► models.h ─► flash/Wokwi
      │                                  │
      │  (synthetic CSVs)                └──► ai_edge.h inference on ESP32
      ▼
{trained models} ──► optimizer.py (Python reference)
                        │
live_demo.py ───────────┤  Station.step() every tick
                        ▼
              build_telemetry() ──► MQTT broker
                                        ├──► ThingsBoard (telemetry + attributes)
                                        └──► Dashboard (telemetry; commands go back)
                                                ▲
    User clicks "Plug in" ──sendCmd('plugIn')──┘
```

In words:

1. **Offline**: `generate_data.py` synthesizes arrival + duration datasets from
   the physical station model → `train_models.py` fits scikit-learn models and
   exports them to `models.h` (C arrays) + joblib files.
2. **On device**: the ESP32 simulator runs the electrical + thermal model every
   tick; `ai_edge.h` computes `lastArrivalProb` (LR) and per-bay `predictedMin`
   (tree); the optimizer produces `allowedA` + `decision` per bay.
3. **Upstream**: telemetry JSON is published every ~2 s to ThingsBoard **and**
   the mirror broker.
4. **Downstream**: the dashboard renders it; manual controls publish commands
   that the firmware (or Python demo) applies; shared attributes from TB
   (`optimizerMode`, `maxStationCurrent_a`, …) reconfigure the station live.

---

## 6. How to run from VS Code (step by step)

### 6.0 One-time setup

1. **Open the project in VS Code**
   ```
   File → Open Folder → …/Final Project
   ```
2. **Create the Python environment** (Terminal → New Terminal):
   ```bash
   cd "Final Project"
   python3 -m venv .venv
   .venv/bin/pip install -r requirements.txt
   ```
   > `requirements.txt` = `scikit-learn`, `numpy`, `pandas`, `paho-mqtt`, `joblib`.
3. **(Optional) Wokwi extension** — install "Wokwi for VS Code" from the
   marketplace if you want the simulated ESP32 to run inside VS Code.
   Otherwise use https://wokwi.com (drag in the `firmware/smart_ev_charger`
   folder).
4. **(Optional, for local firmware compile checks)** install `arduino-cli` and
   `esp32:esp32` core, then the libraries: `PubSubClient`, `ArduinoJson`,
   `Adafruit SSD1306`, `Adafruit GFX Library`.

### 6.1 Verify the AI pipeline + self-tests

```bash
.venv/bin/python scripts/run_tests.py
```
**Expected output** (screenshot-worthy — shows the project works end to end):

```
1) Tree export matches sklearn
  [PASS] tree export matches sklearn (max err 0.0000 min)
2) Arrival probability curve is sane
  [PASS] peak > night (peak 0.30 vs night 0.03)
  [PASS] probability in [0,1]
  [PASS] busy rate increases probability (0.46 > 0.30)
3) Station current never exceeds rating (predictive + hard limit)
  [PASS] hard limit respected (worst 64.0 A vs limit 64 A)
  [PASS] optimizer made decisions
4) Critical temperature -> bay paused (DEFER)
  [PASS] critical temp defers bay (DEFER, 0.0 A)
  [PASS] critical alarm raised
5) Command API
  [PASS] plugIn ok
  [PASS] plugIn on occupied bay fails
  [PASS] setMaxCurrent applied
  [PASS] fault locks bay
  [PASS] clearFault resumes
  [PASS] telemetry schema complete

Result: 14 passed, 0 failed
```

(Re)train fresh models if you like:

```bash
.venv/bin/python ai/generate_data.py     # regenerates ai/data/*.csv
.venv/bin/python ai/train_models.py      # retrains + exports models.h + joblib
```

### 6.2 Run the Python live demo (no hardware — recommended first demo)

```bash
.venv/bin/python scripts/live_demo.py
```

**What you see in the VS Code terminal** — one summary line every 2 s:

```
t=  42 Day 2 14:49       load  57.8% P=   8.54kW E=  12.31kWh AI=  33.1% | B1:IDLE  0.0A   0.0% DEFER     station_load_low_priority   B2:CHRG 17.3A  63.2% THROTTLE  predictive_reserve            B3:IDLE  ...
```

Read the columns: sim time, station load %, power, energy, AI arrival
probability, then per bay: state, current, SoC, decision, reason. You can watch
sessions start (AI predicts arrivals), currents get throttled at peak, and bays
deferred when the feeder is full.

Run variations:

```bash
.venv/bin/python scripts/live_demo.py --no-mqtt            # console only
.venv/bin/python scripts/live_demo.py --tb-token <TOKEN> --tb-host demo.thingsboard.io
```

### 6.3 See the dashboard (with the demo / or fully offline)

While `live_demo.py` (or the Wokwi firmware) is running:

- **Option A — double-click** `dashboard/index.html` (MQTT.js + demo feed work
  without any server).
- **Option B — static server** (VS Code integrated terminal):
  ```bash
  .venv/bin/python dashboard/serve.py
  ```
  then open **http://localhost:8080** in the browser.

**Expected dashboard behaviour:**

1. Top-right badge shows **LIVE · MQTT** within ~2 s (data flowing from the
   broker).
2. If you open the dashboard with *nothing* publishing → after ~12 s the badge
   flips to **DEMO FEED** and the page starts simulating on its own (the same
   schema, driven by `js/demo.js`). Perfect offline viva fallback.
3. Bay cards animate: SoC rings fill, currents climb/fall, decision chips switch
   ALLOW → THROTTLE → DEFER with the reason under each card.
4. Gauges: station load %, AI arrival probability. Charts: load %, power kW,
   arrival probability over time.
5. **Click "Plug in"** on a bay → command `plugIn` is published; with
   `live_demo.py` running you'll see `[CMD] plugIn {...} -> {...}` in its
   terminal, and the bay card lights up in the browser.
6. **Fault** button → bay locks, `FAULT` alarm banner appears
   (red LED in Wokwi). **Prio** cycles priority 1→3.

### 6.4 Run the actual firmware in Wokwi (VS Code)

> ⚠️ **Important:** Wokwi does **not** compile your sketch. `wokwi.toml`
> points to a **prebuilt binary** at `build/smart_ev_charger.ino.elf`
> (already included). Rebuild it after editing the firmware:

```bash
# from the project root (requires arduino-cli + esp32:esp32 core + the 4 libs)
arduino-cli compile --fqbn esp32:esp32:esp32 \
  --output-dir firmware/smart_ev_charger/build firmware/smart_ev_charger
```

1. Open the folder `firmware/smart_ev_charger` as the **VS Code workspace
   root** (File → Open Folder).
2. Press **F1 → "Wokwi: Start Simulator"** (Wokwi extension must be
   installed). No compiling happens in VS Code — it loads the prebuilt ELF.
3. **Expected result immediately:** the **Serial Monitor pane** shows boot
   logs at 115200 baud
   (`Smart Edge-AI EV Charging Station Optimizer …`), the **OLED** renders
   the station screen, and the bay LEDs light as sessions start.
4. WiFi: the extension's bundled **Private IoT Gateway** connects
   `Wokwi-GUEST` to the Internet, so MQTT really reaches `broker.emqx.io` —
   then `dashboard/index.html` shows **LIVE · MQTT** and its controls drive
   the simulated station through `emx/ev/cmd`.
5. (Optional, for full ThingsBoard uplink) edit `config.h`:
   ```cpp
   #define TB_ACCESS_TOKEN "PASTE_YOUR_DEVICE_ACCESS_TOKEN_HERE"
   ```
   then rebuild with the command above and restart the simulator.

> On **wokwi.com** (web), the free tier's virtual WiFi has no Internet access,
> so MQTT will not connect — but the OLED, LEDs and serial console still run
> and prove the firmware works. Open the dashboard's DEMO FEED for the live
> visuals, or use VS Code for real MQTT.

### 6.5 Verify the raw MQTT stream (debugging aid)

```bash
.venv/bin/python scripts/mqtt_probe.py
```
Prints every message on `emx/ev/#` (telemetry, attr state, status, cmd ack) and
proves the pipeline is alive independently of the dashboard.

---

## 7. Results you should be able to show / explain

| Demo step | Where to look | What proves it works |
|---|---|---|
| AI pipeline | `run_tests.py` output | 14/14 PASS, tree export error 0.0000 |
| On-device inference | firmware `ai_edge.h` + dashboard | arrival % varies by sim hour & busy rate; predicted duration matches session length |
| Optimizer decisions | dashboard chips + reasons, Python console | currents never exceed 64 A; bays THROTTLE at peak; lowest priority DEFERs first |
| Safety | fault/temp alarm banner + red LED | DEFER at 80 °C (or on fault); `TEMP_HIGH/OVERLOAD` codes |
| MQTT | `mqtt_probe.py`, ThingsBoard | telemetry every 2 s on both brokers |
| Dashboard | browser | LIVE badge, animated gauge/charts, plug/unplug round-trips |

---

## 8. Telemetry schema (flat keys)

Station keys: `uptime_s, firmware_version, station_total_power_kw,
station_total_current_a, station_load_pct, station_energy_kwh,
max_station_current_a, ambient_temp_c, optimizer_mode (AUTO|MANUAL), busy_rate,
sim_day, sim_hour, sim_time, arrival_probability_pct, predicted_arrivals,
charging_sessions_active, alarms_active, alarm_codes, wifi_rssi_db,
mqtt_tb, mqtt_mirror`

Per bay `bayN_*` (N = 1..3): `state (IDLE|CHARGING|DONE), soc, voltage_v,
current_a, power_kw, energy_kwh, temp_c, target_soc, predicted_min,
time_remaining_min, priority, decision (ALLOW|THROTTLE|DEFER), reason, vehicle`

Alarm codes: `TEMP_HIGH | TEMP_CRIT | OVERLOAD | VOLTAGE | FAULT` (pipe-joined).

> The **same schema** is produced by firmware, `live_demo.py`,
> `js/demo.js`, and consumed by the dashboard — this is the contract that keeps
> all four implementations interchangeable.

---

## 9. Likely viva Q&A (practice answers)

**Q: Why edge AI instead of cloud AI?**
Latency (control decisions every second), bandwidth (offline-tolerant), privacy
(no vehicle data leaves the site), and cost (no cloud inference bill).
The cloud is used for *visibility*, not *control*.

**Q: Why logistic regression and a decision tree — why not a neural net?**
They fit in kilobytes, need no huge runtime, are instantly explainable
("feature weights", "if temp > 80 then defer"), and train fast with scikit-learn.
A NN would give no interpretability advantage here and would not fit the
on-device constraint as elegantly.

**Q: How does the optimizer decide?**
Priority: (1) hard safety (temp, overload, fault), (2) thermal ladder throttling,
(3) predictive head-room reservation, (4) proportional current ratioing with
lowest-priority-first deferral. Hard limit is always enforced last as a
guarantee (even in MANUAL mode).

**Q: How are the models deployed to the ESP32?**
`train_models.py` prints C arrays into `models.h`; `ai_edge.h` contains the
logistic `sigmoid(w·x+b)` and a tree-walk. No ML library on the MCU — pure C.

**Q: What is the mirror broker?**
A public EMQX broker used so the browser dashboard (which speaks MQTT over
WebSockets) always has data even when ThingsBoard isn't configured. The device
publishes the same JSON to both places.

**Q: What would change for a real station?**
Replace the simulator's `step()` with sampling real CTs/NTCs; commands get
routed to real contactors; everything else (AI, optimizer, MQTT, dashboard) is
unchanged — that is the point of the hardware abstraction.

**Q: How do you test it?**
`run_tests.py` checks model-vs-C equality, threshold behaviour, hard-limit
invariant, command API and telemetry schema — all automated (14 checks).

**Q: How accurate is the AI?**
Arrival ROC-AUC 0.79; duration MAE ≈ 43 min, R² 0.91 on synthetic data.
For a viva, honest framing: the *pipeline* is the deliverable; accuracy scales
with real data.

**Q: What is CC/CV?**
Constant Current then Constant Voltage — the standard Li-ion charging profile
the simulator implements: current holds at the limit until voltage cap, then
tapers to ~0; the energy model integrates this curve.

---

## 10. Limitations

- Electrical/thermal values are **simulated** (deterministic PRNG), not
  measured; real CTs/NTCs would feed the same code path.
- Models trained on **synthetic data** — retraining with real logs improves
  accuracy; the pipeline is data-agnostic.
- Public broker (EMQX) is fine for demos; production would use the station's own
  broker with TLS + auth.
- Dashboard and firmware assume one site / one feeder model (3 bays); scaling to
  more bays is a config change + chart tweak.

---

## 11. Future scope

- Retrain on real telemetry (data flywheel; auto-retraining job).
- Add **ToU pricing** aware deferral (cheapest-slot scheduling).
- Bidirectional OCPP *if* interfacing commercial hardware; here MQTT model is
  simpler and standard for DIY/edge.
- Solar + battery input model (renewable-aware ALLOW/THROTTLE).
- Multi-station coordination over MQTT (fleet of feeders).
- Firmware OTA updates + real-time clock sync for true hour-of-day features.

---

## 12. Key files map

```
Final Project/
├── ai/
│   ├── generate_data.py      # synthetic datasets (arrivals/durations)
│   ├── train_models.py       # train + export models.h + joblib + metrics
│   ├── edge_ai.py            # Python inference wrappers (used by demo)
│   ├── data/*.csv            # generated datasets
│   └── models/{models.h,*.joblib,model_metadata.json,plots/}
├── optimizer/
│   └── optimizer.py          # Python Station + optimizer + telemetry schema
├── dashboard/
│   ├── index.html, css/style.css
│   ├── js/{app.js, demo.js, gauges.js, charts.js}
│   └── serve.py              # http://localhost:8080
├── firmware/smart_ev_charger/  # ESP32 firmware + Wokwi (diagram.json, wokwi.toml)
├── scripts/
│   ├── run_tests.py          # 14 automated checks
│   ├── live_demo.py          # full demo without hardware
│   └── mqtt_probe.py         # raw MQTT debug listener
├── docs/images/architecture.svg
├── requirements.txt
└── README.md                 # quick-start guide
```

---

## 13. Two-minute demo script (for the viva)

1. **Terminal 1**: `.venv/bin/python scripts/live_demo.py`
   → point at streaming console lines (sim clock, load %, decisions).
2. **Browser**: open `dashboard/index.html` (or `serve.py` → localhost:8080).
   → badge LIVE, gauges moving, bays charging.
3. **Interact**: press **Plug in** (terminal shows the command; card activates),
   press **Fault** on a bay (red alarm appears, bay DEFERs), press **Prio** twice
   (watch that bay get DEFERed first when load peaks).
4. **Wokwi** (if asked): start `firmware/smart_ev_charger` with the Wokwi
   extension → OLED + LEDs light; dashboard follows the same telemetry.
5. **Evidence**: run `scripts/run_tests.py` → 14/14 PASS. Show `models.h`
   (the exported AI) and mention ROC-AUC 0.79 / R² 0.91.