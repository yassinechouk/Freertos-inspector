// Resources: stacks, heap, queues, interrupts and the cost of observing.
import { S, sortedTasks, fmtTime, escapeHtml } from './store.js';

const $ = id => document.getElementById(id);
const level = (usedPct, warn, bad) => usedPct >= bad ? 'bad' : usedPct >= warn ? 'warn' : 'ok';

export function render() {
  const stacks = sortedTasks().filter(([, t]) => t.st)
    .map(([tag, t]) => ({ name: t.cfg.name, total: t.cfg.stack, free: t.st.stack, used: 100 - t.st.stack / t.cfg.stack * 100 }))
    .sort((a, b) => b.used - a.used);
  $('rs-stacks').innerHTML = stacks.map(s => `
    <div class="meter"><span>${escapeHtml(s.name)}</span>
      <div class="bar ${level(s.used, 70, 90)}"><i style="width:${s.used.toFixed(0)}%"></i></div>
      <b>${s.used.toFixed(0)} %</b><small>${s.total - s.free} / ${s.total} words</small></div>`).join('') || '<p class="note">Waiting.</p>';

  const y = S.sys;
  if (y) {
    const used = 100 - y.heapFree / y.heapTotal * 100, peak = 100 - y.heapMin / y.heapTotal * 100;
    $('rs-heap').innerHTML = `
      <div class="meter"><span>now</span><div class="bar ${level(used, 70, 90)}"><i style="width:${used.toFixed(0)}%"></i></div><b>${used.toFixed(0)} %</b><small>${y.heapTotal - y.heapFree} / ${y.heapTotal} B</small></div>
      <div class="meter"><span>peak</span><div class="bar ${level(peak, 70, 90)}"><i style="width:${peak.toFixed(0)}%"></i></div><b>${peak.toFixed(0)} %</b><small>minimum free ever ${y.heapMin} B</small></div>`;
    $('rs-cost').innerHTML = `
      <dl class="kv">
        <dt title="Cycles spent in the probe hooks / all cycles">Probe hooks</dt><dd>${(y.probe / 10).toFixed(2)} % of the CPU</dd>
        <dt title="Time between two tasks: context switch and scheduler">Kernel (switching)</dt><dd>${(y.kernel / 10).toFixed(2)} %</dd>
        <dt>Trace events</dt><dd>${y.ev} / s</dd>
        <dt>Trace events lost</dt><dd>${y.drop} on the target, ${S.traceDropGw} on the supervisor</dd>
        <dt>Context switch, worst measured</dt><dd>${fmtTime(y.ctx)}</dd>
        <dt>Interrupt to task, worst measured</dt><dd>${fmtTime(y.isr)}</dd>
        <dt>ADC DMA handler, longest in the trace</dt><dd>${fmtTime(S.isrMaxCyc)}</dd>
        <dt>Supervisor link</dt><dd>${S.gw ? `${S.gw.rx} frames, ${S.gw.crc} CRC errors, ${S.gw.lost} lost, Wi-Fi ${S.gw.rssi} dBm` : '–'}</dd>
      </dl>`;
  }

  const qs = [...S.queues.values()];
  $('rs-queues').innerHTML = qs.map(q => {
    const now = q.wait / q.len * 100, peak = q.max / q.len * 100;
    return `<div class="meter"><span>${escapeHtml(q.name)}</span>
      <div class="bar ${level(peak, 75, 100)}"><i style="width:${now.toFixed(0)}%"></i><em style="left:${peak.toFixed(0)}%" title="highest fill"></em></div>
      <b>${q.wait} / ${q.len}</b><small>peak ${q.max}, ${q.fail} send failure${q.fail === 1 ? '' : 's'}</small></div>`;
  }).join('') || '<p class="note">No queue registered (rtp_queue_register).</p>';
}
