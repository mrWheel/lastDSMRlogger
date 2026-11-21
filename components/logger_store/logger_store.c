//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_store.c - hour, day and month smart meter ring file store
//-- Date: 2026-11-21 · Build: 10
//--
//-- The store task owns all ring_file_store work: it mounts the LittleFS
//-- storage partition, removes the ring file of the retired minute store,
//-- breaks any lock file left by an interrupted run, creates the three ring
//-- files on first boot and reopens the retained ones after that. All three
//-- are opened in buffered persistence with a one-record staging buffer.
//-- Every minute the task reads the five meter counters of the most
//-- recently accepted telegram (a reading no telegram published stays 0.0),
//-- adds the three fictional random-cent tariff amounts and upserts the
//-- same snapshot into the current hour, day and month record. An upsert of
//-- the same period updates the active record instead of failing, which
//-- keeps reboots and clock nudges harmless. At every hour change the task
//-- synchronizes all three files so the staged records reach storage.
//-- The console's populate command feeds historical periods in through
//-- loggerStorePopulateRing(): a semicolon separated csv file from the
//-- volume root (the import csv files are shipped there by the storage
//-- partition image built from the project's storageImage/ folder), keyed
//-- by period and columned by OBIS code, is edited into one ring with
//-- missing values becoming 0.0. A mode picks what happens to the records
//-- the ring already holds: merge (default) backfills, overwrite clears the
//-- ring before importing and update only revises stored periods. The
//-- console's ring2csv command writes one ring back to the volume root as
//-- a re-importable csv through loggerStoreRingToCsv().
//-- ==========================================================================

#include "logger_store.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_littlefs.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "logger_receiver.h"
#include "ring_file_store.h"

static const char* tag = "meterStore";

//-- Ring files on the LittleFS volume of the storage partition.
#define STORE_PARTITION_LABEL "storage"
#define STORE_MOUNT_POINT "/rfs"

//-- The ring file of the retired per-minute store; it has no reader
//-- anymore and is removed from the volume when it is still present.
#define STORE_LEGACY_RING_PATH STORE_MOUNT_POINT "/meter.rfs"

//-- Buffered persistence with a one-record staging buffer: the newest
//-- record of every ring lives in RAM until the staging buffer flushes
//-- (a new period opens, an hourly synchronization or a clean close).
#define STORE_PENDING_RECORDS 1u

//-- One snapshot per minute: it refreshes the current record of all three
//-- rings with the latest counters of the newest accepted telegram.
#define STORE_RECORD_INTERVAL_MS 60000u

//-- ring_file_store documents at least 8 KB of stack for any task driving
//-- the component; the extra headroom covers the console formatting too.
#define STORE_TASK_STACK_BYTES 10240
#define STORE_TASK_PRIORITY 3

//-- System-time floor for a usable wall clock; before the first SNTP sync
//-- the clock still sits at the 1970 epoch and cannot name a period.
#define STORE_CLOCK_SYNCED_EPOCH 1700000000

//-- The fictional tariff amounts are a random few cents: 0.01 .. 0.09 EUR.
#define STORE_TARIFF_CENT_MIN 1u
#define STORE_TARIFF_CENT_SPAN 9u

//-- --------------------------------------------------------------------------
//-- The three rings
//-- --------------------------------------------------------------------------

//-- One table entry per aggregation period. centuryModulus strips the
//-- component's century digits from the stored usedKey so callers see the
//-- plain wall-clock key ("YYMMDDhh", "YYMMDD" or "YYMM"). The handles are
//-- owned by the store task and only replaced at open time.
typedef struct
{
  const char* name;         //-- selector name used by console and SPA
  const char* path;         //-- ring file on the mounted volume
  const char* schemaId;     //-- ring_file_store file type
  uint32_t maxRecords;      //-- ring capacity
  uint64_t centuryModulus;  //-- 10 to the number of key digits
  ringFileHandle_t* handle; //-- NULL while the ring is not open
} MeterRing;

static MeterRing rings[LOGGER_RING_COUNT] = {
    {"hours", STORE_MOUNT_POINT "/hours.rfs", RING_FILE_SCHEMA_YYMMDDHH, 49u, 100000000ULL, NULL},
    {"days", STORE_MOUNT_POINT "/days.rfs", RING_FILE_SCHEMA_YYMMDD, 32u, 1000000ULL, NULL},
    {"months", STORE_MOUNT_POINT "/months.rfs", RING_FILE_SCHEMA_YYMM, 24u, 10000ULL, NULL},
};

const char* loggerStoreRingName(loggerMeterRing_t ring)
{
  if ((ring < LOGGER_RING_HOURS) || (ring >= LOGGER_RING_COUNT))
  {
    return "unknown";
  }

  return rings[ring].name;
}

bool loggerStoreRingFromName(const char* name, loggerMeterRing_t* outRing)
{
  if ((name == NULL) || (outRing == NULL) || (name[0] == '\0'))
  {
    return false;
  }

  for (loggerMeterRing_t ring = LOGGER_RING_HOURS; ring < LOGGER_RING_COUNT; ring++)
  {
    if (strcasecmp(name, rings[ring].name) == 0)
    {
      *outRing = ring;
      return true;
    }
  }

  //-- The console spelling for the monthly ring drops the plural s.
  if (strcasecmp(name, "month") == 0)
  {
    *outRing = LOGGER_RING_MONTHS;
    return true;
  }

  return false;
}

//-- --------------------------------------------------------------------------
//-- Reading the five meter counters out of the latest accepted telegram
//-- --------------------------------------------------------------------------

//-- OBIS codes of the wanted measurements. Gas lives on 0-1:24.2.1 in the
//-- Dutch DSMR profiles and on 0-1:24.2.3 in the Belgian eMUCs profile, so
//-- both are collected and the first one published wins.
#define STORE_WANTED_COUNT 6

static const char* const wantedObis[STORE_WANTED_COUNT] = {"1-0:1.8.1", "1-0:1.8.2",  "1-0:2.8.1",
                                                           "1-0:2.8.2", "0-1:24.2.1", "0-1:24.2.3"};

typedef struct
{
  float values[STORE_WANTED_COUNT];
  bool found[STORE_WANTED_COUNT];
} MeterFieldScan;

