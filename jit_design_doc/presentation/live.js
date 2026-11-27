/* ============================================================
   EJIT deck — live animations. Each module: {start(slide), stop(), step(n)}
   hero  : canvas timeline, a noisy signal freezing inside windows
   flows : generic packets riding SVG paths (data-path / data-run / data-dur / data-delay)
   ring  : MPSC ring queue simulation (canvas)
   pool  : W^X code pool pages (svg built here)
   ============================================================ */
(() => {
  'use strict';
  const reg = window.__live = window.__live || {};
  const $ = (s, r = document) => r.querySelector(s);
  const $$ = (s, r = document) => Array.from(r.querySelectorAll(s));
  const css = (n) => getComputedStyle(document.documentElement).getPropertyValue(n).trim();
  const palette = () => ({
    bg: css('--bg'), bg2: css('--bg-2'), bg3: css('--bg-3'), ink: css('--ink'), ink2: css('--ink-2'), ink3: css('--ink-3'), ink4: css('--ink-4'),
    amber: css('--amber'), ice: css('--ice'), violet: css('--violet'), red: css('--red'), lime: css('--lime'), line: css('--line-2'),
  });
  const hexA = (hex, a) => {
    const n = parseInt(hex.slice(1), 16);
    return `rgba(${(n >> 16) & 255},${(n >> 8) & 255},${n & 255},${a})`;
  };
  const mulberry = (a) => () => { a |= 0; a = (a + 0x6D2B79F5) | 0; let t = Math.imul(a ^ (a >>> 15), 1 | a); t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; };
  const ease = (k) => 1 - Math.pow(1 - Math.max(0, Math.min(1, k)), 3);
  const mono = (w, s) => `${w} ${s}px "EJITMono","EJITSans",monospace`;
  const sans = (w, s) => `${w} ${s}px "EJITSans",sans-serif`;
  const still = () => document.documentElement.classList.contains('no-motion') ||
    (window.matchMedia ? matchMedia('(prefers-reduced-motion: reduce)').matches : false);

  /* ================= hero ================= */
  reg.hero = (() => {
    let cv, ctx, raf, t0, P, variant, segs, wins;
    const X0 = 96, X1 = 1504, BT = 500, BB = 784, Y0 = 674, LV = 26, T = 12000, LOOP = 900;
    const WIN = {
      title: [
        { a: 372, b: 700, tag: 'cellType = 0xFD' },
        { a: 948, b: 1300, tag: 'mode = 0 · gain = 7' },
      ],
      end: [
        { a: 470, b: 1120, tag: '窗口内 · 常量已知 · 跑的是特化代码' },
      ],
    };
    function build(seed) {
      const rnd = mulberry(seed), out = [];
      let x = X0, L = 1;
      while (x < X1) { const len = 90 + rnd() * 150; out.push({ x1: Math.min(X1, x + len), L }); x += len; const nx = [-2, -1, 0, 1, 2].filter((v) => Math.abs(v - L) >= 1); L = nx[Math.floor(rnd() * nx.length)]; }
      return out;
    }
    function level(x) {
      for (let i = 0; i < segs.length; i++) {
        const s = segs[i];
        if (x < s.x1) {
          const nx = segs[i + 1];
          if (nx && x > s.x1 - 16) return s.L + (nx.L - s.L) * ((x - (s.x1 - 16)) / 16);
          return s.L;
        }
      }
      return segs[segs.length - 1].L;
    }
    const wiggle = (x) => 3.2 * Math.sin(x * 0.21) + 2.4 * Math.sin(x * 0.077 + 1.3) + 1.6 * Math.sin(x * 0.51 + 0.4);
    function yOf(x) {
      for (const w of wins) {
        if (x >= w.a && x <= w.b) return Y0 - w.L * LV;
        if (x >= w.a - 54 && x < w.a) { const k = (x - (w.a - 54)) / 54; return (Y0 - level(x) * LV + wiggle(x)) * (1 - k) + (Y0 - w.L * LV) * k; }
      }
      return Y0 - level(x) * LV + wiggle(x);
    }
    function draw(now) {
      const tt = now - t0, cyc = Math.floor(tt / (T + LOOP)), t = tt - cyc * (T + LOOP);
      const ph = X0 + (Math.min(t, T) / T) * (X1 - X0);
      const px = Math.min(ph, X1);
      ctx.clearRect(0, 0, 1600, 900);
      const edgeTime = (ex) => t0 + cyc * (T + LOOP) + ((ex - X0) / (X1 - X0)) * T;
      // windows
      wins.forEach((w) => {
        ctx.fillStyle = hexA(P.ice, 0.075); ctx.fillRect(w.a, BT, w.b - w.a, BB - BT);
        ctx.strokeStyle = hexA(P.ice, 0.55); ctx.lineWidth = 1.5;
        ctx.beginPath(); ctx.moveTo(w.a + 0.5, BT); ctx.lineTo(w.a + 0.5, BB); ctx.moveTo(w.b - 0.5, BT); ctx.lineTo(w.b - 0.5, BB); ctx.stroke();
        [w.a, w.b].forEach((ex, j) => {
          const d = now - edgeTime(ex);
          if (d >= 0 && d < 700) {
            const k = 1 - d / 700;
            ctx.strokeStyle = j === 0 ? hexA(P.ice, 0.9 * k + 0.1) : hexA(P.red, 0.9 * k + 0.1);
            ctx.lineWidth = 10 * k + 1.5;
            ctx.beginPath(); ctx.moveTo(ex, BT); ctx.lineTo(ex, BB); ctx.stroke();
            ctx.beginPath(); ctx.arc(ex, yOf(ex), 6 + 26 * (1 - k), 0, Math.PI * 2);
            ctx.strokeStyle = j === 0 ? hexA(P.ice, k) : hexA(P.red, k); ctx.lineWidth = 2; ctx.stroke();
          }
        });
        const passed = px > w.a;
        ctx.font = mono(500, 13); ctx.textAlign = 'left'; ctx.textBaseline = 'alphabetic';
        ctx.fillStyle = hexA(P.ice, passed ? 0.95 : 0.4);
        ctx.fillText('ejit_activate("cell", 3)', w.a + 12, BT - 12);
        ctx.textAlign = 'right';
        ctx.fillStyle = px > w.b ? P.red : hexA(P.ink3, 0.9);
        ctx.fillText('ejit_deactivate', w.b - 12, BB - 12);
        ctx.fillStyle = hexA(P.ice, passed ? 0.95 : 0.4); ctx.font = sans(500, 14);
        ctx.fillText(variant === 'title' ? '窗口内：值不变 → 可视为编译期常量' : w.tag, w.b - 12, BT + 28);
      });
      // captions outside windows
      ctx.font = sans(500, 14); ctx.textAlign = 'left'; ctx.fillStyle = hexA(P.amber, 0.95);
      ctx.fillText('AOT：每次都读内存、每次都判分支', X0 + 4, BT + 28);
      ctx.textAlign = 'right'; ctx.fillText('窗口外 → 回到 AOT', X1, BT + 28);
      // axis
      ctx.strokeStyle = hexA(P.ink, 0.22); ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(X0, BB + 0.5); ctx.lineTo(X1, BB + 0.5); ctx.stroke();
      for (let x = X0; x <= X1; x += 88) { ctx.beginPath(); ctx.moveTo(x + 0.5, BB); ctx.lineTo(x + 0.5, BB + 7); ctx.stroke(); }
      ctx.font = mono(500, 12); ctx.fillStyle = P.ink3;
      ctx.textAlign = 'right'; ctx.fillText('t  →', X1, BB + 26);
      ctx.textAlign = 'left'; ctx.fillText('运行时', X0, BB + 26);
      // trace
      const drawTrace = (xa, xb, alpha) => {
        ctx.lineJoin = 'round'; ctx.lineCap = 'round';
        for (const [col, lw, inWin] of [[P.amber, 2.2, false], [P.ice, 3, true]]) {
          ctx.strokeStyle = hexA(col, alpha); ctx.lineWidth = lw;
          ctx.beginPath(); let pen = false;
          for (let x = xa; x <= xb; x += 1.5) {
            const w = wins.find((w) => x >= w.a && x <= w.b);
            if ((w ? 1 : 0) !== (inWin ? 1 : 0)) { pen = false; continue; }
            const y = yOf(x);
            if (!pen) { ctx.moveTo(x, y); pen = true; } else ctx.lineTo(x, y);
          }
          ctx.stroke();
        }
      };
      drawTrace(X0, X1, 0.16);
      drawTrace(X0, px, 1);
      // frozen value tags
      wins.forEach((w) => {
        if (px <= w.a + 30 || variant !== 'title') return;
        const y = Y0 - w.L * LV, k = Math.min(1, (px - w.a - 30) / 160);
        const label = w.tag;
        ctx.font = mono(700, 15); ctx.textAlign = 'left'; ctx.textBaseline = 'middle';
        const tw = ctx.measureText(label).width, tx = w.a + 18, ty = y - 26;
        ctx.fillStyle = hexA(P.bg, 0.92 * k);
        ctx.beginPath(); ctx.roundRect(tx - 9, ty - 14, tw + 18, 28, 7); ctx.fill();
        ctx.strokeStyle = hexA(P.ice, 0.8 * k); ctx.lineWidth = 1.2; ctx.stroke();
        ctx.fillStyle = hexA(P.ice, k); ctx.fillText(label, tx, ty + 1);
        ctx.textBaseline = 'alphabetic';
      });
      // playhead
      if (px < X1) {
        ctx.strokeStyle = hexA(P.ink, 0.5); ctx.lineWidth = 1;
        ctx.beginPath(); ctx.moveTo(px, BT + 10); ctx.lineTo(px, BB - 4); ctx.stroke();
        ctx.fillStyle = P.ink; ctx.beginPath(); ctx.arc(px, yOf(px), 5, 0, Math.PI * 2); ctx.fill();
      }
    }
    return {
      start(slide) {
        cv = $('canvas.hero', slide); if (!cv) return;
        variant = cv.dataset.hero || 'title';
        cv.width = 1600; cv.height = 900;
        ctx = cv.getContext('2d'); P = palette(); t0 = performance.now();
        wins = WIN[variant]; segs = build(11);
        wins.forEach((w) => { w.L = level(w.a + 24); });
        if (still()) { draw(t0 + (variant === 'title' ? 4200 : 6000)); return; } // one representative frame
        const loop = (now) => { P = palette(); draw(now); raf = requestAnimationFrame(loop); };
        raf = requestAnimationFrame(loop);
      },
      stop() { cancelAnimationFrame(raf); },
    };
  })();

  /* ================= flows (generic svg packets) ================= */
  reg.flows = (() => {
    let raf, items = [], slide;
    return {
      start(s) {
        slide = s;
        items = $$('.pkt[data-path]', s).map((el) => {
          const path = $('#' + el.dataset.path, s);
          if (!path || !path.getTotalLength) return null;
          const L = path.getTotalLength();
          return { el, path, L, dur: +(el.dataset.dur || 900), delay: +(el.dataset.delay || 0), hold: +(el.dataset.hold || 0), back: el.dataset.back === '1', rev: el.dataset.rev === '1' };
        }).filter(Boolean);
        if (still()) { items.forEach((it) => it.el.style.opacity = '0'); return; } // no riding dots
        const t0 = performance.now();
        const loop = (now) => {
          const tt = now - t0;
          for (const it of items) {
            const gate = it.el.parentNode; // the .step wrapper carries the reveal
            if (gate.classList.contains('step') && !gate.classList.contains('on')) continue;
            const period = it.dur + it.delay + it.hold + 700;
            let k = ((tt + it.delay * 1.7) % period) - it.delay;
            if (k < 0 || k > it.dur + it.hold) { it.el.style.opacity = '0'; continue; }
            it.el.style.opacity = '1';
            let u = Math.min(1, k / it.dur);
            if (it.back && k > it.dur) u = 1 - Math.min(1, (k - it.dur) / it.hold);
            if (it.rev) u = 1 - u;
            const p = it.path.getPointAtLength((it.rev ? u : u) * it.L);
            it.el.setAttribute('transform', `translate(${p.x.toFixed(1)},${p.y.toFixed(1)})`);
          }
          raf = requestAnimationFrame(loop);
        };
        raf = requestAnimationFrame(loop);
      },
      stop() { cancelAnimationFrame(raf); items = []; },
    };
  })();

  /* ================= ring (MPSC queue) ================= */
  reg.ring = (() => {
    let cv, ctx, raf, P, t0;
    const CX = 1150, CY = 456, R = 160, NR = 264, N = 32, T = 15000;
    const PROD = [{ a: -150, name: 'Core 0' }, { a: -90, name: 'Core 1' }, { a: -30, name: 'Core 2' }];
    const CONS = { a: 90, name: 'Worker' };
    const pt = (a, r) => [CX + Math.cos((a * Math.PI) / 180) * r, CY + Math.sin((a * Math.PI) / 180) * r];
    // script: time(s), kind, producer, slot
    const SCRIPT = [
      [0.3, 'enq', 0, 0], [1.0, 'enq', 1, 1], [1.7, 'enq', 2, 1], [2.6, 'deq', -1, 0], [3.1, 'enq', 0, 2], [3.8, 'enq', 1, 3],
      [4.5, 'deq', -1, 1], [5.2, 'deq', -1, 2], [5.9, 'enq', 2, 4], [6.6, 'enq', 0, 4], [7.3, 'enq', 1, 5], [8.2, 'deq', -1, 3],
      [8.9, 'enq', 2, 6], [9.6, 'deq', -1, 4], [10.2, 'deq', -1, 5], [10.9, 'enq', 0, 7], [11.6, 'deq', -1, 6], [12.4, 'deq', -1, 7],
    ];
    function stateAt(tt) {
      const slots = Array.from({ length: N }, () => 'free');
      const pk = []; let head = 0, tail = 0, enq = 0, dedup = 0, deq = 0;
      const inflight = new Map();
      SCRIPT.forEach((ev) => {
        const [t, kind, p, slot] = ev, dt = tt - t;
        if (dt < 0) return;
        if (kind === 'enq') {
          if (slots[slot] !== 'free') { // occupied -> dedup bounce
            dedup++;
            pk.push({ kind: 'dedup', p, slot, dt });
            return;
          }
          if (dt < 0.5) { pk.push({ kind: 'enq', p, slot, dt }); return; }
          slots[slot] = dt < 0.75 ? 'reserved' : 'queued'; enq++; head = Math.max(head, slot + 1);
          pk.push({ kind: 'enq', p, slot, dt, done: true });
        } else {
          if (dt < 0.6) { pk.push({ kind: 'deq', slot, dt }); slots[slot] = 'queued'; return; }
          slots[slot] = dt < 1.0 ? 'flash' : 'free'; deq++; tail = Math.max(tail, slot + 1);
        }
      });
      return { slots, pk, head, tail, enq, dedup, deq };
    }
    function draw(now) {
      const tt = ((now - t0) / 1000) % T;
      const st = stateAt(tt);
      ctx.clearRect(0, 0, 1600, 900);
      // lane rings
      ctx.lineWidth = 1; ctx.strokeStyle = hexA(P.ink, 0.08);
      [R - 50, R + 50].forEach((r) => { ctx.beginPath(); ctx.arc(CX, CY, r, 0, Math.PI * 2); ctx.stroke(); });
      // slots
      const gap = 0.045, step = (Math.PI * 2) / N;
      for (let i = 0; i < N; i++) {
        const a0 = -Math.PI / 2 + i * step + gap, a1 = -Math.PI / 2 + (i + 1) * step - gap;
        const s = st.slots[i];
        ctx.beginPath(); ctx.arc(CX, CY, R, a0, a1);
        if (s === 'free') { ctx.strokeStyle = hexA(P.ink, 0.13); ctx.lineWidth = 30; ctx.stroke(); }
        else if (s === 'reserved') { ctx.strokeStyle = P.amber; ctx.lineWidth = 30; ctx.stroke(); }
        else if (s === 'queued') { ctx.strokeStyle = hexA(P.ice, 0.95); ctx.lineWidth = 30; ctx.stroke(); }
        else if (s === 'flash') { ctx.strokeStyle = hexA(P.violet, 0.6); ctx.lineWidth = 30; ctx.stroke(); }
        // slot index ticks every 4
        if (i % 4 === 0) {
          const [x, y] = pt(((a0 + a1) / 2) * 180 / Math.PI, R - 44);
          ctx.font = mono(500, 11); ctx.fillStyle = P.ink3; ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
          ctx.fillText(String(i), x, y);
        }
      }
      // head / tail pointers
      const ptr = (idx, label, col, dir) => {
        const a = -Math.PI / 2 + (idx % N) * step + step / 2;
        const [x, y] = pt((a * 180) / Math.PI, R + 40);
        ctx.fillStyle = col; ctx.beginPath();
        ctx.moveTo(x, y); ctx.lineTo(x - 8, y - 13 * dir); ctx.lineTo(x + 8, y - 13 * dir); ctx.closePath(); ctx.fill();
        ctx.font = mono(700, 12); ctx.textAlign = 'center'; ctx.textBaseline = dir > 0 ? 'top' : 'bottom';
        ctx.fillText(label, x, y + (dir > 0 ? 18 : -20));
      };
      ptr(st.head, 'head', P.amber, -1);
      ptr(st.tail, 'tail', P.violet, 1);
      // producers / consumer
      const node = (a, name, col, sub) => {
        const [x, y] = pt(a, NR);
        ctx.beginPath(); ctx.arc(x, y, 13, 0, Math.PI * 2); ctx.fillStyle = col; ctx.fill();
        ctx.font = mono(700, 13); ctx.fillStyle = P.ink; ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
        ctx.fillText(name, x, y + 30);
        ctx.font = mono(500, 11); ctx.fillStyle = P.ink3; ctx.fillText(sub, x, y + 48);
        return [x, y];
      };
      const ppos = PROD.map((p, i) => node(p.a, p.name, hexA(P.amber, 0.95), 'producer'));
      const cpos = node(CONS.a, 'Worker', hexA(P.violet, 0.95), '唯一 consumer');
      // packets
      st.pk.forEach((k) => {
        const a = -Math.PI / 2 + k.slot * step + step / 2;
        const [sx, sy] = pt((a * 180) / Math.PI, R);
        let from, to, col, u;
        if (k.kind === 'enq' || k.kind === 'dedup') { from = ppos[k.p]; to = [sx, sy]; col = k.kind === 'dedup' ? P.red : P.amber; u = ease(k.dt / 0.5); }
        else { from = [sx, sy]; to = cpos; col = P.violet; u = ease(k.dt / 0.6); }
        if (k.kind === 'dedup' && k.dt > 0.5) { [from, to] = [to, from]; u = ease((k.dt - 0.5) / 0.45); if (k.dt > 1.1) return; }
        if (k.kind === 'enq' && k.dt > 0.5) return;
        if (k.kind === 'deq' && k.dt > 0.6) return;
        const x = from[0] + (to[0] - from[0]) * u, y = from[1] + (to[1] - from[1]) * u;
        if (k.kind === 'dedup') {
          ctx.setLineDash([4, 6]); ctx.strokeStyle = hexA(P.red, 0.5); ctx.lineWidth = 1.2;
          ctx.beginPath(); ctx.moveTo(from[0], from[1]); ctx.lineTo(to[0], to[1]); ctx.stroke(); ctx.setLineDash([]);
        }
        ctx.beginPath(); ctx.arc(x, y, 7, 0, Math.PI * 2); ctx.fillStyle = col; ctx.fill();
        ctx.lineWidth = 2; ctx.strokeStyle = P.bg; ctx.stroke();
        if (k.kind === 'dedup' && k.dt > 0.5) {
          ctx.font = mono(700, 13); ctx.fillStyle = P.red; ctx.textBaseline = 'middle';
          if (x > 1150) { ctx.textAlign = 'right'; ctx.fillText('dedup × 同 funcIndex 已在队', x - 12, y - 4); }
          else { ctx.textAlign = 'left'; ctx.fillText('dedup × 同 funcIndex 已在队', x + 12, y - 4); }
        }
      });
      // counters
      ctx.textAlign = 'left'; ctx.textBaseline = 'alphabetic'; ctx.font = mono(500, 13);
      const cy = CY + NR + 84;
      const cell = (x, label, val, col) => {
        ctx.fillStyle = P.ink3; ctx.fillText(label, x, cy);
        ctx.font = mono(800, 26); ctx.fillStyle = col; ctx.fillText(String(val).padStart(2, '0'), x, cy + 34);
        ctx.font = mono(500, 13);
      };
      cell(CX - 250, 'enqueued', st.enq, P.amber);
      cell(CX - 90, 'dedup 命中', st.dedup, P.red);
      cell(CX + 70, 'consumed', st.deq, P.violet);
      ctx.textAlign = 'right'; ctx.fillStyle = P.ink3;
      ctx.fillText('ring[1024] · 图示 32 槽 · Vyukov MPSC · 单 consumer', 1504, cy + 6);
    }
    return {
      start(slide) {
        cv = $('canvas.ring', slide); if (!cv) return;
        cv.width = 1600; cv.height = 900; ctx = cv.getContext('2d'); P = palette(); t0 = performance.now();
        if (still()) { draw(t0 + 5000); return; } // one representative frame
        const loop = (now) => { P = palette(); draw(now); raf = requestAnimationFrame(loop); };
        raf = requestAnimationFrame(loop);
      },
      stop() { cancelAnimationFrame(raf); },
    };
  })();

  /* ================= pool (W^X pages) ================= */
  reg.pool = (() => {
    let timers = [], phase = 0, cells = [], slide;
    const COLS = 32, ROWS = 16;
    const NS = 'http://www.w3.org/2000/svg';
    function build() {
      const host = document.querySelector('.pool-grid'); if (!host) return false;
      for (let r = 0; r < ROWS; r++) for (let c = 0; c < COLS; c++) {
        const rect = document.createElementNS(NS, 'rect');
        rect.setAttribute('x', String(c * 30)); rect.setAttribute('y', String(r * 26));
        rect.setAttribute('width', '26'); rect.setAttribute('height', '22'); rect.setAttribute('rx', '3');
        rect.setAttribute('class', 'pg');
        host.appendChild(rect); cells.push(rect);
      }
      return true;
    }
    // build at load so printing (which never activates the slide) has the grid
    const built = build();
    return {
      start(s) {
        slide = s;
        if (!built) return;
        this.step(phase, true);
      },
      step(n, force) {
        phase = n; if (!cells.length) return;
        timers.forEach(clearTimeout); timers = [];
        cells.forEach((c) => { c.style.transition = 'none'; c.setAttribute('class', 'pg' + (n >= 4 && cells.indexOf(c) === 0 ? ' waste' : '')); });
        void cells[0].getBoundingClientRect();
        const written = n >= 2 ? 150 : 0, sealed = n >= 3 ? 96 : 0;
        if (still()) {
          cells.forEach((c, i) => c.setAttribute('class', 'pg' + (i < sealed ? ' rx' : i < written ? ' rw' : '') + (n >= 4 && i === 0 ? ' waste' : '')));
          return;
        }
        cells.forEach((c, i) => {
          if (i < sealed) { c.style.transition = ''; timers.push(setTimeout(() => c.setAttribute('class', 'pg rx'), 500 + i * 9)); }
          else if (i < written) { c.style.transition = ''; timers.push(setTimeout(() => c.setAttribute('class', 'pg rw'), 60 + i * 9)); }
        });
        if (n >= 4) { timers.push(setTimeout(() => cells[0].setAttribute('class', 'pg rx waste'), 2400)); }
      },
      stop() { timers.forEach(clearTimeout); timers = []; },
      print() {
        if (!built) return;
        const written = 150, sealed = 96;
        cells.forEach((c, i) => c.setAttribute('class', 'pg' + (i < sealed ? ' rx' : i < written ? ' rw' : '')));
        cells[0].setAttribute('class', 'pg rx waste');
      },
    };
  })();
  // print: canvases and count-ups never run, so paint their end state
  const printReady = () => {
    document.querySelectorAll('[data-count]').forEach((el) => {
      const dec = +(el.dataset.dec || 0);
      el.textContent = (el.dataset.pre || '') + Number(el.dataset.count).toLocaleString('en-US', { minimumFractionDigits: dec, maximumFractionDigits: dec }) + (el.dataset.post || '');
    });
    if (reg.pool && reg.pool.print) reg.pool.print();
  };
  window.addEventListener('beforeprint', printReady);
  if (window.matchMedia) { const mq = window.matchMedia('print'); (mq.addEventListener ? mq.addEventListener('change', printReady) : mq.addListener(printReady)); }
})();
