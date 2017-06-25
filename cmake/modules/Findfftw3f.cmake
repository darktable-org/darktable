# - Try to find 
# Once done, this will define
#
#  _FOUND - system has Glib
#  _INCLUDE_DIRS - the Glib include directories
#  _LIBRARIES - link these to use Glib

include(LibFindMacros)

# Use pkg-config to get hints about paths
libfind_pkg_check_modules(fftw3f_PKGCONF fftw3f)

# Main include dir
find_path(fftw3f_INCLUDE_DIR
  NAMES fftw3.h
  HINTS ${fftw3f_PKGCONF_INCLUDE_DIRS}
#  PATH_SUFFIXES json-glib-1.0
)

# Finally the library itself
find_library(fftw3f
  NAMES fftw3f
  HINTS ${fftw3f_PKGCONF_LIBRARY_DIRS}
)

# Set the include dir variables and the libraries and let libfind_process do the rest.
# NOTE: Singular variables for this library, plural for libraries this this lib depends on.
set(fftw3f_PROCESS_INCLUDES ${fftw3f_INCLUDE_DIR})
set(fftw3f_PROCESS_LIBS ${fftw3f_LIBRARY})
libfind_process(fftw3f)

if(fftw3f_FOUND)
  set(fftw3f_INCLUDE_DIRS ${fftw3f_INCLUDE_DIR})
  set(fftw3f_LIBRARIES ${fftw3f_LIBRARY})
endif(fftw3f_FOUND)


