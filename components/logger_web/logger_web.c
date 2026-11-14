//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_web.c - SPA and JSON API over esp_http_server
//-- Date: 2026-11-14 · Build: 1
//--
//-- Device to browser traffic is JSON built here with explicit escaping;
//-- browser to device updates use application/x-www-form-urlencoded bodies so
//-- no third-party JSON parser is required on the device. Handlers copy
//-- consistent snapshots from the receiver and never hold its lock while
//-- writing to the network. Raw bytes leave as text/plain so no client ever
//-- interprets telegram text as HTML.
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

static httpd_handle_t server;

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
    else if ((*source == '%') && isdigit((unsigned char)source[1]) && isdigit((unsigned char)source[2]))
    {
      unsigned high = 0u;
      unsigned low = 0u;
      hexDigit(source[1], &high);
      hexDigit(source[2], &low);
      out[position++] = (char)((high << 4) | low);
      source += 3;
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
  sbAppendFmt(sb, "{\"rxGpio\":%d,\"country\":", config->rxGpio);
  sbAppendJsonString(sb, config->countryCode);
  sbAppend(sb, ",\"profileId\":");
  sbAppendJsonString(sb, config->profileId);
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

  loggerSettingsConfig active;
  bool hasActive = loggerReceiverGetActiveConfig(&active);

  sbAppend(&sb, "{\"hasActive\":");
  sbAppend(&sb, hasActive ? "true" : "false");
  sbAppend(&sb, ",\"settings\":");

  if (hasActive)
  {
    appendSettingsJson(&sb, &active);
  }
  else
  {
    sbAppend(&sb, "null");
  }

  sbAppend(&sb, ",\"choices\":{\"gpios\":[");

  for (uint8_t index = 0; index < LOGGER_P1_GPIO_COUNT; index++)
  {
    sbAppendFmt(&sb, "%s%d", (index > 0u) ? "," : "", loggerP1GpioChoices[index]);
  }

  sbAppend(&sb, "],\"countries\":[");
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

    sbAppendFmt(&sb, "%s", firstCountry ? "" : ",");
    firstCountry = false;
    sbAppendJsonString(&sb, info.countryCode);
  }

  sbAppend(&sb, "],\"profiles\":[");

  for (uint16_t index = 0; index < profileCount; index++)
  {
    loggerSettingsProfileInfo info;

    if (!loggerSettingsProfileAt(index, &info))
    {
      continue;
    }

    const dsmrProfile *profile = dsmrProfileLookup(info.profileId);

    sbAppendFmt(&sb, "%s{\"id\":", (index > 0u) ? "," : "");
    sbAppendJsonString(&sb, info.profileId);
    sbAppendFmt(&sb, ",\"country\":");
    sbAppendJsonString(&sb, info.countryCode);
    sbAppendFmt(&sb, ",\"label\":");
    sbAppendJsonString(&sb, info.versionLabel);
    sbAppendFmt(&sb, ",\"intervalMs\":%" PRIu32, info.telegramIntervalMs);
    sbAppend(&sb, ",\"versions\":[");

    if ((profile != NULL) && (profile->acceptedVersions != NULL))
    {
      bool firstVersion = true;

      for (uint8_t versionIndex = 0; profile->acceptedVersions[versionIndex] != NULL; versionIndex++)
      {
        sbAppendFmt(&sb, "%s", firstVersion ? "" : ",");
        firstVersion = false;
        sbAppendJsonString(&sb, profile->acceptedVersions[versionIndex]);
      }
    }

    sbAppend(&sb, "]}");
  }

  sbAppend(&sb, "],\"mbusTypes\":[\"gas\",\"water\",\"heat\",\"cooling\",\"electricity\",\"other\"]}}");

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

  char text[64];
  loggerSettingsConfig candidate;
  memset(&candidate, 0, sizeof candidate);
  candidate.structVersion = LOGGER_SETTINGS_VERSION;

  if (!formGet(body, "rxGpio", text, sizeof text))
  {
    return sendError(req, 400, "missing rxGpio");
  }

  candidate.rxGpio = atoi(text);

  if (!formGet(body, "country", candidate.countryCode, sizeof candidate.countryCode))
  {
    return sendError(req, 400, "missing country");
  }

  if (!formGet(body, "profileId", candidate.profileId, sizeof candidate.profileId))
  {
    return sendError(req, 400, "missing profileId");
  }

  for (uint8_t channel = DSMR_MBUS_CHANNEL_MIN; channel <= DSMR_MBUS_CHANNEL_MAX; channel++)
  {
    char key[8];
    snprintf(key, sizeof key, "mbus%u", (unsigned)channel);

    if (!formGet(body, key, text, sizeof text))
    {
      return sendError(req, 400, "missing M-Bus channel selection");
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
        return sendError(req, 400, "unknown M-Bus device type");
      }
    }
    else
    {
      return sendError(req, 400, "unknown M-Bus channel mode");
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
    return sendError(req, (applied == ESP_ERR_INVALID_ARG) ? 400 : 500,
                     (applied == ESP_ERR_INVALID_ARG) ? "invalid configuration" : "activation failed");
  }

  esp_err_t saved = loggerSettingsSave(&candidate);

  if (saved != ESP_OK)
  {
    if (hadPrevious)
    {
      loggerReceiverApplyConfig(&previous);
    }

    return sendError(req, 500, "persistence failed");
  }

  ESP_LOGI(tag, "configuration applied and stored: gpio %d, profile %s", candidate.rxGpio, candidate.profileId);
  return sendJson(req, "{\"ok\":true}");
}

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

