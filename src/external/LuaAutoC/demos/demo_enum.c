#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lautoc.h"

typedef enum {
	case_sensitive,
	case_insensitive,
	not_contiguous =45,
} enum_val;

int main(int argc, char **argv) {
	
  lua_State* L = luaL_newstate();
  luaA_open();
	
  luaA_enum(L,enum_val);
  luaA_enum_value(L,enum_val,case_sensitive,true);
  luaA_enum_value(L,enum_val,case_insensitive,false);
  luaA_enum_value(L,enum_val,not_contiguous,false);
  luaA_enum_value_name(L,enum_val,case_sensitive,"alias_sensitive",true);

  enum_val test_enum = not_contiguous;
  luaA_push(L,enum_val,&test_enum);
  printf("not_contiguous pushed as %s\n",lua_tostring(L,-1));

  lua_pushstring(L,"alias_sensitive");
  luaA_to(L,enum_val,&test_enum,-1);
  printf("alias_sensitive read back as %d\n",test_enum); 

  luaA_close();
  lua_close(L);
	
  return 0;
}
