project(darktable CXX C)
cmake_minimum_required(VERSION 2.6)
cmake_policy(VERSION 2.6)

option(USE_CAMERA_SUPPORT "Detect and use camera support if available." ON)
option(USE_NLS "Build Native Language Support (using gettext)" ON)
option(USE_COLORD "Enable colord support" ON)
option(USE_GEO "Build geotagging parts" ON)
option(USE_FLICKR "Enable Flickr support" ON)
option(USE_GLIBJSON "Enable GlibJson support" ON)
option(USE_GNOME_KEYRING "Build gnome-keyring password storage backend" ON)
option(USE_UNITY "Use libunity to report progress in the launcher" OFF)
option(BUILD_SLIDESHOW "Build the opengl slideshow viewer" ON)
option(USE_OPENMP "Use openmp threading support." ON)
option(USE_OPENCL "Use OpenCL support." ON)
option(USE_GRAPHICSMAGICK "Use GraphicsMagick library for image import." ON)
option(USE_DARKTABLE_PROFILING OFF)
option(CUSTOM_CFLAGS "Don't override compiler optimization flags." OFF)
option(DONT_USE_RAWSPEED "Dont compile rawspeed backend." OFF)
option(BUILD_USERMANUAL "Build all the versions of the usermanual." OFF)
option(INSTALL_IOP_EXPERIMENTAL "Also install unstable, unfinished, broken, and likely-to-change-soon plugins." OFF)
option(INSTALL_IOP_LEGACY "Also install old plugins we want to get rid of." OFF)
option(BINARY_PACKAGE_BUILD "Sets march optimization to generic" OFF)
option(APRIL_FOOLS "Add the \"file manager\" (a shell) to lighttable mode" OFF)
option(GTK3_MIGRATION_CHECKS "Help getting darktable ready for GTK3" OFF)
option(USE_XMLLINT "Run xmllint to test if darktableconfig.xml is valid" ON)
option(USE_OPENJPEG "Enable JPEG 2000 support" ON)
if(APPLE)
	option(USE_MAC_INTEGRATION "Enable OS X integration" ON)
else(APPLE)
	set(USE_MAC_INTEGRATION OFF)
endif(APPLE)

# Generate multi arch triplet
EXECUTE_PROCESS(COMMAND dpkg-architecture -qDEB_HOST_MULTIARCH OUTPUT_VARIABLE CMAKE_ARCH_TRIPLET OUTPUT_STRIP_TRAILING_WHITESPACE)
if(CMAKE_ARCH_TRIPLET)
	message("-- multiarch triplet detected: " ${CMAKE_ARCH_TRIPLET})
	LIST(INSERT CMAKE_PLATFORM_IMPLICIT_LINK_DIRECTORIES 0 /lib/${CMAKE_ARCH_TRIPLET}
		/usr/lib/${CMAKE_ARCH_TRIPLET})
endif()

#
# Set platform defaults...
#
if(APPLE)
	message("-- Mac OSX build detected, setting default features")
	set(BUILD_SLIDESHOW OFF)

	# prefer macports and/or user-installed libraries over system ones
	LIST(APPEND CMAKE_PREFIX_PATH /opt/local /usr/local)
	set(CMAKE_FIND_FRAMEWORK "LAST")
	# except libstdc++ (only one linked via -l flag, not full path)
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -L/usr/lib")
	set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -L/usr/lib")
	set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -L/usr/lib")

	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_DARWIN_C_SOURCE")
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_DARWIN_C_SOURCE")
	add_definitions("-DOS_OBJECT_USE_OBJC=0")
	# Make sure we don't use functions unavailable in OS X 10.6 and later (only works with system compiler, vanilla gcc from macports ignores this)
	add_definitions("-D__MAC_OS_X_VERSION_MAX_ALLOWED=1060")
endif(APPLE)

if(CMAKE_COMPILER_IS_GNUCC)
  execute_process(COMMAND ${CMAKE_C_COMPILER} -dumpversion
      OUTPUT_VARIABLE GCC_VERSION)
  if (GCC_VERSION VERSION_LESS 4.3)
		message("-- Disabling OpenMP because GCC is 4.2 or older!")
		set(USE_OPENMP OFF)
  endif()
