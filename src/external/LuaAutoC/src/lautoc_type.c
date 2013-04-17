#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "lautoc.h"

typedef char* type_name;
typedef size_t type_size;

static type_name* type_names;
static type_size* type_sizes;

static int num_types = 0;
static int num_reserved_types = 128;

void luaA_type_open(void) {
  
  type_names = malloc(sizeof(type_name) * num_reserved_types);
  type_sizes = malloc(sizeof(type_size) * num_reserved_types);
}

void luaA_type_close(void) {
  
  for(int i = 0; i < num_types; i++) {
    free(type_names[i]);
  }
  
  free(type_names);
  free(type_sizes);
}

luaA_Type luaA_type_add(const char* type, size_t size) {
  
  for(int i = 0; i < num_types; i++) {
    if (strcmp(type, type_names[i]) == 0) return i;
  }
  
  if (num_types >= num_reserved_types) {
    num_reserved_types += 128;
    type_names = realloc(type_names, sizeof(type_name) * num_reserved_types);
    type_sizes = realloc(type_sizes, sizeof(type_size) * num_reserved_types);
  }
  
  type_names[num_types] = malloc(strlen(type)+1);
  strcpy(type_names[num_types], type);
  type_sizes[num_types] = size;
  num_types++;
  
  return num_types-1;
}

luaA_Type luaA_type_find(const char* type) {

  for(int i = 0; i < num_types; i++) {
    if (strcmp(type, type_names[i]) == 0) return i;
  }
  
  return -1;
}

const char* luaA_type_name(luaA_Type id) {
  if (id == -1) return "Unknown Type";
  return type_names[id];
}

size_t luaA_type_size(luaA_Type id) {
  if (id == -1) return -1;
  return type_sizes[id];
}
