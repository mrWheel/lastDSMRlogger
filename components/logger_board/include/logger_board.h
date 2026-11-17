//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_board.h - on-device TFT debug lines and power panel
//-- Date: 2026-11-16 · Build: 10
//--
//-- The first release has no on-device user interface. The TFT shows debug
//-- lines describing what the device is doing: the active browser screen
//-- name, telegram reception on P1, the start of the WiFi portal with its
//-- IP address, and the SSID and IP address after connecting. Once the
//-- first telegram arrives the screen is cleared and fully taken over by
//-- the power view: Fase 1-3 and Totaal signed watt bars (consumption red,
//-- feed-in green) with the phase current and voltage and the meter
//-- date/time of OBIS 0-0:1.0.0 right aligned on the title line. All
//-- drawing goes through the
//-- esp32_s3_piggyback component; the board is never driven directly.
//-- ==========================================================================

#ifndef LOGGER_BOARD_H
#define LOGGER_BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

//-- Initializes the board through the component. If the panel cannot be
//-- brought up, every other function becomes a harmless no-op so the logger
//-- keeps running without a display.
esp_err_t loggerBoardInit(void);

//-- Turns the display 180 degrees through the component's rotation API so
//-- enclosures that mount the board the other way round read the panel
//-- correctly. False restores the assembled landscape orientation, true
//-- flips it. The active view is immediately redrawn for the new geometry;
//-- without a usable panel the call is a no-op.
void loggerBoardSetRotation(bool upsideDown);

//-- Shows the name of the screen the browser is currently on.
void loggerBoardShowScreen(const char *screen);

//-- Shows the parserDSMR library version on the boot debug lines, taken
//-- from dsmrParserVersion(). An empty or NULL value leaves the line blank.
void loggerBoardShowParserVersion(const char *version);

//-- Notes an accepted telegram on the P1 input.
void loggerBoardNoteTelegram(uint32_t sequence);

//-- Notes that the captive portal is active at the given URL.
void loggerBoardWifiPortal(const char *url);

//-- Notes the joined network and the assigned IP address.
void loggerBoardWifiConnected(const char *ssid, const char *ip);

//-- --------------------------------------------------------------------------
//-- Power panel (primary screen once the first telegram has been accepted)
//-- --------------------------------------------------------------------------

//-- One power panel frame. Watts are signed the way the panel reads them:
//-- negative is consumption (drawn red, bar to the left of centre) and
//-- positive is feed-in (green, to the right). The delivered and returned
//-- watts are the unsigned magnitudes behind the net figure (always >= 0)
//-- and drive the AUX-button text screen. The totals aggregate the frame;
//-- sequence is the number of the accepted telegram and meterTime carries
//-- the date and time published in OBIS 0-0:1.0.0 as "DD-MM-YYYY HH:MM"
//-- (empty when the frame has none). The system fields serve the
//-- AUX-button medium-press view: the reception counters, the active
//-- country and profile, the P1 RX polarity and the device uptime.
typedef struct
{
  float phaseWatts[3];
  float phaseAmps[3];
  float phaseVolts[3];
  float phaseDeliveredWatts[3];
  float phaseReturnedWatts[3];
  float totalWatts;
  float totalAmps;
  float totalVolts;
  float totalDeliveredWatts;
  float totalReturnedWatts;
  uint32_t sequence;
  char meterTime[20];
  bool testData;
  uint32_t acceptedCount;
  uint32_t rejectedCount;
  char countryCode[4];
  char profileId[32];
  bool rxInverted;
  uint32_t uptimeSeconds;
} loggerPowerSnapshot;

//-- Watt scale ladder shared by the panel bars and the SPA realtime view:
//-- the smallest step that fits the largest absolute watt of a frame is
//-- picked, so both presentations scale identically. Frames above the
//-- largest step stay on that step.
#define LOGGER_POWER_SCALE_COUNT 6u
extern const uint16_t loggerPowerScales[LOGGER_POWER_SCALE_COUNT];

//-- Smallest ladder step that fits the largest absolute watt of the frame
//-- (the three phases and the total). Falls back to the largest step.
uint16_t loggerBoardPickPowerScale(const loggerPowerSnapshot *snapshot);

//-- Clears the screen and takes it over completely with the power panel:
//-- a white name bar ("TELEGRAM nnn" left aligned) with a sub band below
//-- it (meter date/time "DD-MM-YYYY HH:MM" right aligned), then a row per
//-- phase plus Totaal, each with a header band (phase current and voltage
//-- larger than the centred phase label), a centre-zero watt bar carrying
//-- its integer scale labels inside the bar and whose scale follows the
//-- largest absolute watt on the smallest of the steps
//-- 3800/3100/2400/1700/1000/500 W, and the watt value. Panel text uses
//-- the component's 5 by 7 font stretched to a per-text cell size. No
//-- debug lines are drawn while the panel is active, and new telegrams
//-- only refresh this view while an AUX view is open.
void loggerBoardShowPower(const loggerPowerSnapshot *snapshot);

//-- --------------------------------------------------------------------------
//-- AUX button views (driven by the board component input events)
//-- --------------------------------------------------------------------------
//-- A short AUX press from the graphics panel opens the power text screen
//-- (geleverd/opgewekte vermogen of the selected phase, the encoder cycles
//-- Fase 1 -> Fase 2 -> Fase 3 -> Totaal and back); a medium-long press
//-- replaces the bars with the system information view. A short press
//-- returns to the graphics panel.

#endif //-- LOGGER_BOARD_H
