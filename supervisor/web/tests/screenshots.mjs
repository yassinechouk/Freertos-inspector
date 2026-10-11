// README screenshots from a recorded session (Record button) replayed in the dashboard.
// Usage, with the dashboard served on BASE and Playwright installed:
//   SESSION=tests/<file>.json HEALTHY=75 FAULT=91 OUT=docs/images node screenshots.mjs
// HEALTHY and FAULT are session times in seconds: before and just after a fault injected from the Fault lab.
// Without SESSION, the synthetic fixture (tests/make_fixture.py) is used, with its CPU hog at 6 s.
import { readFileSync } from 'node:fs';
const { chromium } = await import('playwright').catch(() => import('playwright-core'));

const base = process.env.BASE || 'http://localhost:8000';
const out = process.env.OUT || '.';
const session = process.env.SESSION || 'tests/fixture-session.json';
const healthy = +(process.env.HEALTHY || 5.6), fault = +(process.env.FAULT || 8.6);
const root = process.env.WEB || new URL('..', import.meta.url).pathname;
const times = JSON.parse(readFileSync(root + '/' + session, 'utf8')).messages.map(m => m[0]);
const indexAt = s => times.findIndex(t => t >= s * 1000);           // first message at or after s seconds

const browser = await chromium.launch(process.env.CHROME ? { executablePath: process.env.CHROME } : {});
const page = await browser.newPage({ viewport: { width: 1360, height: 900 }, colorScheme: 'light' });
const shot = (name, opts = {}) => page.screenshot({ path: `${out}/${name}.png`, ...opts });
const position = async () => +((await page.textContent('#h-src')).match(/replay (\d+)/) || [0, 0])[1];
async function runTo(s) {                     // play at 5x, the last 3 s at 1x, then pause at s seconds
  if ((await page.textContent('#s-play')).includes('Play')) await page.click('#s-play');
  for (const [speed, until] of [['5', s - 3], ['1', s]]) {
    await page.selectOption('#s-speed', speed);
    const target = indexAt(until);
    while (await position() < target) await page.waitForTimeout(20);
  }
  await page.click('#s-play');
  await page.waitForTimeout(600);
}

await page.goto(`${base}/index.html?session=${session}`);
await page.waitForTimeout(500);
await page.click('.tab[data-view="overview"]');
await runTo(healthy);
await shot('inspector', { fullPage: true });
await page.click('.tab[data-view="motor"]');
await page.waitForTimeout(1500);                                  // the 3D view loads on its own
await shot('motor', { fullPage: true });
await page.click('.tab[data-view="timeline"]');
await page.selectOption('#tl-win', '50');
await page.waitForTimeout(400);
await shot('timeline');
await page.click('.tab[data-view="overview"]');
await runTo(fault);
await shot('inspector-fault', { fullPage: true });
await browser.close();
console.log(`screenshots written to ${out}`);
