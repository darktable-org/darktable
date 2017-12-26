# - Try to find libsoup
# Find libsoup headers, libraries and the answer to all questions.
#
#  LIBSOUP2_FOUND                True if libsoup2 got found
#  LIBSOUP2_INCLUDE_DIRS         Location of libsoup2 headers 
#  LIBSOUP2_LIBRARIES            List of libraries to use libsoup2
#  LIBSOUP2_LIBRARY_DIRS         Location of libsoup2 library
#
#  LIBSOUP22_FOUND               True if libsoup2.2 got found
#  LIBSOUP22_INCLUDE_DIRS        Location of libsoup2.2 headers 
#  LIBSOUP22_LIBRARIES           List of libraries to use libsoup2.2
#  LIBSOUP22_LIBRARY_DIRS        Location of libsoup2.2 library
#
#  LIBSOUP24_FOUND               True if libsoup2.4 got found
#  LIBSOUP24_INCLUDE_DIRS        Location of libsoup2.4 headers 
#  LIBSOUP24_LIBRARIES           List of libraries to use libsoup2.4
#  LIBSOUP24_LIBRARY_DIRS        Location of libsoup2.4 library
#
#  Set LIBSOUP2_MIN_VERSION to find libsoup2.2 or libsoup2.4 if only 
#  one of both libraries is supported
#
#  Don't use LIBSOUP2_MIN_VERSION if you want to support 
#  libsoup2.2 and libsoup2.4. 
#  Instead use LIBSPOUP22_MIN_VERSION and LIBSPOUP24_MIN_VERSION.
#
#  Set LIBSPOUP22_MIN_VERSION to find libsoup2.2 which version is
#  greater than LIBSPOUP22_MIN_VERSION
#
#  Set LIBSPOUP24_MIN_VERSION to find libsoup2.4 which version is
#  greater than LIBSPOUP24_MIN_VERSION
#
#  WARNING: It is not possible to set LIBSPOUP22_MIN_VERSION 
#  and support any version of libsoup2.4 at the same time.
#  In this situation you have to set LIBSPOUP24_MIN_VERSION also.
#  The same applies to LIBSPOUP24_MIN_VERSION and libsoup2.2.
#
#  Copyright (c) 2007 Daniel Gollub <dgollub@suse.de>
#  Copyright (c) 2008 Bjoern Ricks  <bjoern.ricks@gmail.com>
#
#  Redistribution and use is allowed according to the terms of the New
#  BSD license.
#  For details see the accompanying COPYING-CMAKE-SCRIPTS file.
#

INCLUDE( FindPkgConfig )

IF ( LibSoup2_FIND_REQUIRED )
	SET( _pkgconfig_REQUIRED "REQUIRED" )
ELSE( LibSoup2_FIND_REQUIRED )
	SET( _pkgconfig_REQUIRED "" )	
ENDIF ( LibSoup2_FIND_REQUIRED )

IF ( LIBSOUP2_MIN_VERSION )
	STRING(REGEX REPLACE "^(2)(\\.)([0-9]*)(\\.?)(.*)" "\\3" LIBSOUP2_VERSION_MINOR "${LIBSOUP2_MIN_VERSION}")
	IF ( LIBSOUP2_VERSION_MINOR EQUAL "2" )
		SET( LIBSOUP22_MIN_VERSION "${LIBSOUP2_MIN_VERSION}" )
	ELSE ( LIBSOUP2_VERSION_MINOR EQUAL "2" )
		SET( LIBSOUP24_MIN_VERSION "${LIBSOUP2_MIN_VERSION}" )
	ENDIF ( LIBSOUP2_VERSION_MINOR EQUAL "2" )
ENDIF ( LIBSOUP2_MIN_VERSION )

# try to find libsoup2.2>=LIBSOUP22_MIN_VERSION
IF ( LIBSOUP22_MIN_VERSION )
	PKG_SEARCH_MODULE( LIBSOUP22 libsoup-2.2>=${LIBSOUP22_MIN_VERSION} libsoup2>=${LIBSOUP22_MIN_VERSION} )
