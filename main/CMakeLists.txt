# lastDSMRlogger - application component
# Date: 2026-11-23 · Build: 8

idf_component_register(
  SRCS "app_main.c"
  INCLUDE_DIRS "."
  REQUIRES logger_board logger_settings logger_receiver logger_store logger_web logger_telnet
           logger_control logger_mqtt logger_tariffs
           nvs_flash esp_wifi esp_netif esp-idf-wifi-provisioner parserdsmr smb_server mdns
)
