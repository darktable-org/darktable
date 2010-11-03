# - Try to find gconf2 
# Find gconf2 headers, libraries and the answer to all questions.
#
#  GCONF2_FOUND               True if gconf2 got found
#  GCONF2_INCLUDEDIR          Location of gconf2 headers 
#  GCONF2_LIBRARIES           List of libaries to use gconf2
#  GCONF2_DEFINITIONS         Definitions to compile gconf2 
#
# Copyright (c) 2007 Juha Tuomala <tuju@iki.fi>
# Copyright (c) 2007 Daniel Gollub <gollub@b1-systems.de>
# Copyright (c) 2007 Alban Browaeys <prahal@yahoo.com>
#
#  Redistribution and use is allowed according to the terms of the New
#  BSD license.
#  For details see the accompanying COPYING-CMAKE-SCRIPTS file.
#

INCLUDE( FindPkgConfig )
# Take care about gconf-2.0.pc settings
IF ( GConf2_FIND_REQUIRED )
  SET( _pkgconfig_REQUIRED "REQUIRED" )
ELSE ( GConf2_FIND_REQUIRED )
  SET( _pkgconfig_REQUIRED "" )
ENDIF ( GConf2_FIND_REQUIRED )

pkg_search_module( GCONF2 ${_pkgconfig_REQUIRED} gconf-2.0 )


# Look for gconf2 include dir and libraries w/o pkgconfig
IF ( NOT GCONF2_FOUND AND NOT PKG_CONFIG_FOUND )
	FIND_PATH( _gconf2_include_DIR gconf/gconf.h PATH_SUFFIXES gconf/2 
		PATHS
		/opt/local/include/
		/sw/include/
		/usr/local/include/
		/usr/include/
	)
	FIND_LIBRARY( _gconf2_link_DIR gconf-2
		PATHS
		/opt/local/lib
		/sw/lib
		/usr/lib
		/usr/local/lib
		/usr/lib64
		/usr/local/lib64
		/opt/lib64
	)
	IF ( _gconf2_include_DIR AND _gconf2_link_DIR )
		SET ( _gconf2_FOUND TRUE )
	ENDIF ( _gconf2_include_DIR AND _gconf2_link_DIR )


	IF ( _gconf2_FOUND )
		SET ( GCONF2_INCLUDE_DIRS ${_gconf2_include_DIR} )
		SET ( GCONF2_LIBRARIES ${_gconf2_link_DIR} )
	ENDIF ( _gconf2_FOUND )

	# Handle dependencies
	IF ( NOT ORBIT2_FOUND )
		FIND_PACKAGE( ORBit2 REQUIRED)
		IF ( ORBIT2_FOUND )
			SET ( GCONF2_INCLUDE_DIRS ${GCONF2_INCLUDE_DIRS} ${ORBIT2_INCLUDE_DIRS} )
			SET ( GCONF2_LIBRARIES ${GCONF2_LIBRARIES} ${ORBIT2_LIBRARIES} )
		ENDIF ( ORBIT2_FOUND )
	ENDIF ( NOT ORBIT2_FOUND )

	# Report results
	IF ( GCONF2_LIBRARIES AND GCONF2_INCLUDE_DIRS AND _gconf2_FOUND )	
		SET( GCONF2_FOUND 1 )
		IF ( NOT GConf2_FIND_QUIETLY )
			MESSAGE( STATUS "Found gconf2: ${GCONF2_LIBRARIES} ${GCONF2_INCLUDE_DIRS}" )
		ENDIF ( NOT GConf2_FIND_QUIETLY )
	ELSE ( GCONF2_LIBRARIES AND GCONF2_INCLUDE_DIRS AND _gconf2_FOUND )	
		IF ( GConf2_FIND_REQUIRED )
			MESSAGE( SEND_ERROR "Could NOT find gconf2" )
		ELSE ( GConf2_FIND_REQUIRED )
			IF ( NOT GConf2_FIND_QUIETLY )
				MESSAGE( STATUS "Could NOT find gconf2" )	
			ENDIF ( NOT GConf2_FIND_QUIETLY )
		ENDIF ( GConf2_FIND_REQUIRED )
	ENDIF ( GCONF2_LIBRARIES AND GCONF2_INCLUDE_DIRS AND _gconf2_FOUND )	

ENDIF ( NOT GCONF2_FOUND AND NOT PKG_CONFIG_FOUND )

# Hide advanced variables from CMake GUIs
MARK_AS_ADVANCED( GCONF2_LIBRARIES GCONF2_INCLUDE_DIRS )

