project(darktable CXX C)
cmake_minimum_required(VERSION 2.6)
cmake_policy(VERSION 2.6)

option(USE_CAMERA_SUPPORT "Detect and use camera support if available." ON)
option(USE_NLS "Build Native Language Support (using gettext)" ON)
option(USE_COLORD "Enable colord support" ON)
option(USE_GEO "Build geotagging parts" ON)
option(USE_LUA "Build lua scripting support" ON)
option(DONT_USE_INTERNAL_LUA "Never fall back to the intree copy of lua" ON)
option(USE_FLICKR "Enable Flickr support" ON)
option(USE_GLIBJSON "Enable GlibJson support" ON)
option(USE_KWALLET "Build kwallet password storage back-end" ON)
option(USE_LIBSECRET "Build libsecret password storage back-end" ON)
option(USE_GNOME_KEYRING "Build gnome-keyring password storage back-end" ON)
option(USE_UNITY "Use libunity to report progress in the launcher" OFF)
option(BUILD_SLIDESHOW "Build the opengl slideshow viewer" ON)
option(USE_OPENMP "Use openmp threading support." ON)
option(USE_OPENCL "Use OpenCL support." ON)
option(USE_GRAPHICSMAGICK "Use GraphicsMagick library for image import." ON)
option(USE_DARKTABLE_PROFILING OFF)
option(CUSTOM_CFLAGS "Don't override compiler optimization flags." OFF)
option(BUILD_USERMANUAL "Build all the versions of the usermanual." OFF)
option(BINARY_PACKAGE_BUILD "Sets march optimization to generic" OFF)
option(APRIL_FOOLS "Add the \"file manager\" (a shell) to lighttable mode" OFF)
option(USE_XMLLINT "Run xmllint to test if darktableconfig.xml is valid" ON)
option(USE_OPENJPEG "Enable JPEG 2000 support" ON)
option(USE_WEBP "Enable WebP export support" ON)
option(BUILD_CMSTEST "Build a test program to check your system's color management setup" ON)
option(USE_OPENEXR "Enable OpenEXR support" ON)
if(APPLE)
	option(USE_MAC_INTEGRATION "Enable OS X integration" ON)
else(APPLE)
	set(USE_MAC_INTEGRATION OFF)
endif(APPLE)

# When cross compiling, CMAKE_INSTALL_PREFIX will point to something like "/opt/darktable", but that's not useful when using the path to load
# modules on runtime. Then we need something like "C:\Program Files\darktable". Doesn't need to be set when doing regular compiles.
if(NOT DEFINED RUNTIME_INSTALL_PREFIX)
  set(RUNTIME_INSTALL_PREFIX ${CMAKE_INSTALL_PREFIX})