//-- Inspect callback: copies the first numeric value published per wanted
//-- OBIS code. Runs while the receive-side lock is held, so it only reads.
static bool collectMeterFields(const loggerTelegramRecord* record, void* userData)
{
  MeterFieldScan* scan = (MeterFieldScan*)userData;

  for (uint16_t index = 0u; index < record->meta.fieldCount; index++)
  {
    const loggerFieldSnapshot* field = &record->fields[index];

    if ((field->value[0] == '\0') || field->valueIsText)
    {
      continue;
    }

    for (uint8_t wanted = 0u; wanted < STORE_WANTED_COUNT; wanted++)
    {
      if (!scan->found[wanted] && (strcmp(field->obis, wantedObis[wanted]) == 0))
      {
        scan->values[wanted] = (float)atof(field->value);
        scan->found[wanted] = true;
      }
    }
  }

  return true;
}

//-- Fills the payload counters from the newest retained telegram. Counters
//-- the telegram did not publish (and the case of no telegram at all) stay
//-- 0.0; the return value only reports whether a telegram was inspected.
static bool readLatestMeterCounters(loggerMeterRecord* record)
{
  memset(record, 0, sizeof *record);

  loggerTelegramMeta meta;

  if (loggerReceiverListRecords(&meta, 1u) != 1u)
  {
    return false;
  }

  MeterFieldScan scan;
  memset(&scan, 0, sizeof scan);

  if (!loggerReceiverInspectRecord(meta.sequence, collectMeterFields, &scan))
  {
    return false;
  }

  record->energyDeliveredTariff1Kwh = scan.values[0];
  record->energyDeliveredTariff2Kwh = scan.values[1];
  record->energyReturnedTariff1Kwh = scan.values[2];
  record->energyReturnedTariff2Kwh = scan.values[3];

  //-- Gas: Dutch code first, Belgian eMUCs code as fallback.
  record->gasDeliveredM3 = scan.found[4] ? scan.values[4] : scan.values[5];

  return true;
}

//-- --------------------------------------------------------------------------
//-- Wall-clock period key
//-- --------------------------------------------------------------------------

//-- Builds the 14-digit EEYYMMDDhhmmss timestamp from the local wall clock
//-- as ring_file_store expects it. False while the clock has not been set
//-- by SNTP yet: a 1970 epoch period must not enter the rings.
static bool currentTimeStamp(uint64_t* outTimeStamp)
{
  time_t now = time(NULL);

  if (now < (time_t)STORE_CLOCK_SYNCED_EPOCH)
  {
    return false;
  }

  struct tm brokenTime;

  if (localtime_r(&now, &brokenTime) == NULL)
  {
    return false;
  }

  //-- Built as digits (century fixed to 20), not through snprintf: ring
  //-- file timestamps are numeric and any out-of-range digit is refused by
  //-- the component with ESP_ERR_INVALID_ARG at the upsert.
  uint64_t year2 = (uint64_t)((brokenTime.tm_year + 1900) % 100);

  if ((int64_t)year2 < 0)
  {
    year2 += 100u;
  }

  uint64_t timeStamp = (20ULL * 100ULL) + year2;
  timeStamp = (timeStamp * 100ULL) + (uint64_t)(brokenTime.tm_mon + 1);
  timeStamp = (timeStamp * 100ULL) + (uint64_t)brokenTime.tm_mday;
  timeStamp = (timeStamp * 100ULL) + (uint64_t)brokenTime.tm_hour;
  timeStamp = (timeStamp * 100ULL) + (uint64_t)brokenTime.tm_min;
  timeStamp = (timeStamp * 100ULL) + (uint64_t)brokenTime.tm_sec;

  *outTimeStamp = timeStamp;
  return true;
}

//-- Wall-clock key of one ring for a timestamp: the usedKey the component
//-- derives from the timestamp for the ring's file type, with the century
//-- digits stripped ("YYMMDDhh", "YYMMDD" or "YYMM"). False when the
//-- timestamp carries digits the file type refuses.
static bool meterRingKey(const MeterRing* ring, uint64_t timeStamp, uint64_t* outKey)
{
  ringFileGranularity_t granularity = RING_FILE_GRANULARITY_NONE;

  if (ringFileGranularityFromSchemaId(ring->schemaId, &granularity) != ESP_OK)
  {
    return false;
  }

  uint64_t usedKey = 0u;

  if (ringFileTimeStampToUsedKey(timeStamp, granularity, &usedKey) != ESP_OK)
  {
    return false;
  }

  *outKey = usedKey % ring->centuryModulus;
  return true;
}

//-- --------------------------------------------------------------------------
//-- Ring file life cycle
//-- --------------------------------------------------------------------------

//-- Opens one ring read-write: created empty on first boot, reopened with
//-- its retained records after that, always buffered with a one-record
//-- staging buffer. False when the ring could not be opened.
static bool openMeterRing(MeterRing* ring)
{
  //-- Right after a reset no live handle can exist, so a lock file left by
  //-- a run that never reached ringFileClose() is stale and must go.
  if (ringFileBreakStaleLock(ring->path) == ESP_OK)
  {
    ESP_LOGI(tag, "removed a stale %s ring lock from an interrupted run", ring->name);
  }

  ringFileConfig_t config;
  memset(&config, 0, sizeof config);
  config.maxRecords = ring->maxRecords;
  config.schemaId = ring->schemaId;
  config.recordSize = sizeof(loggerMeterRecord);
  config.persistMode = RING_FILE_PERSIST_BUFFERED;
  config.maxPendingRecords = STORE_PENDING_RECORDS;

  ringFileHandle_t* handle = NULL;
  esp_err_t result = ringFileCreate(ring->path, &config, &handle);

  if (result == RING_FILE_STORE_ERR_ALREADY_EXISTS)
  {
    ringFileOpenConfig_t openConfig;
    memset(&openConfig, 0, sizeof openConfig);
    openConfig.expectedSchemaId = ring->schemaId;
    openConfig.expectedRecordSize = sizeof(loggerMeterRecord);
    openConfig.readOnly = false;
    openConfig.persistMode = RING_FILE_PERSIST_BUFFERED;
    openConfig.maxPendingRecords = STORE_PENDING_RECORDS;

    result = ringFileOpen(ring->path, &openConfig, &handle);
  }

  if (result != ESP_OK)
  {
    ESP_LOGE(tag, "could not open ring file %s: %s", ring->path, esp_err_to_name(result));
    return false;
  }

  ringFileInfo_t info;
  memset(&info, 0, sizeof info);

  if (ringFileGetInfo(handle, &info) == ESP_OK)
  {
    ESP_LOGI(tag, "ring file %s ready: file type '%s', %" PRIu32 " of %" PRIu32 " records retained",
             ring->path, info.schemaId, info.count, info.capacity);
  }

  ring->handle = handle;
  return true;
}

