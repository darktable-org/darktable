SET(COLORD_FIND_REQUIRED ${Colord_FIND_REQUIRED})

include(Prebuilt)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
  include(FindPkgConfig)
  pkg_check_modules(COLORD colord)
endif (CMAKE_SYSTEM_NAME STREQUAL "Linux")

if (COLORD_FOUND)
  set(COLORD ON CACHE BOOL "Build with libcolord support.")
endif (COLORD_FOUND)
