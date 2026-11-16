//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_receiver.c - P1 receiver, parser state and telegram history
//-- Date: 2026-11-16 · Build: 13
//--
//-- The parserDSMR adapter task owns the parser instance while it runs. The
//-- event callback copies the complete accepted frame and the decoded fields
//-- out of the parser workspace within the documented callback lifetime. A
//-- configuration change stops and joins the receive task, initializes the
//-- new parser profile and starts the UART with the new settings; on any
//-- failure the previous configuration is restored.
//--
//-- Every accepted frame is also forwarded on the UART0 TX pin by a small
//-- dedicated task: at most one burst per five seconds (or per interval when
//-- larger, so above four seconds every accepted telegram is forwarded) and
//-- every burst repeats the exact frame twice. A burst suspends reception
//-- for its whole duration: the live path joins the adapter receive task,
//-- the test generator discards its frames meanwhile, bytes arriving on the
//-- RX pin are dropped uncounted, the parser discards its partial frame and
//-- reception resumes through the parser's own start-of-telegram detection.
//-- The P1 Tx polarity (Forwarding Data) setting drives both copies; outside
//-- a burst TX idles at the polarity of the P1 Rx polarity setting. Test mode
//-- installs its own TX driver because it opens no receive UART.
//-- ==========================================================================

#include "logger_receiver.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "dsmr_uart_esp32.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "logger_board.h"
#include "logger_testdata.h"
#include "nvs.h"

static const char *tag = "loggerReceiver";

//-- The P1 port of the smart meter always uses UART0: RX on GPIO44 (the
//-- stored rxGpio of a live configuration) and TX on GPIO43. The console
//-- runs on the native USB serial/JTAG peripheral, so UART0 is free.
#define LOGGER_P1_UART_PORT UART_NUM_0

//-- Lower priority than the WiFi stack but above the HTTP server task so a
//-- slow browser cannot stall reception.
#define LOGGER_P1_TASK_PRIORITY 6
#define LOGGER_P1_TASK_STACK_BYTES 4096

//-- A last accepted record older than the larger of three profile intervals
//-- and this floor is reported as stale.
#define LOGGER_STALE_FLOOR_MS 15000u

//-- Minimum gap between two forwarding bursts on the UART0 TX pin. A burst
//-- sends the accepted frame twice back to back; when the configured read
//-- interval is larger than this floor, one burst per interval applies.
#define LOGGER_FORWARD_MIN_GAP_MS 5000u
#define LOGGER_FORWARD_TASK_STACK_BYTES 4096
#define LOGGER_FORWARD_TASK_PRIORITY 4

//-- TX ring of the driver a burst installs: sized to the frame plus slack,
//-- so the full copy is buffered in one call and the ISR drains it at line
//-- rate; uart_write_bytes() returns once the bytes are queued and
//-- uart_wait_tx_done() confirms the copy left the pin. Sizing the ring to
//-- the frame instead of the DSMR maximum keeps the heap free for the web
//-- requests running alongside a burst.
#define LOGGER_FORWARD_TX_SLACK_BYTES 512

//-- RX ring of that driver: bytes arriving on the RX pin while reception is
//-- suspended land here and are discarded; they never reach the parser.
#define LOGGER_FORWARD_RX_BUFFER_BYTES 256

//-- Budget for one copy to finish on the wire: a 13000-byte copy takes
//-- about 1.5 s at 115200 baud, the margin absorbs ISR jitter. A timeout
//-- aborts the burst safely and reception resumes.
#define LOGGER_FORWARD_TX_WAIT_MS 5000

//-- Budget for the test generator to leave a running dsmrParserFeed() call
//-- when a burst suspends the parser.
#define LOGGER_FORWARD_FEED_JOIN_MS 2000

//-- Delay slice of the test data generator so a stop joins the task quickly
//-- even with a 600 s interval.
#define LOGGER_TEST_TASK_SLICE_MS 50u

//-- The accepted-telegram total is persisted to NVS every ten minutes so the
//-- running counter survives a reset or a power cycle. On boot the receiver
//-- restores the last persisted value and counts on from there.
#define LOGGER_TELEGRAM_COUNTER_PERSIST_INTERVAL_MS 600000u
#define LOGGER_TELEGRAM_COUNTER_TASK_STACK_BYTES 4096
#define LOGGER_TELEGRAM_COUNTER_TASK_PRIORITY 3

//-- NVS storage of the counter; the namespace is shared with the settings
//-- service and the partition is initialised by the application at boot.
#define LOGGER_NVS_NAMESPACE "dsmrlog"
#define LOGGER_NVS_KEY_TELEGRAM_COUNT "telegramCount"

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

  //-- Retained history, allocated at init from PSRAM when the module has
  //-- it: the slots are only ever touched by task code, so they do not
  //-- belong on the small internal heap (see allocTelegramBuffer).
  loggerTelegramRecord *records;

  bool hasLastAccepted;
  uint32_t lastAcceptedUptimeMs;

  //-- Uptime of the last forwarding burst on the UART0 TX pin; the next
  //-- burst waits for the larger of five seconds and the read interval.
  uint32_t lastForwardUptimeMs;

  //-- Sequence of the accepted telegram selected for the next UART0 TX
  //-- burst. The burst sends straight from the retained history slot of
  //-- that sequence: reception is suspended for the whole burst, so no new
  //-- accept can recycle the slot while its bytes are on the wire (history
  //-- capacity would first need four new frames), and the exact accepted
  //-- bytes and their length stay valid for the entire operation.
  bool forwardPending;
  uint32_t forwardSequence;

  //-- True while the receiver itself owns the UART0 driver (test mode keeps
  //-- it installed across bursts). The live path installs the driver only
  //-- while a burst runs; between bursts the parserDSMR adapter owns it.
  bool ownsTxDriver;

  //-- True while a burst owns UART0. The test generator discards the frames
  //-- it builds meanwhile: they must never reach the parser, update logger
  //-- data or trigger another burst.
  bool forwarding;

  //-- True while the test generator is inside dsmrParserFeed(); a burst
  //-- waits for it to clear before touching the parser state.
  volatile bool generatorFeeding;

  //-- Line-inverse mask currently applied to the receiver-owned UART0
  //-- driver, or -1 when unknown (freshly installed drivers start with the
  //-- mask cleared). Identical consecutive masks skip the hardware update.
  int appliedInverseMask;

  //-- Wakes the forwarding task after the callback selected a frame.
  SemaphoreHandle_t forwardWake;

  uint32_t acceptedCount;
  uint32_t rejectedCount;
  uint32_t dataLossCount;
  char lastDiagReason[48];
  char lastDiagObis[LOGGER_SNAPSHOT_OBIS_MAX];
  char lastError[96];

  //-- Console deduplication: the reason and OBIS of the diagnostic that was
  //-- last logged. Repeated identical diagnostics (an M-Bus type mismatch
  //-- repeats for every subfield of every telegram) reach the console only
  //-- once per reception session; the counter and the web status stay
  //-- complete. Reset on every receiver (re)start so a boot or a settings
  //-- change shows the current diagnostics again.
  char loggedDiagText[70];
  bool loggedDiagValid;

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
static uint8_t *parserWorkspace;

