//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: app_main.c - application entry
//-- Date: 2026-11-25 · Build: 25
//--
//-- Boot order: NVS comes up first and the stored settings are read, because
//-- the stored display mode decides whether the board stays up ("No Display"
//-- is brought up for a reliable boot and torn down once the station has
//-- joined); then the
//-- board debug lines (when enabled), then the P1 receiver. Reception
//-- starts as soon as a valid configuration is stored and
//-- continues while Wi-Fi is disconnected. The Wi-Fi provisioner runs in its
//-- own task because its start path blocks while it retries saved credentials;
//-- the SPA server, the telnet console and the SMB share start after the
//-- first successful connection because the captive portal owns port 80
//-- during provisioning.
//--
//-- The Wi-Fi task also owns runtime link recovery (recoveryWiFi.md): a
//-- connection manager keeps the live association and IP picture, retries a
//-- dropped station with backoff and jitter, watches separate association and
//-- IP deadlines, escalates with a rate-limited adapter restart and brings
//-- every network service back through the regular startup path when the
//-- address is granted again. Keeping the link up is a first-class goal: the
//-- station is the route to the SPA, the console, the SMB share and MQTT.
//-- ==========================================================================

//-- never remove or change in this line,
//-- it indicates the program version (the part "x.y.z" can and will change)
static const char* PROG_VERSION = "v0.6.3";

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "dsmr_parser.h"
#include "logger_board.h"
#include "logger_control.h"
#include "logger_mqtt.h"
#include "logger_receiver.h"
#include "logger_settings.h"
#include "logger_store.h"
#include "logger_tariffs.h"
#include "logger_telnet.h"
#include "logger_web.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "smb_server.h"
#include "wifi_provisioner.h"

static const char* tag = "appMain";

//-- The provisioner's start path blocks while it retries saved credentials,
//-- so it runs in a task of its own and never delays P1 reception.
#define WIFI_TASK_STACK_BYTES 6144
#define WIFI_TASK_PRIORITY 4

//-- The recovery manager wakes on queued link signals and on this tick:
//-- short enough to keep the retry timing honest, long enough to idle.
#define WIFI_MANAGER_TICK_MS 500

//-- Association and IP assignment get separate deadlines so the log names
//-- the phase in which an attempt stalled (recoveryWiFi.md section 6).
#define WIFI_ASSOC_DEADLINE_MS 15000
#define WIFI_IP_DEADLINE_MS 15000

//-- Reconnect backoff: a short first delay, doubled after every failed
//-- attempt up to the cap, plus jitter so several devices that lost the
//-- same AP do not come back in lockstep. The policy resets as soon as the
//-- station has a usable address again.
#define WIFI_RETRY_BASE_MS 2000
#define WIFI_RETRY_MAX_MS 60000
#define WIFI_RETRY_JITTER_FRACTION 4

//-- Escalation for attempts that keep failing while the retry policy itself
//-- runs healthy: one adapter restart per throttle window, through the
//-- manager, always logged (recoveryWiFi.md section 6).
#define WIFI_ADAPTER_RESTART_AFTER_ATTEMPTS 8
#define WIFI_ADAPTER_RESTART_THROTTLE_MS 600000

//-- Safety net behind the events: every window the manager compares its
//-- picture with the driver and netif, so a lost signal cannot stall
//-- recovery for longer than this.
#define WIFI_RECONCILE_MS 30000

static volatile bool wifiEverConnected = false;

//-- Set when the stored display mode is "No Display": the panel comes up
//-- for a reliable boot and the tear-down follows the first join (see
//-- wifiTask and app_main).
static volatile bool pendingDisplayOff = false;

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

//-- Live link state from the recovery manager. The provisioner's
//-- wifi_prov_is_connected() latches its very first success and cannot
//-- report a runtime outage (recoveryWiFi.md section 2), so every "is the
//-- station usable" question in this file answers from here instead.
static volatile bool stationHasIp = false;

//-- True once the mDNS responder is up (one init per boot; the retry path
//-- lives in announceMdns).
static bool mdnsStarted = false;

