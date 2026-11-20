//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_telnet.h - telnet console server on port 23
//-- Date: 2026-11-19 · Build: 1
//--
//-- One operator at a time: claiming a telnet session takes control away
//-- from the web page (which shows its takeover popup) and a second telnet
//-- client takes it from the first (which receives a clear farewell line).
//-- The console answers a small read-only command set about reception, the
//-- retained telegrams, the power frame and the ring file.
//-- ==========================================================================

#ifndef LOGGER_TELNET_H
#define LOGGER_TELNET_H

#include "esp_err.h"

//-- Starts the telnet console task and its listening socket on port 23.
//-- Idempotent: later calls simply report the running state. Start it once
//-- the station has an address, together with the SPA server.
esp_err_t loggerTelnetStart(void);

#endif //-- LOGGER_TELNET_H
