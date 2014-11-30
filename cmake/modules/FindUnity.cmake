SET(UNITY_FIND_REQUIRED ${Unity_FIND_REQUIRED})

include(Prebuilt)

include(LibFindMacros)
libfind_pkg_check_modules(UNITY unity)
foreach(i ${UNITY_LIBRARIES})
	find_library(_unity_LIBRARY NAMES ${i} HINTS ${UNITY_LIBRARY_DIRS})
	LIST(APPEND UNITY_LIBRARY ${_unity_LIBRARY})
	unset(_unity_LIBRARY CACHE)
endforeach(i)
set(UNITY_LIBRARIES ${UNITY_LIBRARY})
unset(UNITY_LIBRARY CACHE)

if (UNITY_FOUND)
  set(UNITY ON CACHE BOOL "Build with libunity support.")
endif (UNITY_FOUND)
