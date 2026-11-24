# lastDSMRlogger - dynamic electricity tariff schedules
# Date: 2026-11-22 · Build: 1
#
# The shared tariff domain: cJSON parses the versioned schedule documents
# (the component was moved to the ESP Component Registry when ESP-IDF 6.0
# dropped it from the core), logger_settings names the MQTT topic prefix and
# logger_store provides the LittleFS mount the binary schedule file lives
# on.

idf_component_register(
  SRCS "logger_tariffs.c"
  INCLUDE_DIRS "include"
  REQUIRES espressif__cjson logger_settings logger_store logger_control
)
