# - Try to find Gettext
# Once done, this will define
#
#  GETTEXT_FOUND - system has Gettext
#  GETTEXT_INCLUDE_DIRS - the Gettext include directories
#  GETTEXT_LIBRARIES - link these to use Gettext
#
# See documentation on how to write CMake scripts at
# http://www.cmake.org/Wiki/CMake:How_To_Find_Libraries

if(GETTEXT_LIBRARIES)
  set(GETTEXT_FIND_QUIETLY TRUE)
endif(GETTEXT_LIBRARIES)

find_path(GETTEXT_INCLUDE_DIR
    NAMES libintl.h
    PATHS /opt/local/lib /sw/local/lib)

if(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
find_library(GETTEXT_LIBRARIES NAMES c) # Gettext is in the GNU libc  
else(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
  find_library(GETTEXT_LIBRARIES 
      NAMES intl
      PATHS /opt/local/lib /sw/local/lib)
endif(${CMAKE_SYSTEM_NAME} MATCHES "Linux")

include(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(
    GETTEXT DEFAULT_MSG
    GETTEXT_LIBRARIES
    GETTEXT_INCLUDE_DIR)
mark_as_advanced(GETTEXT_INCLUDE_DIR GETTEXT_LIBRARIES)
