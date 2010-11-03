# - Try to find Freetype2
# Once done, this will define
#
#  Freetype_FOUND - system has Freetype
#  Freetype_INCLUDE_DIRS - the Freetype include directories
#  Freetype_LIBRARIES - link these to use Freetype

include(LibFindMacros)

# Use pkg-config to get hints about paths
libfind_pkg_check_modules(Freetype_PKGCONF freetype2)

# Include dir
find_path(Freetype_INCLUDE_DIR
  NAMES freetype/freetype.h
  PATHS ${Freetype_PKGCONF_INCLUDE_DIRS}
  PATH_SUFFIXES freetype2
)

# Finally the library itself
find_library(Freetype_LIBRARY
  NAMES freetype
  PATHS ${Freetype_PKGCONF_LIBRARY_DIRS}
)

# Set the include dir variables and the libraries and let libfind_process do the rest.
# NOTE: Singular variables for this library, plural for libraries this this lib depends on.
set(Freetype_PROCESS_INCLUDES Freetype_INCLUDE_DIR)
set(Freetype_PROCESS_LIBS Freetype_LIBRARY)
libfind_process(Freetype)

