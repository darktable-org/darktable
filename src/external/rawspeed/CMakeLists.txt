cmake_minimum_required(VERSION 2.6)

if(USE_XMLLINT)
	execute_process(
		COMMAND ${Xmllint_BIN} --valid ${CMAKE_CURRENT_SOURCE_DIR}/data/cameras.xml
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
		OUTPUT_QUIET ERROR_QUIET
		RESULT_VARIABLE Xmllint_RES
	)
	if(NOT ${Xmllint_RES} EQUAL 0 )
	  # should be FATAL_ERROR, but currently fails => no reason to just break build.
		message(WARNING "external/rawspeed/data/cameras.xml is not valid")
	endif(NOT ${Xmllint_RES} EQUAL 0 )
endif(USE_XMLLINT)

install(FILES data/cameras.xml data/showcameras.xsl DESTINATION ${SHARE_INSTALL}/darktable/rawspeed)

FILE(GLOB RAWSPEED_SOURCES "RawSpeed/*.cpp")
LIST(REMOVE_ITEM RAWSPEED_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/RawSpeed/RawSpeed.cpp")

#
# build librawspeed
#
if(WIN32)
  set(RAWSPEED_LIBS "msvcrt")
else(WIN32)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")
endif(WIN32)

#
# set development flags for rawspeed
#
if(NOT SOURCE_PACKAGE AND NOT APPLE)
  # apple build fails on deprecated warnings..
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror -Werror=type-limits")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror -Werror=type-limits")
elseif(SOURCE_PACKAGE)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_RELEASE")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_RELEASE")
endif()

add_definitions(-std=c++11)
add_library(rawspeed STATIC ${RAWSPEED_SOURCES})
target_link_libraries(rawspeed ${RAWSPEED_LIBS})

# Adapted from src/cli/CMakeLists.txt
if (DEFINED DT_RS_IDENTIFY)
  include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../..)
  include_directories(${CMAKE_CURRENT_BINARY_DIR}/../..)

  add_executable(darktable-rs-identify rawspeed-identify.cpp)
  set_target_properties(darktable-rs-identify PROPERTIES CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
  set_target_properties(darktable-rs-identify PROPERTIES CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)
  if(APPLE)
    set_target_properties(darktable-rs-identify PROPERTIES INSTALL_RPATH @loader_path/../${LIB_INSTALL}/darktable)
  else(APPLE)
    set_target_properties(darktable-rs-identify PROPERTIES INSTALL_RPATH $ORIGIN/../${LIB_INSTALL}/darktable)
  endif(APPLE)
  set_target_properties(darktable-rs-identify PROPERTIES LINKER_LANGUAGE CXX)
  if(CMAKE_COMPILER_IS_GNUCC)
	  if (GCC_VERSION VERSION_GREATER 4.3)
		  if (CMAKE_SYSTEM_NAME MATCHES "^(DragonFly|FreeBSD|NetBSD|OpenBSD)$")
			  message("-- Force link to libintl on *BSD with GCC 4.3+")
			  target_link_libraries(darktable-rs-identify -lintl)
		  endif()
	  endif()
  endif()
  target_link_libraries(darktable-rs-identify lib_darktable)
  install(TARGETS darktable-rs-identify DESTINATION bin)
endif()
