//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_settings.c - versioned settings service
//-- Date: 2026-11-24 · Build: 14
//--
//-- The complete configuration is stored as one versioned blob so a save is
//-- atomic. A stored blob that fails validation is treated as absent: the
//-- logger then awaits a valid configuration instead of using partial data.
//-- ==========================================================================

#include "logger_settings.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "logger_control.h"
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

//-- Layout of the stored blob before the rfs synchronisation interval and
//-- the manual tariff prices were appended. A stored blob of exactly this
//-- size migrates like the one above: its fields land in the identical
//-- prefix of the current struct and the appended fields get their
//-- defaults.
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
  char mqttBroker[LOGGER_MQTT_BROKER_MAX];
  uint16_t mqttPort;
  char mqttUser[LOGGER_MQTT_USER_MAX];
  char mqttPassword[LOGGER_MQTT_PASSWORD_MAX];
  char mqttTopTopic[LOGGER_MQTT_TOPIC_MAX];
  uint32_t mqttIntervalSeconds;
  char hostname[LOGGER_HOSTNAME_MAX];
  uint8_t fixedIpLastOctet;
} loggerSettingsConfigPreTariff;

//-- Layout of the stored blob before the MQTT client id and the tariff
//-- topic sub topic were appended. A stored blob of exactly this size
//-- migrates like the two above: its fields land in the identical prefix
//-- of the current struct and the appended fields get their defaults.
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
  char mqttBroker[LOGGER_MQTT_BROKER_MAX];
  uint16_t mqttPort;
  char mqttUser[LOGGER_MQTT_USER_MAX];
  char mqttPassword[LOGGER_MQTT_PASSWORD_MAX];
  char mqttTopTopic[LOGGER_MQTT_TOPIC_MAX];
  uint32_t mqttIntervalSeconds;
  char hostname[LOGGER_HOSTNAME_MAX];
  uint8_t fixedIpLastOctet;
  uint16_t rfsSyncIntervalMinutes;
  float tariff1PricePerKwh;
  float tariff2PricePerKwh;
  float tariffGasPricePerM3;
  bool dynamicTariffsEnabled;
} loggerSettingsConfigPreTopic;

//-- Layout of the stored blob before the fixed monthly network and
//-- transport costs were appended. A stored blob of exactly this size
//-- migrates like the three above: its fields land in the identical prefix
//-- of the current struct and the appended field gets its default.
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
  char mqttBroker[LOGGER_MQTT_BROKER_MAX];
  uint16_t mqttPort;
  char mqttUser[LOGGER_MQTT_USER_MAX];
  char mqttPassword[LOGGER_MQTT_PASSWORD_MAX];
  char mqttTopTopic[LOGGER_MQTT_TOPIC_MAX];
  uint32_t mqttIntervalSeconds;
  char hostname[LOGGER_HOSTNAME_MAX];
  uint8_t fixedIpLastOctet;
  uint16_t rfsSyncIntervalMinutes;
  float tariff1PricePerKwh;
  float tariff2PricePerKwh;
  float tariffGasPricePerM3;
  bool dynamicTariffsEnabled;
  char mqttClientId[LOGGER_MQTT_CLIENTID_MAX];
  char mqttSubTopic[LOGGER_MQTT_TOPIC_MAX];
} loggerSettingsConfigPreNetworkCosts;

//-- Layout of the stored blob before the export (delivered-back) tariff
//-- prices were appended (the manual import prices carry their old names
//-- here: this is exactly the layout that was stored at the time). A stored
//-- blob of exactly this size migrates like the four above: its fields land
//-- in the identical prefix of the current struct and the appended prices
//-- get their default.
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
  char mqttBroker[LOGGER_MQTT_BROKER_MAX];
  uint16_t mqttPort;
  char mqttUser[LOGGER_MQTT_USER_MAX];
  char mqttPassword[LOGGER_MQTT_PASSWORD_MAX];
  char mqttTopTopic[LOGGER_MQTT_TOPIC_MAX];
  uint32_t mqttIntervalSeconds;
  char hostname[LOGGER_HOSTNAME_MAX];
  uint8_t fixedIpLastOctet;
  uint16_t rfsSyncIntervalMinutes;
  float tariff1PricePerKwh;
  float tariff2PricePerKwh;
  float tariffGasPricePerM3;
  bool dynamicTariffsEnabled;
  char mqttClientId[LOGGER_MQTT_CLIENTID_MAX];
  char mqttSubTopic[LOGGER_MQTT_TOPIC_MAX];
  float networkCostsPerMonthEuro;
} loggerSettingsConfigPreExportTariffs;

//-- Layout of the stored blob before the TFT display time-out was appended.
//-- A stored blob of exactly this size migrates like the five above: its
//-- fields land in the identical prefix of the current struct and the
//-- appended time-out gets its default.
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
  char mqttBroker[LOGGER_MQTT_BROKER_MAX];
  uint16_t mqttPort;
  char mqttUser[LOGGER_MQTT_USER_MAX];
  char mqttPassword[LOGGER_MQTT_PASSWORD_MAX];
  char mqttTopTopic[LOGGER_MQTT_TOPIC_MAX];
  uint32_t mqttIntervalSeconds;
  char hostname[LOGGER_HOSTNAME_MAX];
  uint8_t fixedIpLastOctet;
  uint16_t rfsSyncIntervalMinutes;
  float tariff1PricePerKwh;
  float tariff2PricePerKwh;
  float tariffGasPricePerM3;
  bool dynamicTariffsEnabled;
  char mqttClientId[LOGGER_MQTT_CLIENTID_MAX];
  char mqttSubTopic[LOGGER_MQTT_TOPIC_MAX];
  float networkCostsPerMonthEuro;
  float tariff1ExportPricePerKwh;
  float tariff2ExportPricePerKwh;
} loggerSettingsConfigPreDisplayTimeout;

