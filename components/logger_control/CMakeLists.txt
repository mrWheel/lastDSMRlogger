# lastDSMRlogger - single control client arbiter
# Date: 2026-11-19 · Build: 1
#
# Tiny shared arbiter: exactly one operator (web WebSocket or telnet session)
# may hold control of the logger at a time. It has no dependencies of its own
# so both server components can require it without a cycle.

idf_component_register(
  SRCS "logger_control.c"
  INCLUDE_DIRS "include"
)
