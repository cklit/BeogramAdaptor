#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <ArduinoWebsockets.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <mdns.h>
#include <WebServer.h>
#include <Update.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoHA.h>

#define RXD2 16
#define TXD2 17
#define LEDPIN 47
#define NUMPIXELS 1
#define FIRMWARE_VERSION "UNI.2026.8.11"

bool debugSerial = false;

// ── Platform selection ──────────────────────────────────────────────
// The adaptor supports two B&O platforms with different transports:
//   ASE    → SSE stream on :8080 (/BeoNotify/Notifications) + BeoZone REST
//   Mozart → WebSockets on :9339 (+ /remoteControl) + /api/v1 REST
enum Platform { PLATFORM_ASE, PLATFORM_MOZART };
Platform platform = PLATFORM_ASE;

const int SSE_PORT = 8080;          // ASE notification stream
const int WEBSOCKET_PORT = 9339;    // Mozart notification websocket
const int HALO_WEBSOCKET_PORT = 8080;
const char* DEVICE_NAME = "Beogram";
const char* AP_SSID = "BeogramAdaptor";
const char* AP_PASSWORD = "password";

// ── Timers ──────────────────────────────────────────────────────────
const unsigned long reconnectInterval = 10000;
unsigned long sseLastReconnectAttempt = 0;
unsigned long sseReconnectDelay = 1000;                    // ASE: exponential backoff, start at 1s
static unsigned long wsLastReconnectAttempt = millis();    // Mozart
static unsigned long haloLastReconnectAttempt = millis();
static unsigned long mqttLastReconnectAttempt = millis();
static unsigned long wsLastPingReceived = millis();        // Mozart
static unsigned long wsRemoteLastPingReceived = millis();  // Mozart
static unsigned long haloLastPingReceived = millis();
const unsigned long pingTimeout = 10000;

unsigned long haloActionTime = 0;      // set millis when certain Halo state/page updates are triggered
const unsigned long haloActionDelay = 800;

unsigned long lastStartEventTime = 0;  // Mozart: debounce "started" events
unsigned long delayPlayAfterDigit = 0; // delay PLAY command to CD player, when using digits

const unsigned long stateDebounceDelay = 100;

bool haloControls; 
bool lineInActive = false;
bool waitingForPlay = false;
bool mqttConnected = false;

enum BeogramCommand : uint8_t {
    PLAY = 0x35,
    STOP = 0x26,
    STANDBY = 0x16,
    NEXT = 0x2B,
    PREVIOUS = 0x18,
    WIND = 0x1A,
    REWIND = 0x3A,
    OPEN_FOR_DIGIT = 0x66,
    DIGIT1 = 0x1F,
    DIGIT2 = 0x2F,
    DIGIT3 = 0x0F,
    DIGIT4 = 0x37,
    DIGIT5 = 0x17,
    DIGIT6 = 0x27,
    DIGIT7 = 0x07,
    DIGIT8 = 0x3B,
    DIGIT9 = 0x1B,
    DIGIT0 = 0x3F
};

enum PlaybackState {
    PLAYING,
    PAUSED,
    STOPPED,
    BOOT
};

enum HaloUpdate {
    CONFIG,
    STATE,
    PAGE,
    NONE
};

enum BeogramFeedback : uint8_t {
    TRACK1 = 0x01,   
    TRACK2 = 0x02,
    TRACK3 = 0x03,
    TRACK4 = 0x04,
    TRACK5 = 0x05,
    TRACK6 = 0x06,
    TRACK7 = 0x07,
    TRACK8 = 0x08,
    TRACK9 = 0x09,
    TRACK10 = 0x0A,
    TRACK11 = 0x0B,
    TRACK12 = 0x0C,
    TRACK13 = 0x0D,
    TRACK14 = 0x0E,
    TRACK14_PLUS = 0x0F,
    PLAYING_FB = 0x1E,
    STOPPED_FB = 0x46,
    STANDBY_FB = 0x2E,
    EJECTED_FB = 0x76,
    UNKNOWN_STATE = 0xFF
};

BeogramFeedback identifyState(const uint8_t* sequence, size_t length) {
    if (debugSerial == true) {
        Serial.print("Identifying state for sequence: ");
        for (size_t i = 0; i < length; ++i) {
            Serial.print(sequence[i], HEX);
            Serial.print(" ");
        }
        Serial.print("Length: ");
        Serial.println(length);
    }
    if (length == 5) {
        if (sequence[0] == 0x78 && sequence[4] == 0x7D) return TRACK5;
        if (sequence[0] == 0x78 && sequence[4] == 0x7E) return TRACK6;
        if (sequence[0] == 0x78 && sequence[4] == 0x7C) return TRACK7;  
        if (sequence[0] == 0x78 && sequence[4] == 0x7F) return TRACK13;          
    } else if (length == 2) {
        if (sequence[0] == PLAYING_FB && sequence[1] == PLAYING_FB) return PLAYING_FB;
        if (sequence[0] == STOPPED_FB && sequence[1] == STOPPED_FB) return STOPPED_FB;
        if (sequence[0] == STANDBY_FB && sequence[1] == STANDBY_FB) return STANDBY_FB;
        if (sequence[0] == EJECTED_FB && sequence[1] == EJECTED_FB) return EJECTED_FB;
    } else if (length == 4) {
        if (sequence[0] == 0x78 && sequence[3] == 0x77) return TRACK1;
        if (sequence[0] == 0x78 && sequence[3] == 0x7B) return TRACK2;
        if (sequence[0] == 0x78 && sequence[3] == 0x73) return TRACK3;
        if (sequence[0] == 0x78 && sequence[3] == 0x7D) return TRACK4;    
        if (sequence[0] == 0x78 && sequence[1] == 0x70 && sequence[3] == 0x75) return TRACK5;
        if (sequence[0] == 0x78 && sequence[1] == 0x70 && sequence[3] == 0x79) return TRACK6;
        if (sequence[0] == 0x78 && sequence[1] == 0x70 && sequence[3] == 0x71) return TRACK7;                        
        if (sequence[0] == 0x78 && sequence[1] == 0x70 && sequence[3] == 0x7E) return TRACK8;
        if (sequence[0] == 0x78 && sequence[1] == 0x70 && sequence[3] == 0x76) return TRACK9;
        if (sequence[0] == 0x78 && sequence[1] == 0x70 && sequence[3] == 0x7A) return TRACK10;
        if (sequence[0] == 0x78 && sequence[1] == 0x70 && sequence[3] == 0x72) return TRACK11;
        if (sequence[0] == 0x78 && sequence[2] == 0x78 && sequence[3] == 0x7C) return TRACK12;
        if (sequence[0] == 0x78 && sequence[1] == 0x70 && (sequence[3] == 0x1E || sequence[3] == 0x74)) return TRACK13;
        if (sequence[0] == 0x78 && sequence[1] == 0x70 && (sequence[3] == 0x78 || sequence[3] == 0xF)) return TRACK14;
        if (sequence[0] == 0x78 && sequence[1] == 0x70 && sequence[3] == 0x70) return TRACK14_PLUS;
    }
    return UNKNOWN_STATE;
}

bool isValidIPAddress(const String& ip) {
    int parts[4];
    if (sscanf(ip.c_str(), "%d.%d.%d.%d", &parts[0], &parts[1], &parts[2], &parts[3]) == 4) {
        for (int i = 0; i < 4; i++) {
            if (parts[i] < 0 || parts[i] > 255) return false;
        }
        return true;
    }
    return false;
}

PlaybackState playbackState = BOOT;
HaloUpdate haloUpdate = NONE;
Adafruit_NeoPixel pixels(NUMPIXELS, LEDPIN, NEO_GRB + NEO_KHZ800);
Preferences preferences;
HTTPClient http;

String productIP;                    // unified — replaces sseIP (ASE) / wsIP (Mozart)
String haloIP;
String triggerSource;                // ASE: "LINE IN"/"TOSLINK" — Mozart: "lineIn"/"spdif"

String mqttIP;
String mqttUser;
String mqttPassword;

WiFiManager wm;
using namespace websockets;
WiFiClient sseClient;                // ASE: raw TCP client for the SSE stream
WebsocketsClient wsClient;           // Mozart: product notification websocket
WebsocketsClient remoteClient;       // Mozart: /remoteControl websocket
WebsocketsClient haloClient;         // shared: Beoremote Halo
WebServer server(80);

WiFiClient wifi;

byte mac[6];  

char configUrl[25]; 
char idPlay[35];
char idNext[35];
char idPrev[35];
char idStop[35];
char idStandby[35];
char idTrack[35];
char idPlayback[35];
char idPlaying[35];

HADevice device; 
HAMqtt mqtt(wifi, device);

String macToUnderscoreString(uint8_t* mac, size_t macLength) {
  String macStr;
  for (size_t i = 0; i < macLength; i++) {
    if (i > 0) macStr += "_";
    if (mac[i] < 0x10) macStr += "0";
    macStr += String(mac[i], HEX);
  }
  macStr.toLowerCase();
  return macStr;
}

