// Demo application: the hardware-in-the-loop motor bench that the RTOS runs.
// Live curves (uPlot), the 3D motor (Three.js, loaded on its own) and the controls.
import { S, bus } from './store.js';

const $ = id => document.getElementById(id);
const css = n => getComputedStyle(document.documentElement).getPropertyValue(n).trim();
const WINDOW = 1000;                              // samples: 10 s at 100 Hz
const T = [], SP = [], RPM = [], DUTY = [];
let chartSpeed = null, chartDuty = null, motor = null, send = () => {};

function makeChart(el, series, yLabel, range) {
  const axis = label => ({ stroke: css('--mut'), grid: { stroke: css('--line'), width: 1 }, ticks: { stroke: css('--line') }, label });
  return new uPlot({
    width: el.clientWidth, height: el.clientHeight,
    scales: { x: { time: false }, y: range ? { range } : { auto: true } },
    axes: [axis('time (s)'), axis(yLabel)],
    series: [{ label: 't (s)', value: (u, v) => v == null ? '-' : v.toFixed(2) }, ...series],
    cursor: { drag: { x: false, y: false } },
  }, [[], ...series.map(() => [])], el);
}
const line = (label, color, unit) => ({ label, stroke: css(color), width: 1.6, value: (u, v) => v == null ? '-' : v.toFixed(1) + ' ' + unit });

export function init(ctx) {
  send = ctx.send;
  if (window.uPlot) {
    chartSpeed = makeChart($('mo-speed'), [line('setpoint', '--sp', 'rpm'), line('speed', '--rpm', 'rpm')], 'rpm');
    chartDuty = makeChart($('mo-duty'), [line('duty', '--duty', '%')], '%', [0, 100]);
  }
  import('./motor3d.js')
    .then(m => { motor = m.createMotor($('mo-3d')); $('mo-3dmsg')?.remove(); $('mo-slow').textContent = motor.note; })
    .catch(e => { $('mo-3dmsg').textContent = '3D view unavailable: ' + e.message; });

  document.querySelectorAll('[data-m]').forEach(b => b.onclick = () => send({ cmd: 'mode', value: b.dataset.m }));
  $('mo-sp').oninput = () => { $('mo-spv').textContent = $('mo-sp').value; };
  $('mo-stop').onclick = () => { $('mo-sp').value = 0; $('mo-spv').textContent = 0; send({ cmd: 'sp', value: 0 }); };
  setInterval(() => { if (S.motor.mode === 'REMOTE' && S.source === 'live') send({ cmd: 'sp', value: +$('mo-sp').value }); }, 200);   // operator keep-alive
  addEventListener('resize', resize);

  bus.addEventListener('telemetry', ev => {
    const m = ev.detail;
    for (const [t, sp, rpm, duty] of m.s) { T.push(t / 1000); SP.push(sp); RPM.push(rpm); DUTY.push(duty); }
    const extra = T.length - WINDOW;
    if (extra > 0) for (const a of [T, SP, RPM, DUTY]) a.splice(0, extra);
    const [, sp, rpm, duty] = m.s[m.s.length - 1];
    if (motor) motor.set(rpm, duty / 100, m.fault);
    if (!$('motor').classList.contains('on')) return;
    $('mo-vsp').textContent = sp.toFixed(0); $('mo-vrpm').textContent = rpm.toFixed(1); $('mo-vduty').textContent = duty.toFixed(1);
    $('mo-fault').textContent = m.fault ? 'STALL: output forced off (reset the target)' : 'none';
    document.querySelectorAll('[data-m]').forEach(b => b.classList.toggle('on', b.dataset.m.toUpperCase() === m.mode));
    if (chartSpeed) { chartSpeed.setData([T, SP, RPM]); chartDuty.setData([T, DUTY]); }
  });
}

export function resize() {
  if (!chartSpeed) return;
  chartSpeed.setSize({ width: $('mo-speed').clientWidth, height: $('mo-speed').clientHeight });
  chartDuty.setSize({ width: $('mo-duty').clientWidth, height: $('mo-duty').clientHeight });
}
