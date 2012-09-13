#include <stdlib.h>
#include <string.h>

#include "lautoc.h"

static int hash(const char* s, int size) {
  int h = 0;
  while (*s) h = h * 101 + *s++;
  return abs(h) % size;
}

static luaA_Bucket* luaA_bucket_new(const char* string, void* item) {
  
  luaA_Bucket* b = malloc(sizeof(luaA_Bucket));
  b->item = item;
  
  b->string = malloc(strlen(string) + 1);
  strcpy(b->string, string);
  
  b->next = NULL;
  b->prev = NULL;
  
  return b;
}

static void luaA_bucket_delete(luaA_Bucket* b) {
  if(b->next != NULL) { luaA_bucket_delete(b->next); }
  free(b->string);
  free(b);
}

luaA_Hashtable* luaA_hashtable_new(int size) {

  luaA_Hashtable* ht = malloc(sizeof(luaA_Hashtable));
  
  ht->size = size;
  ht->buckets = malloc( sizeof(luaA_Bucket*) * ht->size );
  
  for(int i = 0; i < size; i++) { ht->buckets[i] = NULL; }
  
  return ht;
}

void luaA_hashtable_delete(luaA_Hashtable* ht) {
  for(int i=0; i< ht->size; i++) {
    if (ht->buckets[i] != NULL) {
      luaA_bucket_delete(ht->buckets[i]);
    }
  }
  free(ht->buckets);
  free(ht);
}

bool luaA_hashtable_contains(luaA_Hashtable* ht, const char* string) {

  if (luaA_hashtable_get(ht, string) == NULL) {
    return false;
  } else {
    return true;
  }

}

void* luaA_hashtable_get(luaA_Hashtable* ht, const char* string) {

  int index = hash(string, ht->size);
  luaA_Bucket* b = ht->buckets[index];
  
  if (b == NULL) {
    return NULL;
  }
  
  while(1){
    if (strcmp(b->string, string) == 0){ return b->item; }
    if (b->next == NULL) { return NULL; }
    else {b = b->next; }
  }
  
  return NULL;

}

void luaA_hashtable_set(luaA_Hashtable* ht, const char* string, void* item) {

  int index = hash(string, ht->size);
  luaA_Bucket* b = ht->buckets[index];
  
  if (b == NULL) {
    luaA_Bucket* new_bucket = luaA_bucket_new(string, item);
    ht->buckets[index] = new_bucket;
    return;
  }
  
  while(1) {
    
    if( strcmp(b->string, string) == 0) {
      b->item = item;
      return;
    }
  
    if( b->next == NULL) {    
      luaA_Bucket* new_bucket = luaA_bucket_new(string, item);
      b->next = new_bucket;
      new_bucket->prev = b;
      return;
    }
  
    b = b->next;
  }

}

char* luaA_hashtable_find(luaA_Hashtable* ht, void* item) {

  for(int i = 0; i < ht->size; i++) {
    luaA_Bucket* b = ht->buckets[i];
    while (b != NULL) {
      if (b->item == item) { return b->string; }
      b = b->next;
    }
  }
  
  return NULL; 
}

void luaA_hashtable_map(luaA_Hashtable* ht, void (*func)(void*)) {

  for(int i = 0; i < ht->size; i++) {
    luaA_Bucket* b = ht->buckets[i];
    while (b != NULL) {
      func(b->item);
      b = b->next;
    }
  }
}

