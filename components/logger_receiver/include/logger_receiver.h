//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_receiver.h - P1 receiver, parser state and telegram history
//-- Date: 2026-11-21 · Build: 8
//--
//-- One receive task owns the receiver and parser state: the parserDSMR UART
//-- adapter task feeds the parser and the event callback copies everything the
//-- SPA needs within the documented callback lifetime. Configuration changes
//-- stop and join the receive task before reinitializing the parser and
//-- starting the UART with the new settings. The history is a bounded RAM ring
//-- that always accommodates the documented minimum number of maximum-sized
//-- accepted telegram records. Slow or disconnected browser clients have no
//-- effect on UART reception.
//-- ==========================================================================

#ifndef LOGGER_RECEIVER_H
#define LOGGER_RECEIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "dsmr_parser.h"
#include "esp_err.h"
#include "logger_board.h"
#include "logger_settings.h"

//-- --------------------------------------------------------------------------
//-- History bounds (RAM limits of the documented target configuration)
//-- --------------------------------------------------------------------------

//-- Records retained in RAM. The oldest record is evicted first. Two
//-- complete telegrams are kept: enough for the console and the SPA to
//-- show the newest frame while the next one is being received.
#define LOGGER_HISTORY_CAPACITY 2

//-- Measurements retained for the SPA realtime views: one sample per
//-- accepted telegram, oldest first when read back. The total watts feed
//-- the Phase Load bar history, the per-phase delivered/returned watts and
//-- the held gas rate feed the Actual Graph rolling table. The ring is
//-- sized at init: it grows to the doubled capacity whenever the heap can
//-- carry it and falls back to the baseline otherwise (see
//-- loggerReceiverGetPowerHistoryCapacity). Each sample carries the time it
//-- belongs to as "HH:MM:SS" so the SPA can label a hovered bar.
#define LOGGER_POWER_HISTORY_CAPACITY 100u
#define LOGGER_POWER_HISTORY_CAPACITY_MAX 200u
#define LOGGER_POWER_HISTORY_TIME_MAX 12

typedef struct
{
  //-- Signed net total watts: negative is consumption, positive feed-in
  //-- (the Phase Load bar history reads this figure).
  float watts;

  //-- Unsigned magnitudes per phase (always >= 0) as published on the
  //-- delivered 1-0:21/41/61.7.0 and returned 1-0:22/42/62.7.0 readings:
  //-- the two stacked series rows of the Actual Graph.
  float phaseUsedWatts[3];
  float phaseReturnedWatts[3];

  //-- Gas consumption rate in m³/h: the rate of the last observed increase
  //-- of the gas counter, held until the counter moves again (the gas field
  //-- only arrives with every tenth telegram or so, so a per-telegram
  //-- difference would be a row of zeros with one spike).
  float gasM3h;

  //-- "HH:MM:SS": the meter time of OBIS 0-0:1.0.0, or the device wall clock
  //-- when the frame published none. Empty when neither source is available.
  char timeText[LOGGER_POWER_HISTORY_TIME_MAX];
} loggerPowerHistorySample;

//-- Field snapshot bound per record. Telegrams with more published fields
//-- keep their complete raw view; the decoded summary truncates at this cap.
#define LOGGER_SNAPSHOT_FIELDS_MAX 96

#define LOGGER_SNAPSHOT_OBIS_MAX 16
#define LOGGER_SNAPSHOT_NAME_MAX 24
#define LOGGER_SNAPSHOT_VALUE_MAX 48
#define LOGGER_SNAPSHOT_UNIT_MAX 12
#define LOGGER_SNAPSHOT_TIMESTAMP_MAX 32
#define LOGGER_SNAPSHOT_PROFILE_MAX LOGGER_SETTINGS_PROFILE_ID_MAX
#define LOGGER_SNAPSHOT_VERSION_MAX 16

//-- One decoded field copied out of the parser workspace. A number formatted
//-- to an empty value string marks an unavailable reading; a measured zero
//-- stays distinguishable from it.
typedef struct
{
  char obis[LOGGER_SNAPSHOT_OBIS_MAX];

  //-- Default display name of the code as published by the parser: the
  //-- built-in name from the active profile field table.
  char name[LOGGER_SNAPSHOT_NAME_MAX];

  char value[LOGGER_SNAPSHOT_VALUE_MAX];
  char unit[LOGGER_SNAPSHOT_UNIT_MAX];
  char timestamp[LOGGER_SNAPSHOT_TIMESTAMP_MAX];
  bool hasTimestamp;
  bool valueIsText;
  bool protocolInvalid;
  uint8_t mbusChannel;
} loggerFieldSnapshot;

typedef struct
{
  uint32_t sequence;
  uint32_t receivedUptimeMs;
  char profileId[LOGGER_SNAPSHOT_PROFILE_MAX];
  char reportedVersion[LOGGER_SNAPSHOT_VERSION_MAX];

  //-- Meter time of OBIS 0-0:1.0.0 as "HH:MM:SS" (the YYMMDDHHMMSSW stamp,
  //-- for example 260208173738S shown as 17:37:38). Empty when the frame
  //-- published no usable timestamp.
  char meterTimeText[12];

  bool crcVerified;
  uint16_t rawLength;
  uint16_t fieldCount;

  //-- Bit n set means M-Bus channel n was disabled when the record was
  //-- captured: the decoded summary omits the channel while its lines stay
  //-- visible in the raw view.
  uint8_t disabledChannelsMask;

  //-- Capture-time decoded diagnostics, for example a channel that published
  //-- no device type. The parser reports a contradiction as a diagnostic
  //-- event right after the accepted telegram and it is appended here too.
  char notes[128];
} loggerTelegramMeta;

