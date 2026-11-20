//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_store.h - per-minute smart meter ring file store
//-- Date: 2026-11-20 · Build: 3
//--
//-- One record per minute lands in a fixed-capacity ring file on the
//-- LittleFS storage partition, stored through mrwheel's ring_file_store
//-- component. The file type is "YYMMDDhhmm": the component prepends its
//-- two automatic key fields (timeStamp and the minute usedKey) to every
//-- record, so the caller's payload struct carries only the measurements.
//-- The ring holds the last 20 minute records; every ten minutes the whole
//-- list is printed to the console with one line per record.
//-- ==========================================================================

#ifndef LOGGER_STORE_H
#define LOGGER_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

//-- --------------------------------------------------------------------------
//-- Stored minute record (ring_file_store payload)
//-- --------------------------------------------------------------------------

//-- Meter counters of the most recently accepted telegram, in the units the
//-- P1 telegram publishes them (kWh and m³). The tariff amounts are
//-- fictional billing figures: a random few cents per tariff. The struct is
//-- stored as raw bytes: pointer-free fixed-width fields only, and any
//-- layout change requires deleting the ring file. The key fields are added
//-- by the ring_file_store component and are never part of this struct.
typedef struct
{
  float energyDeliveredTariff1Kwh;
  float energyDeliveredTariff2Kwh;
  float energyReturnedTariff1Kwh;
  float energyReturnedTariff2Kwh;
  float gasDeliveredM3;
  float tariffAmount1Euro;
  float tariffAmount2Euro;
} loggerMeterRecord;

//-- --------------------------------------------------------------------------
//-- Operations
//-- --------------------------------------------------------------------------

//-- Starts the store task: it mounts the LittleFS storage partition,
//-- creates the ring file on first boot (or reopens the retained one),
//-- writes one record every minute and prints the complete list every ten
//-- minutes. Returns ESP_ERR_NO_MEM when the task could not be created;
//-- storage problems are reported on the console by the task itself.
esp_err_t loggerStoreInit(void);

//-- Visitor for the newest-first ring walk: called once per retained record,
//-- newest minute first, with the 10-digit wall-clock minute key
//-- (YYMMDDhhmm) and a pointer to the stored payload. The callback runs on
//-- the calling task; returning false stops the walk early.
typedef bool (*loggerStoreRecordVisitor)(uint64_t minuteKey, const loggerMeterRecord* record,
                                         void* userData);

//-- Walks the ring from the newest retained record to the oldest, handing
//-- every record to the visitor. Safe from any task (the store task
//-- included): ring_file_store serialises the operations on the shared
//-- handle and every traversal runs on its own cursor. Returns
//-- ESP_ERR_INVALID_STATE while the store is not running, another error
//-- when the walk could not complete, and ESP_OK otherwise (an empty ring
//-- simply never calls the visitor).
esp_err_t loggerStoreListRecords(loggerStoreRecordVisitor visitor, void* userData);

//-- VFS mount point of the LittleFS storage volume, published for the SMB
//-- network share (the component exposes an already mounted VFS path and
//-- the store task owns this mount). Returns the absolute mount point while
//-- the volume is mounted, and NULL before the store task mounted it or when
//-- the store stays disabled.
const char* loggerStoreMountPoint(void);

#endif //-- LOGGER_STORE_H
