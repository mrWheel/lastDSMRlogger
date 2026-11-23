//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_telnet.c - telnet console server on port 23
//-- Date: 2026-11-23 · Build: 17
//--
//-- A plain BSD-socket console task: it listens on port 23, serves exactly
//-- one session at a time and answers a command set from the receiver,
//-- store and settings APIs (read-only except populate, which imports a
//-- LittleFS csv file into one meter ring). The console slot is decided by the
//-- shared logger_control arbiter, which keeps one slot per channel: the
//-- web page holds its own slot and is never displaced from here. Accepting
//-- a telnet client claims the console slot, and when a newer telnet client
//-- claims it back, the evict callback of this component prints a clear
//-- farewell to the session and shuts its socket down, so the task notices
//-- and cleans up on its own. Every output line is built with snprintf; only
//-- the raw telegram dump writes bytes as received.
//-- ==========================================================================

#include "logger_telnet.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "logger_control.h"
#include "logger_receiver.h"
#include "logger_settings.h"
#include "logger_store.h"
#include "wifi_provisioner.h"

//-- The program version string is defined by the application entry
//-- (main/app_main.c) and shown by the version and status commands.
extern const char* PROG_VERSION;

static const char* tag = "loggerTelnet";

#define TELNET_PORT 23
#define TELNET_TASK_STACK_BYTES 8192
#define TELNET_TASK_PRIORITY 5
#define TELNET_LINE_MAX 96
#define TELNET_RECV_BUFFER_BYTES 256
#define TELNET_PRINT_LINE_BYTES 512
#define TELNET_OUT_BUFFER_BYTES 4096
//-- Mount point plus a full d_name (NAME_MAX) plus separator and terminator.
#define TELNET_PATH_BYTES 320
#define TELNET_PROMPT "logger> "

//-- Question the populate command asks for its input file; the next line
//-- typed is taken as the file name (relative to the LittleFS mount point).
#define TELNET_POPULATE_QUESTION                                                                   \
  "input file on LittleFS (e.g. RINGmonths.csv, "                                                  \
  "empty line cancels): "

//-- Minimal telnet negotiation: the console echoes and is full duplex, so
//-- the client can switch local echo off. Incoming negotiation sequences are
//-- skipped by the input state machine.
#define TELNET_IAC 255
#define TELNET_WILL 251
#define TELNET_WONT 252
#define TELNET_DO 253
#define TELNET_DONT 254
#define TELNET_OPT_ECHO 1
#define TELNET_OPT_SGA 3

static TaskHandle_t telnetTask;
static volatile bool taskRunning = false;

//-- Pending populate input: while awaiting the file name the next line is
//-- taken as the answer instead of as a command. One session at a time, so
//-- plain file-scope variables are enough; a fresh client resets them.
static bool populateAwaitingFile = false;
static loggerMeterRing_t populateRing = LOGGER_RING_HOURS;
static loggerPopulateMode_t populateMode = LOGGER_POPULATE_MERGE;

//-- Every command word the console knows. Synonyms stay separate entries
//-- (raw for the raw telegram dump, ringfile and exit as older spellings)
//-- so a shortened word can name them too.
static const char* const telnetCommands[] = {
    "help",     "status",   "version", "telegrams", "telegram", "raw",  "power",
    "showring", "ringfile", "store",   "populate",  "quit",     "exit", "ring2csv",
};

//-- The printf-style session writer is defined further down, the device
//-- state block (part of the status command) further up than its user.
static bool telnetPrintf(int fd, const char* format, ...);
static void printState(int fd);
static void printComponentVersions(int fd);

//-- Resolves one typed command word to the command it names: a
//-- case-insensitive exact match always wins, otherwise the word must be an
//-- unambiguous leading part of exactly one command ("show" and "showri"
//-- both name "showring"). Unknown and ambiguous words are reported here
//-- and give NULL.
static const char* resolveCommand(int fd, const char* typed)
{
  size_t typedLength = strlen(typed);
  const char* found = NULL;
  uint8_t hits = 0u;

  for (size_t index = 0u; index < (sizeof telnetCommands / sizeof telnetCommands[0]); index++)
  {
    if (strcasecmp(telnetCommands[index], typed) == 0)
    {
      return telnetCommands[index];
    }

    if ((typedLength > 0u) && (strncasecmp(telnetCommands[index], typed, typedLength) == 0))
    {
      found = telnetCommands[index];
      hits++;
    }
  }

  if (hits == 1u)
  {
    return found;
  }

  telnetPrintf(fd, "%s command '%s' - type help for the command list\r\n",
               (hits == 0u) ? "unknown" : "ambiguous", typed);
  return NULL;
}

//-- Suffix the importing line carries for a non-default populate mode.
static const char* populateModeLabel(loggerPopulateMode_t mode)
{
  switch (mode)
  {
  case LOGGER_POPULATE_OVERWRITE:
    return " (overwrite)";
  case LOGGER_POPULATE_UPDATE:
    return " (update)";
  default:
    return "";
  }
}

//-- True while fd still holds the telnet console slot: every byte batch is
//-- checked against the arbiter, so a session that lost the slot to a newer
//-- telnet client mid-flight never answers another command.
static bool telnetIsOwner(int fd)
{
  int ownerFd = -1;

  return loggerControlGetOwner(LOGGER_CONTROL_TELNET, &ownerFd) && (ownerFd == fd);
}

//-- --------------------------------------------------------------------------
//-- Sending
//-- --------------------------------------------------------------------------

static bool telnetSend(int fd, const void* data, size_t length)
{
  const uint8_t* cursor = (const uint8_t*)data;

  while (length > 0u)
  {
    int written = send(fd, cursor, length, 0);

    if (written <= 0)
    {
      //-- A lost write is normally harmless (the client is gone), but the
      //-- farewell of an evicted session depends on it: log it for the
      //-- field, it is the only way a missed takeover line explains itself.
      ESP_LOGW(tag, "send to client %d failed: errno %d", fd, errno);
      return false;
    }

    cursor += (size_t)written;
    length -= (size_t)written;
  }

  return true;
}

static bool telnetSendText(int fd, const char* text)
{
  return telnetSend(fd, text, strlen(text));
}

static bool telnetPrintf(int fd, const char* format, ...)
{
  char line[TELNET_PRINT_LINE_BYTES];
  va_list arguments;
  va_start(arguments, format);
  int written = vsnprintf(line, sizeof line, format, arguments);
  va_end(arguments);

  if (written > 0)
  {
    return telnetSendText(fd, line);
  }

  return false;
}

//-- --------------------------------------------------------------------------
//-- Multi-line answer builder
//-- --------------------------------------------------------------------------

//-- Answers that need more than one stack line are built in a heap buffer
//-- first and written in one go: a visitor of the ring file walk must never
//-- block on a stalled socket while it holds the store handle.
typedef struct
{
  char* buffer;
  size_t capacity;
  size_t length;
  bool overflow;
} TelnetOut;

