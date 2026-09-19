/* demo.js — self-contained station simulator used as a fallback "live feed"
   when no MQTT telemetry arrives (great for offline demos / the viva).
   Re-implements the same physics + optimizer + Edge-AI logic as the firmware. */
(function () {
  'use strict';

  const VEHICLES = {
    'Tesla Model 3':   { cap: 60, s0: [15,45], st: 90, cur: 32 },
    'Tesla Model Y':   { cap: 75, s0: [20,50], st: 90, cur: 32 },
    'Hyundai IONIQ 5': { cap: 77, s0: [25,55], st: 95, cur: 26 },
    'Nissan Leaf':     { cap: 40, s0: [20,60], st: 100, cur: 32 },
    'VW ID.4':         { cap: 82, s0: [10,50], st: 90, cur: 30 },
    'Porsche Taycan':  { cap: 93, s0: [15,45], st: 85, cur: 32 },
    'BYD Atto 3':      { cap: 60, s0: [20,50], st: 95, cur: 32 },
    'Audi e-tron GT':  { cap: 85, s0: [10,40], st: 90, cur: 32 }
  };
  const NAMES = Object.keys(VEHICLES);

  const LOGREG_COEF = [-0.170076, -0.384985, -0.015609, 0.335676, 2.138414, 1.33314];
  const LOGREG_BIAS = -4.492506;

  const TEMP_WARN = 55, TEMP_HIGH = 65, TEMP_CRIT = 80, MIN_A = 6;

  function sigmoid(z) {
    return z >= 0 ? 1 / (1 + Math.exp(-z)) : Math.exp(z) / (1 + Math.exp(z));
  }

  /** Edge-AI arrival probability — identical feature engineering to the device. */
  function arrivalProb(hour, weekday, busyRate) {
    const rad = 2 * Math.PI * hour / 24;
    const wd = weekday % 7;
    const isWeekend = wd >= 5 ? 1 : 0;
    const isPeak = (hour >= 6.5 && hour <= 9.5) || (hour >= 16 && hour <= 19.5) ? 1 : 0;
    const x = [Math.sin(rad), Math.cos(rad), wd, isWeekend, isPeak, busyRate];
    let z = LOGREG_BIAS;
    for (let i = 0; i < 6; i++) z += LOGREG_COEF[i] * x[i];
    return sigmoid(z);
  }

  /** Physics-based charging-duration estimate (mirrors the training data model). */
  function chargeMinutes(cap, soc0, socT, ambient, cur, connTemp) {
    const pKw = (230 * cur * 0.98 / 1000) * 0.92;
    const e = cap * (socT - soc0) / 100;
    const cc = Math.max(0, Math.min(1, (80 - soc0) / (socT - soc0)));
    let t = e * cc / pKw * 60 + e * (1 - cc) / (pKw * 0.45) * 60;
    t *= 1 + Math.max(0, connTemp - 65) * 0.02;
    t *= 1 + Math.max(0, 12 - ambient) * 0.01;
    return Math.max(5, Math.min(720, t));
  }

  class Bay {
    constructor(i) { this.index = i; this.priority = i; this.reset(); }
    reset() {
      this.present = false; this.vehicle = ''; this.state = 'IDLE';
      this.soc = 0; this.targetSoc = 90; this.capacity = 60;
      this.voltageV = 230; this.currentA = 0; this.tempC = 25;
      this.powerKw = 0; this.energyKwh = 0; this.limit = 32;
      this.allowedA = 0; this.decision = 'ALLOW'; this.reason = 'idle';
      this.predictedMin = 0; this.timeRemaining = 0; this.enabled = true;
    }
  }

  class DemoStation {
    constructor() {
      this.bays = [new Bay(1), new Bay(2), new Bay(3)];
      this.maxCurrent = 64; this.mode = 'AUTO'; this.busyRate = 1;
      this.autoEventsOn = true;
      this.ambient = 22; this.day = 2; this.minute = 8 * 60;
      this.stationEnergy = 0; this.prob = 0.05; this.predictedArrivals = 0;
      this.alarms = [];
      this.t = 0;
    }

    plugIn(i, vehicle, soc0, socT, prio) {
      const b = this.bays[i - 1];
      if (b.present) return false;
      const spec = VEHICLES[vehicle] || VEHICLES['Tesla Model 3'];
      b.vehicle = vehicle;
      b.capacity = spec.cap;
      b.targetSoc = socT || spec.st;
      b.soc = soc0 != null ? soc0 : spec.s0[0] + Math.random() * (spec.s0[1] - spec.s0[0]);
      b.limit = spec.cur;
      b.priority = prio || (1 + Math.floor(Math.random() * 3));
      b.present = true; b.state = 'CHARGING';
      b.energyKwh = 0; b.tempC = this.ambient + 2;
      b.allowedA = b.limit; b.decision = 'ALLOW'; b.reason = 'normal';
      b.predictedMin = chargeMinutes(b.capacity, b.soc, b.targetSoc, this.ambient, b.limit, b.tempC);
      b.timeRemaining = b.predictedMin;
      return true;
    }
    plugOut(i) {
      const b = this.bays[i - 1];
      if (!b.present) return false;
      b.reset(); b.index = i; return true;
    }

    step(dtMin) {
      this.t += dtMin;
      this.minute += dtMin;
      if (this.minute >= 1440) { this.minute -= 1440; this.day = (this.day + 1) % 7; }
      this.ambient = 22 + 4 * Math.sin((this.minute / 60 - 9) / 24 * 2 * Math.PI);
      this.prob = arrivalProb(this.minute / 60, this.day, this.busyRate);

      let totalA = 0, totalW = 0;
      for (const b of this.bays) {
        if (!b.present) { b.currentA = 0; b.powerKw = 0; b.state = 'IDLE'; continue; }
        if (b.state === 'CHARGING') {
          const taper = b.soc >= 80 ? Math.max(0.05, (100 - b.soc) / 20) : 1;
          b.currentA = b.allowedA * taper;
          if (b.currentA < 0.05) b.currentA = 0;
          b.voltageV = 230 - 0.05 * totalA + (Math.random() * 2 - 1);
          const pW = b.voltageV * b.currentA * 0.98 * 0.92;
          b.powerKw = pW / 1000;
          const dWh = pW * dtMin / 60;
          b.energyKwh += dWh / 1000;
          b.soc += dWh / (b.capacity * 1000) * 100;
          if (b.soc >= b.targetSoc) {
            b.soc = b.targetSoc; b.state = 'DONE'; b.currentA = 0; b.powerKw = 0;
          } else {
            b.timeRemaining = b.capacity * 1000 * (b.targetSoc - b.soc) / 100 / Math.max(pW, 1) * 60;
          }
        } else { b.currentA = 0; b.powerKw = 0; }
        const teq = this.ambient + 5 + 0.02 * b.currentA * b.currentA;
        b.tempC += (teq - b.tempC) * Math.min(1, dtMin / 1.5);
        totalA += b.currentA; totalW += b.powerKw * 1000;
      }
      this.stationEnergy += totalW / 1000 * dtMin / 60;
      this.optimize();
      this.autoEventsTick(dtMin);
    }

    optimize() {
      this.alarms = [];
      for (const b of this.bays) {
        b.allowedA = b.limit; b.decision = 'ALLOW'; b.reason = 'normal';
        if (!b.present) { b.allowedA = 0; b.reason = 'idle'; continue; }
        if (!b.enabled) { b.allowedA = 0; b.decision = 'DEFER'; b.reason = 'disabled'; continue; }
        if (b.state === 'DONE') { b.allowedA = 0; b.reason = 'session_complete'; continue; }
        if (b.tempC >= TEMP_CRIT) { b.allowedA = 0; b.decision = 'DEFER'; b.reason = 'temp_critical'; this.alarms.push('TEMP_CRIT'); continue; }
        if (b.tempC >= TEMP_HIGH) { b.allowedA *= 0.4; b.decision = 'THROTTLE'; b.reason = 'temp_high'; this.alarms.push('TEMP_HIGH'); continue; }
        if (b.tempC >= TEMP_WARN) { b.allowedA *= 0.75; b.decision = 'THROTTLE'; b.reason = 'temp_warm'; }
      }

      if (this.mode === 'AUTO') {
        const free = this.bays.filter(b => !b.present).length;
        this.predictedArrivals = this.prob * free;
        const rsv = this.predictedArrivals * 16;
        let running = this.bays.filter(b => b.state === 'CHARGING');
        for (let iter = 0; iter < 4 && running.length; iter++) {
          const req = running.reduce((s, b) => s + b.allowedA, 0);
          const avail = this.maxCurrent - rsv;
          if (req <= avail + 0.01) break;
          const scale = avail / Math.max(req, 0.01);
          running.forEach(b => { b.allowedA *= scale; b.decision = 'THROTTLE'; b.reason = 'station_load'; });
          const below = running.filter(b => b.allowedA < MIN_A);
          if (below.length) {
            below.sort((a, b) => (b.priority - a.priority) || (a.index - b.index));
            const victim = below[0];
            victim.allowedA = 0; victim.decision = 'DEFER'; victim.reason = 'station_load_low_priority';
            running = running.filter(b => b !== victim);
          } else break;
        }
      }
      // hard fuse
      for (let iter = 0; iter < 4; iter++) {
        const tot = this.bays.filter(b => b.state === 'CHARGING').reduce((s, b) => s + b.allowedA, 0);
        if (tot <= this.maxCurrent + 0.01) break;
        for (const b of this.bays.filter(x => x.state === 'CHARGING')) {
          const cut = Math.min(b.allowedA, (tot - this.maxCurrent) * b.allowedA / Math.max(tot, 0.01));
          b.allowedA -= cut;
          if (b.allowedA < MIN_A) { b.allowedA = 0; b.decision = 'DEFER'; b.reason = 'hard_limit'; break; }
        }
      }
      const loadPct = 100 * this.bays.reduce((s, b) => s + b.currentA, 0) / this.maxCurrent;
      if (loadPct >= 90) this.alarms.push('OVERLOAD');
      this.alarms = [...new Set(this.alarms)];
    }

    autoEventsTick(dtMin) {
      if (!this.autoEventsOn) return;
      this._ev -= dtMin;
      if (this._ev > 0) return;
      const free = this.bays.filter(b => !b.present);
      const pMin = this.prob / 15;
      if (free.length && Math.random() < pMin) {
        const b = free[Math.floor(Math.random() * free.length)];
        this.plugIn(b.index, NAMES[Math.floor(Math.random() * NAMES.length)]);
        this._ev = 4 + Math.random() * 9;
      } else {
        this._ev = 1.5 + Math.random() * 4;
      }
    }
  }
  DemoStation.prototype._ev = 6;

  function telemetry(st) {
    const hour = st.minute / 60;
    const totalA = st.bays.reduce((s, b) => s + b.currentA, 0);
    const kW = st.bays.reduce((s, b) => s + b.powerKw, 0);
    const msg = {
      uptime_s: Math.round(st.t * 60),
      station_total_power_kw: +kW.toFixed(2),
      station_total_current_a: +totalA.toFixed(1),
      station_load_pct: +(100 * totalA / st.maxCurrent).toFixed(1),
      station_energy_kwh: +st.stationEnergy.toFixed(2),
      max_station_current_a: st.maxCurrent,
      ambient_temp_c: +st.ambient.toFixed(1),
      optimizer_mode: st.mode,
      busy_rate: st.busyRate,
      sim_day: st.day,
      sim_hour: +hour.toFixed(1),
      sim_time: fmtClock(st),
      arrival_probability_pct: +(100 * st.prob).toFixed(1),
      predicted_arrivals: +st.predictedArrivals.toFixed(2),
      charging_sessions_active: st.bays.filter(b => b.state === 'CHARGING').length,
      alarms_active: st.alarms.length,
      alarm_codes: st.alarms.join('|'),
      wifi_rssi_db: -58,
      firmware_version: 'demo-js',
      source: 'demo'
    };
    st.bays.forEach(b => {
      msg[`bay${b.index}_state`] = b.state;
      msg[`bay${b.index}_soc`] = +b.soc.toFixed(1);
      msg[`bay${b.index}_voltage_v`] = +b.voltageV.toFixed(1);
      msg[`bay${b.index}_current_a`] = +b.currentA.toFixed(1);
      msg[`bay${b.index}_power_kw`] = +b.powerKw.toFixed(2);
      msg[`bay${b.index}_energy_kwh`] = +b.energyKwh.toFixed(2);
      msg[`bay${b.index}_temp_c`] = +b.tempC.toFixed(1);
      msg[`bay${b.index}_target_soc`] = b.targetSoc;
      msg[`bay${b.index}_predicted_min`] = Math.round(b.predictedMin);
      msg[`bay${b.index}_time_remaining_min`] = Math.round(b.timeRemaining);
      msg[`bay${b.index}_priority`] = b.priority;
      msg[`bay${b.index}_decision`] = b.decision;
      msg[`bay${b.index}_reason`] = b.reason;
      msg[`bay${b.index}_vehicle`] = b.vehicle;
    });
    return msg;
  }

  function fmtClock(st) {
    const h = Math.floor(st.minute / 60);
    const m = Math.floor(st.minute % 60);
    const days = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'];
    return `${days[st.day % 7]} ${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}`;
  }

  function applyCommand(st, cmd, params) {
    const bay = (params && 'bay' in params) ? +params.bay : 1;
    const resp = { cmd, ok: true, bay, detail: 'done' };
    switch (cmd) {
      case 'plugIn':
        resp.ok = st.plugIn(bay, params.vehicle || 'Tesla Model 3', params.soc0, params.socT, params.priority);
        resp.detail = resp.ok ? 'EV plugged in' : 'bay already occupied';
        break;
      case 'plugOut':
        resp.ok = st.plugOut(bay);
        resp.detail = resp.ok ? 'EV unplugged' : 'bay already free';
        break;
      case 'setEnabled': st.bays[bay - 1].enabled = !!params.enabled; break;
      case 'setMode': st.mode = (params.mode || 'AUTO').toUpperCase() === 'MANUAL' ? 'MANUAL' : 'AUTO'; break;
      case 'setMaxCurrent': st.maxCurrent = +params.amps || 64; break;
      case 'setBayLimit': st.bays[bay - 1].limit = +params.amps || 16; break;
      case 'setPriority': st.bays[bay - 1].priority = +params.priority || 2; break;
      case 'setBusyRate': st.busyRate = +params.rate || 1; break;
      case 'setAutoEvents': st.autoEventsOn = !!params.on; break;
      case 'resetEnergy': st.stationEnergy = 0; break;
      default: resp.ok = false; resp.detail = 'unknown cmd: ' + cmd;
    }
    return resp;
  }

  class DemoFeed {
    constructor() {
      this.st = null;
      this.onTelemetry = null;
      this.onAttrState = null;
      this.onAck = null;
      this.timer = null;
    }
    start() {
      if (this.timer) return;
      this.st = new DemoStation();
      this.st.plugIn(2, 'Nissan Leaf');
      this.timer = setInterval(() => {
        this.st.step(2);                       // 2 sim minutes per real tick
        const msg = telemetry(this.st);
        if (this.onTelemetry) this.onTelemetry(msg);
        if (this.onAttrState) this.onAttrState(this.attrState());
      }, 2000);
      if (this.onAttrState) this.onAttrState(this.attrState());
    }
    stop() { if (this.timer) { clearInterval(this.timer); this.timer = null; } }
    active() { return !!this.timer; }
    attrState() {
      return {
        optimizerMode: this.st.mode,
        maxStationCurrent_a: this.st.maxCurrent,
        busyRate: this.st.busyRate,
        autoEvents: this.st.autoEventsOn,
        source: 'demo'
      };
    }
    command(cmd, params) {
      const resp = applyCommand(this.st, cmd, params || {});
      // echo the touched attr state + ack so the UI behaves identically live/demo
      if (this.onAttrState) this.onAttrState(this.attrState());
      if (this.onAck) this.onAck(resp);
    }
  }

  window.DemoFeed = DemoFeed;
})();