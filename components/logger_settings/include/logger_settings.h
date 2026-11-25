//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_settings.h - versioned settings service
//-- Date: 2026-11-24 · Build: 14
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
//-- Version 4: the TFT display orientation became a setting (Normal/Upside
//-- down); stored version 3 blobs are rejected, so one save is required
//-- after updating.
//-- Version 5: the P1 TX polarity (Forwarding Data) became a setting
//-- (Normal/Inverted) for both copies of a forwarding burst; stored version 4
//-- blobs are rejected, so one save is required after updating.
//-- Version 5 (extended): the MQTT publication and the network identity
//-- fields were appended at the end of the struct. Smaller stored blobs
//-- (the five original fields) migrate on load: the appended fields get
//-- their defaults, so no save is required after updating.
//-- Version 5 (extended again): the rfs synchronisation interval and the
//-- manual tariff prices were appended at the end of the struct. Blobs of
//-- either earlier size migrate on load: the appended fields get their
//-- defaults, so no save is required after updating.
//-- Version 5 (extended again): the MQTT client id and the tariff topic
//-- sub topic were appended at the end of the struct. Blobs of any earlier
//-- size migrate on load: the appended fields get their defaults, so no
//-- save is required after updating.
//-- Version 5 (extended once more): the fixed monthly network and transport
//-- costs were appended at the end of the struct. Blobs of any earlier size
//-- migrate on load: the appended field gets its default, so no save is
//-- required after updating.
//-- Version 5 (extended a fifth time): the two export (delivered-back)
//-- tariff prices were appended at the end of the struct, and the manual
//-- import tariff prices gained their import/export names (identical layout,
//-- names only). Blobs of any earlier size migrate on load: the appended
//-- prices get their default, so no save is required after updating.
//-- Version 5 (extended a sixth time): the TFT display time-out in minutes
//-- was appended at the end of the struct. Blobs of any earlier size
//-- migrate on load: the appended field gets its default, so no save is
//-- required after updating.
//-- Version 5 (extended a seventh time): the board display mode was
//-- appended at the end of the struct (true drives the piggyback board,
//-- false selects "No Display"). Blobs of any earlier size migrate on
//-- load: the appended field gets its default (the piggyback board), so no
//-- save is required after updating.
#define LOGGER_SETTINGS_VERSION 5u
#define LOGGER_SETTINGS_PROFILE_ID_MAX 32
#define LOGGER_SETTINGS_COUNTRY_MAX 3

//-- Bounds of the appended MQTT and network fields.
#define LOGGER_MQTT_BROKER_MAX 128
#define LOGGER_MQTT_USER_MAX 64
#define LOGGER_MQTT_PASSWORD_MAX 64
#define LOGGER_MQTT_TOPIC_MAX 64
#define LOGGER_MQTT_CLIENTID_MAX 64
#define LOGGER_HOSTNAME_MAX 33
#define LOGGER_SETTINGS_MQTT_PORT_DEFAULT 1883u
#define LOGGER_SETTINGS_MQTT_INTERVAL_MAX_S 86400u

//-- Accepted read interval bounds in seconds: only one telegram is read
//-- (accepted into the history) per interval.
#define LOGGER_SETTINGS_INTERVAL_MIN_S 2u
#define LOGGER_SETTINGS_INTERVAL_MAX_S 600u

//-- rfs synchronisation interval in minutes: the meter store commits its
//-- staged ring records at least this often (one-minute snapshots coalesce
//-- in the staging buffers, so only the synchronisation touches storage).
#define LOGGER_SETTINGS_RFS_SYNC_MIN_MINUTES 1u
#define LOGGER_SETTINGS_RFS_SYNC_MAX_MINUTES 60u
#define LOGGER_SETTINGS_RFS_SYNC_DEFAULT_MINUTES 10u

