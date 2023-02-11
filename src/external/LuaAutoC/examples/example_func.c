#include "../lautoc.h"

int fib(int n) {
  if (n == 0) { return 1; }
  if (n == 1) { return 1; }
  return fib(n-1) + fib(n-2);
}

int main(int argc, char** argv) {

  /* Init Lua & LuaAutoC */
  lua_State* L = luaL_newstate();
  luaA_open(L);

  /* Register `fib` function */
  luaA_function(L, fib, int, int);

  /* Push integer onto stack and call `fib` */
  lua_pushinteger(L, 25);
  luaA_call(L, fib);

  /* Print result & pop */
  printf("Result: %i\n", (int)lua_tointeger(L, -1));
  lua_pop(L, 1);

  luaA_close(L);
  lua_close(L);

  return 0;
}
