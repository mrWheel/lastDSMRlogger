//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_web.c - SPA, WebSocket transport and JSON API over esp_http_server
//-- Date: 2026-11-22 · Build: 21
//--
//-- The SPA transfers all dynamic data over one WebSocket at /ws: the device
//-- pushes status, telegram-list and realtime-power JSON and answers
//-- form-urlencoded commands (cmd=config, cmd=save, cmd=telegrams,
//-- cmd=telegram, cmd=raw, cmd=power, cmd=ringfile, cmd=graph,
//-- cmd=actualgraph, cmd=hoursgraph (alias of graph&ring=hours),
//-- cmd=view);
//-- raw telegram bytes travel as a binary frame behind their JSON header so
//-- no client ever interprets telegram text as HTML. Browser to device
//-- messages stay form-urlencoded so no third-party JSON parser is required
//-- on the device. The HTTP API serves only the versioned surface: every
//-- datum the telnet console and the SPA can request is a plain GET under
//-- /api/v3/ (a short descriptive name per endpoint, the index at /api/v3
//-- lists them) and the two SPA actions (save, view) are POSTs there; the
//-- frames answer exactly what the console prints, in JSON. There is no
//-- unversioned /api/ layer so a future v4 can define its own syntax. No
//-- frame ever carries the MQTT password: the settings frames omit it and a
//-- save without that field keeps the stored password. Handlers copy consistent snapshots
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
#include <time.h>
#include <unistd.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "logger_board.h"
#include "logger_control.h"
#include "logger_receiver.h"
#include "logger_settings.h"
#include "logger_store.h"
#include "logger_telnet.h"
#include "wifi_provisioner.h"

static const char* tag = "loggerWeb";

//-- The application's notification for a saved configuration (network
//-- identity and MQTT live in the application, not in this component);
//-- installed through loggerWebSetSettingsSavedCallback.
static loggerWebSettingsSavedCb settingsSavedCallback = NULL;

//-- The program version string is defined by the application entry
//-- (main/app_main.c) and shown in the SPA header through the status JSON.
extern const char* PROG_VERSION;

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
//-- The power JSON also carries the bar history of the newest 90 accepted
//-- samples (watts plus their "HH:MM:SS" time text), so it is sized for the
//-- frame plus that array with headroom.
#define WEB_POWER_JSON_BYTES 6144
#define WEB_DETAIL_JSON_BYTES (24u * 1024u)
//-- The ring file JSON lists every retained record of one ring (the hours
//-- ring is the largest at forty-nine: key, five counters and three tariff
//-- amounts each), so it is sized with headroom above the fullest table.
#define WEB_RINGFILE_JSON_BYTES 8192
//-- The hours graph JSON lists one hourly usage line per retained hour
//-- (forty-nine at most: key plus three deltas), well within the same
//-- headroom.
#define WEB_HOURSGRAPH_JSON_BYTES 8192

//-- The Actual Graph frame carries up to the full receiver power history
//-- (a hundred samples, twice that when the heap carried the wider table at
//-- boot) with six phase figures and the gas rate each. One record costs up
//-- to about 105 bytes, so two hundred of them plus the frame tail need
//-- well past the original 12 kB: a truncated frame used to reach the SPA
//-- as malformed JSON and left the view blank.
#define WEB_ACTUALGRAPH_JSON_BYTES 24576

//-- The v3 status frame carries the reception status plus the Wi-Fi and
//-- control owner fields the console's status command reports next to it.
#define WEB_STATUS_V3_JSON_BYTES 1536
//-- The v3 device frame renders the console's state block and the v3 store
//-- frame lists every file on the LittleFS volume with the capacity block,
//-- so both are sized with headroom above the fullest answer.
#define WEB_DEVICE_JSON_BYTES 2048
#define WEB_STORE_JSON_BYTES 8192

//-- --------------------------------------------------------------------------
//-- WebSocket transport
//-- --------------------------------------------------------------------------

//-- Browser clients with an open /ws session. Exactly one browser client
//-- holds the web control slot via the logger_control arbiter (the telnet
//-- console has its own slot and never displaces a page), so in practice
//-- this table only ever carries the owning fd. Pushes run from a dedicated
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
static uint32_t wsPushedPowerSequence;

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
    ESP_LOGI(tag, "websocket client %d %s", fd,
             stored ? "connected" : "rejected: client limit reached");
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
static void wsSendToClient(int fd, const char* text)
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
  frame.payload = (uint8_t*)(uintptr_t)text;
  frame.len = strlen(text);

  if (httpd_ws_send_frame_async(server, fd, &frame) != ESP_OK)
  {
    wsClientRemove(fd);
  }
}

//-- Copies the fd of the /ws client that currently holds the web control
//-- slot. False when no browser holds it (no page has reported itself yet
//-- or every page closed): then no browser may receive pushes at all. The
//-- telnet console has its own slot and never blocks web pushes.
static bool webControlOwnerFd(int* ownerFd)
{
  if (!loggerControlGetOwner(LOGGER_CONTROL_WEB, ownerFd))
  {
    *ownerFd = -1;
    return false;
  }

  return *ownerFd >= 0;
}

static void wsSendToAll(const char* text)
{
  //-- Single-browser rule: only the client holding the web control slot is
  //-- pushed to; every other tab was evicted by the arbiter when this one
  //-- claimed the slot, so the client table already only ever carries that
  //-- one fd.
  int ownerFd = -1;

  if (!webControlOwnerFd(&ownerFd))
  {
    return;
  }

  wsSendToClient(ownerFd, text);
}

//-- The arbiter calls this when a newer browser tab claims the web control
//-- slot from the current web client (the telnet console owns its own slot
//-- and never evicts a page). The evicted page first gets
//-- the takeover text frame, then its socket is force-closed — the async
//-- send is queued before the close, so the browser still shows its popup
//-- with the take-control-back button. Modelled on the single-/ws-client
//-- rule of the reference radio project.
static void webEvictCallback(loggerControlClient evicted, int evictedFd)
{
  (void)evicted;
  wsSendToClient(evictedFd, "{\"type\":\"takenover\",\"message\":\"Control of the logger was "
                            "taken over by another browser page.\"}");

  //-- Give the TCP stack a moment to push the popup frame into the wire
  //-- before the close queues its FIN. A page that loses the socket without
  //-- ever seeing the message does not halt and reconnects within two
  //-- seconds, stealing control back and starting a ping-pong with the new
  //-- owner: message delivery is the one thing that must never lose that
  //-- race, so the close is deliberately delayed a little.
  vTaskDelay(pdMS_TO_TICKS(50));

  httpd_sess_trigger_close(server, evictedFd);
}

//-- Sends one text frame back on the request that received the command.
static esp_err_t wsSendReply(httpd_req_t* req, const char* text)
{
  httpd_ws_frame_t frame;
  memset(&frame, 0, sizeof frame);
  frame.type = HTTPD_WS_TYPE_TEXT;
  frame.payload = (uint8_t*)(uintptr_t)text;
  frame.len = strlen(text);
  return httpd_ws_send_frame(req, &frame);
}

//-- --------------------------------------------------------------------------
//-- JSON string builder
//-- --------------------------------------------------------------------------

typedef struct
{
  char* buffer;
  size_t capacity;
  size_t length;
  bool overflow;
} stringBuilder;

static void sbInit(stringBuilder* sb, char* buffer, size_t capacity)
{
  sb->buffer = buffer;
  sb->capacity = capacity;
  sb->length = 0u;
  sb->overflow = false;
  buffer[0] = '\0';
}

static void sbAppend(stringBuilder* sb, const char* text)
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

