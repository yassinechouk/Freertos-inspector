// RTOS layer: task table, measurements, Gantt timeline from the continuous trace,
// response-time analysis (with what-if editing) and the "doctor" findings.
const $ = id => document.getElementById(id);

const TR_ISR_IN = 0x40, TR_ISR_OUT = 0x80, TR_TICK = 0xC0, ISR_ADC_DMA = 1;
const tasks = new Map();          // tag -> { cfg, st, what: {prio, period, c} }
let sys = null, fcpu = 80e6;
let events = [];                  // [time in cycles (unwrapped, Number), id]
let lastRaw = null, wraps = 0, paused = false, windowMs = 20;
let droppedStm = 0, droppedHere = 0, isrMaxCyc = 0, dirty = true;

const us = cyc => cyc / fcpu * 1e6;
const fmtUs = cyc => cyc == null ? '-' : us(cyc) < 1000 ? us(cyc).toFixed(1) + ' µs' : (us(cyc) / 1000).toFixed(2) + ' ms';
const css = name => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

/* ---- Messages ----------------------------------------------------------------------- */
export function onTask(m) {
  const t = tasks.get(m.tag) || { what: {} };
  t.cfg = m;
  if (m.fcpu) fcpu = m.fcpu;
  tasks.set(m.tag, t);
  dirty = true;
}
export function onStats(m) {
  const t = tasks.get(m.tag) || { what: {} };
  t.st = m;
  tasks.set(m.tag, t);
  dirty = true;
}
export function onSys(m) { sys = m; if (m.fcpu) fcpu = m.fcpu; dirty = true; }

export function onTrace(buf) {
  const v = new DataView(buf);
  droppedStm = v.getUint16(1, true);
  droppedHere = v.getUint16(3, true);
  for (let o = 5; o + 5 <= buf.byteLength; o += 5) {
    const raw = v.getUint32(o, true), id = v.getUint8(o + 4);
    if (lastRaw !== null && raw < lastRaw && lastRaw - raw > 0x80000000) wraps++;   // 32-bit counter wrapped
    lastRaw = raw;
    events.push([wraps * 4294967296 + raw, id]);
  }
  const keep = fcpu * 0.4;                       // keep the last 400 ms
  const last = events.length ? events[events.length - 1][0] : 0;
  let cut = 0;
  while (cut < events.length && events[cut][0] < last - keep) cut++;
  if (cut) events.splice(0, cut);
  measureIsr();
}

/* Longest ADC DMA interrupt seen in the buffer: used as an interference term in the analysis. */
function measureIsr() {
  let start = null, max = 0;
  for (const [t, id] of events) {
    if (id === (TR_ISR_IN | ISR_ADC_DMA)) start = t;
    else if (id === (TR_ISR_OUT | ISR_ADC_DMA) && start !== null) { max = Math.max(max, t - start); start = null; }
  }
  if (max) isrMaxCyc = Math.max(isrMaxCyc * 0.98, max);   // slow decay: keeps the worst case recently seen
}

/* ---- Response-time analysis -------------------------------------------------------------- */
function model() {
  const list = [];
  for (const [tag, t] of tasks) {
    if (!t.cfg || !t.cfg.period) continue;       // idle, timers: no period, no interference model
    const measured = t.st && t.st.cmax ? t.st.cmax : 0;
    list.push({
      tag, name: t.cfg.name,
      prio: t.what.prio ?? t.cfg.prio,
      T: (t.what.period ?? t.cfg.period) / 1000 * fcpu,
      D: t.cfg.deadline / 1000 * fcpu,
      C: t.what.c != null ? t.what.c / 1e6 * fcpu : (measured || t.cfg.wcet),
      Cmeas: measured, Rmeas: t.st ? t.st.rmax : 0,
    });
  }
  return list;
}

/* R = C + sum over higher (or equal) priority work j of ceil(R / Tj) * (Cj + context switch),
   plus the ADC DMA interrupt (above every task). Iterated until it stops moving. */
function rta(list) {
  const ctx = sys ? sys.ctx : 0;
  const isr = isrMaxCyc ? { T: fcpu / 1000, C: isrMaxCyc } : null;
  for (const i of list) {
    const hp = list.filter(j => j !== i && j.prio >= i.prio);
    let R = i.C, prev = -1, iter = 0;
    const limit = Math.max(i.D, i.T) * 20;
    while (R !== prev && R <= limit && iter++ < 200) {
      prev = R;
      R = i.C + hp.reduce((s, j) => s + Math.ceil(prev / j.T) * (j.C + ctx), 0) + (isr ? Math.ceil(prev / isr.T) * isr.C : 0);
    }
    i.R = R > limit ? Infinity : R;
    i.iter = iter;
  }
  return list;
}

