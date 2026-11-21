//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_settings.c - versioned settings service
//-- Date: 2026-11-21 · Build: 6
//--
//-- The complete configuration is stored as one versioned blob so a save is
//-- atomic. A stored blob that fails validation is treated as absent: the
//-- logger then awaits a valid configuration instead of using partial data.
//-- ==========================================================================

#include "logger_settings.h"

#include <ctype.h>
#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* tag = "loggerSettings";

#define NVS_NAMESPACE "dsmrlog"
#define NVS_KEY_CONFIG "cfg"

//-- Layout of the stored blob before the MQTT and network fields were
//-- appended at version 5. A stored blob of exactly this size is migrated:
//-- its fields land in the identical prefix of the current struct and the
//-- appended fields get their defaults.
typedef struct
{
  uint32_t structVersion;
  int rxGpio;
  bool rxInverted;
  bool txInverted;
  uint32_t intervalSeconds;
  char countryCode[LOGGER_SETTINGS_COUNTRY_MAX];
  char profileId[LOGGER_SETTINGS_PROFILE_ID_MAX];
  loggerMbusMode mbusMode[DSMR_MBUS_CHANNEL_MAX + 1];
  dsmrMbusDeviceType mbusType[DSMR_MBUS_CHANNEL_MAX + 1];
  bool displayFlipped;
} loggerSettingsConfigV5;

//-- Human readable protocol version labels for the built-in profiles. The
//-- stable profile identifier is what gets persisted.
typedef struct
{
  const char* profileId;
  const char* versionLabel;
} profileLabelEntry;

static const profileLabelEntry profileLabels[] = {
    {"nl.dsmr.4.0", "DSMR 4.0"},   {"nl.dsmr.4.2", "DSMR 4.2"},
    {"nl.dsmr.5.0", "DSMR 5.0"},   {"nl.dsmr.4", "DSMR 4.0/4.2 (auto select)"},
    {"be.emucs.1.x", "eMUCs 1.x"}, {"be.emucs.2.1", "eMUCs 2.1"},
};

//-- Copies src into dst, truncating instead of overflowing.
static void copyBounded(char* dst, size_t dstSize, const char* src)
{
  size_t length = strlen(src);

  if (length >= dstSize)
  {
    length = dstSize - 1u;
  }

  memcpy(dst, src, length);
  dst[length] = '\0';
}

static char toUpperChar(char character)
{
  if ((character >= 'a') && (character <= 'z'))
  {
    return (char)(character - ('a' - 'A'));
  }

  return character;
}

//-- True when both two letter codes match case insensitively.
static bool countryEquals(const char* left, const char* right)
{
  if ((strlen(left) != 2u) || (strlen(right) != 2u))
  {
    return false;
  }

  return (toUpperChar(left[0]) == toUpperChar(right[0])) &&
         (toUpperChar(left[1]) == toUpperChar(right[1]));
}

//-- True when the text is empty or holds only printable ASCII without
//-- spaces: the shape of every MQTT broker, user, password and topic field.
static bool plainTextOk(const char* text)
{
  for (const char* scan = text; *scan != '\0'; scan++)
  {
    if ((*scan < 0x21) || (*scan > 0x7E))
    {
      return false;
    }
  }

  return true;
}

//-- True when the text is a usable hostname: empty (device default) or a
//-- lowercase DNS label of letters, digits and dashes, starting and ending
//-- on an alphanumeric and at most LOGGER_HOSTNAME_MAX - 1 characters long.
static bool hostnameOk(const char* text)
{
  size_t length = strlen(text);

  if (length == 0u)
  {
    return true;
  }

  if ((length > (LOGGER_HOSTNAME_MAX - 1u)) || (!isalnum((unsigned char)text[0])) ||
      (!isalnum((unsigned char)text[length - 1u])))
  {
    return false;
  }

  for (size_t index = 0u; index < length; index++)
  {
    char character = text[index];

    if (!islower((unsigned char)character) && !isdigit((unsigned char)character) &&
        (character != '-'))
    {
      return false;
    }
  }

  return true;
}

bool loggerSettingsIsLiveGpio(int gpio)
{
  return gpio == LOGGER_RX_GPIO_LIVE;
}

bool loggerSettingsIsTestGpio(int gpio)
{
  return gpio == LOGGER_RX_GPIO_TEST;
}

uint8_t loggerSettingsBuildMbusDevices(const loggerSettingsConfig* config,
                                       dsmrMbusDevice* outDevices, uint8_t deviceCapacity)
{
  if ((config == NULL) || (outDevices == NULL))
  {
    return 0u;
  }

  uint8_t count = 0u;

  for (uint8_t channel = DSMR_MBUS_CHANNEL_MIN; channel <= DSMR_MBUS_CHANNEL_MAX; channel++)
  {
    if (config->mbusMode[channel] != LOGGER_MBUS_MODE_EXPECTED)
    {
      continue;
    }

    if (count >= deviceCapacity)
    {
      break;
    }

    outDevices[count].channel = channel;
    outDevices[count].deviceType = config->mbusType[channel];
    count++;
  }

  return count;
}