//-- Layout of the stored blob before the board display mode was appended.
//-- A stored blob of exactly this size migrates like the six above: its
//-- fields land in the identical prefix of the current struct and the
//-- appended mode gets its default (the piggyback board).
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
  char mqttBroker[LOGGER_MQTT_BROKER_MAX];
  uint16_t mqttPort;
  char mqttUser[LOGGER_MQTT_USER_MAX];
  char mqttPassword[LOGGER_MQTT_PASSWORD_MAX];
  char mqttTopTopic[LOGGER_MQTT_TOPIC_MAX];
  uint32_t mqttIntervalSeconds;
  char hostname[LOGGER_HOSTNAME_MAX];
  uint8_t fixedIpLastOctet;
  uint16_t rfsSyncIntervalMinutes;
  float tariff1PricePerKwh;
  float tariff2PricePerKwh;
  float tariffGasPricePerM3;
  bool dynamicTariffsEnabled;
  char mqttClientId[LOGGER_MQTT_CLIENTID_MAX];
  char mqttSubTopic[LOGGER_MQTT_TOPIC_MAX];
  float networkCostsPerMonthEuro;
  float tariff1ExportPricePerKwh;
  float tariff2ExportPricePerKwh;
  uint16_t displayTimeoutMinutes;
} loggerSettingsConfigPreDisplayMode;

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
    LOGGER_LOGW(tag, "rejecting configuration with struct version %" PRIu32, config->structVersion);
    return false;
  }

  //-- The data source is either the fixed live P1 input on UART0 (RX GPIO44)
  //-- or the internal test data generator (-1). Validation is identical for
  //-- SPA and API input.
  if (!loggerSettingsIsLiveGpio(config->rxGpio) && !loggerSettingsIsTestGpio(config->rxGpio))
  {
    LOGGER_LOGW(tag, "rejecting RX GPIO %d: neither the live UART0 pin nor the test selection",
                config->rxGpio);
    return false;
  }

  //-- One telegram is read per interval; the interval must be within the
  //-- documented bounds.
  if ((config->intervalSeconds < LOGGER_SETTINGS_INTERVAL_MIN_S) ||
      (config->intervalSeconds > LOGGER_SETTINGS_INTERVAL_MAX_S))
  {
    LOGGER_LOGW(tag, "rejecting interval %u s: outside %u..%u s", (unsigned)config->intervalSeconds,
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
    LOGGER_LOGW(tag, "rejecting unknown profile id %s", config->profileId);
    return false;
  }

  //-- The country must match the profile so a mismatched pair is refused.
  if (!countryEquals(config->countryCode, profile->countryCode))
  {
    LOGGER_LOGW(tag, "rejecting country %s for profile %s", config->countryCode, config->profileId);
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
      LOGGER_LOGW(tag, "rejecting unsupported M-Bus device type for channel %u", channel);
      return false;
    }
  }

  //-- MQTT text fields: printable ASCII without spaces. The broker may be
  //-- empty (publication disabled); a non-empty broker must be a usable
  //-- host, IP or full mqtt:// URI.
  if (!plainTextOk(config->mqttBroker))
  {
    LOGGER_LOGW(tag, "rejecting MQTT broker text");
    return false;
  }

  if (!plainTextOk(config->mqttUser) || !plainTextOk(config->mqttPassword))
  {
    LOGGER_LOGW(tag, "rejecting MQTT credential text");
    return false;
  }

  //-- The topic additionally excludes the MQTT wildcards and the $ prefix
  //-- of the broker's own reserved topics.
  if (!plainTextOk(config->mqttTopTopic) || (strpbrk(config->mqttTopTopic, "+#$") != NULL))
  {
    LOGGER_LOGW(tag, "rejecting MQTT topic text");
    return false;
  }

  //-- The client id is broker-facing printable ASCII; the tariff sub topic
  //-- is a topic segment and shares the top topic's wildcard exclusion.
  if (!plainTextOk(config->mqttClientId))
  {
    LOGGER_LOGW(tag, "rejecting MQTT client id text");
    return false;
  }

  if (!plainTextOk(config->mqttSubTopic) || (strpbrk(config->mqttSubTopic, "+#$") != NULL))
  {
    LOGGER_LOGW(tag, "rejecting tariff sub topic text");
    return false;
  }

  if (config->mqttIntervalSeconds > LOGGER_SETTINGS_MQTT_INTERVAL_MAX_S)
  {
    LOGGER_LOGW(tag, "rejecting MQTT interval %u s: above %u s",
                (unsigned)config->mqttIntervalSeconds,
                (unsigned)LOGGER_SETTINGS_MQTT_INTERVAL_MAX_S);
    return false;
  }

  //-- Hostname: empty selects the device default; otherwise a lowercase DNS
  //-- label of letters, digits and dashes, not starting or ending on a dash.
  if (!hostnameOk(config->hostname))
  {
    LOGGER_LOGW(tag, "rejecting hostname '%s'", config->hostname);
    return false;
  }

  if (config->fixedIpLastOctet > 254u)
  {
    LOGGER_LOGW(tag, "rejecting fixed IP last octet %u", (unsigned)config->fixedIpLastOctet);
    return false;
  }

  //-- The rfs synchronisation interval is a whole number of minutes within
  //-- the documented bounds.
  if ((config->rfsSyncIntervalMinutes < LOGGER_SETTINGS_RFS_SYNC_MIN_MINUTES) ||
      (config->rfsSyncIntervalMinutes > LOGGER_SETTINGS_RFS_SYNC_MAX_MINUTES))
  {
    LOGGER_LOGW(tag, "rejecting rfs sync interval %u min: outside %u..%u min",
                (unsigned)config->rfsSyncIntervalMinutes,
                (unsigned)LOGGER_SETTINGS_RFS_SYNC_MIN_MINUTES,
                (unsigned)LOGGER_SETTINGS_RFS_SYNC_MAX_MINUTES);
    return false;
  }

  //-- The TFT display time-out is a whole number of minutes within the
  //-- documented bounds.
  if ((config->displayTimeoutMinutes < LOGGER_SETTINGS_DISPLAY_TIMEOUT_MIN_MINUTES) ||
      (config->displayTimeoutMinutes > LOGGER_SETTINGS_DISPLAY_TIMEOUT_MAX_MINUTES))
  {
    LOGGER_LOGW(tag, "rejecting display time-out %u min: outside %u..%u min",
                (unsigned)config->displayTimeoutMinutes,
                (unsigned)LOGGER_SETTINGS_DISPLAY_TIMEOUT_MIN_MINUTES,
                (unsigned)LOGGER_SETTINGS_DISPLAY_TIMEOUT_MAX_MINUTES);
    return false;
  }

  //-- The five manual tariff prices (the import and export price of both
  //-- energy tariffs and the gas price) must be finite and within the sanity
  //-- bounds; negative prices are valid (negative spot prices occur).
  const float manualTariffPrices[] = {
      config->tariff1ImportPricePerKwh, config->tariff2ImportPricePerKwh,
      config->tariff1ExportPricePerKwh, config->tariff2ExportPricePerKwh,
      config->tariffGasPricePerM3,
  };

  for (size_t index = 0u; index < sizeof manualTariffPrices / sizeof manualTariffPrices[0]; index++)
  {
    if (!isfinite(manualTariffPrices[index]))
    {
      LOGGER_LOGW(tag, "rejecting non-finite manual tariff price %u", (unsigned)index);
      return false;
    }

    if ((manualTariffPrices[index] < LOGGER_SETTINGS_TARIFF_PRICE_MIN) ||
        (manualTariffPrices[index] > LOGGER_SETTINGS_TARIFF_PRICE_MAX))
    {
      LOGGER_LOGW(tag, "rejecting manual tariff price %u outside %g..%g", (unsigned)index,
                  (double)LOGGER_SETTINGS_TARIFF_PRICE_MIN,
                  (double)LOGGER_SETTINGS_TARIFF_PRICE_MAX);
      return false;
    }
  }

  //-- The fixed monthly network and transport amount must be finite and
  //-- within the sanity bounds; it is a fee paid, so it is never negative.
  if (!isfinite(config->networkCostsPerMonthEuro) ||
      (config->networkCostsPerMonthEuro < LOGGER_SETTINGS_NETWORK_COST_MIN_EURO) ||
      (config->networkCostsPerMonthEuro > LOGGER_SETTINGS_NETWORK_COST_MAX_EURO))
  {
    LOGGER_LOGW(tag, "rejecting network costs %g EUR: outside %g..%g",
                (double)config->networkCostsPerMonthEuro,
                (double)LOGGER_SETTINGS_NETWORK_COST_MIN_EURO,
                (double)LOGGER_SETTINGS_NETWORK_COST_MAX_EURO);
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
    LOGGER_LOGI(tag, "no settings namespace yet: %s", esp_err_to_name(result));
    return ESP_ERR_NOT_FOUND;
  }

  size_t blobSize = 0u;
  result = nvs_get_blob(handle, NVS_KEY_CONFIG, NULL, &blobSize);

  //-- A blob of the current size is read as-is; a blob of any earlier
  //-- size migrates (the appended fields get their defaults below). Anything
  //-- else cannot be trusted.
  bool migrated = (blobSize == sizeof(loggerSettingsConfigV5));
  bool migratedTariff = (blobSize == sizeof(loggerSettingsConfigPreTariff));
  bool migratedTopic = (blobSize == sizeof(loggerSettingsConfigPreTopic));
  bool migratedCosts = (blobSize == sizeof(loggerSettingsConfigPreNetworkCosts));
  bool migratedExport = (blobSize == sizeof(loggerSettingsConfigPreExportTariffs));
  bool migratedTimeout = (blobSize == sizeof(loggerSettingsConfigPreDisplayTimeout));
  bool migratedDisplayMode = (blobSize == sizeof(loggerSettingsConfigPreDisplayMode));

  if ((result != ESP_OK) || ((blobSize != sizeof(loggerSettingsConfig)) && !migrated &&
                             !migratedTariff && !migratedTopic && !migratedCosts &&
                             !migratedExport && !migratedTimeout && !migratedDisplayMode))
  {
    nvs_close(handle);

    if (result == ESP_ERR_NVS_NOT_FOUND)
    {
      LOGGER_LOGI(tag, "no stored settings yet, awaiting configuration");
      return ESP_ERR_NOT_FOUND;
    }

    LOGGER_LOGW(tag, "stored settings have the wrong size %u, awaiting configuration",
                (unsigned)blobSize);
    return ESP_ERR_NOT_FOUND;
  }

  memset(config, 0, sizeof *config);
  result = nvs_get_blob(handle, NVS_KEY_CONFIG, config, &blobSize);
  nvs_close(handle);

  if ((migrated || migratedTariff) && (result == ESP_OK))
  {
    config->rfsSyncIntervalMinutes = LOGGER_SETTINGS_RFS_SYNC_DEFAULT_MINUTES;

    if (migrated)
    {
      config->mqttPort = LOGGER_SETTINGS_MQTT_PORT_DEFAULT;
      LOGGER_LOGI(
          tag,
          "stored settings migrated: MQTT, network, store and tariff fields take their defaults");
    }
    else
    {
      LOGGER_LOGI(tag, "stored settings migrated: store and tariff fields take their defaults");
    }
  }
  else if (migratedTopic && (result == ESP_OK))
  {
    LOGGER_LOGI(tag, "stored settings migrated: the tariff topic fields take their defaults");
  }
  else if (migratedCosts && (result == ESP_OK))
  {
    config->networkCostsPerMonthEuro = 0.0f;
    LOGGER_LOGI(tag, "stored settings migrated: the network costs field takes its default");
  }
  else if (migratedExport && (result == ESP_OK))
  {
    //-- The stored blob stopped before the export prices: both zero, which
    //-- simply earns nothing for delivered-back energy until they are saved.
    config->tariff1ExportPricePerKwh = 0.0f;
    config->tariff2ExportPricePerKwh = 0.0f;
    LOGGER_LOGI(tag, "stored settings migrated: the export tariff prices take their default");
  }
  else if (migratedTimeout && (result == ESP_OK))
  {
    //-- The stored blob stopped before the display time-out: the documented
    //-- default keeps the panel on for a sensible while after the last push.
    config->displayTimeoutMinutes = LOGGER_SETTINGS_DISPLAY_TIMEOUT_DEFAULT_MINUTES;
    LOGGER_LOGI(tag, "stored settings migrated: the display time-out takes its default");
  }
  else if (migratedDisplayMode && (result == ESP_OK))
  {
    //-- The stored blob stopped before the board display mode: the default
    //-- keeps today's behaviour, the piggyback board driving the panel.
    config->displayEnabled = true;
    LOGGER_LOGI(tag, "stored settings migrated: the board display mode takes its default");
  }

  if (result != ESP_OK)
  {
    LOGGER_LOGW(tag, "reading stored settings failed: %s", esp_err_to_name(result));
    return ESP_ERR_NOT_FOUND;
  }

  if (!loggerSettingsValidate(config))
  {
    LOGGER_LOGW(tag, "stored settings failed validation, awaiting configuration");
    return ESP_ERR_NOT_FOUND;
  }

  LOGGER_LOGI(tag,
              "loaded settings: gpio %d, profile %s, rx polarity %s, tx polarity %s, display %s, "
              "board %s",
              config->rxGpio, config->profileId, config->rxInverted ? "inverted" : "normal",
              config->txInverted ? "inverted" : "normal",
              config->displayFlipped ? "upside down" : "normal",
              config->displayEnabled ? "piggyback" : "none");
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
    LOGGER_LOGE(tag, "persisting settings failed: %s", esp_err_to_name(result));
    return result;
  }

  LOGGER_LOGI(tag,
              "stored settings: gpio %d, profile %s, rx polarity %s, tx polarity %s, display %s, "
              "board %s",
              config->rxGpio, config->profileId, config->rxInverted ? "inverted" : "normal",
              config->txInverted ? "inverted" : "normal",
              config->displayFlipped ? "upside down" : "normal",
              config->displayEnabled ? "piggyback" : "none");
  return ESP_OK;
}

