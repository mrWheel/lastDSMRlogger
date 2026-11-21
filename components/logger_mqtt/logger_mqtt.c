//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_mqtt.c - MQTT publication of the latest accepted telegram
//-- Date: 2026-11-21 · Build: 5
//--
//-- One background task owns the esp_mqtt client and the broadcast cadence.
//-- It keeps a copy of the relevant settings behind a mutex; the settings
//-- view reconfigures publication live through loggerMqttApplyConfig(), and
//-- the application reports the station state through
//-- loggerMqttSetNetworkReady(). Neither ever touches the esp_mqtt client
//-- directly: they only update the wanted state, and the task brings the
//-- client up or down on its own tick. That keeps every client create,
//-- destroy and publish on one thread (joining the client from a settings
//-- save while the task publishes would deadlock the two mutexes).
//-- Publication is requested when the broker text is non-empty, the
//-- broadcast interval is above zero and the station has joined (a broker
//-- lookup before LWIP is up crashes the tcpip core). Every interval (and
//-- right after every broker connect) the task reads the most recently
//-- accepted telegram out of the receiver history and publishes it as one
//-- JSON document on "<topTopic>/telegram": {"actual":{...}} with one
//-- entry per decoded field. The entry name is the field's display label
//-- when it has one (an adjusted label wins over the profile's built-in
//-- default, as the parser resolves it) and the OBIS code when it has
//-- none; the entry value is the parsed value text.
//-- ==========================================================================

#include "logger_mqtt.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "logger_receiver.h"
#include "mqtt_client.h"

static const char* tag = "loggerMqtt";

#define MQTT_TASK_STACK_BYTES 8192
#define MQTT_TASK_PRIORITY 4
#define MQTT_TICK_MS 1000u
#define MQTT_TOPIC_SUFFIX "/telegram"
#define MQTT_TOPIC_DEFAULT "lastdsmrlogger"
#define MQTT_URI_BYTES 192

//-- Budget for the publish document. A byte can grow to six characters
//-- when it has to escape as \u00xx, so the name and the value of a
//-- field are budgeted at six times their snapshot maxima; the field
//-- budget also carries the JSON scaffolding around one entry. The
//-- document is malloc-ed at this bound so a maximum-sized telegram
//-- always fits whole (truncating JSON would hand subscribers a broken
//-- document).
#define MQTT_FIELD_BUDGET_BYTES 700u
#define MQTT_DOC_BASE_BYTES 512u

//-- The parts of the configuration the client is built from; a change in
//-- any of them (re)connects, the other parts only steer the cadence.
typedef struct
{
  char broker[LOGGER_MQTT_BROKER_MAX];
  uint16_t port;
  char user[LOGGER_MQTT_USER_MAX];
  char password[LOGGER_MQTT_PASSWORD_MAX];
  char topTopic[LOGGER_MQTT_TOPIC_MAX];
  uint32_t intervalSeconds;
  bool publishNow;
} MqttSettings;

//-- Where buildTelegramJson writes: the heap buffer and its capacity.
typedef struct
{
  char* json;
  size_t size;
} JsonTarget;

//-- Protected by settingsMutex: the wanted state, written by the settings
//-- view and the application's Wi-Fi callbacks and read by the task.
static MqttSettings settings;
static bool networkReady = false;
static bool clientShouldRun = false;
static bool clientPendingRestart = false;
static SemaphoreHandle_t settingsMutex;

//-- Owned by the MQTT task and protected by clientMutex. No esp_mqtt call
//-- is ever made while settingsMutex is held, and neither mutex is ever
//-- held across the other: the task can always finish its current work.
static esp_mqtt_client_handle_t client;
static SemaphoreHandle_t clientMutex;
static bool taskStarted = false;

static void mqttTask(void* argument);

static bool publicationRequested(void)
{
  return (settings.broker[0] != '\0') && (settings.intervalSeconds > 0u);
}

//-- Recomputes the wanted client state from the settings and the station.
//-- The caller holds settingsMutex.
static void refreshWantedState(void)
{
  clientShouldRun = publicationRequested() && networkReady;
}

