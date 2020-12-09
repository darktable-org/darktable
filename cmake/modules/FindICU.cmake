#
# Find the ICU includes and library
#

# This module defines
# ICU_INCLUDE_DIR, where to find *.h etc
# ICU_LIBRARY, the libraries
# ICU_FOUND, If false, do not try to use ICU.

include(LibFindMacros)

# Use pkg-config to get hints about paths
libfind_pkg_check_modules(ICU_PKGCONF ICU)

# Find the library
find_library(ICU_LIBRARY
  NAMES libicu.so
  HINTS ${ICU_PKGCONF_LIBRARY_DIRS}
)

if(ICU_LIBRARY)
  # Set the libraries and let libfind_process do the rest.
  # NOTE: Singular variables for this library, plural for libraries this lib depends on.
  set(ICU_PROCESS_LIBS ${ICU_LIBRARY})
  libfind_process(ICU)
endif(ICU_LIBRARY)

if(NOT ICU_FOUND)
  message(STATUS "ICU not found")
endif(NOT ICU_FOUND)
