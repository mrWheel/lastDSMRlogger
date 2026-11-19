//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_store.c - per-minute smart meter ring file store
//-- Date: 2026-11-19 · Build: 2
//--
//-- The store task owns all ring_file_store work: it mounts the LittleFS
//-- storage partition, breaks any lock file left by an interrupted run,
//-- creates the ring file on first boot and reopens the retained one after
//-- that. Every minute it reads the five meter counters of the most
//-- recently accepted telegram (a reading no telegram published stays 0.0),
//-- adds the two fictional random-cent tariff amounts and upserts the
//-- record under the wall-clock minute key "YYMMDDhhmm". An upsert of the
//-- same minute updates the active record instead of failing, which keeps
//-- reboots and clock nudges harmless. Every ten minutes the task walks the
//-- ring from oldest to newest and prints all stored info, one line per
//-- record.
//-- ==========================================================================

#include "logger_store.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_littlefs.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "logger_receiver.h"
#include "ring_file_store.h"

static const char* tag = "meterStore";

//-- The ring opened by the store task, shared with the web task for the
//-- newest-first list of the SPA: ring_file_store serialises all operations
//-- on one writable handle through a FreeRTOS mutex and every traversal runs
//-- on its own cursor, so the two tasks may walk and write concurrently.
static ringFileHandle_t* ringHandle = NULL;

//-- Ring file on the LittleFS volume of the storage partition.
#define STORE_PARTITION_LABEL "storage"
#define STORE_MOUNT_POINT "/rfs"
#define STORE_RING_PATH "/rfs/meter.rfs"

//-- The file holds the last twenty minute records.
#define STORE_MAX_RECORDS 20u

//-- One record per minute; the complete list goes to the console every ten
//-- records, so every ten minutes.
#define STORE_RECORD_INTERVAL_MS 60000u
#define STORE_DUMP_EVERY_RECORDS 10u

//-- ring_file_store documents at least 8 KB of stack for any task driving
//-- the component; the extra headroom covers the console formatting too.
#define STORE_TASK_STACK_BYTES 10240
#define STORE_TASK_PRIORITY 3

//-- System-time floor for a usable wall clock; before the first SNTP sync
//-- the clock still sits at the 1970 epoch and cannot name a minute.
#define STORE_CLOCK_SYNCED_EPOCH 1700000000

//-- The fictional tariff amounts are a random few cents: 0.01 .. 0.09 EUR.
#define STORE_TARIFF_CENT_MIN 1u
#define STORE_TARIFF_CENT_SPAN 9u

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
//-- Wall-clock minute key
//-- --------------------------------------------------------------------------

//-- Builds the 14-digit EEYYMMDDhhmmss timestamp from the local wall clock
//-- as ring_file_store expects it. False while the clock has not been set
//-- by SNTP yet: a 1970 epoch minute must not enter the ring.
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

//-- --------------------------------------------------------------------------
//-- Ring file life cycle
//-- --------------------------------------------------------------------------