//-- mDNS: announces the stored hostname so the SPA is reachable as
//-- http://<hostname>.local whatever the DHCP lease currently is, plus an
//-- _http._tcp service so browsers and service browsers find the logger on
//-- the local network without any address knowledge. The responder is
//-- inited once and the hostname is re-set on every application, so a save
//-- that renames the device moves the .local name along with the DHCP
//-- announcement. A failure (typically esp_netif not up yet before the
//-- first join) is not fatal: the next application — at the station join or
//-- on the next save — tries again.
static void announceMdns(const char* hostname)
{
  if (!mdnsStarted)
  {
    esp_err_t result = mdns_init();

    if (result != ESP_OK)
    {
      LOGGER_LOGW(tag, "mDNS could not start yet: %s (retrying at the next application)",
                  esp_err_to_name(result));
      return;
    }

    mdnsStarted = true;
  }

  //-- The hostname must be set before a service is added: the responder
  //-- refuses a service while the server still has no hostname, and the
  //-- service records the host name it was created under — so a rename
  //-- re-creates the service instead of leaving it pointing at the old name.
  mdns_hostname_set(hostname);
  mdns_instance_name_set("lastDSMRlogger");
  mdns_service_remove_all();

  esp_err_t serviceResult = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

  if (serviceResult != ESP_OK)
  {
    LOGGER_LOGW(tag, "mDNS _http._tcp service could not be added: %s",
                esp_err_to_name(serviceResult));
  }
}

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
    LOGGER_LOGW(tag, "hostname '%s' could not be set", wanted);
  }

  //-- The very same name travels over mDNS (see announceMdns): the lease
  //-- address may come and go with the router, the .local name must not.
  announceMdns(wanted);

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
        LOGGER_LOGI(tag, "station fixed to " IPSTR " (DHCP released)", IP2STR(&ipInfo.ip));
      }
      else
      {
        LOGGER_LOGW(tag, "fixed IP could not be set, staying on DHCP");
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
    LOGGER_LOGI(tag, "fixed IP withdrawn, the station is on DHCP again");
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

  //-- A save that re-enables the display cancels a still-pending deferred
  //-- "No Display" boot tear-down: the operator explicitly wants the panel
  //-- up now, so the Wi-Fi task must not switch it off again afterwards.
  if (config->displayEnabled)
  {
    pendingDisplayOff = false;
  }

  //-- The lease work is deferred to the Wi-Fi task: it would abort this
  //-- very save's own reply along with every other open connection.
  applyNetworkIdentity(&activeNetworkConfig, stationHasIp, true);
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
  LOGGER_LOGI(tag, "clock synchronised via %s", SNTP_SERVER);
}

//-- The SMB share exposes the meter store's mounted LittleFS volume to the
//-- local network through mrwheel's smb_server component. The share is
//-- read/write and guest-accessible: a visitor can inspect, copy, replace,
//-- upload and delete files. The ring files the store keeps open stay
//-- protected by their own lock: esp_littlefs refuses to unlink a path whose
//-- file descriptor the firmware still holds, so the live records cannot be
//-- corrupted while the logger runs.
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
    LOGGER_LOGW(tag, "storage volume not mounted, the SMB share stays off");
    return;
  }

  smb_server_config_t config = SMB_SERVER_DEFAULT_CONFIG();
  config.share_name = "DSMRLOGGER";
  config.mount_point = mountPoint;
  config.read_only = false;

  esp_err_t result = smb_server_start(&config);

  if (result != ESP_OK)
  {
    LOGGER_LOGE(tag, "SMB share could not start: %s", esp_err_to_name(result));
    return;
  }

  LOGGER_LOGI(tag, "SMB share '%s' open on port %u (read/write, guest access)", config.share_name,
              (unsigned)config.port);
}

//-- ==========================================================================
//-- Wi-Fi recovery manager (recoveryWiFi.md)
//--
//-- One owner of the station's desired state: the Wi-Fi task. The persistent
//-- event callbacks only copy signals into the queue; the task serialises
//-- reconnects, deadlines, escalation and the (re)start of every dependent
//-- service. The provisioner keeps its own retry handlers scoped to the
//-- initial attempts: runtime recovery stays disabled until its on_connected
//-- callback posts the handoff signal, and the manager takes the live link
//-- picture over from there. Recovery reuses the regular service startup
//-- path (handleStationUp), and every service keeps its own started guard,
//-- so a duplicated signal can never build a second session or listener.
//-- ==========================================================================

