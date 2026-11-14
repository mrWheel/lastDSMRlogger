//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_receiver.c - P1 receiver, parser state and telegram history
//-- Date: 2026-11-14 · Build: 1
//--
//-- The parserDSMR adapter task owns the parser instance while it runs. The
//-- event callback copies the complete accepted frame and the decoded fields
//-- out of the parser workspace within the documented callback lifetime. A
//-- configuration change stops and joins the receive task, initializes the
//-- new parser profile and starts the UART with the new settings; on any
//-- failure the previous configuration is restored.
//-- ==========================================================================

#include "logger_receiver.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "dsmr_uart_esp32.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "logger_board.h"

static const char *tag = "loggerReceiver";

//-- The P1 receiver uses a hardware UART separate from the console on UART0.
#define LOGGER_P1_UART_PORT UART_NUM_1

//-- Lower priority than the WiFi stack but above the HTTP server task so a
//-- slow browser cannot stall reception.
#define LOGGER_P1_TASK_PRIORITY 6
#define LOGGER_P1_TASK_STACK_BYTES 4096

//-- A last accepted record older than the larger of three profile intervals
//-- and this floor is reported as stale.
#define LOGGER_STALE_FLOOR_MS 15000u

typedef struct
{
  bool initialized;
  bool receiverRunning;
  bool hasActiveConfig;
  loggerSettingsConfig activeConfig;

  dsmrParser parser;
  uint8_t *workspace;
  size_t workspaceSize;
  dsmrUart uart;

  uint32_t nextSequence;
  uint8_t writeIndex;
  bool slotUsed[LOGGER_HISTORY_CAPACITY];
  loggerTelegramRecord records[LOGGER_HISTORY_CAPACITY];

  bool hasLastAccepted;
  uint32_t lastAcceptedUptimeMs;
  uint32_t acceptedCount;
  uint32_t rejectedCount;
  uint32_t dataLossCount;
  char lastDiagReason[48];
  char lastDiagObis[LOGGER_SNAPSHOT_OBIS_MAX];
  char lastError[96];

  //-- mutex guards history, status and the active configuration for readers
  //-- and for the receive task's event callback. controlMutex serializes
  //-- start/stop/reconfigure. They are never held simultaneously with the
  //-- mutex, so stopping the receive task can never deadlock against a
  //-- callback that is taking the mutex.
  SemaphoreHandle_t mutex;
  SemaphoreHandle_t controlMutex;
} LoggerReceiver;

static LoggerReceiver instance;

//-- Telegram buffer, raw frame copy, field array, M-Bus and label copies are
//-- all carved from this workspace by the parser.
static uint8_t parserWorkspace[DSMR_DEFAULT_WORKSPACE_BYTES];

//-- Staging record filled by the parser event callback. The parser refuses
//-- reentrant calls, so exactly one receive task ever writes this buffer.
static loggerTelegramRecord stagingRecord;

static void copyBounded(char *dst, size_t dstSize, const char *src)
{
  size_t length = strlen(src);

  if (length >= dstSize)
  {
    length = dstSize - 1u;
  }

  memcpy(dst, src, length);
  dst[length] = '\0';
}

static uint32_t uptimeMs(void)
{
  return (uint32_t)(esp_timer_get_time() / 1000LL);
}

//-- Appends a diagnostic note to a record, separated by semicolons.
static void appendNote(char *notes, size_t notesSize, const char *note)
{
  size_t length = strlen(notes);

  if ((length > 0u) && ((length + 2u) < notesSize))
  {
    snprintf(notes + length, notesSize - length, "; %s", note);
    return;
  }

  if (length == 0u)
  {
    snprintf(notes, notesSize, "%s", note);
  }
}

//-- True when the snapshot holds the published device type field of the
//-- given M-Bus channel (0-n:24.1.0).
static bool channelHasPublishedType(const loggerTelegramRecord *record, uint8_t channel)
{
  char obis[LOGGER_SNAPSHOT_OBIS_MAX];
  snprintf(obis, sizeof obis, "0-%u:24.1.0", (unsigned)channel);

  for (uint16_t index = 0u; index < record->meta.fieldCount; index++)
  {
    if (strcmp(record->fields[index].obis, obis) == 0)
    {
      return true;
    }
  }

  return false;
}

