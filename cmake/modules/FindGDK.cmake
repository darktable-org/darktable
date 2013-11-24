# - Try to find GDK 2.0
# Once done, this will define
#
#  GDK_FOUND - system has GDK
#  GDK_INCLUDE_DIRS - the GDK include directories
#  GDK_LIBRARIES - link these to use GDK

include(LibFindMacros)

# Dependencies
libfind_package(GDK GDK-PixBuf)
libfind_package(GDK Pango)
libfind_package(GDK GIO)

# Use pkg-config to get hints about paths
libfind_pkg_check_modules(GDK_PKGCONF gdk-2.0)

# Main include dir
find_path(GDK_INCLUDE_DIR
  NAMES gdk/gdk.h
  HINTS ${GDK_PKGCONF_INCLUDE_DIRS}
  PATH_SUFFIXES gtk-2.0
)

# Glib-related libraries also use a separate config header, which is in lib dir
find_path(GDKConfig_INCLUDE_DIR
  NAMES gdkconfig.h
  HINTS ${GDK_PKGCONF_INCLUDE_DIRS}
  PATHS /usr
  PATH_SUFFIXES lib/gtk-2.0/include
)

# Finally the library itself
find_library(GDK_LIBRARY
  NAMES gdk-x11-2.0
  HINTS ${GDK_PKGCONF_LIBRARY_DIRS}
)

# Set the include dir variables and the libraries and let libfind_process do the rest.
# NOTE: Singular variables for this library, plural for libraries this this lib depends on.
set(GDK_PROCESS_INCLUDES GDK_INCLUDE_DIR GDKConfig_INCLUDE_DIR GDK-PixBuf_INCLUDE_DIRS Pango_INCLUDE_DIRS GIO_INCLUDE_DIRS)
set(GDK_PROCESS_LIBS GDK_LIBRARY GDK-PixBuf_LIBRARIES Pango_LIBRARIES GIO_LIBRARIES)
libfind_process(GDK)

