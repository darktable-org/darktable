# - Find the native sqlite3 includes and library
#
# This module defines
#  RSVG2_INCLUDE_DIR, where to find sqlite3.h, etc.
#  RSVG2_LIBRARIES, the libraries to link against to use sqlite3.
#  RSVG2_FOUND, If false, do not try to use sqlite3.
# also defined, but not for general use are
#  RSVG2_LIBRARY, where to find the sqlite3 library.


#=============================================================================
# Copyright 2010 henrik andersson
#=============================================================================

SET(RSVG2_FIND_REQUIRED ${Rsvg2_FIND_REQUIRED})

find_path(RSVG2_INCLUDE_DIR librsvg/rsvg.h PATH_SUFFIXES librsvg-2.0 librsvg-2 )
mark_as_advanced(RSVG2_INCLUDE_DIR)

set(RSVG2_NAMES ${RSVG2_NAMES} rsvg-2 librsvg-2)
find_library(RSVG2_LIBRARY NAMES ${RSVG2_NAMES} )
mark_as_advanced(RSVG2_LIBRARY)

# handle the QUIETLY and REQUIRED arguments and set RSVG2_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(RSVG2 DEFAULT_MSG RSVG2_LIBRARY RSVG2_INCLUDE_DIR)

IF(RSVG2_FOUND)
  SET(Rsvg2_LIBRARIES ${RSVG2_LIBRARY})
  SET(Rsvg2_INCLUDE_DIRS ${RSVG2_INCLUDE_DIR})
ENDIF(RSVG2_FOUND)
