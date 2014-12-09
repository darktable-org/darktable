SET(OSMGPSMAP_FIND_REQUIRED ${OSMGpsMap_FIND_REQUIRED})

include(Prebuilt)

include(LibFindMacros)
libfind_pkg_check_modules(OSMGPSMAP osmgpsmap-1.0)
foreach(i ${OSMGPSMAP_LIBRARIES})
  find_library(_osmgpsmap_LIBRARY NAMES ${i} HINTS ${OSMGPSMAP_LIBRARY_DIRS})
  LIST(APPEND OSMGPSMAP_LIBRARY ${_osmgpsmap_LIBRARY})
  unset(_osmgpsmap_LIBRARY CACHE)
endforeach(i)
set(OSMGPSMAP_LIBRARIES ${OSMGPSMAP_LIBRARY})
unset(OSMGPSMAP_LIBRARY CACHE)

if (OSMGPSMAP_FOUND)
  set(OSMGPSMAP ON CACHE BOOL "Build with libosmgpsmap support.")
endif (OSMGPSMAP_FOUND)
