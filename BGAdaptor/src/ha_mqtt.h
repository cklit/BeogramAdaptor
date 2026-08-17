#pragma once
#include <ArduinoHA.h>
#include "state.h"

// Home Assistant / MQTT: device, entities, connection management, and
// HA-originated button commands.

extern WiFiClient wifi;
extern HADevice device;
extern HAMqtt mqtt;
extern HAButton bgPlay;
extern HAButton bgNext;
extern HAButton bgPrev;
extern HAButton bgStop;
extern HAButton bgStandby;
extern HASensor bgTrack;
extern HASensor bgPlaybackState;
extern HABinarySensor bgPlaying;

String macToUnderscoreString(uint8_t* mac, size_t macLength);
void checkMQTTConnection(bool forceNow = false);
void onButtonCommand(HAButton* sender);
