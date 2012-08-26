# - Find the native sqlite3 includes and library
#
# This module defines
#  SQLITE3_INCLUDE_DIR, where to find sqlite3.h, etc.
#  SQLITE3_LIBRARIES, the libraries to link against to use sqlite3.
#  SQLITE3_FOUND, If false, do not try to use sqlite3.
# also defined, but not for general use are
#  SQLITE3_LIBRARY, where to find the sqlite3 library.


#=============================================================================
# Copyright 2010 henrik andersson
#=============================================================================

SET(SQLITE3_FIND_REQUIRED ${Sqlite3_FIND_REQUIRED})

# prefer macports' sqlite3 over system's, using libsqlite3 supplied with OS X causes frequent crashes
# it's assumed that macports were installed to default location (/opt/local)
find_path(SQLITE3_INCLUDE_DIR sqlite3.h)
mark_as_advanced(SQLITE3_INCLUDE_DIR)

set(SQLITE3_NAMES ${SQLITE3_NAMES} sqlite3 libsqlite3)
find_library(SQLITE3_LIBRARY NAMES ${SQLITE3_NAMES} )
mark_as_advanced(SQLITE3_LIBRARY)

# handle the QUIETLY and REQUIRED arguments and set SQLITE3_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SQLITE3 DEFAULT_MSG SQLITE3_LIBRARY SQLITE3_INCLUDE_DIR)

IF(SQLITE3_FOUND)
  SET(Sqlite3_LIBRARIES ${SQLITE3_LIBRARY})
  SET(Sqlite3_INCLUDE_DIRS ${SQLITE3_INCLUDE_DIR})
ENDIF(SQLITE3_FOUND)
