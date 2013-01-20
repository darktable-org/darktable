/*
    This file is part of darktable,
    copyright (c) 2009--2012 johannes hanika.
    copyright (c) 2010--2012 henrik andersson.
    copyright (c) 2010--2012 tobias ellinghaus.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "common/darktable.h"
#include "common/collection.h"
#include "common/selection.h"
#include "common/exif.h"
#include "common/fswatch.h"
#include "common/pwstorage/pwstorage.h"
#ifdef HAVE_GPHOTO2
#include "common/camera_control.h"
#endif
#include "common/film.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/imageio_module.h"
#include "common/mipmap_cache.h"
#include "common/opencl.h"
#include "common/points.h"
#include "develop/imageop.h"
#include "develop/blend.h"
#include "libs/lib.h"
#include "views/view.h"
#include "control/control.h"
#include "control/jobs/control_jobs.h"
#include "control/signal.h"
#include "control/conf.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "bauhaus/bauhaus.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>
#include <locale.h>
#include <xmmintrin.h>
#ifdef HAVE_GRAPHICSMAGICK
#include <magick/api.h>
#endif
#include "dbus.h"

#if !defined(__APPLE__) && !defined(__FreeBSD__) && !defined(__DragonFly__)
#include <malloc.h>
#endif
#ifdef __APPLE__
#include <sys/malloc.h>
#endif

#if defined(__SUNOS__)
#include <sys/varargs.h>
#endif
#ifdef _OPENMP
#  include <omp.h>
#endif

darktable_t darktable;
const char dt_supported_extensions[] = "3fr,arw,bay,bmq,cap,cine,cr2,crw,cs1,dc2,dcr,dng,erf,fff,exr,ia,iiq,jpeg,jpg,k25,kc2,kdc,mdc,mef,mos,mrw,nef,nrw,orf,pef,pfm,pxn,qtk,raf,raw,rdc,rw2,rwl,sr2,srf,srw,sti,tif,tiff,x3f,png"
#ifdef HAVE_OPENJPEG
",j2c,j2k,jp2,jpc"
#endif
#ifdef HAVE_GRAPHICSMAGICK
",gif,jpc,jp2,bmp,dcm,jng,miff,mng,pbm,pnm,ppm,pgm"
#endif
;

static int usage(const char *argv0)
{
  printf("usage: %s [-d {all,cache,camctl,control,dev,fswatch,lighttable,memory,nan,opencl,perf,pwstorage,sql}] [IMG_1234.{RAW,..}|image_folder/]", argv0);
#ifdef HAVE_OPENCL
  printf(" [--disable-opencl]");
#endif
  printf(" [--library <library file>]");
  printf(" [--datadir <data directory>]");
  printf(" [--moduledir <module directory>]");
  printf(" [--tmpdir <tmp directory>]");
  printf(" [--configdir <user config directory>]");
  printf(" [--cachedir <user config directory>]");
  printf(" [--localedir <locale directory>]");
  printf("\n");
  return 1;
}

#ifndef __APPLE__
typedef void (dt_signal_handler_t)(int) ;
// static dt_signal_handler_t *_dt_sigill_old_handler = NULL;
static dt_signal_handler_t *_dt_sigsegv_old_handler = NULL;
#endif

#if (defined(__FreeBSD_version) && (__FreeBSD_version < 800071)) || \
  defined(__SUNOS__)
static int dprintf(int fd,const char *fmt, ...)
{
  va_list ap;
  FILE *f = fdopen(fd,"a");
  va_start(ap, fmt);
  int rc = vfprintf(f, fmt, ap);
  fclose(f);
  va_end(ap);
  return rc;
}
#endif

#ifndef __APPLE__
static
void _dt_sigsegv_handler(int param)
{
  FILE *fd;
  gchar buf[PIPE_BUF];
  gchar *name_used;
  int fout;
  gboolean delete_file = FALSE;
  char datadir[DT_MAX_PATH_LEN];

  if((fout = g_file_open_tmp("darktable_bt_XXXXXX.txt", &name_used, NULL)) == -1)
    fout = STDOUT_FILENO; // just print everything to stdout

  dprintf(fout, "this is %s reporting a segfault:\n\n", PACKAGE_STRING);
  dt_loc_get_datadir(datadir, DT_MAX_PATH_LEN);
  gchar *command = g_strdup_printf("gdb %s %d -batch -x %s/gdb_commands", darktable.progname, (int)getpid(), datadir);

  if((fd = popen(command, "r")) != NULL)
  {
    gboolean read_something = FALSE;
    while((fgets(buf, PIPE_BUF, fd)) != NULL)
    {
      read_something = TRUE;
      dprintf(fout, "%s", buf);
    }
    pclose(fd);
    if(fout != STDOUT_FILENO)
    {
      if(read_something)
        g_printerr("backtrace written to %s\n", name_used);
      else
      {
        delete_file = TRUE;
        g_printerr("an error occured while trying to execute gdb. please check if gdb is installed on your system.\n");
      }
    }
  }
  else
  {
    delete_file = TRUE;
    g_printerr("an error occured while trying to execute gdb.\n");
  }

  if(fout != STDOUT_FILENO)
    close(fout);
  if(delete_file)
    g_unlink(name_used);
  g_free(command);
  g_free(name_used);

  /* pass it further to the old handler*/
  _dt_sigsegv_old_handler(param);
}
#endif

