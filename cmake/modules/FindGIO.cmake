# - Try to find GIO 2.0
# Once done, this will define
#
#  GIO_FOUND - system has GIO
#  GIO_INCLUDE_DIRS - the GIO include directories
#  GIO_LIBRARIES - link these to use GIO

include(LibFindMacros)

# Dependencies
libfind_package(GIO Glib)

# Use pkg-config to get hints about paths
libfind_pkg_check_modules(GIO_PKGCONF gio-2.0)

# Include dir
find_path(GIO_INCLUDE_DIR
  NAMES gio/gio.h
  HINTS ${GIO_PKGCONF_INCLUDE_DIRS}
  PATH_SUFFIXES glib-2.0
)

# Find the library
find_library(GIO_LIBRARY
  NAMES gio-2.0
  HINTS ${GIO_PKGCONF_LIBRARY_DIRS}
)

# Set the include dir variables and the libraries and let libfind_process do the rest.
# NOTE: Singular variables for this library, plural for libraries this lib depends on.
set(GIO_PROCESS_INCLUDES GIO_INCLUDE_DIR)
set(GIO_PROCESS_LIBS GIO_LIBRARY)
libfind_process(GIO)