//-- TFT display time-out in minutes: the panel backlight goes off this many
//-- minutes after the last physical button action on the board (an encoder
//-- turn or a button press). The AUX button lights the panel again and
//-- restarts the timer. The setting lives in the Display group of the
//-- settings view.
#define LOGGER_SETTINGS_DISPLAY_TIMEOUT_MIN_MINUTES 1u
#define LOGGER_SETTINGS_DISPLAY_TIMEOUT_MAX_MINUTES 60u
#define LOGGER_SETTINGS_DISPLAY_TIMEOUT_DEFAULT_MINUTES 10u

//-- Bounds of the manual tariff prices in EUR per kWh (the import and the
//-- export price of both energy tariffs) and EUR per m³ (gas). Negative
//-- prices are valid: negative spot prices occur. The prices apply when the
//-- dynamic tariff table has no price for the period, and always for gas.
//-- The tariff 1 or tariff 2 price of a direction applies as the meter's
//-- tariff indicator (OBIS 0-0:96.14.0) names the active tariff.
#define LOGGER_SETTINGS_TARIFF_PRICE_MIN (-5.0f)
#define LOGGER_SETTINGS_TARIFF_PRICE_MAX 5.0f

//-- Bounds of the fixed monthly network and transport amount in EUR (the
//-- "Netw." column of the Months view's Costs presentation): a flat fee
//-- added to every month's electricity and gas amounts. A more advanced
//-- treatment of the network costs may follow later.
#define LOGGER_SETTINGS_NETWORK_COST_MIN_EURO 0.0f
#define LOGGER_SETTINGS_NETWORK_COST_MAX_EURO 1000.0f

