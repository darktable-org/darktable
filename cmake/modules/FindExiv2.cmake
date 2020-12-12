# - Find the native exiv2 includes and library
#
# This module defines
#  EXIV2_INCLUDE_DIR, where to find png.h, etc.
#  EXIV2_LIBRARIES, the libraries to link against to use exiv2
#  EXIV2_FOUND, If false, do not try to use exiv2
# also defined, but not for general use are
#  EXIV2_LIBRARY, where to find the exiv2 library

#=============================================================================
# Copyright 2010 henrik andersson
#=============================================================================

include(LibFindMacros)

libfind_pkg_check_modules(Exiv2_PKGCONF exiv2)

find_path(Exiv2_INCLUDE_DIR
    NAMES
        exiv2/image.hpp
    HINTS
        ${Exiv2_PKGCONF_INCLUDEDIR}
        ${Exiv2_PKGCONF_INCLUDE_DIRS}
)
mark_as_advanced(Exiv2_INCLUDE_DIR)

set(Exiv2_NAMES ${Exiv2_NAMES} exiv2 libexiv2)
find_library(Exiv2_LIBRARY
    NAMES
        ${Exiv2_NAMES}
    HINTS
        ${Exiv2_PKGCONF_LIBDIR}
        ${Exiv2_PKGCONF_LIBRARY_DIRS}
)
mark_as_advanced(Exiv2_LIBRARY)

if(WIN32)
  find_library(EXPAT_LIBRARY NAMES expat )
  find_library(ICONV_LIBRARY NAMES iconv )
  list(APPEND Exiv2_LIBRARY ${EXPAT_LIBRARY} ${ICONV_LIBRARY})
endif(WIN32)

if(Exiv2_PKGCONF_VERSION VERSION_LESS Exiv2_FIND_VERSION)
  message(FATAL_ERROR "Exiv2 version check failed.  Version ${Exiv2_PKGCONF_VERSION} was found, at least version ${Exiv2_FIND_VERSION} is required")
endif(Exiv2_PKGCONF_VERSION VERSION_LESS Exiv2_FIND_VERSION)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Exiv2 DEFAULT_MSG Exiv2_LIBRARY Exiv2_INCLUDE_DIR)

IF(Exiv2_FOUND)
  set(Exiv2_LIBRARIES ${Exiv2_LIBRARY})
  set(Exiv2_INCLUDE_DIRS ${Exiv2_INCLUDE_DIR})
ENDIF(Exiv2_FOUND)
