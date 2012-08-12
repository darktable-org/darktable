#
# Find the native LCMS includes and library
#

# This module defines
# LCMS_INCLUDE_DIR, where to find art*.h etc
# LCMS_LIBRARY, the libraries
# LCMS_FOUND, If false, do not try to use LCMS.
# LIBLCMS_LIBS, link information
# LIBLCMS_CFLAGS, cflags for include information


# INCLUDE(UsePkgConfig)

# use pkg-config to get the directories and then use these values
# in the FIND_PATH() and FIND_LIBRARY() calls
# PKGCONFIG(lcms _lcmsIncDir _lcmsLinkDir _lcmsLinkFlags _lcmsCflags)

# SET(LCMS2_LIBS ${_lcmsCflags})

FIND_PATH(LCMS2_INCLUDE_DIR lcms2.h
  PATHS /usr/include
  /usr/local/include
  HINTS ENV LCMS2_INCLUDE_DIR
  PATH_SUFFIXES lcms2
)

FIND_LIBRARY(LCMS2_LIBRARY
  NAMES ${LCMS2_NAMES} lcms2 liblcms2 lcms2dll
  PATHS /usr/lib /usr/local/lib
  HINTS ENV LCMS2_LIBDIR
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LCMS2 DEFAULT_MSG LCMS2_LIBRARY LCMS2_INCLUDE_DIR)

IF(LCMS2_FOUND)
  SET(LCMS2_LIBRARIES ${LCMS2_LIBRARY})
  SET(LCMS2_INCLUDE_DIRS ${LCMS2_INCLUDE_DIR})
ENDIF(LCMS2_FOUND)

