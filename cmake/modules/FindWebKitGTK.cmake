SET(WEBKITGTK_FIND_REQUIRED ${WebKitGTK_FIND_REQUIRED})

include(Prebuilt)

include(LibFindMacros)
libfind_pkg_check_modules(WEBKITGTK webkitgtk-3.0)
foreach(i ${WEBKITGTK_LIBRARIES})
  find_library(_webkitgtk_LIBRARY NAMES ${i} HINTS ${WEBKITGTK_LIBRARY_DIRS})
  LIST(APPEND WEBKITGTK_LIBRARY ${_webkit_LIBRARY})
	unset(_webkitgtk_LIBRARY CACHE)
endforeach(i)
set(WEBKITGTK_LIBRARIES ${WEBKITGTK_LIBRARY})
unset(WEBKITGTK_LIBRARY CACHE)

if (WEBKITGTK_FOUND)
  set(WEBKITGTK ON CACHE BOOL "Build with webkitgtk support.")
endif (WEBKITGTK_FOUND)


