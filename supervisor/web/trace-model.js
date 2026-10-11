// Replays the context-switch trace into segments (who ran, who waited ready, interrupts) and
// per-task activity over a window. Shared by the Timeline and the Inspector.
// Trace ids (protocol/hil_proto.h): 0x00..0x3F task switched in, 0xC0|tag released,
// 0x40|isr interrupt entry, 0x80|isr interrupt exit, 0xFF tick.

/* events: [[cycles, id]...], oldest first. Returns segments that end after tStart:
   { key: tag | 'isr', a, b, kind: 'run' | 'ready' }, releases [{ tag, t }], ticks [t],
   per-task activity { run, wait, delayMax, delaySum, starts, releases } (cycles, inside the window),
   and the task on the CPU at the last event. */
export function replay(events, tStart) {
  const segs = [], releases = [], ticks = [];
  const act = new Map();
  const A = tag => { let x = act.get(tag); if (!x) { x = { run: 0, wait: 0, delayMax: 0, delaySum: 0, starts: 0, releases: 0 }; act.set(tag, x); } return x; };
  const clip = (a, b) => Math.max(0, b - Math.max(a, tStart));
  let cur = null, curStart = null, isrStart = null;
  const readySince = new Map(), lastOut = new Map();
  for (const [t, id] of events) {
    if (id <= 0x3f) {                                        // switched in
      if (cur !== null) {
        if (t > tStart) { segs.push({ key: cur, a: curStart, b: t, kind: 'run' }); A(cur).run += clip(curStart, t); }
        lastOut.set(cur, t);
      }
      const rel = readySince.get(id), pre = lastOut.get(id);
      const since = rel !== undefined ? rel : pre;            // released, or preempted earlier
      if (since !== undefined && since < t && t > tStart) {
        segs.push({ key: id, a: since, b: t, kind: 'ready' });
        A(id).wait += clip(since, t);
      }
      if (rel !== undefined && rel >= tStart && rel <= t) {    // start delay: release -> first run (0 counts)
        const d = t - rel, x = A(id);
        x.delayMax = Math.max(x.delayMax, d); x.delaySum += d; x.starts++;
      }
      readySince.delete(id);
      cur = id; curStart = t;
    } else if (id === 0xff) {
      if (t >= tStart) ticks.push(t);
    } else if ((id & 0xc0) === 0xc0) {                        // released
      readySince.set(id & 0x3f, t);
      lastOut.delete(id & 0x3f);
      if (t >= tStart) { releases.push({ tag: id & 0x3f, t }); A(id & 0x3f).releases++; }
    } else if ((id & 0xc0) === 0x40) { isrStart = t; }
    else if ((id & 0xc0) === 0x80 && isrStart !== null) {
      if (t > tStart) segs.push({ key: 'isr', a: isrStart, b: t, kind: 'run' });
      isrStart = null;
    }
  }
  const tEnd = events.length ? events[events.length - 1][0] : 0;
  if (cur !== null && tEnd > tStart) { segs.push({ key: cur, a: curStart, b: tEnd, kind: 'run' }); A(cur).run += clip(curStart, tEnd); }
  return { segs, releases, ticks, act, running: cur, tEnd };
}

/* Activity of the last `ms` milliseconds of the trace in memory (null if there is no trace yet). */
export function recentActivity(trace, fcpu, ms) {
  if (trace.length < 2) return null;
  const tEnd = trace[trace.length - 1][0];
  const span = ms / 1000 * fcpu;
  const tStart = Math.max(trace[0][0], tEnd - span);
  return { ...replay(trace, tStart), tStart, span: tEnd - tStart };
}
