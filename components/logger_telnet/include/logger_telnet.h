//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_telnet.h - telnet console server on port 23
//-- Date: 2026-11-22 · Build: 3
//--
//-- One operator at a time: claiming a telnet session takes control away
//-- from the web page (which shows its takeover popup) and a second telnet
//-- client takes it from the first (which receives a clear farewell line).
//-- The console answers a small read-only command set about reception, the
//-- retained telegrams, the power frame and the ring file.
//-- ==========================================================================

#ifndef LOGGER_TELNET_H
#define LOGGER_TELNET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "logger_settings.h"

//-- Starts the telnet console task and its listening socket on port 23.
//-- Idempotent: later calls simply report the running state. Start it once
//-- the station has an address, together with the SPA server.
esp_err_t loggerTelnetStart(void);

//-- --------------------------------------------------------------------------
//-- Device state block facts
//-- --------------------------------------------------------------------------

//-- The facts the console's status tail prints as the device state block:
//-- copyright, identity, versions, chip, memory and flash facts. Collected
//-- once here so the console and the /api/v3 device endpoint answer the same
//-- block from one source. Every text field is a NUL-terminated string;
//-- numeric fields read 0 while their has* flag reports the fact absent.
typedef struct
{
  char copyright[40];                 //-- the copyright line the device states about itself
  char hostname[LOGGER_HOSTNAME_MAX]; //-- hostname the station announces ("?" when unknown)
  char firmwareVersion[32];           //-- PROG_VERSION of the application
  char compileDate[16];               //-- build date in DD-MM-YYYY spelling
  char compileStamp[32];              //-- compiler __DATE__ and __TIME__ text
  char idfVersion[24];                //-- ESP-IDF runtime version
  char parserVersion[32];             //-- parserDSMR library version
  char compilerVersion[32];           //-- gcc __VERSION__
  char chipModel[16];                 //-- chip family name
  unsigned chipRevisionMajor;
  unsigned chipRevisionMinor;
  int chipCores;
  uint64_t chipId; //-- efuse mac folded into one number
  int cpuFreqMhz;  //-- configured default cpu frequency
  uint32_t freeHeapBytes;
  uint32_t minFreeHeapBytes; //-- lowest free heap since boot
  uint32_t largestFreeBlockBytes;
  uint32_t psramFreeBytes;
  uint32_t psramTotalBytes;
  bool hasAppPartition; //-- false when the running partition is unknown
  uint32_t appPartitionBytes;
  bool imageVerified;  //-- false when the image length could not be read
  uint32_t imageBytes; //-- verified image length (sketch space left: partition minus this)
  bool hasImageHeader; //-- false when the image header could not be read
  char flashMode[16];  //-- SPI read mode name of the running image
  char flashSpeed[24]; //-- SPI clock factor name of the running image
  bool flashSizeReadable;
  uint32_t flashBytes;     //-- flash chip size (valid when readable)
  char flashSizeError[24]; //-- error name when the flash size is unreadable
  bool hasStoragePartition;
  uint32_t storageKb; //-- size of the LittleFS storage partition in kB
} loggerTelnetDeviceInfo;

//-- Collects the device state block facts from the system side. Safe from
//-- any task and before (or without) a running console; false only for a
//-- null pointer.
bool loggerTelnetGetDeviceInfo(loggerTelnetDeviceInfo* out);

//-- --------------------------------------------------------------------------
//-- Managed component versions
//-- --------------------------------------------------------------------------

//-- One managed component with the release version the component manager
//-- resolved for this build. The table is filled by this component's
//-- CMakeLists from the fetched idf_component.yml manifests, so console and
//-- API report exactly what the firmware was built against; a manifest that
//-- could not be read reports "unknown".
typedef struct
{
  const char* name;    //-- namespaced registry name, e.g. "mrwheel/parserdsmr"
  const char* version; //-- resolved release version, e.g. "0.9.2"
} loggerTelnetComponentVersion;

//-- Access to the static table the console's status and version commands and
//-- the /api/v3/version endpoint print from, so all answer the same list
//-- from one source. The pointer stays valid for the lifetime of the
//-- firmware; *count receives the number of entries. Safe from any task;
//-- returns NULL for a null count pointer.
const loggerTelnetComponentVersion* loggerTelnetGetComponentVersions(size_t* count);

#endif //-- LOGGER_TELNET_H
