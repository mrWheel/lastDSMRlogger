//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_settings.c - versioned settings service
//-- Date: 2026-11-15 · Build: 1
//--
//-- The complete configuration is stored as one versioned blob so a save is
//-- atomic. A stored blob that fails validation is treated as absent: the
//-- logger then awaits a valid configuration instead of using partial data.
//-- ==========================================================================

#include "logger_settings.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *tag = "loggerSettings";

#define NVS_NAMESPACE "dsmrlog"
#define NVS_KEY_CONFIG "cfg"

//-- Selectable P1 RX input pins on the board J5 connector.
const int loggerP1GpioChoices[LOGGER_P1_GPIO_COUNT] = {10, 38, 39, 40, 41, 42, 47, 48};

//-- Human readable protocol version labels for the built-in profiles. The
//-- stable profile identifier is what gets persisted.
typedef struct
{
  const char *profileId;
  const char *versionLabel;
} profileLabelEntry;

static const profileLabelEntry profileLabels[] =
{
  {"nl.dsmr.4.0", "DSMR 4.0"},
  {"nl.dsmr.4.2", "DSMR 4.2"},
  {"nl.dsmr.5.0", "DSMR 5.0"},
  {"nl.dsmr.4", "DSMR 4.0/4.2 (auto select)"},
  {"be.emucs.1.x", "eMUCs 1.x"},
  {"be.emucs.2.1", "eMUCs 2.1"},
};

//-- Copies src into dst, truncating instead of overflowing.
static void copyBounded(char *dst, size_t dstSize, const char *src)
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
static bool countryEquals(const char *left, const char *right)
{
  if ((strlen(left) != 2u) || (strlen(right) != 2u))
  {
    return false;
  }

  return (toUpperChar(left[0]) == toUpperChar(right[0])) && (toUpperChar(left[1]) == toUpperChar(right[1]));
}

bool loggerSettingsIsSelectableGpio(int gpio)
{
  for (uint8_t index = 0; index < LOGGER_P1_GPIO_COUNT; index++)
  {
    if (loggerP1GpioChoices[index] == gpio)
    {
      return true;
    }
  }

  return false;
}

bool loggerSettingsIsTestGpio(int gpio)
{
  return gpio == LOGGER_RX_GPIO_TEST;
}

uint8_t loggerSettingsBuildMbusDevices(const loggerSettingsConfig *config, dsmrMbusDevice *outDevices, uint8_t deviceCapacity)
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

bool loggerSettingsValidate(const loggerSettingsConfig *config)
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

  //-- The RX selection must be an eligible J5 input or the internal test
  //-- data generator (-1). Validation is identical for SPA and API input.
  if (!loggerSettingsIsSelectableGpio(config->rxGpio) && !loggerSettingsIsTestGpio(config->rxGpio))
  {
    ESP_LOGW(tag, "rejecting RX GPIO %d: neither on the board allowlist nor the test selection", config->rxGpio);
    return false;
  }

  if ((config->profileId[0] == '\0') || (config->countryCode[0] == '\0'))
  {
    return false;
  }

  const dsmrProfile *profile = dsmrProfileLookup(config->profileId);

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
    if ((config->mbusMode[channel] < LOGGER_MBUS_MODE_AUTO) || (config->mbusMode[channel] > LOGGER_MBUS_MODE_EXPECTED))
    {
      return false;
    }

    if ((config->mbusMode[channel] == LOGGER_MBUS_MODE_EXPECTED) && (config->mbusType[channel] > DSMR_MBUS_DEVICE_OTHER))
    {
      ESP_LOGW(tag, "rejecting unsupported M-Bus device type for channel %u", channel);
      return false;
    }
  }

  return true;
}

esp_err_t loggerSettingsLoad(loggerSettingsConfig *config)
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

  if ((result != ESP_OK) || (blobSize != sizeof(loggerSettingsConfig)))
  {
    nvs_close(handle);

    if (result == ESP_ERR_NVS_NOT_FOUND)
    {
      ESP_LOGI(tag, "no stored settings yet, awaiting configuration");
      return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGW(tag, "stored settings have the wrong size %u, awaiting configuration", (unsigned)blobSize);
    return ESP_ERR_NOT_FOUND;
  }

  result = nvs_get_blob(handle, NVS_KEY_CONFIG, config, &blobSize);
  nvs_close(handle);

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

  ESP_LOGI(tag, "loaded settings: gpio %d, profile %s", config->rxGpio, config->profileId);
  return ESP_OK;
}

esp_err_t loggerSettingsSave(const loggerSettingsConfig *config)
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

  ESP_LOGI(tag, "stored settings: gpio %d, profile %s", config->rxGpio, config->profileId);
  return ESP_OK;
}

//-- --------------------------------------------------------------------------
//-- Valid choices from the parser profile registry
//-- --------------------------------------------------------------------------

uint16_t loggerSettingsProfileCount(void)
{
  const char *const *ids = dsmrProfileListIds();
  uint16_t count = 0u;

  while ((ids != NULL) && (ids[count] != NULL))
  {
    count++;
  }

  return count;
}

bool loggerSettingsProfileAt(uint16_t index, loggerSettingsProfileInfo *info)
{
  const char *const *ids = dsmrProfileListIds();

  if ((ids == NULL) || (info == NULL))
  {
    return false;
  }

  uint16_t position = 0u;
  const char *profileId = NULL;

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

bool loggerSettingsLookupProfile(const char *profileId, loggerSettingsProfileInfo *info)
{
  if ((profileId == NULL) || (info == NULL))
  {
    return false;
  }

  const dsmrProfile *profile = dsmrProfileLookup(profileId);

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

bool loggerSettingsIsKnownCountry(const char *countryCode)
{
  const char *const *ids = dsmrProfileListIds();

  if ((ids == NULL) || (countryCode == NULL))
  {
    return false;
  }

  for (uint16_t index = 0; ids[index] != NULL; index++)
  {
    const dsmrProfile *profile = dsmrProfileLookup(ids[index]);

    if ((profile != NULL) && countryEquals(countryCode, profile->countryCode))
    {
      return true;
    }
  }

  return false;
}
