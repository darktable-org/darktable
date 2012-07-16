# - Find the native GThread includes and library
#
# This module defines
#  GTHREAD_INCLUDE_DIR, where to find gthread.h, etc.
#  GTHREAD_LIBRARIES, the libraries to link against to use gthread.
#  GTHREAD_FOUND, If false, do not try to use gthread.
# also defined, but not for general use are
#  GTHREAD_LIBRARY, where to find the gthread library.


#=============================================================================
# Copyright 2010 henrik andersson
#=============================================================================

SET(GTHREAD_FIND_REQUIRED ${GThread_FIND_REQUIRED})

find_path(GTHREAD_INCLUDE_DIR glib/gthread.h PATH_SUFFIXES glib-2.0/)
mark_as_advanced(GTHREAD_INCLUDE_DIR)

set(GTHREAD_NAMES ${GTHREAD_NAMES} gthread-2.0 libgthread-2.0)
find_library(GTHREAD_LIBRARY NAMES ${GTHREAD_NAMES} )
mark_as_advanced(GTHREAD_LIBRARY)

# handle the QUIETLY and REQUIRED arguments and set GTHREAD_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(GThread DEFAULT_MSG GTHREAD_LIBRARY GTHREAD_INCLUDE_DIR)

IF(GTHREAD_FOUND)
  SET(GThread_LIBRARIES ${GTHREAD_LIBRARY})
  SET(GThread_INCLUDE_DIRS ${GTHREAD_INCLUDE_DIR})
ENDIF(GTHREAD_FOUND)
