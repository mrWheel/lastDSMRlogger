# lastDSMRlogger - versioned settings service
# Date: 2026-11-24 · Build: 2

idf_component_register(
  SRCS "logger_settings.c"
  INCLUDE_DIRS "include"
  REQUIRES nvs_flash parserdsmr logger_control
)
