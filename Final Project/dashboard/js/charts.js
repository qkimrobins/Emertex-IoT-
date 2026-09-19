/* charts.js — dependency-free canvas charts (area + multi-line). */
(function () {
  'use strict';

  const DPR = () => (window.devicePixelRatio || 1);

  function setupCanvas(canvas, height) {
    const w = canvas.clientWidth || canvas.parentNode.clientWidth;
    const dpr = DPR();
    canvas.width = w * dpr;
    canvas.height = height * dpr;
    canvas.style.height = height + 'px';
    const ctx = canvas.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    return { ctx, w, h: height };
  }

  /**
   * RingBuffer with a fixed capacity.
   */
  class Series {
    constructor(capacity, initial = 0) {
      this.cap = capacity;
      this.buf = new Array(capacity).fill(initial); // oldest first
      this.len = 0;
    }
    push(v) {
      this.buf.push(v);
      if (this.buf.length > this.cap) this.buf.shift();
      this.len = this.buf.length;
    }
    values() { return this.buf; }
    last() { return this.buf[this.buf.length - 1]; }
  }

  /**
   * AreaChart — draws the station load history.
   */
  class AreaChart {
    constructor(canvas, height, opts) {
      this.canvas = canvas;
      this.height = height;
      this.o = Object.assign({ line: '#22d3ee', fill: 'rgba(34,211,238,.18)' }, opts);
      this.data = new Series(240);
      this.secondary = null;          // optional {series, color, label}
    }
    push(v, secondary) {
      this.data.push(v);
      if (this.secondary && secondary !== undefined) this.secondary.series.push(secondary);
    }
    draw(maxValue = 100) {
      const { ctx, w, h } = setupCanvas(this.canvas, this.height);
      const pad = 6;
      ctx.clearRect(0, 0, w, h);

      const vals = this.data.values();
      if (vals.length < 2) return;

      // horizontal gridlines
      ctx.strokeStyle = 'rgba(120,160,255,.08)';
      ctx.lineWidth = 1;
      for (let i = 0; i <= 3; i++) {
        const y = pad + (h - pad * 2) * (i / 3);
        ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
      }

      const xStep = w / (vals.length - 1);
      const yOf = v => h - pad - (Math.max(0, Math.min(maxValue, v)) / maxValue) * (h - pad * 2);

      // secondary (sessions) — soft violet line
      if (this.secondary) {
        const s = this.secondary.series.values();
        ctx.strokeStyle = this.secondary.color;
        ctx.lineWidth = 1.6;
        ctx.beginPath();
        s.forEach((v, i) => {
          const x = i * xStep, y = yOf(v / (this.secondary.max || 3) * maxValue);
          i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
        });
        ctx.stroke();
      }

      // area fill
      ctx.beginPath();
      vals.forEach((v, i) => { const x = i * xStep; i === 0 ? ctx.moveTo(x, yOf(v)) : ctx.lineTo(x, yOf(v)); });
      ctx.lineTo(w, h); ctx.lineTo(0, h); ctx.closePath();
      const grad = ctx.createLinearGradient(0, 0, 0, h);
      grad.addColorStop(0, this.o.fill);
      grad.addColorStop(1, 'rgba(34,211,238,0)');
      ctx.fillStyle = grad;
      ctx.fill();

      // line
      ctx.strokeStyle = this.o.line;
      ctx.lineWidth = 2;
      ctx.shadowColor = this.o.line;
      ctx.shadowBlur = 8;
      ctx.beginPath();
      vals.forEach((v, i) => { const x = i * xStep; i === 0 ? ctx.moveTo(x, yOf(v)) : ctx.lineTo(x, yOf(v)); });
      ctx.stroke();
      ctx.shadowBlur = 0;

      // last point marker
      const lx = xStep * (vals.length - 1), ly = yOf(vals[vals.length - 1]);
      ctx.fillStyle = this.o.line;
      ctx.beginPath(); ctx.arc(lx, ly, 3.2, 0, Math.PI * 2); ctx.fill();
    }
  }

  /**
   * MultiLineChart — power per bay.
   */
  class MultiLineChart {
    constructor(canvas, height, series) {
      this.canvas = canvas;
      this.height = height;
      this.series = series.map(s => ({ color: s.color, data: new Series(240) }));
    }
    push(values) {
      this.series.forEach((s, i) => s.data.push(values[i] !== undefined ? values[i] : 0));
    }
    draw(maxValue = 12) {
      const { ctx, w, h } = setupCanvas(this.canvas, this.height);
      const pad = 6;
      ctx.clearRect(0, 0, w, h);
      const n = this.series[0].data.values().length;
      if (n < 2) return;

      ctx.strokeStyle = 'rgba(120,160,255,.08)';
      for (let i = 0; i <= 3; i++) {
        const y = pad + (h - pad * 2) * (i / 3);
        ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
      }

      const xStep = w / (n - 1);
      const yOf = v => h - pad - (Math.max(0, Math.min(maxValue, v)) / maxValue) * (h - pad * 2);
      this.series.forEach(s => {
        ctx.strokeStyle = s.color; ctx.lineWidth = 1.8;
        ctx.shadowColor = s.color; ctx.shadowBlur = 6;
        ctx.beginPath();
        s.data.values().forEach((v, i) => {
          const x = i * xStep; i === 0 ? ctx.moveTo(x, yOf(v)) : ctx.lineTo(x, yOf(v));
        });
        ctx.stroke();
        ctx.shadowBlur = 0;
      });
    }
  }

  window.ChartLib = { Series, AreaChart, MultiLineChart };
})();