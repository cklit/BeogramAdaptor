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
#define FIRMWARE_VERSION "MOZ.2025.3.30"

bool debugSerial = false; // set to true to print all incoming serial commands from Beogram

const int WEBSOCKET_PORT = 9339;
const int HALO_WEBSOCKET_PORT = 8080;
const char* DEVICE_NAME = "Beogram";
const char* AP_SSID = "BeogramAdaptor";
const char* AP_PASSWORD = "password";

const unsigned long reconnectInterval = 5000;
static unsigned long wsLastReconnectAttempt = millis();  
static unsigned long haloLastReconnectAttempt = millis();
static unsigned long mqttLastReconnectAttempt = millis();
static unsigned long wsLastPingReceived = millis();
static unsigned long wsRemoteLastPingReceived = millis();
static unsigned long haloLastPingReceived = millis();
const unsigned long pingTimeout = 10000;

unsigned long haloActionTime = 0;  //set millis when certain Halo state/page updates are triggered
const unsigned long haloActionDelay = 800; //defines the delay from when haloActionTime is set until the update is sent

unsigned long lastStartEventTime = 0;  // Track the last processed 'started' event time
unsigned long delayPlayAfterDigit = 0; //set millis to delay PLAY command to CD player, when using digits

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
String triggerSource = "lineIn";  // default value

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
HADevice device;
HAMqtt mqtt(wifi, device);
HAButton bgPlay("beogramPlay");
HAButton bgNext("beogramNext");
HAButton bgPrev("BeogramPrev");
HAButton bgStop("beogramStop");
HAButton bgStandby("BeogramStandby");
HASensor bgTrack("beogramCDTrack");
HASensor bgPlaybackState("beogramPlaybackState");

