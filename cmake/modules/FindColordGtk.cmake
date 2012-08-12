SET(COLORDGTK_FIND_REQUIRED ${ColordGtk_FIND_REQUIRED})

include(Prebuilt)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
  include(FindPkgConfig)
  pkg_check_modules(COLORDGTK colord-gtk)
endif (CMAKE_SYSTEM_NAME STREQUAL "Linux")

if (COLORDGTK_FOUND)
  set(COLORDGTK ON CACHE BOOL "Build with libcolord support.")
endif (COLORDGTK_FOUND)