static esp_err_t handleStatusGet(httpd_req_t *req)
{
  loggerReceiverStatus status;
  loggerReceiverGetStatus(&status);

  char buffer[WEB_STATUS_JSON_BYTES];
  stringBuilder sb;
  sbInit(&sb, buffer, sizeof buffer);

  sbAppend(&sb, "{\"state\":");
  sbAppendJsonString(&sb, stateName(status.state));
  sbAppendFmt(&sb, ",\"receiverRunning\":%s", status.receiverRunning ? "true" : "false");
  sbAppendFmt(&sb, ",\"rxGpio\":%d", status.rxGpio);
  sbAppend(&sb, ",\"activeProfileId\":");
  sbAppendJsonString(&sb, status.activeProfileId);
  sbAppendFmt(&sb, ",\"uptimeMs\":%" PRIu32, status.uptimeMs);
  sbAppendFmt(&sb, ",\"staleAfterMs\":%" PRIu32, status.staleAfterMs);
  sbAppendFmt(&sb, ",\"hasLastAccepted\":%s", status.hasLastAccepted ? "true" : "false");
  sbAppendFmt(&sb, ",\"lastAcceptedAgeMs\":%" PRIu32, status.lastAcceptedAgeMs);
  sbAppendFmt(&sb, ",\"acceptedCount\":%" PRIu32, status.acceptedCount);
  sbAppendFmt(&sb, ",\"rejectedCount\":%" PRIu32, status.rejectedCount);
  sbAppendFmt(&sb, ",\"dataLossCount\":%" PRIu32, status.dataLossCount);
  sbAppend(&sb, ",\"lastDiagReason\":");
  sbAppendJsonString(&sb, status.lastDiagReason);
  sbAppend(&sb, ",\"lastDiagObis\":");
  sbAppendJsonString(&sb, status.lastDiagObis);
  sbAppend(&sb, ",\"lastError\":");
  sbAppendJsonString(&sb, status.lastError);

  //-- Wall clock time is unavailable in this release: the SPA shows receive
  //-- age and uptime explicitly.
  sbAppendFmt(&sb, ",\"wallClock\":false}");

  return sendJson(req, buffer);
}

static void appendMetaJson(stringBuilder *sb, const loggerTelegramMeta *meta)
{
  sbAppendFmt(sb, "{\"sequence\":%" PRIu32, meta->sequence);
  sbAppendFmt(sb, ",\"receivedUptimeMs\":%" PRIu32, meta->receivedUptimeMs);
  sbAppend(sb, ",\"profileId\":");
  sbAppendJsonString(sb, meta->profileId);
  sbAppend(sb, ",\"reportedVersion\":");
  sbAppendJsonString(sb, meta->reportedVersion);
  sbAppendFmt(sb, ",\"crcVerified\":%s", meta->crcVerified ? "true" : "false");
  sbAppendFmt(sb, ",\"rawLength\":%u", (unsigned)meta->rawLength);
  sbAppendFmt(sb, ",\"fieldCount\":%u", (unsigned)meta->fieldCount);
  sbAppendFmt(sb, ",\"disabledChannelsMask\":%u", (unsigned)meta->disabledChannelsMask);
  sbAppend(sb, ",\"notes\":");
  sbAppendJsonString(sb, meta->notes);
  sbAppend(sb, "}");
}

