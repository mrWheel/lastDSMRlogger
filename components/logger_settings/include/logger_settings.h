//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_settings.h - versioned settings service
//-- Date: 2026-11-15 · Build: 3
//--
//-- The settings service stores the complete versioned configuration in
//-- ESP-IDF NVS, validates it against the parserDSMR built-in profiles and
//-- the P1 data source and read interval rules, and exposes the valid
//-- choices for the settings view
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
//-- P1 data source selections
//-- --------------------------------------------------------------------------

//-- The P1 port of the smart meter always connects to UART0: RX on GPIO44,
//-- TX on GPIO43 (the meter never needs our TX). These pins are fixed; the
//-- console runs on the native USB serial/JTAG peripheral instead of UART0.
#define LOGGER_P1_UART0_RX_GPIO 44
#define LOGGER_P1_UART0_TX_GPIO 43

//-- Stored RX selection for live data: the fixed UART0 receive pin.
#define LOGGER_RX_GPIO_LIVE LOGGER_P1_UART0_RX_GPIO

//-- Special RX selection: -1 selects the internal test data generator. No
//-- UART is started; the receiver feeds synthetic telegrams through the
//-- parser instead. It is a mode marker, not a board pin.
#define LOGGER_RX_GPIO_TEST (-1)

//-- True when the selection is the fixed live UART0 receive pin.
bool loggerSettingsIsLiveGpio(int gpio);

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

//-- Version 2: the P1 port is fixed on UART0 (GPIO43/44) and the telegram
//-- read interval became a setting; stored version 1 blobs are rejected.
//-- Version 3: the P1 RX polarity became a runtime setting (Normal/Inverted)
//-- instead of a build-time Kconfig choice; stored version 2 blobs are
//-- rejected, so one save is required after updating.
#define LOGGER_SETTINGS_VERSION 3u
#define LOGGER_SETTINGS_PROFILE_ID_MAX 32
#define LOGGER_SETTINGS_COUNTRY_MAX 3

//-- Accepted read interval bounds in seconds: only one telegram is read
//-- (accepted into the history) per interval.
#define LOGGER_SETTINGS_INTERVAL_MIN_S 2u
#define LOGGER_SETTINGS_INTERVAL_MAX_S 600u

typedef struct
{
  uint32_t structVersion;
  int rxGpio;

  //-- P1 RX polarity for the live UART0 input. False reads the signal
  //-- directly (idle high); true inverts the receive line for interfaces
  //-- that invert before the ESP32-S3. Ignored by the test generator.
  bool rxInverted;

  //-- Read interval in seconds (LOGGER_SETTINGS_INTERVAL_MIN_S through
  //-- LOGGER_SETTINGS_INTERVAL_MAX_S).
  uint32_t intervalSeconds;

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