HAButton bgPlay(idPlay); 
HAButton bgNext(idNext);   
HAButton bgPrev(idPrev);
HAButton bgStop(idStop);
HAButton bgStandby(idStandby);
HASensor bgTrack(idTrack);
HASensor bgPlaybackState(idPlayback);
HABinarySensor bgPlaying(idPlaying);

bool productConnected() {
    if (productIP.length() == 0) return false;
    return (platform == PLATFORM_MOZART) ? wsClient.available() : sseClient.connected();
}

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
    <div class="form-group" id="halo-connect-form">
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

function updateStatus(){
  fetch('/status').then(r=>r.json()).then(d=>{
    applyPlatform(d.platform);
    setBadge(document.getElementById('product-status'),d.product_connected);
    setBadge(document.getElementById('halo-ws-status'),d.halo_ws_connected);
    setBadge(document.getElementById('mqtt-status'),d.mqtt_connected);
    document.getElementById('fw-version').textContent=d.firmware;
    document.getElementById('featureToggle').checked=d.feature_enabled;
    document.getElementById('sourceSelect').value=d.trigger_source;

    let hasProduct=d.product_ip&&d.product_ip!=='';
    document.getElementById('product-ip').textContent=hasProduct?d.product_ip:'—';
    document.getElementById('product-connect-form').style.display=hasProduct?'none':'flex';
    document.getElementById('product-action-row').style.display=hasProduct?'flex':'none';

    let hasHalo=d.halo_ip&&d.halo_ip!=='';
    document.getElementById('halo-ip').textContent=hasHalo?d.halo_ip:'—';
    document.getElementById('halo-connect-form').style.display=hasHalo?'none':'flex';
    document.getElementById('halo-action-row').style.display=hasHalo?'flex':'none';
  }).catch(()=>{});
}

document.getElementById('product-connect-btn').addEventListener('click',function(){
  let sel=document.getElementById('discover-results');
  let err=document.getElementById('productIP-error');
  let ip,discovered=null;
  if(sel.value==='__manual__'){
    ip=document.getElementById('productIP').value.trim();
    if(!validateIP(ip)){err.style.display='block';return;}
    discovered=manualPlatform;
  }else if(sel.value){
    ip=sel.value;
    discovered=discoveredDevices[ip];
  }else{
    return;
  }
  err.style.display='none';
  if(discovered&&discovered!==currentPlatform){
    let label=discovered==='mozart'?'Mozart':'ASE';
    if(!confirm('This is a '+label+' product. The adaptor will switch platform and restart. Continue?'))return;
    fetch('/update-platform?platform='+discovered+'&productIP='+encodeURIComponent(ip)).catch(()=>{});
    document.getElementById('product-platform-label').textContent='Restarting\u2026';
    setTimeout(()=>location.reload(),8000);
    return;
  }
  fetch('/update?productIP='+encodeURIComponent(ip)).then(updateStatus);
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
  if(sel.value==='__manual__'){
    ip=document.getElementById('haloIP').value.trim();
    if(!validateIP(ip)){err.style.display='block';return;}
  }else if(sel.value){
    ip=sel.value;
  }else{
    return;
  }
  err.style.display='none';
  fetch('/update-halo?haloIP='+encodeURIComponent(ip)).then(updateStatus);
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

struct ButtonUpdate {
    String id;
    bool pending;
    unsigned long timestamp;
};

ButtonUpdate pendingUpdate = {"", false, 0}; // Track pending button updates

void sendButtonUpdate(const char* buttonID, const char* state = nullptr, const char* title = nullptr, const char* text = nullptr, const char* subtitle = nullptr, int value = -1) {
    JsonDocument doc;
    doc["update"]["type"] = "button";
    doc["update"]["id"] = buttonID;
    if (value != -1) doc["update"]["value"] = value;
    if (state != nullptr) doc["update"]["state"] = state;
    if (title != nullptr) doc["update"]["title"] = title;
    if (subtitle != nullptr) doc["update"]["subtitle"] = subtitle;
    if (text != nullptr) doc["update"]["content"]["text"] = text;
    String output;
    serializeJson(doc, output);
    haloClient.send(output);
}

void sendPageUpdate(const char* pageID, const char* buttonID) {
    JsonDocument doc;
    doc["update"]["type"] = "displaypage";
    doc["update"]["pageid"] = pageID; 
    doc["update"]["buttonid"] = buttonID;
    String output;
    serializeJson(doc, output);
    haloClient.send(output);
}

void sendHexCommand(BeogramCommand command) {
    Serial1.write(command);
    delayMicroseconds(49991);
    Serial1.write(command);
}

// Non-blocking WiFi reconnection
void checkWiFiConnection() {
    static unsigned long lastWifiAttempt = 0;
    if (WiFi.status() != WL_CONNECTED && millis() - lastWifiAttempt > 10000) {
        lastWifiAttempt = millis();
        Serial.println("WiFi lost, attempting to reconnect...");
        WiFi.reconnect();
    }
}

void checkMQTTConnection(bool forceNow = false) {
    if (!mqtt.isConnected() && mqttIP.length() > 0) {
        if (forceNow || millis() - mqttLastReconnectAttempt > reconnectInterval) {
            mqttLastReconnectAttempt = millis();
            IPAddress broker;
            if (broker.fromString(mqttIP)) {
                Serial.println(forceNow ? "⚡ Initial MQTT connect..." : "🔁 Attempting MQTT reconnect...");
                mqtt.begin(broker, mqttUser.c_str(), mqttPassword.c_str());
            } else {
                Serial.println("⚠️ Invalid MQTT broker IP format (connect attempt skipped)");
            }
        }
    }
    mqttConnected = mqtt.isConnected(); // Always keep it updated
}

// ════════════════════════════════════════════════════════════════════
// ASE transport (SSE stream + BeoZone REST)
// ════════════════════════════════════════════════════════════════════

void forceSource() {
    if (WiFi.status() == WL_CONNECTED) {
        String payload = "{\"sourceType\":{\"type\":\"" + triggerSource + "\"}}";
        String url = "http://" + productIP + ":" + String(SSE_PORT) + "/BeoZone/Zone/ActiveSourceType";
        Serial.println("Activating Line-In on the product.");  
        HTTPClient httpForce;
        httpForce.begin(url);
        httpForce.addHeader("Content-Type", "application/json");
        int code = httpForce.POST(payload);
        Serial.println("Status: " + String(code));
        httpForce.end();
    }
}

void connectToSSE() {
    if (sseClient.connected()) return;  // Avoid reconnecting if already connected

    Serial.printf("Connecting to SSE stream: %s\n", productIP.c_str());
    sseClient.stop();  // Ensure the previous client is closed

    if (!sseClient.connect(productIP.c_str(), SSE_PORT)) {
        Serial.println("Connection to server failed!");
        return;
    }

    sseClient.println("GET /BeoNotify/Notifications HTTP/1.1");
    sseClient.println("Host: " + productIP + ":" + String(SSE_PORT));
    sseClient.println("Accept: text/event-stream");
    sseClient.println("Connection: keep-alive");
    sseClient.println();

    Serial.println("Connected to SSE stream!");
}

void checkSSEConnection() {
    if (!sseClient.connected()) {
        unsigned long now = millis();

        // Ensure Wi-Fi is connected before attempting SSE reconnect
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("Wi-Fi disconnected. Waiting for reconnection...");
            return;
        }

        // Wait for the delay before retrying
        if (now - sseLastReconnectAttempt >= sseReconnectDelay) {
            Serial.println("SSE stream lost. Reconnecting...");
            sseLastReconnectAttempt = now;
            connectToSSE();

            // Implement exponential backoff (double the delay up to a max of 8s)
            sseReconnectDelay = min(sseReconnectDelay * 2, 8000UL);
        }
    } else {
        // Reset delay if connection is stable
        sseReconnectDelay = 1000;
    }
}

void processSSE(String message) {
    if (message.startsWith("data: ")) {
        message = message.substring(6);
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, message);
    if (error) {
        Serial.print("JSON parse error: ");
        Serial.println(error.c_str());
        return;
    }

    JsonObject notification = doc["notification"];
    if (notification.isNull()) return;

    String type = notification["type"].as<String>();
    JsonObject data = notification["data"];
    if (data.isNull()) return;

    // Handle CONTROL commands
    if (type == "COMMAND") {
        String category = data["category"].as<String>();
        String key = data["key"].as<String>();
        String event = data["event"].as<String>();
        
        if (lineInActive && category == "Control" && event == "KeyPress") {
            if (key == "Play") {
                Serial.println("✅ Received Control/Play!");
                playbackState = PLAYING;
                sendHexCommand(PLAY);
                Serial.println("Sent PLAY command to Beogram");
                if (haloClient.available()) {
                    sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Playing", "Stop");
                } 
            } else if (key == "Stop") {
                Serial.println("✅ Received Control/Stop!");
                playbackState = PAUSED;
                sendHexCommand(STOP);
                if (haloClient.available()) {
                    sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Stopped", "Play"); 
                }                
                Serial.println("Sent STOP command to Beogram");
            } else if (key == "Wind") {
                Serial.println("✅ Received Control/Wind!");
                sendHexCommand(NEXT);
                Serial.println("Sent NEXT command to Beogram");
            } else if (key == "Rewind") {
                Serial.println("✅ Received Control/Rewind!");
                sendHexCommand(PREVIOUS);
                Serial.println("Sent PREV command to Beogram");
            } else if (key.length() == 1 && isDigit(key[0])) {
                const BeogramCommand digitCommands[10] = {
                    DIGIT0, DIGIT1, DIGIT2, DIGIT3, DIGIT4, DIGIT5, DIGIT6, DIGIT7, DIGIT8, DIGIT9
                };
                BeogramCommand digitCommand = digitCommands[key[0] - '0'];
                sendHexCommand(OPEN_FOR_DIGIT);
                delay(50); 
                sendHexCommand(digitCommand);

                // Start the non-blocking delay
                delayPlayAfterDigit = millis();
                waitingForPlay = true;
                Serial.printf("🔢 Sent Digit %c\n", key[0]);               
            }
        }
    }

    if (type == "SOURCE" || type == "SOURCE_EXPERIENCE_CHANGED") {
        if (data.size() == 0) {
            Serial.println("🛑 Standby mode detected (empty SOURCE data).");
            lineInActive = false;
            playbackState = STOPPED;
            sendHexCommand(STANDBY);
            if (haloClient.available()) {
                sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Stopped", "Play", "");  
            }                    
            Serial.println("Sent STBY command to Beogram");
        } else {
            JsonObject primaryExperience = data["primaryExperience"];
            if (primaryExperience.isNull()) return;

            JsonObject source = primaryExperience["source"];
            if (source.isNull()) return;

            String sourceType = source["sourceType"]["type"].as<String>();

            if (sourceType == triggerSource && !lineInActive) { //to avoid "re-activating" Line-in on SSE reconnect
                Serial.println("✅ Line-in activated!");
                lineInActive = true;
                haloActionTime = millis();  // Store the current time  
                if (haloControls) {
                    haloUpdate = PAGE;
                }           
                if (playbackState != PLAYING) {
                    sendHexCommand(PLAY);
                    playbackState = PLAYING;
                    Serial.println("Sent PLAY command to Beogram");
                }
            } else if (sourceType != triggerSource) {
                Serial.println("❌ Line-in deactivated!");
                lineInActive = false;
                if (haloClient.available()) {
                    sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Stopped", "Play", "");  
                }                             
                if (playbackState == PLAYING) {
                    playbackState = PAUSED;
                    sendHexCommand(STOP);    
                    Serial.println("Sent STOP command to Beogram to Pause playback.");
                }
            }
        }
    }
}

void readSSE() {
    static String lineBuffer = "";

    while (sseClient.available()) {
        char c = sseClient.read();
        if (c == '\n') {
            lineBuffer.trim();
            if (lineBuffer.length() > 0 && (lineBuffer.startsWith("data: ") || lineBuffer.startsWith("{"))) {
                processSSE(lineBuffer);
            }
            lineBuffer = "";
        } else {
            lineBuffer += c;
        }
    }
}

// ════════════════════════════════════════════════════════════════════
// Mozart transport (WebSockets + /api/v1 REST)
// ════════════════════════════════════════════════════════════════════

void handleHttpResponse(const String& endpoint, const String& response) {
    if (endpoint == "/api/v1/playback/state") {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, response);
        if (!error) {
            String source = doc["source"]["id"].as<String>();
            String state = doc["state"]["value"].as<String>();
            if (source == triggerSource && state == "started" && haloClient.available()) {
                sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Playing", "Stop");
                lineInActive = true;
                playbackState = PLAYING;
                Serial.println("Polled Playing state from product");
            } else {
                sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Stopped", "Play");
                Serial.println("Polled Stopped state from product");
            }
        } else {
            Serial.println("JSON parsing failed!");
        }
    }
}

