// Behavioural regression checks for the deck engine.
// Each assertion pins one bug found in review. usage: node check.js <deck.html>
const { chromium } = require(process.env.PW_PATH || 'playwright');
const path = require('path');
let failed = 0;
const ok = (cond, name) => { console.log((cond ? 'PASS  ' : 'FAIL  ') + name); if (!cond) failed++; };
const painted = () => {
  const cv = document.querySelector('canvas.hero');
  if (!cv) return -1;
  const d = cv.getContext('2d').getImageData(96, 500, 1408, 284).data;
  let n = 0; for (let i = 3; i < d.length; i += 4) if (d[i] > 0) n++;
  return n;
};
const rafCount = (ms) => new Promise((res) => {
  let n = 0; const orig = window.requestAnimationFrame;
  window.requestAnimationFrame = (cb) => { n++; return orig(cb); };
  setTimeout(() => { window.requestAnimationFrame = orig; res(n); }, ms);
});
(async () => {
  const file = 'file://' + path.resolve(process.argv[2]);
  const browser = await chromium.launch({ args: ['--no-sandbox', '--allow-file-access-from-files'] });
  const page = await (await browser.newContext({ viewport: { width: 1600, height: 900 } })).newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push('pageerror: ' + e.message));
  page.on('console', (m) => { if (m.type() === 'error') errors.push('console: ' + m.text()); });

  // 1. the slide you land on starts its live animation (boot runs after live.js registers)
  await page.goto(file);
  await page.waitForFunction(() => window.__deck && window.__deck.ready);
  await page.waitForTimeout(1200);
  let n = await page.evaluate(painted);
  ok(n > 500, `landing slide canvas painted (px=${n})`);
  await page.goto(file + '#36');
  await page.waitForFunction(() => window.__deck && window.__deck.ready);
  await page.waitForTimeout(1200);
  n = await page.evaluate(painted);
  ok(n > 500, `deep link #36 canvas painted (px=${n})`);

  // 2. packets actually ride their paths (CSS transform:none must not override the attribute)
  await page.evaluate(() => window.__deck.goto(14, -1));
  await page.keyboard.press('m'); // motion off by goto(); M turns it back on and restarts the module
  await page.waitForTimeout(400);
  const pkt = await page.evaluate(async () => {
    const els = [...document.querySelectorAll('.slide.is-active .pkt')];
    if (!els.length) return null;
    const t = (e) => e.getAttribute('transform') || '';
    const a = els.map(t).join('|');
    await new Promise((r) => setTimeout(r, 600));
    return { moved: a !== els.map(t).join('|'), cs: getComputedStyle(els[0]).transform,
             wrapped: els.every((e) => e.parentNode.classList.contains('step')) };
  });
  ok(pkt && pkt.moved, `packets ride paths (computed=${pkt && pkt.cs})`);
  ok(pkt && pkt.wrapped, 'packets sit inside their .step wrapper');

  // 2b. stepping back hides the packets that left the window
  await page.evaluate(() => window.__deck.go(14, 2));
  await page.waitForTimeout(900);
  const hid = await page.evaluate(() =>
    [...document.querySelectorAll('.slide.is-active .pkt')].every((e) => {
      const g = e.parentNode;
      return g.classList.contains('on') || getComputedStyle(g).opacity === '0';
    }));
  ok(hid, 'withdrawn packets hidden after stepping back');

  // 3. clicking the current thumbnail keeps the position; another thumbnail navigates
  await page.evaluate(() => window.__deck.goto(12, 1));
  await page.keyboard.press('o'); await page.waitForTimeout(500);
  await page.click('.slide.cur'); await page.waitForTimeout(700);
  let st = await page.evaluate(() => window.__deck.state());
  ok(st.i === 11 && st.step === 1, `overview: current thumbnail keeps position (${st.i + 1}.${st.step})`);
  await page.keyboard.press('o'); await page.waitForTimeout(400);
  await page.click('.slide[data-n="20"]'); await page.waitForTimeout(700);
  st = await page.evaluate(() => window.__deck.state());
  ok(st.i === 19, `overview: thumbnail jump navigates (now ${st.i + 1})`);

  // 4. wheel over the notes drawer scrolls the drawer; over the stage it steps
  await page.evaluate(() => window.__deck.goto(9, 0));
  await page.keyboard.press('n'); await page.waitForTimeout(300);
  await page.evaluate(() => document.querySelector('#notes').dispatchEvent(new WheelEvent('wheel', { deltaY: 240, bubbles: true })));
  await page.waitForTimeout(250);
  st = await page.evaluate(() => window.__deck.state());
  ok(st.i === 8 && st.step === 0, `wheel over notes does not step (${st.i + 1}.${st.step})`);
  await page.evaluate(() => document.querySelector('#notes').dispatchEvent(new WheelEvent('wheel', { deltaY: 240, bubbles: true })));
  await page.evaluate(() => document.querySelector('#notes').dispatchEvent(new WheelEvent('wheel', { deltaY: 240, bubbles: true })));
  await page.waitForTimeout(250);
  st = await page.evaluate(() => window.__deck.state());
  ok(st.i === 8 && st.step === 0, `wheel over notes still does not step after repeats (${st.i + 1}.${st.step})`);
  await page.evaluate(() => document.querySelector('#viewport').dispatchEvent(new WheelEvent('wheel', { deltaY: 240, bubbles: true })));
  await page.waitForTimeout(250);
  st = await page.evaluate(() => window.__deck.state());
  ok(st.i === 8 && st.step === 1, `wheel over the stage does step (${st.i + 1}.${st.step})`);
  await page.keyboard.press('n');

  // 5. one tap = one step (the synthesized click after touchend is swallowed)
  await page.evaluate(() => window.__deck.goto(9, 0));
  await page.evaluate(() => {
    const vp = document.querySelector('#viewport');
    const t = new Touch({ identifier: 1, target: vp, clientX: 1200, clientY: 400 });
    vp.dispatchEvent(new TouchEvent('touchstart', { touches: [t] }));
    vp.dispatchEvent(new TouchEvent('touchend', { changedTouches: [t] }));
    vp.dispatchEvent(new MouseEvent('click', { clientX: 1200, clientY: 400, bubbles: true }));
  });
  await page.waitForTimeout(400);
  st = await page.evaluate(() => window.__deck.state());
  ok(st.i === 8 && st.step === 1, `one tap advances exactly one step (${st.i + 1}.${st.step})`);

  // 6. motion off (M) stops the canvas loops; M again resumes them on the same slide
  await page.evaluate(() => window.__deck.goto(1, -1));
  await page.waitForTimeout(400);
  let frames = await page.evaluate(rafCount, 700);
  ok(frames <= 4, `no-motion: canvas loops idle (rAF in 700ms = ${frames})`);
  await page.keyboard.press('m');
  await page.waitForTimeout(400);
  frames = await page.evaluate(rafCount, 700);
  ok(frames > 20, `motion on: canvas loops run (rAF in 700ms = ${frames})`);

  // 6b. prefers-reduced-motion: static frame, no loop
  const ctx2 = await browser.newContext({ viewport: { width: 1600, height: 900 }, reducedMotion: 'reduce' });
  const p2 = await ctx2.newPage();
  await p2.goto(file);
  await p2.waitForFunction(() => window.__deck && window.__deck.ready);
  await p2.waitForTimeout(1200);
  frames = await p2.evaluate(rafCount, 700);
  n = await p2.evaluate(painted);
  ok(frames <= 4 && n > 500, `prefers-reduced-motion: one frame, no loop (rAF=${frames}, px=${n})`);
  await ctx2.close();

  // 7. presenter: follows the main window, keeps its hash, ignores local keys, never drops a step
  const popup = page.waitForEvent('popup');
  await page.keyboard.press('p');
  const pres = await popup;
  await pres.waitForFunction(() => window.__deck && window.__deck.ready);
  await page.evaluate(() => window.__deck.go(14, 1));
  await page.waitForTimeout(1000); // let the slide change and the first state message settle
  await page.evaluate(() => { window.__deck.go(14, 2); window.__deck.go(14, 3); }); // same-slide steps: main applies both instantly
  await page.waitForTimeout(1200);
  let pst = await pres.evaluate(() => ({ s: window.__deck.state(), h: location.hash, cls: document.documentElement.className }));
  ok(pst.cls.includes('presenter') && pst.h === '#presenter', `presenter keeps its hash (${pst.h})`);
  ok(pst.s.i === 13 && pst.s.step === 3, `presenter follows rapid steps, drops none (${pst.s.i + 1}.${pst.s.step})`);
  await pres.keyboard.press('ArrowRight');
  await page.waitForTimeout(400);
  pst = await pres.evaluate(() => window.__deck.state());
  let mst = await page.evaluate(() => window.__deck.state());
  ok(pst.i === 13 && pst.step === 3 && mst.i === 13 && mst.step === 3, `presenter keys do not fork the state (${pst.i + 1}.${pst.step} vs ${mst.i + 1}.${mst.step})`);
  await pres.close();

  // 8. AArch64 immediates are not painted as comments
  await page.evaluate(() => window.__deck.goto(29, -1));
  await page.waitForTimeout(300);
  const imm = await page.evaluate(() => {
    const cs = [...document.querySelectorAll('.slide.is-active .code .c')];
    return { c16: cs.some((s) => s.textContent.includes('#16')), c3: cs.some((s) => s.textContent.includes('#3')) };
  });
  ok(!imm.c16 && !imm.c3, `#16/#3 render as immediates, not comments (${JSON.stringify(imm)})`);

  ok(!errors.length, `no JS errors${errors.length ? ':\n  ' + errors.join('\n  ') : ''}`);
  await browser.close();
  console.log(failed ? `\n${failed} FAILED` : '\nall checks passed');
  process.exit(failed ? 1 : 0);
})();
