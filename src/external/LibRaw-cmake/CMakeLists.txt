# ===========================================================
#
# This file is a part of Libraw project
# <a href="http://www.libraw.org">http://www.libraw.org</a>
#
# @date   2013-09-07
# @brief  Library for reading and processing of RAW images
#
# @author Copyright (C) 2013 by Gilles Caulier
#         <a href="mailto:caulier dot gilles at gmail dot com">caulier dot gilles at gmail dot com</a>
#
# This program is free software; you can redistribute it
# and/or modify it under the terms of the GNU General
# Public License as published by the Free Software Foundation;
# either version 2, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# ============================================================

cmake_minimum_required(VERSION 3.12..16)


# Determine if libraw is built as a subproject (using add_subdirectory)
# or if it is the master project.
set(MASTER_PROJECT OFF)
if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(MASTER_PROJECT ON)
    message(STATUS "CMake version: ${CMAKE_VERSION}")
endif ()

set(LIBRAW_PATH ${CMAKE_CURRENT_SOURCE_DIR} CACHE STRING "Relative path to libraw directory (default=CMAKE_CURRENT_SOURCE_DIR)")
if(NOT EXISTS "${LIBRAW_PATH}")
    message(STATUS "LIBRAW_PATH=${LIBRAW_PATH}")
    message(FATAL_ERROR "LIBRAW_PATH does not contain a valid path to the libraw home")
endif()

file(TO_CMAKE_PATH "${LIBRAW_PATH}" LIBRAW_PATH)

# ==================================================================================================
# Library version info extraction

file(READ ${LIBRAW_PATH}/libraw/libraw_version.h _libraw_version_content)

# API version strings
string(REGEX MATCH "#define LIBRAW_MAJOR_VERSION[ \t]*([0-9]*)\n" _version_major_match ${_libraw_version_content})
set(RAW_LIB_MAJOR_VERSION "${CMAKE_MATCH_1}")

string(REGEX MATCH "#define LIBRAW_MINOR_VERSION[ \t]*([0-9]*)\n" _version_minor_match ${_libraw_version_content})
set(RAW_LIB_MINOR_VERSION "${CMAKE_MATCH_1}")

string(REGEX MATCH "#define LIBRAW_PATCH_VERSION[ \t]*([0-9]*)\n" _version_patch_match ${_libraw_version_content})
set(RAW_LIB_PATCH_VERSION "${CMAKE_MATCH_1}")

# ABI version strings

string(REGEX MATCH "#define LIBRAW_SHLIB_CURRENT[ \t]*([0-9]*)\n" _version_socur_match ${_libraw_version_content})
set(RAW_LIB_SO_CUR_VERSION "${CMAKE_MATCH_1}")

string(REGEX MATCH "#define LIBRAW_SHLIB_REVISION[ \t]*([0-9]*)\n" _version_sorev_match ${_libraw_version_content})
set(RAW_LIB_SO_REV_VERSION "${CMAKE_MATCH_1}")

string(REGEX MATCH "#define LIBRAW_SHLIB_AGE[ \t]*([0-9]*)\n" _version_soage_match ${_libraw_version_content})
set(RAW_LIB_SO_AGE_VERSION "${CMAKE_MATCH_1}")

# Set env. variables accordinly.
set(RAW_LIB_VERSION_STRING    "${RAW_LIB_MAJOR_VERSION}.${RAW_LIB_MINOR_VERSION}.${RAW_LIB_PATCH_VERSION}")
set(RAW_LIB_VERSION_ID        "0x${RAW_LIB_MAJOR_VERSION}${RAW_LIB_MINOR_VERSION}${RAW_LIB_PATCH_VERSION}")
set(RAW_LIB_SO_VERSION_STRING "${RAW_LIB_SO_CUR_VERSION}.${RAW_LIB_SO_REV_VERSION}.${RAW_LIB_SO_AGE_VERSION}")

message(STATUS "LibRaw string version: ${RAW_LIB_VERSION_STRING}")
message(STATUS "LibRaw ID version:     ${RAW_LIB_VERSION_ID}")
message(STATUS "LibRaw SO version:     ${RAW_LIB_SO_VERSION_STRING}")

