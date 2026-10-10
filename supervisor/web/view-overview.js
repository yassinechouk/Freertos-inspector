// Overview: the answer to "is everything fine?" in one screen, then where to look.
import { S, sortedTasks, color, escapeHtml } from './store.js';
import { analyse, health, verdict } from './model.js';
import { alerts, titleOf } from './alerts.js';

const $ = id => document.getElementById(id);
const CATS = [
  ['sched', 'Scheduling', 'deadlines guaranteed by the analysis and met in the measurements'],
  ['cpu', 'CPU', 'load and context switches'],
  ['mem', 'Memory', 'stack margins and heap'],
  ['queues', 'Queues', 'fill level and overflows of registered queues'],
  ['irq', 'Interrupts', 'handler duration and latency to the woken task'],
  ['probe', 'Observation', 'cost of the probe, trace losses, link errors'],
];
const WORD = { ok: 'Healthy', warn: 'Attention', bad: 'Problem', off: 'Waiting for data' };

export function render() {
  const a = analyse();
  const h = health(a);
  const v = verdict(h);
  const vEl = $('ov-verdict');
  vEl.className = 'verdict-big ' + v;
  vEl.querySelector('b').textContent = WORD[v];
  vEl.querySelector('span').textContent = v === 'off' ? 'Connect the target, or load a recorded session.'
    : v === 'ok' ? 'Every task meets its deadline, with margin. Nothing to report.'
    : 'Open the category in orange or red below, then the Alerts view for the explanation.';

  $('ov-lights').innerHTML = CATS.map(([k, name, desc]) => `
    <div class="light ${h[k].s}" title="${escapeHtml(desc)}">
      <i></i><div><b>${name}</b><span>${escapeHtml(h[k].text)}</span></div>
    </div>`).join('');

  const s = S.sys;
  const kpi = (label, val, hint) => `<div title="${escapeHtml(hint)}"><small>${label}</small><span>${val}</span></div>`;
  $('ov-kpis').innerHTML = [
    kpi('CPU load', s ? (100 - s.idle / 10).toFixed(1) + ' %' : '–', 'Share of time not spent in the idle task, last second'),
    kpi('Tasks', [...S.tasks.values()].filter(t => t.cfg).length || '–', 'Discovered automatically when FreeRTOS created them'),
    kpi('Switches / s', s ? s.sw : '–', 'Context switches in the last second'),
    kpi('Kernel', s ? (s.kernel / 10).toFixed(1) + ' %' : '–', 'Time spent switching between tasks (scheduler)'),
    kpi('Probe cost', s ? (s.probe / 10).toFixed(2) + ' %' : '–', 'CPU spent in the RTOS Doctor hooks: the price of observing'),
    kpi('Heap free', s ? `${s.heapFree} / ${s.heapTotal} B` : '–', 'Free now / total. Minimum ever: ' + (s ? s.heapMin + ' B' : '–')),
    kpi('Utilisation (analysis)', a.list.length ? (a.U * 100).toFixed(1) + ' %' : '–', `Sum of C/T. Liu-Layland bound for ${a.list.length + a.irq.length} items: ${(a.LL * 100).toFixed(1)} %`),
  ].join('');

  const rows = sortedTasks().filter(([, t]) => t.st);
  const max = Math.max(1, ...rows.map(([, t]) => t.st.cpu));
  $('ov-cpu').innerHTML = rows.sort((x, y) => y[1].st.cpu - x[1].st.cpu).map(([tag, t]) => `
    <div class="cpurow"><span>${escapeHtml(t.cfg.name)}</span>
      <div class="bar"><i style="width:${(t.st.cpu / max * 100).toFixed(1)}%;background:${color(tag)}"></i></div>
      <b>${(t.st.cpu / 10).toFixed(1)} %</b></div>`).join('') || '<p class="note">No task statistics yet.</p>';

  const recent = alerts.filter(x => x.sev !== 'info').slice(-6).reverse();
  $('ov-alerts').innerHTML = recent.length ? recent.map(x => `
    <li class="${x.sev}"><b>${escapeHtml(titleOf(x))}</b><small>${new Date(x.at).toLocaleTimeString()}${x.count > 1 ? ' · ×' + x.count : ''}</small></li>`).join('')
    : '<li class="ok"><b>No warning so far.</b></li>';
}
