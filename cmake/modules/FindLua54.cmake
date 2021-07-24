SET(Lua_VERSION_TOO_HIGH 5.5)

include(Prebuilt)
include(LibFindMacros)

libfind_pkg_search_module(Lua54 lua54 lua5.4 lua-5.4 lua)

if(Lua54_FIND_VERSION)
  cmake_minimum_required(VERSION 3.10.0)
  set(Lua54_FAILED_VERSION_CHECK true)

  if(Lua54_FIND_VERSION_EXACT)
    if(Lua54_VERSION VERSION_EQUAL Lua54_FIND_VERSION)
      set(Lua54_FAILED_VERSION_CHECK false)
    endif()
  else()
    if(Lua54_VERSION VERSION_EQUAL   Lua54_FIND_VERSION OR
       Lua54_VERSION VERSION_GREATER Lua54_FIND_VERSION AND
       Lua54_VERSION VERSION_LESS Lua_VERSION_TOO_HIGH)
      set(Lua54_FAILED_VERSION_CHECK false)
    endif()
  endif()

  if(Lua54_FAILED_VERSION_CHECK)
    if(Lua54_FIND_REQUIRED AND NOT Lua54_FIND_QUIETLY)
        if(Lua54_FIND_VERSION_EXACT)
            message(FATAL_ERROR "Lua5.4 version check failed.  Version ${Lua54_VERSION} was found, version ${Lua54_FIND_VERSION} is needed exactly.")
        else(Lua54_FIND_VERSION_EXACT)
            message(FATAL_ERROR "Lua5.4 version check failed.  Version ${Lua54_VERSION} was found, at least version ${Lua54_FIND_VERSION} is required")
        endif(Lua54_FIND_VERSION_EXACT)
    endif(Lua54_FIND_REQUIRED AND NOT Lua54_FIND_QUIETLY)

    set(Lua54_FOUND false)
  endif(Lua54_FAILED_VERSION_CHECK)

endif(Lua54_FIND_VERSION)

if(Lua54_FOUND)
  set(Lua54 ON CACHE BOOL "Build with lua5.4 support.")
  if(APPLE)
    foreach(i ${Lua54_LIBRARIES})
      find_library(_lua54_LIBRARY NAMES ${i} HINTS ${Lua54_LIBRARY_DIRS})
      list(APPEND Lua54_LIBRARIES_FULL ${_lua54_LIBRARY})
      unset(_lua54_LIBRARY CACHE)
    endforeach(i)
    set(Lua54_LIBRARIES ${Lua54_LIBRARIES_FULL})
    unset(Lua54_LIBRARIES_FULL CACHE)
  endif()
endif(Lua54_FOUND)