typedef enum
{
  WIFI_MGR_HANDOFF,      //-- the provisioner finished: the manager takes over
  WIFI_MGR_STATION_UP,   //-- WIFI_EVENT_STA_CONNECTED (associated, IP pending)
  WIFI_MGR_STATION_DOWN, //-- WIFI_EVENT_STA_DISCONNECTED (reason copied)
  WIFI_MGR_IP_GRANTED,   //-- IP_EVENT_STA_GOT_IP on the station interface
  WIFI_MGR_IP_LOST       //-- IP_EVENT_STA_LOST_IP on the station interface
} wifiManagerEventKind;

typedef struct
{
  wifiManagerEventKind kind;
  uint8_t reason; //-- 802.11 status code of a disconnect, 0 otherwise
} wifiManagerEvent;

#define WIFI_MANAGER_QUEUE_LEN 16

static QueueHandle_t wifiManagerQueue = NULL;

//-- The link picture, written by the Wi-Fi task only; stationHasIp also
//-- answers the live-status questions of the other tasks.
static bool managerActive = false;
static bool stationAssociated = false;
static bool servicesUp = false;

//-- Attempt bookkeeping for the current outage, owned by the Wi-Fi task.
static bool reconnectDue = false;
static int64_t reconnectAtMs = 0;
static uint32_t retryDelayMs = WIFI_RETRY_BASE_MS;
static bool connectRequested = false;
static int64_t connectRequestedAtMs = 0;
static int64_t ipDeadlineMs = 0;
static bool dhcpRetried = false;
static uint32_t failedAttempts = 0;
static int64_t lastAdapterRestartMs = 0;
static int64_t lastReconcileMs = 0;
static volatile bool managerResync = false;

static int64_t managerNowMs(void)
{
  return esp_timer_get_time() / 1000;
}

static void postWifiManagerEvent(wifiManagerEventKind kind, uint8_t reason)
{
  if (wifiManagerQueue == NULL)
  {
    return;
  }

  wifiManagerEvent event;
  event.kind = kind;
  event.reason = reason;

  if (xQueueSend(wifiManagerQueue, &event, 0) != pdTRUE)
  {
    //-- Never block the event task: a dropped signal is covered by the
    //-- manager reconciling with the live driver state (section 5.1).
    managerResync = true;
  }
}

//-- Short callbacks: copy each signal with its reason and get out (section
//-- 5.1). Every decision is taken later, in the manager task.
static void wifiManagerEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data)
{
  (void)arg;

  if ((base == WIFI_EVENT) && (id == WIFI_EVENT_STA_DISCONNECTED))
  {
    wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*)data;
    postWifiManagerEvent(WIFI_MGR_STATION_DOWN, (event != NULL) ? event->reason : 0);
  }
  else if ((base == WIFI_EVENT) && (id == WIFI_EVENT_STA_CONNECTED))
  {
    postWifiManagerEvent(WIFI_MGR_STATION_UP, 0);
  }
  else if ((base == IP_EVENT) && (id == IP_EVENT_STA_GOT_IP))
  {
    ip_event_got_ip_t* event = (ip_event_got_ip_t*)data;
    esp_netif_t* station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

    //-- Only the station interface drives this manager (section 4).
    if ((event != NULL) && (station != NULL) && (event->esp_netif == station))
    {
      postWifiManagerEvent(WIFI_MGR_IP_GRANTED, 0);
    }
  }
  else if ((base == IP_EVENT) && (id == IP_EVENT_STA_LOST_IP))
  {
    postWifiManagerEvent(WIFI_MGR_IP_LOST, 0);
  }
}

static void wifiManagerResetPolicy(void)
{
  failedAttempts = 0;
  retryDelayMs = WIFI_RETRY_BASE_MS;
  reconnectDue = false;
  connectRequested = false;
  ipDeadlineMs = 0;
  dhcpRetried = false;
}

