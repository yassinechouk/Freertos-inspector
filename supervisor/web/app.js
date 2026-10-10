// RTOS Doctor dashboard: connection to the supervisor, navigation, session controls.
import { S, bus, ingest, ingestTrace } from './store.js';
import * as session from './session.js';
import './alerts.js';
import * as overview from './view-overview.js';
import * as tasks from './view-tasks.js';
import * as timeline from './view-timeline.js';
import * as analysis from './view-analysis.js';
import * as resources from './view-resources.js';
import * as alertsView from './view-alerts.js';
import * as faults from './view-faults.js';
import * as motorView from './view-motor.js';

const $ = id => document.getElementById(id);
const VIEWS = { overview, tasks, timeline, analysis, resources, alerts: alertsView, faults, motor: null };
let ws = null, current = 'overview', msgs = 0, msgRate = 0;

/* ---- Connection --------------------------------------------------------------------- */
function send(o) {
  if (S.source !== 'live') return;
  if (ws && ws.readyState === 1) ws.send(JSON.stringify(o));
}

function connect() {
  if (location.protocol === 'file:') return;
  // ?ws=<host> talks to a gateway while the page is served from elsewhere (development).
  ws = new WebSocket(`ws://${new URLSearchParams(location.search).get('ws') || location.host}/ws`);
  ws.binaryType = 'arraybuffer';
  ws.onopen = () => { S.connected = true; header(); };
  ws.onclose = () => { S.connected = false; header(); setTimeout(connect, 1500); };
  ws.onmessage = e => {
    msgs++;
    session.capture(e.data);
    if (S.source !== 'live') return;                     // replaying: live data is ignored
    if (typeof e.data === 'string') ingest(JSON.parse(e.data));
    else ingestTrace(e.data);
  };
}

/* ---- Header: source, link, target, session ------------------------------------------- */
function pill(el, text, cls) { el.textContent = text; el.className = 'pill ' + (cls || ''); }

function header() {
  const rp = session.replayState();
  if (rp) pill($('h-src'), `replay ${rp.pos}/${rp.total}${rp.paused ? ' (paused)' : ''}`, 'warn');
  else pill($('h-src'), S.connected ? `live · ${msgRate} msg/s` : 'not connected', S.connected ? 'ok' : 'bad');
  const g = S.gw;
  pill($('h-target'), !g ? 'target: –' : g.link ? 'target: online' : 'target: lost', !g ? '' : g.link ? 'ok' : 'bad');
  const c = alertsView.counts();
  $('t-alerts').dataset.badge = c.bad ? c.bad : c.warn ? c.warn : '';
  $('t-alerts').className = 'tab' + (current === 'alerts' ? ' on' : '') + (c.bad ? ' badge-bad' : c.warn ? ' badge-warn' : '');
  $('s-rec').textContent = session.isRecording() ? `■ Stop and save (${session.recordedCount()})` : '● Record';
  $('s-rec').classList.toggle('rec', session.isRecording());
  $('s-live').hidden = !rp;
  $('s-play').hidden = !rp;
  $('s-speed').hidden = !rp;
  if (rp) $('s-play').textContent = rp.paused ? '▶ Play' : '❚❚ Pause';
}

function show(view) {
  current = view;
  document.querySelectorAll('.tab').forEach(t => t.classList.toggle('on', t.dataset.view === view));
  document.querySelectorAll('.view').forEach(v => v.classList.toggle('on', v.id === view));
  if (view === 'motor') motorView.resize();
  if (VIEWS[view]) VIEWS[view].render(true);
  header();
  try { localStorage.setItem('rd-view', view); } catch { /* storage unavailable */ }
}

/* ---- Start --------------------------------------------------------------------------- */
document.querySelectorAll('.tab').forEach(t => t.onclick = () => show(t.dataset.view));
$('s-rec').onclick = () => (session.isRecording() ? session.stopRecording() : session.startRecording());
$('s-load').onclick = () => $('s-file').click();
$('s-file').onchange = async () => {
  const f = $('s-file').files[0];
  if (!f) return;
  try { session.loadSession(await f.text()); } catch (e) { alert('Cannot load this file: ' + e.message); }
  $('s-file').value = '';
};
$('s-play').onclick = () => session.togglePause();
$('s-speed').onchange = () => session.setSpeed(+$('s-speed').value);
$('s-live').onclick = () => session.backToLive();
bus.addEventListener('session', header);

for (const v of [tasks, timeline, analysis, alertsView]) v.init();
faults.init({ send });
motorView.init({ send });

setInterval(() => { msgRate = msgs; msgs = 0; header(); }, 1000);
setInterval(() => { if (VIEWS[current]) VIEWS[current].render(); }, 500);
bus.addEventListener('trace', () => { if (current === 'timeline') timeline.render(); });

let start = 'overview';
try { start = localStorage.getItem('rd-view') || 'overview'; } catch { /* storage unavailable */ }
show(VIEWS.hasOwnProperty(start) ? start : 'overview');
session.autoLoadFromUrl().catch(e => alert('Cannot load the session: ' + e.message)).finally(() => {
  if (S.source === 'live') connect();
});
