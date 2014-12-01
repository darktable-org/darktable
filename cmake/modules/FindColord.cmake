SET(COLORD_FIND_REQUIRED ${Colord_FIND_REQUIRED})

include(Prebuilt)

include(LibFindMacros)
libfind_pkg_check_modules(COLORD colord)
foreach(i ${COLORD_LIBRARIES})
	find_library(_colord_LIBRARY NAMES ${i} HINTS ${COLORD_LIBRARY_DIRS})
	LIST(APPEND COLORD_LIBRARY ${_colord_LIBRARY})
	unset(_colord_LIBRARY CACHE)
endforeach(i)
set(COLORD_LIBRARIES ${COLORD_LIBRARY})
unset(COLORD_LIBRARY CACHE)

if (COLORD_FOUND)
  set(COLORD ON CACHE BOOL "Build with libcolord support.")
endif (COLORD_FOUND)