//-- Arms every network-dependent service for the current session. The first
//-- join and every recovery enter through this same regular startup path
//-- (section 5.3) and the started guards inside SNTP, web, telnet, SMB and
//-- the MQTT gate make repeated calls harmless (section 4: idempotent).
static void handleStationUp(void)
{
  if (servicesUp)
  {
    //-- A duplicated address signal (re-granted lease, or the periodic
    //-- reconciliation meeting the join event) must not stack a second
    //-- anything: the first arm of this session stands.
    return;
  }

  servicesUp = true;

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
      LOGGER_LOGW(tag, "SNTP could not start: the header clock stays unavailable");
    }
  }

  wifi_config_t stationConfig;
  memset(&stationConfig, 0, sizeof stationConfig);
  const char* ssid = "?";

  if (esp_wifi_get_config(WIFI_IF_STA, &stationConfig) == ESP_OK)
  {
    ssid = (const char*)stationConfig.sta.ssid;
  }

  esp_netif_t* station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ipInfo;
  char ip[16] = "?";

  memset(&ipInfo, 0, sizeof ipInfo);

  if ((station != NULL) && (esp_netif_get_ip_info(station, &ipInfo) == ESP_OK))
  {
    snprintf(ip, sizeof ip, IPSTR, IP2STR(&ipInfo.ip));
  }

  loggerBoardWifiConnected(ssid, ip);
  LOGGER_LOGI(tag, "station joined '%s' with IP %s, SPA reachable there", ssid, ip);

  //-- The station now has an address: announce the stored hostname and,
  //-- when a fixed last octet is configured, pin the address on top of the
  //-- lease. Nothing is answered on these sockets yet, so the work runs at
  //-- once.
  applyNetworkIdentity(&activeNetworkConfig, true, false);

  //-- LWIP is up with the join: this is the moment the MQTT client may
  //-- resolve its broker, so publication is released here (it stays off
  //-- while the station is down). On a recovery this is the re-arm the old
  //-- code missed: publication must travel again after the outage.
  loggerMqttSetNetworkReady(true);

  //-- The captive portal released port 80 when it stopped: serve the SPA and
  //-- open the telnet console. Each channel arbitrates its own clients: one
  //-- browser page and one telnet session may be active at the same time.
  loggerWebStart();

  esp_err_t telnetStarted = loggerTelnetStart();

  if (telnetStarted != ESP_OK)
  {
    LOGGER_LOGE(tag, "telnet console could not start: %s", esp_err_to_name(telnetStarted));
  }
  else
  {
    LOGGER_LOGI(tag, "telnet console open on port 23");
  }

  startSmbShare();
}

//-- One reconnect slot per outage: the next attempt gets a due time, the
//-- backoff doubles for the failure after this one and the phase timers
//-- start over (sections 4 and 6).
static void wifiManagerScheduleReconnect(const char* cause)
{
  int64_t now = managerNowMs();
  uint32_t jitterMs = 0;

  if (retryDelayMs > 0)
  {
    jitterMs = (uint32_t)(esp_random() % (retryDelayMs / WIFI_RETRY_JITTER_FRACTION + 1));
  }

  reconnectAtMs = now + (int64_t)retryDelayMs + (int64_t)jitterMs;
  reconnectDue = true;
  connectRequested = false;
  ipDeadlineMs = 0;
  dhcpRetried = false;
  failedAttempts++;

  LOGGER_LOGW(tag,
              "station down (%s): attempt %" PRIu32 " in %" PRId64 " ms (backoff %" PRIu32 " ms)",
              cause, failedAttempts, (int64_t)retryDelayMs + (int64_t)jitterMs, retryDelayMs);

  uint32_t grown = retryDelayMs * 2u;
  retryDelayMs = (grown > WIFI_RETRY_MAX_MS) ? WIFI_RETRY_MAX_MS : grown;
}

