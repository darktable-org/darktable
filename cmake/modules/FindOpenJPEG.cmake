# - Find OpenJPEG library
# Find the native OpenJPEG includes and library
# This module defines
#  OPENJPEG_INCLUDE_DIRS, where to find openjpeg.h, Set when
#                        OPENJPEG_INCLUDE_DIR is found.
#  OPENJPEG_LIBRARIES, libraries to link against to use OpenJPEG.
#  OPENJPEG_ROOT_DIR, The base directory to search for OpenJPEG.
#                    This can also be an environment variable.
#  OPENJPEG_FOUND, If false, do not try to use OpenJPEG.
#
# also defined, but not for general use are
#  OPENJPEG_LIBRARY, where to find the OpenJPEG library.

#=============================================================================
# Copyright 2011 Blender Foundation.
#
# Distributed under the OSI-approved BSD License (the "License");
# see accompanying file Copyright.txt for details.
#
# This software is distributed WITHOUT ANY WARRANTY; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
# See the License for more information.
#=============================================================================

# If OPENJPEG_ROOT_DIR was defined in the environment, use it.
IF(NOT OPENJPEG_ROOT_DIR AND NOT $ENV{OPENJPEG_ROOT_DIR} STREQUAL "")
  SET(OPENJPEG_ROOT_DIR $ENV{OPENJPEG_ROOT_DIR})
ENDIF()

include(LibFindMacros)

libfind_pkg_check_modules(PC_OPENJPEG libopenjpeg1)

SET(_openjpeg_SEARCH_DIRS
  ${OPENJPEG_ROOT_DIR}
  /usr/local
  /sw # Fink
  /opt/local # DarwinPorts
  /opt/csw # Blastwave
)

FIND_PATH(OPENJPEG_INCLUDE_DIR
  NAMES
    openjpeg.h
  HINTS
    ${PC_OPENJPEG_INCLUDEDIR}
    ${PC_OPENJPEG_INCLUDE_DIRS}
    ${_openjpeg_SEARCH_DIRS}
  PATH_SUFFIXES
    include
)

FIND_LIBRARY(OPENJPEG_LIBRARY
  NAMES
    openjpeg
  HINTS
    ${PC_OPENJPEG_LIBDIR}
    ${PC_OPENJPEG_LIBRARY_DIRS}
    ${_openjpeg_SEARCH_DIRS}
  PATH_SUFFIXES
    lib64 lib
  )

# handle the QUIETLY and REQUIRED arguments and set OPENJPEG_FOUND to TRUE if
# all listed variables are TRUE
INCLUDE(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(OpenJPEG DEFAULT_MSG
    OPENJPEG_LIBRARY OPENJPEG_INCLUDE_DIR)

IF(OPENJPEG_FOUND)
  SET(OPENJPEG_LIBRARIES ${OPENJPEG_LIBRARY})
  SET(OPENJPEG_INCLUDE_DIRS ${OPENJPEG_INCLUDE_DIR})

  # 1.3 didn't have pkg-config support, so we can't use that to get the version.
  # 1.5 no longer has the #define OPENJPEG_VERSION ... in openjpeg.h so we can't use that either.
  # Thus we might have to compile a smalltest program to get the version string. Could someone please kill me?

  # see if pkg-config found the library. this should succeed for cross compiles as they are harder to run test programs
  if(NOT "${PC_OPENJPEG_VERSION}" STREQUAL "")
    # use the version as found by pkg-config
    set(OPENJPEG_VERSION ${PC_OPENJPEG_VERSION})
  else(NOT "${PC_OPENJPEG_VERSION}" STREQUAL "")
    # too bad, we have to run our test code
    try_run(run_var result_var
            ${CMAKE_BINARY_DIR}/CMakeTmp
            ${CMAKE_SOURCE_DIR}/cmake/modules/openjpeg_version.c
            CMAKE_FLAGS "-DLINK_LIBRARIES:STRING=${OPENJPEG_LIBRARIES}"
                        "-DINCLUDE_DIRECTORIES:STRING=${OPENJPEG_INCLUDE_DIRS}"
            RUN_OUTPUT_VARIABLE rout_var
    )
    if(result_var)
      set(OPENJPEG_VERSION ${rout_var})
    endif(result_var)
  endif(NOT "${PC_OPENJPEG_VERSION}" STREQUAL "")

ENDIF(OPENJPEG_FOUND)

MARK_AS_ADVANCED(
  OPENJPEG_INCLUDE_DIR
  OPENJPEG_LIBRARY
)
