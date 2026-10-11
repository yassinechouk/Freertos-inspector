// Analysis: response-time analysis from the measurements, "what if" editing, and the steps of
// the iteration for the selected task, so the result can be checked by hand.
import { S, fmtTime, escapeHtml, color } from './store.js';
import { analyse } from './model.js';
import { finding } from './alerts.js';

const $ = id => document.getElementById(id);
let selected = null;

export function init() {
  $('an-body').addEventListener('change', e => {
    const el = e.target, tag = +el.dataset.tag;
    const t = S.tasks.get(tag);
    if (!t) return;
    if (el.dataset.f === 'exclude') t.what.exclude = !el.checked;
    else t.what[el.dataset.f] = el.value === '' ? undefined : +el.value;
    render(true);
  });
  $('an-body').addEventListener('click', e => {
    const tr = e.target.closest('tr[data-tag]');
    if (tr && e.target.tagName !== 'INPUT') { selected = +tr.dataset.tag; render(true); }
  });
  $('an-reset').onclick = () => { for (const t of S.tasks.values()) t.what = {}; render(true); };
}

/* Computed R (solid) and measured R (tick) against the deadline (line at 100 %). */
function vbar(m) {
  const scale = Math.max(m.D, isFinite(m.R) ? m.R : m.D * 1.3, m.Rmeas || 0) * 1.1;
  const p = v => Math.min(100, v / scale * 100).toFixed(1);
  return `<div class="rbar" title="computed R ${fmtTime(m.R)} · measured R ${fmtTime(m.Rmeas)} · deadline ${fmtTime(m.D)}">
    <span class="r" style="width:${isFinite(m.R) ? p(m.R) : 100}%;background:${color(m.id)}"></span>
    ${m.Rmeas ? `<i class="rc" style="left:${p(m.Rmeas)}%"></i>` : ''}<i class="d" style="left:${p(m.D)}%"></i></div>`;
}

