#
# Find the GraphicsMagick includes and library
#

# This module defines
# GraphicsMagick_INCLUDE_DIR, where to find *.h etc
# GraphicsMagick_LIBRARY, the libraries
# GraphicsMagick_FOUND, If false, do not try to use LCMS.


include(LibFindMacros)

# Use pkg-config to get hints about paths
libfind_pkg_check_modules(GraphicsMagick_PKGCONF GraphicsMagick)

# Include dir
find_path(GraphicsMagick_INCLUDE_DIR
  NAMES GraphicsMagick/magick/magick.h
  PATHS ${GraphicsMagick_PKGCONF_INCLUDE_DIRS}
  PATH_SUFFIXES GraphicsMagick
)

# Finally the library itself
find_library(GraphicsMagick_LIBRARY
  NAMES GraphicsMagick
  PATHS ${GraphicsMagick_PKGCONF_LIBRARY_DIRS}
)

# Set the include dir variables and the libraries and let libfind_process do the rest.
# NOTE: Singular variables for this library, plural for libraries this lib depends on.
set(GraphicsMagick_PROCESS_INCLUDES GraphicsMagick_INCLUDE_DIR)
set(GraphicsMagick_PROCESS_LIBS GraphicsMagick_LIBRARY)
libfind_process(GraphicsMagick)





