# lastDSMRlogger - telnet console server
# Date: 2026-11-22 · Build: 3
#
# Plain BSD-socket telnet console on port 23. The single-client rule lives in
# the shared logger_control arbiter, so this component depends on it and on
# the data sources it reports, never on the web server. The state command
# reads chip, flash and running-image facts, hence the system-side
# requirements. The managed component versions it prints come from the
# idf_component.yml manifests the component manager fetched, injected here as
# compile definitions (see below).

idf_component_register(
  SRCS "logger_telnet.c"
  INCLUDE_DIRS "include"
  REQUIRES logger_control logger_receiver logger_settings logger_store
           lwip esp_netif esp_wifi esp-idf-wifi-provisioner
           esp_system esp_hw_support esp_timer heap spi_flash app_update bootloader_support
)

# --- managed component versions for the status command -----------------------
# The component manager materialises every managed component together with
# its published idf_component.yml. In that manifest the component's own
# "version:" field starts at column 0 while the requirement versions of
# dependencies are indented, so dropping the indented lines leaves exactly
# one version to read. A missing manifest or unreadable field falls back to
# "unknown" so the console keeps building and reporting.

get_filename_component(MANAGED_COMPONENTS_DIR "${CMAKE_CURRENT_LIST_DIR}/../../managed_components"
                       ABSOLUTE)

set(TELNET_COMPONENT_VERSION_DEFS "")

foreach(telnetComponentEntry IN ITEMS
    "PARSERDSMR|mrwheel/parserdsmr|mrwheel__parserdsmr"
    "WIFI_PROVISIONER|michmich/esp-idf-wifi-provisioner|michmich__esp-idf-wifi-provisioner"
    "RING_FILE_STORE|mrwheel/ring_file_store|mrwheel__ring_file_store"
    "LITTLEFS|joltwallet/littlefs|joltwallet__littlefs"
    "SMB_SERVER|mrwheel/smb_server|mrwheel__smb_server"
    "MQTT|espressif/mqtt|espressif__mqtt")
  string(REPLACE "|" ";" telnetComponentParts "${telnetComponentEntry}")
  list(GET telnetComponentParts 0 telnetComponentMacro)
  list(GET telnetComponentParts 1 telnetComponentName)
  list(GET telnetComponentParts 2 telnetComponentDir)

  set(telnetComponentVersion "unknown")
  set(telnetComponentManifest "${MANAGED_COMPONENTS_DIR}/${telnetComponentDir}/idf_component.yml")

  if(EXISTS "${telnetComponentManifest}")
    file(READ "${telnetComponentManifest}" telnetManifestText)
    string(REGEX REPLACE "[ \t]+version:[^\n]*\n" "" telnetManifestText "${telnetManifestText}")
    string(REGEX MATCH "version:[ \t]*\"?([0-9][^\" \t\n]*)" telnetVersionMatch
           "${telnetManifestText}")

    if(CMAKE_MATCH_1)
      set(telnetComponentVersion "${CMAKE_MATCH_1}")
    endif()
  endif()

  message(STATUS "telnet status reports ${telnetComponentName} ${telnetComponentVersion}")
  list(APPEND TELNET_COMPONENT_VERSION_DEFS "${telnetComponentMacro}_COMPONENT_NAME=\"${telnetComponentName}\"")
  list(APPEND TELNET_COMPONENT_VERSION_DEFS "${telnetComponentMacro}_COMPONENT_VERSION=\"${telnetComponentVersion}\"")
endforeach()

target_compile_definitions(${COMPONENT_LIB} PRIVATE "${TELNET_COMPONENT_VERSION_DEFS}")