//-- Staging record filled by the parser event callback. The parser refuses
//-- reentrant calls, so exactly one receive task ever writes this buffer.
//-- Both buffers are allocated by loggerReceiverInit from PSRAM when the
//-- module carries it (see allocTelegramBuffer).
static loggerTelegramRecord *stagingRecord;

//-- Carves one of the big telegram buffers (parser workspace, staging
//-- record, retained history), preferring PSRAM: they are only touched by
//-- task code, never by an ISR or a DMA descriptor. The internal fallback
//-- keeps the logger alive on a module without external RAM.
static void *allocTelegramBuffer(size_t size)
{
  void *buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);

  if (buffer == NULL)
  {
    buffer = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }

  return buffer;
}

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

//-- True when the text is exactly "HH:MM:SS".
static bool isHmsText(const char *text)
{
  return isdigit((unsigned char)text[0]) && isdigit((unsigned char)text[1]) && (text[2] == ':') &&
         isdigit((unsigned char)text[3]) && isdigit((unsigned char)text[4]) && (text[5] == ':') &&
         isdigit((unsigned char)text[6]) && isdigit((unsigned char)text[7]);
}

//-- The Telegrams log rows show the meter time published in OBIS 0-0:1.0.0:
//-- the YYMMDDHHMMSSW stamp (for example 260208173738S) read as 17:37:38.
//-- The parser timestamp of the field is preferred; a plain digit value is
//-- parsed as fallback. The text stays empty when the frame has no usable
//-- timestamp.
static void extractMeterTimeText(loggerTelegramRecord *record)
{
  record->meta.meterTimeText[0] = '\0';

  for (uint16_t index = 0u; index < record->meta.fieldCount; index++)
  {
    const loggerFieldSnapshot *field = &record->fields[index];

    if (strcmp(field->obis, "0-0:1.0.0") != 0)
    {
      continue;
    }

    if (field->hasTimestamp)
    {
      const char *timePart = strchr(field->timestamp, 'T');

      if ((timePart != NULL) && isHmsText(timePart + 1))
      {
        memcpy(record->meta.meterTimeText, timePart + 1, 8);
        record->meta.meterTimeText[8] = '\0';
      }

      return;
    }

    if (strlen(field->value) >= 12u)
    {
      bool allDigits = true;

      for (uint8_t position = 0u; position < 12u; position++)
      {
        if (!isdigit((unsigned char)field->value[position]))
        {
          allDigits = false;
          break;
        }
      }

      if (allDigits)
      {
        snprintf(record->meta.meterTimeText, sizeof record->meta.meterTimeText, "%c%c:%c%c:%c%c",
                 field->value[6], field->value[7], field->value[8], field->value[9],
                 field->value[10], field->value[11]);
      }
    }

    return;
  }
}

//-- --------------------------------------------------------------------------
//-- Power panel snapshot for the TFT (receive task context)
//-- --------------------------------------------------------------------------

//-- Copies the first numeric value published for the given OBIS code. An
//-- unavailable reading (empty value) and a text field count as absent.
static bool findNumericField(const loggerTelegramRecord *record, const char *obis, float *outValue)
{
  for (uint16_t index = 0u; index < record->meta.fieldCount; index++)
  {
    const loggerFieldSnapshot *field = &record->fields[index];

    if ((strcmp(field->obis, obis) == 0) && (field->value[0] != '\0') && !field->valueIsText)
    {
      *outValue = (float)atof(field->value);
      return true;
    }
  }

  return false;
}

//-- The power panel title shows the meter date and time of OBIS 0-0:1.0.0
//-- as "DD-MM-YYYY HH:MM" (no seconds): the parser timestamp is preferred,
//-- a plain YYMMDDHHMMSSW digit value is the fallback. Empty when unusable.
static void extractPanelMeterTime(const loggerTelegramRecord *record, char *out, size_t outSize)
{
  out[0] = '\0';

  for (uint16_t index = 0u; index < record->meta.fieldCount; index++)
  {
    const loggerFieldSnapshot *field = &record->fields[index];

    if (strcmp(field->obis, "0-0:1.0.0") != 0)
    {
      continue;
    }

    if (field->hasTimestamp)
    {
      const char *timePart = strchr(field->timestamp, 'T');

      if ((timePart != NULL) && ((size_t)(timePart - field->timestamp) == 10u) && isHmsText(timePart + 1))
      {
        snprintf(out, outSize, "%c%c-%c%c-%c%c%c%c %c%c:%c%c",
                 field->timestamp[8], field->timestamp[9], field->timestamp[5], field->timestamp[6],
                 field->timestamp[0], field->timestamp[1], field->timestamp[2], field->timestamp[3],
                 timePart[1], timePart[2], timePart[4], timePart[5]);
      }

      return;
    }

    if (strlen(field->value) >= 12u)
    {
      bool allDigits = true;

      for (uint8_t position = 0u; position < 12u; position++)
      {
        if (!isdigit((unsigned char)field->value[position]))
        {
          allDigits = false;
          break;
        }
      }

      if (allDigits)
      {
        snprintf(out, outSize, "%c%c-%c%c-20%c%c %c%c:%c%c",
                 field->value[4], field->value[5], field->value[2], field->value[3],
                 field->value[0], field->value[1], field->value[6], field->value[7],
                 field->value[8], field->value[9]);
      }
    }

    return;
  }
}