//-- Mounts LittleFS, clears the retired minute ring file and opens the
//-- three meter rings. False when the volume or any ring could not be
//-- opened; already opened rings are closed again so a retry starts clean.
static bool openMeterRings(void)
{
  esp_vfs_littlefs_conf_t mountConfig;
  memset(&mountConfig, 0, sizeof mountConfig);
  mountConfig.base_path = STORE_MOUNT_POINT;
  mountConfig.partition_label = STORE_PARTITION_LABEL;
  mountConfig.format_if_mount_failed = true;

  esp_err_t result = esp_vfs_littlefs_register(&mountConfig);

  if (result != ESP_OK)
  {
    ESP_LOGE(tag, "could not mount LittleFS partition '%s' at %s: %s", STORE_PARTITION_LABEL,
             STORE_MOUNT_POINT, esp_err_to_name(result));
    return false;
  }

  ESP_LOGI(tag, "mounted LittleFS partition '%s' at %s", STORE_PARTITION_LABEL, STORE_MOUNT_POINT);

  //-- The temporary per-minute store is retired: its ring file and a lock
  //-- it may have left behind no longer belong on the volume.
  (void)ringFileBreakStaleLock(STORE_LEGACY_RING_PATH);

  if (unlink(STORE_LEGACY_RING_PATH) == 0)
  {
    ESP_LOGI(tag, "removed the retired minute ring file %s", STORE_LEGACY_RING_PATH);
  }

  for (loggerMeterRing_t ring = LOGGER_RING_HOURS; ring < LOGGER_RING_COUNT; ring++)
  {
    if (!openMeterRing(&rings[ring]))
    {
      for (loggerMeterRing_t opened = LOGGER_RING_HOURS; opened < ring; opened++)
      {
        ringFileClose(rings[opened].handle);
        rings[opened].handle = NULL;
      }

      esp_vfs_littlefs_unregister(STORE_PARTITION_LABEL);
      return false;
    }
  }

  return true;
}

//-- --------------------------------------------------------------------------
//-- Newest-first list for the console and the SPA
//-- --------------------------------------------------------------------------

//-- Full stored record as ring_file_store returns it: the two automatic key
//-- fields followed by the caller payload.
typedef struct
{
  ringFileKeyFields_t keys;
  loggerMeterRecord meter;
} MeterRingEntry;

//-- Walks one ring from the newest retained record to the oldest, handing
//-- every record to the visitor on the caller's task. A concurrent snapshot
//-- upsert invalidates the cursor: the walk then begins again from the
//-- newest and skips every key already emitted (keys strictly decrease as
//-- the walk ages), so the list stays complete and duplicate-free while the
//-- store task keeps writing.
esp_err_t loggerStoreListRecords(loggerMeterRing_t ring, loggerStoreRecordVisitor visitor,
                                 void* userData)
{
  if ((ring < LOGGER_RING_HOURS) || (ring >= LOGGER_RING_COUNT))
  {
    return ESP_ERR_INVALID_ARG;
  }

  MeterRing* active = &rings[ring];
  ringFileHandle_t* handle = active->handle;

  if (handle == NULL)
  {
    return ESP_ERR_INVALID_STATE;
  }

  MeterRingEntry entry;
  ringFileIterator_t iterator;
  memset(&entry, 0, sizeof entry);
  memset(&iterator, 0, sizeof iterator);

  uint32_t emitted = 0u;
  uint64_t lastEmittedKey = 0u;
  esp_err_t result = ringFileIteratorBegin(handle, &iterator, RING_FILE_ITER_NEWEST, NULL, &entry);

  while (result == ESP_OK)
  {
    if ((emitted == 0u) || (entry.keys.usedKey < lastEmittedKey))
    {
      lastEmittedKey = entry.keys.usedKey;
      emitted++;

      //-- The usedKey carries the component's century digits: hand the
      //-- visitor the plain wall-clock period key of this ring.
      if (!visitor(entry.keys.usedKey % active->centuryModulus, &entry.meter, userData))
      {
        return ESP_OK;
      }
    }

    if (emitted > (active->maxRecords * 2u))
    {
      //-- Safety net against a pathological invalidate/re-begin cycle.
      ESP_LOGW(tag, "%s ring list walk abandoned after %" PRIu32 " entries", active->name, emitted);
      return ESP_FAIL;
    }

    //-- A full ring walk is flash-read heavy: the component inspects every
    //-- slot per step. Hand the cpu to the idle task after each record so
    //-- the task watchdog keeps its reset while a console or web listing
    //-- walks all buckets.
    vTaskDelay(1);

    result = ringFileIteratorPrevious(handle, &iterator, &entry);

    if (result == RING_FILE_STORE_ERR_ITER_INVALIDATED)
    {
      result = ringFileIteratorBegin(handle, &iterator, RING_FILE_ITER_NEWEST, NULL, &entry);
    }
  }

  if ((result != RING_FILE_STORE_ERR_ITER_END) && (result != RING_FILE_STORE_ERR_RING_EMPTY))
  {
    ESP_LOGW(tag, "%s ring list walk stopped after %" PRIu32 " records: %s", active->name, emitted,
             esp_err_to_name(result));
    return result;
  }

  return ESP_OK;
}

//-- --------------------------------------------------------------------------
//-- Populate import
//-- --------------------------------------------------------------------------

