# lastDSMRlogger - MQTT publication service
# Date: 2026-11-21 · Build: 1

idf_component_register(
  SRCS "logger_mqtt.c"
  INCLUDE_DIRS "include"
  REQUIRES espressif__mqtt logger_receiver logger_settings
)
