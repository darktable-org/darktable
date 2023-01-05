#.rst:
# Findlibraw
# -------
#
# Try to find libraw on a Unix system.
#
# This will define the following variables:
#
# ``libraw_FOUND``
#     True if (the requested version of) libraw is available
# ``libraw_VERSION``
#     The version of libraw
# ``libraw_LIBRARIES``
#     This should be passed to target_compile_options() if the target is not
#     used for linking
# ``libraw_INCLUDE_DIRS``
#     This should be passed to target_include_directories() if the target is not
#     used for linking
# ``libraw_DEFINITIONS``
#     This should be passed to target_compile_options() if the target is not
#     used for linking

find_package(PkgConfig QUIET)
pkg_check_modules(PKG_libraw QUIET libraw)

set(libraw_VERSION ${PKG_libraw_VERSION})
set(libraw_DEFINITIONS ${PKG_libraw_CFLAGS_OTHER})

find_path(libraw_INCLUDE_DIR
    NAMES libraw/libraw.h
    HINTS ${PKG_libraw_INCLUDE_DIRS}
)

find_library(libraw_LIBRARY
    NAMES raw
    HINTS ${PKG_libraw_LIBRARY_DIRS}
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(libraw
    FOUND_VAR libraw_FOUND
    REQUIRED_VARS libraw_LIBRARY
                  libraw_INCLUDE_DIR
    VERSION_VAR libraw_VERSION
)

set(libraw_INCLUDE_DIRS ${libraw_INCLUDE_DIR})
set(libraw_LIBRARIES ${libraw_LIBRARY})

mark_as_advanced(libraw_INCLUDE_DIR)
mark_as_advanced(libraw_LIBRARY)