endif(NOT DEFINED RUNTIME_INSTALL_PREFIX)

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
	message("-- Mac OS X build detected, setting default features")
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
  # this part is setting the corresponding CMake variable which gets used for example when creating a source package
  EXECUTE_PROCESS(COMMAND git describe --tags HEAD #OUTPUT_VARIABLE PROJECT_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE
      COMMAND sed "s,^release-,,;s,-,+,;s,-,~,;" OUTPUT_STRIP_TRAILING_WHITESPACE OUTPUT_VARIABLE PROJECT_VERSION
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

  # and here we write out the version into a .h file suitable to be used in the code.
  # the file only gets written when the version is actually different to avoid unneeded recompiles
  add_custom_target(
    generate_version ALL
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/create_version_h.sh ${CMAKE_CURRENT_BINARY_DIR}/src/version_gen.h
    COMMENT "Updating version string"
  )
endif(NOT SOURCE_PACKAGE)

#
# Initial cmake/debian/postinst and prerm script for debian package
#
file(MAKE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian)


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

if(NOT SHARE_INSTALL)
  set(SHARE_INSTALL "share" CACHE STRING "Data file install path. Must be a relative path (from CMAKE_INSTALL_PREFIX), with no trailing slash.")
endif(NOT SHARE_INSTALL)

mark_as_advanced(SHARE_INSTALL)

if(NOT LIB_INSTALL)
  set( LIB_INSTALL lib${LIB_SUFFIX} CACHE STRING "Library file install path. Must be a relative path (from CMAKE_INSTALL_PREFIX), with no trailing slash.")
endif(NOT LIB_INSTALL)

mark_as_advanced(LIB_INSTALL)


# we need some external programs for building darktable
message(STATUS "Looking for external programs")
set(EXTERNAL_PROGRAMS_FOUND 1)

# we need perl for introspection
find_program(perl_BIN perl)
if(${perl_BIN} STREQUAL "perl_BIN-NOTFOUND")
  message(STATUS "Missing perl")
  set(EXTERNAL_PROGRAMS_FOUND 0)
else(${perl_BIN} STREQUAL "perl_BIN-NOTFOUND")
  message(STATUS "Found perl")
endif(${perl_BIN} STREQUAL "perl_BIN-NOTFOUND")

# we need intltool-merge for darktable.desktop
find_program(intltool_merge_BIN intltool-merge)
if(${intltool_merge_BIN} STREQUAL "intltool_merge_BIN-NOTFOUND")
  message(STATUS "Missing intltool-merge")
  set(EXTERNAL_PROGRAMS_FOUND 0)
else(${intltool_merge_BIN} STREQUAL "intltool_merge_BIN-NOTFOUND")
  message(STATUS "Found intltool-merge")
endif(${intltool_merge_BIN} STREQUAL "intltool_merge_BIN-NOTFOUND")

# we need an xslt interpreter to generate preferences_gen.h and darktablerc
find_program(Xsltproc_BIN xsltproc)
if(${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")
  message(STATUS "Missing xsltproc")
  find_program(Saxon_BIN saxon-xslt)
  if(${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
    message(STATUS "Missing saxon-xslt")
    message(STATUS "No xslt interpreter found")
    set(EXTERNAL_PROGRAMS_FOUND 0)
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
    message(STATUS "Missing xmllint")
    set(USE_XMLLINT OFF)
  else(${Xmllint_BIN} STREQUAL "Xmllint_BIN-NOTFOUND")
    message(STATUS "Found xmllint")
  endif(${Xmllint_BIN} STREQUAL "Xmllint_BIN-NOTFOUND")
endif(USE_XMLLINT)

# done with looking for programs
if(NOT EXTERNAL_PROGRAMS_FOUND)
  message(FATAL_ERROR "Some external programs couldn't be found")
else(NOT EXTERNAL_PROGRAMS_FOUND)
  message(STATUS "All external programs found")
endif(NOT EXTERNAL_PROGRAMS_FOUND)


#need to put here do setting LOCALE_DIR variable
if(USE_NLS)
find_package(Gettext)
if(Gettext_FOUND)
	if(NOT LOCALE_DIR)
    SET(LOCALE_DIR "${CMAKE_INSTALL_PREFIX}/share/locale")
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

if(NOT DEFINED RUNTIME_LOCALE_DIR)
  set(RUNTIME_LOCALE_DIR ${LOCALE_DIR})
endif(NOT DEFINED RUNTIME_LOCALE_DIR)

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
if(NOT WIN32)
  IF(CMAKE_SYSTEM MATCHES "SunOS.*")
  add_definitions("-D_XOPEN_SOURCE=600")
  elseif(CMAKE_SYSTEM_NAME MATCHES "^(DragonFly|FreeBSD|NetBSD|OpenBSD)$")
  else(CMAKE_SYSTEM MATCHES "SunOS.*")
  add_definitions("-D_XOPEN_SOURCE=700")
  endif(CMAKE_SYSTEM MATCHES "SunOS.*")
endif(NOT WIN32)

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

