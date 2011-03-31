include(Prebuilt)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
  include(FindPkgConfig)
  pkg_check_modules(VTE vte)
endif (CMAKE_SYSTEM_NAME STREQUAL "Linux")

if (VTE_FOUND)
  set(VTE ON CACHE BOOL "Build with VTE support.")
endif (VTE_FOUND)

# if (VTE)
#   add_definitions(-DVTE)
# endif (VTE)
