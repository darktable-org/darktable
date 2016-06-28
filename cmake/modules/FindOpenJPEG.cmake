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
  PATHS ${OpenJPEG_PKGCONF_INCLUDE_DIRS}
  NO_DEFAULT_PATH
)

# at least in debian, libopenjpeg-dev 1:1.5.2-3.1 installs openjpeg.h
# not just as /usr/include/openjpeg-1.5/openjpeg.h, but also as
# /usr/include/openjpeg.h; without NO_DEFAULT_PATH, cmake does not find
# the right openjpeg.h in /usr/include/openjpeg-2.1/openjpeg.h,
# and does not set OpenJPEG_INCLUDE_DIR to /usr/include/openjpeg-2.1,
# but to /usr/include...

# Finally the library itself
find_library(OpenJPEG_LIBRARY
  NAMES openjp2
  PATHS ${OpenJPEG_PKGCONF_LIBRARY_DIRS}
)

# Set the include dir variables and the libraries and let libfind_process do the rest.
# NOTE: Singular variables for this library, plural for libraries this this lib depends on.
set(OpenJPEG_PROCESS_INCLUDES OpenJPEG_INCLUDE_DIR)
set(OpenJPEG_PROCESS_LIBS OpenJPEG_LIBRARY)
libfind_process(OpenJPEG)
