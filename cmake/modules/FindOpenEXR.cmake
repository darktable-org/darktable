# - Find the native sqlite3 includes and library
#
# This module defines
#  OpenEXR_INCLUDE_DIR, where to find openexr.h, etc.
#  OpenEXR_LIBRARIES, the libraries to link against to use openexr.
#  OpenEXR_FOUND, If false, do not try to use openexr.



#=============================================================================
# Copyright 2010 henrik andersson
#=============================================================================

find_path(_OPENEXR_INCLUDE_DIR  OpenEXR
  PATHS /usr/include
  /usr/include/OpenEXR
  HINTS ENV OPENEXR_INCLUDE_DIR)
set(OpenEXR_INCLUDE_DIR  ${_OPENEXR_INCLUDE_DIR} ${_OPENEXR_INCLUDE_DIR}/OpenEXR)
mark_as_advanced(OpenEXR_INCLUDE_DIR)

# Find libraries and add them to OpenEXR_LIBRARY
find_path(_OPENEXR_LIB_DIR OpenEXR
  HINTS ENV OPENEXR_LIB_DIR)
set(OpenEXR_LIB_DIR ${_OPENEXR_LIB_DIR})
mark_as_advanced(OpenEXR_LIB_DIR)

find_library(_OPENEXR_LIBRARY_IMIMF IlmImf
  HINTS ENV OPENEXR_LIB_DIR)
set(OpenEXR_LIBRARY ${OpenEXR_LIBRARY} ${_OPENEXR_LIBRARY_IMIMF})
find_library(_OPENEXR_LIBRARY_IMATH Imath
  HINTS ENV OPENEXR_LIB_DIR)
set(OpenEXR_LIBRARY ${OpenEXR_LIBRARY} ${_OPENEXR_LIBRARY_IMATH})
find_library(_OPENEXR_LIBRARY_IEX Iex
  HINTS ENV OPENEXR_LIB_DIR)
set(OpenEXR_LIBRARY ${OpenEXR_LIBRARY} ${_OPENEXR_LIBRARY_IEX})
find_library(_OPENEXR_LIBRARY_ILMTHREAD IlmThread
  HINTS ENV OPENEXR_LIB_DIR)
set(OpenEXR_LIBRARY ${OpenEXR_LIBRARY} ${_OPENEXR_LIBRARY_ILMTHREAD})
find_library(_OPENEXR_LIBRARY_HALF Half
  HINTS ENV OPENEXR_LIB_DIR)
set(OpenEXR_LIBRARY ${OpenEXR_LIBRARY} ${_OPENEXR_LIBRARY_HALF})

mark_as_advanced(OpenEXR_LIBRARY)

# handle the QUIETLY and REQUIRED arguments and set OpenEXR_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(OpenEXR DEFAULT_MSG OpenEXR_LIBRARY OpenEXR_INCLUDE_DIR)

IF(OpenEXR_FOUND)
  SET(OpenEXR_LIBDIR ${OpenEXR_LIB_DIR})
  SET(OpenEXR_LIBRARIES ${OpenEXR_LIBRARY})
  SET(OpenEXR_INCLUDE_DIRS ${OpenEXR_INCLUDE_DIR})
ENDIF(OpenEXR_FOUND)
