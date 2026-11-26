//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_telnet.c - telnet console server on port 23
//-- Date: 2026-11-26 · Build: 25
//--
//-- A plain BSD-socket console task: it listens on port 23, serves exactly
//-- one session at a time and answers a command set from the receiver,
//-- store and settings APIs (read-only except populate (imports a LittleFS
//-- csv file into one meter ring), ring2csv (writes one meter ring to the
//-- volume), filesystem delete/rename (remove or rename one volume file,
//-- each after a typed confirmation), settings save/load (export every
//-- settings field as a plain ascii ini file on the volume and read one
//-- back into NVS),
//-- sync (commits all ring files at once) and restart (first
//-- synchronises the ring files and then reboots the device, all after a
//-- typed confirmation)). The verbose {0,1,2,3} command sets
//-- the reporting level at which the background event lines the store, MQTT
//-- and API paths send through the control arbiter reach the console (at
//-- level 3 the arbiter additionally mirrors the serial console log into the
//-- session), and help shows the level currently set; command and option
//-- words shorten to any unique leading part (popu m names populate months),
//-- a file name a command would ask for may follow the command directly
//-- (filesystem show RINGmonths.csv) and [Tab] completes the word under the
//-- cursor from the command table, the option table of the typed command or
//-- the files on the LittleFS volume; a bare [Enter] prints
//-- the help list, and the prompt the session shows is registered with the
//-- arbiter so event and mirror lines end on a re-printed prompt instead of
//-- a blank line; every write of this task brackets the shared console
//-- lock so an event line never interleaves into a command answer. The console slot is decided by
// the
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

//-- The program version string is defined (static) by the application entry
//-- (main/app_main.c) and read through its getter; the version and status
//-- commands show it.
extern const char* getAppVersion(void);

//-- Live Wi-Fi link state from the recovery manager in the application
//-- entry (main/app_main.c): the provisioner's own state latches, this one
//-- follows the station.
extern bool wifiManagerStationHasIp(void);

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

//-- Upper bound for the ring synchronisation the restart command performs
//-- before the reboot: a store task that cannot deliver (for instance a
//-- wall clock that never synchronised) only costs the console this wait.
#define TELNET_RESTART_SYNC_TIMEOUT_MS 10000u

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
//-- plain file-scope variables are enough; a fresh client resets them. The
//-- ring2csv export awaits its csv file name the same way (empty keeps the
//-- default YYMMDDhhmm-<name>.csv), the restart command awaits a typed
//-- y/yes.
static bool populateAwaitingFile = false;
static loggerMeterRing_t populateRing = LOGGER_RING_HOURS;
static loggerPopulateMode_t populateMode = LOGGER_POPULATE_MERGE;
static bool exportAwaitingName = false;
static loggerMeterRing_t exportRing = LOGGER_RING_HOURS;
static bool restartAwaitingConfirm = false;

//-- Pending filesystem and settings questions (the filesystem and settings
//-- commands below): while one is pending the next typed line is its answer
//-- and never a command. One session at a time, so plain file-scope
//-- variables are enough; a fresh client resets them. fsPendingName holds
//-- the file a delete removes or rename renames (the old name), fsPending-
//-- NewName the rename target awaiting the typed confirmation.
static bool fsAwaitingDeleteName = false;
static bool fsAwaitingOldName = false;
static bool fsAwaitingNewName = false;
static bool fsAwaitingShowName = false;
static bool fsAwaitingConfirm = false;
static bool fsConfirmIsRename = false;
static char fsPendingName[TELNET_LINE_MAX];
static char fsPendingNewName[TELNET_LINE_MAX];
static bool settingsAwaitingSaveName = false;
static bool settingsAwaitingLoadName = false;

//-- The question line currently awaiting a file name, kept so a [Tab]
//-- completion listing can re-print the question (and the answer typed so
//-- far) below the choices instead of leaving the user on a bare line.
static char pendingQuestion[TELNET_LINE_MAX];

//-- Every command word the console knows. Synonyms stay separate entries
//-- (raw for the raw telegram dump, ringfile and exit as older spellings)
//-- so a shortened word can name them too.
static const char* const telnetCommands[] = {
    "help",  "status",   "version",  "telegrams",  "telegram", "raw",
    "power", "showring", "ringfile", "filesystem", "settings", "populate",
    "quit",  "exit",     "ring2csv", "sync",       "verbose",  "restart",
};

//-- The option words of the commands that take them: the unique leading-part
//-- abbreviation the command words get applies here too (popu m names
//-- populate months) and [Tab] completes from these tables.
static const char* const filesystemActions[] = {"list", "delete", "rename", "show"};
static const char* const settingsActions[] = {"save", "load"};
static const char* const ringSelections[] = {"hours", "days", "months"};
static const char* const populateModeNames[] = {"overwrite", "update"};
static const char* const verboseLevelNames[] = {"0", "1", "2", "3"};

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
static const char* findCommand(const char* typed, bool* ambiguous)
{
  size_t typedLength = strlen(typed);
  const char* found = NULL;
  uint8_t hits = 0u;

  for (size_t index = 0u; index < (sizeof telnetCommands / sizeof telnetCommands[0]); index++)
  {
    if (strcasecmp(telnetCommands[index], typed) == 0)
    {
      if (ambiguous != NULL)
      {
        *ambiguous = false;
      }

      return telnetCommands[index];
    }

    if ((typedLength > 0u) && (strncasecmp(telnetCommands[index], typed, typedLength) == 0))
    {
      found = telnetCommands[index];
      hits++;
    }
  }

  if (ambiguous != NULL)
  {
    *ambiguous = (hits != 1u);
  }

  return (hits == 1u) ? found : NULL;
}

static const char* resolveCommand(int fd, const char* typed)
{
  bool ambiguous = false;
  const char* command = findCommand(typed, &ambiguous);

  if (command == NULL)
  {
    telnetPrintf(fd, "%s command '%s' - type help for the command list\r\n",
                 ambiguous ? "ambiguous" : "unknown", typed);
  }

  return command;
}

//-- Matches one typed option word against a table of option names the same
//-- way findCommand does for command words: a case-insensitive exact match
//-- always wins, otherwise the word must be an unambiguous leading part of
//-- exactly one name. Silent: callers report, [Tab] completes with it.
static const char* matchOption(const char* typed, const char* const* names, size_t nameCount,
                               bool* ambiguous)
{
  size_t typedLength = strlen(typed);
  const char* found = NULL;
  uint8_t hits = 0u;

  for (size_t index = 0u; index < nameCount; index++)
  {
    if (strcasecmp(names[index], typed) == 0)
    {
      if (ambiguous != NULL)
      {
        *ambiguous = false;
      }

      return names[index];
    }

    if ((typedLength > 0u) && (strncasecmp(names[index], typed, typedLength) == 0))
    {
      found = names[index];
      hits++;
    }
  }

  if (ambiguous != NULL)
  {
    *ambiguous = (hits != 1u);
  }

  return (hits == 1u) ? found : NULL;
}

