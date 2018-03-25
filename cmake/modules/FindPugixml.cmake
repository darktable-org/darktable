# - Try to find Pugixml
# Once done, this will define
#
#  Pugixml_FOUND - system has Pugixml
#  Pugixml_INCLUDE_DIRS - the Pugixml include directories
#  Pugixml_LIBRARIES - link these to use Pugixml

include(LibFindMacros)

libfind_pkg_detect(Pugixml pugixml
  FIND_PATH pugixml.hpp
  FIND_LIBRARY pugixml
)

if (Pugixml_PKGCONF_VERSION)
  set(Pugixml_VERSION "${Pugixml_PKGCONF_VERSION}")
elseif(Pugixml_INCLUDE_DIR)
  # no .pc file, look for version manually.
  # yes, libfind_version_header() does not work here.

  file(READ "${Pugixml_INCLUDE_DIR}/pugixml.hpp" Pugixml_VERSION_CONTENT)
  string(REGEX MATCH "PUGIXML_VERSION *([0-9])([0-9][0-9])" _dummy "${Pugixml_VERSION_CONTENT}")
  set(Pugixml_VERSION "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}")
endif()

# Set the include dir variables and the libraries and let libfind_process do the rest.
# NOTE: Singular variables for this library, plural for libraries this lib depends on.
set(Pugixml_PROCESS_INCLUDES Pugixml_INCLUDE_DIR)
set(Pugixml_PROCESS_LIBS Pugixml_LIBRARY)
libfind_process(Pugixml)
