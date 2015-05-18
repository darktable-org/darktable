# - Find the native exiv2 includes and library
#
# This module defines
#  EXIV2_INCLUDE_DIR, where to find png.h, etc.
#  EXIV2_LIBRARIES, the libraries to link against to use exiv2
#  EXIV2_FOUND, If false, do not try to use exiv2
# also defined, but not for general use are
#  EXIV2_LIBRARY, where to find the exiv2 library
#  EXIV2_VERSION, the version we are compiling against

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

if(EXIV2_INCLUDE_DIR  AND NOT  EXIV2_VERSION)
  file(READ ${EXIV2_INCLUDE_DIR}/exiv2/version.hpp EXIV2_VERSION_CONTENT)
  string(REGEX MATCH "#define EXIV2_MAJOR_VERSION +\\( *([0-9]+) *\\)"  _dummy "${EXIV2_VERSION_CONTENT}")
  set(EXIV2_VERSION_MAJOR "${CMAKE_MATCH_1}")

  string(REGEX MATCH "#define EXIV2_MINOR_VERSION +\\( *([0-9]+) *\\)"  _dummy "${EXIV2_VERSION_CONTENT}")
  set(EXIV2_VERSION_MINOR "${CMAKE_MATCH_1}")

  string(REGEX MATCH "#define EXIV2_PATCH_VERSION +\\( *([0-9]+) *\\)"  _dummy "${EXIV2_VERSION_CONTENT}")
  set(EXIV2_VERSION_PATCH "${CMAKE_MATCH_1}")

  set(EXIV2_VERSION "${EXIV2_VERSION_MAJOR}.${EXIV2_VERSION_MINOR}.${EXIV2_VERSION_PATCH}")
endif(EXIV2_INCLUDE_DIR  AND NOT  EXIV2_VERSION)

if(EXIV2_VERSION VERSION_LESS Exiv2_FIND_VERSION)
  message(FATAL_ERROR "Exiv2 version check failed.  Version ${EXIV2_VERSION} was found, at least version ${Exiv2_FIND_VERSION} is required")
endif(EXIV2_VERSION VERSION_LESS Exiv2_FIND_VERSION)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(EXIV2 DEFAULT_MSG EXIV2_LIBRARY EXIV2_INCLUDE_DIR)

IF(EXIV2_FOUND)
  SET(Exiv2_LIBRARIES ${EXIV2_LIBRARY})
  SET(Exiv2_INCLUDE_DIRS ${EXIV2_INCLUDE_DIR})
ENDIF(EXIV2_FOUND)