//-- Mounts LittleFS, clears a lock left by an interrupted run and returns
//-- the ring open read-write: created empty on first boot, reopened with
//-- its retained records after that. NULL when the store cannot run.
static ringFileHandle_t* openRingFile(void)
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
    return NULL;
  }

  ESP_LOGI(tag, "mounted LittleFS partition '%s' at %s", STORE_PARTITION_LABEL, STORE_MOUNT_POINT);

  //-- Right after a reset no live handle can exist, so a lock file left by
  //-- a run that never reached ringFileClose() is stale and must go.
  if (ringFileBreakStaleLock(STORE_RING_PATH) == ESP_OK)
  {
    ESP_LOGI(tag, "removed a stale ring lock from an interrupted run");
  }

  ringFileConfig_t config;
  memset(&config, 0, sizeof config);
  config.maxRecords = STORE_MAX_RECORDS;
  config.schemaId = RING_FILE_SCHEMA_YYMMDDHHMM;
  config.recordSize = sizeof(loggerMeterRecord);

  ringFileHandle_t* handle = NULL;
  result = ringFileCreate(STORE_RING_PATH, &config, &handle);

  if (result == RING_FILE_STORE_ERR_ALREADY_EXISTS)
  {
    ringFileOpenConfig_t openConfig;
    memset(&openConfig, 0, sizeof openConfig);
    openConfig.expectedSchemaId = RING_FILE_SCHEMA_YYMMDDHHMM;
    openConfig.expectedRecordSize = sizeof(loggerMeterRecord);
    openConfig.readOnly = false;

    result = ringFileOpen(STORE_RING_PATH, &openConfig, &handle);
  }

  if (result != ESP_OK)
  {
    ESP_LOGE(tag, "could not open ring file %s: %s", STORE_RING_PATH, esp_err_to_name(result));
    esp_vfs_littlefs_unregister(STORE_PARTITION_LABEL);
    return NULL;
  }

  ringFileInfo_t info;
  memset(&info, 0, sizeof info);

  if (ringFileGetInfo(handle, &info) == ESP_OK)
  {
    ESP_LOGI(tag,
             "ring file %s ready: file type '%s', %" PRIu32 " of %" PRIu32
             " minute records retained",
             STORE_RING_PATH, info.schemaId, info.count, info.capacity);
  }

  return handle;
}

//-- --------------------------------------------------------------------------
//-- Console list
//-- --------------------------------------------------------------------------

//-- Full stored record as ring_file_store returns it: the two automatic key
//-- fields followed by the caller payload.
typedef struct
{
  ringFileKeyFields_t keys;
  loggerMeterRecord meter;
} MeterRingEntry;

//-- Prints the complete list: one ESP_LOGI line carrying all stored info of
//-- one record, oldest minute first.
static void logRingContents(ringFileHandle_t* handle)
{
  ringFileInfo_t info;
  memset(&info, 0, sizeof info);
  ringFileGetInfo(handle, &info);

  ESP_LOGI(tag, "== meter ring list: %" PRIu32 " of %" PRIu32 " minute records ==", info.count,
           info.capacity);

  MeterRingEntry entry;
  ringFileIterator_t iterator;
  memset(&entry, 0, sizeof entry);
  memset(&iterator, 0, sizeof iterator);

  esp_err_t result = ringFileIteratorBegin(handle, &iterator, RING_FILE_ITER_OLDEST, NULL, &entry);
  uint32_t printed = 0u;

  while (result == ESP_OK)
  {
    ESP_LOGI(tag,
             "%2" PRIu32 " key %010" PRIu64 " edT1=%8.3f edT2=%8.3f erT1=%8.3f erT2=%8.3f "
             "gas=%8.3f t1=%5.2f t2=%5.2f",
             ++printed, entry.keys.usedKey % 10000000000ULL, entry.meter.energyDeliveredTariff1Kwh,
             entry.meter.energyDeliveredTariff2Kwh, entry.meter.energyReturnedTariff1Kwh,
             entry.meter.energyReturnedTariff2Kwh, entry.meter.gasDeliveredM3,
             entry.meter.tariffAmount1Euro, entry.meter.tariffAmount2Euro);

    result = ringFileIteratorNext(handle, &iterator, &entry);
  }

  if ((result != RING_FILE_STORE_ERR_ITER_END) && (result != RING_FILE_STORE_ERR_RING_EMPTY))
  {
    ESP_LOGW(tag, "ring list stopped after %" PRIu32 " records: %s", printed,
             esp_err_to_name(result));
  }

  ESP_LOGI(tag, "== end of meter ring list ==");
}

//-- --------------------------------------------------------------------------
//-- Newest-first list for the SPA
//-- --------------------------------------------------------------------------