#if 0
static
void _dt_sigill_handler(int param)
{
  fprintf(stderr, "[this doesn't seem to work]\n");
  GtkWidget *dlg = gtk_message_dialog_new(NULL,GTK_DIALOG_MODAL,GTK_MESSAGE_ERROR,GTK_BUTTONS_OK,_(
      "darktable has trapped an illegal instruction which probably means that \
an invalid processor optimized codepath is used for your cpu, please try reproduce the crash running 'gdb darktable' from \
the console and post the backtrace log to mailing list with information about your CPU and where you got the package from."));
  gtk_dialog_run(GTK_DIALOG(dlg));

  /* pass it further to the old handler*/
  _dt_sigill_old_handler(param);
}

#if defined(__i386__) && defined(__PIC__)
#define cpuid(level, a, b, c, d) \
  __asm__ ("xchgl %%ebx, %1\n" \
        "cpuid\n" \
        "xchgl  %%ebx, %1\n" \
        : "=a" (a), "=r" (b), "=c" (c), "=d" (d)	\
        : "0" (level) \
      )
#else
#define cpuid(level, a, b, c, d) \
  __asm__ ("cpuid"	\
    : "=a" (a), "=b" (b), "=c" (c), "=d" (d) \
    : "0" (level) \
    )
#endif


static
void dt_check_cpu(int argc,char **argv)
{
  /* hook up SIGILL handler */
  _dt_sigill_old_handler = signal(SIGILL,&_dt_sigill_handler);

  /* call cpuid for  SSE level */
  int ax,bx,cx,dx;
  cpuid(0x1,ax,bx,cx,dx);

  ax = bx = 0;
  char message[512]= {0};
  strcat(message,_("SIMD extensions found: "));
  if((cx & 1) && (darktable.cpu_flags |= DT_CPU_FLAG_SSE3))
    strcat(message,"SSE3 ");
  if( ((dx >> 26) & 1) && (darktable.cpu_flags |= DT_CPU_FLAG_SSE2))
    strcat(message,"SSE2 ");
  if (((dx >> 25) & 1) && (darktable.cpu_flags |= DT_CPU_FLAG_SSE))
    strcat(message,"SSE ");
  if (!darktable.cpu_flags)
    strcat(message,"none");

  /* for now, bail out if SSE2 is not availble */
  if(!(darktable.cpu_flags & DT_CPU_FLAG_SSE2))
  {
    gtk_init (&argc, &argv);

    GtkWidget *dlg = gtk_message_dialog_new(NULL,GTK_DIALOG_MODAL,GTK_MESSAGE_ERROR,GTK_BUTTONS_OK,_(
        "darktable is very cpu intensive and uses SSE2 SIMD instructions \
for heavy calculations. This gives a better user experience but also defines a minimum \
processor requirement.\n\nThe processor in YOUR system does NOT support SSE2. \
darktable will now close down.\n\n%s"),message);

    gtk_dialog_run(GTK_DIALOG(dlg));

    exit(11);
  }
}
#endif

