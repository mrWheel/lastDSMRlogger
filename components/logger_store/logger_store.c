//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_store.c - hour, day and month smart meter ring file store
//-- Date: 2026-11-25 · Build: 22
//--
//-- The store task owns all ring_file_store work: it mounts the LittleFS
//-- storage partition, removes the ring file of the retired minute store,
//-- breaks any lock file left by an interrupted run, creates the three ring
//-- files on first boot and reopens the retained ones after that. All three
//-- are opened in buffered persistence with a one-record staging buffer.
//-- Every minute the task reads the five meter counters of the most
//-- recently accepted telegram (a reading no telegram published stays 0.0)
//-- and upserts the snapshot into the current hour, day and month record.
//-- Each record also stores the Settings network and transport month
//-- amount prorated to the ring's period (the months ring the full
//-- amount, the days ring a thirtieth, the hours ring a thirtieth of a
//-- day). The three tariff amounts of each ring record are filled by the
//-- registered tariff calculator (the tariff component's calculator values
//-- the energy consumed inside the ring's period with the dynamic tariff
//-- table and the manual prices); without a calculator the task adds the
//-- fictional random-cent demo amounts. An upsert of the same period
//-- updates the active record instead of failing, which keeps reboots and
//-- clock nudges harmless. The task synchronizes all three files at least
//-- once per configured rfs synchronisation interval so the staged records
//-- reach storage; snapshots of one period coalesce in the staging buffers.
//-- The console's populate command feeds historical periods in through
//-- loggerStorePopulateRing(): a semicolon separated csv file from the
//-- volume root (the import csv files are shipped there by the storage
//-- partition image built from the project's storageImage/ folder), keyed
//-- by period and columned by OBIS code, is edited into one ring: missing
//-- counter values become 0.0, while a missing or blank ti, te, tg or nc
//-- value is computed from the stored settings (the three average tariffs
//-- from the manual tariff 1 prices and the prorated network and transport
//-- month amount). A mode picks what happens to the records
//-- the ring already holds: merge (default) backfills, overwrite clears the
//-- ring before importing and update only revises stored periods. The
//-- console's ring2csv command writes one ring back to the volume root as
//-- a re-importable csv through loggerStoreRingToCsv().
//--
//-- The SPA's Edit view reads one ring through loggerStoreEditorSnapshot()
//-- (the ring's own bucket order newest first, a content pin and a calendar
//-- key repair preview per row) and revises it through
//-- loggerStoreApplyRingEdits(): the store task rebuilds the complete ring
//-- (repaired keys, edited payloads, draft consumption edits applied newest
//-- to oldest per channel), validates every candidate value, writes it as
//-- a verified journal ring file and publishes it through two atomic
//-- renames, so a failed or interrupted Save always yields the complete old
//-- or the complete new ring (recoverPendingRingEdits() resolves leftovers
//-- at startup, before the rings open). The newest record — the open period
//-- the snapshot upsert rewrites every minute — is locked: shown, never
//-- edited.
//-- ==========================================================================

#include "logger_store.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_littlefs.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "logger_control.h"
#include "logger_receiver.h"
#include "logger_settings.h"
#include "ring_file_store.h"

static const char* tag = "meterStore";

//-- Ring files on the LittleFS volume of the storage partition. The block
//-- size is the one the storage partition image is built with (see the
//-- littlefs build step); it scales the volume block count for the wear
//-- estimate.
#define STORE_PARTITION_LABEL "storage"
#define STORE_MOUNT_POINT "/rfs"
#define STORE_LITTLEFS_BLOCK_SIZE 4096u

//-- The ring file of the retired per-minute store; it has no reader
//-- anymore and is removed from the volume when it is still present.
#define STORE_LEGACY_RING_PATH STORE_MOUNT_POINT "/meter.rfs"

//-- Record layout version marker on the volume. The ring files store the
//-- payload as raw bytes, so a firmware that changed the *meaning* of a
//-- field cannot tell its records from the old ones by size alone (the
//-- tariff amounts moved from one per tariff to one per direction of the
//-- energy, which kept the struct's floats; adding the network costs field
//-- made it nine). The store task compares
//-- this marker after mounting: a volume that does not carry the marker of
//-- the current layout gets its three ring files rebuilt empty (the
//-- retained periods return through the console's populate command). Bump
//-- the version with every payload change, name change included.
#define STORE_LAYOUT_MARKER_PATH STORE_MOUNT_POINT "/ringlayout.ver"
#define STORE_RECORD_LAYOUT_VERSION 3u

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
//-- Only used while no tariff calculator is registered.
#define STORE_TARIFF_CENT_MIN 1u
#define STORE_TARIFF_CENT_SPAN 9u

//-- One snapshot coalesces into a single staged record per ring, so one
//-- synchronisation commits at most one slot image per ring plus the
//-- file-system metadata those writes commit (a pair per file). The block
//-- figures feed the settings view's wear estimate and are deliberately
//-- rounded up. The flash cycle rating is the datasheet minimum of the
//-- ESP32-S3 embedded flash.
#define STORE_SYNC_DATA_BLOCKS_PER_RING 1u
#define STORE_SYNC_METADATA_BLOCKS_PER_RING 2u
#define STORE_FLASH_WRITE_CYCLES 100000u

//-- Calculator filling the tariff amounts of every snapshot record; set
//-- once at boot (see loggerStoreSetTariffCalculator).
static loggerStoreTariffCalculator tariffCalculator = NULL;

//-- Manual synchronisation request (the console's sync command): the flag
//-- forces the next snapshot to commit all rings regardless of the interval,
//-- the semaphore wakes the store task from its minute sleep so the commit
//-- lands at once instead of on the next minute boundary.
static SemaphoreHandle_t storeWakeSem;
static volatile bool syncRequested = false;

//-- Completion token of a synchronisation (the console's restart waits on
//-- it): the store task gives it after every commit of all three rings, so
//-- a waiter learns the staged records have really reached storage.
static SemaphoreHandle_t syncDoneSem;

//-- Locks of the editor transactions (see the RFS history editor section):
//-- storeAccessMutex guards the live handles through the rare moments the
//-- store task closes one (a Save publishes a replacement ring); every walk
//-- of a ring (console, SPA, editor snapshot) holds it, so a walk never
//-- runs against a handle that was just closed. storeWriterMutex serialises
//-- the whole-ring writers against each other: the editor Save and the
//-- populate import never edit one ring at the same time. Both are taken in
//-- that order (writer, then access) and only by tasks that may block.
static SemaphoreHandle_t storeAccessMutex;
static SemaphoreHandle_t storeWriterMutex;

//-- Editor Save jobs: the public entry hands the parsed request to the
//-- store task (the rings' owner) through this queue and waits on the job's
//-- completion token, so a Save never races the minute snapshot (the store
//-- task runs the whole transaction between two snapshots).
static QueueHandle_t editorQueue;

void loggerStoreSetTariffCalculator(loggerStoreTariffCalculator calculator)
{
  tariffCalculator = calculator;
}

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
    //-- Twenty-five months: the Months Graph shows the newest twelve, each
    //-- paired with the same month of the year before, and the oldest of
    //-- those year-ago months still needs its own predecessor in the ring
    //-- for the graph to compute its per-period amounts.
    {"months", STORE_MOUNT_POINT "/months.rfs", RING_FILE_SCHEMA_YYMM, 25u, 10000ULL, NULL},
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
//-- both are collected and the first one published wins. The last entry is
//-- the meter's tariff indicator (001/002): it is not a counter and never
//-- lands in the record, it only names the tariff whose prices value the
//-- snapshot's amounts.
#define STORE_WANTED_COUNT 7

static const char* const wantedObis[STORE_WANTED_COUNT] = {
    "1-0:1.8.1", "1-0:1.8.2", "1-0:2.8.1", "1-0:2.8.2", "0-1:24.2.1", "0-1:24.2.3", "0-0:96.14.0"};

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

