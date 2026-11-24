//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_tariffs.c - dynamic electricity tariff schedules
//-- Date: 2026-11-24 · Build: 6
//--
//-- One sorted interval array in PSRAM is the live schedule; the same
//-- content lands in a binary file on the meter store's LittleFS volume so
//-- accepted schedules survive a reboot. Every operation (ingest, query,
//-- persistence) runs under one mutex, so the MQTT task and the httpd task
//-- can both enter without ordering rules.
//--
//-- The JSON document arrives as bytes (MQTT fragments or an HTTP body) and
//-- is parsed with cJSON, whose allocator is re-pointed at PSRAM in
//-- loggerTariffsInit() (the tariff document is the only cJSON user in the
//-- project). Before the parse a cheap scan enforces printable ASCII and a
//-- small nesting bound: the cJSON recursion runs on the caller's stack
//-- (the 10 KB httpd task among them) and must never chase an adversarial
//-- 1000-deep document.
//--
//-- The file stores each interval as two varints (start delta over the
//-- previous start, duration in seconds) plus the two float prices and the
//-- export-presence byte: a quarter-hour schedule compresses to about 13
//-- bytes per interval, so even the stored bound stays a fraction of the
//-- LittleFS volume. The float round-trip is exact on the little-endian
//-- ESP32-S3 (the file is a device-local artifact, not an interchange
//-- format).
//--
//-- Calendar buckets come from the C library's local-time functions under
//-- the time zone the application sets after the join. Both DST directions
//-- are handled by asking mktime for each period with the DST hint set and
//-- cleared: a normal hour yields one occurrence, the repeated autumn hour
//-- yields two (reported as two buckets with different UTC boundaries) and
//-- the vanished spring hour yields none (its normalised instant still ends
//-- the bucket before it).
//-- ==========================================================================

#include "logger_tariffs.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "logger_control.h"
#include "logger_settings.h"
#include "logger_store.h"

static const char* tag = "loggerTariffs";

//-- Supported epoch window: the format's four-digit years and the local
//-- time functions stay well inside 2000 .. 2100.
#define TARIFF_EPOCH_MIN 946684800LL  //-- 2000-01-01T00:00:00Z
#define TARIFF_EPOCH_MAX 4102444800LL //-- 2100-01-01T00:00:00Z

//-- System-time floor for a usable wall clock; before the first SNTP sync
//-- the clock still sits at the 1970 epoch (same floor the meter store uses).
#define TARIFF_CLOCK_FLOOR 1700000000LL

//-- Binary file header and name on the meter store's volume. Version 1
//-- records carried import and export prices only; version 2 adds the gas
//-- price and a three-bit presence byte (import, export, gas). Loading
//-- accepts both versions: version 1 intervals are imported-price intervals.
#define TARIFF_FILE_MAGIC "LDTF"
#define TARIFF_FILE_VERSION 2u
#define TARIFF_FILE_NAME "tariffs.bin"
#define TARIFF_FILE_TMP_NAME "tariffs.tmp"

//-- Worst-case bytes of one serialised record: two 10-byte varints, three
//-- floats and the presence byte, rounded up.
#define TARIFF_RECORD_MAX_BYTES 40u

#define TARIFF_TOPIC_DEFAULT "lastdsmrlogger"
#define TARIFF_SUBTOPIC_DEFAULT "newTariffs"

//-- Presence byte bits of one version 2 record.
#define TARIFF_PRESENT_IMPORT 0x01u
#define TARIFF_PRESENT_EXPORT 0x02u
#define TARIFF_PRESENT_GAS 0x04u

//-- The nesting bound the pre-parse scan enforces (the shared format nests
//-- object > array > object: three levels; two levels of headroom).
#define TARIFF_JSON_DEPTH_MAX 6

//-- --------------------------------------------------------------------------
//-- State
//-- --------------------------------------------------------------------------

//-- All fields below are owned by this component and guarded by
//-- tariffsMutex. The arrays live in PSRAM: the stored bound is far beyond
//-- a comfortable internal-RAM footprint.
static loggerTariffInterval* store; //-- the live schedule, sorted by start
static loggerTariffInterval* spare; //-- candidate workspace for one ingest
static loggerTariffInterval* batch; //-- one parsed batch workspace
static size_t storeCount;
static char currency[LOGGER_TARIFF_CURRENCY_MAX] = "EUR";
static bool loaded;       //-- the file was read since the mount
static bool buffersReady; //-- the PSRAM workspaces were allocated
static bool hooksInstalled;
static SemaphoreHandle_t tariffsMutex;

//-- cJSON keeps a pointer to this struct (not a copy), so it must outlive
//-- loggerTariffsInit.
static cJSON_Hooks cJSONHooks;

static void freeBuffers(void)
{
  free(store);
  free(spare);
  free(batch);
  store = NULL;
  spare = NULL;
  batch = NULL;
  storeCount = 0u;
  buffersReady = false;
}

static void tariffFree(void* buffer)
{
  free(buffer);
}

static void* psramAllocate(size_t size)
{
  void* buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  if (buffer == NULL)
  {
    buffer = malloc(size);
  }

  return buffer;
}

//-- The caller holds tariffsMutex (the whole component runs on one mutex,
//-- so the workspace allocation follows the same rule as the schedule).
static bool ensureBuffers(void)
{
  if (buffersReady)
  {
    return true;
  }

  size_t spareCapacity = LOGGER_TARIFF_STORED_MAX + LOGGER_TARIFF_BATCH_MAX;
  store = psramAllocate(LOGGER_TARIFF_STORED_MAX * sizeof(loggerTariffInterval));
  spare = psramAllocate(spareCapacity * sizeof(loggerTariffInterval));
  batch = psramAllocate(LOGGER_TARIFF_BATCH_MAX * sizeof(loggerTariffInterval));

  if ((store == NULL) || (spare == NULL) || (batch == NULL))
  {
    freeBuffers();
    return false;
  }

  buffersReady = true;
  return true;
}

//-- --------------------------------------------------------------------------
//-- UTC timestamps
//-- --------------------------------------------------------------------------

//-- Days from the civil date (Howard Hinnant's algorithm): keeps the UTC
//-- conversions independent of the environment's local time functions, so
//-- the timestamps stay UTC whatever the time zone is set to.
static int64_t daysFromCivil(int64_t year, unsigned month, unsigned day)
{
  year -= (month <= 2u);

  int64_t era = (year >= 0) ? (year / 400) : ((year - 399) / 400);
  unsigned yearOfEra = (unsigned)(year - (era * 400));
  unsigned dayOfYear = (153u * (month + ((month > 2u) ? -3u : 9u)) + 2u) / 5u + day - 1u;
  unsigned dayOfEra = yearOfEra * 365u + yearOfEra / 4u - yearOfEra / 100u + dayOfYear;

  return (era * 146097) + (int64_t)dayOfEra - 719468;
}