bool loggerSettingsValidate(const loggerSettingsConfig* config)
{
  if (config == NULL)
  {
    return false;
  }

  if (config->structVersion != LOGGER_SETTINGS_VERSION)
  {
    ESP_LOGW(tag, "rejecting configuration with struct version %" PRIu32, config->structVersion);
    return false;
  }

  //-- The data source is either the fixed live P1 input on UART0 (RX GPIO44)
  //-- or the internal test data generator (-1). Validation is identical for
  //-- SPA and API input.
  if (!loggerSettingsIsLiveGpio(config->rxGpio) && !loggerSettingsIsTestGpio(config->rxGpio))
  {
    ESP_LOGW(tag, "rejecting RX GPIO %d: neither the live UART0 pin nor the test selection",
             config->rxGpio);
    return false;
  }

  //-- One telegram is read per interval; the interval must be within the
  //-- documented bounds.
  if ((config->intervalSeconds < LOGGER_SETTINGS_INTERVAL_MIN_S) ||
      (config->intervalSeconds > LOGGER_SETTINGS_INTERVAL_MAX_S))
  {
    ESP_LOGW(tag, "rejecting interval %u s: outside %u..%u s", (unsigned)config->intervalSeconds,
             (unsigned)LOGGER_SETTINGS_INTERVAL_MIN_S, (unsigned)LOGGER_SETTINGS_INTERVAL_MAX_S);
    return false;
  }

  if ((config->profileId[0] == '\0') || (config->countryCode[0] == '\0'))
  {
    return false;
  }

  const dsmrProfile* profile = dsmrProfileLookup(config->profileId);

  if (profile == NULL)
  {
    ESP_LOGW(tag, "rejecting unknown profile id %s", config->profileId);
    return false;
  }

  //-- The country must match the profile so a mismatched pair is refused.
  if (!countryEquals(config->countryCode, profile->countryCode))
  {
    ESP_LOGW(tag, "rejecting country %s for profile %s", config->countryCode, config->profileId);
    return false;
  }

  for (uint8_t channel = DSMR_MBUS_CHANNEL_MIN; channel <= DSMR_MBUS_CHANNEL_MAX; channel++)
  {
    if ((config->mbusMode[channel] < LOGGER_MBUS_MODE_AUTO) ||
        (config->mbusMode[channel] > LOGGER_MBUS_MODE_EXPECTED))
    {
      return false;
    }

    if ((config->mbusMode[channel] == LOGGER_MBUS_MODE_EXPECTED) &&
        (config->mbusType[channel] > DSMR_MBUS_DEVICE_OTHER))
    {
      ESP_LOGW(tag, "rejecting unsupported M-Bus device type for channel %u", channel);
      return false;
    }
  }

  //-- MQTT text fields: printable ASCII without spaces. The broker may be
  //-- empty (publication disabled); a non-empty broker must be a usable
  //-- host, IP or full mqtt:// URI.
  if (!plainTextOk(config->mqttBroker))
  {
    ESP_LOGW(tag, "rejecting MQTT broker text");
    return false;
  }

  if (!plainTextOk(config->mqttUser) || !plainTextOk(config->mqttPassword))
  {
    ESP_LOGW(tag, "rejecting MQTT credential text");
    return false;
  }

  //-- The topic additionally excludes the MQTT wildcards and the $ prefix
  //-- of the broker's own reserved topics.
  if (!plainTextOk(config->mqttTopTopic) || (strpbrk(config->mqttTopTopic, "+#$") != NULL))
  {
    ESP_LOGW(tag, "rejecting MQTT topic text");
    return false;
  }

  if (config->mqttIntervalSeconds > LOGGER_SETTINGS_MQTT_INTERVAL_MAX_S)
  {
    ESP_LOGW(tag, "rejecting MQTT interval %u s: above %u s", (unsigned)config->mqttIntervalSeconds,
             (unsigned)LOGGER_SETTINGS_MQTT_INTERVAL_MAX_S);
    return false;
  }

  //-- Hostname: empty selects the device default; otherwise a lowercase DNS
  //-- label of letters, digits and dashes, not starting or ending on a dash.
  if (!hostnameOk(config->hostname))
  {
    ESP_LOGW(tag, "rejecting hostname '%s'", config->hostname);
    return false;
  }

  if (config->fixedIpLastOctet > 254u)
  {
    ESP_LOGW(tag, "rejecting fixed IP last octet %u", (unsigned)config->fixedIpLastOctet);
    return false;
  }

  return true;
}

