# lastDSMRlogger - telnet console server
# Date: 2026-11-19 · Build: 1
#
# Plain BSD-socket telnet console on port 23. The single-client rule lives in
# the shared logger_control arbiter, so this component depends on it and on
# the data sources it reports, never on the web server.

idf_component_register(
  SRCS "logger_telnet.c"
  INCLUDE_DIRS "include"
  REQUIRES logger_control logger_receiver logger_settings logger_store
           lwip esp_netif esp_wifi esp-idf-wifi-provisioner
)
