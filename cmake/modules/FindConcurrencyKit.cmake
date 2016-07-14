# - Try to find ConcurrencyKit
#
# Copyright (C) 2016 LebedevRI.
#
# Once done, this will define
#
#  ConcurrencyKit_FOUND - system has ConcurrencyKit
#  ConcurrencyKit_INCLUDE_DIRS - the ConcurrencyKit include directories
#  ConcurrencyKit_LIBRARIES - link these to use ConcurrencyKit

include(LibFindMacros)

# Use pkg-config to get hints about paths
libfind_pkg_check_modules(ConcurrencyKit_PKGCONF ck)

# Include dir
find_path(ConcurrencyKit_INCLUDE_DIR
  NAMES ck_pr.h
  PATHS ${ConcurrencyKit_PKGCONF_INCLUDE_DIRS}
)
mark_as_advanced(ConcurrencyKit_INCLUDE_DIR)

# Finally the library itself
find_library(ConcurrencyKit_LIBRARY
  NAMES ck
  PATHS ${ConcurrencyKit_PKGCONF_LIBRARY_DIRS}
)
mark_as_advanced(ConcurrencyKit_LIBRARY)

libfind_process(ConcurrencyKit)
