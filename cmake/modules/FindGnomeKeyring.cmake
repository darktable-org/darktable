# - Find the native gnome-keyring includes and library
#
# This module defines
#  GnomeKeyring_INCLUDE_DIRS, where to find gnome-keyring.h, etc.
#  GnomeKeyring_LIBRARIES, the libraries to link against to use gnome-keyring.
#  GnomeKeyring_FOUND, If false, do not try to use gnome-keyring.
# also defined, but not for general use are
#  GnomeKeyring_LIBRARY, where to find the gnome-keyring library.


#=============================================================================
# Copyright 2010 henrik andersson
#=============================================================================

SET(GNOMEKEYRING_FIND_REQUIRED ${GnomeKeyring_FIND_REQUIRED})

libfind_pkg_check_modules(GNOMEKEYRING_PKGCONF gnome-keyring-1)

find_path(GNOMEKEYRING_INCLUDE_DIR
          NAMES gnome-keyring.h
          PATHS
            /usr/include/gnome-keyring-1
            /usr/local/include/gnome-keyring-1
            /opt/local/include/gnome-keyring-1
          HINTS ${GNOMEKEYRING_PKGCONF_INCLUDE_DIRS}
)
mark_as_advanced(GNOMEKEYRING_INCLUDE_DIR)

set(GNOMEKEYRING_NAMES ${GNOMEKEYRING_NAMES} gnome-keyring libgnome-keyring)
find_library(GNOMEKEYRING_LIBRARY
             NAMES ${GNOMEKEYRING_NAMES}
             HINTS ${GNOMEKEYRING_PKGCONF_LIBRARY_DIRS}
)
mark_as_advanced(GNOMEKEYRING_LIBRARY)

# handle the QUIETLY and REQUIRED arguments and set GNOMEKEYRING_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(GNOMEKEYRING DEFAULT_MSG GNOMEKEYRING_LIBRARY GNOMEKEYRING_INCLUDE_DIR)

IF(GNOMEKEYRING_FOUND)
  SET(GnomeKeyring_LIBRARIES ${GNOMEKEYRING_LIBRARY})
  SET(GnomeKeyring_INCLUDE_DIRS ${GNOMEKEYRING_INCLUDE_DIR})
  SET(GnomeKeyring_VERSION ${GNOMEKEYRING_PKGCONF_VERSION})
ENDIF(GNOMEKEYRING_FOUND)
