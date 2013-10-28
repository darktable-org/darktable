cmake_minimum_required(VERSION 2.6)

set(LUA_SOURCES
	"src/lapi.c"
	"src/lauxlib.c"
	"src/lbaselib.c"
	"src/lbitlib.c"
	"src/lcode.c"
	"src/lcorolib.c"
	"src/lctype.c"
	"src/ldblib.c"
	"src/ldebug.c"
	"src/ldo.c"
	"src/ldump.c"
	"src/lfunc.c"
	"src/lgc.c"
	"src/linit.c"
	"src/liolib.c"
	"src/llex.c"
	"src/lmathlib.c"
	"src/lmem.c"
	"src/loadlib.c"
	"src/lobject.c"
	"src/lopcodes.c"
	"src/loslib.c"
	"src/lparser.c"
	"src/lstate.c"
	"src/lstring.c"
	"src/lstrlib.c"
	"src/ltable.c"
	"src/ltablib.c"
	"src/ltm.c"
	#"src/lua.c"
	#"src/luac.c"
	"src/lundump.c"
	"src/lvm.c"
	"src/lzio.c"
    )
add_library(lua STATIC ${LUA_SOURCES})
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -Wall -DLUA_COMPAT_ALL")
if(NOT WIN32)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
endif(NOT WIN32)
if(WIN32)
  target_link_libraries(lua)
elseif(APPLE)
  add_definitions("-DLUA_USE_MACOSX")
  target_link_libraries(lua)
else(APPLE)
  add_definitions("-DLUA_USE_LINUX")
  target_link_libraries(lua dl)
endif(WIN32)

# 
# SYSLIBS="-Wl,-E -ldl -lreadline"
# SYSLIBS="-lreadline"