//-- True when the snapshot holds any field of the given M-Bus channel.
static bool channelPresent(const loggerTelegramRecord *record, uint8_t channel)
{
  for (uint16_t index = 0u; index < record->meta.fieldCount; index++)
  {
    if (record->fields[index].mbusChannel == channel)
    {
      return true;
    }
  }

  return false;
}

//-- --------------------------------------------------------------------------
//-- Parser event callback (receive task context)
//-- --------------------------------------------------------------------------

//-- Formats a parser timestamp as supplied by the parser.
static void formatTimestamp(const dsmrTimestamp *timestamp, char *out, size_t outSize)
{
  if (!timestamp->valid)
  {
    copyBounded(out, outSize, "invalid");
    return;
  }

  int written = snprintf(out, outSize, "%04u-%02u-%02uT%02u:%02u:%02u",
                         (unsigned)timestamp->year, (unsigned)timestamp->month, (unsigned)timestamp->day,
                         (unsigned)timestamp->hour, (unsigned)timestamp->minute, (unsigned)timestamp->second);

  if ((written < 0) || ((size_t)written >= outSize))
  {
    return;
  }

  if (timestamp->hasOffset)
  {
    int sign = (timestamp->offsetMinutes < 0) ? '-' : '+';
    int absolute = (timestamp->offsetMinutes < 0) ? (-timestamp->offsetMinutes) : timestamp->offsetMinutes;

    snprintf(out + written, outSize - (size_t)written, "%c%02d:%02d", sign, absolute / 60, absolute % 60);
  }
}

//-- Copies one decoded field into the snapshot. Values are formatted without
//-- locale dependence, so a measured zero and an unavailable reading remain
//-- distinguishable: the value string of an unavailable reading is empty.
static void copyFieldSnapshot(const dsmrTelegramField *field, loggerFieldSnapshot *snapshot)
{
  memset(snapshot, 0, sizeof *snapshot);
  copyBounded(snapshot->obis, sizeof snapshot->obis, (field->obis != NULL) ? field->obis : "");
  copyBounded(snapshot->unit, sizeof snapshot->unit, (field->unit != NULL) ? field->unit : "");
  snapshot->mbusChannel = field->mbusChannel;
  snapshot->hasTimestamp = field->hasTimestamp;
  snapshot->protocolInvalid = field->value.protocolInvalid;

  if (field->hasTimestamp)
  {
    formatTimestamp(&field->timestamp, snapshot->timestamp, sizeof snapshot->timestamp);
  }

  switch (field->value.kind)
  {
    case DSMR_VALUE_TEXT:
    {
      snapshot->valueIsText = true;
      copyBounded(snapshot->value, sizeof snapshot->value, (field->value.text != NULL) ? field->value.text : "");
      break;
    }

    case DSMR_VALUE_NUMBER:
    {
      if (dsmrFormatValue(&field->value, snapshot->value, sizeof snapshot->value) < 0)
      {
        snapshot->value[0] = '\0';
      }

      break;
    }

    case DSMR_VALUE_ABSENT:
    default:
    {
      snapshot->value[0] = '\0';
      break;
    }
  }
}

