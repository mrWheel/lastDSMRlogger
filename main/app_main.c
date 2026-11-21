//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: app_main.c - application entry
//-- Date: 2026-11-20 · Build: 11
//--
//-- Boot order: the board debug lines come up first, then NVS, then the P1
//-- receiver. Reception starts as soon as a valid configuration is stored and
//-- continues while Wi-Fi is disconnected. The Wi-Fi provisioner runs in its
//-- own task because its start path blocks while it retries saved credentials;
//-- the SPA server, the telnet console and the SMB share start after the
//-- first successful connection because the captive portal owns port 80
//-- during provisioning.
//-- ==========================================================================

//-- never remove or change in this line,
//-- it indicates the program version (the part "x.y.z" can and will change)
extern const char* PROG_VERSION;
const char* PROG_VERSION = "v0.4.18";

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
#include "logger_control.h"
#include "logger_receiver.h"
#include "logger_settings.h"
#include "logger_store.h"
#include "logger_telnet.h"
#include "logger_web.h"
#include "nvs_flash.h"
#include "smb_server.h"
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

//-- The SMB share exposes the meter store's mounted LittleFS volume to the
//-- local network through mrwheel's smb_server component. The share is
//-- read-only and guest-accessible: a visitor can inspect or copy the ring
//-- file but can never delete or corrupt the logger's own records.
static void startSmbShare(void)
{
  if (smb_server_is_running())
  {
    //-- A repeated station reconnect reports another connect: one share per
    //-- boot is enough.
    return;
  }

  const char* mountPoint = loggerStoreMountPoint();

  if (mountPoint == NULL)
  {
    ESP_LOGW(tag, "storage volume not mounted, the SMB share stays off");
    return;
  }

  smb_server_config_t config = SMB_SERVER_DEFAULT_CONFIG();
  config.share_name = "DSMRLOGGER";
  config.mount_point = mountPoint;
  config.read_only = true;

  esp_err_t result = smb_server_start(&config);

  if (result != ESP_OK)
  {
    ESP_LOGE(tag, "SMB share could not start: %s", esp_err_to_name(result));
    return;
  }

  ESP_LOGI(tag, "SMB share '%s' open on port %u (read-only, guest access)", config.share_name,
           (unsigned)config.port);
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

  //-- The captive portal released port 80 when it stopped: serve the SPA and
  //-- open the telnet console. Each channel arbitrates its own clients: one
  //-- browser page and one telnet session may be active at the same time.
  loggerWebStart();

  esp_err_t telnetStarted = loggerTelnetStart();

  if (telnetStarted != ESP_OK)
  {
    ESP_LOGE(tag, "telnet console could not start: %s", esp_err_to_name(telnetStarted));
  }
  else
  {
    ESP_LOGI(tag, "telnet console open on port 23");
  }

  startSmbShare();
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

  //-- The single-operator arbiter for the web and telnet control channels:
  //-- ready before any server can claim control.
  ESP_ERROR_CHECK(loggerControlInit());

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

  //-- The meter store runs on its own task: it mounts the LittleFS storage
  //-- partition and keeps the last 49 hour, 32 day and 24 month records in
  //-- three buffered ring files, synchronized together at every hour change.
  esp_err_t storeStarted = loggerStoreInit();

  if (storeStarted != ESP_OK)
  {
    ESP_LOGE(tag, "meter store could not start: %s", esp_err_to_name(storeStarted));
  }

  BaseType_t created =
      xTaskCreate(wifiTask, "wifiProvision", WIFI_TASK_STACK_BYTES, NULL, WIFI_TASK_PRIORITY, NULL);

  if (created != pdPASS)
  {
    ESP_LOGE(tag, "could not create the Wi-Fi provisioner task");
  }

  loggerBoardShowScreen("ready");
}