//-- Compares the manager's picture with the driver and netif state, so a
//-- lost event or an overflowed queue cannot stall recovery (sections 4 and
//-- 5.1). Uses the documented live checks: association, interface and
//-- address.
static void wifiManagerResync(void)
{
  esp_netif_t* station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ipInfo;
  bool hasIp = false;

  memset(&ipInfo, 0, sizeof ipInfo);

  if ((station != NULL) && (esp_netif_get_ip_info(station, &ipInfo) == ESP_OK))
  {
    hasIp = (ipInfo.ip.addr != 0);
  }

  wifi_ap_record_t apRecord;
  bool associated = (esp_wifi_sta_get_ap_info(&apRecord) == ESP_OK);

  if (hasIp)
  {
    stationAssociated = true;
    stationHasIp = true;

    //-- Arm on 'address present but services not armed', never on a
    //-- perceived change: the very first GOT_IP may have arrived before
    //-- the handoff (section 2.3), so by reconciliation time the picture
    //-- can be right while nothing was ever started. The guard inside
    //-- handleStationUp keeps repeated calls harmless.
    if (managerActive && !servicesUp)
    {
      wifiManagerResetPolicy();
      LOGGER_LOGI(tag, "station address confirmed: arming the network services");
      handleStationUp();
    }

    return;
  }

  if (stationHasIp)
  {
    stationHasIp = false;

    if (managerActive)
    {
      LOGGER_LOGW(tag, "address gone (seen by the link reconciliation)");
      servicesUp = false;
      loggerMqttSetNetworkReady(false);
    }
  }

  if (!associated)
  {
    stationAssociated = false;
  }

  if (managerActive && !associated && !reconnectDue && !connectRequested)
  {
    wifiManagerScheduleReconnect("link reconciled down");
  }
}

static void wifiManagerProcessEvent(const wifiManagerEvent* event)
{
  int64_t now = managerNowMs();

  switch (event->kind)
  {
  case WIFI_MGR_HANDOFF:
    if (managerActive)
    {
      break;
    }

    managerActive = true;
    wifiManagerResetPolicy();
    lastReconcileMs = now;

    LOGGER_LOGI(tag, "provisioning done: the recovery manager owns the station link");

    //-- The first GOT_IP may have arrived before the handoff and the link
    //-- may even have dropped since: read the live picture and let it
    //-- schedule recovery when the address is not there (section 2.3).
    wifiManagerResync();
    break;

  case WIFI_MGR_STATION_UP:
    //-- Associated, the address is next: start the IP deadline.
    stationAssociated = true;
    connectRequested = false;
    ipDeadlineMs = now + WIFI_IP_DEADLINE_MS;
    dhcpRetried = false;
    break;

  case WIFI_MGR_STATION_DOWN:
  {
    bool wasAssociated = stationAssociated;
    bool hadIp = stationHasIp;

    stationAssociated = false;
    stationHasIp = false;
    connectRequested = false;

    if (!managerActive)
    {
      //-- The provisioner owns retries while it is setting the station up.
      break;
    }

    LOGGER_LOGW(tag, "station dropped (reason %" PRIu8 ", was associated %s, had address %s)",
                event->reason, wasAssociated ? "yes" : "no", hadIp ? "yes" : "no");

    //-- Publication goes down with the link: without a station there is
    //-- no route to the broker. handleStationUp releases it again.
    servicesUp = false;
    loggerMqttSetNetworkReady(false);

    if (!reconnectDue)
    {
      wifiManagerScheduleReconnect("link lost");
    }
    break;
  }

  case WIFI_MGR_IP_GRANTED:
    //-- The outage is over: reset the backoff, cancel every pending
    //-- attempt and arm the sessions through the regular path.
    wifiManagerResetPolicy();
    stationAssociated = true;
    stationHasIp = true;

    if (managerActive)
    {
      handleStationUp();
    }
    break;

  case WIFI_MGR_IP_LOST:
    if (!stationHasIp)
    {
      break;
    }

    stationHasIp = false;

    if (!managerActive)
    {
      break;
    }

    LOGGER_LOGW(tag, "station lost its address while still associated");
    servicesUp = false;
    loggerMqttSetNetworkReady(false);
    ipDeadlineMs = now + WIFI_IP_DEADLINE_MS;
    dhcpRetried = false;
    break;
  }
}

