cmake_minimum_required(VERSION 2.8.5)

project(darktable CXX C)

# Allow forcing the C/CPP compiler that is actually used during the compilation
# to something other than what is used by the cmake run. This is useful when
# the compiler for some reason breaks the initial cmake checks but works fine
# for actually compiling darktable. This allows building darktable using
# afl-clang-fast achieving a >4x speedup in fuzzing.
IF(DEFINED DT_FORCE_C_COMPILER)
  set(CMAKE_C_COMPILER ${DT_FORCE_C_COMPILER})
endif()
IF(DEFINED DT_FORCE_CXX_COMPILER)
  set(CMAKE_CXX_COMPILER ${DT_FORCE_CXX_COMPILER})
endif()

include(CheckCCompilerFlag)
include(TestBigEndian)

# Check if this is source package build
if(NOT IS_DIRECTORY ${CMAKE_SOURCE_DIR}/.git)
  set(SOURCE_PACKAGE 1)
else()
  set(SOURCE_PACKAGE 0)
endif()

# Include GNUInstallDirs, which sets sensible defaults for install directories.
# See https://cmake.org/cmake/help/v3.0/module/GNUInstallDirs.html for further information.
# These values can be easily overridden if required.
# Some defaults are set for OpenBSD as well (info and man pages).

include(GNUInstallDirs)

option(USE_CAMERA_SUPPORT "Detect and use camera support if available." ON)
option(USE_NLS "Build Native Language Support (using gettext)" ON)
option(USE_COLORD "Enable colord support" ON)
option(USE_GEO "Build geotagging parts" ON)
option(USE_LUA "Build lua scripting support" ON)
option(DONT_USE_INTERNAL_LUA "Never fall back to the intree copy of lua" ON)
option(USE_FLICKR "Enable Flickr support" ON)
option(USE_KWALLET "Build kwallet password storage back-end" ON)
option(USE_LIBSECRET "Build libsecret password storage back-end" ON)
option(USE_UNITY "Use libunity to report progress in the launcher" OFF)
option(BUILD_SLIDESHOW "Build the opengl slideshow viewer" ON)
option(USE_OPENMP "Use openmp threading support." ON)
option(USE_OPENCL "Use OpenCL support." ON)
option(USE_GRAPHICSMAGICK "Use GraphicsMagick library for image import." ON)
option(USE_DARKTABLE_PROFILING OFF)
option(CUSTOM_CFLAGS "Don't override compiler optimization flags." OFF)
option(BUILD_USERMANUAL "Build all the versions of the usermanual." OFF)
option(BINARY_PACKAGE_BUILD "Sets march optimization to generic" OFF)
option(USE_XMLLINT "Run xmllint to test if darktableconfig.xml is valid" ON)
option(USE_OPENJPEG "Enable JPEG 2000 support" ON)
option(USE_WEBP "Enable WebP export support" ON)
option(BUILD_CMSTEST "Build a test program to check your system's color management setup" ON)
option(USE_OPENEXR "Enable OpenEXR support" ON)
option(BUILD_PRINT "Build the print module" ON)
option(BUILD_RS_IDENTIFY "Build the darktable-rs-identify debug aid" OFF)

if(NOT SOURCE_PACKAGE)
  OPTION(BUILD_SSE2_CODEPATHS "(EXPERIMENTAL OPTION, DO NOT DISABLE) Building SSE2-optimized codepaths" ON)

  if(BUILD_SSE2_CODEPATHS)
    CHECK_C_COMPILER_FLAG("-msse2" _MSSE2)
    if(NOT _MSSE2)
      MESSAGE(WARNING "Building of SSE2-optimized codepaths is enabled, but the compiler does not understand -msse2.")
      set(BUILD_SSE2_CODEPATHS OFF)
    endif()
  endif()
else()
  set(BUILD_SSE2_CODEPATHS ON)
endif()

MESSAGE(STATUS "Building SSE2-optimized codepaths: ${BUILD_SSE2_CODEPATHS}")

test_big_endian(BIGENDIAN)
if(${BIGENDIAN})
	# we do not really want those.
	# besides, no one probably tried darktable on such systems
	MESSAGE(FATAL_ERROR "Found big endian system. Bad.")
else()
	MESSAGE(STATUS "Found little endian system. Good.")
endif(${BIGENDIAN})

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


#
# Set package version
#
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/src/") # the src/ subdirectory won't exist yet

