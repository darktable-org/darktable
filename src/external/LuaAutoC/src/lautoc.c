#include "lautoc.h"

void luaA_open(void) {
  luaA_type_open();
  luaA_stack_open();
  luaA_struct_open();
  luaA_call_open();
}

void luaA_close(void) {
  luaA_call_close();
  luaA_struct_close();
  luaA_stack_close();
  luaA_type_close();
}