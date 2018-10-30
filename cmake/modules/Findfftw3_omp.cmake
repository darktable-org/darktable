# - Find the native fftw3 includes and library
#
# This module defines
#  FFTW3_OMP_INCLUDE_DIR, where to find fftw3.h, etc.
#  FFTW3_OMP_LIBRARIES, the libraries to link against to use fftw3.
#  FFTW3_OMP_FOUND, If false, do not try to use fftw3.
# also defined, but not for general use are
#  FFTW3_OMP_LIBRARY, where to find the fftw3 library.


#=============================================================================
# Copyright 2010 henrik andersson
#=============================================================================

include(LibFindMacros)

SET(FFTW3_OMP_FIND_REQUIRED ${FFTW3_OMP_FIND_REQUIRED})

# Use pkg-config to get hints about paths
libfind_pkg_check_modules(FFTW3_OMP_PKGCONF fftw3_omp)

find_path(FFTW3_OMP_INCLUDE_DIR NAMES fftw3.h
  HINTS ${FFTW3_OMP_PKGCONF_INCLUDE_DIRS}
  /usr/include/fftw3
  /include/fftw3
  ENV FFTW3_OMP_INCLUDE_DIR)
mark_as_advanced(FFTW3_OMP_INCLUDE_DIR)

set(FFTW3_OMP_NAMES ${FFTW3_OMP_NAMES} fftw3f_omp libfftw3_omp)
find_library(FFTW3_OMP_LIBRARY NAMES ${FFTW3_OMP_NAMES}
	HINTS ENV FFTW3_OMP_LIB_DIR)
mark_as_advanced(FFTW3_OMP_LIBRARY)

# handle the QUIETLY and REQUIRED arguments and set FFTW3_OMP_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFTW3_OMP DEFAULT_MSG FFTW3_OMP_LIBRARY FFTW3_OMP_INCLUDE_DIR)

IF(FFTW3_OMP_FOUND)
  SET(FFTW3_OMP_LIBRARIES ${FFTW3_OMP_LIBRARY})
  SET(FFTW3_OMP_INCLUDE_DIRS ${FFTW3_OMP_INCLUDE_DIR})
ENDIF(FFTW3_OMP_FOUND)
