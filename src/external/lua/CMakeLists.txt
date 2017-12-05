set(LUA_SOURCES
	"src/lapi.c"
	"src/lcode.c"
	"src/lctype.c"
	"src/ldebug.c"
	"src/ldo.c"
	"src/ldump.c"
	"src/lfunc.c"
	"src/lgc.c"
	"src/llex.c"
	"src/lmem.c"
	"src/lobject.c"
	"src/lopcodes.c"
	"src/lparser.c"
	"src/lstate.c"
	"src/lstring.c"
	"src/ltable.c"
	"src/ltm.c"
	"src/lundump.c"
	"src/lvm.c"
	"src/lzio.c"
	"src/lauxlib.c"
	"src/lbaselib.c"
	"src/lbitlib.c"
	"src/lcorolib.c"
	"src/ldblib.c"
	"src/liolib.c"
	"src/lmathlib.c"
	"src/loslib.c"
	"src/lstrlib.c"
	"src/ltablib.c"
	"src/lutf8lib.c"
	"src/loadlib.c"
	"src/linit.c"
	)
add_library(lua STATIC ${LUA_SOURCES})

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -Wall")

set_target_properties(lua
  PROPERTIES
    POSITION_INDEPENDENT_CODE True
)

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