static esp_err_t handleTelegramsGet(httpd_req_t *req)
{
  loggerTelegramMeta list[LOGGER_HISTORY_CAPACITY];
  uint16_t count = loggerReceiverListRecords(list, LOGGER_HISTORY_CAPACITY);

  char buffer[WEB_LIST_JSON_BYTES];
  stringBuilder sb;
  sbInit(&sb, buffer, sizeof buffer);

  sbAppend(&sb, "{\"telegrams\":[");

  for (uint16_t index = 0u; index < count; index++)
  {
    sbAppendFmt(&sb, "%s", (index > 0u) ? "," : "");
    appendMetaJson(&sb, &list[index]);
  }

  sbAppend(&sb, "]}");

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

  loggerTelegramRecord *record = malloc(sizeof *record);

  if (record == NULL)
  {
    return sendError(req, 500, "out of memory");
  }

  if (!loggerReceiverGetRecord(sequence, record))
  {
    free(record);

    if (rawRequest)
    {
      httpd_resp_set_status(req, "404 Not Found");
      httpd_resp_set_type(req, "text/plain; charset=utf-8");
      return httpd_resp_send(req, "aged out", HTTPD_RESP_USE_STRLEN);
    }

    return sendError(req, 404, "aged out");
  }

  esp_err_t result;

  if (rawRequest)
  {
    //-- Exact bytes of the accepted frame: original line endings and trailer
    //-- preserved, served as plain text so nothing is interpreted as HTML.
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    result = httpd_resp_send(req, (const char *)record->rawBytes, (int)record->meta.rawLength);
  }
  else
  {
    char *json = malloc(WEB_DETAIL_JSON_BYTES);

    if (json == NULL)
    {
      free(record);
      return sendError(req, 500, "out of memory");
    }

    stringBuilder sb;
    sbInit(&sb, json, WEB_DETAIL_JSON_BYTES);

    sbAppend(&sb, "{\"meta\":");
    appendMetaJson(&sb, &record->meta);
    sbAppend(&sb, ",\"fields\":[");

    for (uint16_t index = 0u; index < record->meta.fieldCount; index++)
    {
      const loggerFieldSnapshot *field = &record->fields[index];

      sbAppendFmt(&sb, "%s{\"obis\":", (index > 0u) ? "," : "");
      sbAppendJsonString(&sb, field->obis);
      sbAppend(&sb, ",\"value\":");
      sbAppendJsonString(&sb, field->value);
      sbAppend(&sb, ",\"unit\":");
      sbAppendJsonString(&sb, field->unit);
      sbAppend(&sb, ",\"timestamp\":");
      sbAppendJsonString(&sb, field->timestamp);
      sbAppendFmt(&sb, ",\"hasTimestamp\":%s", field->hasTimestamp ? "true" : "false");
      sbAppendFmt(&sb, ",\"valueIsText\":%s", field->valueIsText ? "true" : "false");
      sbAppendFmt(&sb, ",\"protocolInvalid\":%s", field->protocolInvalid ? "true" : "false");
      sbAppendFmt(&sb, ",\"mbusChannel\":%u}", (unsigned)field->mbusChannel);
    }

    sbAppend(&sb, "]}");

    if (sb.overflow)
    {
      ESP_LOGE(tag, "detail json overflow for sequence %" PRIu32, sequence);
    }

    result = sendJson(req, json);
    free(json);
  }

  free(record);
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
//-- Server lifecycle
//-- --------------------------------------------------------------------------

static esp_err_t registerUri(httpd_handle_t handle, const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *))
{
  httpd_uri_t uriHandler;
  memset(&uriHandler, 0, sizeof uriHandler);
  uriHandler.uri = uri;
  uriHandler.method = method;
  uriHandler.handler = handler;

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
  config.lru_purge_enable = true;

  esp_err_t result = httpd_start(&server, &config);

  if (result != ESP_OK)
  {
    ESP_LOGE(tag, "starting http server failed: %s", esp_err_to_name(result));
    return result;
  }

  registerUri(server, "/", HTTP_GET, handleRoot);
  registerUri(server, "/app.js", HTTP_GET, handleAppJs);
  registerUri(server, "/style.css", HTTP_GET, handleStyleCss);
  registerUri(server, "/favicon.ico", HTTP_GET, handleFavicon);
  registerUri(server, "/api/config", HTTP_GET, handleConfigGet);
  registerUri(server, "/api/config", HTTP_POST, handleConfigSave);
  registerUri(server, "/api/status", HTTP_GET, handleStatusGet);
  registerUri(server, "/api/telegrams", HTTP_GET, handleTelegramsGet);
  registerUri(server, "/api/telegram/*", HTTP_GET, handleTelegramGet);
  registerUri(server, "/api/view", HTTP_POST, handleViewReport);

  ESP_LOGI(tag, "serving SPA and API on port %d", config.server_port);
  return ESP_OK;
}

bool loggerWebIsRunning(void)
{
  return server != NULL;
}
