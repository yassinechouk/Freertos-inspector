/* Dashboard page served by Elyssa (step 2: live values, modes, remote setpoint, events).
   Curves, 3D motor and the RTOS layer come in the next steps. */
#pragma once

static const char DASHBOARD_HTML[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>HIL bench</title>
<style>
:root{--bg:#f6f7f9;--card:#fff;--fg:#1c2330;--mut:#667085;--line:#e3e6eb;--ok:#1a7f37;--warn:#b25e00;--bad:#c62828;--acc:#2f5bd3}
@media (prefers-color-scheme:dark){:root{--bg:#12151b;--card:#1b2029;--fg:#e7eaf0;--mut:#98a2b3;--line:#2b3240;--ok:#3fb950;--warn:#d29922;--bad:#f85149;--acc:#6d8cff}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.4 system-ui,sans-serif}
header{display:flex;flex-wrap:wrap;gap:8px;align-items:center;padding:12px 16px;border-bottom:1px solid var(--line);background:var(--card)}
header h1{font-size:17px;margin:0 12px 0 0}.pill{padding:3px 10px;border-radius:99px;font-size:13px;border:1px solid var(--line);color:var(--mut)}
.pill.ok{color:var(--ok);border-color:var(--ok)}.pill.bad{color:var(--bad);border-color:var(--bad)}.pill.warn{color:var(--warn);border-color:var(--warn)}
main{display:grid;gap:12px;padding:16px;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));max-width:1100px;margin:auto}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px}.card h2{font-size:13px;color:var(--mut);margin:0 0 10px;font-weight:600;text-transform:uppercase;letter-spacing:.04em}
.big{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}.big div span{display:block;font-size:26px;font-variant-numeric:tabular-nums}.big div small{color:var(--mut)}
.modes{display:grid;grid-template-columns:repeat(4,1fr);gap:6px}button{font:inherit;padding:8px;border-radius:8px;border:1px solid var(--line);background:var(--bg);color:var(--fg);cursor:pointer}
button.on{background:var(--acc);border-color:var(--acc);color:#fff}button.stop{background:var(--bad);border-color:var(--bad);color:#fff;width:100%;margin-top:8px}
input[type=range]{width:100%}.row{display:flex;justify-content:space-between;color:var(--mut);font-size:13px}
#log{max-height:220px;overflow:auto;font:12px/1.5 ui-monospace,monospace;margin:0;padding:0;list-style:none}#log li{border-bottom:1px solid var(--line);padding:2px 0}
dl{display:grid;grid-template-columns:auto 1fr;gap:4px 12px;margin:0;font-size:13px}dt{color:var(--mut)}dd{margin:0;font-variant-numeric:tabular-nums}
.note{color:var(--mut);font-size:12px;margin-top:8px}
</style></head><body>
<header><h1>HIL motor bench</h1><span id="pws" class="pill">browser link: connecting</span><span id="pstm" class="pill">STM32: ?</span><span id="pmode" class="pill">mode: ?</span><span id="pfault" class="pill">fault: ?</span></header>
<main>
<section class="card"><h2>Live</h2><div class="big">
<div><small>setpoint</small><span id="vsp">-</span><small>rpm</small></div>
<div><small>speed</small><span id="vrpm">-</span><small>rpm</small></div>
<div><small>duty</small><span id="vduty">-</span><small>%</small></div></div></section>
<section class="card"><h2>Mode</h2><div class="modes">
<button data-m="idle">IDLE</button><button data-m="local">LOCAL</button><button data-m="remote">REMOTE</button><button data-m="test">TEST</button></div>
<p class="note">IDLE: setpoint 0. LOCAL: potentiometer. REMOTE: setpoint below. TEST: step-response square wave.</p></section>
<section class="card"><h2>Remote setpoint</h2>
<div class="row"><span>0</span><b><span id="spv">0</span> rpm</b><span>3000</span></div>
<input id="sp" type="range" min="0" max="3000" step="10" value="0">
<button class="stop" id="stop">STOP (0 rpm)</button>
<p class="note">In REMOTE this page resends the setpoint 5 times per second. If the page closes or the Wi-Fi drops, the STM32 brings the motor back to 0 within 1 s (dead-man).</p></section>
<section class="card"><h2>Link and supervisor</h2><dl>
<dt>frames from STM32</dt><dd id="srx">-</dd><dt>CRC errors</dt><dd id="scrc">-</dd><dt>frames lost</dt><dd id="slost">-</dd>
<dt>Wi-Fi signal</dt><dd id="srssi">-</dd><dt>address</dt><dd id="sip">-</dd><dt>browsers</dt><dd id="scli">-</dd><dt>uptime</dt><dd id="sup">-</dd></dl></section>
<section class="card" style="grid-column:1/-1"><h2>Events</h2><ul id="log"></ul></section>
</main>
<script>
const $=id=>document.getElementById(id);let ws,mode='?',spTimer=null;
function pill(el,text,cls){el.textContent=text;el.className='pill '+(cls||'')}
function log(s){const li=document.createElement('li');li.textContent=new Date().toLocaleTimeString()+'  '+s;$('log').prepend(li);while($('log').children.length>200)$('log').lastChild.remove()}
function send(o){if(ws&&ws.readyState===1)ws.send(JSON.stringify(o))}
function connect(){
 ws=new WebSocket('ws://'+location.host+'/ws');
 ws.onopen=()=>pill($('pws'),'browser link: ok','ok');
 ws.onclose=()=>{pill($('pws'),'browser link: lost','bad');setTimeout(connect,1000)};
 ws.onmessage=e=>{const m=JSON.parse(e.data);
  if(m.type==='tel'&&m.s.length){const s=m.s[m.s.length-1];$('vsp').textContent=s[1].toFixed(0);$('vrpm').textContent=s[2].toFixed(1);$('vduty').textContent=s[3].toFixed(1);
   mode=m.mode;pill($('pmode'),'mode: '+mode,'');pill($('pfault'),m.fault?'fault: STALL (reset the STM32)':'fault: none',m.fault?'bad':'ok');
   document.querySelectorAll('[data-m]').forEach(b=>b.classList.toggle('on',b.dataset.m.toUpperCase()===mode));}
  else if(m.type==='sys'){pill($('pstm'),m.link?'STM32: ok':'STM32: lost',m.link?'ok':'bad');$('srx').textContent=m.rx;$('scrc').textContent=m.crc;$('slost').textContent=m.lost;
   $('srssi').textContent=m.rssi+' dBm';$('sip').textContent=m.ip;$('scli').textContent=m.clients;$('sup').textContent=Math.round(m.up/1000)+' s';}
  else if(m.type==='evt')log('EVENT '+m.name+(m.arg?' ('+m.arg+')':'')+'  t='+m.t+' ms');
  else if(m.type==='ack')log('ACK '+m.cmd+' '+m.status+' '+m.value);
  else if(m.type==='info')log(m.text);};
}
document.querySelectorAll('[data-m]').forEach(b=>b.onclick=()=>send({cmd:'mode',value:b.dataset.m}));
$('sp').oninput=()=>{$('spv').textContent=$('sp').value};
$('stop').onclick=()=>{$('sp').value=0;$('spv').textContent=0;send({cmd:'sp',value:0})};
setInterval(()=>{if(mode==='REMOTE')send({cmd:'sp',value:+$('sp').value})},200);
connect();
</script></body></html>)HTML";
