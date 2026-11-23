//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_store.h - hour, day and month smart meter ring file store
//-- Date: 2026-11-23 · Build: 12
//--
//-- Three fixed-capacity ring files land on the LittleFS storage partition,
//-- written through mrwheel's ring_file_store component: the hours ring
//-- (file type "YYMMDDhh", key "YYMMDDhh"), the days ring (file type
//-- "YYMMDD", key "YYMMDD") and the months ring (file type "YYMM", key
//-- "YYMM"). All three are opened buffered with a one-record staging
//-- buffer; the store task synchronizes the three files together at least
//-- once per configured rfs synchronisation interval (the one-minute
//-- snapshots of the same period coalesce in the staging buffers, so only
//-- the synchronisation touches storage). The component prepends its two automatic key fields
//-- (timeStamp and the period usedKey) to every record, so the caller's
//-- payload struct carries only the measurements.
//--
//-- The console's populate command imports historical periods from a
//-- semicolon separated csv file on the volume (the import files are
//-- shipped to the volume root by the storage partition image built from
//-- the project's storageImage/ folder) into one ring through
//-- ringFileEdit(), missing values becoming 0.0.
//-- ==========================================================================

#ifndef LOGGER_STORE_H
#define LOGGER_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

//-- --------------------------------------------------------------------------
//-- The three meter rings
//-- --------------------------------------------------------------------------

//-- One ring per aggregation period, ordered from the finest to the
//-- coarsest period.
typedef enum
{
  LOGGER_RING_HOURS = 0, //-- file type "YYMMDDhh", key "YYMMDDhh"
  LOGGER_RING_DAYS,      //-- file type "YYMMDD",   key "YYMMDD"
  LOGGER_RING_MONTHS,    //-- file type "YYMM",     key "YYMM"
  LOGGER_RING_COUNT
} loggerMeterRing_t;

//-- Lowercase selector name of a ring ("hours", "days" or "months"), the
//-- spelling the console and the SPA use. Index out of range: "unknown".
const char* loggerStoreRingName(loggerMeterRing_t ring);

//-- Maps a selector name back to its ring, case-insensitively. "month" is
//-- accepted as a synonym of "months". False (handle untouched) for NULL,
//-- the empty string and any other name.
bool loggerStoreRingFromName(const char* name, loggerMeterRing_t* outRing);

//-- --------------------------------------------------------------------------
//-- Stored period record (ring_file_store payload)
//-- --------------------------------------------------------------------------

//-- Meter counters of the most recently accepted telegram, in the units the
//-- P1 telegram publishes them (kWh and m³). The tariff amounts come from
//-- the registered tariff calculator (the tariff component's calculator
//-- values them with the dynamic tariff table and the manual prices, and
//-- picks the tariff 1 or tariff 2 price through the meter's tariff
//-- indicator 0-0:96.14.0); without a registered calculator they stay
//-- fictional billing figures: a random few cents per amount. The amounts
//-- follow the direction of the energy: the import amount is what the used
//-- energy costs, the export amount what the delivered-back energy earns
//-- (a credit), the gas amount what the consumed gas costs. The struct is
//-- stored as raw bytes: pointer-free fixed-width fields only, and any
//-- layout change rebuilds the ring files empty (the retained records of the
//-- old layout cannot be read back). The key fields are
//-- added by the ring_file_store component and are never part of this
//-- struct.
typedef struct
{
  float energyDeliveredTariff1Kwh;
  float energyDeliveredTariff2Kwh;
  float energyReturnedTariff1Kwh;
  float energyReturnedTariff2Kwh;
  float gasDeliveredM3;
  float tariffAmountImportEuro;
  float tariffAmountExportEuro;
  float tariffAmountGasEuro;
} loggerMeterRecord;

//-- --------------------------------------------------------------------------
//-- Tariff calculator
//-- --------------------------------------------------------------------------

//-- Fills the three tariff amounts of one ring record (import, export and
//-- gas). The store task calls it on the store task, once per ring per
//-- snapshot, with the freshly read counters, the period key of that ring
//-- and the tariff indicator the meter published with them (OBIS
//-- 0-0:96.14.0: 1 or 2; 0 when the meter names no active tariff, which
//-- prices like tariff 1). The calculator values the energy that grew inside
//-- this ring's period — the used energy against the import price, the
//-- delivered-back energy against the export price and the gas against the
//-- gas price — and writes the amounts back into the record; the tariff
//-- indicator picks which of the two tariff prices of a direction applies.
//-- The dynamic tariff table and the manual fallback prices are the
//-- calculator's own concern; the store only asks. Registering the tariff
//-- component's calculator replaces the fictional demo amounts the store
//-- would otherwise write.
typedef void (*loggerStoreTariffCalculator)(loggerMeterRing_t ring, uint64_t ringKey,
                                            uint8_t tariffIndicator, loggerMeterRecord* record);

