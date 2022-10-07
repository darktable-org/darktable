# - Find the native webp library and includes
#
# This module defines
#  WebP_INCLUDE_DIR, where to libwebp headers.
#  WebP_LIBRARIES, the libraries to link against to support webp.
#  WebP_FOUND, If false, do not enable webp export support.
# also defined, but not for general use are
#  WebP_LIBRARY, where to find the webp library.

#=============================================================================
# Copyright 2013 Google Inc.
#=============================================================================

include(LibFindMacros)

# Use pkg-config to get hints about paths
libfind_pkg_check_modules(WebP_PKGCONF libwebp)


find_path(WebP_INCLUDE_DIR NAMES webp/encode.h HINTS ${WebP_PKGCONF_INCLUDE_DIRS})
mark_as_advanced(WebP_INCLUDE_DIR)

set(WebP_NAMES ${WebP_NAMES} webp libwebp)
find_library(WebP_LIBRARY NAMES ${WebP_NAMES} HINTS ${WebP_PKGCONF_LIBRARY_DIRS})
set(WebP_MUX_NAMES ${WebP_MUX_NAMES} webpmux libwebpmux)
find_library(WebP_MUX_LIBRARY NAMES ${WebP_MUX_NAMES} HINTS ${WebP_PKGCONF_LIBRARY_DIRS})
set(WebP_LIBRARY ${WebP_LIBRARY} ${WebP_MUX_LIBRARY})
mark_as_advanced(WebP_LIBRARY)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(WebP DEFAULT_MSG WebP_LIBRARY WebP_INCLUDE_DIR)


if(WebP_FIND_VERSION)
  cmake_minimum_required(VERSION 3.10.0)
  set(WebP_FAILED_VERSION_CHECK true)

  if(WebP_FIND_VERSION_EXACT)
    if(WebP_PKGCONF_VERSION VERSION_EQUAL WebP_FIND_VERSION)
      set(WebP_FAILED_VERSION_CHECK false)
    endif()
  else()
    if(WebP_PKGCONF_VERSION VERSION_EQUAL WebP_FIND_VERSION OR
       WebP_PKGCONF_VERSION VERSION_GREATER WebP_FIND_VERSION)
      set(WebP_FAILED_VERSION_CHECK false)
    endif()
  endif()

  if(WebP_FAILED_VERSION_CHECK)
    if(WebP_FIND_REQUIRED AND NOT WebP_FIND_QUIETLY)
        if(WebP_FIND_VERSION_EXACT)
            message(FATAL_ERROR "WebP version check failed.  Version ${WebP_PKGCONF_VERSION} was found, version ${WebP_FIND_VERSION} is needed exactly.")
        else(WebP_FIND_VERSION_EXACT)
            message(FATAL_ERROR "WebP version check failed.  Version ${WebP_PKGCONF_VERSION} was found, at least version ${WebP_FIND_VERSION} is required")
        endif(WebP_FIND_VERSION_EXACT)
    endif(WebP_FIND_REQUIRED AND NOT WebP_FIND_QUIETLY)

    set(WebP_FOUND false)
  endif(WebP_FAILED_VERSION_CHECK)

endif(WebP_FIND_VERSION)


IF(WebP_FOUND)
  SET(WebP_LIBRARIES ${WebP_LIBRARY})
  SET(WebP_INCLUDE_DIRS ${WebP_INCLUDE_DIR})
ENDIF(WebP_FOUND)