/* ---- Doctor ------------------------------------------------------------------------------- */
function doctor(list) {
  const f = [];
  const add = (level, text) => f.push({ level, text });
  for (const i of list) {
    if (i.D && i.R > i.D) add('bad', `${i.name}: computed worst response ${fmtUs(i.R)} exceeds its deadline ${fmtUs(i.D)}: not guaranteed.`);
    const t = tasks.get(i.tag);
    if (t.st && t.st.miss) add('bad', `${i.name}: ${t.st.miss} deadline miss(es) measured.`);
    if (t.cfg.wcet && i.Cmeas > t.cfg.wcet * 1.05) add('warn', `${i.name}: measured C ${fmtUs(i.Cmeas)} above the declared ${fmtUs(t.cfg.wcet)}.`);
    if (i.Rmeas && isFinite(i.R) && i.Rmeas > i.R * 1.02) add('warn', `${i.name}: measured response ${fmtUs(i.Rmeas)} above the computed ${fmtUs(i.R)}: something is not in the model (blocking, unmodelled interrupt).`);
    for (const j of list) {
      if (j !== i && j.T < i.T && j.prio < i.prio) {
        add('info', `${i.name} (T = ${fmtUs(i.T)}) has priority over ${j.name} (T = ${fmtUs(j.T)}): not rate-monotonic. Fine if intended (e.g. a measurement task), but it costs ${j.name} margin.`);
      }
    }
  }
  for (const [, t] of tasks) {
    if (t.cfg && t.st && t.cfg.stack && t.st.stack < t.cfg.stack * 0.2) add('warn', `${t.cfg.name}: only ${t.st.stack} of ${t.cfg.stack} stack words never used (< 20 %).`);
  }
  const U = list.reduce((s, i) => s + i.C / i.T, 0) + (isrMaxCyc ? isrMaxCyc / (fcpu / 1000) : 0);
  const n = list.length + (isrMaxCyc ? 1 : 0);
  const ll = n ? n * (Math.pow(2, 1 / n) - 1) : 1;
  if (U > ll) add('info', `Utilisation ${(U * 100).toFixed(1)} % is above the Liu-Layland bound ${(ll * 100).toFixed(1)} % for ${n} tasks: the bound is only sufficient, the response-time analysis above decides.`);
  if (droppedStm || droppedHere) add('warn', `Trace incomplete: ${droppedStm} event(s) lost on the STM32, ${droppedHere} on the supervisor (buffers full).`);
  if (sys && sys.isr > fcpu * 50e-6) add('info', `Worst interrupt-to-task latency ${fmtUs(sys.isr)} (above 50 µs).`);
  if (!f.some(x => x.level !== 'info')) add('ok', 'No problem found: every periodic task meets its deadline in the analysis and in the measurements.');
  return { list: f, U, ll };
}

/* ---- Rendering: summary, table, findings --------------------------------------------------- */
function render() {
  if (!dirty) return;
  dirty = false;
  const list = rta(model());
  const doc = doctor(list);
  const byTag = new Map(list.map(i => [i.tag, i]));

  if (sys) {
    $('rcpu').textContent = (100 - sys.idle / 10).toFixed(1) + ' %';
    $('rsw').textContent = sys.sw;
    $('rev').textContent = sys.ev;
    $('rctx').textContent = fmtUs(sys.ctx);
    $('risr').textContent = fmtUs(sys.isr);
  }
  $('rutil').textContent = (doc.U * 100).toFixed(1) + ' % (bound ' + (doc.ll * 100).toFixed(1) + ' %)';
  const worst = doc.list.some(x => x.level === 'bad') ? 'bad' : doc.list.some(x => x.level === 'warn') ? 'warn' : 'ok';
  const v = $('rverdict');
  v.textContent = worst === 'bad' ? 'NOT guaranteed' : worst === 'warn' ? 'guaranteed, with warnings' : 'guaranteed';
  v.className = 'verdict ' + worst;

  const rows = [...tasks.entries()].filter(([, t]) => t.cfg).sort((a, b) => b[1].cfg.prio - a[1].cfg.prio);
  if (!$('rtasks').contains(document.activeElement)) $('rtasks').innerHTML = rows.map(([tag, t]) => {
    const c = t.cfg, s = t.st || {}, m = byTag.get(tag);
    const margin = m && c.deadline && isFinite(m.R) ? (1 - m.R / m.D) * 100 : null;
    const cls = !m || !c.deadline ? '' : !isFinite(m.R) || m.R > m.D || s.miss ? 'bad' : margin < 20 ? 'warn' : 'ok';
    const edit = (field, val, unit) => m ? `<input data-tag="${tag}" data-f="${field}" type="number" value="${val}" title="what-if: ${unit}">` : val;
    return `<tr class="${cls}">
      <td><i style="background:${color(tag)}"></i>${c.name}</td>
      <td>${edit('prio', t.what.prio ?? c.prio, 'priority')}</td>
      <td>${c.period ? edit('period', t.what.period ?? c.period, 'ms') : '-'}</td>
      <td>${c.deadline || '-'}</td>
      <td>${c.wcet ? fmtUs(c.wcet) : '-'}</td>
      <td>${s.cmax != null ? fmtUs(s.cmax) : '-'}<br><small>${s.cavg != null ? 'avg ' + fmtUs(s.cavg) : ''}</small></td>
      <td>${m ? edit('c', t.what.c ?? Math.round(us(m.C)), 'µs used by the analysis') : '-'}</td>
      <td>${s.rmax != null ? fmtUs(s.rmax) : '-'}</td>
      <td>${m ? (isFinite(m.R) ? fmtUs(m.R) : '&infin;') : '-'}</td>
      <td>${margin == null ? '-' : margin.toFixed(0) + ' %'}</td>
      <td>${s.miss ?? '-'}</td>
      <td>${s.cpu != null ? (s.cpu / 10).toFixed(1) + ' %' : '-'}</td>
      <td>${s.stack != null ? s.stack + ' / ' + c.stack : '-'}</td></tr>`;
  }).join('');
  $('rfind').innerHTML = doc.list.map(x => `<li class="${x.level}">${x.text}</li>`).join('');
}