gboolean dt_supported_image(const gchar *filename)
{
  gboolean supported = FALSE;
  char **extensions = g_strsplit(dt_supported_extensions, ",", 100);
  char *ext = g_strrstr(filename,".");
  if(!ext) return FALSE;
  ext++;
  for(char **i=extensions; *i!=NULL; i++)
    if(!g_ascii_strncasecmp(ext, *i,strlen(*i)))
    {
      supported = TRUE;
      break;
    }
  g_strfreev(extensions);
  return supported;
}

static void strip_semicolons_from_keymap(const char* path)
{
  char pathtmp[DT_MAX_PATH_LEN];
  FILE *fin = fopen(path, "r");
  FILE *fout;
  int i;
  int c = '\0';

  snprintf(pathtmp, DT_MAX_PATH_LEN, "%s_tmp", path);
  fout = fopen(pathtmp, "w");

  // First ignoring the first three lines
  for(i = 0; i < 3; i++)
  {
    c = fgetc(fin);
    while(c != '\n')
      c = fgetc(fin);
  }

  // Then ignore the first two characters of each line, copying the rest out
  while(c != EOF)
  {
    fseek(fin, 2, SEEK_CUR);
    do
    {
      c = fgetc(fin);
      if(c != EOF)
        fputc(c, fout);
    }
    while(c != '\n' && c != EOF);
  }

  fclose(fin);
  fclose(fout);

  GFile *gpath = g_file_new_for_path(path);
  GFile *gpathtmp = g_file_new_for_path(pathtmp);

  g_file_delete(gpath, NULL, NULL);
  g_file_move(gpathtmp, gpath, 0, NULL, NULL, NULL, NULL);
  g_object_unref(gpath);
  g_object_unref(gpathtmp);
}

static gchar * dt_make_path_absolute(const gchar * input)
{
  gchar *filename = NULL;

  if(g_str_has_prefix(input, "file://")) // in this case we should take care of %XX encodings in the string (for example %20 = ' ')
  {
    input += strlen("file://");
    filename = g_uri_unescape_string(input, NULL);
  }
  else
    filename = g_strdup(input);

  if(g_path_is_absolute(filename) == FALSE)
  {
    char* current_dir = g_get_current_dir();
    char* tmp_filename = g_build_filename(current_dir, filename, NULL);
    g_free(filename);
    filename = (char*)g_malloc(sizeof(char)*MAXPATHLEN);
    if(realpath(tmp_filename, filename) == NULL)
    {
      g_free(current_dir);
      g_free(tmp_filename);
      g_free(filename);
      return NULL;
    }
    g_free(current_dir);
    g_free(tmp_filename);
  }

  return filename;
}

int dt_load_from_string(const gchar* input, gboolean open_image_in_dr)
{
  int id = 0;
  if(input == NULL || input[0] == '\0')
    return 0;

  char* filename = dt_make_path_absolute(input);

  if(filename == NULL)
  {
    dt_control_log(_("found strange path `%s'"), input);
    return 0;
  }

  if(g_file_test(filename, G_FILE_TEST_IS_DIR))
  {
    // import a directory into a film roll
    unsigned int last_char = strlen(filename)-1;
    if(filename[last_char] == '/')
      filename[last_char] = '\0';
    id = dt_film_import(filename);
    if(id)
    {
      dt_film_open(id);
      dt_ctl_switch_mode_to(DT_LIBRARY);
    }
    else
    {
      dt_control_log(_("error loading directory `%s'"), filename);
    }
  }
  else
  {
    // import a single image
    gchar *directory = g_path_get_dirname((const gchar *)filename);
    dt_film_t film;
    const int filmid = dt_film_new(&film, directory);
    id = dt_image_import(filmid, filename, TRUE);
    g_free (directory);
    if(id)
    {
      dt_film_open(filmid);
      // make sure buffers are loaded (load full for testing)
      dt_mipmap_buffer_t buf;
      dt_mipmap_cache_read_get(darktable.mipmap_cache, &buf, id, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING);
      if(!buf.buf)
      {
        id = 0;
        dt_control_log(_("file `%s' has unknown format!"), filename);
      }
      else
      {
        dt_mipmap_cache_read_release(darktable.mipmap_cache, &buf);
        if(open_image_in_dr)
        {
          DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, id);
          dt_ctl_switch_mode_to(DT_DEVELOP);
        }
      }
    }
    else
    {
      dt_control_log(_("error loading file `%s'"), filename);
    }
  }
  g_free(filename);
  return id;
}

