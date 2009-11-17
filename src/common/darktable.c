#ifdef HAVE_CONFIG_H
  #include "../config.h"
#endif
#include "common/darktable.h"
#include "common/film.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "views/view.h"
#include "control/control.h"
#include "gui/gtk.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>
#ifdef HAVE_MAGICK
  #include <magick/MagickCore.h>
#endif
#include <string.h>
#ifdef _OPENMP
#  include <omp.h>
#endif

darktable_t darktable;

int dt_init(int argc, char *argv[])
{
  bindtextdomain (GETTEXT_PACKAGE, DARKTABLE_LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  darktable.progname = argv[0];
#ifdef _OPENMP
  omp_set_num_threads(omp_get_num_procs());
#endif
  darktable.unmuted = 0;
  char *image_to_load = NULL;
  for(int k=1;k<argc;k++)
  {
    if(argv[k][0] == '-')
    {
      if(argv[k][1] == 'd' && argc > k+1)
      {
        if(!strcmp(argv[k+1], "cache"))   darktable.unmuted |= DT_DEBUG_CACHE;   // enable debugging for lib/film/cache module
        if(!strcmp(argv[k+1], "control")) darktable.unmuted |= DT_DEBUG_CONTROL; // enable debugging for scheduler module
        if(!strcmp(argv[k+1], "dev"))     darktable.unmuted |= DT_DEBUG_DEV; // develop module
      }
    }
    else
    {
      image_to_load = argv[k];
    }
  }

#ifdef HAVE_MAGICK
  MagickCoreGenesis(*argv, MagickTrue);
#endif
#ifdef HAVE_GEGL
  (void)setenv("GEGL_PATH", DATADIR"/gegl:/usr/lib/gegl-0.0", 1);
  gegl_init(&argc, &argv);
#endif
  char *homedir = getenv("HOME");
  char filename[512], *c = NULL;
  snprintf(filename, 512, "%s/.darktablerc", homedir);
  FILE *f = fopen(filename, "rb");
  if(f)
  {
    c = fgets(filename, 512, f);
    if(c) for(;c<filename+MIN(512,strlen(filename));c++) if(*c == '\n') *c = '\0';
    fclose(f);
  }
  if(!c) snprintf(filename, 512, "%s/.darktabledb", homedir);
  if(sqlite3_open(filename, &(darktable.db)))
  {
    fprintf(stderr, "[init] could not open database %s!\n", filename);
    sqlite3_close(darktable.db);
    exit(1);
  }
  pthread_mutex_init(&(darktable.db_insert), NULL);

  // has to go first for settings needed by all the others.
  darktable.control = (dt_control_t *)malloc(sizeof(dt_control_t));
  dt_control_init(darktable.control);

  darktable.mipmap_cache = (dt_mipmap_cache_t *)malloc(sizeof(dt_mipmap_cache_t));
  dt_mipmap_cache_init(darktable.mipmap_cache, 500);

  darktable.image_cache = (dt_image_cache_t *)malloc(sizeof(dt_image_cache_t));
  dt_image_cache_init(darktable.image_cache, 500);

  darktable.film = (dt_film_t *)malloc(sizeof(dt_film_t));
  dt_film_init(darktable.film);

  darktable.view_manager = (dt_view_manager_t *)malloc(sizeof(dt_view_manager_t));
  dt_view_manager_init(darktable.view_manager);

  darktable.gui = (dt_gui_gtk_t *)malloc(sizeof(dt_gui_gtk_t));
  dt_gui_gtk_init(darktable.gui, argc, argv);

  dt_control_load_config(darktable.control);
  strncpy(darktable.control->global_settings.dbname, filename, 512); // overwrite if relocated.

  if(image_to_load)
  {
    int id = dt_image_import(1, image_to_load);
    if(id)
    {
      dt_film_open(darktable.film, 1);
      DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, id);
      dt_ctl_switch_mode_to(DT_DEVELOP);
    }
    else
    {
      fprintf(stderr, "[dt_init] could not open image file `%s'!\n", image_to_load);
    }
  }

  return 0;
}

void dt_cleanup()
{
  dt_ctl_switch_mode_to(DT_LIBRARY);
  char *homedir = getenv("HOME");
  char filename[512];
  snprintf(filename, 512, "%s/.darktablerc", homedir);
  FILE *f = fopen(filename, "wb");
  if(f)
  {
    if(fputs(darktable.control->global_settings.dbname, f) == EOF) fprintf(stderr, "[cleanup] could not write to %s!\n", filename);
    fclose(f);
  }
  else fprintf(stderr, "[cleanup] could not write to %s!\n", filename);
  dt_control_write_config(darktable.control);

  dt_control_shutdown(darktable.control);

  dt_gui_gtk_cleanup(darktable.gui);
  free(darktable.gui);
  dt_view_manager_cleanup(darktable.view_manager);
  free(darktable.view_manager);
  dt_film_cleanup(darktable.film);
  free(darktable.film);
  dt_image_cache_cleanup(darktable.image_cache);
  free(darktable.image_cache);
  dt_mipmap_cache_cleanup(darktable.mipmap_cache);
  free(darktable.mipmap_cache);
  dt_control_cleanup(darktable.control);
  free(darktable.control);

  sqlite3_close(darktable.db);
  pthread_mutex_destroy(&(darktable.db_insert));

#ifdef HAVE_MAGICK
  MagickCoreTerminus();
#endif
#ifdef HAVE_GEGL
  gegl_exit();
#endif
}

void dt_print(dt_debug_thread_t thread, const char *msg, ...)
{
  if(darktable.unmuted & thread)
  {
    va_list ap;
    va_start(ap, msg);
    vprintf(msg, ap);
    va_end(ap);
  }
}

void dt_gettime_t(char *datetime, time_t t)
{
  struct tm tt;
  (void)localtime_r(&t, &tt);
  strftime(datetime, 20, "%Y:%m:%d %H:%M:%S", &tt);
}

void dt_gettime(char *datetime)
{
  dt_gettime_t(datetime, time(NULL));
}

void *dt_alloc_align(size_t alignment, size_t size)
{
#if defined(__MACH__) || defined(__APPLE__)
  return malloc(size);
#else
  void *ptr = NULL;
  if(posix_memalign(&ptr, alignment, size)) return NULL;
  return ptr;
#endif
}

void dt_get_datadir(char *datadir, size_t bufsize)
{
  gchar *curr = g_get_current_dir();
  int contains = 0; for(int k=0;darktable.progname[k] != 0;k++) if(darktable.progname[k] == '/') { contains = 1; break; }
  if(darktable.progname[0] == '/') // absolute path
    snprintf(datadir, bufsize, "%s", darktable.progname);
  else if(contains) // relative path
    snprintf(datadir, bufsize, "%s/%s", curr, darktable.progname);
  else
  { // no idea where we have been called. use compiled in path
    g_free(curr);
    snprintf(datadir, bufsize, "%s", DATADIR);
    return;
  }
  size_t len = MIN(strlen(datadir), bufsize);
  char *t = datadir + len; // strip off bin/darktable
  for(;t>datadir && *t!='/';t--); t--;
  if(*t == '.' && *(t-1) != '.') { for(;t>datadir && *t!='/';t--); t--; }
  for(;t>datadir && *t!='/';t--);
  strcpy(t, "/share/darktable");
  g_free(curr);
}

