// Inspector: the landing view. In one screen: is everything fine, what runs on the CPU right now,
// and for every task its priority, period, deadline, response time against that deadline,
// start delay and trend. Then the CPU load over time, the health categories and the latest alerts.
import { S, color, fmtMs, escapeHtml } from './store.js';
import { analyse, health, verdict, describeTasks } from './model.js';
import { alerts, titleOf } from './alerts.js';
import { recentActivity } from './trace-model.js';

const $ = id => document.getElementById(id);
const css = n => getComputedStyle(document.documentElement).getPropertyValue(n).trim();
const CATS = [
  ['sched', 'Scheduling', 'deadlines guaranteed by the analysis and met in the measurements'],
  ['cpu', 'CPU', 'load and context switches'],
  ['mem', 'Memory', 'stack margins and heap'],
  ['queues', 'Queues', 'fill level and overflows of registered queues'],
  ['irq', 'Interrupts', 'handler duration and latency to the woken task'],
  ['probe', 'Observation', 'cost of the probe, trace losses, link errors'],
];
const WORD = { ok: 'All tasks on time', warn: 'Attention needed', bad: 'Deadline problem', off: 'Waiting for data' };
const ICON = { ok: '✓', warn: '!', bad: '✕', off: '–' };
const STRIP_MS = 50, DELAY_MS = 500;
let stripHover = null, loadHover = null, lastStrip = null;

export function init() {
  hoverOn($('ov-strip'), p => { stripHover = p; drawStrip(); });
  hoverOn($('ov-load'), p => { loadHover = p; drawLoad(); });
}

function hoverOn(cv, cb) {
  cv.addEventListener('mousemove', e => { const r = cv.getBoundingClientRect(); cb({ x: e.clientX - r.left, y: e.clientY - r.top }); });
  cv.addEventListener('mouseleave', () => cb(null));
}

export function render() {
  const a = analyse();
  const h = health(a);
  const v = verdict(h);
  const tasks = describeTasks(a);
  const delays = recentActivity(S.trace, S.fcpu, DELAY_MS);
  lastStrip = recentActivity(S.trace, S.fcpu, STRIP_MS);

  renderHero(v, tasks, a);
  drawStrip();
  renderBoard(tasks, delays);
  drawLoad();
  renderCpu(tasks);

  $('ov-lights').innerHTML = CATS.map(([k, name, desc]) => `
    <div class="light ${h[k].s}" title="${escapeHtml(desc)}">
      <i>${ICON[h[k].s]}</i><div><b>${name}</b><span>${escapeHtml(h[k].text)}</span></div>
    </div>`).join('');

  const recent = alerts.filter(x => x.sev !== 'info').slice(-6).reverse();
  $('ov-alerts').innerHTML = recent.length ? recent.map(x => `
    <li class="${x.sev}"><b>${escapeHtml(titleOf(x))}</b><small>${new Date(x.at).toLocaleTimeString()}${x.count > 1 ? ' · ×' + x.count : ''}</small></li>`).join('')
    : '<li class="ok"><b>No warning so far.</b></li>';
}

