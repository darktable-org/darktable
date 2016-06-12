cmake_minimum_required(VERSION 2.8.5)

if(USE_XMLLINT)
	add_custom_target(
		validate_cameras_xml ALL
		COMMAND ${Xmllint_BIN} --valid --noout ${CMAKE_CURRENT_SOURCE_DIR}/data/cameras.xml
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/data/cameras.xml
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
		COMMENT "Checking validity of external/rawspeed/data/cameras.xml"
	)
endif(USE_XMLLINT)

install(FILES data/cameras.xml data/showcameras.xsl DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable/rawspeed)

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
if(USE_XMLLINT)
  add_dependencies(rawspeed validate_cameras_xml)
endif(USE_XMLLINT)
target_link_libraries(rawspeed ${RAWSPEED_LIBS})

# Adapted from src/cli/CMakeLists.txt
if (BUILD_RS_IDENTIFY)
    add_definitions(-DRS_CAMERAS_XML_PATH="@CMAKE_INSTALL_DATAROOTDIR@/darktable/rawspeed/cameras.xml")
  include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../..)
  include_directories(${CMAKE_CURRENT_BINARY_DIR}/../..)

  add_executable(darktable-rs-identify rawspeed-identify.cpp)
  set_target_properties(darktable-rs-identify PROPERTIES CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
  set_target_properties(darktable-rs-identify PROPERTIES CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)
  set_target_properties(darktable-rs-identify PROPERTIES INSTALL_RPATH ${RPATH_DT}/../${CMAKE_INSTALL_LIBDIR}/darktable)
  set_target_properties(darktable-rs-identify PROPERTIES LINKER_LANGUAGE CXX)
  if(CMAKE_COMPILER_IS_GNUCC)
	  if (GCC_VERSION VERSION_GREATER 4.3)
		  if (CMAKE_SYSTEM_NAME MATCHES "^(DragonFly|FreeBSD|NetBSD|OpenBSD)$")
			  message("-- Force link to libintl on *BSD with GCC 4.3+")
			  target_link_libraries(darktable-rs-identify -lintl)
		  endif()
	  endif()
  endif()
  add_dependencies(darktable-rs-identify rawspeed)
  target_link_libraries(darktable-rs-identify rawspeed_static pugixml -ljpeg)
  install(TARGETS darktable-rs-identify DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
