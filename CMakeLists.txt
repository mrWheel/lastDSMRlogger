# lastDSMRlogger - ESP-IDF project root
# Date: 2026-11-14 · Build: 1
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