int dt_init(int argc, char *argv[], const int init_gui)
{
  // make everything go a lot faster.
  _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
#ifndef __APPLE__
  _dt_sigsegv_old_handler = signal(SIGSEGV,&_dt_sigsegv_handler);
#endif

#ifndef __SSE2__
  fprintf(stderr, "[dt_init] unfortunately we depend on SSE2 instructions at this time.\n");
  fprintf(stderr, "[dt_init] please contribute a backport patch (or buy a newer processor).\n");
  return 1;
#endif

#ifdef M_MMAP_THRESHOLD
  mallopt(M_MMAP_THRESHOLD,128*1024) ; /* use mmap() for large allocations */
#endif

  bindtextdomain (GETTEXT_PACKAGE, DARKTABLE_LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);


  // init all pointers to 0:
  memset(&darktable, 0, sizeof(darktable_t));

  darktable.progname = argv[0];

  // database
  gchar *dbfilename_from_command = NULL;
  char *datadirFromCommand = NULL;
  char *moduledirFromCommand = NULL;
  char *tmpdirFromCommand = NULL;
  char *configdirFromCommand = NULL;
  char *cachedirFromCommand = NULL;

  darktable.num_openmp_threads = 1;
#ifdef _OPENMP
  darktable.num_openmp_threads = omp_get_num_procs();
#endif
  darktable.unmuted = 0;
  GSList *images_to_load = NULL;
  for(int k=1; k<argc; k++)
  {
    if(argv[k][0] == '-')
    {
      if(!strcmp(argv[k], "--help"))
      {
        return usage(argv[0]);
      }
      if(!strcmp(argv[k], "-h"))
      {
        return usage(argv[0]);
      }
      else if(!strcmp(argv[k], "--version"))
      {
        printf("this is "PACKAGE_STRING"\ncopyright (c) 2009-2012 johannes hanika\n"PACKAGE_BUGREPORT"\n");
        return 1;
      }
      else if(!strcmp(argv[k], "--library"))
      {
        dbfilename_from_command = argv[++k];
      }
      else if(!strcmp(argv[k], "--datadir"))
      {
        datadirFromCommand = argv[++k];
      }
      else if(!strcmp(argv[k], "--moduledir"))
      {
        moduledirFromCommand = argv[++k];
      }
      else if(!strcmp(argv[k], "--tmpdir"))
      {
        tmpdirFromCommand = argv[++k];
      }
      else if(!strcmp(argv[k], "--configdir"))
      {
        configdirFromCommand = argv[++k];
      }
      else if(!strcmp(argv[k], "--cachedir"))
      {
        cachedirFromCommand = argv[++k];
      }
      else if(!strcmp(argv[k], "--localedir"))
      {
        bindtextdomain (GETTEXT_PACKAGE, argv[++k]);
      }
      else if(argv[k][1] == 'd' && argc > k+1)
      {
        if(!strcmp(argv[k+1], "all"))             darktable.unmuted = 0xffffffff;   // enable all debug information
        else if(!strcmp(argv[k+1], "cache"))      darktable.unmuted |= DT_DEBUG_CACHE;   // enable debugging for lib/film/cache module
        else if(!strcmp(argv[k+1], "control"))    darktable.unmuted |= DT_DEBUG_CONTROL; // enable debugging for scheduler module
        else if(!strcmp(argv[k+1], "dev"))        darktable.unmuted |= DT_DEBUG_DEV; // develop module
        else if(!strcmp(argv[k+1], "fswatch"))    darktable.unmuted |= DT_DEBUG_FSWATCH; // fswatch module
        else if(!strcmp(argv[k+1], "camctl"))     darktable.unmuted |= DT_DEBUG_CAMCTL; // camera control module
        else if(!strcmp(argv[k+1], "perf"))       darktable.unmuted |= DT_DEBUG_PERF; // performance measurements
        else if(!strcmp(argv[k+1], "pwstorage"))  darktable.unmuted |= DT_DEBUG_PWSTORAGE; // pwstorage module
        else if(!strcmp(argv[k+1], "opencl"))     darktable.unmuted |= DT_DEBUG_OPENCL;    // gpu accel via opencl
        else if(!strcmp(argv[k+1], "sql"))        darktable.unmuted |= DT_DEBUG_SQL; // SQLite3 queries
        else if(!strcmp(argv[k+1], "memory"))     darktable.unmuted |= DT_DEBUG_MEMORY; // some stats on mem usage now and then.
        else if(!strcmp(argv[k+1], "lighttable")) darktable.unmuted |= DT_DEBUG_LIGHTTABLE; // lighttable related stuff.
        else if(!strcmp(argv[k+1], "nan"))        darktable.unmuted |= DT_DEBUG_NAN; // check for NANs when processing the pipe.
        else return usage(argv[0]);
        k ++;
      }
      else if(argv[k][1] == 't' && argc > k+1)
      {
        darktable.num_openmp_threads = CLAMP(atol(argv[k+1]), 1, 100);
        printf("[dt_init] using %d threads for openmp parallel sections\n", darktable.num_openmp_threads);
        k ++;
      }
    }
#ifndef MAC_INTEGRATION
    else
    {
      images_to_load = g_slist_append(images_to_load, argv[k]);
    }
#endif
  }

  if(darktable.unmuted & DT_DEBUG_MEMORY)
  {
    fprintf(stderr, "[memory] at startup\n");
    dt_print_mem_usage();
  }

#ifdef _OPENMP
  omp_set_num_threads(darktable.num_openmp_threads);
#endif
  dt_loc_init_datadir(datadirFromCommand);
  dt_loc_init_plugindir(moduledirFromCommand);
  if(dt_loc_init_tmp_dir(tmpdirFromCommand))
  {
    printf(_("ERROR : invalid temporary directory : %s\n"),darktable.tmpdir);
    return usage(argv[0]);
  }
  dt_loc_init_user_config_dir(configdirFromCommand);
  dt_loc_init_user_cache_dir(cachedirFromCommand);

#if !GLIB_CHECK_VERSION(2, 35, 0)
  g_type_init();
#endif

  // does not work, as gtk is not inited yet.
  // even if it were, it's a super bad idea to invoke gtk stuff from
  // a signal handler.
  /* check cput caps */
  // dt_check_cpu(argc,argv);

#ifdef HAVE_GEGL
  char geglpath[DT_MAX_PATH_LEN];
  char datadir[DT_MAX_PATH_LEN];
  dt_loc_get_datadir(datadir, DT_MAX_PATH_LEN);
  snprintf(geglpath, DT_MAX_PATH_LEN, "%s/gegl:/usr/lib/gegl-0.0", datadir);
  (void)setenv("GEGL_PATH", geglpath, 1);
  gegl_init(&argc, &argv);
#endif

  // thread-safe init:
  dt_exif_init();
  char datadir[DT_MAX_PATH_LEN];
  dt_loc_get_user_config_dir (datadir,DT_MAX_PATH_LEN);
  char filename[DT_MAX_PATH_LEN];
  snprintf(filename, DT_MAX_PATH_LEN, "%s/darktablerc", datadir);

  // intialize the config backend. this needs to be done first...
  darktable.conf = (dt_conf_t *)malloc(sizeof(dt_conf_t));
  memset(darktable.conf, 0, sizeof(dt_conf_t));
  dt_conf_init(darktable.conf, filename);

  // set the interface language
  const gchar* lang = dt_conf_get_string("ui_last/gui_language");
  if(lang != NULL && lang[0] != '\0')
  {
    if(setlocale(LC_ALL, lang) != NULL)
      gtk_disable_setlocale();
  }

  // initialize the database
  darktable.db = dt_database_init(dbfilename_from_command);
  if(darktable.db == NULL)
  {
    printf("ERROR : cannot open database\n");
    return 1;
  }
  else if(dt_database_get_already_locked(darktable.db))
  {
    // send the images to the other instance via dbus
    if(images_to_load)
    {
      GSList *p = images_to_load;

      // get a connection!
      GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SESSION,NULL, NULL);

      while (p != NULL)
      {
        // make the filename absolute ...
        gchar *filename = dt_make_path_absolute((gchar*)p->data);
        if(filename == NULL) continue;
        // ... and send it to the running instance of darktable
        g_dbus_connection_call_sync(connection,
                                    "org.darktable.service",
                                    "/darktable",
                                    "org.darktable.service.Remote",
                                    "Open",
                                    g_variant_new ("(s)", filename),
                                    NULL,
                                    G_DBUS_CALL_FLAGS_NONE,
                                    -1,
                                    NULL,
                                    NULL);
        p = g_slist_next(p);
        g_free(filename);
      }

      g_slist_free(images_to_load);
      g_object_unref(connection);
    }

    return 1;
  }

  // Initialize the signal system
  darktable.signals = dt_control_signal_init();

  // Initialize the filesystem watcher
  darktable.fswatch=dt_fswatch_new();

