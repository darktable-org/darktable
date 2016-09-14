# - Find the native pthreads includes and library
#
# This module defines
#  PTHREAD_INCLUDE_DIR, where to find sqlite3.h, etc.
#  PTHREAD_LIBRARIES, the libraries to link against to use sqlite3.
#  PTHREAD_FOUND, If false, do not try to use sqlite3.
# also defined, but not for general use are
#  PTHREAD_LIBRARY, where to find the sqlite3 library.


#=============================================================================
# Copyright 2010 henrik andersson
#=============================================================================

SET(PTHREAD_FIND_REQUIRED ${PThread_FIND_REQUIRED})

if(WIN32)
  ###############################################################################
  # find thread library

  # for testing for c++ system include files
  include(CheckIncludeFileCXX)
  check_include_file_cxx(pthread.h HAVE_PTHREAD_H)

  if(MINGW AND NOT HAVE_PTHREAD_H)
    set(USE_STD_THREADS ON)
  endif()

  find_package(Threads REQUIRED)
  set(DT_POSIX_THREADS "1")
elseif(WIN32)

  find_path(PTHREAD_INCLUDE_DIR pthread.h)
  mark_as_advanced(PTHREAD_INCLUDE_DIR)

  set(PTHREAD_NAMES ${PTHREAD_NAMES} pthread libpthread)
  find_library(PTHREAD_LIBRARY NAMES ${PTHREAD_NAMES} )
  mark_as_advanced(PTHREAD_LIBRARY)

  # handle the QUIETLY and REQUIRED arguments and set PTHREAD_FOUND to TRUE if
  # all listed variables are TRUE
  include(FindPackageHandleStandardArgs)
  find_package_handle_standard_args(PThread DEFAULT_MSG PTHREAD_LIBRARY PTHREAD_INCLUDE_DIR)

  IF(PTHREAD_FOUND)
    SET(PThread_LIBRARIES ${PTHREAD_LIBRARY})
    SET(PThread_INCLUDE_DIRS ${PTHREAD_INCLUDE_DIR})
  ENDIF(PTHREAD_FOUND)
endif(WIN32)