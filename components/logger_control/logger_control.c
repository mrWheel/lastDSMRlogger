//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_control.c - per-channel control client arbiter
//-- Date: 2026-11-24 · Build: 7
//--
//-- One owner slot per control channel behind one mutex, so the web browser
//-- and the telnet console are active at the same time: a new client only
//-- ever displaces the previous owner of its own channel. The evict callback
//-- of the evicted client runs outside the lock: it hands over to other
//-- tasks (the httpd task closes a session, the telnet task shuts a socket
//-- down), and a callback that blocks must never hold the arbiter. The
//-- component also carries the console event sink (background event lines to
//-- the session owning the telnet slot, each line ending on the re-printed
//-- prompt instead of a blank line) and, from reporting level 3 up, the
//-- mirror of the serial console log into the telnet session, best effort
//-- and never blocking the logger. The logger's own code logs through the
//-- LOGGER_LOGx macros, whose single loggerControlLog call writes the line
//-- to the serial console (via esp_log_va, the very function the ESP_LOGx
//-- macros expand to) and mirrors it to the session. A vprintf installed
//-- with esp_log_set_vprintf covers the lines logged by the ESP-IDF and
//-- managed components: it mirrors every info, warning and error line that
//-- does not carry one of the logger's own tags (those already came from
//-- loggerControlLog, so no line is ever mirrored twice).
//-- ==========================================================================

#include "logger_control.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"

static const char* tag = "loggerControl";

//-- One event line is a short sentence: the stack buffer is sized for the
//-- longest one plus the "[hh:mm:ss Event] " frame (and the room the frame
//-- growth adds to the line buffer below), and longer texts are truncated
//-- rather than heap-allocated (an event must never fail on memory).
#define CONSOLE_EVENT_LINE_BYTES 256

//-- Frame room for the "[hh:mm:ss Kind] " stamp around the message.
#define CONSOLE_EVENT_FRAME_BYTES 48

//-- System-time floor for a usable wall clock; before the first SNTP sync
//-- the event stamp reads "--:--:--" instead of a 1970 time.
#define CONSOLE_CLOCK_SYNCED_EPOCH 1700000000

//-- A report from another task waits at most this long for the console
//-- lock: a command answer being written to a stalled client must not turn
//-- into a stalled store, MQTT or httpd task. The event is dropped instead.
#define CONSOLE_EVENT_LOCK_TIMEOUT_MS 100u

//-- Mirror copy of one serial log line (verbose 3): the same text the
//-- monitor shows, rebuilt without colour escapes and with telnet line
//-- endings (every bare LF becomes a CR LF), so the copy needs the room the
//-- extra CRs add plus the re-printed prompt. Shared by the two mirror
//-- paths: loggerControlLog (the logger's own LOGGER_LOGx lines) and the
//-- esp_log_set_vprintf hook (the ESP-IDF and managed component lines).
#define CONSOLE_LOG_TEE_BYTES 512

//-- Longest prompt the console registers ("logger> " fits with room to
//-- spare); longer registrations are truncated.
#define CONSOLE_PROMPT_BYTES 24

static SemaphoreHandle_t controlMutex;

//-- Serialises every write to the telnet session socket (command answers
//-- from the console task and event lines from any other task) so a line is
//-- never interleaved into the middle of another one.
static SemaphoreHandle_t consoleMutex;

//-- The console's verbose reporting level, set by the verbose {0,1,2,3}
//-- command.
static loggerVerboseLevel_t verboseLevel = LOGGER_VERBOSE_OFF;

//-- The prompt the console server has on screen while it waits for input
//-- (registered through loggerControlConsoleSetPrompt); event and mirror
//-- lines re-print it after themselves so the console keeps reading
//-- prompt-first. The mirror only installs its log hook once.
static char consolePrompt[CONSOLE_PROMPT_BYTES];
static bool consolePromptSet = false;
static bool logMirrorInstalled = false;
static vprintf_like_t parentLogVprintf = NULL;

//-- The vprintf hook is defined further down, next to the event sink.
static int consoleLogVprintf(const char* format, va_list arglist);

typedef struct
{
  bool active;
  int fd;
} ControlOwner;

//-- One slot per channel kind: the web browser and the telnet console never
//-- share a slot, each channel arbitrates only its own clients.
static ControlOwner owners[LOGGER_CONTROL_CLIENT_COUNT];
static loggerControlEvictFn evictFns[LOGGER_CONTROL_CLIENT_COUNT];

