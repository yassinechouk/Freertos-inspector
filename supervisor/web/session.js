// Sessions: record everything received, export it to a JSON file, load it back and replay it
// with its original timing. A recorded session also works on a static web host (no device):
// open the page with ?session=<url of a session file> to replay it directly.
import { S, ingest, ingestTrace, reset, bus } from './store.js';

let recording = null;        // { start, msgs: [[ms, 'j', obj] | [ms, 'b', base64]] }
let player = null;           // { msgs, i, t0, speed, paused, pausedAt, duration }

const b64 = buf => { let s = ''; const u = new Uint8Array(buf); for (let i = 0; i < u.length; i++) s += String.fromCharCode(u[i]); return btoa(s); };
const unb64 = str => { const s = atob(str); const u = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i); return u.buffer; };
const notify = () => bus.dispatchEvent(new CustomEvent('session'));

export function isRecording() { return recording !== null; }
export function recordedCount() { return recording ? recording.msgs.length : 0; }
export function replayState() { return player ? { pos: player.i, total: player.msgs.length, paused: player.paused, speed: player.speed } : null; }

/* Called for every live message (before ingestion). */
export function capture(data) {
  if (!recording) return;
  const ms = Math.round(performance.now() - recording.start);
  if (typeof data === 'string') recording.msgs.push([ms, 'j', JSON.parse(data)]);
  else recording.msgs.push([ms, 'b', b64(data)]);
  if (recording.msgs.length % 200 === 0) notify();
}

export function startRecording() { recording = { start: performance.now(), msgs: [] }; notify(); }

export function stopRecording() {
  if (!recording) return;
  const rec = recording;
  recording = null;
  notify();
  const file = { format: 'rtos-doctor-session', version: 1, recordedAt: new Date().toISOString(), fcpu: S.fcpu, messages: rec.msgs };
  const blob = new Blob([JSON.stringify(file)], { type: 'application/json' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = `freertos-inspector-session-${new Date().toISOString().slice(0, 19).replace(/[:T]/g, '-')}.json`;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 10000);
}

export function loadSession(text) {
  const f = JSON.parse(text);
  if (f.format !== 'rtos-doctor-session' || !Array.isArray(f.messages)) throw new Error('not a FreeRTOS Inspector session file');
  S.source = 'replay';
  reset();
  S.synthetic = !!f.synthetic;
  player = { msgs: f.messages, i: 0, t0: performance.now(), speed: 1, paused: false, duration: f.messages.length ? f.messages[f.messages.length - 1][0] : 0 };
  notify();
  requestAnimationFrame(step);
}

function step() {
  if (!player) return;
  if (!player.paused) {
    const now = (performance.now() - player.t0) * player.speed;
    while (player.i < player.msgs.length && player.msgs[player.i][0] <= now) {
      const [, kind, data] = player.msgs[player.i++];
      if (kind === 'j') ingest(data); else ingestTrace(unb64(data));
    }
    if (player.i >= player.msgs.length) { player.paused = true; notify(); }
  }
  requestAnimationFrame(step);
}

export function togglePause() {
  if (!player) return;
  if (player.paused) {
    if (player.i >= player.msgs.length) { reset(); player.i = 0; player.t0 = performance.now(); }      // replay from start
    else { player.t0 = performance.now() - player.pausedAt / player.speed; }
    player.paused = false;
  } else {
    player.pausedAt = (performance.now() - player.t0) * player.speed;
    player.paused = true;
  }
  notify();
}

export function setSpeed(x) {
  if (!player) return;
  const pos = (performance.now() - player.t0) * player.speed;
  player.speed = x;
  player.t0 = performance.now() - pos / x;
  notify();
}

export function backToLive() {
  player = null;
  S.source = 'live';
  S.synthetic = false;
  reset();
  notify();
}

/* ?session=<url>: replay a published session (demo without hardware). */
export async function autoLoadFromUrl() {
  const url = new URLSearchParams(location.search).get('session');
  if (!url) return false;
  const r = await fetch(url);
  loadSession(await r.text());
  return true;
}
