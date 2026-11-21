//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_web.h - SPA and JSON API
//-- Date: 2026-11-21 · Build: 2
//--
//-- One esp_http_server instance serves the SPA and its API from the same
//-- origin. The API exposes active settings and valid choices, settings
//-- updates, reception status and diagnostics, recent telegram metadata, and
//-- individual telegram details and raw bytes. The SPA transfers only the
//-- recent metadata and the telegrams it needs. Handlers copy snapshots and
//-- never hold receive-side locks during network writes.
//-- ==========================================================================

#ifndef LOGGER_WEB_H
#define LOGGER_WEB_H

#include <stdbool.h>

#include "esp_err.h"
#include "logger_settings.h"

//-- Starts the server on port 80. Called after Wi-Fi has connected; the
//-- captive-portal server owns port 80 while provisioning is active. Calling
//-- it again while running is a no-op.
esp_err_t loggerWebStart(void);

bool loggerWebIsRunning(void);

//-- Notification of a successfully applied and persisted configuration: the
//-- application registers a callback to let the services that are not part
//-- of the receive path (network identity, MQTT) adopt the stored values.
//-- The callback runs on the request handling task with a copy of the
//-- complete configuration.
typedef void (*loggerWebSettingsSavedCb)(const loggerSettingsConfig* config);

void loggerWebSetSettingsSavedCallback(loggerWebSettingsSavedCb callback);

#endif //-- LOGGER_WEB_H
