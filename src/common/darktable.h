#ifndef DARKROOM_H
#define DARKROOM_H

#define _XOPEN_SOURCE 600 // for localtime_r
#include <time.h>
#include <inttypes.h>
#include <sqlite3.h>
#include <pthread.h>

#define DT_VERSION 14

#define HANDLE_SQLITE_ERR(rc) \
  if(rc != SQLITE_OK) \
  { \
    fprintf(stderr, "sqlite3 error: %s\n", sqlite3_errmsg(darktable.db)); \
    return 1; \
  } \

struct dt_library_t;
struct dt_gui_gtk_t;
struct dt_control_t;
struct dt_develop_t;
struct dt_mipmap_cache_t;
struct dt_image_cache_t;

typedef enum dt_debug_thread_t
{
  DT_DEBUG_CACHE = 1,
  DT_DEBUG_CONTROL = 2,
  DT_DEBUG_DEV = 4 // powers of two, masking
}
dt_debug_thread_t;

typedef struct darktable_t
{
  int32_t unmuted;
  struct dt_library_t *library;
  struct dt_develop_t *develop;
  struct dt_control_t *control;
  struct dt_gui_gtk_t *gui;
  struct dt_mipmap_cache_t *mipmap_cache;
  struct dt_image_cache_t *image_cache;
  sqlite3 *db;
  pthread_mutex_t db_insert;
}
darktable_t;

extern darktable_t darktable;

int dt_init(int argc, char *argv[]);
void dt_cleanup();
void dt_print(dt_debug_thread_t thread, const char *msg, ...);
void dt_gettime_t(char *datetime, time_t t);
void dt_gettime(char *datetime);
void *dt_alloc_align(size_t alignment, size_t size);

#endif
