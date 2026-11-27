//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_board.h - on-device TFT debug lines and user screens
//-- Date: 2026-11-26 · Build: 18
//--
//-- Before the first telegram the TFT shows debug lines describing what the
//-- device is doing: the active browser screen name, telegram reception on
//-- P1, the start of the WiFi portal with its IP address, and the SSID and
//-- IP address after connecting. Once the first telegram arrives the screen
//-- is cleared and taken over by the user screens: PHASE LOAD (signed watt
//-- bars per phase plus the total, consumption red and feed-in green, the
//-- bar track a few pixels away from both panel edges on a fully white
//-- body, the bar scale following the ladder 100..18000 W), one PHASE
//-- POWER screen per phase plus a TOTAL screen (each showing the delivered
//-- and the returned watts of its channel as two large watt values under
//-- their left aligned captions) and three SYSTEM INFO screens (the device
//-- facts grouped in logical sections, a line too long for the panel split
//-- over two lines). Every screen has a light blue heading row with the
//-- screen name left and its function right, a light blue status row at the
//-- bottom carrying the accepted and rejected telegram counters and a fully
//-- white body; every text is drawn in the component's bold rendering, all
//-- texts are English and no line wraps. The rotary encoder is the only
//-- means to browse the screens; the AUX button never switches a screen, it
//-- only lights a dark panel, and every event on any button restarts the
//-- idle time-out, the idle time-out being a setting of the SPA Display
//-- group.
//-- All drawing goes through the esp32_s3_piggyback component; the board is
//-- never driven directly.
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

//-- --------------------------------------------------------------------------
//-- Display mode (the stored displayEnabled setting, the Display group's
//-- "Piggyback board" / "No Display" toggle of the settings view)
//-- --------------------------------------------------------------------------

//-- Brings the board up or tears it down following the stored display mode.
//-- True ("Piggyback board") drives the panel, the encoder and the buttons;
//-- false ("No Display") leaves the board untouched: the panel is never
//-- initialised, the backlight stays off and the board inputs are not
//-- started. Both calls are idempotent; the mode may switch at any time,
//-- also before any first init (the first "Piggyback board" selection brings
//-- the panel up on the spot, without a restart). While the mode is
//-- "No Display" every drawing and input function stays a harmless no-op.
esp_err_t loggerBoardSetEnabled(bool enabled);

//-- Turns the display 180 degrees through the component's rotation API so
//-- enclosures that mount the board the other way round read the panel
//-- correctly. False restores the assembled landscape orientation, true
//-- flips it. The active view is immediately redrawn for the new geometry;
//-- without a usable panel the call is a no-op.
void loggerBoardSetRotation(bool upsideDown);

//-- Applies the idle display time-out in minutes: the backlight goes off
//-- this many minutes after the last physical button action (an encoder
//-- turn or a button press). Every physical action while the panel is on
//-- restarts the timer; when the panel is dark only the AUX button lights
//-- it again and restarts the timer from that moment. A value of 0 keeps
//-- the panel permanently on. Without a usable panel the call is a no-op.
void loggerBoardSetDisplayTimeoutMinutes(uint16_t minutes);

//-- Shows the name of the screen the browser is currently on.
void loggerBoardShowScreen(const char* screen);

//-- Shows the parserDSMR library version on the boot debug lines, taken
//-- from dsmrParserVersion(). An empty or NULL value leaves the line blank.
void loggerBoardShowParserVersion(const char* version);

//-- Notes an accepted telegram on the P1 input.
void loggerBoardNoteTelegram(uint32_t sequence);

//-- Notes that the captive portal is active at the given URL.
void loggerBoardWifiPortal(const char* url);

//-- Notes the joined network and the assigned IP address.
void loggerBoardWifiConnected(const char* ssid, const char* ip);

//-- --------------------------------------------------------------------------
//-- Power panel (primary screen once the first telegram has been accepted)
//-- --------------------------------------------------------------------------

//-- One power panel frame. Watts are signed the way the panel reads them:
//-- negative is consumption (drawn red, bar to the left of centre) and
//-- positive is feed-in (green, to the right). The delivered and returned
//-- watts are the unsigned magnitudes behind the net figure (always >= 0).
//-- The totals aggregate the frame;
//-- sequence is the number of the accepted telegram and meterTime carries
//-- the date and time published in OBIS 0-0:1.0.0 as "DD-MM-YYYY HH:MM"
//-- (empty when the frame has none). The reception counters serve the
//-- status row; the other system fields serve the encoder-reached SYSTEM
//-- INFO screens: the active country and profile, the P1 RX polarity and
//-- the device uptime.
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
//-- picked, so both presentations scale identically. The steps roughly
//-- double and stay on round numbers, 100/200/400/800/1500/3000/6000/
//-- 12000/18000 W, so the top step covers the theoretical maximum of a
//-- three-phase 25 A / 230 V connection (17250 W). Frames above the
//-- largest step stay on that step.
#define LOGGER_POWER_SCALE_COUNT 9u
extern const uint16_t loggerPowerScales[LOGGER_POWER_SCALE_COUNT];

//-- Smallest ladder step that fits the largest absolute watt of the frame
//-- (the three phases and the total). Falls back to the largest step.
uint16_t loggerBoardPickPowerScale(const loggerPowerSnapshot* snapshot);

//-- Clears the screen and takes it over completely with the power screens:
//-- every screen starts with one light blue heading row (black screen name
//-- left aligned, screen function right aligned, both in the 13x24 native
//-- face) and ends with a light blue status row in the 9x16 face carrying
//-- only the accepted and rejected telegram counters; the body stays fully
//-- white. Every text on every screen is drawn in the component's bold
//-- rendering. The PHASE LOAD screen shows a
//-- row per phase plus Total, each with a header band (phase current left,
//-- phase label centred and voltage right, all in the 13x24 native face), a
//-- centre-zero watt bar carrying its integer 8x12 scale labels inside the
//-- bar, its track a few pixels away from both panel edges and whose scale
//-- follows the largest absolute watt on the smallest of the steps
//-- 100/200/400/800/1500/3000/6000/12000/18000 W, and the watt value in
//-- the scalable 6x8 face at scale 2. The PHASE POWER screens are four
//-- separate screens — one per phase plus the total — each showing that
//-- channel's delivered (import, red) and returned (export, green) watts
//-- as two large 17x32 values centred on the white body under their left
//-- aligned 13x24 captions "Delivered (Import)" and "Returned (Export)".
//-- The three
//-- SYSTEM INFO screens group the device facts into sections, every line
//-- in the 13x24 face and split over two lines when too long for the panel.
//-- No debug lines are drawn while the power screens are active, and new
//-- telegrams refresh the screen currently on view.
void loggerBoardShowPower(const loggerPowerSnapshot* snapshot);

//-- --------------------------------------------------------------------------
//-- Screen navigation (driven by the board component input events)
//-- --------------------------------------------------------------------------
//-- The rotary encoder is the only means to browse: it walks through all
//-- screens one by one, PHASE LOAD -> PHASE 1 -> PHASE 2 -> PHASE 3 ->
//-- TOTAL -> SYSTEM INFO (reception) -> SYSTEM INFO (meter & net) ->
//-- SYSTEM INFO (device) and back. The AUX button never switches a screen
//-- (it is reserved for leaving a future menu); while the idle time-out has
//-- the panel dark, any AUX press lights it again. Every event on any
//-- button — encoder turns, the encoder button and the AUX button —
//-- restarts the idle timer, lit or dark.

#endif //-- LOGGER_BOARD_H
