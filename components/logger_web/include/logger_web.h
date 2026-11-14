//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_web.h - SPA and JSON API
//-- Date: 2026-11-14 · Build: 1
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

//-- Starts the server on port 80. Called after Wi-Fi has connected; the
//-- captive-portal server owns port 80 while provisioning is active. Calling
//-- it again while running is a no-op.
esp_err_t loggerWebStart(void);

bool loggerWebIsRunning(void);

#endif //-- LOGGER_WEB_H