static bool outAppendFmt(TelnetOut* out, const char* format, ...)
{
  if (out->overflow || (out->length >= out->capacity))
  {
    out->overflow = true;
    return false;
  }

  size_t remaining = out->capacity - out->length;
  va_list arguments;
  va_start(arguments, format);
  int written = vsnprintf(out->buffer + out->length, remaining, format, arguments);
  va_end(arguments);

  if ((written < 0) || ((size_t)written >= remaining))
  {
    out->overflow = true;
    return false;
  }

  out->length += (size_t)written;
  return true;
}

//-- --------------------------------------------------------------------------
//-- Commands
//-- --------------------------------------------------------------------------

static const char* stateName(loggerReceiverState state)
{
  switch (state)
  {
  case LOGGER_RECEIVER_AWAITING_CONFIGURATION:
    return "awaiting configuration";
  case LOGGER_RECEIVER_AWAITING_DATA:
    return "awaiting data";
  case LOGGER_RECEIVER_OK:
    return "ok";
  case LOGGER_RECEIVER_STALE:
    return "stale";
  case LOGGER_RECEIVER_ERROR:
    return "error";
  default:
    return "unknown";
  }
}

static void printHelp(int fd)
{
  telnetSendText(
      fd, "\r\navailable commands:\r\n"
          "  help               this command list\r\n"
          "  status             reception, counters, clock, Wi-Fi, control owner\r\n"
          "                     and device state: copyright, identity, versions,\r\n"
          "                     memory, flash\r\n"
          "  version            firmware, parser and managed component versions\r\n"
          "  telegrams          the telegrams retained in RAM\r\n"
          "  telegram [seq]     decoded fields of one telegram (latest accepted)\r\n"
          "  raw [seq]          exact bytes of one telegram (latest accepted)\r\n"
          "  power              the realtime power frame of the newest telegram\r\n"
          "  showring {month|days|hours}\r\n"
          "                     the stored records of one meter ring (default hours)\r\n"
          "  ring2csv {month|days|hours}\r\n"
          "                     write one meter ring to the volume as RING<name>.csv\r\n"
          "  populate {hours|days|months} [overwrite|update]\r\n"
          "                     import a LittleFS csv file into one meter ring: merge\r\n"
          "                     (default) fills free slots, overwrite clears the ring\r\n"
          "                     first, update only revises stored periods\r\n"
          "  store              LittleFS volume: files with sizes, capacity and free space\r\n"
          "  quit               close this session\r\n"
          "\r\ncommands may be shortened to a unique leading part (show == showring)\r\n");
}

static void printStatus(int fd)
{
  loggerReceiverStatus status;
  loggerReceiverGetStatus(&status);

  telnetPrintf(fd, "\r\nstate        : %s (%s)\r\n", stateName(status.state),
               status.receiverRunning ? "receiver running" : "receiver stopped");
  telnetPrintf(fd, "rx gpio      : %d (%s)\r\n", status.rxGpio,
               loggerSettingsIsTestGpio(status.rxGpio) ? "test data" : "live P1");
  telnetPrintf(fd, "profile      : %s   interval: %" PRIu32 " s\r\n", status.activeProfileId,
               status.intervalSeconds);
  telnetPrintf(fd, "uptime       : %" PRIu32 " ms\r\n", status.uptimeMs);
  telnetPrintf(
      fd, "counters     : accepted %" PRIu32 ", rejected %" PRIu32 ", data loss %" PRIu32 "\r\n",
      status.acceptedCount, status.rejectedCount, status.dataLossCount);

  if (status.hasLastAccepted)
  {
    telnetPrintf(fd, "last accept  : %" PRIu32 " ms ago\r\n", status.lastAcceptedAgeMs);
  }
  else
  {
    telnetPrintf(fd, "last accept  : none yet\r\n");
  }

  if ((status.lastDiagReason[0] != '\0') || (status.lastDiagObis[0] != '\0'))
  {
    telnetPrintf(fd, "last diag    : %s %s\r\n", status.lastDiagReason, status.lastDiagObis);
  }

  if (status.lastError[0] != '\0')
  {
    telnetPrintf(fd, "last error   : %s\r\n", status.lastError);
  }

  time_t now = time(NULL);

  if (now >= 1700000000)
  {
    char clockText[24] = "";
    struct tm brokenTime;

    if (localtime_r(&now, &brokenTime) != NULL)
    {
      strftime(clockText, sizeof clockText, "%d-%m-%Y %H:%M:%S", &brokenTime);
    }

    telnetPrintf(fd, "device time  : %s\r\n", clockText);
  }
  else
  {
    telnetPrintf(fd, "device time  : clock not synchronised\r\n");
  }

  wifi_config_t stationConfig;
  memset(&stationConfig, 0, sizeof stationConfig);
  const char* ssid = "?";

  if (esp_wifi_get_config(WIFI_IF_STA, &stationConfig) == ESP_OK)
  {
    ssid = (const char*)stationConfig.sta.ssid;
  }

  esp_netif_ip_info_t ipInfo;
  char ip[16] = "not connected";

  if (wifi_prov_get_ip_info(&ipInfo) == ESP_OK)
  {
    snprintf(ip, sizeof ip, IPSTR, IP2STR(&ipInfo.ip));
  }

  telnetPrintf(fd, "wifi         : '%s' at %s\r\n", ssid, ip);

  //-- One slot per channel: a browser page and this console can each hold
  //-- their own at the same time, so both are reported.
  int webOwnerFd = -1;
  int telnetOwnerFd = -1;

  telnetPrintf(fd, "control      : web %s, telnet %s\r\n",
               loggerControlGetOwner(LOGGER_CONTROL_WEB, &webOwnerFd) ? "claimed" : "free",
               loggerControlGetOwner(LOGGER_CONTROL_TELNET, &telnetOwnerFd) ? "claimed" : "free");

  //-- The former separate state command is part of status now: reception
  //-- and device identity in one answer.
  printState(fd);
}

static void printVersion(int fd)
{
  telnetPrintf(fd, "\r\nfirmware %s, parserDSMR %s\r\n", PROG_VERSION, dsmrParserVersion());

  printComponentVersions(fd);
}

//-- --------------------------------------------------------------------------
//-- Device state block (printed at the end of the status command)
//-- --------------------------------------------------------------------------

//-- The copyright the device states about itself.
#define TELNET_COPYRIGHT "(c) by Willem Aandewiel"

