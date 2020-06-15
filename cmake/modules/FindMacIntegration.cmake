# - Find the native GtkOSXApplication includes and library
#
# This module defines
#  MACINTEGRATION_INCLUDE_DIR, where to find gtkosxapplication.h, etc.
#  MACINTEGRATION_LIBRARIES, the libraries to link against to use GtkOSXApplication.
#  MACINTEGRATION_FOUND, If false, do not try to use GtkOSXApplication.
# also defined, but not for general use are
#  MACINTEGRATION_LIBRARY, where to find the GtkOSXApplication library.


#=============================================================================
# Copyright 2010 henrik andersson
#=============================================================================

SET(MACINTEGRATION_FIND_REQUIRED ${MacIntegration_FIND_REQUIRED})

find_path(MACINTEGRATION_INCLUDE_DIR gtkosxapplication.h PATH_SUFFIXES gtkmacintegration gtkmacintegration-gtk3)
mark_as_advanced(MACINTEGRATION_INCLUDE_DIR)

set(MACINTEGRATION_NAMES ${MACINTEGRATION_NAMES} gtkmacintegration libgtkmacintegration gtkmacintegration-gtk3 libgtkmacintegration-gtk3)
find_library(MACINTEGRATION_LIBRARY NAMES ${MACINTEGRATION_NAMES})
mark_as_advanced(MACINTEGRATION_LIBRARY)

# handle the QUIETLY and REQUIRED arguments and set MACINTEGRATION_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MACINTEGRATION DEFAULT_MSG MACINTEGRATION_LIBRARY MACINTEGRATION_INCLUDE_DIR)

IF(MACINTEGRATION_FOUND)
  SET(MacIntegration_LIBRARIES ${MACINTEGRATION_LIBRARY})
  SET(MacIntegration_INCLUDE_DIRS ${MACINTEGRATION_INCLUDE_DIR})
ENDIF(MACINTEGRATION_FOUND)
