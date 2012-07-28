# - Find the native sqlite3 includes and library
#
# This module defines
#  OpenEXR_INCLUDE_DIR, where to find openexr.h, etc.
#  OpenEXR_LIBRARIES, the libraries to link against to use openexr.
#  OPENEXR_FOUND, If false, do not try to use openexr.



#=============================================================================
# Copyright 2010 henrik andersson
#=============================================================================

SET(OPENEXR_FIND_REQUIRED ${OpenEXR_FIND_REQUIRED})

find_path(_OPENEXR_INCLUDE_DIR  OpenEXR 
	PATHS /usr/include
	/usr/include/OpenEXR
	HINTS ENV OPENEXR_INCLUDE_DIR)
set(OPENEXR_INCLUDE_DIR  ${_OPENEXR_INCLUDE_DIR} ${_OPENEXR_INCLUDE_DIR}/OpenEXR)
mark_as_advanced(OPENEXR_INCLUDE_DIR)

# Find libraries and add them to OPENEXR_LIBRARY
find_path(_OPENEXR_LIB_DIR OpenEXR
	HINTS ENV OPENEXR_LIB_DIR)
set(OPENEXR_LIB_DIR ${_OPENEXR_LIB_DIR})
mark_as_advanced(OPENEXR_LIB_DIR)

find_library(_OPENEXR_LIBRARY_IMIMF IlmImf
	HINTS ENV OPENEXR_LIB_DIR)
set (OPENEXR_LIBRARY ${OPENEXR_LIBRARY} ${_OPENEXR_LIBRARY_IMIMF})
find_library(_OPENEXR_LIBRARY_IMATH Imath 
	HINTS ENV OPENEXR_LIB_DIR)
set (OPENEXR_LIBRARY ${OPENEXR_LIBRARY} ${_OPENEXR_LIBRARY_IMATH})
find_library(_OPENEXR_LIBRARY_IEX Iex
	HINTS ENV OPENEXR_LIB_DIR)
set (OPENEXR_LIBRARY ${OPENEXR_LIBRARY} ${_OPENEXR_LIBRARY_IEX})
find_library(_OPENEXR_LIBRARY_ILMTHREAD IlmThread
	HINTS ENV OPENEXR_LIB_DIR)
set (OPENEXR_LIBRARY ${OPENEXR_LIBRARY} ${_OPENEXR_LIBRARY_ILMTHREAD})

mark_as_advanced(OPENEXR_LIBRARY)

# handle the QUIETLY and REQUIRED arguments and set OPENEXR_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(OPENEXR DEFAULT_MSG OPENEXR_LIBRARY OPENEXR_INCLUDE_DIR)

IF(OPENEXR_FOUND)
  SET(OpenEXR_LIBDIR ${OPENEXR_LIB_DIR})
  SET(OpenEXR_LIBRARIES ${OPENEXR_LIBRARY})
  SET(OpenEXR_INCLUDE_DIRS ${OPENEXR_INCLUDE_DIR})
ENDIF(OPENEXR_FOUND)