typedef struct
{
  uint32_t structVersion;
  int rxGpio;

  //-- P1 RX polarity for the live UART0 input. False reads the signal
  //-- directly (idle high); true inverts the receive line for interfaces
  //-- that invert before the ESP32-S3. Ignored by the test generator.
  bool rxInverted;

  //-- P1 TX polarity for the UART0 forwarding bursts. False sends the two
  //-- copies of the accepted frame as-is; true inverts TXD for interfaces
  //-- that expect an inverted transmit line. It governs both copies of one
  //-- burst; outside a burst TX idles at the polarity of rxInverted.
  bool txInverted;

  //-- Read interval in seconds (LOGGER_SETTINGS_INTERVAL_MIN_S through
  //-- LOGGER_SETTINGS_INTERVAL_MAX_S).
  uint32_t intervalSeconds;

  char countryCode[LOGGER_SETTINGS_COUNTRY_MAX];
  char profileId[LOGGER_SETTINGS_PROFILE_ID_MAX];

  //-- Per M-Bus channel 1 through 4; index 0 is unused.
  loggerMbusMode mbusMode[DSMR_MBUS_CHANNEL_MAX + 1];
  dsmrMbusDeviceType mbusType[DSMR_MBUS_CHANNEL_MAX + 1];

  //-- TFT display orientation. False keeps the assembled landscape
  //-- orientation; true turns the panel 180 degrees for enclosures that
  //-- mount the board the other way round.
  bool displayFlipped;

  //-- ------------------------------------------------------------------------
  //-- MQTT publication (appended at version 5; older blobs leave these at
  //-- their defaults, which keeps publication disabled)
  //-- ------------------------------------------------------------------------

  //-- Broker host, IP or full mqtt:// / mqtts:// URI. Empty disables MQTT.
  char mqttBroker[LOGGER_MQTT_BROKER_MAX];

  //-- Broker port used when the broker text carries no port of its own.
  uint16_t mqttPort;

  //-- Broker credentials; both empty for an anonymous broker.
  char mqttUser[LOGGER_MQTT_USER_MAX];
  char mqttPassword[LOGGER_MQTT_PASSWORD_MAX];

  //-- Top topic of the publication: every broadcast goes to
  //-- "<topTopic>/telegram". Empty selects the default "lastdsmrlogger".
  char mqttTopTopic[LOGGER_MQTT_TOPIC_MAX];

  //-- Broadcast interval in seconds: every interval the most recently
  //-- accepted telegram is published to the broker. 0 means no broadcast.
  uint32_t mqttIntervalSeconds;

  //-- ------------------------------------------------------------------------
  //-- Network identity (appended at version 5)
  //-- ------------------------------------------------------------------------

  //-- Hostname the station announces to DHCP and mDNS. Empty selects the
  //-- device default.
  char hostname[LOGGER_HOSTNAME_MAX];

  //-- Fixed IP: the last octet of the station address, the network part
  //-- taken from the lease the router granted. 0 means no fixed IP (DHCP).
  uint8_t fixedIpLastOctet;

  //-- ------------------------------------------------------------------------
  //-- Meter store and tariffs (appended with the second version 5 extension;
  //-- older blobs leave these at their defaults)
  //-- ------------------------------------------------------------------------

  //-- Minutes between two rfs synchronisations (1 through 60). The store
  //-- task commits the staged ring records at least this often.
  uint16_t rfsSyncIntervalMinutes;

  //-- Manual import (delivered, used) tariff prices in EUR per kWh for the
  //-- two energy tariffs: the fallback for periods the dynamic tariff table
  //-- does not price. The meter's tariff indicator (OBIS 0-0:96.14.0) names
  //-- the tariff active at the moment of a reading and picks which of the
  //-- two prices values that reading's consumed energy. The matching export
  //-- prices live at the end of the struct (appended later).
  float tariff1ImportPricePerKwh;
  float tariff2ImportPricePerKwh;

  //-- Manual gas price in EUR per m³: gas is only ever delivered to the
  //-- home, so it needs no import/export pair, and it is the only gas price
  //-- (the dynamic table's gas price overrides it when present).
  float tariffGasPricePerM3;

  //-- Dynamic tariffs switch. True lets the meter store value the consumed
  //-- and the delivered-back energy with the import and export prices of the
  //-- dynamic tariff table (through the registered tariff calculator);
  //-- periods without a stored price fall back to the manual prices. False
  //-- always uses the manual prices.
  bool dynamicTariffsEnabled;

  //-- ------------------------------------------------------------------------
  //-- Tariff topic identity (appended with the third version 5 extension;
  //-- older blobs leave these at their defaults)
  //-- ------------------------------------------------------------------------

  //-- MQTT client id: announced to the broker when the client connects and
  //-- used as the optional middle segment of the tariff topic. Empty lets
  //-- the client fall back to its generated id and leaves the segment out
  //-- of the topic.
  char mqttClientId[LOGGER_MQTT_CLIENTID_MAX];

  //-- Last segment of the tariff topic (formerly the fixed "newTariffs").
  //-- Empty selects the default "newTariffs".
  char mqttSubTopic[LOGGER_MQTT_TOPIC_MAX];

  //-- ------------------------------------------------------------------------
  //-- Network and transport costs (appended with the fourth version 5
  //-- extension; older blobs leave this at its default)
  //-- ------------------------------------------------------------------------

  //-- Fixed monthly network and transport amount in EUR: the Costs
  //-- presentation of the Months view adds it to every month's electricity
  //-- and gas amounts (the "Netw." column, like the old DSMRlogger32
  //-- history pages). 0 earns nothing until it is filled in.
  float networkCostsPerMonthEuro;

  //-- ------------------------------------------------------------------------
  //-- Export (delivered-back) tariff prices (appended with the fifth version
  //-- 5 extension; older blobs leave these at their default)
  //-- ------------------------------------------------------------------------

  //-- Manual export (returned, delivered back to the grid) tariff prices in
  //-- EUR per kWh for the two energy tariffs: the counterpart of the manual
  //-- import prices above, valued against the energy the meter reports on
  //-- its return registers. The tariff indicator picks which of the two
  //-- applies; the dynamic table's export price overrides both when present.
  float tariff1ExportPricePerKwh;
  float tariff2ExportPricePerKwh;

  //-- ------------------------------------------------------------------------
  //-- TFT display time-out (appended with the sixth version 5 extension;
  //-- older blobs leave this at its default)
  //-- ------------------------------------------------------------------------

  //-- Minutes of physical inactivity before the panel backlight goes off
  //-- (LOGGER_SETTINGS_DISPLAY_TIMEOUT_MIN_MINUTES through
  //-- LOGGER_SETTINGS_DISPLAY_TIMEOUT_MAX_MINUTES). Any board button action
  //-- restarts the timer; an AUX press wakes the panel again.
  uint16_t displayTimeoutMinutes;

  //-- ------------------------------------------------------------------------
  //-- Board display mode (appended with the seventh version 5 extension;
  //-- older blobs leave this at its default)
  //-- ------------------------------------------------------------------------

  //-- True drives the piggyback board ("Piggyback board"): the panel shows
  //-- the debug lines and the power screens and the encoder and buttons are
  //-- handled. False selects "No Display": the panel is never initialised,
  //-- the backlight stays off and the board inputs are not started.
  bool displayEnabled;

  //-- Explicit filler so the blob size grows with the mode flag: without it
  //-- the flag hides in the padding the uint16_t time-out already left behind
  //-- and an up-to-date blob would look exactly like a pre-mode blob.
  uint8_t displayModePadding[3];
} loggerSettingsConfig;