void sendHttpRequest(const String& endpoint, const String& method = "GET", const String& payload = "") {
    if (WiFi.status() == WL_CONNECTED) {
        String url = "http://" + productIP + endpoint;
        Serial.println("Sending " + method + " request to: " + url);
        http.begin(url);
        if (method == "POST") http.addHeader("Content-Type", "application/json");
        int httpResponseCode;
        if (method == "POST") {
            httpResponseCode = payload.isEmpty() ? http.POST("") : http.POST(payload);
        } else {
            httpResponseCode = http.GET();
        }
        Serial.println("HTTP Response code: " + String(httpResponseCode));
        if (httpResponseCode == HTTP_CODE_OK) {
            String response = http.getString();
            handleHttpResponse(endpoint, response);
        }
        http.end();
    } else {
        Serial.println("WiFi not connected, cannot send request.");
    }
}

// Reconnect both Mozart WebSockets only if not already connected
void checkWebSocketConnection() {
    if (millis() - wsLastReconnectAttempt > reconnectInterval) {
        wsLastReconnectAttempt = millis();
        if (!wsClient.available()) {
            Serial.println("Reconnecting product websocket...");
            if (wsClient.connect(("ws://" + productIP + ":" + WEBSOCKET_PORT).c_str())) {
                wsClient.send("Hi Server!");
                Serial.println("Product webSocket reconnected!");
            } else {
                Serial.println("Product webSocket reconnection failed.");
            }
        }
        if (!remoteClient.available()) {
            Serial.println("Reconnecting remote websocket...");
            if (remoteClient.connect(("ws://" + productIP + ":" + WEBSOCKET_PORT + "/remoteControl").c_str())) {
                remoteClient.send("Hi Server!");
                Serial.println("Secondary websocket reconnected!");
            } else {
                Serial.println("Remote webSocket reconnection failed.");
            }
        }
    }
}

void processWebSocketMessage(const String& message) {
    unsigned long currentTime = millis();

    if (message.indexOf("\"eventType\":\"WebSocketEventSourceChange\"") != -1) {
        if (message.indexOf("\"id\":\"" + triggerSource + "\"") != -1) {
            lineInActive = true;
            Serial.println("✅ Line-in activated");
            haloActionTime = millis();
            if (haloControls) haloUpdate = PAGE;
        } else {
            lineInActive = false;
            Serial.println("❌ Source changed, Line-in deactivated");
            if (playbackState == PLAYING) {
                playbackState = PAUSED;
                sendHexCommand(STOP);
                Serial.println("⏹️ Sent STOP command to Beogram to Pause playback.");
                if (haloClient.available()) {
                    sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Stopped", "Play", "");
                }
            }
        }
    } else if (message.indexOf("\"value\":\"networkStandby\"") != -1) {
        playbackState = STOPPED;
        if (haloClient.available()) {
            sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Stopped", "Play", "");
        }
        sendHexCommand(STANDBY);
        Serial.println("🛑 Standby command detected on websocket. Sent STBY command to Beogram");
    } else if (lineInActive) {
        if (message.indexOf("\"value\":\"started\"") != -1) {
            if (currentTime - lastStartEventTime > stateDebounceDelay) {
                lastStartEventTime = currentTime;
                if (playbackState != PLAYING) {
                    sendHexCommand(PLAY);
                    if (haloClient.available()) {
                        sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Playing", "Stop");
                    }
                    Serial.println("▶️ Product changed state to Play from Pause or Standby. Sent PLAY command to Beogram");
                }
            }
        } else if (message.indexOf("\"value\":\"stopped\"") != -1 && playbackState != STOPPED) {
            playbackState = PAUSED;
            sendHexCommand(STOP);
            if (haloClient.available()) {
                sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Stopped", "Play");
            }
            Serial.println("⏸️ Product changed state to Stopped. Sent STOP command to Beogram");
        } else if (message.indexOf("\"value\":\"paused\"") != -1) {
            playbackState = PAUSED;
            sendHexCommand(STOP);
            if (haloClient.available()) {
                sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Stopped", "Play");
            }
            Serial.println("⏸️ Product changed state to Paused. Sent STOP command to Beogram");
        } else if (message.indexOf("\"button\":\"Next\"") != -1) {
            sendHexCommand(NEXT);
            Serial.println("⏭️ Sent NEXT command to Beogram");
        } else if (message.indexOf("\"button\":\"Previous\"") != -1) {
            sendHexCommand(PREVIOUS);
            Serial.println("⏮️ Sent PREV command to Beogram");
        }
    }
}

