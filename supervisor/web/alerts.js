// Alerts: every detector event from the target becomes a readable alert (severity, title,
// explanation, what to do). Findings computed from the measurements are added once when they
// first appear. The log is shared by the Alerts view, the Overview and the Fault lab.
// A condition that repeats (the same detector, the same task) updates one entry with a count
// instead of adding a line per second.
import { S, bus, taskName, queueName } from './store.js';

export const alerts = [];        // newest last: { id, key, at, first, count, sev, code, title, why, task, ev }
let nextId = 1;
const seenFindings = new Set();
const REPEAT_MS = 10000;         // same key again within 10 s: same entry

function add(a) {
  a.key = a.key ?? `${a.code}:${a.task ?? ''}:${['GATEWAY', 'EVENT', 'MODE', 'ACK'].includes(a.code) ? a.title : ''}`;
  a.at = a.at ?? Date.now();
  const i = alerts.findIndex(x => x.key === a.key);
  if (i >= 0 && a.at - alerts[i].at <= REPEAT_MS) {
    const old = alerts.splice(i, 1)[0];
    a.id = old.id; a.first = old.first; a.count = old.count + 1; a.total = (old.total ?? 0) + (a.n ?? 0);
  } else {
    a.id = nextId++; a.first = a.at; a.count = 1; a.total = a.n ?? 0;
  }
  alerts.push(a);
  if (alerts.length > 500) alerts.shift();
  bus.dispatchEvent(new CustomEvent('alert', { detail: a }));
}

const FAULTS = { 1: 'CPU hog', 2: 'stack pressure', 3: 'queue flood', 4: 'priority inversion', 5: 'heap pressure' };
const MODES = ['IDLE', 'LOCAL', 'REMOTE', 'TEST'];

/* Event code -> alert (see protocol/hil_proto.h, HIL_EVT_*). Names are resolved when shown
   (titleOf), because an event can arrive before the task table. */