static void onParserEvent(const dsmrParser *parser, const dsmrParserEvent *event, void *userData)
{
  (void)parser;
  LoggerReceiver *self = (LoggerReceiver *)userData;

  if (event->type == DSMR_EVENT_TELEGRAM)
  {
    const dsmrTelegramResult *result = event->result;

    memset(&stagingRecord, 0, sizeof stagingRecord);
    stagingRecord.meta.receivedUptimeMs = result->receiveTimeMs;
    stagingRecord.meta.crcVerified = (result->crcStatus == DSMR_CRC_STATUS_VERIFIED);
    copyBounded(stagingRecord.meta.profileId, sizeof stagingRecord.meta.profileId, (result->profileId != NULL) ? result->profileId : "");
    copyBounded(stagingRecord.meta.reportedVersion, sizeof stagingRecord.meta.reportedVersion, (result->reportedVersion != NULL) ? result->reportedVersion : "");

    //-- The exact accepted frame: header, body, trailer bytes and every
    //-- original line ending, preserved by the parser before decoding.
    if ((result->rawBytes != NULL) && (result->rawLength > 0u))
    {
      size_t rawLength = (result->rawLength > DSMR_MAX_TELEGRAM_BYTES) ? DSMR_MAX_TELEGRAM_BYTES : result->rawLength;
      memcpy(stagingRecord.rawBytes, result->rawBytes, rawLength);
      stagingRecord.meta.rawLength = (uint16_t)rawLength;
    }

    uint16_t snapshotCount = result->fieldCount;

    if (snapshotCount > LOGGER_SNAPSHOT_FIELDS_MAX)
    {
      snapshotCount = LOGGER_SNAPSHOT_FIELDS_MAX;
    }

    for (uint16_t index = 0; index < snapshotCount; index++)
    {
      copyFieldSnapshot(&result->fields[index], &stagingRecord.fields[index]);
    }

    stagingRecord.meta.fieldCount = snapshotCount;

    xSemaphoreTake(self->mutex, portMAX_DELAY);

    //-- Disabled is a display setting of the logger at capture time; the raw
    //-- view keeps every channel line regardless.
    uint8_t disabledMask = 0u;

    for (uint8_t channel = DSMR_MBUS_CHANNEL_MIN; channel <= DSMR_MBUS_CHANNEL_MAX; channel++)
    {
      if (self->hasActiveConfig && (self->activeConfig.mbusMode[channel] == LOGGER_MBUS_MODE_DISABLED))
      {
        disabledMask |= (uint8_t)(1u << channel);
      }
    }

    stagingRecord.meta.disabledChannelsMask = disabledMask;

    //-- The parser reports a contradiction with a configured device type as a
    //-- diagnostic after the accepted telegram; the logger detects a missing
    //-- published type itself from the decoded fields.
    for (uint8_t channel = DSMR_MBUS_CHANNEL_MIN; channel <= DSMR_MBUS_CHANNEL_MAX; channel++)
    {
      if (channelPresent(&stagingRecord, channel) && !channelHasPublishedType(&stagingRecord, channel))
      {
        char note[48];
        snprintf(note, sizeof note, "missing published device type on channel %u", (unsigned)channel);
        appendNote(stagingRecord.meta.notes, sizeof stagingRecord.meta.notes, note);
      }
    }

    self->writeIndex = (uint8_t)((self->writeIndex + 1u) % LOGGER_HISTORY_CAPACITY);
    self->nextSequence++;
    stagingRecord.meta.sequence = self->nextSequence;
    self->slotUsed[self->writeIndex] = true;
    self->records[self->writeIndex] = stagingRecord;
    self->acceptedCount++;
    self->hasLastAccepted = true;
    self->lastAcceptedUptimeMs = result->receiveTimeMs;
    self->lastError[0] = '\0';
    xSemaphoreGive(self->mutex);

    loggerBoardNoteTelegram(stagingRecord.meta.sequence);
  }
  else if (event->type == DSMR_EVENT_DIAGNOSTIC)
  {
    const dsmrDiagnostic *diagnostic = event->diagnostic;

    xSemaphoreTake(self->mutex, portMAX_DELAY);
    self->rejectedCount++;

    if (diagnostic->reason == DSMR_DIAG_DATA_LOSS)
    {
      self->dataLossCount++;
    }

    if (diagnostic->reason == DSMR_DIAG_MBUS_TYPE_MISMATCH)
    {
      //-- The parser delivers this diagnostic right after the accepted
      //-- telegram of the frame it belongs to: annotate that record.
      if (self->slotUsed[self->writeIndex])
      {
        appendNote(self->records[self->writeIndex].meta.notes, sizeof self->records[self->writeIndex].meta.notes, "M-Bus device type mismatch");
      }
    }

    copyBounded(self->lastDiagReason, sizeof self->lastDiagReason, dsmrDiagReasonToString(diagnostic->reason));
    copyBounded(self->lastDiagObis, sizeof self->lastDiagObis, (diagnostic->obis != NULL) ? diagnostic->obis : "");
    xSemaphoreGive(self->mutex);
  }
}

