// "Driving stats" phone page: charts drawn as inline SVG (the phone is on the
// display's own Wi-Fi with no internet, so no chart libraries).
// Data comes from /api/history (see History.h).
#pragma once
#include <Arduino.h>

static const char STATS_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Driving stats</title>
<style>
:root{color-scheme:dark;--bg:#0b0c0e;--card:#16181c;--line:#26292f;--axis:#383835;--text:#f3f4f6;--dim:#8b919a;--blue:#3987e5;--orange:#d95926}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:16px/1.4 -apple-system,system-ui,Segoe UI,Roboto,sans-serif;padding:16px;max-width:640px;margin:auto}
a{color:var(--dim);text-decoration:none;font-size:14px}
h1{font-size:22px;margin:6px 0 2px}
.sub{color:var(--dim);font-size:14px;margin-bottom:16px}
h2{font-size:16px;margin:0}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px;margin-bottom:14px}
.card p{color:var(--dim);font-size:13px;margin:2px 0 10px}
.tiles{display:grid;grid-template-columns:repeat(2,1fr);gap:10px;margin-bottom:14px}
.tile{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:12px 14px}
.tile small{display:block;color:var(--dim);font-size:13px}
.tile b{font-size:26px;font-weight:650}
.tile span{color:var(--dim);font-size:13px}
.seg{display:flex;gap:6px;margin:0 0 14px}
.seg button{flex:1;background:#23262c;color:var(--text);border:0;border-radius:10px;height:36px;font:inherit;font-size:14px}
.seg button.on{background:var(--text);color:#111;font-weight:600}
.chart{position:relative;touch-action:pan-y}
svg{display:block;width:100%;overflow:visible}
.tip{position:absolute;pointer-events:none;background:#23262c;border:1px solid var(--line);border-radius:10px;padding:8px 10px;font-size:13px;white-space:nowrap;opacity:0;transition:opacity .1s;z-index:2}
.tip b{font-size:15px}
.tip .k{display:inline-block;width:12px;height:2px;margin-right:6px;vertical-align:middle}
.tip div{color:var(--dim)}
.legend{display:flex;gap:16px;font-size:13px;color:var(--dim);margin-bottom:6px}
.legend i{display:inline-block;width:14px;height:2px;margin-right:6px;vertical-align:middle}
details{margin-top:8px}
summary{color:var(--dim);font-size:13px;cursor:pointer}
table{width:100%;border-collapse:collapse;font-size:13px;margin-top:6px;font-variant-numeric:tabular-nums}
td,th{text-align:right;padding:4px 6px;border-bottom:1px solid var(--line)}
th{color:var(--dim);font-weight:500}
td:first-child,th:first-child{text-align:left}
.empty{color:var(--dim);font-size:14px;padding:18px 0;text-align:center}
.danger{background:#23262c;color:#f87171;border:0;border-radius:10px;height:38px;padding:0 14px;font:inherit;font-size:14px}
button.arm{background:#7f1d1d!important;color:#fff!important}
[hidden]{display:none!important}
</style></head><body>
<a href="/">&larr; Sorento Dash</a>
<h1>Driving stats</h1>
<div class="sub" id="sub">Loading...</div>
<div class="tiles" id="tiles"></div>
<div class="seg" id="seg"><button data-n="10">Last 10 drives</button><button data-n="30" class="on">Last 30</button><button data-n="0">All</button></div>
<div class="card"><h2>MPG per drive</h2><p id="mpgNote">Each bar is one drive; the line is the average for the drives shown.</p><div class="chart" id="cMpg"></div><details><summary>Data table</summary><div id="tMpg"></div></details></div>
<div class="card"><h2>Miles per drive</h2><p>Distance of each drive.</p><div class="chart" id="cMiles"></div><details><summary>Data table</summary><div id="tMiles"></div></details></div>
<div class="card"><h2>MPG trend</h2><p>Average of the last 5 drives, by odometer. Smooths out short trips so you can see whether economy is improving or slipping.</p><div class="chart" id="cTrend"></div><details><summary>Data table</summary><div id="tTrend"></div></details></div>
<div class="card"><h2>MPG at the pump</h2><p>Real MPG from full fill-ups, next to what the display estimated for the same tank. The closer the lines, the better calibrated it is.</p><div class="chart" id="cPump"></div><details><summary>Data table</summary><div id="tPump"></div></details></div>
<div class="card"><h2>MPG by speed</h2><p>Lifetime fuel economy in each speed range. Tap a bar for miles and time spent there.</p><div class="chart" id="cSpeed"></div><details><summary>Data table</summary><div id="tSpeed"></div></details></div>
<div style="text-align:right;margin:20px 0"><button class="danger" id="clear">Clear all history</button></div>
<script>
const C={blue:'#3987e5',orange:'#d95926',text:'#f3f4f6',dim:'#8b919a',grid:'#26292f',axis:'#383835',surface:'#16181c'};
const $=id=>document.getElementById(id);
// Two-tap confirm. Phones open this page in a captive-portal window that
// silently blocks confirm() dialogs, so the button itself asks instead.
function sure(b,fn,ask){if(b.dataset.armed){clearTimeout(b._t);delete b.dataset.armed;b.classList.remove('arm');b.textContent=b._orig;fn();return}
 b._orig=b.textContent;b.dataset.armed=1;b.classList.add('arm');b.textContent=ask||'Tap again';
 b._t=setTimeout(()=>{delete b.dataset.armed;b.classList.remove('arm');b.textContent=b._orig},3000)}
let H=null,N=30;
const NS='http://www.w3.org/2000/svg';
function el(tag,attrs,parent){const e=document.createElementNS(NS,tag);for(const k in attrs)e.setAttribute(k,attrs[k]);if(parent)parent.appendChild(e);return e}
function txt(parent,x,y,s,attrs){const t=el('text',Object.assign({x,y,fill:C.dim,'font-size':11},attrs||{}),parent);t.textContent=s;return t}
function fmt(v,d){return v==null||!isFinite(v)?'-':v.toLocaleString(undefined,{minimumFractionDigits:d,maximumFractionDigits:d})}
function dur(s){const h=Math.floor(s/3600),m=Math.round(s%3600/60);return h?h+' h '+m+' min':m+' min'}
function niceStep(v){if(!(v>0))return 1;const p=Math.pow(10,Math.floor(Math.log10(v))),n=v/p;return (n<=1?1:n<=2?2:n<=2.5?2.5:n<=5?5:10)*p}
// Round axis: ~4 intervals of a clean step (1, 2, 2.5, 5 x 10^n) covering [lo, hi].
function axis(lo,hi){const step=niceStep((hi-lo)/4||1),a=Math.floor(lo/step)*step,b=Math.max(a+step,Math.ceil(hi/step)*step),t=[];for(let v=a;v<=b+step/2;v+=step)t.push(+v.toFixed(6));return{lo:a,hi:b,ticks:t,step}}

// One tooltip per chart; values lead, labels follow. Built with textContent.
function tipFor(box){let t=box.querySelector('.tip');if(!t){t=document.createElement('div');t.className='tip';box.appendChild(t)}return t}
function showTip(box,x,y,rows){const t=tipFor(box);t.replaceChildren();rows.forEach((r,i)=>{const d=document.createElement('div');
  if(r.key){const k=document.createElement('span');k.className='k';k.style.background=r.key;d.appendChild(k)}
  if(i===0||r.strong){const b=document.createElement('b');b.textContent=r.v;d.appendChild(b);if(r.l)d.appendChild(document.createTextNode(' '+r.l))}
  else d.textContent=r.v+(r.l?' '+r.l:'');t.appendChild(d)});
  t.style.opacity=1;const w=box.clientWidth,tw=t.offsetWidth;t.style.left=Math.max(0,Math.min(w-tw,x-tw/2))+'px';t.style.top=Math.max(0,y-t.offsetHeight-10)+'px'}
function hideTip(box){const t=box.querySelector('.tip');if(t)t.style.opacity=0}

// Columns: 4px rounded data-end, square at the baseline, <=24px wide, 2px gap.
function columns(box,items,o){
  box.replaceChildren();if(!items.length){const e=document.createElement('div');e.className='empty';e.textContent=o.empty;box.appendChild(e);return}
  const W=box.clientWidth||320,Hh=180,L=38,R=o.refLabel?44:8,T=12,B=24,pw=W-L-R,ph=Hh-T-B;
  const ax=axis(0,Math.max(...items.map(d=>d.v),o.ref||0)),max=ax.hi;const y=v=>T+ph-v/max*ph;
  const svg=el('svg',{viewBox:`0 0 ${W} ${Hh}`,height:Hh,role:'img','aria-label':o.label},box);
  ax.ticks.forEach((v,i)=>{const yy=y(v);el('line',{x1:L,x2:L+pw,y1:yy,y2:yy,stroke:i?C.grid:C.axis,'stroke-width':1},svg);txt(svg,L-6,yy+4,fmt(v,ax.step<1?1:0),{'text-anchor':'end'})});
  const band=pw/items.length,bw=Math.max(2,Math.min(24,band-2));
  const lw=Math.max(...items.map(d=>d.x.length))*6.5+10,every=Math.max(1,Math.ceil(lw/band));
  items.forEach((d,i)=>{const cx=L+band*i+band/2,x0=cx-bw/2,top=y(d.v),h=T+ph-top,r=Math.min(4,bw/2,h);
    const path=h>0?`M${x0},${T+ph}V${top+r}Q${x0},${top} ${x0+r},${top}H${x0+bw-r}Q${x0+bw},${top} ${x0+bw},${top+r}V${T+ph}Z`:'';
    const bar=el('path',{d:path,fill:d.muted?C.axis:o.color},svg);
    if(i%every===0)txt(svg,cx,Hh-6,d.x,{'text-anchor':'middle'});
    const hit=el('rect',{x:L+band*i,y:T,width:band,height:ph,fill:'transparent',tabindex:0},svg);
    const on=()=>{bar.setAttribute('opacity',.75);showTip(box,cx,top,d.tip)},off=()=>{bar.removeAttribute('opacity');hideTip(box)};
    hit.addEventListener('pointerenter',on);hit.addEventListener('pointerdown',on);hit.addEventListener('focus',on);
    hit.addEventListener('pointerleave',off);hit.addEventListener('blur',off)});
  if(o.ref){const yy=y(o.ref);el('line',{x1:L,x2:L+pw,y1:yy,y2:yy,stroke:C.text,'stroke-width':1,opacity:.7},svg);txt(svg,L+pw+4,yy+4,o.refLabel,{fill:C.text})}
}

// Lines with >=8px markers and a 2px surface ring; crosshair snaps to the nearest X.
function lines(box,xs,series,o){
  box.replaceChildren();if(xs.length<2){const e=document.createElement('div');e.className='empty';e.textContent=o.empty;box.appendChild(e);return}
  if(series.length>1){const lg=document.createElement('div');lg.className='legend';series.forEach(s=>{const sp=document.createElement('span'),i=document.createElement('i');i.style.background=s.color;sp.append(i,s.name);lg.appendChild(sp)});box.appendChild(lg)}
  const W=box.clientWidth||320,Hh=180,L=38,R=series.length>1?56:12,T=12,B=24,pw=W-L-R,ph=Hh-T-B;
  const all=series.flatMap(s=>s.v).filter(v=>v!=null),ax=axis(Math.max(0,Math.min(...all)-2),Math.max(...all)+1),lo=ax.lo,hi=ax.hi;
  const y=v=>T+ph-(v-lo)/(hi-lo)*ph,x=i=>L+(xs.length<2?pw/2:pw*i/(xs.length-1));
  const svg=el('svg',{viewBox:`0 0 ${W} ${Hh}`,height:Hh,role:'img','aria-label':o.label},box);
  ax.ticks.forEach((v,i)=>{const yy=y(v);el('line',{x1:L,x2:L+pw,y1:yy,y2:yy,stroke:i?C.grid:C.axis,'stroke-width':1},svg);txt(svg,L-6,yy+4,fmt(v,ax.step<1?1:0),{'text-anchor':'end'})});
  const every=Math.ceil(xs.length/5);xs.forEach((s,i)=>{if(i%every===0)txt(svg,x(i),Hh-6,s,{'text-anchor':'middle'})});
  const cross=el('line',{y1:T,y2:T+ph,stroke:C.dim,'stroke-width':1,opacity:0},svg);
  series.forEach(s=>{let d='';s.v.forEach((v,i)=>{if(v!=null)d+=(d?'L':'M')+x(i)+','+y(v)});
    el('path',{d,fill:'none',stroke:s.color,'stroke-width':2,'stroke-linejoin':'round','stroke-linecap':'round'},svg);
    s.v.forEach((v,i)=>{if(v!=null&&(xs.length<=40||i===xs.length-1))el('circle',{cx:x(i),cy:y(v),r:4,fill:s.color,stroke:C.surface,'stroke-width':2},svg)});
  });
  // Direct end labels only when they don't collide; the legend always carries identity.
  const ends=series.map(s=>s.v[s.v.length-1]).map(v=>v==null?null:y(v));
  if(series.length>1&&ends.every((a,i)=>ends.every((b,j)=>i===j||a==null||b==null||Math.abs(a-b)>=14)))
    series.forEach((s,i)=>{if(ends[i]!=null)txt(svg,x(xs.length-1)+8,ends[i]+4,s.name,{fill:C.text})});
  const hit=el('rect',{x:L,y:T,width:pw,height:ph,fill:'transparent'},svg);
  const move=e=>{const r=svg.getBoundingClientRect(),px=(e.clientX-r.left)*W/r.width;const i=Math.max(0,Math.min(xs.length-1,Math.round((px-L)/pw*(xs.length-1))));
    cross.setAttribute('x1',x(i));cross.setAttribute('x2',x(i));cross.setAttribute('opacity',1);
    showTip(box,x(i)*r.width/W,T,o.tip?o.tip(i):[{v:xs[i]},...series.map(s=>({key:s.color,v:fmt(s.v[i],1),l:s.name+' MPG',strong:true}))])};
  hit.addEventListener('pointermove',move);hit.addEventListener('pointerdown',move);
  hit.addEventListener('pointerleave',()=>{cross.setAttribute('opacity',0);hideTip(box)});
}

function table(target,head,rows){const t=document.createElement('table'),tr=document.createElement('tr');
  head.forEach(h=>{const th=document.createElement('th');th.textContent=h;tr.appendChild(th)});t.appendChild(tr);
  rows.forEach(r=>{const row=document.createElement('tr');r.forEach(c=>{const td=document.createElement('td');td.textContent=c;row.appendChild(td)});t.appendChild(row)});
  target.replaceChildren(t)}

function tile(label,value,note){const d=document.createElement('div');d.className='tile';const s=document.createElement('small');s.textContent=label;
  const b=document.createElement('b');b.textContent=value;d.append(s,b);if(note){const n=document.createElement('span');n.textContent=' '+note;d.appendChild(n)}return d}

function render(){
  if(!H)return;
  const drives=H.drives.map((a,i)=>({n:i+1,t:a[0],odo:a[1],mi:a[2],gal:a[3],sec:a[4],idle:a[5],idleGal:a[6],max:a[7]}));
  drives.forEach(d=>d.mpg=d.gal>0.05&&d.mi>=0.5?d.mi/d.gal:null);
  const label=d=>'#'+d.n;
  const odoOff=H.odoOffset||0,odoName=H.odoOffset!=null?'odometer':'tracked miles';
  // Lifetime tiles
  const mi=drives.reduce((a,d)=>a+d.mi,0),gal=drives.reduce((a,d)=>a+d.gal,0),b=H.bands;
  const good=drives.filter(d=>d.mpg!=null&&d.mi>=5);const best=good.length?Math.max(...good.map(d=>d.mpg)):null;
  $('tiles').replaceChildren(tile('Average MPG',gal>0.1?fmt(mi/gal,1):'-'),tile('Miles tracked',fmt(mi,0)),
    tile('Drives',fmt(drives.length,0),drives.length?'avg '+fmt(mi/drives.length,1)+' mi':''),tile('Best drive',best?fmt(best,1):'-',best?'MPG (5+ mi)':''),
    tile('Fuel used',fmt(gal,1),'gal'),tile('Idling',fmt(b.idleSec/3600,1)+' h',fmt(b.idleGal,2)+' gal'));
  $('sub').textContent=!drives.length?'No drives recorded yet. They appear after your next drive with the OBD adapter connected.':
    drives.length+' drives over '+fmt(mi,0)+' miles'+(H.storage?'':' - storage unavailable, history is not being saved');
  // Drive charts, scoped by the segmented control
  const shown=N?drives.slice(-N):drives,withMpg=shown.filter(d=>d.mpg!=null);
  const sMi=withMpg.reduce((a,d)=>a+d.mi,0),sGal=withMpg.reduce((a,d)=>a+d.gal,0),avg=sGal>0?sMi/sGal:0;
  const dtip=d=>[{v:fmt(d.mpg,1),l:'MPG'},{v:'Drive '+label(d)+' at '+fmt(d.odo+odoOff,0)+' mi'},{v:fmt(d.mi,1)+' mi in '+dur(d.sec)},{v:'idle '+Math.round(d.idle/Math.max(1,d.sec)*100)+'%, top '+d.max+' mph'}];
  columns($('cMpg'),withMpg.map(d=>({x:label(d),v:d.mpg,tip:dtip(d)})),{color:C.blue,label:'MPG per drive',empty:'No drives yet',ref:avg,refLabel:'avg '+fmt(avg,1)});
  table($('tMpg'),['Drive','Miles','Gallons','MPG','Time'],withMpg.slice().reverse().map(d=>[label(d),fmt(d.mi,1),fmt(d.gal,2),fmt(d.mpg,1),dur(d.sec)]));
  columns($('cMiles'),shown.map(d=>({x:label(d),v:d.mi,tip:[{v:fmt(d.mi,1),l:'miles'},{v:label(d)},{v:dur(d.sec)+(d.mpg?', '+fmt(d.mpg,1)+' MPG':'')}]})),{color:C.blue,label:'Miles per drive',empty:'No drives yet'});
  table($('tMiles'),['Drive','Miles','Time','Top mph'],shown.slice().reverse().map(d=>[label(d),fmt(d.mi,1),dur(d.sec),d.max]));
  // MPG trend: fuel-weighted average of the last 5 drives, placed at each drive's odometer
  const trend=withMpg.map((d,i)=>{const w=withMpg.slice(Math.max(0,i-4),i+1),m=w.reduce((a,x)=>a+x.mi,0),g=w.reduce((a,x)=>a+x.gal,0);return{d,v:m/g}});
  lines($('cTrend'),trend.map(p=>fmt(p.d.odo+odoOff,0)),[{name:'5-drive average',color:C.blue,v:trend.map(p=>p.v)}],{label:'MPG trend by '+odoName,empty:'Needs at least two drives.',
    tip:i=>[{v:fmt(trend[i].v,1),l:'MPG (5-drive avg)'},{v:'at '+fmt(trend[i].d.odo+odoOff,0)+' mi, drive '+label(trend[i].d)},{v:'this drive '+fmt(trend[i].d.mpg,1)+' MPG'}]});
  table($('tTrend'),['Drive','At (mi)','5-drive MPG','Drive MPG'],trend.slice().reverse().map(p=>[label(p.d),fmt(p.d.odo+odoOff,0),fmt(p.v,1),fmt(p.d.mpg,1)]));
  // Pump vs estimate (full, complete tanks only)
  const fills=H.fills.map(a=>({t:a[0],odo:a[1],pumped:a[2],est:a[3],mi:a[4],valid:a[5]})).filter(f=>f.valid&&f.pumped>1&&f.est>0.5);
  lines($('cPump'),fills.map((f,i)=>'Fill '+(i+1)),[{name:'Pump',color:C.blue,v:fills.map(f=>f.mi/f.pumped)},{name:'Display',color:C.orange,v:fills.map(f=>f.mi/f.est)}],{label:'MPG at the pump vs display estimate',empty:'Log two full fill-ups (after the first one) to see this.'});
  table($('tPump'),['Fill-up','Miles','Pumped','Pump MPG','Display MPG'],fills.slice().reverse().map((f,i)=>['Fill '+(fills.length-i),fmt(f.mi,0),fmt(f.pumped,2),fmt(f.mi/f.pumped,1),fmt(f.mi/f.est,1)]));
  // By speed band
  const totalSec=b.sec.reduce((a,v)=>a+v,0)+b.idleSec;
  const last=b.min.length-1;
  const bands=b.min.map((m,i)=>({name:i<last?m+'-'+(b.min[i+1]-1):m+'+',short:i===0?'<'+b.min[1]:i<last?String(m):m+'+',mi:b.mi[i],gal:b.gal[i],sec:b.sec[i]})).filter(x=>x.mi>=0.5&&x.gal>0.01);
  columns($('cSpeed'),bands.map(x=>({x:x.short,v:x.mi/x.gal,tip:[{v:fmt(x.mi/x.gal,1),l:'MPG'},{v:x.name+' mph'},{v:fmt(x.mi,0)+' mi, '+Math.round(x.sec/Math.max(1,totalSec)*100)+'% of driving time'}]})),{color:C.blue,label:'MPG by speed',empty:'Not enough driving yet'});
  table($('tSpeed'),['Speed (mph)','Miles','MPG','Time'],bands.map(x=>[x.name,fmt(x.mi,0),fmt(x.mi/x.gal,1),Math.round(x.sec/Math.max(1,totalSec)*100)+'%']).concat([['Idle','0',b.idleGal>0?fmt(b.idleGal,2)+' gal':'-',Math.round(b.idleSec/Math.max(1,totalSec)*100)+'%']]));
}
$('seg').addEventListener('click',e=>{const b=e.target.closest('button');if(!b)return;N=+b.dataset.n;[...$('seg').children].forEach(x=>x.classList.toggle('on',x===b));render()});
$('clear').onclick=e=>sure(e.target,()=>fetch('/api/history/clear',{method:'POST'}).then(load),'Tap again to delete everything');
let lastW=0;window.addEventListener('resize',()=>{if(Math.abs(innerWidth-lastW)>20){lastW=innerWidth;render()}});
function load(){fetch('/api/history').then(r=>r.json()).then(h=>{H=h;lastW=innerWidth;render()}).catch(()=>setTimeout(load,1500))}
load();
</script></body></html>)HTML";
