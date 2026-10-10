// Alerts: the journal, newest first, with an explanation for each entry.
import { escapeHtml } from './store.js';
import { alerts, clearAlerts, titleOf, repeatOf } from './alerts.js';

const $ = id => document.getElementById(id);
let filter = 'all';

export function init() {
  document.querySelectorAll('[data-af]').forEach(b => b.onclick = () => {
    filter = b.dataset.af;
    document.querySelectorAll('[data-af]').forEach(x => x.classList.toggle('on', x === b));
    render();
  });
  $('al-clear').onclick = () => { clearAlerts(); render(); };
}

export function counts() {
  return { bad: alerts.filter(a => a.sev === 'bad').length, warn: alerts.filter(a => a.sev === 'warn').length };
}

export function render() {
  const list = alerts.filter(a => filter === 'all' || a.sev === filter).slice().reverse();
  $('al-list').innerHTML = list.map(a => `
    <li class="${a.sev}">
      <div><span class="sev">${a.sev === 'bad' ? 'problem' : a.sev === 'warn' ? 'warning' : 'info'}</span>
        <b>${escapeHtml(titleOf(a))}</b><small>${new Date(a.at).toLocaleTimeString()}${a.tMs != null ? ` · target t = ${(a.tMs / 1000).toFixed(3)} s` : ''}${a.count > 1 ? ` · <b class="rep">${escapeHtml(repeatOf(a))}</b>` : ''}</small></div>
      ${a.why ? `<p>${escapeHtml(a.why)}</p>` : ''}
    </li>`).join('') || '<li class="ok"><div><b>Nothing to report.</b></div></li>';
}