//-- Resolves one typed option word against a table of option names,
//-- reporting unknown and ambiguous words on the session the way
//-- resolveCommand does for command words; label names the option set in
//-- the report ("ring", "filesystem action", ...) and the report lists the
//-- table's own names as the choices.
static const char* resolveOption(int fd, const char* typed, const char* const* names,
                                 size_t nameCount, const char* label)
{
  bool ambiguous = false;
  const char* found = matchOption(typed, names, nameCount, &ambiguous);

  if (found != NULL)
  {
    return found;
  }

  char choices[TELNET_PRINT_LINE_BYTES];
  choices[0] = '\0';
  size_t used = 0u;

  for (size_t index = 0u; index < nameCount; index++)
  {
    int written =
        snprintf(choices + used, sizeof choices - used, "%s%s",
                 (index == 0u) ? "" : ((index + 1u == nameCount) ? " or " : ", "), names[index]);

    if ((written < 0) || (used + (size_t)written >= sizeof choices))
    {
      break;
    }

    used += (size_t)written;
  }

  telnetPrintf(fd, "%s %s '%s' - use %s\r\n", ambiguous ? "ambiguous" : "unknown", label, typed,
               choices);
  return NULL;
}

//-- Maps a typed ring selector to one meter ring: the store's own name table
//-- first (it keeps the console's singular "month" spelling), then the
//-- unique leading-part abbreviation the command and option words get ("m"
//-- and "mo" both name the months ring).
static bool resolveRing(int fd, const char* typed, loggerMeterRing_t* outRing)
{
  if (loggerStoreRingFromName(typed, outRing))
  {
    return true;
  }

  const char* found = resolveOption(fd, typed, ringSelections,
                                    sizeof ringSelections / sizeof ringSelections[0], "ring");

  return ((found != NULL) && loggerStoreRingFromName(found, outRing));
}

//-- The text with its surrounding spaces removed (a typed file name may
//-- carry spaces around it; inner spaces stay, like the answer of a file
//-- question which takes the whole line).
static char* trimSpaces(char* text)
{
  while (isspace((unsigned char)*text))
  {
    text++;
  }

  size_t length = strlen(text);

  while ((length > 0u) && isspace((unsigned char)text[length - 1u]))
  {
    text[--length] = '\0';
  }

  return text;
}

