//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_web.c - SPA, WebSocket transport and JSON API over esp_http_server
//-- Date: 2026-11-15 · Build: 4
//--
//-- The SPA transfers all dynamic data over one WebSocket at /ws: the device
//-- pushes status and telegram-list JSON and answers form-urlencoded commands
//-- (cmd=config, cmd=save, cmd=telegrams, cmd=telegram, cmd=raw, cmd=view);
//-- raw telegram bytes travel as a binary frame behind their JSON header so
//-- no client ever interprets telegram text as HTML. Browser to device
//-- messages stay form-urlencoded so no third-party JSON parser is required
//-- on the device. The HTTP API remains available for diagnostics and for
//-- clients without WebSocket support. Handlers copy consistent snapshots
//-- from the receiver and never hold its lock while writing to the network;
//-- pushes run in their own task so a slow browser cannot stall reception.
//-- ==========================================================================

#include "logger_web.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "logger_board.h"
#include "logger_receiver.h"
#include "logger_settings.h"

static const char *tag = "loggerWeb";

//-- Embedded SPA assets (EMBED_TXTFILES in CMakeLists.txt).
extern const char indexHtmlStart[] asm("_binary_index_html_start");
extern const char indexHtmlEnd[] asm("_binary_index_html_end");
extern const char appJsStart[] asm("_binary_app_js_start");
extern const char appJsEnd[] asm("_binary_app_js_end");
extern const char styleCssStart[] asm("_binary_style_css_start");
extern const char styleCssEnd[] asm("_binary_style_css_end");

#define WEB_BODY_MAX 512
#define WEB_STATUS_JSON_BYTES 1024
#define WEB_LIST_JSON_BYTES 2048
#define WEB_CONFIG_JSON_BYTES 4096
#define WEB_DETAIL_JSON_BYTES (24u * 1024u)

//-- --------------------------------------------------------------------------
//-- WebSocket transport
//-- --------------------------------------------------------------------------

//-- Browser clients with an open /ws session. Pushes run from a dedicated
//-- task so a stalled client cannot block the receive task or the parser.
#define WS_MAX_CLIENTS 4
#define WS_PUSH_INTERVAL_MS 1000u
#define WS_PUSH_TASK_STACK_BYTES 4096
#define WS_PUSH_TASK_PRIORITY 5

static httpd_handle_t server;
static int wsFds[WS_MAX_CLIENTS];
static SemaphoreHandle_t wsMutex;
static TaskHandle_t wsPushTask;
static uint32_t wsPushedAcceptedCount;

static void wsClientsInit(void)
{
  if (wsMutex == NULL)
  {
    wsMutex = xSemaphoreCreateMutex();

    if (wsMutex == NULL)
    {
      return;
    }
  }

  xSemaphoreTake(wsMutex, portMAX_DELAY);

  for (uint8_t index = 0u; index < WS_MAX_CLIENTS; index++)
  {
    wsFds[index] = -1;
  }

  xSemaphoreGive(wsMutex);
}

static void wsClientAdd(int fd)
{
  if (wsMutex == NULL)
  {
    return;
  }

  xSemaphoreTake(wsMutex, portMAX_DELAY);

  bool known = false;
  bool stored = false;

  for (uint8_t index = 0u; index < WS_MAX_CLIENTS; index++)
  {
    if (wsFds[index] == fd)
    {
      known = true;
      break;
    }
  }

  if (!known)
  {
    for (uint8_t index = 0u; index < WS_MAX_CLIENTS; index++)
    {
      if (wsFds[index] == -1)
      {
        wsFds[index] = fd;
        stored = true;
        break;
      }
    }
  }

  xSemaphoreGive(wsMutex);

  if (!known)
  {
    ESP_LOGI(tag, "websocket client %d %s", fd, stored ? "connected" : "rejected: client limit reached");
  }
}

static void wsClientRemove(int fd)
{
  if (wsMutex == NULL)
  {
    return;
  }

  xSemaphoreTake(wsMutex, portMAX_DELAY);

  for (uint8_t index = 0u; index < WS_MAX_CLIENTS; index++)
  {
    if (wsFds[index] == fd)
    {
      wsFds[index] = -1;
      break;
    }
  }

  xSemaphoreGive(wsMutex);
}

static bool wsClientCount(void)
{
  bool any = false;

  if (wsMutex == NULL)
  {
    return false;
  }

  xSemaphoreTake(wsMutex, portMAX_DELAY);

  for (uint8_t index = 0u; index < WS_MAX_CLIENTS; index++)
  {
    if (wsFds[index] != -1)
    {
      any = true;
      break;
    }
  }

  xSemaphoreGive(wsMutex);
  return any;
}

//-- Sends one text frame to a single client from outside a request. A client
//-- that cannot be reached is dropped from the table.
static void wsSendToClient(int fd, const char *text)
{
  if ((server == NULL) || (fd < 0))
  {
    return;
  }

  if (httpd_ws_get_fd_info(server, fd) != HTTPD_WS_CLIENT_WEBSOCKET)
  {
    wsClientRemove(fd);
    return;
  }

  httpd_ws_frame_t frame;
  memset(&frame, 0, sizeof frame);
  frame.type = HTTPD_WS_TYPE_TEXT;
  frame.payload = (uint8_t *)(uintptr_t)text;
  frame.len = strlen(text);

  if (httpd_ws_send_frame_async(server, fd, &frame) != ESP_OK)
  {
    wsClientRemove(fd);
  }
}

static void wsSendToAll(const char *text)
{
  int targets[WS_MAX_CLIENTS];

  if (wsMutex == NULL)
  {
    return;
  }

  xSemaphoreTake(wsMutex, portMAX_DELAY);
  memcpy(targets, wsFds, sizeof targets);
  xSemaphoreGive(wsMutex);

  for (uint8_t index = 0u; index < WS_MAX_CLIENTS; index++)
  {
    if (targets[index] != -1)
  {
      wsSendToClient(targets[index], text);
    }
  }
}

//-- Sends one text frame back on the request that received the command.
static esp_err_t wsSendReply(httpd_req_t *req, const char *text)
{
  httpd_ws_frame_t frame;
  memset(&frame, 0, sizeof frame);
  frame.type = HTTPD_WS_TYPE_TEXT;
  frame.payload = (uint8_t *)(uintptr_t)text;
  frame.len = strlen(text);
  return httpd_ws_send_frame(req, &frame);
}

