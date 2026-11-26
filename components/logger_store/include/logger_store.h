//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_store.h - hour, day and month smart meter ring file store
//-- Date: 2026-11-25 · Build: 16
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
//-- ringFileEdit(): missing counter values become 0.0, missing ti, te, tg
//-- and nc values are computed from the stored settings (average tariffs
//-- and the prorated network and transport month amount).
//--
//-- The SPA's Edit view reads and revises one ring through the editor API:
//-- loggerStoreEditorSnapshot() hands the retained records newest first
//-- (the ring's own bucket order kept even when keys are corrupt) with a
//-- content pin per row, the expected calendar key per position and a
//-- repair preview, and loggerStoreApplyRingEdits() applies one Save as a
//-- single transaction: the whole ring is rebuilt into a journal file,
//-- verified and published through a rename swap, so a failed or
//-- power-interrupted Save yields either the complete old ring or the
//-- complete new one (startup recovery resolves any unfinished swap before
//-- the rings open). The newest record — the open period the logger
//-- rewrites with every snapshot — is always locked: shown, never edited.
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

  //-- Fixed monthly network and transport amount (Settings, "Network &
  //-- transport costs (month)") as it was configured when this record was
  //-- written, prorated to the ring's period: the months ring stores the
  //-- full month amount, the days ring a thirtieth of it and the hours ring
  //-- a thirtieth of a day. The Months view's Costs table prints this
  //-- stored figure as its Netw. column; populate fills it from the stored
  //-- settings when the import file carries no nc column (or a blank value).
  float networkCostsEuro;
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

//-- Requests an immediate synchronisation of all three ring files (the
//-- console's sync command): the store task is woken from its minute sleep,
//-- takes one more snapshot and commits all rings at once, restarting the
//-- configured rfs synchronisation interval from this moment. False when the
//-- store task is not running (the volume or the rings are not open).
bool loggerStoreRequestSync(void);

//-- Requests an immediate synchronisation like loggerStoreRequestSync() and
//-- waits (bounded by timeoutMs) until the store task has actually committed
//-- all three ring files: true when the commit completed within the timeout,
//-- false when the store task is not running or the wait timed out. The
//-- console's restart uses this so the staged records reach storage before
//-- the reboot tears the stack down.
bool loggerStoreRequestSyncAndWait(uint32_t timeoutMs);

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
//-- RFS history editor (the SPA's Edit view)
//-- --------------------------------------------------------------------------

//-- The five cumulative meter channels of a record, in payload order. A
//-- consumption edit names the channel it edits; the resulting older
//-- reading lands in the same channel of the immediately older row.
#define LOGGER_STORE_EDITOR_CHANNELS 5u
#define LOGGER_STORE_EDITOR_AMOUNTS 3u

//-- One row of the editor snapshot: one retained record in logical (ring)
//-- order, newest first, with everything the Edit view needs to render and
//-- pin it. The keys and the checksum pin the row: a Save carries them back
//-- and the store refuses the whole edit when any pinned row changed since
//-- the snapshot. locked marks the row the logger still owns (the newest
//-- record, the one of the open period): the logger rewrites it with every
//-- snapshot, so the editor shows it but never edits it. The oldest row has
//-- no older baseline, so its consumption inputs stay unavailable (the view
//-- decides that from its position). keyOk reports a stored period key the
//-- ring's schema accepts; when the key differs from the expected sequence
//-- (or is not acceptable at all), keyRepair previews the replacement the
//-- Save would write in keyProblem's words.
typedef struct
{
  uint64_t key;             //-- wall-clock period key as stored ("YYMMDDhh"...)
  uint64_t timeStamp;       //-- full stored timestamp (century digits included)
  uint32_t checksum;        //-- content pin of the stored record
  loggerMeterRecord record; //-- the stored payload
  bool staged;              //-- still in the one-record staging buffer
  bool locked;              //-- newest / open-period record: never editable
  bool keyOk;               //-- stored key legal for this ring's schema
  bool keyRepair;           //-- key differs from the expected sequence
  uint64_t expectedKey;     //-- proposed replacement key (when keyRepair)
  char keyProblem[96];      //-- what is wrong with the key, empty when fine
} loggerStoreEditorRecord;

//-- Snapshot facts beside the rows: the ring's capacity and retained count,
//-- the key the device clock currently names for this ring (0 while the
//-- clock never synchronised) and the anchor verdict. anchorOk says the
//-- newest-period anchor could be trusted and the expected sequence built;
//-- when false the anchorProblem names why, key repairs stay blocked and a
//-- Save can only land when no row needs one.
typedef struct
{
  uint32_t capacity;      //-- ring capacity from the file header
  uint32_t count;         //-- retained records, staging buffer included
  bool storeRunning;      //-- the store task has the ring open
  bool clockSynced;       //-- the device wall clock is usable
  uint64_t openPeriodKey; //-- key of the period the clock names now (0 unknown)
  bool anchorOk;          //-- expected sequence could be derived
  char anchorProblem[96]; //-- why the anchor is untrusted, empty when ok
} loggerStoreEditorMeta;

//-- One row's edit of a Save, by position in the snapshot (index 0 is the
//-- newest row the snapshot handed). The row's checksum (and, for a
//-- consumption edit, the checksum of the immediately older row whose
//-- reading the edit rewrites) must still match storage, else the whole
//-- Save is refused as stale. A consumption edit names the entered use of
//-- the channel for this row; the store derives reading(older) =
//-- reading(this) − use from the draft readings. An amount edit sets the
//-- stored period amount (import, export or gas EUR) as it stands — the
//-- store never reprices it. At most one edit per channel per row;
//-- duplicates are refused.
typedef struct
{
  uint32_t index;    //-- row position in the snapshot, newest first
  uint32_t checksum; //-- pin of the row as the snapshot showed it
  bool hasConsumption[LOGGER_STORE_EDITOR_CHANNELS];
  float consumption[LOGGER_STORE_EDITOR_CHANNELS];
  bool hasAmount[LOGGER_STORE_EDITOR_AMOUNTS];
  float amount[LOGGER_STORE_EDITOR_AMOUNTS];
} loggerStoreRowEdit;

