#include "../lautoc.h"

typedef struct {
  char* name;
  int num_wings;
} birdie;

birdie test_birdie;

birdie* get_instance_ptr(lua_State* L) {
  return &test_birdie;
}

int birdie_index(lua_State* L) {
  const char* membername = lua_tostring(L, -1);
  birdie* self = get_instance_ptr(L);
  return luaA_struct_push_member_name(L, birdie, membername, self);
}

int birdie_newindex(lua_State* L) {
  const char* membername = lua_tostring(L, -2);
  birdie* self = get_instance_ptr(L);
  luaA_struct_to_member_name(L, birdie, membername, self, -1);
  return 0;
}

int main(int argc, char **argv) {
  
  test_birdie.name = "MrFlingly";
  test_birdie.num_wings = 2;
  
  lua_State* L = luaL_newstate();
  luaL_openlibs(L);
  luaA_open(L);
  
  luaA_struct(L, birdie);
  luaA_struct_member(L, birdie, name, char*);
  luaA_struct_member(L, birdie, num_wings, int);
  
  lua_register(L, "birdie_index", birdie_index);
  lua_register(L, "birdie_newindex", birdie_newindex);
  
  luaL_dostring(L, ""
    "Birdie = {}\n"
    "setmetatable(Birdie, Birdie)\n"
    "function Birdie.__call()\n"
    "  local self = {}\n"
    "  setmetatable(self, Birdie)\n"
    "  return self\n"
    "end\n"
    "Birdie.__index = birdie_index\n"
    "Birdie.__newindex = birdie_newindex\n"
    "\n"
    "bird = Birdie()\n"
    "print(bird.name)\n"
    "print(bird.num_wings)\n"
    "bird.num_wings = 3\n"
    "print(bird.num_wings)\n"
    "\n");
  
  luaA_close(L);
  lua_close(L);
  
  return 0;
  
}