const char* htmlPage PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Change Mozart Platform-based product</title>
<style>
  body { font-family: Arial, sans-serif; background-color: #f4f4f9; text-align: center; padding: 50px; }
  .container { background: white; padding: 20px; border-radius: 8px; box-shadow: 0 0 10px rgba(0,0,0,0.1); display: inline-block; }
  input, button { margin: 10px; padding: 10px; }
  .status { margin-top: 20px; }
  .connected { color: green; }
  .disconnected { color: red; }
  #ws-form { text-align: center; } 
  #ws-form input { display: inline-block; margin: 0 auto; } 
  #halo-form { text-align: center; } 
  #halo-form input { display: inline-block; margin: 0 auto; } 

  .small-text {
    font-size: 12px; /* Adjust the size as needed */
    display: inline; /* Ensure it stays on the same line */
  }

</style>
</head>
<body>
  <div class="container">
    <h2>Beogram Adaptor</h2>
    <br>
    <h3>Mozart Platform-based product</h3>
    <div class="status">
      <p><span id="ws-ip-text">Product IP: </span><span id="ws-ip">Loading...</span></span></p>
      <p>Product Websocket Status: <span id="ws-status" class="disconnected">Disconnected</span></p>
    </div>
    <form id="ws-form" action="/update">
      <label for="wsIP">Enter IP address for product:</label><br>
      <input type="text" id="wsIP" name="wsIP" placeholder="Enter product IP address"><br>
      <span id="wsIP-error"></span><br>
      <button type="submit" id="ws-btn">Connect to product</button>
    </form>
    <br><hr><br>
    <h3>Select input</h3><br>
    <form id="source-form">
      <label for="sourceSelect">Beogram is connected to:</label>
      <select id="sourceSelect" name="source">
        <option value="lineIn">Line-In (default)</option>
        <option value="spdif">Optical</option>
      </select>
    </form>
    
    <br><hr><br>
    <h3>Beoremote Halo</h3>
    <div class="status">
      <p><span id="halo-ip-text">Halo IP: </span><span id="halo-ip">Loading...</span></span></p>
      <p>Halo WebSocket Status: <span id="halo-ws-status" class="disconnected">Disconnected</span></p>
    </div>
    <form id="halo-form" action="/update-halo">
      <label for="haloIP">Enter IP address for Beoremote Halo:</label><br>
      <input type="text" id="haloIP" name="haloIP" placeholder="Enter Halo IP address"><br>
      <span id="haloIP-error"></span><br>
      <button type="submit" id="halo-btn">Connect to Halo</button>
    </form>
    <label for="featureToggle" class="checkbox-label" align="center">
      <input type="checkbox" id="featureToggle" align="center"><span class="small-text">Activate Beogram Controls when waking up Halo</span>
    </label>
    <br><hr><br>
    <h3>Home Assistant auto-discovery</h3>
    <p>MQTT Status: <span id="mqtt-status" class="disconnected">Disconnected</span></p>
    <p><a href="/mqtt">Configure MQTT Settings</a></p>    

    <br><hr><br>
    <h3>Firmware Update</h3>
    <p>Current version: <span id="fw-version">Loading...</span></p>
    <form method="POST" action="/update-ota" enctype="multipart/form-data">
      <input type="file" name="update">
      <input type="submit" value="Upload">
    </form>
    <br><hr><br>
    <h3>Guide</h3>
    <p><a href="https://github.com/cklit/BeogramAdaptor" target="_blank">Go to Github repository</a></p>
    <br>
  </div>

<script>
    function updateStatus() {
        fetch("/status")
            .then(response => response.json())
            .then(data => {
                let wsStatusElem = document.getElementById("ws-status");
                wsStatusElem.textContent = data.ws_connected ? "Connected" : "Disconnected";
                wsStatusElem.className = data.ws_connected ? "connected" : "disconnected";

                let haloWsStatusElem = document.getElementById("halo-ws-status");
                haloWsStatusElem.textContent = data.halo_ws_connected ? "Connected" : "Disconnected";
                haloWsStatusElem.className = data.halo_ws_connected ? "connected" : "disconnected";

                let wsIpElem = document.getElementById("ws-ip");
                let haloIpElem = document.getElementById("halo-ip");

                wsIpElem.textContent = data.ws_ip;
                haloIpElem.textContent = data.halo_ip;

                document.getElementById("featureToggle").checked = data.feature_enabled;

                document.getElementById("sourceSelect").value = data.trigger_source;

                document.getElementById("fw-version").textContent = data.firmware;

                let mqttStatusElem = document.getElementById("mqtt-status");
                mqttStatusElem.textContent = data.mqtt_connected ? "Connected" : "Disconnected";
                mqttStatusElem.className = data.mqtt_connected ? "connected" : "disconnected";

                // Handle visibility and button state for product
                let wsInput = document.getElementById("wsIP");
                let wsLabel = document.querySelector("label[for='wsIP']");
                let wsBtn = document.getElementById("ws-btn");

                if (data.ws_ip && data.ws_ip !== "") {
                    wsInput.style.display = "none";
                    wsLabel.style.display = "none";
                    document.getElementById("ws-ip-text").style.display = "inline";
                    wsBtn.textContent = "Unlink product";
                    wsBtn.onclick = function (event) {
                        event.preventDefault();
                        disconnectWs();
                    };
                } else {
                    wsInput.style.display = "block";
                    wsLabel.style.display = "block";
                    document.getElementById("ws-ip-text").style.display = "none";
                    wsBtn.textContent = "Connect to product";
                    wsBtn.onclick = null;  // Remove the previous click handler
                }

                // Handle visibility and button state for Halo
                let haloInput = document.getElementById("haloIP");
                let haloLabel = document.querySelector("label[for='haloIP']");
                let haloBtn = document.getElementById("halo-btn");
                let haloCheckmark = document.getElementById("featureToggle");                
                let haloCheckmarkLabel = document.querySelector("label[for='featureToggle']");
                
                if (data.halo_ip && data.halo_ip !== "") {
                    haloInput.style.display = "none";
                    haloLabel.style.display = "none";                          
                    document.getElementById("halo-ip-text").style.display = "inline";
                    haloBtn.textContent = "Unlink Beoremote Halo";
                    haloBtn.onclick = function (event) {
                        event.preventDefault();
                        disconnectHalo();
                    };
                    haloCheckmark.style.display = "inline";  
                    haloCheckmarkLabel.style.display = "inline";                
                } else {
                    haloInput.style.display = "block";
                    haloLabel.style.display = "block";                   
                    document.getElementById("halo-ip-text").style.display = "none";
                    haloBtn.textContent = "Connect to Beoremote Halo";
                    haloBtn.onclick = null;  // Remove the previous click handler
                    haloCheckmark.style.display = "none";       
                    haloCheckmarkLabel.style.display = "none";
                }
            })
            .catch(error => console.error("Error fetching status:", error));
    }



    function updateProductIP(ip) {
        fetch(`/update?wsIP=${encodeURIComponent(ip)}`, { method: "GET" })
            .then(response => response.text())
            .then(() => {
                updateStatus(); // Refresh the UI with the new IP
            })
            .catch(error => console.error("Error updating product IP:", error));
    }

    function updateHaloIP(ip) {
        fetch(`/update-halo?haloIP=${encodeURIComponent(ip)}`, { method: "GET" })
            .then(response => response.text())
            .then(() => {
                updateStatus(); // Refresh the UI with the new IP
            })
            .catch(error => console.error("Error updating Halo IP:", error));
    }

    function disconnectWs() {
        fetch("/update?wsIP=", { method: "GET" }) // Send empty IP to backend
            .then(() => {           
                updateStatus();
            })
            .catch(error => console.error("Error unlinking product:", error));
    }

    function disconnectHalo() {
        fetch("/update-halo?haloIP=", { method: "GET" }) // Send empty IP to backend
            .then(() => {           
                updateStatus();
            })
            .catch(error => console.error("Error unlinking Beoremote Halo:", error));
    }   

    function validateIP(ip) {
        let parts = ip.split(".");
        if (parts.length !== 4) return false;
        return parts.every(part => {
            let num = parseInt(part, 10);
            return num >= 0 && num <= 255 && part === num.toString();
        });
    }

    document.getElementById("ws-form").addEventListener("submit", function(event) {
        event.preventDefault();
        let wsIP = document.getElementById("wsIP").value;
        let errorSpan = document.getElementById("wsIP-error");

        if (!validateIP(wsIP)) {
            errorSpan.textContent = "Invalid IP address!";
            errorSpan.style.color = "red";
            return;
        } else {
            errorSpan.textContent = "";
        }

        updateProductIP(wsIP);
    });

    document.getElementById("halo-form").addEventListener("submit", function(event) {
        event.preventDefault();
        let haloIP = document.getElementById("haloIP").value;
        let errorSpan = document.getElementById("haloIP-error");

        if (!validateIP(haloIP)) {
            errorSpan.textContent = "Invalid IP address!";
            errorSpan.style.color = "red";
            return;
        } else {
            errorSpan.textContent = "";
        }

        updateHaloIP(haloIP);
    });

    document.getElementById("featureToggle").addEventListener("change", function() {
        fetch(`/update-feature?enabled=${this.checked}`, { method: "GET" })
            .catch(error => console.error("Error updating feature state:", error));
    });

    document.getElementById("sourceSelect").addEventListener("change", function() {
      fetch(`/update-source?source=${this.value}`, { method: "GET" })
        .catch(error => console.error("Error updating source:", error));
    });

    // Run updateStatus every 5 seconds
    setInterval(updateStatus, 5000);
    updateStatus();
</script>
</body>
</html>
)rawliteral";