project(libraw VERSION ${RAW_LIB_VERSION_STRING} LANGUAGES CXX)

# ==================================================================================================
# Project Options
option(BUILD_SHARED_LIBS           "Build library as shared library                 (default=ON)"                 OFF)
option(ENABLE_OPENMP               "Build library with OpenMP support               (default=ON)"                 ON)
option(ENABLE_LCMS                 "Build library with LCMS support                 (default=ON)"                 ON)
option(ENABLE_JASPER               "Build library with libjasper support            (default=ON)"                 ON)
option(ENABLE_EXAMPLES             "Build library with sample command-line programs (default=ON)"    ${MASTER_PROJECT})
option(ENABLE_RAWSPEED             "Build library with extra RawSpeed codec support (default=OFF)"                OFF)
option(ENABLE_DCRAW_DEBUG          "Build library with debug message from dcraw     (default=OFF)"                OFF)
option(ENABLE_X3FTOOLS             "Build library with Foveon X3F support           (default=OFF)"                OFF)
option(ENABLE_6BY9RPI              "Build library with Raspberry Pi RAW support     (default=OFF)"                OFF)
option(LIBRAW_UNINSTALL_TARGET     "Add a custom target to ease removal of installed targets"        ${MASTER_PROJECT})
option(LIBRAW_INSTALL              "Generate the install target."                                    ${MASTER_PROJECT})

set(RAWSPEED_RPATH           "RawSpeed"           CACHE STRING
    "Relative path to extra RawSpeed codec     (default=RawSpeed)")

set(RAWSPEED_PATH           "${CMAKE_CURRENT_SOURCE_DIR}/${RAWSPEED_RPATH}")

# ==================================================================================================
# General definitions rules

if(WIN32 AND NOT DEFINED CMAKE_DEBUG_POSTFIX)
    set(CMAKE_DEBUG_POSTFIX "d")
endif()

# To prevent warnings from M$ compiler
if(MSVC)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    add_definitions(-D_ATL_SECURE_NO_WARNINGS)
    add_definitions(-D_AFX_SECURE_NO_WARNINGS)
endif()

# -- Check dependencies --------------------------------------------------------------------------------

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules ${CMAKE_MODULE_PATH} )

include(MacroBoolTo01)
include(MacroLogFeature)
include(MacroOptionalFindPackage)

# Math library check

if(NOT WIN32)
    FIND_LIBRARY(MATH_LIBRARY m)
endif()

# LCMS version 1 and 2 library check

set(LCMS_SUPPORT_CAN_BE_COMPILED false)
set(LCMS2_FOUND false)
set(LCMS_FOUND false)

if(ENABLE_LCMS)
    message(STATUS "Check for LCMS2 availability...")
    find_package(LCMS2)
    if(LCMS2_FOUND AND (LCMS2_VERSION VERSION_EQUAL 2.1 OR LCMS2_VERSION VERSION_GREATER 2.1))
        message(STATUS "Found LCMS2 : ${LCMS2_LIBRARIES} ${LCMS2_INCLUDE_DIR}")
        include_directories(${LCMS2_INCLUDE_DIR})
        MACRO_LOG_FEATURE(LCMS2_FOUND "LCMS2" "A small-footprint color management engine" "http://www.littlecms.com" FALSE "" "Needed by libkdcraw")
        # Flag to compile Little CMS version 2 with LibRaw
        add_definitions(-DUSE_LCMS2)
        set(LCMS_SUPPORT_CAN_BE_COMPILED true)
    else()
        message(STATUS "Check for LCMS availability instead LCMS2...")
        find_package(LCMS)
        if(LCMS_FOUND)
            message(STATUS "Found LCMS1: ${LCMS_LIBRARIES} ${LCMS_INCLUDE_DIR}")
            include_directories(${LCMS_INCLUDE_DIR})
            MACRO_LOG_FEATURE(LCMS_FOUND "LCMS1" "A small-footprint color management engine" "http://www.littlecms.com" TRUE "" "Needed by libkdcraw")
            # Flag to compile Little CMS version 1 with LibRaw
            add_definitions(-DUSE_LCMS)
            # For compatibility
            set(LCMS2_LIBRARIES ${LCMS_LIBRARIES})
            set(LCMS_SUPPORT_CAN_BE_COMPILED true)
        endif()
    endif()
