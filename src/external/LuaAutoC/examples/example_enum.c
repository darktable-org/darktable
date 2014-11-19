#include "../lautoc.h"

typedef enum {
  DIAMONDS,
  HEARTS,
  CLUBS,
  SPADES,
  INVALID = -1
} cards;

int main(int argc, char **argv) {
	
  lua_State* L = luaL_newstate();
  
  luaA_open(L);

  luaA_enum(L, cards);
  luaA_enum_value(L, cards, DIAMONDS);
  luaA_enum_value(L, cards, HEARTS);
  luaA_enum_value(L, cards, CLUBS);
  luaA_enum_value(L, cards, SPADES);
  luaA_enum_value(L, cards, INVALID);

  cards cval = SPADES;
  const char* lval = "SPADES";
  
  luaA_push(L, cards, &cval);
  printf("%i pushed as %s\n", cval, lua_tostring(L, -1));
  lua_pop(L, 1);
  
  lua_pushstring(L, lval);
  luaA_to(L, cards, &cval, -1);
  printf("%s read back as %i\n", lval, cval); 
  lua_pop(L, 1);
  
  luaA_close(L);
  lua_close(L);
	
  return 0;
}