//-- Fills the payload counters from the newest retained telegram and reports
//-- the tariff indicator that telegram published (1 or 2; 0 when the meter
//-- names no active tariff, and also 0 when no telegram was inspected).
//-- Counters the telegram did not publish stay 0.0; the return value only
//-- reports whether a telegram was inspected.
static bool readLatestMeterCounters(loggerMeterRecord* record, uint8_t* outTariffIndicator)
{
  memset(record, 0, sizeof *record);
  *outTariffIndicator = 0u;

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

  //-- Tariff indicator: the meter publishes it as 001 or 002 (some also as
  //-- 000). Anything but one or two counts as "no active tariff named".
  if (scan.found[6])
  {
    const int indicator = (int)(scan.values[6] + 0.5f);

    if ((indicator == 1) || (indicator == 2))
    {
      *outTariffIndicator = (uint8_t)indicator;
    }
  }

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
//-- Network and transport costs
//-- --------------------------------------------------------------------------

//-- The Settings network and transport amount stored with one record of one
//-- ring: the configured month amount prorated to the ring's period (the
//-- days ring stores a thirtieth of the month, the hours ring a thirtieth
//-- of a day, the months ring the full month amount).
static float networkCostsForRing(loggerMeterRing_t ring, float monthlyEuro)
{
  switch (ring)
  {
  case LOGGER_RING_DAYS:
    return monthlyEuro / 30.0f;
  case LOGGER_RING_HOURS:
    return monthlyEuro / (24.0f * 30.0f);
  default:
    return monthlyEuro;
  }
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
    LOGGER_LOGI(tag, "removed a stale %s ring lock from an interrupted run", ring->name);
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

  //-- A retained file written by a firmware whose loggerMeterRecord had a
  //-- different layout cannot be read back (the schema id and the record
  //-- size both live in the file header). Rebuild such a file empty once,
  //-- like a ring whose capacity changed: the console's populate command
  //-- fills it again.
  if (result == RING_FILE_STORE_ERR_SCHEMA_MISMATCH)
  {
    LOGGER_LOGW(tag,
                "ring file %s was written with another record layout: rebuilding it empty "
                "(re-run populate to fill it)",
                ring->path);
    (void)unlink(ring->path);
    (void)ringFileBreakStaleLock(ring->path);
    handle = NULL;

    result = ringFileCreate(ring->path, &config, &handle);
  }

  if (result != ESP_OK)
  {
    LOGGER_LOGE(tag, "could not open ring file %s: %s", ring->path, esp_err_to_name(result));
    return false;
  }

  ringFileInfo_t info;
  memset(&info, 0, sizeof info);

  if (ringFileGetInfo(handle, &info) == ESP_OK)
  {
    LOGGER_LOGI(tag,
                "ring file %s ready: file type '%s', %" PRIu32 " of %" PRIu32 " records retained",
                ring->path, info.schemaId, info.count, info.capacity);
  }

  //-- A ring file keeps the capacity it was created with in its header, so
  //-- a ring whose configured capacity grew would silently stay on the old
  //-- table. Rebuild such a file empty once (the console's populate command
  //-- fills it again) instead of ignoring the new capacity.
  if ((info.capacity != 0u) && (info.capacity != ring->maxRecords))
  {
    LOGGER_LOGW(tag,
                "ring file %s was built for %" PRIu32 " records but the ring now retains %" PRIu32
                ": rebuilding it empty (re-run populate to fill it)",
                ring->path, info.capacity, ring->maxRecords);
    ringFileClose(handle);
    (void)unlink(ring->path);
    (void)ringFileBreakStaleLock(ring->path);
    handle = NULL;

    result = ringFileCreate(ring->path, &config, &handle);

    if (result != ESP_OK)
    {
      LOGGER_LOGE(tag, "could not rebuild ring file %s: %s", ring->path, esp_err_to_name(result));
      return false;
    }

    if (ringFileGetInfo(handle, &info) == ESP_OK)
    {
      LOGGER_LOGI(
          tag, "ring file %s rebuilt: file type '%s', %" PRIu32 " of %" PRIu32 " records retained",
          ring->path, info.schemaId, info.count, info.capacity);
    }
  }

  ring->handle = handle;
  return true;
}

//-- Rebuilds the three ring files once when the volume still carries the
//-- records of another payload layout (see STORE_RECORD_LAYOUT_VERSION).
//-- Everything else on the volume — the tariff schedule and the import csv
//-- files — stays untouched.
static void ensureRingRecordLayout(void)
{
  uint32_t storedVersion = 0u;
  FILE* marker = fopen(STORE_LAYOUT_MARKER_PATH, "rb");

  if (marker != NULL)
  {
    size_t readCount = fread(&storedVersion, sizeof storedVersion, 1u, marker);
    fclose(marker);

    if (readCount != 1u)
    {
      storedVersion = 0u;
    }
  }

  if (storedVersion == STORE_RECORD_LAYOUT_VERSION)
  {
    return;
  }

  LOGGER_LOGW(tag,
              "ring files carry record layout %u, this firmware stores layout %u: rebuilding the "
              "rings empty (re-run populate to fill them)",
              (unsigned)storedVersion, (unsigned)STORE_RECORD_LAYOUT_VERSION);

  for (loggerMeterRing_t ring = LOGGER_RING_HOURS; ring < LOGGER_RING_COUNT; ring++)
  {
    (void)ringFileBreakStaleLock(rings[ring].path);
    (void)unlink(rings[ring].path);
  }

  FILE* fresh = fopen(STORE_LAYOUT_MARKER_PATH, "wb");

  if (fresh != NULL)
  {
    uint32_t version = STORE_RECORD_LAYOUT_VERSION;
    (void)fwrite(&version, sizeof version, 1u, fresh);
    fclose(fresh);
  }
  else
  {
    LOGGER_LOGW(tag, "could not write the ring layout marker: the rings rebuild again next boot");
  }
}

//-- Takes the editor locks (writer first, then access) with a bounded
//-- wait: false when a lock stayed busy (another whole-ring writer or a
//-- long walk). Releases everything it managed to take before failing.
static bool storeTakeLocks(TickType_t timeout)
{
  if ((storeWriterMutex == NULL) || (storeAccessMutex == NULL))
  {
    //-- The store task never started (no Init, no rings): nothing to guard.
    return false;
  }

  if (xSemaphoreTake(storeWriterMutex, timeout) != pdTRUE)
  {
    return false;
  }

  if (xSemaphoreTake(storeAccessMutex, timeout) != pdTRUE)
  {
    xSemaphoreGive(storeWriterMutex);
    return false;
  }

  return true;
}

static void storeReleaseLocks(void)
{
  if (storeAccessMutex != NULL)
  {
    xSemaphoreGive(storeAccessMutex);
  }

  if (storeWriterMutex != NULL)
  {
    xSemaphoreGive(storeWriterMutex);
  }
}

//-- Only the plain ring access (walks, exports, snapshots) needs the access
//-- guard; these paths never close a handle, they only read it.
static bool storeTakeAccess(TickType_t timeout)
{
  return ((storeAccessMutex != NULL) && (xSemaphoreTake(storeAccessMutex, timeout) == pdTRUE));
}

static void storeReleaseAccess(void)
{
  if (storeAccessMutex != NULL)
  {
    xSemaphoreGive(storeAccessMutex);
  }
}

//-- Resolves an unfinished editor Save left on the volume by an interrupted
//-- run (power loss between the rename steps): deterministic so recovery
//-- yields either the complete old ring or the complete new one, and it
//-- runs before any ring opens. Defined with the editor section below.
static void recoverPendingRingEdits(void);

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
    LOGGER_LOGE(tag, "could not mount LittleFS partition '%s' at %s: %s", STORE_PARTITION_LABEL,
                STORE_MOUNT_POINT, esp_err_to_name(result));
    return false;
  }

  LOGGER_LOGI(tag, "mounted LittleFS partition '%s' at %s", STORE_PARTITION_LABEL,
              STORE_MOUNT_POINT);

  //-- The temporary per-minute store is retired: its ring file and a lock
  //-- it may have left behind no longer belong on the volume.
  (void)ringFileBreakStaleLock(STORE_LEGACY_RING_PATH);

  if (unlink(STORE_LEGACY_RING_PATH) == 0)
  {
    LOGGER_LOGI(tag, "removed the retired minute ring file %s", STORE_LEGACY_RING_PATH);
  }

  ensureRingRecordLayout();

  //-- An editor Save interrupted by a power loss leaves its journal and
  //-- backup behind: resolve them before the rings open (and before any
  //-- automatic rebuild or resizing could meet them unawares).
  recoverPendingRingEdits();

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

  //-- The access guard keeps the walk off a handle an editor Save is about
  //-- to close for its rename swap.
  if (!storeTakeAccess(pdMS_TO_TICKS(60000)))
  {
    return ESP_ERR_INVALID_STATE;
  }

  ringFileHandle_t* handle = active->handle;

  if (handle == NULL)
  {
    storeReleaseAccess();
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
        storeReleaseAccess();
        return ESP_OK;
      }
    }

    if (emitted > (active->maxRecords * 2u))
    {
      //-- Safety net against a pathological invalidate/re-begin cycle.
      LOGGER_LOGW(tag, "%s ring list walk abandoned after %" PRIu32 " entries", active->name,
                  emitted);
      storeReleaseAccess();
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

  storeReleaseAccess();

  if ((result != RING_FILE_STORE_ERR_ITER_END) && (result != RING_FILE_STORE_ERR_RING_EMPTY))
  {
    LOGGER_LOGW(tag, "%s ring list walk stopped after %" PRIu32 " records: %s", active->name,
                emitted, esp_err_to_name(result));
    return result;
  }

  return ESP_OK;
}

//-- ==========================================================================
//-- RFS history editor (the SPA's Edit view)
//--
//-- One Save is a whole-ring transaction: the store task walks the ring,
//-- rebuilds a complete candidate (repaired keys, edited payloads), writes
//-- it to a journal ring file, verifies it on storage and only then swaps
//-- the live file for the journal through two renames (live -> backup,
//-- journal -> live). LittleFS renames are atomic, so every crash moment
//-- leaves a volume state that startup recovery can resolve to either the
//-- complete old ring or the complete new one. The newest record — the open
//-- period the logger rewrites with every snapshot — is locked: the editor
//-- shows it and refuses to edit it, so a Save never fights the snapshot
//-- and saved corrections to closed periods stay (the tariff calculator
//-- only ever values the live period, never a closed record).
//-- ==========================================================================

//-- Volume companions of one ring file during a Save.
#define EDITOR_JOURNAL_SUFFIX ".edit.tmp"
#define EDITOR_BACKUP_SUFFIX ".bak"
#define EDITOR_PATH_BYTES 320

//-- Readable name of a ring_file_store result code, defined with the
//-- populate import below and reused for the editor's failure reasons.
static const char* importErrName(esp_err_t err);

//-- Bounds of the typed SPA API: at most this many records per ring (the
//-- largest ring capacity with headroom for the staged record).
#define EDITOR_MAX_ENTRIES 52u
#define EDITOR_MAX_CONSUMPTION 99999.999f
#define EDITOR_MAX_AMOUNT 99999.99f

//-- Seconds of grace the newest key may sit ahead of the device clock
//-- before the anchor is called untrustworthy (per ring granularity).
#define EDITOR_ANCHOR_GRACE_HOURS_S 7200
#define EDITOR_ANCHOR_GRACE_DAYS_S (2 * 86400)
#define EDITOR_ANCHOR_GRACE_MONTHS_S (62 * 86400)

//-- The five payload readings behind their channel index, so the editor
//-- loops over channels instead of naming each field.
static float* editorChannelField(loggerMeterRecord* record, uint32_t channel)
{
  switch (channel)
  {
  case 0u:
    return &record->energyDeliveredTariff1Kwh;
  case 1u:
    return &record->energyDeliveredTariff2Kwh;
  case 2u:
    return &record->energyReturnedTariff1Kwh;
  case 3u:
    return &record->energyReturnedTariff2Kwh;
  default:
    return &record->gasDeliveredM3;
  }
}

static const float* editorChannelFieldConst(const loggerMeterRecord* record, uint32_t channel)
{
  return editorChannelField((loggerMeterRecord*)record, channel);
}

//-- The three tariff amounts behind their index (import, export, gas).
static float* editorAmountField(loggerMeterRecord* record, uint32_t amount)
{
  switch (amount)
  {
  case 0u:
    return &record->tariffAmountImportEuro;
  case 1u:
    return &record->tariffAmountExportEuro;
  default:
    return &record->tariffAmountGasEuro;
  }
}

//-- The payload field a channel names for the validation messages.
static const char* editorChannelName(uint32_t channel)
{
  static const char* const names[LOGGER_STORE_EDITOR_CHANNELS] = {"Ed T1", "Ed T2", "Er T1",
                                                                  "Er T2", "gas"};

  return (channel < LOGGER_STORE_EDITOR_CHANNELS) ? names[channel] : "channel";
}

static uint8_t editorKeyDigits(loggerMeterRing_t ring)
{
  return (ring == LOGGER_RING_HOURS) ? 8u : ((ring == LOGGER_RING_DAYS) ? 6u : 4u);
}

static ringFileGranularity_t editorGranularity(loggerMeterRing_t ring)
{
  return (ring == LOGGER_RING_HOURS) ? RING_FILE_GRANULARITY_HOUR
                                     : ((ring == LOGGER_RING_DAYS) ? RING_FILE_GRANULARITY_DAY
                                                                   : RING_FILE_GRANULARITY_MONTH);
}

//-- Splits a wall-clock period key into its date parts (the digits the
//-- ring's schema names; the months ring has no day or hour digits and the
//-- days ring no hour). False for impossible digits (month 0 or 13, day 0
//-- or 32, hour 24 ..) — a day that is not valid for its month is caught
//-- separately through the component's timestamp validation.
static bool editorKeyParts(loggerMeterRing_t ring, uint64_t key, unsigned* outYear2,
                           unsigned* outMonth, unsigned* outDay, unsigned* outHour)
{
  const uint8_t digits = editorKeyDigits(ring);
  uint64_t rest = key;

  unsigned hour = 0u;
  unsigned day = 1u;

  if (digits == 8u)
  {
    hour = (unsigned)(rest % 100u);
    rest /= 100u;
  }

  if (digits >= 6u)
  {
    day = (unsigned)(rest % 100u);
    rest /= 100u;
  }

  unsigned month = (unsigned)(rest % 100u);
  unsigned year2 = (unsigned)(rest / 100u);

  if ((year2 > 99u) || (month < 1u) || (month > 12u) || (day < 1u) || (day > 31u) || (hour > 23u))
  {
    return false;
  }

  *outYear2 = year2;
  *outMonth = month;
  *outDay = day;
  *outHour = hour;
  return true;
}

//-- The 14-digit ring timestamp of a period key (minute and second always
//-- zero padding, the day defaults to 01 and the hour to 00 where the ring
//-- has no such digits — the spelling the populate import already uses).
static bool editorKeyToTimeStamp(loggerMeterRing_t ring, uint64_t key, uint64_t* outTimeStamp)
{
  unsigned year2 = 0u;
  unsigned month = 0u;
  unsigned day = 0u;
  unsigned hour = 0u;

  if (!editorKeyParts(ring, key, &year2, &month, &day, &hour))
  {
    return false;
  }

  uint64_t timeStamp = 20u;
  timeStamp = (timeStamp * 100u) + year2;
  timeStamp = (timeStamp * 100u) + month;
  timeStamp = (timeStamp * 100u) + day;
  timeStamp = (timeStamp * 100u) + hour;
  timeStamp = timeStamp * 10000u;

  uint64_t probeUsedKey = 0u;

  if (ringFileTimeStampToUsedKey(timeStamp, editorGranularity(ring), &probeUsedKey) != ESP_OK)
  {
    //-- The component names the exact rules (century 20, real day of the
    //-- month including leap years): a key it refuses is malformed.
    return false;
  }

  *outTimeStamp = timeStamp;
  return true;
}

