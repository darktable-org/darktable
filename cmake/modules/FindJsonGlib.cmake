# - Try to find JsonGlib-1.0
# Once done, this will define
#
#  JsonGlib_FOUND - system has Glib
#  JsonGlib_INCLUDE_DIRS - the Glib include directories
#  JsonGlib_LIBRARIES - link these to use Glib

include(LibFindMacros)

# Use pkg-config to get hints about paths
libfind_pkg_check_modules(JsonGlib_PKGCONF json-glib-1.0)

# Main include dir
find_path(JsonGlib_INCLUDE_DIR
  NAMES json-glib/json-glib.h
  PATHS ${JsonGlib_PKGCONF_INCLUDE_DIRS}
  PATH_SUFFIXES json-glib-1.0
)

# Finally the library itself
find_library(JsonGlib_LIBRARY
  NAMES json-glib-1.0
  PATHS ${JsonGlib_PKGCONF_LIBRARY_DIRS}
)

if(JsonGlib_PKGCONF_FOUND)
  # Set the include dir variables and the libraries and let libfind_process do the rest.
  # NOTE: Singular variables for this library, plural for libraries this this lib depends on.
  set(JsonGlib_PROCESS_INCLUDES JsonGlib_INCLUDE_DIRS)
  set(JsonGlib_PROCESS_LIBS JsonGlib_LIBRARIES)
  libfind_process(JsonGlib)
else(JsonGlib_PKGCONF_FOUND)
  set(USE_GLIBJSON, off)
  set(JsonGlib_FOUND, off)
endif(JsonGlib_PKGCONF_FOUND)

