# - Try to find OpenJPEG
# Once done, this will define
#
#  OpenJPEG_FOUND - system has OpenJPEG
#  OpenJPEG_INCLUDE_DIRS - the OpenJPEG include directories
#  OpenJPEG_LIBRARIES - link these to use OpenJPEG

include(LibFindMacros)

# Use pkg-config to get hints about paths
libfind_pkg_check_modules(OpenJPEG_PKGCONF libopenjp2)

# Include dir
find_path(OpenJPEG_INCLUDE_DIR
  NAMES openjpeg.h
  HINTS ${OpenJPEG_PKGCONF_INCLUDE_DIRS}
)

# Finally the library itself
find_library(OpenJPEG_LIBRARY
  NAMES openjp2
  HINTS ${OpenJPEG_PKGCONF_LIBRARY_DIRS}
)

# Set the include dir variables and the libraries and let libfind_process do the rest.
# NOTE: Singular variables for this library, plural for libraries this lib depends on.
set(OpenJPEG_PROCESS_INCLUDES OpenJPEG_INCLUDE_DIR)
set(OpenJPEG_PROCESS_LIBS OpenJPEG_LIBRARY)
libfind_process(OpenJPEG)