export function render(force) {
  const editing = $('an-body').contains(document.activeElement);
  const a = analyse();

  // Findings logged once in the Alerts view.
  for (const [x, y] of a.rm) {
    finding(`rm-${x.id}-${y.id}`, { sev: 'info', code: 'RM_ORDER', title: `${x.name} (period ${fmtTime(x.T)}) has a lower priority than ${y.name} (period ${fmtTime(y.T)})`,
      why: 'Not rate-monotonic: the task with the shorter period should normally have the higher priority. Acceptable if intended, but it costs margin to the shorter-period task.' });
  }
  for (const i of a.list) if (i.D && i.ok === false) {
    finding(`rta-${i.id}`, { sev: 'bad', code: 'NOT_SCHEDULABLE', task: i.id, title: `${i.name}: worst-case response ${fmtTime(i.R)} exceeds its deadline ${fmtTime(i.D)}`,
      why: 'By calculation, this task can miss its deadline when the higher-priority tasks are released together. Lower their C, raise this task’s priority, or relax its deadline.' });
  }
  for (const i of a.list) if (i.Rmeas && isFinite(i.R) && i.Rmeas > i.R * 1.05) {
    finding(`model-${i.id}`, { sev: 'warn', code: 'MODEL_GAP', task: i.id, title: `${i.name}: measured response ${fmtTime(i.Rmeas)} above the computed ${fmtTime(i.R)}`,
      why: 'Something delays this task that the model does not contain: blocking on a resource, an interrupt not traced, or a task without a measured period.' });
  }

  $('an-sum').innerHTML = `
    <div><small>Utilisation U = Σ C/T</small><span>${(a.U * 100).toFixed(1)} %</span></div>
    <div><small>Liu-Layland bound (n = ${a.list.length + a.irq.length})</small><span>${(a.LL * 100).toFixed(1)} %</span></div>
    <div><small>Context switch used</small><span>${fmtTime(a.ctx)}</span></div>
    <div><small>Interrupts in the model</small><span>${a.irq.map(q => `${q.name}: ${fmtTime(q.C)} every ${fmtTime(q.T)}`).join(', ') || 'none'}</span></div>
    <p class="note">${a.U <= a.LL ? 'U is below the bound: rate-monotonic scheduling is guaranteed for these tasks (sufficient test).'
      : a.U <= 1 ? 'U is above the bound: the bound says nothing, the response-time analysis below decides.'
      : 'U is above 100 %: the CPU cannot do all this work, some task must miss deadlines.'}</p>`;

  if (!editing || force) {
    const rows = [...S.tasks.entries()].filter(([, t]) => t.cfg && !(t.cfg.flags & 1) && t.st)
      .sort((x, y) => (y[1].what.prio ?? y[1].cfg.prio) - (x[1].what.prio ?? x[1].cfg.prio));
    const byTag = new Map(a.list.map(i => [i.id, i]));
    $('an-body').innerHTML = rows.map(([tag, t]) => {
      const m = byTag.get(tag), c = t.cfg, s = t.st;
      const inp = (f, v, title) => `<input type="number" data-tag="${tag}" data-f="${f}" value="${v ?? ''}" title="${title}">`;
      const cls = !m ? 'muted' : m.ok === false ? 'bad' : m.margin !== null && m.margin < 0.2 ? 'warn' : m.D ? 'ok' : '';
      return `<tr data-tag="${tag}" class="${cls} ${selected === tag ? 'sel' : ''}">
        <td><input type="checkbox" data-tag="${tag}" data-f="exclude" ${t.what.exclude ? '' : 'checked'} title="include this task in the analysis"></td>
        <td><i class="sw" style="background:${color(tag)}"></i>${escapeHtml(c.name)}</td>
        <td>${inp('prio', t.what.prio ?? c.prio, 'priority (what if)')}</td>
        <td>${inp('period', t.what.period ?? (s.tavg ? +(s.tavg / S.fcpu * 1000).toFixed(3) : ''), 'period in ms (what if)')}</td>
        <td>${m && m.D ? fmtTime(m.D) : m ? '<small>none</small>' : '–'}</td>
        <td>${inp('c', t.what.c ?? (s.cmax ? +(s.cmax / S.fcpu * 1e6).toFixed(1) : ''), 'C in µs used by the analysis (what if)')}</td>
        <td>${m ? fmtTime(m.R) : '–'}</td>
        <td>${s.rmax ? fmtTime(s.rmax) : '–'}</td>
        <td>${m && m.margin !== null && isFinite(m.margin) ? (m.margin * 100).toFixed(0) + ' %' : m ? '–' : ''}</td>
        <td class="vbar">${m && m.D ? vbar(m) : ''}</td>
        <td>${!m ? '<small>no period yet</small>' : !m.D ? '<small>no deadline</small>' : m.ok === false ? '<b class="bad-text">✕ miss possible</b>' : '<span class="ok-text">✓ guaranteed</span>'}</td></tr>`;
    }).join('');
  }

  const m = a.list.find(i => i.id === selected);
  $('an-steps').innerHTML = !m ? '<p class="note">Click a task to see how its worst-case response time is computed.</p>' : `
    <h3>${escapeHtml(m.name)}: R = C + Σ ⌈R/Tj⌉ (Cj + ctx) + interrupts</h3>
    <p class="note">Higher or equal priority: ${a.list.filter(j => j !== m && j.prio >= m.prio).map(j => `${escapeHtml(j.name)} (C ${fmtTime(j.C)}, T ${fmtTime(j.T)})`).join(', ') || 'none'}.</p>
    <ol class="steps">${m.steps.slice(0, 40).map((r, k) => `<li>R<sub>${k}</sub> = ${fmtTime(r)}</li>`).join('')}</ol>
    <p>${!isFinite(m.R) ? 'No stable value: higher-priority work never leaves enough CPU.'
      : `Stable at <b>${fmtTime(m.R)}</b>` + (m.D ? `: ${m.R <= m.D ? 'within' : 'beyond'} the deadline ${fmtTime(m.D)}.` : ' (event-driven task: no deadline to check).')}</p>`;
}
