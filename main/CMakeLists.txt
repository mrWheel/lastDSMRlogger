# lastDSMRlogger - application component
# Date: 2026-11-16 · Build: 2

idf_component_register(
  SRCS "app_main.c"
  INCLUDE_DIRS "."
  REQUIRES logger_board logger_settings logger_receiver logger_web
           nvs_flash esp_wifi esp-idf-wifi-provisioner parserDSMR
)
