# - Find the native lensfun includes and library
#
# This module defines
#  LENSFUN_INCLUDE_DIR, where to find lensfun.h, etc.
#  LENSFUN_LIBRARIES, the libraries to link against to use lensfun.
#  LENSFUN_FOUND, If false, do not try to use lensfun.
# also defined, but not for general use are
#  LENSFUN_LIBRARY, where to find the lensfun library.


#=============================================================================
# Copyright 2010 henrik andersson
#=============================================================================

include(LibFindMacros)

SET(LENSFUN_FIND_REQUIRED ${LensFun_FIND_REQUIRED})

# Use pkg-config to get hints about paths
libfind_pkg_check_modules(Lensfun_PKGCONF lensfun)

find_path(LENSFUN_INCLUDE_DIR NAMES lensfun.h
  HINTS ${Lensfun_PKGCONF_INCLUDE_DIRS}
  /usr/include/lensfun
  /include/lensfun
  ENV LENSFUN_INCLUDE_DIR)
mark_as_advanced(LENSFUN_INCLUDE_DIR)

set(LENSFUN_NAMES ${LENSFUN_NAMES} lensfun liblensfun)
find_library(LENSFUN_LIBRARY NAMES ${LENSFUN_NAMES} 
	HINTS ENV LENSFUN_LIB_DIR)
mark_as_advanced(LENSFUN_LIBRARY)

# handle the QUIETLY and REQUIRED arguments and set LENSFUN_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LENSFUN DEFAULT_MSG LENSFUN_LIBRARY LENSFUN_INCLUDE_DIR)

IF(LENSFUN_FOUND)
  SET(LensFun_LIBRARIES ${LENSFUN_LIBRARY})
  SET(LensFun_INCLUDE_DIRS ${LENSFUN_INCLUDE_DIR})
ENDIF(LENSFUN_FOUND)