//-- True when the ring's schema accepts the key as a period label.
static bool editorKeyLegal(loggerMeterRing_t ring, uint64_t key)
{
  uint64_t timeStamp = 0u;
  return editorKeyToTimeStamp(ring, key, &timeStamp);
}

//-- The broken-down local wall clock an anchor key stands for. The months
//-- anchor rides the 15th at noon and the days anchor noon itself, so the
//-- daylight-saving shifts (which sit in the small hours) can never push
//-- the probe into the neighbouring month or day.
static bool editorAnchorTime(loggerMeterRing_t ring, uint64_t anchorKey, time_t* outTime,
                             struct tm* outTm)
{
  unsigned year2 = 0u;
  unsigned month = 0u;
  unsigned day = 0u;
  unsigned hour = 0u;

  if (!editorKeyParts(ring, anchorKey, &year2, &month, &day, &hour))
  {
    return false;
  }

  struct tm brokenTime;
  memset(&brokenTime, 0, sizeof brokenTime);
  brokenTime.tm_year = (int)(100u + year2);
  brokenTime.tm_mon = (int)month - 1;
  brokenTime.tm_mday = (ring == LOGGER_RING_MONTHS) ? 15 : (int)day;
  brokenTime.tm_hour =
      (ring == LOGGER_RING_MONTHS) ? 12 : ((ring == LOGGER_RING_DAYS) ? 12 : (int)hour);
  brokenTime.tm_isdst = -1;

  time_t anchorTime = mktime(&brokenTime);

  if (anchorTime == (time_t)-1)
  {
    return false;
  }

  if (localtime_r(&anchorTime, outTm) == NULL)
  {
    return false;
  }

  *outTime = anchorTime;
  return true;
}

//-- The expected key sequence for count records ending at the anchor:
//-- expected[i] names the period of the row at snapshot position i (newest
//-- first). The hours ring steps the anchor's unix time back one hour per
//-- row, so the sequence reproduces the device's daylight-saving behaviour
//-- exactly (the skipped spring-forward hour and the coalesced
//-- fall-back hour — the logger's existing collision policy). The days
//-- ring steps whole days from local noon, the months ring steps calendar
//-- months. False (with the reason) when the anchor cannot be trusted or
//-- the sequence would put two rows in the same period (a repeated local
//-- hour that the hour schema cannot represent side by side).
static bool editorExpectedSequence(loggerMeterRing_t ring, uint64_t anchorKey, uint32_t count,
                                   uint64_t* outKeys, char* outReason, size_t reasonSize)
{
  if (count == 0u)
  {
    return true;
  }

  if (!editorKeyLegal(ring, anchorKey))
  {
    snprintf(outReason, reasonSize,
             "the newest record carries a malformed period key (%llu): the calendar anchor "
             "cannot be trusted",
             (unsigned long long)anchorKey);
    return false;
  }

  time_t anchorTime = 0u;
  struct tm anchorTm;

  if (!editorAnchorTime(ring, anchorKey, &anchorTime, &anchorTm))
  {
    snprintf(outReason, reasonSize, "the newest period key cannot be resolved to a local date");
    return false;
  }

  time_t now = time(NULL);

  if (now >= (time_t)STORE_CLOCK_SYNCED_EPOCH)
  {
    const time_t grace = (ring == LOGGER_RING_HOURS)
                             ? EDITOR_ANCHOR_GRACE_HOURS_S
                             : ((ring == LOGGER_RING_DAYS) ? EDITOR_ANCHOR_GRACE_DAYS_S
                                                           : EDITOR_ANCHOR_GRACE_MONTHS_S);

    if ((anchorTime - now) > grace)
    {
      snprintf(outReason, reasonSize,
               "the newest record names a period far ahead of the device clock: the clock or the "
               "newest key is not trustworthy");
      return false;
    }
  }

  const uint8_t digits = editorKeyDigits(ring);

  for (uint32_t index = 0u; index < count; index++)
  {
    struct tm probe = anchorTm;
    time_t probeTime;

    if (ring == LOGGER_RING_MONTHS)
    {
      probe.tm_mon = anchorTm.tm_mon - (int)index;
      probe.tm_isdst = -1;
      probeTime = mktime(&probe);

      if (probeTime == (time_t)-1)
      {
        snprintf(outReason, reasonSize, "the expected month sequence could not be derived");
        return false;
      }
    }
    else
    {
      const time_t stepSeconds =
          (ring == LOGGER_RING_HOURS) ? ((time_t)index * 3600) : ((time_t)index * 86400);
      probeTime = anchorTime - stepSeconds;
    }

    struct tm probeTm;

    if (localtime_r(&probeTime, &probeTm) == NULL)
    {
      snprintf(outReason, reasonSize, "the expected period sequence could not be derived");
      return false;
    }

    uint64_t expected = (uint64_t)(probeTm.tm_year - 100) * 100u + (uint64_t)(probeTm.tm_mon + 1);

    if (digits >= 6u)
    {
      expected = expected * 100u + (uint64_t)probeTm.tm_mday;
    }

    if (digits == 8u)
    {
      expected = expected * 100u + (uint64_t)probeTm.tm_hour;
    }

    outKeys[index] = expected;

    if ((index > 0u) && (expected == outKeys[index - 1u]))
    {
      snprintf(outReason, reasonSize,
               "the local hour repeats across the daylight-saving transition: the ring schema "
               "cannot name both occurrences (the logger coalesces them into one record)");
      return false;
    }
  }

  return true;
}

//-- FNV-1a over the stored record bytes (the two key fields plus the
//-- payload): the content pin the Save carries back to detect a record
//-- that changed since the snapshot.
static uint32_t editorChecksum(const MeterRingEntry* entry)
{
  const uint8_t* bytes = (const uint8_t*)entry;
  uint32_t hash = 2166136261u;

  for (size_t index = 0u; index < sizeof *entry; index++)
  {
    hash ^= bytes[index];
    hash *= 16777619u;
  }

  return hash;
}

//-- The editor walk of one ring: the retained records in the ring's own
//-- bucket order, newest first, the staging buffer included. Unlike the
//-- console walk it never filters on a decreasing key (a corrupt key must
//-- stay visible, it is what the repair is for) — deduplication of the
//-- records a concurrent snapshot re-hands after an iterator invalidation
//-- runs on physical slots, which stay stable regardless of the keys.
static esp_err_t editorWalkRing(loggerMeterRing_t ring, MeterRingEntry* entries, bool* outStaged,
                                uint32_t maxEntries, uint32_t* outCount, uint32_t* outCapacity)
{
  MeterRing* active = &rings[ring];
  ringFileHandle_t* handle = active->handle;

  *outCount = 0u;
  *outCapacity = active->maxRecords;

  if (handle == NULL)
  {
    return ESP_ERR_INVALID_STATE;
  }

  ringFileInfo_t info;
  memset(&info, 0, sizeof info);

  if (ringFileGetInfo(handle, &info) == ESP_OK)
  {
    *outCapacity = info.capacity;
  }

  const uint32_t capacity = (*outCapacity > 0u) ? *outCapacity : active->maxRecords;
  bool seen[EDITOR_MAX_ENTRIES + 2u];
  bool seenStaged = false;
  memset(seen, 0, sizeof seen);

  MeterRingEntry entry;
  ringFileIterator_t iterator;
  memset(&entry, 0, sizeof entry);
  memset(&iterator, 0, sizeof iterator);

  esp_err_t result = ringFileIteratorBegin(handle, &iterator, RING_FILE_ITER_NEWEST, NULL, &entry);
  uint32_t steps = 0u;

  while (result == ESP_OK)
  {
    uint32_t slot = 0u;
    bool staged = false;

    if (ringFileIteratorGetSlot(handle, &iterator, &slot) == ESP_OK)
    {
      staged = (slot == RING_FILE_SLOT_STAGED);
    }
    else
    {
      slot = capacity; //-- unknown slot: only ever emitted once
    }

    const bool fresh =
        staged ? !seenStaged : ((slot < (sizeof seen / sizeof seen[0])) && !seen[slot]);

    if (fresh)
    {
      if (staged)
      {
        seenStaged = true;
      }
      else if (slot < (sizeof seen / sizeof seen[0]))
      {
        seen[slot] = true;
      }

      if (*outCount < maxEntries)
      {
        entries[*outCount] = entry;

        if (outStaged != NULL)
        {
          outStaged[*outCount] = staged;
        }

        (*outCount)++;
      }
      else
      {
        LOGGER_LOGW(tag, "%s editor walk truncated at %" PRIu32 " records", active->name,
                    *outCount);
        break;
      }
    }

    steps++;

    if (steps > ((capacity + 2u) * 3u))
    {
      LOGGER_LOGW(tag, "%s editor walk abandoned after %" PRIu32 " steps", active->name, steps);
      return ESP_FAIL;
    }

    vTaskDelay(1);

    result = ringFileIteratorPrevious(handle, &iterator, &entry);

    if (result == RING_FILE_STORE_ERR_ITER_INVALIDATED)
    {
      result = ringFileIteratorBegin(handle, &iterator, RING_FILE_ITER_NEWEST, NULL, &entry);
    }
  }

  if ((result != RING_FILE_STORE_ERR_ITER_END) && (result != RING_FILE_STORE_ERR_RING_EMPTY))
  {
    LOGGER_LOGW(tag, "%s editor walk stopped after %" PRIu32 " records: %s", active->name,
                *outCount, esp_err_to_name(result));
    return result;
  }

  return ESP_OK;
}

