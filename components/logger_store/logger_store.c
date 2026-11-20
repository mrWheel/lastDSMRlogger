//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_store.c - hour, day and month smart meter ring file store
//-- Date: 2026-11-20 · Build: 5
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
//-- ==========================================================================

#include "logger_store.h"

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
