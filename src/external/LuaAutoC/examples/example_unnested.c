#include "../lautoc.h"

int fib(int n) {
  if (n == 0) { return 1; }
  if (n == 1) { return 1; }
  return fib(n-1) + fib(n-2);
}

luaA_function_declare(fib, int, int);

int main(int argc, char** argv) {

  lua_State* L = luaL_newstate();
  luaA_open(L);

  luaA_function_register(L, fib, int, int);

  lua_pushinteger(L, 25);
  luaA_call(L, fib);

  printf("Result: %i\n", (int)lua_tointeger(L, -1));
  lua_pop(L, 1);

  luaA_close(L);
  lua_close(L);

  return 0;
}
