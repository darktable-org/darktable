# - Find the native sqlite3 includes and library
#
# This module defines
#  GPHOTO2_INCLUDE_DIR, where to find libgphoto2 header files
#  GPHOTO2_LIBRARIES, the libraries to link against to use libgphoto2
#  GPHOTO2_FOUND, If false, do not try to use libgphoto2.
#  GPHOTO2_VERSION_STRING, e.g. 2.4.14
#  GPHOTO2_VERSION_MAJOR, e.g. 2
#  GPHOTO2_VERSION_MINOR, e.g. 4
#  GPHOTO2_VERSION_PATCH, e.g. 14
#
# also defined, but not for general use are
#  GPHOTO2_LIBRARY, where to find the sqlite3 library.


#=============================================================================
# Copyright 2010 henrik andersson
#=============================================================================

include(LibFindMacros)

SET(GPHOTO2_FIND_REQUIRED ${Gphoto2_FIND_REQUIRED})

find_path(GPHOTO2_INCLUDE_DIR gphoto2/gphoto2.h)
mark_as_advanced(GPHOTO2_INCLUDE_DIR)

set(GPHOTO2_NAMES ${GPHOTO2_NAMES} gphoto2 libgphoto2)
set(GPHOTO2_PORT_NAMES ${GPHOTO2_PORT_NAMES} gphoto2_port libgphoto2_port)
find_library(GPHOTO2_LIBRARY NAMES ${GPHOTO2_NAMES} )
find_library(GPHOTO2_PORT_LIBRARY NAMES ${GPHOTO2_PORT_NAMES} )
mark_as_advanced(GPHOTO2_LIBRARY)
mark_as_advanced(GPHOTO2_PORT_LIBRARY)

# Detect libgphoto2 version
libfind_pkg_check_modules(PC_GPHOTO2 libgphoto2)
if(PC_GPHOTO2_FOUND)
  set(GPHOTO2_VERSION_STRING "${PC_GPHOTO2_VERSION}")
endif()

# handle the QUIETLY and REQUIRED arguments and set GPHOTO2_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(GPHOTO2 DEFAULT_MSG GPHOTO2_LIBRARY GPHOTO2_INCLUDE_DIR)

IF(GPHOTO2_FOUND)
  SET(Gphoto2_LIBRARIES ${GPHOTO2_LIBRARY} ${GPHOTO2_PORT_LIBRARY})
  SET(Gphoto2_INCLUDE_DIRS ${GPHOTO2_INCLUDE_DIR})

  # libgphoto2 dynamically loads and unloads usb library
  # without calling any cleanup functions (since they are absent from libusb-0.1).
  # This leaves usb event handling threads running with invalid callback and return addresses,
  # which causes a crash after any usb event is generated, at least in Mac OS X. 
  # libusb1 backend does correctly call exit function, but ATM it crashes anyway.
  # Workaround is to link against libusb so that it wouldn't get unloaded.
  IF(APPLE)
    find_library(USB_LIBRARY NAMES usb-1.0 libusb-1.0)
    mark_as_advanced(USB_LIBRARY)
    IF(USB_LIBRARY)
      SET(Gphoto2_LIBRARIES ${Gphoto2_LIBRARIES} ${USB_LIBRARY})
    ENDIF(USB_LIBRARY)
  ENDIF(APPLE)

ENDIF(GPHOTO2_FOUND)
