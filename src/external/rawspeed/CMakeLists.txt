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

add_definitions(-std=c++11)
add_library(rawspeed STATIC ${RAWSPEED_SOURCES})
target_link_libraries(rawspeed ${RAWSPEED_LIBS})

# Adapted from src/cli/CMakeLists.txt
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/..)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/..)

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
