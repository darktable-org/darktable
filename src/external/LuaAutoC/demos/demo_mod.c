#include <stdio.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"
#include "lautoc.h"

static float add_numbers(int first, float second) {
  return first + second;
}

static void hello_world(char* person) {
  printf("Hello %s!", person);
}

static int autocall(lua_State* L) {
  return luaA_call_name(L, lua_tostring(L, 1));
}

int main(int argc, char **argv) {
	
  lua_State* L = luaL_newstate();
  luaA_open();
  
  luaA_function(L, add_numbers, float, 2, int, float);
  luaA_function_void(L, hello_world, 1, char*);
  
  lua_pushcfunction(L, autocall);
  lua_setglobal(L, "autocall");
  
  luaL_dostring(L, "autocall(\"add_numbers\", 1, 5.2)");
  luaL_dostring(L, "autocall(\"hello_world\", \"Daniel\")");
  
  luaA_close();
  lua_close(L);
	
  return 0;
}