//-- --------------------------------------------------------------------------
//-- Plain ascii ini export and import (the telnet console's settings
//-- {save|load})
//-- --------------------------------------------------------------------------

//-- Upper bound for an accepted settings file: an export is a few hundred
//-- bytes, anything far above that is not a settings file.
#define LOGGER_SETTINGS_INI_MAX_BYTES 65536u

//-- Sections of the exported ini, each named after the group of the settings
//-- view its fields come from: the P1 port (data source, the two polarities,
//-- the read interval, country and protocol version), the TFT display, the
//-- M-Bus channels, MQTT publication, the network identity, the meter store
//-- and the tariffs.
static const char* const INI_SECTIONS[] = {"p1port",  "display", "mbus",   "mqtt",
                                           "network", "store",   "tariffs"};

//-- True when the (already lower case) name is a section the format carries.
static bool iniIsKnownSection(const char* section)
{
  for (size_t index = 0u; index < (sizeof INI_SECTIONS / sizeof INI_SECTIONS[0]); index++)
  {
    if (strcmp(section, INI_SECTIONS[index]) == 0)
    {
      return true;
    }
  }

  return false;
}

//-- Lower case copy of text into out: the ini matches sections, keys and
//-- values case-insensitively, the export writes the plain spellings.
static void iniLowerCopy(char* out, size_t outSize, const char* text)
{
  size_t index = 0u;

  for (; (index + 1u < outSize) && (text[index] != '\0'); index++)
  {
    out[index] = (char)tolower((unsigned char)text[index]);
  }

  out[index] = '\0';
}

