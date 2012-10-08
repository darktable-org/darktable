# - Find the native PangoCairo includes and library
#
# This module defines
#  PangoCairo_INCLUDE_DIR, where to find pangocairo.h, etc.
#  PangoCairo_LIBRARIES, the libraries to link against to use pangocairo.
#  PANGOCAIRO_FOUND, If false, do not try to use pangocairo.
# also defined, but not for general use are
#  PANGOCAIRO_LIBRARY, where to find the pangocairo library.


#=============================================================================
# Copyright 2010 henrik andersson
#=============================================================================

SET(PANGOCAIRO_FIND_REQUIRED ${PangoCairo_FIND_REQUIRED})

find_path(PANGOCAIRO_INCLUDE_DIR pango/pangocairo.h PATH_SUFFIXES pango-1.0/ )
mark_as_advanced(PANGOCAIRO_INCLUDE_DIR)

set(PANGOCAIRO_NAMES ${PANGOCAIRO_NAMES} pangocairo-1.0 libpangocairo-1.0)
find_library(PANGOCAIRO_LIBRARY NAMES ${PANGOCAIRO_NAMES} )
mark_as_advanced(PANGOCAIRO_LIBRARY)

# handle the QUIETLY and REQUIRED arguments and set PANGOCAIRO_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(PANGOCAIRO DEFAULT_MSG PANGOCAIRO_LIBRARY PANGOCAIRO_INCLUDE_DIR)

IF(PANGOCAIRO_FOUND)
  SET(PangoCairo_LIBRARIES ${PANGOCAIRO_LIBRARY})
  SET(PangoCairo_INCLUDE_DIRS ${PANGOCAIRO_INCLUDE_DIR})
ENDIF(PANGOCAIRO_FOUND)
