#
# Find the GMIC includes and library
#

# This module defines
# GMIC_INCLUDE_DIR, where to find *.h etc
# GMIC_LIBRARY, the libraries
# GMIC_FOUND, If false, do not try to use GMIC.


include(LibFindMacros)

# Use pkg-config to get hints about paths
libfind_pkg_check_modules(GMIC_PKGCONF GMIC)

# Include dir
find_path(GMIC_INCLUDE_DIR
  NAMES gmic.h
  HINTS ${GMIC_PKGCONF_INCLUDE_DIRS}
  PATH_SUFFIXES GMIC
)

# Finally the library itself
find_library(GMIC_LIBRARY
  NAMES gmic
  HINTS ${GMIC_PKGCONF_LIBRARY_DIRS}
)

# Set the include dir variables and the libraries and let libfind_process do the rest.
# NOTE: Singular variables for this library, plural for libraries this lib depends on.
set(GMIC_PROCESS_INCLUDES ${GMIC_INCLUDE_DIR})
set(GMIC_PROCESS_LIBS ${GMIC_LIBRARY})
libfind_process(GMIC)

if(GMIC_FOUND)
  set(GMIC_INCLUDE_DIRS ${GMIC_INCLUDE_DIR})
  set(GMIC_LIBRARIES ${GMIC_LIBRARY})
endif(GMIC_FOUND)
