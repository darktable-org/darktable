# - Try to find libsoup
# Find libsoup headers, libraries and the answer to all questions.
#
#  LibSoup2_FOUND                True if libsoup2 got found
#  LibSoup2_INCLUDE_DIRS         Location of libsoup2 headers
#  LibSoup2_LIBRARIES            List of libraries to use libsoup2
#  LibSoup2_LIBRARY_DIRS         Location of libsoup2 library
#
#  LibSoup22_FOUND               True if libsoup2.2 got found
#  LibSoup22_INCLUDE_DIRS        Location of libsoup2.2 headers
#  LibSoup22_LIBRARIES           List of libraries to use libsoup2.2
#  LibSoup22_LIBRARY_DIRS        Location of libsoup2.2 library
#
#  LibSoup24_FOUND               True if libsoup2.4 got found
#  LibSoup24_INCLUDE_DIRS        Location of libsoup2.4 headers
#  LibSoup24_LIBRARIES           List of libraries to use libsoup2.4
#  LibSoup24_LIBRARY_DIRS        Location of libsoup2.4 library
#
#  Set LibSoup2_MIN_VERSION to find libsoup2.2 or libsoup2.4 if only
#  one of both libraries is supported
#
#  Don't use LibSoup2_MIN_VERSION if you want to support
#  libsoup2.2 and libsoup2.4.
#  Instead use LibSoup22_MIN_VERSION and LibSoup24_MIN_VERSION.
#
#  Set LibSoup22_MIN_VERSION to find libsoup2.2 which version is
#  greater than LibSoup22_MIN_VERSION
#
#  Set LibSoup24_MIN_VERSION to find libsoup2.4 which version is
#  greater than LibSoup24_MIN_VERSION
#
#  WARNING: It is not possible to set LibSoup22_MIN_VERSION
#  and support any version of libsoup2.4 at the same time.
#  In this situation you have to set LibSoup24_MIN_VERSION also.
#  The same applies to LibSoup24_MIN_VERSION and libsoup2.2.
#
#  Copyright (c) 2007 Daniel Gollub <dgollub@suse.de>
#  Copyright (c) 2008 Bjoern Ricks  <bjoern.ricks@gmail.com>
#
#  Redistribution and use is allowed according to the terms of the New
#  BSD license.
#  For details see the accompanying COPYING-CMAKE-SCRIPTS file.
#

include(LibFindMacros)

if(LibSoup2_FIND_REQUIRED)
  set(_pkgconfig_REQUIRED "REQUIRED")
else(LibSoup2_FIND_REQUIRED)
  set(_pkgconfig_REQUIRED "")
endif(LibSoup2_FIND_REQUIRED)

if(LibSoup2_MIN_VERSION)
  string(REGEX REPLACE "^(2)(\\.)([0-9]*)(\\.?)(.*)" "\\3" LibSoup2_VERSION_MINOR "${LibSoup2_MIN_VERSION}")
  if(LibSoup2_VERSION_MINOR EQUAL "2")
    set(LibSoup22_MIN_VERSION "${LibSoup2_MIN_VERSION}")
  else(LibSoup2_VERSION_MINOR EQUAL "2")
    set(LibSoup24_MIN_VERSION "${LibSoup2_MIN_VERSION}")
  endif(LibSoup2_VERSION_MINOR EQUAL "2")
endif(LibSoup2_MIN_VERSION)


# try to find libsoup2.2>=LibSoup22_MIN_VERSION
if(LibSoup22_MIN_VERSION)
  libfind_pkg_search_module(LibSoup22 libsoup-2.2>=${LibSoup22_MIN_VERSION} libsoup2>=${LibSoup22_MIN_VERSION})
endif(LibSoup22_MIN_VERSION)

# try to find libsoup2.4>=LibSoup24_MIN_VERSION
if(LibSoup24_MIN_VERSION)
  libfind_pkg_search_module(LibSoup24 libsoup-2.4>=${LibSoup24_MIN_VERSION} libsoup2>=${LibSoup24_MIN_VERSION})
endif(LibSoup24_MIN_VERSION)

# try to find any version of libsoup2.4 if LibSoup22_MIN_VERSION is not set
if(NOT LibSoup24_FOUND AND NOT LibSoup22_MIN_VERSION)
  libfind_pkg_search_module(LibSoup24 libsoup-2.4 libsoup2)
endif(NOT LibSoup24_FOUND AND NOT LibSoup22_MIN_VERSION)

# try to find any version of libsoup2.2 if LibSoup24_MIN_VERSION is not set
if(NOT LibSoup22_FOUND AND NOT LibSoup24_MIN_VERSION)
  libfind_pkg_search_module(LibSoup22 libsoup-2.2 libsoup2)
endif(NOT LibSoup22_FOUND AND NOT LibSoup24_MIN_VERSION)

# set LibSoup2_ variables
if(LibSoup24_FOUND)
  # prefer libsoup2.4 to libsoup2.2 if both are found
  set(LibSoup2_FOUND ${LibSoup24_FOUND} CACHE INTERNAL "")
  set(LibSoup2_INCLUDE_DIRS ${LibSoup24_INCLUDE_DIRS} CACHE INTERNAL "")
  foreach(i ${LibSoup24_LIBRARIES})
    find_library(_libsoup2_LIBRARY NAMES ${i} HINTS ${LibSoup24_LIBRARY_DIRS})
    list(APPEND LibSoup2_LIBRARIES ${_libsoup2_LIBRARY})
    unset(_libsoup2_LIBRARY CACHE)
  endforeach(i)
  set(LibSoup2_VERSION ${LibSoup24_VERSION} CACHE INTERNAL "")