//-- Converts the compiler's __DATE__ ("Nov 21 2026") to the DD-MM-YYYY
//-- spelling the user reads the firmware date in.
static void formatCompileDate(char* out, size_t outSize)
{
  static const char* const monthNames[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char monthText[4] = "";
  int day = 0;
  int year = 0;

  if (sscanf(__DATE__, "%3s %d %d", monthText, &day, &year) != 3)
  {
    snprintf(out, outSize, "%s", __DATE__);
    return;
  }

  unsigned month = 0u;

  for (unsigned index = 0u; index < (sizeof monthNames / sizeof monthNames[0]); index++)
  {
    if (strcmp(monthNames[index], monthText) == 0)
    {
      month = index + 1u;
      break;
    }
  }

  snprintf(out, outSize, "%02d-%02u-%04d", day, month, year);
}

//-- Name of the image header's SPI read mode.
static const char* spiModeName(uint8_t mode)
{
  switch (mode)
  {
  case ESP_IMAGE_SPI_MODE_QIO:
    return "QIO";
  case ESP_IMAGE_SPI_MODE_QOUT:
    return "QOUT";
  case ESP_IMAGE_SPI_MODE_DIO:
    return "DIO";
  case ESP_IMAGE_SPI_MODE_DOUT:
    return "DOUT";
  case ESP_IMAGE_SPI_MODE_FAST_READ:
    return "FAST_READ";
  case ESP_IMAGE_SPI_MODE_SLOW_READ:
    return "SLOW_READ";
  default:
    return "unknown";
  }
}

//-- Name of the image header's SPI clock division factor.
static const char* spiSpeedName(uint8_t speed)
{
  switch (speed)
  {
  case ESP_IMAGE_SPI_SPEED_DIV_2:
    return "clock div 2";
  case ESP_IMAGE_SPI_SPEED_DIV_3:
    return "clock div 3";
  case ESP_IMAGE_SPI_SPEED_DIV_4:
    return "clock div 4";
  case ESP_IMAGE_SPI_SPEED_DIV_1:
    return "clock div 1";
  default:
    return "unknown";
  }
}

bool loggerTelnetGetDeviceInfo(loggerTelnetDeviceInfo* out)
{
  if (out == NULL)
  {
    return false;
  }

  memset(out, 0, sizeof *out);

  snprintf(out->copyright, sizeof out->copyright, "%s", TELNET_COPYRIGHT);

  //-- The hostname the station actually announces (set from the settings at
  //-- the last join or save); "?" until one is known.
  snprintf(out->hostname, sizeof out->hostname, "?");
  esp_netif_t* station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

  if (station != NULL)
  {
    const char* applied = NULL;

    if ((esp_netif_get_hostname(station, &applied) == ESP_OK) && (applied != NULL))
    {
      snprintf(out->hostname, sizeof out->hostname, "%s", applied);
    }
  }

  snprintf(out->firmwareVersion, sizeof out->firmwareVersion, "%s", PROG_VERSION);
  formatCompileDate(out->compileDate, sizeof out->compileDate);
  snprintf(out->compileStamp, sizeof out->compileStamp, "%s %s", __DATE__, __TIME__);
  snprintf(out->idfVersion, sizeof out->idfVersion, "%s", esp_get_idf_version());
  snprintf(out->parserVersion, sizeof out->parserVersion, "%s", dsmrParserVersion());
  snprintf(out->compilerVersion, sizeof out->compilerVersion, "%s", __VERSION__);

  esp_chip_info_t chip;
  esp_chip_info(&chip);
  snprintf(out->chipModel, sizeof out->chipModel, "ESP32-S3");
  out->chipRevisionMajor = (unsigned)(chip.revision / 100);
  out->chipRevisionMinor = (unsigned)(chip.revision % 100);
  out->chipCores = (int)chip.cores;

  uint8_t mac[6];

  if (esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY) == ESP_OK)
  {
    for (uint8_t index = 0u; index < sizeof mac; index++)
    {
      out->chipId = (out->chipId << 8) | mac[index];
    }
  }

  out->cpuFreqMhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
  out->freeHeapBytes = esp_get_free_heap_size();
  out->minFreeHeapBytes = esp_get_minimum_free_heap_size();
  out->largestFreeBlockBytes = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  out->psramFreeBytes = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  out->psramTotalBytes = (uint32_t)heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

  //-- The running application image: partition total, verified image length
  //-- and the room left in the partition (the sketch space).
  const esp_partition_t* app = esp_ota_get_running_partition();
  out->hasAppPartition = (app != NULL);

  if (app != NULL)
  {
    out->appPartitionBytes = (uint32_t)app->size;

    esp_image_metadata_t metadata;
    esp_partition_pos_t position = {app->address, app->size};

    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &position, &metadata) == ESP_OK)
    {
      out->imageVerified = true;
      out->imageBytes = metadata.image_len;
    }

    //-- The image header carries the SPI mode and clock factor the chip is
    //-- read with.
    esp_image_header_t header;

    if ((esp_partition_read(app, 0, &header, sizeof header) == ESP_OK) &&
        (header.magic == ESP_IMAGE_HEADER_MAGIC))
    {
      out->hasImageHeader = true;
      snprintf(out->flashMode, sizeof out->flashMode, "%s", spiModeName(header.spi_mode));
      snprintf(out->flashSpeed, sizeof out->flashSpeed, "%s", spiSpeedName(header.spi_speed));
    }
  }

  uint32_t flashBytes = 0u;
  esp_err_t flashSizeRead = esp_flash_get_size(NULL, &flashBytes);
  out->flashSizeReadable = (flashSizeRead == ESP_OK);
  out->flashBytes = flashBytes;

  if (!out->flashSizeReadable)
  {
    snprintf(out->flashSizeError, sizeof out->flashSizeError, "%s", esp_err_to_name(flashSizeRead));
  }

  const esp_partition_t* storage =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
  out->hasStoragePartition = (storage != NULL);

  if (storage != NULL)
  {
    out->storageKb = (uint32_t)(storage->size / 1024u);
  }

  return true;
}

//-- The managed component table the console and the /api/v3/version endpoint
//-- report from (see logger_telnet.h). The name and version strings are
//-- injected by this component's CMakeLists, which reads them from the
//-- idf_component.yml manifests the component manager fetched, so both
//-- report exactly the releases this firmware was built with.
static const loggerTelnetComponentVersion COMPONENT_VERSIONS[] = {
    {PARSERDSMR_COMPONENT_NAME, PARSERDSMR_COMPONENT_VERSION},
    {WIFI_PROVISIONER_COMPONENT_NAME, WIFI_PROVISIONER_COMPONENT_VERSION},
    {RING_FILE_STORE_COMPONENT_NAME, RING_FILE_STORE_COMPONENT_VERSION},
    {LITTLEFS_COMPONENT_NAME, LITTLEFS_COMPONENT_VERSION},
    {SMB_SERVER_COMPONENT_NAME, SMB_SERVER_COMPONENT_VERSION},
    {MQTT_COMPONENT_NAME, MQTT_COMPONENT_VERSION},
};