void processRemoteWebSocketMessage(const String& message) {
    if (message.indexOf("\"eventType\":\"WebSocketEventBeoRemoteButton\"") != -1 &&
       message.indexOf("\"Type\":\"KeyPress\"") != -1 && lineInActive) {
        if (message.indexOf("\"Key\":\"Wind\"") != -1) {
            sendHexCommand(NEXT);
            Serial.println("⏭️ Remote command: NEXT (Wind)");
        } else if (message.indexOf("\"Key\":\"Rewind\"") != -1) {
            sendHexCommand(PREVIOUS);
            Serial.println("⏮️ Remote command: PREV (Rewind)");
        } else if (message.indexOf("\"Key\":\"Control/Wind\"") != -1) {
            sendHexCommand(NEXT);
            Serial.println("⏭️ Remote command: Control/Wind");
        } else if (message.indexOf("\"Key\":\"Control/Rewind\"") != -1) {
            sendHexCommand(PREVIOUS);
            Serial.println("⏮️ Remote command: Control/Rewind");
        } else if (message.indexOf("\"Key\":\"Control/Stop\"") != -1) {
            sendHexCommand(STOP);
            Serial.println("⏹️ Remote command: Control/Stop");
        } else if (message.indexOf("\"Key\":\"Control/Play\"") != -1) {
            sendHexCommand(PLAY);
            Serial.println("▶️ Remote command: Control/Play");
        } else if (message.indexOf("\"Key\":\"Control/Digit") != -1) {
            int digitIndex = message.indexOf("\"Key\":\"Control/Digit") + 20;
            char digitChar = message[digitIndex];
            if (isdigit(digitChar)) {
                const BeogramCommand digitCommands[10] = {
                    DIGIT0, DIGIT1, DIGIT2, DIGIT3, DIGIT4,
                    DIGIT5, DIGIT6, DIGIT7, DIGIT8, DIGIT9
                };
                BeogramCommand digitCommand = digitCommands[digitChar - '0'];
                sendHexCommand(OPEN_FOR_DIGIT);
                delay(50);
                sendHexCommand(digitCommand);
                delayPlayAfterDigit = millis();
                waitingForPlay = true;
                Serial.printf("🔢 Sent Digit %c\n", digitChar);
            }
        }
    }
}

// ════════════════════════════════════════════════════════════════════
// Shared logic
// ════════════════════════════════════════════════════════════════════

void checkPingWebsocket() {
    if (platform == PLATFORM_MOZART) {
        if (wsClient.available() && productIP.length() > 0) {    
            if (millis() - wsLastPingReceived >= pingTimeout) {
                wsClient.ping();
                wsLastPingReceived = millis();
            }
        } else if (!wsClient.available() && productIP.length() > 0) {    
            if (millis() - wsLastPingReceived >= pingTimeout) {
                wsClient.close();
                checkWebSocketConnection();
            }
        }

        if (remoteClient.available() && productIP.length() > 0) {    
            if (millis() - wsRemoteLastPingReceived >= pingTimeout) {
                remoteClient.ping();
                wsRemoteLastPingReceived = millis();
            }
        } else if (!remoteClient.available() && productIP.length() > 0) {    
            if (millis() - wsRemoteLastPingReceived >= pingTimeout) {
                remoteClient.close();
                checkWebSocketConnection();
            }
        }
    }

    if (haloClient.available()) {
        if (millis() - haloLastPingReceived >= pingTimeout) {
            haloClient.ping();
            haloLastPingReceived = millis();
        }
    }
}

void handleRoot() {
    server.send(200, "text/html", htmlPage);
}

void handleOTAUpdate() {
    HTTPUpload& upload = server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("OTA Update Start: %s\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        Serial.printf("Writing %d bytes...\n", upload.currentSize);
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            Serial.printf("OTA Update Success! %d bytes\n", upload.totalSize);
        } else {
            Update.printError(Serial);
        }
    }
}

void handleUpdate() {
    if (server.hasArg("productIP")) {
        String newIP = server.arg("productIP");
        if (newIP == "") {
            if (platform == PLATFORM_MOZART) {
                wsClient.close();
                remoteClient.close();
            } else {
                sseClient.stop();
            }
            productIP = newIP;
            preferences.putString("productIP", productIP);
            server.send(200, "text/plain", "Unlinked product");
            Serial.println("Unlinked product."); 
            return;
        } else if (!isValidIPAddress(newIP)) {
            server.send(400, "text/html", "<h2>Invalid IP Address</h2><a href='/'>Go Back</a>");
            Serial.println("Invalid IP Address - not saved.");             
            return;
        }
        productIP = newIP;
        preferences.putString("productIP", productIP);

        if (platform == PLATFORM_MOZART) {
            wsClient.close();
            remoteClient.close();
            wsClient.connect(("ws://" + productIP + ":" + WEBSOCKET_PORT).c_str());
            remoteClient.connect(("ws://" + productIP + ":" + WEBSOCKET_PORT + "/remoteControl").c_str());
            server.send(200, "text/html", "<h2>IP Updated to " + productIP + "</h2><a href='/'>Go Back</a>");
            if (wsClient.available()) {
                String overlayPayload = R"rawliteral(
                {
                    "volumeAbsolute": 50,
                    "textToSpeech": {
                        "lang": "en-gb",
                        "text": "The Beogram Adaptor is now monitoring this product"
                    }
                }
                )rawliteral";
                sendHttpRequest("/api/v1/overlay/play", "POST", overlayPayload);
            }
        } else {
            server.send(200, "text/html", "<h2>IP Updated to " + productIP + "</h2><a href='/'>Go Back</a>");
            sseClient.stop();
            connectToSSE();
        }
    } else {
        server.send(400, "text/html", "<h2>No IP Address Provided</h2><a href='/'>Go Back</a>");
    }
}

void handleUpdateHalo() {
    if (server.hasArg("haloIP")) {
        String newHaloIP = server.arg("haloIP");
        if (newHaloIP == "") {
            haloClient.close();
            haloIP = newHaloIP;
            preferences.putString("haloIP", haloIP); // Store the Halo IP in preferences
            server.send(200, "text/plain", "Unlinked Halo");
            Serial.println("Unlinked Halo."); 
            return;
        } else if (!isValidIPAddress(newHaloIP)) {
            server.send(400, "text/html", "<h2>Invalid IP Address for Beoremote Halo</h2><a href='/'>Go Back</a>");
            Serial.println("Invalid IP Address - not saved."); 
            return;
        }
        haloIP = newHaloIP;
        preferences.putString("haloIP", haloIP); // Store the Halo IP in preferences

        // If there is an existing connection, close it first
        haloClient.close();

        // Now establish a new WebSocket connection to the Beoremote Halo WebSocket server
        haloClient.connect(("ws://" + haloIP + ":" + HALO_WEBSOCKET_PORT).c_str());
        server.send(200, "text/html", "<h2>Halo IP Updated to " + haloIP + "</h2><a href='/'>Go Back</a>");
    } else {
        server.send(400, "text/html", "<h2>No Halo IP Address Provided</h2><a href='/'>Go Back</a>");
    }
}

void handleUpdateTriggerSource() {
    if (server.hasArg("source")) {
        String newSource = server.arg("source");
        bool valid = (platform == PLATFORM_MOZART)
            ? (newSource == "lineIn" || newSource == "spdif")
            : (newSource == "LINE IN" || newSource == "TOSLINK");
        if (valid) {
            triggerSource = newSource;
            preferences.putString("triggerSource", triggerSource);
            server.send(200, "text/plain", "Source updated");
            return;
        }
    }
    server.send(400, "text/plain", "Invalid source");
}

void handleUpdatePlatform() {
    if (server.hasArg("platform")) {
        String newPlatform = server.arg("platform");
        if (newPlatform == "ase" || newPlatform == "mozart") {
            preferences.putString("platform", newPlatform);
            // The stored product IP points at the old platform's product, and the
            // trigger source values differ between platforms — reset both. If a
            // product IP is supplied (discovery flow), store it instead so the
            // adaptor connects to the discovered product right after restart.
            String newIP = server.hasArg("productIP") ? server.arg("productIP") : "";
            if (newIP != "" && !isValidIPAddress(newIP)) newIP = "";
            preferences.putString("productIP", newIP);
            preferences.putString("triggerSource", newPlatform == "mozart" ? "lineIn" : "LINE IN");
            server.send(200, "text/plain", "Platform updated. Restarting...");
            Serial.println("Platform changed to " + newPlatform + ". Restarting...");
            delay(500);
            ESP.restart();
            return;
        }
    }
    server.send(400, "text/plain", "Invalid platform");
}

// Browse the network for B&O products via mDNS. Discovery only — the
// actual connection still uses the IP address the user confirms.
// The service type identifies the platform: ASE products announce
// _beoremote._tcp, Mozart _bangolufsen._tcp — so both are scanned and
// each hit is tagged, letting the UI switch platform automatically.
// The friendly name lives in the "name" (ASE) / "fn" (Mozart) TXT record.
//
// The raw ESP-IDF query API is used instead of ESPmDNS.queryService()
// because the wrapper hardcodes a 3 s window. A longer window is far more
// reliable: the stack retransmits the query several times while the search
// is active, so slow or lossy responders get multiple chances to answer.
#define MDNS_SCAN_TIME_MS 5000   // per service type