//-- --------------------------------------------------------------------------
//-- JSON string builder
//-- --------------------------------------------------------------------------

typedef struct
{
  char *buffer;
  size_t capacity;
  size_t length;
  bool overflow;
} stringBuilder;

static void sbInit(stringBuilder *sb, char *buffer, size_t capacity)
{
  sb->buffer = buffer;
  sb->capacity = capacity;
  sb->length = 0u;
  sb->overflow = false;
  buffer[0] = '\0';
}

static void sbAppend(stringBuilder *sb, const char *text)
{
  if (sb->overflow || (sb->length >= sb->capacity))
  {
    sb->overflow = true;
    return;
  }

  size_t remaining = sb->capacity - sb->length;
  int written = snprintf(sb->buffer + sb->length, remaining, "%s", text);

  if ((written < 0) || ((size_t)written >= remaining))
  {
    sb->overflow = true;
    return;
  }

  sb->length += (size_t)written;
}

static void sbAppendFmt(stringBuilder *sb, const char *format, ...)
{
  if (sb->overflow || (sb->length >= sb->capacity))
  {
    sb->overflow = true;
    return;
  }

  size_t remaining = sb->capacity - sb->length;
  va_list arguments;
  va_start(arguments, format);
  int written = vsnprintf(sb->buffer + sb->length, remaining, format, arguments);
  va_end(arguments);

  if ((written < 0) || ((size_t)written >= remaining))
  {
    sb->overflow = true;
    return;
  }

  sb->length += (size_t)written;
}

//-- Appends a JSON string token with quotes, escaping per RFC 8259.
static void sbAppendJsonString(stringBuilder *sb, const char *value)
{
  sbAppend(sb, "\"");

  for (const char *cursor = value; *cursor != '\0'; cursor++)
  {
    char character = *cursor;

    if ((character == '"') || (character == '\\'))
    {
      sbAppendFmt(sb, "\\%c", character);
    }
    else if ((unsigned char)character < 0x20u)
    {
      sbAppendFmt(sb, "\\u%04X", (unsigned)(unsigned char)character);
    }
    else
    {
      char single[2] = {character, '\0'};
      sbAppend(sb, single);
    }
  }

  sbAppend(sb, "\"");
}

