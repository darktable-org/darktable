# - Find the native Pugixml includes and library

find_path(Pugixml_INCLUDE_DIR 
  NAMES
    pugixml.hpp
  PATHS
    /usr/local/include
    /usr/include
)

find_library(Pugixml_LIBRARY
  NAMES
    pugixml
  PATHS
    /usr/local/lib
    /usr/lib
)

if (NOT Pugixml_INCLUDE_DIR OR NOT Pugixml_LIBRARY)
  message(FATAL_ERROR "Couldn't find pugixml, you probably need libpugixml-dev or similar")
endif()

file(READ ${Pugixml_INCLUDE_DIR}/pugixml.hpp Pugixml_VERSION_CONTENT)
string(REGEX MATCH "PUGIXML_VERSION *([0-9])([0-9][0-9])"  _dummy "${Pugixml_VERSION_CONTENT}")
set(Pugixml_VERSION "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}")

if(Pugixml_VERSION VERSION_LESS Pugixml_FIND_VERSION)
  message(FATAL_ERROR "pugixml version check failed.  Version ${Pugixml_VERSION} was found, at least version ${Pugixml_FIND_VERSION} is required")
endif()

SET(Pugixml_LIBRARIES ${Pugixml_LIBRARY})
SET(Pugixml_INCLUDE_DIRS ${Pugixml_INCLUDE_DIR})

include(FindPackageHandleStandardArgs)
# handle the QUIETLY and REQUIRED arguments and set Pugixml_FOUND to TRUE
# if all listed variables are TRUE
find_package_handle_standard_args(Pugixml  DEFAULT_MSG
                                  Pugixml_LIBRARY Pugixml_INCLUDE_DIR)
mark_as_advanced(Pugixml_INCLUDE_DIR Pugixml_LIBRARY )