#ifdef HAVE_GPHOTO2
  // Initialize the camera control
  darktable.camctl=dt_camctl_new();
#endif

  // get max lighttable thumbnail size:
  darktable.thumbnail_width  = CLAMPS(dt_conf_get_int("plugins/lighttable/thumbnail_width"),  200, 3000);
  darktable.thumbnail_height = CLAMPS(dt_conf_get_int("plugins/lighttable/thumbnail_height"), 200, 3000);
  // and make sure it can be mip-mapped all the way from mip4 to mip0
  darktable.thumbnail_width  /= 16;
  darktable.thumbnail_width  *= 16;
  darktable.thumbnail_height /= 16;
  darktable.thumbnail_height *= 16;

  // Initialize the password storage engine
  darktable.pwstorage=dt_pwstorage_new();

  // FIXME: move there into dt_database_t
  dt_pthread_mutex_init(&(darktable.db_insert), NULL);
  dt_pthread_mutex_init(&(darktable.plugin_threadsafe), NULL);
  dt_pthread_mutex_init(&(darktable.capabilities_threadsafe), NULL);
  darktable.control = (dt_control_t *)malloc(sizeof(dt_control_t));
  memset(darktable.control, 0, sizeof(dt_control_t));
  if(init_gui)
  {
    dt_control_init(darktable.control);
  }
  else
  {
    // this is in memory, so schema can't exist yet.
    if(dbfilename_from_command && !strcmp(dbfilename_from_command, ":memory:"))
    {
      dt_control_create_database_schema();
      dt_gui_presets_init(); // also init preset db schema.
    }
    darktable.control->running = 0;
    darktable.control->accelerators = NULL;
    dt_pthread_mutex_init(&darktable.control->run_mutex, NULL);
  }

  // initialize collection query
  darktable.collection_listeners = NULL;
  darktable.collection = dt_collection_new(NULL);

  /* initialize sellection */
  darktable.selection = dt_selection_new();

  /* capabilities set to NULL */
  darktable.capabilities = NULL;

