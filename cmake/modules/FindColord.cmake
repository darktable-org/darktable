SET(COLORD_FIND_REQUIRED ${Colord_FIND_REQUIRED})

include(Prebuilt)

if (PKG_CONFIG_FOUND)
  include(FindPkgConfig)
  pkg_check_modules(COLORD colord)
endif (PKG_CONFIG_FOUND)

if (COLORD_FOUND)
  set(COLORD ON CACHE BOOL "Build with libcolord support.")
endif (COLORD_FOUND)