esp_err_t loggerStoreEditorSnapshot(loggerMeterRing_t ring, loggerStoreEditorMeta* outMeta,
                                    loggerStoreEditorRecord* outRecords, uint32_t maxRecords,
                                    uint32_t* outCount)
{
  if ((ring >= LOGGER_RING_COUNT) || (outMeta == NULL) || (outCount == NULL))
  {
    return ESP_ERR_INVALID_ARG;
  }

  memset(outMeta, 0, sizeof *outMeta);
  *outCount = 0u;

  if (rings[ring].handle == NULL)
  {
    return ESP_ERR_INVALID_STATE;
  }

  if (!storeTakeAccess(pdMS_TO_TICKS(60000)))
  {
    return ESP_ERR_INVALID_STATE;
  }

  MeterRingEntry* entries = calloc(EDITOR_MAX_ENTRIES, sizeof *entries);
  bool* stagedFlags = calloc(EDITOR_MAX_ENTRIES, sizeof *stagedFlags);
  uint64_t* expectedKeys = calloc(EDITOR_MAX_ENTRIES, sizeof *expectedKeys);
  esp_err_t outcome = ESP_ERR_NO_MEM;

  if ((entries != NULL) && (stagedFlags != NULL) && (expectedKeys != NULL))
  {
    uint32_t capacity = 0u;
    outcome =
        editorWalkRing(ring, entries, stagedFlags, EDITOR_MAX_ENTRIES, &outMeta->count, &capacity);
    outMeta->capacity = capacity;
    outMeta->storeRunning = true;

    uint64_t timeStamp = 0u;
    outMeta->clockSynced = currentTimeStamp(&timeStamp);

    if (outMeta->clockSynced)
    {
      (void)meterRingKey(&rings[ring], timeStamp, &outMeta->openPeriodKey);
    }

    uint64_t anchorKey =
        (outMeta->count > 0u) ? (entries[0].keys.usedKey % rings[ring].centuryModulus) : 0u;

    outMeta->anchorOk =
        editorExpectedSequence(ring, anchorKey, outMeta->count, expectedKeys,
                               outMeta->anchorProblem, sizeof outMeta->anchorProblem);

    if ((outcome == ESP_OK) && (outMeta->count > maxRecords))
    {
      LOGGER_LOGW(tag, "%s editor snapshot truncated: %" PRIu32 " records beyond %" PRIu32,
                  rings[ring].name, outMeta->count - maxRecords, maxRecords);
      outMeta->count = maxRecords;
    }

    for (uint32_t index = 0u; (outcome == ESP_OK) && (index < outMeta->count); index++)
    {
      loggerStoreEditorRecord* row = &outRecords[index];
      const uint64_t key = entries[index].keys.usedKey % rings[ring].centuryModulus;

      memset(row, 0, sizeof *row);
      row->key = key;
      row->timeStamp = entries[index].keys.timeStamp;
      row->record = entries[index].meter;
      row->staged = stagedFlags[index];
      row->checksum = editorChecksum(&entries[index]);
      row->locked = (index == 0u) || (outMeta->clockSynced && (key == outMeta->openPeriodKey));
      row->keyOk = editorKeyLegal(ring, key);

      if (!outMeta->anchorOk)
      {
        //-- No repair preview without a trustworthy anchor: the row keeps
        //-- its key and the Save will only land when no row needs one.
        if (!row->keyOk)
        {
          snprintf(row->keyProblem, sizeof row->keyProblem,
                   "malformed period key: no trustworthy calendar anchor, repair blocked "
                   "(see toolbar note)");
        }
      }
      else if (!row->keyOk)
      {
        row->keyRepair = true;
        row->expectedKey = expectedKeys[index];
        snprintf(row->keyProblem, sizeof row->keyProblem,
                 "malformed period key: the Save will relabel it to %llu",
                 (unsigned long long)expectedKeys[index]);
      }
      else if (key != expectedKeys[index])
      {
        bool duplicate =
            ((index > 0u) && (entries[index - 1u].keys.usedKey == entries[index].keys.usedKey)) ||
            ((index + 1u < outMeta->count) &&
             (entries[index + 1u].keys.usedKey == entries[index].keys.usedKey));

        row->keyRepair = true;
        row->expectedKey = expectedKeys[index];
        snprintf(row->keyProblem, sizeof row->keyProblem,
                 "%s period key: the Save will relabel "
                 "it to %llu",
                 duplicate ? "duplicate" : "out of sequence",
                 (unsigned long long)expectedKeys[index]);
      }
    }

    *outCount = (outcome == ESP_OK) ? outMeta->count : 0u;
  }

  free(entries);
  free(stagedFlags);
  free(expectedKeys);
  storeReleaseAccess();

  return outcome;
}

//-- --------------------------------------------------------------------------
//-- The Save transaction
//-- --------------------------------------------------------------------------

typedef struct
{
  loggerMeterRing_t ring;
  const loggerStoreRowEdit* edits;
  uint32_t editCount;
  uint32_t snapshotCount;
  uint64_t snapshotNewestKey;
  char* outReason;
  size_t reasonSize;
  SemaphoreHandle_t done;
  esp_err_t outcome;
} EditorJob;

//-- One edit lookup result: the request entries for a row position.
typedef struct
{
  bool used;
  bool hasConsumption[LOGGER_STORE_EDITOR_CHANNELS];
  float consumption[LOGGER_STORE_EDITOR_CHANNELS];
  bool hasAmount[LOGGER_STORE_EDITOR_AMOUNTS];
  float amount[LOGGER_STORE_EDITOR_AMOUNTS];
} EditorRowEdits;

static void editorFail(EditorJob* job, esp_err_t err, const char* fmt, ...)
{
  va_list args;
  va_start(args, fmt);

  if ((job->outReason != NULL) && (job->reasonSize > 0u))
  {
    vsnprintf(job->outReason, job->reasonSize, fmt, args);
  }

  va_end(args);
  job->outcome = err;
}

//-- Rounds to the precision the ring stores (three decimals for the kWh
//-- and m³ counters, two for the EUR amounts).
static float editorRound3(float value)
{
  return nearbyintf(value * 1000.0f) / 1000.0f;
}

static float editorRound2(float value)
{
  return nearbyintf(value * 100.0f) / 100.0f;
}

