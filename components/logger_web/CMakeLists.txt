# lastDSMRlogger - SPA and JSON API over esp_http_server
# Date: 2026-11-14 · Build: 1
#
# The SPA assets are embedded in the firmware image so the device serves the
# single-page web application and its API from the same origin without any
# external service.

idf_component_register(
  SRCS "logger_web.c"
  INCLUDE_DIRS "include"
  EMBED_TXTFILES "web/index.html" "web/app.js" "web/style.css"
  REQUIRES logger_receiver logger_settings logger_board esp_http_server
)
