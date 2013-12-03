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

find_path(WEBP_INCLUDE_DIR NAMES webp/encode.h)
mark_as_advanced(WEBP_INCLUDE_DIR)

set(WEBP_NAMES ${WEBP_NAMES} webp libwebp)
find_library(WEBP_LIBRARY NAMES ${WEBP_NAMES} )
mark_as_advanced(WEBP_LIBRARY)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(WEBP DEFAULT_MSG WEBP_LIBRARY WEBP_INCLUDE_DIR)

IF(WEBP_FOUND)
  SET(WebP_LIBRARIES ${WEBP_LIBRARY})
  SET(WebP_INCLUDE_DIRS ${WEBP_INCLUDE_DIR})
ENDIF(WEBP_FOUND)