//-- The whole Save, on the store task between two snapshots.
static void runEditorJob(EditorJob* job)
{
  MeterRing* active = &rings[job->ring];

  if (!storeTakeLocks(pdMS_TO_TICKS(60000)))
  {
    editorFail(job, ESP_ERR_INVALID_STATE,
               "another store writer (populate import or a Save) is still running; try again");
    return;
  }

  MeterRingEntry* entries = calloc(EDITOR_MAX_ENTRIES, sizeof *entries);
  EditorRowEdits* rowEdits = calloc(EDITOR_MAX_ENTRIES, sizeof *rowEdits);
  loggerMeterRecord* draft = calloc(EDITOR_MAX_ENTRIES, sizeof draft[0]);
  uint64_t* expectedKeys = calloc(EDITOR_MAX_ENTRIES, sizeof *expectedKeys);
  uint64_t* finalKeys = calloc(EDITOR_MAX_ENTRIES, sizeof *finalKeys);
  uint64_t* finalKeysTs = calloc(EDITOR_MAX_ENTRIES, sizeof finalKeysTs[0]);

  if ((entries == NULL) || (rowEdits == NULL) || (draft == NULL) || (expectedKeys == NULL) ||
      (finalKeys == NULL) || (finalKeysTs == NULL))
  {
    editorFail(job, ESP_ERR_NO_MEM, "the editor save ran out of memory");
    goto cleanup;
  }

  uint32_t capacity = 0u;
  uint32_t count = 0u;
  job->outcome = editorWalkRing(job->ring, entries, NULL, EDITOR_MAX_ENTRIES, &count, &capacity);

  if (job->outcome != ESP_OK)
  {
    editorFail(job, job->outcome, "the ring could not be read: %s", importErrName(job->outcome));
    goto cleanup;
  }

  if (count == 0u)
  {
    if (job->editCount > 0u)
    {
      editorFail(job, ESP_ERR_INVALID_STATE,
                 "the ring holds no records to edit; reload the editor");
      goto cleanup;
    }

    job->outcome = ESP_OK;
    goto cleanup;
  }

  //-- Stale snapshot guard: the record count and the newest key name the
  //-- ring the editor saw; a rollover or a new snapshot that moved either
  //-- means the positions (and the baselines the edits lean on) no longer
  //-- line up and the operator has to reload.
  if ((count != job->snapshotCount) ||
      ((entries[0].keys.usedKey % active->centuryModulus) != job->snapshotNewestKey))
  {
    editorFail(job, ESP_ERR_INVALID_STATE,
               "the ring changed while the page was open (new periods arrived or the ring "
               "wrapped); reload the editor and try again");
    goto cleanup;
  }

  //-- The key of the open period the logger keeps rewriting; the row that
  //-- carries it is locked exactly like the newest row.
  uint64_t openKey = 0u;
  uint64_t timeStampNow = 0u;

  if (currentTimeStamp(&timeStampNow))
  {
    (void)meterRingKey(active, timeStampNow, &openKey);
  }

  //-- Map the request onto rows and pin every edited row.
  for (uint32_t index = 0u; index < job->editCount; index++)
  {
    const loggerStoreRowEdit* edit = &job->edits[index];

    if (edit->index >= count)
    {
      editorFail(job, ESP_ERR_INVALID_ARG, "the request names row %lu beyond the ring",
                 (unsigned long)edit->index);
      goto cleanup;
    }

    if (rowEdits[edit->index].used)
    {
      editorFail(job, ESP_ERR_INVALID_ARG, "duplicate edit target for row %lu",
                 (unsigned long)edit->index);
      goto cleanup;
    }

    const uint64_t key = entries[edit->index].keys.usedKey % active->centuryModulus;

    if ((edit->index == 0u) || (key == openKey))
    {
      editorFail(job, ESP_ERR_INVALID_ARG,
                 "the newest (open-period) record is rewritten by the logger every minute and "
                 "cannot be edited");
      goto cleanup;
    }

    if (edit->checksum != editorChecksum(&entries[edit->index]))
    {
      editorFail(job, ESP_ERR_INVALID_STATE,
                 "record %llu changed since the snapshot; reload the editor and try again",
                 (unsigned long long)key);
      goto cleanup;
    }

    rowEdits[edit->index].used = true;

    for (uint32_t channel = 0u; channel < LOGGER_STORE_EDITOR_CHANNELS; channel++)
    {
      rowEdits[edit->index].hasConsumption[channel] = edit->hasConsumption[channel];
      rowEdits[edit->index].consumption[channel] = edit->consumption[channel];
    }

    for (uint32_t amount = 0u; amount < LOGGER_STORE_EDITOR_AMOUNTS; amount++)
    {
      rowEdits[edit->index].hasAmount[amount] = edit->hasAmount[amount];
      rowEdits[edit->index].amount[amount] = edit->amount[amount];
    }
  }

  //-- Value bounds of the entered numbers (finite, positive consumption,
  //-- bounded amounts, the decimal precision the ring stores).
  for (uint32_t index = 0u; index < count; index++)
  {
    if (!rowEdits[index].used)
    {
      continue;
    }

    for (uint32_t channel = 0u; channel < LOGGER_STORE_EDITOR_CHANNELS; channel++)
    {
      if (!rowEdits[index].hasConsumption[channel])
      {
        continue;
      }

      const float value = rowEdits[index].consumption[channel];

      if (!isfinite(value) || (value <= 0.0f) || (value > EDITOR_MAX_CONSUMPTION) ||
          (fabsf(value - editorRound3(value)) > 0.0002f))
      {
        editorFail(job, ESP_ERR_INVALID_ARG,
                   "row %lu %s: the entered use must be a positive number with at most three "
                   "decimals, up to %u",
                   (unsigned long)index, editorChannelName(channel),
                   (unsigned)EDITOR_MAX_CONSUMPTION);
        goto cleanup;
      }

      if (index + 1u >= count)
      {
        editorFail(job, ESP_ERR_INVALID_ARG,
                   "the oldest record has no older baseline, so its use cannot be edited");
        goto cleanup;
      }
    }

    for (uint32_t amount = 0u; amount < LOGGER_STORE_EDITOR_AMOUNTS; amount++)
    {
      if (!rowEdits[index].hasAmount[amount])
      {
        continue;
      }

      const float value = rowEdits[index].amount[amount];

      if (!isfinite(value) || (fabsf(value) > EDITOR_MAX_AMOUNT) ||
          (fabsf(value - editorRound2(value)) > 0.002f))
      {
        editorFail(job, ESP_ERR_INVALID_ARG,
                   "row %lu: a period amount must be a number with at most two decimals, "
                   "between -%.2f and %.2f",
                   (unsigned long)index, (double)EDITOR_MAX_AMOUNT, (double)EDITOR_MAX_AMOUNT);
        goto cleanup;
      }
    }
  }

  //-- The draft: copies of the stored payloads. Consumption edits are
  //-- applied newest to oldest per channel, so the result never depends on
  //-- the order the browser happened to receive the edits in and every
  //-- explicitly requested difference survives as long as the combined
  //-- draft is valid.
  for (uint32_t index = 0u; index < count; index++)
  {
    draft[index] = entries[index].meter;
  }

  for (uint32_t channel = 0u; channel < LOGGER_STORE_EDITOR_CHANNELS; channel++)
  {
    for (uint32_t index = 0u; index + 1u < count; index++)
    {
      if (!rowEdits[index].hasConsumption[channel])
      {
        continue;
      }

      //-- An edit to A's use keeps A's current draft reading and sets the
      //-- immediately older reading to A's reading minus the entered use.
      *editorChannelField(&draft[index + 1u], channel) =
          *editorChannelField(&draft[index], channel) - rowEdits[index].consumption[channel];
    }
  }

  for (uint32_t index = 0u; index < count; index++)
  {
    if (!rowEdits[index].used)
    {
      continue;
    }

    for (uint32_t amount = 0u; amount < LOGGER_STORE_EDITOR_AMOUNTS; amount++)
    {
      if (rowEdits[index].hasAmount[amount])
      {
        *editorAmountField(&draft[index], amount) = rowEdits[index].amount[amount];
      }
    }
  }

  //-- Convert to the persisted representation first, then validate: what
  //-- passes here is exactly what lands on storage.
  for (uint32_t index = 0u; index < count; index++)
  {
    for (uint32_t channel = 0u; channel < LOGGER_STORE_EDITOR_CHANNELS; channel++)
    {
      *editorChannelField(&draft[index], channel) =
          editorRound3(*editorChannelField(&draft[index], channel));
    }

    for (uint32_t amount = 0u; amount < LOGGER_STORE_EDITOR_AMOUNTS; amount++)
    {
      *editorAmountField(&draft[index], amount) =
          editorRound2(*editorAmountField(&draft[index], amount));
    }
  }

  for (uint32_t channel = 0u; channel < LOGGER_STORE_EDITOR_CHANNELS; channel++)
  {
    bool recorded = false;

    for (uint32_t index = 0u; (index < count) && !recorded; index++)
    {
      recorded = (*editorChannelFieldConst(&draft[index], channel) != 0.0f);
    }

    if (!recorded)
    {
      //-- A channel no record ever used (an unused tariff register, a meter
      //-- without gas) is not a recorded channel: equal zeros stay valid
      //-- and the view shows the channel as unavailable.
      continue;
    }

    for (uint32_t index = 0u; index + 1u < count; index++)
    {
      const float newer = *editorChannelFieldConst(&draft[index], channel);
      const float older = *editorChannelFieldConst(&draft[index + 1u], channel);
      const uint64_t newerKey = entries[index].keys.usedKey % active->centuryModulus;
      const uint64_t olderKey = entries[index + 1u].keys.usedKey % active->centuryModulus;

      if (older < 0.0f)
      {
        editorFail(job, ESP_ERR_INVALID_ARG,
                   "%s: the reading of %llu would become negative (a meter reset or a "
                   "discontinuity is not allowed here)",
                   editorChannelName(channel), (unsigned long long)olderKey);
        goto cleanup;
      }

      if (!(older < newer))
      {
        editorFail(job, ESP_ERR_INVALID_ARG,
                   "%s: the reading of %llu must stay lower than the reading of %llu "
                   "(cumulative readings are strictly descending; equal readings, zero-use "
                   "periods included, are invalid)",
                   editorChannelName(channel), (unsigned long long)olderKey,
                   (unsigned long long)newerKey);
        goto cleanup;
      }
    }
  }

  //-- The key plan: with a trustworthy anchor the candidate takes the
  //-- expected sequence (a row whose key already matches keeps its exact
  //-- stored timestamp, a mismatching or malformed key is relabeled);
  //-- without one the Save may only land when every stored key is legal
  //-- and unique, keeping the ring as it is.
  const uint64_t anchorKey = entries[0].keys.usedKey % active->centuryModulus;
  char anchorReason[96] = "";
  const bool anchorOk = editorExpectedSequence(job->ring, anchorKey, count, expectedKeys,
                                               anchorReason, sizeof anchorReason);

  if (anchorOk)
  {
    for (uint32_t index = 0u; index < count; index++)
    {
      const uint64_t key = entries[index].keys.usedKey % active->centuryModulus;

      if (key == expectedKeys[index])
      {
        finalKeysTs[index] = entries[index].keys.timeStamp;
      }
      else if (!editorKeyToTimeStamp(job->ring, expectedKeys[index], &finalKeysTs[index]))
      {
        editorFail(job, ESP_ERR_INVALID_ARG, "the proposed key %llu is not a legal period",
                   (unsigned long long)expectedKeys[index]);
        goto cleanup;
      }

      finalKeys[index] = expectedKeys[index];
    }
  }
  else
  {
    bool blocked = false;

    for (uint32_t index = 0u; (index < count) && !blocked; index++)
    {
      const uint64_t key = entries[index].keys.usedKey % active->centuryModulus;
      finalKeys[index] = key;
      finalKeysTs[index] = entries[index].keys.timeStamp;

      if (!editorKeyLegal(job->ring, key))
      {
        editorFail(job, ESP_ERR_INVALID_ARG,
                   "record %llu carries a malformed period key and the calendar anchor is "
                   "unresolved (%s): the Save is blocked until the keys can be repaired",
                   (unsigned long long)key, anchorReason);
        blocked = true;
        break;
      }

      if ((index > 0u) && (key == finalKeys[index - 1u]))
      {
        editorFail(job, ESP_ERR_INVALID_ARG,
                   "duplicate period key %llu cannot be published without a repair: reload "
                   "the editor once the clock is trustworthy",
                   (unsigned long long)key);
        blocked = true;
        break;
      }
    }

    if (blocked)
    {
      goto cleanup;
    }
  }

  //-- Nothing changed at all: answer success without touching storage.
  bool changed = false;

  for (uint32_t index = 0u; (index < count) && !changed; index++)
  {
    changed = (memcmp(&draft[index], &entries[index].meter, sizeof draft[index]) != 0) ||
              (finalKeysTs[index] != entries[index].keys.timeStamp);
  }

  if (!changed)
  {
    job->outcome = ESP_OK;
    goto cleanup;
  }

  //-- The journal: a complete ring file beside the live one, built through
  //-- the ring library with the live ring's own schema and capacity,
  //-- records added oldest to newest (append order keeps the logical
  //-- order), flushed and closed.
  char journalPath[EDITOR_PATH_BYTES];
  char backupPath[EDITOR_PATH_BYTES];
  char lockPath[EDITOR_PATH_BYTES + 8];
  snprintf(journalPath, sizeof journalPath, "%s%s", active->path, EDITOR_JOURNAL_SUFFIX);
  snprintf(backupPath, sizeof backupPath, "%s%s", active->path, EDITOR_BACKUP_SUFFIX);

  (void)unlink(journalPath);
  snprintf(lockPath, sizeof lockPath, "%s.lck", journalPath);
  (void)unlink(lockPath);

  ringFileConfig_t journalConfig;
  memset(&journalConfig, 0, sizeof journalConfig);
  journalConfig.maxRecords = (capacity > 0u) ? capacity : active->maxRecords;
  journalConfig.schemaId = active->schemaId;
  journalConfig.recordSize = sizeof(loggerMeterRecord);
  journalConfig.persistMode = RING_FILE_PERSIST_IMMEDIATE;

  ringFileHandle_t* journal = NULL;
  esp_err_t result = ringFileCreate(journalPath, &journalConfig, &journal);

  if (result != ESP_OK)
  {
    editorFail(job, result, "the journal file could not be created: %s", importErrName(result));
    goto cleanup;
  }

  for (uint32_t index = count; index > 0u; index--)
  {
    result = ringFileAdd(journal, finalKeysTs[index - 1u], &draft[index - 1u]);

    if (result != ESP_OK)
    {
      break;
    }
  }

  ringFileClose(journal);
  journal = NULL;

  if (result != ESP_OK)
  {
    (void)unlink(journalPath);
    editorFail(job, result, "the journal ring could not be built: %s", importErrName(result));
    goto cleanup;
  }

  //-- Reopen and verify the complete logical contents before anything is
  //-- published: count, capacity and every key and payload byte.
  ringFileOpenConfig_t openConfig;
  memset(&openConfig, 0, sizeof openConfig);
  openConfig.expectedSchemaId = active->schemaId;
  openConfig.expectedRecordSize = sizeof(loggerMeterRecord);
  openConfig.readOnly = true;

  ringFileHandle_t* verified = NULL;
  result = ringFileOpen(journalPath, &openConfig, &verified);

  if (result != ESP_OK)
  {
    (void)unlink(journalPath);
    editorFail(job, result, "the journal ring could not be reopened for verification: %s",
               importErrName(result));
    goto cleanup;
  }

  ringFileInfo_t journalInfo;
  memset(&journalInfo, 0, sizeof journalInfo);
  bool verifiedOk = (ringFileGetInfo(verified, &journalInfo) == ESP_OK) &&
                    (journalInfo.count == count) &&
                    (journalInfo.capacity == journalConfig.maxRecords);

  if (verifiedOk)
  {
    MeterRingEntry checkEntry;
    ringFileIterator_t checkIterator;
    memset(&checkEntry, 0, sizeof checkEntry);
    memset(&checkIterator, 0, sizeof checkIterator);

    result =
        ringFileIteratorBegin(verified, &checkIterator, RING_FILE_ITER_NEWEST, NULL, &checkEntry);
    uint32_t index = 0u;

    while ((result == ESP_OK) && (index < count))
    {
      //-- The stored usedKey is the ring's own truncation of the full
      //-- timestamp (century included): let the component derive it so the
      //-- check matches the schema's digits for every granularity.
      uint64_t expectedUsedKey = 0u;

      if ((ringFileTimeStampToUsedKey(finalKeysTs[index], editorGranularity(job->ring),
                                      &expectedUsedKey) != ESP_OK) ||
          (checkEntry.keys.usedKey != expectedUsedKey) ||
          (memcmp(&checkEntry.meter, &draft[index], sizeof checkEntry.meter) != 0))
      {
        verifiedOk = false;
        break;
      }

      result = ringFileIteratorPrevious(verified, &checkIterator, &checkEntry);
      index++;
    }

    if ((index != count) || ((result != RING_FILE_STORE_ERR_ITER_END) && (result != ESP_OK)))
    {
      verifiedOk = false;
    }
  }

  ringFileClose(verified);

  if (!verifiedOk)
  {
    (void)unlink(journalPath);
    editorFail(job, ESP_FAIL, "the journal ring did not verify on storage; nothing was changed");
    goto cleanup;
  }

  //-- The publish, still on the store task between two snapshots: flush
  //-- and close the live handle, then the two renames. Each crash moment
  //-- leaves a state startup recovery resolves to the old or the new ring.
  (void)ringFileSync(active->handle);
  ringFileClose(active->handle);
  active->handle = NULL;

  (void)unlink(backupPath);
  result = rename(active->path, backupPath);

  if (result != 0)
  {
    openMeterRing(active);
    editorFail(job, ESP_FAIL,
               "the live ring could not be moved aside (errno %d): the old ring "
               "stays in place",
               errno);
    goto cleanup;
  }

  result = rename(journalPath, active->path);

  if (result != 0)
  {
    (void)rename(backupPath, active->path);
    openMeterRing(active);
    editorFail(job, ESP_FAIL,
               "the new ring could not be published (errno %d): the old ring was restored", errno);
    goto cleanup;
  }

  if (!openMeterRing(active))
  {
    (void)unlink(active->path);
    (void)rename(backupPath, active->path);
    (void)openMeterRing(active);
    editorFail(job, ESP_FAIL,
               "the new ring could not be opened after the swap: the old ring was restored");
    goto cleanup;
  }

  (void)unlink(backupPath);

  LOGGER_LOGI(tag, "editor save published the %s ring: %" PRIu32 " record(s)", active->name, count);
  job->outcome = ESP_OK;

cleanup:
  free(entries);
  free(rowEdits);
  free(draft);
  free(expectedKeys);
  free(finalKeys);
  free(finalKeysTs);
  storeReleaseLocks();
}

