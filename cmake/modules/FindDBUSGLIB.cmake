include(Prebuilt)

if (STANDALONE)
  include(FindPkgConfig)

  pkg_check_modules(DBUSGLIB REQUIRED dbus-glib-1)

elseif (LINUX)
  use_prebuilt_binary(dbusglib)
  set(DBUSGLIB_FOUND ON FORCE BOOL)
  set(DBUSGLIB_INCLUDE_DIRS
      ${LIBS_PREBUILT_DIR}/${LL_ARCH_DIR}/include/glib-2.0
      )
  # We don't need to explicitly link against dbus-glib itself, because
  # the viewer probes for the system's copy at runtime.
  set(DBUSGLIB_LIBRARIES
      gobject-2.0
      glib-2.0
      )
endif (STANDALONE)

if (DBUSGLIB_FOUND)
  set(DBUSGLIB ON CACHE BOOL "Build with dbus-glib message bus support.")
endif (DBUSGLIB_FOUND)

if (DBUSGLIB)
  add_definitions(-DLL_DBUS_ENABLED=1)
endif (DBUSGLIB)