static void wifiManagerTick(void)
{
  int64_t now = managerNowMs();

  if (managerResync)
  {
    managerResync = false;
    wifiManagerResync();
  }

  //-- "No Display" boot: now that the station has joined, remove the panel
  //-- and the board inputs again (a tear-down while the network stack is
  //-- still coming up crashes the logger, so this waits).
  if (pendingDisplayOff && wifiEverConnected)
  {
    pendingDisplayOff = false;
    loggerBoardSetEnabled(false);
  }

  //-- Deferred lease work from a settings save (see applyNetworkIdentity):
  //-- performed here, well after the save has answered its browser.
  if (stationHasIp)
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
          LOGGER_LOGI(tag, "re-announcing the hostname to DHCP");
          esp_netif_dhcpc_stop(station);
          esp_netif_dhcpc_start(station);
        }
      }
    }
  }

  if (!managerActive)
  {
    //-- Runtime reconnect actions stay disabled while the provisioner is
    //-- setting up (section 2.1).
    return;
  }

  if (!stationHasIp)
  {
    if (connectRequested)
    {
      if ((now - connectRequestedAtMs) >= WIFI_ASSOC_DEADLINE_MS)
      {
        LOGGER_LOGW(tag, "association deadline expired");
        wifiManagerScheduleReconnect("association deadline");
      }
    }
    else if (stationAssociated)
    {
      //-- Associated with no address: wait with a deadline, re-request DHCP
      //-- once, then break the association and let the retry policy start a
      //-- fresh attempt (section 4, second row).
      if (now >= ipDeadlineMs)
      {
        if (!dhcpRetried)
        {
          dhcpRetried = true;
          ipDeadlineMs = now + WIFI_IP_DEADLINE_MS;

          LOGGER_LOGW(tag, "IP deadline expired: re-requesting the address from DHCP");

          esp_netif_t* station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

          if (station != NULL)
          {
            esp_netif_dhcpc_stop(station);
            esp_netif_dhcpc_start(station);
          }
        }
        else
        {
          LOGGER_LOGW(tag, "IP deadline expired again: restarting the association");
          stationAssociated = false;
          esp_wifi_disconnect();
          wifiManagerScheduleReconnect("IP deadline");
        }
      }
    }
    else if (reconnectDue && (now >= reconnectAtMs))
    {
      if ((failedAttempts >= WIFI_ADAPTER_RESTART_AFTER_ATTEMPTS) &&
          ((now - lastAdapterRestartMs) >= WIFI_ADAPTER_RESTART_THROTTLE_MS))
      {
        lastAdapterRestartMs = now;

        LOGGER_LOGW(tag, "connection keeps failing: restarting the Wi-Fi adapter (escalation)");
        esp_wifi_stop();
        esp_wifi_start();
      }

      esp_err_t result = esp_wifi_connect();

      if (result == ESP_OK)
      {
        connectRequested = true;
        connectRequestedAtMs = now;
        LOGGER_LOGI(tag, "reconnect attempt %" PRIu32 " requested", failedAttempts);
      }
      else
      {
        //-- Check the request result before waiting for its outcome
        //-- (section 5.2).
        LOGGER_LOGW(tag, "connect request rejected: %s", esp_err_to_name(result));
        wifiManagerScheduleReconnect("connect request rejected");
      }
    }
  }

  if ((now - lastReconcileMs) >= WIFI_RECONCILE_MS)
  {
    lastReconcileMs = now;
    wifiManagerResync();
  }
}

static void onWifiConnected(void)
{
  wifiEverConnected = true;

  //-- The provisioner only ever calls this after a successful join: stored
  //-- credentials at boot or a completed portal setup. Hand the runtime link
  //-- over to the recovery manager — the callback runs on the provisioner's
  //-- context, so it only posts; the manager reads the live association and
  //-- IP state itself when it handles the handoff (section 2.3).
  postWifiManagerEvent(WIFI_MGR_HANDOFF, 0);
}

