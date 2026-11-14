# lastDSMRlogger - on-device debug lines via the board component
# Date: 2026-11-14 · Build: 1

idf_component_register(
  SRCS "logger_board.c"
  INCLUDE_DIRS "include"
  REQUIRES esp32_s3_piggyback
)