export function fromEvent(e) {
  const a = e.arg >>> 0, tag = a & 0xff, hi = a >>> 8;
  const t = taskName(tag);
  switch (e.code) {
    case 7: return { sev: 'bad', code: 'DEADLINE_MISS', task: tag, n: hi, title: `${t}: ${hi} deadline miss${hi > 1 ? 'es' : ''} in the last second`,
      why: 'A job ended after its deadline (by default the measured period). Open the Timeline: a higher-priority task or an interrupt ran too long, or the job itself grew. The Analysis view tells whether this is expected from the priorities.' };
    case 8: return { sev: 'warn', code: 'PRIO_INHERIT', task: tag, title: `${t} inherited priority ${hi & 0xff} (blocking ${taskName(hi >>> 8)})`,
      why: `${taskName(hi >>> 8)} waits for a mutex held by ${t}. The kernel raises ${t} so that medium-priority tasks cannot stretch the wait: the inversion is bounded, but it still delays the waiting task.` };
    case 9: { const us = hi; return { sev: us > 1000 ? 'warn' : 'info', code: 'PRIO_RESTORE', task: tag,
      title: `${t} back to its base priority after ${us >= 1000 ? (us / 1000).toFixed(2) + ' ms' : us + ' µs'}`,
      why: 'Time during which a higher-priority task was blocked by this mutex holder. Keep critical sections short.' }; }
    case 10: return { sev: 'warn', code: 'QUEUE_FULL', task: 'q' + tag, n: hi, title: `${queueName(tag)}: ${hi} send${hi > 1 ? 's' : ''} failed, queue full`,
      why: 'The producer is faster than the consumer, or the consumer is blocked. Data was dropped (or senders were blocked). Enlarge the queue, speed up the consumer, or pace the producer.' };
    case 11: return { sev: 'info', code: 'FAULT_INJECTED', task: a, title: `Fault injected: ${FAULTS[a] || a}`, why: 'Deliberate fault from the Fault lab: the matching detector should report it within a few seconds.' };
    case 12: { const tt = S.tasks.get(tag); const pct = tt && tt.cfg ? Math.round(hi / tt.cfg.stack * 100) : '?';
      return { sev: 'warn', code: 'STACK_LOW', task: tag, title: `${t}: stack nearly full (${hi} words never used, ${pct} %)`,
        why: 'Less than 30 % of the stack has never been used. A deeper call chain or a bigger local variable could overflow it, which corrupts memory. Increase the stack or reduce the locals.' }; }
    case 13: return { sev: 'bad', code: 'CPU_HOG', task: tag, title: `${t}: one job ran ${hi} ms without blocking`,
      why: 'While it runs, every lower-priority task waits. Split the work, block on an event, or lower its priority.' };
    case 14: return { sev: 'bad', code: 'STARVED', task: tag, title: `${t} was ready for a whole second without running`,
      why: 'Higher-priority tasks used all the CPU. Look at the CPU share per task and at the priorities.' };
    case 15: return { sev: 'warn', code: 'HEAP_LOW', title: `Heap: minimum free ${a} bytes (${S.sys ? Math.round(a / S.sys.heapTotal * 100) : '?'} %)`,
      why: 'The free heap went below 30 % at least once. An allocation may fail later (malloc-failed hook). Check for leaks, or allocate statically.' };
    case 1: return { sev: 'bad', code: 'STALL', title: 'Motor stall protection: output forced off', why: 'High duty without motion for 0.5 s (blocked shaft or lost encoder). Latched until reset.' };
    case 2: return { sev: 'warn', code: 'LINK_LOST', title: 'Target: no frame from the supervisor for 1 s', why: 'The target no longer hears the supervisor: commands cannot reach it. The target restarts its receiver by itself; check the wire from the supervisor TX to the target RX.' };
    case 3: return { sev: 'info', code: 'LINK_RESTORED', title: 'Target: supervisor link restored', why: '' };
    case 4: return { sev: 'info', code: 'MODE', title: `Mode changed to ${MODES[a] || a}`, why: '' };
    case 5: return { sev: 'warn', code: 'CMD_REJECTED', title: 'Command rejected by the target', why: 'The target refused a command (format or range).' };
    case 6: return { sev: 'warn', code: 'SP_TIMEOUT', title: 'Remote setpoint expired: motor back to 0', why: 'No setpoint refresh for 1 s (page closed or Wi-Fi lost): the dead-man worked.' };
    default: return { sev: 'info', code: 'EVENT', title: `Event ${e.code} (arg ${e.arg})`, why: '' };
  }
}

/* Title with the current task names (the table may have arrived after the event). */
export function titleOf(a) { return a.ev ? fromEvent(a.ev).title : a.title; }

/* "x52 since 18:08:40, 2080 in total" for a repeated entry, '' otherwise. */
export function repeatOf(a) {
  if (!a.count || a.count < 2) return '';
  return `×${a.count} since ${new Date(a.first).toLocaleTimeString()}${a.total ? `, ${a.total} in total` : ''}`;
}

/* Findings from the measurements, logged once (until the page is reloaded). */
export function finding(key, a) {
  if (seenFindings.has(key)) return;
  seenFindings.add(key);
  add({ key: 'finding:' + key, ...a });
}

export function clearAlerts() { alerts.length = 0; seenFindings.clear(); bus.dispatchEvent(new CustomEvent('alert')); }

bus.addEventListener('event', ev => add({ ...fromEvent(ev.detail), tMs: ev.detail.t, ev: ev.detail }));
bus.addEventListener('note', ev => {
  const m = ev.detail;
  if (m.type === 'info') add({ sev: /LOST|no ACK/.test(m.text) ? 'warn' : 'info', code: 'GATEWAY', title: m.text,
    why: /no ACK/.test(m.text) ? 'The supervisor sent the command but the target did not answer: the link from the supervisor to the target is not working (wire, or target receiver stopped).' : '' });
  else if (m.type === 'ack' && m.status !== 'ok') add({ sev: 'warn', code: 'ACK', title: `Command ${m.cmd} ${m.status} (${m.value})`, why: '' });
});
