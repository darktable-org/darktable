# - Try to find FFTW3
# Once done, this will define
#
#  FFTW3_FOUND - system has FFTW3
#  FFTW3_INCLUDE_DIRS - the FFTW3 include directories
#  FFTW3_LIBRARIES - link these to use FFTW3

# INCLUDE(UsePkgConfig)

# use pkg-config to get the directories and then use these values
# in the FIND_PATH() and FIND_LIBRARY() calls
# PKGCONFIG(FFTW3 _FFTW3IncDir _FFTW3LinkDir _flickculrLinkFlags _FFTW3Cflags)

FIND_PATH(FFTW3_INCLUDE_DIR fftw3.h)

FIND_LIBRARY(FFTW3_LIBRARY
  NAMES fftw3f libfftw3f
)
message("FFTW3_INCLUDE_DIR: ${FFTW3_INCLUDE_DIR}")
message("FFTW3_LIBRARY: ${FFTW3_LIBRARY}")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFTW3 DEFAULT_MSG FFTW3_LIBRARY FFTW3_INCLUDE_DIR)

IF(FFTW3_FOUND)
  SET(FFTW3_LIBRARIES ${FFTW3_LIBRARY})
  SET(FFTW3_INCLUDE_DIRS ${FFTW3_INCLUDE_DIR})
ENDIF(FFTW3_FOUND)