// Struct to manage button update timing
struct ButtonUpdate {
    String id;
    bool pending;
    unsigned long timestamp;
};

ButtonUpdate pendingUpdate = {"", false, 0}; // Track pending button updates

void sendButtonUpdate(const char* buttonID, const char* state = nullptr, const char* title = nullptr, const char* text = nullptr, const char* subtitle = nullptr, int value = -1) {
    DynamicJsonDocument doc(1024);
    
    doc["update"]["type"] = "button";
    doc["update"]["id"] = buttonID;
    if (value != -1) {
        doc["update"]["value"] = value;
    }
    if (state != nullptr) {
        doc["update"]["state"] = state;
    }
    if (title != nullptr) {
        doc["update"]["title"] = title;
    }   
    if (subtitle != nullptr) {
        doc["update"]["subtitle"] = subtitle;
    }    

    if (text != nullptr) {
        doc["update"]["content"]["text"] = text;
    }    

    String output;
    serializeJson(doc, output);

    // Assuming you have a function to send the update string, e.g., sendUpdateToServer
    haloClient.send(output);
}

void sendPageUpdate(const char* pageID, const char* buttonID) {
    DynamicJsonDocument doc(1024);
    
    doc["update"]["type"] = "displaypage";
    doc["update"]["pageid"] = pageID; 
    doc["update"]["buttonid"] = buttonID;

    String output;
    serializeJson(doc, output);

    // Assuming you have a function to send the update string, e.g., sendUpdateToServer
    haloClient.send(output);
}

void checkWiFiConnection() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi lost, attempting to reconnect...");
        WiFi.reconnect();
        unsigned long startAttemptTime = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
            delay(500);
            Serial.print(".");
        }
        Serial.println(WiFi.status() == WL_CONNECTED ? "\nReconnected!" : "\nFailed to reconnect.");
    }
}

