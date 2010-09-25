# - Find FFTW
# Find the native FFTW includes and library
# This module defines
#  FFTW_INCLUDE_DIR, where to find fftw3.h, etc.
#  FFTW_LIBRARIES, the libraries needed to use FFTW.
#  FFTW_FOUND, If false, do not try to use FFTW.
# also defined, but not for general use are
#  FFTW_LIBRARY, where to find the FFTW library.

FIND_PATH(FFTW_INCLUDE_DIR fftw3.h
/usr/local/include
/usr/include
)

SET(FFTW_NAMES ${FFTW_NAMES} fftw3 fftw3f fftw3-3)
FIND_LIBRARY(FFTW_LIBRARY
  NAMES ${FFTW_NAMES}
  PATHS /usr/lib /usr/local/lib
  )

IF (FFTW_LIBRARY AND FFTW_INCLUDE_DIR)
    SET(FFTW_LIBRARIES ${FFTW_LIBRARY})
    SET(FFTW_FOUND "YES")
ELSE (FFTW_LIBRARY AND FFTW_INCLUDE_DIR)
  SET(FFTW_FOUND "NO")
ENDIF (FFTW_LIBRARY AND FFTW_INCLUDE_DIR)


IF (FFTW_FOUND)
   IF (NOT FFTW_FIND_QUIETLY)
      MESSAGE(STATUS "Found FFTW: ${FFTW_LIBRARIES}")
   ENDIF (NOT FFTW_FIND_QUIETLY)
ELSE (FFTW_FOUND)
   IF (FFTW_FIND_REQUIRED)
      MESSAGE(FATAL_ERROR "Could not find FFTW library")
   ENDIF (FFTW_FIND_REQUIRED)
ENDIF (FFTW_FOUND)

SET (ON_LINUX ${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
IF (${ON_LINUX})
   SET (FFTW_EXECUTABLE_LIBRARIES fftw3f fftw3f_threads)
ENDIF (${ON_LINUX})
