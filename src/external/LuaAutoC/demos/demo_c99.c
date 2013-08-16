#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lautoc.h"

static float add_numbers(int first, float second) {
  return first + second;
}

luaA_function_decl(add_numbers, float, 2, int, float);

int main(int argc, char **argv) {
  
  lua_State* L = luaL_newstate();
  luaA_open();
  
  luaA_function_reg(L, add_numbers, float, 2, int, float);
  
  lua_pushnumber(L, 6.13);
  lua_pushinteger(L, 5);
  luaA_call(L, add_numbers);
  
  printf("Result: %f\n", lua_tonumber(L, -1));
  
  lua_settop(L, 0);
  
  luaA_close();
  lua_close(L);
  
  return 0;
}