//-- Splits the next space-delimited word off *cursor (its trailing space
//-- becomes a terminator) and moves the cursor past it. NULL when nothing
//-- is left. The words live in the caller's line buffer.
static char* nextArgument(char** cursor)
{
  char* start = trimSpaces(*cursor);
  char* end = start;

  while ((*end != '\0') && !isspace((unsigned char)*end))
  {
    end++;
  }

  if (*end != '\0')
  {
    *end++ = '\0';
  }

  *cursor = end;
  return (start[0] != '\0') ? start : NULL;
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
  //-- Console writes bracket the shared console lock: the verbose event
  //-- lines of the store, MQTT and API paths write to this very socket from
  //-- their own tasks, and a command answer must never interleave with one.
  loggerControlConsoleLock();
  bool delivered = true;
  const uint8_t* cursor = (const uint8_t*)data;

  while (length > 0u)
  {
    int written = send(fd, cursor, length, 0);

    if (written <= 0)
    {
      //-- A lost write is normally harmless (the client is gone), but the
      //-- farewell of an evicted session depends on it: log it for the
      //-- field, it is the only way a missed takeover line explains itself.
      LOGGER_LOGW(tag, "send to client %d failed: errno %d", fd, errno);
      delivered = false;
      break;
    }

    cursor += (size_t)written;
    length -= (size_t)written;
  }

  loggerControlConsoleUnlock();
  return delivered;
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

//-- Sends the prompt and registers it with the arbiter: background event
//-- and mirror lines re-print it after themselves, so the console keeps
//-- reading prompt-first and never leaves a blank line behind. A command
//-- question clears the registration (the question itself sends
//-- loggerControlConsoleSetPrompt(NULL) when it takes the prompt's place).
static bool telnetShowPrompt(int fd)
{
  loggerControlConsoleSetPrompt(TELNET_PROMPT);
  return telnetSendText(fd, TELNET_PROMPT);
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
          "  ring2csv {month|days|hours} [csv]\r\n"
          "                     write one meter ring to the volume (asks the csv file\r\n"
          "                     name unless it follows the command; empty =\r\n"
          "                     YYMMDDhhmm-<name>.csv, newest period first)\r\n"
          "  populate {hours|days|months} [overwrite|update] [file]\r\n"
          "                     import a LittleFS csv file into one meter ring: merge\r\n"
          "                     (default) fills free slots, overwrite clears the ring\r\n"
          "                     first, update only revises stored periods; the file\r\n"
          "                     may follow the command directly\r\n"
          "  filesystem {list|delete|rename|show} [file]\r\n"
          "                     LittleFS volume: list files with sizes, capacity\r\n"
          "                     and free space; delete and rename ask the file\r\n"
          "                     names (and an Are you sure (N/y) confirmation);\r\n"
          "                     show prints a file's contents skipping control\r\n"
          "                     characters except tab, CR and LF (shown as-is);\r\n"
          "                     the file may follow the command directly\r\n"
          "  settings {save|load} [file]\r\n"
          "                     save asks the ini file name (empty =\r\n"
          "                     YYMMDDhhmm-settings.ini, a plain name gets the\r\n"
          "                     .ini suffix) and writes every settings field as\r\n"
          "                     plain ascii ini to the volume; load asks the\r\n"
          "                     file and stores its values back into NVS\r\n"
          "                     (restart to apply them)\r\n"
          "  sync               write all ring files to storage now (resets the interval)\r\n");

  //-- The verbose line carries the reporting level currently set, so the
  //-- command list doubles as the current setting ("verbose 2 {0,1,2,3}").
  telnetPrintf(fd, "  verbose %d {0,1,2,3}\r\n", (int)loggerControlVerboseLevel());
  telnetSendText(fd, "                     0 no debug lines, 1 events + errors,\r\n"
                     "                     2 + telegram related, 3 + info + warnings\r\n"
                     "                     (3 also mirrors the serial console log)\r\n"
                     "  restart            synchronise the ring files and reboot the device\r\n"
                     "                     (asks for confirmation)\r\n"
                     "  quit               close this session\r\n"
                     "\r\ncommands and options may be shortened to a unique leading part\r\n"
                     "  (show == showring, popu m == populate months)\r\n"
                     "a file name may follow its command directly\r\n"
                     "  (filesystem show RINGmonths.csv)\r\n"
                     "[Tab] completes command, option and volume file names and lists\r\n"
                     "the choices when more than one matches\r\n"
                     "an empty line shows this command list too\r\n");
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

  //-- The recovery manager owns the live link state: the provisioner's
  //-- wifi_prov_get_ip_info() latches its first join and can keep reporting
  //-- the last address over an outage, so its answer only counts while the
  //-- manager actually sees an address in hand.
  if (wifiManagerStationHasIp() && (wifi_prov_get_ip_info(&ipInfo) == ESP_OK))
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
  telnetPrintf(fd, "\r\nfirmware %s, parserDSMR %s\r\n", getAppVersion(), dsmrParserVersion());

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

  snprintf(out->firmwareVersion, sizeof out->firmwareVersion, "%s", getAppVersion());
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

  if ((periodText != NULL) && (periodText[0] != '\0') && !resolveRing(fd, periodText, &ring))
  {
    //-- resolveRing reported the unknown or ambiguous ring.
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

//-- LittleFS volume overview (the filesystem list action): every file on the
//-- mounted storage volume with its size, followed by the total capacity and
//-- the free space. The walk
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

//-- --------------------------------------------------------------------------
//-- Volume file helpers (the filesystem and settings commands)
//-- --------------------------------------------------------------------------

//-- Answers the mount question shared by the volume-touching commands.
static bool telnetVolumeMounted(int fd)
{
  if (loggerStoreMountPoint() != NULL)
  {
    return true;
  }

  telnetPrintf(fd, "\r\nthe LittleFS storage volume is not mounted (meter store disabled)\r\n");
  return false;
}

//-- True when fileName names a regular file at the volume root.
static bool telnetFileExists(const char* fileName)
{
  char path[TELNET_PATH_BYTES];
  snprintf(path, sizeof path, "%s/%s", loggerStoreMountPoint(), fileName);

  struct stat details;
  memset(&details, 0, sizeof details);

  return (stat(path, &details) == 0) && S_ISREG(details.st_mode);
}

//-- Upper bound for a filesystem show dump: the bytes the console prints
//-- from one file before it reports the rest as truncated.
#define TELNET_SHOW_MAX_BYTES 65536u

//-- Bytes filesystem show passes on to the session: every printable byte
//-- plus tab, carriage return and line feed, which are shown as they are.
static bool isShownByte(uint8_t byte)
{
  return isprint((int)byte) || (byte == '\t') || (byte == '\r') || (byte == '\n');
}

//-- Prints a volume file to the session skipping every control character
//-- except tab, CR and LF: only bytes meant to be seen reach the socket, so
//-- a binary file can never emit escape or flow bytes into the console. The
//-- answer streams in chunks and stops after TELNET_SHOW_MAX_BYTES content
//-- bytes.
static void printVolumeFile(int fd, const char* fileName)
{
  char path[TELNET_PATH_BYTES];
  snprintf(path, sizeof path, "%s/%s", loggerStoreMountPoint(), fileName);

  FILE* file = fopen(path, "rb");

  if (file == NULL)
  {
    telnetPrintf(fd, "file '%s' could not be opened (errno %d)\r\n", fileName, errno);
    return;
  }

  telnetPrintf(fd, "\r\n%s:\r\n", fileName);

  uint8_t chunk[128];
  char printable[TELNET_PRINT_LINE_BYTES];
  size_t printablePos = 0u;
  size_t shownBytes = 0u;
  bool truncated = false;
  size_t readCount = 0u;

  while (!truncated && ((readCount = fread(chunk, 1u, sizeof chunk, file)) > 0u))
  {
    for (size_t index = 0u; index < readCount; index++)
    {
      if (shownBytes >= TELNET_SHOW_MAX_BYTES)
      {
        truncated = true;
        break;
      }

      if (isShownByte(chunk[index]))
      {
        shownBytes++;
        printable[printablePos++] = (char)chunk[index];

        if (printablePos == sizeof printable)
        {
          telnetSend(fd, printable, printablePos);
          printablePos = 0u;
        }
      }
    }
  }

  if (printablePos > 0u)
  {
    telnetSend(fd, printable, printablePos);
  }

  fclose(file);

  if (truncated)
  {
    telnetPrintf(fd, "\r\n... contents truncated at %" PRIu32 " bytes\r\n", (uint32_t)shownBytes);
  }
  else
  {
    telnetPrintf(fd, "\r\n(%" PRIu32 " bytes shown from '%s')\r\n", (uint32_t)shownBytes, fileName);
  }
}

//-- Final name of a settings export: the empty answer keeps the wall-clock
//-- stamp YYMMDDhhmm-settings.ini, a typed name gets the .ini suffix when
//-- it does not carry one (the same rule ring2csv applies to .csv names).
static void buildSettingsFileName(const char* requested, char* outName, size_t outSize)
{
  if ((requested == NULL) || (requested[0] == '\0'))
  {
    char stamp[16] = "0000000000";
    time_t now = time(NULL);
    struct tm brokenTime;

    if ((now >= 1700000000) && (localtime_r(&now, &brokenTime) != NULL))
    {
      strftime(stamp, sizeof stamp, "%y%m%d%H%M", &brokenTime);
    }

    snprintf(outName, outSize, "%s-settings.ini", stamp);
    return;
  }

  size_t length = strlen(requested);

  if ((length >= 4u) && (strcasecmp(requested + length - 4u, ".ini") == 0))
  {
    snprintf(outName, outSize, "%s", requested);
  }
  else
  {
    snprintf(outName, outSize, "%s.ini", requested);
  }
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

//-- --------------------------------------------------------------------------
//-- Question answers (shared by the pending-answer blocks of handleLine and
//-- by the file names that follow their command directly)
//-- --------------------------------------------------------------------------

//-- Answers the ring2csv csv-name question with a typed (or directly
//-- supplied) name; the default name is passed as NULL so the store stamps
//-- YYMMDDhhmm-<ring>.csv. Reports the outcome.
static void answerExportName(int fd, const char* name)
{
  uint32_t records = 0u;
  char finalName[TELNET_PATH_BYTES];
  finalName[0] = '\0';

  esp_err_t outcome =
      loggerStoreRingToCsvNamed(exportRing, name, &records, finalName, sizeof finalName);

  if (outcome != ESP_OK)
  {
    telnetPrintf(fd, "ring2csv failed: %s\r\n", esp_err_to_name(outcome));
    return;
  }

  telnetPrintf(fd, "wrote %" PRIu32 " record(s) to %s on the volume (newest first)\r\n", records,
               finalName);
}

//-- Answers the delete file question with a typed (or directly supplied)
//-- name: an unknown file is reported, an existing one gets the typed
//-- confirmation. True when the confirmation question is now open.
static bool answerFsDeleteName(int fd, const char* name)
{
  if (!telnetFileExists(name))
  {
    telnetPrintf(fd, "no such file on the volume: '%s'\r\n", name);
    return false;
  }

  snprintf(fsPendingName, sizeof fsPendingName, "%s", name);
  fsConfirmIsRename = false;
  fsAwaitingConfirm = true;
  loggerControlConsoleSetPrompt(NULL);
  telnetPrintf(fd, "\r\ndelete '%s' from the volume.\r\nAre you sure (N/y)? ", fsPendingName);
  return true;
}

//-- Answers the rename old-name question with a typed (or directly supplied)
//-- name: an unknown file is reported, an existing one is stored as the
//-- rename source and the new name is asked. True when a question is open.
static bool answerFsOldName(int fd, const char* name)
{
  if (!telnetFileExists(name))
  {
    telnetPrintf(fd, "no such file on the volume: '%s'\r\n", name);
    return false;
  }

  snprintf(fsPendingName, sizeof fsPendingName, "%s", name);
  fsAwaitingNewName = true;
  snprintf(pendingQuestion, sizeof pendingQuestion, "%s", "new name: ");
  loggerControlConsoleSetPrompt(NULL);
  telnetPrintf(fd, "\r\nnew name: ");
  return true;
}

//-- Answers the rename new-name question with a typed (or directly supplied)
//-- name: an existing name is refused, a free one gets the typed
//-- confirmation. True when the confirmation question is now open.
static bool answerFsNewName(int fd, const char* name)
{
  if (telnetFileExists(name))
  {
    telnetPrintf(fd, "a file named '%s' already exists on the volume\r\n", name);
    return false;
  }

  snprintf(fsPendingNewName, sizeof fsPendingNewName, "%s", name);
  fsConfirmIsRename = true;
  fsAwaitingConfirm = true;
  loggerControlConsoleSetPrompt(NULL);
  telnetPrintf(fd, "\r\nrename '%s' to '%s'.\r\nAre you sure (N/y)? ", fsPendingName,
               fsPendingNewName);
  return true;
}

//-- Answers the show file question with a typed (or directly supplied) name:
//-- an unknown file is reported, an existing one is printed.
static void answerFsShowName(int fd, const char* name)
{
  if (!telnetFileExists(name))
  {
    telnetPrintf(fd, "no such file on the volume: '%s'\r\n", name);
    return;
  }

  printVolumeFile(fd, name);
}

//-- Answers the settings save name question with a typed (or directly
//-- supplied) name; the default name is passed as NULL so the file is
//-- stamped YYMMDDhhmm-settings.ini. Reports the outcome.
static void answerSettingsSaveName(int fd, const char* requested)
{
  char finalName[TELNET_LINE_MAX + 8];
  buildSettingsFileName(requested, finalName, sizeof finalName);

  char path[TELNET_PATH_BYTES];
  snprintf(path, sizeof path, "%s/%s", loggerStoreMountPoint(), finalName);

  uint32_t fields = 0u;
  esp_err_t outcome = loggerSettingsExportToFile(path, &fields);

  if (outcome != ESP_OK)
  {
    telnetPrintf(fd, "settings save to %s failed: %s\r\n", finalName, esp_err_to_name(outcome));
    return;
  }

  telnetPrintf(fd, "wrote %" PRIu32 " setting(s) to %s on the volume\r\n", fields, finalName);
}

//-- Answers the settings load question with a typed (or directly supplied)
//-- name: an unknown file is reported, an existing one is read back into
//-- NVS (a restart applies the values).
static void answerSettingsLoadName(int fd, const char* name)
{
  if (!telnetFileExists(name))
  {
    telnetPrintf(fd, "no such file on the volume: '%s'\r\n", name);
    return;
  }

  char path[TELNET_PATH_BYTES];
  snprintf(path, sizeof path, "%s/%s", loggerStoreMountPoint(), name);

  uint32_t fields = 0u;
  esp_err_t outcome = loggerSettingsImportFromFile(path, &fields);

  if (outcome != ESP_OK)
  {
    telnetPrintf(fd, "settings load of %s failed: %s\r\n", name, esp_err_to_name(outcome));
    return;
  }

  telnetPrintf(fd, "loaded %" PRIu32 " setting(s) from %s into NVS - restart to apply them\r\n",
               fields, name);
}

//-- --------------------------------------------------------------------------
//-- Tab completion
//-- --------------------------------------------------------------------------

#define TELNET_COMPLETION_MAX_NAMES 24

//-- One completion run fills this table; the single console task is its only
//-- user, so a static buffer keeps the task stack clear of it. The truncated
//-- flag notes a listing that hit the bound (only file listings can).
static char completionNames[TELNET_COMPLETION_MAX_NAMES][TELNET_LINE_MAX];
static bool completionTruncated = false;

static int compareCompletionNames(const void* left, const void* right)
{
  return strcasecmp((const char*)left, (const char*)right);
}

//-- True while the session awaits an answer that names a volume file: [Tab]
//-- then completes the whole line against the files on the LittleFS volume.
static bool awaitingFileNameAnswer(void)
{
  return populateAwaitingFile || exportAwaitingName || fsAwaitingDeleteName || fsAwaitingOldName ||
         fsAwaitingNewName || fsAwaitingShowName || settingsAwaitingSaveName ||
         settingsAwaitingLoadName;
}

//-- Copies the table names that start with the prefix (case-insensitive)
//-- into the completion table, alphabetically. Returns the collected count.
static size_t collectTableNames(const char* prefix, const char* const* names, size_t nameCount)
{
  completionTruncated = false;
  size_t prefixLength = strlen(prefix);
  size_t count = 0u;

  for (size_t index = 0u; index < nameCount; index++)
  {
    if (strncasecmp(names[index], prefix, prefixLength) != 0)
    {
      continue;
    }

    if (count < TELNET_COMPLETION_MAX_NAMES)
    {
      snprintf(completionNames[count], TELNET_LINE_MAX, "%s", names[index]);
      count++;
    }
  }

  qsort(completionNames, count, sizeof completionNames[0], compareCompletionNames);
  return count;
}

//-- The same for the files on the LittleFS volume.
static size_t collectVolumeNames(const char* prefix)
{
  completionTruncated = false;
  const char* mountPoint = loggerStoreMountPoint();

  if (mountPoint == NULL)
  {
    return 0u;
  }

  DIR* directory = opendir(mountPoint);

  if (directory == NULL)
  {
    return 0u;
  }

  size_t prefixLength = strlen(prefix);
  size_t count = 0u;
  struct dirent* entry = NULL;

  while ((entry = readdir(directory)) != NULL)
  {
    if ((strcmp(entry->d_name, ".") == 0) || (strcmp(entry->d_name, "..") == 0))
    {
      continue;
    }

    if ((prefixLength > 0u) && (strncasecmp(entry->d_name, prefix, prefixLength) != 0))
    {
      continue;
    }

    if (count < TELNET_COMPLETION_MAX_NAMES)
    {
      //-- The precision keeps a longer d_name inside the row and silence
      //-- about it; volume names stay well below this bound.
      snprintf(completionNames[count], TELNET_LINE_MAX, "%.*s", TELNET_LINE_MAX - 1, entry->d_name);
      count++;
    }
    else
    {
      completionTruncated = true;
    }
  }

  closedir(directory);

  qsort(completionNames, count, sizeof completionNames[0], compareCompletionNames);
  return count;
}

//-- Completes the word the cursor sits on when [Tab] is typed. At the
//-- prompt the first word completes from the command table and the words
//-- behind it from the option table of the command typed or, for the file
//-- arguments of the filesystem, settings, populate and ring2csv commands,
//-- from the files on the volume; while a file question is open the whole
//-- line completes against the volume files. One match is typed out for the
//-- user, several are listed and the prompt or question is re-printed below
//-- the list so the text typed so far stays visible.
static void handleTab(int fd, char* line, size_t* linePos)
{
  const char* const* tableNames = NULL;
  size_t tableNameCount = 0u;
  bool fileCompletion = false;
  size_t tokenStart = 0u;
  bool atQuestion = awaitingFileNameAnswer();
  char prefix[TELNET_LINE_MAX];

  if (!atQuestion)
  {
    //-- The word at the cursor: everything after the last space typed.
    tokenStart = *linePos;

    while ((tokenStart > 0u) && (line[tokenStart - 1u] != ' '))
    {
      tokenStart--;
    }

    //-- How many words sit before it: zero names the command word.
    size_t wordIndex = 0u;
    bool betweenWords = true;

    for (size_t index = 0u; index < tokenStart; index++)
    {
      if (line[index] == ' ')
      {
        betweenWords = true;
      }
      else if (betweenWords)
      {
        betweenWords = false;
        wordIndex++;
      }
    }

    memcpy(prefix, line + tokenStart, *linePos - tokenStart);
    prefix[*linePos - tokenStart] = '\0';

    if (wordIndex == 0u)
    {
      tableNames = telnetCommands;
      tableNameCount = sizeof telnetCommands / sizeof telnetCommands[0];
    }
    else
    {
      //-- The command word decides which table (or the volume) completes.
      char firstWord[TELNET_LINE_MAX];
      size_t firstLength = 0u;

      while ((firstLength < (sizeof firstWord - 1u)) && (firstLength < tokenStart) &&
             (line[firstLength] != ' '))
      {
        firstWord[firstLength] = line[firstLength];
        firstLength++;
      }

      firstWord[firstLength] = '\0';

      bool ambiguous = false;
      const char* command = findCommand(firstWord, &ambiguous);

      if (command == NULL)
      {
        return;
      }

      if (strcmp(command, "filesystem") == 0)
      {
        if (wordIndex == 1u)
        {
          tableNames = filesystemActions;
          tableNameCount = sizeof filesystemActions / sizeof filesystemActions[0];
        }
        else
        {
          fileCompletion = true;
        }
      }
      else if (strcmp(command, "settings") == 0)
      {
        if (wordIndex == 1u)
        {
          tableNames = settingsActions;
          tableNameCount = sizeof settingsActions / sizeof settingsActions[0];
        }
        else
        {
          fileCompletion = true;
        }
      }
      else if (strcmp(command, "populate") == 0)
      {
        if (wordIndex == 1u)
        {
          tableNames = ringSelections;
          tableNameCount = sizeof ringSelections / sizeof ringSelections[0];
        }
        else if (wordIndex == 2u)
        {
          //-- The third word is the import mode or, when it names no mode,
          //-- already the csv file to import.
          if (matchOption(prefix, populateModeNames,
                          sizeof populateModeNames / sizeof populateModeNames[0], NULL) != NULL)
          {
            tableNames = populateModeNames;
            tableNameCount = sizeof populateModeNames / sizeof populateModeNames[0];
          }
          else
          {
            fileCompletion = true;
          }
        }
        else
        {
          fileCompletion = true;
        }
      }
      else if (strcmp(command, "ring2csv") == 0)
      {
        if (wordIndex == 1u)
        {
          tableNames = ringSelections;
          tableNameCount = sizeof ringSelections / sizeof ringSelections[0];
        }
        else
        {
          fileCompletion = true;
        }
      }
      else if ((strcmp(command, "showring") == 0) || (strcmp(command, "ringfile") == 0))
      {
        if (wordIndex == 1u)
        {
          tableNames = ringSelections;
          tableNameCount = sizeof ringSelections / sizeof ringSelections[0];
        }
        else
        {
          return;
        }
      }
      else if (strcmp(command, "verbose") == 0)
      {
        if (wordIndex == 1u)
        {
          tableNames = verboseLevelNames;
          tableNameCount = sizeof verboseLevelNames / sizeof verboseLevelNames[0];
        }
        else
        {
          return;
        }
      }
      else
      {
        return;
      }
    }
  }
  else
  {
    //-- A file question is open: the whole line is the file name.
    fileCompletion = true;
    memcpy(prefix, line, *linePos);
    prefix[*linePos] = '\0';
  }

  size_t count = fileCompletion ? collectVolumeNames(prefix)
                                : collectTableNames(prefix, tableNames, tableNameCount);

  if ((count == 0u) || ((count == 1u) && (strcasecmp(prefix, completionNames[0]) == 0)))
  {
    //-- Nothing matches, or the word is already typed in full.
    return;
  }

  if (count == 1u)
  {
    char replacement[TELNET_LINE_MAX + 2];
    snprintf(replacement, sizeof replacement, "%s", completionNames[0]);

    if (tableNames != NULL)
    {
      //-- Commands and options end here: the space invites the next word.
      strcat(replacement, " ");
    }

    if ((tokenStart + strlen(replacement) + 1u) > TELNET_LINE_MAX)
    {
      return;
    }

    //-- Erase the typed word, echo the completed one and keep the line.
    for (size_t index = *linePos; index > tokenStart; index--)
    {
      telnetSendText(fd, "\b \b");
    }

    telnetSendText(fd, replacement);
    memcpy(line + tokenStart, replacement, strlen(replacement) + 1u);
    *linePos = tokenStart + strlen(replacement);
    return;
  }

  //-- Several matches: list them, then re-print the question or prompt
  //-- with the text typed so far below the list.
  telnetSendText(fd, "\r\n");

  for (size_t index = 0u; index < count; index++)
  {
    telnetPrintf(fd, "%s\r\n", completionNames[index]);
  }

  if (completionTruncated)
  {
    telnetSendText(fd, "...\r\n");
  }

  if (atQuestion)
  {
    telnetSendText(fd, pendingQuestion);
  }
  else
  {
    telnetShowPrompt(fd);
  }

  //-- Only the typed part of the buffer: the bytes behind the cursor are
  //-- stale from earlier, longer lines.
  telnetSend(fd, line, *linePos);
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

    telnetShowPrompt(fd);
    return true;
  }

  //-- The ring2csv export awaits its csv file name the same way; an empty
  //-- line is not a cancel but the default name (RING<ring>.csv).
  if (exportAwaitingName)
  {
    exportAwaitingName = false;
    answerExportName(fd, (*line != '\0') ? line : NULL);
    telnetShowPrompt(fd);
    return true;
  }

  //-- The restart command awaits a typed yes; the question defaults to no,
  //-- so anything but y/yes (an empty line included) cancels.
  if (restartAwaitingConfirm)
  {
    restartAwaitingConfirm = false;

    if ((strcasecmp(line, "y") == 0) || (strcasecmp(line, "yes") == 0))
    {
      telnetPrintf(fd, "\r\nrestarting lastDSMRlogger ...\r\n");

      //-- A restart always synchronises first: the staged ring records must
      //-- have reached storage before the reboot tears the stack down. The
      //-- store task commits all three rings (the sync command's behaviour,
      //-- interval timer restarted) and the console waits, bounded, for the
      //-- commit to complete.
      telnetSendText(fd, "synchronising the ring files ...\r\n");

      if (loggerStoreRequestSyncAndWait(TELNET_RESTART_SYNC_TIMEOUT_MS))
      {
        telnetSendText(fd, "ring files synchronized\r\n");
      }
      else
      {
        telnetSendText(fd, "ring synchronisation did not confirm in time "
                           "(is the meter store running?)\r\n");
      }

      //-- A short grace period so the farewell really leaves the socket
      //-- before the reboot takes the stack down.
      vTaskDelay(pdMS_TO_TICKS(500));
      esp_restart();
    }

    telnetPrintf(fd, "restart cancelled\r\n");
    telnetShowPrompt(fd);
    return true;
  }

  //-- filesystem delete awaits the name of the file to remove: an unknown
  //-- file is reported, an existing one is confirmed before it is removed.
  if (fsAwaitingDeleteName)
  {
    fsAwaitingDeleteName = false;

    if (*line == '\0')
    {
      telnetPrintf(fd, "delete cancelled\r\n");
    }
    else if (!answerFsDeleteName(fd, line))
    {
      telnetShowPrompt(fd);
    }

    return true;
  }

  //-- filesystem rename awaits the old name first: the target is only asked
  //-- for when there really is a file to rename.
  if (fsAwaitingOldName)
  {
    fsAwaitingOldName = false;

    if (*line == '\0')
    {
      telnetPrintf(fd, "rename cancelled\r\n");
    }
    else if (!answerFsOldName(fd, line))
    {
      telnetShowPrompt(fd);
    }

    return true;
  }

  //-- The second rename question awaits the new name: an existing name is
  //-- refused, a free one is confirmed before the rename happens.
  if (fsAwaitingNewName)
  {
    fsAwaitingNewName = false;

    if (*line == '\0')
    {
      telnetPrintf(fd, "rename cancelled\r\n");
    }
    else if (!answerFsNewName(fd, line))
    {
      telnetShowPrompt(fd);
    }

    return true;
  }

  //-- filesystem show awaits the file to print (tab, CR and LF shown as-is).
  if (fsAwaitingShowName)
  {
    fsAwaitingShowName = false;

    if (*line == '\0')
    {
      telnetPrintf(fd, "show cancelled\r\n");
    }
    else
    {
      answerFsShowName(fd, line);
    }

    telnetShowPrompt(fd);
    return true;
  }

  //-- The delete and rename confirmations default to no like the restart
  //-- question does: anything but y/yes (an empty line included) cancels.
  if (fsAwaitingConfirm)
  {
    fsAwaitingConfirm = false;

    if ((strcasecmp(line, "y") == 0) || (strcasecmp(line, "yes") == 0))
    {
      char path[TELNET_PATH_BYTES];
      snprintf(path, sizeof path, "%s/%s", loggerStoreMountPoint(), fsPendingName);

      if (!fsConfirmIsRename)
      {
        if (remove(path) == 0)
        {
          telnetPrintf(fd, "deleted '%s' from the volume\r\n", fsPendingName);
        }
        else
        {
          telnetPrintf(fd, "delete of '%s' failed (errno %d)\r\n", fsPendingName, errno);
        }
      }
      else
      {
        char newPath[TELNET_PATH_BYTES];
        snprintf(newPath, sizeof newPath, "%s/%s", loggerStoreMountPoint(), fsPendingNewName);

        if (rename(path, newPath) == 0)
        {
          telnetPrintf(fd, "renamed '%s' to '%s' on the volume\r\n", fsPendingName,
                       fsPendingNewName);
        }
        else
        {
          telnetPrintf(fd, "rename of '%s' failed (errno %d)\r\n", fsPendingName, errno);
        }
      }
    }
    else
    {
      telnetPrintf(fd, "cancelled\r\n");
    }

    telnetShowPrompt(fd);
    return true;
  }

  //-- settings save awaits the ini file name: the empty answer keeps the
  //-- stamped default, a typed name gets the .ini suffix; every settings
  //-- field then lands at the volume root as one plain ascii ini file.
  if (settingsAwaitingSaveName)
  {
    settingsAwaitingSaveName = false;
    answerSettingsSaveName(fd, (*line != '\0') ? line : NULL);
    telnetShowPrompt(fd);
    return true;
  }

  //-- settings load awaits the file to read back: the load needs a name, so
  //-- an empty line cancels; the values land in NVS and apply on a restart.
  if (settingsAwaitingLoadName)
  {
    settingsAwaitingLoadName = false;

    if (*line == '\0')
    {
      telnetPrintf(fd, "load cancelled\r\n");
    }
    else
    {
      answerSettingsLoadName(fd, line);
    }

    telnetShowPrompt(fd);
    return true;
  }

  //-- A bare [Enter] answers the help list: the console never sits silent on
  //-- an empty line (while a question is open the empty line was already
  //-- taken as the answer above).
  if (*line == '\0')
  {
    printHelp(fd);
    telnetShowPrompt(fd);
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
    telnetShowPrompt(fd);
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
    //-- The first word picks the ring (no default: exporting is a write),
    //-- the optional rest names the csv file (empty keeps the default
    //-- YYMMDDhhmm-<name>.csv, stamped with the wall clock at export time,
    //-- a plain name gets the .csv suffix); without it the command asks.
    //-- The export lands at the volume root newest period first, ready for
    //-- the SMB share and a populate re-import.
    char* cursor = (arguments != NULL) ? arguments : (char*)"";
    char* ringText = nextArgument(&cursor);
    loggerMeterRing_t ring = LOGGER_RING_HOURS;

    if (ringText == NULL)
    {
      telnetPrintf(fd, "usage: ring2csv {hours|days|months} [csv file]\r\n");
    }
    else if (!resolveRing(fd, ringText, &ring))
    {
      //-- resolveRing reported the unknown or ambiguous ring.
    }
    else
    {
      exportRing = ring;
      char* csvName = trimSpaces(cursor);

      if (csvName[0] != '\0')
      {
        answerExportName(fd, csvName);
      }
      else
      {
        exportAwaitingName = true;

        //-- The question replaces the prompt: an event line landing while the
        //-- question waits must not re-print a prompt below the answer.
        snprintf(pendingQuestion, sizeof pendingQuestion,
                 "new csv file name (empty = YYMMDDhhmm-%s.csv): ", loggerStoreRingName(ring));
        loggerControlConsoleSetPrompt(NULL);
        telnetPrintf(
            fd, "\r\nexporting the %s ring.\r\nnew csv file name (empty = YYMMDDhhmm-%s.csv): ",
            loggerStoreRingName(ring), loggerStoreRingName(ring));
        return true; //-- the file name replaces the next prompt
      }
    }
  }
  else if (strcmp(command, "sync") == 0)
  {
    //-- Force the ring files to storage at once and restart the interval
    //-- timer from this moment; the store task confirms on the console log.
    if (loggerStoreRequestSync())
    {
      telnetPrintf(fd, "\r\nring synchronisation requested: the ring files are written now "
                       "and the synchronisation interval restarts\r\n");
    }
    else
    {
      telnetPrintf(fd, "\r\nthe meter store is not running\r\n");
    }
  }
  else if (strcmp(command, "verbose") == 0)
  {
    if (arguments != NULL)
    {
      while (isspace((unsigned char)*arguments))
      {
        arguments++;
      }
    }

    if ((arguments == NULL) || (arguments[0] == '\0'))
    {
      //-- No level typed: show the usage together with the level currently
      //-- set, so "verbose" alone answers the same question help does.
      loggerVerboseLevel_t current = loggerControlVerboseLevel();
      telnetPrintf(fd,
                   "\r\nusage: verbose {0,1,2,3} (currently %d: %s)\r\n"
                   "  0  no debug lines\r\n"
                   "  1  events and errors\r\n"
                   "  2  events, telegram related and errors\r\n"
                   "  3  events, info, warnings and errors, plus a copy of\r\n"
                   "     every serial console log line the monitor shows\r\n",
                   (int)current, loggerControlVerboseName(current));
    }
    else
    {
      char* end = NULL;
      long value = strtol(arguments, &end, 10);

      if ((end == arguments) || ((*end != '\0') && !isspace((unsigned char)*end)) || (value < 0) ||
          (value >= (long)LOGGER_VERBOSE_LEVEL_COUNT))
      {
        telnetPrintf(fd, "unknown verbose level '%s' - use 0, 1, 2 or 3\r\n", arguments);
      }
      else
      {
        loggerVerboseLevel_t level = loggerControlVerboseSet((loggerVerboseLevel_t)value);
        telnetPrintf(fd, "\r\nconsole event reporting: %d (%s)\r\n", (int)level,
                     loggerControlVerboseName(level));
      }
    }
  }
  else if (strcmp(command, "restart") == 0)
  {
    restartAwaitingConfirm = true;
    loggerControlConsoleSetPrompt(NULL);
    telnetPrintf(fd, "\r\nrestart the device now? are you sure (N/y)? ");
    return true; //-- the answer replaces the next prompt
  }
  else if (strcmp(command, "filesystem") == 0)
  {
    //-- The action word picks the volume operation (it shortens like the
    //-- command words: file s == filesystem show): list reports the volume
    //-- (the old store command), delete and rename confirm before they touch
    //-- a file, show prints one file's contents (tab, CR and LF shown as-is,
    //-- other control characters skipped). The file a command would ask for
    //-- may follow the action directly; without it the question is asked.
    char* cursor = (arguments != NULL) ? arguments : (char*)"";
    char* actionText = nextArgument(&cursor);
    const char* action = (actionText != NULL)
                             ? resolveOption(fd, actionText, filesystemActions,
                                             sizeof filesystemActions / sizeof filesystemActions[0],
                                             "filesystem action")
                             : NULL;

    if (actionText == NULL)
    {
      telnetPrintf(fd, "usage: filesystem {list|delete|rename|show} [file]\r\n");
    }
    else if (action == NULL)
    {
      //-- resolveOption reported the unknown or ambiguous action.
    }
    else if (strcmp(action, "list") == 0)
    {
      printStore(fd);
    }
    else if (strcmp(action, "delete") == 0)
    {
      if (telnetVolumeMounted(fd))
      {
        char* fileName = trimSpaces(cursor);

        if (fileName[0] != '\0')
        {
          if (!answerFsDeleteName(fd, fileName))
          {
            telnetShowPrompt(fd);
          }

          return true;
        }

        fsAwaitingDeleteName = true;
        snprintf(pendingQuestion, sizeof pendingQuestion, "%s", "delete what file: ");
        loggerControlConsoleSetPrompt(NULL);
        telnetPrintf(fd, "\r\ndelete what file: ");
        return true;
      }
    }
    else if (strcmp(action, "rename") == 0)
    {
      if (telnetVolumeMounted(fd))
      {
        char* oldName = nextArgument(&cursor);

        if (oldName == NULL)
        {
          fsAwaitingOldName = true;
          snprintf(pendingQuestion, sizeof pendingQuestion, "%s", "rename old name: ");
          loggerControlConsoleSetPrompt(NULL);
          telnetPrintf(fd, "\r\nrename old name: ");
          return true;
        }

        if (!answerFsOldName(fd, oldName))
        {
          telnetShowPrompt(fd);
          return true;
        }

        //-- The new name may ride along too; without it the question asks.
        char* newName = trimSpaces(cursor);

        if ((newName[0] != '\0') && !answerFsNewName(fd, newName))
        {
          telnetShowPrompt(fd);
        }

        return true;
      }
    }
    else if (strcmp(action, "show") == 0)
    {
      if (telnetVolumeMounted(fd))
      {
        char* fileName = trimSpaces(cursor);

        if (fileName[0] != '\0')
        {
          answerFsShowName(fd, fileName);
        }
        else
        {
          fsAwaitingShowName = true;
          snprintf(pendingQuestion, sizeof pendingQuestion, "%s", "show what file: ");
          loggerControlConsoleSetPrompt(NULL);
          telnetPrintf(fd, "\r\nshow what file: ");
          return true;
        }
      }
    }
  }
  else if (strcmp(command, "settings") == 0)
  {
    //-- save writes every settings field as a plain ascii ini file on the
    //-- volume, load reads such a file back into NVS; the action shortens
    //-- like the command words and the file name may follow it directly;
    //-- without one save keeps its stamped default and load asks.
    char* cursor = (arguments != NULL) ? arguments : (char*)"";
    char* actionText = nextArgument(&cursor);
    const char* action =
        (actionText != NULL)
            ? resolveOption(fd, actionText, settingsActions,
                            sizeof settingsActions / sizeof settingsActions[0], "settings action")
            : NULL;

    if (actionText == NULL)
    {
      telnetPrintf(fd, "usage: settings {save|load} [file]\r\n");
    }
    else if (action == NULL)
    {
      //-- resolveOption reported the unknown or ambiguous action.
    }
    else if (strcmp(action, "save") == 0)
    {
      if (telnetVolumeMounted(fd))
      {
        char* fileName = trimSpaces(cursor);

        if (fileName[0] != '\0')
        {
          answerSettingsSaveName(fd, fileName);
        }
        else
        {
          settingsAwaitingSaveName = true;
          snprintf(pendingQuestion, sizeof pendingQuestion, "%s",
                   "save settings to (empty = YYMMDDhhmm-settings.ini): ");
          loggerControlConsoleSetPrompt(NULL);
          telnetPrintf(fd, "\r\nsave settings to (empty = YYMMDDhhmm-settings.ini): ");
          return true;
        }
      }
    }
    else if (strcmp(action, "load") == 0)
    {
      if (telnetVolumeMounted(fd))
      {
        char* fileName = trimSpaces(cursor);

        if (fileName[0] != '\0')
        {
          answerSettingsLoadName(fd, fileName);
        }
        else
        {
          settingsAwaitingLoadName = true;
          snprintf(pendingQuestion, sizeof pendingQuestion, "%s", "load what settings file: ");
          loggerControlConsoleSetPrompt(NULL);
          telnetPrintf(fd, "\r\nload what settings file: ");
          return true;
        }
      }
    }
  }
  else if (strcmp(command, "populate") == 0)
  {
    //-- The first word picks the ring (no default: importing is a write),
    //-- the optional second one how the ring's current records are treated
    //-- (merge is the default) and the optional rest names the csv file to
    //-- import. Ring and mode shorten like the command words (popu m names
    //-- populate months); a word that names no mode is the file, and
    //-- without one the command asks.
    char* cursor = (arguments != NULL) ? arguments : (char*)"";
    char* ringText = nextArgument(&cursor);
    loggerMeterRing_t ring = LOGGER_RING_HOURS;
    loggerPopulateMode_t mode = LOGGER_POPULATE_MERGE;

    if (ringText == NULL)
    {
      telnetPrintf(fd, "usage: populate {hours|days|months} [overwrite|update] [file]\r\n");
    }
    else if (!resolveRing(fd, ringText, &ring))
    {
      //-- resolveRing reported the unknown or ambiguous ring.
    }
    else
    {
      //-- The second word is the mode (or its shortened form) or, when it
      //-- names no mode, the csv file itself.
      char* fileName = cursor;
      char modeWord[TELNET_LINE_MAX];
      size_t wordLength = 0u;

      while ((wordLength < (sizeof modeWord - 1u)) && (fileName[wordLength] != '\0') &&
             !isspace((unsigned char)fileName[wordLength]))
      {
        modeWord[wordLength] = fileName[wordLength];
        wordLength++;
      }

      modeWord[wordLength] = '\0';

      const char* modeHit =
          (modeWord[0] != '\0')
              ? matchOption(modeWord, populateModeNames,
                            sizeof populateModeNames / sizeof populateModeNames[0], NULL)
              : NULL;

      if (modeHit != NULL)
      {
        mode = (strcasecmp(modeHit, "overwrite") == 0) ? LOGGER_POPULATE_OVERWRITE
                                                       : LOGGER_POPULATE_UPDATE;
        fileName = trimSpaces(fileName + wordLength);
      }
      else
      {
        fileName = trimSpaces(fileName);
      }

      populateRing = ring;
      populateMode = mode;

      if (fileName[0] != '\0')
      {
        runPopulate(fd, fileName);
      }
      else
      {
        populateAwaitingFile = true;
        snprintf(pendingQuestion, sizeof pendingQuestion, "%s", TELNET_POPULATE_QUESTION);
        loggerControlConsoleSetPrompt(NULL);
        telnetPrintf(fd, "\r\nimporting into the %s ring%s.\r\n%s", loggerStoreRingName(ring),
                     populateModeLabel(mode), TELNET_POPULATE_QUESTION);
        return true; //-- the file name replaces the next prompt
      }
    }
  }
  else if ((strcmp(command, "quit") == 0) || (strcmp(command, "exit") == 0))
  {
    telnetSendText(fd, "Goodbye.\r\n");
    return false;
  }

  telnetShowPrompt(fd);
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
  loggerControlConsoleSetPrompt(NULL);
  loggerControlRelease(LOGGER_CONTROL_TELNET, fd);
  close(fd);
  LOGGER_LOGI(tag, "telnet client %d disconnected", fd);
}

//-- Creates, binds and listens on the console port. Returns the socket or -1.
static int telnetOpenListen(void)
{
  int listenFd = socket(AF_INET, SOCK_STREAM, 0);

  if (listenFd < 0)
  {
    LOGGER_LOGE(tag, "could not create the listen socket: errno %d", errno);
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
    LOGGER_LOGE(tag, "could not listen on port %d: errno %d", TELNET_PORT, errno);
    close(listenFd);
    return -1;
  }

  LOGGER_LOGI(tag, "telnet console listening on port %d", TELNET_PORT);
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

      LOGGER_LOGW(tag, "select failed: errno %d, reopening the console", errno);

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

            //-- Every finished line goes to the command handler: an empty
            //-- line prints the help list there, and while a question is
            //-- open (populate file, ring2csv name, restart confirmation,
            //-- the filesystem delete/rename/show names and their
            //-- confirmations, the settings save/load names) the empty line
            //-- is the answer.
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
          else if (byte == '\t')
          {
            //-- [Tab] completes the word under the cursor: command, option
            //-- or volume file name (see handleTab).
            pendingCr = false;
            handleTab(clientFd, line, &linePos);
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
        exportAwaitingName = false;
        restartAwaitingConfirm = false;
        fsAwaitingDeleteName = false;
        fsAwaitingOldName = false;
        fsAwaitingNewName = false;
        fsAwaitingShowName = false;
        fsAwaitingConfirm = false;
        settingsAwaitingSaveName = false;
        settingsAwaitingLoadName = false;

        const uint8_t willEcho[3] = {TELNET_IAC, TELNET_WILL, TELNET_OPT_ECHO};
        const uint8_t willSga[3] = {TELNET_IAC, TELNET_WILL, TELNET_OPT_SGA};
        bool greeted = telnetSend(clientFd, willEcho, sizeof willEcho);
        greeted = telnetSend(clientFd, willSga, sizeof willSga) && greeted;
        greeted = telnetPrintf(clientFd, "\r\nlastDSMRlogger %s - telnet console (%s)\r\n",
                               getAppVersion(), peerIp) &&
                  greeted;
        greeted =
            telnetSendText(clientFd,
                           "This session takes the telnet console slot: the previous telnet\r\n"
                           "session, if any, is closed. An open web page keeps its own control.\r\n"
                           "Type help for the command list, an empty line shows it too.\r\n") &&
            greeted;
        greeted = telnetShowPrompt(clientFd) && greeted;

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
        LOGGER_LOGI(tag, "telnet client %d from %s took control", newFd, peerIp);
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
    LOGGER_LOGE(tag, "could not create the telnet console task");
    return ESP_ERR_NO_MEM;
  }

  return ESP_OK;
}
