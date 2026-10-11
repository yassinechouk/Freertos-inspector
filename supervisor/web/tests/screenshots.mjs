// README screenshots from the SYNTHETIC fixture session (tests/make_fixture.py): the dashboard
// replays a simulated task set, and every picture shows the "simulated data" badge.
// Usage (dashboard served on BASE, Playwright installed): OUT=docs/images node screenshots.mjs
const { chromium } = await import('playwright').catch(() => import('playwright-core'));

const base = process.env.BASE || 'http://localhost:8000';
const out = process.env.OUT || '.';
const browser = await chromium.launch(process.env.CHROME ? { executablePath: process.env.CHROME } : {});
const page = await browser.newPage({ viewport: { width: 1360, height: 900 }, colorScheme: 'light', deviceScaleFactor: 1 });
const shot = (name, opts = {}) => page.screenshot({ path: `${out}/${name}.png`, ...opts });

await page.goto(`${base}/index.html?session=tests/fixture-session.json`);
await page.click('.tab[data-view="overview"]');
await page.waitForTimeout(5600);                                  // healthy: before the fault at 6 s
await page.click('#s-play');                                      // pause the replay
await page.waitForTimeout(400);
await shot('inspector', { fullPage: true });
await page.click('.tab[data-view="timeline"]');
await page.selectOption('#tl-win', '50');
await page.waitForTimeout(400);
await shot('timeline');
await page.click('.tab[data-view="analysis"]');
await page.click('#an-body tr[data-tag="2"] td:nth-child(2)');
await page.waitForTimeout(400);
await shot('analysis', { fullPage: true });
await page.click('#s-play');                                      // resume: the CPU hog is injected at 6 s
await page.click('.tab[data-view="overview"]');
await page.waitForTimeout(3000);
await shot('inspector-fault', { fullPage: true });
await page.click('.tab[data-view="tasks"]');
await page.click('#tk-body tr[data-tag="3"]');
await page.waitForTimeout(600);
await shot('tasks', { fullPage: true });
await browser.close();
console.log(`screenshots written to ${out}`);
