# lastDSMRlogger - ESP-IDF project root
# Date: 2026-11-21 · Build: 4
#
# The parserDSMR dependency is pinned to the sibling checkout. The ESP-IDF
# build adds the checkout's components/parserDSMR directory to
# EXTRA_COMPONENT_DIRS and requires the parserDSMR component. The checkout
# preserves the relative symlinks from that component directory to the root
# src/, include/ and adapter/ directories. See README.md for the pinned
# revision and the clone command.

cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../parserDSMR/components/parserDSMR")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lastDSMRlogger)

# The storage partition image is built from storageImage/ at every build
# (joltwallet's littlefs_create_partition_image, its own venv included).
# storageImage/ is the drop folder for the csv files the telnet console's
# populate command imports: files placed in the folder land at the volume
# root (e.g. RINGmonths.csv). Put new input files in there, rebuild
# and land the image on the device with
#   python $IDF_PATH/components/partition_table/parttool.py \
#     -p PORT write_partition --partition-name storage --input build/storage.bin
# FLASH_IN_PROJECT stays off on purpose: a normal firmware re-flash must
# never wipe the meter ring files that live on the same volume.
#
# The image is built from a staging copy of storageImage/, never from the
# folder itself: Finder drops .DS_Store (and AppleDouble ._ files) into every
# folder it browses, and those must never land on the LittleFS volume. The
# staging step mirrors the folder and filters the junk out before the image
# is built.
set(STORAGE_STAGE_DIR "${CMAKE_BINARY_DIR}/storageStage")

add_custom_target(storageStage ALL
  COMMAND ${CMAKE_COMMAND}
    -DSRC=${CMAKE_CURRENT_LIST_DIR}/storageImage
    -DDST=${STORAGE_STAGE_DIR}
    -P ${CMAKE_CURRENT_LIST_DIR}/cmake/StageStorageFolder.cmake
  COMMENT "staging storageImage for the LittleFS image (without .DS_Store)")

littlefs_create_partition_image(storage ${STORAGE_STAGE_DIR})
add_dependencies(littlefs_storage_bin storageStage)
