project(darktable CXX C)
cmake_minimum_required(VERSION 2.6)
cmake_policy(VERSION 2.6)
set(PROJECT_VERSION "0.6+")

#
# Configurable options
#
option(DARKTABLE_USE_GCONF_IF_AVAILABLE on)			# Does check if gconf is available by default

# Avoid source tree pollution
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
	message(FATAL_ERROR "In-source builds are not permitted. Make a separate folder for building:\nmkdir build; cd build; cmake ..\nBefore that, remove the files already created:\nrm -rf CMakeCache.txt CMakeFiles")
endif(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/modules/")

# Add a sensible build type default and warning because empty means no optimization and no debug info.
if(NOT CMAKE_BUILD_TYPE)
	message("WARNING: CMAKE_BUILD_TYPE is not defined!\n         Defaulting to CMAKE_BUILD_TYPE=RelWithDebInfo. Use ccmake to set a proper value.")
	SET(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel." FORCE)
endif(NOT CMAKE_BUILD_TYPE)

if(WIN32)
	# use current directory
	set(RELATIVE_PATH_FROM_BIN "." CACHE STRING "Relative path to library from binary")
else(WIN32)
	# use parent directory
	set(RELATIVE_PATH_FROM_BIN ".." CACHE STRING "Relative path to library from binary")
endif(WIN32)

if(WIN32)
	set(SHARE_INSTALL "." CACHE STRING "Data file install path. Must be a relative path (from CMAKE_INSTALL_PREFIX), with no trailing slash.")
else(WIN32)
	if(NOT SHARE_INSTALL)
		set(SHARE_INSTALL "share/darktable" CACHE STRING "Data file install path. Must be a relative path (from CMAKE_INSTALL_PREFIX), with no trailing slash.")
	endif(NOT SHARE_INSTALL)
endif(WIN32)

mark_as_advanced(SHARE_INSTALL)

if(WIN32)
	set(LIB_INSTALL "." CACHE STRING "Library file install path. Must be a relative path (from CMAKE_INSTALL_PREFIX), with no trailing slash.")
else(WIN32)
	if(NOT LIB_INSTALL)
		set( LIB_INSTALL "lib" CACHE STRING "Library file install path. Must be a relative path (from CMAKE_INSTALL_PREFIX), with no trailing slash.")
	endif(NOT LIB_INSTALL)
endif(WIN32)

mark_as_advanced(LIB_INSTALL)

#need to put here do setting LOCALE_DIR variable
find_package(Gettext)
if(Gettext_FOUND)
	if(NOT LOCALE_DIR)
		if(WIN32)
			SET(LOCALE_DIR "bin/locale")
		elseif(APPLE)
			SET(LOCALE_DIR "share/locale")
		else()#other os
			SET(LOCALE_DIR "share/locale")
		endif(WIN32)
	endif(NOT LOCALE_DIR)
	
	if(NOT LOCALE_DIR)
		message(QUIET "Set LOCALE_DIR to path to get Gettext working")
	else()
		find_package(Msgfmt)
		if(Msgfmt_FOUND)
			message(STATUS "Found msgfmt to convert language file. Translation enabled")
			add_subdirectory(po)
		else()
			message(STATUS "Cannot find msgfmt to convert language file. Translation won't be enabled")
		endif()
	endif(NOT LOCALE_DIR)
endif(Gettext_FOUND)

add_subdirectory(src)

# Disable check of gconf on win32 platform
if(WIN32)
	option(DARKTABLE_USE_GCONF_IF_AVAILABLE off)
endif(WIN32)

#
# Add files that should go into shared
#
set(SHARE_FILES darktable.gtkrc darktable.glade)
install(FILES ${SHARE_FILES} DESTINATION ${SHARE_INSTALL})
