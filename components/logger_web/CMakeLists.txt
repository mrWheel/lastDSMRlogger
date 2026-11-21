# lastDSMRlogger - SPA and JSON API over esp_http_server
# Date: 2026-11-21 · Build: 4
#
# The SPA assets are embedded in the firmware image so the device serves the
# single-page web application and its API from the same origin without any
# external service. The single-operator rule (one web or telnet client at a
# time) is decided by the shared logger_control arbiter. The /api/v3 data
# endpoints answer the console's facts too: the device state block comes
# from the telnet console's own getter, the Wi-Fi facts from esp_wifi,
# esp_netif and the provisioner, hence those requirements.

idf_component_register(
  SRCS "logger_web.c"
  INCLUDE_DIRS "include"
  EMBED_TXTFILES "web/index.html" "web/app.js" "web/style.css"
  REQUIRES logger_control logger_receiver logger_settings logger_board logger_store
           logger_telnet
           esp_http_server esp_wifi esp_netif esp_system esp-idf-wifi-provisioner
)
