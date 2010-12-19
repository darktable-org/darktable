include(Prebuilt)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
  include(FindPkgConfig)
  pkg_check_modules(DBUSGLIB REQUIRED dbus-glib-1)
endif (CMAKE_SYSTEM_NAME STREQUAL "Linux")

if (DBUSGLIB_FOUND)
  set(DBUSGLIB ON CACHE BOOL "Build with dbus-glib message bus support.")
endif (DBUSGLIB_FOUND)

if (DBUSGLIB)
  add_definitions(-DLL_DBUS_ENABLED=1)
endif (DBUSGLIB)