typedef struct
{
  loggerTelegramMeta meta;
  uint8_t rawBytes[DSMR_MAX_TELEGRAM_BYTES];
  loggerFieldSnapshot fields[LOGGER_SNAPSHOT_FIELDS_MAX];
} loggerTelegramRecord;

//-- Inspect one retained record in place instead of copying the whole
//-- 23 KB structure: the callback receives a pointer to the retained slot
//-- while the receive-side lock is held. The callback must only read from
//-- the record and must never block or touch the network. Returns false
//-- when the sequence is not (or no longer) retained.
bool loggerReceiverInspectRecord(uint32_t sequence,
                                 bool (*inspect)(const loggerTelegramRecord* record,
                                                 void* userData),
                                 void* userData);

//-- --------------------------------------------------------------------------
//-- Reception status and diagnostics
//-- --------------------------------------------------------------------------

typedef enum
{
  //-- No valid configuration: the receiver is idle and awaits settings.
  LOGGER_RECEIVER_AWAITING_CONFIGURATION = 0,

  //-- The receiver runs but no telegram has been accepted yet.
  LOGGER_RECEIVER_AWAITING_DATA,

  //-- A telegram was accepted within the staleness window.
  LOGGER_RECEIVER_OK,

  //-- The last accepted record aged beyond the staleness window.
  LOGGER_RECEIVER_STALE,

  //-- The receiver could not be started or failed to restore.
  LOGGER_RECEIVER_ERROR,
} loggerReceiverState;

typedef struct
{
  loggerReceiverState state;
  bool receiverRunning;

  //-- Active receiver-side values, kept separate from the stored settings
  //-- while a change is in progress.
  int rxGpio;

  //-- Active read interval in seconds: at most one telegram is accepted
  //-- per interval.
  uint32_t intervalSeconds;

  char activeProfileId[LOGGER_SNAPSHOT_PROFILE_MAX];

  uint32_t uptimeMs;
  uint32_t lastAcceptedAgeMs;
  bool hasLastAccepted;
  uint32_t staleAfterMs;

  uint32_t acceptedCount;
  uint32_t rejectedCount;
  uint32_t dataLossCount;
  char lastDiagReason[48];
  char lastDiagObis[LOGGER_SNAPSHOT_OBIS_MAX];

  char lastError[96];
} loggerReceiverStatus;

//-- --------------------------------------------------------------------------
//-- Operations
//-- --------------------------------------------------------------------------

//-- Prepares the receiver and history without starting reception. Call once
//-- before app_main starts other components.
esp_err_t loggerReceiverInit(void);

//-- Starts reception with the given validated configuration. Used at boot and
//-- as the restore path when a change fails.
esp_err_t loggerReceiverStart(const loggerSettingsConfig* config);

//-- Applies a complete configuration change as one operation: stop and join
//-- the receive task, initialize the new parser profile, start the UART with
//-- the new settings. On failure the previous configuration is restored and
//-- the error is returned. Returns ESP_ERR_INVALID_ARG for an invalid
//-- candidate; the active configuration is then untouched.
esp_err_t loggerReceiverApplyConfig(const loggerSettingsConfig* config);

//-- Copies a consistent status snapshot. Safe from any task.
void loggerReceiverGetStatus(loggerReceiverStatus* status);

//-- Copies the configuration the receiver is currently running with. False
//-- when no configuration has been activated yet.
bool loggerReceiverGetActiveConfig(loggerSettingsConfig* config);

//-- Copies the power frame of the most recently accepted telegram (the same
//-- frame the TFT panel shows) together with the uptime at which it was
//-- captured. False when no telegram has been accepted yet, in which case
//-- the outputs are untouched. Safe from any task.
bool loggerReceiverGetPowerSnapshot(loggerPowerSnapshot* snapshot, uint32_t* capturedUptimeMs);

//-- Copies the newest retained power history samples (one per accepted
//-- telegram, oldest first) into outSamples, at most maxSamples of them:
//-- when fewer are asked than retained, the newest ones are copied. Each
//-- sample carries the total watts, the per-phase delivered and returned
//-- watts, the held gas rate and its "HH:MM:SS" time text. Returns the
//-- number of samples written. Safe from any task.
uint16_t loggerReceiverGetPowerHistory(loggerPowerHistorySample* outSamples, uint16_t maxSamples);

//-- Number of sample slots the power history ring actually got at init:
//-- between LOGGER_POWER_HISTORY_CAPACITY and LOGGER_POWER_HISTORY_CAPACITY_MAX,
//-- depending on the memory that was available then. Callers size their
//-- copy buffers (and the Actual Graph plot) from this figure.
uint16_t loggerReceiverGetPowerHistoryCapacity(void);

//-- Copies metadata of the retained records, newest first. Returns the
//-- number of records written.
uint16_t loggerReceiverListRecords(loggerTelegramMeta* outRecords, uint16_t maxRecords);

//-- Copies one retained record by sequence number. False when it aged out.
bool loggerReceiverGetRecord(uint32_t sequence, loggerTelegramRecord* outRecord);

#endif //-- LOGGER_RECEIVER_H
