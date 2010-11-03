# - Try to find gdk-pixbuf 2.0
# Once done, this will define
#
#  GDK-PixBuf_FOUND - system has GDK-PixBuf
#  GDK-PixBuf_INCLUDE_DIRS - the GDK-PixBuf include directories
#  GDK-PixBuf_LIBRARIES - link these to use GDK-PixBuf

include(LibFindMacros)

# Use pkg-config to get hints about paths
libfind_pkg_check_modules(GDK-PixBuf_PKGCONF gdk-pixbuf-2.0)

# Main include dir
find_path(GDK-PixBuf_INCLUDE_DIR
  NAMES gdk-pixbuf/gdk-pixbuf.h
  PATHS ${GDK-PixBuf_PKGCONF_INCLUDE_DIRS}
  PATH_SUFFIXES gtk-2.0
)

# Finally the library itself
find_library(GDK-PixBuf_LIBRARY
  NAMES gdk_pixbuf-2.0
  PATHS ${GDK-PixBuf_PKGCONF_LIBRARY_DIRS}
)

# Set the include dir variables and the libraries and let libfind_process do the rest.
# NOTE: Singular variables for this library, plural for libraries this this lib depends on.
set(GDK-PixBuf_PROCESS_INCLUDES GDK-PixBuf_INCLUDE_DIR)
set(GDK-PixBuf_PROCESS_LIBS GDK-PixBuf_LIBRARY)
libfind_process(GDK-PixBuf)