esp_err_t loggerControlInit(void)
{
  if (controlMutex == NULL)
  {
    controlMutex = xSemaphoreCreateMutex();

    if (controlMutex == NULL)
    {
      LOGGER_LOGE(tag, "could not create the control mutex");
      return ESP_ERR_NO_MEM;
    }
  }

  if (consoleMutex == NULL)
  {
    consoleMutex = xSemaphoreCreateMutex();

    if (consoleMutex == NULL)
    {
      LOGGER_LOGE(tag, "could not create the console mutex");
      return ESP_ERR_NO_MEM;
    }
  }

  xSemaphoreTake(controlMutex, portMAX_DELAY);

  for (uint8_t index = 0u; index < LOGGER_CONTROL_CLIENT_COUNT; index++)
  {
    owners[index].active = false;
    owners[index].fd = -1;
    evictFns[index] = NULL;
  }

  xSemaphoreGive(controlMutex);

  //-- Install the serial log mirror once: from reporting level 3 up every
  //-- log line the monitor shows is copied into the telnet session too (see
  //-- consoleLogVprintf); below that level the hook only passes through. The
  //-- default writer is chained, so the serial console keeps printing.
  if (!logMirrorInstalled)
  {
    logMirrorInstalled = true;
    parentLogVprintf = esp_log_set_vprintf(consoleLogVprintf);
  }

  return ESP_OK;
}

void loggerControlRegisterEvict(loggerControlClient client, loggerControlEvictFn evict)
{
  if (client >= LOGGER_CONTROL_CLIENT_COUNT)
  {
    return;
  }

  evictFns[client] = evict;
}

bool loggerControlClaim(loggerControlClient client, int fd)
{
  if (client >= LOGGER_CONTROL_CLIENT_COUNT)
  {
    return false;
  }

  int evictedFd = -1;
  bool hasEvicted = false;

  if (controlMutex != NULL)
  {
    xSemaphoreTake(controlMutex, portMAX_DELAY);
  }

  ControlOwner* slot = &owners[client];

  if (slot->active && (slot->fd == fd))
  {
    if (controlMutex != NULL)
    {
      xSemaphoreGive(controlMutex);
    }

    return false;
  }

  if (slot->active)
  {
    evictedFd = slot->fd;
    hasEvicted = true;
  }

  slot->active = true;
  slot->fd = fd;

  if (controlMutex != NULL)
  {
    xSemaphoreGive(controlMutex);
  }

  if (hasEvicted && (evictFns[client] != NULL))
  {
    evictFns[client](client, evictedFd);
  }

  LOGGER_LOGI(tag, "%s channel is now with fd %d (was fd %d)",
              (client == LOGGER_CONTROL_TELNET) ? "telnet" : "web", fd,
              hasEvicted ? evictedFd : -1);
  return true;
}

void loggerControlRelease(loggerControlClient client, int fd)
{
  if (client >= LOGGER_CONTROL_CLIENT_COUNT)
  {
    return;
  }

  if (controlMutex != NULL)
  {
    xSemaphoreTake(controlMutex, portMAX_DELAY);
  }

  if (owners[client].active && (owners[client].fd == fd))
  {
    owners[client].active = false;
    owners[client].fd = -1;
  }

  if (controlMutex != NULL)
  {
    xSemaphoreGive(controlMutex);
  }
}

bool loggerControlGetOwner(loggerControlClient client, int* fd)
{
  if ((client >= LOGGER_CONTROL_CLIENT_COUNT) || (fd == NULL))
  {
    return false;
  }

  bool active = false;

  if (controlMutex != NULL)
  {
    xSemaphoreTake(controlMutex, portMAX_DELAY);
  }

  active = owners[client].active;

  if (active)
  {
    *fd = owners[client].fd;
  }

  if (controlMutex != NULL)
  {
    xSemaphoreGive(controlMutex);
  }

  return active;
}

//-- --------------------------------------------------------------------------
//-- Console event reporting (the console's verbose command)
//-- --------------------------------------------------------------------------

loggerVerboseLevel_t loggerControlVerboseSet(loggerVerboseLevel_t level)
{
  if ((int)level < 0)
  {
    level = LOGGER_VERBOSE_OFF;
  }

  if (level >= LOGGER_VERBOSE_LEVEL_COUNT)
  {
    level = (loggerVerboseLevel_t)(LOGGER_VERBOSE_LEVEL_COUNT - 1);
  }

  if (controlMutex != NULL)
  {
    xSemaphoreTake(controlMutex, portMAX_DELAY);
  }

  verboseLevel = level;
  loggerVerboseLevel_t applied = verboseLevel;

  if (controlMutex != NULL)
  {
    xSemaphoreGive(controlMutex);
  }

  return applied;
}

