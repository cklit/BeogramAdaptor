#include "webui.h"
#include "webpage.h"
#include "beogram.h"
#include "transport.h"
#include "halo.h"
#include "ha_mqtt.h"
#include "discovery.h"
#include <WiFi.h>
#include <WiFiManager.h>

extern WiFiManager wm;  // defined in main.cpp
#include <Update.h>
#include <ArduinoJson.h>

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
            productSerial = "";
            preferences.putString("productIP", productIP);
            preferences.putString("productSerial", productSerial);
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
        // Serial number only accompanies discovery-based links; manual IP
        // entry sends none, which intentionally clears any old one.
        productSerial = server.hasArg("productSerial") ? server.arg("productSerial") : "";
        preferences.putString("productSerial", productSerial);

        if (platform == PLATFORM_MOZART) {
            wsClient.close();
            remoteClient.close();
            wsClient.connect(("ws://" + productIP + ":" + WEBSOCKET_PORT).c_str());
            remoteClient.connect(("ws://" + productIP + ":" + WEBSOCKET_PORT + "/remoteControl").c_str());
            server.send(200, "text/html", "<h2>IP Updated to " + productIP + "</h2><a href='/'>Go Back</a>");
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
            haloSerial = "";
            preferences.putString("haloIP", haloIP); // Store the Halo IP in preferences
            preferences.putString("haloSerial", haloSerial);
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
        // Serial only accompanies discovery-based links; manual IP entry
        // sends none, which intentionally clears any old one.
        haloSerial = server.hasArg("haloSerial") ? server.arg("haloSerial") : "";
        preferences.putString("haloSerial", haloSerial);

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
            preferences.putString("productSerial", newIP != "" && server.hasArg("productSerial") ? server.arg("productSerial") : "");
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

void handleUpdateDeviceType() {
    if (server.hasArg("type")) {
        String value = server.arg("type");
        if (value == "cd" || value == "record" || value == "tape") {
            deviceType = (value == "record") ? DEVICE_RECORD
                       : (value == "tape")   ? DEVICE_TAPE
                                             : DEVICE_CD;
            preferences.putString("deviceType", value);
            // The Halo layout differs between the two, so push the new
            // configuration straight away — no restart needed.
            if (haloClient.available()) sendConfigToHalo();
            server.send(200, "text/plain", "OK");
            return;
        }
    }
    server.send(400, "text/plain", "Invalid device type");
}

void handleStatus() {
    String jsonResponse = "{";
    jsonResponse += "\"platform\":\"" + String(platform == PLATFORM_MOZART ? "mozart" : "ase") + "\",";
    jsonResponse += "\"product_ip\":\"" + productIP + "\",";
    jsonResponse += "\"product_serial\":\"" + productSerial + "\",";
    jsonResponse += "\"beogram_state\":\"" + beogramStateText + "\",";
    jsonResponse += "\"beogram_track\":\"" + beogramTrack + "\",";
    jsonResponse += String("\"beogram_playing\":") + (beogramPlaying ? "true" : "false") + ",";
    jsonResponse += "\"product_connected\":" + String(productConnected() ? "true" : "false") + ",";
    jsonResponse += "\"halo_ip\":\"" + haloIP + "\",";
    jsonResponse += "\"halo_serial\":\"" + haloSerial + "\",";
    jsonResponse += "\"halo_ws_connected\":" + String(haloClient.available() ? "true" : "false") + ",";    
    jsonResponse += "\"firmware\":\"" + String(FIRMWARE_VERSION) + "\",";
    jsonResponse += "\"device_type\":\"" + String(deviceType == DEVICE_RECORD ? "record" : deviceType == DEVICE_TAPE ? "tape" : "cd") + "\",";
    jsonResponse += "\"feature_enabled\": " + String(haloControls ? "true" : "false") + ",";    
    jsonResponse += "\"mqtt_connected\":" + String(mqttConnected ? "true" : "false")+ ",";
    jsonResponse += "\"trigger_source\":\"" + triggerSource + "\"";            
    jsonResponse += "}";

    server.send(200, "application/json", jsonResponse);
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
            <p class="sub">Restarting in AP mode&hellip; Connect to <strong>BGAdaptor</strong> to reconfigure.</p>
        </div>
        </div>
        </body>
        </html>
        )rawliteral");
    wm.resetSettings();
    delay(1000);
    ESP.restart(); 
}

void registerWebRoutes() {
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
    server.on("/update-devicetype", HTTP_GET, handleUpdateDeviceType);
    server.on("/status", handleStatus);
    server.on("/update-ota", HTTP_POST, []() {
        server.send(200, "text/plain", (Update.hasError()) ? "Update Failed!" : "Update Successful! Rebooting...");
        delay(1000);
        ESP.restart();
    }, handleOTAUpdate);
    server.begin();
}
