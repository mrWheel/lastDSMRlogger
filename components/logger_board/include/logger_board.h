//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_board.h - on-device TFT debug lines
//-- Date: 2026-11-14 · Build: 1
//--
//-- The first release has no on-device user interface. The TFT shows only
//-- debug lines describing what the device is doing: the active browser
//-- screen name, telegram reception on P1, the start of the WiFi portal with
//-- its IP address, and the SSID and IP address after connecting. All drawing
//-- goes through the esp32_s3_piggyback component; the board is never driven
//-- directly.
//-- ==========================================================================

#ifndef LOGGER_BOARD_H
#define LOGGER_BOARD_H

#include <stdint.h>

#include "esp_err.h"

//-- Initializes the board through the component. If the panel cannot be
//-- brought up, every other function becomes a harmless no-op so the logger
//-- keeps running without a display.
esp_err_t loggerBoardInit(void);

//-- Shows the name of the screen the browser is currently on.
void loggerBoardShowScreen(const char *screen);

//-- Notes an accepted telegram on the P1 input.
void loggerBoardNoteTelegram(uint32_t sequence);

//-- Notes that the captive portal is active at the given URL.
void loggerBoardWifiPortal(const char *url);

//-- Notes the joined network and the assigned IP address.
void loggerBoardWifiConnected(const char *ssid, const char *ip);

#endif //-- LOGGER_BOARD_H
