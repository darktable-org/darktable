# - Find the native gconf-2 includes and library
#
# This module defines
#  GConf2_INCLUDE_DIR, where to find gconf/gconf.h, etc.
#  GConf2_LIBRARIES, the libraries to link against to use gconf-2.
#  GCONF2_FOUND, If false, do not try to use gconf-2.
# also defined, but not for general use are
#  GCONF2_LIBRARY, where to find the gconf-2 library.


#=============================================================================
# Copyright 2010 henrik andersson
#=============================================================================

find_path(GCONF2_INCLUDE_DIR gconf/gconf.h PATH_SUFFIXES gconf/2 )
mark_as_advanced(GCONF2_INCLUDE_DIR)

set(GCONF2_NAMES ${GCONF2_NAMES} gconf-2 libgconf-2)
find_library(GCONF2_LIBRARY NAMES ${GCONF2_NAMES} )
mark_as_advanced(GCONF2_LIBRARY)

# handle the QUIETLY and REQUIRED arguments and set GCONF2_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(GCONF2 DEFAULT_MSG GCONF2_LIBRARY GCONF2_INCLUDE_DIR)

IF(GCONF2_FOUND)
  SET(GConf2_LIBRARIES ${GCONF2_LIBRARY})
  SET(GConf2_INCLUDE_DIRS ${GCONF2_INCLUDE_DIR})
ENDIF(GCONF2_FOUND)