endif()

# For registration to libraw_config.h
MACRO_BOOL_TO_01(LCMS_SUPPORT_CAN_BE_COMPILED LIBRAW_USE_LCMS)

# zlib library check

find_package(ZLIB)
find_package(JPEG)

# Flag to use zlib with LibRaw DNG deflate codec
if(ZLIB_FOUND)
    add_definitions(-DUSE_ZLIB)
endif()

# For registration to libraw_config.h
MACRO_BOOL_TO_01(ZLIB_FOUND LIBRAW_USE_DNGDEFLATECODEC)

# JPEG library check
find_package(JPEG)
if(JPEG_FOUND)
    if (${JPEG_VERSION} LESS 80)
        set(JPEG8_FOUND FALSE)
    else()
        set(JPEG8_FOUND TRUE)
    endif()
endif()

MACRO_LOG_FEATURE(JPEG8_FOUND "libjpeg" "JPEG image format support" "http://www.ijg.org" FALSE "80" "needed for the LibRaw DNG lossy codec")

# Flag to use libjpeg with LibRaw DNG lossy codec
if(JPEG8_FOUND)
    add_definitions(-DUSE_JPEG)
    add_definitions(-DUSE_JPEG8)
endif()

# For registration to libraw_config.h
MACRO_BOOL_TO_01(JPEG8_FOUND LIBRAW_USE_DNGLOSSYCODEC)

# OpenMP check
set(OPENMP_SUPPORT_CAN_BE_COMPILED false)
if(ENABLE_OPENMP)
    find_package(OpenMP)
    if(OpenMP_FOUND)
        set(OPENMP_SUPPORT_CAN_BE_COMPILED true)
    endif()
endif()

# For registration to libraw_config.h
MACRO_BOOL_TO_01(OPENMP_SUPPORT_CAN_BE_COMPILED LIBRAW_USE_OPENMP)

# Jasper library check
set(JASPER_SUPPORT_CAN_BE_COMPILED false)
if(ENABLE_JASPER)
    find_package(Jasper)

    # Flag to use libjasper with LibRaw RedCine codec
    if(JASPER_FOUND)
        add_definitions(-DUSE_JASPER)
        include_directories(${JASPER_INCLUDE_DIR})
        set(JASPER_SUPPORT_CAN_BE_COMPILED true)
    endif()
endif()

# For registration to libraw_config.h
MACRO_BOOL_TO_01(JASPER_SUPPORT_CAN_BE_COMPILED LIBRAW_USE_REDCINECODEC)

# For RawSpeed Codec Support

set(RAWSPEED_FOUND false)
set(RAWSPEED_SUPPORT_CAN_BE_COMPILED false)

if(ENABLE_RAWSPEED)
    find_package(LibXml2)
    find_package(Threads REQUIRED)

    message(STATUS "RawSpeed codec path: ${RAWSPEED_PATH}")

    if(EXISTS "${RAWSPEED_PATH}/Common.cpp")
        set(RAWSPEED_FOUND true)
    else()
        message(STATUS "RawSpeed source code not found. Please checkout source code from RawStudio project website.")
    endif()

    if(ENABLE_RAWSPEED AND RAWSPEED_FOUND AND JPEG8_FOUND AND LIBXML2_FOUND AND PTHREADS_FOUND)

        set(RAWSPEED_SUPPORT_CAN_BE_COMPILED true)

    else()
        if(NOT JPEG8_FOUND)
            message(STATUS "LibJPEG dependency not resolved. LibRaw cannot be compiled with RawSpeed codec")
        endif()

        if(NOT LIBXML2_FOUND)
            message(STATUS "LibXML2 dependency not resolved. LibRaw cannot be compiled with RawSpeed codec")
        endif()

        if(NOT PTHREADS_FOUND)
            message(STATUS "Pthreads dependency not resolved. LibRaw cannot be compiled with RawSpeed codec")
        endif()

    endif()
endif()