#ifdef HAVE_GRAPHICSMAGICK
  /* GraphicsMagick init */
  InitializeMagick(darktable.progname);
#endif

  darktable.opencl = (dt_opencl_t *)malloc(sizeof(dt_opencl_t));
  memset(darktable.opencl, 0, sizeof(dt_opencl_t));
  dt_opencl_init(darktable.opencl, argc, argv);

  darktable.blendop = (dt_blendop_t *)malloc(sizeof(dt_blendop_t));
  memset(darktable.blendop, 0, sizeof(dt_blendop_t));
  dt_develop_blend_init(darktable.blendop);

  darktable.points = (dt_points_t *)malloc(sizeof(dt_points_t));
  memset(darktable.points, 0, sizeof(dt_points_t));
  dt_points_init(darktable.points, dt_get_num_threads());

  // must come before mipmap_cache, because that one will need to access
  // image dimensions stored in here:
  darktable.image_cache = (dt_image_cache_t *)malloc(sizeof(dt_image_cache_t));
  memset(darktable.image_cache, 0, sizeof(dt_image_cache_t));
  dt_image_cache_init(darktable.image_cache);

  darktable.mipmap_cache = (dt_mipmap_cache_t *)malloc(sizeof(dt_mipmap_cache_t));
  memset(darktable.mipmap_cache, 0, sizeof(dt_mipmap_cache_t));
  dt_mipmap_cache_init(darktable.mipmap_cache);

  // The GUI must be initialized before the views, because the init()
  // functions of the views depend on darktable.control->accels_* to register
  // their keyboard accelerators

  if(init_gui)
  {
    darktable.gui = (dt_gui_gtk_t *)malloc(sizeof(dt_gui_gtk_t));
    memset(darktable.gui,0,sizeof(dt_gui_gtk_t));
    if(dt_gui_gtk_init(darktable.gui, argc, argv)) return 1;
    dt_bauhaus_init();
  }
  else darktable.gui = NULL;

  darktable.view_manager = (dt_view_manager_t *)malloc(sizeof(dt_view_manager_t));
  memset(darktable.view_manager, 0, sizeof(dt_view_manager_t));
  dt_view_manager_init(darktable.view_manager);

  // load the darkroom mode plugins once:
  dt_iop_load_modules_so();

  if(init_gui)
  {
    darktable.lib = (dt_lib_t *)malloc(sizeof(dt_lib_t));
    memset(darktable.lib, 0, sizeof(dt_lib_t));
    dt_lib_init(darktable.lib);

    dt_control_load_config(darktable.control);
    g_strlcpy(darktable.control->global_settings.dbname, filename, 512); // overwrite if relocated.
  }
  darktable.imageio = (dt_imageio_t *)malloc(sizeof(dt_imageio_t));
  memset(darktable.imageio, 0, sizeof(dt_imageio_t));
  dt_imageio_init(darktable.imageio);

  if(init_gui)
  {
    // Loading the keybindings
    char keyfile[DT_MAX_PATH_LEN];

    // First dump the default keymapping
    snprintf(keyfile, DT_MAX_PATH_LEN, "%s/keyboardrc_default", datadir);
    gtk_accel_map_save(keyfile);

    // Removing extraneous semi-colons from the default keymap
    strip_semicolons_from_keymap(keyfile);

    // Then load any modified keys if available
    snprintf(keyfile, DT_MAX_PATH_LEN, "%s/keyboardrc", datadir);
    if(g_file_test(keyfile, G_FILE_TEST_EXISTS))
      gtk_accel_map_load(keyfile);
    else
      gtk_accel_map_save(keyfile); // Save the default keymap if none is present

    // I doubt that connecting to dbus for darktable-cli makes sense
    darktable.dbus = dt_dbus_init();

    // load image(s) specified on cmdline
    int id = 0;
    if(images_to_load)
    {
      // If only one image is listed, attempt to load it in darkroom
      gboolean load_in_dr = (g_slist_next(images_to_load) == NULL);
      GSList *p = images_to_load;

      while (p != NULL)
      {
        id = MAX(id, dt_load_from_string((gchar*)p->data, load_in_dr));
        p = g_slist_next(p);
      }

      if (!load_in_dr || id == 0)
        dt_ctl_switch_mode_to(DT_LIBRARY);

      g_slist_free(images_to_load);
    }
    else
      dt_ctl_switch_mode_to(DT_LIBRARY);
  }

  /* start the indexer background job */
  dt_control_start_indexer();

  if(darktable.unmuted & DT_DEBUG_MEMORY)
  {
    fprintf(stderr, "[memory] after successful startup\n");
    dt_print_mem_usage();
  }

  return 0;
}