//-- Import limits: one hand-edited csv line stays far below these bounds;
//-- a longer line is reported as rejected instead of half-imported.
#define STORE_IMPORT_LINE_BYTES 256
#define STORE_IMPORT_MAX_COLUMNS 40
#define STORE_IMPORT_PATH_BYTES 320

//-- The payload field a value column feeds.
typedef enum
{
  IMPORT_FIELD_NONE = -1,
  IMPORT_FIELD_DELIVERED_1 = 0,
  IMPORT_FIELD_DELIVERED_2,
  IMPORT_FIELD_RETURNED_1,
  IMPORT_FIELD_RETURNED_2,
  IMPORT_FIELD_GAS,
  IMPORT_FIELD_TARIFF_1,
  IMPORT_FIELD_TARIFF_2,
  IMPORT_FIELD_TARIFF_GAS
} ImportField;

//-- Header column name to payload field. The counters are named by their
//-- OBIS code (without method suffix); the gas counter is accepted under the
//-- catalogue code 0-1:24.1.0 and under the Dutch and Belgian publishing
//-- codes alike. The three tariff amounts have no OBIS code of their own and
//-- carry the same rubrics the ring print uses: t1, t2 and tg.
static const struct
{
  const char* obis;
  ImportField field;
} importObisMap[] = {
    {"1-0:1.8.1", IMPORT_FIELD_DELIVERED_1}, {"1-0:1.8.2", IMPORT_FIELD_DELIVERED_2},
    {"1-0:2.8.1", IMPORT_FIELD_RETURNED_1},  {"1-0:2.8.2", IMPORT_FIELD_RETURNED_2},
    {"0-1:24.1.0", IMPORT_FIELD_GAS},        {"0-1:24.2.1", IMPORT_FIELD_GAS},
    {"0-1:24.2.3", IMPORT_FIELD_GAS},        {"t1", IMPORT_FIELD_TARIFF_1},
    {"t2", IMPORT_FIELD_TARIFF_2},           {"tg", IMPORT_FIELD_TARIFF_GAS},
};

//-- Period key column spellings the header line may start with; the data
//-- line keys may then carry 8, 6 or 4 digits.
static const char* const importKeyNames[] = {"YYMMDDHH", "YYMMDD", "YYMM"};

//-- Trims leading and trailing blanks and tabs (and a surviving line
//-- ending) in place; returns the new start of the text.
static char* trimText(char* text)
{
  while ((text[0] == ' ') || (text[0] == '\t'))
  {
    text++;
  }

  size_t length = strlen(text);

  while ((length > 0u) && ((text[length - 1u] == ' ') || (text[length - 1u] == '\t') ||
                           (text[length - 1u] == '\r') || (text[length - 1u] == '\n')))
  {
    text[--length] = '\0';
  }

  return text;
}

//-- Splits a line on ';' into at most STORE_IMPORT_MAX_COLUMNS fields (the
//-- separators become terminators). Empty fields are kept: a missing value
//-- is a blank field, not a missing separator. Columns beyond the maximum
//-- are consumed but not captured. Returns the captured field count.
static uint8_t splitCsvLine(char* line, char** fields, uint8_t maxFields)
{
  uint8_t count = 1u;
  fields[0] = line;
  char* separator = strchr(line, ';');

  while (separator != NULL)
  {
    *separator++ = '\0';

    if (count < maxFields)
    {
      fields[count++] = separator;
    }

    separator = strchr(separator, ';');
  }

  return count;
}

//-- Builds the 14-digit ring timestamp from a period key text: 8 digits
//-- "YYMMDDhh", 6 digits "YYMMDD" (hour 00) or 4 digits "YYMM" (day 01,
//-- hour 00); minutes and seconds are always zero padding. False for any
//-- other spelling, non-digits or plainly out-of-range date parts (the
//-- component refuses such timestamps at the edit anyway).
static bool importKeyToTimeStamp(const char* keyText, uint64_t* outTimeStamp)
{
  size_t length = strlen(keyText);

  if ((length != 4u) && (length != 6u) && (length != 8u))
  {
    return false;
  }

  for (size_t index = 0u; index < length; index++)
  {
    if (!isdigit((unsigned char)keyText[index]))
    {
      return false;
    }
  }

  uint64_t digits = strtoull(keyText, NULL, 10);
  unsigned hour = 0u;
  unsigned day = 1u;

  if (length == 8u)
  {
    hour = (unsigned)(digits % 100u);
    digits /= 100u;
  }

  if (length >= 6u)
  {
    day = (unsigned)(digits % 100u);
    digits /= 100u;
  }

  unsigned month = (unsigned)(digits % 100u);
  unsigned year2 = (unsigned)(digits / 100u);

  if ((month < 1u) || (month > 12u) || (day < 1u) || (day > 31u) || (hour > 23u))
  {
    return false;
  }

  uint64_t timeStamp = 20u;
  timeStamp = (timeStamp * 100u) + year2;
  timeStamp = (timeStamp * 100u) + month;
  timeStamp = (timeStamp * 100u) + day;
  timeStamp = (timeStamp * 100u) + hour;
  timeStamp = timeStamp * 10000u; //-- zero-padded minutes and seconds

  *outTimeStamp = timeStamp;
  return true;
}

//-- Maps one trimmed header column name to the payload field it feeds. An
//-- "*255" method suffix is cut off before the comparison; unknown names -
//-- including the export files' "#" and empty trailing columns - feed
//-- nothing.
static ImportField importFieldForColumn(char* columnName)
{
  char* methodSuffix = strchr(columnName, '*');

  if (methodSuffix != NULL)
  {
    *methodSuffix = '\0';
  }

  for (size_t index = 0u; index < (sizeof importObisMap / sizeof importObisMap[0]); index++)
  {
    if (strcasecmp(columnName, importObisMap[index].obis) == 0)
    {
      return importObisMap[index].field;
    }
  }

  return IMPORT_FIELD_NONE;
}

//-- True when the trimmed header text names one of the accepted period key
//-- columns.
static bool isImportKeyName(const char* text)
{
  for (size_t index = 0u; index < (sizeof importKeyNames / sizeof importKeyNames[0]); index++)
  {
    if (strcasecmp(text, importKeyNames[index]) == 0)
    {
      return true;
    }
  }

  return false;
}