# For registration to libraw_config.h
MACRO_BOOL_TO_01(RAWSPEED_SUPPORT_CAN_BE_COMPILED LIBRAW_USE_RAWSPEED)

# -- Compilation rules for RawSpeed library -------------------------------------------------------------

if(RAWSPEED_SUPPORT_CAN_BE_COMPILED)
    include_directories(${RAWSPEED_PATH})

    include_directories(${LIBXML2_INCLUDE_DIR} ${PTHREADS_INCLUDE_DIR})

    # Flag to include RawSpeed codec with Libraw
    add_definitions(-DUSE_RAWSPEED)

    add_definitions(${LIBXML2_DEFINITIONS} ${PTHREADS_DEFINITIONS})

    set(librawspeed_LIB_SRCS ${RAWSPEED_PATH}/ArwDecoder.cpp
                             ${RAWSPEED_PATH}/BitPumpJPEG.cpp
                             ${RAWSPEED_PATH}/BitPumpMSB.cpp
                             ${RAWSPEED_PATH}/BitPumpMSB32.cpp
                             ${RAWSPEED_PATH}/BitPumpPlain.cpp
                             ${RAWSPEED_PATH}/BlackArea.cpp
                             ${RAWSPEED_PATH}/ByteStream.cpp
                             ${RAWSPEED_PATH}/ByteStreamSwap.cpp
                             ${RAWSPEED_PATH}/Camera.cpp
                             ${RAWSPEED_PATH}/CameraMetaData.cpp
                             ${RAWSPEED_PATH}/CameraMetadataException.cpp
                             ${RAWSPEED_PATH}/CameraSensorInfo.cpp
                             ${RAWSPEED_PATH}/ColorFilterArray.cpp
                             ${RAWSPEED_PATH}/Common.cpp
                             ${RAWSPEED_PATH}/Cr2Decoder.cpp
                             ${RAWSPEED_PATH}/DngDecoder.cpp
                             ${RAWSPEED_PATH}/DngDecoderSlices.cpp
                             ${RAWSPEED_PATH}/DngOpcodes.cpp
                             ${RAWSPEED_PATH}/FileIOException.cpp
                             ${RAWSPEED_PATH}/FileMap.cpp
                             ${RAWSPEED_PATH}/IOException.cpp
                             ${RAWSPEED_PATH}/LJpegDecompressor.cpp
                             ${RAWSPEED_PATH}/LJpegPlain.cpp
                             ${RAWSPEED_PATH}/NefDecoder.cpp
                             ${RAWSPEED_PATH}/NikonDecompressor.cpp
                             ${RAWSPEED_PATH}/OrfDecoder.cpp
                             ${RAWSPEED_PATH}/PefDecoder.cpp
                             ${RAWSPEED_PATH}/PentaxDecompressor.cpp
                             ${RAWSPEED_PATH}/RawDecoder.cpp
                             ${RAWSPEED_PATH}/RawDecoderException.cpp
                             ${RAWSPEED_PATH}/RawImage.cpp
                             ${RAWSPEED_PATH}/RawImageDataFloat.cpp
                             ${RAWSPEED_PATH}/RawImageDataU16.cpp
                             ${RAWSPEED_PATH}/RawParser.cpp
                             ${RAWSPEED_PATH}/Rw2Decoder.cpp
                             ${RAWSPEED_PATH}/SrwDecoder.cpp
                             ${RAWSPEED_PATH}/TiffEntry.cpp
                             ${RAWSPEED_PATH}/TiffEntryBE.cpp
                             ${RAWSPEED_PATH}/TiffIFD.cpp
                             ${RAWSPEED_PATH}/TiffIFDBE.cpp
                             ${RAWSPEED_PATH}/TiffParser.cpp
                             ${RAWSPEED_PATH}/TiffParserException.cpp
                             ${RAWSPEED_PATH}/TiffParserHeaderless.cpp
                             ${RAWSPEED_PATH}/TiffParserOlympus.cpp
    )

endif()

# -- Common LibRaw library compilation rules ------------------------------------------------------------------

# Flag to add debug print on the console
if(ENABLE_DCRAW_DEBUG)
    add_definitions(-DDCRAW_VERBOSE)
endif()