//-- Walks the ring from the newest retained record to the oldest, handing
//-- every record to the visitor on the caller's task. A concurrent minute
//-- upsert invalidates the cursor: the walk then begins again from the
//-- newest and skips every key already emitted (keys strictly decrease as
//-- the walk ages), so the list stays complete and duplicate-free while the
//-- store task keeps writing.
esp_err_t loggerStoreListRecords(loggerStoreRecordVisitor visitor, void* userData)
{
  ringFileHandle_t* handle = ringHandle;

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
      //-- visitor the plain 10-digit wall-clock minute (YYMMDDhhmm).
      if (!visitor(entry.keys.usedKey % 10000000000ULL, &entry.meter, userData))
      {
        return ESP_OK;
      }
    }

    if (emitted > (STORE_MAX_RECORDS * 2u))
    {
      //-- Safety net against a pathological invalidate/re-begin cycle.
      ESP_LOGW(tag, "ring list walk abandoned after %" PRIu32 " entries", emitted);
      return ESP_FAIL;
    }

    result = ringFileIteratorPrevious(handle, &iterator, &entry);

    if (result == RING_FILE_STORE_ERR_ITER_INVALIDATED)
    {
      result = ringFileIteratorBegin(handle, &iterator, RING_FILE_ITER_NEWEST, NULL, &entry);
    }
  }

  if ((result != RING_FILE_STORE_ERR_ITER_END) && (result != RING_FILE_STORE_ERR_RING_EMPTY))
  {
    ESP_LOGW(tag, "ring list walk stopped after %" PRIu32 " records: %s", emitted,
             esp_err_to_name(result));
    return result;
  }

  return ESP_OK;
}

//-- --------------------------------------------------------------------------
//-- Store task
//-- --------------------------------------------------------------------------

//-- Writes one minute record and upserts it under the wall-clock minute.
static void storeMinuteRecord(ringFileHandle_t* handle)
{
  uint64_t timeStamp = 0u;

  if (!currentTimeStamp(&timeStamp))
  {
    ESP_LOGI(tag, "wall clock not synchronised yet, minute record postponed");
    return;
  }

  loggerMeterRecord record;
  bool telegramSeen = readLatestMeterCounters(&record);

  //-- The fictional tariff amounts: a random few cents per tariff.
  record.tariffAmount1Euro =
      (float)(STORE_TARIFF_CENT_MIN + (esp_random() % STORE_TARIFF_CENT_SPAN)) / 100.0f;
  record.tariffAmount2Euro =
      (float)(STORE_TARIFF_CENT_MIN + (esp_random() % STORE_TARIFF_CENT_SPAN)) / 100.0f;

  esp_err_t result = ringFileUpsert(handle, timeStamp, &record);

  //-- The minute key as the user reads it: YYMMDDhhmm, the usedKey without
  //-- the component's century digits.
  uint64_t minuteKey = (timeStamp / 100ULL) % 10000000000ULL;

  if (result == ESP_OK)
  {
    ESP_LOGI(tag,
             "stored key %010" PRIu64 " edT1=%8.3f edT2=%8.3f erT1=%8.3f erT2=%8.3f "
             "gas=%8.3f t1=%5.2f t2=%5.2f%s",
             minuteKey, record.energyDeliveredTariff1Kwh, record.energyDeliveredTariff2Kwh,
             record.energyReturnedTariff1Kwh, record.energyReturnedTariff2Kwh,
             record.gasDeliveredM3, record.tariffAmount1Euro, record.tariffAmount2Euro,
             telegramSeen ? "" : " (no telegram yet)");
  }
  else
  {
    ESP_LOGW(tag, "minute record for key %010" PRIu64 " refused: %s", minuteKey,
             esp_err_to_name(result));
  }
}

static void storeTask(void* argument)
{
  (void)argument;

  ringFileHandle_t* handle = openRingFile();

  if (handle == NULL)
  {
    ESP_LOGE(tag, "meter store stays disabled");
    vTaskDelete(NULL);
    return;
  }

  //-- Publish the handle for the SPA list: from here the web task may walk
  //-- the ring newest to oldest alongside the minute writer.
  ringHandle = handle;

  uint32_t minuteCount = 0u;

  for (;;)
  {
    vTaskDelay(pdMS_TO_TICKS(STORE_RECORD_INTERVAL_MS));
    minuteCount++;

    storeMinuteRecord(handle);

    if ((minuteCount % STORE_DUMP_EVERY_RECORDS) == 0u)
    {
      logRingContents(handle);
    }
  }
}

//-- --------------------------------------------------------------------------
//-- Public operations
//-- --------------------------------------------------------------------------

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