//-- Readable name for the ring_file_store result codes (esp_err_to_name
//-- knows only the standard ESP codes, the custom ones come out "ERROR").
static const char* importErrName(esp_err_t err)
{
  switch (err)
  {
  case RING_FILE_STORE_ERR_BUCKET_OUT_OF_RANGE:
    return "outside the retained window";
  case RING_FILE_STORE_ERR_KEY_NOT_FOUND:
    return "the period is not stored";
  case RING_FILE_STORE_ERR_NOT_NEWER:
    return "not newer than the newest bucket";
  case RING_FILE_STORE_ERR_READONLY:
    return "the ring is open read-only";
  case RING_FILE_STORE_ERR_IO:
  case RING_FILE_STORE_ERR_IO_FAULTED:
    return "storage I/O failure";
  case RING_FILE_STORE_ERR_CORRUPT:
    return "the ring file is corrupt";
  case RING_FILE_STORE_ERR_STORAGE_FULL:
    return "the volume is full";
  default:
    return esp_err_to_name(err);
  }
}

//-- Work item of one import; the calling task waits on done.
typedef struct
{
  loggerMeterRing_t ring;
  const char* relativePath;
  loggerPopulateMode_t mode;
  loggerStoreImportResult* result;
  SemaphoreHandle_t done;
  esp_err_t outcome;
} ImportJob;

//-- Readable name of a populate mode (log and console lines).
static const char* populateModeName(loggerPopulateMode_t mode)
{
  switch (mode)
  {
  case LOGGER_POPULATE_OVERWRITE:
    return "overwrite";
  case LOGGER_POPULATE_UPDATE:
    return "update";
  default:
    return "merge";
  }
}

//-- The import itself, executed on the dedicated import task with the
//-- arguments validated by the public wrapper.
static esp_err_t importRingFile(loggerMeterRing_t ring, const char* relativePath,
                                loggerPopulateMode_t mode, loggerStoreImportResult* result)
{
  char path[STORE_IMPORT_PATH_BYTES];
  snprintf(path, sizeof path, "%s/%s", STORE_MOUNT_POINT, relativePath);

  FILE* file = fopen(path, "r");

  if (file == NULL)
  {
    ESP_LOGW(tag, "populate could not open %s: errno %d", path, errno);
    snprintf(result->firstError, sizeof result->firstError, "cannot open '%.60s' (errno %d)",
             relativePath, errno);
    return ESP_ERR_NOT_FOUND;
  }

  ImportField fieldOfColumn[STORE_IMPORT_MAX_COLUMNS];

  //-- 0xFF fills every enum-sized entry with -1 (IMPORT_FIELD_NONE).
  memset(fieldOfColumn, 0xFF, sizeof fieldOfColumn);

  bool headerRead = false;
  bool ringCleared = false;
  esp_err_t outcome = ESP_OK;
  char line[STORE_IMPORT_LINE_BYTES];

  while (fgets(line, sizeof line, file) != NULL)
  {
    //-- A line that did not fit the reader is malformed: its tail is
    //-- dropped and the line is rejected instead of half-imported.
    size_t lineLength = strlen(line);
    bool truncated = (lineLength == (sizeof line - 1u)) && (line[lineLength - 1u] != '\n');

    if (truncated)
    {
      int dropped = 0;

      while (((dropped = fgetc(file)) != EOF) && (dropped != '\n'))
      {
      }
    }

    char copy[STORE_IMPORT_LINE_BYTES];
    snprintf(copy, sizeof copy, "%s", line);
    char* text = trimText(copy);

    if (text[0] == '\0')
    {
      continue;
    }

    char* fields[STORE_IMPORT_MAX_COLUMNS];
    uint8_t columnCount = splitCsvLine(text, fields, STORE_IMPORT_MAX_COLUMNS);

    //-- Keep the pointer trimText returns: it also skips leading blanks.
    for (uint8_t index = 0u; index < columnCount; index++)
    {
      fields[index] = trimText(fields[index]);
    }

    if (!headerRead)
    {
      headerRead = true;

      if (!isImportKeyName(fields[0]))
      {
        outcome = ESP_ERR_INVALID_ARG;
        snprintf(result->firstError, sizeof result->firstError,
                 "header must start with YYMMDDHH, YYMMDD or YYMM (found '%.24s')", fields[0]);
        break;
      }

      for (uint8_t index = 1u; index < columnCount; index++)
      {
        fieldOfColumn[index] = importFieldForColumn(fields[index]);
      }

      continue;
    }

    result->rowsRead++;

    uint64_t timeStamp = 0u;

    if (truncated || !importKeyToTimeStamp(fields[0], &timeStamp))
    {
      result->rowsRejected++;

      if (result->firstError[0] == '\0')
      {
        snprintf(result->firstError, sizeof result->firstError, "line %lu rejected: no usable key",
                 (unsigned long)result->rowsRead);
      }

      continue;
    }

    loggerMeterRecord record;
    memset(&record, 0, sizeof record);

    for (uint8_t index = 1u; index < columnCount; index++)
    {
      switch (fieldOfColumn[index])
      {
      case IMPORT_FIELD_DELIVERED_1:
        record.energyDeliveredTariff1Kwh = (float)atof(fields[index]);
        break;
      case IMPORT_FIELD_DELIVERED_2:
        record.energyDeliveredTariff2Kwh = (float)atof(fields[index]);
        break;
      case IMPORT_FIELD_RETURNED_1:
        record.energyReturnedTariff1Kwh = (float)atof(fields[index]);
        break;
      case IMPORT_FIELD_RETURNED_2:
        record.energyReturnedTariff2Kwh = (float)atof(fields[index]);
        break;
      case IMPORT_FIELD_GAS:
        record.gasDeliveredM3 = (float)atof(fields[index]);
        break;
      case IMPORT_FIELD_TARIFF_1:
        record.tariffAmount1Euro = (float)atof(fields[index]);
        break;
      case IMPORT_FIELD_TARIFF_2:
        record.tariffAmount2Euro = (float)atof(fields[index]);
        break;
      case IMPORT_FIELD_TARIFF_GAS:
        record.tariffAmountGasEuro = (float)atof(fields[index]);
        break;
      default:
        break;
      }
    }

    esp_err_t edited;

    if (mode == LOGGER_POPULATE_UPDATE)
    {
      //-- Update mode never adds a period: only keys the ring already
      //-- stores get their values revised, everything else is refused.
      uint64_t usedKey = 0u;
      ringFileGranularity_t granularity = RING_FILE_GRANULARITY_NONE;
      MeterRingEntry stored;

      if ((ringFileGranularityFromSchemaId(rings[ring].schemaId, &granularity) != ESP_OK) ||
          (ringFileTimeStampToUsedKey(timeStamp, granularity, &usedKey) != ESP_OK))
      {
        edited = ESP_ERR_INVALID_ARG;
      }
      else
      {
        edited = ringFileReadByKey(rings[ring].handle, usedKey, &stored);

        if (edited == ESP_OK)
        {
          edited = ringFileEdit(rings[ring].handle, timeStamp, &record);
        }
      }
    }
    else
    {
      //-- Overwrite mode empties the ring just before the first valid
      //-- data line, so an import that never reaches one leaves the ring
      //-- untouched. An empty ring accepts any bucket, which lets the whole
      //-- file land without retained-window refusals.
      if ((mode == LOGGER_POPULATE_OVERWRITE) && !ringCleared)
      {
        ringCleared = true;
        esp_err_t cleared = ringFileClear(rings[ring].handle);

        if (cleared != ESP_OK)
        {
          outcome = cleared;
          snprintf(result->firstError, sizeof result->firstError, "could not clear the ring: %s",
                   importErrName(cleared));
          break;
        }
      }

      //-- ringFileEdit backfills historical periods (ringFileAdd would refuse
      //-- anything older than the newest stored bucket) and keeps its
      //-- position when the period is already active.
      edited = ringFileEdit(rings[ring].handle, timeStamp, &record);
    }

    if (edited == ESP_OK)
    {
      result->bucketsWritten++;
    }
    else
    {
      result->bucketsRefused++;

      if (result->firstError[0] == '\0')
      {
        snprintf(result->firstError, sizeof result->firstError, "key %.20s refused: %s", fields[0],
                 importErrName(edited));
      }
    }
  }

  fclose(file);

  if (headerRead && (outcome == ESP_OK))
  {
    esp_err_t synced = ringFileSync(rings[ring].handle);

    if (synced != ESP_OK)
    {
      outcome = synced;
      snprintf(result->firstError, sizeof result->firstError, "could not synchronize: %s",
               importErrName(synced));
    }
  }

  if (!headerRead && (outcome == ESP_OK))
  {
    outcome = ESP_ERR_INVALID_ARG;
    snprintf(result->firstError, sizeof result->firstError, "file has no header line");
  }

  ESP_LOGI(
      tag,
      "populate %s ring from %s (%s): %lu line(s) read, %lu written, %lu rejected, %lu refused",
      rings[ring].name, path, populateModeName(mode), (unsigned long)result->rowsRead,
      (unsigned long)result->bucketsWritten, (unsigned long)result->rowsRejected,
      (unsigned long)result->bucketsRefused);

  return outcome;
}