//-- Lists the managed components and their resolved versions.
static void printComponentVersions(int fd)
{
  telnetPrintf(fd, "\r\ncomponent versions:\r\n");

  for (size_t index = 0u; index < (sizeof COMPONENT_VERSIONS / sizeof COMPONENT_VERSIONS[0]);
       index++)
  {
    telnetPrintf(fd, "  %-36s %s\r\n", COMPONENT_VERSIONS[index].name,
                 COMPONENT_VERSIONS[index].version);
  }
}

const loggerTelnetComponentVersion* loggerTelnetGetComponentVersions(size_t* count)
{
  if (count == NULL)
  {
    return NULL;
  }

  *count = sizeof COMPONENT_VERSIONS / sizeof COMPONENT_VERSIONS[0];
  return COMPONENT_VERSIONS;
}

static void printState(int fd)
{
  loggerTelnetDeviceInfo info;

  if (!loggerTelnetGetDeviceInfo(&info))
  {
    return;
  }

  telnetPrintf(fd, "\r\n%s\r\n", info.copyright);
  telnetPrintf(fd, "hostname       : %s\r\n", info.hostname);
  telnetPrintf(fd, "firmware       : %s (%s)\r\n", info.firmwareVersion, info.compileDate);
  telnetPrintf(fd, "compiled       : %s\r\n", info.compileStamp);
  telnetPrintf(fd, "compiled with  : ESP-IDF %s, parserDSMR %s, gcc %s\r\n", info.idfVersion,
               info.parserVersion, info.compilerVersion);
  telnetPrintf(fd, "sdk version    : %s\r\n", info.idfVersion);

  printComponentVersions(fd);

  telnetPrintf(fd, "chip model     : %s (revision %u.%u, %d core(s))\r\n", info.chipModel,
               info.chipRevisionMajor, info.chipRevisionMinor, info.chipCores);
  telnetPrintf(fd, "chip ID        : %" PRIu64 " (efuse mac)\r\n", info.chipId);
  telnetPrintf(fd, "CPU freq       : %d MHz (configured)\r\n", info.cpuFreqMhz);
  telnetPrintf(fd, "free heap      : %" PRIu32 " bytes\r\n", info.freeHeapBytes);
  telnetPrintf(fd, "min free heap  : %" PRIu32 " bytes (lowest since boot)\r\n",
               info.minFreeHeapBytes);
  telnetPrintf(fd, "max block      : %u bytes (largest free block)\r\n",
               (unsigned)info.largestFreeBlockBytes);
  telnetPrintf(fd, "free PSRAM     : %u bytes\r\n", (unsigned)info.psramFreeBytes);
  telnetPrintf(fd, "PSRAM size     : %u bytes\r\n", (unsigned)info.psramTotalBytes);

  if (info.hasAppPartition)
  {
    telnetPrintf(fd, "program size   : %" PRIu32 " bytes (running app partition)\r\n",
                 info.appPartitionBytes);

    if (info.imageVerified)
    {
      telnetPrintf(fd, "program used   : %" PRIu32 " bytes\r\n", info.imageBytes);
      telnetPrintf(fd, "free sketch    : %" PRIu32 " bytes\r\n",
                   info.appPartitionBytes - info.imageBytes);
    }
    else
    {
      telnetPrintf(fd, "program used   : not verified\r\n");
    }

    if (info.hasImageHeader)
    {
      telnetPrintf(fd, "flash mode     : %s\r\n", info.flashMode);
      telnetPrintf(fd, "flash speed    : %s\r\n", info.flashSpeed);
    }
  }

  if (info.flashSizeReadable)
  {
    telnetPrintf(fd, "flash size     : %" PRIu32 " bytes\r\n", info.flashBytes);
  }
  else
  {
    telnetPrintf(fd, "flash size     : unreadable (%s)\r\n", info.flashSizeError);
  }

  if (info.hasStoragePartition)
  {
    telnetPrintf(fd, "littlefs size  : %" PRIu32 " kB\r\n", info.storageKb);
  }
  else
  {
    telnetPrintf(fd, "littlefs size  : no storage partition\r\n");
  }
}

static void printTelegramList(int fd)
{
  loggerTelegramMeta records[LOGGER_HISTORY_CAPACITY];
  uint16_t count = loggerReceiverListRecords(records, LOGGER_HISTORY_CAPACITY);

  telnetPrintf(fd, "\r\n%u telegram(s) retained (newest first):\r\n", (unsigned)count);

  for (uint16_t index = 0u; index < count; index++)
  {
    telnetPrintf(fd, "  #%" PRIu32 "  meter %s  crc %s  %u bytes  %u fields  %s  ver %s\r\n",
                 records[index].sequence,
                 (records[index].meterTimeText[0] != '\0') ? records[index].meterTimeText
                                                           : "--:--:--",
                 records[index].crcVerified ? "ok" : "missing", (unsigned)records[index].rawLength,
                 (unsigned)records[index].fieldCount, records[index].profileId,
                 records[index].reportedVersion);

    if (records[index].notes[0] != '\0')
    {
      telnetPrintf(fd, "        notes: %s\r\n", records[index].notes);
    }
  }
}

//-- Sequence of the most recently accepted telegram: the newest entry of
//-- the retained list (the list is handed newest first). False when no
//-- telegram was accepted yet, so the telegram and raw commands can simply
//-- show the latest telegram when no sequence is typed.
static bool latestAcceptedSequence(uint32_t* outSequence)
{
  loggerTelegramMeta newest;

  if (loggerReceiverListRecords(&newest, 1u) == 0u)
  {
    return false;
  }

  *outSequence = newest.sequence;
  return true;
}

static void printTelegramDetail(int fd, uint32_t sequence)
{
  loggerTelegramRecord* record = malloc(sizeof *record);

  if (record == NULL)
  {
    telnetPrintf(fd, "out of memory\r\n");
    return;
  }

  if (!loggerReceiverGetRecord(sequence, record))
  {
    free(record);
    telnetPrintf(fd, "telegram #%" PRIu32 " is not (or no longer) retained\r\n", sequence);
    return;
  }

  TelnetOut out;
  char* buffer = malloc(TELNET_OUT_BUFFER_BYTES);

  if (buffer == NULL)
  {
    free(record);
    telnetPrintf(fd, "out of memory\r\n");
    return;
  }

  out.buffer = buffer;
  out.capacity = TELNET_OUT_BUFFER_BYTES;
  out.length = 0u;
  out.overflow = false;

  outAppendFmt(&out, "\r\n#%" PRIu32 "  %s  reported %s  crc %s  %u bytes  %u fields\r\n",
               record->meta.sequence, record->meta.profileId, record->meta.reportedVersion,
               record->meta.crcVerified ? "ok" : "missing", (unsigned)record->meta.rawLength,
               (unsigned)record->meta.fieldCount);

  if (record->meta.notes[0] != '\0')
  {
    outAppendFmt(&out, "notes: %s\r\n", record->meta.notes);
  }

  for (uint16_t index = 0u; index < record->meta.fieldCount; index++)
  {
    const loggerFieldSnapshot* field = &record->fields[index];

    if (!outAppendFmt(&out, "%-15s %-24s %12s %-10s", field->obis, field->name, field->value,
                      field->unit))
    {
      break;
    }

    if (field->hasTimestamp)
    {
      if (!outAppendFmt(&out, " @ %s", field->timestamp))
      {
        break;
      }
    }

    if (field->mbusChannel != 0u)
    {
      if (!outAppendFmt(&out, "  (M-Bus ch %u)", (unsigned)field->mbusChannel))
      {
        break;
      }
    }

    if (!outAppendFmt(&out, "\r\n"))
    {
      break;
    }
  }

  if (out.overflow)
  {
    outAppendFmt(&out, "... output truncated\r\n");
  }

  telnetSend(fd, out.buffer, out.length);
  free(buffer);
  free(record);
}