loggerVerboseLevel_t loggerControlVerboseLevel(void)
{
  return verboseLevel;
}

const char* loggerControlVerboseName(loggerVerboseLevel_t level)
{
  switch (level)
  {
  case LOGGER_VERBOSE_EVENTS:
    return "events and errors";
  case LOGGER_VERBOSE_TELEGRAMS:
    return "events, telegram related and errors";
  case LOGGER_VERBOSE_ALL:
    return "events, info, warnings and errors";
  default:
    return "off";
  }
}

//-- The bracket tag a console event line prints for a reason kind: the
//-- capitalised kind word the operator reads after the time.
static const char* consoleEventKindName(loggerConsoleEventKind kind)
{
  switch (kind)
  {
  case LOGGER_CONSOLE_ERROR:
    return "Error";
  case LOGGER_CONSOLE_TELEGRAM:
    return "Telegram";
  case LOGGER_CONSOLE_INFO:
    return "Info";
  case LOGGER_CONSOLE_WARNING:
    return "Warning";
  case LOGGER_CONSOLE_EVENT:
  default:
    return "Event";
  }
}

void loggerControlConsoleSetPrompt(const char* prompt)
{
  if ((prompt == NULL) || (prompt[0] == '\0'))
  {
    consolePromptSet = false;
    return;
  }

  snprintf(consolePrompt, sizeof consolePrompt, "%s", prompt);
  consolePromptSet = true;
}

void loggerControlConsoleLock(void)
{
  if (consoleMutex != NULL)
  {
    xSemaphoreTake(consoleMutex, portMAX_DELAY);
  }
}

void loggerControlConsoleUnlock(void)
{
  if (consoleMutex != NULL)
  {
    xSemaphoreGive(consoleMutex);
  }
}

void loggerControlConsoleEvent(loggerVerboseLevel_t minimumLevel, loggerConsoleEventKind kind,
                               const char* format, ...)
{
  if ((minimumLevel == LOGGER_VERBOSE_OFF) || (format == NULL) || (consoleMutex == NULL) ||
      (controlMutex == NULL))
  {
    return;
  }

  //-- Both the level and the owning socket are read under the arbiter lock;
  //-- a session that closes in the meantime only loses this one line (a
  //-- send to a just-closed or reused fd can at worst hand a stranger a
  //-- stray "[hh:mm:ss Kind]" line, which is exactly what the telnet console
  //-- would have shown the real owner anyway).
  xSemaphoreTake(controlMutex, portMAX_DELAY);
  loggerVerboseLevel_t level = verboseLevel;
  int fd = owners[LOGGER_CONTROL_TELNET].active ? owners[LOGGER_CONTROL_TELNET].fd : -1;
  xSemaphoreGive(controlMutex);

  if ((level < minimumLevel) || (fd < 0))
  {
    return;
  }

  char message[CONSOLE_EVENT_LINE_BYTES];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(message, sizeof message, format, arguments);
  va_end(arguments);

  //-- The line is framed by the wall-clock time and the reason kind, so the
  //-- operator sees at a glance when it happened and why it was written:
  //-- "[hh:mm:ss Event]", "[hh:mm:ss Error]", "[hh:mm:ss Telegram]", ...
  char stamp[12] = "--:--:--";
  time_t now = time(NULL);

  if (now >= (time_t)CONSOLE_CLOCK_SYNCED_EPOCH)
  {
    struct tm brokenTime;

    if (localtime_r(&now, &brokenTime) != NULL)
    {
      strftime(stamp, sizeof stamp, "%H:%M:%S", &brokenTime);
    }
  }

  //-- The line starts on a fresh line, ends with one and re-prints the
  //-- waiting prompt (when the console shows one): consecutive events
  //-- follow each other directly and no blank line is left behind.
  char line[CONSOLE_EVENT_LINE_BYTES + CONSOLE_EVENT_FRAME_BYTES + CONSOLE_PROMPT_BYTES];
  snprintf(line, sizeof line, "\r\n[%s %s] %s\r\n%s", stamp, consoleEventKindName(kind), message,
           consolePromptSet ? consolePrompt : "");

  //-- A command answer being written to a stalled client must not stall the
  //-- reporting task: a busy console loses the event instead.
  if (xSemaphoreTake(consoleMutex, pdMS_TO_TICKS(CONSOLE_EVENT_LOCK_TIMEOUT_MS)) != pdTRUE)
  {
    return;
  }

  const char* cursor = line;
  size_t remaining = strlen(line);

  while (remaining > 0u)
  {
    ssize_t written = send(fd, cursor, remaining, MSG_DONTWAIT);

    //-- A would-block or a dead socket drops the rest of the line: events
    //-- are best-effort and never wait on the console.
    if (written <= 0)
    {
      break;
    }

    cursor += (size_t)written;
    remaining -= (size_t)written;
  }

  xSemaphoreGive(consoleMutex);
}

