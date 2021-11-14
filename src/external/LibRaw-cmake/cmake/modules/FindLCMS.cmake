# - Find LCMS
# Find the LCMS (Little Color Management System) library and includes and
# This module defines
#  LCMS_INCLUDE_DIR, where to find lcms.h
#  LCMS_LIBRARIES, the libraries needed to use LCMS.
#  LCMS_DOT_VERSION, The version number of the LCMS library, e.g. "1.19"
#  LCMS_VERSION, Similar to LCMS_DOT_VERSION, but without the dots, e.g. "119"
#  LCMS_FOUND, If false, do not try to use LCMS.
#
# The minimum required version of LCMS can be specified using the
# standard syntax, e.g. find_package(LCMS 1.10)

# Copyright (c) 2008, Adrian Page, <adrian@pagenet.plus.com>
# Copyright (c) 2009, Cyrille Berger, <cberger@cberger.net>
#
# Redistribution and use is allowed according to the terms of the BSD license.
# For details see the accompanying LICENSE file.


# use pkg-config to get the directories and then use these values
# in the FIND_PATH() and FIND_LIBRARY() calls
if(NOT WIN32)
   find_package(PkgConfig)
   pkg_check_modules(PC_LCMS lcms)
   set(LCMS_DEFINITIONS ${PC_LCMS_CFLAGS_OTHER})
endif(NOT WIN32)

find_path(LCMS_INCLUDE_DIR lcms.h
   HINTS
   ${PC_LCMS_INCLUDEDIR}
   ${PC_LCMS_INCLUDE_DIRS}
   PATH_SUFFIXES lcms liblcms1
)

find_library(LCMS_LIBRARIES NAMES lcms liblcms lcms-1 liblcms-1
   HINTS
   ${PC_LCMS_LIBDIR}
   ${PC_LCMS_LIBRARY_DIRS}
   PATH_SUFFIXES lcms
)

# Store the LCMS version number in the cache, so we don't have to search every time again
if(LCMS_INCLUDE_DIR  AND NOT  LCMS_VERSION)
   file(READ ${LCMS_INCLUDE_DIR}/lcms.h LCMS_VERSION_CONTENT)
   string(REGEX MATCH "#define LCMS_VERSION[ ]*[0-9]*\n" LCMS_VERSION_MATCH ${LCMS_VERSION_CONTENT})
   if(LCMS_VERSION_MATCH)
      string(REGEX REPLACE "#define LCMS_VERSION[ ]*([0-9]*)\n" "\\1" _LCMS_VERSION ${LCMS_VERSION_MATCH})
      string(SUBSTRING ${_LCMS_VERSION} 0 1 LCMS_MAJOR_VERSION)
      string(SUBSTRING ${_LCMS_VERSION} 1 2 LCMS_MINOR_VERSION)
   endif(LCMS_VERSION_MATCH)
   set(LCMS_VERSION "${LCMS_MAJOR_VERSION}${LCMS_MINOR_VERSION}" CACHE STRING "Version number of lcms" FORCE)
   set(LCMS_DOT_VERSION "${LCMS_MAJOR_VERSION}.${LCMS_MINOR_VERSION}" CACHE STRING "Version number of lcms split into components" FORCE)
endif(LCMS_INCLUDE_DIR  AND NOT  LCMS_VERSION)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LCMS REQUIRED_VARS LCMS_LIBRARIES LCMS_INCLUDE_DIR
                                       VERSION_VAR LCMS_DOT_VERSION )

mark_as_advanced(LCMS_INCLUDE_DIR LCMS_LIBRARIES LCMS_VERSION)