static void printTelegramRaw(int fd, uint32_t sequence)
{
  loggerTelegramRecord* record = malloc(sizeof *record);

  if (record == NULL)
  {
    telnetPrintf(fd, "out of memory\r\n");
    return;
  }

  if (!loggerReceiverGetRecord(sequence, record))
  {
    free(record);
    telnetPrintf(fd, "telegram #%" PRIu32 " is not (or no longer) retained\r\n", sequence);
    return;
  }

  //-- Exact accepted bytes: original line endings and trailer preserved.
  telnetSend(fd, "\r\n", 2u);
  telnetSend(fd, record->rawBytes, record->meta.rawLength);
  telnetSend(fd, "\r\n", 2u);
  free(record);
}

static void printPower(int fd)
{
  loggerPowerSnapshot snapshot;
  uint32_t capturedUptimeMs = 0u;

  if (!loggerReceiverGetPowerSnapshot(&snapshot, &capturedUptimeMs))
  {
    telnetPrintf(fd, "\r\nno accepted telegram yet\r\n");
    return;
  }

  telnetPrintf(fd, "\r\npower frame  : meter %s%s  country %s  profile %s\r\n",
               (snapshot.meterTime[0] != '\0') ? snapshot.meterTime : "--:--:--",
               snapshot.testData ? "  (test data)" : "", snapshot.countryCode, snapshot.profileId);

  for (uint8_t phase = 0u; phase < 3u; phase++)
  {
    telnetPrintf(fd, "  phase %u    : %10.1f W  %8.3f A  %6.1f V\r\n", (unsigned)(phase + 1u),
                 snapshot.phaseWatts[phase], snapshot.phaseAmps[phase], snapshot.phaseVolts[phase]);
  }

  telnetPrintf(fd, "  total      : %10.1f W  %8.3f A  %6.1f V\r\n", snapshot.totalWatts,
               snapshot.totalAmps, snapshot.totalVolts);
}

//-- Period key label per ring: the header spelling the import csv uses for
//-- the same column. Its length is the width of the key column.
static const char* const ringKeyNames[LOGGER_RING_COUNT] = {"YYMMDDHH", "YYMMDD", "YYMM"};

//-- Value column labels of the ring print: the OBIS codes of the five
//-- counters (gas under its catalogue code), the three tariff amounts and
//-- the network costs. The tariff amounts are fictional billing figures
//-- without a code of their own, so they keep their rubrics, named for the
//-- direction of the energy they value: ti (import), te (export) and
//-- tg (gas); the network costs carry the rubric nc.
static const char* const ringValueLabels[] = {
    "1-0:1.8.1", "1-0:1.8.2", "1-0:2.8.1", "1-0:2.8.2", "0-1:24.1.0", "ti", "te", "tg", "nc"};

//-- Visitor of the ring walk: builds the answer in memory instead of writing
//-- to the socket while the store handle is held by the walk. One record is
//-- one semicolon separated line without field names: the labels live once
//-- in the header above the records. Every value column is right aligned at
//-- width 10, the key is zero padded to the width of the ring's key label.
typedef struct
{
  TelnetOut* out;
  int keyWidth;
} RingPrint;

static bool appendRingFileRecord(uint64_t key, const loggerMeterRecord* record, void* userData)
{
  RingPrint* print = (RingPrint*)userData;

  outAppendFmt(print->out,
               "%0*" PRIu64 ";%10.3f;%10.3f;%10.3f;%10.3f;%10.3f;%10.2f;%10.2f;%10.2f;%10.2f\r\n",
               print->keyWidth, key, record->energyDeliveredTariff1Kwh,
               record->energyDeliveredTariff2Kwh, record->energyReturnedTariff1Kwh,
               record->energyReturnedTariff2Kwh, record->gasDeliveredM3,
               record->tariffAmountImportEuro, record->tariffAmountExportEuro,
               record->tariffAmountGasEuro, record->networkCostsEuro);
  return !print->out->overflow;
}

static void printRingFile(int fd, const char* periodText)
{
  loggerMeterRing_t ring = LOGGER_RING_HOURS;

  if ((periodText != NULL) && (periodText[0] != '\0') &&
      !loggerStoreRingFromName(periodText, &ring))
  {
    telnetPrintf(fd, "unknown ring '%s' - use month, days or hours\r\n", periodText);
    return;
  }

  TelnetOut out;
  char* buffer = malloc(TELNET_OUT_BUFFER_BYTES);

  if (buffer == NULL)
  {
    telnetPrintf(fd, "out of memory\r\n");
    return;
  }

  out.buffer = buffer;
  out.capacity = TELNET_OUT_BUFFER_BYTES;
  out.length = 0u;
  out.overflow = false;

  RingPrint print;
  print.out = &out;
  print.keyWidth = (int)strlen(ringKeyNames[ring]);

  outAppendFmt(&out, "\r\n%s ring records (newest first):\r\n", loggerStoreRingName(ring));

  //-- The column rubrics once, right aligned above the columns they name.
  outAppendFmt(&out, "%*s;%10s;%10s;%10s;%10s;%10s;%10s;%10s;%10s;%10s\r\n", print.keyWidth,
               ringKeyNames[ring], ringValueLabels[0], ringValueLabels[1], ringValueLabels[2],
               ringValueLabels[3], ringValueLabels[4], ringValueLabels[5], ringValueLabels[6],
               ringValueLabels[7], ringValueLabels[8]);
  esp_err_t walked = loggerStoreListRecords(ring, appendRingFileRecord, &print);

  if (walked != ESP_OK)
  {
    outAppendFmt(&out, "(ring file not available: %s)\r\n", esp_err_to_name(walked));
  }

  telnetSend(fd, out.buffer, out.length);
  free(buffer);
}

