#include <stdio.h>
#include <stdlib.h>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "lautoc.h"

typedef struct {
  char* name;
  int num_wings;
} birdie;

static birdie test_birdie;
static birdie* get_instance_ptr(lua_State* L) {
  return &test_birdie;
}

static int birdie_index(lua_State* L) {
  const char* membername = lua_tostring(L, -1);
  birdie* self = get_instance_ptr(L);
  return luaA_struct_push_member_name(L, birdie, self, membername);
}

static int birdie_newindex(lua_State* L) {
  const char* membername = lua_tostring(L, -2);
  birdie* self = get_instance_ptr(L);
  luaA_struct_to_member_name(L, membername, self, membername, -1);
  //luaA_struct_to_member_name(L, birdie, self, membername, -1);
  return 0;
}

int main(int argc, char **argv) {
  
  test_birdie.name = "MrFlingly";
  test_birdie.num_wings = 2;
  
  lua_State* L = luaL_newstate();
  luaL_openlibs(L);
  luaA_open();
  
  luaA_struct(L, birdie);
  luaA_struct_member(L, birdie, name, char*);
  luaA_struct_member(L, birdie, num_wings, int);
  
  lua_pushcfunction(L, birdie_index);
  lua_setglobal(L, "birdie_index");
  
  lua_pushcfunction(L, birdie_newindex);
  lua_setglobal(L, "birdie_newindex");
  
  luaL_dostring(L, ""
    "Birdie = {}\n"
    "setmetatable(Birdie, Birdie)\n"
    "Birdie.__index = birdie_index\n"
    "Birdie.__newindex = birdie_newindex\n"
    "function Birdie.__call()\n"
    "  local self = {}\n"
    "  setmetatable(self, Birdie)\n"
    "  return self\n"
    "end\n"
    "\n"
    "bird = Birdie()\n"
    "print(bird.name)\n"
    "print(bird.num_wings)\n"
    "\n"
    );
  
  luaA_close();
  lua_close(L);
  
  return 0;
  
}