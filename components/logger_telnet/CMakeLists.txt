# lastDSMRlogger - telnet console server
# Date: 2026-11-21 · Build: 2
#
# Plain BSD-socket telnet console on port 23. The single-client rule lives in
# the shared logger_control arbiter, so this component depends on it and on
# the data sources it reports, never on the web server. The state command
# reads chip, flash and running-image facts, hence the system-side
# requirements.

idf_component_register(
  SRCS "logger_telnet.c"
  INCLUDE_DIRS "include"
  REQUIRES logger_control logger_receiver logger_settings logger_store
           lwip esp_netif esp_wifi esp-idf-wifi-provisioner
           esp_system esp_hw_support esp_timer heap spi_flash app_update bootloader_support
)
