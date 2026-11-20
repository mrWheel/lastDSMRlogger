//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_store.h - hour, day and month smart meter ring file store
//-- Date: 2026-11-20 · Build: 5
//--
//-- Three fixed-capacity ring files land on the LittleFS storage partition,
//-- written through mrwheel's ring_file_store component: the hours ring
//-- (file type "YYMMDDhh", key "YYMMDDhh"), the days ring (file type
//-- "YYMMDD", key "YYMMDD") and the months ring (file type "YYMM", key
//-- "YYMM"). All three are opened buffered with a one-record staging
//-- buffer; the store task synchronizes the three files together at every
//-- hour change. The component prepends its two automatic key fields
//-- (timeStamp and the period usedKey) to every record, so the caller's
//-- payload struct carries only the measurements.
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
//-- P1 telegram publishes them (kWh and m³). The tariff amounts are
//-- fictional billing figures: a random few cents per tariff. The struct is
//-- stored as raw bytes: pointer-free fixed-width fields only, and any
//-- layout change requires deleting the ring files. The key fields are
//-- added by the ring_file_store component and are never part of this
//-- struct.
typedef struct
{
  float energyDeliveredTariff1Kwh;
  float energyDeliveredTariff2Kwh;
  float energyReturnedTariff1Kwh;
  float energyReturnedTariff2Kwh;
  float gasDeliveredM3;
  float tariffAmount1Euro;
  float tariffAmount2Euro;
  float tariffAmountGasEuro;
} loggerMeterRecord;

//-- --------------------------------------------------------------------------
//-- Operations
//-- --------------------------------------------------------------------------

//-- Starts the store task: it mounts the LittleFS storage partition,
//-- removes the ring file of the retired minute store, creates the three
//-- ring files on first boot (or reopens the retained ones after that),
//-- refreshes the current hour, day and month record every minute with the
//-- latest counters and synchronizes all three files at every hour change.
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

#endif //-- LOGGER_STORE_H