//-- --------------------------------------------------------------------------
//-- Receiver start / stop (callers hold the mutex)
//-- --------------------------------------------------------------------------

static esp_err_t startLocked(LoggerReceiver *self, const loggerSettingsConfig *config)
{
  dsmrParserConfig parserConfig;
  memset(&parserConfig, 0, sizeof parserConfig);
  parserConfig.countryCode = config->countryCode;
  parserConfig.profileId = config->profileId;

  //-- Explicit channel selections are submitted as expected device types.
  //-- Auto channels follow the type reported in 0-n:24.1.0, and Disabled is
  //-- a display-only setting handled when building the decoded summary.
  dsmrMbusDevice devices[DSMR_MAX_MBUS_DEVICES];
  uint8_t deviceCount = loggerSettingsBuildMbusDevices(config, devices, DSMR_MAX_MBUS_DEVICES);
  parserConfig.mbusDevices = (deviceCount > 0u) ? devices : NULL;
  parserConfig.mbusDeviceCount = deviceCount;

  //-- The parser initializes before any bytes are consumed, so a restarted
  //-- receiver cannot consume data for a half applied profile.
  dsmrParserStatus status = dsmrParserInit(&self->parser, &parserConfig, self->workspace, self->workspaceSize);

  if (status != DSMR_OK)
  {
    xSemaphoreTake(self->mutex, portMAX_DELAY);
    snprintf(self->lastError, sizeof self->lastError, "parser init failed: %s", dsmrStatusToString(status));
    ESP_LOGE(tag, "%s", self->lastError);
    xSemaphoreGive(self->mutex);
    return ESP_FAIL;
  }

  dsmrUartConfig uartConfig;
  memset(&uartConfig, 0, sizeof uartConfig);
  uartConfig.uartPort = LOGGER_P1_UART_PORT;
  uartConfig.rxPin = config->rxGpio;
  uartConfig.txPin = -1;

  //-- The first release uses an externally enabled request signal.
  uartConfig.requestGpio = -1;
  uartConfig.requestActiveHigh = true;
  uartConfig.taskStackSize = LOGGER_P1_TASK_STACK_BYTES;
  uartConfig.taskPriority = LOGGER_P1_TASK_PRIORITY;

#if defined(CONFIG_LASTDSMRLOGGER_P1_RX_INVERTED)
  uartConfig.invertRx = true;
#else
  uartConfig.invertRx = false;
#endif

  esp_err_t result = dsmrUartStart(&self->uart, &self->parser, &uartConfig, onParserEvent, self);

  xSemaphoreTake(self->mutex, portMAX_DELAY);

  if (result != ESP_OK)
  {
    snprintf(self->lastError, sizeof self->lastError, "uart start failed on GPIO %d: %s", config->rxGpio, esp_err_to_name(result));
  }
  else
  {
    self->receiverRunning = true;
    self->lastError[0] = '\0';
  }

  xSemaphoreGive(self->mutex);

  if (result != ESP_OK)
  {
    ESP_LOGE(tag, "%s", self->lastError);
    return result;
  }

  return ESP_OK;
}

//-- Stops and joins the receive task. Never called with the data mutex held:
//-- the callback of the task being joined takes that mutex.
static void stopJoined(LoggerReceiver *self)
{
  if (!self->receiverRunning)
  {
    return;
  }

  dsmrUartStop(&self->uart);

  xSemaphoreTake(self->mutex, portMAX_DELAY);
  self->receiverRunning = false;
  xSemaphoreGive(self->mutex);
}

