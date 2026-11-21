# Stages storageImage/ into the build tree for the LittleFS partition image,
# leaving macOS junk out: Finder drops .DS_Store (and AppleDouble ._ files)
# into every folder it browses, and those must never land on the meter
# store's volume.
#
# Usage: cmake -DSRC=<storageImage folder> -DDST=<staging folder> -P this.cmake

if(NOT DEFINED SRC OR NOT DEFINED DST)
  message(FATAL_ERROR "SRC and DST must be defined")
endif()

file(REMOVE_RECURSE "${DST}")
file(MAKE_DIRECTORY "${DST}")

# GLOB_RECURSE does not list dot files, so .DS_Store is already out of the
# result; the name filter stays as the explicit guarantee (and also keeps
# AppleDouble ._ companions, which are not dot files).
file(GLOB_RECURSE entries RELATIVE "${SRC}" "${SRC}/*")

foreach(entry ${entries})
  get_filename_component(entryName "${entry}" NAME)

  if((entryName STREQUAL ".DS_Store") OR (entryName MATCHES "^\\._"))
    continue()
  endif()

  get_filename_component(entryDir "${DST}/${entry}" DIRECTORY)
  file(MAKE_DIRECTORY "${entryDir}")
  file(COPY "${SRC}/${entry}" DESTINATION "${entryDir}")
endforeach()
