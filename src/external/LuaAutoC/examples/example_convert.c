#include "../lautoc.h"

typedef struct {
  int fst, snd;
} pair;

static int luaA_push_pair(lua_State* L, luaA_Type t, const void* c_in) {
  pair* p = (pair*)c_in;
  lua_pushinteger(L, p->fst);
  lua_pushinteger(L, p->snd);
  return 2;
}

static void luaA_to_pair(lua_State* L, luaA_Type t, void* c_out, int index) {
  pair* p = (pair*)c_out;
  p->snd = lua_tointeger(L, index);
  p->fst = lua_tointeger(L, index-1);
}

typedef struct {
  int id;
  int legs;
  float height;
} table;

int main(int argc, char **argv) {
	
  lua_State* L = luaL_newstate();
  luaA_open(L);
	
  luaA_conversion(L, pair, luaA_push_pair, luaA_to_pair);
	
  pair p = {20, 10};
  luaA_push(L, pair, &p);
  lua_pop(L, 2);
  
  luaA_struct(L, table);
  luaA_struct_member(L, table, id, int);
  luaA_struct_member(L, table, legs, int);
  luaA_struct_member(L, table, height, float);
  
  table t = {0, 4, 0.72};

  luaA_push(L, table, &t);

  lua_getfield(L, -1, "legs");
  printf("legs: %i\n", (int)lua_tointeger(L, -1));
  lua_pop(L, 1);

  lua_getfield(L, -1, "height");
  printf("height: %f\n", lua_tonumber(L, -1));
  lua_pop(L, 1);

  lua_pop(L, 1);
  
  luaA_close(L);
  lua_close(L);
	
  return 0;
}
