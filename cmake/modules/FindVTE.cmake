include(Prebuilt)

include(LibFindMacros)
libfind_pkg_check_modules(VTE vte)
foreach(i ${VTE_LIBRARIES})
	find_library(_vte_LIBRARY NAMES ${i} HINTS ${VTE_LIBRARY_DIRS})
	LIST(APPEND VTE_LIBRARY ${_vte_LIBRARY})
	unset(_vte_LIBRARY CACHE)
endforeach(i)
set(VTE_LIBRARIES ${VTE_LIBRARY})
unset(VTE_LIBRARY CACHE)

if (VTE_FOUND)
  set(VTE ON CACHE BOOL "Build with VTE support.")
endif (VTE_FOUND)

# if (VTE)
#   add_definitions(-DVTE)
# endif (VTE)