ENDIF ( LIBSOUP22_MIN_VERSION )

# try to find libsoup2.4>=LIBSOUP24_MIN_VERSION
IF ( LIBSOUP24_MIN_VERSION )
	PKG_SEARCH_MODULE( LIBSOUP24 libsoup-2.4>=${LIBSOUP24_MIN_VERSION} libsoup2>=${LIBSOUP24_MIN_VERSION} )
ENDIF ( LIBSOUP24_MIN_VERSION )	

# try to find any version of libsoup2.4 if LIBSOUP22_MIN_VERSION is not set
IF ( NOT LIBSOUP24_FOUND AND NOT LIBSOUP22_MIN_VERSION )
	PKG_SEARCH_MODULE( LIBSOUP24 libsoup-2.4 libsoup2 )
ENDIF ( NOT LIBSOUP24_FOUND AND NOT LIBSOUP22_MIN_VERSION)

# try to find any version of libsoup2.2 if LIBSOUP24_MIN_VERSION is not set
IF ( NOT LIBSOUP22_FOUND AND NOT LIBSOUP24_MIN_VERSION )
	PKG_SEARCH_MODULE( LIBSOUP22 libsoup-2.2 libsoup2 )
ENDIF ( NOT LIBSOUP22_FOUND AND NOT LIBSOUP24_MIN_VERSION)

# set LIBSOUP2_ variables
IF ( LIBSOUP24_FOUND )
	# prefer libsoup2.4 to libsoup2.2 if both are found
	SET( LIBSOUP2_FOUND ${LIBSOUP24_FOUND} CACHE INTERNAL "" )
	SET( LIBSOUP2_INCLUDE_DIRS ${LIBSOUP24_INCLUDE_DIRS} CACHE INTERNAL "" )
	foreach(i ${LIBSOUP24_LIBRARIES})
		find_library(_libsoup2_LIBRARY NAMES ${i} HINTS ${LIBSOUP24_LIBRARY_DIRS})
		LIST(APPEND LIBSOUP2_LIBRARIES ${_libsoup2_LIBRARY})
		unset(_libsoup2_LIBRARY CACHE)
	endforeach(i)
	SET( LIBSOUP2_VERSION ${LIBSOUP24_VERSION} CACHE INTERNAL "" )
ELSEIF ( LIBSOUP22_FOUND )
	SET( LIBSOUP2_FOUND ${LIBSOUP22_FOUND} CACHE INTERNAL "" )
	SET( LIBSOUP2_INCLUDE_DIRS ${LIBSOUP22_INCLUDE_DIRS} CACHE INTERNAL "" )
	foreach(i ${LIBSOUP22_LIBRARIES})
		find_library(_libsoup2_LIBRARY NAMES ${i} HINTS ${LIBSOUP22_LIBRARY_DIRS})
		LIST(APPEND LIBSOUP2_LIBRARIES ${_libsoup2_LIBRARY})
		unset(_libsoup2_LIBRARY CACHE)
	endforeach(i)
	SET( LIBSOUP2_VERSION ${LIBSOUP22_VERSION} CACHE INTERNAL "" )
ELSEIF( PKG_CONFIG_FOUND AND LibSoup2_FIND_REQUIRED )
	# raise an error if both libs are not found 
	# and FIND_PACKAGE( LibSoup2 REQUIRED ) was called
	MESSAGE( SEND_ERROR "package libsoup2 not found" )
ENDIF ( LIBSOUP24_FOUND )

