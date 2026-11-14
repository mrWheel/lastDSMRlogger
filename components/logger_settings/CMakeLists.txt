# lastDSMRlogger - versioned settings service
# Date: 2026-11-14 · Build: 1

idf_component_register(
  SRCS "logger_settings.c"
  INCLUDE_DIRS "include"
  REQUIRES nvs_flash parserDSMR
)
