# - Try to find LensFun
# Once done this will define
#
#  LENSFUN_FOUND - system has LensFun
#  LENSFUN_INCLUDE_DIR - the LensFun include directory
#  LENSFUN_LIBRARIES - Link these to use LensFun
#  LENSFUN_DEFINITIONS - Compiler switches required for using LensFun
#
#=============================================================================
#  Copyright (c) 2020 Andreas Schneider <asn@cryptomilk.org>
#
#  Distributed under the OSI-approved BSD License (the "License");
#  see accompanying file Copyright.txt for details.
#
#  This software is distributed WITHOUT ANY WARRANTY; without even the
#  implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
#  See the License for more information.
#=============================================================================
#

if (UNIX)
  find_package(PkgConfig)
  if (PKG_CONFIG_FOUND)
    pkg_check_modules(_LENSFUN lensfun)
  endif (PKG_CONFIG_FOUND)
endif (UNIX)

find_path(LENSFUN_INCLUDE_DIR
    NAMES
        lensfun.h
    PATHS
        ${_LENSFUN_INCLUDEDIR}
    PATH_SUFFIXES
        lensfun
)

find_library(LENSFUN_LIBRARY
    NAMES
        lensfun
    PATHS
        ${_LENSFUN_LIBDIR}
)

if (LENSFUN_LIBRARY)
    set(LENSFUN_LIBRARIES
        ${LENSFUN_LIBRARIES}
        ${LENSFUN_LIBRARY}
    )
endif (LENSFUN_LIBRARY)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LensFun
                                  FOUND_VAR LENSFUN_FOUND
                                  REQUIRED_VARS LENSFUN_LIBRARY LENSFUN_LIBRARIES LENSFUN_INCLUDE_DIR
                                  VERSION_VAR _LENSFUN_VERSION)

# show the LENSFUN_INCLUDE_DIR and LENSFUN_LIBRARIES variables only in the advanced view
mark_as_advanced(LENSFUN_INCLUDE_DIR LENSFUN_LIBRARIES)
