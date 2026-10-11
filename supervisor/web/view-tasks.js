// Tasks: every number the probe measures for each task, then a card with curves for the selected one.
import { S, color, fmtMs, escapeHtml, STATES } from './store.js';
import { analyse, describeTasks } from './model.js';

const $ = id => document.getElementById(id);
const css = n => getComputedStyle(document.documentElement).getPropertyValue(n).trim();
let selected = null, chartRC = null, chartCpu = null, chartTag = null;

export function init() {
  $('tk-body').addEventListener('click', e => {
    const tr = e.target.closest('tr[data-tag]');
    if (!tr) return;
    selected = +tr.dataset.tag;
    render();
  });
  addEventListener('resize', resizeCharts);
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
  const tasks = describeTasks(a);
  if (selected === null && tasks.length) selected = (tasks.find(t => !t.idle) || tasks[0]).tag;
  $('tk-body').innerHTML = tasks.map(d => {
    const t = d.t, c = t.cfg, s = t.st || {};
    const boosted = s.prio != null && s.prio !== c.prio;
    const jitter = s.tavg ? Math.max(s.tmax - s.tavg, s.tavg - s.tmin) : 0;
    const cls = d.status.s === 'off' ? '' : d.status.s;
    return `<tr data-tag="${d.tag}" class="${cls} ${selected === d.tag ? 'sel' : ''}">
      <td><i class="sw" style="background:${color(d.tag)}"></i><b>${escapeHtml(c.name)}</b></td>
      <td><span class="badge st-${s.state ?? 'x'}">${STATES[s.state] ?? '–'}</span></td>
      <td><span class="prio">${c.prio}</span>${boosted ? ` <b class="boost" title="priority inherited from a mutex waiter">→ ${s.prio}</b>` : ''}</td>
      <td>${d.idle ? '–' : d.kind === 'periodic' ? fmtMs(d.T) + `<br><small>± ${fmtMs(jitter)}</small>` : d.kind === 'event' ? (d.T ? `~ ${fmtMs(d.T)}<br><small>irregular</small>` : '<small>on demand</small>') : '<small>no job yet</small>'}</td>
      <td>${d.D ? fmtMs(d.D) + `<br><small>${d.declared ? 'declared' : '= period'}</small>` : '<small>none</small>'}</td>
      <td>${d.idle || !s.cmax ? '–' : fmtMs(s.cavg) + `<br><small>max ${fmtMs(s.cmax)}</small>`}</td>
      <td>${d.idle || !s.rmax ? '–' : fmtMs(s.rmin) + ' … ' + fmtMs(s.rmax)}</td>
      <td>${d.Rc != null ? fmtMs(d.Rc) : '–'}</td>
      <td>${d.idle ? '–' : (s.miss ?? '–')}${d.miss10 ? ` <b class="bad-text">+${d.miss10}</b>` : ''}</td>
      <td>${s.cpu != null ? (s.cpu / 10).toFixed(1) + ' %' : '–'}</td>
      <td>${stackCell(t)}</td></tr>`;
  }).join('');
  renderDetail(tasks.find(d => d.tag === selected));
}