//-- Name of one M-Bus device type: the same tokens the settings view of the
//-- SPA uses, so a channel line reads channel2 = "gas" the way the screen
//-- shows the selection.
static const char* mbusTypeName(dsmrMbusDeviceType type)
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
  default:
    return "other";
  }
}

//-- Device type behind one channel token; false when the token names no
//-- known type.
static bool mbusTypeByName(const char* token, dsmrMbusDeviceType* outType)
{
  static const struct
  {
    const char* name;
    dsmrMbusDeviceType type;
  } NAMES[] = {
      {"gas", DSMR_MBUS_DEVICE_GAS},
      {"water", DSMR_MBUS_DEVICE_WATER},
      {"heat", DSMR_MBUS_DEVICE_HEAT},
      {"cooling", DSMR_MBUS_DEVICE_COOLING},
      {"electricity", DSMR_MBUS_DEVICE_ELECTRICITY},
      {"other", DSMR_MBUS_DEVICE_OTHER},
  };

  for (size_t index = 0u; index < (sizeof NAMES / sizeof NAMES[0]); index++)
  {
    if (strcasecmp(token, NAMES[index].name) == 0)
    {
      *outType = NAMES[index].type;
      return true;
    }
  }

  return false;
}

//-- Trims leading and trailing whitespace in place and returns the start.
static char* trimText(char* text)
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