esp_err_t loggerStoreApplyRingEdits(loggerMeterRing_t ring, const loggerStoreRowEdit* edits,
                                    uint32_t editCount, uint32_t snapshotCount,
                                    uint64_t snapshotNewestKey, char* outReason, size_t reasonSize)
{
  if ((ring >= LOGGER_RING_COUNT) || ((editCount > 0u) && (edits == NULL)))
  {
    return ESP_ERR_INVALID_ARG;
  }

  if (editCount > LOGGER_STORE_EDITOR_MAX_EDITS)
  {
    return ESP_ERR_INVALID_ARG;
  }

  if (rings[ring].handle == NULL)
  {
    return ESP_ERR_INVALID_STATE;
  }

  if ((outReason != NULL) && (reasonSize > 0u))
  {
    outReason[0] = '\0';
  }

  if (editorQueue == NULL)
  {
    return ESP_ERR_INVALID_STATE;
  }

  SemaphoreHandle_t done = xSemaphoreCreateBinary();

  if (done == NULL)
  {
    return ESP_ERR_NO_MEM;
  }

  EditorJob job;
  memset(&job, 0, sizeof job);
  job.ring = ring;
  job.edits = edits;
  job.editCount = editCount;
  job.snapshotCount = snapshotCount;
  job.snapshotNewestKey = snapshotNewestKey;
  job.outReason = outReason;
  job.reasonSize = reasonSize;
  job.done = done;
  job.outcome = ESP_FAIL;

  EditorJob* editorJob = &job;

  if (xQueueSend(editorQueue, &editorJob, pdMS_TO_TICKS(5000)) != pdTRUE)
  {
    vSemaphoreDelete(done);
    return ESP_ERR_INVALID_STATE;
  }

  if (storeWakeSem != NULL)
  {
    xSemaphoreGive(storeWakeSem);
  }

  const bool answered = (xSemaphoreTake(done, pdMS_TO_TICKS(120000)) == pdTRUE);
  vSemaphoreDelete(done);

  if (!answered)
  {
    if ((outReason != NULL) && (reasonSize > 0u))
    {
      snprintf(outReason, reasonSize, "the store task did not answer the save in time");
    }

    return ESP_ERR_TIMEOUT;
  }

  return job.outcome;
}

//-- Resolves the volume companions a Save left behind (see the swap in
//-- runEditorJob): backup + journal means the swap never completed (undo
//-- to the old ring), a lone backup means the new ring was published
//-- (drop the old copy), a lone journal was never published (drop it).
//-- Runs at startup before any ring opens.
static void recoverPendingRingEdits(void)
{
  for (loggerMeterRing_t ring = LOGGER_RING_HOURS; ring < LOGGER_RING_COUNT; ring++)
  {
    char journalPath[EDITOR_PATH_BYTES];
    char backupPath[EDITOR_PATH_BYTES];
    snprintf(journalPath, sizeof journalPath, "%s%s", rings[ring].path, EDITOR_JOURNAL_SUFFIX);
    snprintf(backupPath, sizeof backupPath, "%s%s", rings[ring].path, EDITOR_BACKUP_SUFFIX);

    struct stat details;
    const bool journal = (stat(journalPath, &details) == 0);
    const bool backup = (stat(backupPath, &details) == 0);

    if (backup && journal)
    {
      //-- Interrupted between the two renames: undo back to the old ring.
      //-- The rename replaces the live file in one step, so a failing
      //-- restore never leaves the ring without a file on disk.
      if (rename(backupPath, rings[ring].path) == 0)
      {
        LOGGER_LOGW(tag, "reverted the interrupted editor save of the %s ring", rings[ring].name);
      }
      else
      {
        LOGGER_LOGE(tag, "could not restore the %s ring backup (errno %d)", rings[ring].name,
                    errno);
      }

      (void)unlink(journalPath);
    }
    else if (backup)
    {
      struct stat liveDetails;

      if (stat(rings[ring].path, &liveDetails) != 0)
      {
        //-- The live file is gone while its retained copy stayed: put the
        //-- copy back rather than let the ring open (and grow) as an empty
        //-- one — recovery never replaces a ring with nothing.
        if (rename(backupPath, rings[ring].path) == 0)
        {
          LOGGER_LOGW(tag, "restored the %s ring from its retained copy", rings[ring].name);
        }
        else
        {
          LOGGER_LOGE(tag, "could not restore the %s ring copy (errno %d)", rings[ring].name,
                      errno);
        }
      }
      else
      {
        //-- The new ring is in place; the retained copy can go.
        (void)unlink(backupPath);
      }
    }
    else if (journal)
    {
      (void)unlink(journalPath);
    }
  }
}

//-- --------------------------------------------------------------------------
//-- Populate import

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
  IMPORT_FIELD_AMOUNT_IMPORT,
  IMPORT_FIELD_AMOUNT_EXPORT,
  IMPORT_FIELD_AMOUNT_GAS,
  IMPORT_FIELD_AMOUNT_NETWORK
} ImportField;

//-- Header column name to payload field. The counters are named by their
//-- OBIS code (without method suffix); the gas counter is accepted under the
//-- catalogue code 0-1:24.1.0 and under the Dutch and Belgian publishing
//-- codes alike. The three tariff amounts have no OBIS code of their own and
//-- carry the same rubrics the ring print uses, named for the direction of
//-- the energy they value: ti (import), te (export) and tg (gas); the
//-- network and transport month amount carries the rubric nc. A line that
//-- leaves one of these four values blank (or a file whose header omits the
//-- column) gets it computed from the stored settings instead of imported
//-- as 0.0 (see importRingFile).
static const struct
{
  const char* obis;
  ImportField field;
} importObisMap[] = {
    {"1-0:1.8.1", IMPORT_FIELD_DELIVERED_1}, {"1-0:1.8.2", IMPORT_FIELD_DELIVERED_2},
    {"1-0:2.8.1", IMPORT_FIELD_RETURNED_1},  {"1-0:2.8.2", IMPORT_FIELD_RETURNED_2},
    {"0-1:24.1.0", IMPORT_FIELD_GAS},        {"0-1:24.2.1", IMPORT_FIELD_GAS},
    {"0-1:24.2.3", IMPORT_FIELD_GAS},        {"ti", IMPORT_FIELD_AMOUNT_IMPORT},
    {"te", IMPORT_FIELD_AMOUNT_EXPORT},      {"tg", IMPORT_FIELD_AMOUNT_GAS},
    {"nc", IMPORT_FIELD_AMOUNT_NETWORK},
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
    LOGGER_LOGW(tag, "populate could not open %s: errno %d", path, errno);
    snprintf(result->firstError, sizeof result->firstError, "cannot open '%.60s' (errno %d)",
             relativePath, errno);
    return ESP_ERR_NOT_FOUND;
  }

  ImportField fieldOfColumn[STORE_IMPORT_MAX_COLUMNS];

  //-- 0xFF fills every enum-sized entry with -1 (IMPORT_FIELD_NONE).
  memset(fieldOfColumn, 0xFF, sizeof fieldOfColumn);

  //-- The stored settings name the tariffs and the network amount that
  //-- value every line whose ti, te, tg or nc value is blank or missing:
  //-- the manual tariff 1 prices (the way a meter that names no active
  //-- tariff prices) and the network and transport month amount prorated
  //-- to the ring's period. Read once for the whole import.
  loggerSettingsConfig settings;
  bool haveSettings = (loggerSettingsLoad(&settings) == ESP_OK);

  if (!haveSettings)
  {
    memset(&settings, 0, sizeof settings);
  }

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

      uint8_t mappedColumns = 0u;

      for (uint8_t index = 1u; index < columnCount; index++)
      {
        fieldOfColumn[index] = importFieldForColumn(fields[index]);

        if (fieldOfColumn[index] != IMPORT_FIELD_NONE)
        {
          mappedColumns++;
        }
      }

      //-- A header that names no known value column is almost always a
      //-- hand-written file using its own rubrics (EDT1, GDT, ...): the
      //-- import still lands every period, but every counter as 0.0 (the
      //-- four amounts are still computed from the settings). Warn loudly
      //-- instead of letting a ring silently fill with zeros.
      if (mappedColumns == 0u)
      {
        LOGGER_LOGW(tag,
                    "populate %s ring from %s: no value column of the header is known, every "
                    "counter imports as 0.0 (ti, te, tg and nc are computed from the settings)",
                    rings[ring].name, path);

        if (result->firstError[0] == '\0')
        {
          snprintf(result->firstError, sizeof result->firstError,
                   "header names no known value column (expected OBIS codes and ti/te/tg/nc)");
        }
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

    //-- The four amount columns the line may leave blank: a blank (or a
    //-- header without the column) is not a value and gets computed from
    //-- the settings below instead of staying 0.0.
    bool hasImportAmount = false;
    bool hasExportAmount = false;
    bool hasGasAmount = false;
    bool hasNetworkAmount = false;

    for (uint8_t index = 1u; index < columnCount; index++)
    {
      bool fieldGiven = (fields[index][0] != '\0');

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
      case IMPORT_FIELD_AMOUNT_IMPORT:
        record.tariffAmountImportEuro = (float)atof(fields[index]);
        hasImportAmount = fieldGiven;
        break;
      case IMPORT_FIELD_AMOUNT_EXPORT:
        record.tariffAmountExportEuro = (float)atof(fields[index]);
        hasExportAmount = fieldGiven;
        break;
      case IMPORT_FIELD_AMOUNT_GAS:
        record.tariffAmountGasEuro = (float)atof(fields[index]);
        hasGasAmount = fieldGiven;
        break;
      case IMPORT_FIELD_AMOUNT_NETWORK:
        record.networkCostsEuro = (float)atof(fields[index]);
        hasNetworkAmount = fieldGiven;
        break;
      default:
        break;
      }
    }

    //-- The values the file did not carry are stored anyway (the Costs
    //-- presentation needs them for imported history): the tariff amounts
    //-- as the average tariffs of the manual tariff 1 prices (an imported
    //-- line names no tariff indicator, so it prices like tariff 1) and
    //-- the network amount as the month amount prorated to this ring.
    if (!hasImportAmount)
    {
      record.tariffAmountImportEuro = settings.tariff1ImportPricePerKwh;
    }

    if (!hasExportAmount)
    {
      record.tariffAmountExportEuro = settings.tariff1ExportPricePerKwh;
    }

    if (!hasGasAmount)
    {
      record.tariffAmountGasEuro = settings.tariffGasPricePerM3;
    }

    if (!hasNetworkAmount)
    {
      record.networkCostsEuro = networkCostsForRing(ring, settings.networkCostsPerMonthEuro);
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

  LOGGER_LOGI(
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

  //-- The import is a whole-ring writer too: it never runs alongside an
  //-- editor Save (writer lock) and never against a handle that one is
  //-- about to close (access lock).
  if (!storeTakeLocks(pdMS_TO_TICKS(60000)))
  {
    job->outcome = ESP_ERR_INVALID_STATE;
    snprintf(job->result->firstError, sizeof job->result->firstError,
             "the ring is busy (an editor save is publishing); try again");
    xSemaphoreGive(job->done);
    vTaskDelete(NULL);
    return;
  }

  job->outcome = importRingFile(job->ring, job->relativePath, job->mode, job->result);
  storeReleaseLocks();
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
    LOGGER_LOGE(tag, "could not create the ring import task");
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
//-- gas code is accepted for all published gas codes alike), the three
//-- tariff amounts under the rubrics the ring print uses, named for the
//-- direction of the energy they value (ti, te and tg), and the network
//-- and transport amount under the rubric nc.
#define STORE_EXPORT_HEADER                                                                        \
  "YYMMDDHH;1-0:1.8.1*255;1-0:1.8.2*255;1-0:2.8.1*255;1-0:2.8.2*255;0-1:24.1.0*255;ti;te;tg;"      \
  "nc\r\n"

//-- Default csv name of a ring2csv export: the wall-clock stamp YYMMDDhhmm
//-- of the export moment plus the ring selector ("2611240730-hours.csv"),
//-- so consecutive exports of one ring never overwrite each other on the
//-- volume. A wall clock that never synchronised stamps all digits zero.
static void exportDefaultName(loggerMeterRing_t ring, char* outName, size_t outSize)
{
  char stamp[16] = "0000000000";
  time_t now = time(NULL);
  struct tm brokenTime;

  if ((now >= (time_t)STORE_CLOCK_SYNCED_EPOCH) && (localtime_r(&now, &brokenTime) != NULL))
  {
    strftime(stamp, sizeof stamp, "%y%m%d%H%M", &brokenTime);
  }

  snprintf(outName, outSize, "%s-%s.csv", stamp, loggerStoreRingName(ring));
}

//-- Walks the ring newest first into a buffer (deduplicating a cursor that
//-- is invalidated by a concurrent snapshot), then writes the rows: key as
//-- eight YYMMDDHH digits, the five counters at three decimals and the three
//-- tariff amounts and the network costs at two. An empty ring still produces the
//-- header line. The caller may hand an already open stream (the in-memory
//-- csv text of the API); with file NULL the volume file is opened and
//-- closed here like the console's ring2csv always did: relativeName names
//-- it at the volume root (already validated by the caller; NULL keeps the
//-- default YYMMDDhhmm-<name>.csv) and newestFirst picks newest period
//-- first, the
//-- spelling ring2csv writes, while the API download keeps oldest first.
static esp_err_t exportRingFile(loggerMeterRing_t ring, FILE* file, const char* relativeName,
                                bool newestFirst, uint32_t* outRecords)
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
      LOGGER_LOGW(tag, "%s ring export walk abandoned after %" PRIu32 " steps", active->name,
                  steps);
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
    LOGGER_LOGW(tag, "%s ring export walk stopped after %" PRIu32 " records: %s", active->name,
                collected, esp_err_to_name(result));
    return result;
  }

  bool ownFile = (file == NULL);
  char path[STORE_IMPORT_PATH_BYTES];
  path[0] = '\0';

  if (ownFile)
  {
    if (relativeName != NULL)
    {
      snprintf(path, sizeof path, STORE_MOUNT_POINT "/%s", relativeName);
    }
    else
    {
      //-- The generated name is at most stamp + '-' + selector + ".csv";
      //-- the modest buffer keeps the mount-point concatenation provably
      //-- safe against the path size.
      char defaultName[64];
      exportDefaultName(ring, defaultName, sizeof defaultName);
      snprintf(path, sizeof path, STORE_MOUNT_POINT "/%s", defaultName);
    }

    file = fopen(path, "w");

    if (file == NULL)
    {
      LOGGER_LOGW(tag, "ring2csv could not open %s: errno %d", path, errno);
      return ESP_FAIL;
    }
  }

  fprintf(file, STORE_EXPORT_HEADER);

  //-- The buffer holds the records newest first (the walk began at the
  //-- newest): ring2csv writes them as they came, the API download walks the
  //-- buffer backwards so its file stays oldest first.
  for (uint32_t index = 0u; index < collected; index++)
  {
    const MeterRingEntry* record = &entries[newestFirst ? index : (collected - 1u - index)];

    //-- Eight plain YYMMDDHH digits: strip the component's minute/second
    //-- digits and the century from the full timestamp.
    uint64_t key = (record->keys.timeStamp / 10000ULL) % 100000000ULL;

    fprintf(file, "%08" PRIu64 ";%.3f;%.3f;%.3f;%.3f;%.3f;%.2f;%.2f;%.2f;%.2f\r\n", key,
            record->meter.energyDeliveredTariff1Kwh, record->meter.energyDeliveredTariff2Kwh,
            record->meter.energyReturnedTariff1Kwh, record->meter.energyReturnedTariff2Kwh,
            record->meter.gasDeliveredM3, record->meter.tariffAmountImportEuro,
            record->meter.tariffAmountExportEuro, record->meter.tariffAmountGasEuro,
            record->meter.networkCostsEuro);
  }

  if (ownFile)
  {
    fclose(file);
  }

  if (outRecords != NULL)
  {
    *outRecords = collected;
  }

  if (ownFile)
  {
    LOGGER_LOGI(tag, "ring2csv wrote %" PRIu32 " record(s) of the %s ring to %s", collected,
                active->name, path);
  }

  return ESP_OK;
}

