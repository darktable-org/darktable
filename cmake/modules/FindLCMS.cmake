# - Find the native sqlite3 includes and library
#
# This module defines
#  LCMS_INCLUDE_DIR, where to find sqlite3.h, etc.
#  LCMS_LIBRARIES, the libraries to link against to use sqlite3.
#  LCMS_FOUND, If false, do not try to use sqlite3.
# also defined, but not for general use are
#  LCMS_LIBRARY, where to find the sqlite3 library.


#=============================================================================
# Copyright 2010 henrik andersson
#=============================================================================

find_path(LCMS_INCLUDE_DIR lcms.h)
mark_as_advanced(LCMS_INCLUDE_DIR)

set(LCMS_NAMES ${LCMS_NAMES} lcms liblcms)
find_library(LCMS_LIBRARY NAMES ${LCMS_NAMES} )
mark_as_advanced(LCMS_LIBRARY)

# handle the QUIETLY and REQUIRED arguments and set LCMS_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LCMS DEFAULT_MSG LCMS_LIBRARY LCMS_INCLUDE_DIR)

IF(LCMS_FOUND)
  SET(LCMS_LIBRARIES ${LCMS_LIBRARY})
  SET(LCMS_INCLUDE_DIRS ${LCMS_INCLUDE_DIR})
ENDIF(LCMS_FOUND)