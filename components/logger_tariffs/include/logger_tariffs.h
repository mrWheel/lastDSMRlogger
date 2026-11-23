//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_tariffs.h - dynamic electricity tariff schedules
//-- Date: 2026-11-23 · Build: 4
//--
//-- The shared tariff domain of the logger: prices per kWh for imported and
//-- exported energy plus a price per m³ for gas, submitted as one versioned
//-- JSON document either through
//-- the MQTT tariff topic (the MQTT client feeds it in) or through the HTTP
//-- API (the web component feeds it in). The same document format is
//-- accepted on both paths, so a supplier feed can be published through Home
//-- Assistant, Node-RED or a custom application without knowing which path
//-- carried it.
//--
//-- Validation is all-or-nothing: a batch that fails any check is refused
//-- whole and the stored schedule stays untouched. A valid batch is applied
//-- atomically: intervals with the same start and end replace the stored
//-- ones (a duplicate retained delivery is harmless), any other overlap with
//-- the stored schedule is a conflict that refuses the batch. Intervals
//-- outside the submitted batch keep standing for historical queries; the
//-- current and previous calendar month are retained across reboots in a
//-- binary file on the meter store's LittleFS volume.
//--
//-- The calendar buckets of the hour, day and month aggregations are built
//-- with the project's configured time zone (the application sets the
//-- European CET-1CEST rules after the station joins). On the autumn
//-- daylight-saving transition the repeated local hour returns as two
//-- separate buckets with different UTC boundaries.
//-- ==========================================================================

#ifndef LOGGER_TARIFFS_H
#define LOGGER_TARIFFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"
#include "logger_store.h"

//-- --------------------------------------------------------------------------
//-- Documented bounds (README.md, Dynamic electricity tariffs)
//-- --------------------------------------------------------------------------

//-- Largest accepted tariff JSON document, on both the MQTT and the HTTP
//-- path (covers well over two full local days of quarter-hour prices).
#define LOGGER_TARIFF_JSON_MAX_BYTES (96u * 1024u)

//-- Largest number of intervals in one submitted batch (two full local days
//-- of quarter-hour prices, including a 25-hour day, is well under this).
#define LOGGER_TARIFF_BATCH_MAX 1024u

//-- Largest number of intervals retained across reboots (over 60 days of
//-- quarter-hour prices, so the current and previous calendar month always
//-- fit). A batch that would push the store past this bound is refused.
#define LOGGER_TARIFF_STORED_MAX 8192u

//-- Largest number of buckets one aggregated query may return (a whole
//-- calendar month at hour resolution); a wider requested range is refused.
#define LOGGER_TARIFF_BUCKET_MAX 744u

//-- Room for one effective MQTT topic string.
#define LOGGER_TARIFF_TOPIC_MAX 160u

//-- Room for the stored three-letter currency code.
#define LOGGER_TARIFF_CURRENCY_MAX 8u

//-- --------------------------------------------------------------------------
//-- Model
//-- --------------------------------------------------------------------------

//-- One accepted tariff interval: its prices apply from start (inclusive)
//-- to end (exclusive), all times are UTC seconds since the epoch. An
//-- interval carries at least one of the three prices; a missing price is
//-- unknown, never zero, and its value is only meaningful while the
//-- matching has* flag is set. Gas shares the electricity document because
//-- suppliers report both from one feed; a gas day typically spans many
//-- electricity quarter-hours.
typedef struct
{
  int64_t startEpochS;
  int64_t endEpochS;
  float importPricePerKwh;
  float exportPricePerKwh;
  float gasPricePerM3;
  bool hasImport;
  bool hasExport;
  bool hasGas;
} loggerTariffInterval;

//-- Aggregation resolutions of the tariff query API.
typedef enum
{
  LOGGER_TARIFF_RAW = 0, //-- the original stored intervals
  LOGGER_TARIFF_HOUR,    //-- time-weighted average per local calendar hour
  LOGGER_TARIFF_DAY,     //-- time-weighted average per local calendar day
  LOGGER_TARIFF_MONTH    //-- time-weighted average per local calendar month
} loggerTariffResolution_t;

//-- Outcome of one ingestion attempt. Every rejection leaves the stored
//-- schedule untouched; reason carries the human-readable cause.
typedef enum
{
  LOGGER_TARIFF_OK = 0,    //-- the batch was accepted and persisted
  LOGGER_TARIFF_REJECTED,  //-- invalid document (map to HTTP 400)
  LOGGER_TARIFF_CONFLICT,  //-- overlaps a stored interval (map to HTTP 400)
  LOGGER_TARIFF_TOO_LARGE, //-- payload or batch over its bound (map to HTTP 400)
  LOGGER_TARIFF_NO_SPACE,  //-- memory or stored-interval bound reached (map to HTTP 500)
  LOGGER_TARIFF_IO_ERROR   //-- the schedule could not be persisted (map to HTTP 500)
} loggerTariffIngestResult_t;

//-- --------------------------------------------------------------------------
//-- Ingestion (shared by the MQTT client and the HTTP API)
//-- --------------------------------------------------------------------------

