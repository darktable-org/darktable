# Try to find iso-codes
# The following variables will be set:
#
#  IsoCodes_LOCATION
#  IsoCodes_LOCALEDIR

find_package(PkgConfig QUIET)
pkg_check_modules(PC_IsoCodes iso-codes)

set(IsoCodes_FOUND 0)

if(PC_IsoCodes_VERSION VERSION_LESS IsoCodes_FIND_VERSION)
  message(STATUS "iso-codes version ${PC_IsoCodes_VERSION} found but we need at least version ${IsoCodes_FIND_VERSION}")
else()
  if(PC_IsoCodes_FOUND)
    set(IsoCodes_FOUND 1)
    set(IsoCodes_LOCATION "${PC_IsoCodes_PREFIX}/share/iso-codes/json")
    set(IsoCodes_LOCALEDIR "${PC_IsoCodes_PREFIX}/share/locale")
  endif(PC_IsoCodes_FOUND)
endif()