# For registration to libraw_config.h
MACRO_BOOL_TO_01(ENABLE_DCRAW_DEBUG LIBRAW_USE_DCRAW_DEBUG)

# Flag to add Foveon X3F support
if(ENABLE_X3FTOOLS)
    add_definitions(-DUSE_X3FTOOLS)
endif()

# For registration to libraw_config.h
MACRO_BOOL_TO_01(ENABLE_X3FTOOLS LIBRAW_USE_X3FTOOLS)

# Flag to add Raspberry Pi RAW support
if(ENABLE_6BY9RPI)
    add_definitions(-DUSE_6BY9RPI)
endif()

# For registration to libraw_config.h
MACRO_BOOL_TO_01(ENABLE_6BY9RPI LIBRAW_USE_6BY9RPI)

# Create a config header for client application.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/data/libraw_config.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/libraw_config.h)

# Put the include dirs which are in the source or build tree
# before all other include dirs, so the headers in the sources
# are preferred over the already installed ones
set(CMAKE_INCLUDE_DIRECTORIES_PROJECT_BEFORE ON)

include_directories(${CMAKE_CURRENT_BINARY_DIR}/
                    ${LIBRAW_PATH}/
                   )

# -- Log messages about configuration ------------------------------------------------------------------

message(STATUS "")
message(STATUS "----------------------------------------------------------------------------------")
message(STATUS " Libraw ${RAW_LIB_VERSION_STRING} configuration            <http://www.libraw.org>")
message(STATUS "")

if(OPENMP_SUPPORT_CAN_BE_COMPILED)
    message(STATUS " Libraw will be compiled with OpenMP support .................. YES")
else()
    message(STATUS " Libraw will be compiled with OpenMP support .................. NO")
endif()

if(LCMS_SUPPORT_CAN_BE_COMPILED)
    message(STATUS " Libraw will be compiled with LCMS support .................... YES")
else()
    message(STATUS " Libraw will be compiled with LCMS support .................... NO")
endif()

if(ENABLE_EXAMPLES)
    message(STATUS " Libraw will be compiled with example command-line programs ... YES")
else()
    message(STATUS " Libraw will be compiled with example command-line programs ... NO")
endif()

if(JASPER_SUPPORT_CAN_BE_COMPILED)
    message(STATUS " Libraw will be compiled with RedCine codec support ........... YES")
else()
    message(STATUS " Libraw will be compiled with RedCine codec support ........... NO")
endif()

if(ZLIB_FOUND)
    message(STATUS " Libraw will be compiled with DNG deflate codec support ....... YES")
else()
    message(STATUS " Libraw will be compiled with DNG deflate codec support ....... NO")
endif()

if(JPEG8_FOUND)
    message(STATUS " Libraw will be compiled with DNG lossy codec support ......... YES")
else()
    message(STATUS " Libraw will be compiled with DNG lossy codec support ......... NO")
endif()

if(RAWSPEED_SUPPORT_CAN_BE_COMPILED)
    message(STATUS " Libraw will be compiled with RawSpeed support ................ YES")
else()
    message(STATUS " Libraw will be compiled with RawSpeed support ................ NO")
endif()

if(ENABLE_DCRAW_DEBUG)
    message(STATUS " Libraw will be compiled with debug message from dcraw ........ YES")
else()
    message(STATUS " Libraw will be compiled with debug message from dcraw ........ NO")
endif()

if(ENABLE_X3FTOOLS)
    message(STATUS " Libraw will be compiled with Foveon X3F support .............. YES")
else()
    message(STATUS " Libraw will be compiled with Foveon X3F support .............. NO")
endif()

if(ENABLE_6BY9RPI)
    message(STATUS " Libraw will be compiled with Raspberry Pi RAW support ........ YES")
else()
    message(STATUS " Libraw will be compiled with Raspberry Pi RAW support ........ NO")
endif()

if(BUILD_SHARED_LIBS)
    message(STATUS " Libraw will be compiled as a shared library")
else()
    message(STATUS " Libraw will be compiled as a static library")
endif()

message(STATUS "----------------------------------------------------------------------------------")
message(STATUS "")

