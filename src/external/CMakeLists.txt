set(BUILD_TESTING_SAVE "${BUILD_TESTING}")

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(WITH_OPENMP ${USE_OPENMP} CACHE BOOL "" FORCE)

set(RAWSPEED_PATH "${CMAKE_CURRENT_SOURCE_DIR}/rawspeed")

if(NOT(EXISTS "${RAWSPEED_PATH}" AND IS_DIRECTORY "${RAWSPEED_PATH}" AND EXISTS "${RAWSPEED_PATH}/CMakeLists.txt"))
  message(FATAL_ERROR "RawSpeed submodule not found. You probably want to run:\n$ git submodule init\nand then\n$ git submodule update")
endif()

set(CMAKE_C_FLAGS_SAVE "${CMAKE_C_FLAGS}")
set(CMAKE_CXX_FLAGS_SAVE "${CMAKE_CXX_FLAGS}")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -w")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w")

set(CMAKE_CXX_CLANG_TIDY_SAVE "${CMAKE_CXX_CLANG_TIDY}")
set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE_SAVE "${CMAKE_CXX_INCLUDE_WHAT_YOU_USE}")

unset(CMAKE_CXX_CLANG_TIDY)
unset(CMAKE_CXX_INCLUDE_WHAT_YOU_USE)

add_subdirectory(rawspeed)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS_SAVE}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS_SAVE}")

set(CMAKE_CXX_CLANG_TIDY "${CMAKE_CXX_CLANG_TIDY_SAVE}")
set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE "${CMAKE_CXX_INCLUDE_WHAT_YOU_USE_SAVE}")

#
# the rawspeed part is a bit of a hack:
# the static linking didn't work since it was pulling -lstdc++ and -lm into linker flags.
# so we do a custom dependency and pretend an imported librawsped.a so no other -l are
# appended.
#
add_library(rawspeed_static_dt STATIC IMPORTED GLOBAL)
add_dependencies(rawspeed_static_dt rawspeed_static rawspeed)
set_target_properties(rawspeed_static_dt
  PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/rawspeed/src/librawspeed/librawspeed_static.a
    INTERFACE_INCLUDE_DIRECTORIES $<TARGET_PROPERTY:rawspeed_static,INTERFACE_INCLUDE_DIRECTORIES>
    INTERFACE_LINK_LIBRARIES $<TARGET_PROPERTY:rawspeed_static,INTERFACE_LINK_LIBRARIES>
    POSITION_INDEPENDENT_CODE True
)

set(BUILD_TESTING "${BUILD_TESTING_SAVE}" CACHE BOOL "" FORCE)
