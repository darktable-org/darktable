include(Prebuilt)

if (PKG_CONFIG_FOUND)
  include(FindPkgConfig)
  pkg_check_modules(VTE vte)
endif (PKG_CONFIG_FOUND)

if (VTE_FOUND)
  set(VTE ON CACHE BOOL "Build with VTE support.")
endif (VTE_FOUND)

# if (VTE)
#   add_definitions(-DVTE)
# endif (VTE)
