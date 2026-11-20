//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_control.h - single control client arbiter
//-- Date: 2026-11-19 · Build: 1
//--
//-- Exactly one operator may control the logger at a time: either one web
//-- browser (its /ws session) or one telnet session. The arbiter tracks that
//-- owner. A server claims control when its client reports itself; the
//-- previous owner is evicted through the evict callback its own server
//-- registered, so this component never needs to know about esp_http_server
//-- or sockets and neither server has to depend on the other.
//-- ==========================================================================

#ifndef LOGGER_CONTROL_H
#define LOGGER_CONTROL_H

#include <stdbool.h>

#include "esp_err.h"

typedef enum
{
  //-- A browser's /ws session on the SPA server.
  LOGGER_CONTROL_WEB = 0,

  //-- The single client of the telnet console server.
  LOGGER_CONTROL_TELNET,

  LOGGER_CONTROL_CLIENT_COUNT
} loggerControlClient;

//-- Called with the evicted owner when another client claims control. The
//-- callback must only hand the message to that client and end its session;
//-- it must not touch the arbiter. It may run on the claiming client's task,
//-- which for a web eviction is the httpd task and for a telnet eviction the
//-- httpd task or the telnet task.
typedef void (*loggerControlEvictFn)(loggerControlClient evicted, int evictedFd);

//-- Creates the arbiter state. Call once from app_main before any server can
//-- start; starting a server without it only degrades the guard, not the
//-- servers themselves.
esp_err_t loggerControlInit(void);

//-- Registers how to evict the current owner of the given kind. Each server
//-- calls this once at start-up, before its first claim.
void loggerControlRegisterEvict(loggerControlClient client, loggerControlEvictFn evict);

//-- Hands control to (client, fd). False when that client already owns
//-- control (a cheap no-op, safe to call on every frame); true when the
//-- owner changed, in which case the previous owner's evict callback has
//-- already run.
bool loggerControlClaim(loggerControlClient client, int fd);

//-- Clears ownership only when (client, fd) is still the owner, so a late
//-- close of an already-evicted client can never clear a newer owner.
void loggerControlRelease(loggerControlClient client, int fd);

//-- Copies the current owner. False when nobody holds control.
bool loggerControlGetOwner(loggerControlClient* client, int* fd);

#endif //-- LOGGER_CONTROL_H
