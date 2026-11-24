//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_control.h - per-channel control client arbiter
//-- Date: 2026-11-24 · Build: 7
//--
//-- Exactly one operator per control channel: the arbiter tracks one owner
//-- for the web browser (its /ws session) and one owner for the telnet
//-- console, so a browser page and a telnet session are active at the same
//-- time. A new client only ever displaces the previous owner of its own
//-- channel and never touches the other one. The previous owner is evicted
//-- through the evict callback its own server registered, so this component
//-- never needs to know about esp_http_server or sockets and neither server
//-- has to depend on the other. The arbiter also carries the console's
//-- verbose reporting level and the event sink that writes background-event
//-- lines to whichever session currently owns the telnet slot.
//-- ==========================================================================

#ifndef LOGGER_CONTROL_H
#define LOGGER_CONTROL_H

#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"

typedef enum
{
  //-- A browser's /ws session on the SPA server.
  LOGGER_CONTROL_WEB = 0,

  //-- The single client of the telnet console server.
  LOGGER_CONTROL_TELNET,

  LOGGER_CONTROL_CLIENT_COUNT
} loggerControlClient;

//-- Called with the evicted owner when a newer client of the same channel
//-- claims its slot. The callback must only hand the message to that client
//-- and end its session; it must not touch the arbiter. It may run on the
//-- claiming client's task, which for a web eviction is the httpd task and
//-- for a telnet eviction the httpd task or the telnet task.
typedef void (*loggerControlEvictFn)(loggerControlClient evicted, int evictedFd);

//-- Creates the arbiter state. Call once from app_main before any server can
//-- start; starting a server without it only degrades the guard, not the
//-- servers themselves.
esp_err_t loggerControlInit(void);

//-- Registers how to evict the current owner of the given kind. Each server
//-- calls this once at start-up, before its first claim.
void loggerControlRegisterEvict(loggerControlClient client, loggerControlEvictFn evict);

//-- Hands the channel of the given kind to fd. False when that fd already
//-- owns the channel (a cheap no-op, safe to call on every frame); true when
//-- the owner of that channel changed, in which case the previous owner's
//-- evict callback has already run. The other channel is never touched.
bool loggerControlClaim(loggerControlClient client, int fd);

//-- Clears ownership of the channel only when fd is still its owner, so a
//-- late close of an already-evicted client can never clear a newer owner.
void loggerControlRelease(loggerControlClient client, int fd);

//-- Copies the fd of the client that currently owns the channel of the
//-- given kind. False when that channel has no owner; the state of the
//-- other channel is never reported here.
bool loggerControlGetOwner(loggerControlClient client, int* fd);

//-- --------------------------------------------------------------------------
//-- Console event reporting (the console's verbose command)
//-- --------------------------------------------------------------------------

//-- How loudly the telnet console reports background lines (ring
//-- synchronisations, MQTT traffic, API updates): 0 stays silent, 1 prints
//-- the event and error lines, 2 adds the telegram-related lines and 3 also
//-- adds the chatty info and warning lines (per-request and per-download
//-- chatter). At level 3 the serial console log is mirrored into the session
//-- too: every info, warning and error line the monitor shows reaches the
//-- telnet client as well. The arbiter carries the level because it already knows who
//-- owns the console slot: an event can only be delivered by its owner's
//-- socket, and components that report events stay independent of the
//-- console server itself.
typedef enum
{
  LOGGER_VERBOSE_OFF = 0,   //-- no debug lines at all
  LOGGER_VERBOSE_EVENTS,    //-- events and errors
  LOGGER_VERBOSE_TELEGRAMS, //-- also the telegram-related lines
  LOGGER_VERBOSE_ALL,       //-- also the chatty info and warning lines
  LOGGER_VERBOSE_LEVEL_COUNT
} loggerVerboseLevel_t;

//-- Applies one reporting level and returns the level actually set (an
//-- out-of-range value clamps to the nearest valid one). The console's
//-- verbose {0,1,2,3} command calls this.
loggerVerboseLevel_t loggerControlVerboseSet(loggerVerboseLevel_t level);

//-- The current reporting level (command handlers and callers that want to
//-- skip building an event's data when nobody would see it).
loggerVerboseLevel_t loggerControlVerboseLevel(void);