void collectService(const char* service, const char* nameKey,
                    const char* platformTag, JsonArray& arr, String& seenIPs) {
    mdns_result_t* results = nullptr;
    esp_err_t err = mdns_query_ptr(service, "_tcp", MDNS_SCAN_TIME_MS, 20, &results);
    if (err != ESP_OK) {
        Serial.printf("mDNS query %s failed: %d\n", service, err);
        return;
    }

    for (mdns_result_t* r = results; r != nullptr; r = r->next) {
        String ip = "";
        for (mdns_ip_addr_t* a = r->addr; a != nullptr; a = a->next) {
            if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                ip = IPAddress(a->addr.u_addr.ip4.addr).toString();
                break;
            }
        }
        if (ip == "" || seenIPs.indexOf("|" + ip + "|") >= 0) continue;
        seenIPs += ip + "|";

        String friendly = "";
        for (size_t t = 0; t < r->txt_count; t++) {
            if (strcmp(r->txt[t].key, nameKey) == 0 && r->txt[t].value != nullptr) {
                friendly = r->txt[t].value;
                break;
            }
        }
        if (friendly == "" && r->instance_name != nullptr) friendly = r->instance_name;
        if (friendly == "" && r->hostname != nullptr) friendly = r->hostname;

        JsonObject d = arr.add<JsonObject>();
        d["name"]     = friendly;
        d["ip"]       = ip;
        d["platform"] = platformTag;
    }
    mdns_query_results_free(results);
}

