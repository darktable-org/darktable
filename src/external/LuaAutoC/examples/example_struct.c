#include "../lautoc.h"

typedef struct {
  float x, y, z;
} vec3;

int main(int argc, char **argv) {
	
  lua_State* L = luaL_newstate();
  
  luaA_open(L);
  luaA_struct(L, vec3);
  luaA_struct_member(L, vec3, x, float);
  luaA_struct_member(L, vec3, y, float);
  luaA_struct_member(L, vec3, z, float);
  
  vec3 pos = {1.0f, 2.11f, 3.16f};

  luaA_struct_push_member(L, vec3, x, &pos);
  printf("x: %f\n", lua_tonumber(L, -1));
  lua_pop(L, 1);

  lua_pushnumber(L, 0.0);
  luaA_struct_to_member(L, vec3, x, &pos, -1);
  lua_pop(L, 1);

  luaA_struct_push_member(L, vec3, x, &pos);
  printf("x: %f\n", lua_tonumber(L, -1));
  lua_pop(L, 1);

  luaA_close(L);
  lua_close(L);
	
  return 0;
}
