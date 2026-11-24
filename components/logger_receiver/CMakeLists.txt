# lastDSMRlogger - P1 receiver, parser ownership and telegram history
# Date: 2026-11-24 · Build: 3

idf_component_register(
  SRCS "logger_receiver.c" "logger_testdata.c"
  INCLUDE_DIRS "include"
  REQUIRES parserdsmr logger_settings logger_board logger_control esp_timer nvs_flash
)