/* ---- Verdict and key figures ------------------------------------------------------------ */
function renderHero(v, tasks, a) {
  const el = $('ov-verdict');
  el.className = 'hero wide ' + v;
  const withD = tasks.filter(t => t.D);
  const late = tasks.filter(t => t.status.s === 'bad');
  const s = S.sys;
  const sentence = v === 'off' ? 'Connect the target, or load a recorded session.'
    : late.length ? `${late.map(t => t.name).join(', ')} ${late.length > 1 ? 'are' : 'is'} late or not guaranteed. The rows in red below say why.`
    : `${tasks.length} tasks discovered, ${withD.length} with a deadline${withD.length ? ', all met' : ''}${s ? `, CPU ${(100 - s.idle / 10).toFixed(0)} % busy` : ''}.`
      + (v === 'warn' ? ' A health category needs attention: see Health below.' : '');
  el.querySelector('b').textContent = WORD[v];
  el.querySelector('span').textContent = sentence;

  const load = s ? 100 - s.idle / 10 : 0;
  const misses = withD.reduce((n, t) => n + t.miss10, 0);
  const tight = withD.filter(t => t.Rc != null && isFinite(t.Rc)).map(t => ({ t, m: 1 - t.Rc / t.D })).sort((x, y) => x.m - y.m)[0];
  const tile = (label, val, sub, cls = '', hint = '') => `<div class="tile ${cls}" title="${escapeHtml(hint)}"><small>${label}</small><b>${val}</b><span>${sub}</span></div>`;
  $('ov-kpis').innerHTML = [
    tile('CPU load', s ? load.toFixed(1) + ' %' : '–', s ? `${s.sw} switches/s` : '&nbsp;', !s ? '' : load > 90 ? 'bad' : load > 70 ? 'warn' : '', 'Share of the last second not spent in the idle task'),
    tile('Deadline misses', withD.length ? misses : '–', 'last 10 s', misses ? 'bad' : withD.length ? 'ok' : '', 'Jobs that finished after their deadline, all tasks with a deadline'),
    tile('Tightest task', tight ? escapeHtml(tight.t.name) : '–', !tight ? '&nbsp;' : tight.m < 0 ? `computed R ${(-tight.m * 100).toFixed(0)} % over the deadline` : `${(tight.m * 100).toFixed(0)} % margin (computed)`, tight && tight.m < 0 ? 'bad' : tight && tight.m < 0.2 ? 'warn' : '', 'Smallest margin between the worst-case response time computed by the analysis and the deadline'),
    tile('Tasks', tasks.length || '–', `${withD.length} with a deadline`, '', 'Discovered automatically when FreeRTOS created them'),
    tile('Utilisation', a.list.length ? (a.U * 100).toFixed(1) + ' %' : '–', a.list.length ? `bound ${(a.LL * 100).toFixed(0)} % (Liu-Layland)` : '&nbsp;', a.U > 1 ? 'bad' : '', 'Sum of C/T over the analysed tasks and interrupts'),
    tile('Probe cost', s ? (s.probe / 10).toFixed(2) + ' %' : '–', 'CPU spent observing', s && s.probe > 20 ? 'warn' : '', 'CPU time spent in the RTOS Doctor hooks'),
  ].join('');
}

/* ---- On the CPU now: one row, coloured by task -------------------------------------------- */
function drawStrip() {
  const cv = $('ov-strip'), g = setup(cv);
  if (!g) return;
  const { W, H } = g;
  const r = lastStrip;
  const axis = 16, top = 2, h = H - axis - top;
  if (!r) { text(g.c, 'Waiting for trace events…', 8, H / 2, css('--mut')); $('ov-strip-legend').innerHTML = ''; return; }
  const x = t => (t - r.tStart) / r.span * W;
  g.c.fillStyle = css('--hover'); g.c.fillRect(0, top, W, h);
  for (const sg of r.segs) {
    if (sg.kind !== 'run' || sg.key === 'isr') continue;
    g.c.fillStyle = color(sg.key);
    g.c.fillRect(x(Math.max(sg.a, r.tStart)), top, Math.max(1, x(sg.b) - x(Math.max(sg.a, r.tStart))), h);
  }
  g.c.fillStyle = css('--bad');                             // interrupts: a thin band on top
  for (const sg of r.segs) if (sg.key === 'isr') g.c.fillRect(x(Math.max(sg.a, r.tStart)), top, Math.max(1.5, x(sg.b) - x(sg.a)), 5);
  g.c.fillStyle = css('--mut'); g.c.font = '11px system-ui, sans-serif';
  for (let ms = 0; ms <= STRIP_MS; ms += 10) {
    const xx = ms / STRIP_MS * (W - 1);
    g.c.fillRect(xx, H - axis, 1, 4);
    const lab = ms === STRIP_MS ? 'now' : `−${STRIP_MS - ms} ms`;
    g.c.fillText(lab, Math.min(Math.max(0, xx - 14), W - 26), H - 2);
  }
  if (stripHover) {
    const t = r.tStart + stripHover.x / W * r.span;
    const sg = r.segs.find(s => s.kind === 'run' && s.key !== 'isr' && s.a <= t && s.b >= t);
    g.c.fillStyle = css('--fg'); g.c.fillRect(stripHover.x, top, 1, h);
    if (sg) tip(g.c, `${S.tasks.get(sg.key)?.cfg?.name ?? 'task ' + sg.key}: ran ${fmtMs(sg.b - sg.a)}`, stripHover.x, top + 4, W);
  }
  const share = [...r.act.entries()].filter(([, v]) => v.run > 0).sort((p, q) => q[1].run - p[1].run);
  const name = tag => S.tasks.get(tag)?.cfg?.name ?? `task ${tag}`;
  $('ov-strip-legend').innerHTML = (r.running != null ? `<span class="now">On the CPU at the last event: <b>${escapeHtml(name(r.running))}</b></span>` : '')
    + share.map(([tag, v]) => `<span><i class="sw" style="background:${color(tag)}"></i>${escapeHtml(name(tag))} <b>${(v.run / r.span * 100).toFixed(0)} %</b></span>`).join('');
}

