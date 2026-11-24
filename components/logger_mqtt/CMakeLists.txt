# lastDSMRlogger - MQTT publication service and tariff ingestion
# Date: 2026-11-22 · Build: 2

idf_component_register(
  SRCS "logger_mqtt.c"
  INCLUDE_DIRS "include"
  REQUIRES espressif__mqtt logger_receiver logger_settings logger_tariffs logger_control
)