//-- Builds the signed power frame for the TFT panel from the decoded fields,
//-- following the DSMRlogger32 power bars: the net power of a phase is
//-- returned minus delivered (negative is consumption, drawn red; positive
//-- is feed-in, drawn green). The profile publishes delivered kW on
//-- 1-0:21/41/61.7.0, returned kW on 1-0:22/42/62.7.0, current on
//-- 1-0:31/51/71.7.0 and voltage on 1-0:32/52/72.7.0 (cross-checked with
//-- dsmr2Lib). The header current is the signed net watt over the phase
//-- voltage; the published unsigned current is the fallback magnitude. The
//-- totals use the published total delivered/returned readings, with the
//-- phase sum as fallback.
static void extractPowerSnapshot(const loggerTelegramRecord *record, bool testData, uint32_t sequence, loggerPowerSnapshot *snapshot)
{
  static const char *const deliveredObis[3] = {"1-0:21.7.0", "1-0:41.7.0", "1-0:61.7.0"};
  static const char *const returnedObis[3] = {"1-0:22.7.0", "1-0:42.7.0", "1-0:62.7.0"};
  static const char *const ampObis[3] = {"1-0:31.7.0", "1-0:51.7.0", "1-0:71.7.0"};
  static const char *const voltObis[3] = {"1-0:32.7.0", "1-0:52.7.0", "1-0:72.7.0"};

  memset(snapshot, 0, sizeof *snapshot);
  snapshot->testData = testData;
  snapshot->sequence = sequence;

  float deliveredTotalKw = 0.0f;
  float returnedTotalKw = 0.0f;
  bool hasDeliveredTotal = findNumericField(record, "1-0:1.7.0", &deliveredTotalKw);
  bool hasReturnedTotal = findNumericField(record, "1-0:2.7.0", &returnedTotalKw);

  float wattSum = 0.0f;
  float deliveredSum = 0.0f;
  float returnedSum = 0.0f;
  float voltSum = 0.0f;
  uint8_t voltCount = 0u;

  for (uint8_t phase = 0u; phase < 3u; phase++)
  {
    float deliveredKw = 0.0f;
    float returnedKw = 0.0f;
    float publishedAmps = 0.0f;
    float volts = 0.0f;

    findNumericField(record, deliveredObis[phase], &deliveredKw);
    findNumericField(record, returnedObis[phase], &returnedKw);
    findNumericField(record, ampObis[phase], &publishedAmps);
    findNumericField(record, voltObis[phase], &volts);

    float watts = (returnedKw - deliveredKw) * 1000.0f;
    snapshot->phaseWatts[phase] = watts;
    snapshot->phaseDeliveredWatts[phase] = deliveredKw * 1000.0f;
    snapshot->phaseReturnedWatts[phase] = returnedKw * 1000.0f;
    snapshot->phaseVolts[phase] = volts;

    if (volts > 0.0f)
    {
      snapshot->phaseAmps[phase] = watts / volts;
      voltSum += volts;
      voltCount++;
    }
    else
    {
      snapshot->phaseAmps[phase] = (watts < 0.0f) ? -publishedAmps : publishedAmps;
    }

    wattSum += watts;
    deliveredSum += snapshot->phaseDeliveredWatts[phase];
    returnedSum += snapshot->phaseReturnedWatts[phase];
  }

  if (hasDeliveredTotal || hasReturnedTotal)
  {
    snapshot->totalWatts = (returnedTotalKw - deliveredTotalKw) * 1000.0f;
    snapshot->totalDeliveredWatts = deliveredTotalKw * 1000.0f;
    snapshot->totalReturnedWatts = returnedTotalKw * 1000.0f;
  }
  else
  {
    snapshot->totalWatts = wattSum;
    snapshot->totalDeliveredWatts = deliveredSum;
    snapshot->totalReturnedWatts = returnedSum;
  }

  snapshot->totalVolts = (voltCount > 0u) ? (voltSum / (float)voltCount) : 0.0f;
  snapshot->totalAmps = (snapshot->totalVolts > 0.0f) ? (snapshot->totalWatts / snapshot->totalVolts) : 0.0f;

  extractPanelMeterTime(record, snapshot->meterTime, sizeof snapshot->meterTime);
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

    //-- Only one telegram is read per configured interval: a complete
    //-- candidate arriving sooner than the interval after the last accepted
    //-- record is ignored without touching the history or the counters.
    xSemaphoreTake(self->mutex, portMAX_DELAY);
    bool tooEarly = self->hasLastAccepted &&
                    ((uint32_t)(result->receiveTimeMs - self->lastAcceptedUptimeMs) <
                     (self->activeConfig.intervalSeconds * 1000u));
    xSemaphoreGive(self->mutex);

    if (tooEarly)
    {
      return;
    }

    memset(stagingRecord, 0, sizeof *stagingRecord);
    stagingRecord->meta.receivedUptimeMs = result->receiveTimeMs;
    stagingRecord->meta.crcVerified = (result->crcStatus == DSMR_CRC_STATUS_VERIFIED);
    copyBounded(stagingRecord->meta.profileId, sizeof *stagingRecord->meta.profileId, (result->profileId != NULL) ? result->profileId : "");
    copyBounded(stagingRecord->meta.reportedVersion, sizeof *stagingRecord->meta.reportedVersion, (result->reportedVersion != NULL) ? result->reportedVersion : "");

    //-- The exact accepted frame: header, body, trailer bytes and every
    //-- original line ending, preserved by the parser before decoding.
    if ((result->rawBytes != NULL) && (result->rawLength > 0u))
    {
      size_t rawLength = (result->rawLength > DSMR_MAX_TELEGRAM_BYTES) ? DSMR_MAX_TELEGRAM_BYTES : result->rawLength;
      memcpy(stagingRecord->rawBytes, result->rawBytes, rawLength);
      stagingRecord->meta.rawLength = (uint16_t)rawLength;
    }

    uint16_t snapshotCount = result->fieldCount;

    if (snapshotCount > LOGGER_SNAPSHOT_FIELDS_MAX)
    {
      snapshotCount = LOGGER_SNAPSHOT_FIELDS_MAX;
    }

    for (uint16_t index = 0; index < snapshotCount; index++)
    {
      copyFieldSnapshot(&result->fields[index], &stagingRecord->fields[index]);
    }

    stagingRecord->meta.fieldCount = snapshotCount;
    extractMeterTimeText(stagingRecord);

    bool doForward = false;
    bool testDataMode = false;
    bool haveShownConfig = false;
    loggerSettingsConfig shownConfig;
    uint32_t acceptedShown = 0u;
    uint32_t rejectedShown = 0u;
    memset(&shownConfig, 0, sizeof shownConfig);

    xSemaphoreTake(self->mutex, portMAX_DELAY);

    testDataMode = self->hasActiveConfig && loggerSettingsIsTestGpio(self->activeConfig.rxGpio);

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

    stagingRecord->meta.disabledChannelsMask = disabledMask;

    //-- The parser reports a contradiction with a configured device type as a
    //-- diagnostic after the accepted telegram; the logger detects a missing
    //-- published type itself from the decoded fields.
    for (uint8_t channel = DSMR_MBUS_CHANNEL_MIN; channel <= DSMR_MBUS_CHANNEL_MAX; channel++)
    {
      if (channelPresent(stagingRecord, channel) && !channelHasPublishedType(stagingRecord, channel))
      {
        char note[48];
        snprintf(note, sizeof note, "missing published device type on channel %u", (unsigned)channel);
        appendNote(stagingRecord->meta.notes, sizeof *stagingRecord->meta.notes, note);
      }
    }

    self->writeIndex = (uint8_t)((self->writeIndex + 1u) % LOGGER_HISTORY_CAPACITY);
    self->nextSequence++;
    stagingRecord->meta.sequence = self->nextSequence;
    self->slotUsed[self->writeIndex] = true;
    self->records[self->writeIndex] = *stagingRecord;
    self->acceptedCount++;
    self->hasLastAccepted = true;
    self->lastAcceptedUptimeMs = result->receiveTimeMs;
    self->lastError[0] = '\0';

    //-- Snapshot of the counters and the active configuration for the TFT
    //-- system information view, taken under the same lock.
    haveShownConfig = self->hasActiveConfig;
    shownConfig = self->activeConfig;
    acceptedShown = self->acceptedCount;
    rejectedShown = self->rejectedCount;

    //-- The accepted frame is forwarded on the UART0 TX pin; every burst
    //-- repeats the exact frame twice with reception suspended for its
    //-- duration. A burst takes longer than the shortest intervals, so the
    //-- pacing floor is five seconds: at intervals up to four seconds (the
    //-- two-second cadence would collide with the burst itself) at most one
    //-- burst per five seconds; above four seconds every accepted telegram
    //-- is forwarded. The burst itself runs in the forwarding task.
    uint32_t forwardWindowMs = self->activeConfig.intervalSeconds * 1000u;

    if (forwardWindowMs < LOGGER_FORWARD_MIN_GAP_MS)
    {
      forwardWindowMs = LOGGER_FORWARD_MIN_GAP_MS;
    }

    doForward = (stagingRecord->meta.rawLength > 0u) &&
                ((uint32_t)(result->receiveTimeMs - self->lastForwardUptimeMs) >= forwardWindowMs);

    if (doForward)
    {
      //-- The burst reads the exact bytes and length from the history slot
      //-- this commit just filled, addressed by sequence; the slot stays
      //-- intact until the burst ends because reception is suspended.
      self->forwardPending = true;
      self->forwardSequence = stagingRecord->meta.sequence;
      self->lastForwardUptimeMs = result->receiveTimeMs;
    }

    xSemaphoreGive(self->mutex);

    if (doForward && (self->forwardWake != NULL))
    {
      xSemaphoreGive(self->forwardWake);
    }

    loggerBoardNoteTelegram(stagingRecord->meta.sequence);

    //-- The TFT power panel becomes the primary screen with the first
    //-- accepted telegram: signed per-phase and total watts, phase
    //-- currents and voltages and the meter date/time.
    loggerPowerSnapshot powerSnapshot;
    extractPowerSnapshot(stagingRecord, testDataMode, stagingRecord->meta.sequence, &powerSnapshot);
    powerSnapshot.acceptedCount = acceptedShown;
    powerSnapshot.rejectedCount = rejectedShown;
    powerSnapshot.uptimeSeconds = uptimeMs() / 1000u;

    if (haveShownConfig)
    {
      snprintf(powerSnapshot.countryCode, sizeof powerSnapshot.countryCode, "%s", shownConfig.countryCode);
      snprintf(powerSnapshot.profileId, sizeof powerSnapshot.profileId, "%s", shownConfig.profileId);
      powerSnapshot.rxInverted = shownConfig.rxInverted;
    }

    loggerBoardShowPower(&powerSnapshot);

    //-- One console line per accepted telegram: sequence, meter time from
    //-- OBIS 0-0:1.0.0, size, CRC verdict and active profile.
    ESP_LOGI(tag, "accepted telegram #%" PRIu32 ": meter %s, %u bytes, %s, profile %s",
             stagingRecord->meta.sequence,
             (stagingRecord->meta.meterTimeText[0] != '\0') ? stagingRecord->meta.meterTimeText : "--:--:--",
             (unsigned)stagingRecord->meta.rawLength,
             stagingRecord->meta.crcVerified ? "crc ok" : "no crc",
             stagingRecord->meta.profileId);
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

    //-- Diagnostics reach the console with their reason (and OBIS when the
    //-- parser names the offending line); totals stay visible in the web
    //-- status strip. Not every diagnostic is a rejection: an accepted
    //-- telegram can carry an M-Bus type mismatch. An identical diagnostic
    //-- text is logged once per reception session only, so a repeating
    //-- M-Bus mismatch stays quiet until the next start or settings
    //-- change, which resets the deduplication.
    char diagText[70];

    if (self->lastDiagObis[0] != '\0')
    {
      snprintf(diagText, sizeof diagText, "%s @ %s", self->lastDiagReason, self->lastDiagObis);
    }
    else
    {
      snprintf(diagText, sizeof diagText, "%s", self->lastDiagReason);
    }

    if (!self->loggedDiagValid || (strcmp(self->loggedDiagText, diagText) != 0))
    {
      copyBounded(self->loggedDiagText, sizeof self->loggedDiagText, diagText);
      self->loggedDiagValid = true;
      ESP_LOGW(tag, "diagnostic: %s", diagText);
    }

    xSemaphoreGive(self->mutex);
  }
}