esp_err_t loggerSettingsLoad(loggerSettingsConfig* config)
{
  if (config == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t handle;
  esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);

  if (result != ESP_OK)
  {
    ESP_LOGI(tag, "no settings namespace yet: %s", esp_err_to_name(result));
    return ESP_ERR_NOT_FOUND;
  }

  size_t blobSize = 0u;
  result = nvs_get_blob(handle, NVS_KEY_CONFIG, NULL, &blobSize);

  //-- A blob of the current size is read as-is; a blob of the pre-extension
  //-- size migrates (the appended fields get their defaults below). Anything
  //-- else cannot be trusted.
  bool migrated = (blobSize == sizeof(loggerSettingsConfigV5));

  if ((result != ESP_OK) || ((blobSize != sizeof(loggerSettingsConfig)) && !migrated))
  {
    nvs_close(handle);

    if (result == ESP_ERR_NVS_NOT_FOUND)
    {
      ESP_LOGI(tag, "no stored settings yet, awaiting configuration");
      return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGW(tag, "stored settings have the wrong size %u, awaiting configuration",
             (unsigned)blobSize);
    return ESP_ERR_NOT_FOUND;
  }

  memset(config, 0, sizeof *config);
  result = nvs_get_blob(handle, NVS_KEY_CONFIG, config, &blobSize);
  nvs_close(handle);

  if (migrated && (result == ESP_OK))
  {
    config->mqttPort = LOGGER_SETTINGS_MQTT_PORT_DEFAULT;
    ESP_LOGI(tag, "stored settings migrated: MQTT and network fields take their defaults");
  }

  if (result != ESP_OK)
  {
    ESP_LOGW(tag, "reading stored settings failed: %s", esp_err_to_name(result));
    return ESP_ERR_NOT_FOUND;
  }

  if (!loggerSettingsValidate(config))
  {
    ESP_LOGW(tag, "stored settings failed validation, awaiting configuration");
    return ESP_ERR_NOT_FOUND;
  }

  ESP_LOGI(tag, "loaded settings: gpio %d, profile %s, rx polarity %s, tx polarity %s, display %s",
           config->rxGpio, config->profileId, config->rxInverted ? "inverted" : "normal",
           config->txInverted ? "inverted" : "normal",
           config->displayFlipped ? "upside down" : "normal");
  return ESP_OK;
}

esp_err_t loggerSettingsSave(const loggerSettingsConfig* config)
{
  if (!loggerSettingsValidate(config))
  {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t handle;
  esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);

  if (result != ESP_OK)
  {
    return result;
  }

  result = nvs_set_blob(handle, NVS_KEY_CONFIG, config, sizeof *config);

  if (result == ESP_OK)
  {
    result = nvs_commit(handle);
  }

  nvs_close(handle);

  if (result != ESP_OK)
  {
    ESP_LOGE(tag, "persisting settings failed: %s", esp_err_to_name(result));
    return result;
  }

  ESP_LOGI(tag, "stored settings: gpio %d, profile %s, rx polarity %s, tx polarity %s, display %s",
           config->rxGpio, config->profileId, config->rxInverted ? "inverted" : "normal",
           config->txInverted ? "inverted" : "normal",
           config->displayFlipped ? "upside down" : "normal");
  return ESP_OK;
}

//-- --------------------------------------------------------------------------
//-- Valid choices from the parser profile registry
//-- --------------------------------------------------------------------------

uint16_t loggerSettingsProfileCount(void)
{
  const char* const* ids = dsmrProfileListIds();
  uint16_t count = 0u;

  while ((ids != NULL) && (ids[count] != NULL))
  {
    count++;
  }

  return count;
}

bool loggerSettingsProfileAt(uint16_t index, loggerSettingsProfileInfo* info)
{
  const char* const* ids = dsmrProfileListIds();

  if ((ids == NULL) || (info == NULL))
  {
    return false;
  }

  uint16_t position = 0u;
  const char* profileId = NULL;

  while (ids[position] != NULL)
  {
    if (position == index)
    {
      profileId = ids[position];
      break;
    }

    position++;
  }

  if (profileId == NULL)
  {
    return false;
  }

  return loggerSettingsLookupProfile(profileId, info);
}

bool loggerSettingsLookupProfile(const char* profileId, loggerSettingsProfileInfo* info)
{
  if ((profileId == NULL) || (info == NULL))
  {
    return false;
  }

  const dsmrProfile* profile = dsmrProfileLookup(profileId);

  if (profile == NULL)
  {
    return false;
  }

  memset(info, 0, sizeof *info);
  copyBounded(info->profileId, sizeof info->profileId, profile->profileId);
  copyBounded(info->countryCode, sizeof info->countryCode, profile->countryCode);
  info->telegramIntervalMs = profile->telegramIntervalMs;

  info->versionLabel[0] = '\0';

  for (uint8_t index = 0; index < sizeof profileLabels / sizeof profileLabels[0]; index++)
  {
    if (strcmp(profileLabels[index].profileId, profile->profileId) == 0)
    {
      copyBounded(info->versionLabel, sizeof info->versionLabel, profileLabels[index].versionLabel);
      break;
    }
  }

  if (info->versionLabel[0] == '\0')
  {
    copyBounded(info->versionLabel, sizeof info->versionLabel, profile->profileId);
  }

  return true;
}

bool loggerSettingsIsKnownCountry(const char* countryCode)
{
  const char* const* ids = dsmrProfileListIds();

  if ((ids == NULL) || (countryCode == NULL))
  {
    return false;
  }

  for (uint16_t index = 0; ids[index] != NULL; index++)
  {
    const dsmrProfile* profile = dsmrProfileLookup(ids[index]);

    if ((profile != NULL) && countryEquals(countryCode, profile->countryCode))
    {
      return true;
    }
  }

  return false;
}
