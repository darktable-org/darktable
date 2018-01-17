# Try to find iso-codes
# The following variables will be set:
#
#  ISO_CODES_LOCATION
#  ISO_CODES_LOCALEDIR

find_package(PkgConfig QUIET)
pkg_check_modules(PC_ISO_CODES iso-codes)

set(ISO_CODES_FOUND 0)

if(PC_ISO_CODES_VERSION VERSION_LESS IsoCodes_FIND_VERSION)
  message(STATUS "iso-codes version ${PC_ISO_CODES_VERSION} found but we need at least version ${IsoCodes_FIND_VERSION}")
else()
  if(PC_ISO_CODES_FOUND)
    set(ISO_CODES_FOUND 1)
    set(ISO_CODES_LOCATION "${PC_ISO_CODES_PREFIX}/share/iso-codes/json")
    set(ISO_CODES_LOCALEDIR "${PC_ISO_CODES_PREFIX}/share/locale")
  endif(PC_ISO_CODES_FOUND)
endif()
