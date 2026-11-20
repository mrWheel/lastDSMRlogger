//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_control.c - single control client arbiter
//-- Date: 2026-11-19 · Build: 1
//--
//-- Owner state behind one mutex. The evict callback of the evicted client
//-- runs outside the lock: it hands over to other tasks (the httpd task
//-- closes a session, the telnet task shuts a socket down), and a callback
//-- that blocks must never hold the arbiter.
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
  loggerControlClient client;
  int fd;
} ControlOwner;

static ControlOwner owner;
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
  owner.active = false;
  owner.client = LOGGER_CONTROL_WEB;
  owner.fd = -1;

  for (uint8_t index = 0u; index < LOGGER_CONTROL_CLIENT_COUNT; index++)
  {
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
  loggerControlClient evicted = LOGGER_CONTROL_WEB;
  int evictedFd = -1;
  bool hasEvicted = false;

  if (controlMutex != NULL)
  {
    xSemaphoreTake(controlMutex, portMAX_DELAY);
  }

  if (owner.active && (owner.client == client) && (owner.fd == fd))
  {
    if (controlMutex != NULL)
    {
      xSemaphoreGive(controlMutex);
    }

    return false;
  }

  if (owner.active)
  {
    evicted = owner.client;
    evictedFd = owner.fd;
    hasEvicted = true;
  }

  owner.active = true;
  owner.client = client;
  owner.fd = fd;

  if (controlMutex != NULL)
  {
    xSemaphoreGive(controlMutex);
  }

  if (hasEvicted && (evictFns[evicted] != NULL))
  {
    evictFns[evicted](evicted, evictedFd);
  }

  ESP_LOGI(tag, "control is now with %s client fd %d (was %s fd %d)",
           (client == LOGGER_CONTROL_TELNET) ? "telnet" : "web", fd,
           hasEvicted ? ((evicted == LOGGER_CONTROL_TELNET) ? "telnet" : "web") : "nobody",
           hasEvicted ? evictedFd : -1);
  return true;
}

void loggerControlRelease(loggerControlClient client, int fd)
{
  if (controlMutex != NULL)
  {
    xSemaphoreTake(controlMutex, portMAX_DELAY);
  }

  if (owner.active && (owner.client == client) && (owner.fd == fd))
  {
    owner.active = false;
    owner.fd = -1;
  }

  if (controlMutex != NULL)
  {
    xSemaphoreGive(controlMutex);
  }
}

bool loggerControlGetOwner(loggerControlClient* client, int* fd)
{
  bool active = false;

  if (controlMutex != NULL)
  {
    xSemaphoreTake(controlMutex, portMAX_DELAY);
  }

  active = owner.active;

  if (active)
  {
    *client = owner.client;
    *fd = owner.fd;
  }

  if (controlMutex != NULL)
  {
    xSemaphoreGive(controlMutex);
  }

  return active;
}
