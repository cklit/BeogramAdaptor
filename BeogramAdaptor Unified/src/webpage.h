#pragma once
#include <Arduino.h>

// The embedded configuration page (HTML + CSS + JS), served at "/".
// UI-only — no firmware logic belongs in this file.

static const char* htmlPage PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Beogram Adaptor</title>
  <link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/@tabler/icons-webfont@latest/tabler-icons.min.css">
  <style>
    *{box-sizing:border-box;margin:0;padding:0}
    body{font-family:system-ui,sans-serif;background:#f0f0f0;padding:1.5rem 1rem;color:#111}
    @media(prefers-color-scheme:dark){body{background:#1a1a1a;color:#eee}}
    .page{max-width:560px;margin:0 auto;display:flex;flex-direction:column;gap:1rem}
    .page-title{display:flex;align-items:center;gap:10px;padding:.25rem 0 .5rem}
    .page-title i{font-size:22px;color:#666}
    .page-title h1{font-size:18px;font-weight:500}
    .card{background:#fff;border:1px solid #e0e0e0;border-radius:12px;padding:1.25rem 1.5rem}
    @media(prefers-color-scheme:dark){.card{background:#252525;border-color:#333}}
    .card-header{display:flex;align-items:center;gap:10px;margin-bottom:1rem}
    .card-header i{font-size:18px;color:#888}
    .card-header h2{font-size:15px;font-weight:500}
    .status-row{display:flex;align-items:center;justify-content:space-between;margin-bottom:.65rem}
    .status-label{font-size:13px;color:#666}
    @media(prefers-color-scheme:dark){.status-label{color:#aaa}}
    .badge{display:inline-flex;align-items:center;gap:5px;padding:3px 10px;border-radius:20px;font-size:12px;font-weight:500}
    .badge.connected{background:#e1f5ee;color:#0f6e56}
    .badge.disconnected{background:#f0f0f0;color:#888}
    @media(prefers-color-scheme:dark){.badge.disconnected{background:#333;color:#aaa}}
    .badge i{font-size:10px}
    .ip-chip{font-size:12px;font-family:monospace;color:#666;background:#f5f5f5;padding:2px 8px;border-radius:4px}
    @media(prefers-color-scheme:dark){.ip-chip{background:#333;color:#bbb}}
    .divider{height:1px;background:#f0f0f0;margin:.75rem 0}
    @media(prefers-color-scheme:dark){.divider{background:#333}}
    .form-group{display:flex;flex-direction:column;gap:6px;margin-top:.75rem}
    .form-group label{font-size:13px;color:#666}
    @media(prefers-color-scheme:dark){.form-group label{color:#aaa}}
    .input-row{display:flex;gap:8px}
    .hdr-btn{margin-left:auto;background:none;border:none;color:#999;cursor:pointer;padding:4px 6px;border-radius:6px;font-size:16px}
    .hdr-btn:hover{color:#333;background:#f0f0f0}
    @media(prefers-color-scheme:dark){.hdr-btn{color:#888}.hdr-btn:hover{color:#ddd;background:#333}}
    .input-row input{flex:1;font-size:13px;padding:0 10px;height:36px;border:1px solid #ddd;border-radius:8px;background:#fff;color:#111}
    @media(prefers-color-scheme:dark){.input-row input{background:#1a1a1a;border-color:#444;color:#eee}}
    .btn{height:36px;padding:0 14px;font-size:13px;border:1px solid #ddd;border-radius:8px;background:#fff;color:#111;cursor:pointer;white-space:nowrap}
    .btn:hover{background:#f5f5f5}
    @media(prefers-color-scheme:dark){.btn{background:#2a2a2a;border-color:#444;color:#eee}.btn:hover{background:#333}}
    .btn-highlight{background:#1D9E75;border-color:#1D9E75;color:#fff}
    .btn-highlight:hover{background:#178a65}
    @media(prefers-color-scheme:dark){.btn-highlight{background:#1D9E75;border-color:#1D9E75;color:#fff}.btn-highlight:hover{background:#178a65}}
    .btn-danger{border-color:#f09595;color:#a32d2d}
    .btn-danger:hover{background:#fcebeb}
    @media(prefers-color-scheme:dark){.btn-danger{border-color:#793333;color:#f09595}.btn-danger:hover{background:#2a1a1a}}
    .select-row{display:flex;align-items:center;justify-content:space-between;gap:1rem}
    .select-row label{font-size:13px;color:#666}
    @media(prefers-color-scheme:dark){.select-row label{color:#aaa}}
    select{height:34px;padding:0 10px;font-size:13px;border:1px solid #ddd;border-radius:8px;background:#fff;color:#111}
    @media(prefers-color-scheme:dark){select{background:#1a1a1a;border-color:#444;color:#eee}}
    .seg{display:flex;border:1px solid #ddd;border-radius:8px;overflow:hidden}
    @media(prefers-color-scheme:dark){.seg{border-color:#444}}
    .seg button{flex:1;height:34px;padding:0 14px;font-size:13px;border:none;background:#fff;color:#666;cursor:pointer}
    @media(prefers-color-scheme:dark){.seg button{background:#1a1a1a;color:#aaa}}
    .seg button.active{background:#1D9E75;color:#fff}
    .toggle-row{display:flex;align-items:center;justify-content:space-between;gap:1rem;margin-top:.5rem}
    .toggle-row span{font-size:13px;color:#666}
    @media(prefers-color-scheme:dark){.toggle-row span{color:#aaa}}
    .toggle{position:relative;width:40px;height:22px;flex-shrink:0}
    .toggle input{opacity:0;width:0;height:0}
    .toggle-slider{position:absolute;inset:0;background:#ccc;border-radius:11px;cursor:pointer;transition:background .2s}
    .toggle input:checked+.toggle-slider{background:#1D9E75}
    .toggle-slider:before{content:'';position:absolute;width:16px;height:16px;left:3px;top:3px;background:#fff;border-radius:50%;transition:transform .2s}
    .toggle input:checked+.toggle-slider:before{transform:translateX(18px)}
    .fw-row{display:flex;align-items:center;justify-content:space-between}
    .fw-row span{font-size:13px;color:#666}
    @media(prefers-color-scheme:dark){.fw-row span{color:#aaa}}
    .fw-val{font-size:12px;font-family:monospace;color:#666;background:#f5f5f5;padding:2px 8px;border-radius:4px}
    @media(prefers-color-scheme:dark){.fw-val{background:#333;color:#bbb}}
    .file-row{display:flex;align-items:center;gap:8px;margin-top:.75rem}
    .file-row input[type=file]{font-size:12px;color:#666;flex:1}
    .link-row{display:flex;align-items:center;gap:8px;font-size:13px;color:#666}
    .link-row i{font-size:16px}
    .link-row a{color:#185fa5;text-decoration:none}
    .link-row a:hover{text-decoration:underline}
    @media(prefers-color-scheme:dark){.link-row a{color:#85b7eb}}
    .error-text{font-size:12px;color:#a32d2d;display:none}
    .info-text{display:none;font-size:12px;color:#888;margin-top:6px}
    .action-row{display:flex;gap:8px;margin-top:.75rem;flex-wrap:wrap}
    .hint{font-size:12px;color:#888;margin-top:.5rem}
  </style>
</head>
<body>
<div class="page">
  <div class="page-title">
    <i class="ti ti-disc"></i>
    <h1>Beogram Adaptor</h1>
  </div>

  <div class="card">
    <div class="card-header"><i class="ti ti-disc"></i><h2>Beogram</h2><button class="hdr-btn" id="bg-standby" title="Beogram standby"><i class="ti ti-power"></i></button></div>
    <div class="status-row">
      <span class="status-label">State</span>
      <span class="badge disconnected" id="bg-state"><i class="ti ti-circle"></i>Unknown</span>
    </div>
    <div class="status-row">
      <span class="status-label">Track</span>
      <span class="ip-chip" id="bg-track">-</span>
    </div>
    <div class="input-row" style="margin-top:8px;justify-content:center">
      <button class="btn" id="bg-prev" title="Previous track"><i class="ti ti-player-skip-back"></i></button>
      <button class="btn" id="bg-playpause" title="Play"><i class="ti ti-player-play" id="bg-playpause-icon"></i></button>
      <button class="btn" id="bg-next" title="Next track"><i class="ti ti-player-skip-forward"></i></button>
    </div>
  </div>

  <div class="card">
    <div class="card-header"><i class="ti ti-device-speaker"></i><h2 id="product-platform-label">Product</h2></div>
    <div class="form-group" id="product-connect-form" style="margin-top:0;margin-bottom:1.25rem">
      <label for="discover-results">Product</label>
      <select id="discover-results" style="width:100%">
        <option value="">Select a product&hellip;</option>
        <option value="__manual__">Enter IP address manually&hellip;</option>
      </select>
      <div class="input-row" style="margin-top:8px">
        <button class="btn" id="product-scan-btn"><i class="ti ti-radar-2"></i>&nbsp;Start product scan</button>
      </div>
      <span class="info-text" id="scan-note" style="display:none">Scanning the network &mdash; this takes around 10 seconds&hellip;</span>
      <div id="manual-ip-row" style="display:none;margin-top:8px">
        <div class="seg" id="manualPlatformSeg" style="margin-bottom:8px">
          <button data-platform="ase" id="mseg-ase">ASE</button>
          <button data-platform="mozart" id="mseg-mozart">Mozart</button>
        </div>
        <div class="input-row">
          <input type="text" id="productIP" placeholder="e.g. 192.168.1.42">
        </div>
      </div>
      <span class="error-text" id="productIP-error">Invalid IP address</span>
      <span class="error-text" id="discover-error">No products found on the network</span>
      <div class="input-row" style="margin-top:8px">
        <button class="btn" id="product-connect-btn">Connect</button>
      </div>
    </div>
    <div class="status-row" id="product-serial-row" style="display:none">
      <span class="status-label">Serial number</span>
      <span class="ip-chip" id="product-serial"></span>
    </div>
    <div class="status-row">
      <span class="status-label">IP address</span>
      <span class="ip-chip" id="product-ip">—</span>
    </div>
    <div class="status-row">
      <span class="status-label">Connection</span>
      <span class="badge disconnected" id="product-status"><i class="ti ti-circle"></i>Disconnected</span>
    </div>
    <div class="divider"></div>
    <div class="select-row" style="margin-bottom:0">
      <label for="sourceSelect">Input source</label>
      <select id="sourceSelect"></select>
    </div>
    <div class="action-row" id="product-action-row" style="display:none">
      <button class="btn btn-danger" id="product-unlink-btn">Unlink product</button>
    </div>
  </div>

  <div class="card">
    <div class="card-header"><i class="ti ti-remote"></i><h2>Beoremote Halo</h2></div>
    <div class="form-group" id="halo-connect-form" style="margin-top:0;margin-bottom:1.25rem">
      <label for="halo-discover-results">Halo</label>
      <select id="halo-discover-results" style="width:100%">
        <option value="">Select a Halo&hellip;</option>
        <option value="__manual__">Enter IP address manually&hellip;</option>
      </select>
      <div class="input-row" style="margin-top:8px">
        <button class="btn" id="halo-scan-btn"><i class="ti ti-radar-2"></i>&nbsp;Start Halo scan</button>
      </div>
      <span class="info-text" id="halo-scan-note" style="display:none">Scanning the network &mdash; this takes around 5 seconds&hellip;</span>
      <div class="input-row" id="halo-manual-ip-row" style="display:none;margin-top:8px">
        <input type="text" id="haloIP" placeholder="e.g. 192.168.1.55">
      </div>
      <span class="error-text" id="haloIP-error">Invalid IP address</span>
      <span class="error-text" id="halo-discover-error">No Halos found on the network</span>
      <div class="input-row" style="margin-top:8px">
        <button class="btn" id="halo-connect-btn">Connect</button>
      </div>
    </div>
    <div class="status-row" id="halo-serial-row" style="display:none">
      <span class="status-label">Serial number</span>
      <span class="ip-chip" id="halo-serial"></span>
    </div>
    <div class="status-row">
      <span class="status-label">IP address</span>
      <span class="ip-chip" id="halo-ip">—</span>
    </div>
    <div class="status-row" style="margin-bottom:0">
      <span class="status-label">Connection</span>
      <span class="badge disconnected" id="halo-ws-status"><i class="ti ti-circle"></i>Disconnected</span>
    </div>
    <div class="divider"></div>
    <div class="toggle-row">
      <span>Activate controls when Halo wakes up</span>
      <label class="toggle">
        <input type="checkbox" id="featureToggle">
        <span class="toggle-slider"></span>
      </label>
    </div>
    <div class="action-row" id="halo-action-row" style="display:none">
      <button class="btn btn-danger" id="halo-unlink-btn">Unlink Halo</button>
    </div>
  </div>

  <div class="card">
    <div class="card-header"><i class="ti ti-smart-home"></i><h2>Home Assistant</h2></div>
    <div class="status-row" style="margin-bottom:0">
      <span class="status-label">MQTT</span>
      <span class="badge disconnected" id="mqtt-status"><i class="ti ti-circle"></i>Disconnected</span>
    </div>
    <div class="action-row">
      <a href="/mqtt"><button class="btn">Configure MQTT</button></a>
    </div>
  </div>

  <div class="card">
    <div class="card-header"><i class="ti ti-upload"></i><h2>Firmware update</h2></div>
    <div class="fw-row">
      <span>Current version</span>
      <span class="fw-val" id="fw-version">Loading...</span>
    </div>
    <form method="POST" action="/update-ota" enctype="multipart/form-data">
      <div class="file-row">
        <input type="file" name="update" accept=".bin">
        <button type="submit" class="btn">Upload</button>
      </div>
    </form>
  </div>

  <div class="card">
    <div class="link-row">
      <i class="ti ti-brand-github"></i>
      <a href="https://github.com/cklit/BeogramAdaptor" target="_blank">View on GitHub</a>
    </div>
  </div>
</div>

<script>
const PLATFORM_LABELS={ase:'ASE platform product',mozart:'Mozart platform product'};
const SOURCE_OPTIONS={
  ase:[['LINE IN','Line-In (default)'],['TOSLINK','Optical']],
  mozart:[['lineIn','Line-In (default)'],['spdif','Optical']]
};
let currentPlatform='';

function validateIP(ip){
  let p=ip.split('.');
  if(p.length!==4)return false;
  return p.every(x=>{let n=parseInt(x,10);return n>=0&&n<=255&&x===n.toString()});
}

function setBadge(el,connected){
  el.className='badge '+(connected?'connected':'disconnected');
  el.innerHTML=connected?'<i class="ti ti-circle-filled"></i>Connected':'<i class="ti ti-circle"></i>Disconnected';
}

let manualPlatform='';
function setManualPlatform(p){
  manualPlatform=p;
  document.getElementById('mseg-ase').className=p==='ase'?'active':'';
  document.getElementById('mseg-mozart').className=p==='mozart'?'active':'';
}
document.getElementById('manualPlatformSeg').addEventListener('click',function(e){
  let btn=e.target.closest('button');
  if(btn)setManualPlatform(btn.dataset.platform);
});

function applyPlatform(p){
  if(p===currentPlatform)return;
  currentPlatform=p;
  document.getElementById('product-platform-label').textContent=PLATFORM_LABELS[p]||'Product';
  setManualPlatform(p);
  let sel=document.getElementById('sourceSelect');
  sel.innerHTML='';
  (SOURCE_OPTIONS[p]||[]).forEach(o=>{
    let e=document.createElement('option');
    e.value=o[0];e.textContent=o[1];
    sel.appendChild(e);
  });
}

let bgPlayingNow=false;
function renderBeogram(state,track,playing){
  bgPlayingNow=!!playing;
  let b=document.getElementById('bg-state');
  b.className='badge '+(playing?'connected':'disconnected');
  b.innerHTML='<i class="ti ti-circle"></i>'+state;
  document.getElementById('bg-track').textContent=track||'-';
  let pp=document.getElementById('bg-playpause');
  document.getElementById('bg-playpause-icon').className=playing?'ti ti-player-pause':'ti ti-player-play';
  pp.title=playing?'Stop':'Play';
}

let bgWs=null;
function connectBgWs(){
  bgWs=new WebSocket('ws://'+location.hostname+':81');
  bgWs.onmessage=function(e){
    try{let d=JSON.parse(e.data);renderBeogram(d.state,d.track,d.playing);}catch(err){}
  };
  bgWs.onclose=function(){setTimeout(connectBgWs,3000);};
  bgWs.onerror=function(){bgWs.close();};
}
connectBgWs();

['next','prev','standby'].forEach(function(cmd){
  document.getElementById('bg-'+cmd).addEventListener('click',function(){
    fetch('/command/'+cmd,{method:'POST'});
  });
});
document.getElementById('bg-playpause').addEventListener('click',function(){
  fetch('/command/'+(bgPlayingNow?'stop':'play'),{method:'POST'});
});

function updateStatus(){
  fetch('/status').then(r=>r.json()).then(d=>{
    applyPlatform(d.platform);
    setBadge(document.getElementById('product-status'),d.product_connected);
    setBadge(document.getElementById('halo-ws-status'),d.halo_ws_connected);
    setBadge(document.getElementById('mqtt-status'),d.mqtt_connected);
    document.getElementById('fw-version').textContent=d.firmware;
    document.getElementById('featureToggle').checked=d.feature_enabled;
    document.getElementById('sourceSelect').value=d.trigger_source;

    if(!bgWs||bgWs.readyState!==1)renderBeogram(d.beogram_state,d.beogram_track,d.beogram_playing);
    let hasProduct=d.product_ip&&d.product_ip!=='';
    document.getElementById('product-ip').textContent=hasProduct?d.product_ip:'—';
    let sn=d.product_serial||'';
    document.getElementById('product-serial-row').style.display=hasProduct?'flex':'none';
    document.getElementById('product-serial').textContent=sn||'N/A. Manually added';
    document.getElementById('product-connect-form').style.display=hasProduct?'none':'flex';
    document.getElementById('product-action-row').style.display=hasProduct?'flex':'none';

    let hasHalo=d.halo_ip&&d.halo_ip!=='';
    document.getElementById('halo-ip').textContent=hasHalo?d.halo_ip:'—';
    let hsn=d.halo_serial||'';
    document.getElementById('halo-serial-row').style.display=hasHalo?'flex':'none';
    document.getElementById('halo-serial').textContent=hsn||'N/A. Manually added';
    document.getElementById('halo-connect-form').style.display=hasHalo?'none':'flex';
    document.getElementById('halo-action-row').style.display=hasHalo?'flex':'none';
  }).catch(()=>{});
}

document.getElementById('product-connect-btn').addEventListener('click',function(){
  let sel=document.getElementById('discover-results');
  let err=document.getElementById('productIP-error');
  let ip,discovered=null,name='';
  if(sel.value==='__manual__'){
    ip=document.getElementById('productIP').value.trim();
    if(!validateIP(ip)){err.style.display='block';return;}
    discovered=manualPlatform;
  }else if(sel.value){
    ip=sel.value;
    discovered=discoveredDevices[ip];
    name=(discoveredMeta[ip]||{}).serial||'';
  }else{
    return;
  }
  err.style.display='none';
  if(discovered&&discovered!==currentPlatform){
    let label=discovered==='mozart'?'Mozart':'ASE';
    if(!confirm('This is a '+label+' product. The adaptor will switch platform and restart. Continue?'))return;
    fetch('/update-platform?platform='+discovered+'&productIP='+encodeURIComponent(ip)+'&productSerial='+encodeURIComponent(name)).catch(()=>{});
    document.getElementById('product-platform-label').textContent='Restarting\u2026';
    setTimeout(()=>location.reload(),8000);
    return;
  }
  fetch('/update?productIP='+encodeURIComponent(ip)+(name?'&productSerial='+encodeURIComponent(name):'')).then(updateStatus);
});

document.getElementById('product-unlink-btn').addEventListener('click',function(){
  fetch('/update?productIP=').then(updateStatus);
});

function refreshConnectHighlight(selId,ipId,btnId){
  let sel=document.getElementById(selId);
  let ready=false;
  if(sel.value==='__manual__')ready=validateIP(document.getElementById(ipId).value.trim());
  else if(sel.value)ready=true;
  document.getElementById(btnId).classList.toggle('btn-highlight',ready);
}
function refreshHighlights(){
  refreshConnectHighlight('discover-results','productIP','product-connect-btn');
  refreshConnectHighlight('halo-discover-results','haloIP','halo-connect-btn');
}
document.getElementById('productIP').addEventListener('input',refreshHighlights);
document.getElementById('haloIP').addEventListener('input',refreshHighlights);

let discoveredDevices={};
let discoveredMeta={};
function rebuildDiscoverOptions(devices){
  let sel=document.getElementById('discover-results');
  devices.forEach(dev=>{discoveredMeta[dev.ip]=dev;});
  let prev=sel.value;
  sel.innerHTML='<option value="">Select a product\u2026</option>';
  discoveredDevices={};
  Object.values(discoveredMeta).forEach(dev=>{
    discoveredDevices[dev.ip]=dev.platform;
    let o=document.createElement('option');
    o.value=dev.ip;
    o.textContent=dev.name+' ('+dev.ip+') \u2014 '+(dev.platform==='mozart'?'Mozart':'ASE');
    sel.appendChild(o);
  });
  let m=document.createElement('option');
  m.value='__manual__';m.textContent='Enter IP address manually\u2026';
  sel.appendChild(m);
  if(prev&&(prev==='__manual__'||discoveredDevices[prev]))sel.value=prev;
  else if(Object.keys(discoveredDevices).length===1)sel.selectedIndex=1;
  sel.dispatchEvent(new Event('change'));
}

document.getElementById('product-scan-btn').addEventListener('click',function(){
  let btn=this,err=document.getElementById('discover-error'),note=document.getElementById('scan-note');
  btn.disabled=true;
  btn.innerHTML='<i class="ti ti-loader-2"></i>&nbsp;Scanning\u2026';
  err.style.display='none';note.style.display='block';
  fetch('/discover').then(r=>r.json()).then(d=>{
    let devices=(d&&d.devices)||[];
    rebuildDiscoverOptions(devices);
    if(Object.keys(discoveredDevices).length===0)err.style.display='block';
  }).catch(()=>{err.style.display='block';})
  .finally(()=>{
    btn.disabled=false;
    btn.innerHTML='<i class="ti ti-radar-2"></i>&nbsp;Start product scan';
    note.style.display='none';
  });
});

document.getElementById('discover-results').addEventListener('change',function(){
  document.getElementById('manual-ip-row').style.display=(this.value==='__manual__')?'block':'none';
  document.getElementById('productIP-error').style.display='none';
  refreshHighlights();
});

document.getElementById('halo-connect-btn').addEventListener('click',function(){
  let sel=document.getElementById('halo-discover-results');
  let err=document.getElementById('haloIP-error');
  let ip;
  let serial='';
  if(sel.value==='__manual__'){
    ip=document.getElementById('haloIP').value.trim();
    if(!validateIP(ip)){err.style.display='block';return;}
  }else if(sel.value){
    ip=sel.value;
    serial=(discoveredHalos[ip]||{}).serial||'';
  }else{
    return;
  }
  err.style.display='none';
  fetch('/update-halo?haloIP='+encodeURIComponent(ip)+(serial?'&haloSerial='+encodeURIComponent(serial):'')).then(updateStatus);
});

let discoveredHalos={};
function rebuildHaloOptions(devices){
  let sel=document.getElementById('halo-discover-results');
  devices.forEach(dev=>{discoveredHalos[dev.ip]=dev;});
  let prev=sel.value;
  sel.innerHTML='<option value="">Select a Halo\u2026</option>';
  Object.values(discoveredHalos).forEach(dev=>{
    let o=document.createElement('option');
    o.value=dev.ip;
    o.textContent=dev.name+' ('+dev.ip+')';
    sel.appendChild(o);
  });
  let m=document.createElement('option');
  m.value='__manual__';m.textContent='Enter IP address manually\u2026';
  sel.appendChild(m);
  if(prev&&(prev==='__manual__'||discoveredHalos[prev]))sel.value=prev;
  else if(Object.keys(discoveredHalos).length===1)sel.selectedIndex=1;
  sel.dispatchEvent(new Event('change'));
}

document.getElementById('halo-scan-btn').addEventListener('click',function(){
  let btn=this,err=document.getElementById('halo-discover-error'),note=document.getElementById('halo-scan-note');
  btn.disabled=true;
  btn.innerHTML='<i class="ti ti-loader-2"></i>&nbsp;Scanning\u2026';
  err.style.display='none';note.style.display='block';
  fetch('/discover-halo').then(r=>r.json()).then(d=>{
    rebuildHaloOptions((d&&d.devices)||[]);
    if(Object.keys(discoveredHalos).length===0)err.style.display='block';
  }).catch(()=>{err.style.display='block';})
  .finally(()=>{
    btn.disabled=false;
    btn.innerHTML='<i class="ti ti-radar-2"></i>&nbsp;Start Halo scan';
    note.style.display='none';
  });
});

document.getElementById('halo-discover-results').addEventListener('change',function(){
  document.getElementById('halo-manual-ip-row').style.display=(this.value==='__manual__')?'flex':'none';
  document.getElementById('haloIP-error').style.display='none';
  refreshHighlights();
});

document.getElementById('halo-unlink-btn').addEventListener('click',function(){
  fetch('/update-halo?haloIP=').then(updateStatus);
});

document.getElementById('featureToggle').addEventListener('change',function(){
  fetch('/update-feature?enabled='+this.checked);
});

document.getElementById('sourceSelect').addEventListener('change',function(){
  fetch('/update-source?source='+this.value);
});

setInterval(updateStatus,5000);
updateStatus();
</script>
</body>
</html>
)rawliteral";
