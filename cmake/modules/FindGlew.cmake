include(Prebuilt)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
  include(FindPkgConfig)
  pkg_check_modules(GLEW glew)
endif (CMAKE_SYSTEM_NAME STREQUAL "Linux")

if (GLEW_FOUND)
  set(GLEW ON CACHE BOOL "Build with GLEW support.")
endif (GLEW_FOUND)

# if (GLEW)
#   add_definitions(-DGLEW=1)
# endif (GLEW)
