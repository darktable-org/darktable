#
# Find the GMIC includes and library
#

# This module defines
# GMIC_INCLUDE_DIR, where to find *.h etc
# GMIC_LIBRARY, the libraries
# GMIC_FOUND, If false, do not try to use GMIC.

set(GMIC_VERSION_NEEDED 270)
set(GMIC_DEPENDENCIES_FOUND TRUE)

include(LibFindMacros)

# Use pkg-config to get hints about paths
libfind_pkg_check_modules(GMIC_PKGCONF GMIC)

# Include dir
find_path(GMIC_INCLUDE_DIR
  NAMES gmic.h
  HINTS ${GMIC_PKGCONF_INCLUDE_DIRS}
  PATH_SUFFIXES GMIC
)

# Finally the library itself
find_library(GMIC_LIBRARY
  NAMES gmic
  HINTS ${GMIC_PKGCONF_LIBRARY_DIRS}
)

if(GMIC_LIBRARY AND GMIC_INCLUDE_DIR)
  set(CMAKE_REQUIRED_INCLUDES ${GMIC_INCLUDE_DIR})
  check_cxx_source_compiles("
  #include <gmic.h>
  #if gmic_version < ${GMIC_VERSION_NEEDED} || gmic_version >= 1000
  #error OLD_VERSION
  #endif
  int main() { return 0; }
  " GMIC_VERSION_OK)

  if(NOT GMIC_VERSION_OK)
    message(STATUS "Found GMIC but version < ${GMIC_VERSION_NEEDED}. Compressed lut will not be available")
  else()
    if(WIN32)
      find_library(FFTW_LIBRARY NAMES fftw3)
      if(NOT FFTW_LIBRARY)
        message(STATUS "missing GMIC dependency fftw3")
      else()
        list(APPEND GMIC_LIBRARY ${FFTW_LIBRARY})
      endif(NOT FFTW_LIBRARY)
      # workaround for msys2 gmic 2.9.0-3. Should be reviewed when gmic 2.9.3 is available
      find_library(OPENCV_CORE_LIBRARY NAMES opencv_core)
      find_library(OPENCV_VIDEOIO_LIBRARY NAMES opencv_videoio)
      if(NOT OPENCV_CORE_LIBRARY OR NOT OPENCV_VIDEOIO_LIBRARY)
        message(STATUS "missing GMIC dependencies OpenCV")
        set(GMIC_DEPENDENCIES_FOUND FALSE)
      else()
        list(APPEND GMIC_LIBRARY ${OPENCV_CORE_LIBRARY})
        list(APPEND GMIC_LIBRARY ${OPENCV_VIDEOIO_LIBRARY})
      endif(NOT OPENCV_CORE_LIBRARY OR NOT OPENCV_VIDEOIO_LIBRARY)
    endif(WIN32)

    # workaround for msys2 gmic 2.9.0-3. Should be reviewed when gmic 2.9.3 is available
    if(GMIC_DEPENDENCIES_FOUND)

      # Set the include dir variables and the libraries and let libfind_process do the rest.
      # NOTE: Singular variables for this library, plural for libraries this lib depends on.
      set(GMIC_PROCESS_INCLUDES ${GMIC_INCLUDE_DIR})
      set(GMIC_PROCESS_LIBS ${GMIC_LIBRARY})
      libfind_process(GMIC)

      if(GMIC_FOUND)
        set(GMIC_INCLUDE_DIRS ${GMIC_INCLUDE_DIR})
        set(GMIC_LIBRARIES ${GMIC_LIBRARY})
      else()
        message(STATUS "GMIC not found")
      endif(GMIC_FOUND)

    else()
      message(STATUS "GMIC dependencies not found")
    endif(GMIC_DEPENDENCIES_FOUND)

  endif(NOT GMIC_VERSION_OK)
else()
  message(STATUS "GMIC not found")
endif(GMIC_LIBRARY AND GMIC_INCLUDE_DIR)