typedef struct
{
  loggerMeterRing_t ring;
  FILE* file;
  const char* relativeName;
  bool newestFirst;
  uint32_t* outRecords;
  SemaphoreHandle_t done;
  esp_err_t outcome;
} ExportJob;

static void exportRingTask(void* argument)
{
  ExportJob* job = (ExportJob*)argument;

  //-- The export walk rides the shared handle: keep it off a handle an
  //-- editor Save is closing for its rename swap.
  if (!storeTakeAccess(pdMS_TO_TICKS(60000)))
  {
    job->outcome = ESP_ERR_INVALID_STATE;
    xSemaphoreGive(job->done);
    vTaskDelete(NULL);
    return;
  }

  job->outcome =
      exportRingFile(job->ring, job->file, job->relativeName, job->newestFirst, job->outRecords);
  storeReleaseAccess();
  xSemaphoreGive(job->done);
  vTaskDelete(NULL);
}

//-- Shared task dance of both exports: the walk runs on its own task (the
//-- component asks for a generous stack for any task driving the ring
//-- cursor) while the calling task waits on the semaphore.
static esp_err_t runExportJob(loggerMeterRing_t ring, FILE* file, const char* relativeName,
                              bool newestFirst, uint32_t* outRecords)
{
  SemaphoreHandle_t done = xSemaphoreCreateBinary();

  if (done == NULL)
  {
    return ESP_ERR_NO_MEM;
  }

  ExportJob job;
  job.ring = ring;
  job.file = file;
  job.relativeName = relativeName;
  job.newestFirst = newestFirst;
  job.outRecords = outRecords;
  job.done = done;
  job.outcome = ESP_FAIL;

  if (xTaskCreate(exportRingTask, "ringExport", STORE_EXPORT_TASK_STACK_BYTES, &job,
                  STORE_EXPORT_TASK_PRIORITY, NULL) != pdPASS)
  {
    vSemaphoreDelete(done);
    LOGGER_LOGE(tag, "could not create the ring export task");
    return ESP_ERR_NO_MEM;
  }

  xSemaphoreTake(done, portMAX_DELAY);
  vSemaphoreDelete(done);

  return job.outcome;
}

//-- Accepted volume name of a ring2csv export: a plain file name at the
//-- volume root, never empty, never hidden, never climbing out of the
//-- volume. A missing ".csv" suffix is appended so every export stays a csv
//-- the populate command will accept. The final name lands in outName.
static bool exportNameUsable(const char* requested, char* outName, size_t outSize)
{
  if ((requested == NULL) || (outName == NULL) || (outSize == 0u) || (requested[0] == '\0') ||
      (requested[0] == '.') || (strchr(requested, '/') != NULL) ||
      (strchr(requested, '\\') != NULL) || (strstr(requested, "..") != NULL))
  {
    return false;
  }

  size_t length = strlen(requested);

  if ((length >= 4u) && (strcasecmp(requested + length - 4u, ".csv") == 0))
  {
    if (length >= outSize)
    {
      return false;
    }

    snprintf(outName, outSize, "%s", requested);
  }
  else
  {
    if ((length + 4u) >= outSize)
    {
      return false;
    }

    snprintf(outName, outSize, "%s.csv", requested);
  }

  return true;
}

esp_err_t loggerStoreRingToCsv(loggerMeterRing_t ring, uint32_t* outRecords)
{
  return loggerStoreRingToCsvNamed(ring, NULL, outRecords, NULL, 0u);
}

esp_err_t loggerStoreRingToCsvNamed(loggerMeterRing_t ring, const char* relativePath,
                                    uint32_t* outRecords, char* outFileName, size_t outFileNameSize)
{
  if ((ring < LOGGER_RING_HOURS) || (ring >= LOGGER_RING_COUNT))
  {
    return ESP_ERR_INVALID_ARG;
  }

  if (outFileName != NULL)
  {
    outFileName[0] = '\0';
  }

  if (outRecords != NULL)
  {
    *outRecords = 0u;
  }

  if (rings[ring].handle == NULL)
  {
    return ESP_ERR_INVALID_STATE;
  }

  char name[STORE_IMPORT_PATH_BYTES];

  if ((relativePath != NULL) && (relativePath[0] != '\0'))
  {
    if (!exportNameUsable(relativePath, name, sizeof name))
    {
      return ESP_ERR_INVALID_ARG;
    }
  }
  else
  {
    //-- Default export name: the wall-clock stamp plus the ring selector,
    //-- so exports land side by side instead of overwriting one file.
    exportDefaultName(ring, name, sizeof name);
  }

  if ((outFileName != NULL) && (outFileNameSize > 0u))
  {
    snprintf(outFileName, outFileNameSize, "%s", name);
  }

  //-- The volume file is written newest period first (the console's ring2csv
  //-- spelling); the import accepts either order.
  return runExportJob(ring, NULL, name, true, outRecords);
}