//-- LittleFS volume overview: every file on the mounted storage volume with
//-- its size, followed by the total capacity and the free space. The walk
//-- runs on the console task while the store task keeps writing its ring
//-- snapshots: the LittleFS VFS layer serialises both sides, and the answer is
//-- built in memory (like the ring file walk) so the socket is never written
//-- to mid-scan.
static void printStore(int fd)
{
  const char* mountPoint = loggerStoreMountPoint();

  if (mountPoint == NULL)
  {
    telnetPrintf(fd, "\r\nthe LittleFS storage volume is not mounted (meter store disabled)\r\n");
    return;
  }

  TelnetOut out;
  char* buffer = malloc(TELNET_OUT_BUFFER_BYTES);

  if (buffer == NULL)
  {
    telnetPrintf(fd, "out of memory\r\n");
    return;
  }

  out.buffer = buffer;
  out.capacity = TELNET_OUT_BUFFER_BYTES;
  out.length = 0u;
  out.overflow = false;

  outAppendFmt(&out, "\r\nLittleFS volume at %s:\r\n", mountPoint);

  DIR* directory = opendir(mountPoint);

  if (directory == NULL)
  {
    outAppendFmt(&out, "(the volume directory could not be opened: errno %d)\r\n", errno);
  }
  else
  {
    uint32_t fileCount = 0u;
    struct dirent* entry = NULL;

    while ((entry = readdir(directory)) != NULL)
    {
      if ((strcmp(entry->d_name, ".") == 0) || (strcmp(entry->d_name, "..") == 0))
      {
        continue;
      }

      char path[TELNET_PATH_BYTES];
      snprintf(path, sizeof path, "%s/%s", mountPoint, entry->d_name);

      struct stat details;
      memset(&details, 0, sizeof details);

      if (stat(path, &details) != 0)
      {
        outAppendFmt(&out, "  %-24s  (not readable: errno %d)\r\n", entry->d_name, errno);
        continue;
      }

      fileCount++;

      if (S_ISDIR(details.st_mode))
      {
        outAppendFmt(&out, "  %-24s  <directory>\r\n", entry->d_name);
      }
      else
      {
        outAppendFmt(&out, "  %-24s %10ld bytes\r\n", entry->d_name, (long)details.st_size);
      }
    }

    closedir(directory);

    outAppendFmt(&out, "%" PRIu32 " file(s) on the volume\r\n", fileCount);
  }

  size_t totalBytes = 0u;
  size_t usedBytes = 0u;

  if (loggerStoreVolumeInfo(&totalBytes, &usedBytes))
  {
    outAppendFmt(&out, "capacity     : %lu bytes total, %lu used, %lu free\r\n",
                 (unsigned long)totalBytes, (unsigned long)usedBytes,
                 (unsigned long)(totalBytes - usedBytes));
  }
  else
  {
    outAppendFmt(&out, "capacity     : statistics not available\r\n");
  }

  if (out.overflow)
  {
    outAppendFmt(&out, "... output truncated\r\n");
  }

  telnetSend(fd, out.buffer, out.length);
  free(buffer);
}

//-- Runs the populate import of one csv file into the pending ring and
//-- reports the outcome. The import runs on the console task: ring_file_store
//-- serialises the edits against the store task on the shared handle, so the
//-- ring keeps serving reads and snapshots while the file is imported.
static void runPopulate(int fd, const char* fileName)
{
  loggerStoreImportResult result;
  memset(&result, 0, sizeof result);

  telnetPrintf(fd, "\r\nimporting '%s' into the %s ring%s ...", fileName,
               loggerStoreRingName(populateRing), populateModeLabel(populateMode));

  esp_err_t outcome = loggerStorePopulateRing(populateRing, fileName, populateMode, &result);

  if (outcome != ESP_OK)
  {
    telnetPrintf(fd, "\r\npopulate failed: %s%s%s\r\n", esp_err_to_name(outcome),
                 (result.firstError[0] != '\0') ? " - " : "", result.firstError);
    return;
  }

  telnetPrintf(fd,
               "\r\n%" PRIu32 " line(s) read, %" PRIu32 " edit(s) written, "
               "%" PRIu32 " line(s) rejected, %" PRIu32 " refused\r\n",
               result.rowsRead, result.bucketsWritten, result.rowsRejected, result.bucketsRefused);

  if (result.firstError[0] != '\0')
  {
    telnetPrintf(fd, "first refusal: %s\r\n", result.firstError);
  }
}

