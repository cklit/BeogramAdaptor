#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <ArduinoWebsockets.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <Update.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoHA.h>

#define RXD2 16
#define TXD2 17
#define PIN 47
#define NUMPIXELS 1
#define DELAYVAL 500
#define FIRMWARE_VERSION "ASE.2025.3.30"

bool debugSerial = false; // set to true to print all incoming serial commands from Beogram

const int SSE_PORT = 8080;
const int HALO_WEBSOCKET_PORT = 8080;
const char* DEVICE_NAME = "Beogram";
const char* AP_SSID = "BeogramAdaptor";
const char* AP_PASSWORD = "password";

unsigned long lastReconnectAttempt = 0;
unsigned long reconnectDelay = 1000; // Start with 1 second
const unsigned long reconnectInterval = 5000;

const unsigned long pingTimeout = 10000;
unsigned long haloActionTime = 0;  //set millis when certain Halo state/page updates are triggered

const unsigned long haloActionDelay = 800; //defines the delay from when haloActionTime is set until the update is sent

static unsigned long mqttLastReconnectAttempt = millis();

unsigned long delayPlayAfterDigit = 0; //set millis to delay PLAY command to CD player, when using digits
static unsigned long haloLastReconnectAttempt = millis();
static unsigned long haloLastPingReceived = millis();

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

String sseIP;
String haloIP;
String triggerSource = "LINE IN";  // default value
String mqttIP;
String mqttUser;
String mqttPassword;

PlaybackState playbackState = BOOT;
HaloUpdate haloUpdate = NONE;
BeogramCommand pendingPlayCommand;
Adafruit_NeoPixel pixels(NUMPIXELS, PIN, NEO_GRB + NEO_KHZ800);
WiFiClient client;
Preferences preferences;
WebServer server(80);
HTTPClient http;

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

WiFiManager wm;
using namespace websockets;
WebsocketsClient haloClient;

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

