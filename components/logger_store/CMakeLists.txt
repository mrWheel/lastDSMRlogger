# lastDSMRlogger - per-minute smart meter ring file store
# Date: 2026-11-19 · Build: 1

idf_component_register(
  SRCS "logger_store.c"
  INCLUDE_DIRS "include"
  REQUIRES ring_file_store littlefs logger_receiver
)
