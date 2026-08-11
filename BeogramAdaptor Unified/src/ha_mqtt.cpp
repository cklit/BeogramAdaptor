#include "ha_mqtt.h"
#include "beogram.h"

// IMPORTANT — initialization order. The HA entities self-register with
// the HAMqtt instance from their constructors, so mqtt MUST be
// constructed before any entity. Within one translation unit C++
// guarantees top-to-bottom order; across files it guarantees nothing.
// Therefore wifi, device, mqtt and all entities live together in this
// file, in this order. Do not move any of them to another file.
WiFiClient wifi;
HADevice device;
HAMqtt mqtt(wifi, device);

String macToUnderscoreString(uint8_t* mac, size_t macLength) {
  String macStr;
  for (size_t i = 0; i < macLength; i++) {
    if (i > 0) macStr += "_";
    if (mac[i] < 0x10) macStr += "0";
    macStr += String(mac[i], HEX);
  }
  macStr.toLowerCase();
  return macStr;
}

HAButton bgPlay(idPlay); 
HAButton bgNext(idNext);   
HAButton bgPrev(idPrev);
HAButton bgStop(idStop);
HAButton bgStandby(idStandby);
HASensor bgTrack(idTrack);
HASensor bgPlaybackState(idPlayback);
HABinarySensor bgPlaying(idPlaying);

void checkMQTTConnection(bool forceNow) {
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

// ════════════════════════════════════════════════════════════════════
// ASE transport (SSE stream + BeoZone REST)
// ════════════════════════════════════════════════════════════════════

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