# -- Dedicated libraw target which does not support multi-threading ---------------------------------------

if(RAW_LIB_VERSION_STRING VERSION_LESS 0.21)
    set(libraw_LIB_SRCS ${LIBRAW_PATH}/internal/dcraw_common.cpp
                        ${LIBRAW_PATH}/internal/dcraw_fileio.cpp
                        ${LIBRAW_PATH}/internal/demosaic_packs.cpp
                        ${LIBRAW_PATH}/src/libraw_cxx.cpp
                        ${LIBRAW_PATH}/src/libraw_c_api.cpp
                        ${LIBRAW_PATH}/src/libraw_datastream.cpp
    )
else()
    file(GLOB_RECURSE libraw_LIB_SRCS CONFIGURE_DEPENDS "${LIBRAW_PATH}/src/*.cpp")

    # Kill all warnings in LibRaw
    set_source_files_properties(
      ${libraw_LIB_SRCS}
      PROPERTIES
      COMPILE_FLAGS
      "-w")

    # Exclude placeholder (stub) implementations
    file(GLOB_RECURSE exclude_libraw_LIB_SRCS CONFIGURE_DEPENDS "${LIBRAW_PATH}/src/*_ph.cpp")
    list(REMOVE_ITEM libraw_LIB_SRCS ${exclude_libraw_LIB_SRCS})
endif()

if(RAWSPEED_SUPPORT_CAN_BE_COMPILED)
    set(libraw_LIB_SRCS ${libraw_LIB_SRCS} ${librawspeed_LIB_SRCS})
endif()


add_library(raw STATIC ${libraw_LIB_SRCS})
add_library(libraw::libraw ALIAS raw)
target_compile_definitions(raw PRIVATE LIBRAW_NOTHREADS)

# Flag to export library symbols
if (WIN32)
    target_compile_definitions(raw PRIVATE LIBRAW_BUILDLIB)
endif()

# Static builds use LIBRAW_NODLL:
if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(raw PUBLIC LIBRAW_NODLL)
endif()

target_include_directories(raw
        PUBLIC
            $<INSTALL_INTERFACE:libraw>
            $<BUILD_INTERFACE:${LIBRAW_PATH}>)

target_link_libraries(raw PUBLIC ${MATH_LIBRARY})

if(WIN32)
    target_link_libraries(raw PUBLIC ws2_32)
endif()

if(OPENMP_SUPPORT_CAN_BE_COMPILED)
    target_link_libraries(raw PUBLIC OpenMP::OpenMP_CXX)
    if(MINGW)
        target_compile_definitions(raw PRIVATE LIBRAW_FORCE_OPENMP)
    endif()
endif()

if(LCMS_SUPPORT_CAN_BE_COMPILED)
    target_link_libraries(raw PUBLIC ${LCMS2_LIBRARIES})
endif()

if(ZLIB_FOUND)
    target_link_libraries(raw PUBLIC ZLIB::ZLIB)
endif()

if(JPEG8_FOUND)
    target_link_libraries(raw PUBLIC JPEG::JPEG)
endif()

if(JASPER_SUPPORT_CAN_BE_COMPILED)
    target_link_libraries(raw PUBLIC ${JASPER_LIBRARIES})
endif()

if(RAWSPEED_SUPPORT_CAN_BE_COMPILED)
    target_link_libraries(raw PUBLIC ${LIBXML2_LIBRARIES})
endif()

set_target_properties(raw PROPERTIES VERSION     ${RAW_LIB_SO_VERSION_STRING})
set_target_properties(raw PROPERTIES SOVERSION   ${RAW_LIB_SO_CUR_VERSION})
set_target_properties(raw PROPERTIES OUTPUT_NAME "raw")
set_target_properties(raw PROPERTIES COMPILE_PDB_NAME "raw")

# -- Dedicated libraw target to support multi-threading ---------------------------------------------

set(libraw_r_LIB_SRCS ${libraw_LIB_SRCS})

add_library(raw_r STATIC ${libraw_r_LIB_SRCS})
add_library(libraw::libraw_r ALIAS raw_r)

# Flag to export library symbols
if(WIN32)
    target_compile_definitions(raw_r PRIVATE LIBRAW_BUILDLIB)