//-- --------------------------------------------------------------------------
//-- Public operations
//-- --------------------------------------------------------------------------

esp_err_t loggerReceiverInit(void)
{
  LoggerReceiver *self = &instance;
  memset(self, 0, sizeof *self);

  self->mutex = xSemaphoreCreateMutex();
  self->controlMutex = xSemaphoreCreateMutex();

  if ((self->mutex == NULL) || (self->controlMutex == NULL))
  {
    return ESP_ERR_NO_MEM;
  }

  dsmrParserConfig probe;
  memset(&probe, 0, sizeof probe);
  probe.countryCode = "NL";
  probe.profileId = "nl.dsmr.5.0";
  self->workspaceSize = dsmrParserWorkspaceSizeNeeded(&probe);

  if (self->workspaceSize > sizeof parserWorkspace)
  {
    ESP_LOGE(tag, "workspace too small: need %u have %u", (unsigned)self->workspaceSize, (unsigned)sizeof parserWorkspace);
    return ESP_ERR_INVALID_SIZE;
  }

  self->workspace = parserWorkspace;
  self->initialized = true;
  return ESP_OK;
}

esp_err_t loggerReceiverStart(const loggerSettingsConfig *config)
{
  LoggerReceiver *self = &instance;

  if (!self->initialized || (config == NULL))
  {
    return ESP_ERR_INVALID_STATE;
  }

  //-- Serializes receiver control against a concurrent configuration change.
  xSemaphoreTake(self->controlMutex, portMAX_DELAY);
  esp_err_t result = startLocked(self, config);

  if (result == ESP_OK)
  {
    xSemaphoreTake(self->mutex, portMAX_DELAY);
    self->activeConfig = *config;
    self->hasActiveConfig = true;
    self->hasLastAccepted = false;
    xSemaphoreGive(self->mutex);
  }

  xSemaphoreGive(self->controlMutex);
  return result;
}

esp_err_t loggerReceiverApplyConfig(const loggerSettingsConfig *config)
{
  LoggerReceiver *self = &instance;

  if (!self->initialized)
  {
    return ESP_ERR_INVALID_STATE;
  }

  //-- Reject invalid candidates before touching the active configuration.
  if (!loggerSettingsValidate(config))
  {
    return ESP_ERR_INVALID_ARG;
  }

  //-- Serializes receiver control; the data mutex is only taken for short
  //-- field copies, never around stop/join, so the receive task's callback
  //-- cannot deadlock against a configuration change.
  xSemaphoreTake(self->controlMutex, portMAX_DELAY);

  xSemaphoreTake(self->mutex, portMAX_DELAY);
  loggerSettingsConfig previous = self->activeConfig;
  bool hadActive = self->hasActiveConfig;
  xSemaphoreGive(self->mutex);

  stopJoined(self);

  esp_err_t result = startLocked(self, config);

  if (result != ESP_OK)
  {
    //-- The previous configuration stays active; a failed change restores it.
    if (hadActive)
    {
      esp_err_t restore = startLocked(self, &previous);

      if (restore != ESP_OK)
      {
        xSemaphoreTake(self->mutex, portMAX_DELAY);
        snprintf(self->lastError, sizeof self->lastError, "restore of previous configuration failed: %s", esp_err_to_name(restore));
        ESP_LOGE(tag, "%s", self->lastError);
        xSemaphoreGive(self->mutex);
      }
    }

    xSemaphoreGive(self->controlMutex);
    return result;
  }

  xSemaphoreTake(self->mutex, portMAX_DELAY);
  self->activeConfig = *config;
  self->hasActiveConfig = true;

  //-- Previously captured records stay available; the state returns to
  //-- awaiting data until the new profile accepts a telegram.
  self->hasLastAccepted = false;
  xSemaphoreGive(self->mutex);

  xSemaphoreGive(self->controlMutex);
  return ESP_OK;
}

