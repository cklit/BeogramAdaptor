#include "discovery.h"
#include "transport.h"
#include <ESPmDNS.h>
#include <mdns.h>
#include <WiFi.h>

// Browse the network for B&O products via mDNS. Discovery only — the
// actual connection still uses the IP address the user confirms.
// The service type identifies the platform: ASE products announce
// _beoremote._tcp, Mozart _bangolufsen._tcp — so both are scanned and
// each hit is tagged, letting the UI switch platform automatically.
// The friendly name lives in the "name" (ASE) / "fn" (Mozart) TXT record.
//
// The raw ESP-IDF query API is used instead of ESPmDNS.queryService()
// because the wrapper hardcodes a 3 s window. A longer window is far more
// reliable: the stack retransmits the query several times while the search
// is active, so slow or lossy responders get multiple chances to answer.

// Serial number location differs per platform:
//   Mozart — TXT "sn" holds it directly.
//   ASE    — TXT "jid" embeds it: 8 digits starting at character 14
//            (e.g. "2213.1200000.24680135@products..." → "24680135").
// serialStart/serialLen slice the TXT value; serialLen 0 = whole value.
void collectService(const char* service, const char* nameKey,
                    const char* serialKey, int serialStart, int serialLen,
                    const char* platformTag, JsonArray& arr, String& seenIPs) {
    mdns_result_t* results = nullptr;
    esp_err_t err = mdns_query_ptr(service, "_tcp", MDNS_SCAN_TIME_MS, 20, &results);
    if (err != ESP_OK) {
        Serial.printf("mDNS query %s failed: %d\n", service, err);
        return;
    }

    for (mdns_result_t* r = results; r != nullptr; r = r->next) {
        String ip = "";
        for (mdns_ip_addr_t* a = r->addr; a != nullptr; a = a->next) {
            if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                ip = IPAddress(a->addr.u_addr.ip4.addr).toString();
                break;
            }
        }
        if (ip == "" || seenIPs.indexOf("|" + ip + "|") >= 0) continue;
        seenIPs += ip + "|";

        String friendly = "";
        for (size_t t = 0; t < r->txt_count; t++) {
            if (strcmp(r->txt[t].key, nameKey) == 0 && r->txt[t].value != nullptr) {
                friendly = r->txt[t].value;
                break;
            }
        }
        if (friendly == "" && r->instance_name != nullptr) friendly = r->instance_name;
        if (friendly == "" && r->hostname != nullptr) friendly = r->hostname;

        String serial = "";
        if (serialKey != nullptr) {
            for (size_t t = 0; t < r->txt_count; t++) {
                if (strcmp(r->txt[t].key, serialKey) == 0 && r->txt[t].value != nullptr) {
                    serial = r->txt[t].value;
                    break;
                }
            }
            if (serialLen > 0) {
                serial = ((int)serial.length() >= serialStart + serialLen)
                         ? serial.substring(serialStart, serialStart + serialLen) : "";
            }
        }

        JsonObject d = arr.add<JsonObject>();
        d["name"]     = friendly;
        d["ip"]       = ip;
        d["serial"]   = serial;
        d["platform"] = platformTag;
    }
    mdns_query_results_free(results);
}

void handleDiscover() {
    JsonDocument doc;
    JsonArray arr = doc["devices"].to<JsonArray>();
    String seenIPs = "|";

    // Mozart first: wins dedup if a product announces both services
    collectService("_bangolufsen", "fn",   "sn",  0,  0, "mozart", arr, seenIPs);
    collectService("_beoremote",   "name", "jid", 13, 8, "ase",    arr, seenIPs);

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

// Beoremote Halo announces _zenith._tcp (Zenith is its internal codename).
void handleDiscoverHalo() {
    JsonDocument doc;
    JsonArray arr = doc["devices"].to<JsonArray>();
    String seenIPs = "|";

    collectService("_zenith", "fn", "serial", 0, 0, "halo", arr, seenIPs);

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

// ── Self-healing product connection ─────────────────────────────────
// Connect-by-IP stays the fast path, but the serial number stored at
// discovery-link time is the product's real identity. If the connection
// has been down long enough that normal reconnects clearly aren't
// working, rescan the active platform's service type and look for that
// serial — if the product moved to a new IP (DHCP lease change), follow
// it there and persist. Manually-entered IPs have no stored serial and
// intentionally never self-heal.
static unsigned long productDisconnectedSince = 0;
static unsigned long lastRecoveryScan = 0;

void checkProductRecovery() {
    if (productIP.length() == 0 || productSerial.length() == 0) return;
    if (productConnected()) {
        productDisconnectedSince = 0;
        return;
    }

    unsigned long now = millis();
    if (productDisconnectedSince == 0) {
        productDisconnectedSince = now;
        return;
    }
    if (now - productDisconnectedSince < RECOVERY_AFTER_MS) return;   // let normal reconnects try first
    if (lastRecoveryScan != 0 && now - lastRecoveryScan < RECOVERY_SCAN_INTERVAL_MS) return;
    lastRecoveryScan = now;

    Serial.println("Product unreachable — scanning mDNS for serial " + productSerial);
    JsonDocument doc;
    JsonArray arr = doc["devices"].to<JsonArray>();
    String seenIPs = "|";
    if (platform == PLATFORM_MOZART) {
        collectService("_bangolufsen", "fn",   "sn",  0,  0, "mozart", arr, seenIPs);
    } else {
        collectService("_beoremote",   "name", "jid", 13, 8, "ase",    arr, seenIPs);
    }

    for (JsonObject dev : arr) {
        if (productSerial == dev["serial"].as<String>()) {
            String newIP = dev["ip"].as<String>();
            if (newIP != productIP) {
                Serial.println("Product moved from " + productIP + " to " + newIP + " — following");
                productIP = newIP;
                preferences.putString("productIP", productIP);
                if (platform == PLATFORM_MOZART) {
                    wsClient.close();
                    remoteClient.close();
                    wsClient.connect(("ws://" + productIP + ":" + WEBSOCKET_PORT).c_str());
                    remoteClient.connect(("ws://" + productIP + ":" + WEBSOCKET_PORT + "/remoteControl").c_str());
                } else {
                    sseClient.stop();
                    connectToSSE();
                }
            }
            return;
        }
    }
    Serial.println("Recovery scan found no product with matching serial");
}
