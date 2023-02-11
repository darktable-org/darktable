#include "../lautoc.h"

/* Hello Module Begin */

void hello_world(void) {
  puts("Hello World!");
}

void hello_repeat(int times) {
  for (int i = 0; i < times; i++) {
    hello_world();
  }
}

void hello_person(const char* person) {
  printf("Hello %s!\n", person);
}

int hello_subcount(const char* greeting) {
  int count = 0;
  const char *tmp = greeting;
  while((tmp = strstr(tmp, "hello"))) {
    count++; tmp++;
  }
  return count;
}

/* Hello Module End */

int C(lua_State* L) {
  return luaA_call_name(L, lua_tostring(L, 1));
}

int main(int argc, char **argv) {
	
  lua_State* L = luaL_newstate();

  luaA_open(L);
  luaA_function(L, hello_world, void);
  luaA_function(L, hello_repeat, void, int);
  luaA_function(L, hello_person, void, const char*);
  luaA_function(L, hello_subcount, int, const char*);

  lua_register(L, "C", C);

  luaL_dostring(L,
    "C('hello_world')\n"
    "C('hello_person', 'Daniel')\n"
    "C('hello_repeat', C('hello_subcount', 'hello hello'))\n"
  );

  luaA_close(L);
  lua_close(L);
	
  return 0;
}
