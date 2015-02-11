# - Try to find Gettext
# Once done, this will define
#
#  Gettext_FOUND - system has Gettext
#  Gettext_INCLUDE_DIRS - the Gettext include directories
#  Gettext_LIBRARIES - link these to use Gettext
#
# See documentation on how to write CMake scripts at
# http://www.cmake.org/Wiki/CMake:How_To_Find_Libraries

include(LibFindMacros)

# On Linux there is no pkgconfig script, but with this we force Gettext_PKGCONF_INCLUDE_DIRS to ""
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(Gettext_PKGCONF_INCLUDE_DIRS "")
else(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  libfind_pkg_check_modules(Gettext_PKGCONF Gettext)
endif(CMAKE_SYSTEM_NAME STREQUAL "Linux")

if(WIN32 OR APPLE)
  set(Gettext_LIBRARY_SEARCH_DIRS
    /opt/local/lib
    /sw/local/lib
  )

  find_library(Gettext_LIBRARY
    NAMES intl
    PATHS ${Gettext_LIBRARY_SEARCH_DIRS}
    HINTS ${Gettext_PKGCONF_LIBRARY_DIRS}
  )

  set(Gettext_PROCESS_LIBS Gettext_LIBRARY)
endif()

find_path(Gettext_INCLUDE_DIR
  NAMES libintl.h
  HINTS ${Gettext_PKGCONF_INCLUDE_DIRS}
  PATHS /opt/local/include
)

set(Gettext_PROCESS_INCLUDES Gettext_INCLUDE_DIR)
libfind_process(Gettext)
