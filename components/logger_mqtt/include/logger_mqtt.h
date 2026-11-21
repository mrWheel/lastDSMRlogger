//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_mqtt.h - MQTT publication of the latest accepted telegram
//-- Date: 2026-11-21 · Build: 4
//--
//-- When the stored configuration names a broker and a broadcast interval
//-- above zero, a background task publishes the most recently accepted
//-- telegram to "<topTopic>/telegram" every interval seconds, as one JSON
//-- document {"actual":{...}} with one entry per decoded field: the field's
//-- display label (adjusted label, else the profile's built-in name, else
//-- the OBIS code) mapped onto the parsed value — a JSON number when the
//-- value text spells a decimal number, a JSON string otherwise. A broker change
//-- through the settings view reconfigures the client live; interval 0 or an
//-- empty broker keeps publication off.
//-- The client only ever runs while the station has joined: the application
//-- reports that with loggerMqttSetNetworkReady() (LWIP is not up before
//-- the join, and the client would crash on its first broker lookup). Once
//-- up, the client retries on its own when the broker disappears.
//-- ==========================================================================

#ifndef LOGGER_MQTT_H
#define LOGGER_MQTT_H

#include <stdbool.h>

#include "esp_err.h"
#include "logger_settings.h"

//-- Applies a complete configuration: stores it, (re)starts the client when
//-- publication is requested and stops it when it is not. Callable at every
//-- moment, including at boot before Wi-Fi joined. Returns ESP_ERR_INVALID_ARG
//-- for a null argument and ESP_ERR_NO_MEM when the client could not be
//-- created; publication simply stays off in that case.
esp_err_t loggerMqttApplyConfig(const loggerSettingsConfig* config);

//-- Reports whether the station has joined. The client is brought up while
//-- ready and publication is configured, and taken down when the station
//-- drops. The application calls this from its Wi-Fi connect/disconnect
//-- paths; it is safe before the first loggerMqttApplyConfig().
void loggerMqttSetNetworkReady(bool ready);

//-- True when publication is configured (broker named and interval above
//-- zero) and the client is connected to the broker.
bool loggerMqttIsConnected(void);

#endif //-- LOGGER_MQTT_H
