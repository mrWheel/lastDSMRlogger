//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: app_main.c - application entry
//-- Date: 2026-11-16 · Build: 5
//--
//-- Boot order: the board debug lines come up first, then NVS, then the P1
//-- receiver. Reception starts as soon as a valid configuration is stored and
//-- continues while Wi-Fi is disconnected. The Wi-Fi provisioner runs in its
//-- own task because its start path blocks while it retries saved credentials;
//-- the SPA server starts after the first successful connection because the
//-- captive portal owns port 80 during provisioning.
//-- ==========================================================================

//-- never remove or change in this line,
//-- it indicates the program version (the part "x.y.z" can and will change)
const char* PROG_VERSION = "v0.4.6";

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dsmr_parser.h"
#include "logger_board.h"
#include "logger_receiver.h"
#include "logger_settings.h"
#include "logger_web.h"
#include "nvs_flash.h"
#include "wifi_provisioner.h"

static const char* tag = "appMain";

//-- The provisioner's start path blocks while it retries saved credentials,
//-- so it runs in a task of its own and never delays P1 reception.
#define WIFI_TASK_STACK_BYTES 6144
#define WIFI_TASK_PRIORITY 4
#define WIFI_RECONNECT_POLL_MS 2000

static volatile bool wifiEverConnected = false;

//-- Wall clock: SNTP sets the system clock once the station has joined. The
//-- European time zone with the EU daylight-saving rules lets the SPA header
//-- show the local date and time the way the user reads it.
#define LOGGER_TIMEZONE "CET-1CEST,M3.5.0/2,M10.5.0/3"
#define SNTP_SERVER "pool.ntp.org"

static volatile bool sntpStarted = false;

static void onSntpSynced(struct timeval* tv)
{
  (void)tv;
  ESP_LOGI(tag, "clock synchronised via %s", SNTP_SERVER);
}

static void onWifiConnected(void)
{
  wifiEverConnected = true;

  //-- One start per boot is enough: the SNTP service keeps refreshing the
  //-- system time on its own once it runs.
  if (!sntpStarted)
  {
    sntpStarted = true;
    setenv("TZ", LOGGER_TIMEZONE, 1);
    tzset();

    esp_sntp_config_t sntpConfig = ESP_NETIF_SNTP_DEFAULT_CONFIG(SNTP_SERVER);
    sntpConfig.sync_cb = onSntpSynced;

    if (esp_netif_sntp_init(&sntpConfig) != ESP_OK)
    {
      ESP_LOGW(tag, "SNTP could not start: the header clock stays unavailable");
    }
  }

  wifi_config_t stationConfig;
  memset(&stationConfig, 0, sizeof stationConfig);
  const char* ssid = "?";

  if (esp_wifi_get_config(WIFI_IF_STA, &stationConfig) == ESP_OK)
  {
    ssid = (const char*)stationConfig.sta.ssid;
  }

  esp_netif_ip_info_t ipInfo;
  char ip[16] = "?";

  if (wifi_prov_get_ip_info(&ipInfo) == ESP_OK)
  {
    snprintf(ip, sizeof ip, IPSTR, IP2STR(&ipInfo.ip));
  }

  loggerBoardWifiConnected(ssid, ip);
  ESP_LOGI(tag, "station joined '%s' with IP %s, SPA reachable there", ssid, ip);

  //-- The captive portal released port 80 when it stopped: serve the SPA.
  loggerWebStart();
}

static void onWifiPortalStart(void)
{
  loggerBoardWifiPortal("http://192.168.4.1/");
  ESP_LOGI(tag, "captive portal open: connect to the setup AP and visit http://192.168.4.1/");
}

static void wifiTask(void* argument)
{
  (void)argument;

  wifi_prov_config_t provisionerConfig = WIFI_PROV_DEFAULT_CONFIG();
  provisionerConfig.ap_ssid = "lastDSMRlogger-Setup";
  provisionerConfig.on_connected = onWifiConnected;
  provisionerConfig.on_portal_start = onWifiPortalStart;

  esp_err_t result = wifi_prov_start(&provisionerConfig);

  if (result != ESP_OK)
  {
    ESP_LOGE(tag, "provisioner start failed: %s", esp_err_to_name(result));
  }

  //-- Keep the link up: the provisioner unregisters its retry handler after
  //-- the first successful join. Reconnect only after that first connection
  //-- so the component owns retries during provisioning. P1 telegrams keep
  //-- being processed while the station is disconnected.
  for (;;)
  {
    vTaskDelay(pdMS_TO_TICKS(WIFI_RECONNECT_POLL_MS));

    if (wifiEverConnected && !wifi_prov_is_connected())
    {
      ESP_LOGW(tag, "station disconnected, requesting a reconnect");
      esp_wifi_connect();
    }
  }
}

static void initialiseNvs(void)
{
  esp_err_t result = nvs_flash_init();

  if ((result == ESP_ERR_NVS_NO_FREE_PAGES) || (result == ESP_ERR_NVS_NEW_VERSION_FOUND))
  {
    ESP_LOGW(tag, "NVS unusable, erasing and reinitialising");
    ESP_ERROR_CHECK(nvs_flash_erase());
    result = nvs_flash_init();
  }

  ESP_ERROR_CHECK(result);
}

void app_main(void)
{
  ESP_LOGI(tag, "lastDSMRlogger %s starting", PROG_VERSION);
  ESP_LOGI(tag, "parserDSMR library version %s", dsmrParserVersion());

  loggerBoardInit();
  loggerBoardShowScreen("boot");

  //-- The parser library version rides along on the boot debug lines so the
  //-- panel shows which parser build is decoding the telegrams.
  loggerBoardShowParserVersion(dsmrParserVersion());

  initialiseNvs();

  ESP_ERROR_CHECK(loggerReceiverInit());

  //-- On first boot, or when saved settings cannot be loaded, reception
  //-- awaits a valid configuration from the settings view.
  loggerSettingsConfig settings;

  if (loggerSettingsLoad(&settings) == ESP_OK)
  {
    //-- The panel orientation is part of the stored configuration: apply it
    //-- before reception takes the screen over.
    loggerBoardSetRotation(settings.displayFlipped);

    esp_err_t started = loggerReceiverStart(&settings);

    if (started != ESP_OK)
    {
      ESP_LOGE(tag, "receiver could not start with stored settings: %s", esp_err_to_name(started));
    }
  }
  else
  {
    ESP_LOGI(tag, "no valid stored settings, awaiting configuration");
  }

  BaseType_t created =
      xTaskCreate(wifiTask, "wifiProvision", WIFI_TASK_STACK_BYTES, NULL, WIFI_TASK_PRIORITY, NULL);

  if (created != pdPASS)
  {
    ESP_LOGE(tag, "could not create the Wi-Fi provisioner task");
  }

  loggerBoardShowScreen("ready");
}