void handleDiscover() {
    JsonDocument doc;
    JsonArray arr = doc["devices"].to<JsonArray>();
    String seenIPs = "|";

    // Mozart first: wins dedup if a product announces both services
    collectService("_bangolufsen", "fn",   "mozart", arr, seenIPs);
    collectService("_beoremote",   "name", "ase",    arr, seenIPs);

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

// Beoremote Halo announces _zenith._tcp (Zenith is its internal codename).
void handleDiscoverHalo() {
    JsonDocument doc;
    JsonArray arr = doc["devices"].to<JsonArray>();
    String seenIPs = "|";

    collectService("_zenith", "fn", "halo", arr, seenIPs);

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

void handleMqttReset() {
    preferences.remove("mqttIP");
    preferences.remove("mqttUser");
    preferences.remove("mqttPassword");

    mqttIP = "";
    mqttUser = "";
    mqttPassword = "";

    server.send(200, "text/html", R"rawliteral(
        <!DOCTYPE html>
        <html lang="en">
        <head>
        <meta charset="UTF-8">
        <meta name="viewport" content="width=device-width, initial-scale=1.0">
        <meta http-equiv="refresh" content="5;url=/mqtt">
        <title>Reset</title>
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
            .card-header{display:flex;align-items:center;gap:10px;margin-bottom:.5rem}
            .card-header i{font-size:18px;color:#a32d2d}
            .card-header h2{font-size:15px;font-weight:500}
            .sub{font-size:13px;color:#888;margin-bottom:1rem}
            .btn{height:36px;padding:0 14px;font-size:13px;border:1px solid #ddd;border-radius:8px;background:#fff;color:#111;cursor:pointer;text-decoration:none;display:inline-flex;align-items:center;gap:6px}
            .btn:hover{background:#f5f5f5}
            @media(prefers-color-scheme:dark){.btn{background:#2a2a2a;border-color:#444;color:#eee}.btn:hover{background:#333}}
        </style>
        </head>
        <body>
        <div class="page">
        <div class="page-title">
            <i class="ti ti-smart-home"></i>
            <h1>Home Assistant</h1>
        </div>
        <div class="card">
            <div class="card-header">
            <i class="ti ti-trash"></i>
            <h2>MQTT settings cleared</h2>
            </div>
            <p class="sub">Restarting to apply changes&hellip;</p>
            <a href="/mqtt" class="btn"><i class="ti ti-arrow-left" style="font-size:14px"></i>Back to MQTT</a>
        </div>
        </div>
        </body>
        </html>
        )rawliteral");

    delay(1000);
    ESP.restart();
}

void handleMqttUpdate() {
    if (server.hasArg("ip") && server.hasArg("user") && server.hasArg("pass")) {
        mqttIP = server.arg("ip");
        mqttUser = server.arg("user");
        mqttPassword = server.arg("pass");

        preferences.putString("mqttIP", mqttIP);
        preferences.putString("mqttUser", mqttUser);
        preferences.putString("mqttPassword", mqttPassword);

        server.send(200, "text/html", R"rawliteral(
            <!DOCTYPE html>
            <html lang="en">
            <head>
            <meta charset="UTF-8">
            <meta name="viewport" content="width=device-width, initial-scale=1.0">
            <meta http-equiv="refresh" content="5;url=/">
            <title>Saved</title>
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
                .card-header{display:flex;align-items:center;gap:10px;margin-bottom:.5rem}
                .card-header i{font-size:18px;color:#1D9E75}
                .card-header h2{font-size:15px;font-weight:500}
                .sub{font-size:13px;color:#888;margin-bottom:1rem}
                .btn{height:36px;padding:0 14px;font-size:13px;border:1px solid #ddd;border-radius:8px;background:#fff;color:#111;cursor:pointer;text-decoration:none;display:inline-flex;align-items:center;gap:6px}
                .btn:hover{background:#f5f5f5}
                @media(prefers-color-scheme:dark){.btn{background:#2a2a2a;border-color:#444;color:#eee}.btn:hover{background:#333}}
            </style>
            </head>
            <body>
            <div class="page">
            <div class="page-title">
                <i class="ti ti-smart-home"></i>
                <h1>Home Assistant</h1>
            </div>
            <div class="card">
                <div class="card-header">
                <i class="ti ti-circle-check"></i>
                <h2>Settings saved</h2>
                </div>
                <p class="sub">Restarting to apply changes&hellip;</p>
                <a href="/mqtt" class="btn"><i class="ti ti-arrow-left" style="font-size:14px"></i>Back to MQTT</a>
            </div>
            </div>
            </body>
            </html>
            )rawliteral");

        delay(1000);
        ESP.restart();
    } else {
        server.send(400, "text/html", R"rawliteral(
            <!DOCTYPE html>
            <html lang="en">
            <head>
            <meta charset="UTF-8">
            <meta name="viewport" content="width=device-width, initial-scale=1.0">
            <title>Error</title>
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
                .card-header{display:flex;align-items:center;gap:10px;margin-bottom:.5rem}
                .card-header i{font-size:18px;color:#a32d2d}
                .card-header h2{font-size:15px;font-weight:500}
                .sub{font-size:13px;color:#888;margin-bottom:1rem}
                .btn{height:36px;padding:0 14px;font-size:13px;border:1px solid #ddd;border-radius:8px;background:#fff;color:#111;cursor:pointer;text-decoration:none;display:inline-flex;align-items:center;gap:6px}
                .btn:hover{background:#f5f5f5}
                @media(prefers-color-scheme:dark){.btn{background:#2a2a2a;border-color:#444;color:#eee}.btn:hover{background:#333}}
            </style>
            </head>
            <body>
            <div class="page">
            <div class="page-title">
                <i class="ti ti-smart-home"></i>
                <h1>Home Assistant</h1>
            </div>
            <div class="card">
                <div class="card-header">
                <i class="ti ti-circle-x"></i>
                <h2>Missing parameters</h2>
                </div>
                <p class="sub">Please fill in all fields and try again.</p>
                <a href="/mqtt" class="btn"><i class="ti ti-arrow-left" style="font-size:14px"></i>Back to MQTT</a>
            </div>
            </div>
            </body>
            </html>
            )rawliteral");
    }
}

void handleMqttConfig() {
    String html = R"rawliteral(
        <!DOCTYPE html>
        <html lang="en">
        <head>
        <meta charset="UTF-8">
        <meta name="viewport" content="width=device-width, initial-scale=1.0">
        <title>MQTT Configuration</title>
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
            .form-group{display:flex;flex-direction:column;gap:6px;margin-bottom:.75rem}
            .form-group label{font-size:13px;color:#666}
            @media(prefers-color-scheme:dark){.form-group label{color:#aaa}}
            .form-group input{font-size:13px;padding:0 10px;height:36px;border:1px solid #ddd;border-radius:8px;background:#fff;color:#111;width:100%}
            @media(prefers-color-scheme:dark){.form-group input{background:#1a1a1a;border-color:#444;color:#eee}}
            .divider{height:1px;background:#f0f0f0;margin:.75rem 0}
            @media(prefers-color-scheme:dark){.divider{background:#333}}
            .btn{height:36px;padding:0 14px;font-size:13px;border:1px solid #ddd;border-radius:8px;background:#fff;color:#111;cursor:pointer;white-space:nowrap;width:100%}
            .btn:hover{background:#f5f5f5}
            @media(prefers-color-scheme:dark){.btn{background:#2a2a2a;border-color:#444;color:#eee}.btn:hover{background:#333}}
            .btn-danger{border-color:#f09595;color:#a32d2d}
            .btn-danger:hover{background:#fcebeb}
            @media(prefers-color-scheme:dark){.btn-danger{border-color:#793333;color:#f09595}.btn-danger:hover{background:#2a1a1a}}
            .back-link{display:flex;align-items:center;gap:6px;font-size:13px;color:#666;text-decoration:none}
            .back-link:hover{color:#111}
            @media(prefers-color-scheme:dark){.back-link{color:#aaa}.back-link:hover{color:#eee}}
            .status-label{font-size:13px;color:#666}
            @media(prefers-color-scheme:dark){.status-label{color:#aaa}}
        </style>
        </head>
        <body>
        <div class="page">
        <div class="page-title">
            <i class="ti ti-smart-home"></i>
            <h1>Home Assistant</h1>
        </div>

        <div class="card">
            <div class="card-header"><i class="ti ti-server"></i><h2>MQTT broker</h2></div>
            <form method="POST" action="/mqtt">
            <div class="form-group">
                <label for="ip">Broker IP address</label>
                <input type="text" id="ip" name="ip" placeholder="e.g. 192.168.1.10" value=")rawliteral" + mqttIP + R"rawliteral(">
            </div>
            <div class="form-group">
                <label for="user">Username</label>
                <input type="text" id="user" name="user" autocomplete="username" value=")rawliteral" + mqttUser + R"rawliteral(">
            </div>
            <div class="form-group" style="margin-bottom:1rem">
                <label for="pass">Password</label>
                <input type="password" id="pass" name="pass" autocomplete="current-password" value=")rawliteral" + mqttPassword + R"rawliteral(">
            </div>
            <button type="submit" class="btn">Save settings</button>
            </form>
            <div class="divider"></div>
            <form id="reset-form" method="GET" action="/mqtt/reset">
            <button type="submit" class="btn btn-danger">Reset MQTT settings</button>
            </form>
        </div>

        <a href="/" class="card back-link">
            <i class="ti ti-arrow-left" style="font-size:16px"></i>
            Back to main page
        </a>
        </div>
        <script>
        document.getElementById('reset-form').addEventListener('submit',function(e){
            if(!confirm('This will erase your MQTT settings. Are you sure?'))e.preventDefault();
        });
        </script>
        </body>
        </html>
        )rawliteral";
    server.send(200, "text/html", html);
}

void handleUpdateFeature() {
    if (server.hasArg("enabled")) {
        String value = server.arg("enabled");
        haloControls = (value == "true");
        preferences.putBool("feature_enabled", haloControls);
    }
    server.send(200, "text/plain", "OK");
}

void handleStatus() {
    String jsonResponse = "{";
    jsonResponse += "\"platform\":\"" + String(platform == PLATFORM_MOZART ? "mozart" : "ase") + "\",";
    jsonResponse += "\"product_ip\":\"" + productIP + "\",";
    jsonResponse += "\"product_connected\":" + String(productConnected() ? "true" : "false") + ",";
    jsonResponse += "\"halo_ip\":\"" + haloIP + "\",";
    jsonResponse += "\"halo_ws_connected\":" + String(haloClient.available() ? "true" : "false") + ",";    
    jsonResponse += "\"firmware\":\"" + String(FIRMWARE_VERSION) + "\",";
    jsonResponse += "\"feature_enabled\": " + String(haloControls ? "true" : "false") + ",";    
    jsonResponse += "\"mqtt_connected\":" + String(mqttConnected ? "true" : "false")+ ",";
    jsonResponse += "\"trigger_source\":\"" + triggerSource + "\"";            
    jsonResponse += "}";

    server.send(200, "application/json", jsonResponse);
}

void sendConfigToHalo() {
    String jsonMessage = "{"
        "\"configuration\": {"
            "\"version\": \"1.0.1\","
            "\"id\": \"ae32d6dd-3300-4725-a6a0-2df6b5f8326f\","
            "\"pages\": ["
                "{"
                    "\"title\": \"Beogram\","
                    "\"id\": \"67461a06-74b6-4114-a808-ab90e8abc03f\","
                    "\"buttons\": ["
                        "{"
                            "\"id\": \"032ed0e4-c61f-4d22-af95-740741217d55\","
                            "\"title\": \"\","
                            "\"subtitle\": \"\","
                            "\"value\": 100,"
                            "\"state\": \"inactive\","
                            "\"content\": { \"text\": \"Prev\" }"
                        "},"
                        "{"
                            "\"id\": \"872b4893-bfdf-4d51-bb53-b5738149fc61\","
                            "\"title\": \"\","
                            "\"subtitle\": \"\","
                            "\"value\": 100,"
                            "\"state\": \"inactive\","
                            "\"content\": { \"text\": \"Play\" }"
                        "},"
                        "{"
                            "\"id\": \"03481fcc-e2cc-47ba-bcae-6152bbf93692\","
                            "\"title\": \"\","
                            "\"subtitle\": \"\","
                            "\"value\": 100,"
                            "\"state\": \"inactive\","
                            "\"content\": { \"text\": \"Next\" }"
                        "}"
                    "]"
                "}"
            "]"
        "}"
    "}";

    haloClient.send(jsonMessage);
    Serial.println("📡 Sent configuration update to Halo");

    if (platform == PLATFORM_MOZART) {
        // Mozart exposes playback state over REST — poll it to sync the Halo button
        if (productIP.length() > 0) sendHttpRequest("/api/v1/playback/state");
    } else {
        // ASE has no equivalent poll — derive the button state from lineInActive
        haloUpdate = STATE;
    }
}

void onMessageCallback(WebsocketsMessage message) {
    //Serial.println("Message from Halo: " + message.data());

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, message.data());

    if (error) {
        Serial.println("JSON parsing failed");
        return;
    }

    // Handle button press events dynamically
    if (doc["event"].is<JsonObject>() && doc["event"]["type"] == "button" && doc["event"]["state"] == "pressed") {
        String buttonID = doc["event"]["id"].as<String>();
        Serial.print("Halo button pressed: ");

        if (buttonID == "872b4893-bfdf-4d51-bb53-b5738149fc61") {
            if (playbackState != PLAYING) {
              Serial.println("PLAY");
              sendHexCommand(PLAY);
            } else {
              Serial.println("STOP");
              sendHexCommand(STOP);
            }
        } else if (buttonID == "032ed0e4-c61f-4d22-af95-740741217d55") {
            Serial.println("PREV");
            sendHexCommand(PREVIOUS);
        } else if (buttonID == "03481fcc-e2cc-47ba-bcae-6152bbf93692") {
            Serial.println("NEXT");
            sendHexCommand(NEXT);
        } else if (buttonID == "03481fcc-e2cc-47ba-bcae-6152bbf93482") {
            Serial.println("STBY");
            sendHexCommand(STANDBY);
        } else {
            Serial.println("Unknown Button");
        }

        if (haloClient.available()) {
            sendButtonUpdate(pendingUpdate.id.c_str(), "inactive", nullptr, nullptr, nullptr, 0);
        }
        
        // Schedule second update (active state) after 500ms
        pendingUpdate.id = buttonID;
        pendingUpdate.pending = true;
        pendingUpdate.timestamp = millis();
    }

    if (haloControls && lineInActive && doc["event"].is<JsonObject>() && doc["event"]["type"] == "system" && doc["event"]["state"] == "active") {
        haloActionTime = millis();  // Store the current time
        haloUpdate = PAGE;
    }
}    

void secondButtonUpdate() {
    if (haloClient.available() && pendingUpdate.pending && millis() - pendingUpdate.timestamp >= haloActionDelay) {
        sendButtonUpdate(pendingUpdate.id.c_str(), "inactive", nullptr, nullptr, nullptr,  100);
        pendingUpdate.pending = false;  // Reset update tracker
    }
}

void connectToHalo() {
    haloClient.poll();    
    if (millis() - haloLastReconnectAttempt > reconnectInterval) {
        haloLastReconnectAttempt = millis(); 
        if (!haloClient.available() && haloIP.length() > 0) {
            Serial.println("🔄 Reconnecting to Halo WebSocket at: " + haloIP);
            if (haloClient.connect(("ws://" + haloIP + ":" + HALO_WEBSOCKET_PORT).c_str())) {
                Serial.println("✅ Reconnected to Beoremote Halo WebSocket!");
                haloClient.onMessage(onMessageCallback);
                if (platform == PLATFORM_MOZART && productIP.length() > 0) {
                    sendHttpRequest("/api/v1/playback/state");
                }
            } else {
                Serial.println("❌ Failed to connect to Halo WebSocket.");
            }
        } 
    }
}

void activateHaloPage() {
    if (haloClient.available() && haloUpdate == PAGE && (millis() - haloActionTime >= haloActionDelay)) {
        haloUpdate = NONE;  
        sendPageUpdate("67461a06-74b6-4114-a808-ab90e8abc03f", "872b4893-bfdf-4d51-bb53-b5738149fc61");
    }

    if (haloClient.available() && haloUpdate == STATE && (millis() - haloActionTime >= haloActionDelay)) {
        if (!lineInActive) {
            haloUpdate = NONE;
            sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Stopped", "Play");
        } else {
            haloUpdate = NONE;
            sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Playing", "Stop");
        }
    }
}

void sendPlayAfterDelay() {
    if (waitingForPlay && millis() - delayPlayAfterDigit >= 1200) {
        sendHexCommand(PLAY);           
        waitingForPlay = false; // Reset flag
        Serial.println("▶️ Sent PLAY after 1200ms delay");
    }
}

void processBuffer(BeogramFeedback state) {
    if (state == PLAYING_FB) {
        playbackState = PLAYING;  
        Serial.println("▶️ Beogram reported ON state.");
        if (mqtt.isConnected()) {
            bgPlaybackState.setValue("Playing");
            bgPlaying.setState(true);
        }
        if (haloClient.available()) {
            sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Playing", "Stop");
        }       
        if (platform == PLATFORM_MOZART) {
            if (!lineInActive) {
                sendHttpRequest("/api/v1/playback/sources/active/" + triggerSource, "POST");
            } else {
                sendHttpRequest("/api/v1/playback/command/play", "POST");
            }
        } else {
            if (!lineInActive) {
                forceSource();
            }
        }
    } else if (state == STOPPED_FB || state == STANDBY_FB) {
        Serial.println(state == STOPPED_FB ? "Beogram reported OFF state." : "Beogram reported STANDBY state.");
        if (mqtt.isConnected()) {
            bgTrack.setValue("-");
            bgPlaybackState.setValue(state == STOPPED_FB ? "Stopped" : "Standby");
            bgPlaying.setState(false);
        }
        if (playbackState == PLAYING && lineInActive) {
            playbackState = STOPPED;
            Serial.println(state == STOPPED_FB ? "⏹️ Beogram has stopped." : "⏹️ Beogram has turned off.");
            if (platform == PLATFORM_MOZART) {
                sendHttpRequest("/api/v1/playback/command/stop", "POST");
            }
            if (haloClient.available()) {
                sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Stopped", "Play", " ");
            }
        }        
    } else if (state == EJECTED_FB) {
        playbackState = STOPPED;
        Serial.println("⏏️ Beogram tray was ejected");
        if (mqtt.isConnected()) {
            bgTrack.setValue("-");
            bgPlaybackState.setValue("Ejected"); 
            bgPlaying.setState(false);    
        }
        if (haloClient.available()) {
            sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Stopped", "Play", "Tray ejected");  
        }            
        if (platform == PLATFORM_MOZART && lineInActive) {
            sendHttpRequest("/api/v1/playback/command/stop", "POST");
        }
    } else if (state == TRACK14_PLUS && playbackState == PLAYING) {
        Serial.print("Track identified: ");
        Serial.println("14+");
        if (mqtt.isConnected()) {        
            bgTrack.setValue("14+");  
        }
        if (haloClient.available()) {
            sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, nullptr, nullptr, "Track 14+");
        }
    } else if (state != UNKNOWN_STATE && playbackState == PLAYING) {
        Serial.print("Track identified: ");
        Serial.println(state, DEC);
        if (haloClient.available()) {
            char subtitle[20];
            sprintf(subtitle, "Track %d", state);
            sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, nullptr, nullptr, subtitle);
        }     
        char trackNumber[20];
        sprintf(trackNumber, "%d", state);
        if (mqtt.isConnected()) {
            bgTrack.setValue(trackNumber);
        }
    } 
}

void handleSerial1Data() {
    static uint8_t buffer[5];
    static size_t bufferIndex = 0;
    static unsigned long lastByteTime = 0;

    while (Serial1.available()) {
        uint8_t receivedByte = Serial1.read();
        unsigned long currentTime = millis();

        if (debugSerial == true) {
          Serial.print("Received byte: 0x");
          Serial.println(receivedByte, HEX);        
        }

        // Store the received byte in the buffer
        buffer[bufferIndex++] = receivedByte;

        lastByteTime = currentTime;

        // Check if we have received 5 bytes
        if (bufferIndex == 5) {
            BeogramFeedback state = identifyState(buffer, bufferIndex);
            processBuffer(state);
            bufferIndex = 0;  // Reset buffer after processing
        }
    }

    // Check if 35 ms have passed since the last byte was received
    if (millis() - lastByteTime > 55 && bufferIndex > 0) {
        BeogramFeedback state = identifyState(buffer, bufferIndex);
        processBuffer(state);
        bufferIndex = 0;  // Reset buffer after processing
    }
}

void handleResetWifi() {
    server.send(200, "text/html", R"rawliteral(
        <!DOCTYPE html>
        <html lang="en">
        <head>
        <meta charset="UTF-8">
        <meta name="viewport" content="width=device-width, initial-scale=1.0">
        <title>WiFi Reset</title>
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
            .card-header{display:flex;align-items:center;gap:10px;margin-bottom:.5rem}
            .card-header i{font-size:18px;color:#a32d2d}
            .card-header h2{font-size:15px;font-weight:500}
            .sub{font-size:13px;color:#888}
        </style>
        </head>
        <body>
        <div class="page">
        <div class="page-title">
            <i class="ti ti-wifi"></i>
            <h1>WiFi</h1>
        </div>
        <div class="card">
            <div class="card-header">
            <i class="ti ti-wifi-off"></i>
            <h2>WiFi settings cleared</h2>
            </div>
            <p class="sub">Restarting in AP mode&hellip; Connect to <strong>BeogramAdaptor</strong> to reconfigure.</p>
        </div>
        </div>
        </body>
        </html>
        )rawliteral");
    wm.resetSettings();
    delay(1000);
    ESP.restart(); 
}

void updateLEDStatus() {
    pixels.clear();
    if (WiFi.status() != WL_CONNECTED) {
        pixels.setPixelColor(0, pixels.Color(0, 2, 0));  // Red for WiFi issue
    } else if (!productConnected()) {
        pixels.setPixelColor(0, pixels.Color(0, 0, 2));  // Blue for product connection issue
    } else {
        pixels.setPixelColor(0, pixels.Color(2, 0, 0));  // Green for everything else
    }
    pixels.show();
}

void onButtonCommand(HAButton* sender)
{
    if (sender == &bgPlay) {
        sendHexCommand(PLAY);  // PLAY
    } else if (sender == &bgNext) {
        sendHexCommand(NEXT);  // NEXT
    } else if (sender == &bgPrev) {
        sendHexCommand(PREVIOUS);  // PREVIOUS
    } else if (sender == &bgStop) {
        sendHexCommand(STOP);  // STOP
    } else if (sender == &bgStandby) {
        sendHexCommand(STANDBY);  // STANDBY
    }
}

void setup() {
    Serial.begin(115200);
    Serial1.begin(320, SERIAL_7N1, RXD2, TXD2, true);

    pixels.begin();
    pixels.clear();
    pixels.setPixelColor(0, pixels.Color(0, 2, 0));  // Red for WiFi issue

    WiFi.mode(WIFI_STA);

    bool res = wm.autoConnect(AP_SSID, AP_PASSWORD);
    if (!res) {
        Serial.println("Failed to connect to WiFi. Retrying...");
        for (int i = 0; i < 5; i++) {
            Serial.println("Retrying WiFi...");
            WiFi.begin();
            delay(5000);
            if (WiFi.status() == WL_CONNECTED) break;
        }
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("Could not connect to WiFi. Restarting ESP32...");
            ESP.restart();
        }
    } else {
        Serial.println("Connected to WiFi!");
    }

    // Disable WiFi modem power save. With power save on, the ESP32 sleeps
    // between beacons and drops many multicast frames — the main cause of
    // mDNS discovery intermittently missing products. Also improves the
    // latency of incoming SSE/websocket/MQTT traffic.
    WiFi.setSleep(false);

    if (!MDNS.begin(DEVICE_NAME)) {
    Serial.println("Error setting up MDNS responder!");
    while (1) {
        delay(1000);
        }
    }
    Serial.println("mDNS responder started");

    // ── Preferences & platform resolution ───────────────────────────
    preferences.begin("beogramadaptor", false);

    String platformStr = preferences.getString("platform", "");
    if (platformStr == "") {
        // Migration from single-platform firmware: infer platform from
        // which legacy IP key is populated, and carry the IP over.
        String legacyWs = preferences.getString("wsIP", "");
        String legacySse = preferences.getString("sseIP", "");
        if (legacyWs.length() > 0) {
            platformStr = "mozart";
            preferences.putString("productIP", legacyWs);
            Serial.println("Migrated Mozart install (wsIP → productIP)");
        } else if (legacySse.length() > 0) {
            platformStr = "ase";
            preferences.putString("productIP", legacySse);
            Serial.println("Migrated ASE install (sseIP → productIP)");
        } else {
            platformStr = "ase";  // fresh install default
        }
        preferences.putString("platform", platformStr);
    }
    platform = (platformStr == "mozart") ? PLATFORM_MOZART : PLATFORM_ASE;
    Serial.println(String("Platform: ") + (platform == PLATFORM_MOZART ? "Mozart" : "ASE"));

    productIP = preferences.getString("productIP", "");
    haloIP = preferences.getString("haloIP", "");
    haloControls = preferences.getBool("feature_enabled", false);
    mqttIP = preferences.getString("mqttIP", "");
    mqttUser = preferences.getString("mqttUser", "");
    mqttPassword = preferences.getString("mqttPassword", "");
    triggerSource = preferences.getString("triggerSource", platform == PLATFORM_MOZART ? "lineIn" : "LINE IN");    

    // ── Home Assistant / MQTT device ────────────────────────────────
    WiFi.macAddress(mac);
    String macSuffix = macToUnderscoreString(mac, sizeof(mac));

    snprintf(idPlay,     sizeof(idPlay),     "beogramPlay_%s",     macSuffix.c_str());
    snprintf(idNext,     sizeof(idNext),     "beogramNext_%s",     macSuffix.c_str());
    snprintf(idPrev,     sizeof(idPrev),     "beogramPrev_%s",     macSuffix.c_str());
    snprintf(idStop,     sizeof(idStop),     "beogramStop_%s",     macSuffix.c_str());
    snprintf(idStandby,  sizeof(idStandby),  "beogramStandby_%s",  macSuffix.c_str());
    snprintf(idTrack,    sizeof(idTrack),    "beogramCDTrack_%s",  macSuffix.c_str());
    snprintf(idPlayback, sizeof(idPlayback), "beogramState_%s",    macSuffix.c_str());
    snprintf(idPlaying,  sizeof(idPlaying),  "beogramPlaying_%s",  macSuffix.c_str());
    snprintf(configUrl,  sizeof(configUrl),  "http://%s/",         WiFi.localIP().toString().c_str());
    
    // Unified HA device unique ID: raw MAC bytes (canonical ArduinoHA
    // pattern, hex-encoded by the library). The legacy Mozart firmware
    // passed the MAC-suffix *string* as bytes instead, so devices
    // upgrading from Moz will re-register once under the new ID; the
    // stale device can be removed from the MQTT integration in HA.
    device.setUniqueId(mac, sizeof(mac));

    device.setConfigurationUrl(configUrl);
    device.setName("BeogramAdaptor");
    device.setSoftwareVersion(FIRMWARE_VERSION);
    device.enableSharedAvailability();
    device.enableLastWill();    
    bgPlay.setIcon("mdi:play-circle");
    bgPlay.setName("Play");
    bgNext.setIcon("mdi:skip-next-circle");
    bgNext.setName("Next");
    bgPrev.setIcon("mdi:skip-previous-circle");
    bgPrev.setName("Prev");  
    bgStop.setIcon("mdi:stop-circle");
    bgStop.setName("Stop");
    bgStandby.setIcon("mdi:power-standby");
    bgStandby.setName("Standby"); 
    bgTrack.setIcon("mdi:music-note-eighth");
    bgTrack.setName("Track");  
    bgPlaybackState.setIcon("mdi:album");
    bgPlaybackState.setName("State");
    bgPlaying.setDeviceClass("running");
    bgPlaying.setName("Playing");
    bgPlaying.setIcon("mdi:disc-player");      
    mqtt.setDiscoveryPrefix("homeassistant");

    bgPlay.onCommand(onButtonCommand);
    bgNext.onCommand(onButtonCommand);
    bgPrev.onCommand(onButtonCommand);
    bgStop.onCommand(onButtonCommand);
    bgStandby.onCommand(onButtonCommand);      

    // ── Mozart product websockets ───────────────────────────────────
    wsClient.onMessage([](WebsocketsMessage msg) { processWebSocketMessage(msg.data()); });
    wsClient.onEvent([](WebsocketsEvent event, String data) {
        if (event == WebsocketsEvent::ConnectionOpened) {
            wsLastPingReceived = millis();
            Serial.println("Websocket connected");
        } else if (event == WebsocketsEvent::ConnectionClosed) {
            Serial.println("Websocket closed");
        } else if (event == WebsocketsEvent::GotPing || event == WebsocketsEvent::GotPong) {
            wsLastPingReceived = millis();
        }
    });

    remoteClient.onMessage([](WebsocketsMessage msg) { processRemoteWebSocketMessage(msg.data()); });
    remoteClient.onEvent([](WebsocketsEvent event, String data) {
        if (event == WebsocketsEvent::ConnectionOpened) {
            Serial.println("Secondary websocket connected");
            wsRemoteLastPingReceived = millis();
        } else if (event == WebsocketsEvent::ConnectionClosed) {
            Serial.println("Secondary websocket closed");
        } else if (event == WebsocketsEvent::GotPing || event == WebsocketsEvent::GotPong) {
            wsRemoteLastPingReceived = millis();
        }
    });

    if (platform == PLATFORM_MOZART && productIP.length() > 0) {
        wsClient.connect(("ws://" + productIP + ":" + WEBSOCKET_PORT).c_str());
        remoteClient.connect(("ws://" + productIP + ":" + WEBSOCKET_PORT + "/remoteControl").c_str());
    }

    // ── Beoremote Halo (shared) ─────────────────────────────────────
    haloClient.onMessage(onMessageCallback); 
    haloClient.onEvent([](WebsocketsEvent event, String data) {
        if (event == WebsocketsEvent::ConnectionOpened) {
            haloLastPingReceived = millis();
            Serial.println("Halo Websocket connected");
            sendConfigToHalo();
        } else if (event == WebsocketsEvent::ConnectionClosed) {
            Serial.println("Halo Websocket closed");
        } else if (event == WebsocketsEvent::GotPing || event == WebsocketsEvent::GotPong) {
            haloLastPingReceived = millis();
        }
    });
  
    if (haloIP.length() > 0) {
        haloClient.connect(("ws://" + haloIP + ":" + HALO_WEBSOCKET_PORT).c_str());
    }

    // ── Web server routes ───────────────────────────────────────────
    server.on("/update-source", HTTP_GET, handleUpdateTriggerSource);
    server.on("/update-platform", HTTP_GET, handleUpdatePlatform);
    server.on("/discover", HTTP_GET, handleDiscover);
    server.on("/discover-halo", HTTP_GET, handleDiscoverHalo);

    server.on("/settings/reset-wifi", HTTP_GET, handleResetWifi);

    server.on("/mqtt", HTTP_GET, handleMqttConfig);
    server.on("/mqtt", HTTP_POST, handleMqttUpdate);
    server.on("/mqtt/reset", HTTP_GET, handleMqttReset);    

    server.on("/command/play", HTTP_POST, []() {
        sendHexCommand(PLAY);  // PLAY
        server.send(200, "application/json", "{\"status\":\"Play command sent\"}");
    });

    server.on("/command/stop", HTTP_POST, []() {
        sendHexCommand(STOP);  // STOP
        server.send(200, "application/json", "{\"status\":\"Stop command sent\"}");
    });

    server.on("/command/next", HTTP_POST, []() {
        sendHexCommand(NEXT);  // NEXT
        server.send(200, "application/json", "{\"status\":\"Next command sent\"}");
    });

    server.on("/command/prev", HTTP_POST, []() {
        sendHexCommand(PREVIOUS);  // PREVIOUS
        server.send(200, "application/json", "{\"status\":\"Previous command sent\"}");
    });

    server.on("/command/standby", HTTP_POST, []() {
        sendHexCommand(STANDBY);  // STANDBY
        server.send(200, "application/json", "{\"status\":\"Standby command sent\"}");
    });


    server.on("/", handleRoot);
    server.on("/update", HTTP_GET, handleUpdate);
    server.on("/update-halo", HTTP_GET, handleUpdateHalo);
    server.on("/update-feature", HTTP_GET, handleUpdateFeature);
    server.on("/status", handleStatus);
    server.on("/update-ota", HTTP_POST, []() {
        server.send(200, "text/plain", (Update.hasError()) ? "Update Failed!" : "Update Successful! Rebooting...");
        delay(1000);
        ESP.restart();
    }, handleOTAUpdate);
    server.begin();

    // Only send greeting if connection was actually established
    if (platform == PLATFORM_MOZART && productIP.length() > 0 && wsClient.available()) {
        wsClient.send("Hi Server!");
    }

    MDNS.addService("http", "tcp", 80);

    checkMQTTConnection(true);  // Force immediate connect attempt

    if (platform == PLATFORM_ASE && productIP.length() > 0) {
        connectToSSE();
    }
}

void loop() {
    updateLEDStatus();

    if (platform == PLATFORM_MOZART) {
        wsClient.poll();
        remoteClient.poll();
    } else {
        if (productIP.length() > 0) {
            checkSSEConnection();
        }
        readSSE();
    }

    connectToHalo();
    checkPingWebsocket();
    handleSerial1Data();
    server.handleClient();
    checkWiFiConnection();
    sendPlayAfterDelay();
    secondButtonUpdate();
    activateHaloPage();
    mqtt.loop();
    checkMQTTConnection();    

    if (Serial.available() > 0) {
        String input = Serial.readStringUntil('\n');
        input.trim();
        
        if (input == "debug 1") {
            debugSerial = true;
            Serial.println("Debug mode enabled");
        } else if (input == "debug 0") {
            debugSerial = false;
            Serial.println("Debug mode disabled");
        }
    }
}
