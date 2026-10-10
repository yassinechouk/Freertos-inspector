// Tasks: what is configured and how each task behaves. Click a row for its detail card.
import { S, sortedTasks, color, fmtTime, escapeHtml, STATES } from './store.js';
import { analyse, recentMisses } from './model.js';

const $ = id => document.getElementById(id);
let selected = null;

export function init() {
  $('tk-body').addEventListener('click', e => {
    const tr = e.target.closest('tr[data-tag]');
    if (!tr) return;
    selected = +tr.dataset.tag;
    render();
  });
}

function stackCell(t) {
  if (!t.st) return '–';
  const usedPct = 100 - t.st.stack / t.cfg.stack * 100;
  const cls = usedPct > 90 ? 'bad' : usedPct > 70 ? 'warn' : 'ok';
  return `<div class="bar small ${cls}" title="${t.st.stack} of ${t.cfg.stack} words never used"><i style="width:${usedPct.toFixed(0)}%"></i></div>
          <small>${usedPct.toFixed(0)} % used</small>`;
}

export function render() {
  const a = analyse();
  const byTag = new Map(a.list.map(i => [i.id, i]));
  const rows = sortedTasks();
  $('tk-body').innerHTML = rows.map(([tag, t]) => {
    const c = t.cfg, s = t.st || {}, m = byTag.get(tag);
    const idle = c.flags & 1;
    const boosted = s.prio != null && s.prio !== c.prio;
    const miss10 = recentMisses(t);
    const cls = miss10 ? 'bad' : m && m.ok === false ? 'bad' : m && m.margin < 0.2 ? 'warn' : '';
    const jitter = s.tavg ? Math.max(s.tmax - s.tavg, s.tavg - s.tmin) : 0;
    return `<tr data-tag="${tag}" class="${cls} ${selected === tag ? 'sel' : ''}">
      <td><i class="sw" style="background:${color(tag)}"></i>${escapeHtml(c.name)}</td>
      <td><span class="badge st-${s.state ?? 'x'}">${STATES[s.state] ?? '–'}</span></td>
      <td>${c.prio}${boosted ? ` <b class="boost" title="priority inherited from a mutex waiter">→ ${s.prio}</b>` : ''}</td>
      <td>${idle ? '–' : s.tavg ? fmtTime(s.tavg) + `<br><small>±${fmtTime(jitter)}</small>` : '<small>not periodic yet</small>'}</td>
      <td>${idle ? '–' : c.deadline ? c.deadline + ' ms' : '<small>= period</small>'}</td>
      <td>${idle ? '–' : s.cmax ? fmtTime(s.cavg) + `<br><small>max ${fmtTime(s.cmax)}</small>` : '–'}</td>
      <td>${idle ? '–' : s.rmax ? fmtTime(s.rmin) + ' … ' + fmtTime(s.rmax) : '–'}</td>
      <td>${idle ? '–' : (s.miss ?? '–')}${miss10 ? ` <b class="bad-text">+${miss10}</b>` : ''}</td>
      <td>${s.cpu != null ? (s.cpu / 10).toFixed(1) + ' %' : '–'}</td>
      <td>${stackCell(t)}</td></tr>`;
  }).join('');
  renderDetail(byTag);
}

function sparkline(canvas, values, colorCss, maxHint) {
  const ctx = canvas.getContext('2d'), W = canvas.clientWidth, H = canvas.clientHeight, dpr = Math.min(devicePixelRatio || 1, 2);
  canvas.width = W * dpr; canvas.height = H * dpr; ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, W, H);
  if (!values.length) return;
  const max = Math.max(maxHint || 0, ...values, 1e-9);
  ctx.strokeStyle = colorCss; ctx.lineWidth = 1.5; ctx.beginPath();
  values.forEach((v, i) => { const x = i / Math.max(values.length - 1, 1) * (W - 2) + 1, y = H - 2 - v / max * (H - 4); i ? ctx.lineTo(x, y) : ctx.moveTo(x, y); });
  ctx.stroke();
}

function renderDetail(byTag) {
  const box = $('tk-detail');
  const t = S.tasks.get(selected);
  if (!t || !t.cfg) { box.innerHTML = '<p class="note">Click a task to see its card.</p>'; return; }
  const c = t.cfg, s = t.st || {}, m = byTag.get(selected);
  const line = (k, v, hint = '') => `<dt title="${escapeHtml(hint)}">${k}</dt><dd>${v}</dd>`;
  box.innerHTML = `
    <h3><i class="sw" style="background:${color(selected)}"></i>${escapeHtml(c.name)}</h3>
    <dl class="kv">
      ${line('State', STATES[s.state] ?? '–')}
      ${line('Priority', c.prio + (s.prio != null && s.prio !== c.prio ? ` (now ${s.prio}, inherited)` : ''), 'Base priority; the current one can be higher while it holds a mutex wanted by a higher-priority task')}
      ${line('Stack', `${c.stack} words, ${s.stack ?? '–'} never used`, 'High-water mark: the free space that was never touched since reset')}
      ${line('Release interval', s.tavg ? `${fmtTime(s.tavg)} (min ${fmtTime(s.tmin)}, max ${fmtTime(s.tmax)})` : '–', 'Time between two releases (task becoming ready). Its spread is the release jitter')}
      ${line('Deadline', c.deadline ? c.deadline + ' ms (declared)' : 'implicit: the measured period')}
      ${line('Execution time C', s.cmax ? `avg ${fmtTime(s.cavg)}, max ${fmtTime(s.cmax)}` : '–', 'CPU time actually used by one job (release to block), preemptions excluded')}
      ${line('Response time R', s.rmax ? `min ${fmtTime(s.rmin)}, max ${fmtTime(s.rmax)}` : '–', 'From release to the end of the job, waits included')}
      ${line('Computed worst R', m ? fmtTime(m.R) + (m.ok === false ? ' (exceeds the deadline)' : '') : '–', 'Response-time analysis with the measured C of every task')}
      ${line('Deadline misses', s.miss ?? '–')}
      ${line('Jobs per second', s.jobs ?? '–')}
    </dl>
    <div class="sparks">
      <div><small>CPU share, last ${t.hist.cpu.length} s</small><canvas id="sp-cpu"></canvas></div>
      <div><small>Deadline misses per second</small><canvas id="sp-miss"></canvas></div>
    </div>`;
  sparkline($('sp-cpu'), t.hist.cpu, color(selected));
  sparkline($('sp-miss'), t.hist.miss, getComputedStyle(document.documentElement).getPropertyValue('--bad').trim(), 1);
}