//-- Loads the stored configuration and validates it. Returns ESP_ERR_NOT_FOUND
//-- on first boot or when the stored data cannot be trusted, in which case
//-- reception shall await a valid configuration.
esp_err_t loggerSettingsLoad(loggerSettingsConfig* config);

//-- Validates a complete candidate configuration: selectable GPIO, known
//-- profile with matching country, and per-channel M-Bus modes and types.
bool loggerSettingsValidate(const loggerSettingsConfig* config);

//-- Validates and persists the complete configuration as one operation.
esp_err_t loggerSettingsSave(const loggerSettingsConfig* config);

//-- --------------------------------------------------------------------------
//-- Plain ascii ini export and import (the telnet console's settings
//-- {save|load})
//-- --------------------------------------------------------------------------

//-- Serialises the complete stored configuration to a plain ascii ini file
//-- at the given absolute path: one "[section]" header per group of the
//-- settings view, then one "key = value" line per settings field (text
//-- quoted, numbers plain). The fields: [p1port] dataSource, P1RXPolarity,
//-- P1TXPolarity, interval, country and profile; [display] board
//-- (piggyback or none), orientation and timeoutMinutes;
//-- [mbus] channel1 through channel4 (auto, disabled or a device type);
//-- [mqtt] brokerUrl, brokerPort, user, password, topTopic, clientId,
//-- subTopic and broadcastInterval; [network] hostname and fixedIpLastOctet;
//-- [store] rfsSyncIntervalMinutes; [tariffs] dynamicTariffs, the five
//-- manual prices and networkCostsPerMonth. Returns the number of fields
//-- written; without a valid stored configuration nothing is written and
//-- ESP_ERR_NOT_FOUND is returned.
esp_err_t loggerSettingsExportToFile(const char* absolutePath, uint32_t* outFields);

//-- Reads back a file written by loggerSettingsExportToFile() (the absolute
//-- path again) into the configuration. The currently stored values form the
//-- base: a key the file omits keeps its stored value. An unknown section or
//-- key and a malformed value abort the load, and the complete candidate is
//-- validated before anything is stored, so a hand-edited file can never
//-- leave an invalid configuration in NVS. Returns the number of fields
//-- applied; they apply after a restart.
esp_err_t loggerSettingsImportFromFile(const char* absolutePath, uint32_t* outFields);

//-- Copies the expected M-Bus device declarations (explicit selections only)
//-- into outDevices and returns the number of entries written.
uint8_t loggerSettingsBuildMbusDevices(const loggerSettingsConfig* config,
                                       dsmrMbusDevice* outDevices, uint8_t deviceCapacity);

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
bool loggerSettingsProfileAt(uint16_t index, loggerSettingsProfileInfo* info);

//-- Resolves one profile by stable identifier; false when unknown.
bool loggerSettingsLookupProfile(const char* profileId, loggerSettingsProfileInfo* info);

//-- True when the two letter country code matches a built-in profile.
bool loggerSettingsIsKnownCountry(const char* countryCode);

#endif //-- LOGGER_SETTINGS_H