//-- The esp_event handler the mqtt component dispatches through: the event
//-- data is the esp_mqtt_event_t of the published event.
static void mqttEventHandler(void* argument, esp_event_base_t eventBase, int32_t eventId,
                             void* eventData)
{
  (void)argument;
  (void)eventBase;
  (void)eventData;

  if (eventId == MQTT_EVENT_CONNECTED)
  {
    ESP_LOGI(tag, "connected to the broker");

    //-- A fresh connection broadcasts at once: a subscriber that just
    //-- appeared sees the latest telegram without waiting a whole interval.
    xSemaphoreTake(settingsMutex, portMAX_DELAY);
    settings.publishNow = true;
    xSemaphoreGive(settingsMutex);
  }
  else if (eventId == MQTT_EVENT_DISCONNECTED)
  {
    ESP_LOGW(tag, "disconnected from the broker (retrying on its own)");
  }
  else if (eventId == MQTT_EVENT_ERROR)
  {
    ESP_LOGW(tag, "broker connection error");
  }
}

//-- Builds the broker URI: a full mqtt:// or mqtts:// text is taken as
//-- given, a bare host gets the mqtt scheme and the configured port.
static void buildBrokerUri(const MqttSettings* source, char* outUri, size_t outSize)
{
  if (strstr(source->broker, "://") != NULL)
  {
    snprintf(outUri, outSize, "%s", source->broker);
    return;
  }

  uint16_t port = (source->port != 0u) ? source->port : (uint16_t)LOGGER_SETTINGS_MQTT_PORT_DEFAULT;
  snprintf(outUri, outSize, "mqtt://%s:%u", source->broker, (unsigned)port);
}

//-- Creates and starts the client for the given settings. The caller holds
//-- clientMutex.
static esp_err_t startClient(const MqttSettings* source)
{
  char uri[MQTT_URI_BYTES];
  buildBrokerUri(source, uri, sizeof uri);

  esp_mqtt_client_config_t config;
  memset(&config, 0, sizeof config);
  config.broker.address.uri = uri;
  config.credentials.username = (source->user[0] != '\0') ? source->user : NULL;
  config.credentials.authentication.password = (source->user[0] != '\0') ? source->password : NULL;

  esp_mqtt_client_handle_t created = esp_mqtt_client_init(&config);

  if (created == NULL)
  {
    ESP_LOGE(tag, "could not create the MQTT client");
    return ESP_ERR_NO_MEM;
  }

  esp_mqtt_client_register_event(created, ESP_EVENT_ANY_ID, mqttEventHandler, NULL);
  esp_err_t started = esp_mqtt_client_start(created);

  if (started != ESP_OK)
  {
    ESP_LOGE(tag, "MQTT client could not start: %s", esp_err_to_name(started));
    esp_mqtt_client_destroy(created);
    return started;
  }

  client = created;
  ESP_LOGI(tag, "MQTT client started for %s", uri);
  return ESP_OK;
}

//-- Destroys the client. The caller holds clientMutex.
static void stopClient(void)
{
  if (client != NULL)
  {
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    client = NULL;
  }
}

//-- Appends formatted text. snprintf reports what it *would* have written,
//-- so a truncation must never move the cursor: the length is poisoned to
//-- the size and every later append becomes a no-op. The builder checks the
//-- poison and refuses to publish a half-built document.
static void appendPrintf(JsonTarget* target, size_t* length, const char* format, ...)
{
  if (*length + 1u >= target->size)
  {
    return;
  }

  va_list args;
  va_start(args, format);
  int written = vsnprintf(target->json + *length, target->size - *length, format, args);
  va_end(args);

  if ((written < 0) || ((size_t)written >= (target->size - *length)))
  {
    *length = target->size;
    return;
  }

  *length += (size_t)written;
}

