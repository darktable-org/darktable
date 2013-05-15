#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lautoc.h"

typedef struct {
  float x, y, z;
} vector3;

int main(int argc, char **argv) {
	
  lua_State* L = luaL_newstate();
  luaA_open();
	
  luaA_struct(L, vector3);
  luaA_struct_member(L, vector3, x, float);
  luaA_struct_member(L, vector3, y, float);
  luaA_struct_member(L, vector3, z, float);
  
  vector3 position = {1.0f, 2.11f, 3.16f};
  
  luaA_struct_push_member(L, vector3, &position, y);
  
  printf("Y: %f\n", lua_tonumber(L, -1));
  
  lua_pop(L, 1);
  
  luaA_close();
  lua_close(L);
	
  return 0;
}
