#include "transport.h"
#include "beogram.h"
#include "halo.h"
#include "ha_mqtt.h"
#include <WiFi.h>
#include <ArduinoJson.h>

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

void sendHttpRequest(const String& endpoint, const String& method, const String& payload) {
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
