# lastDSMRlogger - SPA and JSON API over esp_http_server
# Date: 2026-11-19 · Build: 3
#
# The SPA assets are embedded in the firmware image so the device serves the
# single-page web application and its API from the same origin without any
# external service. The single-operator rule (one web or telnet client at a
# time) is decided by the shared logger_control arbiter.

idf_component_register(
  SRCS "logger_web.c"
  INCLUDE_DIRS "include"
  EMBED_TXTFILES "web/index.html" "web/app.js" "web/style.css"
  REQUIRES logger_control logger_receiver logger_settings logger_board logger_store
           esp_http_server
)
