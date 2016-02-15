cmake_minimum_required(VERSION 2.6)

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
# We need C++11
#
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	execute_process(COMMAND ${CMAKE_CXX_COMPILER} -dumpversion OUTPUT_VARIABLE GCC_VERSION)
	if (GCC_VERSION VERSION_LESS 4.7)
		set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++0x")
	else()
		set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
	endif()
else()
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
endif()

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

add_library(rawspeed STATIC ${RAWSPEED_SOURCES})
target_link_libraries(rawspeed ${RAWSPEED_LIBS})