//-- --------------------------------------------------------------------------
//-- Test data generator (RX selection LOGGER_RX_GPIO_TEST)
//-- --------------------------------------------------------------------------

//-- When the RX selection is -1 no UART is opened: this task becomes the
//-- single feed task for the parser (the ownership rule of the receive task
//-- still holds — exactly one task feeds the parser at any time). It builds
//-- a complete telegram per period with the shape of the verified parser
//-- fixtures, randomising at least the timestamps and the measurement
//-- values, and passes it through dsmrParserFeed into the same event
//-- callback the UART adapter uses. The line shapes are taken from the
//-- accepted fixtures of the parser repository so the frames verify their
//-- CRC and decode under the selected profile; lines a profile does not
//-- define are skipped by the parser's unknown-field policy.

#define TEST_TELEGRAM_BUFFER_BYTES 2048
#define TEST_TASK_JOIN_TIMEOUT_MS 15000u

static TaskHandle_t testTaskHandle;

typedef struct
{
  LoggerReceiver *self;
  const dsmrProfile *profile;
  uint32_t periodMs;

  //-- Per-run stop flag: the stopper clears it and the task observes it
  //-- within one delay slice, so a reconfigure never leaves a zombie
  //-- generator behind that could feed the parser alongside the next task.
  volatile bool running;

  //-- Every run owns its frame buffer: a zombie that has not observed its
  //-- stop flag yet can never interleave bytes with the next run's builder.
  char telegram[TEST_TELEGRAM_BUFFER_BYTES];
} TestTaskArguments;

//-- Two alternating argument blocks: a run never reuses the block of a run
//-- that might still be finishing, so a fast double reconfigure (save twice
//-- within one join window) cannot flip a zombie's stop flag back to true
//-- and leave two tasks feeding the same parser concurrently.
static TestTaskArguments testArgumentsPool[2];
static uint8_t testArgumentsNext;
static TestTaskArguments *testArgumentsActive;

//-- The frame builder lives in logger_testdata.c so it can also be checked
//-- on the host against the real parser; randomness comes from the ESP
//-- random source.
static uint32_t testRandomLimit(uint32_t limit)
{
  return esp_random() % limit;
}