//-- --------------------------------------------------------------------------
//-- Serial log mirror (verbose 3)
//-- --------------------------------------------------------------------------

//-- One mirror pass at a time: a log line emitted while the hook is busy
//-- (another task mirroring, or this hook's own socket write logging) goes
//-- to the serial console only, so the mirror can never feed itself.
static volatile bool logMirrorBusy = false;

//-- Work buffers in .bss instead of the caller's stack: the hook runs on the
//-- stack of the task that logged, and some tasks are tight (the 4 kB P1
//-- receive task), so mirroring a line must never overflow its caller. The
//-- busy flag gives the buffers a single owner per pass; a concurrent line
//-- simply stays serial-only.
static char teeText[CONSOLE_LOG_TEE_BYTES];
static char teeOut[(CONSOLE_LOG_TEE_BYTES * 2) + CONSOLE_PROMPT_BYTES + 8u];

//-- The tags the logger's own files log with: the vprintf hook leaves these
//-- lines alone because loggerControlLog already mirrors them, so a line of
//-- our own code reaches the session exactly once.
static const char* const ownLogTags[] = {
    "appMain",        "loggerBoard", "loggerControl", "loggerMqtt",   "loggerReceiver",
    "loggerSettings", "meterStore",  "loggerTariffs", "loggerTelnet", "loggerWeb"};

//-- True when the tee buffer holds a log line carrying one of the logger's
//-- own tags: in "I (1234) tag: ..." the tag is the part between ") " and
//-- the ':' (or a space) that follows it.
static bool consoleLogTagIsOwn(const char* text, size_t levelStart, size_t textMax)
{
  size_t cursor = levelStart;

  while ((cursor < textMax) && (text[cursor] != ')'))
  {
    cursor++;
  }

  //-- A line without the closing ')' and ") " separator is not recognised:
  //-- mirror it rather than silently drop it.
  if ((cursor + 2u) >= textMax)
  {
    return false;
  }

  size_t tagStart = cursor + 2u;
  cursor = tagStart;

  while ((cursor < textMax) && (text[cursor] != ':') && (text[cursor] != ' '))
  {
    cursor++;
  }

  size_t tagLength = cursor - tagStart;

  for (size_t index = 0u; index < (sizeof ownLogTags / sizeof ownLogTags[0]); index++)
  {
    if ((strlen(ownLogTags[index]) == tagLength) &&
        (strncmp(text + tagStart, ownLogTags[index], tagLength) == 0))
    {
      return true;
    }
  }

  return false;
}

//-- Hands one finished mirror line to the owning session: under the shared
//-- console lock (with the event sink's short timeout) and with a
//-- non-blocking send, so a stalled or dead console loses the copy while the
//-- logging task never waits.
static void consoleMirrorSend(int fd, const char* text, size_t length)
{
  if (xSemaphoreTake(consoleMutex, pdMS_TO_TICKS(CONSOLE_EVENT_LOCK_TIMEOUT_MS)) != pdTRUE)
  {
    return;
  }

  const char* cursor = text;
  size_t remaining = length;

  while (remaining > 0u)
  {
    ssize_t written = send(fd, cursor, remaining, MSG_DONTWAIT);

    if (written <= 0)
    {
      break;
    }

    cursor += (size_t)written;
    remaining -= (size_t)written;
  }

  xSemaphoreGive(consoleMutex);
}