endif()

# Static builds use LIBRAW_NODLL:
if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(raw_r PUBLIC LIBRAW_NODLL)
endif()

# Always build position-independent code (PIC), even when building Libraw as a
# static library so that shared libraries can link against it, not just
# executables (PIC does not apply on Windows).
# Use set_target_properties() not append_target_property() here as
# POSITION_INDEPENDENT_CODE is a binary ON/OFF switch.
set_target_properties(raw PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(raw_r PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_link_libraries(raw_r PUBLIC ${MATH_LIBRARY})
target_include_directories(raw_r
        PUBLIC
        $<INSTALL_INTERFACE:libraw>
        $<BUILD_INTERFACE:${LIBRAW_PATH}>)

if(WIN32)
    target_link_libraries(raw_r PUBLIC ws2_32)
endif()

if(OPENMP_SUPPORT_CAN_BE_COMPILED)
    target_link_libraries(raw_r PUBLIC OpenMP::OpenMP_CXX)
    if(MINGW)
        target_compile_definitions(raw_r PRIVATE LIBRAW_FORCE_OPENMP)
    endif()
endif()

if(LCMS_SUPPORT_CAN_BE_COMPILED)
    target_link_libraries(raw_r PUBLIC ${LCMS2_LIBRARIES})
endif()

if(ZLIB_FOUND)
    target_link_libraries(raw_r PUBLIC ZLIB::ZLIB)
endif()

if(JPEG8_FOUND)
    target_link_libraries(raw_r PUBLIC JPEG::JPEG)
endif()

if(JASPER_SUPPORT_CAN_BE_COMPILED)
    target_link_libraries(raw_r PUBLIC ${JASPER_LIBRARIES})
endif()

if(RAWSPEED_SUPPORT_CAN_BE_COMPILED)
    target_link_libraries(raw_r PUBLIC ${LIBXML2_LIBRARIES} Threads::Threads)
endif()

set_target_properties(raw_r PROPERTIES VERSION     ${RAW_LIB_SO_VERSION_STRING})
set_target_properties(raw_r PROPERTIES SOVERSION   ${RAW_LIB_SO_CUR_VERSION})
set_target_properties(raw_r PROPERTIES OUTPUT_NAME "raw_r")
set_target_properties(raw_r PROPERTIES COMPILE_PDB_NAME "raw_r")

# -- Files to install -------------------------------------------------------------------------------------
if (LIBRAW_INSTALL)
    # Configure and install data file for packaging.
    include(GNUInstallDirs)

    if(NOT MSVC)
        configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/data/libraw.pc.cmake ${CMAKE_CURRENT_BINARY_DIR}/libraw.pc @ONLY)
        install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libraw.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

        configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/data/libraw_r.pc.cmake ${CMAKE_CURRENT_BINARY_DIR}/libraw_r.pc @ONLY)
        install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libraw_r.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

        configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/data/libraw.lsm.cmake ${CMAKE_CURRENT_BINARY_DIR}/libraw.lsm)
    endif()

    # Install Shared header files.
    install(FILES       ${LIBRAW_PATH}/libraw/libraw.h
                        ${LIBRAW_PATH}/libraw/libraw_alloc.h
                        ${LIBRAW_PATH}/libraw/libraw_const.h
                        ${LIBRAW_PATH}/libraw/libraw_datastream.h
                        ${LIBRAW_PATH}/libraw/libraw_internal.h
                        ${LIBRAW_PATH}/libraw/libraw_types.h
                        ${LIBRAW_PATH}/libraw/libraw_version.h
                        ${CMAKE_CURRENT_BINARY_DIR}/libraw_config.h
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/libraw
            COMPONENT   Devel
           )

    # Install Shared binary files.
    install(TARGETS raw raw_r
            EXPORT ${PROJECT_NAME}Targets
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
           )


    if(NOT BUILD_SHARED_LIBS AND "${CMAKE_CXX_SIMULATE_ID}" STREQUAL "MSVC")
        message("ClangCl does not support pdb generation with static libraries")
    elseif(MSVC)
        install(FILES ${PROJECT_BINARY_DIR}/raw.pdb ${PROJECT_BINARY_DIR}/raw_r.pdb
                DESTINATION ${CMAKE_INSTALL_LIBDIR}
                CONFIGURATIONS Debug RelWithDebInfo
               )
    endif()

    # Install find cmake script to the system for client applications.
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules/FindLibRaw.cmake
            DESTINATION ${CMAKE_INSTALL_DATADIR}/cmake/libraw)

    # Install doc data files.
    if(NOT MSVC)
        install(FILES       ${LIBRAW_PATH}/COPYRIGHT
                            ${LIBRAW_PATH}/LICENSE.CDDL
                            ${LIBRAW_PATH}/LICENSE.LGPL
                            ${LIBRAW_PATH}/Changelog.txt
                DESTINATION ${CMAKE_INSTALL_DOCDIR}
                COMPONENT main
               )
    endif()

    # Uninstall rules
    if(LIBRAW_UNINSTALL_TARGET)
        configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules/Uninstall.cmake ${CMAKE_BINARY_DIR}/cmake_uninstall.cmake COPYONLY)
        add_custom_target(uninstall "${CMAKE_COMMAND}" -P "${CMAKE_BINARY_DIR}/cmake_uninstall.cmake")
    endif()

    # Export the package for use from the build tree
    export(TARGETS raw raw_r
        NAMESPACE libraw:: FILE cmake/${PROJECT_NAME}Targets.cmake)
    export(PACKAGE ${PROJECT_NAME})

    include(CMakePackageConfigHelpers)
    write_basic_package_version_file(
        cmake/${PROJECT_NAME}ConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY AnyNewerVersion)

    configure_package_config_file(
        cmake/${PROJECT_NAME}Config.cmake.in
        cmake/${PROJECT_NAME}Config.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/)

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/cmake/${PROJECT_NAME}Config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/cmake/${PROJECT_NAME}ConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/)

    install(EXPORT ${PROJECT_NAME}Targets
        NAMESPACE libraw:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/)
