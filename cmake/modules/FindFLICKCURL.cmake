# - Try to find FlickCurl
# Once done, this will define
#
#  FLICKCURL_FOUND - system has Glib
#  FLICKCURL_INCLUDE_DIRS - the Glib include directories
#  FLICKCURL_LIBRARIES - link these to use Glib


# INCLUDE(UsePkgConfig)

# use pkg-config to get the directories and then use these values
# in the FIND_PATH() and FIND_LIBRARY() calls
# PKGCONFIG(flickcurl _flickcurlIncDir _flickcurlLinkDir _flickculrLinkFlags _flickcurlCflags)

# SET(FLICKCURL_LIBS ${_flickcurlCflags})

FIND_PATH(FLICKCURL_INCLUDE_DIR flickcurl.h
  PATHS /usr/include
  /usr/local/include
  /opt/local/include
  HINTS ENV FLICKCURL_INCLUDE_DIR
  PATH_SUFFIXES flickcurl
)

FIND_LIBRARY(FLICKCURL_LIBRARY
  NAMES ${FLICKCURL_NAMES} flickcurl libflickcurl.so libflickcurl.dylib
  PATHS /usr/lib /usr/local/lib /opt/local/lib
  HINTS ENV FLICKCURL_LIBRARY
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FLICKCURL DEFAULT_MSG FLICKCURL_LIBRARY FLICKCURL_INCLUDE_DIR)

IF(FLICKCURL_FOUND)
  SET(FLICKCURL_LIBRARIES ${FLICKCURL_LIBRARY})
  SET(FLICKCURL_INCLUDE_DIRS ${FLICKCURL_INCLUDE_DIR})
ENDIF(FLICKCURL_FOUND)
