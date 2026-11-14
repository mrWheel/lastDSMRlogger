//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_settings.h - versioned settings service
//-- Date: 2026-11-15 · Build: 1
//--
//-- The settings service stores the complete versioned configuration in
//-- ESP-IDF NVS, validates it against the parserDSMR built-in profiles and
//-- the board allowlist, and exposes the valid choices for the settings view
//-- of the SPA. Validation runs identically for a selection arriving through
//-- the SPA and through the API: only the service validates.
//-- ==========================================================================

#ifndef LOGGER_SETTINGS_H
#define LOGGER_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#include "dsmr_parser.h"
#include "esp_err.h"

//-- --------------------------------------------------------------------------
//-- P1 RX pin allowlist
//-- --------------------------------------------------------------------------

//-- Selectable P1 RX input pins on the board J5 connector. These GPIOs are
//-- wired only to J5 and are free for general use when the board component
//-- drives the display, encoder, buttons, SD and I2C resources. GPIO43 and
//-- GPIO44 stay reserved for the console on UART0.
#define LOGGER_P1_GPIO_COUNT 8
extern const int loggerP1GpioChoices[LOGGER_P1_GPIO_COUNT];

//-- Special RX selection: -1 selects the internal test data generator. No
//-- UART is started; the receiver feeds synthetic telegrams through the
//-- parser instead. It is a mode marker, not a board pin, so it stays out of
//-- the J5 allowlist and is validated separately.
#define LOGGER_RX_GPIO_TEST (-1)

bool loggerSettingsIsSelectableGpio(int gpio);

//-- True when the selection is the internal test data generator.
bool loggerSettingsIsTestGpio(int gpio);

//-- --------------------------------------------------------------------------
//-- Settings model
//-- --------------------------------------------------------------------------

//-- M-Bus channel display mode. Disabled is a logger display setting: the
//-- channel is omitted from the decoded summary while its lines stay visible
//-- in the raw telegram. Expected submits the device type through the parser
//-- configuration so a mismatch yields DSMR_DIAG_MBUS_TYPE_MISMATCH.
typedef enum
{
  LOGGER_MBUS_MODE_AUTO = 0,
  LOGGER_MBUS_MODE_DISABLED,
  LOGGER_MBUS_MODE_EXPECTED,
} loggerMbusMode;

#define LOGGER_SETTINGS_VERSION 1u
#define LOGGER_SETTINGS_PROFILE_ID_MAX 32
#define LOGGER_SETTINGS_COUNTRY_MAX 3

typedef struct
{
  uint32_t structVersion;
  int rxGpio;
  char countryCode[LOGGER_SETTINGS_COUNTRY_MAX];
  char profileId[LOGGER_SETTINGS_PROFILE_ID_MAX];

  //-- Per M-Bus channel 1 through 4; index 0 is unused.
  loggerMbusMode mbusMode[DSMR_MBUS_CHANNEL_MAX + 1];
  dsmrMbusDeviceType mbusType[DSMR_MBUS_CHANNEL_MAX + 1];
} loggerSettingsConfig;

//-- Loads the stored configuration and validates it. Returns ESP_ERR_NOT_FOUND
//-- on first boot or when the stored data cannot be trusted, in which case
//-- reception shall await a valid configuration.
esp_err_t loggerSettingsLoad(loggerSettingsConfig *config);

//-- Validates a complete candidate configuration: selectable GPIO, known
//-- profile with matching country, and per-channel M-Bus modes and types.
bool loggerSettingsValidate(const loggerSettingsConfig *config);

//-- Validates and persists the complete configuration as one operation.
esp_err_t loggerSettingsSave(const loggerSettingsConfig *config);

//-- Copies the expected M-Bus device declarations (explicit selections only)
//-- into outDevices and returns the number of entries written.
uint8_t loggerSettingsBuildMbusDevices(const loggerSettingsConfig *config, dsmrMbusDevice *outDevices, uint8_t deviceCapacity);

//-- --------------------------------------------------------------------------
//-- Valid choices for the settings view, obtained from the parser profiles
//-- --------------------------------------------------------------------------

typedef struct
{
  char profileId[LOGGER_SETTINGS_PROFILE_ID_MAX];
  char countryCode[LOGGER_SETTINGS_COUNTRY_MAX];
  char versionLabel[32];
  uint32_t telegramIntervalMs;
} loggerSettingsProfileInfo;

//-- Number of built-in profiles, from dsmrProfileListIds().
uint16_t loggerSettingsProfileCount(void);

//-- Profile metadata at the given index; false when out of range.
bool loggerSettingsProfileAt(uint16_t index, loggerSettingsProfileInfo *info);

//-- Resolves one profile by stable identifier; false when unknown.
bool loggerSettingsLookupProfile(const char *profileId, loggerSettingsProfileInfo *info);

//-- True when the two letter country code matches a built-in profile.
bool loggerSettingsIsKnownCountry(const char *countryCode);

#endif //-- LOGGER_SETTINGS_H
