# - Find the native sqlite3 includes and library
#
# This module defines
#  EXIV2_INCLUDE_DIR, where to find png.h, etc.
#  EXIV2_LIBRARIES, the libraries to link against to use sqlite3.
#  EXIV2_FOUND, If false, do not try to use sqlite3.
# also defined, but not for general use are
#  EXIV2_LIBRARY, where to find the sqlite3 library.


#=============================================================================
# Copyright 2010 henrik andersson
#=============================================================================

SET(EXIV2_FIND_REQUIRED ${Exiv2_FIND_REQUIRED})

find_path(EXIV2_INCLUDE_DIR NAMES exiv2/image.hpp)
mark_as_advanced(EXIV2_INCLUDE_DIR)

set(EXIV2_NAMES ${EXIV2_NAMES} exiv2 libexiv2)
find_library(EXIV2_LIBRARY NAMES ${EXIV2_NAMES} )
mark_as_advanced(EXIV2_LIBRARY)

if(WIN32)
  find_library(EXPAT_LIBRARY NAMES expat )
  find_library(ICONV_LIBRARY NAMES iconv )
  list(APPEND EXIV2_LIBRARY ${EXPAT_LIBRARY} ${ICONV_LIBRARY})
endif(WIN32)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(EXIV2 DEFAULT_MSG EXIV2_LIBRARY EXIV2_INCLUDE_DIR)

IF(EXIV2_FOUND)
  SET(Exiv2_LIBRARIES ${EXIV2_LIBRARY})
  SET(Exiv2_INCLUDE_DIRS ${EXIV2_INCLUDE_DIR})
ENDIF(EXIV2_FOUND)
