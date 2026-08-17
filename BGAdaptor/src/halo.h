#pragma once
#include "state.h"

// Beoremote Halo: websocket client, configuration JSON, wheel/button
// event handling, and display page/button updates.

struct ButtonUpdate {
    String id;
    bool pending;
    unsigned long timestamp;
};
extern ButtonUpdate pendingUpdate;  // Track pending button updates

void sendButtonUpdate(const char* buttonID, const char* state = nullptr, const char* title = nullptr, const char* text = nullptr, const char* subtitle = nullptr, int value = -1);
void sendPageUpdate(const char* pageID, const char* buttonID);
void sendConfigToHalo();
void onMessageCallback(WebsocketsMessage message);
void secondButtonUpdate();
void connectToHalo();
void activateHaloPage();