//-- Registers (or with NULL, unregisters) the calculator used for every
//-- snapshot from now on. Called once at boot, before or after the store
//-- task starts; the pointer is read per snapshot.
void loggerStoreSetTariffCalculator(loggerStoreTariffCalculator calculator);

//-- --------------------------------------------------------------------------
//-- Operations
//-- --------------------------------------------------------------------------

//-- Starts the store task: it mounts the LittleFS storage partition,
//-- removes the ring file of the retired minute store, creates the three
//-- ring files on first boot (or reopens the retained ones after that),
//-- refreshes the current hour, day and month record every minute with the
//-- latest counters and synchronizes all three files at least once per
//-- configured rfs synchronisation interval.
//-- Returns ESP_ERR_NO_MEM when the task could not be created; storage
//-- problems are reported on the console by the task itself.
esp_err_t loggerStoreInit(void);

//-- Visitor for the newest-first ring walk: called once per retained record,
//-- newest period first, with the wall-clock period key without the
//-- component's century digits (hours "YYMMDDhh", days "YYMMDD", months
//-- "YYMM") and a pointer to the stored payload. The callback runs on the
//-- calling task; returning false stops the walk early.
typedef bool (*loggerStoreRecordVisitor)(uint64_t key, const loggerMeterRecord* record,
                                         void* userData);

//-- Walks one ring from the newest retained record to the oldest, handing
//-- every record to the visitor. Safe from any task (the store task
//-- included): ring_file_store serialises the operations on the shared
//-- handle and every traversal runs on its own cursor. Returns
//-- ESP_ERR_INVALID_ARG for an invalid ring, ESP_ERR_INVALID_STATE while
//-- the store is not running, another error when the walk could not
//-- complete, and ESP_OK otherwise (an empty ring simply never calls the
//-- visitor).
esp_err_t loggerStoreListRecords(loggerMeterRing_t ring, loggerStoreRecordVisitor visitor,
                                 void* userData);

//-- --------------------------------------------------------------------------
//-- Populate import
//-- --------------------------------------------------------------------------

//-- How a populate import treats the records the ring already holds.
typedef enum
{
  LOGGER_POPULATE_MERGE = 0, //-- keeps stored records and backfills every free
                             //-- slot of the retained window (the default)
  LOGGER_POPULATE_OVERWRITE, //-- clears the ring before the first data line,
                             //-- so the file alone names the stored periods
  LOGGER_POPULATE_UPDATE,    //-- only revises periods the ring already stores;
                             //-- keys it does not hold are refused
  LOGGER_POPULATE_COUNT
} loggerPopulateMode_t;

//-- Outcome counters of one populate import.
typedef struct
{
  uint32_t rowsRead;       //-- data lines seen after the header line
  uint32_t rowsRejected;   //-- lines without a usable period key
  uint32_t bucketsWritten; //-- edits the ring file accepted (insert or update)
  uint32_t bucketsRefused; //-- edits the ring file refused (e.g. out of window)
  char firstError[96];     //-- first refusal or failure, empty when none
} loggerStoreImportResult;

//-- Imports one semicolon separated csv file from the LittleFS volume into
//-- one meter ring (the console's populate command). The first non-blank
//-- line is the header: its first column names the period key ("YYMMDDHH",
//-- "YYMMDD" or "YYMM"), the remaining columns name the values. The five
//-- counters are OBIS codes: 1-0:1.8.1, 1-0:1.8.2, 1-0:2.8.1 and 1-0:2.8.2
//-- (kWh) and the gas counter 0-1:24.1.0 (also accepted as 0-1:24.2.1 or
//-- 0-1:24.2.3), all case-insensitive and with an optional "*255" method
//-- suffix. The three tariff amounts carry the rubrics of the ring print:
//-- ti (import), te (export) and tg (gas). Unknown columns - including the
//-- "#" and empty trailing
//-- columns of handwritten files - are skipped. Every data line starts with
//-- the period key and carries its values in header order; a blank or
//-- missing value becomes 0.0. The mode picks how the ring's current records are
//-- treated: MERGE backfills the retained window through ringFileEdit()
//-- (periods older than the ring capacity are refused and counted),
//-- OVERWRITE clears the ring just before the first valid data line so the
//-- file alone names the stored periods, and UPDATE only revises periods
//-- the ring already stores (every other key is refused as not stored). An
//-- import that never reaches a valid data line leaves the ring untouched
//-- in all three modes. The path is relative to the
//-- LittleFS mount point ("RINGmonths.csv", the import csv files land at
//-- the volume root with the storage partition image) and may not climb out
//-- of the volume. Returns ESP_ERR_INVALID_ARG for a missing argument, a bad
//-- mode or a bad header, ESP_ERR_NOT_FOUND when the file does not exist,
//-- ESP_ERR_INVALID_STATE while the store is not running, and ESP_OK when
//-- the file was processed (per-line outcomes land in result).
esp_err_t loggerStorePopulateRing(loggerMeterRing_t ring, const char* relativePath,
                                  loggerPopulateMode_t mode, loggerStoreImportResult* result);

