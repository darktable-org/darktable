#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lautoc.h"

typedef struct {
  luaA_Type type;
  size_t offset;
  char* name;
} struct_member_entry;

typedef struct {
  luaA_Type type_id;
  int num_members;
  int num_reserved_members;
  struct_member_entry** members;
} struct_entry;

static luaA_Hashtable* struct_table = NULL;

void luaA_struct_open(void) {
  struct_table = luaA_hashtable_new(256);
}

static void struct_entry_delete(struct_entry* se) {

  for(int i = 0; i < se->num_members; i++) {
    free(se->members[i]->name);
    free(se->members[i]);
  }
  
  free(se->members);
  free(se);
  
}

void luaA_struct_close(void) {

  luaA_hashtable_map(struct_table, (void(*)(void*))struct_entry_delete);
  luaA_hashtable_delete(struct_table);

}

int luaA_struct_push_member_offset_typeid(lua_State* L, luaA_Type type,const void* cstruct, size_t offset) {

  struct_entry* se = luaA_hashtable_get(struct_table, luaA_type_name(type));
  if (se != NULL) {
  
    for(int j = 0; j < se->num_members; j++) {
    if (se->members[j]->offset == offset) {
      struct_member_entry* sme = se->members[j];
      return luaA_push_typeid(L, sme->type, cstruct+sme->offset);
    }
    }
    
    lua_pushfstring(L, "luaA_struct_push_member: Member offset '%i' not registered for struct '%s'!", offset, luaA_type_name(type));
    lua_error(L);  
  }
  
  lua_pushfstring(L, "luaA_struct_push_member: Struct '%s' not registered!", luaA_type_name(type));
  lua_error(L);
  return 0;
}

int luaA_struct_push_member_name_typeid(lua_State* L, luaA_Type type,const void* cstruct, const char* member) {

  struct_entry* se = luaA_hashtable_get(struct_table, luaA_type_name(type));
  if (se != NULL) {
  
    for(int j = 0; j < se->num_members; j++) {
    if (strcmp(se->members[j]->name, member) == 0) {
      struct_member_entry* sme = se->members[j];
      return luaA_push_typeid(L, sme->type, cstruct+sme->offset);
    }
    }
    
    lua_pushfstring(L, "luaA_struct_push_member_name: Member '%s' not registered for struct '%s'!", member, luaA_type_name(type));
    lua_error(L);  
  }
  
  lua_pushfstring(L, "luaA_struct_push_member_name: Struct '%s' not registered!", luaA_type_name(type));
  lua_error(L);
  return 0;
}

void luaA_struct_to_member_offset_typeid(lua_State* L, luaA_Type type, void* cstruct, size_t offset, int index) {

  struct_entry* se = luaA_hashtable_get(struct_table, luaA_type_name(type));
  if (se != NULL) {
    
    for(int j = 0; j < se->num_members; j++) {
    if (se->members[j]->offset == offset) {
      struct_member_entry* sme = se->members[j];
      return luaA_to_typeid(L, sme->type, cstruct+sme->offset, index);
    }
    }
    
    lua_pushfstring(L, "luaA_struct_to_member: Member offset '%i' not registered for struct '%s'!", offset, luaA_type_name(type));
    lua_error(L);  
  }
  
  lua_pushfstring(L, "luaA_struct_to_member: Struct '%s' not registered!", luaA_type_name(type));
  lua_error(L);
}

void luaA_struct_to_member_name_typeid(lua_State* L, luaA_Type type, void* cstruct, const char* member, int index) {

  struct_entry* se = luaA_hashtable_get(struct_table, luaA_type_name(type));
  if (se != NULL) {
    
    for(int j = 0; j < se->num_members; j++) {
    if (strcmp(se->members[j]->name, member) == 0) {
      struct_member_entry* sme = se->members[j];
      return luaA_to_typeid(L, sme->type, cstruct+sme->offset, index);
    }
    }
    
    lua_pushfstring(L, "luaA_struct_to_member_name: Member '%s' not registered for struct '%s'!", member, luaA_type_name(type));
    lua_error(L);  
  }
  
  lua_pushfstring(L, "luaA_struct_to_member_name: Struct '%s' not registered!", luaA_type_name(type));
  lua_error(L);
}

bool luaA_struct_has_member_offset_typeid(lua_State* L, luaA_Type type, size_t offset) {

  struct_entry* se = luaA_hashtable_get(struct_table, luaA_type_name(type));
  if (se != NULL) {
    for(int j = 0; j < se->num_members; j++) {
      if (se->members[j]->offset == offset) { return true; }
    }
    return false;
  }
  
  lua_pushfstring(L, "lua_autostruct: Struct '%s' not registered!", luaA_type_name(type));
  lua_error(L);
  return false;
}

bool luaA_struct_has_member_name_typeid(lua_State* L, luaA_Type type, const char* member) {

  struct_entry* se = luaA_hashtable_get(struct_table, luaA_type_name(type));
  if (se != NULL) {
    for(int j = 0; j < se->num_members; j++) {
      if (strcmp(se->members[j]->name, member) == 0) { return true; }
    }
    return false;
  }
  
  lua_pushfstring(L, "lua_autostruct: Struct '%s' not registered!", luaA_type_name(type));
  lua_error(L);
  return false;
}


