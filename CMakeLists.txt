cmake_minimum_required(VERSION 3.1)

cmake_policy(SET CMP0011 NEW)
cmake_policy(SET CMP0025 NEW)
cmake_policy(SET CMP0048 OLD) # don't touch PROJECT_VERSION/VERSION

#
# Avoid source tree pollution
#
set(CMAKE_DISABLE_SOURCE_CHANGES ON)
set(CMAKE_DISABLE_IN_SOURCE_BUILD ON)

If(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are not permitted. Make a separate folder for building:\nmkdir build; cd build; cmake ..\nBefore that, remove the files already created:\nrm -rf CMakeCache.txt CMakeFiles")
endif(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/modules/")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/")

project(darktable CXX C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
option(USE_MAP "Build Map View parts" ON)
option(USE_LUA "Build lua scripting support" ON)
option(DONT_USE_INTERNAL_LUA "Never fall back to the intree copy of lua" ON)
option(USE_FLICKR "Enable Flickr support" ON)
option(USE_KWALLET "Build kwallet password storage back-end" ON)
option(USE_LIBSECRET "Build libsecret password storage back-end" ON)
option(USE_UNITY "Use libunity to report progress in the launcher" OFF)
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
option(BUILD_RS_IDENTIFY "Build the darktable-rs-identify debug aid" ON)
option(BUILD_SSE2_CODEPATHS "(EXPERIMENTAL OPTION, DO NOT DISABLE) Building SSE2-optimized codepaths" ON)
option(VALIDATE_APPDATA_FILE "Use appstream-util (if found) to validate the .appdata file" OFF)
option(BUILD_TESTS "Build tests in src/tests/, runnable from the build/ directory" OFF)
option(BUILD_BATTERY_INDICATOR "Add an icon to the top toolbar showing the state of a laptop battery" OFF)
option(BUILD_MSYS2_INSTALL "Build an MSYS2 version of the install, aka for Windows platform, but without dependency installs" OFF)
option(BUILD_NOISE_TOOLS "Build tools for generating noise proifles" OFF)

if(USE_OPENCL)
  option(TESTBUILD_OPENCL_PROGRAMS "Test-compile opencl programs (needs llvm and clang 3.9+)" ON)
else()
  set(TESTBUILD_OPENCL_PROGRAMS OFF)
endif()

if(BUILD_SSE2_CODEPATHS)
  CHECK_C_COMPILER_FLAG("-msse2" _MSSE2)
  if(NOT _MSSE2)
    MESSAGE(WARNING "Building of SSE2-optimized codepaths is enabled, but the compiler does not understand -msse2.")
    set(BUILD_SSE2_CODEPATHS OFF)
  endif()
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

set(CMAKE_REQUIRED_INCLUDES_OLD "${CMAKE_REQUIRED_INCLUDES}")
list(APPEND CMAKE_REQUIRED_INCLUDES "${CMAKE_SOURCE_DIR}")

check_c_source_compiles("
#include <stdio.h>
int main() {
#include \"src/is_supported_platform.h\"
}" IS_SUPPORTED_PLATFORM)
if(NOT IS_SUPPORTED_PLATFORM)
  MESSAGE(FATAL_ERROR "The target platform is not supported!")
endif(NOT IS_SUPPORTED_PLATFORM)
MESSAGE(STATUS "Is the target platform supported: ${IS_SUPPORTED_PLATFORM}")

set(CMAKE_REQUIRED_INCLUDES "${CMAKE_REQUIRED_INCLUDES_OLD}")
unset(CMAKE_REQUIRED_INCLUDES_OLD)

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

include(compiler-versions)

if(WIN32)
  message("-- Win32 build detected, setting default features")
  set(USE_COLORD OFF)
  set(USE_KWALLET OFF)
  set(BUILD_CMSTEST OFF)
  set(BUILD_PRINT OFF)
  set(TESTBUILD_OPENCL_PROGRAMS OFF)
  if(BUILD_MSYS2_INSTALL)
    add_definitions(-DMSYS2_INSTALL)
  endif()
endif(WIN32)


#
# Set package version
#
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/src/") # the src/ subdirectory won't exist yet

# adds custom command to generate header containing version info.
# takes 1 optional parameter - version override.
function(generate_version_gen_c)
  if(ARGC EQUAL 2)
    # if a version override was specified, use it
    set(_VERSION "${ARGV0}")
    set(_TYPE "${ARGV1}")
  else()
    # else, the tool will autodetect the version
    set(_VERSION "")
    set(_TYPE "git checkout")
  endif()

  add_custom_target(
    create_version_gen ALL
    COMMAND ${CMAKE_SOURCE_DIR}/tools/create_version_c.sh ${CMAKE_BINARY_DIR}/src/version_gen.c ${_VERSION}
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/create_version_c.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Updating version string (${_TYPE})"
    VERBATIM # else might break when export-subst was needed but did not happen
  )
endfunction(generate_version_gen_c)

if(DEFINED PROJECT_VERSION)
  #project version is defined by -D on the cmake command line
  # only use that value, do not update it at make time
  generate_version_gen_c(${PROJECT_VERSION} "version override")
else(DEFINED PROJECT_VERSION)
  if(NOT SOURCE_PACKAGE) # i.e., a git checkout
    # this part is setting the corresponding CMake variable which gets used for example when creating a source package
    execute_process(
      COMMAND sh ${CMAKE_SOURCE_DIR}/tools/get_git_version_string.sh OUTPUT_STRIP_TRAILING_WHITESPACE
      OUTPUT_VARIABLE PROJECT_VERSION
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    # FIXME: PROJECT_VERSION will not be updated automatically, until you rerun cmake
    generate_version_gen_c()
  else(NOT SOURCE_PACKAGE)
    if(NOT EXISTS ${CMAKE_SOURCE_DIR}/src/version_gen.c)
      # should be expanded by git archive due to export-subst in .gitattributes
      set(PROJECT_VERSION "archive-$Format:%H$")
      # but was it expanded?
      if(PROJECT_VERSION MATCHES Format)
        set(PROJECT_VERSION "unknown-version")
      endif(PROJECT_VERSION MATCHES Format)
      generate_version_gen_c(${PROJECT_VERSION} "source package")
    else(NOT EXISTS ${CMAKE_SOURCE_DIR}/src/version_gen.c)
      # no need to create version_gen.c if it's already shipped. that is for example the case with our release tarballs
      execute_process(
        COMMAND sh ${CMAKE_SOURCE_DIR}/tools/parse_version_c.sh ${CMAKE_SOURCE_DIR}/src/version_gen.c OUTPUT_STRIP_TRAILING_WHITESPACE
        OUTPUT_VARIABLE PROJECT_VERSION
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      )
      # FIXME: (irrelevant) PROJECT_VERSION will not be updated automatically, until you rerun cmake
      # but generate_version target expects it to be in build dir, so we need to copy it
      add_custom_target(
        create_version_gen ALL
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/src/version_gen.c ${CMAKE_BINARY_DIR}/src/version_gen.c
        DEPENDS ${CMAKE_SOURCE_DIR}/src/version_gen.c
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Updating version string (source package) - ${PROJECT_VERSION}"
      )
    endif(NOT EXISTS ${CMAKE_SOURCE_DIR}/src/version_gen.c)
  endif(NOT SOURCE_PACKAGE)
endif(DEFINED PROJECT_VERSION)

# needed to make sure that version string is actually updated.
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/src/version_gen.c
  COMMAND ${CMAKE_COMMAND} -E echo
  DEPENDS create_version_gen
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# WARNING: no target should reference version_gen.c directly. instead, they should add_dependencies(yourtarget generate_version)
add_custom_target(
  generate_version ALL
  DEPENDS ${CMAKE_BINARY_DIR}/src/version_gen.c
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# Add a sensible build type default and warning because empty means no optimization and no debug info.
if(NOT CMAKE_BUILD_TYPE)
	message("WARNING: CMAKE_BUILD_TYPE is not defined!\n         Defaulting to CMAKE_BUILD_TYPE=RelWithDebInfo. Use ccmake to set a proper value.")
	SET(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel." FORCE)
endif(NOT CMAKE_BUILD_TYPE)

if(CMAKE_BUILD_TYPE MATCHES "^[Dd][Ee][Bb][Uu][Gg]$" AND SOURCE_PACKAGE)
  message(FATAL_ERROR "ERROR: Debug build type most likely isn't what you want, use RelWithDebInfo instead. If you're absolutely sure that this is what you want then just comment out this line.")
endif()

include(compiler-warnings)
include(windows-macros)

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

# we need appstream-util to check darktable.appdata.xml
if(VALIDATE_APPDATA_FILE)
  find_program(appstream_util_BIN appstream-util)
  if(${appstream_util_BIN} STREQUAL "appstream_util_BIN-NOTFOUND")
    message(STATUS "Missing appstream-util, problems in darktable.appdata.xml might go unnoticed")
    set(VALIDATE_APPDATA_FILE OFF)
  else(${appstream_util_BIN} STREQUAL "appstream_util_BIN-NOTFOUND")
    message(STATUS "Found appstream-util")
  endif(${appstream_util_BIN} STREQUAL "appstream_util_BIN-NOTFOUND")
endif(VALIDATE_APPDATA_FILE)

if(TESTBUILD_OPENCL_PROGRAMS)
  set(TESTBUILD_OPENCL_PROGRAMS OFF)

  macro(find_llvm versions)
    foreach(version ${versions})
      find_package(LLVM ${version} CONFIG)
      if(LLVM_FOUND)
        break()
      endif()
    endforeach()
  endmacro()

  # 3.9 is the first version with which this works.
  find_llvm("7;6.0;5.0;4.0;3.9")

  if (LLVM_FOUND)
    message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")

    find_program(CLANG_OPENCL_COMPILER
      NAMES clang-${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR} clang-${LLVM_PACKAGE_VERSION} clang${LLVM_VERSION_MAJOR}${LLVM_VERSION_MINOR} clang-${LLVM_VERSION_MAJOR} clang${LLVM_VERSION_MAJOR}
    )

    if (NOT ${CLANG_OPENCL_COMPILER} STREQUAL "CLANG_OPENCL_COMPILER-NOTFOUND")
      message(STATUS "Found clang compiler - ${CLANG_OPENCL_COMPILER}")

      find_path(CLANG_OPENCL_INCLUDE_DIR opencl-c.h
        HINTS ${LLVM_INSTALL_PREFIX}/lib/clang ${LLVM_INSTALL_PREFIX}/lib64/clang
        PATH_SUFFIXES include ${LLVM_PACKAGE_VERSION}/include
        NO_DEFAULT_PATH
      )

      if (NOT ${CLANG_OPENCL_INCLUDE_DIR} STREQUAL "CLANG_OPENCL_INCLUDE_DIR-NOTFOUND")
        message(STATUS "Found clang opencl-c.h header in ${CLANG_OPENCL_INCLUDE_DIR}")
        set(TESTBUILD_OPENCL_PROGRAMS ON)
      else()
        message(WARNING "Could not find clang opencl-c.h header include dir")
        message(WARNING "Test-compilation of OpenCL programs can not be done.")
      endif()
    else()
      message(WARNING "Could not find appropriate clang compiler")
      message(WARNING "Test-compilation of OpenCL programs can not be done.")
    endif()
  else()
    message(WARNING "Could not find LLVM 3.9+")
    message(WARNING "Test-compilation of OpenCL programs can not be done.")
  endif()
endif()

if(USE_OPENCL AND TESTBUILD_OPENCL_PROGRAMS)
  message(STATUS "Will be able to test-compile OpenCL programs. Nice.")
elseif(USE_OPENCL)
  message(STATUS "Test-compilation of OpenCL programs is disabled.")
endif()

# we need jsonschema to check noiseprofiles.json
find_program(jsonschema_BIN jsonschema)
if(${jsonschema_BIN} STREQUAL "jsonschema_BIN-NOTFOUND")
  message(STATUS "Missing jsonschema, problems in noiseprofiles.json might go unnoticed")
  set(VALIDATE_JSON 0)
else(${jsonschema_BIN} STREQUAL "jsonschema_BIN-NOTFOUND")
  message(STATUS "Found jsonschema")
  set(VALIDATE_JSON 1)
endif(${jsonschema_BIN} STREQUAL "jsonschema_BIN-NOTFOUND")

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
    if(GETTEXT_MSGFMT_EXECUTABLE)
        message(STATUS "Found msgfmt to convert .po file. Translation enabled")
        add_subdirectory(po)
    else()
        message(STATUS "Cannot find msgfmt to convert .po file. Translation won't be enabled")
    endif()
endif(USE_NLS)

# needed to generate file "preferences_gen.h" accordingly
if(USE_OPENCL)
	SET(HAVE_OPENCL 1)
else()
	SET(HAVE_OPENCL 0)
endif(USE_OPENCL)

if(NOT SOURCE_PACKAGE AND NOT (CMAKE_VERSION VERSION_LESS 3.3) AND DEFINED ENV{_DO_IWYU})
  find_program(iwyu_path NAMES include-what-you-use iwyu)

  if(iwyu_path)
    set(DT_CMAKE_INCLUDE_WHAT_YOU_USE ${iwyu_path} -Xiwyu --mapping_file=${CMAKE_SOURCE_DIR}/iwyu.imp -Xiwyu --prefix_header_includes=add)
  endif()

  find_program(iwyu_tool_path NAMES iwyu_tool.py)
  if (iwyu_tool_path)
    add_custom_command(
      OUTPUT "${CMAKE_BINARY_DIR}/iwyu.log"
      COMMAND "${iwyu_tool_path}" -v -p "${CMAKE_BINARY_DIR}"
              -- --mapping_file=${CMAKE_SOURCE_DIR}/iwyu.imp
              --prefix_header_includes=add 2>
              "${CMAKE_BINARY_DIR}/iwyu.log"
      WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
      COMMENT "Running include-what-you-use tool"
      VERBATIM
    )
    add_custom_target(iwyu
      DEPENDS "${CMAKE_BINARY_DIR}/iwyu.log"
      VERBATIM
    )
  endif()

  find_program(fix_includes_path NAMES fix_includes.py)
  if (fix_includes_path)
    add_custom_target(iwyu_fix
      COMMAND "${fix_includes_path}" --noblank_lines --comments
              --nosafe_headers < "${CMAKE_BINARY_DIR}/iwyu.log" || true
      COMMAND ${CMAKE_COMMAND} -E remove "${CMAKE_BINARY_DIR}/iwyu.log"
      DEPENDS "${CMAKE_BINARY_DIR}/iwyu.log"
      WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
      COMMENT "Running include-what-you-use fix_includes tool"
      VERBATIM
    )
  endif()
endif()

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

# Set default component name - that way external modules like RawSpeed will install their
# materials under the default component and not under 'Unspecified'
set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME DTApplication)

# lets continue into build directories
include(data/supported_extensions.cmake) # this file needs to be included first as it gets amended in src/
add_subdirectory(src) # src/ needs to be before data/ so that the correct CSS file gets installed
add_subdirectory(data)
add_subdirectory(doc)
add_subdirectory(tools)

# This contains fixup_bundle
# And adding a separate subderectory as a last one will make sure
# that fixup_bundle will run _after_ all files has been installed
add_subdirectory(packaging)

include(cmake/darktable-packaging.cmake)

# uninstall target
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    IMMEDIATE @ONLY)

add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)
