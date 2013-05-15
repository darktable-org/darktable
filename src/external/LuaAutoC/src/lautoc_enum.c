#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "lua.h"
#include "lauxlib.h"
#include "lautoc.h"

typedef struct enum_entry{
  void* value;
  bool case_sensitive;
  char* name;
  struct enum_entry*next;
} enum_entry;

typedef struct enum_type{
  luaA_Type type;
  size_t size;
  struct enum_entry*first;
} enum_type;

static luaA_Hashtable* enum_table = NULL;

void luaA_enum_open(void) {
  enum_table = luaA_hashtable_new(256);
}

static void enum_entry_delete(enum_entry* ee) {
  if(!ee) return;
  enum_entry_delete(ee->next);
  free(ee->name);
  free(ee->value);
  free(ee);
  
}

static void enum_type_delete(enum_type* et) {
  if(!et) return;
  enum_entry_delete(et->first);
  free(et);
  
}

void luaA_enum_close(void) {

  luaA_hashtable_map(enum_table, (void(*)(void*))enum_type_delete);
  luaA_hashtable_delete(enum_table);

}


int luaA_enum_push_typeid(lua_State *L, luaA_Type type, const void *  cin){

  enum_type* et = luaA_hashtable_get(enum_table, luaA_type_name(type));
  if (et != NULL) {
    enum_entry * ee = et->first;
    while(ee) {
      if(!memcmp(cin,ee->value,et->size)){
	lua_pushstring(L,ee->name);
	return 1;
      }
      ee = ee->next;
    }
  
    
    lua_pushfstring(L, "luaA_enum_push_value: value '%d' not registered for enum '%s'!", *(int*)cin, luaA_type_name(type));
    lua_error(L);  
  }
  
  lua_pushfstring(L, "luaA_enum_push_value: Enum '%s' not registered!", luaA_type_name(type));
  lua_error(L);
  return 0;
}


void luaA_enum_to_typeid(lua_State* L, luaA_Type type, void *c_in,int index) {

  enum_type* et = luaA_hashtable_get(enum_table, luaA_type_name(type));
  if(!lua_isstring(L,index) || lua_isnumber(L,index)) { // we don't want numbers here at all
	  lua_pushfstring(L,"lua_enum_to_value: incorrect value passed '%s'",luaL_tolstring(L,index,NULL));
	  lua_error(L);
  }
  const char* value = lua_tostring(L,index);

  if (et != NULL) {
    enum_entry * ee = et->first;
    while(ee) {
      if(ee->case_sensitive && !strcmp(ee->name, value)){
	memcpy(c_in,ee->value,et->size);
	return;
      } else if(!ee->case_sensitive && !strcasecmp(ee->name, value)){
	memcpy(c_in,ee->value,et->size);
	return;
      }
      ee = ee->next;
    }
  
    
    lua_pushfstring(L, "luaA_enum_to_value: name '%s' not registered for enum '%s'!", value, luaA_type_name(type));
    lua_error(L);  
  }
  
  lua_pushfstring(L, "luaA_enum_push_value: Enum '%s' not registered!", luaA_type_name(type));
  lua_error(L);
}

bool luaA_enum_has_value_typeid(lua_State* L, luaA_Type type, const void* value) {

  enum_type* et = luaA_hashtable_get(enum_table, luaA_type_name(type));
  if (et != NULL) {
    enum_entry * ee = et->first;
    while(ee) {
      if(memcmp(ee->value,value,et->size)) {
	return true;
      }
      ee = ee->next;
    }
    return false;
  }
  
  lua_pushfstring(L, "luaA_enum_has_value: Enum '%s' not registered!", luaA_type_name(type));
  lua_error(L);
  return false;
}


bool luaA_enum_has_name_typeid(lua_State* L, luaA_Type type, const char* name){

  enum_type* et = luaA_hashtable_get(enum_table, luaA_type_name(type));
  if (et != NULL) {
    enum_entry * ee = et->first;
    while(ee) {
      if(ee->case_sensitive && strcmp(ee->name, name)){
	return true;
      } else if(!ee->case_sensitive && strcasecmp(ee->name, name)){
	return true;
      }
      ee = ee->next;
    }
    return false;
  }
  
  lua_pushfstring(L, "luaA_enum_has_name: Enum '%s' not registered!", luaA_type_name(type));
  lua_error(L);
  return false;
}

void luaA_enum_typeid(lua_State *L,luaA_Type type,size_t size) {
  enum_type* et = malloc(sizeof(enum_type));
  et->type = type;
  et->first = NULL;
  et->size = size;
  luaA_hashtable_set(enum_table,luaA_type_name(type),et);
}
void luaA_enum_value_typeid_name(lua_State *L, luaA_Type type, const void* value, const char*value_name,bool case_sensitive){
  enum_type* et = luaA_hashtable_get(enum_table, luaA_type_name(type));
  if (et != NULL) {
    enum_entry* ee = malloc(sizeof(enum_entry));
    ee->value = malloc(et->size);
    memcpy(ee->value,value,et->size);
    ee->name = strdup(value_name);
    ee->case_sensitive = case_sensitive;
    ee->next =NULL;

    enum_entry* prev_se = et->first;
    if(prev_se == NULL) {
      et->first = ee;
    } else {
      while(prev_se->next) prev_se = prev_se->next;
      prev_se->next = ee;
    }
  }else {
    lua_pushfstring(L, "luaA_enum_value: Enum '%s' not registered!", luaA_type_name(type));
    lua_error(L);
  }
}

bool luaA_enum_registered_typeid(lua_State *L, luaA_Type type){

  enum_entry* ee = luaA_hashtable_get(enum_table, luaA_type_name(type));
  if (ee == NULL) { return false; } else { return true; }

}