const char* htmlPage PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Change ASE Platform-based product</title>
<style>
  body { font-family: Arial, sans-serif; background-color: #f4f4f9; text-align: center; padding: 50px; }
  .container { background: white; padding: 20px; border-radius: 8px; box-shadow: 0 0 10px rgba(0,0,0,0.1); display: inline-block; }
  input, button { margin: 10px; padding: 10px; }
  .status { margin-top: 20px; }
  .connected { color: green; }
  .disconnected { color: red; }
  #sse-form { text-align: center; } 
  #sse-form input { display: inline-block; margin: 0 auto; } 
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
    <h3>ASE Platform-based product</h3>
    <div class="status">
      <p><span id="sse-ip-text">Product IP: </span><span id="sse-ip">Loading...</span></span></p>
      <p>Product Websocket Status: <span id="sse-status" class="disconnected">Disconnected</span></p>
    </div>
    <form id="sse-form" action="/update">
      <label for="sseIP">Enter IP address for product:</label><br>
      <input type="text" id="sseIP" name="sseIP" placeholder="Enter product IP address"><br>
      <span id="sseIP-error"></span><br>
      <button type="submit" id="sse-btn">Connect to product</button>
    </form>
    <br><hr><br>
    <h3>Select input</h3><br>
    <form id="source-form">
      <label for="sourceSelect">Beogram is connected to:</label>
      <select id="sourceSelect" name="source">
        <option value="LINE IN">Line-In (default)</option>
        <option value="TOSLINK">Optical</option>
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
                let sseStatusElem = document.getElementById("sse-status");
                sseStatusElem.textContent = data.sse_connected ? "Connected" : "Disconnected";
                sseStatusElem.className = data.sse_connected ? "connected" : "disconnected";

                let haloWsStatusElem = document.getElementById("halo-ws-status");
                haloWsStatusElem.textContent = data.halo_ws_connected ? "Connected" : "Disconnected";
                haloWsStatusElem.className = data.halo_ws_connected ? "connected" : "disconnected";

                let sseIpElem = document.getElementById("sse-ip");
                let haloIpElem = document.getElementById("halo-ip");

                sseIpElem.textContent = data.sse_ip;
                haloIpElem.textContent = data.halo_ip;

                document.getElementById("featureToggle").checked = data.feature_enabled;

                document.getElementById("sourceSelect").value = data.trigger_source;

                document.getElementById("fw-version").textContent = data.firmware;

                let mqttStatusElem = document.getElementById("mqtt-status");
                mqttStatusElem.textContent = data.mqtt_connected ? "Connected" : "Disconnected";
                mqttStatusElem.className = data.mqtt_connected ? "connected" : "disconnected";

                // Handle visibility and button state for product
                let sseInput = document.getElementById("sseIP");
                let sseLabel = document.querySelector("label[for='sseIP']");
                let sseBtn = document.getElementById("sse-btn");

                if (data.sse_ip && data.sse_ip !== "") {
                    sseInput.style.display = "none";
                    sseLabel.style.display = "none";
                    document.getElementById("sse-ip-text").style.display = "inline";
                    sseBtn.textContent = "Unlink product";
                    sseBtn.onclick = function (event) {
                        event.preventDefault();
                        disconnectWs();
                    };
                } else {
                    sseInput.style.display = "block";
                    sseLabel.style.display = "block";
                    document.getElementById("sse-ip-text").style.display = "none";
                    sseBtn.textContent = "Connect to product";
                    sseBtn.onclick = null;  // Remove the previous click handler
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
        fetch(`/update?sseIP=${encodeURIComponent(ip)}`, { method: "GET" })
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
        fetch("/update?sseIP=", { method: "GET" }) // Send empty IP to backend
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

    document.getElementById("sse-form").addEventListener("submit", function(event) {
        event.preventDefault();
        let sseIP = document.getElementById("sseIP").value;
        let errorSpan = document.getElementById("sseIP-error");

        if (!validateIP(sseIP)) {
            errorSpan.textContent = "Invalid IP address!";
            errorSpan.style.color = "red";
            return;
        } else {
            errorSpan.textContent = "";
        }

        updateProductIP(sseIP);
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

void checkPingWebsocket() {
//    wsLastPingReceived = millis();
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
void forceSource() {
    if (WiFi.status() == WL_CONNECTED) {
        String payload = "{\"sourceType\":{\"type\":\"" + triggerSource + "\"}}";
        String url = "http://" + sseIP + ":" + String(SSE_PORT) + "/BeoZone/Zone/ActiveSourceType";
        Serial.println("Activating Line-In on the product.");  
        HTTPClient http;
        http.begin(url);
        http.addHeader("Content-Type", "application/json");
        int code = http.POST(payload);
        Serial.println("Status: " + String(code));
        http.end();
    }
}

void connectToServer() {
    if (client.connected()) return;  // Avoid reconnecting if already connected

    Serial.printf("Connecting to SSE stream: %s\n", sseIP.c_str());
    client.stop();  // Ensure the previous client is closed

    if (!client.connect(sseIP.c_str(), SSE_PORT)) {
        Serial.println("Connection to server failed!");
        return;
    }

    client.println("GET /BeoNotify/Notifications HTTP/1.1");
    client.println("Host: " + sseIP + ":" + String(SSE_PORT));
    client.println("Accept: text/event-stream");
    client.println("Connection: keep-alive");
    client.println();

    Serial.println("Connected to SSE stream!");
}

void handleUpdate() {
    if (server.hasArg("sseIP")) {
        String newIP = server.arg("sseIP");
        if (newIP == "") {
            client.stop();
            sseIP = newIP;
            preferences.putString("sseIP", sseIP);
            Serial.println("Unlinked product."); 
            return;
        } else if (!isValidIPAddress(newIP)) {
            server.send(400, "text/html", "<h2>Invalid IP Address</h2><a href='/'>Go Back</a>");
            Serial.println("Invalid IP Address - not saved.");             
            return;
        }
        sseIP = newIP;
        preferences.putString("sseIP", sseIP);

        server.send(200, "text/html", "<h2>IP Updated to " + sseIP + "</h2><a href='/'>Go Back</a>");
        client.stop();
        connectToServer();
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
        if (newSource == "LINE IN" || newSource == "TOSLINK") {
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
    jsonResponse += "\"sse_ip\":\"" + sseIP + "\",";
    jsonResponse += "\"sse_connected\":";
    jsonResponse += client.connected() ? "true" : "false";  //sse-client
    jsonResponse += ",";
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
    haloUpdate = STATE;
}

void processSSE(String message) {
    if (message.startsWith("data: ")) {
        message = message.substring(6);
    }

    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, message);
    if (error) return;

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
            forceSource();
        }
    } else if (state == STOPPED_FB) {
        Serial.println("Beogram reported OFF state.");
        if (playbackState == PLAYING && lineInActive) {
            playbackState = STOPPED;
            bgTrack.setValue("-");
            bgPlaybackState.setValue(state == STOPPED_FB ? "Stopped" : "Standby");            
            Serial.println("⏹️ Beogram has stopped.");
            if (haloClient.available()) {
                sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Stopped", "Play");
            }                                   
        } 
    } else if (state == STANDBY_FB) {
        Serial.println("Beogram reported STANDBY state.");
        if (playbackState == PLAYING && lineInActive) {
            playbackState = STOPPED;
            bgTrack.setValue("-");
            bgPlaybackState.setValue(state == STOPPED_FB ? "Stopped" : "Standby");
            Serial.println("⏹️ Beogram has turned off.");
            if (haloClient.available()) {
                sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Stopped", "Play", " ");
            }                          
        }            
    } else if (state == EJECTED_FB) {
        Serial.println("⏏️ Beogram tray was ejected");
        playbackState = STOPPED;
        bgTrack.setValue("-");
        bgPlaybackState.setValue("Ejected");          
        if (haloClient.available()) {
            sendButtonUpdate("872b4893-bfdf-4d51-bb53-b5738149fc61", nullptr, "Stopped", "Play", "Tray ejected");  
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


void checkSSEConnection() {
    if (!client.connected()) {
        unsigned long now = millis();

        // Ensure Wi-Fi is connected before attempting SSE reconnect
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("Wi-Fi disconnected. Waiting for reconnection...");
            return;
        }

        // Wait for the delay before retrying
        if (now - lastReconnectAttempt >= reconnectDelay) {
            Serial.println("SSE stream lost. Reconnecting...");
            lastReconnectAttempt = now;
            connectToServer();

            // Implement exponential backoff (double the delay up to a max of 8s)
            reconnectDelay = min(reconnectDelay * 2, 8000UL);
        }
    } else {
        // Reset delay if connection is stable
        reconnectDelay = 1000;
    }
}

void readSSE() {
    static String lineBuffer = "";

    while (client.available()) {
        char c = client.read();
        if (c == '\n') {
            if (lineBuffer.length() > 0) {
                processSSE(lineBuffer);
                lineBuffer = "";
            }
        } else {
            lineBuffer += c;
        }
    }
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

void updateLEDStatus() {
    pixels.clear();
    if (WiFi.status() != WL_CONNECTED) {
        pixels.setPixelColor(0, pixels.Color(0, 2, 0));  // Red for WiFi issue
    } else if (!client.connected()) {
        pixels.setPixelColor(0, pixels.Color(0, 0, 2));  // Blue for SSE issue
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

    byte mac[6];
    WiFi.macAddress(mac);
    device.setUniqueId(mac, sizeof(mac));
    device.setName("BeogramAdaptor");
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
    bgTrack.setName("Track");  
    bgPlaybackState.setIcon("mdi:album");
    bgPlaybackState.setName("State");
    mqtt.setDiscoveryPrefix("homeassistant");           

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
    sseIP = preferences.getString("sseIP", "");
    haloIP = preferences.getString("haloIP", "");
    haloControls = preferences.getBool("feature_enabled", false);
    mqttIP = preferences.getString("mqttIP", "");
    mqttUser = preferences.getString("mqttUser", "");
    mqttPassword = preferences.getString("mqttPassword", "");
    triggerSource = preferences.getString("triggerSource", "LINE IN");    

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

    MDNS.addService("http", "tcp", 80);

    checkMQTTConnection(true);  // Force immediate connect attempt

    if (sseIP.length() > 0) {
        connectToServer();
    }
}

void loop() {
    updateLEDStatus();

    if (sseIP.length() > 0) {
        connectToHalo();
        checkSSEConnection();
    }    

    checkPingWebsocket();
    handleSerial1Data();
    server.handleClient();
    checkWiFiConnection();
    sendPlayAfterDelay();
    secondButtonUpdate();
    activateHaloPage();
    mqtt.loop();
    checkMQTTConnection();    
    readSSE();
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