static esp_err_t sendJson(httpd_req_t *req, const char *json)
{
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t sendError(httpd_req_t *req, httpd_err_code_t code, const char *message)
{
  char buffer[160];
  snprintf(buffer, sizeof buffer, "{\"ok\":false,\"error\":\"%s\"}", message);
  httpd_resp_set_status(req, (code >= 500) ? "500 Internal Server Error" : "400 Bad Request");
  return sendJson(req, buffer);
}

static esp_err_t sendAsset(httpd_req_t *req, const char *start, const char *end, const char *contentType)
{
  //-- EMBED_TXTFILES appends a terminating NUL after the file bytes: never
  //-- send it. A trailing NUL makes browsers reject the JavaScript with a
  //-- syntax error, which silently kills the whole SPA (dead buttons).
  while ((end > start) && (end[-1] == '\0'))
  {
    end--;
  }

  //-- The assets are embedded in the firmware and change with every flash:
  //-- always revalidate so a browser reload never runs a stale app.js after
  //-- a firmware update.
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_type(req, contentType);
  return httpd_resp_send(req, start, (end - start));
}

//-- --------------------------------------------------------------------------
//-- Form body parsing (browser to device updates)
//-- --------------------------------------------------------------------------

static int readBody(httpd_req_t *req, char *buffer, size_t bufferSize)
{
  int length = httpd_req_recv(req, buffer, bufferSize - 1u);

  if (length <= 0)
  {
    return -1;
  }

  buffer[length] = '\0';
  return length;
}

static bool hexDigit(char character, unsigned *out)
{
  if ((character >= '0') && (character <= '9'))
  {
    *out = (unsigned)(character - '0');
    return true;
  }

  if ((character >= 'a') && (character <= 'f'))
  {
    *out = (unsigned)(character - 'a') + 10u;
    return true;
  }

  if ((character >= 'A') && (character <= 'F'))
  {
    *out = (unsigned)(character - 'A') + 10u;
    return true;
  }

  return false;
}

//-- Decodes an application/x-www-form-urlencoded token.
static void urlDecode(const char *source, char *out, size_t outSize)
{
  size_t position = 0u;

  while ((*source != '\0') && ((position + 1u) < outSize))
  {
    if (*source == '+')
    {
      out[position++] = ' ';
      source++;
    }
    else if ((*source == '%') && isxdigit((unsigned char)source[1]) && isxdigit((unsigned char)source[2]))
    {
      unsigned high = 0u;
      unsigned low = 0u;

      //-- Hex digits include A-F: the browser encodes the mode separator as
      //-- %3A, so requiring decimal digits broke every expected:<type> value.
      if (hexDigit(source[1], &high) && hexDigit(source[2], &low))
      {
        out[position++] = (char)((high << 4) | low);
        source += 3;
      }
      else
      {
        out[position++] = *source;
        source++;
      }
    }
    else
    {
      out[position++] = *source;
      source++;
    }
  }

  out[position] = '\0';
}

//-- Copies and decodes the value of key from the form body. False when the
//-- key is absent.
static bool formGet(const char *body, const char *key, char *out, size_t outSize)
{
  size_t keyLength = strlen(key);
  const char *cursor = body;

  while (cursor != NULL)
  {
    if ((strncmp(cursor, key, keyLength) == 0) && (cursor[keyLength] == '='))
    {
      const char *valueStart = cursor + keyLength + 1u;
      const char *valueEnd = strchr(valueStart, '&');
      size_t valueLength = (valueEnd != NULL) ? (size_t)(valueEnd - valueStart) : strlen(valueStart);

      char raw[WEB_BODY_MAX];

      if (valueLength >= sizeof raw)
      {
        valueLength = sizeof raw - 1u;
      }

      memcpy(raw, valueStart, valueLength);
      raw[valueLength] = '\0';
      urlDecode(raw, out, outSize);
      return true;
    }

    cursor = strchr(cursor, '&');
    cursor = (cursor != NULL) ? (cursor + 1) : NULL;
  }

  return false;
}

//-- --------------------------------------------------------------------------
//-- Settings JSON
//-- --------------------------------------------------------------------------

static const char *mbusTypeToString(dsmrMbusDeviceType type)
{
  switch (type)
  {
    case DSMR_MBUS_DEVICE_GAS: return "gas";
    case DSMR_MBUS_DEVICE_WATER: return "water";
    case DSMR_MBUS_DEVICE_HEAT: return "heat";
    case DSMR_MBUS_DEVICE_COOLING: return "cooling";
    case DSMR_MBUS_DEVICE_ELECTRICITY: return "electricity";
    case DSMR_MBUS_DEVICE_OTHER: return "other";
    default: return "other";
  }
}

static bool mbusTypeFromString(const char *text, dsmrMbusDeviceType *out)
{
  struct
  {
    const char *name;
    dsmrMbusDeviceType type;
  } types[] =
  {
    {"gas", DSMR_MBUS_DEVICE_GAS},
    {"water", DSMR_MBUS_DEVICE_WATER},
    {"heat", DSMR_MBUS_DEVICE_HEAT},
    {"cooling", DSMR_MBUS_DEVICE_COOLING},
    {"electricity", DSMR_MBUS_DEVICE_ELECTRICITY},
    {"other", DSMR_MBUS_DEVICE_OTHER},
  };

  for (size_t index = 0; index < sizeof types / sizeof types[0]; index++)
  {
    if (strcmp(text, types[index].name) == 0)
    {
      *out = types[index].type;
      return true;
    }
  }

  return false;
}

static void appendSettingsJson(stringBuilder *sb, const loggerSettingsConfig *config)
{
  sbAppendFmt(sb, "{\"rxGpio\":%d,\"intervalSeconds\":%" PRIu32 ",\"country\":", config->rxGpio, config->intervalSeconds);
  sbAppendJsonString(sb, config->countryCode);
  sbAppend(sb, ",\"profileId\":");
  sbAppendJsonString(sb, config->profileId);

  //-- Live P1 RX polarity: false reads the idle-high signal directly, true
  //-- inverts the receive line for interfaces that invert.
  sbAppend(sb, ",\"rxInverted\":");
  sbAppend(sb, config->rxInverted ? "true" : "false");

  sbAppend(sb, ",\"mbus\":[");

  for (uint8_t channel = DSMR_MBUS_CHANNEL_MIN; channel <= DSMR_MBUS_CHANNEL_MAX; channel++)
  {
    if (channel != DSMR_MBUS_CHANNEL_MIN)
    {
      sbAppend(sb, ",");
    }

    if (config->mbusMode[channel] == LOGGER_MBUS_MODE_AUTO)
    {
      sbAppend(sb, "\"auto\"");
    }
    else if (config->mbusMode[channel] == LOGGER_MBUS_MODE_DISABLED)
    {
      sbAppend(sb, "\"disabled\"");
    }
    else
    {
      sbAppendFmt(sb, "\"expected:");
      sbAppend(sb, mbusTypeToString(config->mbusType[channel]));
      sbAppend(sb, "\"");
    }
  }

  sbAppend(sb, "]}");
}

//-- Configuration snapshot for the settings view. The prefill comes from the
//-- configuration currently stored in NVS; the receiver's active
//-- configuration is the fallback when NVS cannot be read back, and both
//-- presence flags are reported so the SPA can word its message correctly.
static void appendConfigJson(stringBuilder *sb)
{
  loggerSettingsConfig stored;
  bool hasStored = (loggerSettingsLoad(&stored) == ESP_OK);

  loggerSettingsConfig active;
  bool hasActive = loggerReceiverGetActiveConfig(&active);

  loggerSettingsConfig *settings;

  if (hasStored)
  {
    settings = &stored;
  }
  else if (hasActive)
  {
    settings = &active;
  }
  else
  {
    settings = NULL;
  }

  sbAppend(sb, "{\"type\":\"config\",\"hasStored\":");
  sbAppend(sb, hasStored ? "true" : "false");
  sbAppend(sb, ",\"hasActive\":");
  sbAppend(sb, hasActive ? "true" : "false");
  sbAppend(sb, ",\"settings\":");

  if (settings != NULL)
  {
    appendSettingsJson(sb, settings);
  }
  else
  {
    sbAppend(sb, "null");
  }

  //-- The data source is a two-position choice: live data on the fixed P1
  //-- UART0 pins or the internal test generator. The interval bounds are
  //-- published so the SPA can configure its interval field.
  sbAppendFmt(sb, ",\"choices\":{\"liveGpio\":%d", LOGGER_RX_GPIO_LIVE);
  sbAppendFmt(sb, ",\"testGpio\":%d", LOGGER_RX_GPIO_TEST);
  sbAppendFmt(sb, ",\"intervalMin\":%u", (unsigned)LOGGER_SETTINGS_INTERVAL_MIN_S);
  sbAppendFmt(sb, ",\"intervalMax\":%u", (unsigned)LOGGER_SETTINGS_INTERVAL_MAX_S);
  sbAppend(sb, ",\"countries\":[");
  uint16_t profileCount = loggerSettingsProfileCount();
  char knownCountries[8][LOGGER_SETTINGS_COUNTRY_MAX];
  uint8_t knownCountryCount = 0u;
  bool firstCountry = true;

  for (uint16_t index = 0; index < profileCount; index++)
  {
    loggerSettingsProfileInfo info;

    if (!loggerSettingsProfileAt(index, &info))
    {
      continue;
    }

    bool known = false;

    for (uint8_t other = 0u; other < knownCountryCount; other++)
    {
      if (strcmp(knownCountries[other], info.countryCode) == 0)
      {
        known = true;
        break;
      }
    }

    if (known)
    {
      continue;
    }

    snprintf(knownCountries[knownCountryCount], LOGGER_SETTINGS_COUNTRY_MAX, "%s", info.countryCode);
    knownCountryCount++;

    sbAppendFmt(sb, "%s", firstCountry ? "" : ",");
    firstCountry = false;
    sbAppendJsonString(sb, info.countryCode);
  }

  sbAppend(sb, "],\"profiles\":[");

  for (uint16_t index = 0; index < profileCount; index++)
  {
    loggerSettingsProfileInfo info;

    if (!loggerSettingsProfileAt(index, &info))
    {
      continue;
    }

    const dsmrProfile *profile = dsmrProfileLookup(info.profileId);

    sbAppendFmt(sb, "%s{\"id\":", (index > 0u) ? "," : "");
    sbAppendJsonString(sb, info.profileId);
    sbAppendFmt(sb, ",\"country\":");
    sbAppendJsonString(sb, info.countryCode);
    sbAppendFmt(sb, ",\"label\":");
    sbAppendJsonString(sb, info.versionLabel);
    sbAppendFmt(sb, ",\"intervalMs\":%" PRIu32, info.telegramIntervalMs);
    sbAppend(sb, ",\"versions\":[");

    if ((profile != NULL) && (profile->acceptedVersions != NULL))
    {
      bool firstVersion = true;

      for (uint8_t versionIndex = 0; profile->acceptedVersions[versionIndex] != NULL; versionIndex++)
      {
        sbAppendFmt(sb, "%s", firstVersion ? "" : ",");
        firstVersion = false;
        sbAppendJsonString(sb, profile->acceptedVersions[versionIndex]);
      }
    }

    sbAppend(sb, "]}");
  }

  sbAppend(sb, "],\"mbusTypes\":[\"gas\",\"water\",\"heat\",\"cooling\",\"electricity\",\"other\"]}}");
}

//-- Applies and persists a complete configuration from a form-urlencoded
//-- body. Used by the HTTP POST and by cmd=save on the WebSocket so both
//-- paths validate and roll back identically. NULL on success, otherwise the
//-- failure reason for the SPA.
static const char *applySettingsFromForm(const char *body)
{
  char text[64];
  loggerSettingsConfig candidate;
  memset(&candidate, 0, sizeof candidate);
  candidate.structVersion = LOGGER_SETTINGS_VERSION;

  if (!formGet(body, "rxGpio", text, sizeof text))
  {
    return "missing rxGpio";
  }

  candidate.rxGpio = atoi(text);

  //-- The read interval in seconds is validated by clamping: a value below
  //-- the minimum becomes the minimum, a value above the maximum becomes the
  //-- maximum, so a save never fails over the interval alone.
  if (!formGet(body, "interval", text, sizeof text))
  {
    return "missing interval";
  }

  int interval = atoi(text);

  if (interval < (int)LOGGER_SETTINGS_INTERVAL_MIN_S)
  {
    interval = (int)LOGGER_SETTINGS_INTERVAL_MIN_S;
  }

  if (interval > (int)LOGGER_SETTINGS_INTERVAL_MAX_S)
  {
    interval = (int)LOGGER_SETTINGS_INTERVAL_MAX_S;
  }

  candidate.intervalSeconds = (uint32_t)interval;

  //-- P1 RX polarity toggle: 1 inverts the receive line (interfaces that
  //-- invert before the ESP32-S3), 0 or an absent field reads the idle-high
  //-- signal directly.
  if (formGet(body, "rxInverted", text, sizeof text))
  {
    candidate.rxInverted = (text[0] == '1');
  }

  if (!formGet(body, "country", candidate.countryCode, sizeof candidate.countryCode))
  {
    return "missing country";
  }

  if (!formGet(body, "profileId", candidate.profileId, sizeof candidate.profileId))
  {
    return "missing profileId";
  }

  for (uint8_t channel = DSMR_MBUS_CHANNEL_MIN; channel <= DSMR_MBUS_CHANNEL_MAX; channel++)
  {
    char key[8];
    snprintf(key, sizeof key, "mbus%u", (unsigned)channel);

    if (!formGet(body, key, text, sizeof text))
    {
      return "missing M-Bus channel selection";
    }

    if (strcmp(text, "auto") == 0)
    {
      candidate.mbusMode[channel] = LOGGER_MBUS_MODE_AUTO;
    }
    else if (strcmp(text, "disabled") == 0)
    {
      candidate.mbusMode[channel] = LOGGER_MBUS_MODE_DISABLED;
    }
    else if (strncmp(text, "expected:", 9) == 0)
    {
      candidate.mbusMode[channel] = LOGGER_MBUS_MODE_EXPECTED;

      if (!mbusTypeFromString(text + 9, &candidate.mbusType[channel]))
      {
        return "unknown M-Bus device type";
      }
    }
    else
    {
      return "unknown M-Bus channel mode";
    }
  }

  //-- Keep the configuration the receiver is running with so a persistence
  //-- failure can be rolled back completely.
  loggerSettingsConfig previous;
  bool hadPrevious = loggerReceiverGetActiveConfig(&previous);

  esp_err_t applied = loggerReceiverApplyConfig(&candidate);

  if (applied != ESP_OK)
  {
    //-- The receiver already restored the previous configuration on failure.
    return (applied == ESP_ERR_INVALID_ARG) ? "invalid configuration" : "activation failed";
  }

  esp_err_t saved = loggerSettingsSave(&candidate);

  if (saved != ESP_OK)
  {
    if (hadPrevious)
    {
      loggerReceiverApplyConfig(&previous);
    }

    return "persistence failed";
  }

  ESP_LOGI(tag, "configuration applied and stored: gpio %d, profile %s, rx polarity %s", candidate.rxGpio, candidate.profileId,
           candidate.rxInverted ? "inverted" : "normal");
  return NULL;
}

//-- --------------------------------------------------------------------------
//-- Status, list, detail and raw JSON builders
//-- --------------------------------------------------------------------------

static const char *stateName(loggerReceiverState state)
{
  switch (state)
  {
    case LOGGER_RECEIVER_AWAITING_CONFIGURATION: return "awaitingConfiguration";
    case LOGGER_RECEIVER_AWAITING_DATA: return "awaitingData";
    case LOGGER_RECEIVER_OK: return "ok";
    case LOGGER_RECEIVER_STALE: return "stale";
    case LOGGER_RECEIVER_ERROR: return "error";
    default: return "unknown";
  }
}

static void appendStatusJson(stringBuilder *sb, const loggerReceiverStatus *status)
{
  sbAppend(sb, "{\"type\":\"status\",\"state\":");
  sbAppendJsonString(sb, stateName(status->state));
  sbAppendFmt(sb, ",\"receiverRunning\":%s", status->receiverRunning ? "true" : "false");
  sbAppendFmt(sb, ",\"rxGpio\":%d", status->rxGpio);
  sbAppend(sb, ",\"testMode\":");
  sbAppend(sb, (loggerSettingsIsTestGpio(status->rxGpio) && status->receiverRunning) ? "true" : "false");
  sbAppendFmt(sb, ",\"intervalSeconds\":%" PRIu32, status->intervalSeconds);
  sbAppend(sb, ",\"activeProfileId\":");
  sbAppendJsonString(sb, status->activeProfileId);
  sbAppendFmt(sb, ",\"uptimeMs\":%" PRIu32, status->uptimeMs);
  sbAppendFmt(sb, ",\"staleAfterMs\":%" PRIu32, status->staleAfterMs);
  sbAppendFmt(sb, ",\"hasLastAccepted\":%s", status->hasLastAccepted ? "true" : "false");
  sbAppendFmt(sb, ",\"lastAcceptedAgeMs\":%" PRIu32, status->lastAcceptedAgeMs);
  sbAppendFmt(sb, ",\"acceptedCount\":%" PRIu32, status->acceptedCount);
  sbAppendFmt(sb, ",\"rejectedCount\":%" PRIu32, status->rejectedCount);
  sbAppendFmt(sb, ",\"dataLossCount\":%" PRIu32, status->dataLossCount);
  sbAppend(sb, ",\"lastDiagReason\":");
  sbAppendJsonString(sb, status->lastDiagReason);
  sbAppend(sb, ",\"lastDiagObis\":");
  sbAppendJsonString(sb, status->lastDiagObis);
  sbAppend(sb, ",\"lastError\":");
  sbAppendJsonString(sb, status->lastError);

  //-- Wall clock time is unavailable in this release: the SPA shows receive
  //-- age and uptime explicitly.
  sbAppendFmt(sb, ",\"wallClock\":false}");
}

static void appendMetaJson(stringBuilder *sb, const loggerTelegramMeta *meta)
{
  sbAppendFmt(sb, "{\"sequence\":%" PRIu32, meta->sequence);
  sbAppendFmt(sb, ",\"receivedUptimeMs\":%" PRIu32, meta->receivedUptimeMs);
  sbAppend(sb, ",\"profileId\":");
  sbAppendJsonString(sb, meta->profileId);
  sbAppend(sb, ",\"reportedVersion\":");
  sbAppendJsonString(sb, meta->reportedVersion);

  //-- Meter time of OBIS 0-0:1.0.0 as "HH:MM:SS"; empty when unavailable.
  sbAppend(sb, ",\"meterTimeText\":");
  sbAppendJsonString(sb, meta->meterTimeText);
  sbAppendFmt(sb, ",\"crcVerified\":%s", meta->crcVerified ? "true" : "false");
  sbAppendFmt(sb, ",\"rawLength\":%u", (unsigned)meta->rawLength);
  sbAppendFmt(sb, ",\"fieldCount\":%u", (unsigned)meta->fieldCount);
  sbAppendFmt(sb, ",\"disabledChannelsMask\":%u", (unsigned)meta->disabledChannelsMask);
  sbAppend(sb, ",\"notes\":");
  sbAppendJsonString(sb, meta->notes);
  sbAppend(sb, "}");
}

static void appendTelegramListJson(stringBuilder *sb)
{
  loggerTelegramMeta list[LOGGER_HISTORY_CAPACITY];
  uint16_t count = loggerReceiverListRecords(list, LOGGER_HISTORY_CAPACITY);

  sbAppend(sb, "{\"type\":\"telegrams\",\"telegrams\":[");

  for (uint16_t index = 0u; index < count; index++)
  {
    sbAppendFmt(sb, "%s", (index > 0u) ? "," : "");
    appendMetaJson(sb, &list[index]);
  }

  sbAppend(sb, "]}");
}

//-- Callback of appendTelegramDetailJson: renders one retained record in
//-- place while the receive-side lock is held. Memory only: no network and
//-- no blocking, as required by the inspect contract.
static bool buildDetailFromRecord(const loggerTelegramRecord *record, void *userData)
{
  stringBuilder *sb = (stringBuilder *)userData;

  sbAppend(sb, "{\"type\":\"telegram\",\"meta\":");
  appendMetaJson(sb, &record->meta);
  sbAppend(sb, ",\"fields\":[");

  for (uint16_t index = 0u; index < record->meta.fieldCount; index++)
  {
    const loggerFieldSnapshot *field = &record->fields[index];

    sbAppendFmt(sb, "%s{\"obis\":", (index > 0u) ? "," : "");
    sbAppendJsonString(sb, field->obis);
    sbAppend(sb, ",\"value\":");
    sbAppendJsonString(sb, field->value);
    sbAppend(sb, ",\"unit\":");
    sbAppendJsonString(sb, field->unit);
    sbAppend(sb, ",\"timestamp\":");
    sbAppendJsonString(sb, field->timestamp);
    sbAppendFmt(sb, ",\"hasTimestamp\":%s", field->hasTimestamp ? "true" : "false");
    sbAppendFmt(sb, ",\"valueIsText\":%s", field->valueIsText ? "true" : "false");
    sbAppendFmt(sb, ",\"protocolInvalid\":%s", field->protocolInvalid ? "true" : "false");
    sbAppendFmt(sb, ",\"mbusChannel\":%u}", (unsigned)field->mbusChannel);
  }

  sbAppend(sb, "]}");
  return true;
}

//-- Builds the decoded detail JSON of one retained record. False when the
//-- sequence aged out; the caller reports that to the client. The JSON is
//-- rendered straight from the retained slot: copying the 23 KB record to
//-- the heap next to the JSON buffer exhausted the free heap on this build
//-- and made every detail request fail over as "aged out".
static bool appendTelegramDetailJson(stringBuilder *sb, uint32_t sequence)
{
  return loggerReceiverInspectRecord(sequence, buildDetailFromRecord, sb);
}

//-- --------------------------------------------------------------------------
//-- SPA asset handlers
//-- --------------------------------------------------------------------------

static esp_err_t handleRoot(httpd_req_t *req)
{
  return sendAsset(req, indexHtmlStart, indexHtmlEnd, "text/html; charset=utf-8");
}

static esp_err_t handleAppJs(httpd_req_t *req)
{
  return sendAsset(req, appJsStart, appJsEnd, "application/javascript; charset=utf-8");
}

static esp_err_t handleStyleCss(httpd_req_t *req)
{
  return sendAsset(req, styleCssStart, styleCssEnd, "text/css; charset=utf-8");
}

static esp_err_t handleFavicon(httpd_req_t *req)
{
  httpd_resp_set_status(req, "204 No Content");
  return httpd_resp_send(req, NULL, 0);
}

//-- --------------------------------------------------------------------------
//-- API handlers
//-- --------------------------------------------------------------------------

static esp_err_t handleConfigGet(httpd_req_t *req)
{
  char *buffer = malloc(WEB_CONFIG_JSON_BYTES);

  if (buffer == NULL)
  {
    return sendError(req, 500, "out of memory");
  }

  stringBuilder sb;
  sbInit(&sb, buffer, WEB_CONFIG_JSON_BYTES);
  appendConfigJson(&sb);

  if (sb.overflow)
  {
    ESP_LOGE(tag, "config json overflow");
  }

  esp_err_t result = sendJson(req, buffer);
  free(buffer);
  return result;
}

static esp_err_t handleConfigSave(httpd_req_t *req)
{
  char body[WEB_BODY_MAX];

  if (readBody(req, body, sizeof body) < 0)
  {
    return sendError(req, 400, "cannot read body");
  }

  const char *failure = applySettingsFromForm(body);

  if (failure != NULL)
  {
    return sendError(req, 400, failure);
  }

  return sendJson(req, "{\"ok\":true}");
}

static esp_err_t handleStatusGet(httpd_req_t *req)
{
  loggerReceiverStatus status;
  loggerReceiverGetStatus(&status);

  char buffer[WEB_STATUS_JSON_BYTES];
  stringBuilder sb;
  sbInit(&sb, buffer, sizeof buffer);
  appendStatusJson(&sb, &status);

  return sendJson(req, buffer);
}

static esp_err_t handleTelegramsGet(httpd_req_t *req)
{
  char buffer[WEB_LIST_JSON_BYTES];
  stringBuilder sb;
  sbInit(&sb, buffer, sizeof buffer);
  appendTelegramListJson(&sb);

  if (sb.overflow)
  {
    ESP_LOGE(tag, "telegram list json overflow");
  }

  return sendJson(req, buffer);
}

static esp_err_t handleTelegramGet(httpd_req_t *req)
{
  //-- The wildcard URI is either /api/telegram/<seq> or /api/telegram/<seq>/raw.
  const char *argument = req->uri + strlen("/api/telegram/");
  bool rawRequest = (strstr(argument, "/raw") != NULL);
  char sequenceText[16];
  snprintf(sequenceText, sizeof sequenceText, "%s", argument);

  for (char *cursor = sequenceText; *cursor != '\0'; cursor++)
  {
    if (*cursor == '/')
    {
      *cursor = '\0';
      break;
    }
  }

  uint32_t sequence = (uint32_t)strtoul(sequenceText, NULL, 10);

  if (rawRequest)
  {
    loggerTelegramRecord *record = malloc(sizeof *record);

    if (record == NULL)
    {
      return sendError(req, 500, "out of memory");
    }

    if (!loggerReceiverGetRecord(sequence, record))
    {
      free(record);

      httpd_resp_set_status(req, "404 Not Found");
      httpd_resp_set_type(req, "text/plain; charset=utf-8");
      return httpd_resp_send(req, "aged out", HTTPD_RESP_USE_STRLEN);
    }

    //-- Exact bytes of the accepted frame: original line endings and trailer
    //-- preserved, served as plain text so nothing is interpreted as HTML.
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    esp_err_t result = httpd_resp_send(req, (const char *)record->rawBytes, (int)record->meta.rawLength);
    free(record);
    return result;
  }

  char *json = malloc(WEB_DETAIL_JSON_BYTES);

  if (json == NULL)
  {
    return sendError(req, 500, "out of memory");
  }

  stringBuilder sb;
  sbInit(&sb, json, WEB_DETAIL_JSON_BYTES);

  if (!appendTelegramDetailJson(&sb, sequence))
  {
    free(json);
    return sendError(req, 404, "aged out");
  }

  if (sb.overflow)
  {
    ESP_LOGE(tag, "detail json overflow for sequence %" PRIu32, sequence);
  }

  esp_err_t result = sendJson(req, json);
  free(json);
  return result;
}

static esp_err_t handleViewReport(httpd_req_t *req)
{
  char body[WEB_BODY_MAX];

  if (readBody(req, body, sizeof body) < 0)
  {
    return sendError(req, 400, "cannot read body");
  }

  char screen[32];

  if (!formGet(body, "screen", screen, sizeof screen))
  {
    return sendError(req, 400, "missing screen");
  }

  loggerBoardShowScreen(screen);
  return sendJson(req, "{\"ok\":true}");
}

//-- --------------------------------------------------------------------------
//-- WebSocket handlers
//-- --------------------------------------------------------------------------

//-- Answers one form-urlencoded command frame received on /ws. Responses are
//-- JSON text frames, except cmd=raw which sends a JSON header frame followed
//-- by a binary frame carrying the exact accepted bytes.
static void wsHandleCommand(httpd_req_t *req, const char *body)
{
  char command[16];

  if (!formGet(body, "cmd", command, sizeof command))
  {
    wsSendReply(req, "{\"type\":\"error\",\"request\":\"\",\"error\":\"missing cmd\"}");
    return;
  }

  if (strcmp(command, "config") == 0)
  {
    char *buffer = malloc(WEB_CONFIG_JSON_BYTES);

    if (buffer == NULL)
    {
      wsSendReply(req, "{\"type\":\"error\",\"request\":\"config\",\"error\":\"out of memory\"}");
      return;
    }

    stringBuilder sb;
    sbInit(&sb, buffer, WEB_CONFIG_JSON_BYTES);
    appendConfigJson(&sb);
    wsSendReply(req, buffer);
    free(buffer);
    return;
  }

  if (strcmp(command, "status") == 0)
  {
    loggerReceiverStatus status;
    loggerReceiverGetStatus(&status);

    char buffer[WEB_STATUS_JSON_BYTES];
    stringBuilder sb;
    sbInit(&sb, buffer, sizeof buffer);
    appendStatusJson(&sb, &status);
    wsSendReply(req, buffer);
    return;
  }

  if (strcmp(command, "telegrams") == 0)
  {
    char *buffer = malloc(WEB_LIST_JSON_BYTES);

    if (buffer == NULL)
    {
      wsSendReply(req, "{\"type\":\"error\",\"request\":\"telegrams\",\"error\":\"out of memory\"}");
      return;
    }

    stringBuilder sb;
    sbInit(&sb, buffer, WEB_LIST_JSON_BYTES);
    appendTelegramListJson(&sb);
    wsSendReply(req, buffer);
    free(buffer);
    return;
  }

  if ((strcmp(command, "telegram") == 0) || (strcmp(command, "raw") == 0))
  {
    bool rawRequest = (strcmp(command, "raw") == 0);
    char sequenceText[16];

    if (!formGet(body, "seq", sequenceText, sizeof sequenceText))
    {
      wsSendReply(req, "{\"type\":\"error\",\"request\":\"telegram\",\"error\":\"missing seq\"}");
      return;
    }

    uint32_t sequence = (uint32_t)strtoul(sequenceText, NULL, 10);

    if (rawRequest)
    {
      loggerTelegramRecord *record = malloc(sizeof *record);

      if (record == NULL)
      {
        wsSendReply(req, "{\"type\":\"error\",\"request\":\"raw\",\"error\":\"out of memory\"}");
        return;
      }

      if (!loggerReceiverGetRecord(sequence, record))
      {
        free(record);
        wsSendReply(req, "{\"type\":\"error\",\"request\":\"raw\",\"seq\":0,\"error\":\"aged out\"}");
        return;
      }

      char header[64];
      snprintf(header, sizeof header, "{\"type\":\"raw\",\"sequence\":%" PRIu32 ",\"length\":%u}",
               sequence, (unsigned)record->meta.rawLength);
      wsSendReply(req, header);

      //-- Exact bytes of the accepted frame in a binary frame: original line
      //-- endings and trailer preserved, never interpreted as HTML.
      httpd_ws_frame_t frame;
      memset(&frame, 0, sizeof frame);
      frame.type = HTTPD_WS_TYPE_BINARY;
      frame.payload = record->rawBytes;
      frame.len = record->meta.rawLength;
      esp_err_t sent = httpd_ws_send_frame(req, &frame);
      (void)sent;
      free(record);
      return;
    }

    char *json = malloc(WEB_DETAIL_JSON_BYTES);

    if (json == NULL)
    {
      wsSendReply(req, "{\"type\":\"error\",\"request\":\"telegram\",\"error\":\"out of memory\"}");
      return;
    }

    stringBuilder sb;
    sbInit(&sb, json, WEB_DETAIL_JSON_BYTES);

    if (!appendTelegramDetailJson(&sb, sequence))
    {
      free(json);
      wsSendReply(req, "{\"type\":\"error\",\"request\":\"telegram\",\"error\":\"aged out\"}");
      return;
    }

    wsSendReply(req, json);
    free(json);
    return;
  }

  if (strcmp(command, "save") == 0)
  {
    const char *failure = applySettingsFromForm(body);

    if (failure != NULL)
    {
      char reply[160];
      snprintf(reply, sizeof reply, "{\"type\":\"save\",\"ok\":false,\"error\":\"%s\"}", failure);
      wsSendReply(req, reply);
      return;
    }

    wsSendReply(req, "{\"type\":\"save\",\"ok\":true}");

    //-- A successful save shows the active configuration: send the fresh
    //-- config snapshot right away.
    char *buffer = malloc(WEB_CONFIG_JSON_BYTES);

    if (buffer != NULL)
    {
      stringBuilder sb;
      sbInit(&sb, buffer, WEB_CONFIG_JSON_BYTES);
      appendConfigJson(&sb);
      wsSendReply(req, buffer);
      free(buffer);
    }
    return;
  }

  if (strcmp(command, "view") == 0)
  {
    char screen[32];

    if (!formGet(body, "screen", screen, sizeof screen))
    {
      wsSendReply(req, "{\"type\":\"error\",\"request\":\"view\",\"error\":\"missing screen\"}");
      return;
    }

    loggerBoardShowScreen(screen);
    wsSendReply(req, "{\"type\":\"view\",\"ok\":true}");
    return;
  }

  char reply[96];
  snprintf(reply, sizeof reply, "{\"type\":\"error\",\"request\":\"%s\",\"error\":\"unknown command\"}", command);
  wsSendReply(req, reply);
}

static esp_err_t handleWebSocket(httpd_req_t *req)
{
  //-- ESP-IDF 6.0 answers the handshake inside httpd (httpd_uri.c returns
  //-- without calling the handler), so this handler is only ever entered for
  //-- data frames: that is where a client must be registered for pushes.
  //-- wsClientAdd ignores sockets it already knows, so calling it on every
  //-- frame is cheap and cannot miss a client.
  wsClientAdd(httpd_req_to_sockfd(req));

  if (req->method == HTTP_GET)
  {
    return ESP_OK;
  }

  httpd_ws_frame_t frame;
  memset(&frame, 0, sizeof frame);
  frame.type = HTTPD_WS_TYPE_TEXT;

  esp_err_t result = httpd_ws_recv_frame(req, &frame, 0u);

  if (result != ESP_OK)
  {
    ESP_LOGW(tag, "websocket frame header failed: %s", esp_err_to_name(result));
    wsClientRemove(httpd_req_to_sockfd(req));
    return result;
  }

  if (frame.type == HTTPD_WS_TYPE_CLOSE)
  {
    wsClientRemove(httpd_req_to_sockfd(req));
    return ESP_OK;
  }

  if (frame.type == HTTPD_WS_TYPE_PING)
  {
    esp_err_t pong = ESP_OK;

    //-- Drain the ping payload before answering, otherwise the unread bytes
    //-- would desynchronise the next frame read.
    if (frame.len > 0u)
    {
      char *payload = malloc(frame.len + 1u);

      if (payload != NULL)
      {
        frame.payload = (uint8_t *)payload;
        pong = httpd_ws_recv_frame(req, &frame, frame.len);
        free(payload);
      }
    }

    if (pong != ESP_OK)
    {
      return pong;
    }

    frame.type = HTTPD_WS_TYPE_PONG;
    frame.len = 0u;
    frame.payload = NULL;
    return httpd_ws_send_frame(req, &frame);
  }

  if ((frame.type != HTTPD_WS_TYPE_TEXT) || (!frame.final) || (frame.len == 0u))
  {
    //-- Consume the payload of anything ignored so the next frame header
    //-- stays aligned in the socket stream.
    if (frame.len > 0u)
    {
      char *payload = malloc(frame.len + 1u);

      if (payload != NULL)
      {
        frame.payload = (uint8_t *)payload;
        esp_err_t drained = httpd_ws_recv_frame(req, &frame, frame.len);
        free(payload);
        return drained;
      }
    }

    return ESP_OK;
  }

  if (frame.len >= WEB_BODY_MAX)
  {
    wsSendReply(req, "{\"type\":\"error\",\"request\":\"\",\"error\":\"command too long\"}");
    return ESP_OK;
  }

  char *body = calloc(1u, frame.len + 1u);

  if (body == NULL)
  {
    wsSendReply(req, "{\"type\":\"error\",\"request\":\"\",\"error\":\"out of memory\"}");
    return ESP_ERR_NO_MEM;
  }

  frame.payload = (uint8_t *)body;
  result = httpd_ws_recv_frame(req, &frame, frame.len);

  if (result == ESP_OK)
  {
    wsHandleCommand(req, body);
  }

  free(body);
  return result;
}

//-- Pushes status every second and the telegram list when a new telegram was
//-- accepted. Runs in its own task: a slow or stalled client can only stall
//-- this task, never the receive task or the parser.
static void wsPushTaskLoop(void *argument)
{
  (void)argument;

  for (;;)
  {
    vTaskDelay(pdMS_TO_TICKS(WS_PUSH_INTERVAL_MS));

    if ((server == NULL) || (!wsClientCount()))
    {
      continue;
    }

    loggerReceiverStatus status;
    loggerReceiverGetStatus(&status);

    char buffer[WEB_STATUS_JSON_BYTES];
    stringBuilder sb;
    sbInit(&sb, buffer, sizeof buffer);
    appendStatusJson(&sb, &status);

    if (!sb.overflow)
    {
      wsSendToAll(buffer);
    }

    if (status.acceptedCount != wsPushedAcceptedCount)
    {
      char *list = malloc(WEB_LIST_JSON_BYTES);

      if (list != NULL)
      {
        stringBuilder listSb;
        sbInit(&listSb, list, WEB_LIST_JSON_BYTES);
        appendTelegramListJson(&listSb);

        if (!listSb.overflow)
        {
          wsSendToAll(list);
        }

        free(list);
      }

      wsPushedAcceptedCount = status.acceptedCount;
    }
  }
}

//-- --------------------------------------------------------------------------
//-- Server lifecycle
//-- --------------------------------------------------------------------------

static esp_err_t registerUri(httpd_handle_t handle, const char *uri, httpd_method_t method, bool isWebSocket, esp_err_t (*handler)(httpd_req_t *))
{
  httpd_uri_t uriHandler;
  memset(&uriHandler, 0, sizeof uriHandler);
  uriHandler.uri = uri;
  uriHandler.method = method;
  uriHandler.handler = handler;
  uriHandler.is_websocket = isWebSocket;

  esp_err_t result = httpd_register_uri_handler(handle, &uriHandler);

  if (result != ESP_OK)
  {
    ESP_LOGE(tag, "registering %s failed: %s", uri, esp_err_to_name(result));
  }

  return result;
}

esp_err_t loggerWebStart(void)
{
  if (server != NULL)
  {
    return ESP_OK;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 12;

  //-- WebSocket command handling builds the telegram detail JSON (nested
  //-- string builders plus vsnprintf) on the httpd task stack: the 4 KB
  //-- default overflows there, which silently killed the whole web server
  //-- (dropped telegrams and details, blank panes). 8 KB measured headroom.
  config.stack_size = 8192;

  //-- WebSocket sessions stay open for the life of the page; an LRU purge
  //-- could evict them to make room for ordinary requests.
  config.lru_purge_enable = false;

  esp_err_t result = httpd_start(&server, &config);

  if (result != ESP_OK)
  {
    ESP_LOGE(tag, "starting http server failed: %s", esp_err_to_name(result));
    return result;
  }

  wsClientsInit();
  wsPushedAcceptedCount = 0u;

  registerUri(server, "/", HTTP_GET, false, handleRoot);
  registerUri(server, "/app.js", HTTP_GET, false, handleAppJs);
  registerUri(server, "/style.css", HTTP_GET, false, handleStyleCss);
  registerUri(server, "/favicon.ico", HTTP_GET, false, handleFavicon);
  registerUri(server, "/api/config", HTTP_GET, false, handleConfigGet);
  registerUri(server, "/api/config", HTTP_POST, false, handleConfigSave);
  registerUri(server, "/api/status", HTTP_GET, false, handleStatusGet);
  registerUri(server, "/api/telegrams", HTTP_GET, false, handleTelegramsGet);
  registerUri(server, "/api/telegram/*", HTTP_GET, false, handleTelegramGet);
  registerUri(server, "/api/view", HTTP_POST, false, handleViewReport);
  registerUri(server, "/ws", HTTP_GET, true, handleWebSocket);

  if (wsPushTask == NULL)
  {
    BaseType_t created = xTaskCreate(wsPushTaskLoop, "wsPush", WS_PUSH_TASK_STACK_BYTES, NULL, WS_PUSH_TASK_PRIORITY, &wsPushTask);

    if (created != pdPASS)
    {
      ESP_LOGE(tag, "could not create the websocket push task");
      wsPushTask = NULL;
    }
  }

  ESP_LOGI(tag, "serving SPA, API and WebSocket on port %d", config.server_port);
  return ESP_OK;
}

bool loggerWebIsRunning(void)
{
  return server != NULL;
}