# adds custom command to generate header containing version info.
# takes 1 optional parameter - version override.
function(generate_version_gen_h)
  if(ARGC EQUAL 1)
    # if a version override was specified, use it
    set(_VERSION "${ARGV0}")
  else()
    # else, the tool will autodetect the version
    set(_VERSION "")
  endif()

  add_custom_target(
    create_version_gen ALL
    COMMAND ${CMAKE_SOURCE_DIR}/tools/create_version_h.sh ${CMAKE_CURRENT_BINARY_DIR}/src/version_gen.h ${_VERSION}
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/create_version_h.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Updating version string"
    VERBATIM # else might break when export-subst was needed but did not happen
  )
endfunction(generate_version_gen_h)

if(DEFINED PROJECT_VERSION)
  #project version is defined by -D on the cmake command line
  # only use that value, do not update it at make time
  generate_version_gen_h(${PROJECT_VERSION})
else(DEFINED PROJECT_VERSION)
  if(NOT SOURCE_PACKAGE) # i.e., a git checkout
    # this part is setting the corresponding CMake variable which gets used for example when creating a source package
    execute_process(
      COMMAND ${CMAKE_SOURCE_DIR}/tools/get_git_version_string.sh OUTPUT_STRIP_TRAILING_WHITESPACE
      OUTPUT_VARIABLE PROJECT_VERSION
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    generate_version_gen_h()
  else(NOT SOURCE_PACKAGE)
    if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/version_gen.h)
      # should be expanded by git archive due to export-subst in .gitattributes
      set(PROJECT_VERSION "archive-$Format:%H$")
      # but was it expanded?
      if(PROJECT_VERSION MATCHES Format)
        set(PROJECT_VERSION "unknown-version")
      endif(PROJECT_VERSION MATCHES Format)
      generate_version_gen_h(${PROJECT_VERSION})
    else(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/version_gen.h)
      # no need to create version_gen.h if it's already shipped. that is for example the case with our release tarballs
      # but generate_version target expects it to be in build dir, so we need to copy it
      add_custom_target(
        create_version_gen ALL
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/src/version_gen.h ${CMAKE_CURRENT_BINARY_DIR}/src/version_gen.h
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/version_gen.h
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Updating version string"
      )
    endif(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/version_gen.h)
  endif(NOT SOURCE_PACKAGE)
endif(DEFINED PROJECT_VERSION)

# needed to make sure that version string is actually updated.
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/src/version_gen.h
  COMMAND ${CMAKE_COMMAND} -E echo
  DEPENDS create_version_gen
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# WARNING: no target should reference version_gen.h directly. instead, they should add_dependencies(yourtarget generate_version)
add_custom_target(
  generate_version ALL
  DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/src/version_gen.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

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

if(CMAKE_BUILD_TYPE MATCHES "^[Dd][Ee][Bb][Uu][Gg]$" AND SOURCE_PACKAGE)
  message(FATAL_ERROR "ERROR: Debug build type most likely isn't what you want, use RelWithDebInfo instead. If you're absolutely sure that this is what you want then just comment out this line.")
endif()

# Setting the runtime path works differently on OSX than on Linux or BSD.
# Hence use a variable that maps those in the correct way for each OS.
if(APPLE)
    # The string "@loader_path" should end up in the executable as-is.
    set(RPATH_DT "@loader_path")
else(APPLE)
    # Note that $ORIGIN is not a variable but has a special meaning at runtime.
    # The string "$ORIGIN" should end up in the executable as-is.
    set(RPATH_DT "$ORIGIN")
endif(APPLE)

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

# we need desktop-file-validate to check darktable.desktop
find_program(desktop_file_validate_BIN desktop-file-validate)
if(${desktop_file_validate_BIN} STREQUAL "desktop_file_validate_BIN-NOTFOUND")
  message(STATUS "Missing desktop-file-validate, problems in darktable.desktop might go unnoticed")
  set(VALIDATE_DESKTOP_FILE 0)
else(${desktop_file_validate_BIN} STREQUAL "desktop_file_validate_BIN-NOTFOUND")
  message(STATUS "Found desktop-file-validate")
  set(VALIDATE_DESKTOP_FILE 1)
endif(${desktop_file_validate_BIN} STREQUAL "desktop_file_validate_BIN-NOTFOUND")

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

# The path can be modified by setting CMAKE_INSTALL_LOCALEDIR
if(USE_NLS)
    find_package(Gettext)
    if(Gettext_FOUND)
        find_package(Msgfmt)
        if(Msgfmt_FOUND)
            message(STATUS "Found msgfmt to convert language file. Translation enabled")
            add_subdirectory(po)
        else()
            message(STATUS "Cannot find msgfmt to convert language file. Translation won't be enabled")
        endif()
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
set(THEME_DIRECTORY "${CMAKE_INSTALL_DATAROOTDIR}/icons/${THEME}")

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