/* ---- Task board ---------------------------------------------------------------------------- */
function renderBoard(tasks, delays) {
  const head = `<div class="brow bhead" role="row">
      <div role="columnheader">Task</div><div role="columnheader" title="FreeRTOS priority: higher runs first">Priority</div>
      <div role="columnheader" title="Measured time between two releases">Period</div><div role="columnheader">Deadline</div>
      <div role="columnheader">Response time vs deadline</div><div role="columnheader">Status</div>
      <div role="columnheader" title="CPU share, last second">CPU</div><div role="columnheader" title="Longest wait from ready to running, last 0.5 s">Start delay</div>
      <div role="columnheader" title="Worst response time per second, as a share of the deadline">Trend (60 s)</div></div>`;
  const rows = tasks.map(t => {
    const st = t.t.st || {};
    const jitter = st.tavg ? Math.max(st.tmax - st.tavg, st.tavg - st.tmin) : 0;
    const period = t.kind === 'periodic' ? `${fmtMs(t.T)}<small>± ${fmtMs(jitter)} jitter</small>`
      : t.kind === 'event' ? (t.T ? `~ ${fmtMs(t.T)}<small>irregular</small>` : '–<small>on demand</small>') : '–<small>&nbsp;</small>';
    const deadline = t.D ? `${fmtMs(t.D)}<small>${t.declared ? 'declared' : '= period (implicit)'}</small>`
      : `–<small>${t.idle ? '&nbsp;' : 'none'}</small>`;
    const ac = delays && delays.act.get(t.tag);
    const delay = t.idle ? '–' : ac && ac.starts ? `${fmtMs(ac.delayMax)}<small>${ac.starts} start${ac.starts > 1 ? 's' : ''}</small>` : '–<small>no release</small>';
    const boosted = t.prioNow != null && t.prioNow !== t.prio;
    const running = lastStrip && lastStrip.running === t.tag;
    return `<div class="brow s-${t.status.s}${t.idle ? ' idle' : ''}" role="row" data-tag="${t.tag}">
      <div class="b-name"><i class="sw" style="background:${color(t.tag)}"></i><b>${escapeHtml(t.name)}</b>${running ? '<em class="live" title="on the CPU at the last trace event">running</em>' : ''}</div>
      <div class="b-prio"><span class="prio">${t.prio}</span>${boosted ? `<span class="boost" title="priority inherited from a mutex waiter">→ ${t.prioNow}</span>` : ''}</div>
      <div class="b-num" data-label="Period">${period}</div>
      <div class="b-num" data-label="Deadline">${deadline}</div>
      <div class="b-bar">${rbar(t)}</div>
      <div class="b-status"><span class="chip ${t.status.s}"><i>${ICON[t.status.s]}</i>${escapeHtml(t.status.text)}</span></div>
      <div class="b-num" data-label="CPU">${t.idle || t.t.st ? t.cpu.toFixed(1) + ' %' : '–'}</div>
      <div class="b-num" data-label="Start delay">${delay}</div>
      <div class="b-trend"><canvas class="spark" data-tag="${t.tag}"></canvas></div>
    </div>`;
  }).join('');
  $('ov-board').innerHTML = tasks.length ? head + rows : '<p class="note">No task discovered yet. The probe reports every task the moment FreeRTOS creates it.</p>';
  for (const cv of $('ov-board').querySelectorAll('canvas.spark')) {
    const t = tasks.find(x => x.tag === +cv.dataset.tag);
    trend(cv, t);
  }
}