static void testGeneratorTask(void *argument)
{
  TestTaskArguments *arguments = (TestTaskArguments *)argument;
  LoggerReceiver *self = arguments->self;
  const dsmrProfile *profile = arguments->profile;

  //-- Test data is produced at the configured read interval so the test
  //-- behaves exactly like a meter answering once per interval. Clamped
  //-- defensively to the validated interval bounds.
  uint32_t periodMs = arguments->periodMs;

  if (periodMs < (LOGGER_SETTINGS_INTERVAL_MIN_S * 1000u))
  {
    periodMs = LOGGER_SETTINGS_INTERVAL_MIN_S * 1000u;
  }

  if (periodMs > (LOGGER_SETTINGS_INTERVAL_MAX_S * 1000u))
  {
    periodMs = LOGGER_SETTINGS_INTERVAL_MAX_S * 1000u;
  }

  bool firstFrame = true;

  while (arguments->running)
  {
    //-- A (re)started generator reads at least one complete telegram right
    //-- away: after a configuration change the view never waits a whole
    //-- interval for the first reading. Subsequent frames follow the meter
    //-- cadence, one per interval, with the delay sliced so a stop joins
    //-- this task quickly even at 600 s.
    if (!firstFrame)
    {
      uint32_t sleptMs = 0u;

      while (arguments->running && (sleptMs < periodMs))
      {
        vTaskDelay(pdMS_TO_TICKS(LOGGER_TEST_TASK_SLICE_MS));
        sleptMs += LOGGER_TEST_TASK_SLICE_MS;
      }
    }

    firstFrame = false;

    if (!arguments->running)
    {
      break;
    }

    size_t length = loggerTestDataBuild(profile, arguments->telegram, sizeof arguments->telegram);

    if (length > 0u)
    {
      //-- While a burst owns UART0 the generated frames are discarded: they
      //-- must never reach the parser, update logger data or trigger another
      //-- burst. The flag and the feed marker change under one lock so a
      //-- burst starting right now either sees the discarded frame or the
      //-- running feed and waits for it to leave the parser.
      bool discardForBurst;

      xSemaphoreTake(self->mutex, portMAX_DELAY);
      discardForBurst = self->forwarding;

      if (!discardForBurst)
      {
        self->generatorFeeding = true;
      }

      xSemaphoreGive(self->mutex);

      if (!discardForBurst)
      {
        dsmrParserFeed(&self->parser, (const uint8_t *)arguments->telegram, length, uptimeMs(), onParserEvent, self);
        self->generatorFeeding = false;
      }
    }
  }

  testTaskHandle = NULL;
  vTaskDelete(NULL);
}

static void stopTestGenerator(void);

static esp_err_t startTestGenerator(LoggerReceiver *self, const loggerSettingsConfig *config)
{
  //-- Defensive: a generator must never be started while one still runs.
  if (testTaskHandle != NULL)
  {
    stopTestGenerator();
  }

  TestTaskArguments *arguments = &testArgumentsPool[testArgumentsNext];

  //-- Consume this block for the coming run; the next run takes the other,
  //-- so the block of a still-finishing zombie is never handed out again.
  testArgumentsNext = (uint8_t)((testArgumentsNext + 1u) % 2u);

  arguments->self = self;
  arguments->profile = dsmrProfileLookup(config->profileId);
  arguments->periodMs = config->intervalSeconds * 1000u;

  //-- Random seeds so every run produces different measurement progressions.
  loggerTestDataSeed(testRandomLimit);

  arguments->running = true;

  BaseType_t created = xTaskCreate(testGeneratorTask, "p1TestData", LOGGER_P1_TASK_STACK_BYTES,
                                   arguments, LOGGER_P1_TASK_PRIORITY, &testTaskHandle);

  if (created != pdPASS)
  {
    arguments->running = false;
    testTaskHandle = NULL;
    return ESP_ERR_NO_MEM;
  }

  testArgumentsActive = arguments;
  ESP_LOGI(tag, "test data generator started: profile %s, one telegram per %" PRIu32 " s",
           config->profileId, config->intervalSeconds);
  return ESP_OK;
}

static void stopTestGenerator(void)
{
  if (testArgumentsActive != NULL)
  {
    testArgumentsActive->running = false;
    ESP_LOGI(tag, "test data generator stopping");
  }

  uint32_t waitedMs = 0u;

  //-- Join the generator task: it may be inside dsmrParserFeed right now and
  //-- the parser must not be reinitialised under a running feed.
  while ((testTaskHandle != NULL) && (waitedMs < TEST_TASK_JOIN_TIMEOUT_MS))
  {
    vTaskDelay(pdMS_TO_TICKS(20));
    waitedMs += 20u;
  }

  testArgumentsActive = NULL;
}

//-- --------------------------------------------------------------------------
//-- UART0 TX forwarding (dedicated task)
//-- --------------------------------------------------------------------------

//-- Line-inverse mask while a burst transmits: the RX bit the P1 Rx polarity
//-- setting requires plus the TX bit the P1 Tx polarity (Forwarding Data)
//-- setting selects. uart_set_line_inverse() replaces the whole mask, so the
//-- RX bit must be carried along on every call or reception would flip.
static uint32_t forwardBurstMask(bool rxInverted, bool txInverted)
{
  uint32_t mask = rxInverted ? (uint32_t)UART_SIGNAL_RXD_INV : 0u;

  if (txInverted)
  {
    mask |= (uint32_t)UART_SIGNAL_TXD_INV;
  }

  return mask;
}

//-- Line-inverse mask outside a burst: TX idles at the polarity the P1 Rx
//-- polarity setting selects so both sides of the interface agree.
static uint32_t forwardIdleMask(bool rxInverted)
{
  return rxInverted ? (uint32_t)(UART_SIGNAL_RXD_INV | UART_SIGNAL_TXD_INV) : 0u;
}

//-- Applies a full line-inverse mask to the receiver-owned UART0 driver.
//-- Identical consecutive masks skip the hardware update; the tracked mask
//-- becomes unknown whenever a driver is installed or deleted, because a
//-- fresh driver starts with the inversion cleared.
static void applyForwardMask(LoggerReceiver *self, uint32_t mask)
{
  if (!uart_is_driver_installed(LOGGER_P1_UART_PORT))
  {
    return;
  }

  if (self->appliedInverseMask == (int)mask)
  {
    return;
  }

  esp_err_t result = uart_set_line_inverse(LOGGER_P1_UART_PORT, mask);

  if (result != ESP_OK)
  {
    ESP_LOGW(tag, "UART0 line inversion 0x%02" PRIX32 " failed: %s", mask, esp_err_to_name(result));
    return;
  }

  self->appliedInverseMask = (int)mask;
}

