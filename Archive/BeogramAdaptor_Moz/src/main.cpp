#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <ArduinoWebsockets.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <Update.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoHA.h>

#define RXD2 16
#define TXD2 17
#define LEDPIN 47
#define NUMPIXELS 1
#define FIRMWARE_VERSION "MOZ.2026.7.5"

bool debugSerial = false;

const int WEBSOCKET_PORT = 9339;
const int HALO_WEBSOCKET_PORT = 8080;
const char* DEVICE_NAME = "Beogram";
const char* AP_SSID = "BeogramAdaptor";
const char* AP_PASSWORD = "password";

const unsigned long reconnectInterval = 10000;
static unsigned long wsLastReconnectAttempt = millis();  
static unsigned long haloLastReconnectAttempt = millis();
static unsigned long mqttLastReconnectAttempt = millis();
static unsigned long wsLastPingReceived = millis();
static unsigned long wsRemoteLastPingReceived = millis();
static unsigned long haloLastPingReceived = millis();
const unsigned long pingTimeout = 10000;

unsigned long haloActionTime = 0;
const unsigned long haloActionDelay = 800;

unsigned long lastStartEventTime = 0;
unsigned long delayPlayAfterDigit = 0;

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
String wsIP;
String haloIP;
String triggerSource = "lineIn";

String mqttIP;
String mqttUser;
String mqttPassword;

