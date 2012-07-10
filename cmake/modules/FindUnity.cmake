SET(UNITY_FIND_REQUIRED ${Unity_FIND_REQUIRED})

include(Prebuilt)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
  include(FindPkgConfig)
  pkg_check_modules(UNITY unity)
endif (CMAKE_SYSTEM_NAME STREQUAL "Linux")

if (UNITY_FOUND)
  set(UNITY ON CACHE BOOL "Build with libunity support.")
endif (UNITY_FOUND)
