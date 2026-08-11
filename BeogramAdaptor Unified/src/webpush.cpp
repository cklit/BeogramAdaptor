#include "webpush.h"

static WebsocketsServer uiServer;
static const int MAX_UI_CLIENTS = 4;
static WebsocketsClient uiClients[MAX_UI_CLIENTS];

static String beogramStateJson() {
    String json = "{\"state\":\"" + beogramStateText + "\",";
    json += "\"track\":\"" + beogramTrack + "\",";
    json += "\"playing\":" + String(beogramPlaying ? "true" : "false") + "}";
    return json;
}

void webpushBegin() {
    uiServer.listen(UI_WS_PORT);
}

void webpushLoop() {
    // Accept new browser connections and greet them with the current state
    if (uiServer.poll()) {
        for (int i = 0; i < MAX_UI_CLIENTS; i++) {
            if (!uiClients[i].available()) {
                uiClients[i] = uiServer.accept();
                if (uiClients[i].available()) {
                    uiClients[i].send(beogramStateJson());
                }
                break;
            }
        }
    }
    // Service connected clients
    for (int i = 0; i < MAX_UI_CLIENTS; i++) {
        if (uiClients[i].available()) uiClients[i].poll();
    }
    // Push on change, flagged from processBuffer
    if (beogramStateDirty) {
        beogramStateDirty = false;
        broadcastBeogramState();
    }
}

void broadcastBeogramState() {
    String json = beogramStateJson();
    for (int i = 0; i < MAX_UI_CLIENTS; i++) {
        if (uiClients[i].available()) uiClients[i].send(json);
    }
}
