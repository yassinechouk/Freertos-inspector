// HIL bench dashboard: WebSocket client, live curves (uPlot), health indicators, controls.
import * as rtos from '/rtos.js';

const $ = id => document.getElementById(id);
const css = name => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

const WINDOW = 1000;                       // samples kept: 10 s at 100 Hz
const T = [], SP = [], RPM = [], DUTY = [];
let ws = null, mode = '?', msgs = 0, msgRate = 0, lastTel = 0;
let prevCrc = null, prevLost = null, motor = null;

/* ---- 3D motor: optional, loaded on its own so a WebGL problem never breaks the rest ---- */
import('/motor3d.js')
  .then(m => { motor = m.createMotor($('motor3d')); $('motor3dmsg')?.remove(); $('slowmo').textContent = motor.note; })
  .catch(e => { $('motor3dmsg').textContent = '3D view unavailable: ' + e.message; });

/* ---- Charts ------------------------------------------------------------------------- */
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
const chartSpeed = makeChart($('chartSpeed'), [line('setpoint', '--sp', 'rpm'), line('speed', '--rpm', 'rpm')], 'rpm');
const chartDuty = makeChart($('chartDuty'), [line('duty', '--duty', '%')], '%', [0, 100]);
addEventListener('resize', () => {
  for (const [c, el] of [[chartSpeed, $('chartSpeed')], [chartDuty, $('chartDuty')]]) c.setSize({ width: el.clientWidth, height: el.clientHeight });
});

/* ---- Indicators ----------------------------------------------------------------------- */
function pill(el, text, cls) { el.textContent = text; el.className = 'pill ' + (cls || ''); }
function ind(name, cls, text) { $('h' + name).className = cls; $('t' + name).textContent = text; }
function log(s) {
  const li = document.createElement('li');
  li.textContent = new Date().toLocaleTimeString() + '  ' + s;
  $('log').prepend(li);
  while ($('log').children.length > 300) $('log').lastChild.remove();
}

setInterval(() => {                         // message rate and staleness, once per second
  msgRate = msgs; msgs = 0;
  const open = ws && ws.readyState === 1;
  const stale = Date.now() - lastTel > 2000;
  ind('ws', !open ? 'bad' : stale ? 'warn' : 'ok', open ? `${msgRate} msg/s` + (stale ? ', no telemetry' : '') : 'disconnected');
}, 1000);

/* ---- Messages from Elyssa --------------------------------------------------------------- */
function onTelemetry(m) {
  for (const [t, sp, rpm, duty] of m.s) { T.push(t / 1000); SP.push(sp); RPM.push(rpm); DUTY.push(duty); }
  const extra = T.length - WINDOW;
  if (extra > 0) for (const a of [T, SP, RPM, DUTY]) a.splice(0, extra);
  chartSpeed.setData([T, SP, RPM]);
  chartDuty.setData([T, DUTY]);

  const [, sp, rpm, duty] = m.s[m.s.length - 1];
  $('vsp').textContent = sp.toFixed(0); $('vrpm').textContent = rpm.toFixed(1); $('vduty').textContent = duty.toFixed(1);
  mode = m.mode;
  pill($('pmode'), 'mode: ' + mode);
  pill($('pfault'), m.fault ? 'fault: STALL' : 'fault: none', m.fault ? 'bad' : 'ok');
  ind('fault', m.fault ? 'bad' : 'ok', m.fault ? 'STALL (reset the STM32)' : 'none');
  document.querySelectorAll('[data-m]').forEach(b => b.classList.toggle('on', b.dataset.m.toUpperCase() === mode));
  if (motor) motor.set(rpm, duty / 100, m.fault);
  lastTel = Date.now();
}

function onSys(m) {
  pill($('pstm'), m.link ? 'STM32: ok' : 'STM32: lost', m.link ? 'ok' : 'bad');
  ind('stm', m.link ? 'ok' : 'bad', m.link ? `${m.rx} frames` : 'no frame for 300 ms');
  ind('crc', prevCrc !== null && m.crc > prevCrc ? 'warn' : m.crc ? 'warn' : 'ok', String(m.crc));
  ind('lost', prevLost !== null && m.lost > prevLost ? 'warn' : 'ok', String(m.lost));
  prevCrc = m.crc; prevLost = m.lost;
  ind('wifi', m.rssi >= -67 ? 'ok' : m.rssi >= -80 ? 'warn' : 'bad', `${m.rssi} dBm`);
  ind('heap', m.heap > 50000 ? 'ok' : 'warn', `${Math.round(m.heap / 1024)} kB free`);
  $('addr').textContent = `supervisor at ${m.ip}, up ${Math.round(m.up / 1000)} s, ${m.clients} browser(s)`;
}

function connect() {
  ws = new WebSocket(`ws://${location.host}/ws`);
  ws.binaryType = 'arraybuffer';
  ws.onopen = () => pill($('pws'), 'browser: ok', 'ok');
  ws.onclose = () => { pill($('pws'), 'browser: lost', 'bad'); setTimeout(connect, 1000); };
  ws.onmessage = e => {
    msgs++;
    if (typeof e.data !== 'string') { if (new Uint8Array(e.data)[0] === 0x54) rtos.onTrace(e.data); return; }
    const m = JSON.parse(e.data);
    if (m.type === 'tel' && m.s.length) onTelemetry(m);
    else if (m.type === 'sys') onSys(m);
    else if (m.type === 'evt') log(`EVENT ${m.name}${m.arg ? ' (' + m.arg + ')' : ''}  t=${m.t} ms`);
    else if (m.type === 'ack') log(`ACK ${m.cmd} ${m.status} ${m.value}`);
    else if (m.type === 'info') log(m.text);
    else if (m.type === 'task') rtos.onTask(m);
    else if (m.type === 'tstat') rtos.onStats(m);
    else if (m.type === 'rsys') rtos.onSys(m);
  };
}

/* ---- Controls --------------------------------------------------------------------------- */
function send(o) { if (ws && ws.readyState === 1) ws.send(JSON.stringify(o)); }
document.querySelectorAll('[data-m]').forEach(b => b.onclick = () => send({ cmd: 'mode', value: b.dataset.m }));
$('sp').oninput = () => { $('spv').textContent = $('sp').value; };
$('stop').onclick = () => { $('sp').value = 0; $('spv').textContent = 0; send({ cmd: 'sp', value: 0 }); };
setInterval(() => { if (mode === 'REMOTE') send({ cmd: 'sp', value: +$('sp').value }); }, 200);   // operator keep-alive

document.querySelectorAll('.tab').forEach(t => t.onclick = () => {
  document.querySelectorAll('.tab').forEach(x => x.classList.toggle('on', x === t));
  document.querySelectorAll('.view').forEach(v => v.classList.toggle('on', v.id === t.dataset.tab));
  dispatchEvent(new Event('resize'));
});

rtos.init();
connect();