//-- Handles one complete command line. Returns false when the session must
//-- close (the quit command).
static bool handleLine(int fd, char* line)
{
  while (isspace((unsigned char)*line))
  {
    line++;
  }

  //-- While a populate awaits its file name the next line is the answer,
  //-- not a command; an empty line cancels the import.
  if (populateAwaitingFile)
  {
    populateAwaitingFile = false;

    if (*line == '\0')
    {
      telnetPrintf(fd, "populate cancelled\r\n");
    }
    else
    {
      runPopulate(fd, line);
    }

    telnetSendText(fd, TELNET_PROMPT);
    return true;
  }

  if (*line == '\0')
  {
    telnetSendText(fd, TELNET_PROMPT);
    return true;
  }

  char* arguments = strchr(line, ' ');

  if (arguments != NULL)
  {
    *arguments++ = '\0';
  }

  //-- The command word may be an exact (case-insensitive) command name or
  //-- an unambiguous leading part of one; the argument text stays as typed.
  const char* command = resolveCommand(fd, line);

  if (command == NULL)
  {
    telnetSendText(fd, TELNET_PROMPT);
    return true;
  }

  if (strcmp(command, "help") == 0)
  {
    printHelp(fd);
  }
  else if (strcmp(command, "status") == 0)
  {
    printStatus(fd);
  }
  else if (strcmp(command, "version") == 0)
  {
    printVersion(fd);
  }
  else if (strcmp(command, "telegrams") == 0)
  {
    printTelegramList(fd);
  }
  else if ((strcmp(command, "telegram") == 0) || (strcmp(command, "raw") == 0))
  {
    bool rawRequest = (strcmp(command, "raw") == 0);
    uint32_t sequence = 0u;
    bool haveSequence;

    if ((arguments != NULL) && (arguments[0] != '\0'))
    {
      sequence = (uint32_t)strtoul(arguments, NULL, 10);
      haveSequence = true;
    }
    else
    {
      //-- No sequence typed: show the most recently accepted telegram, the
      //-- same default the sequence-less v3 endpoints use.
      haveSequence = latestAcceptedSequence(&sequence);
    }

    if (!haveSequence)
    {
      telnetPrintf(fd, "no accepted telegram yet - try again after the next one\r\n");
    }
    else if (rawRequest)
    {
      printTelegramRaw(fd, sequence);
    }
    else
    {
      printTelegramDetail(fd, sequence);
    }
  }
  else if (strcmp(command, "power") == 0)
  {
    printPower(fd);
  }
  else if ((strcmp(command, "showring") == 0) || (strcmp(command, "ringfile") == 0))
  {
    //-- The argument picks the ring (month, days or hours, default hours);
    //-- ringfile stays as the old synonym of showring hours.
    if (arguments != NULL)
    {
      while (isspace((unsigned char)*arguments))
      {
        arguments++;
      }
    }

    printRingFile(fd, arguments);
  }
  else if (strcmp(command, "ring2csv") == 0)
  {
    //-- The selector picks the ring (no default: exporting is a write); the
    //-- file lands at the volume root as RING<name>.csv, ready for the SMB
    //-- share and for a populate re-import.
    loggerMeterRing_t ring = LOGGER_RING_HOURS;

    if ((arguments == NULL) || (arguments[0] == '\0'))
    {
      telnetPrintf(fd, "usage: ring2csv {hours|days|months}\r\n");
    }
    else if (!loggerStoreRingFromName(arguments, &ring))
    {
      telnetPrintf(fd, "unknown ring '%s' - use month, days or hours\r\n", arguments);
    }
    else
    {
      uint32_t records = 0u;
      esp_err_t outcome = loggerStoreRingToCsv(ring, &records);

      if (outcome != ESP_OK)
      {
        telnetPrintf(fd, "ring2csv failed: %s\r\n", esp_err_to_name(outcome));
      }
      else
      {
        telnetPrintf(fd, "wrote %" PRIu32 " record(s) to RING%s.csv on the volume\r\n", records,
                     loggerStoreRingName(ring));
      }
    }
  }
  else if (strcmp(command, "store") == 0)
  {
    printStore(fd);
  }
  else if (strcmp(command, "populate") == 0)
  {
    //-- The first argument picks the ring (no default: importing is a
    //-- write), the optional second one how the ring's current records are
    //-- treated (merge is the default); accepted arguments then ask for
    //-- the csv file to import.
    loggerMeterRing_t ring = LOGGER_RING_HOURS;
    loggerPopulateMode_t mode = LOGGER_POPULATE_MERGE;
    bool accepted = true;
    char ringText[32];
    char modeText[32];
    ringText[0] = '\0';
    modeText[0] = '\0';

    if ((arguments == NULL) || (arguments[0] == '\0'))
    {
      telnetPrintf(fd, "usage: populate {hours|days|months} [overwrite|update]\r\n");
      accepted = false;
    }
    else
    {
      (void)sscanf(arguments, "%31s %31s", ringText, modeText);
    }

    if (accepted && !loggerStoreRingFromName(ringText, &ring))
    {
      telnetPrintf(fd, "unknown ring '%s' - use month, days or hours\r\n", ringText);
      accepted = false;
    }

    if (accepted && (modeText[0] != '\0'))
    {
      if (strcasecmp(modeText, "overwrite") == 0)
      {
        mode = LOGGER_POPULATE_OVERWRITE;
      }
      else if (strcasecmp(modeText, "update") == 0)
      {
        mode = LOGGER_POPULATE_UPDATE;
      }
      else
      {
        telnetPrintf(fd, "unknown mode '%s' - use overwrite or update\r\n", modeText);
        accepted = false;
      }
    }

    if (accepted)
    {
      populateRing = ring;
      populateMode = mode;
      populateAwaitingFile = true;
      telnetPrintf(fd, "\r\nimporting into the %s ring%s.\r\n%s", loggerStoreRingName(ring),
                   populateModeLabel(mode), TELNET_POPULATE_QUESTION);
      return true; //-- the file name replaces the next prompt
    }
  }
  else if ((strcmp(command, "quit") == 0) || (strcmp(command, "exit") == 0))
  {
    telnetSendText(fd, "Goodbye.\r\n");
    return false;
  }

  telnetSendText(fd, TELNET_PROMPT);
  return true;
}

//-- --------------------------------------------------------------------------
//-- Eviction of this server's client by the arbiter
//-- --------------------------------------------------------------------------

//-- Runs when a newer telnet client claims the console slot from this
//-- session (the web page has its own slot and never evicts telnet): print
//-- the clear farewell and force the socket closed. The
//-- console task notices the shut-down socket, cleans up its own state and
//-- releases control (the release is a no-op because the new owner is
//-- already registered by then).
static void telnetEvictCallback(loggerControlClient evicted, int evictedFd)
{
  (void)evicted;
  telnetSendText(evictedFd,
                 "\r\n*** The telnet console was taken over by another telnet client.\r\n"
                 "*** This telnet session is closed. Goodbye.\r\n");
  shutdown(evictedFd, SHUT_RDWR);
}

//-- --------------------------------------------------------------------------
//-- Console task
//-- --------------------------------------------------------------------------

//-- No board (TFT) calls on this task: loggerBoardShowScreen() draws under
//-- the board mutex, which the panel refresh path can hold for a long
//-- window while it repaints. A console task must never wait behind a
//-- screen redraw, so telnet activity is reported on the console only.
static void telnetCloseClient(int fd)
{
  loggerControlRelease(LOGGER_CONTROL_TELNET, fd);
  close(fd);
  ESP_LOGI(tag, "telnet client %d disconnected", fd);
}

//-- Creates, binds and listens on the console port. Returns the socket or -1.
static int telnetOpenListen(void)
{
  int listenFd = socket(AF_INET, SOCK_STREAM, 0);

  if (listenFd < 0)
  {
    ESP_LOGE(tag, "could not create the listen socket: errno %d", errno);
    return -1;
  }

  int reuse = 1;
  setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);

  struct sockaddr_in address;
  memset(&address, 0, sizeof address);
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(TELNET_PORT);

  if ((bind(listenFd, (struct sockaddr*)&address, sizeof address) != 0) ||
      (listen(listenFd, 1) != 0))
  {
    ESP_LOGE(tag, "could not listen on port %d: errno %d", TELNET_PORT, errno);
    close(listenFd);
    return -1;
  }

  ESP_LOGI(tag, "telnet console listening on port %d", TELNET_PORT);
  return listenFd;
}

