// Screenshot helper for the deck.
// usage: node shot.js <html> <outdir> [spec...]
//   spec: 3        -> slide 3, all steps
//         3@1      -> slide 3, step 1
//         3@0+800  -> slide 3, step 0, extra wait 800ms after enter
//         all      -> every slide at final step
// env: SCALE=2 LIGHT=1 W=1600 H=900
const { chromium } = require(process.env.PW_PATH || 'playwright');
const path = require('path');
const fs = require('fs');

(async () => {
  const [html, outdir, ...specs] = process.argv.slice(2);
  const W = +(process.env.W || 1600), H = +(process.env.H || 900);
  const SCALE = +(process.env.SCALE || 1);
  fs.mkdirSync(outdir, { recursive: true });
  const browser = await chromium.launch({
    args: ['--no-sandbox', '--allow-file-access-from-files', '--force-color-profile=srgb'],
  });
  const ctx = await browser.newContext({
    viewport: { width: W, height: H },
    deviceScaleFactor: SCALE,
  });
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push('pageerror: ' + e.message));
  page.on('console', (m) => { if (m.type() === 'error') errors.push('console: ' + m.text()); });
  const url = 'file://' + path.resolve(html);
  await page.goto(url);
  await page.waitForFunction(() => window.__deck && window.__deck.ready, null, { timeout: 20000 });
  await page.evaluate(() => document.fonts.ready);
  await page.addStyleTag({ content: '#controls{display:none!important}' });
  const total = await page.evaluate(() => window.__deck.count());
  if (process.env.LIGHT) await page.evaluate(() => window.__deck.setTheme('light'));
  let list = [];
  for (const s of specs) {
    if (s === 'all') { for (let i = 1; i <= total; i++) list.push({ n: i, step: -1, wait: 0 }); continue; }
    const m = /^(\d+)(?:@(-?\d+))?(?:\+(\d+))?$/.exec(s);
    if (!m) { console.error('bad spec', s); continue; }
    list.push({ n: +m[1], step: m[2] === undefined ? -1 : +m[2], wait: +(m[3] || 0) });
  }
  for (const it of list) {
    const steps = await page.evaluate(({ n, step }) => window.__deck.goto(n, step), it);
    await page.waitForTimeout(1400 + it.wait);
    const name = `s${String(it.n).padStart(2, '0')}${it.step >= 0 ? '@' + it.step : ''}${process.env.LIGHT ? '-light' : ''}.png`;
    const clip = process.env.CLIP ? process.env.CLIP.split(',').map(Number) : null;
    await page.screenshot(clip ? { path: path.join(outdir, name), clip: { x: clip[0], y: clip[1], width: clip[2], height: clip[3] } } : { path: path.join(outdir, name) });
    console.log('shot', name, 'steps=' + steps);
  }
  if (errors.length) { console.log('ERRORS:\n' + errors.join('\n')); process.exitCode = 2; }
  await browser.close();
})();
