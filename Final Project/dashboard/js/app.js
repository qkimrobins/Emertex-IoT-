/* app.js — dashboard orchestration: MQTT client, demo-feed fallback, live UI. */
(function () {
  'use strict';

  const $ = sel => document.querySelector(sel);
  const $$ = sel => Array.from(document.querySelectorAll(sel));

  // ---------------- configuration ----------------
  const CFG = {
    broker: () => $('#ctlBroker').value.trim() || 'wss://broker.emqx.io:8084/mqtt',
    teleTopic: () => $('#ctlTeleTopic').value.trim() || 'emx/ev/telemetry',
    cmdTopic: () => $('#ctlCmdTopic').value.trim() || 'emx/ev/cmd'
  };

  const AV_TOPICS = ['emx/ev/telemetry', 'emx/ev/attr/state', 'emx/ev/status', 'emx/ev/cmd/ack'];

  // ---------------- state ----------------
  const S = {
    client: null,
    lastMsg: 0,
    tele: null,
    attr: null,
    status: null,
    demo: null,
    demoStarted: false,
    uptimeSec: 0,
    energyHist: [],
    employGauge: null,
    arrivalGauge: null,
    bayUI: [],
    loadChart: null,
    powerChart: null,
    arrivalChart: null
  };

  // ---------------- helpers ----------------
  const f1 = v => (+v).toFixed(1);
  const f2 = v => (+v).toFixed(2);
  const pad = n => String(n).padStart(2, '0');

  function fmtDur(min) {
    min = Math.max(0, Math.round(+min || 0));
    const h = Math.floor(min / 60), m = min % 60;
    return h > 0 ? `${h}h ${pad(m)}m` : `${m}m`;
  }

  function toast(msg, kind) {
    const t = $('#toast');
    t.textContent = msg;
    t.className = 'toast show' + (kind ? ' ' + kind : '');
    clearTimeout(t._t);
    t._t = setTimeout(() => (t.className = 'toast'), 2600);
  }

  // ---------------- build bay cards ----------------
  function buildBayCards() {
    const wrap = $('#bayCards');
    wrap.innerHTML = '';
    S.bayUI = [1, 2, 3].map(i => {
      const card = document.createElement('div');
      card.className = 'bay-card idle';
      card.innerHTML = `
        <div class="bay-head">
          <div>
            <div class="bay-vehicle" id="v${i}">— bay ${i} —</div>
            <div class="dim" style="font-size:11px">Bay ${i}</div>
          </div>
          <div class="bay-badges">
            <span class="mini-chip idle" id="st${i}">IDLE</span>
            <span class="mini-chip prio" id="pr${i}">P2</span>
          </div>
        </div>
        <div class="bay-mid">
          <div class="soc-ring" id="ring${i}"></div>
          <div class="bay-stats">
            <div><div class="lbl">Voltage</div><div class="val" id="V${i}">230.0 V</div></div>
            <div><div class="lbl">Current</div><div class="val" id="A${i}">0.0 A</div></div>
            <div><div class="lbl">Power</div><div class="val" id="kW${i}">0.00 kW</div></div>
            <div><div class="lbl">Temp</div><div class="val" id="T${i}">-- °C</div></div>
          </div>
        </div>
        <div class="bay-timer">
          <span><span class="dim">AI est.</span> <b id="aiMin${i}" class="mono">--</b></span>
          <span><span class="dim">left</span> <b id="left${i}" class="mono">--</b></span>
          <span class="mini-chip allow" id="dec${i}">ALLOW</span>
        </div>
        <div class="bay-reason" id="why${i}">idle</div>
        <div class="bay-controls">
          <select class="btn sm" id="sel${i}">${Object.keys(VEHICLE_NAMES()).map(n => `<option>${n}</option>`).join('')}</select>
          <button class="btn sm" id="plug${i}">Plug in</button>
          <button class="btn sm ghost" id="unplug${i}">Unplug</button>
          <button class="btn sm ghost" id="prio${i}">Prio ∘</button>
          <button class="btn sm danger" id="fault${i}">Fault</button>
        </div>`;
      wrap.appendChild(card);

      const ui = {
        card,
        els: {
          vehicle: card.querySelector(`#v${i}`), state: card.querySelector(`#st${i}`),
          prio: card.querySelector(`#pr${i}`), ring: card.querySelector(`#ring${i}`),
          V: card.querySelector(`#V${i}`), A: card.querySelector(`#A${i}`),
          kW: card.querySelector(`#kW${i}`), T: card.querySelector(`#T${i}`),
          aiMin: card.querySelector(`#aiMin${i}`), left: card.querySelector(`#left${i}`),
          dec: card.querySelector(`#dec${i}`), why: card.querySelector(`#why${i}`),
          sel: card.querySelector(`#sel${i}`)
        },
        ring: new window.SocRing(card.querySelector(`#ring${i}`))
      };

      card.querySelector(`#plug${i}`).onclick = () => sendCmd('plugIn', { bay: i, vehicle: ui.els.sel.value });
      card.querySelector(`#unplug${i}`).onclick = () => sendCmd('plugOut', { bay: i });
      card.querySelector(`#prio${i}`).onclick = () => sendCmd('setPriority', { bay: i, priority: nextPriority(i) });
      card.querySelector(`#fault${i}`).onclick = () => sendCmd('fault', { bay: i });
      return ui;
    });
  }

  /* vehicle list shared by the demo + firmware presets */
  function VEHICLE_NAMES() {
    return {
      'Tesla Model 3': 1, 'Tesla Model Y': 1, 'Hyundai IONIQ 5': 1, 'Nissan Leaf': 1,
      'VW ID.4': 1, 'Porsche Taycan': 1, 'BYD Atto 3': 1, 'Audi e-tron GT': 1
    };
  }

  const prioCycle = { 1: 3, 2: 1, 3: 2 };
  function nextPriority(bay) {
    const cur = S.tele ? (+S.tele[`bay${bay}_priority`] || 2) : 2;
    return prioCycle[cur] || 2;
  }

  // ---------------- render ----------------
  function renderTelemetry(m) {
    S.tele = m;
    const loadPct = +m.station_load_pct || 0;

    /* gauges + KPI */
    S.employGauge.set(loadPct);
    S.arrivalGauge.set(+m.arrival_probability_pct || 0);
    $('#kpiPower').innerHTML = f2(m.station_total_power_kw) + '<span class="unit">kW</span>';
    $('#powerBar').style.width = Math.min(100, loadPct) + '%';
    $('#kpiSessions').innerHTML =
      (+m.charging_sessions_active || 0) + '<span class="unit">/ 3</span>';
    $('#kpiEnergy').innerHTML = f2(m.station_energy_kwh) + '<span class="unit">kWh</span>';
    $('#loadCurrentTxt').textContent = f1(m.station_total_current_a) + ' A';
    $('#loadHudTxt').textContent = `load ${f1(loadPct)}% · cur ${f1(m.station_total_current_a)} A`;
    $('#powerHudTxt').textContent = `total ${f2(m.station_total_power_kw)} kW`;

    /* AI card */
    $('#aiProbTxt').textContent = f1(m.arrival_probability_pct) + ' %';
    $('#aiSessionsTxt').textContent = f2(m.predicted_arrivals);
    const reserve = (+m.predicted_arrivals || 0) * 16;
    $('#aiReserveTxt').textContent = f1(reserve) + ' A';
    $('#predArrival').textContent = f2(m.predicted_arrivals);

    /* bays */
    let active = 0;
    const dots = $('#bayDots');
    dots.innerHTML = '';
    [1, 2, 3].forEach(i => {
      const ui = S.bayUI[i - 1];
      const st = m[`bay${i}_state`], dec = m[`bay${i}_decision`], veh = m[`bay${i}_vehicle`] || '';
      const charging = st === 'CHARGING';

      if (charging) active++;
      ui.card.className = `bay-card ${!veh ? 'idle' : dec === 'ALLOW' ? 'charging' : dec === 'THROTTLE' ? 'throttle' : 'defer'}`;

      ui.els.state.textContent = st;
      ui.els.state.className = `mini-chip ${st === 'CHARGING' ? 'charging' : st === 'DONE' ? 'done' : 'idle'}`;
      ui.els.prio.textContent = 'P' + (m[`bay${i}_priority`] || 2);
      ui.els.vehicle.textContent = veh || `— bay ${i} —`;
      ui.els.V.textContent = f1(m[`bay${i}_voltage_v`]) + ' V';
      ui.els.A.textContent = f1(m[`bay${i}_current_a`]) + ' A';
      ui.els.kW.textContent = f2(m[`bay${i}_power_kw`]) + ' kW';
      const temp = +m[`bay${i}_temp_c`] || 0;
      ui.els.T.textContent = f1(temp) + ' °C';
      ui.els.T.className = 'val ' + (temp >= 65 ? 'crit' : temp >= 55 ? 'hot' : '');
      ui.els.aiMin.textContent = veh ? fmtDur(m[`bay${i}_predicted_min`]) : '--';
      ui.els.left.textContent = veh ? fmtDur(m[`bay${i}_time_remaining_min`]) : '--';
      ui.els.dec.textContent = dec || 'ALLOW';
      ui.els.dec.className = 'mini-chip ' + (dec || 'allow').toLowerCase();
      ui.els.why.textContent = (veh ? m[`bay${i}_reason`] : 'idle') + (st === 'DONE' ? ' · session complete' : '');
      ui.ring.set(+(m[`bay${i}_soc`] || 0), charging);

      const dot = document.createElement('span');
      if (charging) dot.className = dec === 'ALLOW' ? 'on' : dec === 'THROTTLE' ? 'thr' : 'defer';
      dots.appendChild(dot);

      if (!veh) ui.els.sel.value = 'Tesla Model 3';
    });

    const detail = active === 0 ? 'all bays idle' : `${active} bay(s) delivering power`;
    $('#sessionsDetail').textContent = detail;
    $('#baySumTxt').textContent = `${active}/3 busy · ${f2(m.station_total_power_kw)} kW`;

    /* decisions + load meter */
    const decList = $('#decisionList');
    decList.innerHTML = '';
    [1, 2, 3].forEach(i => {
      const st = m[`bay${i}_state`], dec = m[`bay${i}_decision`] || 'ALLOW';
      const veh = m[`bay${i}_vehicle`] || 'Bay ' + i;
      const row = document.createElement('div');
      row.className = 'decision-row';
      row.innerHTML = `
        <span class="d-bay">B${i}</span>
        <span style="flex:1">${veh}</span>
        <span class="decision-chip ${dec}">${dec}</span>
        <span class="d-why">${st === 'IDLE' ? 'idle' : (m[`bay${i}_reason`] || 'normal')}</span>`;
      decList.appendChild(row);
    });
    $('#loadMeterTxt').textContent = `${f1(m.station_total_current_a)} A / ${m.max_station_current_a} A`;
    $('#loadFill').style.width = Math.min(100, loadPct) + '%';
    const over = loadPct >= 90;
    $('#loadFill').style.background = over ? 'linear-gradient(90deg,#f87171,#fbbf24)' : 'linear-gradient(90deg,#22d3ee,#34d399)';

    /* alarms */
    const codes = (m.alarm_codes || '').split('|').filter(Boolean);
    const list = $('#alarmList');
    const tag = $('#alarmCountTag');
    if (!codes.length) {
      list.innerHTML = '<div class="alarm-empty">No active alarms — station nominal</div>';
      tag.textContent = '0 active';
      tag.className = 'tag tag-alarm zero';
    } else {
      tag.textContent = codes.length + ' active';
      tag.className = 'tag tag-alarm';
      list.innerHTML = '';
      codes.forEach(c => {
        const item = document.createElement('div');
        const warn = c.includes('TEMP_HIGH') || c === 'OVERLOAD';
        item.className = 'alarm-item' + (warn ? ' lvl-warn' : '');
        item.innerHTML = `<span class="a-dot"></span><b>${c}</b><span class="dim" style="margin-left:auto;font-family:var(--mono);font-size:11px">${alarmHint(c)}</span>`;
        list.appendChild(item);
      });
    }

    /* mode switch */
    $('#modeSwitch').querySelectorAll('.mode-opt').forEach(o =>
      o.classList.toggle('active', o.dataset.mode === (m.optimizer_mode || 'AUTO')));

    /* clock + uptime */
    if (m.sim_time) $('#simClock').textContent = m.sim_time;
    $('#ctlSimClock').textContent = m.sim_time + ' · day ' + m.sim_day;
    S.uptimeSec = +m.uptime_s || S.uptimeSec;

    /* charts */
    S.loadChart.push(loadPct, +m.charging_sessions_active || 0);
    S.powerChart.push([1, 2, 3].map(i => +m[`bay${i}_power_kw`] || 0));
    S.arrivalChart.push(+m.arrival_probability_pct || 0);
    S.energyHist.push(+m.station_energy_kwh || 0);

    /* live badge */
    if (m.source === 'demo') $('#simClock').parentElement && setBadge('demo');
  }

  function alarmHint(code) {
    const hints = {
      TEMP_HIGH: 'connector > 65 °C', TEMP_CRIT: 'connector > 80 °C — charging paused',
      OVERLOAD: 'load ≥ 90 % of rating', VOLTAGE: 'mains < 210 V', FAULT: 'bay fault locked'
    };
    return hints[code] || 'station alarm';
  }

  function renderAttrState(a) {
    if (!a) return;
    S.attr = a;
    if (a.optimizerMode) {
      $('#modeSwitch').querySelectorAll('.mode-opt').forEach(o =>
        o.classList.toggle('active', o.dataset.mode === a.optimizerMode));
    }
    if (a.maxStationCurrent_a) {
      $('#ctlMaxCurrent').value = a.maxStationCurrent_a;
      $('#ctlMaxCurrentTxt').textContent = a.maxStationCurrent_a + ' A';
      $('#kpiLoad').querySelector('.kpi-foot span.dim').textContent =
        `/ ${a.maxStationCurrent_a} A rated`;
    }
    if (a.busyRate) {
      $('#ctlBusyRate').value = a.busyRate;
      $('#ctlBusyRateTxt').textContent = f2(a.busyRate) + '×';
    }
  }

  function renderStatus(s) {
    if (!s) return;
    S.status = s;
    const tb = !!s.tb, mir = !!s.mirror;
    setChip($('#tbChip'), tb ? 'ok' : 'err', tb ? 'ThingsBoard' : 'ThingsBoard · off');
    setChip($('#mqttChip'), mir ? 'ok' : 'err', mir ? 'MQTT' : 'MQTT · no data');
  }

  function setChip(el, cls, label) {
    el.className = 'chip ' + cls;
    el.lastChild.textContent = label;
  }

  function setBadge(mode) {
    const b = $('#sourceBadge');
    if (mode === 'live') { b.textContent = 'LIVE · MQTT'; b.className = 'chip chip-demo live'; }
    else { b.textContent = 'DEMO FEED'; b.className = 'chip chip-demo'; }
  }

  // ---------------- history charts ----------------
  function drawHistoryCharts() {
    S.loadChart.draw(110);
    S.powerChart.draw(12);
    drawArrival();
    drawEnergySpark();
    requestAnimationFrame(drawHistoryCharts);
  }

  function drawArrival() {
    const vals = S.arrivalChart.values();
    if (vals.length < 2) return;
    const cv = $('#chartArrival');
    const dpr = window.devicePixelRatio || 1;
    const w = cv.clientWidth || 300;
    cv.width = w * dpr; cv.height = 110 * dpr;
    cv.style.height = '110px';
    const ctx = cv.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, 110);
    const xStep = w / (vals.length - 1);
    const yOf = v => 104 - (Math.max(0, Math.min(100, v)) / 100) * 92;
    ctx.beginPath();
    vals.forEach((v, i) => (i === 0 ? ctx.moveTo(i * xStep, yOf(v)) : ctx.lineTo(i * xStep, yOf(v))));
    ctx.strokeStyle = '#a78bfa'; ctx.lineWidth = 2;
    ctx.shadowColor = '#a78bfa'; ctx.shadowBlur = 7; ctx.stroke(); ctx.shadowBlur = 0;
  }

  function drawEnergySpark() {
    const cv = $('#sparkEnergy');
    const dpr = window.devicePixelRatio || 1;
    const w = cv.clientWidth || 200;
    cv.width = w * dpr; cv.height = 36 * dpr;
    cv.style.height = '36px';
    const ctx = cv.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, 36);
    const vals = S.energyHist;
    if (vals.length < 2) return;
    const mx = Math.max(...vals, 1);
    const xStep = w / (vals.length - 1);
    const yOf = v => 32 - (v / mx) * 28;
    ctx.beginPath();
    vals.forEach((v, i) => (i === 0 ? ctx.moveTo(i * xStep, yOf(v)) : ctx.lineTo(i * xStep, yOf(v))));
    ctx.strokeStyle = '#34d399'; ctx.lineWidth = 1.6; ctx.stroke();
  }

  // ---------------- commands ----------------
  function sendCmd(cmd, params) {
    if (S.demo && S.demo.active()) {
      S.demo.command(cmd, params || {});
      return;
    }
    if (S.client && S.client.connected) {
      S.client.publish(CFG.cmdTopic(), JSON.stringify({ cmd, params: params || {} }));
    } else {
      toast('MQTT not connected — trying demo feed', 'err');
      startDemo();
    }
  }

  // ---------------- demo feed fallback ----------------
  function startDemo() {
    if (S.demoStarted) return;
    S.demoStarted = true;
    setBadge('demo');
    S.demo = new window.DemoFeed();
    S.demo.onTelemetry = m => { S.lastMsg = Date.now(); renderTelemetry(m); };
    S.demo.onAttrState = a => renderAttrState(a);
    S.demo.onAck = r => { if (!r.ok) toast(r.detail || 'command failed', 'err'); };
    S.demo.start();
  }

  // ---------------- MQTT ----------------
  function connectMqtt() {
    if (S.client) { try { S.client.end(true); } catch (e) { /* noop */ } }
    setChip($('#mqttChip'), 'warn', 'MQTT · connecting…');
    const client = mqtt.connect(CFG.broker(), {
      connectTimeout: 8000,
      reconnectPeriod: 6000,
      clientId: 'ev-dash-' + Math.floor(Math.random() * 1e6)
    });
    S.client = client;

    client.on('connect', () => {
      setChip($('#mqttChip'), 'ok', 'MQTT');
      AV_TOPICS.forEach(t => { try { client.subscribe(t); } catch (e) { /* noop */ } });
      toast('Connected to MQTT broker', 'ok');
    });
    client.on('message', (topic, payload) => {
      S.lastMsg = Date.now();
      if (S.demo && S.demo.active()) S.demo.stop();
      try {
        const m = JSON.parse(payload.toString());
        if (topic.includes('telemetry')) {
          setBadge('live');
          renderTelemetry(m);
        } else if (topic.includes('attr')) {
          renderAttrState(m);
        } else if (topic.includes('status')) {
          renderStatus(m);
        } else if (topic.includes('ack')) {
          if (m && m.ok === false) toast(m.detail || 'command failed', 'err');
        }
      } catch (e) { /* ignore non-JSON */ }
    });
    client.on('error', err => {
      setChip($('#mqttChip'), 'err', 'MQTT · error');
    });
    client.on('close', () => setChip($('#mqttChip'), 'warn', 'MQTT · reconnecting'));
  }

  // ---------------- settings drawer ----------------
  function setupDrawer() {
    const open = () => { $('#drawer').classList.add('open'); $('#drawerScrim').classList.add('open'); };
    const close = () => { $('#drawer').classList.remove('open'); $('#drawerScrim').classList.remove('open'); };
    $('#btnSettings').onclick = open;
    $('#btnCloseDrawer').onclick = close;
    $('#drawerScrim').onclick = close;

    $('#ctlMaxCurrent').oninput = e => {
      $('#ctlMaxCurrentTxt').textContent = e.target.value + ' A';
    };
    $('#ctlMaxCurrent').onchange = e => sendCmd('setMaxCurrent', { amps: +e.target.value });
    $('#ctlBusyRate').oninput = e => {
      $('#ctlBusyRateTxt').textContent = f2(e.target.value) + '×';
    };
    $('#ctlBusyRate').onchange = e => sendCmd('setBusyRate', { rate: +e.target.value });
    $('#ctlAutoEvents').onchange = e => sendCmd('setAutoEvents', { on: e.target.checked });
    $('#btnResetEnergy').onclick = () => sendCmd('resetEnergy', {});
    $('#btnReconnect').onclick = () => { connectMqtt(); toast('Reconnecting MQTT…', 'ok'); };

    /* mode switch */
    $('#modeSwitch').addEventListener('click', e => {
      const opt = e.target.closest('.mode-opt');
      if (!opt) return;
      sendCmd('setMode', { mode: opt.dataset.mode });
    });
  }

  // ---------------- wiring the drawer sliders to attr state ----------------
  function sealSlidersFromAttr() {
    // keep local slider labels in sync with the actual attribute state
    const ki = setInterval(() => {
      if (S.attr && S.attr.maxStationCurrent_a) {
        $('#ctlMaxCurrent').value = S.attr.maxStationCurrent_a;
        $('#ctlMaxCurrentTxt').textContent = S.attr.maxStationCurrent_a + ' A';
      }
      if (S.attr && S.attr.busyRate) {
        $('#ctlBusyRate').value = S.attr.busyRate;
        $('#ctlBusyRateTxt').textContent = f2(S.attr.busyRate) + '×';
      }
    }, 2000);
    window._sliderSync = ki;
  }

  // ---------------- init ----------------
  function init() {
    buildBayCards();

    /* gauges */
    S.employGauge = new window.Gauge($('#gaugeLoad'), {
      min: 0, max: 120, format: v => Math.round(v) + '%',
      color: v => v >= 90 ? '#f87171' : v >= 75 ? '#fbbf24' : '#22d3ee'
    });
    S.arrivalGauge = new window.Gauge($('#gaugeArrival'), {
      min: 0, max: 100, format: v => Math.round(v) + '%',
      color: v => v >= 40 ? '#a78bfa' : '#34d399'
    });

    /* charts */
    const { Series, AreaChart, MultiLineChart } = window.ChartLib;
    S.loadChart = new AreaChart($('#chartLoad'), 190, {
      line: '#22d3ee', fill: 'rgba(34,211,238,.16)'
    });
    S.loadChart.secondary = { series: new Series(240), color: '#34d399', max: 3 };
    S.powerChart = new MultiLineChart($('#chartPower'), 190, [
      { color: '#22d3ee' }, { color: '#a78bfa' }, { color: '#fbbf24' }
    ]);
    const arrivalSeries = new Series(240);
    S.arrivalChart = arrivalSeries;
    S.loadChart.draw(110);
    S.powerChart.draw(12);

    setupDrawer();
    sealSlidersFromAttr();
    connectMqtt();

    /* graceful demo-feed fallback when MQTT stays silent */
    setInterval(() => {
      const idle = Date.now() - S.lastMsg > 12000;
      const wantDemo = $('#ctlDemoMode').checked;
      if (idle && wantDemo && !(S.demo && S.demo.active())) {
        startDemo();
        toast('No MQTT data — demo feed simulation started', 'ok');
      } else if (S.lastMsg > 1000 && !idle && S.demo && S.demo.active()) {
        S.demo.stop();
      }
    }, 4000);

    /* kick the clock */
    S.employGauge.set(0, false);
    S.arrivalGauge.set(0, false);
    drawHistoryCharts();
  }

  document.addEventListener('DOMContentLoaded', init);
})();