include(Prebuilt)
include(LibFindMacros)

libfind_pkg_check_modules(DBUSGLIB dbus-glib-1)
foreach(i ${DBUSGLIB_LIBRARIES})
  find_library(_dbusglib_LIBRARY NAMES ${i} HINTS ${DBUSGLIB_LIBRARY_DIRS})
  list(APPEND DBUSGLIB_LIBRARY ${_dbusglib_LIBRARY})
  unset(_dbusglib_LIBRARY CACHE)
endforeach(i)
set(DBUSGLIB_LIBRARIES ${DBUSGLIB_LIBRARY})
unset(DBUSGLIB_LIBRARY CACHE)

if (DBUSGLIB_FOUND)
  set(DBUSGLIB ON CACHE BOOL "Build with dbus-glib message bus support.")
endif (DBUSGLIB_FOUND)

if (DBUSGLIB)
  add_definitions(-DLL_DBUS_ENABLED=1)
endif (DBUSGLIB)