void dt_cleanup()
{
  dt_ctl_switch_mode_to(DT_MODE_NONE);
  const int init_gui = (darktable.gui != NULL);

  if(init_gui)
  {
    dt_dbus_destroy(darktable.dbus);

    dt_control_write_config(darktable.control);
    dt_control_shutdown(darktable.control);

    dt_lib_cleanup(darktable.lib);
    free(darktable.lib);
  }
  dt_view_manager_cleanup(darktable.view_manager);
  free(darktable.view_manager);
  if(init_gui)
  {
    dt_imageio_cleanup(darktable.imageio);
    free(darktable.imageio);
    dt_gui_gtk_cleanup(darktable.gui);
    free(darktable.gui);
  }
  dt_image_cache_cleanup(darktable.image_cache);
  free(darktable.image_cache);
  dt_mipmap_cache_cleanup(darktable.mipmap_cache);
  free(darktable.mipmap_cache);
  if(init_gui)
  {
    dt_control_cleanup(darktable.control);
    free(darktable.control);
  }
  dt_conf_cleanup(darktable.conf);
  free(darktable.conf);
  dt_points_cleanup(darktable.points);
  free(darktable.points);
  dt_iop_unload_modules_so();
  dt_opencl_cleanup(darktable.opencl);
  free(darktable.opencl);
#ifdef HAVE_GPHOTO2
  dt_camctl_destroy(darktable.camctl);
#endif
  dt_pwstorage_destroy(darktable.pwstorage);
  dt_fswatch_destroy(darktable.fswatch);

#ifdef HAVE_GRAPHICSMAGICK
  DestroyMagick();
#endif

  dt_database_destroy(darktable.db);

  dt_bauhaus_cleanup();

  dt_capabilities_cleanup();

  dt_pthread_mutex_destroy(&(darktable.db_insert));
  dt_pthread_mutex_destroy(&(darktable.plugin_threadsafe));
  dt_pthread_mutex_destroy(&(darktable.capabilities_threadsafe));

  dt_exif_cleanup();
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
    fflush(stdout);
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
#if defined(__MACH__) || defined(__APPLE__) || (defined(__FreeBSD_version) && __FreeBSD_version < 700013)
  return malloc(size);
#else
  void *ptr = NULL;
  if(posix_memalign(&ptr, alignment, size)) return NULL;
  return ptr;
#endif
}

