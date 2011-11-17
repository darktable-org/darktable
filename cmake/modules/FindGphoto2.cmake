# - Find the native sqlite3 includes and library
#
# This module defines
#  GPHOTO2_INCLUDE_DIR, where to find sqlite3.h, etc.
#  GPHOTO2_LIBRARIES, the libraries to link against to use sqlite3.
#  GPHOTO2_FOUND, If false, do not try to use sqlite3.
# also defined, but not for general use are
#  GPHOTO2_LIBRARY, where to find the sqlite3 library.


#=============================================================================
# Copyright 2010 henrik andersson
#=============================================================================

find_path(GPHOTO2_INCLUDE_DIR gphoto2/gphoto2.h)
mark_as_advanced(GPHOTO2_INCLUDE_DIR)

set(GPHOTO2_NAMES ${GPHOTO2_NAMES} gphoto2 libgphoto2 libgphoto2_port)
find_library(GPHOTO2_LIBRARY NAMES ${GPHOTO2_NAMES} )
mark_as_advanced(GPHOTO2_LIBRARY)

# handle the QUIETLY and REQUIRED arguments and set GPHOTO2_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(GPHOTO2 DEFAULT_MSG GPHOTO2_LIBRARY GPHOTO2_INCLUDE_DIR)

IF(GPHOTO2_FOUND)
  SET(Gphoto2_LIBRARIES ${GPHOTO2_LIBRARY})
  SET(Gphoto2_INCLUDE_DIRS ${GPHOTO2_INCLUDE_DIR})
ENDIF(GPHOTO2_FOUND)
