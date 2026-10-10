// Timeline: a Gantt chart rebuilt from the continuous trace. Solid = running,
// light = ready (released or preempted, waiting for the CPU), red = interrupt handler.
import { S, sortedTasks, color, fmtTime } from './store.js';

const $ = id => document.getElementById(id);
const css = n => getComputedStyle(document.documentElement).getPropertyValue(n).trim();
let windowMs = 20, paused = false, frozen = null, hover = null;

export function init() {
  $('tl-win').onchange = () => { windowMs = +$('tl-win').value; draw(); };
  $('tl-pause').onclick = () => {
    paused = !paused;
    frozen = paused ? S.trace.slice() : null;
    $('tl-pause').textContent = paused ? 'Resume' : 'Pause';
    draw();
  };
  const cv = $('tl-canvas');
  cv.addEventListener('mousemove', e => { const r = cv.getBoundingClientRect(); hover = { x: e.clientX - r.left, y: e.clientY - r.top }; draw(); });
  cv.addEventListener('mouseleave', () => { hover = null; draw(); });
  addEventListener('resize', draw);
}

export function render() { if (!paused) draw(); }

function draw() {
  const ev = frozen || S.trace;
  const cv = $('tl-canvas'), g = cv.getContext('2d');
  const W = cv.clientWidth, H = cv.clientHeight, dpr = Math.min(devicePixelRatio || 1, 2);
  if (!W) return;
  if (cv.width !== W * dpr || cv.height !== H * dpr) { cv.width = W * dpr; cv.height = H * dpr; }
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect(0, 0, W, H);
  g.font = '12px system-ui, sans-serif';
  if (ev.length < 2) { g.fillStyle = css('--mut'); g.fillText('Waiting for trace events…', 12, 24); return; }

  const rows = [{ key: 'isr', name: 'ADC DMA irq' }, ...sortedTasks().map(([tag, t]) => ({ key: tag, name: t.cfg.name }))];
  const index = new Map(rows.map((r, i) => [r.key, i]));
  const left = 96, top = 4, axis = 22;
  const rowH = Math.max(12, Math.min(24, (H - top - axis) / rows.length));
  const tEnd = ev[ev.length - 1][0], span = windowMs / 1000 * S.fcpu, tStart = tEnd - span;
  const x = t => left + (t - tStart) / span * (W - left - 8);

  rows.forEach((r, i) => {
    if (i % 2 === 0) { g.fillStyle = css('--line'); g.globalAlpha = 0.35; g.fillRect(left, top + i * rowH, W - left, rowH); g.globalAlpha = 1; }
    g.fillStyle = css('--fg');
    g.fillText(r.name.length > 13 ? r.name.slice(0, 12) + '…' : r.name, 6, top + i * rowH + rowH * 0.68);
  });

  const bar = (key, a, b, kind) => {
    const i = index.get(key);
    if (i === undefined || b <= tStart) return;
    a = Math.max(a, tStart);
    const y = top + i * rowH;
    if (kind === 'run') { g.fillStyle = key === 'isr' ? css('--bad') : color(key); g.fillRect(x(a), y + 2, Math.max(1, x(b) - x(a)), rowH - 4); }
    else { g.fillStyle = color(key); g.globalAlpha = 0.22; g.fillRect(x(a), y + rowH * 0.3, Math.max(1, x(b) - x(a)), rowH * 0.4); g.globalAlpha = 1; }
  };

  // Replay the events: who runs, who waits ready.
  let cur = null, curStart = null, isrStart = null;
  const readySince = new Map(), lastOut = new Map();
  for (const [t, id] of ev) {
    if (id <= 0x3f) {                                       // switched in
      if (cur !== null) { bar(cur, curStart, t, 'run'); lastOut.set(cur, t); }
      const r = readySince.has(id) ? readySince.get(id) : lastOut.get(id);   // released, or preempted earlier
      if (r !== undefined && r < t) bar(id, r, t, 'ready');
      readySince.delete(id);
      cur = id; curStart = t;
    } else if (id === 0xff) {
      if (t >= tStart) { g.fillStyle = css('--mut'); g.fillRect(x(t), H - axis, 1, 5); }
    } else if ((id & 0xc0) === 0xc0) {                      // released
      readySince.set(id & 0x3f, t);
      lastOut.delete(id & 0x3f);
    } else if ((id & 0xc0) === 0x40) { isrStart = t; }
    else if ((id & 0xc0) === 0x80 && isrStart !== null) { bar('isr', isrStart, t, 'run'); isrStart = null; }
  }
  if (cur !== null) bar(cur, curStart, tEnd, 'run');

  g.fillStyle = css('--mut');                                // time axis
  const step = windowMs <= 5 ? 0.5 : windowMs <= 20 ? 2 : windowMs <= 50 ? 5 : 10;
  for (let ms = 0; ms <= windowMs + 1e-9; ms += step) {
    const xx = left + ms / windowMs * (W - left - 8);
    g.fillRect(xx, H - axis, 1, 3);
    g.fillText(`${+(ms - windowMs).toFixed(1)} ms`, xx - 14, H - 6);
  }
  if (hover && hover.x > left) {                             // cursor with the time under it
    g.strokeStyle = css('--fg'); g.globalAlpha = 0.4; g.beginPath(); g.moveTo(hover.x, 0); g.lineTo(hover.x, H - axis); g.stroke(); g.globalAlpha = 1;
    const t = tStart + (hover.x - left) / (W - left - 8) * span;
    g.fillStyle = css('--fg'); g.fillText(`${fmtTime(t - tStart)} from the left edge`, Math.min(hover.x + 6, W - 150), 14);
  }
  $('tl-info').textContent = `${ev.length} events in memory · lost: ${S.traceDropStm} on the target, ${S.traceDropGw} on the supervisor`;
}