//-- Validates the whole document and applies it atomically. Returns the
//-- outcome and writes a short English reason (usable in the API reply and
//-- the log) for every rejection. The json buffer carries exactly length
//-- document bytes and must hold a terminating NUL at json[length]
//-- (trailing garbage after the document is refused). The document is at
//-- most LOGGER_TARIFF_JSON_MAX_BYTES long. The source names the entry
//-- point ("HTTP API" or "MQTT") and only labels the log line.
loggerTariffIngestResult_t loggerTariffsIngestJson(const char* json, size_t length,
                                                   const char* source, char* reasonOut,
                                                   size_t reasonSize);

//-- --------------------------------------------------------------------------
//-- Queries
//-- --------------------------------------------------------------------------

//-- True when the interval active at now was written to out (false when the
//-- clock is not synchronised or no stored interval covers now).
bool loggerTariffsActiveInterval(time_t now, loggerTariffInterval* outInterval);

//-- True when the wall clock has been synchronised (the same floor the
//-- meter store uses for a usable clock).
bool loggerTariffsClockSynced(void);

//-- The currency of the most recently accepted batch ("EUR" until a batch
//-- names another; the pointer stays valid).
const char* loggerTariffsCurrency(void);

//-- Number of intervals currently retained.
size_t loggerTariffsStoredCount(void);

//-- Visitor over the raw stored intervals overlapping the half-open range
//-- [from,to): called once per interval, oldest first. Returning false
//-- stops the walk. The interval pointer is only valid during the call.
typedef bool (*loggerTariffIntervalVisitor)(const loggerTariffInterval* interval, void* userData);

esp_err_t loggerTariffsForEachRaw(time_t from, time_t to, loggerTariffIntervalVisitor visitor,
                                  void* userData);

//-- One computed bucket of an hour, day or month aggregation. The UTC
//-- boundaries are authoritative; localLabel names the local calendar
//-- bucket (the two occurrences of a repeated hour share their label).
//-- An average is only meaningful while its covered seconds are above zero.
typedef struct
{
  int64_t startUtcS;
  int64_t endUtcS;
  char localLabel[24];
  double importPricePerKwh;
  uint64_t importCoveredSeconds;
  double exportPricePerKwh;
  uint64_t exportCoveredSeconds;
  uint64_t totalSeconds; //-- length of the requested portion of the bucket
} loggerTariffBucket;

typedef bool (*loggerTariffBucketVisitor)(const loggerTariffBucket* bucket, void* userData);

//-- Walks the calendar buckets of [from,to) in ascending order and hands
//-- every bucket that overlaps the range to the visitor. ESP_ERR_INVALID_ARG
//-- for a bad range or resolution, ESP_ERR_INVALID_STATE when the bucket
//-- bound would be exceeded, ESP_OK otherwise (an empty window simply never
//-- calls the visitor).
esp_err_t loggerTariffsAggregate(time_t from, time_t to, loggerTariffResolution_t resolution,
                                 loggerTariffBucketVisitor visitor, void* userData);

//-- --------------------------------------------------------------------------
//-- Meter store tariff calculator
//-- --------------------------------------------------------------------------

//-- Tariff calculator for the meter store (register it with
//-- loggerStoreSetTariffCalculator). For one ring snapshot it values the
//-- energy that grew inside that ring's period per direction: the used
//-- energy against the import price (the tariff amount is a cost), the
//-- delivered-back energy against the export price (the tariff amount is a
//-- credit) and the gas against the gas price. While the dynamic tariffs
//-- switch is on, the prices the table holds for that instant rule;
//-- otherwise, or where the table covers nothing, the manual prices do (a
//-- dynamic tariff is one price that always applies, there is no
//-- normal/off-peak distinction). The meter's tariff indicator (OBIS
//-- 0-0:96.14.0, passed in by the store with the snapshot's counters) picks
//-- which of the two manual prices of a direction applies; a meter that
//-- names no active tariff (0) prices like tariff 1. The amounts accumulate
//-- inside the current period (they restart when a new period opens or after
//-- a reboot, as the staged record is rewritten anyway).
void loggerTariffsMeterAmountCalculator(loggerMeterRing_t ring, uint64_t ringKey,
                                        uint8_t tariffIndicator, loggerMeterRecord* record);

//-- --------------------------------------------------------------------------
//-- MQTT topic and timestamp helpers
//-- --------------------------------------------------------------------------

//-- Builds the effective MQTT tariff topic from the stored MQTT
//-- configuration: "<top topic or 'lastdsmrlogger'>/<client id>/<sub topic
//-- or 'newTariffs'>". The client id segment is left out while the client
//-- id setting is empty.
void loggerTariffsEffectiveTopic(char* out, size_t outSize);

//-- True when text is exactly "YYYY-MM-DDTHH:MM:SSZ" naming a valid UTC
//-- instant within the supported range; the epoch seconds land in out.
bool loggerTariffsParseUtc(const char* text, int64_t* out);

//-- Formats epoch seconds back into "YYYY-MM-DDTHH:MM:SSZ" (UTC).
void loggerTariffsFormatUtc(int64_t epoch, char* out, size_t outSize);

//-- --------------------------------------------------------------------------
//-- Lifecycle
//-- --------------------------------------------------------------------------

//-- Creates the state (buffers land in PSRAM where available). The stored
//-- schedule loads lazily once the meter store has mounted the volume, so
//-- the call order with respect to the store task does not matter.
esp_err_t loggerTariffsInit(void);

#endif //-- LOGGER_TARIFFS_H
