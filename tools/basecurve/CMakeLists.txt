cmake_minimum_required(VERSION 2.8.5)
include(CheckCCompilerFlag)
include(GNUInstallDirs)

set(CC_FLAGS "-g -Wall")
set(CMAKE_C_FLAGS "${CC_FLAGS} -std=c99")
check_c_compiler_flag(-pedantic CMAKE_C_COMPILER_ARG_PEDANTIC)
if(CMAKE_C_COMPILER_ARG_PEDANTIC)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pedantic")
endif(CMAKE_C_COMPILER_ARG_PEDANTIC)
set(CMAKE_C_FLAGS_DEBUG "-O0")
set(CMAKE_C_FLAGS_RELEASE "-O3")
set(CMAKE_CXX_FLAGS "${CC_FLAGS}")
set(CMAKE_CXX_FLAGS_DEBUG "-O0")
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

find_package(OpenMP)
if (OPENMP_FOUND)
    set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS}")
    set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")
endif()

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/modules")
find_library(M_LIBRARY m)
find_package(Exiv2 REQUIRED)
include_directories(${EXIV2_INCLUDE_DIR})

add_executable(dt-curve-tool dt-curve-tool.c exif-wrapper.cpp)
target_link_libraries(dt-curve-tool ${EXIV2_LIBRARY} ${M_LIBRARY})
install(TARGETS dt-curve-tool RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/dt-curve-tool-helper
                   COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/dt-curve-tool-helper ${CMAKE_CURRENT_BINARY_DIR}/dt-curve-tool-helper
                   DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/dt-curve-tool-helper
                   COMMENT "Copying dt-curve-tool-helper")
add_custom_target(copy_dt-curve-tool-helper ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/dt-curve-tool-helper)
install(FILES
          ${CMAKE_CURRENT_SOURCE_DIR}/dt-curve-tool-helper
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        PERMISSIONS
          OWNER_EXECUTE OWNER_WRITE OWNER_READ
          GROUP_EXECUTE GROUP_READ
          WORLD_EXECUTE WORLD_READ)