void loggerReceiverGetStatus(loggerReceiverStatus *status)
{
  LoggerReceiver *self = &instance;

  if (status == NULL)
  {
    return;
  }

  memset(status, 0, sizeof *status);
  xSemaphoreTake(self->mutex, portMAX_DELAY);

  status->receiverRunning = self->receiverRunning;
  status->acceptedCount = self->acceptedCount;
  status->rejectedCount = self->rejectedCount;
  status->dataLossCount = self->dataLossCount;
  copyBounded(status->lastDiagReason, sizeof status->lastDiagReason, self->lastDiagReason);
  copyBounded(status->lastDiagObis, sizeof status->lastDiagObis, self->lastDiagObis);
  copyBounded(status->lastError, sizeof status->lastError, self->lastError);

  if (self->hasActiveConfig)
  {
    status->rxGpio = self->activeConfig.rxGpio;
    copyBounded(status->activeProfileId, sizeof status->activeProfileId, self->activeConfig.profileId);
  }

  uint32_t staleAfterMs = LOGGER_STALE_FLOOR_MS;
  const dsmrProfile *profile = self->hasActiveConfig ? dsmrProfileLookup(self->activeConfig.profileId) : NULL;

  if ((profile != NULL) && (profile->telegramIntervalMs * 3u > staleAfterMs))
  {
    staleAfterMs = profile->telegramIntervalMs * 3u;
  }

  status->staleAfterMs = staleAfterMs;
  status->uptimeMs = uptimeMs();

  if (!self->receiverRunning)
  {
    status->state = (self->lastError[0] != '\0') ? LOGGER_RECEIVER_ERROR : LOGGER_RECEIVER_AWAITING_CONFIGURATION;
  }
  else if (!self->hasLastAccepted)
  {
    status->state = LOGGER_RECEIVER_AWAITING_DATA;
  }
  else
  {
    status->hasLastAccepted = true;
    status->lastAcceptedAgeMs = (uint32_t)(status->uptimeMs - self->lastAcceptedUptimeMs);
    status->state = (status->lastAcceptedAgeMs > staleAfterMs) ? LOGGER_RECEIVER_STALE : LOGGER_RECEIVER_OK;
  }

  xSemaphoreGive(self->mutex);
}

bool loggerReceiverGetActiveConfig(loggerSettingsConfig *config)
{
  LoggerReceiver *self = &instance;

  if (config == NULL)
  {
    return false;
  }

  xSemaphoreTake(self->mutex, portMAX_DELAY);
  bool found = self->hasActiveConfig;

  if (found)
  {
    *config = self->activeConfig;
  }

  xSemaphoreGive(self->mutex);
  return found;
}

uint16_t loggerReceiverListRecords(loggerTelegramMeta *outRecords, uint16_t maxRecords)
{
  LoggerReceiver *self = &instance;

  if ((outRecords == NULL) || (maxRecords == 0u))
  {
    return 0u;
  }

  xSemaphoreTake(self->mutex, portMAX_DELAY);
  uint16_t written = 0u;

  //-- Newest first.
  for (uint16_t offset = 0u; (offset < LOGGER_HISTORY_CAPACITY) && (written < maxRecords); offset++)
  {
    uint8_t slot = (uint8_t)((self->writeIndex + LOGGER_HISTORY_CAPACITY - offset) % LOGGER_HISTORY_CAPACITY);

    if (!self->slotUsed[slot])
    {
      continue;
    }

    outRecords[written] = self->records[slot].meta;
    written++;
  }

  xSemaphoreGive(self->mutex);
  return written;
}

bool loggerReceiverGetRecord(uint32_t sequence, loggerTelegramRecord *outRecord)
{
  LoggerReceiver *self = &instance;

  if (outRecord == NULL)
  {
    return false;
  }

  bool found = false;
  xSemaphoreTake(self->mutex, portMAX_DELAY);

  for (uint8_t slot = 0; slot < LOGGER_HISTORY_CAPACITY; slot++)
  {
    if (self->slotUsed[slot] && (self->records[slot].meta.sequence == sequence))
    {
      *outRecord = self->records[slot];
      found = true;
      break;
    }
  }

  xSemaphoreGive(self->mutex);
  return found;
}
