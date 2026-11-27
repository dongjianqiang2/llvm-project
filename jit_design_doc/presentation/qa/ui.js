// UI-state screenshots: overview, notes, help, presenter, transitions, print.
const { chromium } = require(process.env.PW_PATH || 'playwright');
const path = require('path'); const fs = require('fs');
(async () => {
  const [html, outdir] = process.argv.slice(2);
  fs.mkdirSync(outdir, { recursive: true });
  const browser = await chromium.launch({ args: ['--no-sandbox', '--allow-file-access-from-files', '--force-color-profile=srgb'] });
  const ctx = await browser.newContext({ viewport: { width: 1600, height: 900 } });
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push('pageerror: ' + e.message));
  page.on('console', (m) => { if (m.type() === 'error') errors.push('console: ' + m.text()); });
  const url = 'file://' + path.resolve(html);
  await page.goto(url);
  await page.waitForFunction(() => window.__deck && window.__deck.ready);
  await page.evaluate(() => document.fonts.ready);
  await page.addStyleTag({ content: '#controls{display:none!important}' });
  const shot = async (name) => { await page.screenshot({ path: path.join(outdir, name) }); console.log('shot', name); };
  // overview
  await page.evaluate(() => window.__deck.goto(12));
  await page.keyboard.press('o'); await page.waitForTimeout(700); await shot('ui_overview.png');
  await page.keyboard.press('Escape'); await page.waitForTimeout(300);
  // notes drawer
  await page.keyboard.press('n'); await page.waitForTimeout(500); await shot('ui_notes.png');
  await page.keyboard.press('n'); await page.waitForTimeout(200);
  // help
  await page.keyboard.press('?'); await page.waitForTimeout(300); await shot('ui_help.png');
  await page.keyboard.press('Escape'); await page.waitForTimeout(200);
  // transitions: push (within act), shutter (act change), zoom
  await page.evaluate(() => { window.__deck.goto(9); window.__deck.motion(true); });
  await page.evaluate(() => { window.__deck.go(10); });
  await page.waitForTimeout(220); await shot('tr_push_220ms.png');
  await page.waitForTimeout(900);
  await page.evaluate(() => { window.__deck.goto(17); window.__deck.motion(true); });
  await page.evaluate(() => { window.__deck.go(18); });
  await page.waitForTimeout(330); await shot('tr_shutter_cover.png');
  await page.waitForTimeout(330); await shot('tr_shutter_swap.png');
  await page.waitForTimeout(400); await shot('tr_shutter_reveal.png');
  await page.waitForTimeout(900);
  await page.evaluate(() => { window.__deck.goto(7, -1); window.__deck.motion(true); });
  await page.evaluate(() => { window.__deck.go(8); });
  await page.waitForTimeout(300); await shot('tr_zoom_300ms.png');
  await page.waitForTimeout(900);
  // print layout (emulated print media, full page)
  await page.emulateMedia({ media: 'print' });
  await page.waitForTimeout(300);
  const h = await page.evaluate(() => document.documentElement.scrollHeight);
  console.log('print scrollHeight', h, 'expected', 900 * 40);
  await page.screenshot({ path: path.join(outdir, 'print_full.png'), fullPage: true });
  const pdf = await page.pdf({ path: path.join(outdir, 'deck.pdf'), width: '1600px', height: '900px', printBackground: true, preferCSSPageSize: true });
  console.log('pdf bytes', pdf.length);
  await page.emulateMedia({ media: 'screen' });
  // presenter window
  const p2 = await ctx.newPage();
  await p2.setViewportSize({ width: 1360, height: 900 });
  await p2.goto(url + '#presenter');
  await p2.waitForFunction(() => window.__deck && window.__deck.ready);
  await p2.evaluate(() => document.fonts.ready);
  // drive the main window; presenter should follow via postMessage
  await page.bringToFront();
  await page.evaluate(() => window.__deck.goto(14, 2));
  await page.evaluate(() => { window.opener = null; });
  await p2.waitForTimeout(1200);
  await p2.screenshot({ path: path.join(outdir, 'ui_presenter.png') });
  console.log('shot ui_presenter.png');
  if (errors.length) { console.log('ERRORS:\n' + errors.join('\n')); process.exitCode = 2; }
  await browser.close();
})();