function renderDetail(d) {
  const box = $('tk-detail');
  if (!d) { box.innerHTML = '<p class="note">Click a task to see its card.</p>'; chartTag = null; return; }
  const t = d.t, c = t.cfg, s = t.st || {};
  const line = (k, v, hint = '') => `<dt title="${escapeHtml(hint)}">${k}</dt><dd>${v}</dd>`;
  if (chartTag !== d.tag) {
    box.innerHTML = `
      <h3><i class="sw" style="background:${color(d.tag)}"></i>${escapeHtml(c.name)} <span class="chip ${d.status.s}">${escapeHtml(d.status.text)}</span></h3>
      <div class="detail">
        <dl class="kv" id="tk-kv"></dl>
        <div class="detail-charts">
          <div><small>Worst response time R and execution time C per second${d.D ? ', against the deadline' : ''} (ms)</small><div id="tk-rc" class="chart"></div></div>
          <div><small>CPU share per second (%)</small><div id="tk-cpu" class="chart small"></div></div>
        </div>
      </div>`;
    makeCharts(d);
    chartTag = d.tag;
  } else {
    box.querySelector('h3 .chip').className = 'chip ' + d.status.s;
    box.querySelector('h3 .chip').textContent = d.status.text;
  }
  $('tk-kv').innerHTML = `
      ${line('Priority', c.prio + (s.prio != null && s.prio !== c.prio ? ` (now ${s.prio}, inherited)` : ''), 'Base priority; the current one can be higher while it holds a mutex wanted by a higher-priority task')}
      ${line('Period', d.kind === 'periodic' ? `${fmtMs(d.T)} (min ${fmtMs(s.tmin)}, max ${fmtMs(s.tmax)})` : d.kind === 'event' ? (d.T ? `irregular, ${fmtMs(d.T)} on average` : 'none: runs on demand') : '–', 'Time between two releases (task becoming ready). Its spread is the release jitter')}
      ${line('Deadline', d.D ? `${fmtMs(d.D)} (${d.declared ? 'declared with rtp_set_deadline' : 'implicit: the measured period'})` : 'none (event-driven task)')}
      ${line('Execution time C', s.cmax ? `avg ${fmtMs(s.cavg)}, max ${fmtMs(s.cmax)} since reset` : '–', 'CPU time actually used by one job (release to block), preemptions excluded')}
      ${line('Response time R', s.rmax ? `min ${fmtMs(s.rmin)}, max ${fmtMs(s.rmax)} since reset` : '–', 'From release to the end of the job, waits included')}
      ${line('Computed worst R', d.Rc != null ? fmtMs(d.Rc) + (d.D && d.Rc > d.D ? ' (exceeds the deadline)' : '') : '–', 'Response-time analysis with the measured C of every task')}
      ${line('Deadline misses', `${s.miss ?? '–'} since reset, ${d.miss10} in the last 10 s`)}
      ${line('Jobs per second', s.jobs ?? '–')}
      ${line('Stack', `${c.stack} words, ${s.stack ?? '–'} never used`, 'High-water mark: the free space that was never touched since reset')}`;
  updateCharts(d);
}

function opts(el, series, range) {
  const axis = { stroke: css('--mut'), grid: { stroke: css('--line'), width: 1 }, ticks: { stroke: css('--line') } };
  return {
    width: el.clientWidth || 400, height: el.clientHeight || 180,
    scales: { x: { time: false }, y: range ? { range } : { auto: true, range: (u, lo, hi) => [0, Math.max(hi * 1.1, 0.01)] } },
    axes: [{ ...axis, values: (u, v) => v.map(x => x + ' s') }, axis],
    series: [{ label: 'time', value: (u, v) => v == null ? '-' : v + ' s' }, ...series],
    cursor: { drag: { x: false, y: false } },
    legend: { live: true },
  };
}

function makeCharts(d) {
  chartRC?.destroy(); chartCpu?.destroy(); chartRC = chartCpu = null;
  if (!window.uPlot) return;
  const ms = (u, v) => v == null ? '-' : v.toFixed(3) + ' ms';
  const col = color(d.tag);
  chartRC = new uPlot(opts($('tk-rc'), [
    { label: 'R (worst)', stroke: col, width: 2, value: ms },
    { label: 'C (worst)', stroke: col, width: 1.5, dash: [], fill: col + '22', value: ms },
    { label: 'deadline', stroke: css('--bad'), width: 1, value: ms },
  ]), [[], [], [], []], $('tk-rc'));
  chartCpu = new uPlot(opts($('tk-cpu'), [{ label: 'CPU', stroke: col, width: 2, fill: col + '22', value: (u, v) => v == null ? '-' : v.toFixed(1) + ' %' }]), [[], []], $('tk-cpu'));
}

function updateCharts(d) {
  if (!chartRC) return;
  const h = d.t.hist, toMs = v => v / S.fcpu * 1000;
  const n = h.rmax.length, x = h.rmax.map((_, i) => i - n + 1);
  chartRC.setData([x, h.rmax.map(toMs), h.cmax.map(toMs), x.map(() => (d.D ? toMs(d.D) : null))]);
  const m = h.cpu.length;
  chartCpu.setData([h.cpu.map((_, i) => i - m + 1), h.cpu]);
  if (Math.abs(chartRC.width - $('tk-rc').clientWidth) > 2) resizeCharts();   // the view was hidden, or resized
}

function resizeCharts() {
  if (chartRC) chartRC.setSize({ width: $('tk-rc').clientWidth, height: $('tk-rc').clientHeight });
  if (chartCpu) chartCpu.setSize({ width: $('tk-cpu').clientWidth, height: $('tk-cpu').clientHeight });
}