//-- Two-state word token: the file carries words instead of 0/1 (the
//-- polarities read "normal" or "inverted", the orientation "normal" or
//-- "upsideDown", the board display mode "none" or "piggyback").
static bool iniParseWord(const char* text, const char* falseWord, const char* trueWord,
                         bool* outValue)
{
  if (strcasecmp(text, falseWord) == 0)
  {
    *outValue = false;
    return true;
  }

  if (strcasecmp(text, trueWord) == 0)
  {
    *outValue = true;
    return true;
  }

  return false;
}

//-- Boolean token: true/false, on/off, yes/no and 1/0 (case-insensitive).
static bool iniParseBool(const char* text, bool* outValue)
{
  static const char* const TRUE_TOKENS[] = {"true", "on", "yes", "1"};
  static const char* const FALSE_TOKENS[] = {"false", "off", "no", "0"};

  for (size_t index = 0u; index < (sizeof TRUE_TOKENS / sizeof TRUE_TOKENS[0]); index++)
  {
    if (strcasecmp(text, TRUE_TOKENS[index]) == 0)
    {
      *outValue = true;
      return true;
    }
  }

  for (size_t index = 0u; index < (sizeof FALSE_TOKENS / sizeof FALSE_TOKENS[0]); index++)
  {
    if (strcasecmp(text, FALSE_TOKENS[index]) == 0)
    {
      *outValue = false;
      return true;
    }
  }

  return false;
}

//-- Unsigned number with an accepted upper bound; an empty text, trailing
//-- garbage or an out-of-range value is refused.
static bool iniParseUnsigned(const char* text, unsigned long long upperBound,
                             unsigned long long* outValue)
{
  errno = 0;
  char* end = NULL;
  unsigned long long value = strtoull(text, &end, 10);

  if ((end == text) || (*end != '\0') || (errno == ERANGE) || (value > upperBound))
  {
    return false;
  }

  *outValue = value;
  return true;
}

//-- Decimal number of one price line ("0.25000", "-1.5"); trailing garbage
//-- and non-finite results are refused. The settings validation still checks
//-- the accepted value against the price bounds.
static bool iniParseNumber(const char* text, float* outValue)
{
  errno = 0;
  char* end = NULL;
  double value = strtod(text, &end);

  if ((end == text) || (*end != '\0') || (errno == ERANGE) || !isfinite(value))
  {
    return false;
  }

  *outValue = (float)value;
  return true;
}

//-- Copies a text value into a settings field; a value longer than the field
//-- can carry is refused instead of silently truncated.
static bool iniAssignText(char* dest, size_t destSize, const char* value)
{
  if (strlen(value) >= destSize)
  {
    return false;
  }

  snprintf(dest, destSize, "%s", value);
  return true;
}

//-- Channel 1 through 4 behind one "channel<n>" key; false for any other key
//-- or an out-of-range number.
static bool iniChannelNumber(const char* key, uint8_t* outChannel)
{
  if (strncasecmp(key, "channel", 7) != 0)
  {
    return false;
  }

  const char* digits = key + 7;

  if ((digits[0] < '1') || (digits[0] > ('0' + DSMR_MBUS_CHANNEL_MAX)) || (digits[1] != '\0'))
  {
    return false;
  }

  *outChannel = (uint8_t)(digits[0] - '0');
  return true;
}

//-- Ini writer: counts the key lines written and remembers the first write
//-- failure, so a half-written export can be dropped.
typedef struct
{
  FILE* file;
  bool failed;
  uint32_t fields;
} IniWriter;

static void iniWriteSection(IniWriter* writer, const char* section)
{
  if (fprintf(writer->file, "\r\n[%s]\r\n", section) < 0)
  {
    writer->failed = true;
  }
}

//-- One plain ascii field: text values quoted, numbers unquoted.
static void iniWriteText(IniWriter* writer, const char* key, const char* value)
{
  if (fprintf(writer->file, "%s = \"%s\"\r\n", key, value) < 0)
  {
    writer->failed = true;
    return;
  }

  writer->fields++;
}

static void iniWriteNumber(IniWriter* writer, const char* key, unsigned long value)
{
  if (fprintf(writer->file, "%s = %lu\r\n", key, value) < 0)
  {
    writer->failed = true;
    return;
  }

  writer->fields++;
}

static void iniWritePrice(IniWriter* writer, const char* key, double value)
{
  if (fprintf(writer->file, "%s = %.5f\r\n", key, value) < 0)
  {
    writer->failed = true;
    return;
  }

  writer->fields++;
}

