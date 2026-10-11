// UI smoke test: loads the dashboard in a real headless browser with the synthetic fixture
// session, opens every view, and fails on any JavaScript error. Screenshots go to /out.
// Run inside the Playwright Docker image (see tools/test_ui.ps1).
// CHROME=<path> uses a system Chromium with playwright-core (e.g. Alpine: apk add chromium).
const { chromium } = await import('playwright').catch(() => import('playwright-core'));

const base = process.env.BASE || 'http://localhost:8000';
const out = process.env.OUT || '/out';
const views = ['overview', 'tasks', 'timeline', 'analysis', 'resources', 'alerts', 'faults', 'motor'];
const errors = [];

const browser = await chromium.launch(process.env.CHROME ? { executablePath: process.env.CHROME } : {});
for (const scheme of ['light', 'dark']) {
  const page = await browser.newPage({ viewport: { width: 1360, height: 900 }, colorScheme: scheme });
  page.on('pageerror', e => errors.push(`[${scheme}] page error: ${e.message}`));
  page.on('console', m => { if (m.type() === 'error' && !/WebSocket|ws:\/\/|favicon|Failed to load resource/.test(m.text())) errors.push(`[${scheme}] console: ${m.text()}`); });
  page.on('response', r => { if (r.status() >= 400 && !/favicon/.test(r.url())) errors.push(`[${scheme}] HTTP ${r.status()}: ${r.url()}`); });
  await page.goto(`${base}/index.html?session=tests/fixture-session.json`);
  await page.waitForTimeout(8500);                         // let the 9 s fixture play past the injected fault
  for (const v of views) {
    await page.click(`.tab[data-view="${v}"]`);
    if (v === 'tasks') await page.click('#tk-body tr[data-tag="3"]');
    if (v === 'analysis') await page.click('#an-body tr[data-tag="3"] td:nth-child(2)');
    await page.waitForTimeout(700);
    await page.screenshot({ path: `${out}/${scheme}-${v}.png`, fullPage: true });
  }
  // a few content checks: the data really reached the views
  await page.click('.tab[data-view="overview"]');
  await page.waitForTimeout(600);
  const board = await page.locator('#ov-board .brow:not(.bhead)').count();
  if (board < 10) errors.push(`[${scheme}] inspector board has ${board} task rows`);
  const late = await page.locator('#ov-board .brow.s-bad').count();
  if (late < 1) errors.push(`[${scheme}] the injected CPU hog made no task late on the board`);
  if (!/On the CPU at the last event/.test(await page.textContent('#ov-strip-legend') || '')) errors.push(`[${scheme}] CPU strip legend missing`);
  await page.click('.tab[data-view="tasks"]');
  const rows = await page.locator('#tk-body tr').count();
  if (rows < 5) errors.push(`[${scheme}] tasks table has ${rows} rows`);
  await page.click('.tab[data-view="alerts"]');
  const alerts = await page.locator('#al-list li').count();
  if (alerts < 5) errors.push(`[${scheme}] alerts list has ${alerts} entries`);
  await page.click('.tab[data-view="timeline"]');
  await page.waitForTimeout(300);
  const info = await page.textContent('#tl-info');
  if (!/events in memory/.test(info || '')) errors.push(`[${scheme}] timeline did not draw`);
  await page.close();
}
// phone width
const phone = await browser.newPage({ viewport: { width: 390, height: 844 } });
phone.on('pageerror', e => errors.push(`[phone] page error: ${e.message}`));
await phone.goto(`${base}/index.html?session=tests/fixture-session.json`);
await phone.waitForTimeout(4000);
const overflow = await phone.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
if (overflow > 2) errors.push(`[phone] horizontal overflow of ${overflow}px`);
await phone.screenshot({ path: `${out}/phone-overview.png`, fullPage: true });
await browser.close();

if (errors.length) { console.log('UI SMOKE TEST FAILED\n' + errors.join('\n')); process.exit(1); }
console.log('UI smoke test passed: 8 views x 2 themes + phone width, no JavaScript error');
