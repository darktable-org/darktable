# - Find the native fftw3 includes and library
#
# This module defines
#  FFTW3_INCLUDE_DIR, where to find fftw3.h, etc.
#  FFTW3_LIBRARIES, the libraries to link against to use fftw3.
#  FFTW3_FOUND, If false, do not try to use fftw3.
# also defined, but not for general use are
#  FFTW3_LIBRARY, where to find the fftw3 library.


#=============================================================================
# Copyright 2010 henrik andersson
#=============================================================================

include(LibFindMacros)

SET(FFTW3_FIND_REQUIRED ${FFTW3_FIND_REQUIRED})

# Use pkg-config to get hints about paths
libfind_pkg_check_modules(FFTW3_PKGCONF fftw3)

find_path(FFTW3_INCLUDE_DIR NAMES fftw3.h
  HINTS ${FFTW3_PKGCONF_INCLUDE_DIRS}
  /usr/include/fftw3
  /include/fftw3
  ENV FFTW3_INCLUDE_DIR)
mark_as_advanced(FFTW3_INCLUDE_DIR)

set(FFTW3_NAMES ${FFTW3_NAMES} fftw3f libfftw3)
find_library(FFTW3_LIBRARY NAMES ${FFTW3_NAMES}
	HINTS ENV FFTW3_LIB_DIR)
mark_as_advanced(FFTW3_LIBRARY)

# handle the QUIETLY and REQUIRED arguments and set FFTW3_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFTW3 DEFAULT_MSG FFTW3_LIBRARY FFTW3_INCLUDE_DIR)

IF(FFTW3_FOUND)
  SET(FFTW3_LIBRARIES ${FFTW3_LIBRARY})
  SET(FFTW3_INCLUDE_DIRS ${FFTW3_INCLUDE_DIR})
ENDIF(FFTW3_FOUND)