//-- Appends count bytes as a JSON string (quotes, backslashes and control
//-- characters escaped; the raw frame's CR and LF become \r and \n). The
//-- raw frame is not NUL-terminated in the record, so this takes a pointer
//-- and a length and needs no stack copy of its own.
static void appendJsonChars(JsonTarget* target, size_t* length, const char* data, size_t count)
{
  if ((data == NULL) || (*length + 2u >= target->size))
  {
    *length = target->size;
    return;
  }

  target->json[(*length)++] = '"';

  for (size_t index = 0u; (index < count) && (*length + 7u < target->size); index++)
  {
    unsigned char character = (unsigned char)data[index];

    if ((character == '"') || (character == '\\'))
    {
      target->json[(*length)++] = '\\';
      target->json[(*length)++] = (char)character;
    }
    else if (character == '\r')
    {
      target->json[(*length)++] = '\\';
      target->json[(*length)++] = 'r';
    }
    else if (character == '\n')
    {
      target->json[(*length)++] = '\\';
      target->json[(*length)++] = 'n';
    }
    else if (character == '\t')
    {
      target->json[(*length)++] = '\\';
      target->json[(*length)++] = 't';
    }
    else if (character < 0x20u)
    {
      appendPrintf(target, length, "\\u%04x", (unsigned)character);

      if (*length >= target->size)
      {
        return;
      }
    }
    else
    {
      target->json[(*length)++] = (char)character;
    }
  }

  if (*length + 1u >= target->size)
  {
    //-- No room for the closing quote: the document would be invalid.
    *length = target->size;
    return;
  }

  target->json[(*length)++] = '"';
}

//-- Publication name of one decoded field: the parser already resolves the
//-- display name (an application-adjusted label wins over the built-in
//-- default of the profile's field table, and a field with neither carries
//-- an empty name in the snapshot), so only the fallback to the OBIS code
//-- is made here.
static const char* fieldKeyName(const loggerFieldSnapshot* field)
{
  return (field->name[0] != '\0') ? field->name : field->obis;
}

//-- True when the value text spells a plain decimal number that is also a
//-- valid JSON number: an optional minus sign and digits with at most one
//-- decimal point, at least one digit, no leading zero before a multi-digit
//-- integer and no exponent. Such values are published as JSON numbers so
//-- subscribers can plot and compute them directly; every other text
//-- (timestamps, device ids, status words) stays a quoted JSON string.
static bool isNumericValue(const char* text)
{
  if (text[0] == '-')
  {
    text++;
  }

  size_t length = strlen(text);

  if (length == 0u)
  {
    return false;
  }

  //-- Long digit runs are ids, not measurements (an equipment id is 32
  //-- decimal digits): a JSON number of that size would lose precision in
  //-- any double-based parser, so they stay strings.
  if (length > 15u)
  {
    return false;
  }

  //-- JSON numbers start and end on a digit (no ".5", no "1.").
  if ((text[0] < '0') || (text[0] > '9') || (text[length - 1u] < '0') || (text[length - 1u] > '9'))
  {
    return false;
  }

  //-- A plain "0" or "0.x" is fine; "00" or "01.5" is not a JSON number.
  if ((text[0] == '0') && (length > 1u) && (text[1] != '.'))
  {
    return false;
  }

  bool hasDigit = false;
  bool hasDot = false;

  for (size_t index = 0u; index < length; index++)
  {
    if (text[index] == '.')
    {
      if (hasDot)
      {
        return false;
      }

      hasDot = true;
      continue;
    }

    if ((text[index] < '0') || (text[index] > '9'))
    {
      return false;
    }

    hasDigit = true;
  }

  return hasDigit;
}

