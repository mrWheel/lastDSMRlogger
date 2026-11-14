# lastDSMRlogger - P1 receiver, parser ownership and telegram history
# Date: 2026-11-14 · Build: 1

idf_component_register(
  SRCS "logger_receiver.c"
  INCLUDE_DIRS "include"
  REQUIRES parserDSMR logger_settings logger_board esp_timer
)