function color(tag) {
  if (tag === 8) return '#8a94a6';                // idle
  return `hsl(${(tag * 67) % 360} 65% 50%)`;
}

/* ---- Gantt ---------------------------------------------------------------------------------- */
function drawGantt() {
  const cv = $('gantt'), ctx2 = cv.getContext('2d');
  const W = cv.clientWidth, H = cv.clientHeight, dpr = Math.min(devicePixelRatio || 1, 2);
  if (cv.width !== W * dpr || cv.height !== H * dpr) { cv.width = W * dpr; cv.height = H * dpr; }
  ctx2.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx2.clearRect(0, 0, W, H);
  if (events.length < 2) { ctx2.fillStyle = css('--mut'); ctx2.fillText('waiting for trace events ...', 10, 20); return; }

  const rows = [...tasks.entries()].filter(([, t]) => t.cfg).sort((a, b) => b[1].cfg.prio - a[1].cfg.prio).map(([tag, t]) => ({ tag, name: t.cfg.name }));
  rows.unshift({ tag: 'isr', name: 'ADC DMA irq' });
  const rowIndex = new Map(rows.map((r, i) => [r.tag, i]));
  const left = 90, top = 6, axis = 22, rowH = Math.max(12, Math.min(26, (H - top - axis) / rows.length));
  const tEnd = events[events.length - 1][0], span = windowMs / 1000 * fcpu, tStart = tEnd - span;
  const x = t => left + (t - tStart) / span * (W - left - 6);

  ctx2.font = '12px system-ui, sans-serif';
  rows.forEach((r, i) => {
    ctx2.fillStyle = i % 2 ? 'transparent' : css('--line');
    ctx2.globalAlpha = 0.35; ctx2.fillRect(left, top + i * rowH, W - left, rowH); ctx2.globalAlpha = 1;
    ctx2.fillStyle = css('--fg');
    ctx2.fillText(r.name, 4, top + i * rowH + rowH * 0.7);
  });

  let cur = null, curT = null, isrT = null;
  for (const [t, id] of events) {
    if (id <= 0x3F) {                                    // task switched in
      if (cur !== null && t > tStart) bar(cur, Math.max(curT, tStart), t);
      cur = id; curT = t;
    } else if ((id & 0xC0) === TR_ISR_IN) { isrT = t; }
    else if ((id & 0xC0) === TR_ISR_OUT && isrT !== null) { if (t > tStart) bar('isr', Math.max(isrT, tStart), t, true); isrT = null; }
    else if (id === TR_TICK && t > tStart) {
      ctx2.fillStyle = css('--mut');
      ctx2.fillRect(x(t), H - axis, 1, 5);
    }
  }
  if (cur !== null) bar(cur, Math.max(curT, tStart), tEnd);

  function bar(tag, a, b, isr) {
    const i = rowIndex.get(tag);
    if (i === undefined) return;
    ctx2.fillStyle = isr ? css('--bad') : color(tag);
    ctx2.fillRect(x(a), top + i * rowH + 2, Math.max(1, x(b) - x(a)), rowH - 4);
  }

  ctx2.fillStyle = css('--mut');                          // time axis, ms from the window start
  const steps = windowMs <= 5 ? 1 : windowMs <= 20 ? 2 : windowMs <= 50 ? 5 : 10;
  for (let ms = 0; ms <= windowMs; ms += steps) {
    const xx = left + ms / windowMs * (W - left - 6);
    ctx2.fillRect(xx, H - axis, 1, 3);
    ctx2.fillText(ms + ' ms', xx - 10, H - 6);
  }
}

/* ---- What-if editing and controls ------------------------------------------------------------ */
export function init() {
  $('rtasks').addEventListener('change', e => {
    const el = e.target;
    if (!el.dataset.tag) return;
    const t = tasks.get(+el.dataset.tag);
    t.what[el.dataset.f] = el.value === '' ? undefined : +el.value;
    dirty = true; render();
  });
  $('rreset').onclick = () => { for (const [, t] of tasks) t.what = {}; dirty = true; render(); };
  $('gpause').onclick = () => { paused = !paused; $('gpause').textContent = paused ? 'Resume' : 'Pause'; };
  $('gwin').onchange = () => { windowMs = +$('gwin').value; drawGantt(); };
  addEventListener('resize', () => drawGantt());
  setInterval(() => { render(); if (!paused && document.getElementById('rtos').classList.contains('on')) drawGantt(); }, 200);
}