//-- Inspector of the retained record: builds the JSON document while the
//-- receiver-side lock is held (read-only, no blocking, as the inspect
//-- contract demands). The document is {"actual":{...}} with one entry per
//-- decoded field: the display name (or the OBIS code when the field has
//-- no label) mapped onto the parsed value — a JSON number when the value
//-- text spells a decimal number, a JSON string in every other case.
static bool buildTelegramJson(const loggerTelegramRecord* record, void* userData)
{
  JsonTarget* target = (JsonTarget*)userData;
  size_t length = 0u;
  bool firstEntry = true;

  appendPrintf(target, &length, "{\"actual\":{");

  for (uint16_t index = 0u; (index < record->meta.fieldCount) && (length < target->size); index++)
  {
    const loggerFieldSnapshot* field = &record->fields[index];
    const char* key = fieldKeyName(field);

    if (key[0] == '\0')
    {
      //-- Neither a label nor a code: nothing to name the entry with.
      continue;
    }

    //-- Two fields sharing a display name would build an invalid object
    //-- (duplicate keys): the second one falls back to its OBIS code.
    for (uint16_t prior = 0u; prior < index; prior++)
    {
      if (strcmp(fieldKeyName(&record->fields[prior]), key) == 0)
      {
        key = field->obis;
        break;
      }
    }

    if (key[0] == '\0')
    {
      continue;
    }

    if (!firstEntry)
    {
      appendPrintf(target, &length, ",");
    }

    firstEntry = false;
    appendJsonChars(target, &length, key, strlen(key));
    appendPrintf(target, &length, ":");

    if (isNumericValue(field->value))
    {
      appendPrintf(target, &length, "%s", field->value);
    }
    else
    {
      appendJsonChars(target, &length, field->value, strlen(field->value));
    }
  }

  appendPrintf(target, &length, "}}");

  //-- The poison means a part did not fit: never publish half a document.
  return (length < target->size);
}

//-- Publishes the most recently accepted telegram once. False when there is
//-- nothing to publish or the broker is not reachable. The caller holds a
//-- settings copy; only the client handle is shared, under clientMutex.
static bool publishLatestTelegram(const MqttSettings* source)
{
  loggerTelegramMeta meta;

  if (loggerReceiverListRecords(&meta, 1u) != 1u)
  {
    return false;
  }

  //-- Size the document at the worst-case bound of this very telegram:
  //-- every field at its budget (name and value fully escaped, plus the
  //-- JSON scaffolding around the entry). It always fits, so no build is
  //-- ever truncated. (The old fixed 16 KB buffer could overflow on a
  //-- full-sized frame: snprintf reports what it *would* have written, the
  //-- cursor ran past the buffer and wrote over the heap.)
  size_t needed = MQTT_DOC_BASE_BYTES + (size_t)meta.fieldCount * MQTT_FIELD_BUDGET_BYTES;
  char* json = malloc(needed);

  if (json == NULL)
  {
    ESP_LOGW(tag, "out of memory for the MQTT telegram document");
    return false;
  }

  JsonTarget target = {json, needed};
  bool built = loggerReceiverInspectRecord(meta.sequence, buildTelegramJson, &target);
  bool published = false;

  if (built)
  {
    char topic[LOGGER_MQTT_TOPIC_MAX + sizeof(MQTT_TOPIC_SUFFIX)];
    snprintf(topic, sizeof topic, "%s%s",
             (source->topTopic[0] != '\0') ? source->topTopic : MQTT_TOPIC_DEFAULT,
             MQTT_TOPIC_SUFFIX);

    xSemaphoreTake(clientMutex, portMAX_DELAY);
    published = ((client != NULL) && (esp_mqtt_client_publish(client, topic, json, -1, 0, 0) >= 0));
    xSemaphoreGive(clientMutex);

    if (published)
    {
      ESP_LOGI(tag, "published telegram #%" PRIu32 " to %s", meta.sequence, topic);
    }
    else
    {
      ESP_LOGW(tag, "could not publish telegram #%" PRIu32 " to %s", meta.sequence, topic);
    }
  }

  free(json);
  return published;
}