//-- --------------------------------------------------------------------------
//-- Csv export (ring2csv)
//-- --------------------------------------------------------------------------

//-- Writes one meter ring back to the LittleFS volume as a semicolon
//-- separated csv file (the console's ring2csv command): the file lands at
//-- the volume root as "RING<hours|days|months>.csv", carries the header
//-- "YYMMDDHH;1-0:1.8.1*255;1-0:1.8.2*255;1-0:2.8.1*255;1-0:2.8.2*255;
//-- 0-1:24.1.0*255;ti;te;tg" and one line per retained record, oldest
//-- period first, with the key as eight digits, the five counters at three
//-- decimals and the three tariff amounts at two.
//-- The result re-imports through loggerStorePopulateRing() unchanged.
//-- outRecords (optional) receives the number of records written. Returns
//-- ESP_ERR_INVALID_ARG for an invalid ring, ESP_ERR_INVALID_STATE while the
//-- store is not running, an I/O error when the file could not be written
//-- and ESP_OK otherwise.
esp_err_t loggerStoreRingToCsv(loggerMeterRing_t ring, uint32_t* outRecords);

//-- Same csv content as the RING<hours|days|months>.csv export file, built
//-- in memory instead of written to the volume (the API's csv download of
//-- one ring): header line first, one line per retained record, oldest
//-- period first, formatted exactly like the file so the text re-imports
//-- through loggerStorePopulateRing() unchanged. *outText receives a
//-- heap-allocated NUL-terminated buffer that the caller releases with
//-- free(); it stays NULL on failure. outRecords (optional) receives the
//-- number of records rendered. Returns ESP_ERR_INVALID_ARG for an invalid
//-- ring or a null outText, ESP_ERR_INVALID_STATE while the store is not
//-- running, ESP_ERR_NO_MEM when the buffer could not be built and another
//-- error when the walk could not complete.
esp_err_t loggerStoreRingToCsvText(loggerMeterRing_t ring, char** outText, uint32_t* outRecords);

//-- Visitor of the LittleFS volume scan: called once per entry of the
//-- volume root. "readable" reports whether the size could be read (stat);
//-- unreadable entries carry sizeBytes 0 and directories too report their
//-- name only. The callback runs on the calling task; returning false stops
//-- the scan early.
typedef bool (*loggerStoreFileVisitor)(const char* name, bool isDirectory, bool readable,
                                       uint32_t sizeBytes, void* userData);

//-- Scans the files on the mounted LittleFS volume (the console's store
//-- command listing). Returns ESP_ERR_INVALID_ARG for a null visitor,
//-- ESP_ERR_INVALID_STATE while the volume is not mounted, ESP_FAIL when
//-- the volume directory could not be opened and ESP_OK otherwise.
esp_err_t loggerStoreListFiles(loggerStoreFileVisitor visitor, void* userData);

//-- VFS mount point of the LittleFS storage volume, published for the SMB
//-- network share (the component exposes an already mounted VFS path and
//-- the store task owns this mount). Returns the absolute mount point while
//-- the volume is mounted, and NULL before the store task mounted it or when
//-- the store stays disabled.
const char* loggerStoreMountPoint(void);

//-- Capacity statistics of the mounted LittleFS storage volume: total bytes
//-- and used bytes (the free space is the difference). Keeps the partition
//-- label private to this component. Returns false when the store is not
//-- running or the statistics could not be read; the outputs stay untouched
//-- in that case.
bool loggerStoreVolumeInfo(size_t* totalBytes, size_t* usedBytes);

//-- Rough LittleFS wear inputs for the settings view's lifetime estimate of
//-- the configured rfs synchronisation interval: the number of flash blocks
//-- on the volume, the number of block-sized writes one synchronisation is
//-- estimated to cause (staged slot images plus the file-system metadata
//-- they commit) and the program/erase cycle rating of one flash block.
//-- The estimate is deliberately pessimistic (uniform wear over all blocks
//-- assumed); the SPA words it as an estimate. Returns false when the
//-- volume is not mounted; the outputs stay untouched in that case.
bool loggerStoreRfsWearInfo(uint32_t* totalBlocks, uint32_t* blockWritesPerSync,
                            uint32_t* flashWriteCycles);

#endif //-- LOGGER_STORE_H
