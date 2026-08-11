#include "transport.h"
#include "beogram.h"
#include "halo.h"
#include "ha_mqtt.h"
#include <WiFi.h>
#include <ArduinoJson.h>

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
