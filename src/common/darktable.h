#ifndef DARKTABLE_H
#define DARKTABLE_H

#define _XOPEN_SOURCE 600 // for localtime_r
#include <time.h>
#include <inttypes.h>
#include <sqlite3.h>
#include <pthread.h>
#include <glib/gi18n.h>

#define DT_MODULE_VERSION 1
#define DT_VERSION 33
#define DT_CONFIG_VERSION 33

// every module has to define this:
#ifdef _DEBUG
#define DT_MODULE(MODVER) \
int dt_module_dt_version() \
{\
  return -DT_MODULE_VERSION; \
}\
int dt_module_mod_version() \
{\
  return MODVER; \
}
#else
#define DT_MODULE \
int dt_module_dt_version() \
{\
  return DT_MODULE_VERSION; \
}\
int dt_module_mod_version(MODVER) \
{\
  return MODVER; \
}
#endif

// ..to be able to compare it against this:
static inline int dt_version()
{
#ifdef _DEBUG
  return -DT_MODULE_VERSION;
#else
  return DT_MODULE_VERSION;
#endif
}

#ifndef M_PI
  #define M_PI 3.14159265358979323846
#endif

#define HANDLE_SQLITE_ERR(rc) \
  if(rc != SQLITE_OK) \
  { \
    fprintf(stderr, "sqlite3 error: %s\n", sqlite3_errmsg(darktable.db)); \
    return 1; \
  } \

struct dt_film_t;
struct dt_gui_gtk_t;
struct dt_control_t;
struct dt_develop_t;
struct dt_mipmap_cache_t;
struct dt_image_cache_t;
struct dt_lib_t;
struct dt_conf_t;

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
  struct dt_conf_t         *conf;
  struct dt_develop_t      *develop;
  struct dt_film_t         *film;
  struct dt_lib_t          *lib;
  struct dt_view_manager_t *view_manager;
  struct dt_control_t      *control;
  struct dt_gui_gtk_t      *gui;
  struct dt_mipmap_cache_t *mipmap_cache;
  struct dt_image_cache_t  *image_cache;
  sqlite3                  *db;
  pthread_mutex_t db_insert;
  pthread_mutex_t plugin_threadsafe;
  char *progname;
}
darktable_t;

extern darktable_t darktable;
extern const char dt_supported_extensions[];

int dt_init(int argc, char *argv[]);
void dt_cleanup();
void dt_print(dt_debug_thread_t thread, const char *msg, ...);
void dt_gettime_t(char *datetime, time_t t);
void dt_gettime(char *datetime);
void *dt_alloc_align(size_t alignment, size_t size);
void dt_get_datadir(char *datadir, size_t bufsize);
void dt_get_plugindir(char *datadir, size_t bufsize);

#endif
