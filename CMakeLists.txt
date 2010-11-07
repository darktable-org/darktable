project(darktable CXX C)
cmake_minimum_required(VERSION 2.6)
cmake_policy(VERSION 2.6)

option(USE_GCONF_BACKEND "Detect and use gconf backend if available." ON)
option(USE_CAMERA_SUPPORT "Detect and use camera support if available." ON)


#
# Set platform defaults...
#
if(APPLE)
	message("-- Mac OSX build detected, setting default features");
	set(USE_GCONF_BACKEND OFF) 
	set(USE_CAMERA_SUPPORT OFF)
endif(APPLE)

if(WIN32)
 message("-- Win32 build detected, setting default features");
  set(USE_GCONF_BACKEND OFF) 
  set(USE_CAMERA_SUPPORT OFF)
endif(WIN32)


#
# Auto create version
#
exec_program(git ARGS describe --tags HEAD | "sed 's,^release-,,;s,-,+,;s,-,~,;'" OUTPUT_VARIABLE _dt_decoration)
exec_program(git ARGS rev-parse --short HEAD OUTPUT_VARIABLE _dt_sha1sum)
execute_process(COMMAND echo "#ifndef DT_VERSION_H\n#define DT_VERSION_H\n#define DT_VERSION_SHA1SUM \"${_dt_sha1sum}\"\n#define DT_VERSION_DECORATION \"${_dt_decoration}\"\n#endif" OUTPUT_VARIABLE _version_h)
file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/src/common/version.h "${_version_h}")

set(PROJECT_VERSION "${_dt_decoration}")

include(cmake/darktable-packaging.cmake)

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
	set(RELATIVE_PATH_FROM_BIN ${CMAKE_INSTALL_PREFIX} CACHE STRING "Relative path to library from binary")
else(WIN32)
	set(RELATIVE_PATH_FROM_BIN ${CMAKE_INSTALL_PREFIX} CACHE STRING "Relative path to library from binary")
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


#
# ##
#
if(APPLE)
	set(CMAKE_LIBRARY_PATH ${CMAKE_LIBRARY_PATH} /opt/local/lib)
	set(CMAKE_INCLUDE_PATH ${CMAKE_INCLUDE_PATH} /opt/local/include)
endif(APPLE)

# lets continue into build directories
add_subdirectory(src)
add_subdirectory(data)
add_subdirectory(doc)