elseif(LibSoup22_FOUND)
  set(LibSoup2_FOUND ${LibSoup22_FOUND} CACHE INTERNAL "")
  set(LibSoup2_INCLUDE_DIRS ${LibSoup22_INCLUDE_DIRS} CACHE INTERNAL "")
  foreach(i ${LibSoup22_LIBRARIES})
    find_library(_libsoup2_LIBRARY NAMES ${i} HINTS ${LibSoup22_LIBRARY_DIRS})
    list(APPEND LibSoup2_LIBRARIES ${_libsoup2_LIBRARY})
    unset(_libsoup2_LIBRARY CACHE)
  endforeach(i)
  set(LibSoup2_VERSION ${LibSoup22_VERSION} CACHE INTERNAL "")
elseif(PKG_CONFIG_FOUND AND LibSoup2_FIND_REQUIRED)
  # raise an error if both libs are not found
  # and FIND_PACKAGE( LibSoup2 REQUIRED ) was called
  message( SEND_ERROR "package libsoup2 not found")
endif(LibSoup24_FOUND)

if(NOT LibSoup2_FOUND AND NOT PKG_CONFIG_FOUND)
  # WARNING:
  # This case is executed if pkg-config isn't installed.
  # Currently in this case it is only checked if libsoup2.2 is available.
  # Therefore please don't use this cmake module without pkg-config!
  find_path(_libsoup2_include_DIR libsoup/soup.h PATH_SUFFIXES libsoup libsoup-2.2)
  find_library( _libsoup2_LIBRARY soup-2.2)

  if(_libsoup2_include_DIR AND _libsoup2_LIBRARY)
    set(_libsoup2_FOUND TRUE)
  endif(_libsoup2_include_DIR AND _libsoup2_LIBRARY)

  if(_libsoup2_FOUND)
    set(LibSoup2_INCLUDE_DIRS ${_libsoup2_include_DIR})
    set(LibSoup2_LIBRARIES ${_libsoup2_LIBRARY})

    # find requited glib2
    if(NOT GLIB2_FOUND)
      find_package(GLIB2 REQUIRED)
      if(GLIB2_FOUND)
        set(LibSoup2_INCLUDE_DIRS ${LibSoup2_INCLUDE_DIRS} ${GLIB2_INCLUDE_DIRS})
        set(LibSoup2_LIBRARIES ${LibSoup2_LIBRARIES} ${GLIB2_LIBRARIES})
      endif(GLIB2_FOUND)
    endif(NOT GLIB2_FOUND)

    # find required libxml2
    if(NOT LIBXML2_FOUND)
      find_package(LibXml2 REQUIRED)
      if(LIBXML2_FOUND)
        set(LibSoup2_INCLUDE_DIRS ${LibSoup2_INCLUDE_DIRS} ${LIBXML2_INCLUDE_DIRS})
        set(LibSoup2_LIBRARIES ${LibSoup2_LIBRARIES} ${LIBXML2_LIBRARIES})
      endif(LIBXML2_FOUND)
    endif(NOT LIBXML2_FOUND)

    # find required gnutls
    if(NOT GNUTLS_FOUND)
      find_package(GNUTLS REQUIRED)
      if(GNUTLS_FOUND)
        set(LibSoup2_INCLUDE_DIRS ${LibSoup2_INCLUDE_DIRS} ${GNUTLS_INCLUDE_DIRS})
        set(LibSoup2_LIBRARIES ${LibSoup2_LIBRARIES} ${GNUTLS_LIBRARIES})
      endif(GNUTLS_FOUND)
    endif(NOT GNUTLS_FOUND)
  endif(_libsoup2_FOUND)

  mark_as_advanced(_libsoup2_include_DIR  _libsoup2_LIBRARY)

  # Report results
  if(LibSoup2_LIBRARIES AND LibSoup2_INCLUDE_DIRS AND _libsoup2_FOUND)
    set(LibSoup2_FOUND 1)
    if(NOT LibSoup2_FIND_QUIETLY)
      message(STATUS "Found libsoup2: ${_libsoup2_LIBRARY}")
    endif(NOT LibSoup2_FIND_QUIETLY)
  else(LibSoup2_LIBRARIES AND LibSoup2_INCLUDE_DIRS AND _libsoup2_FOUND)
    if(LibSoup2_FIND_REQUIRED)
      message(SEND_ERROR "Could NOT find libsoup2")
    else(LibSoup2_FIND_REQUIRED)
      if(NOT LibSoup2_FIND_QUIETLY)
        message(STATUS "Could NOT find libsoup2")
      endif(NOT LibSoup2_FIND_QUIETLY)
    endif(LibSoup2_FIND_REQUIRED)
  endif(LibSoup2_LIBRARIES AND LibSoup2_INCLUDE_DIRS AND _libsoup2_FOUND)
endif(NOT LibSoup2_FOUND AND NOT PKG_CONFIG_FOUND)

# Hide advanced variables from CMake GUIs
mark_as_advanced(LibSoup2_LIBRARIES LibSoup2_INCLUDE_DIRS)
