//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_control.c - per-channel control client arbiter
//-- Date: 2026-11-20 · Build: 2
//--
//-- One owner slot per control channel behind one mutex, so the web browser
//-- and the telnet console are active at the same time: a new client only
//-- ever displaces the previous owner of its own channel. The evict callback
//-- of the evicted client runs outside the lock: it hands over to other
//-- tasks (the httpd task closes a session, the telnet task shuts a socket
//-- down), and a callback that blocks must never hold the arbiter.
//-- ==========================================================================

#include "logger_control.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char* tag = "loggerControl";

static SemaphoreHandle_t controlMutex;

typedef struct
{
  bool active;
  int fd;
} ControlOwner;

//-- One slot per channel kind: the web browser and the telnet console never
//-- share a slot, each channel arbitrates only its own clients.
static ControlOwner owners[LOGGER_CONTROL_CLIENT_COUNT];
static loggerControlEvictFn evictFns[LOGGER_CONTROL_CLIENT_COUNT];

esp_err_t loggerControlInit(void)
{
  if (controlMutex == NULL)
  {
    controlMutex = xSemaphoreCreateMutex();

    if (controlMutex == NULL)
    {
      ESP_LOGE(tag, "could not create the control mutex");
      return ESP_ERR_NO_MEM;
    }
  }

  xSemaphoreTake(controlMutex, portMAX_DELAY);

  for (uint8_t index = 0u; index < LOGGER_CONTROL_CLIENT_COUNT; index++)
  {
    owners[index].active = false;
    owners[index].fd = -1;
    evictFns[index] = NULL;
  }

  xSemaphoreGive(controlMutex);
  return ESP_OK;
}

void loggerControlRegisterEvict(loggerControlClient client, loggerControlEvictFn evict)
{
  if (client >= LOGGER_CONTROL_CLIENT_COUNT)
  {
    return;
  }

  evictFns[client] = evict;
}

bool loggerControlClaim(loggerControlClient client, int fd)
{
  if (client >= LOGGER_CONTROL_CLIENT_COUNT)
  {
    return false;
  }

  int evictedFd = -1;
  bool hasEvicted = false;

  if (controlMutex != NULL)
  {
    xSemaphoreTake(controlMutex, portMAX_DELAY);
  }

  ControlOwner* slot = &owners[client];

  if (slot->active && (slot->fd == fd))
  {
    if (controlMutex != NULL)
    {
      xSemaphoreGive(controlMutex);
    }

    return false;
  }

  if (slot->active)
  {
    evictedFd = slot->fd;
    hasEvicted = true;
  }

  slot->active = true;
  slot->fd = fd;

  if (controlMutex != NULL)
  {
    xSemaphoreGive(controlMutex);
  }

  if (hasEvicted && (evictFns[client] != NULL))
  {
    evictFns[client](client, evictedFd);
  }

  ESP_LOGI(tag, "%s channel is now with fd %d (was fd %d)",
           (client == LOGGER_CONTROL_TELNET) ? "telnet" : "web", fd, hasEvicted ? evictedFd : -1);
  return true;
}

void loggerControlRelease(loggerControlClient client, int fd)
{
  if (client >= LOGGER_CONTROL_CLIENT_COUNT)
  {
    return;
  }

  if (controlMutex != NULL)
  {
    xSemaphoreTake(controlMutex, portMAX_DELAY);
  }

  if (owners[client].active && (owners[client].fd == fd))
  {
    owners[client].active = false;
    owners[client].fd = -1;
  }

  if (controlMutex != NULL)
  {
    xSemaphoreGive(controlMutex);
  }
}

bool loggerControlGetOwner(loggerControlClient client, int* fd)
{
  if ((client >= LOGGER_CONTROL_CLIENT_COUNT) || (fd == NULL))
  {
    return false;
  }

  bool active = false;

  if (controlMutex != NULL)
  {
    xSemaphoreTake(controlMutex, portMAX_DELAY);
  }

  active = owners[client].active;

  if (active)
  {
    *fd = owners[client].fd;
  }

  if (controlMutex != NULL)
  {
    xSemaphoreGive(controlMutex);
  }

  return active;
}