IF( NOT LIBSOUP2_FOUND AND NOT PKG_CONFIG_FOUND )
	# WARNING:
	# This case is executed if pkg-config isn't installed.
	# Currently in this case it is only checked if libsoup2.2 is available.
	# Therefore please don't use this cmake module without pkg-config!
	FIND_PATH( _libsoup2_include_DIR libsoup/soup.h PATH_SUFFIXES libsoup libsoup-2.2 )
	FIND_LIBRARY( _libsoup2_LIBRARY soup-2.2)

	IF ( _libsoup2_include_DIR AND _libsoup2_LIBRARY )
		SET ( _libsoup2_FOUND TRUE )
	ENDIF ( _libsoup2_include_DIR AND _libsoup2_LIBRARY )

	IF ( _libsoup2_FOUND )
		SET ( LIBSOUP2_INCLUDE_DIRS ${_libsoup2_include_DIR} )
		SET ( LIBSOUP2_LIBRARIES ${_libsoup2_LIBRARY} )
	
		# find requited glib2
		IF( NOT GLIB2_FOUND )
			FIND_PACKAGE( GLIB2 REQUIRED )
			IF ( GLIB2_FOUND )
				SET ( LIBSOUP2_INCLUDE_DIRS ${LIBSOUP2_INCLUDE_DIRS} ${GLIB2_INCLUDE_DIRS} )
				SET ( LIBSOUP2_LIBRARIES ${LIBSOUP2_LIBRARIES} ${GLIB2_LIBRARIES} )
			ENDIF ( GLIB2_FOUND )
		ENDIF( NOT GLIB2_FOUND )
		
		# find required libxml2
		IF( NOT LIBXML2_FOUND )
			FIND_PACKAGE( LibXml2 REQUIRED )
			IF ( LIBXML2_FOUND )
				SET ( LIBSOUP2_INCLUDE_DIRS ${LIBSOUP2_INCLUDE_DIRS} ${LIBXML2_INCLUDE_DIRS} )
				SET ( LIBSOUP2_LIBRARIES ${LIBSOUP2_LIBRARIES} ${LIBXML2_LIBRARIES} )
			ENDIF( LIBXML2_FOUND )
		ENDIF( NOT LIBXML2_FOUND )
		
		# find required gnutls
		IF( NOT GNUTLS_FOUND )
			FIND_PACKAGE( GNUTLS REQUIRED )
			IF ( GNUTLS_FOUND )
				SET ( LIBSOUP2_INCLUDE_DIRS ${LIBSOUP2_INCLUDE_DIRS} ${GNUTLS_INCLUDE_DIRS} )
				SET ( LIBSOUP2_LIBRARIES ${LIBSOUP2_LIBRARIES} ${GNUTLS_LIBRARIES} )
			ENDIF( GNUTLS_FOUND )
		ENDIF( NOT GNUTLS_FOUND )
	ENDIF ( _libsoup2_FOUND )

	MARK_AS_ADVANCED( _libsoup2_include_DIR  _libsoup2_LIBRARY )

	# Report results
	IF ( LIBSOUP2_LIBRARIES AND LIBSOUP2_INCLUDE_DIRS AND _libsoup2_FOUND )	
		SET( LIBSOUP2_FOUND 1 )
		IF ( NOT LibSoup2_FIND_QUIETLY )
			MESSAGE( STATUS "Found libsoup2: ${_libsoup2_LIBRARY}" )
		ENDIF ( NOT LibSoup2_FIND_QUIETLY )
	ELSE ( LIBSOUP2_LIBRARIES AND LIBSOUP_INCLUDE_DIRS AND _libsoup2_FOUND )	
		IF ( LibSoup2_FIND_REQUIRED )
			MESSAGE( SEND_ERROR "Could NOT find libsoup2" )
		ELSE ( LibSoup2_FIND_REQUIRED )
			IF ( NOT LibSoup2_FIND_QUIETLY )
				MESSAGE( STATUS "Could NOT find libsoup2" )	
			ENDIF ( NOT LibSoup2_FIND_QUIETLY )
		ENDIF ( LibSoup2_FIND_REQUIRED )
	ENDIF ( LIBSOUP2_LIBRARIES AND LIBSOUP2_INCLUDE_DIRS AND _libsoup2_FOUND )
ENDIF( NOT LIBSOUP2_FOUND AND NOT PKG_CONFIG_FOUND )

# Hide advanced variables from CMake GUIs
MARK_AS_ADVANCED( LIBSOUP2_LIBRARIES LIBSOUP2_INCLUDE_DIRS )