const char* luaA_struct_next_member_name_typeid(lua_State* L, luaA_Type type, const char* member) {

  struct_entry* se = luaA_hashtable_get(struct_table, luaA_type_name(type));
  if (se != NULL) {
    if( se->num_members == 0) {
	    return NULL;
    }

    if(member == NULL && se->num_members > 0) {
	    return se->members[0]->name;
    }
    for(int j = 0; j < se->num_members; j++) {
      if (strcmp(se->members[j]->name, member) == 0) { 
	j++;
	if(j == se->num_members) return NULL;
	return se->members[j]->name;
      }
    }
    lua_pushfstring(L, "luaA_struct_to_member_name: Member '%s' not registered for struct '%s'!", member, luaA_type_name(type));
    lua_error(L);  
    return NULL;
  }
  
  lua_pushfstring(L, "lua_autostruct: Struct '%s' not registered!", luaA_type_name(type));
  lua_error(L);
  return NULL;
}

luaA_Type luaA_struct_typeof_member_offset_typeid(lua_State* L, luaA_Type type,  size_t offset) {

  struct_entry* se = luaA_hashtable_get(struct_table, luaA_type_name(type));
  if (se != NULL) {
    
    for(int j = 0; j < se->num_members; j++) {
    if (se->members[j]->offset == offset) {
      struct_member_entry* sme = se->members[j];
      return  sme->type;
    }
    }
    
    lua_pushfstring(L, "luaA_struct_typeof_member: Member offset '%i' not registered for struct '%s'!", offset, luaA_type_name(type));
    return lua_error(L);  
  }
  
  lua_pushfstring(L, "luaA_struct_typeof_member: Struct '%s' not registered!", luaA_type_name(type));
  return lua_error(L);
}

luaA_Type luaA_struct_typeof_member_name_typeid(lua_State* L, luaA_Type type,  const char* member) {

  struct_entry* se = luaA_hashtable_get(struct_table, luaA_type_name(type));
  if (se != NULL) {
    
    for(int j = 0; j < se->num_members; j++) {
    if (strcmp(se->members[j]->name, member) == 0) {
      struct_member_entry* sme = se->members[j];
      return  sme->type;
    }
    }
    
    lua_pushfstring(L, "luaA_struct_typeof_member_name: Member '%s' not registered for struct '%s'!", member, luaA_type_name(type));
    return lua_error(L);  
  }
  
  lua_pushfstring(L, "luaA_struct_typeof_member_name: Struct '%s' not registered!", luaA_type_name(type));
  return lua_error(L);
}
void luaA_struct_typeid(lua_State* L, luaA_Type type) {

  struct_entry* se = malloc(sizeof(struct_entry));
  se->type_id = type;
  se->num_members = 0;
  se->num_reserved_members = 32;
  se->members = malloc(sizeof(struct_member_entry*) * se->num_reserved_members);
  
  luaA_hashtable_set(struct_table, luaA_type_name(type), se);

}

void luaA_struct_member_typeid(lua_State* L, luaA_Type type, const char* member, luaA_Type member_type, size_t offset) {

  struct_entry* se = luaA_hashtable_get(struct_table, luaA_type_name(type));
  if (se != NULL) {
    
    if (se->num_members >= se->num_reserved_members) {
      se->num_reserved_members += 32;
      se->members = realloc(se->members, sizeof(struct_member_entry*) * se->num_reserved_members);
    }
    
    struct_member_entry* sme = malloc(sizeof(struct_member_entry));
    sme->type = member_type;
    sme->offset = offset;
    sme->name = malloc(strlen(member) + 1);
    strcpy(sme->name, member);
    
    se->members[se->num_members] = sme;
    se->num_members++;
    return;
    
  }
  
  lua_pushfstring(L, "lua_autostruct: Struct '%s' not registered!", luaA_type_name(type));
  lua_error(L);
}

bool luaA_struct_registered_typeid(lua_State* L, luaA_Type type) {

  struct_entry* se = luaA_hashtable_get(struct_table, luaA_type_name(type));
  if (se == NULL) { return false; } else { return true; }

}

int luaA_struct_push_typeid(lua_State* L, luaA_Type type,const void* c_in) {

  struct_entry* se = luaA_hashtable_get(struct_table, luaA_type_name(type));
  if (se != NULL) {
    
    lua_newtable(L);
    
    for(int j = 0; j < se->num_members; j++) {
      struct_member_entry* sme = se->members[j];
      luaA_struct_push_member_name_typeid(L, type, c_in, sme->name);
      lua_setfield(L, -2, sme->name);
    }
    return 1;
  }
  
  lua_pushfstring(L, "lua_autostruct: Struct '%s' not registered!", luaA_type_name(type));
  lua_error(L);
  return 0;
}

void luaA_struct_to_typeid(lua_State* L, luaA_Type type, void* c_out, int index) {

  struct_entry* se = luaA_hashtable_get(struct_table, luaA_type_name(type));
  if (se != NULL) {
    for(int j = 0; j < se->num_members; j++) {
      struct_member_entry* sme = se->members[j];
      lua_getfield(L, index, sme->name);
      luaA_struct_to_member_name_typeid(L, type, c_out, sme->name, index);
    }
    return;
  }
  
  lua_pushfstring(L, "lua_autostruct: Struct '%s' not registered!", luaA_type_name(type));
  lua_error(L);

}
