// Timeline: a Gantt chart rebuilt from the continuous trace, one row per task, highest priority
// at the top. Solid = running, light = ready (released or preempted, waiting for the CPU),
// red = interrupt handler, ▲ = release, | = the deadline of that release.
import { S, sortedTasks, color, fmtMs, escapeHtml } from './store.js';
import { describeTasks } from './model.js';
import { replay } from './trace-model.js';

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
  if (cv.width !== Math.round(W * dpr) || cv.height !== Math.round(H * dpr)) { cv.width = Math.round(W * dpr); cv.height = Math.round(H * dpr); }
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect(0, 0, W, H);
  g.font = '12px system-ui, sans-serif';
  if (ev.length < 2) { g.fillStyle = css('--mut'); g.fillText('Waiting for trace events…', 12, 24); return; }

  const deadlines = new Map(describeTasks().map(d => [d.tag, d.D]));
  const rows = [{ key: 'isr', name: 'ADC DMA irq', prio: 'irq' }, ...sortedTasks().map(([tag, t]) => ({ key: tag, name: t.cfg.name, prio: t.cfg.prio }))];
  const index = new Map(rows.map((r, i) => [r.key, i]));
  const left = 128, top = 4, axis = 22, right = 8;
  const rowH = Math.max(14, Math.min(26, (H - top - axis) / rows.length));
  const tEnd = ev[ev.length - 1][0], span = windowMs / 1000 * S.fcpu, tStart = tEnd - span;
  const x = t => left + (t - tStart) / span * (W - left - right);
  const r = replay(ev, tStart);

  rows.forEach((row, i) => {
    const y = top + i * rowH;
    if (i % 2 === 0) { g.fillStyle = css('--hover'); g.fillRect(left, y, W - left, rowH); }
    g.fillStyle = row.key === 'isr' ? css('--bad') : color(row.key);
    g.fillRect(6, y + rowH / 2 - 4, 8, 8);
    g.fillStyle = css('--fg');
    g.fillText(row.name.length > 11 ? row.name.slice(0, 10) + '…' : row.name, 20, y + rowH * 0.68);
    g.fillStyle = css('--mut');
    g.fillText(String(row.prio), left - 26, y + rowH * 0.68);
  });
  g.fillStyle = css('--line'); g.fillRect(left - 1, top, 1, rows.length * rowH);

  for (const sg of r.segs) {
    const i = index.get(sg.key);
    if (i === undefined) continue;
    const a = Math.max(sg.a, tStart), y = top + i * rowH;
    if (sg.kind === 'run') {
      g.fillStyle = sg.key === 'isr' ? css('--bad') : color(sg.key);
      g.fillRect(x(a), y + 3, Math.max(1, x(sg.b) - x(a)), rowH - 6);
    } else {
      g.fillStyle = color(sg.key); g.globalAlpha = 0.25;
      g.fillRect(x(a), y + rowH * 0.32, Math.max(1, x(sg.b) - x(a)), rowH * 0.36);
      g.globalAlpha = 1;
    }
  }
  // Releases (▲) and the matching absolute deadlines (|), when there is room to read them
  if (windowMs <= 50) {
    for (const { tag, t } of r.releases) {
      const i = index.get(tag);
      if (i === undefined) continue;
      const y = top + (i + 1) * rowH - 2, xx = x(t);
      g.fillStyle = css('--fg');
      g.beginPath(); g.moveTo(xx, y - 6); g.lineTo(xx - 3.5, y); g.lineTo(xx + 3.5, y); g.closePath(); g.fill();
      const D = deadlines.get(tag);
      if (D && t + D <= tEnd) { g.fillStyle = css('--bad'); g.fillRect(x(t + D), top + i * rowH + 1, 1.5, rowH - 2); }
    }
  }
  g.fillStyle = css('--mut');                                // ticks and time axis
  for (const t of r.ticks) g.fillRect(x(t), H - axis, 1, 5);
  const step = windowMs <= 5 ? 0.5 : windowMs <= 20 ? 2 : windowMs <= 50 ? 5 : windowMs <= 100 ? 10 : 50;
  for (let ms = 0; ms <= windowMs + 1e-9; ms += step) {
    const xx = left + ms / windowMs * (W - left - right);
    g.fillRect(xx, H - axis, 1, 3);
    const lab = ms === windowMs ? 'now' : `−${+(windowMs - ms).toFixed(1)} ms`;
    g.fillText(lab, Math.min(xx - 14, W - 34), H - 6);
  }
  if (hover && hover.x > left) {                             // cursor: which task, doing what
    const t = tStart + (hover.x - left) / (W - left - right) * span;
    const i = Math.floor((hover.y - top) / rowH), row = rows[i];
    g.strokeStyle = css('--fg'); g.globalAlpha = 0.4; g.beginPath(); g.moveTo(hover.x, 0); g.lineTo(hover.x, H - axis); g.stroke(); g.globalAlpha = 1;
    let label = `${fmtMs(t - tStart)} after the left edge`;
    if (row) {
      const sg = r.segs.find(s => s.key === row.key && s.a <= t && s.b >= t);
      if (sg) label = `${row.name}: ${sg.kind === 'run' ? 'running' : 'ready, waiting for the CPU'} for ${fmtMs(sg.b - sg.a)}`;
    }
    g.font = '12px system-ui, sans-serif';
    const w = g.measureText(label).width + 12, xx = Math.min(hover.x + 8, W - w);
    g.fillStyle = css('--fg'); g.globalAlpha = 0.92; g.fillRect(xx, 4, w, 20); g.globalAlpha = 1;
    g.fillStyle = css('--card'); g.fillText(label, xx + 6, 18);
  }
  $('tl-info').innerHTML = `${ev.length} events in memory · lost: ${S.traceDropStm} on the target, ${S.traceDropGw} on the supervisor`
    + (r.running != null ? ` · on the CPU at the last event: <b>${escapeHtml(S.tasks.get(r.running)?.cfg?.name ?? 'task ' + r.running)}</b>` : '');
}
