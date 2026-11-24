# lastDSMRlogger - per-channel control client arbiter
# Date: 2026-11-20 · Build: 2
#
# Tiny shared arbiter: exactly one operator per channel (one web WebSocket
# session and one telnet session) may hold control of the logger, so both
# channels are active at the same time. It has no dependencies of its own
# so both server components can require it without a cycle.

idf_component_register(
  SRCS "logger_control.c"
  INCLUDE_DIRS "include"
  REQUIRES lwip
)