esp_err_t loggerSettingsExportToFile(const char* absolutePath, uint32_t* outFields)
{
  if ((absolutePath == NULL) || (outFields == NULL))
  {
    return ESP_ERR_INVALID_ARG;
  }

  *outFields = 0u;

  //-- The export mirrors the stored configuration: exactly the settings the
  //-- settings view of the SPA shows and mutates.
  loggerSettingsConfig config;
  esp_err_t result = loggerSettingsLoad(&config);

  if (result != ESP_OK)
  {
    LOGGER_LOGW(tag, "ini export refused: no valid stored settings (%s)", esp_err_to_name(result));
    return result;
  }

  FILE* file = fopen(absolutePath, "wb");

  if (file == NULL)
  {
    LOGGER_LOGW(tag, "ini export could not create %s: errno %d", absolutePath, errno);
    return ESP_FAIL;
  }

  IniWriter writer;
  writer.file = file;
  writer.failed = false;
  writer.fields = 0u;

  if (fprintf(file, "; lastDSMRlogger settings (plain ascii ini)\r\n"
                    "; every field mutated through the settings view of the SPA\r\n"
                    "; load it back with the telnet command \"settings load\"\r\n") < 0)
  {
    writer.failed = true;
  }

  iniWriteSection(&writer, "p1port");
  iniWriteText(&writer, "dataSource", loggerSettingsIsTestGpio(config.rxGpio) ? "test" : "live");
  iniWriteText(&writer, "P1RXPolarity", config.rxInverted ? "inverted" : "normal");
  iniWriteText(&writer, "P1TXPolarity", config.txInverted ? "inverted" : "normal");
  iniWriteNumber(&writer, "interval", (unsigned long)config.intervalSeconds);
  iniWriteText(&writer, "country", config.countryCode);
  iniWriteText(&writer, "profile", config.profileId);

  iniWriteSection(&writer, "display");
  iniWriteText(&writer, "board", config.displayEnabled ? "piggyback" : "none");
  iniWriteText(&writer, "orientation", config.displayFlipped ? "upsideDown" : "normal");
  iniWriteNumber(&writer, "timeoutMinutes", (unsigned long)config.displayTimeoutMinutes);

  iniWriteSection(&writer, "mbus");

  for (uint8_t channel = 1u; channel <= DSMR_MBUS_CHANNEL_MAX; channel++)
  {
    char key[16];
    snprintf(key, sizeof key, "channel%u", (unsigned)channel);

    const char* value = "auto";

    if (config.mbusMode[channel] == LOGGER_MBUS_MODE_DISABLED)
    {
      value = "disabled";
    }
    else if (config.mbusMode[channel] == LOGGER_MBUS_MODE_EXPECTED)
    {
      value = mbusTypeName(config.mbusType[channel]);
    }

    iniWriteText(&writer, key, value);
  }

  iniWriteSection(&writer, "mqtt");
  iniWriteText(&writer, "brokerUrl", config.mqttBroker);
  iniWriteNumber(&writer, "brokerPort", (unsigned long)config.mqttPort);
  iniWriteText(&writer, "user", config.mqttUser);
  iniWriteText(&writer, "password", config.mqttPassword);
  iniWriteText(&writer, "topTopic", config.mqttTopTopic);
  iniWriteText(&writer, "clientId", config.mqttClientId);
  iniWriteText(&writer, "subTopic", config.mqttSubTopic);
  iniWriteNumber(&writer, "broadcastInterval", (unsigned long)config.mqttIntervalSeconds);

  iniWriteSection(&writer, "network");
  iniWriteText(&writer, "hostname", config.hostname);
  iniWriteNumber(&writer, "fixedIpLastOctet", (unsigned long)config.fixedIpLastOctet);

  iniWriteSection(&writer, "store");
  iniWriteNumber(&writer, "rfsSyncIntervalMinutes", (unsigned long)config.rfsSyncIntervalMinutes);

  iniWriteSection(&writer, "tariffs");
  iniWriteText(&writer, "dynamicTariffs", config.dynamicTariffsEnabled ? "true" : "false");
  iniWritePrice(&writer, "tariff1Delivered", (double)config.tariff1ImportPricePerKwh);
  iniWritePrice(&writer, "tariff2Delivered", (double)config.tariff2ImportPricePerKwh);
  iniWritePrice(&writer, "tariff1Received", (double)config.tariff1ExportPricePerKwh);
  iniWritePrice(&writer, "tariff2Received", (double)config.tariff2ExportPricePerKwh);
  iniWritePrice(&writer, "tariffGasDelivered", (double)config.tariffGasPricePerM3);
  iniWritePrice(&writer, "networkCostsPerMonth", (double)config.networkCostsPerMonthEuro);

  fclose(file);

  if (writer.failed)
  {
    //-- A half-written export is no file anyone should load: drop it.
    remove(absolutePath);
    LOGGER_LOGW(tag, "ini export to %s failed", absolutePath);
    return ESP_FAIL;
  }

  *outFields = writer.fields;
  LOGGER_LOGI(tag, "ini export wrote %" PRIu32 " setting(s) to %s", writer.fields, absolutePath);
  return ESP_OK;
}

