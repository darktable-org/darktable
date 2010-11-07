# - Find the native GThread includes and library
#
# This module defines
#  GTHREAD_INCLUDE_DIR, where to find sqlite3.h, etc.
#  GTHREAD_LIBRARIES, the libraries to link against to use sqlite3.
#  GTHREAD_FOUND, If false, do not try to use sqlite3.
# also defined, but not for general use are
#  GTHREAD_LIBRARY, where to find the sqlite3 library.


#=============================================================================
# Copyright 2010 henrik andersson
#=============================================================================

find_path(GTHREAD_INCLUDE_DIR glib-2.0/gmodule.h)
mark_as_advanced(GTHREAD_INCLUDE_DIR)

set(GTHREAD_NAMES ${GTHREAD_NAMES} gmodule-2.0 libgmodule-2.0)
find_library(GTHREAD_LIBRARY NAMES ${GTHREAD_NAMES} )
mark_as_advanced(GTHREAD_LIBRARY)

# handle the QUIETLY and REQUIRED arguments and set GTHREAD_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(GModule DEFAULT_MSG GTHREAD_LIBRARY GTHREAD_INCLUDE_DIR)

IF(GTHREAD_FOUND)
  SET(GThread_LIBRARIES ${GTHREAD_LIBRARY})
  SET(GThread_INCLUDE_DIRS ${GTHREAD_INCLUDE_DIR})
ENDIF(GTHREAD_FOUND)