WiFiManager wm;
using namespace websockets;
WebsocketsClient client;
WebsocketsClient remoteClient;
WebsocketsClient haloClient;
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
        .btn-danger{border-color:#f09595;color:#a32d2d}
        .btn-danger:hover{background:#fcebeb}
        @media(prefers-color-scheme:dark){.btn-danger{border-color:#793333;color:#f09595}.btn-danger:hover{background:#2a1a1a}}
        .select-row{display:flex;align-items:center;justify-content:space-between;gap:1rem}
        .select-row label{font-size:13px;color:#666}
        @media(prefers-color-scheme:dark){.select-row label{color:#aaa}}
        select{height:34px;padding:0 10px;font-size:13px;border:1px solid #ddd;border-radius:8px;background:#fff;color:#111}
        @media(prefers-color-scheme:dark){select{background:#1a1a1a;border-color:#444;color:#eee}}
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
        .action-row{display:flex;gap:8px;margin-top:.75rem;flex-wrap:wrap}
    </style>
    </head>
    <body>
    <div class="page">
    <div class="page-title">
        <i class="ti ti-disc"></i>
        <h1>Beogram Adaptor</h1>
    </div>

    <div class="card">
        <div class="card-header"><i class="ti ti-device-speaker"></i><h2>Mozart product</h2></div>
        <div class="status-row">
        <span class="status-label">IP address</span>
        <span class="ip-chip" id="ws-ip">Loading...</span>
        </div>
        <div class="status-row" style="margin-bottom:0">
        <span class="status-label">WebSocket</span>
        <span class="badge disconnected" id="ws-status"><i class="ti ti-circle"></i>Disconnected</span>
        </div>
        <div class="divider"></div>
        <div class="select-row">
        <label for="sourceSelect">Input source</label>
        <select id="sourceSelect">
            <option value="lineIn">Line-In (default)</option>
            <option value="spdif">Optical</option>
        </select>
        </div>
        <div class="form-group" id="ws-connect-form">
        <label for="wsIP">Product IP address</label>
        <div class="input-row">
            <input type="text" id="wsIP" placeholder="e.g. 192.168.1.42">
            <button class="btn" id="ws-connect-btn">Connect</button>
        </div>
        <span class="error-text" id="wsIP-error">Invalid IP address</span>
        </div>
        <div class="action-row" id="ws-action-row" style="display:none">
        <button class="btn btn-danger" id="ws-unlink-btn">Unlink product</button>
        </div>
    </div>

    <div class="card">
        <div class="card-header"><i class="ti ti-remote"></i><h2>Beoremote Halo</h2></div>
        <div class="status-row">
        <span class="status-label">IP address</span>
        <span class="ip-chip" id="halo-ip">Loading...</span>
        </div>
        <div class="status-row" style="margin-bottom:0">
        <span class="status-label">WebSocket</span>
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
        <label for="haloIP">Halo IP address</label>
        <div class="input-row">
            <input type="text" id="haloIP" placeholder="e.g. 192.168.1.55">
            <button class="btn" id="halo-connect-btn">Connect</button>
        </div>
        <span class="error-text" id="haloIP-error">Invalid IP address</span>
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
    function validateIP(ip){
    let p=ip.split('.');
    if(p.length!==4)return false;
    return p.every(x=>{let n=parseInt(x,10);return n>=0&&n<=255&&x===n.toString()});
    }

    function setBadge(el,connected){
    el.className='badge '+(connected?'connected':'disconnected');
    el.innerHTML=connected?'<i class="ti ti-circle-filled"></i>Connected':'<i class="ti ti-circle"></i>Disconnected';
    }

    function updateStatus(){
    fetch('/status').then(r=>r.json()).then(d=>{
        setBadge(document.getElementById('ws-status'),d.ws_connected);
        setBadge(document.getElementById('halo-ws-status'),d.halo_ws_connected);
        setBadge(document.getElementById('mqtt-status'),d.mqtt_connected);
        document.getElementById('fw-version').textContent=d.firmware;
        document.getElementById('featureToggle').checked=d.feature_enabled;
        document.getElementById('sourceSelect').value=d.trigger_source;

        let hasWs=d.ws_ip&&d.ws_ip!=='';
        document.getElementById('ws-ip').textContent=hasWs?d.ws_ip:'—';
        document.getElementById('ws-connect-form').style.display=hasWs?'none':'flex';
        document.getElementById('ws-action-row').style.display=hasWs?'flex':'none';

        let hasHalo=d.halo_ip&&d.halo_ip!=='';
        document.getElementById('halo-ip').textContent=hasHalo?d.halo_ip:'—';
        document.getElementById('halo-connect-form').style.display=hasHalo?'none':'flex';
        document.getElementById('halo-action-row').style.display=hasHalo?'flex':'none';
    }).catch(()=>{});
    }

    document.getElementById('ws-connect-btn').addEventListener('click',function(){
    let ip=document.getElementById('wsIP').value;
    let err=document.getElementById('wsIP-error');
    if(!validateIP(ip)){err.style.display='block';return;}
    err.style.display='none';
    fetch('/update?wsIP='+encodeURIComponent(ip)).then(updateStatus);
    });

    document.getElementById('ws-unlink-btn').addEventListener('click',function(){
    fetch('/update?wsIP=').then(updateStatus);
    });

    document.getElementById('halo-connect-btn').addEventListener('click',function(){
    let ip=document.getElementById('haloIP').value;
    let err=document.getElementById('haloIP-error');
    if(!validateIP(ip)){err.style.display='block';return;}
    err.style.display='none';
    fetch('/update-halo?haloIP='+encodeURIComponent(ip)).then(updateStatus);
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

ButtonUpdate pendingUpdate = {"", false, 0};

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

// Non-blocking WiFi reconnection
void checkWiFiConnection() {
    static unsigned long lastWifiAttempt = 0;
    if (WiFi.status() != WL_CONNECTED && millis() - lastWifiAttempt > 10000) {
        lastWifiAttempt = millis();
        Serial.println("WiFi lost, attempting to reconnect...");
        WiFi.reconnect();
    }
}

// Reconnect both WebSockets only if not already connected
void checkWebSocketConnection() {
    if (millis() - wsLastReconnectAttempt > reconnectInterval) {
        wsLastReconnectAttempt = millis();
        if (!client.available()) {
            Serial.println("Reconnecting product websocket...");
            if (client.connect(("ws://" + wsIP + ":" + WEBSOCKET_PORT).c_str())) {
                client.send("Hi Server!");
                Serial.println("Product webSocket reconnected!");
            } else {
                Serial.println("Product webSocket reconnection failed.");
            }
        }
        if (!remoteClient.available()) {
            Serial.println("Reconnecting remote websocket...");
            if (remoteClient.connect(("ws://" + wsIP + ":" + WEBSOCKET_PORT + "/remoteControl").c_str())) {
                remoteClient.send("Hi Server!");
                Serial.println("Secondary websocket reconnected!");
            } else {
                Serial.println("Remote webSocket reconnection failed.");
            }
        }
    }
}

void checkPingWebsocket() {
    if (client.available() && wsIP.length() > 0) {    
        if (millis() - wsLastPingReceived >= pingTimeout) {
            client.ping();
            wsLastPingReceived = millis();
        }
    } else if (!client.available() && wsIP.length() > 0) {    
        if (millis() - wsLastPingReceived >= pingTimeout) {
            client.close();
            checkWebSocketConnection();
        }
    }

    if (remoteClient.available() && wsIP.length() > 0) {    
        if (millis() - wsRemoteLastPingReceived >= pingTimeout) {
            remoteClient.ping();
            wsRemoteLastPingReceived = millis();
        }
    } else if (!remoteClient.available() && wsIP.length() > 0) {    
        if (millis() - wsRemoteLastPingReceived >= pingTimeout) {
            remoteClient.close();
            checkWebSocketConnection();
        }
    }

    if (haloClient.available()) {
        if (millis() - haloLastPingReceived >= pingTimeout) {
            haloClient.ping();
            haloLastPingReceived = millis();
        }
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
    mqttConnected = mqtt.isConnected();
}

void handleRoot() {
    server.send(200, "text/html", htmlPage);
}

void handleOTAUpdate() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("OTA Update Start: %s\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        Serial.printf("Writing %d bytes...\n", upload.currentSize);
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            Serial.printf("OTA Update Success! %d bytes\n", upload.totalSize);
        } else {
            Update.printError(Serial);
        }
    }
}

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
        String url = "http://" + wsIP + endpoint;
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

void handleUpdate() {
    if (server.hasArg("wsIP")) {
        String newIP = server.arg("wsIP");
        if (newIP == "") {
            client.close();
            remoteClient.close();
            wsIP = newIP;
            preferences.putString("wsIP", wsIP);
            Serial.println("Unlinked product."); 
            return;
        } else if (!isValidIPAddress(newIP)) {
            server.send(400, "text/html", "<h2>Invalid IP Address</h2><a href='/'>Go Back</a>");
            Serial.println("Invalid IP Address - not saved."); 
            return;
        }
        wsIP = newIP;
        preferences.putString("wsIP", wsIP);
        client.close();
        remoteClient.close();
        client.connect(("ws://" + wsIP + ":" + WEBSOCKET_PORT).c_str());
        remoteClient.connect(("ws://" + wsIP + ":" + WEBSOCKET_PORT + "/remoteControl").c_str());
        server.send(200, "text/html", "<h2>IP Updated to " + wsIP + "</h2><a href='/'>Go Back</a>");
        if (client.available()) {
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
        server.send(400, "text/html", "<h2>No IP Address Provided</h2><a href='/'>Go Back</a>");
    }
}

void handleUpdateHalo() {
    if (server.hasArg("haloIP")) {
        String newHaloIP = server.arg("haloIP");
        if (newHaloIP == "") {
            haloClient.close();
            haloIP = newHaloIP;
            preferences.putString("haloIP", haloIP);
            Serial.println("Unlinked Halo."); 
            return;
        } else if (!isValidIPAddress(newHaloIP)) {
            server.send(400, "text/html", "<h2>Invalid IP Address for Beoremote Halo</h2><a href='/'>Go Back</a>");
            Serial.println("Invalid IP Address - not saved."); 
            return;
        }
        haloIP = newHaloIP;
        preferences.putString("haloIP", haloIP);
        haloClient.close();
        haloClient.connect(("ws://" + haloIP + ":8080").c_str());
        server.send(200, "text/html", "<h2>Halo IP Updated to " + haloIP + "</h2><a href='/'>Go Back</a>");
    } else {
        server.send(400, "text/html", "<h2>No Halo IP Address Provided</h2><a href='/'>Go Back</a>");
    }
}

void handleUpdateTriggerSource() {
    if (server.hasArg("source")) {
        String newSource = server.arg("source");
        if (newSource == "lineIn" || newSource == "spdif") {
            triggerSource = newSource;
            preferences.putString("triggerSource", triggerSource);
            server.send(200, "text/plain", "Source updated");
            return;
        }
    }
    server.send(400, "text/plain", "Invalid source");
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
    jsonResponse += "\"ws_ip\":\"" + wsIP + "\",";
    jsonResponse += "\"ws_connected\":" + String(client.available() ? "true" : "false") + ",";
    jsonResponse += "\"halo_ip\":\"" + haloIP + "\",";
    jsonResponse += "\"halo_ws_connected\":" + String(haloClient.available() ? "true" : "false") + ",";    
    jsonResponse += "\"firmware\":\"" + String(FIRMWARE_VERSION) + "\",";
    jsonResponse += "\"feature_enabled\": " + String(haloControls ? "true" : "false") + ",";    
    jsonResponse += "\"mqtt_connected\":" + String(mqttConnected ? "true" : "false")+ ",";
    jsonResponse += "\"trigger_source\":\"" + triggerSource + "\"";
    jsonResponse += "}";
    server.send(200, "application/json", jsonResponse);
}

void sendHexCommand(BeogramCommand command) {
    Serial1.write(command);
    delayMicroseconds(49991);
    Serial1.write(command);
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
    sendHttpRequest("/api/v1/playback/state");
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

void sendPlayAfterDelay() {
    if (waitingForPlay && millis() - delayPlayAfterDigit >= 1200) {
        sendHexCommand(PLAY);
        waitingForPlay = false;
        Serial.println("▶️ Sent PLAY after 1200ms delay");
    }
}

void processBuffer(BeogramFeedback state) {
    if (state == PLAYING_FB) {
        Serial.println("▶️ Beogram reported ON state.");
        playbackState = PLAYING;
        if (mqtt.isConnected()) {
            bgPlaybackState.setValue("Playing");
            bgPlaying.setState(true);
        }
        if (haloClient.available()) {
            sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Playing", "Stop");
        }
        if (!lineInActive) {
            sendHttpRequest("/api/v1/playback/sources/active/" + triggerSource, "POST");
        } else {
            sendHttpRequest("/api/v1/playback/command/play", "POST");
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
            sendHttpRequest("/api/v1/playback/command/stop", "POST");
            if (haloClient.available()) {
                sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Stopped", "Play", " ");
            }
        }
    } else if (state == EJECTED_FB) {
        Serial.println("⏏️ Beogram tray was ejected");
        if (mqtt.isConnected()) {
            bgTrack.setValue("-");
            bgPlaybackState.setValue("Ejected");
            bgPlaying.setState(false);
        }
        playbackState = STOPPED;
        if (haloClient.available()) {
            sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Stopped", "Play", "Tray ejected");
        }
        if (lineInActive) sendHttpRequest("/api/v1/playback/command/stop", "POST");
    } else if (state == TRACK14_PLUS && playbackState == PLAYING) {
        Serial.println("Track identified: 14+");
        if (mqtt.isConnected()) bgTrack.setValue("14+");
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
        if (mqtt.isConnected()) bgTrack.setValue(trackNumber);
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
        buffer[bufferIndex++] = receivedByte;
        lastByteTime = currentTime;
        if (bufferIndex == 5) {
            BeogramFeedback state = identifyState(buffer, bufferIndex);
            processBuffer(state);
            bufferIndex = 0;
        }
    }
    if (millis() - lastByteTime > 55 && bufferIndex > 0) {
        BeogramFeedback state = identifyState(buffer, bufferIndex);
        processBuffer(state);
        bufferIndex = 0;
    }
}

void updateLEDStatus() {
    pixels.clear();
    if (WiFi.status() != WL_CONNECTED) {
        pixels.setPixelColor(0, pixels.Color(0, 2, 0));
    } else if (!client.available()) {
        pixels.setPixelColor(0, pixels.Color(0, 0, 2));
    } else {
        pixels.setPixelColor(0, pixels.Color(2, 0, 0));
    }
    pixels.show();
}

void onMessageCallback(WebsocketsMessage message) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, message.data());
    if (error) {
        Serial.println("JSON parsing failed");
        return;
    }
    if (doc["event"].is<JsonObject>() && doc["event"]["type"] == "button" && doc["event"]["state"] == "pressed") {
        String buttonID = doc["event"]["id"].as<String>();
        Serial.print("Halo button pressed: ");
        if (buttonID == "872b4893-bfdf-4d51-bb53-b5738149fc61") {
            if (playbackState != PLAYING) { Serial.println("PLAY"); sendHexCommand(PLAY); }
            else { Serial.println("STOP"); sendHexCommand(STOP); }
        } else if (buttonID == "032ed0e4-c61f-4d22-af95-740741217d55") {
            Serial.println("PREV"); sendHexCommand(PREVIOUS);
        } else if (buttonID == "03481fcc-e2cc-47ba-bcae-6152bbf93692") {
            Serial.println("NEXT"); sendHexCommand(NEXT);
        } else if (buttonID == "03481fcc-e2cc-47ba-bcae-6152bbf93482") {
            Serial.println("STBY"); sendHexCommand(STANDBY);
        } else {
            Serial.println("Unknown Button");
        }
        if (haloClient.available()) {
            sendButtonUpdate(pendingUpdate.id.c_str(), "inactive", nullptr, nullptr, nullptr, 0);
        }
        pendingUpdate.id = buttonID;
        pendingUpdate.pending = true;
        pendingUpdate.timestamp = millis();
    }
    if (haloControls && lineInActive && doc["event"].is<JsonObject>() && doc["event"]["type"] == "system" && doc["event"]["state"] == "active") {
        haloActionTime = millis();
        haloUpdate = PAGE;
    }
}

void secondButtonUpdate() {
    if (haloClient.available() && pendingUpdate.pending && millis() - pendingUpdate.timestamp >= haloActionDelay) {
        sendButtonUpdate(pendingUpdate.id.c_str(), "inactive", nullptr, nullptr, nullptr, 100);
        pendingUpdate.pending = false;
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
                sendHttpRequest("/api/v1/playback/state");
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
        haloUpdate = NONE;
        if (!lineInActive) {
            sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Stopped", "Play");
        } else {
            sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Playing", "Stop");
        }
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

void onButtonCommand(HAButton* sender) {
    if (sender == &bgPlay) sendHexCommand(PLAY);
    else if (sender == &bgNext) sendHexCommand(NEXT);
    else if (sender == &bgPrev) sendHexCommand(PREVIOUS);
    else if (sender == &bgStop) sendHexCommand(STOP);
    else if (sender == &bgStandby) sendHexCommand(STANDBY);
}

void setup() {
    Serial.begin(115200);
    Serial1.begin(320, SERIAL_7N1, RXD2, TXD2, true);

    pixels.begin();
    pixels.clear();
    pixels.setPixelColor(0, pixels.Color(0, 2, 0));

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

    if (!MDNS.begin(DEVICE_NAME)) {
        Serial.println("Error setting up MDNS responder!");
        while (1) { delay(1000); }
    }
    Serial.println("mDNS responder started");

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

    device.setUniqueId((const byte*)macSuffix.c_str(), macSuffix.length());
    device.setConfigurationUrl(configUrl);
    device.setName("BeogramAdaptor");
    device.setSoftwareVersion(FIRMWARE_VERSION);
    device.enableSharedAvailability();
    device.enableLastWill();
    bgPlay.setIcon("mdi:play-circle");     bgPlay.setName("Play");
    bgNext.setIcon("mdi:skip-next-circle"); bgNext.setName("Next");
    bgPrev.setIcon("mdi:skip-previous-circle"); bgPrev.setName("Previous");
    bgStop.setIcon("mdi:stop-circle");     bgStop.setName("Stop");
    bgStandby.setIcon("mdi:power-standby"); bgStandby.setName("Standby");
    bgTrack.setIcon("mdi:music-note-eighth"); bgTrack.setName("Track");
    bgPlaybackState.setIcon("mdi:album");  bgPlaybackState.setName("State");
    bgPlaying.setDeviceClass("running");   bgPlaying.setName("Playing");
    bgPlaying.setIcon("mdi:disc-player");
    mqtt.setDiscoveryPrefix("homeassistant");

    bgPlay.onCommand(onButtonCommand);
    bgNext.onCommand(onButtonCommand);
    bgPrev.onCommand(onButtonCommand);
    bgStop.onCommand(onButtonCommand);
    bgStandby.onCommand(onButtonCommand);

    preferences.begin("beogramadaptor", false);
    wsIP = preferences.getString("wsIP", "");
    haloIP = preferences.getString("haloIP", "");
    haloControls = preferences.getBool("feature_enabled", false);
    mqttIP = preferences.getString("mqttIP", "");
    mqttUser = preferences.getString("mqttUser", "");
    mqttPassword = preferences.getString("mqttPassword", "");
    triggerSource = preferences.getString("triggerSource", "lineIn");

    client.onMessage([](WebsocketsMessage msg) { processWebSocketMessage(msg.data()); });
    client.onEvent([](WebsocketsEvent event, String data) {
        if (event == WebsocketsEvent::ConnectionOpened) {
            wsLastPingReceived = millis();
            Serial.println("Websocket connected");
        } else if (event == WebsocketsEvent::ConnectionClosed) {
            Serial.println("Websocket closed");
        } else if (event == WebsocketsEvent::GotPing || event == WebsocketsEvent::GotPong) {
            wsLastPingReceived = millis();
        }
    });

    if (wsIP.length() > 0) {
        client.connect(("ws://" + wsIP + ":" + WEBSOCKET_PORT).c_str());
    }

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
    if (wsIP.length() > 0) {
        remoteClient.connect(("ws://" + wsIP + ":" + WEBSOCKET_PORT + "/remoteControl").c_str());
    }

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

    server.on("/update-source", HTTP_GET, handleUpdateTriggerSource);
    server.on("/settings/reset-wifi", HTTP_GET, handleResetWifi);
    server.on("/mqtt", HTTP_GET, handleMqttConfig);
    server.on("/mqtt", HTTP_POST, handleMqttUpdate);
    server.on("/mqtt/reset", HTTP_GET, handleMqttReset);

    server.on("/command/play", HTTP_POST, []() {
        sendHexCommand(PLAY);
        server.send(200, "application/json", "{\"status\":\"Play command sent\"}");
    });
    server.on("/command/stop", HTTP_POST, []() {
        sendHexCommand(STOP);
        server.send(200, "application/json", "{\"status\":\"Stop command sent\"}");
    });
    server.on("/command/next", HTTP_POST, []() {
        sendHexCommand(NEXT);
        server.send(200, "application/json", "{\"status\":\"Next command sent\"}");
    });
    server.on("/command/prev", HTTP_POST, []() {
        sendHexCommand(PREVIOUS);
        server.send(200, "application/json", "{\"status\":\"Previous command sent\"}");
    });
    server.on("/command/standby", HTTP_POST, []() {
        sendHexCommand(STANDBY);
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
    if (wsIP.length() > 0 && client.available()) {
        client.send("Hi Server!");
    }

    MDNS.addService("http", "tcp", 80);
    checkMQTTConnection(true);
}

void loop() {
    updateLEDStatus();
    client.poll();
    remoteClient.poll();
    checkWiFiConnection();
    connectToHalo();
    checkPingWebsocket();
    handleSerial1Data();
    server.handleClient();
    sendPlayAfterDelay();
    secondButtonUpdate();
    activateHaloPage();
    mqtt.loop();
    checkMQTTConnection();
    if (Serial.available() > 0) {
        String input = Serial.readStringUntil('\n');
        input.trim();
        if (input == "debug 1") { debugSerial = true; Serial.println("Debug mode enabled"); }
        else if (input == "debug 0") { debugSerial = false; Serial.println("Debug mode disabled"); }
    }
}