//-- Applies one "key = value" line of the named section to the candidate
//-- configuration. An unknown key or a malformed value aborts the load: a
//-- typo in a hand-edited file must not silently keep the stored value.
//-- Malformed values report ESP_ERR_INVALID_SIZE, unknown names
//-- ESP_ERR_NOT_FOUND.
static esp_err_t iniApplyValue(loggerSettingsConfig* config, const char* section, const char* key,
                               const char* value)
{
  if (strcmp(section, "p1port") == 0)
  {
    if (strcasecmp(key, "dataSource") == 0)
    {
      if (strcasecmp(value, "live") == 0)
      {
        config->rxGpio = LOGGER_RX_GPIO_LIVE;
        return ESP_OK;
      }

      if (strcasecmp(value, "test") == 0)
      {
        config->rxGpio = LOGGER_RX_GPIO_TEST;
        return ESP_OK;
      }

      return ESP_ERR_INVALID_SIZE;
    }

    if (strcasecmp(key, "P1RXPolarity") == 0)
    {
      return iniParseWord(value, "normal", "inverted", &config->rxInverted) ? ESP_OK
                                                                            : ESP_ERR_INVALID_SIZE;
    }

    if (strcasecmp(key, "P1TXPolarity") == 0)
    {
      return iniParseWord(value, "normal", "inverted", &config->txInverted) ? ESP_OK
                                                                            : ESP_ERR_INVALID_SIZE;
    }

    if (strcasecmp(key, "interval") == 0)
    {
      unsigned long long number = 0u;

      if (!iniParseUnsigned(value, UINT32_MAX, &number))
      {
        return ESP_ERR_INVALID_SIZE;
      }

      config->intervalSeconds = (uint32_t)number;
      return ESP_OK;
    }

    if (strcasecmp(key, "country") == 0)
    {
      return iniAssignText(config->countryCode, sizeof config->countryCode, value)
                 ? ESP_OK
                 : ESP_ERR_INVALID_SIZE;
    }

    if (strcasecmp(key, "profile") == 0)
    {
      return iniAssignText(config->profileId, sizeof config->profileId, value)
                 ? ESP_OK
                 : ESP_ERR_INVALID_SIZE;
    }

    return ESP_ERR_NOT_FOUND;
  }

  if (strcmp(section, "display") == 0)
  {
    //-- The board display mode: "piggyback" drives the panel, encoder and
    //-- buttons; "none" selects "No Display" (the board stays untouched).
    if (strcasecmp(key, "board") == 0)
    {
      return iniParseWord(value, "none", "piggyback", &config->displayEnabled)
                 ? ESP_OK
                 : ESP_ERR_INVALID_SIZE;
    }

    if (strcasecmp(key, "orientation") == 0)
    {
      return iniParseWord(value, "normal", "upsideDown", &config->displayFlipped)
                 ? ESP_OK
                 : ESP_ERR_INVALID_SIZE;
    }

    //-- The display time-out in minutes; the complete candidate is
    //-- validated against the documented bounds before anything is stored.
    if (strcasecmp(key, "timeoutMinutes") == 0)
    {
      unsigned long long number = 0u;

      if (!iniParseUnsigned(value, UINT16_MAX, &number))
      {
        return ESP_ERR_INVALID_SIZE;
      }

      config->displayTimeoutMinutes = (uint16_t)number;
      return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
  }

  if (strcmp(section, "mbus") == 0)
  {
    uint8_t channel = 0u;

    if (!iniChannelNumber(key, &channel))
    {
      return ESP_ERR_NOT_FOUND;
    }

    if (strcasecmp(value, "auto") == 0)
    {
      config->mbusMode[channel] = LOGGER_MBUS_MODE_AUTO;
      return ESP_OK;
    }

    if (strcasecmp(value, "disabled") == 0)
    {
      config->mbusMode[channel] = LOGGER_MBUS_MODE_DISABLED;
      return ESP_OK;
    }

    if (mbusTypeByName(value, &config->mbusType[channel]))
    {
      config->mbusMode[channel] = LOGGER_MBUS_MODE_EXPECTED;
      return ESP_OK;
    }

    return ESP_ERR_INVALID_SIZE;
  }

  if (strcmp(section, "mqtt") == 0)
  {
    if (strcasecmp(key, "brokerUrl") == 0)
    {
      return iniAssignText(config->mqttBroker, sizeof config->mqttBroker, value)
                 ? ESP_OK
                 : ESP_ERR_INVALID_SIZE;
    }

    if (strcasecmp(key, "brokerPort") == 0)
    {
      unsigned long long number = 0u;

      if (!iniParseUnsigned(value, UINT16_MAX, &number))
      {
        return ESP_ERR_INVALID_SIZE;
      }

      config->mqttPort = (uint16_t)number;
      return ESP_OK;
    }

    if (strcasecmp(key, "user") == 0)
    {
      return iniAssignText(config->mqttUser, sizeof config->mqttUser, value) ? ESP_OK
                                                                             : ESP_ERR_INVALID_SIZE;
    }

    if (strcasecmp(key, "password") == 0)
    {
      return iniAssignText(config->mqttPassword, sizeof config->mqttPassword, value)
                 ? ESP_OK
                 : ESP_ERR_INVALID_SIZE;
    }

    if (strcasecmp(key, "topTopic") == 0)
    {
      return iniAssignText(config->mqttTopTopic, sizeof config->mqttTopTopic, value)
                 ? ESP_OK
                 : ESP_ERR_INVALID_SIZE;
    }

    if (strcasecmp(key, "clientId") == 0)
    {
      return iniAssignText(config->mqttClientId, sizeof config->mqttClientId, value)
                 ? ESP_OK
                 : ESP_ERR_INVALID_SIZE;
    }

    if (strcasecmp(key, "subTopic") == 0)
    {
      return iniAssignText(config->mqttSubTopic, sizeof config->mqttSubTopic, value)
                 ? ESP_OK
                 : ESP_ERR_INVALID_SIZE;
    }

    if (strcasecmp(key, "broadcastInterval") == 0)
    {
      unsigned long long number = 0u;

      if (!iniParseUnsigned(value, UINT32_MAX, &number))
      {
        return ESP_ERR_INVALID_SIZE;
      }

      config->mqttIntervalSeconds = (uint32_t)number;
      return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
  }

  if (strcmp(section, "network") == 0)
  {
    if (strcasecmp(key, "hostname") == 0)
    {
      return iniAssignText(config->hostname, sizeof config->hostname, value) ? ESP_OK
                                                                             : ESP_ERR_INVALID_SIZE;
    }

    if (strcasecmp(key, "fixedIpLastOctet") == 0)
    {
      unsigned long long number = 0u;

      if (!iniParseUnsigned(value, UINT8_MAX, &number))
      {
        return ESP_ERR_INVALID_SIZE;
      }

      config->fixedIpLastOctet = (uint8_t)number;
      return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
  }

  if (strcmp(section, "store") == 0)
  {
    if (strcasecmp(key, "rfsSyncIntervalMinutes") == 0)
    {
      unsigned long long number = 0u;

      if (!iniParseUnsigned(value, UINT16_MAX, &number))
      {
        return ESP_ERR_INVALID_SIZE;
      }

      config->rfsSyncIntervalMinutes = (uint16_t)number;
      return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
  }

  if (strcmp(section, "tariffs") == 0)
  {
    if (strcasecmp(key, "dynamicTariffs") == 0)
    {
      return iniParseBool(value, &config->dynamicTariffsEnabled) ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }

    float price = 0.0f;

    if (strcasecmp(key, "tariff1Delivered") == 0)
    {
      if (!iniParseNumber(value, &price))
      {
        return ESP_ERR_INVALID_SIZE;
      }

      config->tariff1ImportPricePerKwh = price;
      return ESP_OK;
    }

    if (strcasecmp(key, "tariff2Delivered") == 0)
    {
      if (!iniParseNumber(value, &price))
      {
        return ESP_ERR_INVALID_SIZE;
      }

      config->tariff2ImportPricePerKwh = price;
      return ESP_OK;
    }

    if (strcasecmp(key, "tariff1Received") == 0)
    {
      if (!iniParseNumber(value, &price))
      {
        return ESP_ERR_INVALID_SIZE;
      }

      config->tariff1ExportPricePerKwh = price;
      return ESP_OK;
    }

    if (strcasecmp(key, "tariff2Received") == 0)
    {
      if (!iniParseNumber(value, &price))
      {
        return ESP_ERR_INVALID_SIZE;
      }

      config->tariff2ExportPricePerKwh = price;
      return ESP_OK;
    }

    if (strcasecmp(key, "tariffGasDelivered") == 0)
    {
      if (!iniParseNumber(value, &price))
      {
        return ESP_ERR_INVALID_SIZE;
      }

      config->tariffGasPricePerM3 = price;
      return ESP_OK;
    }

    if (strcasecmp(key, "networkCostsPerMonth") == 0)
    {
      if (!iniParseNumber(value, &price))
      {
        return ESP_ERR_INVALID_SIZE;
      }

      config->networkCostsPerMonthEuro = price;
      return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
  }

  return ESP_ERR_NOT_FOUND;
}

esp_err_t loggerSettingsImportFromFile(const char* absolutePath, uint32_t* outFields)
{
  if ((absolutePath == NULL) || (outFields == NULL))
  {
    return ESP_ERR_INVALID_ARG;
  }

  *outFields = 0u;

  FILE* file = fopen(absolutePath, "rb");

  if (file == NULL)
  {
    LOGGER_LOGW(tag, "ini import could not open %s: errno %d", absolutePath, errno);
    return ESP_FAIL;
  }

  fseek(file, 0, SEEK_END);
  long fileSize = ftell(file);
  rewind(file);

  if ((fileSize <= 0) || ((size_t)fileSize > LOGGER_SETTINGS_INI_MAX_BYTES))
  {
    fclose(file);
    LOGGER_LOGW(tag, "ini import rejects %s: size %ld bytes", absolutePath, fileSize);
    return ESP_ERR_INVALID_SIZE;
  }

  char* text = malloc((size_t)fileSize + 1u);

  if (text == NULL)
  {
    fclose(file);
    return ESP_ERR_NO_MEM;
  }

  size_t readCount = fread(text, 1u, (size_t)fileSize, file);
  fclose(file);
  text[readCount] = '\0';

  //-- The file overwrites the stored configuration field by field: a key the
  //-- file does not carry keeps its stored value, so a hand-trimmed file
  //-- stays loadable. Without any stored settings the candidate starts at the
  //-- zeroed struct; a complete export carries every field anyway.
  loggerSettingsConfig candidate;

  if (loggerSettingsLoad(&candidate) != ESP_OK)
  {
    memset(&candidate, 0, sizeof candidate);
  }

  esp_err_t result = ESP_OK;
  uint32_t applied = 0u;
  uint32_t lineNumber = 0u;
  char section[16] = "";
  char* line = text;

  while ((result == ESP_OK) && (line != NULL))
  {
    char* next = strchr(line, '\n');

    if (next != NULL)
    {
      //-- A crlf-terminated file keeps its carriage return out of the value.
      if ((next > line) && (next[-1] == '\r'))
      {
        next[-1] = '\0';
      }

      *next++ = '\0';
    }

    lineNumber++;
    char* trimmed = trimText(line);

    if ((*trimmed == '\0') || (*trimmed == ';') || (*trimmed == '#'))
    {
      line = next;
      continue;
    }

    if (*trimmed == '[')
    {
      char* sectionEnd = strchr(trimmed, ']');

      if (sectionEnd == NULL)
      {
        result = ESP_ERR_INVALID_SIZE;
        break;
      }

      *sectionEnd = '\0';
      iniLowerCopy(section, sizeof section, trimText(trimmed + 1));

      if ((section[0] == '\0') || !iniIsKnownSection(section))
      {
        result = ESP_ERR_NOT_FOUND;
        break;
      }
    }
    else
    {
      //-- Values outside a [section] have no group to land in.
      if (section[0] == '\0')
      {
        result = ESP_ERR_INVALID_SIZE;
        break;
      }

      char* separator = strchr(trimmed, '=');

      if (separator == NULL)
      {
        result = ESP_ERR_INVALID_SIZE;
        break;
      }

      *separator++ = '\0';
      char* key = trimText(trimmed);
      char* value = trimText(separator);

      //-- Text values are written quoted; a closing quote is taken off so the
      //-- value lands without its quotes.
      if ((value[0] == '"') && (strrchr(value + 1u, '"') != NULL))
      {
        *strrchr(value + 1u, '"') = '\0';
        value++;
      }

      result = iniApplyValue(&candidate, section, key, value);

      if (result == ESP_OK)
      {
        applied++;
      }
    }

    line = next;
  }

  if (result == ESP_OK)
  {
    candidate.structVersion = LOGGER_SETTINGS_VERSION;

    if (!loggerSettingsValidate(&candidate))
    {
      LOGGER_LOGW(tag, "ini import of %s left an invalid configuration, nothing stored",
                  absolutePath);
      result = ESP_ERR_INVALID_ARG;
    }
    else
    {
      result = loggerSettingsSave(&candidate);

      if (result == ESP_OK)
      {
        *outFields = applied;
        LOGGER_LOGI(tag, "ini import stored %" PRIu32 " setting(s) from %s", applied, absolutePath);
      }
    }
  }

  if (result != ESP_OK)
  {
    LOGGER_LOGW(tag, "ini import of %s stopped at line %" PRIu32 ": %s", absolutePath, lineNumber,
                esp_err_to_name(result));
  }

  free(text);
  return result;
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