//-- Brings the client up or down to the wanted state and runs one publish
//-- round when the interval expired. The task is the only place the client
//-- lives: saves and Wi-Fi callbacks merely flip the wanted flags.
static void mqttTask(void* argument)
{
  (void)argument;

  uint32_t sinceBroadcastMs = 0u;

  for (;;)
  {
    vTaskDelay(pdMS_TO_TICKS(MQTT_TICK_MS));

    xSemaphoreTake(settingsMutex, portMAX_DELAY);
    MqttSettings copy = settings;
    bool publishNow = settings.publishNow;
    settings.publishNow = false;
    bool shouldRun = clientShouldRun;
    bool restart = clientPendingRestart;
    clientPendingRestart = false;
    xSemaphoreGive(settingsMutex);

    xSemaphoreTake(clientMutex, portMAX_DELAY);

    if (!shouldRun)
    {
      stopClient();
    }
    else if ((client == NULL) || restart)
    {
      stopClient();

      if (startClient(&copy) != ESP_OK)
      {
        //-- Stay with the client down; the next tick retries.
        shouldRun = false;
      }
    }

    xSemaphoreGive(clientMutex);

    if (!shouldRun)
    {
      sinceBroadcastMs = 0u;
      continue;
    }

    sinceBroadcastMs += MQTT_TICK_MS;

    if ((sinceBroadcastMs >= (copy.intervalSeconds * 1000u)) || publishNow)
    {
      sinceBroadcastMs = 0u;
      publishLatestTelegram(&copy);
    }
  }
}

static bool ensureMutexes(void)
{
  if ((settingsMutex != NULL) && (clientMutex != NULL))
  {
    return true;
  }

  if (settingsMutex == NULL)
  {
    settingsMutex = xSemaphoreCreateMutex();
  }

  if (clientMutex == NULL)
  {
    clientMutex = xSemaphoreCreateMutex();
  }

  return (settingsMutex != NULL) && (clientMutex != NULL);
}

esp_err_t loggerMqttApplyConfig(const loggerSettingsConfig* config)
{
  if (config == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  if (!ensureMutexes())
  {
    return ESP_ERR_NO_MEM;
  }

  xSemaphoreTake(settingsMutex, portMAX_DELAY);

  bool reconnect = (strcmp(settings.broker, config->mqttBroker) != 0) ||
                   (settings.port != config->mqttPort) ||
                   (strcmp(settings.user, config->mqttUser) != 0) ||
                   (strcmp(settings.password, config->mqttPassword) != 0);

  MqttSettings updated;
  memset(&updated, 0, sizeof updated);
  snprintf(updated.broker, sizeof updated.broker, "%s", config->mqttBroker);
  updated.port = config->mqttPort;
  snprintf(updated.user, sizeof updated.user, "%s", config->mqttUser);
  snprintf(updated.password, sizeof updated.password, "%s", config->mqttPassword);
  snprintf(updated.topTopic, sizeof updated.topTopic, "%s", config->mqttTopTopic);
  updated.intervalSeconds = config->mqttIntervalSeconds;
  updated.publishNow = settings.publishNow;
  settings = updated;

  refreshWantedState();

  if (reconnect && clientShouldRun)
  {
    clientPendingRestart = true;
  }

  esp_err_t result = ESP_OK;

  if (!taskStarted)
  {
    BaseType_t created =
        xTaskCreate(mqttTask, "loggerMqtt", MQTT_TASK_STACK_BYTES, NULL, MQTT_TASK_PRIORITY, NULL);

    if (created != pdPASS)
    {
      ESP_LOGE(tag, "could not create the MQTT task");
      result = ESP_ERR_NO_MEM;
    }
    else
    {
      taskStarted = true;
    }
  }

  xSemaphoreGive(settingsMutex);
  return result;
}

void loggerMqttSetNetworkReady(bool ready)
{
  if (!ensureMutexes())
  {
    networkReady = ready;
    return;
  }

  xSemaphoreTake(settingsMutex, portMAX_DELAY);
  networkReady = ready;
  refreshWantedState();
  xSemaphoreGive(settingsMutex);
}

bool loggerMqttIsConnected(void)
{
  if (!ensureMutexes())
  {
    return false;
  }

  xSemaphoreTake(settingsMutex, portMAX_DELAY);
  bool requested = clientShouldRun;
  xSemaphoreGive(settingsMutex);

  if (!requested)
  {
    return false;
  }

  xSemaphoreTake(clientMutex, portMAX_DELAY);
  bool connected =
      (client != NULL) && (esp_mqtt_client_get_state(client) == MQTT_CLIENT_STATE_CONNECTED);
  xSemaphoreGive(clientMutex);

  return connected;
}
