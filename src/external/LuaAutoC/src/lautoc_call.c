#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "lautoc.h"

#define MAX_ARG_NUM 10

typedef struct {
  char* name;
  void* src_func;
  luaA_Func auto_func;
  luaA_Type ret_type;
  int num_args;
  luaA_Type arg_types[MAX_ARG_NUM];
} func_entry;

static luaA_Hashtable* func_ptr_table;
static luaA_Hashtable* func_name_table;

void luaA_call_open(void) {
  func_ptr_table = luaA_hashtable_new(1024);
  func_name_table = luaA_hashtable_new(1024);
}

static void func_entry_delete(func_entry* fe) {
  free(fe->name);
  free(fe);
}

void luaA_call_close(void) {
  luaA_hashtable_map(func_ptr_table, (void(*)(void*))func_entry_delete);
  
  luaA_hashtable_delete(func_ptr_table);
  luaA_hashtable_delete(func_name_table);
}

#define RET_STACK_SIZE 128
#define ARG_STACK_SIZE 1024

static char ret_stack[RET_STACK_SIZE];
static void* ret_stack_ptr = ret_stack;

static char arg_stack[ARG_STACK_SIZE];
static void* arg_stack_ptr = arg_stack;

static int ret_stack_space() {
  return (void*)ret_stack - ret_stack_ptr + RET_STACK_SIZE;
}

static int arg_stack_space() {
  return (void*)arg_stack - arg_stack_ptr + ARG_STACK_SIZE;
}

static int total_arg_size(func_entry* fe) {
  
  int total = 0;
  for(int i = 0; i < fe->num_args; i++) {
    total += luaA_type_size(fe->arg_types[i]);
  }
  return total;
  
}

static int luaA_call_entry(lua_State* L, func_entry* fe) {
  
  int ret_data_size = luaA_type_size(fe->ret_type);
  int arg_data_size = total_arg_size(fe);
  
  int ret_using_heap = 0; int arg_using_heap = 0;
  void* ret_data = ret_stack_ptr;
  void* arg_data = arg_stack_ptr;
  
  if (ret_data_size > ret_stack_space()) {
    ret_using_heap = 1; ret_data = malloc(ret_data_size);
  }
  
  if (arg_data_size > arg_stack_space()) {
    arg_using_heap = 1; arg_data = malloc(arg_data_size);
  }
  
  /* Pop args in reverse order but place in memory in forward order */
  
  void* arg_top = arg_data + arg_data_size;
  
  for(int j = fe->num_args-1; j >= 0; j--) { 
    arg_top -= luaA_type_size(fe->arg_types[j]);
    luaA_to_typeid(L, fe->arg_types[j], arg_top, -j-1);
  }
  
  arg_data += arg_data_size;
  ret_data += ret_data_size;
  
  /* If not using heap update stack pointers */
  if (!ret_using_heap) { ret_stack_ptr = ret_data; }
  if (!arg_using_heap) { arg_stack_ptr = arg_data; }
  
  arg_data -= arg_data_size;
  ret_data -= ret_data_size;
  
  fe->auto_func(ret_data, arg_data);
  int count = luaA_push_typeid(L, fe->ret_type, ret_data);
  
  /* Either free heap data or reduce stack pointers */
  if (ret_using_heap) { free(ret_data); } else { ret_stack_ptr -= ret_data_size; } 
  if (arg_using_heap) { free(arg_data); } else { arg_stack_ptr -= arg_data_size; }
  
  return count;
}

int luaA_call(lua_State* L, void* func_ptr) {
  
  char ptr_string[128];
  sprintf(ptr_string, "%p", func_ptr);
  
  func_entry* fe = luaA_hashtable_get(func_ptr_table, ptr_string);
  if (fe != NULL) {
    return luaA_call_entry(L, fe);
  }
  
  lua_pushfstring(L, "luaA_call: Function with address '%p' is not registered!", func_ptr);
  lua_error(L);
  return 0;
}

int luaA_call_name(lua_State* L, const char* func_name) {
  
  func_entry* fe = luaA_hashtable_get(func_name_table, func_name);
  if (fe != NULL) {
    return luaA_call_entry(L, fe);
  }
  
  lua_pushfstring(L, "luaA_call_name: Function '%s' is not registered!", func_name);
  lua_error(L);
  return 0;
}

void luaA_function_typeid(lua_State* L, void* src_func, luaA_Func auto_func, char* name, luaA_Type ret_t, int num_args, ...) {

  if (num_args >= MAX_ARG_NUM) {
    lua_pushfstring(L, "luaA_func_add: Function has %i arguments - maximum supported is %i!", num_args, MAX_ARG_NUM);
    lua_error(L);  
  }
  
  func_entry* fe = malloc(sizeof(func_entry));
  
  fe->name = malloc(strlen(name) + 1);
  strcpy(fe->name, name);
  
  fe->src_func = src_func;
  fe->auto_func = auto_func;
  
  fe->ret_type = ret_t;
  fe->num_args = num_args;
  
  va_list argl;
  va_start(argl, num_args);
  for(int i = 0; i < num_args; i++) {
    fe->arg_types[i] = va_arg(argl, luaA_Type);
  }
  
  char ptr_string[128];
  sprintf(ptr_string, "%p", src_func);
  
  luaA_hashtable_set(func_name_table, name, fe);
  luaA_hashtable_set(func_ptr_table, ptr_string, fe);

}