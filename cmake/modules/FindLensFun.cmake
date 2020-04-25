# - Find the native lensfun includes and library
#
# This module defines
#  LensFun_INCLUDE_DIR, where to find lensfun.h, etc.
#  LensFun_LIBRARIES, the libraries to link against to use lensfun.
#  LensFun_FOUND, If false, do not try to use lensfun.
# also defined, but not for general use are
#  LensFun_LIBRARY, where to find the lensfun library.


#=============================================================================
# Copyright 2010 henrik andersson
#=============================================================================

include(LibFindMacros)

# Use pkg-config to get hints about paths
libfind_pkg_check_modules(LensFun_PKGCONF lensfun)

find_path(LensFun_INCLUDE_DIR NAMES lensfun.h
  HINTS ${LensFun_PKGCONF_INCLUDE_DIRS}
  /usr/include/lensfun
  /include/lensfun
  ENV LENSFUN_INCLUDE_DIR)
mark_as_advanced(LensFun_INCLUDE_DIR)

set(LensFun_NAMES ${LensFun_NAMES} lensfun liblensfun)
find_library(LensFun_LIBRARY NAMES ${LensFun_NAMES}
  HINTS ENV LENSFUN_LIB_DIR)
mark_as_advanced(LensFun_LIBRARY)

# handle the QUIETLY and REQUIRED arguments and set LensFun_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LensFun DEFAULT_MSG LensFun_LIBRARY LensFun_INCLUDE_DIR)

IF(LensFun_FOUND)
  SET(LensFun_LIBRARIES ${LensFun_LIBRARY})
  SET(LensFun_INCLUDE_DIRS ${LensFun_INCLUDE_DIR})
ENDIF(LensFun_FOUND)
