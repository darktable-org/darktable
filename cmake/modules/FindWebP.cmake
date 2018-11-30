# - Find the native webp library and includes
#
# This module defines
#  WEBP_INCLUDE_DIR, where to libwebp headers.
#  WEBP_LIBRARIES, the libraries to link against to support webp.
#  WEBP_FOUND, If false, do not enable webp export support.
# also defined, but not for general use are
#  WEBP_LIBRARY, where to find the webp library.

#=============================================================================
# Copyright 2013 Google Inc.
#=============================================================================

SET(WEBP_FIND_REQUIRED ${WebP_FIND_REQUIRED})
SET(WEBP_FIND_VERSION ${WebP_FIND_VERSION})
SET(WEBP_FIND_VERSION_EXACT ${WebP_FIND_VERSION_EXACT})
SET(WEBP_FIND_QUIETLY ${WebP_FIND_QUIETLY})


include(LibFindMacros)

# Use pkg-config to get hints about paths
libfind_pkg_check_modules(WEBP_PKGCONF libwebp)


find_path(WEBP_INCLUDE_DIR NAMES webp/encode.h HINTS ${WEBP_PKGCONF_INCLUDE_DIRS})
mark_as_advanced(WEBP_INCLUDE_DIR)

set(WEBP_NAMES ${WEBP_NAMES} webp libwebp)
find_library(WEBP_LIBRARY NAMES ${WEBP_NAMES} HINTS ${WEBP_PKGCONF_LIBRARY_DIRS})
mark_as_advanced(WEBP_LIBRARY)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(WEBP DEFAULT_MSG WEBP_LIBRARY WEBP_INCLUDE_DIR)


if(WEBP_FIND_VERSION)
  cmake_minimum_required(VERSION 3.4.0)
  set(WEBP_FAILED_VERSION_CHECK true)

  if(WEBP_FIND_VERSION_EXACT)
    if(WEBP_PKGCONF_VERSION VERSION_EQUAL WEBP_FIND_VERSION)
      set(WEBP_FAILED_VERSION_CHECK false)
    endif()
  else()
    if(WEBP_PKGCONF_VERSION VERSION_EQUAL WEBP_FIND_VERSION OR
       WEBP_PKGCONF_VERSION VERSION_GREATER WEBP_FIND_VERSION)
      set(WEBP_FAILED_VERSION_CHECK false)
    endif()
  endif()

  if(WEBP_FAILED_VERSION_CHECK)
    if(WEBP_FIND_REQUIRED AND NOT WEBP_FIND_QUIETLY)
        if(WEBP_FIND_VERSION_EXACT)
            message(FATAL_ERROR "WebP version check failed.  Version ${WEBP_PKGCONF_VERSION} was found, version ${WEBP_FIND_VERSION} is needed exactly.")
        else(WEBP_FIND_VERSION_EXACT)
            message(FATAL_ERROR "WebP version check failed.  Version ${WEBP_PKGCONF_VERSION} was found, at least version ${WEBP_FIND_VERSION} is required")
        endif(WEBP_FIND_VERSION_EXACT)
    endif(WEBP_FIND_REQUIRED AND NOT WEBP_FIND_QUIETLY)

    set(WEBP_FOUND false)
  endif(WEBP_FAILED_VERSION_CHECK)

endif(WEBP_FIND_VERSION)


IF(WEBP_FOUND)

  SET(WebP_LIBRARIES ${WEBP_LIBRARY})
  SET(WebP_INCLUDE_DIRS ${WEBP_INCLUDE_DIR})
ENDIF(WEBP_FOUND)