//-- Upper bound of one Save request (rows per ring plus headroom); a larger
//-- request is refused as oversized.
#define LOGGER_STORE_EDITOR_MAX_EDITS 64u

//-- Builds the editor snapshot of one ring on the calling task: the meta
//-- facts plus up to maxRecords rows in logical order, newest first
//-- (staging buffer included). The walk keeps the ring's own bucket order
//-- even when keys are corrupt: unlike loggerStoreListRecords it never
//-- drops a record over a key that does not decrease. Returns
//-- ESP_ERR_INVALID_ARG for a bad argument, ESP_ERR_INVALID_STATE while the
//-- store is not running (meta->storeRunning also reports it), and ESP_OK
//-- with the count filled. The rows carry no edit state: the view layers
//-- its drafts over these stored values.
esp_err_t loggerStoreEditorSnapshot(loggerMeterRing_t ring, loggerStoreEditorMeta* outMeta,
                                    loggerStoreEditorRecord* outRecords, uint32_t maxRecords,
                                    uint32_t* outCount);

//-- Applies one editor Save to one ring as a single transaction. The edits
//-- rebuild a draft over the stored records (explicit consumption edits
//-- applied newest to oldest per channel, amount edits stored verbatim),
//-- the expected key sequence repairs the period keys, and the complete
//-- candidate is validated on the server before anything is written. The
//-- ring is then replaced through a journal file and a rename swap, so a
//-- failed or power-interrupted Save yields either the complete old ring or
//-- the complete new one, and normal logging keeps running while the page
//-- stays open. outReason (optional) receives a one-line human explanation
//-- on failure. Returns ESP_ERR_INVALID_ARG for a bad request (unknown
//-- row, locked row, oldest-row consumption edit, duplicate edit target,
//-- oversized request, invalid value, failed validation), ESP_ERR_INVALID_STATE
//-- while the store is not running or another writer is active, an I/O
//-- error when the replacement could not be published, and ESP_OK when the
//-- new ring is durably in place (a Save that changes nothing answers ESP_OK
//-- without writing).
esp_err_t loggerStoreApplyRingEdits(loggerMeterRing_t ring, const loggerStoreRowEdit* edits,
                                    uint32_t editCount, uint32_t snapshotCount,
                                    uint64_t snapshotNewestKey, char* outReason, size_t reasonSize);

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
//-- ti (import), te (export) and tg (gas), and the monthly network and
//-- transport amount the rubric nc. Unknown columns - including the
//-- "#" and empty trailing
//-- columns of handwritten files - are skipped. Every data line starts with
//-- the period key and carries its values in header order; a blank or
//-- missing counter value becomes 0.0. A missing or blank ti, te, tg or nc
//-- is not imported as 0.0: the three tariff amounts are filled with the
//-- average tariffs of the stored manual prices (the tariff 1 prices, the
//-- way a meter that names no active tariff prices) and nc with the stored
//-- network and transport month amount prorated to the ring's period. The mode picks how the ring's
// current records are
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
//-- the volume root as "YYMMDDhhmm-<hours|days|months>.csv" (the export
//-- moment's wall-clock stamp plus the ring selector), carries the header
//-- "YYMMDDHH;1-0:1.8.1*255;1-0:1.8.2*255;1-0:2.8.1*255;1-0:2.8.2*255;
//-- 0-1:24.1.0*255;ti;te;tg;nc" and one line per retained record, newest
//-- period first, with the key as eight digits, the five counters at three
//-- decimals, the three tariff amounts and the network costs at two.
//-- The result re-imports through loggerStorePopulateRing() unchanged
//-- (the import reads the key from every line, so the order is free).
//-- outRecords (optional) receives the number of records written. Returns
//-- ESP_ERR_INVALID_ARG for an invalid ring, ESP_ERR_INVALID_STATE while the
//-- store is not running, an I/O error when the file could not be written
//-- and ESP_OK otherwise.
esp_err_t loggerStoreRingToCsv(loggerMeterRing_t ring, uint32_t* outRecords);

//-- Same export with a caller-chosen file name (the console's ring2csv asks
//-- for one): relativePath NULL keeps the default
//-- "YYMMDDhhmm-<name>.csv", stamped with the wall clock at export time,
//-- any other text names the file at the volume root. A missing ".csv" suffix
//-- is appended, and a name that is empty, too long or tries to climb out of
//-- the volume (a path separator or a "..") is refused with
//-- ESP_ERR_INVALID_ARG before anything is written. outFileName (optional)
//-- receives the final relative file name as written, so a caller can report
//-- exactly what landed on the volume.
esp_err_t loggerStoreRingToCsvNamed(loggerMeterRing_t ring, const char* relativePath,
                                    uint32_t* outRecords, char* outFileName,
                                    size_t outFileNameSize);

//-- Same csv content as the "YYMMDDhhmm-<hours|days|months>.csv" export
//-- file, built
//-- in memory instead of written to the volume (the API's csv download of
//-- one ring): header line first, one line per retained record, oldest
//-- period first (the ring2csv volume file prints newest first; the import
//-- accepts either order), formatted otherwise exactly like the file so the
//-- text re-imports through loggerStorePopulateRing() unchanged. *outText receives a
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
