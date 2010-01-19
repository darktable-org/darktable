#ifdef HAVE_CONFIG_H
  #include "../config.h"
#endif
#include "common/darktable.h"
#include "common/film.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "libs/lib.h"
#include "views/view.h"
#include "control/control.h"
#include "control/conf.h"
#include "gui/gtk.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <lcms.h>
#ifdef HAVE_MAGICK
  #include <magick/MagickCore.h>
#endif
#include <string.h>
#ifdef _OPENMP
#  include <omp.h>
#endif

darktable_t darktable;
const char dt_supported_extensions[] = "3fr,arw,bay,bmq,cine,cr2,crw,cs1,dc2,dcr,dng,erf,fff,hdr,ia,jpg,jpeg,k25,kc2,kdc,mdc,mef,mos,mrw,nef,nrw,orf,pef,pfm,pxn,qtk,raf,raw,rdc,rw2,rwl,sr2,srf,sti,x3f";

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
        k ++;
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
  (void)cmsErrorAction(LCMS_ERROR_IGNORE);
  char *homedir = getenv("HOME");
  char filename[512];
  snprintf(filename, 512, "%s/.darktablerc", homedir);

  // has to go first for settings needed by all the others.
  darktable.conf = (dt_conf_t *)malloc(sizeof(dt_conf_t));
  dt_conf_init(darktable.conf, filename);

  char dbfilename[1024];
  gchar *dbname = dt_conf_get_string("database");
  if(dbname[0] != '/') snprintf(dbfilename, 512, "%s/%s", homedir, dbname);
  else                 snprintf(dbfilename, 512, "%s", dbname);

  if(sqlite3_open(dbfilename, &(darktable.db)))
  {
    fprintf(stderr, "[init] could not open database `%s'!\n", dbname);
    fprintf(stderr, "[init] maybe your ~/.darktablerc is corrupt?\n");
    dt_get_datadir(dbfilename, 512);
    fprintf(stderr, "[init] try `cp %s/darktablerc ~/.darktablerc'\n", dbfilename);
    sqlite3_close(darktable.db);
    g_free(dbname);
    exit(1);
  }
  g_free(dbname);
  pthread_mutex_init(&(darktable.db_insert), NULL);
  pthread_mutex_init(&(darktable.plugin_threadsafe), NULL);

  darktable.control = (dt_control_t *)malloc(sizeof(dt_control_t));
  dt_control_init(darktable.control);

  int thumbnails = dt_conf_get_int ("mipmap_cache_thumbnails");
  thumbnails = MIN(1000, MAX(20, thumbnails));

  darktable.mipmap_cache = (dt_mipmap_cache_t *)malloc(sizeof(dt_mipmap_cache_t));
  dt_mipmap_cache_init(darktable.mipmap_cache, thumbnails);

  darktable.image_cache = (dt_image_cache_t *)malloc(sizeof(dt_image_cache_t));
  dt_image_cache_init(darktable.image_cache, thumbnails);

  darktable.film = (dt_film_t *)malloc(sizeof(dt_film_t));
  dt_film_init(darktable.film);

  darktable.lib = (dt_lib_t *)malloc(sizeof(dt_lib_t));
  dt_lib_init(darktable.lib);

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
      // TODO: gtk window!
      fprintf(stderr, "[dt_init] could not open image file `%s'!\n", image_to_load);
    }
  }

  return 0;
}

void dt_cleanup()
{
  dt_ctl_switch_mode_to(DT_LIBRARY);

  dt_control_write_config(darktable.control);
  dt_control_shutdown(darktable.control);

  dt_gui_gtk_cleanup(darktable.gui);
  free(darktable.gui);
  dt_view_manager_cleanup(darktable.view_manager);
  free(darktable.view_manager);
  dt_lib_cleanup(darktable.lib);
  free(darktable.lib);
  dt_film_cleanup(darktable.film);
  free(darktable.film);
  dt_image_cache_cleanup(darktable.image_cache);
  free(darktable.image_cache);
  dt_mipmap_cache_cleanup(darktable.mipmap_cache);
  free(darktable.mipmap_cache);
  dt_control_cleanup(darktable.control);
  free(darktable.control);
  dt_conf_cleanup(darktable.conf);
  free(darktable.conf);

  sqlite3_close(darktable.db);
  pthread_mutex_destroy(&(darktable.db_insert));
  pthread_mutex_destroy(&(darktable.plugin_threadsafe));

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

