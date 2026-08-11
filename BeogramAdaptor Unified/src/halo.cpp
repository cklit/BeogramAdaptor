#include "halo.h"
#include "beogram.h"
#include "transport.h"
#include <ArduinoJson.h>

ButtonUpdate pendingUpdate = {"", false, 0}; // Track pending button updates

void sendButtonUpdate(const char* buttonID, const char* state, const char* title, const char* text, const char* subtitle, int value) {
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