static void sbAppendFmt(stringBuilder* sb, const char* format, ...)
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
static void sbAppendJsonString(stringBuilder* sb, const char* value)
{
  sbAppend(sb, "\"");

  for (const char* cursor = value; *cursor != '\0'; cursor++)
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

static esp_err_t sendJson(httpd_req_t* req, const char* json)
{
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t sendError(httpd_req_t* req, httpd_err_code_t code, const char* message)
{
  char buffer[160];
  snprintf(buffer, sizeof buffer, "{\"ok\":false,\"error\":\"%s\"}", message);
  httpd_resp_set_status(req, (code >= 500) ? "500 Internal Server Error" : "400 Bad Request");
  return sendJson(req, buffer);
}

static esp_err_t sendAsset(httpd_req_t* req, const char* start, const char* end,
                           const char* contentType)
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

static int readBody(httpd_req_t* req, char* buffer, size_t bufferSize)
{
  int length = httpd_req_recv(req, buffer, bufferSize - 1u);

  if (length <= 0)
  {
    return -1;
  }

  buffer[length] = '\0';
  return length;
}

static bool hexDigit(char character, unsigned* out)
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
static void urlDecode(const char* source, char* out, size_t outSize)
{
  size_t position = 0u;

  while ((*source != '\0') && ((position + 1u) < outSize))
  {
    if (*source == '+')
    {
      out[position++] = ' ';
      source++;
    }
    else if ((*source == '%') && isxdigit((unsigned char)source[1]) &&
             isxdigit((unsigned char)source[2]))
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
static bool formGet(const char* body, const char* key, char* out, size_t outSize)
{
  size_t keyLength = strlen(key);
  const char* cursor = body;

  while (cursor != NULL)
  {
    if ((strncmp(cursor, key, keyLength) == 0) && (cursor[keyLength] == '='))
    {
      const char* valueStart = cursor + keyLength + 1u;
      const char* valueEnd = strchr(valueStart, '&');
      size_t valueLength =
          (valueEnd != NULL) ? (size_t)(valueEnd - valueStart) : strlen(valueStart);

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

static const char* mbusTypeToString(dsmrMbusDeviceType type)
{
  switch (type)
  {
  case DSMR_MBUS_DEVICE_GAS:
    return "gas";
  case DSMR_MBUS_DEVICE_WATER:
    return "water";
  case DSMR_MBUS_DEVICE_HEAT:
    return "heat";
  case DSMR_MBUS_DEVICE_COOLING:
    return "cooling";
  case DSMR_MBUS_DEVICE_ELECTRICITY:
    return "electricity";
  case DSMR_MBUS_DEVICE_OTHER:
    return "other";
  default:
    return "other";
  }
}

static bool mbusTypeFromString(const char* text, dsmrMbusDeviceType* out)
{
  struct
  {
    const char* name;
    dsmrMbusDeviceType type;
  } types[] = {
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

static void appendSettingsJson(stringBuilder* sb, const loggerSettingsConfig* config)
{
  sbAppendFmt(sb, "{\"rxGpio\":%d,\"intervalSeconds\":%" PRIu32 ",\"country\":", config->rxGpio,
              config->intervalSeconds);
  sbAppendJsonString(sb, config->countryCode);
  sbAppend(sb, ",\"profileId\":");
  sbAppendJsonString(sb, config->profileId);

  //-- Live P1 RX polarity: false reads the idle-high signal directly, true
  //-- inverts the receive line for interfaces that invert.
  sbAppend(sb, ",\"rxInverted\":");
  sbAppend(sb, config->rxInverted ? "true" : "false");

  //-- P1 TX polarity for the forwarding bursts: false sends both copies
  //-- as-is, true inverts TXD while the burst runs.
  sbAppend(sb, ",\"txInverted\":");
  sbAppend(sb, config->txInverted ? "true" : "false");

  //-- TFT display orientation: false is the assembled landscape, true turns
  //-- the panel 180 degrees.
  sbAppend(sb, ",\"displayFlipped\":");
  sbAppend(sb, config->displayFlipped ? "true" : "false");

  //-- MQTT publication and network identity, prefilled into the settings
  //-- view. The password rides along so the field can show what is stored.
  sbAppend(sb, ",\"mqttBroker\":");
  sbAppendJsonString(sb, config->mqttBroker);
  sbAppendFmt(sb, ",\"mqttPort\":%u", (unsigned)config->mqttPort);
  sbAppend(sb, ",\"mqttUser\":");
  sbAppendJsonString(sb, config->mqttUser);

  //-- The MQTT password is never sent out over any channel (security rule):
  //-- no settings frame on HTTP or WebSocket carries it. The SPA keeps the
  //-- field empty and only sends a password when the operator types a new
  //-- one; an omitted field keeps the stored password (see
  //-- applySettingsFromForm).
  sbAppend(sb, ",\"mqttTopTopic\":");
  sbAppendJsonString(sb, config->mqttTopTopic);
  sbAppendFmt(sb, ",\"mqttIntervalSeconds\":%" PRIu32, config->mqttIntervalSeconds);
  sbAppend(sb, ",\"hostname\":");
  sbAppendJsonString(sb, config->hostname);
  sbAppendFmt(sb, ",\"fixedIpLastOctet\":%u", (unsigned)config->fixedIpLastOctet);

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
static void appendConfigJson(stringBuilder* sb)
{
  loggerSettingsConfig stored;
  bool hasStored = (loggerSettingsLoad(&stored) == ESP_OK);

  loggerSettingsConfig active;
  bool hasActive = loggerReceiverGetActiveConfig(&active);

  loggerSettingsConfig* settings;

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

    snprintf(knownCountries[knownCountryCount], LOGGER_SETTINGS_COUNTRY_MAX, "%s",
             info.countryCode);
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

    const dsmrProfile* profile = dsmrProfileLookup(info.profileId);

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

      for (uint8_t versionIndex = 0; profile->acceptedVersions[versionIndex] != NULL;
           versionIndex++)
      {
        sbAppendFmt(sb, "%s", firstVersion ? "" : ",");
        firstVersion = false;
        sbAppendJsonString(sb, profile->acceptedVersions[versionIndex]);
      }
    }

    sbAppend(sb, "]}");
  }

  sbAppend(sb,
           "],\"mbusTypes\":[\"gas\",\"water\",\"heat\",\"cooling\",\"electricity\",\"other\"]}}");
}

//-- Applies and persists a complete configuration from a form-urlencoded
//-- body. Used by the HTTP POST and by cmd=save on the WebSocket so both
//-- paths validate and roll back identically. NULL on success, otherwise the
//-- failure reason for the SPA.
//-- Copies src into dst with leading and trailing blanks removed,
//-- truncating instead of overflowing.
static void copyTrimmed(char* dst, size_t dstSize, const char* src)
{
  while ((*src == ' ') || (*src == '\t'))
  {
    src++;
  }

  size_t length = strlen(src);

  while ((length > 0u) && ((src[length - 1u] == ' ') || (src[length - 1u] == '\t')))
  {
    length--;
  }

  if (length >= dstSize)
  {
    length = dstSize - 1u;
  }

  memcpy(dst, src, length);
  dst[length] = '\0';
}

//-- Lowercases every letter of the buffer in place (hostname spelling).
static void lowercaseText(char* text)
{
  while (*text != '\0')
  {
    if ((*text >= 'A') && (*text <= 'Z'))
    {
      *text = (char)(*text + ('a' - 'A'));
    }

    text++;
  }
}

static const char* applySettingsFromForm(const char* body)
{
  char text[LOGGER_MQTT_BROKER_MAX];
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

  //-- P1 TX polarity toggle for the forwarding bursts: 1 sends both copies
  //-- of the frame inverted, 0 or an absent field sends them as-is.
  if (formGet(body, "txInverted", text, sizeof text))
  {
    candidate.txInverted = (text[0] == '1');
  }

  //-- Display orientation toggle: 1 turns the panel 180 degrees, 0 or an
  //-- absent field keeps the assembled landscape orientation.
  if (formGet(body, "displayFlipped", text, sizeof text))
  {
    candidate.displayFlipped = (text[0] == '1');
  }

  //-- MQTT publication fields. Absent fields leave the candidate zeroed,
  //-- which keeps publication disabled; blanks around the values are cut.
  if (formGet(body, "mqttBroker", text, sizeof text))
  {
    copyTrimmed(candidate.mqttBroker, sizeof candidate.mqttBroker, text);
  }

  if (formGet(body, "mqttUser", text, sizeof text))
  {
    copyTrimmed(candidate.mqttUser, sizeof candidate.mqttUser, text);
  }

  //-- The MQTT password never travels to the client (see appendSettingsJson),
  //-- so the SPA can only send one it just received from the operator: an
  //-- omitted field keeps the stored password (the settings frame simply does
  //-- not carry it), a present empty field is an explicit clear.
  const char* mqttPasswordOutcome = "kept";

  if (formGet(body, "mqttPassword", text, sizeof text))
  {
    copyTrimmed(candidate.mqttPassword, sizeof candidate.mqttPassword, text);
    mqttPasswordOutcome = (candidate.mqttPassword[0] != '\0') ? "updated" : "cleared";
  }
  else
  {
    loggerSettingsConfig carried;

    //-- Same precedence as the settings frame: the stored configuration
    //-- first, the receiver's active one as the fallback.
    if ((loggerSettingsLoad(&carried) == ESP_OK) || loggerReceiverGetActiveConfig(&carried))
    {
      snprintf(candidate.mqttPassword, sizeof candidate.mqttPassword, "%s", carried.mqttPassword);
    }
  }

  if (formGet(body, "mqttTopTopic", text, sizeof text))
  {
    copyTrimmed(candidate.mqttTopTopic, sizeof candidate.mqttTopTopic, text);
  }

  //-- The broker port is clamped like the read interval: an unusable value
  //-- becomes the default 1883 whenever a broker is named at all.
  int mqttPort = 0;

  if (formGet(body, "mqttPort", text, sizeof text))
  {
    mqttPort = atoi(text);
  }

  if ((candidate.mqttBroker[0] != '\0') && ((mqttPort < 1) || (mqttPort > 65535)))
  {
    mqttPort = (int)LOGGER_SETTINGS_MQTT_PORT_DEFAULT;
  }

  candidate.mqttPort = (uint16_t)((mqttPort < 0) ? 0 : mqttPort);

  //-- The broadcast interval is clamped to its bounds; 0 means no broadcast.
  int mqttInterval = 0;

  if (formGet(body, "mqttInterval", text, sizeof text))
  {
    mqttInterval = atoi(text);
  }

  if (mqttInterval < 0)
  {
    mqttInterval = 0;
  }

  if (mqttInterval > (int)LOGGER_SETTINGS_MQTT_INTERVAL_MAX_S)
  {
    mqttInterval = (int)LOGGER_SETTINGS_MQTT_INTERVAL_MAX_S;
  }

  candidate.mqttIntervalSeconds = (uint32_t)mqttInterval;

  //-- The hostname arrives in any casing: stored lowercase.
  if (formGet(body, "hostname", text, sizeof text))
  {
    copyTrimmed(candidate.hostname, sizeof candidate.hostname, text);
    lowercaseText(candidate.hostname);
  }

  //-- The fixed IP keeps only its last octet; out-of-range input becomes a
  //-- plain DHCP address (0).
  int fixedOctet = 0;

  if (formGet(body, "fixedIp", text, sizeof text))
  {
    fixedOctet = atoi(text);
  }

  candidate.fixedIpLastOctet =
      (uint8_t)((fixedOctet < 1) ? 0 : ((fixedOctet > 254) ? 254 : fixedOctet));

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

  //-- The persisted configuration owns the panel orientation too: apply it
  //-- only now, after activation and persistence both succeeded.
  loggerBoardSetRotation(candidate.displayFlipped);

  //-- Services outside the receive path (network identity, MQTT) adopt the
  //-- stored values through the application's notification callback.
  if (settingsSavedCallback != NULL)
  {
    settingsSavedCallback(&candidate);
  }

  //-- The password itself is never logged: only what happened to it.
  ESP_LOGI(tag,
           "configuration applied and stored: gpio %d, profile %s, rx polarity %s, tx polarity %s, "
           "display %s, mqtt password %s",
           candidate.rxGpio, candidate.profileId, candidate.rxInverted ? "inverted" : "normal",
           candidate.txInverted ? "inverted" : "normal",
           candidate.displayFlipped ? "upside down" : "normal", mqttPasswordOutcome);
  return NULL;
}

//-- --------------------------------------------------------------------------
//-- Status, list, detail and raw JSON builders
//-- --------------------------------------------------------------------------

static const char* stateName(loggerReceiverState state)
{
  switch (state)
  {
  case LOGGER_RECEIVER_AWAITING_CONFIGURATION:
    return "awaitingConfiguration";
  case LOGGER_RECEIVER_AWAITING_DATA:
    return "awaitingData";
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

//-- Emits every status field but the closing brace: the plain status frame
//-- closes it right away, the v3 frame adds the console's Wi-Fi and control
//-- facts first.
static void appendStatusFields(stringBuilder* sb, const loggerReceiverStatus* status)
{
  sbAppend(sb, "{\"type\":\"status\",\"state\":");
  sbAppendJsonString(sb, stateName(status->state));
  sbAppendFmt(sb, ",\"receiverRunning\":%s", status->receiverRunning ? "true" : "false");
  sbAppendFmt(sb, ",\"rxGpio\":%d", status->rxGpio);
  sbAppend(sb, ",\"testMode\":");
  sbAppend(sb, (loggerSettingsIsTestGpio(status->rxGpio) && status->receiverRunning) ? "true"
                                                                                     : "false");
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

  //-- Wall clock: the application's SNTP client sets the system clock after
  //-- the station joins. Until the first sync the header shows the uptime
  //-- instead, so the unavailability stays explicit.
  time_t now = time(NULL);
  bool wallClockAvailable = (now >= 1700000000);

  char deviceTime[20] = "";
  struct tm brokenTime;

  if (wallClockAvailable && (localtime_r(&now, &brokenTime) != NULL))
  {
    strftime(deviceTime, sizeof deviceTime, "%d-%m-%Y %H:%M", &brokenTime);
  }

  sbAppend(sb, ",\"firmwareVersion\":");
  sbAppendJsonString(sb, PROG_VERSION);
  sbAppend(sb, ",\"deviceTime\":");
  sbAppendJsonString(sb, deviceTime);
  sbAppendFmt(sb, ",\"wallClock\":%s", wallClockAvailable ? "true" : "false");
}

static void appendStatusJson(stringBuilder* sb, const loggerReceiverStatus* status)
{
  appendStatusFields(sb, status);
  sbAppend(sb, "}");
}

static void appendMetaJson(stringBuilder* sb, const loggerTelegramMeta* meta)
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

static void appendTelegramListJson(stringBuilder* sb)
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

//-- Power frame for the SPA realtime view: the signed watt, ampere and volt
//-- values of the three phases and the total of the most recently accepted
//-- telegram — the same frame the TFT panel draws. The scale is the panel's
//-- ladder step (loggerBoardPickPowerScale), so both presentations scale
//-- identically. The history array carries the newest retained samples (one
//-- per accepted telegram, oldest first): each sample is an object with its
//-- total watts and the "HH:MM:SS" time it belongs to, so the bar history of
//-- the SPA follows the device, not just what one browser happened to see,
//-- and a hovered bar can show its time and power. valid stays false until
//-- the first accepted telegram.
static void appendPowerJson(stringBuilder* sb)
{
  loggerPowerSnapshot snapshot;
  uint32_t capturedUptimeMs = 0u;
  bool valid = loggerReceiverGetPowerSnapshot(&snapshot, &capturedUptimeMs);

  sbAppend(sb, "{\"type\":\"power\",\"valid\":");
  sbAppend(sb, valid ? "true" : "false");

  if (!valid)
  {
    sbAppend(sb, "}");
    return;
  }

  loggerReceiverStatus status;
  loggerReceiverGetStatus(&status);

  sbAppendFmt(sb, ",\"sequence\":%" PRIu32, snapshot.sequence);
  sbAppendFmt(sb, ",\"capturedUptimeMs\":%" PRIu32, capturedUptimeMs);
  sbAppendFmt(sb, ",\"ageMs\":%" PRIu32, (uint32_t)(status.uptimeMs - capturedUptimeMs));
  sbAppendFmt(sb, ",\"testMode\":%s", snapshot.testData ? "true" : "false");
  sbAppend(sb, ",\"meterTime\":");
  sbAppendJsonString(sb, snapshot.meterTime);
  sbAppendFmt(sb, ",\"scale\":%" PRIu16, loggerBoardPickPowerScale(&snapshot));

  sbAppend(sb, ",\"phases\":[");

  for (uint8_t phase = 0u; phase < 3u; phase++)
  {
    sbAppendFmt(sb, "%s{\"watts\":%.1f,\"amps\":%.3f,\"volts\":%.1f}", (phase > 0u) ? "," : "",
                snapshot.phaseWatts[phase], snapshot.phaseAmps[phase], snapshot.phaseVolts[phase]);
  }

  sbAppendFmt(sb, "],\"total\":{\"watts\":%.1f,\"amps\":%.3f,\"volts\":%.1f}", snapshot.totalWatts,
              snapshot.totalAmps, snapshot.totalVolts);

  sbAppend(sb, ",\"country\":");
  sbAppendJsonString(sb, snapshot.countryCode);
  sbAppend(sb, ",\"profileId\":");
  sbAppendJsonString(sb, snapshot.profileId);
  sbAppendFmt(sb, ",\"intervalSeconds\":%" PRIu32, status.intervalSeconds);

  sbAppend(sb, ",\"history\":[");

  //-- The sample array is heap-allocated: appendPowerJson runs on the small
  //-- wsPush task stack too, where 90 samples do not fit. Out of memory
  //-- simply sends an empty history.
  loggerPowerHistorySample* history = malloc(sizeof *history * LOGGER_POWER_HISTORY_CAPACITY);

  if (history != NULL)
  {
    uint16_t historyCount = loggerReceiverGetPowerHistory(history, LOGGER_POWER_HISTORY_CAPACITY);

    for (uint16_t index = 0u; index < historyCount; index++)
    {
      sbAppendFmt(sb, "%s{\"watts\":%.1f,\"time\":", (index > 0u) ? "," : "",
                  (double)history[index].watts);
      sbAppendJsonString(sb, history[index].timeText);
      sbAppend(sb, "}");
    }

    free(history);
  }

  sbAppend(sb, "]}");
}

//-- Ring file frame for the SPA Show RingFile view: every retained record
//-- of the selected meter ring (hours, days or months), newest period
//-- first, exactly as the ring walk returns them. The counters keep the
//-- units of the telegram (kWh and m³), the tariff amounts are the
//-- fictional billing cents in EUR. ring echoes the walked selector and
//-- available reports whether the store walk completed: while the store
//-- task never opened its rings the records array simply stays empty.
typedef struct
{
  stringBuilder* sb;
  uint32_t emitted;
} RingFileEmit;

//-- Visitor of the ring walk: appends one record as a JSON object. Runs on
//-- the httpd task between the walk's own cursor operations.
static bool appendRingFileRecord(uint64_t key, const loggerMeterRecord* record, void* userData)
{
  RingFileEmit* emit = (RingFileEmit*)userData;
  stringBuilder* sb = emit->sb;

  sbAppendFmt(sb,
              "%s{\"key\":%" PRIu64 ",\"edT1\":%.3f,\"edT2\":%.3f,\"erT1\":%.3f,\"erT2\":%.3f,"
              "\"gas\":%.3f,\"t1\":%.2f,\"t2\":%.2f,\"tG\":%.2f}",
              (emit->emitted > 0u) ? "," : "", key, record->energyDeliveredTariff1Kwh,
              record->energyDeliveredTariff2Kwh, record->energyReturnedTariff1Kwh,
              record->energyReturnedTariff2Kwh, record->gasDeliveredM3, record->tariffAmount1Euro,
              record->tariffAmount2Euro, record->tariffAmountGasEuro);
  emit->emitted++;

  //-- Stop the walk the moment the buffer is full: the client receives the
  //-- records that fit instead of a truncated JSON tail.
  return !sb->overflow;
}

//-- The optional ring selector of a ring file request: "ring=hours|days|"
//-- "month(s)" read from a form body or a URL query. Absent or empty keeps
//-- the hours default; an invalid name is reported as false so the caller
//-- can answer with an error.
static bool ringFromParameters(const char* parameters, loggerMeterRing_t* ring)
{
  *ring = LOGGER_RING_HOURS;

  if (parameters == NULL)
  {
    return true;
  }

  char name[16];

  if (!formGet(parameters, "ring", name, sizeof name))
  {
    return true;
  }

  return loggerStoreRingFromName(name, ring);
}

static void appendRingFileJson(stringBuilder* sb, loggerMeterRing_t ring)
{
  RingFileEmit emit;
  memset(&emit, 0, sizeof emit);
  emit.sb = sb;

  sbAppendFmt(sb, "{\"type\":\"ringfile\",\"ring\":\"%s\",\"records\":[",
              loggerStoreRingName(ring));
  esp_err_t walked = loggerStoreListRecords(ring, appendRingFileRecord, &emit);
  sbAppend(sb, "],\"available\":");
  sbAppendFmt(sb, "%s}", (walked == ESP_OK) ? "true" : "false");
}

//-- --------------------------------------------------------------------------
//-- Hours graph (Hours Graph view)
//-- --------------------------------------------------------------------------

//-- Period usage for the Hours, Days and Months Graph views. The ring
//-- stores the cumulative counters of every retained period, so the amount
//-- of one period is the difference between two consecutive retained
//-- records. The ring walk hands its records newest first; they are
//-- collected into this small array (the full capacity of the largest ring)
//-- and rendered oldest first so the graph reads left to right. The oldest
//-- record has no predecessor and is left out. A difference below zero (a
//-- replaced meter resets its counters) is clamped to zero. A record that
//-- does not name the period directly after its retained predecessor
//-- (imported history that ends in the past, periods the ring no longer
//-- holds) would credit that whole gap to one bar, so such records are left
//-- out and the graph shows a hole.
#define WEB_HOURSGRAPH_MAX_RECORDS 49u

typedef struct
{
  uint64_t keys[WEB_HOURSGRAPH_MAX_RECORDS];
  loggerMeterRecord records[WEB_HOURSGRAPH_MAX_RECORDS];
  uint32_t collected;
} PeriodGraphCollect;

//-- Visitor of the period ring walk: stores each record newest first, the
//-- order the walk hands them. The walk stops once the ring capacity is
//-- reached; anything beyond would be a broken ring anyway.
static bool collectPeriodGraphRecord(uint64_t key, const loggerMeterRecord* record, void* userData)
{
  PeriodGraphCollect* collect = (PeriodGraphCollect*)userData;

  if (collect->collected >= WEB_HOURSGRAPH_MAX_RECORDS)
  {
    return false;
  }

  collect->keys[collect->collected] = key;
  collect->records[collect->collected] = *record;
  collect->collected++;
  return true;
}

//-- One hour's usage from two cumulative counter totals: the newer total
//-- minus the older one, clamped at zero for a counter reset.
static float usageDelta(float newerTotal, float olderTotal)
{
  float delta = newerTotal - olderTotal;
  return (delta > 0.0f) ? delta : 0.0f;
}

//-- Number of days in one month of a 2-digit year (20YY): the calendar
//-- needed to ask whether two YYMMDDhh keys name consecutive hours.
static uint8_t daysInMonth2000(unsigned year2, unsigned month)
{
  static const uint8_t daysPerMonth[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

  if ((month == 2u) && (((2000u + year2) % 4u) == 0u) &&
      (((2000u + year2) % 100u) != 0u || ((2000u + year2) % 400u) == 0u))
  {
    return 29u;
  }

  return ((month >= 1u) && (month <= 12u)) ? daysPerMonth[month - 1u] : 31u;
}

//-- The hour key that follows a YYMMDDhh key (calendar carry over hour,
//-- day, month and year), so two retained records can be recognised as
//-- naming directly consecutive hours.
static uint64_t nextHourKey(uint64_t key)
{
  unsigned hour = (unsigned)(key % 100u);
  key /= 100u;
  unsigned day = (unsigned)(key % 100u);
  key /= 100u;
  unsigned month = (unsigned)(key % 100u);
  unsigned year2 = (unsigned)(key / 100u);

  hour++;

  if (hour > 23u)
  {
    hour = 0u;
    day++;

    if (day > daysInMonth2000(year2, month))
    {
      day = 1u;
      month++;

      if (month > 12u)
      {
        month = 1u;
        year2 = (year2 + 1u) % 100u;
      }
    }
  }

  return ((uint64_t)(year2 * 100u + month) * 100u + day) * 100u + hour;
}

//-- The day key that follows a YYMMDD key (calendar carry over day, month
//-- and year), so two retained records can be recognised as naming directly
//-- consecutive days.
static uint64_t nextDayKey(uint64_t key)
{
  unsigned day = (unsigned)(key % 100u);
  key /= 100u;
  unsigned month = (unsigned)(key % 100u);
  unsigned year2 = (unsigned)(key / 100u);

  day++;

  if (day > daysInMonth2000(year2, month))
  {
    day = 1u;
    month++;

    if (month > 12u)
    {
      month = 1u;
      year2 = (year2 + 1u) % 100u;
    }
  }

  return ((uint64_t)year2 * 100u + month) * 100u + day;
}

//-- The month key that follows a YYMM key (calendar carry over year), so
//-- two retained records can be recognised as naming directly consecutive
//-- months.
static uint64_t nextMonthKey(uint64_t key)
{
  unsigned month = (unsigned)(key % 100u);
  unsigned year2 = (unsigned)(key / 100u);

  month++;

  if (month > 12u)
  {
    month = 1u;
    year2 = (year2 + 1u) % 100u;
  }

  return (uint64_t)year2 * 100u + month;
}

//-- The key of the period that follows a retained key of one ring: the
//-- calendar successor in the ring's own key shape (YYMMDDhh, YYMMDD or
//-- YYMM).
static uint64_t nextPeriodKey(loggerMeterRing_t ring, uint64_t key)
{
  if (ring == LOGGER_RING_DAYS)
  {
    return nextDayKey(key);
  }

  if (ring == LOGGER_RING_MONTHS)
  {
    return nextMonthKey(key);
  }

  return nextHourKey(key);
}

//-- Emits one period graph frame under a caller-chosen type name: one line
//-- per retained period (except the oldest) with the per-period
//-- used/returned kWh and gas m³, oldest first, plus the ring's selector
//-- name as the walk echoed it.
static void appendPeriodGraphFrame(stringBuilder* sb, loggerMeterRing_t ring, const char* typeName)
{
  PeriodGraphCollect* collect = calloc(1u, sizeof *collect);

  if (collect == NULL)
  {
    sbAppendFmt(sb, "{\"type\":\"%s\",\"ring\":\"%s\",\"records\":[],\"available\":false}",
                typeName, loggerStoreRingName(ring));
    return;
  }

  esp_err_t walked = loggerStoreListRecords(ring, collectPeriodGraphRecord, collect);

  sbAppendFmt(sb, "{\"type\":\"%s\",\"ring\":\"%s\",\"records\":[", typeName,
              loggerStoreRingName(ring));

  uint32_t emitted = 0u;

  //-- Oldest first: the walk filled the arrays newest first, so the array
  //-- is walked backwards and the oldest entry (with no predecessor to
  //-- subtract) is skipped.
  for (uint32_t index = collect->collected; index > 1u;)
  {
    index--;

    //-- Only subtract when the two records name directly consecutive
    //-- periods: a retained gap (imported history ending in the past,
    //-- periods aged out of the ring) would credit the whole gap to one bar.
    if (nextPeriodKey(ring, collect->keys[index]) != collect->keys[index - 1u])
    {
      continue;
    }

    const loggerMeterRecord* newer = &collect->records[index - 1u];
    const loggerMeterRecord* older = &collect->records[index];

    float usedKwh = usageDelta(newer->energyDeliveredTariff1Kwh + newer->energyDeliveredTariff2Kwh,
                               older->energyDeliveredTariff1Kwh + older->energyDeliveredTariff2Kwh);
    float returnedKwh =
        usageDelta(newer->energyReturnedTariff1Kwh + newer->energyReturnedTariff2Kwh,
                   older->energyReturnedTariff1Kwh + older->energyReturnedTariff2Kwh);
    float gasM3 = usageDelta(newer->gasDeliveredM3, older->gasDeliveredM3);

    sbAppendFmt(sb, "%s{\"key\":%" PRIu64 ",\"used\":%.3f,\"returned\":%.3f,\"gas\":%.3f}",
                (emitted > 0u) ? "," : "", collect->keys[index - 1u], usedKwh, returnedKwh, gasM3);
    emitted++;
  }

  sbAppend(sb, "],\"available\":");
  sbAppendFmt(sb, "%s}", (walked == ESP_OK) ? "true" : "false");

  free(collect);
}

//-- Period graph frames of the SPA: cmd=graph&ring=<hours|days|months>
//-- answers under type "graph" with the ring echoed.
static void appendPeriodGraphJson(stringBuilder* sb, loggerMeterRing_t ring)
{
  appendPeriodGraphFrame(sb, ring, "graph");
}

//-- Legacy spelling of the hours ring graph frame (cmd=hoursgraph on the
//-- WebSocket): the same frame as graph&ring=hours, answering under the
//-- original type name so existing clients keep working.
static void appendHoursGraphJson(stringBuilder* sb)
{
  appendPeriodGraphFrame(sb, LOGGER_RING_HOURS, "hoursgraph");
}

//-- Actual Graph frame: the rolling table of the newest accepted telegrams
//-- kept by the receiver (a hundred samples, twice that when the heap
//-- carried the wider table at boot). The frame echoes the table's real
//-- slot count as "capacity" so the SPA plots on the matching pitch. Each
//-- record carries the sample's "HH:MM:SS" time, the unsigned delivered and
//-- returned watts per phase and the held gas m³/h rate, oldest sample
//-- first: the SPA draws them left to right, newest at the right edge, so
//-- the graph fills from the right and scrolls left once the table is
//-- full. The sample array is heap-allocated: this runs on the small wsPush
//-- and httpd task stacks too, where the samples do not fit. Out of memory
//-- sends an empty table.
static void appendActualGraphJson(stringBuilder* sb)
{
  sbAppend(sb, "{\"type\":\"actualgraph\",\"records\":[");

  uint16_t count = 0u;
  uint16_t capacity = loggerReceiverGetPowerHistoryCapacity();

  if (capacity == 0u)
  {
    capacity = LOGGER_POWER_HISTORY_CAPACITY;
  }

  loggerPowerHistorySample* history = malloc(sizeof *history * capacity);

  if (history != NULL)
  {
    count = loggerReceiverGetPowerHistory(history, capacity);

    for (uint16_t index = 0u; index < count; index++)
    {
      sbAppend(sb, (index > 0u) ? "," : "");
      sbAppend(sb, "{\"time\":");
      sbAppendJsonString(sb, history[index].timeText);
      sbAppendFmt(
          sb, ",\"used\":[%.1f,%.1f,%.1f],\"returned\":[%.1f,%.1f,%.1f],\"gas\":%.3f}",
          (double)history[index].phaseUsedWatts[0], (double)history[index].phaseUsedWatts[1],
          (double)history[index].phaseUsedWatts[2], (double)history[index].phaseReturnedWatts[0],
          (double)history[index].phaseReturnedWatts[1],
          (double)history[index].phaseReturnedWatts[2], (double)history[index].gasM3h);
    }

    free(history);
  }

  sbAppend(sb, "],\"capacity\":");
  sbAppendFmt(sb, "%" PRIu16 ",\"available\":", capacity);
  sbAppendFmt(sb, "%s}", (count > 0u) ? "true" : "false");
}

//-- Callback of appendTelegramDetailJson: renders one retained record in
//-- place while the receive-side lock is held. Memory only: no network and
//-- no blocking, as required by the inspect contract.
static bool buildDetailFromRecord(const loggerTelegramRecord* record, void* userData)
{
  stringBuilder* sb = (stringBuilder*)userData;

  sbAppend(sb, "{\"type\":\"telegram\",\"meta\":");
  appendMetaJson(sb, &record->meta);
  sbAppend(sb, ",\"fields\":[");

  for (uint16_t index = 0u; index < record->meta.fieldCount; index++)
  {
    const loggerFieldSnapshot* field = &record->fields[index];

    sbAppendFmt(sb, "%s{\"obis\":", (index > 0u) ? "," : "");
    sbAppendJsonString(sb, field->obis);
    sbAppend(sb, ",\"name\":");
    sbAppendJsonString(sb, field->name);
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
static bool appendTelegramDetailJson(stringBuilder* sb, uint32_t sequence)
{
  return loggerReceiverInspectRecord(sequence, buildDetailFromRecord, sb);
}

//-- --------------------------------------------------------------------------
//-- SPA asset handlers
//-- --------------------------------------------------------------------------

static esp_err_t handleRoot(httpd_req_t* req)
{
  return sendAsset(req, indexHtmlStart, indexHtmlEnd, "text/html; charset=utf-8");
}

static esp_err_t handleAppJs(httpd_req_t* req)
{
  return sendAsset(req, appJsStart, appJsEnd, "application/javascript; charset=utf-8");
}

static esp_err_t handleStyleCss(httpd_req_t* req)
{
  return sendAsset(req, styleCssStart, styleCssEnd, "text/css; charset=utf-8");
}

static esp_err_t handleFavicon(httpd_req_t* req)
{
  httpd_resp_set_status(req, "204 No Content");
  return httpd_resp_send(req, NULL, 0);
}

//-- --------------------------------------------------------------------------
//-- API handlers
//-- --------------------------------------------------------------------------

static esp_err_t handleConfigGet(httpd_req_t* req)
{
  char* buffer = malloc(WEB_CONFIG_JSON_BYTES);

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

static esp_err_t handleConfigSave(httpd_req_t* req)
{
  char body[WEB_BODY_MAX];

  if (readBody(req, body, sizeof body) < 0)
  {
    return sendError(req, 400, "cannot read body");
  }

  const char* failure = applySettingsFromForm(body);

  if (failure != NULL)
  {
    return sendError(req, 400, failure);
  }

  return sendJson(req, "{\"ok\":true}");
}

static esp_err_t handleTelegramsGet(httpd_req_t* req)
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

//-- Serves the exact accepted bytes of one retained record as plain text
//-- (original line endings and trailer preserved, so nothing can be
//-- interpreted as HTML). Used by the v3 raw endpoint; an aged-out sequence
//-- answers like the console reports it.
static esp_err_t serveTelegramRawBytes(httpd_req_t* req, uint32_t sequence)
{
  loggerTelegramRecord* record = malloc(sizeof *record);

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

  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  esp_err_t result =
      httpd_resp_send(req, (const char*)record->rawBytes, (int)record->meta.rawLength);
  free(record);
  return result;
}

//-- Serves the decoded detail JSON of one retained record (the same frame
//-- the cmd=telegram answer sends). Used by the v3 telegram endpoint.
static esp_err_t serveTelegramDetailJson(httpd_req_t* req, uint32_t sequence)
{
  char* json = malloc(WEB_DETAIL_JSON_BYTES);

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

static esp_err_t handlePowerGet(httpd_req_t* req)
{
  //-- The frame plus its 90-sample history is sized for the heap: the httpd
  //-- task stack stays free of the large buffer, as on the other JSON paths.
  char* buffer = malloc(WEB_POWER_JSON_BYTES);

  if (buffer == NULL)
  {
    return sendError(req, 500, "out of memory");
  }

  stringBuilder sb;
  sbInit(&sb, buffer, WEB_POWER_JSON_BYTES);
  appendPowerJson(&sb);

  if (sb.overflow)
  {
    ESP_LOGE(tag, "power json overflow");
  }

  esp_err_t result = sendJson(req, buffer);
  free(buffer);
  return result;
}

static esp_err_t handleRingFileGet(httpd_req_t* req)
{
  //-- The ring table of one meter ring file for diagnostics: the same frame
  //-- the cmd=ringfile answer sends. The optional ?ring= query selects the
  //-- ring (hours by default).
  loggerMeterRing_t ring = LOGGER_RING_HOURS;
  const char* query = strchr(req->uri, '?');

  if ((query != NULL) && !ringFromParameters(query + 1, &ring))
  {
    return sendError(req, 400, "unknown ring");
  }

  char* buffer = malloc(WEB_RINGFILE_JSON_BYTES);

  if (buffer == NULL)
  {
    return sendError(req, 500, "out of memory");
  }

  stringBuilder sb;
  sbInit(&sb, buffer, WEB_RINGFILE_JSON_BYTES);
  appendRingFileJson(&sb, ring);

  if (sb.overflow)
  {
    ESP_LOGE(tag, "ring file json overflow");
  }

  esp_err_t result = sendJson(req, buffer);
  free(buffer);
  return result;
}

static esp_err_t handleGraphGet(httpd_req_t* req)
{
  //-- The usage series of one ring for the Hours, Days and Months Graph
  //-- views for diagnostics: the same frame the cmd=graph answer sends. The
  //-- optional ?ring= query selects the ring (hours by default).
  loggerMeterRing_t ring = LOGGER_RING_HOURS;
  const char* query = strchr(req->uri, '?');

  if ((query != NULL) && !ringFromParameters(query + 1, &ring))
  {
    return sendError(req, 400, "unknown ring");
  }

  char* buffer = malloc(WEB_HOURSGRAPH_JSON_BYTES);

  if (buffer == NULL)
  {
    return sendError(req, 500, "out of memory");
  }

  stringBuilder sb;
  sbInit(&sb, buffer, WEB_HOURSGRAPH_JSON_BYTES);
  appendPeriodGraphJson(&sb, ring);

  if (sb.overflow)
  {
    ESP_LOGE(tag, "period graph json overflow");
  }

  esp_err_t result = sendJson(req, buffer);
  free(buffer);
  return result;
}

static esp_err_t handleActualGraphGet(httpd_req_t* req)
{
  //-- The rolling per-phase table of the Actual Graph view for diagnostics:
  //-- the same frame the cmd=actualgraph answer sends.
  char* buffer = malloc(WEB_ACTUALGRAPH_JSON_BYTES);

  if (buffer == NULL)
  {
    return sendError(req, 500, "out of memory");
  }

  stringBuilder sb;
  sbInit(&sb, buffer, WEB_ACTUALGRAPH_JSON_BYTES);
  appendActualGraphJson(&sb);

  if (sb.overflow)
  {
    ESP_LOGE(tag, "actual graph json overflow");
    free(buffer);
    return sendError(req, 500, "actual graph json overflow");
  }

  esp_err_t result = sendJson(req, buffer);
  free(buffer);
  return result;
}

static esp_err_t handleViewReport(httpd_req_t* req)
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
//-- v3 API: standard data calls under /api/v3/
//-- --------------------------------------------------------------------------

//-- Every datum the telnet console and the SPA can request and show is also
//-- retrievable here as a plain GET, each endpoint under a short
//-- descriptive English name. The frames reuse the WebSocket JSON builders
//-- (so they carry the same "type" tag and fields the SPA receives); the
//-- console-only answers — version line, device state block, LittleFS
//-- volume listing and the csv rendering of a ring — are added as JSON (or
//-- csv text) in this section. The two actions the SPA normally sends over
//-- the WebSocket (save settings, report the viewed screen) are the POST
//-- endpoints below; a populate import stays on the console. There is no
//-- unversioned /api/ surface anymore: everything lives under /api/v3/.

typedef struct
{
  const char* method;
  const char* uri;
  const char* description;
} ApiV3Entry;

//-- The published v3 surface, listed verbatim by the index endpoint.
static const ApiV3Entry apiV3Entries[] = {
    {"GET", "/api/v3", "this index of the v3 endpoints"},
    {"GET", "/api/v3/config", "stored settings with the active state and valid choices"},
    {"POST", "/api/v3/config",
     "save form-urlencoded settings; omitting mqttPassword keeps the stored password"},
    {"GET", "/api/v3/status", "reception state, counters, clock, Wi-Fi and control owners"},
    {"GET", "/api/v3/version",
     "firmware, parser, ESP-IDF and compiler versions plus the managed component versions"},
    {"GET", "/api/v3/device",
     "device state block: identity, versions, memory, chip and flash facts"},
    {"GET", "/api/v3/telegrams", "metadata of the retained telegram records, newest first"},
    {"GET", "/api/v3/telegram", "decoded fields of the most recently accepted telegram"},
    {"GET", "/api/v3/telegram/<seq>", "decoded fields of one retained telegram"},
    {"GET", "/api/v3/raw", "exact bytes of the most recently accepted telegram (text/plain)"},
    {"GET", "/api/v3/raw/<seq>", "exact accepted bytes of one retained telegram (text/plain)"},
    {"GET", "/api/v3/power", "realtime power frame of the newest telegram with its sample history"},
    {"GET", "/api/v3/ringfile?ring=<hours|days|months>",
     "stored records of one meter ring, newest first"},
    {"GET", "/api/v3/graph?ring=<hours|days|months>",
     "per-period used/returned kWh and gas m3 of one ring, oldest first"},
    {"GET", "/api/v3/actualgraph", "rolling per-phase sample table of the newest telegrams"},
    {"GET", "/api/v3/store", "LittleFS volume: files with sizes, capacity and free space"},
    {"GET", "/api/v3/csv?ring=<hours|days|months>",
     "one meter ring as re-importable csv (text/csv, the ring2csv content)"},
    {"POST", "/api/v3/view", "reports the active browser screen name to the device display"},
};

static esp_err_t handleApiV3IndexGet(httpd_req_t* req)
{
  char* buffer = malloc(WEB_CONFIG_JSON_BYTES);

  if (buffer == NULL)
  {
    return sendError(req, 500, "out of memory");
  }

  stringBuilder sb;
  sbInit(&sb, buffer, WEB_CONFIG_JSON_BYTES);
  sbAppend(&sb, "{\"type\":\"api\",\"api\":\"v3\",\"endpoints\":[");

  for (size_t index = 0u; index < (sizeof apiV3Entries / sizeof apiV3Entries[0]); index++)
  {
    sbAppendFmt(&sb, "%s{\"method\":", (index > 0u) ? "," : "");
    sbAppendJsonString(&sb, apiV3Entries[index].method);
    sbAppend(&sb, ",\"uri\":");
    sbAppendJsonString(&sb, apiV3Entries[index].uri);
    sbAppend(&sb, ",\"description\":");
    sbAppendJsonString(&sb, apiV3Entries[index].description);
    sbAppend(&sb, "}");
  }

  sbAppend(&sb, "]}");

  esp_err_t result = sendJson(req, buffer);
  free(buffer);
  return result;
}

static esp_err_t handleApiV3StatusGet(httpd_req_t* req)
{
  loggerReceiverStatus status;
  loggerReceiverGetStatus(&status);

  char buffer[WEB_STATUS_V3_JSON_BYTES];
  stringBuilder sb;
  sbInit(&sb, buffer, sizeof buffer);
  appendStatusFields(&sb, &status);

  //-- The Wi-Fi facts the console's status command prints next to the
  //-- reception block: the configured SSID and the granted address.
  wifi_config_t stationConfig;
  memset(&stationConfig, 0, sizeof stationConfig);
  const char* ssid = "?";

  if (esp_wifi_get_config(WIFI_IF_STA, &stationConfig) == ESP_OK)
  {
    ssid = (const char*)stationConfig.sta.ssid;
  }

  sbAppend(&sb, ",\"wifiSsid\":");
  sbAppendJsonString(&sb, ssid);

  esp_netif_ip_info_t ipInfo;
  memset(&ipInfo, 0, sizeof ipInfo);
  char ip[16] = "not connected";
  bool wifiConnected = (wifi_prov_get_ip_info(&ipInfo) == ESP_OK);

  if (wifiConnected)
  {
    snprintf(ip, sizeof ip, IPSTR, IP2STR(&ipInfo.ip));
  }

  sbAppend(&sb, ",\"wifiIp\":");
  sbAppendJsonString(&sb, ip);
  sbAppendFmt(&sb, ",\"wifiConnected\":%s", wifiConnected ? "true" : "false");

  //-- One control slot per channel: a browser page and a telnet session can
  //-- each hold their own, so both claims are reported.
  int ownerFd = -1;
  bool webClaimed = loggerControlGetOwner(LOGGER_CONTROL_WEB, &ownerFd);
  ownerFd = -1;
  bool telnetClaimed = loggerControlGetOwner(LOGGER_CONTROL_TELNET, &ownerFd);

  sbAppendFmt(&sb, ",\"controlWebClaimed\":%s", webClaimed ? "true" : "false");
  sbAppendFmt(&sb, ",\"controlTelnetClaimed\":%s", telnetClaimed ? "true" : "false");
  sbAppend(&sb, "}");

  return sendJson(req, buffer);
}

static esp_err_t handleApiV3VersionGet(httpd_req_t* req)
{
  //-- The console's version line as JSON, plus the compile facts the state
  //-- block prints above it.
  char compileStamp[32];
  snprintf(compileStamp, sizeof compileStamp, "%s %s", __DATE__, __TIME__);

  char buffer[1024];
  stringBuilder sb;
  sbInit(&sb, buffer, sizeof buffer);
  sbAppend(&sb, "{\"type\":\"version\",\"firmwareVersion\":");
  sbAppendJsonString(&sb, PROG_VERSION);
  sbAppend(&sb, ",\"parserVersion\":");
  sbAppendJsonString(&sb, dsmrParserVersion());
  sbAppend(&sb, ",\"idfVersion\":");
  sbAppendJsonString(&sb, esp_get_idf_version());
  sbAppend(&sb, ",\"compilerVersion\":");
  sbAppendJsonString(&sb, __VERSION__);
  sbAppend(&sb, ",\"compileStamp\":");
  sbAppendJsonString(&sb, compileStamp);

  //-- The managed component list the console's status and version commands
  //-- print, from the very table the console uses (logger_telnet owns it
  //-- together with the state block).
  size_t componentCount = 0u;
  const loggerTelnetComponentVersion* components =
      loggerTelnetGetComponentVersions(&componentCount);

  sbAppend(&sb, ",\"components\":[");

  for (size_t componentIndex = 0u; components != NULL && componentIndex < componentCount;
       componentIndex++)
  {
    if (componentIndex > 0u)
    {
      sbAppend(&sb, ",");
    }

    sbAppend(&sb, "{\"name\":");
    sbAppendJsonString(&sb, components[componentIndex].name);
    sbAppend(&sb, ",\"version\":");
    sbAppendJsonString(&sb, components[componentIndex].version);
    sbAppend(&sb, "}");
  }

  sbAppend(&sb, "]}");

  return sendJson(req, buffer);
}

static esp_err_t handleApiV3DeviceGet(httpd_req_t* req)
{
  //-- The console's state block as JSON: same facts, collected by the very
  //-- getter the console prints from (logger_telnet owns the state block).
  loggerTelnetDeviceInfo info;

  if (!loggerTelnetGetDeviceInfo(&info))
  {
    return sendError(req, 500, "device facts unavailable");
  }

  char* buffer = malloc(WEB_DEVICE_JSON_BYTES);

  if (buffer == NULL)
  {
    return sendError(req, 500, "out of memory");
  }

  stringBuilder sb;
  sbInit(&sb, buffer, WEB_DEVICE_JSON_BYTES);

  sbAppend(&sb, "{\"type\":\"device\",\"copyright\":");
  sbAppendJsonString(&sb, info.copyright);
  sbAppend(&sb, ",\"hostname\":");
  sbAppendJsonString(&sb, info.hostname);
  sbAppend(&sb, ",\"firmwareVersion\":");
  sbAppendJsonString(&sb, info.firmwareVersion);
  sbAppend(&sb, ",\"compileDate\":");
  sbAppendJsonString(&sb, info.compileDate);
  sbAppend(&sb, ",\"compileStamp\":");
  sbAppendJsonString(&sb, info.compileStamp);
  sbAppend(&sb, ",\"idfVersion\":");
  sbAppendJsonString(&sb, info.idfVersion);
  sbAppend(&sb, ",\"parserVersion\":");
  sbAppendJsonString(&sb, info.parserVersion);
  sbAppend(&sb, ",\"compilerVersion\":");
  sbAppendJsonString(&sb, info.compilerVersion);
  sbAppend(&sb, ",\"chipModel\":");
  sbAppendJsonString(&sb, info.chipModel);
  sbAppendFmt(&sb, ",\"chipRevisionMajor\":%u,\"chipRevisionMinor\":%u,\"chipCores\":%d",
              info.chipRevisionMajor, info.chipRevisionMinor, info.chipCores);
  sbAppendFmt(&sb, ",\"chipId\":%" PRIu64, info.chipId);
  sbAppendFmt(&sb, ",\"cpuFreqMhz\":%d", info.cpuFreqMhz);
  sbAppendFmt(&sb, ",\"freeHeapBytes\":%" PRIu32, info.freeHeapBytes);
  sbAppendFmt(&sb, ",\"minFreeHeapBytes\":%" PRIu32, info.minFreeHeapBytes);
  sbAppendFmt(&sb, ",\"largestFreeBlockBytes\":%" PRIu32, info.largestFreeBlockBytes);
  sbAppendFmt(&sb, ",\"psramFreeBytes\":%" PRIu32, info.psramFreeBytes);
  sbAppendFmt(&sb, ",\"psramTotalBytes\":%" PRIu32, info.psramTotalBytes);
  sbAppendFmt(&sb, ",\"hasAppPartition\":%s", info.hasAppPartition ? "true" : "false");
  sbAppendFmt(&sb, ",\"appPartitionBytes\":%" PRIu32, info.appPartitionBytes);
  sbAppendFmt(&sb, ",\"imageVerified\":%s", info.imageVerified ? "true" : "false");
  sbAppendFmt(&sb, ",\"imageBytes\":%" PRIu32, info.imageBytes);
  sbAppendFmt(&sb, ",\"freeSketchBytes\":%" PRIu32,
              info.imageVerified ? (info.appPartitionBytes - info.imageBytes) : 0u);
  sbAppendFmt(&sb, ",\"hasImageHeader\":%s", info.hasImageHeader ? "true" : "false");
  sbAppend(&sb, ",\"flashMode\":");
  sbAppendJsonString(&sb, info.flashMode);
  sbAppend(&sb, ",\"flashSpeed\":");
  sbAppendJsonString(&sb, info.flashSpeed);
  sbAppendFmt(&sb, ",\"flashSizeReadable\":%s", info.flashSizeReadable ? "true" : "false");
  sbAppendFmt(&sb, ",\"flashBytes\":%" PRIu32, info.flashBytes);
  sbAppend(&sb, ",\"flashSizeError\":");
  sbAppendJsonString(&sb, info.flashSizeError);
  sbAppendFmt(&sb, ",\"hasStoragePartition\":%s", info.hasStoragePartition ? "true" : "false");
  sbAppendFmt(&sb, ",\"storageKb\":%" PRIu32, info.storageKb);
  sbAppend(&sb, "}");

  if (sb.overflow)
  {
    ESP_LOGE(tag, "device json overflow");
  }

  esp_err_t result = sendJson(req, buffer);
  free(buffer);
  return result;
}

//-- Visitor of the volume scan: renders one file as a JSON entry.
typedef struct
{
  stringBuilder* sb;
  uint32_t emitted;
} StoreFileEmit;

static bool appendStoreFileEntry(const char* name, bool isDirectory, bool readable,
                                 uint32_t sizeBytes, void* userData)
{
  StoreFileEmit* emit = (StoreFileEmit*)userData;
  stringBuilder* sb = emit->sb;

  sbAppendFmt(sb, "%s{\"name\":", (emit->emitted > 0u) ? "," : "");
  sbAppendJsonString(sb, name);
  sbAppendFmt(sb, ",\"directory\":%s,\"readable\":%s,\"sizeBytes\":%" PRIu32 "}",
              isDirectory ? "true" : "false", readable ? "true" : "false", sizeBytes);
  emit->emitted++;

  //-- Stop the scan the moment the buffer is full: the client receives the
  //-- entries that fit instead of a truncated JSON tail.
  return !sb->overflow;
}

static esp_err_t handleApiV3StoreGet(httpd_req_t* req)
{
  //-- The console's store command as JSON: every file on the mounted
  //-- LittleFS volume with its size, then the capacity block.
  char* buffer = malloc(WEB_STORE_JSON_BYTES);

  if (buffer == NULL)
  {
    return sendError(req, 500, "out of memory");
  }

  stringBuilder sb;
  sbInit(&sb, buffer, WEB_STORE_JSON_BYTES);

  bool mounted = (loggerStoreMountPoint() != NULL);
  sbAppendFmt(&sb, "{\"type\":\"store\",\"mounted\":%s,\"files\":[", mounted ? "true" : "false");

  StoreFileEmit emit;
  memset(&emit, 0, sizeof emit);
  emit.sb = &sb;
  esp_err_t scanned =
      mounted ? loggerStoreListFiles(appendStoreFileEntry, &emit) : ESP_ERR_INVALID_STATE;

  sbAppendFmt(&sb, "],\"available\":%s", (scanned == ESP_OK) ? "true" : "false");

  size_t totalBytes = 0u;
  size_t usedBytes = 0u;
  bool hasCapacity = loggerStoreVolumeInfo(&totalBytes, &usedBytes);

  sbAppendFmt(&sb, ",\"capacity\":{\"available\":%s", hasCapacity ? "true" : "false");

  if (hasCapacity)
  {
    sbAppendFmt(&sb, ",\"totalBytes\":%lu,\"usedBytes\":%lu,\"freeBytes\":%lu",
                (unsigned long)totalBytes, (unsigned long)usedBytes,
                (unsigned long)(totalBytes - usedBytes));
  }

  //-- Closes both the capacity object and the root object.
  sbAppend(&sb, "}}");

  if (sb.overflow)
  {
    ESP_LOGE(tag, "store json overflow");
  }

  esp_err_t result = sendJson(req, buffer);
  free(buffer);
  return result;
}

static esp_err_t handleApiV3CsvGet(httpd_req_t* req)
{
  //-- The csv content the console's ring2csv command writes to the volume,
  //-- served directly as a download: same header, same lines, same file
  //-- name (the text re-imports through populate unchanged). The optional
  //-- ?ring= query selects the ring (hours by default).
  loggerMeterRing_t ring = LOGGER_RING_HOURS;
  const char* query = strchr(req->uri, '?');

  if ((query != NULL) && !ringFromParameters(query + 1, &ring))
  {
    return sendError(req, 400, "unknown ring");
  }

  char* text = NULL;
  uint32_t records = 0u;
  esp_err_t outcome = loggerStoreRingToCsvText(ring, &text, &records);

  if (outcome != ESP_OK)
  {
    free(text);
    return sendError(req, 500,
                     (outcome == ESP_ERR_INVALID_STATE) ? "meter store not running"
                                                        : esp_err_to_name(outcome));
  }

  char disposition[64];
  snprintf(disposition, sizeof disposition, "attachment; filename=\"RING%s.csv\"",
           loggerStoreRingName(ring));

  httpd_resp_set_hdr(req, "Content-Disposition", disposition);
  httpd_resp_set_type(req, "text/csv; charset=utf-8");

  ESP_LOGI(tag, "v3 csv served %" PRIu32 " record(s) of the %s ring", records,
           loggerStoreRingName(ring));

  esp_err_t result = httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
  free(text);
  return result;
}

//-- Sequence of the most recently accepted telegram (the newest entry of
//-- the retained list). False while no telegram was accepted at all, so the
//-- endpoints without a sequence can answer like the console does.
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

static esp_err_t handleApiV3TelegramGet(httpd_req_t* req)
{
  const char* argument = req->uri + strlen("/api/v3/telegram");

  while (*argument == '/')
  {
    argument++;
  }

  //-- No sequence in the URI: the most recently accepted telegram, like the
  //-- console's telegram command without an argument.
  if (argument[0] == '\0')
  {
    uint32_t sequence = 0u;

    if (!latestAcceptedSequence(&sequence))
    {
      return sendError(req, 404, "no accepted telegram yet");
    }

    return serveTelegramDetailJson(req, sequence);
  }

  return serveTelegramDetailJson(req, (uint32_t)strtoul(argument, NULL, 10));
}

static esp_err_t handleApiV3RawGet(httpd_req_t* req)
{
  const char* argument = req->uri + strlen("/api/v3/raw");

  while (*argument == '/')
  {
    argument++;
  }

  //-- No sequence: the most recently accepted telegram, like the console's
  //-- raw command without an argument.
  if (argument[0] == '\0')
  {
    uint32_t sequence = 0u;

    if (!latestAcceptedSequence(&sequence))
    {
      httpd_resp_set_status(req, "404 Not Found");
      httpd_resp_set_type(req, "text/plain; charset=utf-8");
      return httpd_resp_send(req, "no accepted telegram yet", HTTPD_RESP_USE_STRLEN);
    }

    return serveTelegramRawBytes(req, sequence);
  }

  return serveTelegramRawBytes(req, (uint32_t)strtoul(argument, NULL, 10));
}

//-- --------------------------------------------------------------------------
//-- WebSocket handlers
//-- --------------------------------------------------------------------------

//-- Answers one form-urlencoded command frame received on /ws. Responses are
//-- JSON text frames, except cmd=raw which sends a JSON header frame followed
//-- by a binary frame carrying the exact accepted bytes.
static void wsHandleCommand(httpd_req_t* req, const char* body)
{
  char command[16];

  if (!formGet(body, "cmd", command, sizeof command))
  {
    wsSendReply(req, "{\"type\":\"error\",\"request\":\"\",\"error\":\"missing cmd\"}");
    return;
  }

  if (strcmp(command, "config") == 0)
  {
    char* buffer = malloc(WEB_CONFIG_JSON_BYTES);

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

  if (strcmp(command, "power") == 0)
  {
    char* buffer = malloc(WEB_POWER_JSON_BYTES);

    if (buffer == NULL)
    {
      wsSendReply(req, "{\"type\":\"error\",\"request\":\"power\",\"error\":\"out of memory\"}");
      return;
    }

    stringBuilder sb;
    sbInit(&sb, buffer, WEB_POWER_JSON_BYTES);
    appendPowerJson(&sb);
    wsSendReply(req, buffer);
    free(buffer);
    return;
  }

  if (strcmp(command, "ringfile") == 0)
  {
    loggerMeterRing_t ring = LOGGER_RING_HOURS;

    if (!ringFromParameters(body, &ring))
    {
      wsSendReply(req, "{\"type\":\"error\",\"request\":\"ringfile\",\"error\":\"unknown ring\"}");
      return;
    }

    char* buffer = malloc(WEB_RINGFILE_JSON_BYTES);

    if (buffer == NULL)
    {
      wsSendReply(req, "{\"type\":\"error\",\"request\":\"ringfile\",\"error\":\"out of memory\"}");
      return;
    }

    stringBuilder sb;
    sbInit(&sb, buffer, WEB_RINGFILE_JSON_BYTES);
    appendRingFileJson(&sb, ring);

    if (sb.overflow)
    {
      ESP_LOGE(tag, "ring file json overflow");
    }

    wsSendReply(req, buffer);
    free(buffer);
    return;
  }

  if (strcmp(command, "graph") == 0)
  {
    //-- The usage series of one ring for the Hours, Days and Months Graph
    //-- views: the optional ring selector picks the ring (hours default).
    loggerMeterRing_t ring = LOGGER_RING_HOURS;

    if (!ringFromParameters(body, &ring))
    {
      wsSendReply(req, "{\"type\":\"error\",\"request\":\"graph\",\"error\":\"unknown ring\"}");
      return;
    }

    char* buffer = malloc(WEB_HOURSGRAPH_JSON_BYTES);

    if (buffer == NULL)
    {
      char oom[96];
      snprintf(oom, sizeof oom,
               "{\"type\":\"error\",\"request\":\"graph\",\"ring\":\"%s\","
               "\"error\":\"out of memory\"}",
               loggerStoreRingName(ring));
      wsSendReply(req, oom);
      return;
    }

    stringBuilder sb;
    sbInit(&sb, buffer, WEB_HOURSGRAPH_JSON_BYTES);
    appendPeriodGraphJson(&sb, ring);

    if (sb.overflow)
    {
      ESP_LOGE(tag, "period graph json overflow");
    }

    wsSendReply(req, buffer);
    free(buffer);
    return;
  }

  if (strcmp(command, "hoursgraph") == 0)
  {
    char* buffer = malloc(WEB_HOURSGRAPH_JSON_BYTES);

    if (buffer == NULL)
    {
      wsSendReply(req,
                  "{\"type\":\"error\",\"request\":\"hoursgraph\",\"error\":\"out of memory\"}");
      return;
    }

    stringBuilder sb;
    sbInit(&sb, buffer, WEB_HOURSGRAPH_JSON_BYTES);
    appendHoursGraphJson(&sb);

    if (sb.overflow)
    {
      ESP_LOGE(tag, "hours graph json overflow");
    }

    wsSendReply(req, buffer);
    free(buffer);
    return;
  }

  if (strcmp(command, "actualgraph") == 0)
  {
    char* buffer = malloc(WEB_ACTUALGRAPH_JSON_BYTES);

    if (buffer == NULL)
    {
      wsSendReply(req,
                  "{\"type\":\"error\",\"request\":\"actualgraph\",\"error\":\"out of memory\"}");
      return;
    }

    stringBuilder sb;
    sbInit(&sb, buffer, WEB_ACTUALGRAPH_JSON_BYTES);
    appendActualGraphJson(&sb);

    if (sb.overflow)
    {
      //-- A truncated frame is malformed JSON: the SPA could not parse it
      //-- and the view stayed blank. Answer a clean error instead, which the
      //-- SPA shows as the view's state line.
      ESP_LOGE(tag, "actual graph json overflow");
      wsSendReply(req, "{\"type\":\"error\",\"request\":\"actualgraph\","
                       "\"error\":\"frame too large\"}");
    }
    else
    {
      wsSendReply(req, buffer);
    }

    free(buffer);
    return;
  }

  if (strcmp(command, "telegrams") == 0)
  {
    char* buffer = malloc(WEB_LIST_JSON_BYTES);

    if (buffer == NULL)
    {
      wsSendReply(req,
                  "{\"type\":\"error\",\"request\":\"telegrams\",\"error\":\"out of memory\"}");
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
      loggerTelegramRecord* record = malloc(sizeof *record);

      if (record == NULL)
      {
        wsSendReply(req, "{\"type\":\"error\",\"request\":\"raw\",\"error\":\"out of memory\"}");
        return;
      }

      if (!loggerReceiverGetRecord(sequence, record))
      {
        free(record);
        wsSendReply(req,
                    "{\"type\":\"error\",\"request\":\"raw\",\"seq\":0,\"error\":\"aged out\"}");
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

    char* json = malloc(WEB_DETAIL_JSON_BYTES);

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
    const char* failure = applySettingsFromForm(body);

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
    char* buffer = malloc(WEB_CONFIG_JSON_BYTES);

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
  snprintf(reply, sizeof reply,
           "{\"type\":\"error\",\"request\":\"%s\",\"error\":\"unknown command\"}", command);
  wsSendReply(req, reply);
}

static esp_err_t handleWebSocket(httpd_req_t* req)
{
  //-- ESP-IDF 6.0 answers the handshake inside httpd (httpd_uri.c returns
  //-- without calling the handler), so this handler is only ever entered for
  //-- data frames: that is where a client must be registered for pushes.
  //-- wsClientAdd ignores sockets it already knows, so calling it on every
  //-- frame is cheap and cannot miss a client.
  int clientFd = httpd_req_to_sockfd(req);
  wsClientAdd(clientFd);

  //-- Single-browser rule: the reporting of a session claims the web
  //-- control slot. The arbiter evicts the previous web client (popup)
  //-- through its registered callback; the telnet console owns a separate
  //-- slot and is never touched from here. For the current owner this is a
  //-- cheap no-op, same per-frame check as the reference project uses.
  loggerControlClaim(LOGGER_CONTROL_WEB, clientFd);

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
      char* payload = malloc(frame.len + 1u);

      if (payload != NULL)
      {
        frame.payload = (uint8_t*)payload;
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
      char* payload = malloc(frame.len + 1u);

      if (payload != NULL)
      {
        frame.payload = (uint8_t*)payload;
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

  char* body = calloc(1u, frame.len + 1u);

  if (body == NULL)
  {
    wsSendReply(req, "{\"type\":\"error\",\"request\":\"\",\"error\":\"out of memory\"}");
    return ESP_ERR_NO_MEM;
  }

  frame.payload = (uint8_t*)body;
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
static void wsPushTaskLoop(void* argument)
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
      char* list = malloc(WEB_LIST_JSON_BYTES);

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

    //-- Realtime power: push the panel frame of the newest accepted telegram
    //-- so the SPA diagram refreshes the moment a telegram arrives — every
    //-- interval when the interval is short, while the SPA's own timer keeps
    //-- it at least every five seconds regardless.
    loggerPowerSnapshot power;
    uint32_t powerUptimeMs = 0u;

    if (loggerReceiverGetPowerSnapshot(&power, &powerUptimeMs) &&
        (power.sequence != wsPushedPowerSequence))
    {
      char* powerJson = malloc(WEB_POWER_JSON_BYTES);

      if (powerJson != NULL)
      {
        stringBuilder powerSb;
        sbInit(&powerSb, powerJson, WEB_POWER_JSON_BYTES);
        appendPowerJson(&powerSb);

        if (!powerSb.overflow)
        {
          wsSendToAll(powerJson);
          wsPushedPowerSequence = power.sequence;
        }

        free(powerJson);
      }
    }
  }
}

//-- Clears the /ws registration and the web control slot when a socket really
//-- closes, so a reconnect after an ordinary disconnect is never locked out
//-- by a stale owner entry (only an actual close clears it — the reference
//-- project's pitfall: a reconnect must not look like a takeover). The
//-- close_fn contract: this function owns the close() call.
static void webCloseFn(httpd_handle_t handle, int sockfd)
{
  (void)handle;
  wsClientRemove(sockfd);
  loggerControlRelease(LOGGER_CONTROL_WEB, sockfd);
  close(sockfd);
}

//-- --------------------------------------------------------------------------
//-- Server lifecycle
//-- --------------------------------------------------------------------------

static esp_err_t registerUri(httpd_handle_t handle, const char* uri, httpd_method_t method,
                             bool isWebSocket, esp_err_t (*handler)(httpd_req_t*))
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

  //-- Room for every URI below with headroom: the assets, the v2 API, the
  //-- /api/v3 data endpoints and the single /ws handler.
  config.max_uri_handlers = 32;

  //-- The per-sequence endpoints carry a trailing wildcard (/api/telegram/*,
  //-- /api/v3/telegram/*, /api/v3/raw/*): esp_http_server honours those only
  //-- once the documented wildcard matcher is selected — the default matcher
  //-- is a plain string compare, which never matches a wildcarded URI.
  config.uri_match_fn = httpd_uri_match_wildcard;

  //-- A single page load opens roughly five concurrent sockets; the default
  //-- of four was already too tight for one browser (see the sizing note in
  //-- sdkconfig.defaults). The telnet console and SNTP share the LWIP pool.
  config.max_open_sockets = 8;

  //-- WebSocket command handling builds the telegram detail JSON (nested
  //-- string builders plus vsnprintf) on the httpd task stack: the 4 KB
  //-- default overflows there, which silently killed the whole web server
  //-- (dropped telegrams and details, blank panes). The Show RingFile answer
  //-- also drives ring_file_store from this task, and the component
  //-- documents at least 8 KB of stack for any task using it; 10 KB covers
  //-- both paths with headroom.
  config.stack_size = 10240;

  //-- WebSocket sessions stay open for the life of the page; an LRU purge
  //-- could evict them to make room for ordinary requests.
  config.lru_purge_enable = false;

  //-- Clears the web control slot and the /ws table when a socket closes.
  config.close_fn = webCloseFn;

  esp_err_t result = httpd_start(&server, &config);

  if (result != ESP_OK)
  {
    ESP_LOGE(tag, "starting http server failed: %s", esp_err_to_name(result));
    return result;
  }

  wsClientsInit();
  wsPushedAcceptedCount = 0u;
  wsPushedPowerSequence = 0u;
  loggerControlRegisterEvict(LOGGER_CONTROL_WEB, webEvictCallback);

  registerUri(server, "/", HTTP_GET, false, handleRoot);
  registerUri(server, "/app.js", HTTP_GET, false, handleAppJs);
  registerUri(server, "/style.css", HTTP_GET, false, handleStyleCss);
  registerUri(server, "/favicon.ico", HTTP_GET, false, handleFavicon);

  //-- The whole API lives under /api/v3/: everything the console and the
  //-- SPA can request is a plain GET here (the two SPA actions are POSTs).
  //-- There is deliberately no unversioned /api/ surface: an API version 4
  //-- can then introduce its own syntax without an unversioned layer beside
  //-- it. The frames are URI-agnostic, so several v3 routes re-enter the
  //-- shared builders' handlers.
  registerUri(server, "/api/v3", HTTP_GET, false, handleApiV3IndexGet);
  registerUri(server, "/api/v3/config", HTTP_GET, false, handleConfigGet);
  registerUri(server, "/api/v3/config", HTTP_POST, false, handleConfigSave);
  registerUri(server, "/api/v3/status", HTTP_GET, false, handleApiV3StatusGet);
  registerUri(server, "/api/v3/version", HTTP_GET, false, handleApiV3VersionGet);
  registerUri(server, "/api/v3/device", HTTP_GET, false, handleApiV3DeviceGet);
  registerUri(server, "/api/v3/telegrams", HTTP_GET, false, handleTelegramsGet);
  registerUri(server, "/api/v3/telegram", HTTP_GET, false, handleApiV3TelegramGet);
  registerUri(server, "/api/v3/telegram/*", HTTP_GET, false, handleApiV3TelegramGet);
  registerUri(server, "/api/v3/raw", HTTP_GET, false, handleApiV3RawGet);
  registerUri(server, "/api/v3/raw/*", HTTP_GET, false, handleApiV3RawGet);
  registerUri(server, "/api/v3/power", HTTP_GET, false, handlePowerGet);
  registerUri(server, "/api/v3/ringfile", HTTP_GET, false, handleRingFileGet);
  registerUri(server, "/api/v3/graph", HTTP_GET, false, handleGraphGet);
  registerUri(server, "/api/v3/actualgraph", HTTP_GET, false, handleActualGraphGet);
  registerUri(server, "/api/v3/store", HTTP_GET, false, handleApiV3StoreGet);
  registerUri(server, "/api/v3/csv", HTTP_GET, false, handleApiV3CsvGet);
  registerUri(server, "/api/v3/view", HTTP_POST, false, handleViewReport);

  registerUri(server, "/ws", HTTP_GET, true, handleWebSocket);

  if (wsPushTask == NULL)
  {
    BaseType_t created = xTaskCreate(wsPushTaskLoop, "wsPush", WS_PUSH_TASK_STACK_BYTES, NULL,
                                     WS_PUSH_TASK_PRIORITY, &wsPushTask);

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

void loggerWebSetSettingsSavedCallback(loggerWebSettingsSavedCb callback)
{
  settingsSavedCallback = callback;
}
