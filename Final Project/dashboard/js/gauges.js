/* gauges.js — SVG arc gauges + SOC rings with animated values. */
(function () {
  'use strict';

  const TAU = Math.PI * 2;

  /** Convert a value in [min,max] to an SVG angle in the -120°..+120° arc. */
  function valueToAngle(v, min, max) {
    const clamped = Math.max(min, Math.min(max, v));
    const frac = (clamped - min) / (max - min); // 0..1
    return -120 + frac * 240;                   // degrees
  }

  function polar(cx, cy, r, deg) {
    const rad = (deg - 90) * Math.PI / 180;
    return { x: cx + r * Math.cos(rad), y: cy + r * Math.sin(rad) };
  }

  function arcPath(cx, cy, r, a0, a1) {
    // arc from a0 to a1 (degrees). Approximate with a circular arc using
    // large-arc flag when the sweep exceeds 180°.
    if (a1 <= a0) return '';
    const p0 = polar(cx, cy, r, a0);
    const p1 = polar(cx, cy, r, a1);
    const large = (a1 - a0) > 180 ? 1 : 0;
    return `M ${p0.x.toFixed(2)} ${p0.y.toFixed(2)} A ${r} ${r} 0 ${large} 1 ${p1.x.toFixed(2)} ${p1.y.toFixed(2)}`;
  }

  /**
   * Gauge — 240° arc with an animated needle arc + center value.
   * el:      container element
   * opts:    {min, max, format(v), color(v), label}
   */
  class Gauge {
    constructor(el, opts) {
      this.el = el;
      this.o = Object.assign({ min: 0, max: 100, format: v => v.toFixed(0), color: () => '#22d3ee' }, opts);
      this.value = this.o.min;

      const w = 120, h = 64, cx = 60, cy = 58, r = 48;
      el.innerHTML =
        `<svg viewBox="0 0 ${w} ${h}">
           <path class="g-track" d="${arcPath(cx, cy, r, -120, 120)}" fill="none" stroke="rgba(120,160,255,.14)" stroke-width="7" stroke-linecap="round"/>
           <path class="g-fill" d="" fill="none" stroke="#22d3ee" stroke-width="7" stroke-linecap="round"/>
           <line class="g-needle" x1="0" y1="0" x2="0" y2="0" stroke="#e6edf7" stroke-width="2" stroke-linecap="round"/>
           <circle class="g-hub" cx="${cx}" cy="${cy}" r="4" fill="#22d3ee"/>
         </svg>
         <div class="g-value"></div>`;
      this.fill = el.querySelector('.g-fill');
      this.needle = el.querySelector('.g-needle');
      this.hub = el.querySelector('.g-hub');
      this.valEl = el.querySelector('.g-value');
      this.cx = cx; this.cy = cy; this.r = r;
    }

    set(v, animate = true) {
      this.value = v;
      const a = valueToAngle(v, this.o.min, this.o.max);
      const target = arcPath(this.cx, this.cy, this.r, -120, a);
      const fromRad = a * Math.PI / 180;
      const tip = polar(this.cx, this.cy, this.r - 14, a);
      // needle from hub to tip
      this.needle.setAttribute('x1', this.cx);
      this.needle.setAttribute('y1', this.cy);
      this.needle.setAttribute('x2', tip.x.toFixed(2));
      this.needle.setAttribute('y2', tip.y.toFixed(2));

      const color = this.o.color(v);
      this.fill.setAttribute('stroke', color);
      this.hub.setAttribute('fill', color);
      this.needle.setAttribute('stroke', color);
      this.valEl.textContent = this.o.format(v);
      this.valEl.style.color = color;

      if (!animate) { this.fill.setAttribute('d', target); return; }
      // simple transition by re-requesting (CSS transition needs a stable path,
      // so we animate the dash offset instead of the path). Fallback: snap.
      this.fill.setAttribute('d', target);
      this.fill.style.transition = 'none';
    }
  }

  /**
   * SocRing — circular progress ring for a bay.
   */
  class SocRing {
    constructor(el) {
      const R = 30, C = 2 * Math.PI * R;
      el.innerHTML =
        `<svg viewBox="0 0 74 74">
           <circle class="ring-bg" cx="37" cy="37" r="${R}" fill="none" stroke-width="6"/>
           <circle class="ring-fill" cx="37" cy="37" r="${R}" fill="none" stroke-width="6"
                   stroke-dasharray="${C}" stroke-dashoffset="${C}"/>
         </svg>
         <div class="soc-txt">--</div>`;
      this.circ = C;
      this.fill = el.querySelector('.ring-fill');
      this.txt = el.querySelector('.soc-txt');
    }
    set(soc, charging) {
      const f = Math.max(0, Math.min(1, soc / 100));
      this.fill.setAttribute('stroke-dashoffset', (this.circ * (1 - f)).toFixed(1));
      const color = (soc >= 90) ? '#34d399' : charging ? '#22d3ee' : '#4b5a78';
      this.fill.setAttribute('stroke', color);
      this.txt.textContent = soc.toFixed(0) + '%';
      this.txt.style.color = color;
    }
  }

  window.Gauge = Gauge;
  window.SocRing = SocRing;
})();