//-- Installs the receiver's own driver on UART0 when none is installed. The
//-- line settings follow the resolved profile, mirroring the adapter
//-- configuration. Test mode keeps this driver installed across bursts (it
//-- opens no receive UART); the live path installs it after the adapter
//-- stopped and deletes it again when the burst has been written.
static void ensureForwardTxDriver(LoggerReceiver *self, bool rxInverted, uint16_t frameLength)
{
  if (uart_is_driver_installed(LOGGER_P1_UART_PORT))
  {
    return;
  }

  const dsmrProfile *profile = self->parser.profile;

  uart_config_t uartConfig;
  memset(&uartConfig, 0, sizeof uartConfig);
  uartConfig.baud_rate = (profile != NULL) ? (int)profile->uartBaudRate : 115200;
  uartConfig.data_bits = ((profile != NULL) && (profile->uartDataBits == 7)) ? UART_DATA_7_BITS : UART_DATA_8_BITS;
  uartConfig.parity = ((profile == NULL) || (profile->uartParity == 0)) ? UART_PARITY_DISABLE
                    : ((profile->uartParity == 1) ? UART_PARITY_ODD : UART_PARITY_EVEN);
  uartConfig.stop_bits = ((profile != NULL) && (profile->uartStopBits == 2)) ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
  uartConfig.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uartConfig.source_clk = UART_SCLK_DEFAULT;

  esp_err_t result = uart_driver_install(LOGGER_P1_UART_PORT, LOGGER_FORWARD_RX_BUFFER_BYTES,
                                         (int)frameLength + LOGGER_FORWARD_TX_SLACK_BYTES,
                                         0, NULL, 0);

  if (result == ESP_OK)
  {
    result = uart_param_config(LOGGER_P1_UART_PORT, &uartConfig);
  }

  if (result == ESP_OK)
  {
    result = uart_set_pin(LOGGER_P1_UART_PORT, LOGGER_P1_UART0_TX_GPIO, LOGGER_P1_UART0_RX_GPIO,
                          UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  }

  xSemaphoreTake(self->mutex, portMAX_DELAY);

  if (result != ESP_OK)
  {
    self->ownsTxDriver = false;
    self->appliedInverseMask = -1;
    xSemaphoreGive(self->mutex);

    ESP_LOGW(tag, "UART0 TX driver could not be installed: %s (burst skipped)", esp_err_to_name(result));

    if (uart_is_driver_installed(LOGGER_P1_UART_PORT))
    {
      uart_driver_delete(LOGGER_P1_UART_PORT);
    }

    return;
  }

  self->ownsTxDriver = true;
  self->appliedInverseMask = 0;
  xSemaphoreGive(self->mutex);

  applyForwardMask(self, forwardIdleMask(rxInverted));
}

static void releaseForwardTxDriver(LoggerReceiver *self)
{
  xSemaphoreTake(self->mutex, portMAX_DELAY);
  bool owned = self->ownsTxDriver;
  self->ownsTxDriver = false;
  self->appliedInverseMask = -1;
  xSemaphoreGive(self->mutex);

  if (owned && uart_is_driver_installed(LOGGER_P1_UART_PORT))
  {
    uart_driver_delete(LOGGER_P1_UART_PORT);
  }
}

//-- Marks the UART0 line-inverse mask as unknown after the driver was
//-- deleted from under the burst task (the adapter deletes it on stop).
static void forgetForwardMask(LoggerReceiver *self)
{
  xSemaphoreTake(self->mutex, portMAX_DELAY);
  self->appliedInverseMask = -1;
  xSemaphoreGive(self->mutex);
}

//-- Starts the live adapter task on UART0 with the given configuration and
//-- brings TX to its idle polarity. The adapter itself only applies the RX
//-- inverse, so the tracked mask starts from what it sets and TX is aligned
//-- here: TX idles at the polarity selected by the P1 Rx polarity setting.
static esp_err_t startLiveAdapter(LoggerReceiver *self, const loggerSettingsConfig *config)
{
  dsmrUartConfig uartConfig;
  memset(&uartConfig, 0, sizeof uartConfig);
  uartConfig.uartPort = LOGGER_P1_UART_PORT;
  uartConfig.rxPin = config->rxGpio;
  uartConfig.txPin = LOGGER_P1_UART0_TX_GPIO;

  //-- The first release uses an externally enabled request signal.
  uartConfig.requestGpio = -1;
  uartConfig.requestActiveHigh = true;
  uartConfig.taskStackSize = LOGGER_P1_TASK_STACK_BYTES;
  uartConfig.taskPriority = LOGGER_P1_TASK_PRIORITY;

  //-- RX polarity is a runtime setting chosen in the SPA: normal reads the
  //-- idle-high signal directly, inverted flips the receive line for
  //-- interface boards that invert before the ESP32-S3.
  uartConfig.invertRx = config->rxInverted;

  ESP_LOGI(tag, "P1 RX on UART0 gpio %d, polarity %s, tx idles at %s", config->rxGpio,
           config->rxInverted ? "inverted" : "normal",
           config->rxInverted ? "inverted" : "normal");

  esp_err_t result = dsmrUartStart(&self->uart, &self->parser, &uartConfig, onParserEvent, self);

  xSemaphoreTake(self->mutex, portMAX_DELAY);
  self->appliedInverseMask = (result != ESP_OK) ? -1 : (config->rxInverted ? (int)UART_SIGNAL_RXD_INV : 0);
  xSemaphoreGive(self->mutex);

  if (result == ESP_OK)
  {
    applyForwardMask(self, forwardIdleMask(config->rxInverted));

    //-- The driver is fresh; drop anything left in the hardware FIFO from
    //-- before the (re)start so the parser sees only new bytes.
    uart_flush_input(LOGGER_P1_UART_PORT);
  }

  return result;
}

//-- Waits until the test generator is not inside dsmrParserFeed(): the
//-- burst must not touch the parser state from two tasks at once.
static void forwardJoinTestFeed(LoggerReceiver *self)
{
  uint32_t waitedMs = 0u;

  while (self->generatorFeeding && (waitedMs < LOGGER_FORWARD_FEED_JOIN_MS))
  {
    vTaskDelay(pdMS_TO_TICKS(5));
    waitedMs += 5u;
  }
}

//-- The parser discards its partial frame on purpose when a burst ends:
//-- that intentional drop must not update the receive counters or the
//-- console, so the notification goes to this sink instead of the SPA
//-- callback. The parser's own start-of-telegram detection then re-arms, so
//-- reception resumes cleanly even when the meter is already partway
//-- through a telegram: the overlapping frame is skipped whole.
static void forwardDiscardCallback(const dsmrParser *parser, const dsmrParserEvent *event, void *userData)
{
  (void)parser;
  (void)event;
  (void)userData;
}

//-- Queues one copy of the retained frame and waits until it has finished
//-- transmitting on the wire. The payload points into the history slot of
//-- the forwarded sequence, which stays untouched until the burst ends.
//-- Returns false on a partial write or timeout.
static bool forwardCopy(LoggerReceiver *self, const uint8_t *payload, uint16_t length,
                        bool rxInverted, bool txInverted)
{
  applyForwardMask(self, forwardBurstMask(rxInverted, txInverted));

  int written = uart_write_bytes(LOGGER_P1_UART_PORT, (const char *)payload, length);

  if (written != (int)length)
  {
    ESP_LOGW(tag, "UART0 TX copy incomplete: %d of %u bytes", written, (unsigned)length);
    return false;
  }

  esp_err_t result = uart_wait_tx_done(LOGGER_P1_UART_PORT, pdMS_TO_TICKS(LOGGER_FORWARD_TX_WAIT_MS));

  if (result != ESP_OK)
  {
    ESP_LOGW(tag, "UART0 TX copy did not finish on the wire within %d ms: %s",
             LOGGER_FORWARD_TX_WAIT_MS, esp_err_to_name(result));
    return false;
  }

  return true;
}

//-- Sends every selected burst twice on the UART0 TX pin. The whole sequence
//-- runs under the control mutex, so a settings update or another task
//-- cannot interrupt the polarity and parser sequence: a save arriving
//-- during a burst waits and applies to the next operation, while both
//-- copies of the current telegram keep one polarity. The console never
//-- touches UART0 (it runs on USB-Serial-JTAG), so logging during a burst
//-- is safe. Blocking writes run in this task only, never in reception.
static void forwardTaskLoop(void *argument)
{
  LoggerReceiver *self = (LoggerReceiver *)argument;

  for (;;)
  {
    xSemaphoreTake(self->forwardWake, portMAX_DELAY);

    //-- Serialize the burst against start/stop/reconfigure.
    xSemaphoreTake(self->controlMutex, portMAX_DELAY);

    xSemaphoreTake(self->mutex, portMAX_DELAY);
    uint16_t length = 0u;
    const uint8_t *payload = NULL;

    if (self->forwardPending)
    {
      self->forwardPending = false;

      //-- Address the retained frame by sequence rather than copying it: the
      //-- slot of the selected telegram cannot be recycled before the burst
      //-- ends (reception is suspended and the history would need four new
      //-- accepts to wrap around to it), so the exact accepted bytes and
      //-- their length stay valid on the wire without a second buffer.
      for (uint8_t slot = 0u; slot < LOGGER_HISTORY_CAPACITY; slot++)
      {
        if (self->slotUsed[slot] && (self->records[slot].meta.sequence == self->forwardSequence))
        {
          payload = self->records[slot].rawBytes;
          length = self->records[slot].meta.rawLength;
          break;
        }
      }
    }

    bool rxInverted = self->hasActiveConfig && self->activeConfig.rxInverted;
    bool txInverted = self->hasActiveConfig && self->activeConfig.txInverted;
    bool testMode = self->hasActiveConfig && loggerSettingsIsTestGpio(self->activeConfig.rxGpio);
    loggerSettingsConfig resume = self->activeConfig;
    bool configured = self->hasActiveConfig;

    if (length > 0u)
    {
      self->forwarding = true;
    }

    xSemaphoreGive(self->mutex);

    if ((length == 0u) || (!configured))
    {
      xSemaphoreGive(self->controlMutex);
      continue;
    }

    uint32_t startMs = uptimeMs();

    //-- Suspend normal reception before the first byte leaves the pin. The
    //-- live path joins the adapter receive task, which is the only task
    //-- allowed to feed the parser: no byte can reach the parser while the
    //-- burst runs. The test generator observes the forwarding flag and the
    //-- burst waits for a frame already inside the parser.
    if (testMode)
    {
      forwardJoinTestFeed(self);
    }
    else
    {
      dsmrUartStop(&self->uart);
      forgetForwardMask(self);
    }

    ensureForwardTxDriver(self, rxInverted, length);

    bool ok = uart_is_driver_installed(LOGGER_P1_UART_PORT);

    if (ok)
    {
      ok = forwardCopy(self, payload, length, rxInverted, txInverted) &&
           forwardCopy(self, payload, length, rxInverted, txInverted);
    }

    //-- TX idle again (or the burst was safely stopped): TX returns to the
    //-- polarity selected by the current P1 Rx polarity setting; the full
    //-- replacement mask keeps the RX bit of that same setting.
    applyForwardMask(self, forwardIdleMask(rxInverted));

    //-- Drop every byte the RX pin captured while reception was suspended so
    //-- stale data can never reach the parser after the resume, then discard
    //-- the parser's partial frame; its start-of-telegram detection re-arms
    //-- reception on the next complete frame.
    if (uart_is_driver_installed(LOGGER_P1_UART_PORT))
    {
      uart_flush_input(LOGGER_P1_UART_PORT);
    }

    dsmrParserNotifyDataLoss(&self->parser, forwardDiscardCallback, NULL);

    if (!testMode)
    {
      releaseForwardTxDriver(self);

      esp_err_t resumed = startLiveAdapter(self, &resume);

      if (resumed != ESP_OK)
      {
        xSemaphoreTake(self->mutex, portMAX_DELAY);
        snprintf(self->lastError, sizeof self->lastError, "receiver could not resume after forwarding: %s",
                 esp_err_to_name(resumed));
        self->receiverRunning = false;
        xSemaphoreGive(self->mutex);
        ESP_LOGE(tag, "%s", self->lastError);
      }
    }

    //-- The free heap after the burst lets memory pressure be read from the
    //-- console: web detail and raw requests malloc alongside the burst.
    ESP_LOGI(tag, "forwarded %u bytes x 2 on UART0 TX, tx polarity %s, %s in %u ms, free heap %u",
             (unsigned)length, txInverted ? "inverted" : "normal", ok ? "sent" : "aborted",
             (unsigned)(uptimeMs() - startMs), (unsigned)esp_get_free_heap_size());

    xSemaphoreTake(self->mutex, portMAX_DELAY);
    self->forwarding = false;
    xSemaphoreGive(self->mutex);

    xSemaphoreGive(self->controlMutex);
  }
}

//-- --------------------------------------------------------------------------
//-- Accepted-telegram counter persistence (dedicated task)
//-- --------------------------------------------------------------------------

//-- Writes the accepted-telegram total to NVS. The write is skipped when the
//-- value has not changed since the last persist so an idle or unconfigured
//-- meter does not wear the flash for nothing.
static void persistTelegramCount(LoggerReceiver *self, uint32_t *lastPersistedCount)
{
  xSemaphoreTake(self->mutex, portMAX_DELAY);
  uint32_t count = self->acceptedCount;
  xSemaphoreGive(self->mutex);

  if (count == *lastPersistedCount)
  {
    return;
  }

  nvs_handle_t handle;
  esp_err_t result = nvs_open(LOGGER_NVS_NAMESPACE, NVS_READWRITE, &handle);

  if (result != ESP_OK)
  {
    ESP_LOGE(tag, "opening the counter namespace failed: %s", esp_err_to_name(result));
    return;
  }

  result = nvs_set_u32(handle, LOGGER_NVS_KEY_TELEGRAM_COUNT, count);

  if (result == ESP_OK)
  {
    result = nvs_commit(handle);
  }

  nvs_close(handle);

  if (result != ESP_OK)
  {
    ESP_LOGE(tag, "persisting the telegram counter failed: %s", esp_err_to_name(result));
    return;
  }

  *lastPersistedCount = count;
  ESP_LOGI(tag, "telegram counter persisted: %" PRIu32, count);
}

//-- Blocks the whole ten-minute interval and then persists the counter. The
//-- task never touches the parser or the receiver control state and only
//-- reads the counter under the data mutex, so it needs no join path.
static void telegramCounterTaskLoop(void *argument)
{
  LoggerReceiver *self = (LoggerReceiver *)argument;
  uint32_t lastPersistedCount = self->acceptedCount;

  for (;;)
  {
    vTaskDelay(pdMS_TO_TICKS(LOGGER_TELEGRAM_COUNTER_PERSIST_INTERVAL_MS));
    persistTelegramCount(self, &lastPersistedCount);
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

  esp_err_t result;

  if (loggerSettingsIsTestGpio(config->rxGpio))
  {
    //-- No UART is opened: the test data generator becomes the single feed
    //-- task and passes synthetic frames through the same parser and event
    //-- callback as the UART adapter. Accepted test frames are still
    //-- forwarded on the TX pin, so the receiver installs its own driver.
    result = startTestGenerator(self, config);

    if (result == ESP_OK)
    {
      //-- The test driver stays installed across bursts, so its TX ring is
      //-- sized for the largest frame the generator can build; every copy
      //-- then queues in one non-blocking uart_write_bytes() call.
      ensureForwardTxDriver(self, config->rxInverted, TEST_TELEGRAM_BUFFER_BYTES);
    }
  }
  else
  {
    result = startLiveAdapter(self, config);
  }

  xSemaphoreTake(self->mutex, portMAX_DELAY);

  if (result != ESP_OK)
  {
    snprintf(self->lastError, sizeof self->lastError, "receiver start failed on GPIO %d: %s", config->rxGpio, esp_err_to_name(result));
  }
  else
  {
    self->receiverRunning = true;
    self->lastError[0] = '\0';

    //-- A boot or a settings change starts a new reception session: the
    //-- console deduplication is reset so the diagnostics of the new
    //-- configuration each show once, and stay quiet after that.
    self->loggedDiagValid = false;
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

  //-- The running feed task is either the UART adapter task or, with the
  //-- test selection, the test data generator task. Both are joined before
  //-- the parser is reinitialised.
  if (loggerSettingsIsTestGpio(self->activeConfig.rxGpio))
  {
    stopTestGenerator();
    releaseForwardTxDriver(self);
  }
  else
  {
    dsmrUartStop(&self->uart);
    forgetForwardMask(self);
  }

  xSemaphoreTake(self->mutex, portMAX_DELAY);
  self->receiverRunning = false;
  xSemaphoreGive(self->mutex);
}

//-- --------------------------------------------------------------------------
//-- Public operations
//-- --------------------------------------------------------------------------

esp_err_t loggerReceiverInit(void)
{
  //-- The UART0 driver re-attaches GPIO 43/44 each time the driver around a
  //-- forwarding burst is (re)installed; its pin reservation bookkeeping then
  //-- reports "GPIO 43/44 is not usable, maybe used by others" although both
  //-- pins serve UART0 on this board by design. Keep only the real errors of
  //-- the driver tag and drop that warning and info noise from the console.
  esp_log_level_set("uart", ESP_LOG_ERROR);

  LoggerReceiver *self = &instance;
  memset(self, 0, sizeof *self);

  self->mutex = xSemaphoreCreateMutex();
  self->controlMutex = xSemaphoreCreateMutex();
  self->forwardWake = xSemaphoreCreateBinary();

  if ((self->mutex == NULL) || (self->controlMutex == NULL) || (self->forwardWake == NULL))
  {
    return ESP_ERR_NO_MEM;
  }

  //-- The big telegram buffers (staging record, retained history, parser
  //-- workspace further down) are carved here, preferring PSRAM: they keep
  //-- more than a hundred kilobytes of frame storage off the small internal
  //-- heap that the radios, task stacks and drivers need.
  stagingRecord = (loggerTelegramRecord *)allocTelegramBuffer(sizeof *stagingRecord);
  self->records = (loggerTelegramRecord *)allocTelegramBuffer(sizeof(*self->records) * LOGGER_HISTORY_CAPACITY);

  if ((stagingRecord == NULL) || (self->records == NULL))
  {
    ESP_LOGE(tag, "telegram buffers could not be allocated");
    return ESP_ERR_NO_MEM;
  }

  //-- The accepted-telegram total continues across a reset or power cycle:
  //-- the value the counter task last persisted is the starting point of the
  //-- new reception session. A missing key (first boot) simply keeps zero.
  nvs_handle_t handle;

  if (nvs_open(LOGGER_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK)
  {
    uint32_t storedCount = 0u;

    if (nvs_get_u32(handle, LOGGER_NVS_KEY_TELEGRAM_COUNT, &storedCount) == ESP_OK)
    {
      self->acceptedCount = storedCount;
      ESP_LOGI(tag, "telegram counter restored from NVS: %" PRIu32, storedCount);
    }

    nvs_close(handle);
  }

  //-- The forwarding task lives for the whole run and blocks until the
  //-- callback stages a burst; without a driver installed a burst is skipped.
  if (xTaskCreate(forwardTaskLoop, "p1Forward", LOGGER_FORWARD_TASK_STACK_BYTES, self,
                  LOGGER_FORWARD_TASK_PRIORITY, NULL) != pdPASS)
  {
    ESP_LOGW(tag, "forwarding task could not be created: TX forwarding disabled");
  }
  else
  {
    ESP_LOGI(tag, "UART0 TX forwarding ready: each burst repeats the frame twice at the configured TX polarity with reception suspended, at most one burst per 5 s (or per interval when larger)");
  }

  //-- The counter task persists the accepted-telegram total every ten
  //-- minutes so the running total survives a reset or power cycle.
  if (xTaskCreate(telegramCounterTaskLoop, "p1Counter", LOGGER_TELEGRAM_COUNTER_TASK_STACK_BYTES, self,
                  LOGGER_TELEGRAM_COUNTER_TASK_PRIORITY, NULL) != pdPASS)
  {
    ESP_LOGW(tag, "telegram counter task could not be created: counter persistence disabled");
  }

  dsmrParserConfig probe;
  memset(&probe, 0, sizeof probe);
  probe.countryCode = "NL";
  probe.profileId = "nl.dsmr.5.0";
  self->workspaceSize = dsmrParserWorkspaceSizeNeeded(&probe);

  if (self->workspaceSize > DSMR_DEFAULT_WORKSPACE_BYTES)
  {
    ESP_LOGE(tag, "workspace too small: need %u have %u", (unsigned)self->workspaceSize,
             (unsigned)DSMR_DEFAULT_WORKSPACE_BYTES);
    return ESP_ERR_INVALID_SIZE;
  }

  parserWorkspace = (uint8_t *)allocTelegramBuffer(DSMR_DEFAULT_WORKSPACE_BYTES);

  if (parserWorkspace == NULL)
  {
    ESP_LOGE(tag, "parser workspace could not be allocated");
    return ESP_ERR_NO_MEM;
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
    status->intervalSeconds = self->activeConfig.intervalSeconds;
    copyBounded(status->activeProfileId, sizeof status->activeProfileId, self->activeConfig.profileId);
  }

  //-- A telegram is accepted at most once per read interval, so staleness
  //-- follows three configured intervals with the floor as minimum.
  uint32_t staleAfterMs = LOGGER_STALE_FLOOR_MS;

  if (self->hasActiveConfig)
  {
    uint32_t intervalStaleMs = self->activeConfig.intervalSeconds * 3000u;

    if (intervalStaleMs > staleAfterMs)
    {
      staleAfterMs = intervalStaleMs;
    }
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

bool loggerReceiverInspectRecord(uint32_t sequence, bool (*inspect)(const loggerTelegramRecord *record, void *userData), void *userData)
{
  LoggerReceiver *self = &instance;

  if (inspect == NULL)
  {
    return false;
  }

  bool found = false;
  xSemaphoreTake(self->mutex, portMAX_DELAY);

  for (uint8_t slot = 0; slot < LOGGER_HISTORY_CAPACITY; slot++)
  {
    if (self->slotUsed[slot] && (self->records[slot].meta.sequence == sequence))
    {
      found = inspect(&self->records[slot], userData);
      break;
    }
  }

  xSemaphoreGive(self->mutex);
  return found;
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
