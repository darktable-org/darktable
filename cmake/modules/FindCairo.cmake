# - Try to find Cairo
# Once done, this will define
#
#  Cairo_FOUND - system has Cairo
#  Cairo_INCLUDE_DIRS - the Cairo include directories
#  Cairo_LIBRARIES - link these to use Cairo

include(LibFindMacros)

# Dependencies
libfind_package(Cairo Freetype)

# Use pkg-config to get hints about paths
libfind_pkg_check_modules(Cairo_PKGCONF cairo)

# Include dir
find_path(Cairo_INCLUDE_DIR
  NAMES cairo.h
  PATHS ${Cairo_PKGCONF_INCLUDE_DIRS}
  PATH_SUFFIXES cairo
)

# Finally the library itself
find_library(Cairo_LIBRARY
  NAMES cairo
  PATHS ${Cairo_PKGCONF_LIBRARY_DIRS}
)

# Set the include dir variables and the libraries and let libfind_process do the rest.
# NOTE: Singular variables for this library, plural for libraries this this lib depends on.
set(Cairo_PROCESS_INCLUDES Cairo_INCLUDE_DIR Freetype_INCLUDE_DIRS)
set(Cairo_PROCESS_LIBS Cairo_LIBRARY Freetype_LIBRARIES)
libfind_process(Cairo)

