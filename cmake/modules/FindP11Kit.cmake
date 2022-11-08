# - Try to find p11-kit
# Once done, this will define
#
#  P11Kit_FOUND - system has p11-kit
#  P11Kit_INCLUDE_DIRS - the p11-kit include directories
#  P11Kit_LINK_LIBRARIES - link these to use p11-kit

include(LibFindMacros)
libfind_pkg_search_module(P11Kit p11-kit-1)
if(P11Kit_FOUND)
  message(STATUS "Found p11-kit ${P11Kit_VERSION}")
endif()
