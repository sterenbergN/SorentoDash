// Phone setup: a Wi-Fi access point with a captive portal serving a small
// page to name, reorder, hide and test horns, and adjust settings.
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Update.h>
#include "EventLog.h"
#include "StatsPage.h"

// Implemented by the sketch.
String webStateJson();
void webSaveHorns(const String &body);
void webSaveSettings(const String &body);
void webPlay(const String &body);
void webObd(const String &body);
void webMaint(const String &body);
String webHistoryJson();
String webHealthJson();
void webClearHistory();
String webBackupJson();
String webRestore(const String &body);
String webLogText();
void webClearLog();
void webOtaProgress(size_t done, size_t total);
void webOtaFinished(bool ok);

static const char SETUP_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sorento Dash</title>
<style>
:root{--bg:#0b0c0e;--card:#16181c;--line:#26292f;--text:#f3f4f6;--dim:#8b919a;--accent:#ff8a1f;--ok:#34d399;--warn:#f59e0b;--bad:#f87171}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:16px/1.4 -apple-system,system-ui,Segoe UI,Roboto,sans-serif;padding:16px;max-width:560px;margin:auto}
h1{font-size:22px;margin:4px 0 2px}
header.top{display:flex;justify-content:space-between;align-items:center;gap:10px}
header.top .btn{height:32px;font-size:13.5px;color:var(--dim)}
#tabs{position:sticky;top:0;z-index:5;display:flex;gap:6px;overflow-x:auto;background:var(--bg);padding:10px 0;margin-bottom:6px;scrollbar-width:none}
#tabs::-webkit-scrollbar{display:none}
#tabs button{flex:1 0 auto;display:inline-flex;align-items:center;justify-content:center;height:36px;min-width:0;border-radius:18px;padding:0 9px;font-size:13.5px;background:var(--card);border:1px solid var(--line);color:var(--dim);text-decoration:none}
#tabs button.on{background:var(--text);border-color:var(--text);color:#111;font-weight:600}
#tabs button.flag::after{content:'';width:7px;height:7px;border-radius:50%;background:var(--warn);margin-left:6px}
section>h2:first-child{margin-top:8px}
.note{color:var(--dim);font-size:13px;margin:0 0 8px}
.act{display:flex;justify-content:flex-end;gap:10px;margin-top:10px}
.sub{color:var(--dim);font-size:14px;margin:0 0 12px}
h2{font-size:13px;letter-spacing:.08em;text-transform:uppercase;color:var(--dim);margin:22px 0 8px}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;overflow:hidden}
.row{display:flex;align-items:center;gap:8px;padding:10px 10px;border-top:1px solid var(--line)}
.row:first-child{border-top:0}
.row.hidden .name{opacity:.45}
.sw{width:30px;height:30px;border:0;padding:0;border-radius:50%;overflow:hidden;background:none;flex:none;cursor:pointer}
.sw::-webkit-color-swatch-wrapper{padding:0}.sw::-webkit-color-swatch{border:0;border-radius:50%}.sw::-moz-color-swatch{border:0;border-radius:50%}
.name{flex:1;min-width:0}
.name input{width:100%;background:transparent;border:0;border-bottom:1px dashed #3a3f47;color:var(--text);font:inherit;font-weight:600;padding:4px 0;outline:none}
.name input::placeholder{color:#9ca3af}
.name input:focus{border-bottom:1px solid var(--accent)}
.name small{display:block;color:var(--dim);font-size:12px}
button{background:#23262c;color:var(--text);border:0;border-radius:10px;height:38px;min-width:38px;padding:0 10px;font:inherit;font-size:15px;cursor:pointer}
button:active{background:#32363d}
button.play{background:var(--accent);color:#111;font-weight:700}
button.on{color:var(--ok)}
button.vis{font-size:13px;min-width:64px}
button.danger{color:var(--bad)}
button.arm{background:#7f1d1d!important;color:#fff!important}
[hidden]{display:none!important}
button{white-space:nowrap}
.hc summary{display:flex;justify-content:space-between;gap:10px;align-items:center;cursor:pointer;list-style:none}
.hc summary::-webkit-details-marker{display:none}
.hc p{margin:8px 0 0;white-space:pre-line}
.chip{font-size:12px;font-weight:700;border-radius:8px;padding:2px 8px;white-space:nowrap;background:#23262c;color:var(--dim)}
.chip.ok{color:#34d399}.chip.watch{color:#f59e0b}.chip.check{color:#fff;background:#b91c1c}
a.btn{display:inline-flex;align-items:center;background:#23262c;color:var(--text);border-radius:10px;height:38px;padding:0 12px;text-decoration:none;font-size:15px}
#log{white-space:pre-wrap;word-break:break-word;font-size:12px;line-height:1.45;max-height:60vh;overflow:auto;background:#0b0c0f;padding:10px;border-radius:10px;margin:10px 0 0}
.set{padding:14px}
.set label{display:flex;justify-content:space-between;font-weight:600;margin-bottom:6px}
.set label span{color:var(--dim);font-weight:500}
.set+.set{border-top:1px solid var(--line)}
input[type=range]{width:100%;accent-color:var(--accent);height:28px}
input.num{width:90px;background:#23262c;border:0;border-radius:10px;height:38px;color:var(--text);font:inherit;padding:0 10px}
select{background:#23262c;color:var(--text);border:0;border-radius:10px;height:38px;font:inherit;padding:0 8px}
.toggle{display:flex;justify-content:space-between;align-items:center;gap:10px}
.grid{display:grid;grid-template-columns:repeat(2,1fr);gap:1px;background:var(--line)}
.grid div{background:var(--card);padding:12px 14px}
.grid b{display:block;font-size:24px;font-variant-numeric:tabular-nums}
.grid small,.muted{color:var(--dim);font-size:13px}
.code{display:flex;gap:10px;align-items:baseline;padding:4px 0}
.code b{font-family:ui-monospace,Menlo,monospace;color:var(--warn)}
#toast{position:fixed;left:50%;bottom:20px;transform:translateX(-50%);background:#23262c;padding:8px 16px;border-radius:20px;font-size:14px;opacity:0;transition:opacity .2s;pointer-events:none}
#toast.show{opacity:1}
.status{font-size:13px;color:var(--dim)}
.svc{padding:12px 14px;border-top:1px solid var(--line)}
.svc .top{display:flex;gap:8px;align-items:center}
.svc input.nm{flex:1;min-width:0;background:transparent;border:0;border-bottom:1px dashed #3a3f47;color:var(--text);font:inherit;font-weight:600;padding:4px 0;outline:none}
.svc .meta{display:flex;flex-wrap:wrap;gap:8px 14px;align-items:center;margin-top:8px;font-size:13px;color:var(--dim)}
.svc .meta input{width:92px;height:32px}
.due{font-weight:600}.due.ok{color:var(--ok)}.due.soon{color:var(--warn)}.due.over{color:var(--bad)}
progress{width:100%;height:10px;accent-color:var(--accent)}
.status b{color:var(--ok)}
.status b.off{color:var(--warn)}
</style></head><body>
<header class="top"><h1>Sorento Dash</h1><a class="btn" href="/stats">Stats &amp; charts &rarr;</a></header>
<nav id="tabs"><button data-tab="car">Car</button><button data-tab="horns">Horns</button><button data-tab="display">Display</button><button data-tab="health">Health</button><button data-tab="service">Service</button><button data-tab="system">System</button></nav>

<section data-tab="car">
<div class="card">
 <div class="set"><b id="obdState"></b><div class="muted" id="obdAdapter"></div></div>
 <div class="grid">
  <div><small>Instant MPG</small><b id="lMpg">-</b></div>
  <div><small>Speed</small><b id="lMph">-</b></div>
  <div><small>Coolant</small><b id="lCool">-</b></div>
  <div><small>Battery</small><b id="lVolt">-</b></div>
 </div>
</div>
<h2>Trip</h2>
<div class="card">
 <div class="set"><label>Trip meter <span id="tripTxt"></span></label>
  <p class="note">Like the trip odometer on the dash. Resetting it doesn't affect fill-ups, calibration or history.</p>
  <div class="toggle"><span class="muted" id="tripMore"></span><button id="resetTrip">Reset trip</button></div></div>
</div>
<h2>Fuel</h2>
<div class="card">
 <div class="set"><label>Since fill-up</label><div id="tankTxt" style="margin-bottom:6px"></div>
  <p class="note">Fill until the pump clicks off, then enter the gallons pumped.</p>
  <div class="toggle"><input class="num" id="pumped" type="number" inputmode="decimal" step="0.1" min="0" placeholder="Gallons"><button class="play" id="fill">I filled up</button></div>
  <p class="note" style="margin-top:14px">Not at the pump? Set the level from the fuel gauge to get a range now.</p>
  <div class="toggle"><select id="level"><option value="100">Full</option><option value="87.5">7/8</option><option value="75">3/4</option><option value="62.5">5/8</option><option value="50" selected>1/2</option><option value="37.5">3/8</option><option value="25">1/4</option><option value="12.5">1/8</option><option value="5">Empty</option></select><button id="setLevel">Set level</button></div></div>
 <div class="set"><label>MPG accuracy <span id="calTxt"></span></label>
  <div class="muted" id="calStep"></div>
  <div class="muted" id="calLast" style="margin-top:6px"></div>
  <div id="incompleteRow" style="margin-top:10px" hidden><p class="note" style="color:var(--warn)">Some driving since the last fill-up wasn't tracked (the display ran 5+ minutes without the adapter), so the next fill-up won't adjust the calibration.</p><div class="act"><button id="tankOk">It was tracked</button></div></div>
  <div id="calResetRow" style="margin-top:10px" hidden><p class="note">Only start over if MPG has been clearly wrong for several tanks.</p><div class="act"><button id="resetCal">Start over</button></div></div></div>
</div>
<h2>Trouble codes</h2>
<div class="card">
 <div class="set"><label>Engine</label><div id="codes" class="muted">-</div>
  <div class="toggle" id="clearRow" style="margin-top:10px"><span class="muted">Only clear codes after fixing the problem.</span><button class="danger" id="clear">Clear codes</button></div></div>
 <div class="set"><label>Transmission <span id="transTxt"></span></label><div id="transCodes" class="muted"></div>
  <p class="note">The transmission computer keeps its own list. Check with the car stopped and the ignition on; readings pause for up to 30 seconds, and the details go to the event log.</p>
  <div class="act"><button id="readTrans">Check transmission</button></div></div>
</div>
</section>

<section data-tab="horns">
<p class="sub">Tap a name to rename it, or the dot to pick its color. Changes save automatically. <span class="status" id="status"></span></p>
<div class="card" id="horns"></div>
<h2>Sound</h2>
<div class="card">
 <div class="set"><label>Horn volume <span id="volv"></span></label><input type="range" id="vol" min="0" max="100" step="5"></div>
</div>
</section>

<section data-tab="display">
<div class="card">
 <div class="set"><label>Screen brightness <span id="brv"></span></label><input type="range" id="br" min="10" max="100" step="5"></div>
 <div class="set toggle"><div><b>Dim after</b><div class="muted">Dims the screen when the knob hasn't been touched for this long.</div></div><select id="dimAfter"><option value="15">15 sec</option><option value="30">30 sec</option><option value="60">1 min</option><option value="120">2 min</option><option value="300">5 min</option><option value="600">10 min</option><option value="0">Never</option></select></div>
 <div class="set"><label>Dimmed brightness <span id="dimv"></span></label><input type="range" id="dim" min="10" max="100" step="5"></div>
 <div class="set toggle"><b>Horn screen background</b><select id="tint"><option value="0">Black</option><option value="1">Subtle color</option><option value="2">Bold color</option></select></div>
 <div class="set toggle"><b>LED ring</b><button id="leds"></button></div>
</div>
<h2>Home pages</h2>
<p class="sub">The car pages the knob and swipes step through. Trouble codes and Engine health come back by themselves while they have something to report.</p>
<div class="card" id="pages"></div>
</section>

<section data-tab="health">
<div class="card">
 <div class="set"><div class="muted" id="healthIntro"></div></div>
 <div id="health"></div>
 <div class="set toggle"><span class="muted">After a repair, let it learn what's normal again.</span><button id="relearn">Relearn</button></div>
</div>
</section>

<section data-tab="service">
<div class="card">
 <div class="set"><label>Odometer <span id="odoNow"></span></label>
  <p class="note">Enter the odometer reading once; the display counts miles from OBD after that.</p>
  <div class="toggle"><input class="num" id="odo" type="number" inputmode="numeric" placeholder="Miles" style="width:130px"><button id="setOdo">Set</button></div></div>
 <div class="set"><label>Speed correction <span id="scNow"></span></label>
  <p class="note" id="scHelp"></p>
  <div class="toggle"><span class="muted">Set by hand (%), e.g. from GPS: display 60 mph, GPS 61 = +1.7</span><input class="num" id="sc" type="number" inputmode="decimal" step="0.1" style="width:80px"></div></div>
</div>
<h2>Reminders</h2>
<div class="card">
 <div id="svc"></div>
 <div class="set act" style="margin-top:0"><button id="addSvc">+ Add item</button></div>
</div>
</section>

<section data-tab="system">
<h2>Car setup</h2>
<div class="card">
 <div class="set toggle"><b>Engine size (L)</b><input class="num" id="disp" type="number" inputmode="decimal" step="0.1"></div>
 <div class="set toggle"><b>Tank size (gal)</b><input class="num" id="tankCap" type="number" inputmode="decimal" step="0.1"></div>
 <div class="set toggle"><div><b>Read-only mode</b><div class="muted">Never clear trouble codes. Clearing also wipes the engine computer's learned idle and fuel trims.</div></div><button id="ro"></button></div>
 <div class="set toggle"><div><b>Demo car</b><div class="muted">Simulated data, for trying the car screens without an adapter.</div></div><button id="sim"></button></div>
 <div class="set toggle"><div><b>OBD adapter</b><div class="muted">Forget the remembered adapter to pair a different one.</div></div><button id="forget">Forget</button></div>
</div>
<h2>Backup</h2>
<div class="card">
 <div class="set"><p class="note">Saves every setting, horn name, MPG calibration, tank and trip, service record and the drive history in one file on your phone.</p>
  <a class="btn" id="backup" href="/api/backup" download="sorento-dash-backup.json">Download backup</a>
  <p class="note" style="margin:10px 0 0">If nothing downloads, open <b>192.168.4.1</b> in your phone's browser (Safari or Chrome) while joined to the display's Wi-Fi, and try there.</p></div>
 <div class="set"><label>Restore</label>
  <p class="note">Replaces everything on the display with the backup, then restarts it.</p>
  <input type="file" id="restoreFile" accept=".json,application/json" style="max-width:100%">
  <div class="act"><button id="restore">Restore</button></div>
  <div class="muted" id="restoreMsg" style="margin-top:8px"></div></div>
</div>
<h2>Firmware</h2>
<div class="card">
 <div class="set"><label>Installed <span id="fw"></span></label>
  <p class="note">Pick a Sorento Dash .bin file to update the display over Wi-Fi. Keep this page open until it restarts.</p>
  <input type="file" id="bin" accept=".bin" style="max-width:100%">
  <div style="margin-top:10px;display:flex;gap:10px;align-items:center"><button class="play" id="upload">Install</button><progress id="prog" max="100" value="0" hidden></progress></div>
  <div class="muted" id="fwMsg" style="margin-top:8px"></div></div>
</div>
<h2>Event log</h2>
<div class="card">
 <div class="set"><p class="note">Restarts, adapter connections, drives, fill-ups, warnings and setting changes, newest first. "#12 0:05:23" means the 12th start of the display, 5 min 23 s after it started.</p>
  <div style="display:flex;gap:10px;flex-wrap:wrap"><button id="logShow">Show log</button><a class="btn" href="/api/log" download="sorento-dash-log.txt">Download</a><button id="logClear">Clear</button></div>
  <pre id="log" hidden></pre></div>
</div>
</section>
<div id="toast"></div>
<script>
const COLORS=['#ff8a1f','#22d3ee','#a3e635','#f472b6','#facc15','#a78bfa','#f87171','#2dd4bf'];
let S={horns:[]};
const $=id=>document.getElementById(id);
// Two-tap confirm. Phones open this page in a captive-portal window that
// silently blocks confirm() dialogs, so the button itself asks instead.
function sure(b,fn,ask){if(b.dataset.armed){clearTimeout(b._t);delete b.dataset.armed;b.classList.remove('arm');b.textContent=b._orig;fn();return}
 b._orig=b.textContent;b.dataset.armed=1;b.classList.add('arm');b.textContent=ask||'Tap again';
 b._t=setTimeout(()=>{delete b.dataset.armed;b.classList.remove('arm');b.textContent=b._orig},3000)}
function toast(t){const e=$('toast');e.textContent=t;e.classList.add('show');clearTimeout(e._t);e._t=setTimeout(()=>e.classList.remove('show'),1400)}
async function post(url,obj){try{await fetch(url,{method:'POST',body:JSON.stringify(obj)});return true}catch(e){toast('Not saved - reconnect Wi-Fi');return false}}
function saveHorns(){post('/api/horns',{horns:S.horns.map(h=>({key:h.key,name:h.name,hidden:h.hidden,color:h.color}))}).then(ok=>ok&&toast('Saved'))}
function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
function f(v,d){return v==null?'-':(+v).toFixed(d)}
function show(t){document.querySelectorAll('section[data-tab]').forEach(s=>s.hidden=s.dataset.tab!=t);
 document.querySelectorAll('#tabs button').forEach(b=>{b.classList.toggle('on',b.dataset.tab==t);if(b.dataset.tab==t)b.scrollIntoView({inline:'center',block:'nearest'})});
 try{localStorage.setItem('tab',t)}catch(e){}window.scrollTo(0,0)}
$('tabs').onclick=e=>{const b=e.target.closest('button');if(b)show(b.dataset.tab)};
onhashchange=()=>{const t=location.hash.slice(1);if(document.querySelector('section[data-tab="'+t+'"]'))show(t)};
{let t=location.hash.slice(1);try{t=t||localStorage.getItem('tab')}catch(e){}show(document.querySelector('section[data-tab="'+t+'"]')?t:'car')}
// A dot on a tab that has something needing attention.
function flag(t,on){document.querySelector('#tabs button[data-tab='+t+']').classList.toggle('flag',!!on)}
function render(){
 let vis=0;
 $('horns').innerHTML=S.horns.map((h,i)=>{const auto=COLORS[vis%COLORS.length];if(!h.hidden)vis++;const c=h.color||auto;
  return `<div class="row ${h.hidden?'hidden':''}"><input type="color" class="sw" value="${c}" data-c="${i}" aria-label="Color">
  <div class="name"><input value="${esc(h.name)}" placeholder="${esc(h.auto)}" maxlength="24" data-i="${i}"><small>${esc(h.file)}</small></div>
  <button class="play" data-play="${i}" aria-label="Play">&#9654;</button>
  <button data-up="${i}" aria-label="Move up">&#8593;</button>
  <button data-down="${i}" aria-label="Move down">&#8595;</button>
  <button data-hide="${i}" class="vis ${h.hidden?'':'on'}" aria-label="Show on dial">${h.hidden?'Hidden':'Shown'}</button></div>`}).join('')
  ||'<div class="row">No sound files found on the sound board.</div>';
 $('vol').value=S.volume;$('volv').textContent=S.volume+'%';
 $('br').value=S.brightness;$('brv').textContent=S.brightness+'%';
 $('dimAfter').value=S.dimAfter;$('dim').value=S.dimPct;$('dimv').textContent=S.dimPct+'%';
 $('tint').value=S.tint;
 $('pages').innerHTML=(S.pages||[]).map((p,i)=>`<div class="row ${p.hidden?'hidden':''}"><div class="name"><b>${esc(p.name)}</b></div><button data-pg="${i}" class="vis ${p.hidden?'':'on'}">${p.hidden?'Hidden':'Shown'}</button></div>`).join('');
 $('leds').textContent=S.leds?'On':'Off';$('leds').className=S.leds?'on':'';
 if(S.obd){$('disp').value=S.obd.disp;$('tankCap').value=S.obd.tankCap}
 $('fw').textContent=S.fw||'';
 renderSvc();
 renderLive();
}
function renderLive(){
 $('status').innerHTML=S.connected?'Sound board <b>connected</b>.':'Sound board <b class="off">not connected</b>.';
 const o=S.obd;if(!o)return;
 $('obdState').textContent=o.state+(o.sim?' (demo car)':'');
 $('obdAdapter').textContent=o.adapter?('Adapter: '+o.adapter):'Plug a Bluetooth LE OBD-II adapter into the port under the dash.';
 $('lMpg').textContent=o.mpg==null?(o.gph==null?'-':f(o.gph,1)+' gal/h'):f(o.mpg,1);
 $('lMph').textContent=o.mph==null?'-':f(o.mph,0)+' mph';
 $('lCool').textContent=o.coolant==null?'-':f(o.coolant,0)+'°F';
 $('lVolt').textContent=o.volts==null?'-':f(o.volts,1)+' V';
 $('tripTxt').textContent=f(o.trip.mi,1)+' mi · '+(o.trip.mpg==null?'-':f(o.trip.mpg,1))+' MPG';
 $('tripMore').textContent=f(o.trip.gal,2)+' gal used · '+Math.round(o.trip.sec/60)+' min';
 $('tankTxt').textContent=f(o.tank.mi,0)+' mi · '+f(o.tank.gal,1)+' gal · '+(o.range==null?'level unknown':'~'+f(o.level,0)+'%, ~'+f(o.range,0)+' mi left');
 $('incompleteRow').hidden=!o.tankIncomplete;
 // MPG calibration: where it stands, what the last fill-up did, what to do next.
 const tracked=o.tankFull&&!(o.tankStart>0)&&!o.tankIncomplete;
 $('calTxt').textContent=o.calFills>0?'calibrated from '+o.calFills+' fill-up'+(o.calFills>1?'s':'')+' (\u00d7'+f(o.cal,2)+')':Math.abs(o.cal-1)>0.005?'adjusted (\u00d7'+f(o.cal,2)+')':tracked?'calibrates at next fill-up':'after 2 full fill-ups';
 $('calStep').textContent=(o.tankStart>0?'This tank was started from the gauge: fine for range, too rough to calibrate with. ':'')+
  (!tracked?'Step 1: fill the tank until the pump clicks off and log the gallons. That starts tracking; the full fill-up after it calibrates.':
  o.calFills>0?'Each full fill-up you log compares the display\'s fuel count with the pump and nudges the estimate halfway toward it.':
  'Step 2: drive normally, then fill up completely again and log the gallons. The display compares its own fuel count with the pump and corrects itself.');
 const LF={1:'adjusted the calibration',2:'started tracking',3:'skipped: some driving wasn\'t tracked',4:'skipped: pump and estimate too far apart (partial fill?)',5:'skipped: too few gallons to compare'};
 $('calLast').textContent=o.lastFill?'Last fill-up: '+(o.lastFillEst>0&&o.lastFill!=2?'display counted '+f(o.lastFillEst,1)+' gal, pump said '+f(o.lastFillPumped,1)+' gal, ':'')+LF[o.lastFill]+'.':'';
 $('calResetRow').hidden=!(o.calFills>0||Math.abs(o.cal-1)>0.005);
 $('codes').innerHTML=!o.codesRead?'Not read yet':(o.codes.length?o.codes.map(c=>`<div class="code"><b>${c.code}</b><span>${esc(c.text||'Look this code up online')}</span></div>`).join(''):'No trouble codes'+(o.mil?' (check engine light on)':''));
 $('transTxt').textContent=o.trans||'not checked';$('readTrans').disabled=!!o.transBusy;
 $('transCodes').innerHTML=(o.transCodes||[]).map(c=>`<div class="code"><b>${c.code}</b><span>${esc(c.text||'Look this code up online')}${c.now?' (active now)':' (stored)'}</span></div>`).join('');
 flag('car',o.mil||o.codes.length||(o.transCodes||[]).length);
 $('ro').textContent=o.readOnly?'On':'Off';$('ro').className=o.readOnly?'on':'';$('clearRow').hidden=!!o.readOnly;
 $('sim').textContent=o.sim?'On':'Off';$('sim').className=o.sim?'on':'';
}
$('horns').addEventListener('change',e=>{const d=e.target.dataset;
 if(d.i!==undefined){S.horns[d.i].name=e.target.value.trim();saveHorns()}
 if(d.c!==undefined){S.horns[d.c].color=e.target.value;saveHorns()}});
$('horns').addEventListener('click',e=>{const b=e.target.closest('button');if(!b)return;const d=b.dataset,H=S.horns;
 if(d.play!==undefined){post('/api/play',{key:H[d.play].key});return}
 if(d.up!==undefined&&d.up>0){const i=+d.up;[H[i-1],H[i]]=[H[i],H[i-1]]}
 else if(d.down!==undefined&&d.down<H.length-1){const i=+d.down;[H[i+1],H[i]]=[H[i],H[i+1]]}
 else if(d.hide!==undefined){H[d.hide].hidden=!H[d.hide].hidden}
 else return;
 render();saveHorns()});
$('pages').onclick=e=>{const b=e.target.closest('button');if(!b)return;const P=S.pages,i=+b.dataset.pg;
 if(!P[i].hidden&&P.filter(p=>!p.hidden).length<2){toast('Keep at least one page');return}
 P[i].hidden=!P[i].hidden;render();post('/api/settings',{hiddenPages:P.reduce((m,p,j)=>m|(p.hidden?1<<j:0),0)}).then(ok=>ok&&toast('Saved'))};
$('vol').oninput=e=>$('volv').textContent=e.target.value+'%';
$('vol').onchange=e=>{S.volume=+e.target.value;post('/api/settings',{volume:S.volume})};
$('br').oninput=e=>{$('brv').textContent=e.target.value+'%';post('/api/settings',{brightness:+e.target.value})};
$('br').onchange=e=>{S.brightness=+e.target.value;post('/api/settings',{brightness:S.brightness,save:true})};
$('dimAfter').onchange=e=>{S.dimAfter=+e.target.value;post('/api/settings',{dimAfter:S.dimAfter}).then(ok=>ok&&toast('Saved'))};
$('dim').oninput=e=>$('dimv').textContent=e.target.value+'%';
$('dim').onchange=e=>{S.dimPct=+e.target.value;post('/api/settings',{dimPct:S.dimPct}).then(ok=>ok&&toast('Saved'))};
$('setLevel').onclick=e=>sure(e.target,()=>post('/api/obd',{fuelLevel:+$('level').value}).then(()=>toast('Tank level set')),'Tap again to set');
$('tint').onchange=e=>{S.tint=+e.target.value;post('/api/settings',{tint:S.tint})};
$('leds').onclick=()=>{S.leds=!S.leds;render();post('/api/settings',{leds:S.leds})};
$('resetTrip').onclick=e=>sure(e.target,()=>post('/api/obd',{resetTrip:true}).then(()=>toast('Trip reset')));
$('fill').onclick=()=>{const g=parseFloat($('pumped').value);if(!(g>0)){toast('Enter the gallons pumped');return}
 post('/api/obd',{fillUp:g}).then(()=>{$('pumped').value='';toast('Tank reset & calibrated')})};
$('clear').onclick=e=>sure(e.target,()=>post('/api/obd',{clearCodes:true}).then(()=>toast('Clearing codes')),'Tap again to clear');
$('readTrans').onclick=()=>{const o=S.obd;if(o.state!='Connected'){toast('Turn the ignition on first');return}
 if(o.mph>1){toast('Stop the car first');return}
 post('/api/obd',{readTrans:true}).then(()=>toast('Checking transmission'))};
$('disp').onchange=e=>post('/api/obd',{disp:+e.target.value}).then(()=>toast('Saved'));
$('tankCap').onchange=e=>post('/api/obd',{tankCap:+e.target.value}).then(()=>toast('Saved'));
$('sim').onclick=()=>{S.obd.sim=!S.obd.sim;renderLive();post('/api/obd',{sim:S.obd.sim})};
$('tankOk').onclick=()=>post('/api/obd',{tankComplete:true}).then(()=>toast('Marked as tracked'));
$('resetCal').onclick=e=>sure(e.target,()=>post('/api/obd',{resetCal:true}).then(()=>toast('Calibration reset to default')),'Tap again to reset');
$('ro').onclick=()=>{S.obd.readOnly=!S.obd.readOnly;renderLive();post('/api/obd',{readOnly:S.obd.readOnly})};
$('forget').onclick=()=>post('/api/obd',{forget:true}).then(()=>toast('Adapter forgotten'));
function num(v){return v==null?'':Math.round(v)}
function renderSvc(){
 const m=S.maint;if(!m)return;
 $('odoNow').textContent=m.odometer==null?'not set':Math.round(m.odometer).toLocaleString()+' mi';
 $('scNow').textContent=Math.abs(m.speedCal)<0.05?'none':(m.speedCal>0?'+':'')+m.speedCal.toFixed(1)+'%';
 $('sc').value=m.speedCal;
 $('scHelp').textContent='Learned when you correct the odometer: the display compares the real miles since the last correction with the miles it counted ('+
  (m.sinceSet==null?'starts from your next odometer entry':Math.round(m.sinceSet)+' counted so far, 200+ needed')+'). Corrects speed, trip, range, MPG and the odometer.';
 flag('service',m.items.some(it=>it.remaining!=null&&it.remaining<=300));
 $('svc').innerHTML=m.items.map((it,i)=>{let st='<span class="due">Not logged yet</span>';
  if(it.remaining!=null){const r=Math.round(it.remaining);
   st=r<0?`<span class="due over">Overdue by ${(-r).toLocaleString()} mi</span>`:`<span class="due ${r<=300?'soon':'ok'}">Due in ${r.toLocaleString()} mi</span>`}
  return `<div class="svc"><div class="top"><input class="nm" value="${esc(it.name)}" data-sn="${i}" maxlength="24"><button data-done="${i}">Done today</button></div>
  <div class="meta">${st}<span>every <input class="num" type="number" inputmode="numeric" value="${it.interval}" data-si="${i}"> mi</span>
  ${m.odometer!=null?`<span>last at <input class="num" type="number" inputmode="numeric" placeholder="odometer" value="${num(it.last)}" data-sl="${i}"></span>`:''}
  <button class="danger" data-del="${i}" aria-label="Remove">&times;</button></div></div>`}).join('');
}
function saveSvc(){post('/api/maint',{items:S.maint.items.map(it=>({name:it.name,interval:it.interval,last:it.last}))}).then(()=>refresh(true))}
$('svc').addEventListener('change',e=>{const d=e.target.dataset,I=S.maint.items,v=e.target.value;
 if(d.sn!==undefined)I[d.sn].name=v.trim()||I[d.sn].name;
 else if(d.si!==undefined)I[d.si].interval=Math.max(100,parseInt(v)||I[d.si].interval);
 else if(d.sl!==undefined)I[d.sl].last=v===''?null:parseFloat(v);
 else return;saveSvc()});
$('svc').addEventListener('click',e=>{const b=e.target.closest('button');if(!b)return;const d=b.dataset;
 if(d.done!==undefined){post('/api/maint',{done:+d.done}).then(()=>{toast('Logged');refresh(true)})}
 if(d.del!==undefined)sure(b,()=>{S.maint.items.splice(+d.del,1);saveSvc();toast('Removed')},'Delete?')});
$('addSvc').onclick=()=>{S.maint.items.push({name:'New item',interval:10000,last:null});saveSvc()};
$('sc').onchange=e=>post('/api/maint',{speedCal:+e.target.value}).then(()=>{toast('Saved');refresh(true)});
$('setOdo').onclick=()=>{const v=parseFloat($('odo').value);if(!(v>0)){toast('Enter the odometer reading');return}
 post('/api/maint',{odometer:v}).then(()=>{$('odo').value='';toast('Odometer set');refresh(true)})};
$('upload').onclick=()=>{const f=$('bin').files[0];if(!f){toast('Choose a .bin file first');return}
 const x=new XMLHttpRequest(),fd=new FormData();fd.append('firmware',f,f.name);
 $('prog').hidden=false;$('prog').value=0;$('fwMsg').textContent='Uploading...';$('upload').disabled=true;
 x.upload.onprogress=e=>{if(e.lengthComputable)$('prog').value=e.loaded*100/e.total};
 x.onload=()=>{$('upload').disabled=false;if(x.status==200){$('prog').value=100;$('fwMsg').textContent='Installed. The display is restarting - reconnect to its Wi-Fi in a few seconds.'}
  else $('fwMsg').textContent=x.responseText||'Update failed'};
 x.onerror=()=>{$('upload').disabled=false;$('fwMsg').textContent='Upload failed - check the Wi-Fi connection and try again.'};
 x.open('POST','/update');x.send(fd)};
$('backup').onclick=()=>{$('backup').download='sorento-dash-backup-'+new Date().toISOString().slice(0,10)+'.json'};
$('restore').onclick=e=>{const file=$('restoreFile').files[0];if(!file){toast('Pick a backup file first');return}
 sure(e.target,async()=>{$('restoreMsg').textContent='Restoring...';
  try{const r=await fetch('/api/restore',{method:'POST',body:await file.text()});const t=await r.text();
   $('restoreMsg').textContent=r.ok?'Restored. The display is restarting - reconnect to its Wi-Fi in a few seconds.':t}
  catch(err){$('restoreMsg').textContent='Restore failed - check the Wi-Fi connection and try again.'}},'Tap again to replace everything')};
function showLog(){fetch('/api/log').then(r=>r.text()).then(t=>{const l=t.trim().split('\n');const head=l.shift();
 $('log').textContent=head+'\n\n'+(l.length?l.reverse().join('\n'):'(empty)');$('log').hidden=false;$('logShow').textContent='Refresh'})}
$('logShow').onclick=showLog;
$('logClear').onclick=e=>sure(e.target,()=>post('/api/log/clear',{}).then(()=>{toast('Log cleared');if(!$('log').hidden)showLog()}),'Tap again to clear');
function loadHealth(){fetch('/api/health').then(r=>r.json()).then(h=>{
 $('healthIntro').textContent='Early warnings for worn or failing parts, from '+h.drives+' drive'+(h.drives==1?'':'s')+' so far'+
  (h.learnLeft>0?' (learning this car\'s normal: '+h.learnLeft+' more to go)':'')+'. Tap a check for details. These point at likely suspects, not a diagnosis.';
 flag('health',h.checks.some(c=>c.status=='check'||c.status=='watch'));
 const L={check:'Check',watch:'Watch',ok:'OK',learning:'Learning',nodata:'Needs data'},R={check:4,watch:3,ok:2,learning:1,nodata:0};
 $('health').innerHTML=h.checks.sort((a,b)=>R[b.status]-R[a.status]).map(c=>`<details class="set hc"><summary><b>${esc(c.name)}</b><span style="display:flex;gap:8px;align-items:center"><span class="muted">${esc(c.summary)}</span><span class="chip ${c.status}">${L[c.status]}</span></span></summary><p class="muted">${esc(c.detail)}</p></details>`).join('')}).catch(()=>{})}
loadHealth();setInterval(loadHealth,30000);
$('relearn').onclick=e=>sure(e.target,()=>post('/api/obd',{relearnHealth:true}).then(()=>{toast('Relearning');setTimeout(loadHealth,500)}),'Tap again to relearn');
function refresh(full){fetch('/api/state').then(r=>r.json()).then(j=>{S.maint=j.maint;if(full)renderSvc()})}
function load(){fetch('/api/state').then(r=>r.json()).then(j=>{S=j;render()}).catch(()=>setTimeout(load,1000))}
load();
// Refresh only live readouts so typing in a box is never interrupted.
setInterval(()=>fetch('/api/state').then(r=>r.json()).then(j=>{
 if(!S.horns.length&&j.horns.length){S=j;render()}else{S.connected=j.connected;S.obd=j.obd;renderLive()}}).catch(()=>{}),2500);
</script></body></html>)HTML";

class WebSetup {
public:
  String ssid = "SorentoDash";
  String password;
  const char *url = "http://192.168.4.1";

  void begin() {
    // Per-device password derived from the chip ID.
    uint64_t mac = ESP.getEfuseMac();
    char buf[9];
    snprintf(buf, sizeof(buf), "%08lx", (unsigned long)((mac >> 16) ^ (mac & 0xFFFFFFFF)) & 0xFFFFFFFF);
    password = buf;

    server.on("/", HTTP_GET, [this] { touch(); server.send_P(200, "text/html", SETUP_PAGE); });
    server.on("/api/state", HTTP_GET, [this] { touch(); server.send(200, "application/json", webStateJson()); });
    server.on("/api/horns", HTTP_POST, [this] { touch(); webSaveHorns(server.arg("plain")); server.send(200, "text/plain", "ok"); });
    server.on("/api/settings", HTTP_POST, [this] { touch(); webSaveSettings(server.arg("plain")); server.send(200, "text/plain", "ok"); });
    server.on("/api/play", HTTP_POST, [this] { touch(); webPlay(server.arg("plain")); server.send(200, "text/plain", "ok"); });
    server.on("/api/obd", HTTP_POST, [this] { touch(); webObd(server.arg("plain")); server.send(200, "text/plain", "ok"); });
    server.on("/stats", HTTP_GET, [this] { touch(); server.send_P(200, "text/html", STATS_PAGE); });
    server.on("/api/health", HTTP_GET, [this] { touch(); server.send(200, "application/json", webHealthJson()); });
    server.on("/api/history", HTTP_GET, [this] { touch(); server.send(200, "application/json", webHistoryJson()); });
    server.on("/api/history/clear", HTTP_POST, [this] { touch(); webClearHistory(); server.send(200, "text/plain", "ok"); });
    server.on("/api/maint", HTTP_POST, [this] { touch(); webMaint(server.arg("plain")); server.send(200, "text/plain", "ok"); });
    server.on("/api/backup", HTTP_GET, [this] {
      touch();
      server.sendHeader("Content-Disposition", "attachment; filename=\"sorento-dash-backup.json\"");
      server.send(200, "application/json", webBackupJson());
    });
    server.on("/api/restore", HTTP_POST, [this] {
      touch();
      String err = webRestore(server.arg("plain"));
      server.sendHeader("Connection", "close");
      server.send(err.length() ? 400 : 200, "text/plain", err.length() ? err : String("OK"));
      if (err.isEmpty()) {
        events.add("Backup received, restarting to restore it");
        delay(500);
        ESP.restart();
      }
    });
    server.on("/api/log", HTTP_GET, [this] { touch(); server.send(200, "text/plain; charset=utf-8", webLogText()); });
    server.on("/api/log/clear", HTTP_POST, [this] { touch(); webClearLog(); server.send(200, "text/plain", "ok"); });
    server.on("/update", HTTP_POST, [this] { finishOta(); }, [this] { receiveOta(); });
    // Captive portal: send every other request (OS connectivity checks) to the page.
    server.onNotFound([this] {
      touch();
      server.sendHeader("Location", url, true);
      server.send(302, "text/plain", "");
    });
  }

  bool active() const { return running; }
  int stations() const { return running ? WiFi.softAPgetStationNum() : 0; }
  uint32_t lastActivity() const { return lastSeen; }

  // "WIFI:" URI understood by phone cameras to join a network.
  String joinUri() const { return "WIFI:T:WPA;S:" + ssid + ";P:" + password + ";;"; }

  void start() {
    if (running) return;
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    WiFi.softAP(ssid.c_str(), password.c_str());
    dns.start(53, "*", IPAddress(192, 168, 4, 1));

    server.begin();
    running = true;
    touch();
    events.add("Phone setup Wi-Fi on");
  }

  void stop() {
    if (!running) return;
    server.stop();
    dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    running = false;
    events.add("Phone setup Wi-Fi off");
  }

  void loop() {
    if (!running) return;
    dns.processNextRequest();
    server.handleClient();
  }

private:
  WebServer server{80};
  DNSServer dns;
  bool running = false;
  uint32_t lastSeen = 0;

  void touch() { lastSeen = millis(); }

  // ---- firmware update (OTA)
  // Every Sorento Dash build contains this string (it is right here), so an
  // upload without it is some other firmware and gets rejected.
  static constexpr const char *MARKER = "SorentoDash-firmware-image";
  String otaErr;
  size_t otaTotal = 0, markerPos = 0;
  bool markerFound = false;

  void receiveOta() {
    HTTPUpload &up = server.upload();
    touch();
    if (up.status == UPLOAD_FILE_START) {
      otaErr = "";
      markerPos = 0;
      markerFound = false;
      otaTotal = server.clientContentLength();
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) otaErr = Update.errorString();
    } else if (up.status == UPLOAD_FILE_WRITE) {
      if (otaErr.length()) return;
      if (up.totalSize == 0 && up.currentSize && up.buf[0] != 0xE9) {
        otaErr = "That file isn't an ESP32 firmware image.";
        Update.abort();
        return;
      }
      for (size_t i = 0; i < up.currentSize && !markerFound; i++) {
        char c = up.buf[i];
        markerPos = c == MARKER[markerPos] ? markerPos + 1 : (c == MARKER[0] ? 1 : 0);
        if (!MARKER[markerPos]) markerFound = true;
      }
      if (Update.write(up.buf, up.currentSize) != up.currentSize) otaErr = Update.errorString();
      webOtaProgress(up.totalSize + up.currentSize, otaTotal);
    } else if (up.status == UPLOAD_FILE_END) {
      if (otaErr.length()) return;
      if (!markerFound) {
        otaErr = "That file isn't Sorento Dash firmware.";
        Update.abort();
      } else if (!Update.end(true)) {
        otaErr = Update.errorString();
      }
    } else if (up.status == UPLOAD_FILE_ABORTED) {
      Update.abort();
      otaErr = "Upload interrupted.";
    }
  }

  void finishOta() {
    bool ok = otaErr.isEmpty() && Update.isFinished();
    if (!ok && otaErr.isEmpty()) otaErr = "Update failed.";
    if (!ok) events.add("Firmware update failed: %s", otaErr.c_str());
    server.sendHeader("Connection", "close");
    server.send(ok ? 200 : 400, "text/plain", ok ? "OK" : otaErr);
    webOtaFinished(ok);
    if (ok) {
      delay(500);
      ESP.restart();
    }
  }
};
