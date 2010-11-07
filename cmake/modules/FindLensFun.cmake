# - Try to find lensfun
# Once done this will define
#
#  LENSFUN_FOUND - system has lensfun
#  LENSFUN_INCLUDE_DIRS - the lensfun include directory
#  LENSFUN_LIBRARIES - Link these to use lensfun
#  LENSFUN_DEFINITIONS - Compiler switches required for using lensfun
#
#  Copyright (c) 2008 Adrian Schroeter <adrian@suse.de>
#
#  Redistribution and use is allowed according to the terms of the New
#  BSD license.
#  For details see the accompanying COPYING-CMAKE-SCRIPTS file.
#

if (LENSFUN_LIBRARIES AND LENSFUN_INCLUDE_DIRS)
  # in cache already
  set(LENSFUN_FOUND TRUE)
else (LENSFUN_LIBRARIES AND LENSFUN_INCLUDE_DIRS)
  # use pkg-config to get the directories and then use these values
  # in the FIND_PATH() and FIND_LIBRARY() calls
  IF (NOT WIN32)
    include(UsePkgConfig)

    pkgconfig(lensfun _lensfunIncDir _lensfunLinkDir _lensfunLinkFlags _lensfunCflags)
  
    set(LENSFUN_DEFINITIONS ${_lensfunCflags})
  ENDIF (NOT WIN32)

  find_path(LENSFUN_INCLUDE_DIR
    NAMES
      lensfun.h
    PATHS
      ${_lensfunIncDir}
      /opt/local/include
      /sw/include
  )

  find_library(LENSFUN_LIBRARY
    NAMES
      lensfun
    PATHS
      ${_lensfunLinkDir}
      /opt/local/lib
      /sw/lib
  )

  if (LENSFUN_LIBRARY)
    set(LENSFUN_FOUND TRUE)
  endif (LENSFUN_LIBRARY)

  set(LENSFUN_INCLUDE_DIRS
    ${LENSFUN_INCLUDE_DIR}
  )

  if (LENSFUN_FOUND)
    set(LENSFUN_LIBRARIES
      ${LENSFUN_LIBRARIES}
      ${LENSFUN_LIBRARY}
    )
  endif (LENSFUN_FOUND)

  FIND_PACKAGE_HANDLE_STANDARD_ARGS(LensFun DEFAULT_MSG LENSFUN_INCLUDE_DIRS LENSFUN_LIBRARIES )

  # show the LENSFUN_INCLUDE_DIRS and LENSFUN_LIBRARIES variables only in the advanced view
  mark_as_advanced(LENSFUN_INCLUDE_DIRS LENSFUN_LIBRARIES)

endif (LENSFUN_LIBRARIES AND LENSFUN_INCLUDE_DIRS)

