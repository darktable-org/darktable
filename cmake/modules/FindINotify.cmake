# - Find the native sqlite3 includes and library
#
# This module defines
#  INOTIFY_INCLUDE_DIR, where to find inotify.h, etc.
#  INOTIFY_FOUND, If false, do not try to use inotify.
# also defined, but not for general use are
#  INOTIFY_LIBRARY, where to find the inotify library.


#=============================================================================
# Copyright 2010 henrik andersson
#=============================================================================

SET(INOTIFY_FIND_REQUIRED ${INotify_FIND_REQUIRED})

find_path(INOTIFY_INCLUDE_DIR sys/inotify.h)
mark_as_advanced(INOTIFY_INCLUDE_DIR)


# handle the QUIETLY and REQUIRED arguments and set INOTIFY_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(INOTIFY DEFAULT_MSG INOTIFY_INCLUDE_DIR)

IF(INOTIFY_FOUND)
  SET(INotify_INCLUDE_DIRS ${INOTIFY_INCLUDE_DIR})
ENDIF(INOTIFY_FOUND)