esp_err_t loggerStoreRingToCsvText(loggerMeterRing_t ring, char** outText, uint32_t* outRecords)
{
  if ((ring < LOGGER_RING_HOURS) || (ring >= LOGGER_RING_COUNT) || (outText == NULL))
  {
    return ESP_ERR_INVALID_ARG;
  }

  *outText = NULL;

  if (outRecords != NULL)
  {
    *outRecords = 0u;
  }

  if (rings[ring].handle == NULL)
  {
    return ESP_ERR_INVALID_STATE;
  }

  //-- The memstream grows on demand; fclose finalises it and the buffer
  //-- stays allocated for the caller to free().
  char* text = NULL;
  size_t textLength = 0u;
  FILE* stream = open_memstream(&text, &textLength);

  if (stream == NULL)
  {
    return ESP_ERR_NO_MEM;
  }

  //-- The download keeps its historic oldest-first row order (the volume
  //-- export prints newest first).
  esp_err_t outcome = runExportJob(ring, stream, NULL, false, outRecords);
  bool writeFailed = (ferror(stream) != 0);
  fclose(stream);

  if ((outcome != ESP_OK) || writeFailed || (text == NULL))
  {
    free(text);
    return (outcome != ESP_OK) ? outcome : ESP_ERR_NO_MEM;
  }

  *outText = text;
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
    LOGGER_LOGI(tag, "wall clock not synchronised yet, ring snapshot postponed");
    return false;
  }

  loggerMeterRecord record;
  uint8_t tariffIndicator = 0u;
  bool telegramSeen = readLatestMeterCounters(&record, &tariffIndicator);

  //-- The network and transport month amount stored with every record of
  //-- this snapshot, prorated to each ring's period below. An unreadable
  //-- settings block stores 0.0.
  loggerSettingsConfig snapshotSettings;

  if (loggerSettingsLoad(&snapshotSettings) != ESP_OK)
  {
    memset(&snapshotSettings, 0, sizeof snapshotSettings);
  }

  uint64_t hourKey = 0u;
  (void)meterRingKey(&rings[LOGGER_RING_HOURS], timeStamp, &hourKey);

  //-- The hours ring amounts name the tariffs in the one summary line.
  loggerMeterRecord logged = record;

  for (loggerMeterRing_t ring = LOGGER_RING_HOURS; ring < LOGGER_RING_COUNT; ring++)
  {
    //-- The tariff amounts belong to the ring's own period: every ring gets
    //-- its own copy of the snapshot for the calculator (or the demo
    //-- amounts while no calculator is registered).
    loggerMeterRecord ringRecord = record;
    uint64_t ringKey = 0u;
    (void)meterRingKey(&rings[ring], timeStamp, &ringKey);

    //-- The network amount belongs to the ring's own period: every ring
    //-- stores the month amount prorated to its granularity.
    ringRecord.networkCostsEuro =
        networkCostsForRing(ring, snapshotSettings.networkCostsPerMonthEuro);

    if (tariffCalculator != NULL)
    {
      tariffCalculator(ring, ringKey, tariffIndicator, &ringRecord);
    }
    else
    {
      //-- The fictional tariff amounts: a random few cents per amount.
      ringRecord.tariffAmountImportEuro =
          (float)(STORE_TARIFF_CENT_MIN + (esp_random() % STORE_TARIFF_CENT_SPAN)) / 100.0f;
      ringRecord.tariffAmountExportEuro =
          (float)(STORE_TARIFF_CENT_MIN + (esp_random() % STORE_TARIFF_CENT_SPAN)) / 100.0f;
      ringRecord.tariffAmountGasEuro =
          (float)(STORE_TARIFF_CENT_MIN + (esp_random() % STORE_TARIFF_CENT_SPAN)) / 100.0f;
    }

    esp_err_t result = ringFileUpsert(rings[ring].handle, timeStamp, &ringRecord);

    if (result != ESP_OK)
    {
      LOGGER_LOGW(tag, "%s ring record for key %" PRIu64 " refused: %s", rings[ring].name, ringKey,
                  esp_err_to_name(result));
    }

    if (ring == LOGGER_RING_HOURS)
    {
      logged = ringRecord;
    }
  }

  //-- One summary line per snapshot: the hour key leads, the counters keep
  //-- the units of the telegram (kWh and m³), the tariff amounts are in EUR
  //-- (import, export and gas), nc is the hour ring's share of the network
  //-- and transport month amount and the tariff is the indicator the meter
  //-- published (0 when it named none).
  LOGGER_LOGI(tag,
              "stored h=%08" PRIu64 " edT1=%8.3f edT2=%8.3f erT1=%8.3f erT2=%8.3f "
              "gas=%8.3f ti=%5.2f te=%5.2f tg=%5.2f nc=%6.3f tariff=%u%s",
              hourKey, record.energyDeliveredTariff1Kwh, record.energyDeliveredTariff2Kwh,
              record.energyReturnedTariff1Kwh, record.energyReturnedTariff2Kwh,
              record.gasDeliveredM3, logged.tariffAmountImportEuro, logged.tariffAmountExportEuro,
              logged.tariffAmountGasEuro, logged.networkCostsEuro, (unsigned)tariffIndicator,
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
      LOGGER_LOGW(tag, "could not synchronize the %s ring: %s", rings[ring].name,
                  esp_err_to_name(result));
    }
  }

  LOGGER_LOGI(tag, "hour %08" PRIu64 ": all ring files synchronized", hourKey);

  //-- The verbose console hears the same news the serial log gets.
  loggerControlConsoleEvent(LOGGER_VERBOSE_EVENTS, LOGGER_CONSOLE_EVENT,
                            "store: all ring files synchronized (hour %08" PRIu64 ")", hourKey);
}

static void storeTask(void* argument)
{
  (void)argument;

  if (!openMeterRings())
  {
    LOGGER_LOGE(tag, "meter store stays disabled");
    vTaskDelete(NULL);
    return;
  }

  //-- Time of the last synchronization and whether one has happened yet:
  //-- the very first snapshot is synchronized immediately (the behaviour
  //-- the old hour-change trigger gave), later ones at most once per
  //-- configured rfs synchronisation interval.
  uint64_t lastSyncMs = 0u;
  bool syncedOnce = false;

  for (;;)
  {
    //-- The minute tick is a semaphore timeout so the console's sync command
    //-- can wake the task early: a give makes the very next iteration run
    //-- (which, with the request flag set, also commits the rings).
    if (storeWakeSem != NULL)
    {
      xSemaphoreTake(storeWakeSem, pdMS_TO_TICKS(STORE_RECORD_INTERVAL_MS));
    }
    else
    {
      vTaskDelay(pdMS_TO_TICKS(STORE_RECORD_INTERVAL_MS));
    }

    //-- Editor Save jobs first: the store task runs the whole transaction
    //-- here, between two snapshots, so a publish never interleaves with
    //-- the minute's upsert.
    EditorJob* editorJob = NULL;

    while ((editorQueue != NULL) && (xQueueReceive(editorQueue, &editorJob, 0) == pdTRUE))
    {
      runEditorJob(editorJob);
      xSemaphoreGive(editorJob->done);
    }

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

    //-- The interval is re-read with every snapshot so a saved change takes
    //-- effect within one minute; an unusable stored value falls back to the
    //-- documented default.
    loggerSettingsConfig settings;

    if (loggerSettingsLoad(&settings) != ESP_OK)
    {
      memset(&settings, 0, sizeof settings);
      settings.rfsSyncIntervalMinutes = LOGGER_SETTINGS_RFS_SYNC_DEFAULT_MINUTES;
    }

    if ((settings.rfsSyncIntervalMinutes < LOGGER_SETTINGS_RFS_SYNC_MIN_MINUTES) ||
        (settings.rfsSyncIntervalMinutes > LOGGER_SETTINGS_RFS_SYNC_MAX_MINUTES))
    {
      settings.rfsSyncIntervalMinutes = LOGGER_SETTINGS_RFS_SYNC_DEFAULT_MINUTES;
    }

    uint64_t nowMs = (uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint64_t syncPeriodMs = (uint64_t)settings.rfsSyncIntervalMinutes * 60000u;

    //-- A console sync overrides the interval timer; the commit below then
    //-- restarts the interval from this moment (lastSyncMs is re-stamped).
    bool forcedSync = syncRequested;
    syncRequested = false;

    if (syncedOnce && !forcedSync && ((nowMs - lastSyncMs) < syncPeriodMs))
    {
      continue;
    }

    syncRings(hourKey);
    lastSyncMs = nowMs;
    syncedOnce = true;

    //-- The staged records have reached storage: release a waiter of the
    //-- console's restart (loggerStoreRequestSyncAndWait).
    if (syncDoneSem != NULL)
    {
      xSemaphoreGive(syncDoneSem);
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

bool loggerStoreRfsWearInfo(uint32_t* totalBlocks, uint32_t* blockWritesPerSync,
                            uint32_t* flashWriteCycles)
{
  if ((totalBlocks == NULL) || (blockWritesPerSync == NULL) || (flashWriteCycles == NULL))
  {
    return false;
  }

  size_t total = 0u;
  size_t used = 0u;

  if ((rings[LOGGER_RING_HOURS].handle == NULL) ||
      (esp_littlefs_info(STORE_PARTITION_LABEL, &total, &used) != ESP_OK) ||
      (total < STORE_LITTLEFS_BLOCK_SIZE))
  {
    return false;
  }

  *totalBlocks = (uint32_t)(total / STORE_LITTLEFS_BLOCK_SIZE);
  *blockWritesPerSync = (uint32_t)LOGGER_RING_COUNT *
                        (STORE_SYNC_DATA_BLOCKS_PER_RING + STORE_SYNC_METADATA_BLOCKS_PER_RING);
  *flashWriteCycles = STORE_FLASH_WRITE_CYCLES;
  return true;
}

esp_err_t loggerStoreListFiles(loggerStoreFileVisitor visitor, void* userData)
{
  if (visitor == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  if (loggerStoreMountPoint() == NULL)
  {
    return ESP_ERR_INVALID_STATE;
  }

  DIR* directory = opendir(STORE_MOUNT_POINT);

  if (directory == NULL)
  {
    LOGGER_LOGW(tag, "could not open the volume directory: errno %d", errno);
    return ESP_FAIL;
  }

  struct dirent* entry = NULL;

  while ((entry = readdir(directory)) != NULL)
  {
    if ((strcmp(entry->d_name, ".") == 0) || (strcmp(entry->d_name, "..") == 0))
    {
      continue;
    }

    char path[STORE_IMPORT_PATH_BYTES];
    snprintf(path, sizeof path, "%s/%s", STORE_MOUNT_POINT, entry->d_name);

    struct stat details;
    memset(&details, 0, sizeof details);

    if (stat(path, &details) != 0)
    {
      //-- Unreadable entry: still reported by name, size unknown.
      if (!visitor(entry->d_name, false, false, 0u, userData))
      {
        break;
      }

      continue;
    }

    if (!visitor(entry->d_name, S_ISDIR(details.st_mode), true, (uint32_t)details.st_size,
                 userData))
    {
      break;
    }
  }

  closedir(directory);
  return ESP_OK;
}

esp_err_t loggerStoreInit(void)
{
  //-- The wake source of the manual sync (created once, kept for life): a
  //-- give pulls the store task out of its minute sleep right away.
  if (storeWakeSem == NULL)
  {
    storeWakeSem = xSemaphoreCreateBinary();

    if (storeWakeSem == NULL)
    {
      LOGGER_LOGE(tag, "could not create the store wake semaphore");
      return ESP_ERR_NO_MEM;
    }
  }

  if (syncDoneSem == NULL)
  {
    syncDoneSem = xSemaphoreCreateBinary();

    if (syncDoneSem == NULL)
    {
      LOGGER_LOGE(tag, "could not create the store sync-done semaphore");
      return ESP_ERR_NO_MEM;
    }
  }

  //-- The editor transaction guards and the Save job queue (created once,
  //-- kept for life; see the RFS history editor section).
  if (storeAccessMutex == NULL)
  {
    storeAccessMutex = xSemaphoreCreateMutex();

    if (storeAccessMutex == NULL)
    {
      LOGGER_LOGE(tag, "could not create the store access mutex");
      return ESP_ERR_NO_MEM;
    }
  }

  if (storeWriterMutex == NULL)
  {
    storeWriterMutex = xSemaphoreCreateMutex();

    if (storeWriterMutex == NULL)
    {
      LOGGER_LOGE(tag, "could not create the store writer mutex");
      return ESP_ERR_NO_MEM;
    }
  }

  if (editorQueue == NULL)
  {
    editorQueue = xQueueCreate(4, sizeof(EditorJob*));

    if (editorQueue == NULL)
    {
      LOGGER_LOGE(tag, "could not create the editor save queue");
      return ESP_ERR_NO_MEM;
    }
  }

  BaseType_t created =
      xTaskCreate(storeTask, "meterStore", STORE_TASK_STACK_BYTES, NULL, STORE_TASK_PRIORITY, NULL);

  if (created != pdPASS)
  {
    LOGGER_LOGE(tag, "could not create the meter store task");
    return ESP_ERR_NO_MEM;
  }

  return ESP_OK;
}

bool loggerStoreRequestSync(void)
{
  //-- The store task must be up with its rings open for a commit to happen.
  if ((storeWakeSem == NULL) || (rings[LOGGER_RING_HOURS].handle == NULL))
  {
    return false;
  }

  syncRequested = true;
  xSemaphoreGive(storeWakeSem);
  return true;
}

bool loggerStoreRequestSyncAndWait(uint32_t timeoutMs)
{
  //-- Same entry conditions as the plain request; the completion token must
  //-- exist too (the store task gives it after every commit).
  if ((storeWakeSem == NULL) || (syncDoneSem == NULL) || (rings[LOGGER_RING_HOURS].handle == NULL))
  {
    return false;
  }

  //-- Drop a token an earlier commit may have left behind: only a commit
  //-- completing from this moment on may release the wait below.
  xSemaphoreTake(syncDoneSem, 0);

  if (!loggerStoreRequestSync())
  {
    return false;
  }

  //-- The store task wakes, takes one more snapshot and commits all three
  //-- rings; the token arrives when that commit has completed. A store task
  //-- that cannot deliver (wall clock not synchronised yet) only costs the
  //-- caller the timeout.
  return (xSemaphoreTake(syncDoneSem, pdMS_TO_TICKS(timeoutMs)) == pdTRUE);
}