endif()

if(WIN32)
 message("-- Win32 build detected, setting default features")
  set(USE_CAMERA_SUPPORT OFF)
endif(WIN32)

# Check if this is source package build
if(NOT IS_DIRECTORY ${CMAKE_SOURCE_DIR}/.git)
	set(SOURCE_PACKAGE 1)
endif()

#
# Set package version
#
if(NOT SOURCE_PACKAGE)
	message("-- Determining version")
	exec_program(git ${CMAKE_CURRENT_SOURCE_DIR} ARGS describe --tags HEAD | "sed 's,^release-,,;s,-,+,;s,-,~,;'" OUTPUT_VARIABLE PROJECT_VERSION)
	configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.cmake.cmake" "${CMAKE_CURRENT_BINARY_DIR}/version.cmake" @ONLY)
else(NOT SOURCE_PACKAGE)
	include(cmake/version.cmake)
endif()

#
# Initial cmake/debian/postinst and prerm script for debian package
#
make_directory(${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian)


#
# Avoid source tree pollution
#
If(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
	message(FATAL_ERROR "In-source builds are not permitted. Make a separate folder for building:\nmkdir build; cd build; cmake ..\nBefore that, remove the files already created:\nrm -rf CMakeCache.txt CMakeFiles")
endif(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/modules/")

# Add a sensible build type default and warning because empty means no optimization and no debug info.
if(NOT CMAKE_BUILD_TYPE)
	message("WARNING: CMAKE_BUILD_TYPE is not defined!\n         Defaulting to CMAKE_BUILD_TYPE=RelWithDebInfo. Use ccmake to set a proper value.")
	SET(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel." FORCE)
endif(NOT CMAKE_BUILD_TYPE)

if(CMAKE_BUILD_TYPE MATCHES "^[Dd][Ee][Bb][Uu][Gg]$")
  message(FATAL_ERROR "ERROR: Debug build type most likely isn't what you want, use RelWithDebInfo instead. If you're absolutely sure that this is what you want then just comment out this line.")
endif()

if(WIN32)
	set(SHARE_INSTALL "." CACHE STRING "Data file install path. Must be a relative path (from CMAKE_INSTALL_PREFIX), with no trailing slash.")
else(WIN32)
	if(NOT SHARE_INSTALL)
		set(SHARE_INSTALL "share" CACHE STRING "Data file install path. Must be a relative path (from CMAKE_INSTALL_PREFIX), with no trailing slash.")
	endif(NOT SHARE_INSTALL)
endif(WIN32)

mark_as_advanced(SHARE_INSTALL)

if(WIN32)
	set(LIB_INSTALL "." CACHE STRING "Library file install path. Must be a relative path (from CMAKE_INSTALL_PREFIX), with no trailing slash.")
else(WIN32)
	if(NOT LIB_INSTALL)
		set( LIB_INSTALL lib${LIB_SUFFIX} CACHE STRING "Library file install path. Must be a relative path (from CMAKE_INSTALL_PREFIX), with no trailing slash.")
	endif(NOT LIB_INSTALL)
endif(WIN32)

mark_as_advanced(LIB_INSTALL)

#need to put here do setting LOCALE_DIR variable
if(USE_NLS)
find_package(Gettext)
if(Gettext_FOUND)
	if(NOT LOCALE_DIR)
		if(WIN32)
			SET(LOCALE_DIR "bin/locale")
		else()#other os
			SET(LOCALE_DIR "${CMAKE_INSTALL_PREFIX}/share/locale")
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
endif(USE_NLS)

#find a temporary directory
if(NOT TMP_DIR)
	SET(TMP_DIR "~/.local/tmp")
endif(NOT TMP_DIR)

#find a cache directory
if(NOT CACHE_DIR)
	SET(CACHE_DIR "~/.cache/darktable")
endif(NOT CACHE_DIR)

# needed to generate file "preferences_gen.h" accordingly
if(USE_OPENCL)
	SET(HAVE_OPENCL 1)
else()
	SET(HAVE_OPENCL 0)
endif(USE_OPENCL)

# See http://developer.gnome.org/gtk3/3.0/gtk-migrating-2-to-3.html
if(GTK3_MIGRATION_CHECKS)
	message("-- Add defines to help GTK3 migration")
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DGTK_DISABLE_SINGLE_INCLUDES -DGDK_DISABLE_DEPRECATED -DGTK_DISABLE_DEPRECATED -DGSEAL_ENABLE")
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DGTK_DISABLE_SINGLE_INCLUDES -DGDK_DISABLE_DEPRECATED -DGTK_DISABLE_DEPRECATED -DGSEAL_ENABLE")
endif(GTK3_MIGRATION_CHECKS)

#
# Test SSE level
#
# if(NOT USE_SSE_FLAG)
# 	set(SSE_C_TESTS "sse4" "sse3" "sse2" "sse")
# 	message("-- Checking SSE instructions support by current CPU")
# 	foreach(sse_test ${SSE_C_TESTS})
# 		if(NOT SSE_FOUND)
# 			if(WIN32)
# 				set(SSE_CHECK_COMMAND "FAILURE")
# 			elseif(APPLE)
# 				set(SSE_CHECK_COMMAND sysctl -a | grep ${sse_test})
# 			else()#other os
# 				set(SSE_CHECK_COMMAND grep ${sse_test} /proc/cpuinfo)
# 			endif(WIN32)
# 			execute_process(COMMAND ${SSE_CHECK_COMMAND} RESULT_VARIABLE ret_var OUTPUT_VARIABLE out_var)
#
# 			if(ret_var EQUAL 0) # grep returns 0 on success
# 				set(SSE_FOUND TRUE)
# 				message("--  ${sse_test} detected and working.")
# 				set(USE_SSE_SET ${sse_test})
# 			endif(ret_var EQUAL 0)
#
# 		endif(NOT SSE_FOUND)
# 	endforeach(sse_test)
# endif(NOT USE_SSE_FLAG)
#
# # set the SSE
# if(USE_SSE_SET)
# 	set(CMAKE_C_FLAGS ${CMAKE_C_FLAGS} "-m${USE_SSE_SET}")
# endif(USE_SSE_SET)

# setup some theme specific variables
set(THEME hicolor)
set(THEME_DIRECTORY ${CMAKE_INSTALL_PREFIX}/share/icons/${THEME})

# we need some specific functions:
IF(CMAKE_SYSTEM MATCHES "SunOS.*")
add_definitions("-D_XOPEN_SOURCE=600")
elseif(CMAKE_SYSTEM_NAME MATCHES "^(DragonFly|FreeBSD|NetBSD|OpenBSD)$")
else(CMAKE_SYSTEM MATCHES "SunOS.*")
add_definitions("-D_XOPEN_SOURCE=700")
endif(CMAKE_SYSTEM MATCHES "SunOS.*")

# we need an xslt interpreter to generate preferences_gen.h and darktablerc
find_program(Xsltproc_BIN xsltproc)
if(${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")
	message(STATUS "Missing xsltproc")
	find_program(Saxon_BIN saxon-xslt)
	if(${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
		message(STATUS "Missing saxon-xslt")
		message(FATAL_ERROR "No xslt interpreter found")
	else(${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
		message(STATUS "Found saxon-xslt")
	endif(${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
else(${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")
	message(STATUS "Found xsltproc")
endif(${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")

# do we have xmllint?
if(USE_XMLLINT)
	find_program(Xmllint_BIN xmllint)
	if(${Xmllint_BIN} STREQUAL "Xmllint_BIN-NOTFOUND")
		message(STATUS "Could NOT find xmllint")
		set(USE_XMLLINT OFF)
	endif(${Xmllint_BIN} STREQUAL "Xmllint_BIN-NOTFOUND")
endif(USE_XMLLINT)

# lets continue into build directories
add_subdirectory(src)
add_subdirectory(data)
add_subdirectory(doc)

include(cmake/darktable-packaging.cmake)

# uninstall target
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    IMMEDIATE @ONLY)

add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)