endif(LIBRAW_INSTALL)

# -- Compile LibRaw Examples --------------------------------------------------------------------------------

# add a small macro so that this is a bit cleaner
macro(LIBRAW_BUILD_SAMPLES)
    set(_filename ${ARGV0})
    set(_rawlib ${ARGV1})
    string(REPLACE "." ";" _temp ${_filename})
    list(GET _temp 0 _target)

    set(${_target}_SRCS ${LIBRAW_PATH}/samples/${_filename})

    add_executable(${_target} ${${_target}_SRCS})
    target_compile_options(${_target} PRIVATE -w)

    target_link_libraries(${_target} PRIVATE ${_rawlib})

    if(${_rawlib} MATCHES "raw_r")
        target_link_libraries(${_target} PUBLIC ${PTHREADS_LIBRARY})
    endif()

    install(TARGETS ${_target}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endmacro(LIBRAW_BUILD_SAMPLES)

if(ENABLE_EXAMPLES)
    LIBRAW_BUILD_SAMPLES(simple_dcraw.cpp raw)

    if(EXISTS mem_image.cpp)
        LIBRAW_BUILD_SAMPLES(mem_image.cpp raw)
    else()
        LIBRAW_BUILD_SAMPLES(mem_image_sample.cpp raw)
    endif()

    LIBRAW_BUILD_SAMPLES(dcraw_emu.cpp raw)
    LIBRAW_BUILD_SAMPLES(4channels.cpp raw)
    LIBRAW_BUILD_SAMPLES(unprocessed_raw.cpp raw)
    LIBRAW_BUILD_SAMPLES(raw-identify.cpp raw)
    LIBRAW_BUILD_SAMPLES(multirender_test.cpp raw)
    LIBRAW_BUILD_SAMPLES(postprocessing_benchmark.cpp raw)

    if(TARGET Threads::Threads)
        if(MSVC)
            LIBRAW_BUILD_SAMPLES(half_mt_win32.c raw_r)
        else()
            LIBRAW_BUILD_SAMPLES(dcraw_half.c raw_r)
            LIBRAW_BUILD_SAMPLES(half_mt.c raw_r)
        endif()
    endif()
endif()
