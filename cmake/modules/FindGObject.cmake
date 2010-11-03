# - Try to find GObject 2.0
# Once done, this will define
#
#  GObject_FOUND - system has GObject
#  GObject_INCLUDE_DIRS - the GObject include directories
#  GObject_LIBRARIES - link these to use GObject

include(LibFindMacros)

# Dependencies
libfind_package(GObject Glib)

# Use pkg-config to get hints about paths
libfind_pkg_check_modules(GObject_PKGCONF gobject-2.0)

# Find the library
find_library(GObject_LIBRARY
  NAMES gobject-2.0
  PATHS ${GObject_PKGCONF_LIBRARY_DIRS}
)

find_path(GObject_INCLUDE_DIR
  NAMES gobject/gobject.h
  PATHS ${GObject_PKGCONF_INCLUDE_DIRS}
  PATHS ${Glib_INCLUDE_DIRS}
)

set(GObject_PROCESS_INCLUDES GObject_INCLUDE_DIR Glib_INCLUDE_DIRS)
set(GObject_PROCESS_LIBS GObject_LIBRARY Glib_LIBRARIES)
libfind_process(GObject)