//-- Installed with esp_log_set_vprintf by loggerControlInit: every log line
//-- first goes to the writer that was in place before (the serial console),
//-- exactly as without this hook. While the console reports at verbose 3 the
//-- info, warning and error lines - everything the serial monitor shows at
//-- the default level - are additionally mirrored to the session owning the
//-- telnet slot: colour escapes stripped, bare line feeds turned into the CR
//-- LF the telnet client needs and the waiting prompt re-printed after the
//-- line. Mirroring never blocks the logging task: a log line from an ISR is
//-- passed through untouched, the socket is written non-blocking and the
//-- shared console lock is taken with the same short timeout the event sink
//-- uses (a busy console simply loses the mirror copy, never the line). The
//-- logger's own code logs through loggerControlLog, which mirrors there;
//-- the hook recognises those tags by the tag word in the line and leaves
//-- them, so no line of our own code is ever mirrored twice.
static int consoleLogVprintf(const char* format, va_list arglist)
{
  int printed = (parentLogVprintf != NULL) ? parentLogVprintf(format, arglist) : 0;

  if ((verboseLevel < LOGGER_VERBOSE_ALL) || (consoleMutex == NULL) || (controlMutex == NULL) ||
      (xPortInIsrContext() != pdFALSE))
  {
    return printed;
  }

  //-- Claim the busy flag (and with it the work buffers) with one atomic
  //-- exchange: a second log line arriving while a pass owns them stays
  //-- serial-only.
  bool claimed = !__atomic_exchange_n(&logMirrorBusy, true, __ATOMIC_ACQ_REL);

  if (!claimed)
  {
    return printed;
  }

  bool mirrorThis = false;
  int fd = -1;
  size_t outLength = 0u;
  va_list arguments;
  va_copy(arguments, arglist);
  int textLength = vsnprintf(teeText, sizeof teeText, format, arguments);
  va_end(arguments);

  size_t textMax = (textLength > 0) ? (size_t)textLength : 0u;

  if (textMax > sizeof teeText - 1u)
  {
    textMax = sizeof teeText - 1u;
  }

  //-- The level letter is the first visible character of the line: colour
  //-- escapes (when the log module is configured to print them) sit in front
  //-- of it. Only the lines the monitor shows are mirrored, so debug and
  //-- verbose lines stay serial-only.
  size_t scan = 0u;

  while ((scan < textMax) && (teeText[scan] == '\033'))
  {
    while ((scan < textMax) && (teeText[scan] != 'm'))
    {
      scan++;
    }

    scan++;
  }

  if ((scan < textMax) &&
      ((teeText[scan] == 'I') || (teeText[scan] == 'W') || (teeText[scan] == 'E')) &&
      !consoleLogTagIsOwn(teeText, scan, textMax))
  {
    xSemaphoreTake(controlMutex, portMAX_DELAY);
    fd = owners[LOGGER_CONTROL_TELNET].active ? owners[LOGGER_CONTROL_TELNET].fd : -1;
    xSemaphoreGive(controlMutex);
    mirrorThis = (fd >= 0);
  }

  bool visible = false;

  if (mirrorThis)
  {
    //-- Rebuild the line for the telnet client: it starts on a fresh line
    //-- (like an event line, so it never mixes into the prompt line), colour
    //-- escapes are stripped, every bare LF becomes a CR LF and the waiting
    //-- prompt is appended so the console keeps reading prompt-first.
    teeOut[outLength++] = '\r';
    teeOut[outLength++] = '\n';

    for (size_t index = 0u; (index < textMax) && ((outLength + 4u) < sizeof teeOut); index++)
    {
      char symbol = teeText[index];

      if (symbol == '\033')
      {
        while ((index < textMax) && (teeText[index] != 'm'))
        {
          index++;
        }

        continue;
      }

      if ((symbol == '\n') && ((index == 0u) || (teeText[index - 1] != '\r')))
      {
        teeOut[outLength++] = '\r';
      }

      if ((symbol != '\r') && (symbol != '\n'))
      {
        visible = true;
      }

      teeOut[outLength++] = symbol;
    }

    if (consolePromptSet)
    {
      size_t promptLength = strlen(consolePrompt);

      if ((outLength + promptLength) < sizeof teeOut)
      {
        memcpy(teeOut + outLength, consolePrompt, promptLength);
        outLength += promptLength;
      }
    }

    teeOut[outLength] = '\0';
  }

  if (visible)
  {
    consoleMirrorSend(fd, teeOut, outLength);
  }

  __atomic_store_n(&logMirrorBusy, false, __ATOMIC_RELEASE);
  return printed;
}

//-- --------------------------------------------------------------------------
//-- Own-code logging (LOGGER_LOGx)
//-- --------------------------------------------------------------------------