static void telnetTaskLoop(void* argument)
{
  (void)argument;

  //-- The console survives its own bad days: a select() error or a lost
  //-- listen socket only drops the current session and reopens the socket a
  //-- moment later. The task is never deleted: a single lwIP hiccup (for
  //-- example a closed fd surfacing as EBADF) once took port 23 down until
  //-- the next reboot, which users experience as "telnet stops working".
  int listenFd = telnetOpenListen();
  int clientFd = -1;
  char line[TELNET_LINE_MAX];
  size_t linePos = 0u;
  uint8_t iacState = 0u;
  bool pendingCr = false;

  for (;;)
  {
    if (listenFd < 0)
    {
      listenFd = telnetOpenListen();

      if (listenFd < 0)
      {
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
    }

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(listenFd, &readSet);

    int highestFd = listenFd;

    if (clientFd >= 0)
    {
      FD_SET(clientFd, &readSet);

      if (clientFd > highestFd)
      {
        highestFd = clientFd;
      }
    }

    int ready = select(highestFd + 1, &readSet, NULL, NULL, NULL);

    if (ready < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }

      ESP_LOGW(tag, "select failed: errno %d, reopening the console", errno);

      if (clientFd >= 0)
      {
        telnetCloseClient(clientFd);
        clientFd = -1;
      }

      close(listenFd);
      listenFd = -1;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if ((clientFd >= 0) && FD_ISSET(clientFd, &readSet))
    {
      uint8_t buffer[TELNET_RECV_BUFFER_BYTES];
      int received = recv(clientFd, buffer, sizeof buffer, 0);

      if (received == 0)
      {
        telnetCloseClient(clientFd);
        clientFd = -1;
      }
      else if ((received < 0) && (errno != EINTR) && (errno != EAGAIN) && (errno != EWOULDBLOCK))
      {
        //-- Only a real error closes the session: a signal or a would-block
        //-- simply means no data this round.
        telnetCloseClient(clientFd);
        clientFd = -1;
      }
      else if (!telnetIsOwner(clientFd))
      {
        //-- The console slot moved to a newer telnet client while bytes
        //-- were in flight: the evict callback already said goodbye; finish
        //-- the session here.
        telnetCloseClient(clientFd);
        clientFd = -1;
      }
      else
      {
        for (int index = 0; index < received; index++)
        {
          uint8_t byte = buffer[index];

          //-- Skip telnet negotiation sequences (IAC <cmd> [opt]).
          if (iacState == 1u)
          {
            iacState = ((byte == TELNET_WILL) || (byte == TELNET_WONT) || (byte == TELNET_DO) ||
                        (byte == TELNET_DONT))
                           ? 2u
                           : ((byte == TELNET_IAC) ? 1u : 0u);
            continue;
          }

          if (iacState == 2u)
          {
            iacState = 0u;
            continue;
          }

          if (byte == TELNET_IAC)
          {
            iacState = 1u;
            continue;
          }

          if ((byte == '\r') || (byte == '\n'))
          {
            //-- A CR LF pair ends one line: the LF of a just-flushed CR is
            //-- skipped, so the pair never reads as an extra (empty) line.
            if ((byte == '\n') && pendingCr)
            {
              pendingCr = false;
              continue;
            }

            pendingCr = (byte == '\r');

            //-- An empty line normally does nothing; while a populate
            //-- awaits its file name it is the cancel answer.
            if ((linePos > 0u) || populateAwaitingFile)
            {
              line[linePos] = '\0';
              telnetSendText(clientFd, "\r\n");

              if (!handleLine(clientFd, line))
              {
                telnetCloseClient(clientFd);
                clientFd = -1;
                break;
              }

              linePos = 0u;
            }
          }
          else if ((byte == '\b') || (byte == 127u))
          {
            pendingCr = false;

            if (linePos > 0u)
            {
              linePos--;
              telnetSendText(clientFd, "\b \b");
            }
          }
          else if (isprint((int)byte))
          {
            pendingCr = false;

            if (linePos + 1u < sizeof line)
            {
              line[linePos++] = (char)byte;
              char echo[2] = {(char)byte, '\0'};
              telnetSend(clientFd, echo, 1u);
            }
          }
        }
      }
    }

    if (FD_ISSET(listenFd, &readSet))
    {
      struct sockaddr_in peer;
      socklen_t peerLength = sizeof peer;
      int newFd = accept(listenFd, (struct sockaddr*)&peer, &peerLength);

      if (newFd >= 0)
      {
        char peerIp[16];
        snprintf(peerIp, sizeof peerIp, "%s", inet_ntoa(peer.sin_addr));

        //-- Claiming the console slot evicts an older telnet session
        //-- through its arbiter callback (farewell line and shut-down
        //-- socket); the web page holds its own slot and keeps running
        //-- untouched.
        loggerControlClaim(LOGGER_CONTROL_TELNET, newFd);

        if ((clientFd >= 0) && (clientFd != newFd))
        {
          close(clientFd);
        }

        clientFd = newFd;
        linePos = 0u;
        iacState = 0u;
        pendingCr = false;
        populateAwaitingFile = false;
        populateMode = LOGGER_POPULATE_MERGE;

        const uint8_t willEcho[3] = {TELNET_IAC, TELNET_WILL, TELNET_OPT_ECHO};
        const uint8_t willSga[3] = {TELNET_IAC, TELNET_WILL, TELNET_OPT_SGA};
        bool greeted = telnetSend(clientFd, willEcho, sizeof willEcho);
        greeted = telnetSend(clientFd, willSga, sizeof willSga) && greeted;
        greeted = telnetPrintf(clientFd, "\r\nlastDSMRlogger %s - telnet console (%s)\r\n",
                               PROG_VERSION, peerIp) &&
                  greeted;
        greeted =
            telnetSendText(clientFd,
                           "This session takes the telnet console slot: the previous telnet\r\n"
                           "session, if any, is closed. An open web page keeps its own control.\r\n"
                           "Type help for the command list.\r\n") &&
            greeted;
        greeted = telnetSendText(clientFd, TELNET_PROMPT) && greeted;

        if (!greeted)
        {
          //-- The fresh socket is already dead (often a close racing with a
          //-- just-evicted sibling connection): do not keep a zombie in the
          //-- session slot, drop it and wait for the next client. Clearing
          //-- clientFd is mandatory: a closed fd left in the select() set
          //-- makes select fail with EBADF, which would tear down the
          //-- listen socket and refuse new console connections for a second.
          close(newFd);
          loggerControlRelease(LOGGER_CONTROL_TELNET, newFd);
          clientFd = -1;
          continue;
        }

        //-- Console-only reporting: never draw on the TFT board task from
        //-- here, the board mutex can be held for a long repaint window.
        ESP_LOGI(tag, "telnet client %d from %s took control", newFd, peerIp);
      }
    }
  }

  //-- Unreachable in normal operation (the loop self-heals instead of
  //-- leaving); kept so a future exit path still cleans up.
  if (clientFd >= 0)
  {
    telnetCloseClient(clientFd);
  }

  if (listenFd >= 0)
  {
    close(listenFd);
  }

  taskRunning = false;
  vTaskDelete(NULL);
}

//-- --------------------------------------------------------------------------
//-- Lifecycle
//-- --------------------------------------------------------------------------

esp_err_t loggerTelnetStart(void)
{
  if (taskRunning)
  {
    return ESP_OK;
  }

  loggerControlRegisterEvict(LOGGER_CONTROL_TELNET, telnetEvictCallback);

  taskRunning = true;
  BaseType_t created = xTaskCreate(telnetTaskLoop, "telnetConsole", TELNET_TASK_STACK_BYTES, NULL,
                                   TELNET_TASK_PRIORITY, &telnetTask);

  if (created != pdPASS)
  {
    taskRunning = false;
    ESP_LOGE(tag, "could not create the telnet console task");
    return ESP_ERR_NO_MEM;
  }

  return ESP_OK;
}