//-- The import drives ring_file_store (LittleFS writes included), and the
//-- component asks every driving task for at least 8 KB of stack on top of
//-- its own locals: the console task (8 KB total) overflowed when it ran
//-- the import itself, so a one-shot task of its own does the work.
#define STORE_IMPORT_TASK_STACK_BYTES 10240
#define STORE_IMPORT_TASK_PRIORITY 4

static void importRingTask(void* argument)
{
  ImportJob* job = (ImportJob*)argument;

  job->outcome = importRingFile(job->ring, job->relativePath, job->mode, job->result);
  xSemaphoreGive(job->done);
  vTaskDelete(NULL);
}

esp_err_t loggerStorePopulateRing(loggerMeterRing_t ring, const char* relativePath,
                                  loggerPopulateMode_t mode, loggerStoreImportResult* result)
{
  if ((ring < LOGGER_RING_HOURS) || (ring >= LOGGER_RING_COUNT) || (relativePath == NULL) ||
      (relativePath[0] == '\0') || (result == NULL) || (mode < LOGGER_POPULATE_MERGE) ||
      (mode >= LOGGER_POPULATE_COUNT))
  {
    return ESP_ERR_INVALID_ARG;
  }

  memset(result, 0, sizeof *result);

  if (rings[ring].handle == NULL)
  {
    return ESP_ERR_INVALID_STATE;
  }

  //-- The import never leaves the mounted volume: no absolute path and no
  //-- parent traversal.
  if ((relativePath[0] == '/') || (strstr(relativePath, "..") != NULL))
  {
    return ESP_ERR_INVALID_ARG;
  }

  SemaphoreHandle_t done = xSemaphoreCreateBinary();

  if (done == NULL)
  {
    return ESP_ERR_NO_MEM;
  }

  ImportJob job;
  job.ring = ring;
  job.relativePath = relativePath;
  job.mode = mode;
  job.result = result;
  job.done = done;
  job.outcome = ESP_FAIL;

  if (xTaskCreate(importRingTask, "ringImport", STORE_IMPORT_TASK_STACK_BYTES, &job,
                  STORE_IMPORT_TASK_PRIORITY, NULL) != pdPASS)
  {
    vSemaphoreDelete(done);
    ESP_LOGE(tag, "could not create the ring import task");
    return ESP_ERR_NO_MEM;
  }

  //-- One small csv lands in milliseconds; the wait only ever lasts as long
  //-- as the LittleFS writes of the imported periods take.
  xSemaphoreTake(done, portMAX_DELAY);
  vSemaphoreDelete(done);

  return job.outcome;
}

//-- --------------------------------------------------------------------------
//-- Csv export (ring2csv)
//-- --------------------------------------------------------------------------

//-- The export walks and writes through ring_file_store and LittleFS just
//-- like the import, so it runs on a one-shot task with the same stack.
#define STORE_EXPORT_TASK_STACK_BYTES 10240
#define STORE_EXPORT_TASK_PRIORITY 4

//-- The retained window is bounded by the largest ring capacity (hours);
//-- the walk can never commit more records than that.
#define STORE_EXPORT_MAX_RECORDS 64u