static void onWifiPortalStart(void)
{
  loggerBoardWifiPortal("http://192.168.4.1/");
  LOGGER_LOGI(tag, "captive portal open: connect to the setup AP and visit http://192.168.4.1/");
}

static void wifiTask(void* argument)
{
  (void)argument;

  wifiManagerQueue = xQueueCreate(WIFI_MANAGER_QUEUE_LEN, sizeof(wifiManagerEvent));

  if (wifiManagerQueue == NULL)
  {
    LOGGER_LOGE(tag, "could not create the Wi-Fi recovery queue");
    vTaskDelete(NULL);
    return;
  }

  //-- Bring up NVS, ESP-NETIF and the default event loop once before
  //-- subscribing, coordinated with the application start (section 2.1);
  //-- wifi_prov_start() reuses the very same initialised resources.
  esp_err_t initialised = wifi_prov_init();

  if (initialised != ESP_OK)
  {
    LOGGER_LOGE(tag, "Wi-Fi stack could not initialise: %s", esp_err_to_name(initialised));
  }

  //-- The persistent link handlers are registered before the provisioner
  //-- starts connecting, so not even the very first signals are missed;
  //-- until the handoff every signal only updates the picture and never
  //-- triggers a reconnect, because the component owns retries during
  //-- provisioning. The instances live for the whole boot.
  esp_event_handler_instance_t stationDownHandler = NULL;
  esp_event_handler_instance_t stationUpHandler = NULL;
  esp_event_handler_instance_t ipGrantedHandler = NULL;
  esp_event_handler_instance_t ipLostHandler = NULL;

  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                      &wifiManagerEventHandler, NULL,
                                                      &stationDownHandler));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &wifiManagerEventHandler, NULL, &stationUpHandler));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiManagerEventHandler, NULL, &ipGrantedHandler));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_LOST_IP, &wifiManagerEventHandler, NULL, &ipLostHandler));

  wifi_prov_config_t provisionerConfig = WIFI_PROV_DEFAULT_CONFIG();
  provisionerConfig.ap_ssid = "lastDSMRlogger-Setup";
  provisionerConfig.on_connected = onWifiConnected;
  provisionerConfig.on_portal_start = onWifiPortalStart;

  esp_err_t result = wifi_prov_start(&provisionerConfig);

  if (result != ESP_OK)
  {
    LOGGER_LOGE(tag, "provisioner start failed: %s", esp_err_to_name(result));
  }

  //-- The manager loop: every queued signal is handled first, then the tick
  //-- runs the timers (backoff, association and IP deadlines, the periodic
  //-- reconciliation) and the deferred lease work. P1 telegrams keep being
  //-- processed while the station is disconnected.
  for (;;)
  {
    wifiManagerEvent event;

    if (xQueueReceive(wifiManagerQueue, &event, pdMS_TO_TICKS(WIFI_MANAGER_TICK_MS)) == pdTRUE)
    {
      wifiManagerProcessEvent(&event);
    }

    wifiManagerTick();
  }
}

static void initialiseNvs(void)
{
  esp_err_t result = nvs_flash_init();

  if ((result == ESP_ERR_NVS_NO_FREE_PAGES) || (result == ESP_ERR_NVS_NEW_VERSION_FOUND))
  {
    LOGGER_LOGW(tag, "NVS unusable, erasing and reinitialising");
    ESP_ERROR_CHECK(nvs_flash_erase());
    result = nvs_flash_init();
  }

  ESP_ERROR_CHECK(result);
}

//-- Read-only accessor for the firmware version. The components read the
//-- version through this getter instead of extern-ing the global PROG_VERSION,
//-- which keeps the application string static and free of any name clash with
//-- a component that defines a PROG_VERSION of its own.
const char* getAppVersion(void)
{
  return PROG_VERSION;
}

//-- Read-only live link state for the status reporters. The provisioner's
//-- wifi_prov_is_connected() latches its very first success and its IP
//-- getter can keep reporting the last address over an outage, so the SPA
//-- status and the telnet `status` answer the Wi-Fi questions from the
//-- recovery manager's live picture instead (recoveryWiFi.md section 2).
bool wifiManagerStationHasIp(void)
{
  return stationHasIp;
}