//-- The label a command prints for a level ("off", "events and errors",
//-- "events, telegram related and errors", "events, info, warnings and
//-- errors").
const char* loggerControlVerboseName(loggerVerboseLevel_t level);

//-- The reason kind a console event line carries. The bracket tag names it
//-- after the line's wall-clock time ("[hh:mm:ss Event]", "[hh:mm:ss Error]",
//-- ...), so one glance tells why the line was written. The kinds follow the
//-- verbose levels: Event and Error lines come with reporting 1, Telegram
//-- lines with 2 and Info and Warning lines with 3.
typedef enum
{
  LOGGER_CONSOLE_EVENT = 0, //-- a notable background event
  LOGGER_CONSOLE_ERROR,     //-- a failing or refused background operation
  LOGGER_CONSOLE_TELEGRAM,  //-- a telegram-related line (per publish)
  LOGGER_CONSOLE_INFO,      //-- chatty information (per download)
  LOGGER_CONSOLE_WARNING,   //-- a noteworthy oddity
  LOGGER_CONSOLE_KIND_COUNT
} loggerConsoleEventKind;

//-- Sends one tagged line "[hh:mm:ss <Kind>] <text>" to the telnet console
//-- when the current level reaches minimumLevel and a session owns the slot
//-- (the stamp reads "--:--:--" until the wall clock is synchronised). The
//-- line carries no blank padding: it starts on a fresh line and ends with
//-- one, followed by the registered prompt (see loggerControlConsoleSetPrompt)
//-- when one is set, so the console keeps reading prompt-first and no empty
//-- line is left behind. The call never blocks on the socket: the line is
//-- written with a non-blocking send (a stalled client simply loses the
//-- event) and the shared console lock is taken with a short timeout, so a
//-- report from the store, MQTT or httpd task can never pile up behind a
//-- slow console.
void loggerControlConsoleEvent(loggerVerboseLevel_t minimumLevel, loggerConsoleEventKind kind,
                               const char* format, ...);

//-- Registers the prompt text the telnet console has on screen while it
//-- waits for input. Event and mirror lines re-print it after themselves so
//-- the console never leaves a bare line behind. The console server sets it
//-- while the session shows its prompt and clears it (NULL) while a command
//-- question replaces the prompt; the string must outlive the registration.
void loggerControlConsoleSetPrompt(const char* prompt);

//-- --------------------------------------------------------------------------
//-- Own-code logging (the LOGGER_LOGx macros)
//-- --------------------------------------------------------------------------

//-- Logs one line of the logger's own code: writes it to the serial console
//-- exactly as ESP_LOGx does (same level and tag checks, timestamp and
//-- colour) and, from reporting level 3 up with a telnet session holding the
//-- console slot, mirrors the same line into that session too (info, warning
//-- and error only, like the monitor shows; debug lines stay serial-only).
//-- The mirror is best effort and never blocks the logging task. The tag
//-- follows the ESP_LOGx convention. Use through the LOGGER_LOGx macros.
void loggerControlLog(esp_log_level_t level, const char* tag, const char* format, ...)
    __attribute__((format(printf, 3, 4)));

//-- The logger's own log macros: identical serial console output to
//-- ESP_LOGE, ESP_LOGW and ESP_LOGI, plus the telnet mirror at reporting
//-- level 3. The format arguments are evaluated once, by the one call.
#define LOGGER_LOGE(tag, format, ...) loggerControlLog(ESP_LOG_ERROR, tag, format, ##__VA_ARGS__)
#define LOGGER_LOGW(tag, format, ...) loggerControlLog(ESP_LOG_WARN, tag, format, ##__VA_ARGS__)
#define LOGGER_LOGI(tag, format, ...) loggerControlLog(ESP_LOG_INFO, tag, format, ##__VA_ARGS__)

//-- Serialises ordinary console output against the asynchronous event lines:
//-- the console server brackets every write to its session socket with these
//-- so an event never interleaves half a line into a command answer.
void loggerControlConsoleLock(void);
void loggerControlConsoleUnlock(void);

#endif //-- LOGGER_CONTROL_H
