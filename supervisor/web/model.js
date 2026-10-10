// Builds the analysis model from the measurements (and the what-if edits), runs the analysis,
// and grades the system. Used by the Overview, Tasks and Analysis views.
import { S } from './store.js';
import { responseTimes, utilisation, llBound, rmViolations } from './analysis-core.js';

/* Worst value of the last N seconds of a per-second history (0 if none). */
const recentMax = (a, n = 10) => Math.max(0, ...(a || []).slice(-n));

/* Periodic or sporadic tasks with a measured release interval. All times in CPU cycles.
   C = the what-if value if set, else the worst C measured in the last 10 s. Not the worst since
   reset: after an overload, a task that vTaskDelayUntil() lets catch up runs several periods in
   one job, and that one job would dominate the analysis for ever. D = explicit deadline, else the
   measured mean period (implicit deadline). */
export function buildModel() {
  const list = [];
  for (const [tag, t] of S.tasks) {
    if (!t.cfg || !t.st || (t.cfg.flags & 1)) continue;          // idle: no jobs
    if (t.what.exclude) continue;
    const T = t.what.period != null ? t.what.period / 1000 * S.fcpu : t.st.tavg;
    const Cmeas = recentMax(t.hist.cmax) || t.st.cmax;
    if (!T || !Cmeas) continue;                                   // period or C not known yet
    const D = t.cfg.deadline ? t.cfg.deadline / 1000 * S.fcpu : T;
    list.push({
      id: tag, name: t.cfg.name,
      prio: t.what.prio ?? t.cfg.prio,
      T, D,
      C: t.what.c != null ? t.what.c / 1e6 * S.fcpu : Cmeas,
      Rmeas: recentMax(t.hist.rmax) || t.st.rmax, Cavg: t.st.cavg, misses: t.st.miss,
    });
  }
  return list;
}

/* Interrupt load seen in the trace: the ADC DMA handler, period estimated from its entries. */
export function interruptModel() {
  if (!S.isrMaxCyc) return [];
  let first = null, last = null, n = 0;
  for (const [t, id] of S.trace) if (id === 0x41) { if (first === null) first = t; last = t; n++; }
  const T = n > 1 ? (last - first) / (n - 1) : S.fcpu / 1000;
  return [{ name: 'ADC DMA irq', T, C: S.isrMaxCyc }];
}

export function analyse() {
  const list = buildModel();
  const irq = interruptModel();
  const ctx = S.sys ? S.sys.ctx : 0;
  const rta = responseTimes(list, { ctx, interrupts: irq });
  for (const i of list) {
    const r = rta.get(i.id);
    i.R = r.R; i.steps = r.steps; i.ok = r.ok;
    i.margin = isFinite(i.R) && i.D ? 1 - i.R / i.D : -Infinity;
  }
  const U = utilisation(list, irq);
  const n = list.length + irq.length;
  return { list, irq, ctx, U, LL: llBound(n), rm: rmViolations(list) };
}

/* Misses reported during the last N seconds, from the per-task history. */
export function recentMisses(t, seconds = 10) {
  return t.hist.miss.slice(-seconds).reduce((s, x) => s + x, 0);
}

/* Health of each category: 'ok' | 'warn' | 'bad' | 'off' (no data), with a one-line reason. */
export function health(a = analyse()) {
  const h = {};
  const tasks = [...S.tasks.values()].filter(t => t.cfg && t.st);

  // Scheduling: deadlines in the analysis and in the measurements
  const missing = tasks.filter(t => recentMisses(t) > 0);
  const notGuaranteed = a.list.filter(i => i.ok === false);
  const tight = a.list.filter(i => i.ok && i.margin < 0.2);
  h.sched = !a.list.length ? { s: 'off', text: 'waiting for measurements' }
    : missing.length ? { s: 'bad', text: `${missing.map(t => t.cfg.name).join(', ')} missed deadlines (last 10 s)` }
    : notGuaranteed.length ? { s: 'bad', text: `${notGuaranteed.map(i => i.name).join(', ')} not guaranteed by the analysis` }
    : tight.length ? { s: 'warn', text: `${tight.map(i => i.name).join(', ')}: less than 20 % margin` }
    : { s: 'ok', text: `${a.list.length} tasks analysed, all deadlines guaranteed` };

  // CPU
  if (!S.sys) h.cpu = { s: 'off', text: 'waiting' };
  else { const load = 100 - S.sys.idle / 10; h.cpu = { s: load > 90 ? 'bad' : load > 70 ? 'warn' : 'ok', text: `${load.toFixed(1)} % busy, ${S.sys.sw} switches/s` }; }

  // Memory: stacks and heap
  let worst = null;
  for (const t of tasks) {
    const pct = t.st.stack / t.cfg.stack * 100;
    if (!worst || pct < worst.pct) worst = { name: t.cfg.name, pct };
  }
  const heapPct = S.sys && S.sys.heapTotal ? S.sys.heapMin / S.sys.heapTotal * 100 : null;
  const memBad = (worst && worst.pct < 10) || (heapPct !== null && heapPct < 10);
  const memWarn = (worst && worst.pct < 30) || (heapPct !== null && heapPct < 30);
  h.mem = !worst ? { s: 'off', text: 'waiting' }
    : { s: memBad ? 'bad' : memWarn ? 'warn' : 'ok', text: `lowest stack margin ${worst.name} ${worst.pct.toFixed(0)} %` + (heapPct !== null ? `, heap min ${heapPct.toFixed(0)} % free` : '') };

  // Queues
  const qs = [...S.queues.values()];
  const full = qs.filter(q => q.fail > (q.prevFail ?? q.fail) || q.max >= q.len);
  h.queues = !qs.length ? { s: 'off', text: 'no queue registered' }
    : full.length ? { s: 'warn', text: `${full.map(q => q.name).join(', ')} reached its capacity` }
    : { s: 'ok', text: `${qs.length} queue(s), none full` };

  // Interrupts
  if (!S.sys) h.irq = { s: 'off', text: 'waiting' };
  else {
    const lat = S.sys.isr / S.fcpu * 1e6, dur = S.isrMaxCyc / S.fcpu * 1e6;
    h.irq = { s: lat > 100 ? 'bad' : lat > 50 ? 'warn' : 'ok', text: `interrupt-to-task ${lat.toFixed(1)} µs worst, ADC handler ${dur.toFixed(1)} µs` };
  }

  // Observation chain: probe cost, trace losses, link quality
  if (!S.sys) h.probe = { s: 'off', text: 'waiting' };
  else {
    // losses and CRC errors of the last 10 s (counters are cumulative: compare with 10 s ago)
    const L = S.lossHist, now = L[L.length - 1], old = L[Math.max(0, L.length - 11)];
    const lost = now && old ? Math.max(0, now.drop - old.drop) : 0;
    const crc = now && old ? Math.max(0, now.crc - old.crc) : 0;
    const p = S.sys.probe / 10;
    h.probe = { s: p > 5 ? 'bad' : (p > 2 || lost > 0 || crc > 0) ? 'warn' : 'ok',
      text: `probe ${p.toFixed(1)} % CPU; last 10 s: ${lost} trace events lost, ${crc} CRC errors` };
  }
  return h;
}

export function verdict(h) {
  const v = Object.values(h).map(x => x.s);
  return v.includes('bad') ? 'bad' : v.includes('warn') ? 'warn' : v.every(x => x === 'off') ? 'off' : 'ok';
}