void dt_show_times(const dt_times_t *start, const char *prefix, const char *suffix, ...)
{
  dt_times_t end;
  char buf[120];		/* Arbitrary size, should be lots big enough for everything used in DT */
  int i;

  /* Skip all the calculations an everything if -d perf isn't on */
  if (darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_get_times(&end);
    i = sprintf(buf, "%s took %.3f secs (%.3f CPU)", prefix, end.clock - start->clock, end.user - start->user);
    if (suffix != NULL)
    {
      va_list ap;
      va_start(ap, suffix);
      buf[i++] = ' ';
      vsnprintf(buf + i, sizeof buf - i, suffix, ap);
      va_end(ap);
    }
    dt_print(DT_DEBUG_PERF, "%s\n", buf);
  }
}

void dt_configure_defaults()
{
  const int atom_cores = dt_get_num_atom_cores();
  const int threads = dt_get_num_threads();
  const size_t mem = dt_get_total_memory();
  const int bits = (sizeof(void*) == 4) ? 32 : 64;
  fprintf(stderr, "[defaults] found a %d-bit system with %zu kb ram and %d cores (%d atom based)\n", bits, mem, threads, atom_cores);
  if(mem > (2u<<20) && threads > 4)
  {
    fprintf(stderr, "[defaults] setting high quality defaults\n");
    dt_conf_set_int("worker_threads", 8);
    dt_conf_set_int("cache_memory", 1u<<30);
    dt_conf_set_int("plugins/lighttable/thumbnail_width", 1300);
    dt_conf_set_int("plugins/lighttable/thumbnail_height", 1000);
    dt_conf_set_bool("plugins/lighttable/low_quality_thumbnails", FALSE);
  }
  if(mem < (1u<<20) || threads <= 2 || bits < 64 || atom_cores > 0)
  {
    fprintf(stderr, "[defaults] setting very conservative defaults\n");
    dt_conf_set_int("worker_threads", 1);
    dt_conf_set_int("cache_memory", 200u<<20);
    dt_conf_set_int("host_memory_limit", 500);
    dt_conf_set_int("singlebuffer_limit", 8);
    dt_conf_set_int("plugins/lighttable/thumbnail_width", 800);
    dt_conf_set_int("plugins/lighttable/thumbnail_height", 500);
    dt_conf_set_string("plugins/darkroom/demosaic/quality", "always bilinear (fast)");
    dt_conf_set_bool("plugins/lighttable/low_quality_thumbnails", TRUE);
  }
}


int dt_capabilities_check(char *capability)
{
  GList *capabilities = darktable.capabilities;

  while(capabilities)
  {
    if(!strcmp(capabilities->data, capability))
    {
      return TRUE;
    }
    capabilities = g_list_next(capabilities);
  }
  return FALSE;
}


void dt_capabilities_add(char *capability)
{
  dt_pthread_mutex_lock(&darktable.capabilities_threadsafe);

  if(!dt_capabilities_check(capability))
    darktable.capabilities = g_list_append(darktable.capabilities, capability);

  dt_pthread_mutex_unlock(&darktable.capabilities_threadsafe);
}


void dt_capabilities_remove(char *capability)
{
  dt_pthread_mutex_lock(&darktable.capabilities_threadsafe);

  darktable.capabilities = g_list_remove(darktable.capabilities, capability);

  dt_pthread_mutex_unlock(&darktable.capabilities_threadsafe);
}


void dt_capabilities_cleanup()
{
  while(darktable.capabilities)
    darktable.capabilities = g_list_delete_link(darktable.capabilities, darktable.capabilities);
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
