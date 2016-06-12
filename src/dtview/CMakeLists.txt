cmake_minimum_required(VERSION 2.8.5)

if(USE_LUA)
	if(LUA52_FOUND)
		include_directories(SYSTEM ${LUA52_INCLUDE_DIRS})
	else()
		include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../external/lua/src)
	endif(LUA52_FOUND)
	# liblautoc for lua automated interface generation
	include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../external/LuaAutoC/include)
endif(USE_LUA)

include_directories(SYSTEM ${SDL_INCLUDE_DIR})
include_directories(SYSTEM ${OPENGL_INCLUDE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/..)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/..)
add_executable(darktable-viewer main.c)

set_target_properties(darktable-viewer PROPERTIES CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set_target_properties(darktable-viewer PROPERTIES CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)
set_target_properties(darktable-viewer PROPERTIES INSTALL_RPATH ${RPATH_DT}/../${CMAKE_INSTALL_LIBDIR}/darktable)
set_target_properties(darktable-viewer PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(darktable-viewer lib_darktable ${SDL_LIBRARY} ${OPENGL_LIBRARIES})
install(TARGETS darktable-viewer DESTINATION ${CMAKE_INSTALL_BINDIR})
