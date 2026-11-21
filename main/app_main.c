//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: app_main.c - application entry
//-- Date: 2026-11-21 · Build: 14
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
const char* PROG_VERSION = "v0.4.20";

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
#include "logger_mqtt.h"
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

//-- --------------------------------------------------------------------------
//-- Network identity and MQTT (settings fields outside the receive path)
//-- --------------------------------------------------------------------------

//-- Hostname announced to DHCP and mDNS when the settings carry none.
#define LOGGER_HOSTNAME_DEFAULT "dsmrlogger"

//-- The configuration last saved (or loaded at boot), kept here because the
//-- network services re-apply it after every station reconnect. All-zero
//-- (no stored settings yet) simply means: default hostname, DHCP.
static loggerSettingsConfig activeNetworkConfig;

//-- True while the station address is the fixed one from the settings.
static bool fixedIpApplied = false;

//-- Lease work requested by a settings save, performed later by the Wi-Fi
//-- task. Re-announcing the hostname or moving the address briefly leaves
//-- the station interface and aborts every open TCP connection on it, so it
//-- must not run inside the save handler: the save could never send its own
//-- reply (the browser would see a reset while the save did succeed).
static volatile bool pendingFixedIp = false;
static volatile bool pendingHostnameAnnounce = false;

//-- Applies the stored hostname and fixed IP to the station interface. Safe
//-- before the join (only the hostname is stored then); after the join a
//-- changed hostname is re-announced through a DHCP re-request and the fixed
//-- IP replaces the granted address (the network part comes from the lease).
static void applyNetworkIdentity(const loggerSettingsConfig* config, bool connected,
                                 bool deferLeaseWork)
{
  esp_netif_t* station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

  if (station == NULL)
  {
    return;
  }

  const char* wanted = (config->hostname[0] != '\0') ? config->hostname : LOGGER_HOSTNAME_DEFAULT;
  const char* current = NULL;

  if ((esp_netif_get_hostname(station, &current) != ESP_OK) || (current == NULL))
  {
    current = "";
  }

  bool hostnameChanged = (strcmp(current, wanted) != 0);

  if (hostnameChanged && (esp_netif_set_hostname(station, wanted) != ESP_OK))
  {
    ESP_LOGW(tag, "hostname '%s' could not be set", wanted);
  }

  if (!connected)
  {
    return;
  }

  esp_netif_ip_info_t ipInfo;

  if (config->fixedIpLastOctet != 0u)
  {
    if (esp_netif_get_ip_info(station, &ipInfo) != ESP_OK)
    {
      return;
    }

    uint8_t* octets = (uint8_t*)&ipInfo.ip.addr;

    if (octets[3] != config->fixedIpLastOctet)
    {
      if (deferLeaseWork)
      {
        pendingFixedIp = true;
        return;
      }

      octets[3] = config->fixedIpLastOctet;
      esp_netif_dhcpc_stop(station);

      if (esp_netif_set_ip_info(station, &ipInfo) == ESP_OK)
      {
        fixedIpApplied = true;
        ESP_LOGI(tag, "station fixed to " IPSTR " (DHCP released)", IP2STR(&ipInfo.ip));
      }
      else
      {
        ESP_LOGW(tag, "fixed IP could not be set, staying on DHCP");
        esp_netif_dhcpc_start(station);
        fixedIpApplied = false;
      }
    }
    else
    {
      fixedIpApplied = true;
    }
  }
  else if (fixedIpApplied)
  {
    if (deferLeaseWork)
    {
      pendingFixedIp = true;
      return;
    }

    esp_netif_dhcpc_start(station);
    fixedIpApplied = false;
    ESP_LOGI(tag, "fixed IP withdrawn, the station is on DHCP again");
  }
  else if (hostnameChanged)
  {
    //-- The lease stands but the name is new: re-request so the router
    //-- records the station under its new hostname.
    if (deferLeaseWork)
    {
      pendingHostnameAnnounce = true;
      return;
    }

    esp_netif_dhcpc_stop(station);
    esp_netif_dhcpc_start(station);
  }
}

//-- Notification from the settings view: the complete configuration was
//-- applied and persisted, so the network services adopt it right away.
static void onSettingsSaved(const loggerSettingsConfig* config)
{
  activeNetworkConfig = *config;

  //-- The lease work is deferred to the Wi-Fi task: it would abort this
  //-- very save's own reply along with every other open connection.
  applyNetworkIdentity(&activeNetworkConfig, wifi_prov_is_connected(), true);
  loggerMqttApplyConfig(config);
}

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

  //-- The station now has a lease: announce the stored hostname and, when a
  //-- fixed last octet is configured, pin the address on top of the lease.
  //-- Nothing is answered on these sockets yet, so the work runs at once.
  applyNetworkIdentity(&activeNetworkConfig, true, false);

  //-- LWIP is up with the join: this is the moment the MQTT client may
  //-- resolve its broker, so publication is released here (it stays off
  //-- while the station is down).
  loggerMqttSetNetworkReady(true);

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

      //-- Publication goes down with the link: without a station there is
      //-- no route to the broker. onWifiConnected releases it again.
      loggerMqttSetNetworkReady(false);
      esp_wifi_connect();
      continue;
    }

    //-- Deferred lease work from a settings save (see applyNetworkIdentity):
    //-- performed here, well after the save has answered its browser.
    if (wifiEverConnected && wifi_prov_is_connected())
    {
      if (pendingFixedIp)
      {
        pendingFixedIp = false;
        applyNetworkIdentity(&activeNetworkConfig, true, false);
      }
      else if (pendingHostnameAnnounce)
      {
        pendingHostnameAnnounce = false;

        //-- Only meaningful while the station stays on DHCP; the fixed-IP
        //-- branch above owns the address when one is configured.
        if (activeNetworkConfig.fixedIpLastOctet == 0u)
        {
          esp_netif_t* station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

          if (station != NULL)
          {
            ESP_LOGI(tag, "re-announcing the hostname to DHCP");
            esp_netif_dhcpc_stop(station);
            esp_netif_dhcpc_start(station);
          }
        }
      }
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

    //-- The network identity takes the stored hostname (the fixed IP waits
    //-- for the first lease) and the MQTT client starts when the settings
    //-- request publication and the station has joined; both re-apply on
    //-- every station reconnect and on every save through the settings view.
    activeNetworkConfig = settings;
    applyNetworkIdentity(&settings, false, false);
    loggerMqttApplyConfig(&settings);
  }
  else
  {
    ESP_LOGI(tag, "no valid stored settings, awaiting configuration");
  }

  //-- The settings view notifies this application after a complete save so
  //-- the services outside the receive path adopt the stored values.
  loggerWebSetSettingsSavedCallback(onSettingsSaved);

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