//-- Header of an exported file: the period key column, the five counter
//-- columns in the spelling the populate import maps back (the 0-1:24.1.0
//-- gas code is accepted for all published gas codes alike) and the three
//-- tariff amounts under the rubrics the ring print uses (t1, t2 and tg).
#define STORE_EXPORT_HEADER                                                                        \
  "YYMMDDHH;1-0:1.8.1*255;1-0:1.8.2*255;1-0:2.8.1*255;1-0:2.8.2*255;0-1:24.1.0*255;t1;t2;tg\r\n"

//-- Walks the ring newest first into a buffer (deduplicating a cursor that
//-- is invalidated by a concurrent snapshot), then writes the file oldest
//-- first: key as eight YYMMDDHH digits, the five counters at three decimals
//-- and the three tariff amounts at two. An empty ring still produces the
//-- header line.
static esp_err_t exportRingFile(loggerMeterRing_t ring, uint32_t* outRecords)
{
  MeterRing* active = &rings[ring];
  ringFileHandle_t* handle = active->handle;

  if (handle == NULL)
  {
    return ESP_ERR_INVALID_STATE;
  }

  MeterRingEntry entries[STORE_EXPORT_MAX_RECORDS];
  uint32_t collected = 0u;
  uint64_t lastEmittedKey = 0u;
  uint32_t steps = 0u;
  MeterRingEntry entry;
  ringFileIterator_t iterator;
  memset(&entry, 0, sizeof entry);
  memset(&iterator, 0, sizeof iterator);

  esp_err_t result = ringFileIteratorBegin(handle, &iterator, RING_FILE_ITER_NEWEST, NULL, &entry);

  while (result == ESP_OK)
  {
    if ((collected == 0u) || (entry.keys.usedKey < lastEmittedKey))
    {
      lastEmittedKey = entry.keys.usedKey;

      if (collected < STORE_EXPORT_MAX_RECORDS)
      {
        entries[collected++] = entry;
      }
    }

    steps++;

    if (steps > (active->maxRecords * 3u))
    {
      ESP_LOGW(tag, "%s ring export walk abandoned after %" PRIu32 " steps", active->name, steps);
      return ESP_FAIL;
    }

    //-- Full ring walks are flash-read heavy: hand the cpu to the idle task
    //-- after each record so the task watchdog stays quiet (as in the list
    //-- walk).
    vTaskDelay(1);

    result = ringFileIteratorPrevious(handle, &iterator, &entry);

    if (result == RING_FILE_STORE_ERR_ITER_INVALIDATED)
    {
      //-- A concurrent snapshot moved the cursor: collect the whole ring
      //-- again from the newest.
      collected = 0u;
      lastEmittedKey = 0u;
      result = ringFileIteratorBegin(handle, &iterator, RING_FILE_ITER_NEWEST, NULL, &entry);
    }
  }

  if ((result != RING_FILE_STORE_ERR_ITER_END) && (result != RING_FILE_STORE_ERR_RING_EMPTY))
  {
    ESP_LOGW(tag, "%s ring export walk stopped after %" PRIu32 " records: %s", active->name,
             collected, esp_err_to_name(result));
    return result;
  }

  char path[STORE_IMPORT_PATH_BYTES];
  snprintf(path, sizeof path, STORE_MOUNT_POINT "/RING%s.csv", active->name);

  FILE* file = fopen(path, "w");

  if (file == NULL)
  {
    ESP_LOGW(tag, "ring2csv could not open %s: errno %d", path, errno);
    return ESP_FAIL;
  }

  fprintf(file, STORE_EXPORT_HEADER);

  //-- Newest first in the buffer, oldest first on storage.
  for (uint32_t index = collected; index > 0u;)
  {
    index--;
    const MeterRingEntry* record = &entries[index];

    //-- Eight plain YYMMDDHH digits: strip the component's minute/second
    //-- digits and the century from the full timestamp.
    uint64_t key = (record->keys.timeStamp / 10000ULL) % 100000000ULL;

    fprintf(file, "%08" PRIu64 ";%.3f;%.3f;%.3f;%.3f;%.3f;%.2f;%.2f;%.2f\r\n", key,
            record->meter.energyDeliveredTariff1Kwh, record->meter.energyDeliveredTariff2Kwh,
            record->meter.energyReturnedTariff1Kwh, record->meter.energyReturnedTariff2Kwh,
            record->meter.gasDeliveredM3, record->meter.tariffAmount1Euro,
            record->meter.tariffAmount2Euro, record->meter.tariffAmountGasEuro);
  }

  fclose(file);

  if (outRecords != NULL)
  {
    *outRecords = collected;
  }

  ESP_LOGI(tag, "ring2csv wrote %" PRIu32 " record(s) of the %s ring to %s", collected,
           active->name, path);

  return ESP_OK;
}

typedef struct
{
  loggerMeterRing_t ring;
  uint32_t* outRecords;
  SemaphoreHandle_t done;
  esp_err_t outcome;
} ExportJob;

static void exportRingTask(void* argument)
{
  ExportJob* job = (ExportJob*)argument;

  job->outcome = exportRingFile(job->ring, job->outRecords);
  xSemaphoreGive(job->done);
  vTaskDelete(NULL);
}

esp_err_t loggerStoreRingToCsv(loggerMeterRing_t ring, uint32_t* outRecords)
{
  if ((ring < LOGGER_RING_HOURS) || (ring >= LOGGER_RING_COUNT))
  {
    return ESP_ERR_INVALID_ARG;
  }

  if (outRecords != NULL)
  {
    *outRecords = 0u;
  }

  if (rings[ring].handle == NULL)
  {
    return ESP_ERR_INVALID_STATE;
  }

  SemaphoreHandle_t done = xSemaphoreCreateBinary();

  if (done == NULL)
  {
    return ESP_ERR_NO_MEM;
  }

  ExportJob job;
  job.ring = ring;
  job.outRecords = outRecords;
  job.done = done;
  job.outcome = ESP_FAIL;

  if (xTaskCreate(exportRingTask, "ringExport", STORE_EXPORT_TASK_STACK_BYTES, &job,
                  STORE_EXPORT_TASK_PRIORITY, NULL) != pdPASS)
  {
    vSemaphoreDelete(done);
    ESP_LOGE(tag, "could not create the ring export task");
    return ESP_ERR_NO_MEM;
  }

  xSemaphoreTake(done, portMAX_DELAY);
  vSemaphoreDelete(done);

  return job.outcome;
}

