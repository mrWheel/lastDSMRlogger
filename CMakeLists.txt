# lastDSMRlogger - ESP-IDF project root
# Date: 2026-11-20 · Build: 2
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
# storageImage/assets/ is the drop folder for the csv files the telnet
# console's populate command imports; put new input files in there, rebuild
# and land the image on the device with
#   python $IDF_PATH/components/partition_table/parttool.py \
#     -p PORT write_partition --partition-name storage --input build/storage.bin
# FLASH_IN_PROJECT stays off on purpose: a normal firmware re-flash must
# never wipe the meter ring files that live on the same volume.
littlefs_create_partition_image(storage storageImage)
