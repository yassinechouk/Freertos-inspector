// Fault lab: inject a deliberate fault on the target and check that the matching detector fires.
// This is how the tool proves it works: every detector has a fault that triggers it.
import { escapeHtml } from './store.js';
import { bus } from './store.js';

const $ = id => document.getElementById(id);

const FAULTS = [
  { key: 'hog', title: 'CPU hog', expect: ['CPU_HOG'],
    what: 'A task at priority 18 runs 150 ms without ever blocking.',
    learn: 'Every lower-priority task (taskB, the link, the health task…) waits: their response times jump and deadlines are missed. In the Timeline, one long bar and everyone below it waiting.' },
  { key: 'stack', title: 'Stack pressure', expect: ['STACK_LOW'],
    what: 'A task calls a function with a 1000-byte local array: about 74 % of its stack is used once.',
    learn: 'The high-water mark remembers it: less than 30 % never used. No real overflow (that would stop the system by design), but the margin is gone. Reported once per target reset.' },
  { key: 'queue', title: 'Queue flood', expect: ['QUEUE_FULL'],
    what: 'A producer pushes 40 items without waiting into an 8-item queue read every 20 ms.',
    learn: 'The queue fills in microseconds: 32 sends fail. In Resources, the fill bar hits the top and the failure count rises.' },
  { key: 'inversion', title: 'Priority inversion', expect: ['PRIO_INHERIT', 'PRIO_RESTORE'],
    what: 'A priority-9 task takes a mutex and works 6 ms; a priority-21 task then needs the same mutex.',
    learn: 'FreeRTOS raises the holder to 21 (priority inheritance) until it releases the mutex: the inversion is bounded, and its duration is reported.' },
  { key: 'heap', title: 'Heap pressure', expect: ['HEAP_LOW'],
    what: 'Blocks of 256 bytes are allocated until about 25 % of the heap is free, kept 3 s, then freed.',
    learn: 'The minimum-ever free heap drops below 30 % and stays there: a later allocation could fail. Reported once per target reset.' },
];

const KEYS = { 1: 'hog', 2: 'stack', 3: 'queue', 4: 'inversion', 5: 'heap' };   // HIL_FAULT_* -> card
const runs = new Map();   // key -> { at, t0 (target ms), seen: Set, doneAt, delay (s, target clock) }
let send = () => {};

export function init(ctx) {
  send = ctx.send;
  $('fl-cards').innerHTML = FAULTS.map(f => `
    <section class="card fault" id="fl-${f.key}">
      <h2>${f.title}</h2>
      <p>${escapeHtml(f.what)}</p>
      <p class="note">${escapeHtml(f.learn)}</p>
      <p class="expect">Expected detection: ${f.expect.map(e => `<code>${e}</code>`).join(' + ')}</p>
      <button data-fault="${f.key}">Inject</button>
      <div class="result"></div>
    </section>`).join('');
  $('fl-cards').addEventListener('click', e => {
    const k = e.target.dataset.fault;
    if (!k) return;
    runs.set(k, { at: Date.now(), seen: new Set() });
    send({ cmd: 'fault', value: k });
    render();
  });
  bus.addEventListener('alert', ev => {
    const a = ev.detail;
    if (!a) return;
    // The target reports every injection (from this page, another browser, the console, or a replayed
    // session): follow it, and measure the detection delay on the target clock when it is known.
    if (a.code === 'FAULT_INJECTED') {
      const key = KEYS[a.task];
      if (key) {
        const r = runs.get(key);
        if (r && !r.t0 && Date.now() - r.at < 3000) r.t0 = a.tMs;
        else runs.set(key, { at: Date.now(), t0: a.tMs, seen: new Set() });
      }
    }
    for (const [k, r] of runs) {
      if (Date.now() - r.at >= 8000 || r.doneAt) continue;
      r.seen.add(a.code);
      const f = FAULTS.find(x => x.key === k);
      if (f.expect.every(e => r.seen.has(e))) {
        r.doneAt = Date.now();                                       // time of the detection, not of the display
        if (r.t0 != null && a.tMs != null) r.delay = (a.tMs - r.t0) / 1000;
      }
    }
    render();
  });
  // Did the target receive the command? (ACK from the target, or "no ACK" from the gateway)
  bus.addEventListener('note', ev => {
    const m = ev.detail, last = [...runs.entries()].sort((x, y) => y[1].at - x[1].at)[0];
    if (!last || Date.now() - last[1].at > 2000) return;
    if (m.type === 'ack' && m.cmd === 'fault') last[1].ack = m.status;
    else if (m.type === 'info' && /no ACK/.test(m.text)) last[1].ack = 'none';
    render();
  });
}

export function render() {
  for (const f of FAULTS) {
    const r = runs.get(f.key), box = document.querySelector(`#fl-${f.key} .result`);
    if (!box) continue;
    if (!r) { box.innerHTML = ''; continue; }
    const age = (Date.now() - r.at) / 1000;
    const got = f.expect.filter(e => r.seen.has(e));
    const all = got.length === f.expect.length;
    if (r.ack === 'none' || (r.ack && r.ack !== 'ok')) {
      box.className = 'result bad';
      box.innerHTML = r.ack === 'none' ? 'The target did not acknowledge the command: it never received it (link from the supervisor to the target).'
        : `The target refused the command (${escapeHtml(r.ack)}).`;
      continue;
    }
    box.className = 'result ' + (all ? 'ok' : age > 8 ? 'bad' : 'wait');
    const delay = r.delay ?? ((r.doneAt ?? Date.now()) - r.at) / 1000;
    box.innerHTML = all ? `✓ Detected: ${got.join(' + ')}, ${delay.toFixed(2)} s after injection${r.delay != null ? ' (target clock)' : ''}`
      : age > 8 ? `Not detected within 8 s${got.length ? ` (only ${got.join(', ')})` : ''}`
      : `Waiting for the detectors… ${got.length ? 'seen: ' + got.join(', ') : ''}`;
  }
}
