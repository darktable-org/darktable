SET(COLORDGTK_FIND_REQUIRED ${ColordGTK_FIND_REQUIRED})

include(Prebuilt)

include(LibFindMacros)
libfind_pkg_check_modules(COLORDGTK colord-gtk)
foreach(i ${COLORDGTK_LIBRARIES})
	find_library(_colordgtk_LIBRARY NAMES ${i} HINTS ${COLORDGTK_LIBRARY_DIRS})
	LIST(APPEND COLORDGTK_LIBRARY ${_colordgtk_LIBRARY})
	unset(_colordgtk_LIBRARY CACHE)
endforeach(i)
set(COLORDGTK_LIBRARIES ${COLORDGTK_LIBRARY})
unset(COLORDGTK_LIBRARY CACHE)

if (COLORDGTK_FOUND)
  set(COLORDGTK ON CACHE BOOL "Build with libcolord-gtk support.")
endif (COLORDGTK_FOUND)
