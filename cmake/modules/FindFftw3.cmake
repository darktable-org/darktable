# - Try to find Fftw3
# Once done, this will define
#
#  Fftw3_FOUND - system has Fftw3
#  Fftw3_INCLUDE_DIRS - the Fftw3 include directories
#  Fftw3_LIBRARIES - link these to use Fftw3
#
#  Usage: libfind_pkg_detect(<prefix> <pkg-config args> FIND_PATH <name> [other args] FIND_LIBRARY <name> [other args])
#  E.g. libfind_pkg_detect(SDL2 sdl2 FIND_PATH SDL.h PATH_SUFFIXES SDL2 FIND_LIBRARY SDL2)

include(LibFindMacros)

libfind_pkg_detect(Fftw3 fftw3
  FIND_PATH fftw3.h
  FIND_LIBRARY fftw3f libfftw3f
)

if (Fftw3_PKGCONF_VERSION)
  set(Fftw3_VERSION "${Fftw3_PKGCONF_VERSION}")
endif()

# Set the include dir variables and the libraries and let libfind_process do the rest.
# NOTE: Singular variables for this library, plural for libraries this this lib depends on.
if (Fftw3_FOUND)
  set(Fftw3_PROCESS_INCLUDES Fftw3_INCLUDE_DIR)
  set(Fftw3_PROCESS_LIBS Fftw3_LIBRARY)
  libfind_process(Fftw3)
endif()

