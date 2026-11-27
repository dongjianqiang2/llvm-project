// Layout audit: per slide (final step), report content bbox in stage coords.
// usage: node measure.js <html>
const { chromium } = require(process.env.PW_PATH || 'playwright');
const path = require('path');
(async () => {
  const browser = await chromium.launch({ args: ['--no-sandbox', '--allow-file-access-from-files'] });
  const page = await browser.newPage({ viewport: { width: 1600, height: 900 } });
  await page.goto('file://' + path.resolve(process.argv[2]));
  await page.waitForFunction(() => window.__deck && window.__deck.ready);
  await page.evaluate(() => document.fonts.ready);
  await page.addStyleTag({ content: '#controls{display:none!important}' });
  const total = await page.evaluate(() => window.__deck.count());
  for (let n = 1; n <= total; n++) {
    const r = await page.evaluate((n) => {
      window.__deck.goto(n, -1);
      const st = document.getElementById('stage').getBoundingClientRect();
      const k = st.width / 1600;
      const slide = document.querySelectorAll('#slides .slide')[n - 1];
      if (!slide.querySelector('.s-body')) return null;      // free-form slide (cover / opener): nothing to audit
      let bot = 0, right = 0, top = 1e9, left = 1e9, worst = '';
      for (const el of slide.querySelectorAll('.s-body *, .s-body')) {
        const cs = getComputedStyle(el);
        if (cs.display === 'none' || cs.visibility === 'hidden' || +cs.opacity === 0) continue;
        if (el.closest('.step:not(.on)') || el.classList.contains('canvas')) continue;
        if (el.closest('svg') && !el.closest('svg').classList.contains('svg')) continue;
        const b = el.getBoundingClientRect();
        if (!b.width && !b.height) continue;
        // skip svg internals: use the svg root only
        if (el.ownerSVGElement || el.tagName.toLowerCase() === 'svg') continue;
        const x1 = (b.right - st.left) / k, y1 = (b.bottom - st.top) / k;
        const x0 = (b.left - st.left) / k, y0 = (b.top - st.top) / k;
        if (y1 > bot) { bot = y1; worst = el.className || el.tagName; }
        if (x1 > right) right = x1;
        if (y0 < top) top = y0;
        if (x0 < left) left = x0;
      }
      return { bot, right, top, left, worst: String(worst).slice(0, 40), name: slide.dataset.name, steps: slide.dataset.steps };
    }, n);
    if (!r) { console.log(`${String(n).padStart(2)} (free-form slide)`); continue; }
    const gap = Math.round(824 - r.bot);
    const flag = r.bot > 830 ? ' OVERFLOW-B' : gap > 130 ? ' empty' : '';
    const flagR = r.right > 1506 ? ' OVERFLOW-R' : '';
    console.log(`${String(n).padStart(2)} ${r.name.padEnd(22)} bottom=${Math.round(r.bot)} gap=${String(gap).padStart(4)} right=${Math.round(r.right)} top=${Math.round(r.top)}${flag}${flagR}   <${r.worst}>`);
  }
  await browser.close();
})();