void app_main(void)
{
  //-- Serial quiet mode: the console shows warnings and errors only. The
  //-- tariff store stays at info level so every tariff batch fetched through
  //-- the HTTP API or MQTT remains visible, and the telnet console keeps its
  //-- per-session connect and disconnect lines.
  esp_log_level_set("*", ESP_LOG_WARN);
  esp_log_level_set("loggerTariffs", ESP_LOG_INFO);
  esp_log_level_set("loggerTelnet", ESP_LOG_INFO);

  LOGGER_LOGI(tag, "lastDSMRlogger %s starting", PROG_VERSION);
  LOGGER_LOGI(tag, "parserDSMR library version %s", dsmrParserVersion());

  initialiseNvs();

  //-- The board is brought up from the stored settings: the display mode
  //-- decides whether the panel and the board inputs are driven ("No
  //-- Display" leaves them off). Without stored settings the piggyback
  //-- board is the default, the boot debug lines being part of the product;
  //-- a later save can switch the mode without a restart.
  //--
  //-- The panel is always brought up while the logger starts: a boot that
  //-- never drives the panel crashes before the network comes up, so the
  //-- tear-down of the "No Display" mode waits until the station has
  //-- joined, when a tear-down is known to be safe (the settings view
  //-- toggles it the same way at runtime).
  loggerSettingsConfig settings;
  bool haveSettings = (loggerSettingsLoad(&settings) == ESP_OK);
  bool displayEnabled = haveSettings ? settings.displayEnabled : true;

  loggerBoardSetEnabled(true);

  if (!displayEnabled)
  {
    pendingDisplayOff = true;
  }

  loggerBoardShowScreen("boot");

  //-- The parser library version rides along on the boot debug lines so the
  //-- panel shows which parser build is decoding the telegrams.
  loggerBoardShowParserVersion(dsmrParserVersion());

  //-- The single-operator arbiter for the web and telnet control channels:
  //-- ready before any server can claim control.
  ESP_ERROR_CHECK(loggerControlInit());

  ESP_ERROR_CHECK(loggerReceiverInit());

  //-- On first boot, or when saved settings cannot be loaded, reception
  //-- awaits a valid configuration from the settings view.
  if (haveSettings)
  {
    //-- The panel orientation and the idle display time-out are part of the
    //-- stored configuration: apply them before reception takes the screen
    //-- over.
    loggerBoardSetRotation(settings.displayFlipped);
    loggerBoardSetDisplayTimeoutMinutes(settings.displayTimeoutMinutes);

    esp_err_t started = loggerReceiverStart(&settings);

    if (started != ESP_OK)
    {
      LOGGER_LOGE(tag, "receiver could not start with stored settings: %s",
                  esp_err_to_name(started));
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
    LOGGER_LOGI(tag, "no valid stored settings, awaiting configuration");
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
    LOGGER_LOGE(tag, "meter store could not start: %s", esp_err_to_name(storeStarted));
  }

  //-- The dynamic electricity tariffs: shared ingestion for the MQTT
  //-- tariff topic and the API, with the accepted schedule persisted on the
  //-- meter store's volume (loaded lazily once that volume is mounted).
  esp_err_t tariffsStarted = loggerTariffsInit();

  if (tariffsStarted != ESP_OK)
  {
    LOGGER_LOGE(tag, "tariffs could not start: %s", esp_err_to_name(tariffsStarted));
  }
  else
  {
    //-- The meter store values its ring records through the tariff
    //-- calculator: dynamic tariff prices with the manual fallback.
    loggerStoreSetTariffCalculator(loggerTariffsMeterAmountCalculator);
  }

  BaseType_t created =
      xTaskCreate(wifiTask, "wifiProvision", WIFI_TASK_STACK_BYTES, NULL, WIFI_TASK_PRIORITY, NULL);

  if (created != pdPASS)
  {
    LOGGER_LOGE(tag, "could not create the Wi-Fi provisioner task");
  }

  loggerBoardShowScreen("ready");
}
