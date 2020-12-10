#
# Find the ICU includes and library
#

# This module defines
# ICU_LIBRARIES, the libraries
# ICU_FOUND, If false, do not try to use ICU.

include(LibFindMacros)

# Use pkg-config to get hints about paths
libfind_pkg_check_modules(ICU_PKGCONF icu-i18n)

# Find the libraries
find_library(ICU_LIBRARY
  NAMES icui18n
  HINTS ${ICU_PKGCONF_LIBRARY_DIRS}
)

if(ICU_LIBRARY)
  set(ICU_PROCESS_LIBS ${ICU_LIBRARY})
  libfind_process(ICU)
endif(ICU_LIBRARY)

if(ICU_FOUND)
  set(ICU_LIBRARIES ${ICU_LIBRARY})
else()
  message(STATUS "ICU not found")
endif(ICU_FOUND)
