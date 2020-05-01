# - Find the native PangoCairo includes and library
#
# This module defines
#  PangoCairo_INCLUDE_DIR, where to find pangocairo.h, etc.
#  PangoCairo_LIBRARIES, the libraries to link against to use pangocairo.
#  PangoCairo_FOUND, If false, do not try to use pangocairo.
# also defined, but not for general use are
#  PangoCairo_LIBRARY, where to find the pangocairo library.


#=============================================================================
# Copyright 2010 henrik andersson
#=============================================================================

find_path(PangoCairo_INCLUDE_DIR pango/pangocairo.h PATH_SUFFIXES pango-1.0/ )
mark_as_advanced(PangoCairo_INCLUDE_DIR)

set(PangoCairo_NAMES ${PangoCairo_NAMES} pangocairo-1.0 libpangocairo-1.0)
find_library(PangoCairo_LIBRARY NAMES ${PangoCairo_NAMES} )
mark_as_advanced(PangoCairo_LIBRARY)

# handle the QUIETLY and REQUIRED arguments and set PangoCairo_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(PangoCairo DEFAULT_MSG PangoCairo_LIBRARY PangoCairo_INCLUDE_DIR)

IF(PangoCairo_FOUND)
  SET(PangoCairo_LIBRARIES ${PangoCairo_LIBRARY})
  SET(PangoCairo_INCLUDE_DIRS ${PangoCairo_INCLUDE_DIR})
ENDIF(PangoCairo_FOUND)