//-- --------------------------------------------------------------------------
//-- Store task
//-- --------------------------------------------------------------------------

//-- Reads the latest counters once and upserts the same snapshot into the
//-- current record of all three rings. In buffered mode the records stay in
//-- the one-record staging buffers until a new period opens or the hourly
//-- synchronization commits them. Reports the wall-clock timestamp it used.
static bool storeSnapshot(uint64_t* outTimeStamp)
{
  uint64_t timeStamp = 0u;

  if (!currentTimeStamp(&timeStamp))
  {
    ESP_LOGI(tag, "wall clock not synchronised yet, ring snapshot postponed");
    return false;
  }

  loggerMeterRecord record;
  bool telegramSeen = readLatestMeterCounters(&record);

  //-- The fictional tariff amounts: a random few cents per tariff.
  record.tariffAmount1Euro =
      (float)(STORE_TARIFF_CENT_MIN + (esp_random() % STORE_TARIFF_CENT_SPAN)) / 100.0f;
  record.tariffAmount2Euro =
      (float)(STORE_TARIFF_CENT_MIN + (esp_random() % STORE_TARIFF_CENT_SPAN)) / 100.0f;
  record.tariffAmountGasEuro =
      (float)(STORE_TARIFF_CENT_MIN + (esp_random() % STORE_TARIFF_CENT_SPAN)) / 100.0f;

  for (loggerMeterRing_t ring = LOGGER_RING_HOURS; ring < LOGGER_RING_COUNT; ring++)
  {
    esp_err_t result = ringFileUpsert(rings[ring].handle, timeStamp, &record);

    if (result != ESP_OK)
    {
      uint64_t key = 0u;
      (void)meterRingKey(&rings[ring], timeStamp, &key);
      ESP_LOGW(tag, "%s ring record for key %" PRIu64 " refused: %s", rings[ring].name, key,
               esp_err_to_name(result));
    }
  }

  //-- One summary line per snapshot: the hour key leads, the counters keep
  //-- the units of the telegram (kWh and m³), the tariffs are in EUR.
  uint64_t hourKey = 0u;
  (void)meterRingKey(&rings[LOGGER_RING_HOURS], timeStamp, &hourKey);

  ESP_LOGI(tag,
           "stored h=%08" PRIu64 " edT1=%8.3f edT2=%8.3f erT1=%8.3f erT2=%8.3f "
           "gas=%8.3f t1=%5.2f t2=%5.2f tg=%5.2f%s",
           hourKey, record.energyDeliveredTariff1Kwh, record.energyDeliveredTariff2Kwh,
           record.energyReturnedTariff1Kwh, record.energyReturnedTariff2Kwh, record.gasDeliveredM3,
           record.tariffAmount1Euro, record.tariffAmount2Euro, record.tariffAmountGasEuro,
           telegramSeen ? "" : " (no telegram yet)");

  *outTimeStamp = timeStamp;
  return true;
}

//-- Commits the staged records of all three rings to storage. A clean
//-- handle performs no write and no sync.
static void syncRings(uint64_t hourKey)
{
  for (loggerMeterRing_t ring = LOGGER_RING_HOURS; ring < LOGGER_RING_COUNT; ring++)
  {
    esp_err_t result = ringFileSync(rings[ring].handle);

    if (result != ESP_OK)
    {
      ESP_LOGW(tag, "could not synchronize the %s ring: %s", rings[ring].name,
               esp_err_to_name(result));
    }
  }

  ESP_LOGI(tag, "hour %08" PRIu64 ": all ring files synchronized", hourKey);
}

static void storeTask(void* argument)
{
  (void)argument;

  if (!openMeterRings())
  {
    ESP_LOGE(tag, "meter store stays disabled");
    vTaskDelete(NULL);
    return;
  }

  //-- Publish the hour key of the last hourly synchronization; a new hour
  //-- triggers the next one, so the very first snapshot is synchronized
  //-- with the hour it belongs to.
  uint64_t lastSyncedHour = 0u;

  for (;;)
  {
    vTaskDelay(pdMS_TO_TICKS(STORE_RECORD_INTERVAL_MS));

    uint64_t timeStamp = 0u;

    if (!storeSnapshot(&timeStamp))
    {
      continue;
    }

    uint64_t hourKey = 0u;

    if (!meterRingKey(&rings[LOGGER_RING_HOURS], timeStamp, &hourKey))
    {
      continue;
    }

    if (hourKey != lastSyncedHour)
    {
      syncRings(hourKey);
      lastSyncedHour = hourKey;
    }
  }
}

//-- --------------------------------------------------------------------------
//-- Public operations
//-- --------------------------------------------------------------------------

const char* loggerStoreMountPoint(void)
{
  //-- The handles are published once the volume is mounted and all three
  //-- rings are open, so one of them doubles as the "volume available" flag
  //-- for the SMB share.
  return (rings[LOGGER_RING_HOURS].handle != NULL) ? STORE_MOUNT_POINT : NULL;
}

bool loggerStoreVolumeInfo(size_t* totalBytes, size_t* usedBytes)
{
  if (rings[LOGGER_RING_HOURS].handle == NULL)
  {
    return false;
  }

  size_t total = 0u;
  size_t used = 0u;

  if (esp_littlefs_info(STORE_PARTITION_LABEL, &total, &used) != ESP_OK)
  {
    return false;
  }

  *totalBytes = total;
  *usedBytes = used;
  return true;
}

esp_err_t loggerStoreInit(void)
{
  BaseType_t created =
      xTaskCreate(storeTask, "meterStore", STORE_TASK_STACK_BYTES, NULL, STORE_TASK_PRIORITY, NULL);

  if (created != pdPASS)
  {
    ESP_LOGE(tag, "could not create the meter store task");
    return ESP_ERR_NO_MEM;
  }

  return ESP_OK;
}
