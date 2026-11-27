/* ============================================================
   EJIT deck — engine: navigation, transitions, steps, HUD, overview,
   presenter, notes, theme, hash, input.
   Live animations register themselves in window.__live[name].
   ============================================================ */
(() => {
  'use strict';
  const $ = (s, r = document) => r.querySelector(s);
  const $$ = (s, r = document) => Array.from(r.querySelectorAll(s));
  const wait = (ms) => new Promise((r) => setTimeout(r, ms));

  const stage = $('#stage'), slidesEl = $('#slides'), viewport = $('#viewport');
  const slides = $$('.slide', slidesEl);
  const N = slides.length;
  slides.forEach((s, i) => { s.dataset.n = i + 1; });
  const ACTS = ['开场', '问题', '机制', '疯狂', '诚实', '收束', '附录'];
  const actCount = [0, 0, 0, 0, 0, 0, 0];
  slides.forEach((s) => { actCount[+s.dataset.act || 0]++; });

  const DUR = { push: 600, fade: 500, zoom: 650, rise: 600, drop: 600, cover: 0, none: 0 };
  const S = { i: 0, step: 0, busy: false, overview: false, t0: Date.now(), black: false };
  const reduced = () => matchMedia('(prefers-reduced-motion: reduce)').matches;
  const liveReg = window.__live = window.__live || {};
  let curLive = null;

  const stepsOf = (s) => +(s.dataset.steps || 0);

  // ---------- steps ----------
  function applyStep(s, step, instant) {
    if (instant) { s.classList.add('no-trans'); }
    $$('[data-step]', s).forEach((el) => {
      const a = +el.dataset.step;
      const b = el.dataset.until !== undefined ? +el.dataset.until : Infinity;
      const on = step >= a && step < b;
      el.classList.toggle('on', on);
      el.classList.toggle('out', step >= b);
    });
    if (instant) { void s.offsetWidth; s.classList.remove('no-trans'); }
    if (curLive && curLive.step) curLive.step(step);
  }

  // ---------- HUD ----------
  const hudAct = $('.hud-act'), hudNum = $('.hud-num'), hudTl = $('.hud-tl');
  actCount.forEach((c, a) => {
    if (!c) return;
    const w = document.createElement('div');
    w.className = 'w'; w.style.flex = String(c); w.dataset.act = a;
    hudTl.appendChild(w);
  });
  function hud() {
    const s = slides[S.i], a = +s.dataset.act || 0;
    const name = s.dataset.actname || ACTS[a];
    hudAct.textContent = name;
    hudNum.innerHTML = `<b>${String(S.i + 1).padStart(2, '0')}</b> / ${N}`;
    $$('.w', hudTl).forEach((w) => {
      const on = +w.dataset.act === a;
      w.classList.toggle('on', on);
      if (on) {
        const idx = slides.slice(0, S.i + 1).filter((x) => (+x.dataset.act || 0) === a).length;
        w.style.setProperty('--p', Math.round((idx / actCount[a]) * 100) + '%');
      } else w.style.setProperty('--p', '0%');
    });
    document.documentElement.style.setProperty('--hud-c', 'var(--ice)');
  }

  // ---------- live animations ----------
  function startLive(s) {
    stopLive();
    const name = s.dataset.live;
    const mod = name && liveReg[name];
    if (mod && mod.start) { mod.start(s); curLive = mod; }
  }
  function stopLive() {
    if (curLive && curLive.stop) curLive.stop();
    curLive = null;
  }

  // ---------- transitions ----------
  function pickTransition(from, to, dir) {
    const a = slides[from].dataset.act, b = slides[to].dataset.act;
    let t = slides[dir > 0 ? to : from].dataset.in;
    if (!t) t = a !== b ? 'shutter' : 'push';
    if (reduced()) t = t === 'shutter' || t === 'wipe' ? 'fade' : t;
    return t;
  }
  function show(i, step, dir, instant) {
    const s = slides[i];
    slidesEl.style.setProperty('--dir', String(dir));
    s.classList.add('is-active');
    startLive(s);
    applyStep(s, step, instant);
    runCounts(s);
  }
  function hideAll() {
    slides.forEach((s) => { s.classList.remove('is-active', 'is-leaving'); });
    stopLive();
  }
  async function go(i, step, dir = 1, opts = {}) {
    i = Math.max(0, Math.min(N - 1, i));
    step = Math.max(0, Math.min(stepsOf(slides[i]), step === undefined || step < 0 ? stepsOf(slides[i]) : step));
    if (S.overview) toggleOverview(false, true);
    if (S.busy) return false;
    if (i === S.i && slides[S.i].classList.contains('is-active')) {
      if (step !== S.step) { S.step = step; applyStep(slides[i], step, !!opts.instant); afterChange(); }
      return true;
    }
    const t = opts.trans || pickTransition(S.i, i, dir);
    const old = slides[S.i], nw = slides[i];
    const instant = !!opts.instant || document.documentElement.classList.contains('no-motion');
    S.busy = true;
    if (t === 'wipe' || t === 'shutter') {
      const els = t === 'wipe' ? [$('#wipe')] : [$('#shut-t'), $('#shut-b')];
      const cover = t === 'wipe' ? 440 : 420, reveal = t === 'wipe' ? 500 : 520;
      if (!instant) {
        els.forEach((e) => { e.classList.remove('reveal'); void e.offsetWidth; e.classList.add('cover'); });
        await wait(cover);
      }
      hideAll();
      S.i = i; S.step = step;
      show(i, step, dir, true);
      els.forEach((e) => { e.classList.remove('cover'); e.classList.add('reveal'); });
      if (!instant) await wait(reveal);
      els.forEach((e) => e.classList.remove('reveal'));
    } else {
      slidesEl.className = 't-' + t;
      const d = instant || reduced() ? 0 : (DUR[t] ?? 500);
      if (slides[S.i].classList.contains('is-active')) { old.classList.remove('is-active'); old.classList.add('is-leaving'); }
      else hideAll();
      S.i = i; S.step = step;
      show(i, step, dir, instant);
      if (d) await wait(d + 40);
      old.classList.remove('is-leaving');
    }
    S.busy = false;
    afterChange();
    return true;
  }
  function afterChange() {
    hud();
    if (!isPresenter()) { // keep #presenter in the presenter window, or a reload loses the role
      const h = '#' + (S.i + 1) + (S.step ? '.' + S.step : '');
      try { history.replaceState(null, '', h); } catch (e) { /* file:// with opaque origin */ }
    }
    fillNotes();
    broadcast();
  }

  function next() {
    if (S.busy) return;
    if (S.step < stepsOf(slides[S.i])) go(S.i, S.step + 1, 1);
    else if (S.i < N - 1) go(S.i + 1, 0, 1);
  }
  function prev() {
    if (S.busy) return;
    if (S.step > 0) go(S.i, S.step - 1, -1);
    else if (S.i > 0) go(S.i - 1, -1, -1);
  }

  // ---------- number count-up ----------
  const fmt = (v, dec) => v.toLocaleString('en-US', { minimumFractionDigits: dec, maximumFractionDigits: dec });
  function runCounts(s) {
    $$('[data-count]', s).forEach((el) => {
      const to = parseFloat(el.dataset.count), dec = +(el.dataset.dec || 0);
      const dur = +(el.dataset.dur || 1400), delay = +(el.dataset.delay || 0);
      const pre = el.dataset.pre || '', post = el.dataset.post || '';
      if (document.documentElement.classList.contains('no-motion') || reduced()) { el.textContent = pre + fmt(to, dec) + post; return; }
      const t0 = performance.now() + delay;
      el.textContent = pre + fmt(0, dec) + post;
      let raf;
      const tick = (now) => {
        if (!s.classList.contains('is-active')) return;
        const k = Math.min(1, Math.max(0, (now - t0) / dur));
        const e = 1 - Math.pow(1 - k, 4);
        el.textContent = pre + fmt(to * e, dec) + post;
        if (k < 1) raf = requestAnimationFrame(tick);
      };
      raf = requestAnimationFrame(tick);
    });
  }

  // ---------- overview ----------
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const stopAllLive = () => { stopLive(); Object.values(liveReg).forEach((m) => m.stop && m.stop()); };
  let ovToken = 0;
  async function paintLiveThumbs() {
    // canvas slides: let each animation run briefly, then leave its last frame on the thumbnail
    const tok = ++ovToken;
    stopLive();
    for (const s of slides) {
      if (tok !== ovToken) return;
      const name = s.dataset.live, mod = name && liveReg[name];
      if (!mod || name === 'flows') continue;
      if (name === 'pool') { mod.print && mod.print(); continue; }
      mod.stop(); mod.start(s);
      if (reduced()) continue;
      await sleep(name === 'hero' ? 2700 : 900);
    }
  }
  function toggleOverview(on, silent) {
    const was = S.overview;
    S.overview = on;
    stage.classList.toggle('overview', on);
    $('#ovhint').classList.toggle('show', on);
    if (on) {
      slides.forEach((s) => s.classList.toggle('cur', +s.dataset.n === S.i + 1));
      requestAnimationFrame(() => { const c = $('.slide.cur'); if (c) c.scrollIntoView({ block: 'center' }); });
      paintLiveThumbs();
      if (!silent) toast('总览 · 点击跳转 · Esc 返回');
    } else {
      if (was) { ovToken++; stopAllLive(); startLive(slides[S.i]); applyStep(slides[S.i], S.step, true); }
      if (!silent) stage.scrollTop = 0;
    }
  }
  slidesEl.addEventListener('click', (e) => {
    if (!S.overview) return;
    const s = e.target.closest('.slide');
    if (!s) return;
    e.stopPropagation(); // or the same click reaches #viewport and advances a step
    const i = +s.dataset.n - 1;
    toggleOverview(false, true);
    if (i !== S.i) go(i, -1, i > S.i ? 1 : -1, { trans: 'fade' });
  });

  // ---------- notes / toast / overlays ----------
  const notesEl = $('#notes');
  function fillNotes() {
    const s = slides[S.i];
    const n = $('.notes', s);
    const title = $('.title', s) || $('.display', s) || $('.stmt', s);
    notesEl.innerHTML = `<div class="nh"><span>备注 · ${S.i + 1}/${N}</span><span>N 关闭</span></div>` +
      (n ? n.innerHTML : '<p class="empty">（本页没有备注）</p>');
    if (document.documentElement.classList.contains('presenter')) fillPresenter();
  }
  function toggleNotes(force) { notesEl.classList.toggle('show', force); document.body.classList.toggle('notes-open', notesEl.classList.contains('show')); }
  let toastT;
  function toast(msg) {
    const t = $('#toast'); t.textContent = msg; t.classList.add('show');
    clearTimeout(toastT); toastT = setTimeout(() => t.classList.remove('show'), 1800);
  }
  function blackout(kind) {
    viewport.classList.remove('blackout', 'whiteout');
    if (S.black === kind) { S.black = false; return; }
    S.black = kind; viewport.classList.add(kind);
  }

  // ---------- theme ----------
  function setTheme(t) {
    document.documentElement.dataset.theme = t;
    try { localStorage.setItem('ejit-talk-theme', t); } catch (e) { /* ignore */ }
  }
  function toggleTheme() {
    setTheme(document.documentElement.dataset.theme === 'light' ? 'dark' : 'light');
    toast(document.documentElement.dataset.theme === 'light' ? '浅色主题' : '深色主题');
    startLive(slides[S.i]);
    broadcast();
  }

  // ---------- presenter link ----------
  const isPresenter = () => document.documentElement.classList.contains('presenter');
  let presWin = null;
  function broadcast() {
    const msg = { ejit: true, type: 'state', i: S.i, step: S.step, elapsed: Date.now() - S.t0, theme: document.documentElement.dataset.theme };
    try {
      if (isPresenter() && window.opener) window.opener.postMessage(msg, '*');
      else if (presWin) presWin.postMessage(msg, '*');
    } catch (e) { /* ignore */ }
  }
  function onMsg(e) {
    const d = e.data;
    if (!d || !d.ejit) return;
    if (isPresenter()) {
      if (d.type === 'state') { setThemeSilent(d.theme); S.t0 = Date.now() - d.elapsed; if (d.i !== S.i || d.step !== S.step) go(d.i, d.step, 1, { trans: 'none' }); else fillPresenter(); }
    } else {
      if (d.type === 'go') go(d.i, d.step, 1, { trans: d.trans || 'push' });
      else if (d.type === 'cmd') { if (d.name === 'next') next(); else if (d.name === 'prev') prev(); else if (d.name) blackout(d.name); }
      else if (d.type === 'ping') broadcast();
    }
  }
  function setThemeSilent(t) { if (t && t !== document.documentElement.dataset.theme) document.documentElement.dataset.theme = t; }
  function openPresenter() {
    if (isPresenter()) return;
    const url = location.href.split('#')[0] + '#presenter';
    presWin = window.open(url, 'ejit-presenter', 'width=1360,height=900');
    if (!presWin) { toast('弹窗被拦截：请允许弹窗后重试（P）'); return; }
    toast('已打开演讲者视图');
    setTimeout(broadcast, 600);
  }
  function fillPresenter() {
    const bar = $('#pbar'); if (!bar) return;
    const el = Date.now() - S.t0, sec = Math.floor(el / 1000);
    const hh = Math.floor(sec / 3600), mm = String(Math.floor((sec % 3600) / 60)).padStart(2, '0'), ss = String(sec % 60).padStart(2, '0');
    const t = $('.t', bar); t.textContent = (hh ? hh + ':' : '') + mm + ':' + ss;
    t.classList.toggle('warn', !hh && mm >= '20' && S.i < N * 0.6);
    $('.n', bar).textContent = `第 ${S.i + 1} / ${N} 页 · 步骤 ${S.step}/${stepsOf(slides[S.i])}`;
    const nx = slides[S.i + 1];
    const nt = nx ? (($('.title', nx) || {}).textContent || $('.display', nx)?.textContent || $('.stmt .display', nx)?.textContent || '') : '— 已是最后一页 —';
    const pn = $('#pnotes');
    pn.innerHTML = `<div class="nh"><span>当前页备注</span><span>${S.i + 1}/${N}</span></div>` +
      `<div class="next">下一页：<b>${(nt || '').replace(/\s+/g, ' ').trim()}</b></div>` +
      ($('.notes', slides[S.i]) ? $('.notes', slides[S.i]).innerHTML : '<p class="empty">（本页没有备注）</p>');
  }
  setInterval(() => { if (isPresenter() && $('#pbar')) fillPresenter(); }, 500);

  // ---------- fit ----------
  function fit() {
    const r = viewport.getBoundingClientRect();
    const s = Math.min(r.width / 1600, r.height / 900);
    const x = (r.width - 1600 * s) / 2, y = (r.height - 900 * s) / 2;
    stage.style.transform = `translate(${x}px, ${y}px) scale(${s})`;
  }
  addEventListener('resize', fit);
  if (window.ResizeObserver) new ResizeObserver(() => fit()).observe(viewport);

  // ---------- input ----------
  let wheelT = 0;
  addEventListener('wheel', (e) => {
    if (S.overview || isPresenter()) return;
    if (e.target.closest && e.target.closest('#notes,#pnotes,#help,.overlay')) return; // scrolling a panel
    const now = Date.now();
    if (now - wheelT < 900 || Math.abs(e.deltaY) < 12) return; // one flick = one step
    wheelT = now; if (e.deltaY > 0) next(); else prev();
  }, { passive: true });
  addEventListener('keydown', (e) => {
    if (e.metaKey || e.ctrlKey || e.altKey) return;
    const k = e.key;
    const help = $('#help');
    if (k === 'Escape') {
      if (help.classList.contains('show')) help.classList.remove('show');
      else if (S.overview) toggleOverview(false);
      else if (notesEl.classList.contains('show')) toggleNotes(false);
      return;
    }
    if (k === '?' || k === 'F1') { e.preventDefault(); help.classList.toggle('show'); return; }
    if (help.classList.contains('show')) return;
    if (isPresenter()) return;
    if (S.overview) {
      if (k === 'o' || k === 'O') toggleOverview(false);
      return;
    }
    switch (k) {
      case 'ArrowRight': case 'ArrowDown': case ' ': case 'PageDown': case 'Enter': case 'j': case 'J': case 'l': case 'L': e.preventDefault(); next(); break;
      case 'ArrowLeft': case 'ArrowUp': case 'Backspace': case 'PageUp': case 'k': case 'K': case 'h': case 'H': e.preventDefault(); prev(); break;
      case 'Home': e.preventDefault(); go(0, -1, -1); break;
      case 'End': e.preventDefault(); go(N - 1, -1, 1); break;
      case 'o': case 'O': toggleOverview(!S.overview); break;
      case 'n': case 'N': toggleNotes(); break;
      case 'p': case 'P': openPresenter(); break;
      case 't': case 'T': toggleTheme(); break;
      case 'f': case 'F': if (document.fullscreenElement) document.exitFullscreen(); else document.documentElement.requestFullscreen?.(); break;
      case 'b': case 'B': case '.': blackout('blackout'); break;
      case 'w': case 'W': case ',': blackout('whiteout'); break;
      case 'm': case 'M': document.documentElement.classList.toggle('no-motion'); startLive(slides[S.i]); toast(document.documentElement.classList.contains('no-motion') ? '动效已关闭' : '动效已开启'); break;
      case 'r': case 'R': S.t0 = Date.now(); toast('计时已归零'); broadcast(); break;
      default: return;
    }
  });
  // touch
  let tx = 0, ty = 0, tt = 0, lastTouch = 0;
  viewport.addEventListener('touchstart', (e) => { const t = e.touches[0]; tx = t.clientX; ty = t.clientY; tt = Date.now(); }, { passive: true });
  viewport.addEventListener('touchend', (e) => {
    if (isPresenter()) return;
    lastTouch = Date.now(); // swallow the click the browser synthesizes after this tap
    const t = e.changedTouches[0], dx = t.clientX - tx, dy = t.clientY - ty;
    if (Math.abs(dx) > 56 && Math.abs(dx) > Math.abs(dy)) { if (dx < 0) next(); else prev(); }
    else if (Math.abs(dx) < 20 && Math.abs(dy) < 20 && Date.now() - tt < 400 && !S.overview) { if (t.clientX > innerWidth / 2) next(); else prev(); }
  }, { passive: true });
  viewport.addEventListener('click', (e) => {
    if (S.overview || isPresenter() || Date.now() - lastTouch < 500) return;
    if (e.target.closest('a,button,.hit,.code')) return;
    if (e.clientY > innerHeight - 70 && e.clientX > innerWidth - 240) return;
    if (e.clientX > innerWidth / 2) next(); else prev();
  });
  // controls
  let mouseT;
  const controls = $('#controls');
  addEventListener('mousemove', () => {
    controls.classList.add('show');
    clearTimeout(mouseT); mouseT = setTimeout(() => controls.classList.remove('show'), 2200);
  });
  $('#c-prev').onclick = (e) => { e.stopPropagation(); prev(); };
  $('#c-next').onclick = (e) => { e.stopPropagation(); next(); };
  $('#c-ov').onclick = (e) => { e.stopPropagation(); toggleOverview(!S.overview); };
  $('#c-fs').onclick = (e) => { e.stopPropagation(); if (document.fullscreenElement) document.exitFullscreen(); else document.documentElement.requestFullscreen?.(); };
  $('#c-th').onclick = (e) => { e.stopPropagation(); toggleTheme(); };
  $('#c-nt').onclick = (e) => { e.stopPropagation(); toggleNotes(); };
  addEventListener('message', onMsg);
  addEventListener('popstate', () => { const p = parseHash(); if (!p.pres) go(p.i, p.step, p.i >= S.i ? 1 : -1, { trans: 'fade' }); });
  document.addEventListener('visibilitychange', () => { if (document.hidden) stopLive(); else if (!S.overview && slides[S.i]) startLive(slides[S.i]); });

  function parseHash() {
    const h = location.hash.replace(/^#\/?/, '');
    if (h === 'presenter') return { pres: true };
    const m = /^(\d+)(?:\.(\d+))?$/.exec(h);
    if (!m) return { i: 0, step: -1 };
    return { i: Math.min(N, +m[1]) - 1, step: m[2] !== undefined ? +m[2] : -1 };
  }

  // ---------- boot ----------
  function boot() {
    const p = parseHash();
    fit();
    let theme = 'dark';
    try { theme = localStorage.getItem('ejit-talk-theme') || 'dark'; } catch (e) { /* ignore */ }
    if (p.pres) document.documentElement.classList.add('presenter');
    setTheme(theme);
    fit();
    toggleOverview(false, true);
    hideAll();
    const i = Math.max(0, p.i || 0);
    S.i = i; S.step = Math.min(p.step < 0 ? stepsOf(slides[i]) : p.step, stepsOf(slides[i]));
    if (p.pres) { S.step = 0; }
    show(i, S.step, 1, true);
    hud(); fillNotes();
    if (p.pres) setTimeout(() => { try { window.opener && window.opener.postMessage({ ejit: true, type: 'ping' }, '*'); } catch (e) { /* ignore */ } }, 300);
    document.fonts && document.fonts.ready.then(() => { fit(); });
    window.__deck.ready = true;
  }

  window.__deck = {
    ready: false, count: () => N,
    goto(n, step) { // 1-based slide number; step -1 = final
      const i = Math.max(1, Math.min(N, n)) - 1;
      S.busy = false;
      toggleOverview(false, true);
      document.documentElement.classList.add('no-motion');
      hideAll();
      S.i = i; S.step = step === undefined || step < 0 ? stepsOf(slides[i]) : Math.min(step, stepsOf(slides[i]));
      slidesEl.className = 't-none';
      show(i, S.step, 1, true);
      afterChange();
      return stepsOf(slides[i]);
    },
    setTheme, next, prev,
    go(n, step) { // animated navigation (for testing transitions)
      document.documentElement.classList.remove('no-motion');
      return go(n - 1, step === undefined ? -1 : step, n - 1 >= S.i ? 1 : -1);
    },
    motion(on) { document.documentElement.classList.toggle('no-motion', !on); },
    state: () => ({ i: S.i, step: S.step, n: N }),
  };
  // live.js (concatenated after this file) registers the window.__live modules;
  // the assembled page calls window.__deckBoot() once the whole script has run,
  // so the slide you land on starts its animation.
  window.__deckBoot = boot;
})();
