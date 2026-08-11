#pragma once
#include "state.h"

// Shared transport surface. Each platform implements its half; callers
// dispatch through these without caring which platform is active.

inline bool productConnected() {
    if (productIP.length() == 0) return false;
    return (platform == PLATFORM_MOZART) ? wsClient.available() : sseClient.connected();
}

// ASE (transport_ase.cpp): SSE stream + BeoZone REST
void forceSource();
void connectToSSE();
void checkSSEConnection();
void processSSE(String message);
void readSSE();

// Mozart (transport_moz.cpp): dual websockets + /api/v1 REST
void handleHttpResponse(const String& endpoint, const String& response);
void sendHttpRequest(const String& endpoint, const String& method = "GET", const String& payload = "");
void checkWebSocketConnection();
void processWebSocketMessage(const String& message);
void processRemoteWebSocketMessage(const String& message);
