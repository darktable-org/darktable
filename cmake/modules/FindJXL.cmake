# Find libjxl
# Will define:
# - JXL_FOUND
# - JXL_INCLUDE_DIRS directory to include for libjxl headers
# - JXL_LIBRARIES libraries to link to

include(LibFindMacros)

# Use pkg-config to get hints about paths
libfind_pkg_check_modules(JXL_PKGCONF libjxl)

find_path(JXL_INCLUDE_DIR
  NAMES jxl/encode.h
  HINTS ${JXL_PKGCONF_INCLUDE_DIRS}
)

find_library(JXL_LIBRARY
  NAMES jxl
  HINTS ${JXL_PKGCONF_LIBRARY_DIRS}
)

find_library(JXL_THREADS_LIBRARY
  NAMES jxl_threads
  HINTS ${JXL_PKGCONF_LIBRARY_DIRS}
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(JXL DEFAULT_MSG JXL_LIBRARY JXL_INCLUDE_DIR)

if(JXL_PKGCONF_VERSION VERSION_LESS JXL_FIND_VERSION)
  set(JXL_FOUND false)
endif()

if(JXL_FOUND)
  set(JXL_LIBRARIES ${JXL_LIBRARY} ${JXL_THREADS_LIBRARY})
  set(JXL_INCLUDE_DIRS ${JXL_INCLUDE_DIR})
endif(JXL_FOUND)