static void civilFromDays(int64_t days, int* year, unsigned* month, unsigned* day)
{
  int64_t z = days + 719468;
  int64_t era = (z >= 0) ? (z / 146097) : ((z - 146096) / 146097);
  unsigned dayOfEra = (unsigned)(z - (era * 146097));
  unsigned yearOfEra =
      (unsigned)((dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365);
  int64_t y = (int64_t)yearOfEra + era * 400;
  unsigned dayOfYear = dayOfEra - (365u * yearOfEra + yearOfEra / 4u - yearOfEra / 100u);
  unsigned monthPrime = (5u * dayOfYear + 2u) / 153u;

  *day = dayOfYear - (153u * monthPrime + 2u) / 5u + 1u;
  *month = monthPrime + ((monthPrime < 10u) ? 3u : -9u);
  *year = (int)(y + (*month <= 2u));
}

static bool isLeapYear(int year)
{
  return ((year % 4) == 0) && (((year % 100) != 0) || ((year % 400) == 0));
}

static unsigned daysInMonth(int year, unsigned month)
{
  static const unsigned daysPerMonth[12] = {31u, 28u, 31u, 30u, 31u, 30u,
                                            31u, 31u, 30u, 31u, 30u, 31u};

  if ((month == 2u) && isLeapYear(year))
  {
    return 29u;
  }

  return daysPerMonth[month - 1u];
}

static bool digitsFixed(const char* text, size_t* position, size_t count, unsigned* out)
{
  unsigned value = 0u;

  for (size_t index = 0u; index < count; index++)
  {
    char character = text[*position + index];

    if ((character < '0') || (character > '9'))
    {
      return false;
    }

    value = value * 10u + (unsigned)(character - '0');
  }

  *position += count;
  *out = value;
  return true;
}

bool loggerTariffsParseUtc(const char* text, int64_t* out)
{
  if ((text == NULL) || (out == NULL))
  {
    return false;
  }

  if (strlen(text) != 20u)
  {
    return false;
  }

  size_t position = 0u;
  unsigned year = 0u;
  unsigned month = 0u;
  unsigned day = 0u;
  unsigned hour = 0u;
  unsigned minute = 0u;
  unsigned second = 0u;

  if (!digitsFixed(text, &position, 4u, &year) || (text[position++] != '-') ||
      !digitsFixed(text, &position, 2u, &month) || (text[position++] != '-') ||
      !digitsFixed(text, &position, 2u, &day) || (text[position++] != 'T') ||
      !digitsFixed(text, &position, 2u, &hour) || (text[position++] != ':') ||
      !digitsFixed(text, &position, 2u, &minute) || (text[position++] != ':') ||
      !digitsFixed(text, &position, 2u, &second))
  {
    return false;
  }

  if (text[position++] != 'Z')
  {
    return false;
  }

  if ((year < 2000u) || (year > 2099u) || (month < 1u) || (month > 12u) || (day < 1u) ||
      (day > daysInMonth((int)year, month)) || (hour > 23u) || (minute > 59u) || (second > 59u))
  {
    return false;
  }

  int64_t epoch = (daysFromCivil((int64_t)year, month, day) * 86400LL) + ((int64_t)hour * 3600LL) +
                  ((int64_t)minute * 60LL) + (int64_t)second;

  if ((epoch < TARIFF_EPOCH_MIN) || (epoch >= TARIFF_EPOCH_MAX))
  {
    return false;
  }

  *out = epoch;
  return true;
}

void loggerTariffsFormatUtc(int64_t epoch, char* out, size_t outSize)
{
  if ((out == NULL) || (outSize == 0u))
  {
    return;
  }

  int64_t seconds = epoch;
  int64_t days = seconds / 86400LL;

  if (seconds < 0)
  {
    days -= 1;
  }

  int64_t rest = seconds - (days * 86400LL);
  unsigned hour = (unsigned)(rest / 3600LL);
  rest %= 3600LL;
  unsigned minute = (unsigned)(rest / 60LL);
  unsigned second = (unsigned)(rest % 60LL);

  int year = 0;
  unsigned month = 0u;
  unsigned day = 0u;
  civilFromDays(days, &year, &month, &day);

  snprintf(out, outSize, "%04d-%02d-%02dT%02d:%02d:%02dZ", year, (int)month, (int)day, (int)hour,
           (int)minute, (int)second);
}

bool loggerTariffsClockSynced(void)
{
  return (time(NULL) >= (time_t)TARIFF_CLOCK_FLOOR);
}

//-- --------------------------------------------------------------------------
//-- Binary persistence on the meter store's volume
//-- --------------------------------------------------------------------------

static size_t writeUvarint(uint8_t* out, size_t size, size_t* position, uint64_t value)
{
  size_t written = 0u;

  do
  {
    if ((*position + written) >= size)
    {
      return 0u;
    }

    uint8_t byte = (uint8_t)(value & 0x7Fu);
    value >>= 7;

    if (value != 0u)
    {
      byte |= 0x80u;
    }

    out[*position + written] = byte;
    written++;
  } while (value != 0u);

  *position += written;
  return written;
}

static bool readUvarint(const uint8_t* data, size_t size, size_t* position, uint64_t* out)
{
  uint64_t value = 0u;
  unsigned shift = 0u;

  for (;;)
  {
    if (*position >= size)
    {
      return false;
    }

    uint8_t byte = data[*position];
    *position += 1u;
    value |= ((uint64_t)(byte & 0x7Fu)) << shift;

    if ((byte & 0x80u) == 0u)
    {
      break;
    }

    shift += 7u;

    if (shift > 63u)
    {
      return false;
    }
  }

  *out = value;
  return true;
}

static void tariffFilePath(char* out, size_t outSize, bool temporary)
{
  snprintf(out, outSize, "%s/%s", loggerStoreMountPoint(),
           temporary ? TARIFF_FILE_TMP_NAME : TARIFF_FILE_NAME);
}

//-- Serialises one schedule into a freshly malloc-ed buffer. The caller
//-- frees the buffer. NULL when the bound or the allocation fails.
static uint8_t* serializeSchedule(const loggerTariffInterval* intervals, size_t count,
                                  size_t* outSize)
{
  size_t currencyLength = strlen(currency);
  size_t capacity = 16u + 8u + count * TARIFF_RECORD_MAX_BYTES + 32u;
  uint8_t* buffer = psramAllocate(capacity);

  if (buffer == NULL)
  {
    return NULL;
  }

  size_t position = 0u;
  memcpy(buffer + position, TARIFF_FILE_MAGIC, 4u);
  position += 4u;
  buffer[position++] = (uint8_t)(TARIFF_FILE_VERSION & 0xFFu);
  buffer[position++] = (uint8_t)((TARIFF_FILE_VERSION >> 8) & 0xFFu);
  buffer[position++] = (uint8_t)(currencyLength & 0xFFu);
  buffer[position++] = (uint8_t)((currencyLength >> 8) & 0xFFu);
  memcpy(buffer + position, currency, currencyLength);
  position += currencyLength;

  buffer[position++] = (uint8_t)(count & 0xFFu);
  buffer[position++] = (uint8_t)((count >> 8) & 0xFFu);
  buffer[position++] = (uint8_t)((count >> 16) & 0xFFu);
  buffer[position++] = (uint8_t)((count >> 24) & 0xFFu);

  int64_t previousStart = 0;

  for (size_t index = 0u; index < count; index++)
  {
    const loggerTariffInterval* interval = &intervals[index];
    uint64_t startDelta = (uint64_t)(interval->startEpochS - previousStart);
    uint64_t duration = (uint64_t)(interval->endEpochS - interval->startEpochS);

    if (writeUvarint(buffer, capacity, &position, startDelta) == 0u)
    {
      free(buffer);
      return NULL;
    }

    if (writeUvarint(buffer, capacity, &position, duration) == 0u)
    {
      free(buffer);
      return NULL;
    }

    if (position + 13u > capacity)
    {
      free(buffer);
      return NULL;
    }

    //-- Three floats plus the presence byte: a missing price keeps its
    //-- zeroed value and only the matching presence bit is stored.
    memcpy(buffer + position, &interval->importPricePerKwh, sizeof(float));
    position += sizeof(float);
    memcpy(buffer + position, &interval->exportPricePerKwh, sizeof(float));
    position += sizeof(float);
    memcpy(buffer + position, &interval->gasPricePerM3, sizeof(float));
    position += sizeof(float);

    uint8_t presence = 0u;

    if (interval->hasImport)
    {
      presence |= TARIFF_PRESENT_IMPORT;
    }

    if (interval->hasExport)
    {
      presence |= TARIFF_PRESENT_EXPORT;
    }

    if (interval->hasGas)
    {
      presence |= TARIFF_PRESENT_GAS;
    }

    buffer[position++] = presence;
    previousStart = interval->startEpochS;
  }

  *outSize = position;
  return buffer;
}

//-- True when the schedule is whole on disk. The write goes to a temporary
//-- file that replaces the real one: a failure mid-write leaves the
//-- previous schedule in place.
static bool persistSchedule(const loggerTariffInterval* intervals, size_t count)
{
  if (loggerStoreMountPoint() == NULL)
  {
    LOGGER_LOGW(tag, "storage volume is not mounted, the schedule stays in RAM only");
    return false;
  }

  size_t payloadSize = 0u;
  uint8_t* payload = serializeSchedule(intervals, count, &payloadSize);

  if (payload == NULL)
  {
    LOGGER_LOGE(tag, "could not serialise the tariff schedule");
    return false;
  }

  char temporaryPath[96];
  char finalPath[96];
  tariffFilePath(temporaryPath, sizeof temporaryPath, true);
  tariffFilePath(finalPath, sizeof finalPath, false);

  bool stored = false;
  FILE* file = fopen(temporaryPath, "wb");

  if (file != NULL)
  {
    stored = (fwrite(payload, 1u, payloadSize, file) == payloadSize);

    if (fclose(file) != 0)
    {
      stored = false;
    }
  }

  free(payload);

  if (!stored)
  {
    LOGGER_LOGE(tag, "could not write %s", temporaryPath);
    remove(temporaryPath);
    return false;
  }

  //-- littlefs replaces an existing target atomically: the previous
  //-- schedule stays readable until the rename lands.
  if (rename(temporaryPath, finalPath) != 0)
  {
    LOGGER_LOGE(tag, "could not rename the tariff schedule into place");
    remove(temporaryPath);
    return false;
  }

  ESP_LOGD(tag, "persisted %u tariff interval(s) to %s", (unsigned)count, finalPath);
  return true;
}

static void storeCurrency(const char* candidate)
{
  if ((candidate != NULL) && (candidate[0] != '\0'))
  {
    snprintf(currency, sizeof currency, "%s", candidate);
  }
}

//-- Reads the schedule file into spare. The caller holds the mutex; on any
//-- inconsistency the schedule simply starts empty (a corrupt file must
//-- never take the API or MQTT path down). The mount is not yet available
//-- while the meter store task is still starting: the load is then retried
//-- on the next operation.
static void ensureLoaded(void)
{
  if (loaded || (loggerStoreMountPoint() == NULL))
  {
    return;
  }

  loaded = true;

  char path[96];
  tariffFilePath(path, sizeof path, false);
  FILE* file = fopen(path, "rb");

  if (file == NULL)
  {
    storeCount = 0u;
    return;
  }

  fseek(file, 0, SEEK_END);
  long fileSize = ftell(file);
  fseek(file, 0, SEEK_SET);

  size_t capacity = 16u + 8u + LOGGER_TARIFF_STORED_MAX * TARIFF_RECORD_MAX_BYTES + 32u;

  if ((fileSize < 0) || ((size_t)fileSize > capacity))
  {
    LOGGER_LOGW(tag, "tariff schedule file has an implausible size, starting empty");
    fclose(file);
    storeCount = 0u;
    return;
  }

  uint8_t* payload = psramAllocate((size_t)fileSize + 1u);

  if (payload == NULL)
  {
    LOGGER_LOGE(tag, "out of memory reading the tariff schedule");
    fclose(file);
    return;
  }

  bool read = (fread(payload, 1u, (size_t)fileSize, file) == (size_t)fileSize);
  fclose(file);

  size_t position = 0u;
  bool headerOk = read && (fileSize >= 12) && (memcmp(payload, TARIFF_FILE_MAGIC, 4u) == 0);
  uint16_t fileVersion = 0u;

  if (headerOk)
  {
    fileVersion = (uint16_t)(payload[4] | ((uint16_t)payload[5] << 8));
  }

  //-- Both on-disk versions load: version 1 has no gas prices (its records
  //-- are imported-price intervals) and a different presence byte shape.
  if (!headerOk || ((fileVersion != 1u) && (fileVersion != (uint16_t)TARIFF_FILE_VERSION)))
  {
    LOGGER_LOGW(tag, "tariff schedule file is not readable, starting empty");
    free(payload);
    storeCount = 0u;
    return;
  }

  //-- Fixed record tail: three floats and the presence byte for version 2,
  //-- two floats and the export presence byte for version 1.
  size_t recordTailBytes = (fileVersion == 1u) ? 9u : 13u;

  position = 6u;
  unsigned currencyLength = (unsigned)(payload[6] | (payload[7] << 8));
  position = 8u;

  if (currencyLength >= LOGGER_TARIFF_CURRENCY_MAX)
  {
    free(payload);
    storeCount = 0u;
    return;
  }

  char storedCurrency[LOGGER_TARIFF_CURRENCY_MAX];
  memcpy(storedCurrency, payload + position, currencyLength);
  storedCurrency[currencyLength] = '\0';
  position += currencyLength;

  uint32_t count = (uint32_t)payload[position] | ((uint32_t)payload[position + 1u] << 8) |
                   ((uint32_t)payload[position + 2u] << 16) |
                   ((uint32_t)payload[position + 3u] << 24);
  position += 4u;

  if (count > LOGGER_TARIFF_STORED_MAX)
  {
    free(payload);
    storeCount = 0u;
    return;
  }

  size_t loadedCount = 0u;
  int64_t previousStart = 0;
  bool valid = true;

  while (loadedCount < count)
  {
    uint64_t startDelta = 0u;
    uint64_t duration = 0u;

    if (!readUvarint(payload, (size_t)fileSize, &position, &startDelta) ||
        !readUvarint(payload, (size_t)fileSize, &position, &duration) ||
        (position + recordTailBytes > (size_t)fileSize))
    {
      valid = false;
      break;
    }

    loggerTariffInterval* interval = &spare[loadedCount];
    interval->startEpochS = previousStart + (int64_t)startDelta;
    interval->endEpochS = interval->startEpochS + (int64_t)duration;
    interval->hasImport = false;
    interval->hasExport = false;
    interval->hasGas = false;
    interval->importPricePerKwh = 0.0f;
    interval->exportPricePerKwh = 0.0f;
    interval->gasPricePerM3 = 0.0f;

    memcpy(&interval->importPricePerKwh, payload + position, sizeof(float));
    position += sizeof(float);
    memcpy(&interval->exportPricePerKwh, payload + position, sizeof(float));
    position += sizeof(float);

    if (fileVersion == 1u)
    {
      //-- Version 1 required an import price and only flagged the export.
      interval->hasImport = true;
      interval->hasExport = (payload[position++] != 0u);
    }
    else
    {
      memcpy(&interval->gasPricePerM3, payload + position, sizeof(float));
      position += sizeof(float);

      uint8_t presence = payload[position++];
      interval->hasImport = (presence & TARIFF_PRESENT_IMPORT) != 0u;
      interval->hasExport = (presence & TARIFF_PRESENT_EXPORT) != 0u;
      interval->hasGas = (presence & TARIFF_PRESENT_GAS) != 0u;
    }

    if ((duration == 0u) || (interval->endEpochS <= interval->startEpochS) ||
        (interval->startEpochS < TARIFF_EPOCH_MIN) || (interval->endEpochS > TARIFF_EPOCH_MAX) ||
        (!interval->hasImport && !interval->hasExport && !interval->hasGas) ||
        ((loadedCount > 0u) && (interval->startEpochS <= previousStart)))
    {
      valid = false;
      break;
    }

    previousStart = interval->startEpochS;
    loadedCount++;
  }

  if (!valid || (position != (size_t)fileSize))
  {
    LOGGER_LOGW(tag, "tariff schedule file failed validation, starting empty");
    free(payload);
    storeCount = 0u;
    return;
  }

  memcpy(store, spare, loadedCount * sizeof(loggerTariffInterval));
  storeCount = loadedCount;
  storeCurrency(storedCurrency);
  free(payload);

  LOGGER_LOGI(tag, "loaded %u tariff interval(s) from %s", (unsigned)loadedCount, path);
}

//-- --------------------------------------------------------------------------
//-- Ingestion
//-- --------------------------------------------------------------------------

//-- Cheap pre-parse guard: the shared format is pure printable ASCII and
//-- nests only a few levels deep. Enforcing both here (instead of trusting
//-- the caller) keeps a hostile or corrupted document from driving the
//-- cJSON recursion -- and with it the caller's stack -- arbitrarily deep.
static bool payloadLooksSafe(const char* json, size_t length, char* reason, size_t reasonSize)
{
  int depth = 0;

  for (size_t index = 0u; index < length; index++)
  {
    unsigned char byte = (unsigned char)json[index];

    if ((byte < 0x20u) || (byte > 0x7Eu))
    {
      snprintf(reason, reasonSize, "the document must be printable ASCII");
      return false;
    }

    if ((byte == '{') || (byte == '['))
    {
      depth++;
    }
    else if ((byte == '}') || (byte == ']'))
    {
      depth--;
    }

    if ((depth < 0) || (depth > TARIFF_JSON_DEPTH_MAX))
    {
      snprintf(reason, reasonSize, "the document nests too deeply");
      return false;
    }
  }

  if (depth != 0)
  {
    snprintf(reason, reasonSize, "unbalanced braces or brackets");
    return false;
  }

  return true;
}

static int compareIntervals(const void* left, const void* right)
{
  const loggerTariffInterval* a = (const loggerTariffInterval*)left;
  const loggerTariffInterval* b = (const loggerTariffInterval*)right;

  if (a->startEpochS != b->startEpochS)
  {
    return (a->startEpochS < b->startEpochS) ? -1 : 1;
  }

  if (a->endEpochS != b->endEpochS)
  {
    return (a->endEpochS < b->endEpochS) ? -1 : 1;
  }

  return 0;
}

//-- Parses and fully validates one batch into batch[]. On success the
//-- array holds count sorted, non-overlapping intervals and the currency is
//-- copied. Every failure writes a reason and refuses the whole batch.
static loggerTariffIngestResult_t parseBatch(const char* json, size_t length, size_t* outCount,
                                             char* currencyOut, char* reason, size_t reasonSize)
{
  *outCount = 0u;

  if (!payloadLooksSafe(json, length, reason, reasonSize))
  {
    return LOGGER_TARIFF_REJECTED;
  }

  //-- The length includes the terminating NUL (json[length]) exactly like
  //-- cJSON_ParseWithOpts does: without it, require_null_terminated always
  //-- fails against the byte just past the declared length.
  const char* parseEnd = NULL;
  cJSON* root = cJSON_ParseWithLengthOpts(json, length + 1u, &parseEnd, true);

  if (root == NULL)
  {
    snprintf(reason, reasonSize, "malformed JSON document");
    return LOGGER_TARIFF_REJECTED;
  }

  loggerTariffIngestResult_t result = LOGGER_TARIFF_OK;
  cJSON* schema = cJSON_GetObjectItemCaseSensitive(root, "schemaVersion");
  cJSON* currencyItem = cJSON_GetObjectItemCaseSensitive(root, "currency");
  cJSON* intervalsItem = cJSON_GetObjectItemCaseSensitive(root, "intervals");
  char parsedCurrency[LOGGER_TARIFF_CURRENCY_MAX] = "";

  if (!cJSON_IsNumber(schema) || (schema->valuedouble != 1.0))
  {
    snprintf(reason, reasonSize, "unsupported schemaVersion");
    result = LOGGER_TARIFF_REJECTED;
  }
  else if (!cJSON_IsString(currencyItem) || (strlen(currencyItem->valuestring) != 3u))
  {
    snprintf(reason, reasonSize, "currency must be a three-letter code");
    result = LOGGER_TARIFF_REJECTED;
  }
  else if (!cJSON_IsArray(intervalsItem))
  {
    snprintf(reason, reasonSize, "intervals must be an array");
    result = LOGGER_TARIFF_REJECTED;
  }
  else
  {
    for (unsigned index = 0u; index < 3u; index++)
    {
      char letter = currencyItem->valuestring[index];

      if ((letter < 'A') || (letter > 'z') || ((letter > 'Z') && (letter < 'a')))
      {
        parsedCurrency[0] = '\0';
        break;
      }

      parsedCurrency[index] = (char)((letter >= 'a') ? (letter - ('a' - 'A')) : letter);
    }

    parsedCurrency[3] = '\0';

    if (parsedCurrency[0] == '\0')
    {
      snprintf(reason, reasonSize, "currency must be a three-letter code");
      result = LOGGER_TARIFF_REJECTED;
    }
  }

  size_t count = 0u;

  if (result == LOGGER_TARIFF_OK)
  {
    int arraySize = cJSON_GetArraySize(intervalsItem);

    if (arraySize == 0)
    {
      snprintf(reason, reasonSize, "the intervals list is empty");
      result = LOGGER_TARIFF_REJECTED;
    }
    else if (arraySize > (int)LOGGER_TARIFF_BATCH_MAX)
    {
      snprintf(reason, reasonSize, "the batch exceeds %u intervals",
               (unsigned)LOGGER_TARIFF_BATCH_MAX);
      result = LOGGER_TARIFF_TOO_LARGE;
    }
  }

  if (result == LOGGER_TARIFF_OK)
  {
    cJSON* item = NULL;

    cJSON_ArrayForEach(item, intervalsItem)
    {
      loggerTariffInterval* interval = &batch[count];
      cJSON* start = cJSON_GetObjectItemCaseSensitive(item, "start");
      cJSON* end = cJSON_GetObjectItemCaseSensitive(item, "end");
      cJSON* importPrice = cJSON_GetObjectItemCaseSensitive(item, "importPricePerKwh");
      cJSON* exportPrice = cJSON_GetObjectItemCaseSensitive(item, "exportPricePerKwh");
      cJSON* gasPrice = cJSON_GetObjectItemCaseSensitive(item, "gasPricePerM3");

      if (!cJSON_IsObject(item))
      {
        snprintf(reason, reasonSize, "every interval must be an object");
        result = LOGGER_TARIFF_REJECTED;
        break;
      }

      if (!cJSON_IsString(start) ||
          !loggerTariffsParseUtc(start->valuestring, &interval->startEpochS) ||
          !cJSON_IsString(end) || !loggerTariffsParseUtc(end->valuestring, &interval->endEpochS))
      {
        snprintf(reason, reasonSize, "invalid interval timestamps");
        result = LOGGER_TARIFF_REJECTED;
        break;
      }

      if (interval->endEpochS <= interval->startEpochS)
      {
        snprintf(reason, reasonSize, "the interval end must be after its start");
        result = LOGGER_TARIFF_REJECTED;
        break;
      }

      //-- Each of the three prices is optional (absent or null means
      //-- unknown); the interval must carry at least one of them.
      interval->hasImport = false;
      interval->hasExport = false;
      interval->hasGas = false;
      interval->importPricePerKwh = 0.0f;
      interval->exportPricePerKwh = 0.0f;
      interval->gasPricePerM3 = 0.0f;

      if ((importPrice != NULL) && !cJSON_IsNull(importPrice))
      {
        if (!cJSON_IsNumber(importPrice) || !isfinite(importPrice->valuedouble))
        {
          snprintf(reason, reasonSize, "importPricePerKwh must be a finite number or null");
          result = LOGGER_TARIFF_REJECTED;
          break;
        }

        interval->importPricePerKwh = (float)importPrice->valuedouble;
        interval->hasImport = true;
      }

      if ((exportPrice != NULL) && !cJSON_IsNull(exportPrice))
      {
        if (!cJSON_IsNumber(exportPrice) || !isfinite(exportPrice->valuedouble))
        {
          snprintf(reason, reasonSize, "exportPricePerKwh must be a finite number or null");
          result = LOGGER_TARIFF_REJECTED;
          break;
        }

        interval->exportPricePerKwh = (float)exportPrice->valuedouble;
        interval->hasExport = true;
      }

      if ((gasPrice != NULL) && !cJSON_IsNull(gasPrice))
      {
        if (!cJSON_IsNumber(gasPrice) || !isfinite(gasPrice->valuedouble))
        {
          snprintf(reason, reasonSize, "gasPricePerM3 must be a finite number or null");
          result = LOGGER_TARIFF_REJECTED;
          break;
        }

        interval->gasPricePerM3 = (float)gasPrice->valuedouble;
        interval->hasGas = true;
      }

      if (!interval->hasImport && !interval->hasExport && !interval->hasGas)
      {
        snprintf(reason, reasonSize, "every interval must carry at least one price");
        result = LOGGER_TARIFF_REJECTED;
        break;
      }

      count++;
    }
  }

  if (result == LOGGER_TARIFF_OK)
  {
    qsort(batch, count, sizeof(loggerTariffInterval), compareIntervals);

    for (size_t index = 1u; index < count; index++)
    {
      if (batch[index - 1u].endEpochS > batch[index].startEpochS)
      {
        snprintf(reason, reasonSize, "overlapping intervals in the batch");
        result = LOGGER_TARIFF_REJECTED;
        break;
      }
    }
  }

  cJSON_Delete(root);

  if (result == LOGGER_TARIFF_OK)
  {
    *outCount = count;
    snprintf(currencyOut, LOGGER_TARIFF_CURRENCY_MAX, "%s", parsedCurrency);
  }

  return result;
}

static bool overlaps(const loggerTariffInterval* left, const loggerTariffInterval* right)
{
  return (left->startEpochS < right->endEpochS) && (right->startEpochS < left->endEpochS);
}

//-- The first instant of the previous local calendar month: intervals
//-- ending before it are no longer inside the retained window. False when
//-- the clock is not synchronised (pruning then simply waits).
static bool previousMonthStartUtc(int64_t* out)
{
  time_t now = time(NULL);

  if ((int64_t)now < TARIFF_CLOCK_FLOOR)
  {
    return false;
  }

  struct tm local;

  if (localtime_r(&now, &local) == NULL)
  {
    return false;
  }

  local.tm_mday = 1;
  local.tm_hour = 0;
  local.tm_min = 0;
  local.tm_sec = 0;
  local.tm_isdst = -1;

  time_t thisMonth = mktime(&local);

  if (thisMonth == (time_t)-1)
  {
    return false;
  }

  local.tm_mon -= 1;

  if (local.tm_mon < 0)
  {
    local.tm_mon = 11;
    local.tm_year -= 1;
  }

  local.tm_isdst = -1;
  time_t previousMonth = mktime(&local);

  if (previousMonth == (time_t)-1)
  {
    return false;
  }

  *out = (int64_t)previousMonth;
  return true;
}

loggerTariffIngestResult_t loggerTariffsIngestJson(const char* json, size_t length,
                                                   const char* source, char* reasonOut,
                                                   size_t reasonSize)
{
  char reason[160] = "";
  char newCurrency[LOGGER_TARIFF_CURRENCY_MAX];
  size_t count = 0u;

  if ((json == NULL) || (reasonOut == NULL) || (reasonSize == 0u))
  {
    return LOGGER_TARIFF_REJECTED;
  }

  reasonOut[0] = '\0';

  if (length > LOGGER_TARIFF_JSON_MAX_BYTES)
  {
    snprintf(reason, sizeof reason, "the document exceeds %u bytes",
             (unsigned)LOGGER_TARIFF_JSON_MAX_BYTES);
    snprintf(reasonOut, reasonSize, "%s", reason);
    return LOGGER_TARIFF_TOO_LARGE;
  }

  xSemaphoreTake(tariffsMutex, portMAX_DELAY);

  if (!ensureBuffers())
  {
    xSemaphoreGive(tariffsMutex);
    snprintf(reasonOut, reasonSize, "out of memory for the tariff schedules");
    return LOGGER_TARIFF_NO_SPACE;
  }

  loggerTariffIngestResult_t result =
      parseBatch(json, length, &count, newCurrency, reason, sizeof reason);

  if (result != LOGGER_TARIFF_OK)
  {
    xSemaphoreGive(tariffsMutex);
    snprintf(reasonOut, reasonSize, "%s", reason);
    return result;
  }

  ensureLoaded();

  //-- Classify every stored interval against the batch: an exact same
  //-- start and end is replaced, any other overlap is a conflict that
  //-- refuses the whole batch and keeps the stored schedule intact.
  bool* replaced = calloc(storeCount > 0u ? storeCount : 1u, sizeof(bool));

  if (replaced == NULL)
  {
    xSemaphoreGive(tariffsMutex);
    snprintf(reasonOut, reasonSize, "out of memory for the tariff schedules");
    return LOGGER_TARIFF_NO_SPACE;
  }

  for (size_t stored = 0u; (stored < storeCount) && (result == LOGGER_TARIFF_OK); stored++)
  {
    for (size_t incoming = 0u; incoming < count; incoming++)
    {
      if (!overlaps(&store[stored], &batch[incoming]))
      {
        continue;
      }

      if ((store[stored].startEpochS == batch[incoming].startEpochS) &&
          (store[stored].endEpochS == batch[incoming].endEpochS))
      {
        replaced[stored] = true;
      }
      else
      {
        snprintf(reason, sizeof reason, "the batch overlaps a stored interval");
        result = LOGGER_TARIFF_CONFLICT;
        break;
      }
    }
  }

  size_t candidateCount = 0u;

  if (result == LOGGER_TARIFF_OK)
  {
    for (size_t stored = 0u; stored < storeCount; stored++)
    {
      if (!replaced[stored])
      {
        spare[candidateCount++] = store[stored];
      }
    }

    memcpy(&spare[candidateCount], batch, count * sizeof(loggerTariffInterval));
    candidateCount += count;
    qsort(spare, candidateCount, sizeof(loggerTariffInterval), compareIntervals);
  }

  free(replaced);

  //-- The current and previous calendar month must survive: everything
  //-- that ended before the previous month began is dropped first, so the
  //-- stored bound only bites on schedules the user keeps pushing far into
  //-- the future.
  int64_t retentionCutoff = 0;

  if ((result == LOGGER_TARIFF_OK) && previousMonthStartUtc(&retentionCutoff))
  {
    size_t kept = 0u;

    for (size_t index = 0u; index < candidateCount; index++)
    {
      if (spare[index].endEpochS > retentionCutoff)
      {
        spare[kept++] = spare[index];
      }
    }

    candidateCount = kept;
  }

  if ((result == LOGGER_TARIFF_OK) && (candidateCount > LOGGER_TARIFF_STORED_MAX))
  {
    snprintf(reason, sizeof reason, "the schedule exceeds %u stored intervals",
             (unsigned)LOGGER_TARIFF_STORED_MAX);
    result = LOGGER_TARIFF_NO_SPACE;
  }

  if ((result == LOGGER_TARIFF_OK) && !persistSchedule(spare, candidateCount))
  {
    snprintf(reason, sizeof reason, "the schedule could not be persisted");
    result = LOGGER_TARIFF_IO_ERROR;
  }

  if (result == LOGGER_TARIFF_OK)
  {
    memcpy(store, spare, candidateCount * sizeof(loggerTariffInterval));
    storeCount = candidateCount;
    storeCurrency(newCurrency);

    LOGGER_LOGI(tag, "accepted %u interval(s) via %s, %u stored, currency %s", (unsigned)count,
                (source != NULL ? source : "unknown"), (unsigned)storeCount, currency);
  }

  xSemaphoreGive(tariffsMutex);

  if (result != LOGGER_TARIFF_OK)
  {
    snprintf(reasonOut, reasonSize, "%s", reason);
  }

  //-- The verbose console hears every schedule update, accepted or refused,
  //-- with the source that delivered it (MQTT or the HTTP API).
  if (result == LOGGER_TARIFF_OK)
  {
    loggerControlConsoleEvent(LOGGER_VERBOSE_EVENTS, LOGGER_CONSOLE_EVENT,
                              "tariffs: schedule accepted via %s (%u interval(s))",
                              (source != NULL) ? source : "unknown", (unsigned)count);
  }
  else
  {
    loggerControlConsoleEvent(LOGGER_VERBOSE_EVENTS, LOGGER_CONSOLE_ERROR,
                              "tariffs: schedule refused via %s - %s",
                              (source != NULL) ? source : "unknown", reason);
  }

  return result;
}

//-- --------------------------------------------------------------------------
//-- Queries
//-- --------------------------------------------------------------------------

bool loggerTariffsActiveInterval(time_t now, loggerTariffInterval* outInterval)
{
  if ((outInterval == NULL) || ((int64_t)now < TARIFF_CLOCK_FLOOR))
  {
    return false;
  }

  xSemaphoreTake(tariffsMutex, portMAX_DELAY);

  if (!ensureBuffers())
  {
    xSemaphoreGive(tariffsMutex);
    return false;
  }

  ensureLoaded();

  bool found = false;

  for (size_t index = 0u; index < storeCount; index++)
  {
    if ((store[index].startEpochS <= (int64_t)now) && (store[index].endEpochS > (int64_t)now))
    {
      *outInterval = store[index];
      found = true;
      break;
    }
  }

  xSemaphoreGive(tariffsMutex);
  return found;
}

const char* loggerTariffsCurrency(void)
{
  xSemaphoreTake(tariffsMutex, portMAX_DELAY);

  if (ensureBuffers())
  {
    ensureLoaded();
  }

  xSemaphoreGive(tariffsMutex);

  return currency;
}

size_t loggerTariffsStoredCount(void)
{
  xSemaphoreTake(tariffsMutex, portMAX_DELAY);
  size_t count = 0u;

  if (ensureBuffers())
  {
    ensureLoaded();
    count = storeCount;
  }

  xSemaphoreGive(tariffsMutex);

  return count;
}

esp_err_t loggerTariffsForEachRaw(time_t from, time_t to, loggerTariffIntervalVisitor visitor,
                                  void* userData)
{
  if ((visitor == NULL) || (to <= from))
  {
    return ESP_ERR_INVALID_ARG;
  }

  xSemaphoreTake(tariffsMutex, portMAX_DELAY);

  if (!ensureBuffers())
  {
    xSemaphoreGive(tariffsMutex);
    return ESP_ERR_NO_MEM;
  }

  ensureLoaded();

  esp_err_t result = ESP_OK;

  for (size_t index = 0u; index < storeCount; index++)
  {
    if (store[index].endEpochS <= (int64_t)from)
    {
      continue;
    }

    if (store[index].startEpochS >= (int64_t)to)
    {
      break;
    }

    if (!visitor(&store[index], userData))
    {
      break;
    }
  }

  xSemaphoreGive(tariffsMutex);
  return result;
}

//-- --------------------------------------------------------------------------
//-- Calendar buckets (local calendar, DST aware)
//-- --------------------------------------------------------------------------

//-- Occurrences of one local period as UTC instants: the DST hint set and
//-- cleared through mktime. A normal period yields one instant, the
//-- repeated autumn hour two, the vanished spring hour zero real buckets
//-- (its instants still bound the neighbouring buckets).
static size_t periodOccurrences(const struct tm* period, int64_t* out, size_t capacity)
{
  struct tm hinted = *period;
  hinted.tm_isdst = 1;
  time_t withDst = mktime(&hinted);

  hinted = *period;
  hinted.tm_isdst = 0;
  time_t withoutDst = mktime(&hinted);

  size_t count = 0u;

  if ((withDst != (time_t)-1) && (count < capacity))
  {
    out[count++] = (int64_t)withDst;
  }

  if ((withoutDst != (time_t)-1) && (withoutDst != withDst) && (count < capacity))
  {
    out[count++] = (time_t)withoutDst;
  }

  if ((count == 2u) && (out[0] > out[1]))
  {
    int64_t swap = out[0];
    out[0] = out[1];
    out[1] = swap;
  }

  return count;
}

//-- True when the instant really names the local period (the normalised
//-- instant of a vanished hour names the hour after it instead).
static bool occurrenceMatchesPeriod(const struct tm* period, int64_t utc,
                                    loggerTariffResolution_t resolution)
{
  time_t instant = (time_t)utc;
  struct tm back;

  if (localtime_r(&instant, &back) == NULL)
  {
    return false;
  }

  if ((back.tm_year != period->tm_year) || (back.tm_mon != period->tm_mon) ||
      (back.tm_mday != period->tm_mday))
  {
    return false;
  }

  if (resolution == LOGGER_TARIFF_HOUR)
  {
    return (back.tm_hour == period->tm_hour);
  }

  return (back.tm_hour == 0);
}

static void truncatePeriod(struct tm* period, loggerTariffResolution_t resolution)
{
  period->tm_min = 0;
  period->tm_sec = 0;

  if (resolution != LOGGER_TARIFF_HOUR)
  {
    period->tm_hour = 0;
  }

  if (resolution == LOGGER_TARIFF_MONTH)
  {
    period->tm_mday = 1;
  }
}

static void advancePeriod(struct tm* period, loggerTariffResolution_t resolution)
{
  if (resolution == LOGGER_TARIFF_HOUR)
  {
    period->tm_hour += 1;

    if (period->tm_hour > 23)
    {
      period->tm_hour = 0;
      period->tm_mday += 1;
      unsigned days = daysInMonth(period->tm_year + 1900, (unsigned)(period->tm_mon + 1));

      if (period->tm_mday > (int)days)
      {
        period->tm_mday = 1;
        period->tm_mon += 1;

        if (period->tm_mon > 11)
        {
          period->tm_mon = 0;
          period->tm_year += 1;
        }
      }
    }

    return;
  }

  if (resolution == LOGGER_TARIFF_DAY)
  {
    period->tm_mday += 1;
    unsigned days = daysInMonth(period->tm_year + 1900, (unsigned)(period->tm_mon + 1));

    if (period->tm_mday > (int)days)
    {
      period->tm_mday = 1;
      period->tm_mon += 1;

      if (period->tm_mon > 11)
      {
        period->tm_mon = 0;
        period->tm_year += 1;
      }
    }

    return;
  }

  period->tm_mon += 1;

  if (period->tm_mon > 11)
  {
    period->tm_mon = 0;
    period->tm_year += 1;
  }
}

static void writePeriodLabel(const struct tm* period, loggerTariffResolution_t resolution,
                             char* out, size_t outSize)
{
  if (resolution == LOGGER_TARIFF_HOUR)
  {
    snprintf(out, outSize, "%04d-%02d-%02dT%02d", period->tm_year + 1900, period->tm_mon + 1,
             period->tm_mday, period->tm_hour);
    return;
  }

  if (resolution == LOGGER_TARIFF_DAY)
  {
    snprintf(out, outSize, "%04d-%02d-%02d", period->tm_year + 1900, period->tm_mon + 1,
             period->tm_mday);
    return;
  }

  snprintf(out, outSize, "%04d-%02d", period->tm_year + 1900, period->tm_mon + 1);
}

esp_err_t loggerTariffsAggregate(time_t from, time_t to, loggerTariffResolution_t resolution,
                                 loggerTariffBucketVisitor visitor, void* userData)
{
  if ((visitor == NULL) || (to <= from) || (resolution == LOGGER_TARIFF_RAW) ||
      (resolution >= LOGGER_TARIFF_MONTH + 1))
  {
    return ESP_ERR_INVALID_ARG;
  }

  xSemaphoreTake(tariffsMutex, portMAX_DELAY);

  if (!ensureBuffers())
  {
    xSemaphoreGive(tariffsMutex);
    return ESP_ERR_NO_MEM;
  }

  ensureLoaded();

  esp_err_t result = ESP_OK;
  struct tm period;
  time_t anchor = from;

  if (localtime_r(&anchor, &period) == NULL)
  {
    xSemaphoreGive(tariffsMutex);
    return ESP_ERR_INVALID_ARG;
  }

  truncatePeriod(&period, resolution);

  int64_t periodCands[2];
  size_t periodCount = periodOccurrences(&period, periodCands, 2u);
  size_t bucketsEmitted = 0u;
  size_t cursor = 0u;
  bool stopWalk = false;

  for (;;)
  {
    struct tm nextPeriod = period;
    advancePeriod(&nextPeriod, resolution);

    int64_t nextCands[2];
    size_t nextCount = periodOccurrences(&nextPeriod, nextCands, 2u);

    for (size_t index = 0u; index < periodCount; index++)
    {
      int64_t bucketStart = periodCands[index];

      if (bucketStart >= (int64_t)to)
      {
        break;
      }

      int64_t bucketEnd = INT64_MAX;

      //-- Only instants that really land inside their own calendar period
      //-- may bound a bucket: a DST-guessed candidate can name an instant
      //-- that belongs to the neighbouring period (a false "midnight" one
      //-- hour early) and would truncate a perfectly normal day.
      for (size_t other = index + 1u; other < periodCount; other++)
      {
        if ((periodCands[other] > bucketStart) &&
            occurrenceMatchesPeriod(&period, periodCands[other], resolution))
        {
          bucketEnd = periodCands[other];
          break;
        }
      }

      for (size_t other = 0u; (other < nextCount) && (bucketEnd == INT64_MAX); other++)
      {
        if ((nextCands[other] > bucketStart) &&
            occurrenceMatchesPeriod(&nextPeriod, nextCands[other], resolution))
        {
          bucketEnd = nextCands[other];
          break;
        }
      }

      //-- A period that names no real instant at all (the spring-forward
      //-- skipped hour) still has to bound the bucket before it, so fall
      //-- back to the earliest later candidate even when it matches none.
      for (size_t other = 0u; (other < nextCount) && (bucketEnd == INT64_MAX); other++)
      {
        if (nextCands[other] > bucketStart)
        {
          bucketEnd = nextCands[other];
          break;
        }
      }

      if (bucketEnd == INT64_MAX)
      {
        result = ESP_ERR_INVALID_STATE;
        break;
      }

      if (!occurrenceMatchesPeriod(&period, bucketStart, resolution))
      {
        continue;
      }

      int64_t clipStart = (bucketStart > (int64_t)from) ? bucketStart : (int64_t)from;
      int64_t clipEnd = (bucketEnd < (int64_t)to) ? bucketEnd : (int64_t)to;

      if (clipEnd <= clipStart)
      {
        continue;
      }

      loggerTariffBucket bucket;
      memset(&bucket, 0, sizeof bucket);
      bucket.startUtcS = bucketStart;
      bucket.endUtcS = bucketEnd;
      bucket.totalSeconds = (uint64_t)(clipEnd - clipStart);
      writePeriodLabel(&period, resolution, bucket.localLabel, sizeof bucket.localLabel);

      while ((cursor < storeCount) && (store[cursor].endEpochS <= clipStart))
      {
        cursor++;
      }

      double importPriceSeconds = 0.0;
      double exportPriceSeconds = 0.0;
      uint64_t importCoverage = 0u;
      uint64_t exportCoverage = 0u;

      for (size_t interval = cursor;
           (interval < storeCount) && (store[interval].startEpochS < clipEnd); interval++)
      {
        int64_t coveredStart =
            (store[interval].startEpochS > clipStart) ? store[interval].startEpochS : clipStart;
        int64_t coveredEnd =
            (store[interval].endEpochS < clipEnd) ? store[interval].endEpochS : clipEnd;

        if (coveredEnd <= coveredStart)
        {
          continue;
        }

        uint64_t coveredSeconds = (uint64_t)(coveredEnd - coveredStart);

        if (store[interval].hasImport)
        {
          importPriceSeconds += (double)store[interval].importPricePerKwh * (double)coveredSeconds;
          importCoverage += coveredSeconds;
        }

        if (store[interval].hasExport)
        {
          exportPriceSeconds += (double)store[interval].exportPricePerKwh * (double)coveredSeconds;
          exportCoverage += coveredSeconds;
        }
      }

      bucket.importCoveredSeconds = importCoverage;
      bucket.exportCoveredSeconds = exportCoverage;
      bucket.importPricePerKwh =
          (importCoverage > 0u) ? (importPriceSeconds / (double)importCoverage) : 0.0;
      bucket.exportPricePerKwh =
          (exportCoverage > 0u) ? (exportPriceSeconds / (double)exportCoverage) : 0.0;

      bucketsEmitted++;

      if (bucketsEmitted > LOGGER_TARIFF_BUCKET_MAX)
      {
        result = ESP_ERR_INVALID_STATE;
        break;
      }

      if (!visitor(&bucket, userData))
      {
        stopWalk = true;
        break;
      }
    }

    if ((result != ESP_OK) || stopWalk)
    {
      break;
    }

    bool allPastRange = true;

    for (size_t index = 0u; index < periodCount; index++)
    {
      if (periodCands[index] < (int64_t)to)
      {
        allPastRange = false;
        break;
      }
    }

    if (allPastRange || (nextCount == 0u))
    {
      break;
    }

    period = nextPeriod;
    periodCount = nextCount;
    periodCands[0] = nextCands[0];
    periodCands[1] = (nextCount > 1u) ? nextCands[1] : nextCands[0];

    if ((period.tm_year + 1900 > 2100) || (bucketsEmitted > LOGGER_TARIFF_BUCKET_MAX))
    {
      result = ESP_ERR_INVALID_STATE;
      break;
    }
  }

  xSemaphoreGive(tariffsMutex);
  return result;
}

//-- --------------------------------------------------------------------------
//-- Meter store tariff calculator
//-- --------------------------------------------------------------------------

//-- A meter that never published a telegram reports 0.0 counters; the jump
//-- to its real cumulative readings (and a meter replacement) is refused as
//-- consumption and only moves the baseline. One minute can never plausibly
//-- carry more than these amounts.
#define TARIFF_MAX_MINUTE_KWH 50.0f
#define TARIFF_MAX_MINUTE_M3 20.0f

//-- Accumulation state of one ring: the counters of the previous snapshot
//-- and the amounts earned inside the period named by ringKey. The amounts
//-- follow the direction of the energy: import (what the used energy costs),
//-- export (what the delivered-back energy earns) and gas. Only the store
//-- task drives the calculator, so no lock guards these entries (the price
//-- lookup below takes the component lock itself).
typedef struct
{
  bool haveState;
  uint64_t ringKey;
  float lastEdT1Kwh;
  float lastEdT2Kwh;
  float lastErT1Kwh;
  float lastErT2Kwh;
  float lastGasM3;
  float amountImportEuro;
  float amountExportEuro;
  float amountGasEuro;
} RingAmountState;

static RingAmountState ringAmounts[LOGGER_RING_COUNT];

//-- The portion of a cumulative counter that grew since the previous
//-- snapshot: nothing for a fall (meter replacement) and nothing for an
//-- implausible jump (a meter that never published a telegram reports 0.0
//-- until its first real telegram arrives).
static float counterDelta(float now, float previous, float maxPerSnapshot)
{
  if ((now <= previous) || ((now - previous) > maxPerSnapshot))
  {
    return 0.0f;
  }

  return now - previous;
}

void loggerTariffsMeterAmountCalculator(loggerMeterRing_t ring, uint64_t ringKey,
                                        uint8_t tariffIndicator, loggerMeterRecord* record)
{
  if ((record == NULL) || (ring >= LOGGER_RING_COUNT))
  {
    return;
  }

  loggerSettingsConfig settings;
  bool haveSettings = (loggerSettingsLoad(&settings) == ESP_OK);

  //-- The meter's tariff indicator (OBIS 0-0:96.14.0) names the tariff that
  //-- is active at the moment of the reading and picks which price of a
  //-- direction values this snapshot's energy. A meter that names no active
  //-- tariff (indicator 0) prices like tariff 1.
  const bool tariff2Active = (tariffIndicator == 2u);

  double priceImport = haveSettings ? (double)(tariff2Active ? settings.tariff2ImportPricePerKwh
                                                             : settings.tariff1ImportPricePerKwh)
                                    : 0.0;
  double priceExport = haveSettings ? (double)(tariff2Active ? settings.tariff2ExportPricePerKwh
                                                             : settings.tariff1ExportPricePerKwh)
                                    : 0.0;
  double priceGas = haveSettings ? (double)settings.tariffGasPricePerM3 : 0.0;

  //-- With dynamic tariffs on, the import and export prices the table holds
  //-- for this instant replace both manual prices of their direction (one
  //-- dynamic tariff always applies, there is no normal/off-peak
  //-- distinction) and the gas price the table holds prices the gas; minutes
  //-- the table does not cover — or prices it does not carry — keep the
  //-- manual prices.
  if (haveSettings && settings.dynamicTariffsEnabled)
  {
    loggerTariffInterval active;

    //-- Also false while the clock is not synchronised: no instant, no
    //-- dynamic price, so the manual prices stand.
    if (loggerTariffsActiveInterval(time(NULL), &active))
    {
      if (active.hasImport)
      {
        priceImport = (double)active.importPricePerKwh;
      }

      if (active.hasExport)
      {
        priceExport = (double)active.exportPricePerKwh;
      }

      if (active.hasGas)
      {
        priceGas = (double)active.gasPricePerM3;
      }
    }
  }

  RingAmountState* state = &ringAmounts[ring];

  if (!state->haveState || (state->ringKey != ringKey))
  {
    //-- A new period opened (or the first snapshot after a reboot): the
    //-- amounts restart together with the period.
    state->haveState = true;
    state->ringKey = ringKey;
    state->amountImportEuro = 0.0f;
    state->amountExportEuro = 0.0f;
    state->amountGasEuro = 0.0f;
    state->lastEdT1Kwh = record->energyDeliveredTariff1Kwh;
    state->lastEdT2Kwh = record->energyDeliveredTariff2Kwh;
    state->lastErT1Kwh = record->energyReturnedTariff1Kwh;
    state->lastErT2Kwh = record->energyReturnedTariff2Kwh;
    state->lastGasM3 = record->gasDeliveredM3;
  }

  //-- Both registers of a direction grew inside this snapshot's minute and
  //-- are valued as one amount: the tariff indicator, not the register a
  //-- reading landed in, names the tariff that prices the energy (meters
  //-- with a single register per direction publish everything in one).
  float deltaUsed =
      counterDelta(record->energyDeliveredTariff1Kwh, state->lastEdT1Kwh, TARIFF_MAX_MINUTE_KWH) +
      counterDelta(record->energyDeliveredTariff2Kwh, state->lastEdT2Kwh, TARIFF_MAX_MINUTE_KWH);
  float deltaReturned =
      counterDelta(record->energyReturnedTariff1Kwh, state->lastErT1Kwh, TARIFF_MAX_MINUTE_KWH) +
      counterDelta(record->energyReturnedTariff2Kwh, state->lastErT2Kwh, TARIFF_MAX_MINUTE_KWH);
  float deltaGas = counterDelta(record->gasDeliveredM3, state->lastGasM3, TARIFF_MAX_MINUTE_M3);

  state->lastEdT1Kwh = record->energyDeliveredTariff1Kwh;
  state->lastEdT2Kwh = record->energyDeliveredTariff2Kwh;
  state->lastErT1Kwh = record->energyReturnedTariff1Kwh;
  state->lastErT2Kwh = record->energyReturnedTariff2Kwh;
  state->lastGasM3 = record->gasDeliveredM3;

  state->amountImportEuro += (float)((double)deltaUsed * priceImport);
  state->amountExportEuro += (float)((double)deltaReturned * priceExport);
  state->amountGasEuro += (float)((double)deltaGas * priceGas);

  record->tariffAmountImportEuro = state->amountImportEuro;
  record->tariffAmountExportEuro = state->amountExportEuro;
  record->tariffAmountGasEuro = state->amountGasEuro;
}

//-- --------------------------------------------------------------------------
//-- MQTT topic and lifecycle
//-- --------------------------------------------------------------------------

void loggerTariffsEffectiveTopic(char* out, size_t outSize)
{
  if ((out == NULL) || (outSize == 0u))
  {
    return;
  }

  char topTopic[LOGGER_MQTT_TOPIC_MAX] = "";
  char clientId[LOGGER_MQTT_CLIENTID_MAX] = "";
  char subTopic[LOGGER_MQTT_TOPIC_MAX] = "";
  loggerSettingsConfig config;

  if (loggerSettingsLoad(&config) == ESP_OK)
  {
    snprintf(topTopic, sizeof topTopic, "%s", config.mqttTopTopic);
    snprintf(clientId, sizeof clientId, "%s", config.mqttClientId);
    snprintf(subTopic, sizeof subTopic, "%s", config.mqttSubTopic);
  }

  if (topTopic[0] == '\0')
  {
    snprintf(topTopic, sizeof topTopic, "%s", TARIFF_TOPIC_DEFAULT);
  }

  if (subTopic[0] == '\0')
  {
    snprintf(subTopic, sizeof subTopic, "%s", TARIFF_SUBTOPIC_DEFAULT);
  }

  //-- An empty client id leaves the device segment out: several loggers on
  //-- one broker separate themselves by naming a client id in Settings.
  if (clientId[0] != '\0')
  {
    snprintf(out, outSize, "%s/%s/%s", topTopic, clientId, subTopic);
  }
  else
  {
    snprintf(out, outSize, "%s/%s", topTopic, subTopic);
  }
}

esp_err_t loggerTariffsInit(void)
{
  if (tariffsMutex == NULL)
  {
    tariffsMutex = xSemaphoreCreateMutex();

    if (tariffsMutex == NULL)
    {
      return ESP_ERR_NO_MEM;
    }
  }

  if (!hooksInstalled)
  {
    //-- The tariff document is the project's only cJSON user: keep its
    //-- parse tree in PSRAM so a maximum-sized document cannot exhaust
    //-- internal RAM.
    memset(&cJSONHooks, 0, sizeof cJSONHooks);
    cJSONHooks.malloc_fn = psramAllocate;
    cJSONHooks.free_fn = tariffFree;
    cJSON_InitHooks(&cJSONHooks);
    hooksInstalled = true;
  }

  LOGGER_LOGI(tag, "dynamic tariffs ready (batch %u, stored %u, document %u bytes)",
              (unsigned)LOGGER_TARIFF_BATCH_MAX, (unsigned)LOGGER_TARIFF_STORED_MAX,
              (unsigned)LOGGER_TARIFF_JSON_MAX_BYTES);
  return ESP_OK;
}
