# lastDSMRlogger - application component
# Date: 2026-11-14 · Build: 1

idf_component_register(
  SRCS "app_main.c"
  INCLUDE_DIRS "."
  REQUIRES logger_board logger_settings logger_receiver logger_web
           nvs_flash esp_wifi esp-idf-wifi-provisioner
)
