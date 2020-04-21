SET(Lua_VERSION_TOO_HIGH 5.4)

include(Prebuilt)
include(LibFindMacros)

libfind_pkg_search_module(Lua53 lua53 lua5.3 lua-5.3 lua)

if(Lua53_FIND_VERSION)
  cmake_minimum_required(VERSION 3.10.0)
  set(Lua53_FAILED_VERSION_CHECK true)

  if(Lua53_FIND_VERSION_EXACT)
    if(Lua53_VERSION VERSION_EQUAL Lua53_FIND_VERSION)
      set(Lua53_FAILED_VERSION_CHECK false)
    endif()
  else()
    if(Lua53_VERSION VERSION_EQUAL   Lua53_FIND_VERSION OR
       Lua53_VERSION VERSION_GREATER Lua53_FIND_VERSION AND
       Lua53_VERSION VERSION_LESS Lua_VERSION_TOO_HIGH)
      set(Lua53_FAILED_VERSION_CHECK false)
    endif()
  endif()

  if(Lua53_FAILED_VERSION_CHECK)
    if(Lua53_FIND_REQUIRED AND NOT Lua53_FIND_QUIETLY)
        if(Lua53_FIND_VERSION_EXACT)
            message(FATAL_ERROR "Lua5.3 version check failed.  Version ${Lua53_VERSION} was found, version ${Lua53_FIND_VERSION} is needed exactly.")
        else(Lua53_FIND_VERSION_EXACT)
            message(FATAL_ERROR "Lua5.3 version check failed.  Version ${Lua53_VERSION} was found, at least version ${Lua53_FIND_VERSION} is required")
        endif(Lua53_FIND_VERSION_EXACT)
    endif(Lua53_FIND_REQUIRED AND NOT Lua53_FIND_QUIETLY)

    set(Lua53_FOUND false)
  endif(Lua53_FAILED_VERSION_CHECK)

endif(Lua53_FIND_VERSION)

if(Lua53_FOUND)
  set(Lua53 ON CACHE BOOL "Build with lua5.3 support.")
  if(APPLE)
    foreach(i ${Lua53_LIBRARIES})
      find_library(_lua53_LIBRARY NAMES ${i} HINTS ${Lua53_LIBRARY_DIRS})
      list(APPEND Lua53_LIBRARIES_FULL ${_lua53_LIBRARY})
      unset(_lua53_LIBRARY CACHE)
    endforeach(i)
    set(Lua53_LIBRARIES ${Lua53_LIBRARIES_FULL})
    unset(Lua53_LIBRARIES_FULL CACHE)
  endif()
endif(Lua53_FOUND)
