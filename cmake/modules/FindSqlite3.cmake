# - Try to find Sqlite3
# Once done, this will define
#
#  Sqlite3_FOUND - system has Sqlite3
#  Sqlite3_INCLUDE_DIRS - the Sqlite3 include directories
#  Sqlite3_LIBRARIES - link these to use Sqlite3

include(LibFindMacros)

libfind_pkg_detect(Sqlite3 sqlite3
  FIND_PATH sqlite3.h
  FIND_LIBRARY sqlite3 libsqlite3
)

if (Sqlite3_PKGCONF_VERSION)
  set(Sqlite3_VERSION "${Sqlite3_PKGCONF_VERSION}")
endif()

# Set the include dir variables and the libraries and let libfind_process do the rest.
# NOTE: Singular variables for this library, plural for libraries this lib depends on.
set(Sqlite3_PROCESS_INCLUDES Sqlite3_INCLUDE_DIR)
set(Sqlite3_PROCESS_LIBS Sqlite3_LIBRARY)
libfind_process(Sqlite3)
