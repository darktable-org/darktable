# - Try to find FFTW3
# Once done, this will define
#
#  FFTW3_FOUND - system has FFTW3
#  FFTW3_INCLUDE_DIRS - the FFTW3 include directories
#  FFTW3_LIBRARIES - link these to use FFTW3

# INCLUDE(UsePkgConfig)

FIND_PATH(FFTW3_INCLUDE_DIR fftw3.h)

FIND_LIBRARY(FFTW3_LIBRARY
  NAMES fftw3f libfftw3f
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFTW3 DEFAULT_MSG FFTW3_LIBRARY FFTW3_INCLUDE_DIR)

IF(FFTW3_FOUND)
  SET(FFTW3_LIBRARIES ${FFTW3_LIBRARY})
  SET(FFTW3_INCLUDE_DIRS ${FFTW3_INCLUDE_DIR})
ENDIF(FFTW3_FOUND)
