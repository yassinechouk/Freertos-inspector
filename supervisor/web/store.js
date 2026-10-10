// Central state of the dashboard: everything received (live or replayed) goes through ingest().
// Views read S and listen to the 'update', 'event' and 'trace' notifications.

export const S = {
  fcpu: 80e6,
  tasks: new Map(),      // tag -> { cfg, st, prevMiss, hist: { cpu: [], miss: [] }, what: {} }
  queues: new Map(),     // qid -> { cfg+stats, prevFail }
  sys: null,             // last RTOS_SYS
  gw: null,              // last gateway (supervisor) status
  tel: [],               // last telemetry batch [[t, sp, rpm, duty]...]
  motor: { mode: '?', fault: 0 },
  events: [],            // raw events from the target, newest last
  trace: [],             // [cycles (unwrapped), id]
  traceDropStm: 0, traceDropGw: 0,
  lossHist: [],          // once per second: cumulative trace losses and CRC errors (for "last 10 s")
  isrMaxCyc: 0,
  connected: false,      // browser <-> supervisor
  source: 'live',        // 'live' | 'replay'
};

export const bus = new EventTarget();
const emit = (name, detail) => bus.dispatchEvent(new CustomEvent(name, { detail }));

export function reset() {
  S.tasks.clear(); S.queues.clear();
  S.sys = null; S.gw = null; S.tel = []; S.events = []; S.trace = [];
  S.traceDropStm = 0; S.traceDropGw = 0; S.isrMaxCyc = 0; S.lossHist = [];
  lastRaw = null; wraps = 0;
  emit('update');
}

function task(tag) {
  let t = S.tasks.get(tag);
  if (!t) { t = { cfg: null, st: null, prevMiss: null, hist: { cpu: [], miss: [], cmax: [], rmax: [] }, what: {} }; S.tasks.set(tag, t); }
  return t;
}

const HIST = 120;   // seconds of per-task history kept
const push = (a, v) => { a.push(v); if (a.length > HIST) a.shift(); };

/* One JSON message (object) from the supervisor. */
export function ingest(m) {
  switch (m.type) {
    case 'task':
      task(m.tag).cfg = m;
      if (m.fcpu) S.fcpu = m.fcpu;
      break;
    case 'tstat': {
      const t = task(m.tag);
      const newMiss = t.prevMiss === null ? 0 : Math.max(0, m.miss - t.prevMiss);
      t.prevMiss = m.miss;
      t.st = m;
      push(t.hist.cpu, m.cpu / 10);
      push(t.hist.miss, newMiss);
      push(t.hist.cmax, m.cmaxl ?? m.cmax);       // older sessions: only the since-reset maximum
      push(t.hist.rmax, m.rmaxl ?? m.rmax);
      break;
    }
    case 'qstat': {
      const q = S.queues.get(m.qid) || {};
      S.queues.set(m.qid, { ...q, ...m, prevFail: q.fail ?? m.fail });
      break;
    }
    case 'rsys':
      S.sys = m;
      if (m.fcpu) S.fcpu = m.fcpu;
      push(S.lossHist, { t: Date.now(), drop: m.drop + S.traceDropGw, crc: S.gw ? S.gw.crc : 0 });
      break;
    case 'sys':
      S.gw = m;
      break;
    case 'tel':
      if (m.s && m.s.length) { S.tel = m.s; S.motor = { mode: m.mode, fault: m.fault }; }
      emit('telemetry', m);
      return;
    case 'evt':
      S.events.push(m);
      if (S.events.length > 2000) S.events.shift();
      emit('event', m);
      return;
    case 'ack':
    case 'info':
      emit('note', m);
      return;
    default:
      return;
  }
  emit('update');
}

/* Binary trace batch: 'T', u16 dropped (target), u16 dropped (gateway), N x { u32 cycles, u8 id }. */
let lastRaw = null, wraps = 0;
export function ingestTrace(buf) {
  const v = new DataView(buf);
  if (v.byteLength < 5 || v.getUint8(0) !== 0x54) return;
  S.traceDropStm = v.getUint16(1, true);
  S.traceDropGw = v.getUint16(3, true);
  for (let o = 5; o + 5 <= buf.byteLength; o += 5) {
    const raw = v.getUint32(o, true), id = v.getUint8(o + 4);
    if (lastRaw !== null && raw < lastRaw && lastRaw - raw > 0x80000000) wraps++;   // 32-bit counter wrapped
    lastRaw = raw;
    S.trace.push([wraps * 4294967296 + raw, id]);
  }
  const keep = S.fcpu * 0.5;                        // last 500 ms
  const last = S.trace.length ? S.trace[S.trace.length - 1][0] : 0;
  let cut = 0;
  while (cut < S.trace.length && S.trace[cut][0] < last - keep) cut++;
  if (cut) S.trace.splice(0, cut);
  measureIsr();
  emit('trace');
}

/* Longest ADC DMA interrupt in the buffer (slow decay keeps the recent worst case). */
function measureIsr() {
  let start = null, max = 0;
  for (const [t, id] of S.trace) {
    if (id === 0x41) start = t;
    else if (id === 0x81 && start !== null) { max = Math.max(max, t - start); start = null; }
  }
  if (max) S.isrMaxCyc = Math.max(S.isrMaxCyc * 0.98, max);
}

/* ---- Helpers shared by the views ------------------------------------------------------ */
export const us = cyc => cyc / S.fcpu * 1e6;
export function fmtTime(cyc) {
  if (cyc == null || !isFinite(cyc)) return cyc === Infinity ? '∞' : '–';
  const u = us(cyc);
  return u < 1000 ? u.toFixed(u < 10 ? 2 : 1) + ' µs' : (u / 1000).toFixed(u < 1e5 ? 2 : 0) + ' ms';
}
export const STATES = ['running', 'ready', 'blocked', 'suspended', 'deleted'];
export function taskName(tag) { const t = S.tasks.get(tag); return t && t.cfg ? t.cfg.name : `task ${tag}`; }
export function queueName(qid) { const q = S.queues.get(qid); return q ? q.name : `queue ${qid}`; }
export function color(tag) {
  const t = S.tasks.get(tag);
  if (t && t.cfg && (t.cfg.flags & 1)) return '#8a94a6';      // idle
  return `hsl(${(tag * 67) % 360} 62% 52%)`;
}
export function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}
/* Tasks sorted by current priority, highest first (only those whose configuration is known). */
export function sortedTasks() {
  return [...S.tasks.entries()].filter(([, t]) => t.cfg)
    .sort((a, b) => (b[1].st?.prio ?? b[1].cfg.prio) - (a[1].st?.prio ?? a[1].cfg.prio) || a[0] - b[0]);
}