void checkWebSocketConnection() {
    if (millis() - wsLastReconnectAttempt > reconnectInterval) {
        Serial.println("Reconnecting product websocket...");
        wsLastReconnectAttempt = millis();        
        if (client.connect(("ws://" + wsIP + ":" + WEBSOCKET_PORT).c_str())) {
            client.send("Hi Server!");
            Serial.println("Product webSocket reconnected!");
        } else {
            Serial.println("Product webSocket reconnection failed.");
        }
        if (remoteClient.connect(("ws://" + wsIP + ":" + WEBSOCKET_PORT + "/remoteControl").c_str())) {
            remoteClient.send("Hi Server!");
            Serial.println("Secondary websocket reconnected!");            
        } else {
            Serial.println("Remote webSocket reconnection failed.");
        }
    }
}

void checkPingWebsocket() {
//    wsLastPingReceived = millis();
    if (client.available() && wsIP.length() > 0) {    
        if (millis() - wsLastPingReceived >= pingTimeout) {
            client.ping();
            wsLastPingReceived = millis();
        }
    } else if (!client.available() && wsIP.length() > 0) {    
        if (millis() - wsLastPingReceived >= pingTimeout) {
            client.close();
            delay(10);
            checkWebSocketConnection();  // Attempt reconnection
        }
    } else if (remoteClient.available() && wsIP.length() > 0) {    
        if (millis() - wsRemoteLastPingReceived >= pingTimeout) {
            remoteClient.ping();
            wsRemoteLastPingReceived = millis();
        }
    } else if (!remoteClient.available() && wsIP.length() > 0) {    
        if (millis() - wsRemoteLastPingReceived >= pingTimeout) {
            remoteClient.close();
            delay(10);
            checkWebSocketConnection();  // Attempt reconnection
        }
    }  

    if (haloClient.available()) {
        if (millis() - haloLastPingReceived >= pingTimeout) {
            haloClient.ping();
            haloLastPingReceived = millis(); // Reset the timer
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

    mqttConnected = mqtt.isConnected(); // Always keep it updated
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

void handleHttpResponse(const String& endpoint, const String& response) {
    if (endpoint == "/api/v1/playback/state") {
        StaticJsonDocument<512> doc;
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
        if (method == "POST") {
            http.addHeader("Content-Type", "application/json");
        }

        int httpResponseCode;
        if (method == "POST") {
            httpResponseCode = payload.isEmpty() ? http.POST("") : http.POST(payload);
        } else {  // Default to GET
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
        // Only send HTTP command if WebSocket is connected
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
            preferences.putString("haloIP", haloIP); // Store the Halo IP in preferences
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
        haloClient.connect(("ws://" + haloIP + ":8080").c_str());  // Adjust the port if needed
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

    server.send(200, "text/html", "<h2>MQTT settings cleared.</h2><a href='/mqtt'>Go Back</a>");
    delay(1000);
    ESP.restart(); // Optional: reboot to apply changes
}

void handleMqttUpdate() {
    if (server.hasArg("ip") && server.hasArg("user") && server.hasArg("pass")) {
        mqttIP = server.arg("ip");
        mqttUser = server.arg("user");
        mqttPassword = server.arg("pass");

        preferences.putString("mqttIP", mqttIP);
        preferences.putString("mqttUser", mqttUser);
        preferences.putString("mqttPassword", mqttPassword);

        server.send(200, "text/html", "<h2>MQTT settings saved.</h2><a href='/mqtt'>Go Back</a>");
        delay(1000);
        ESP.restart(); // Optional but clean for applying settings
    } else {
        server.send(400, "text/plain", "Missing parameters");
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
      <style>
        body { font-family: Arial, sans-serif; background-color: #f4f4f9; text-align: center; padding: 50px; }
        .container { background: white; padding: 20px; border-radius: 8px; box-shadow: 0 0 10px rgba(0,0,0,0.1); display: inline-block; }
        input, button { margin: 10px; padding: 10px; width: 80%; }
        label { display: block; margin-top: 10px; }
        a { display: inline-block; margin-top: 20px; color: #333; text-decoration: none; }
      </style>
    </head>
    <body>
      <div class="container">
        <h2>MQTT Configuration</h2>
        <form method="POST" action="/mqtt">
          <label for="ip">Broker IP Address:</label>
          <input type="text" id="ip" name="ip" value=")rawliteral" + mqttIP + R"rawliteral(">

          <label for="user">Username:</label>
          <input type="text" id="user" name="user" value=")rawliteral" + mqttUser + R"rawliteral(">

          <label for="pass">Password:</label>
          <input type="password" id="pass" name="pass" value=")rawliteral" + mqttPassword + R"rawliteral(">

          <button type="submit">Save Settings</button>
        </form>

        <form id="reset-form" method="GET" action="/mqtt/reset">
          <button type="submit" style="background-color: #d9534f;">Reset MQTT Settings</button>
        </form>

        <script>
          document.getElementById('reset-form').addEventListener('submit', function(e) {
            if (!confirm('⚠️ This will erase your MQTT settings. Are you sure?')) {
              e.preventDefault();
            }
          });
        </script>

        <a href="/">← Back to Main Page</a>
      </div>
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
//                        "},"
//                        "{"
//                            "\"id\": \"03481fcc-e2cc-47ba-bcae-6152bbf93482\","
//                            "\"title\": \"\","
//                            "\"subtitle\": \"\","
//                            "\"value\": 100,"
//                            "\"state\": \"inactive\","
//                            "\"content\": { \"text\": \"Stby\" }"
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
            haloActionTime = millis();  // Store the current time  
            if (haloControls) {
                haloUpdate = PAGE;
            }             
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
    }

    else if (message.indexOf("\"value\":\"networkStandby\"") != -1) {
        sendHexCommand(STANDBY);
        playbackState = STOPPED;
        if (haloClient.available()) {
            sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Stopped", "Play", "");  
        }
 
        Serial.println("🛑 Standby command detected on websocket. Sent STBY command to Beogram");
    } 

    else if (lineInActive) {
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
        } 
        else if (message.indexOf("\"value\":\"stopped\"") != -1 && playbackState != STOPPED) {
            playbackState = PAUSED;
            sendHexCommand(STOP);    
            if (haloClient.available()) {
                sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Stopped", "Play"); 
            }                
            Serial.println("⏸️ Product changed state to Stopped. Sent STOP command to Beogram");
        } 
        else if (message.indexOf("\"value\":\"paused\"") != -1) {
            playbackState = PAUSED;
            sendHexCommand(STOP);
            if (haloClient.available()) {
                sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Stopped", "Play"); 
            }
            Serial.println("⏸️ Product changed state to Paused. Sent STOP command to Beogram");
        }         
        else if (message.indexOf("\"button\":\"Next\"") != -1) {
            sendHexCommand(NEXT);
            Serial.println("⏭️ Sent NEXT command to Beogram");
        } 
        else if (message.indexOf("\"button\":\"Previous\"") != -1) {
            sendHexCommand(PREVIOUS);
            Serial.println("⏮️ Sent PREV command to Beogram");
        }
    }
}

void processRemoteWebSocketMessage(const String& message) {
    if (message.indexOf("\"eventType\":\"WebSocketEventBeoRemoteButton\"") != -1 &&
       message.indexOf("\"Type\":\"KeyPress\"") != -1 && lineInActive) {  

        // Handle standard control commands
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
        } 
        // Handle digit commands
        else if (message.indexOf("\"Key\":\"Control/Digit") != -1) {
            int digitIndex = message.indexOf("\"Key\":\"Control/Digit") + 20; // Find position of digit
            char digitChar = message[digitIndex]; // Extract digit character

            if (isdigit(digitChar)) { // Safer check
                const BeogramCommand digitCommands[10] = {
                    DIGIT0, DIGIT1, DIGIT2, DIGIT3, DIGIT4, 
                    DIGIT5, DIGIT6, DIGIT7, DIGIT8, DIGIT9
                };

                BeogramCommand digitCommand = digitCommands[digitChar - '0'];

                // Send OPEN_FOR_DIGIT first
                sendHexCommand(OPEN_FOR_DIGIT);
                delay(50);
                sendHexCommand(digitCommand);

                // Start the non-blocking delay
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
        waitingForPlay = false; // Reset flag
        Serial.println("▶️ Sent PLAY after 1200ms delay");
    }
}

void processBuffer(BeogramFeedback state) {
    if (state == PLAYING_FB) {
        Serial.println("▶️ Beogram reported ON state.");
        playbackState = PLAYING;
        bgPlaybackState.setValue("Playing");  
        if (haloClient.available()) {
            sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Playing", "Stop");
        }
        if (!lineInActive) {
            sendHttpRequest("/api/v1/playback/sources/active/" + triggerSource, "POST");
        } else if (lineInActive) {
            sendHttpRequest("/api/v1/playback/command/play", "POST");
        }
    } else if (state == STOPPED_FB || state == STANDBY_FB) {
        Serial.println(state == STOPPED_FB ? "Beogram reported OFF state." : "Beogram reported STANDBY state.");
        bgTrack.setValue("-");
        bgPlaybackState.setValue(state == STOPPED_FB ? "Stopped" : "Standby");
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
        bgTrack.setValue("-");
        bgPlaybackState.setValue("Ejected");          
        playbackState = STOPPED;
        if (haloClient.available()) {
            sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Stopped", "Play", "Tray ejected");
        }

        if (lineInActive) {
            sendHttpRequest("/api/v1/playback/command/stop", "POST");
        }
    } else if (state == TRACK14_PLUS && playbackState == PLAYING) {
        Serial.print("Track identified: ");
        Serial.println("14+");
        bgTrack.setValue("14+");
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
        bgTrack.setValue(trackNumber);

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

void updateLEDStatus() {
    pixels.clear();
    if (WiFi.status() != WL_CONNECTED) {
        pixels.setPixelColor(0, pixels.Color(0, 2, 0));  // Red for WiFi issue
    } else if (!client.available()) {
        pixels.setPixelColor(0, pixels.Color(0, 0, 2));  // Blue for SSE issue
    } else {
        pixels.setPixelColor(0, pixels.Color(2, 0, 0));  // Green when connected to SSE
    }
    pixels.show();
}

void onMessageCallback(WebsocketsMessage message) {
    //Serial.println("Message from Halo: " + message.data());

    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, message.data());

    if (error) {
        Serial.println("JSON parsing failed");
        return;
    }

    // Handle button press events dynamically
    if (doc.containsKey("event") && doc["event"]["type"] == "button" && doc["event"]["state"] == "pressed") {
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

    if (haloControls && lineInActive && doc.containsKey("event") && doc["event"]["type"] == "system" && doc["event"]["state"] == "active") {
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
        if (!lineInActive) {
            haloUpdate = NONE;
            sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Stopped", "Play");
        } else {
            haloUpdate = NONE;
            sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Playing", "Stop");
        }
    }
}

void handleResetWifi() {
    server.send(200, "application/json", "{\"Connection\":\"WiFi has been reset. Restarting in AP mode.\"}");
    wm.resetSettings();
    delay(1000);
    ESP.restart(); 
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

    byte mac[6];
    WiFi.macAddress(mac);
    device.setUniqueId(mac, sizeof(mac));
    device.setName("Beogram Adaptor");
    device.setSoftwareVersion(FIRMWARE_VERSION);
    device.enableSharedAvailability();
    device.enableLastWill();
    bgPlay.setIcon("mdi:play-circle");
    bgPlay.setName("Play");
    bgNext.setIcon("mdi:skip-next-circle");
    bgNext.setName("Next");
    bgPrev.setIcon("mdi:skip-previous-circle");
    bgPrev.setName("Previous");  
    bgStop.setIcon("mdi:stop-circle");
    bgStop.setName("Stop");
    bgStandby.setIcon("mdi:power-standby");
    bgStandby.setName("Standby"); 
    bgTrack.setIcon("mdi:music-note-eighth");
    bgTrack.setName("Track number");  
    bgPlaybackState.setIcon("mdi:album");
    bgPlaybackState.setName("Beogram state");            

    bgPlay.onCommand(onButtonCommand);
    bgNext.onCommand(onButtonCommand);
    bgPrev.onCommand(onButtonCommand);
    bgStop.onCommand(onButtonCommand);
    bgStandby.onCommand(onButtonCommand);       

    if (!MDNS.begin(DEVICE_NAME)) {
        Serial.println("Error setting up MDNS responder!");
        while (1) {
            delay(1000);
        }
    }
    Serial.println("mDNS responder started");

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

    client.send("Hi Server!");

    MDNS.addService("http", "tcp", 80);

    checkMQTTConnection(true);  // Force immediate connect attempt

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
        
        if (input == "debug 1") {
            debugSerial = true;
            Serial.println("Debug mode enabled");
        } else if (input == "debug 0") {
            debugSerial = false;
            Serial.println("Debug mode disabled");
        }
    }    
}
