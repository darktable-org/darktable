include(Prebuilt)

if (PKG_CONFIG_FOUND)
  include(FindPkgConfig)
  pkg_check_modules(DBUSGLIB REQUIRED dbus-glib-1)
endif (PKG_CONFIG_FOUND)

if (DBUSGLIB_FOUND)
  set(DBUSGLIB ON CACHE BOOL "Build with dbus-glib message bus support.")
endif (DBUSGLIB_FOUND)

if (DBUSGLIB)
  add_definitions(-DLL_DBUS_ENABLED=1)
endif (DBUSGLIB)
