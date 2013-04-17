#include <stdio.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"
#include "lautoc.h"

typedef struct {
  int x, y;
} pair;

static int luaA_push_pair(lua_State* L, luaA_Type t, const void* c_in) {
  pair p = *(pair*)c_in;
  lua_pushinteger(L, p.x);
  lua_pushinteger(L, p.y);
  return 2;
}

static void luaA_to_pair(lua_State* L, luaA_Type t, void* c_out, int index) {
  pair* p = (pair*)c_out;
  p->y = lua_tointeger(L, index);
  p->x = lua_tointeger(L, index-1);
}

typedef struct {
  char* first_name;
  char* second_name;
  float coolness;
} person_details;

int main(int argc, char **argv) {
	
  lua_State* L = luaL_newstate();
  luaA_open();
	
  luaA_conversion(pair, luaA_push_pair, luaA_to_pair);
	
  luaA_struct(L, person_details);
  luaA_struct_member(L, person_details, first_name, char*);
  luaA_struct_member(L, person_details, second_name, char*);
  luaA_struct_member(L, person_details, coolness, float);

  pair p = {1, 2};
  person_details my_details = {"Daniel", "Holden", 125212.213};
  
  luaA_push(L, pair, &p);
  printf("Pair: (%s, %s)\n", lua_tostring(L, -2), lua_tostring(L, -1));
  lua_pop(L, 2);
  
  luaA_push(L, person_details, &my_details);
  
  lua_getfield(L, -1, "first_name");
  printf("First Name: %s\n", lua_tostring(L, -1));
  lua_pop(L, 1);
  
  lua_getfield(L, -1, "second_name");
  printf("Second Name: %s\n", lua_tostring(L, -1));
  lua_pop(L, 1);
  
  lua_pop(L, 1);
  
  luaA_close();
  lua_close(L);
	
  return 0;
}