/* Horizontal bar on one scale: C (solid), R (lighter, up to R), deadline (line), computed R (tick). */
function rbar(t) {
  if (t.idle || !t.R) return `<div class="rbar empty"></div><small>${t.idle ? 'runs when nothing else is ready' : 'no complete job yet'}</small>`;
  const scale = Math.max(t.D || 0, t.R, isFinite(t.Rc) ? t.Rc || 0 : 0, t.C) * 1.12;
  const pct = v => Math.min(100, v / scale * 100).toFixed(2);
  const col = color(t.tag);
  const share = t.D ? ` · <b>${(t.R / t.D * 100).toFixed(0)} %</b> of the deadline` : '';
  return `<div class="rbar" title="C ${fmtMs(t.C)} · R ${fmtMs(t.R)}${t.D ? ' · D ' + fmtMs(t.D) : ''}${t.Rc != null ? ' · computed R ' + fmtMs(t.Rc) : ''}">
      <span class="r" style="width:${pct(t.R)}%;background:${col}"></span>
      <span class="c" style="width:${pct(t.C)}%;background:${col}"></span>
      ${t.Rc != null && isFinite(t.Rc) ? `<i class="rc" style="left:${pct(t.Rc)}%"></i>` : ''}
      ${t.D ? `<i class="d" style="left:${pct(t.D)}%"></i>` : ''}
    </div><small>R ${fmtMs(t.R)}${share}</small>`;
}

function trend(cv, t) {
  const g = setup(cv);
  if (!g || !t || t.idle) return;
  const { c, W, H } = g;
  const hist = t.t.hist;
  const vals = (t.D ? hist.rmax.map(r => r / t.D * 100) : hist.cpu).slice(-60);
  if (!vals.length) return;
  const top = t.D ? Math.max(120, ...vals) : Math.max(5, ...vals);
  const y = v => H - 2 - Math.min(v, top) / top * (H - 4);
  if (t.D) { c.fillStyle = css('--bad'); c.globalAlpha = 0.55; c.fillRect(0, Math.round(y(100)), W, 1); c.globalAlpha = 1; }
  c.strokeStyle = color(t.tag); c.lineWidth = 1.5; c.lineJoin = 'round'; c.beginPath();
  vals.forEach((v, i) => { const xx = (60 - vals.length + i) / 59 * (W - 2) + 1; i ? c.lineTo(xx, y(v)) : c.moveTo(xx, y(v)); });
  c.stroke();
  cv.title = t.D ? `Worst R per second as a share of the deadline (line = 100 %). Last: ${vals[vals.length - 1].toFixed(0)} %, peak ${Math.max(...vals).toFixed(0)} %`
    : `CPU share per second (no deadline). Last: ${vals[vals.length - 1].toFixed(1)} %`;
}