//-- The one call behind the LOGGER_LOGE/W/I macros, called with the line's
//-- level and tag: it writes the line to the serial console through
//-- esp_log_va - the very function the ESP_LOGx macros expand to, so level
//-- and tag checks, timestamp and colour are identical - and then, with
//-- reporting on at level 3 and a telnet session holding the console slot,
//-- mirrors the same line into that session. The mirror carries the level
//-- letter, the same uptime stamp and the tag the monitor shows, without
//-- colour, with telnet line endings and the re-printed prompt. It is built
//-- from a copy of the caller's argument list, so the arguments are
//-- evaluated exactly once, and it follows the event sink's rules: best
//-- effort, never from an ISR, never waiting on the console.
void loggerControlLog(esp_log_level_t level, const char* tag, const char* format, ...)
{
  esp_log_config_t config = ESP_LOG_CONFIG_INIT((uint32_t)level | ESP_LOG_CONFIGS_DEFAULT);

  va_list arguments;
  va_start(arguments, format);

  va_list mirrorArguments;
  va_copy(mirrorArguments, arguments);

  esp_log_va(config, tag, format, arguments);
  va_end(arguments);

  char levelLetter = 0;

  if (level == ESP_LOG_ERROR)
  {
    levelLetter = 'E';
  }
  else if (level == ESP_LOG_WARN)
  {
    levelLetter = 'W';
  }
  else if (level == ESP_LOG_INFO)
  {
    levelLetter = 'I';
  }

  bool mirror = (levelLetter != 0) && (verboseLevel >= LOGGER_VERBOSE_ALL) &&
                (consoleMutex != NULL) && (controlMutex != NULL) &&
                (xPortInIsrContext() == pdFALSE);

  //-- Same single-pass claim the hook uses: the tee buffers have one owner
  //-- per pass and a line arriving while they are busy stays serial-only.
  if (mirror)
  {
    mirror = !__atomic_exchange_n(&logMirrorBusy, true, __ATOMIC_ACQ_REL);
  }

  if (!mirror)
  {
    va_end(mirrorArguments);
    return;
  }

  xSemaphoreTake(controlMutex, portMAX_DELAY);
  int fd = owners[LOGGER_CONTROL_TELNET].active ? owners[LOGGER_CONTROL_TELNET].fd : -1;
  xSemaphoreGive(controlMutex);

  if (fd >= 0)
  {
    vsnprintf(teeText, sizeof teeText, format, mirrorArguments);

    //-- The line starts on a fresh line (like an event line, so it never
    //-- mixes into the prompt line), reads "I (12345) tag: text" as the
    //-- monitor shows it, and ends on the re-printed waiting prompt.
    size_t outLength = 0u;

    teeOut[outLength++] = '\r';
    teeOut[outLength++] = '\n';

    int prefixLength =
        snprintf(teeOut + outLength, sizeof teeOut - outLength, "%c (%u) %s: ", levelLetter,
                 (unsigned)esp_log_timestamp(), (tag != NULL) ? tag : "");

    if (prefixLength > 0)
    {
      size_t prefixBytes = (size_t)prefixLength;

      if (prefixBytes > (sizeof teeOut - outLength - 1u))
      {
        prefixBytes = sizeof teeOut - outLength - 1u;
      }

      outLength += prefixBytes;
    }

    bool visible = false;

    for (size_t index = 0u; teeText[index] != '\0'; index++)
    {
      if ((outLength + 4u) >= sizeof teeOut)
      {
        break;
      }

      char symbol = teeText[index];

      if ((symbol == '\n') && ((index == 0u) || (teeText[index - 1] != '\r')))
      {
        teeOut[outLength++] = '\r';
      }

      if ((symbol != '\r') && (symbol != '\n'))
      {
        visible = true;
      }

      teeOut[outLength++] = symbol;
    }

    if (consolePromptSet)
    {
      size_t promptLength = strlen(consolePrompt);

      if ((outLength + promptLength) < sizeof teeOut)
      {
        memcpy(teeOut + outLength, consolePrompt, promptLength);
        outLength += promptLength;
      }
    }

    teeOut[outLength] = '\0';

    if (visible)
    {
      consoleMirrorSend(fd, teeOut, outLength);
    }
  }

  __atomic_store_n(&logMirrorBusy, false, __ATOMIC_RELEASE);
  va_end(mirrorArguments);
}