/* ---- CPU load over time --------------------------------------------------------------------- */
function drawLoad() {
  const cv = $('ov-load'), g = setup(cv);
  if (!g) return;
  const { c, W, H } = g;
  const vals = S.loadHist.slice(-120);
  const left = 34, bottom = 18, top = 6, w = W - left - 6, h = H - top - bottom;
  const x = i => left + (120 - vals.length + i) / 119 * w, y = v => top + h - v / 100 * h;
  c.font = '11px system-ui, sans-serif';
  for (const p of [0, 25, 50, 75, 100]) {
    c.fillStyle = css('--line'); c.fillRect(left, Math.round(y(p)), w, 1);
    c.fillStyle = css('--mut'); c.fillText(p + ' %', 0, y(p) + 4);
  }
  for (const [s, lab] of [[0, '−2 min'], [60, '−1 min'], [119, 'now']]) c.fillText(lab, Math.min(left + s / 119 * w - 12, W - 26), H - 3);
  if (vals.length < 2) { $('ov-load-note').textContent = 'One point per second once the target reports.'; return; }
  const col = css('--series');
  c.beginPath(); c.moveTo(x(0), y(0)); vals.forEach((v, i) => c.lineTo(x(i), y(v))); c.lineTo(x(vals.length - 1), y(0)); c.closePath();
  c.fillStyle = col; c.globalAlpha = 0.14; c.fill(); c.globalAlpha = 1;
  c.beginPath(); vals.forEach((v, i) => (i ? c.lineTo(x(i), y(v)) : c.moveTo(x(i), y(v))));
  c.strokeStyle = col; c.lineWidth = 2; c.lineJoin = 'round'; c.stroke();
  if (loadHover && loadHover.x >= left) {
    const i = Math.round((loadHover.x - left) / w * 119) - (120 - vals.length);
    if (i >= 0 && i < vals.length) {
      c.fillStyle = css('--mut'); c.fillRect(x(i), top, 1, h);
      c.beginPath(); c.arc(x(i), y(vals[i]), 4, 0, 7); c.fillStyle = col; c.fill(); c.strokeStyle = css('--card'); c.lineWidth = 2; c.stroke();
      tip(c, `${vals[i].toFixed(1)} % busy, ${vals.length - 1 - i} s ago`, x(i), top + 4, W);
    }
  }
  const peak = Math.max(...vals);
  $('ov-load-note').textContent = `Now ${vals[vals.length - 1].toFixed(1)} %, peak ${peak.toFixed(1)} % over the last ${vals.length} s. Busy = everything except the idle task.`;
}

function renderCpu(tasks) {
  const rows = tasks.filter(t => t.t.st && !t.idle).sort((p, q) => q.cpu - p.cpu);
  const max = Math.max(1, ...rows.map(t => t.cpu));
  $('ov-cpu').innerHTML = rows.map(t => `
    <div class="cpurow"><span><i class="sw" style="background:${color(t.tag)}"></i>${escapeHtml(t.name)}</span>
      <div class="bar"><i style="width:${(t.cpu / max * 100).toFixed(1)}%;background:${color(t.tag)}"></i></div>
      <b>${t.cpu.toFixed(1)} %</b></div>`).join('') || '<p class="note">No task statistics yet.</p>';
}

/* ---- Canvas helpers ------------------------------------------------------------------------- */
function setup(cv) {
  const W = cv.clientWidth, H = cv.clientHeight, dpr = Math.min(devicePixelRatio || 1, 2);
  if (!W || !H) return null;
  if (cv.width !== Math.round(W * dpr) || cv.height !== Math.round(H * dpr)) { cv.width = Math.round(W * dpr); cv.height = Math.round(H * dpr); }
  const c = cv.getContext('2d');
  c.setTransform(dpr, 0, 0, dpr, 0, 0);
  c.clearRect(0, 0, W, H);
  return { c, W, H };
}
function text(c, s, x, y, col) { c.fillStyle = col; c.font = '12px system-ui, sans-serif'; c.fillText(s, x, y); }
function tip(c, s, x, y, W) {
  c.font = '12px system-ui, sans-serif';
  const w = c.measureText(s).width + 12, xx = Math.min(Math.max(0, x + 8), W - w);
  c.fillStyle = css('--fg'); c.globalAlpha = 0.92; c.fillRect(xx, y, w, 20); c.globalAlpha = 1;
  c.fillStyle = css('--card'); c.fillText(s, xx + 6, y + 14);
}
