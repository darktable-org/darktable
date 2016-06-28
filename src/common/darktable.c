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

#include "version.h"

#if !defined(__APPLE__) && !defined(__FreeBSD__) && !defined(__OpenBSD__) && !defined(__DragonFly__)
#include <malloc.h>
#endif
#ifdef __APPLE__
#include <sys/malloc.h>
#endif

#include "common/darktable.h"
#include "common/collection.h"
#include "common/colorspaces.h"
#include "common/selection.h"
#include "common/exif.h"
#include "common/fswatch.h"
#include "common/pwstorage/pwstorage.h"
#ifdef HAVE_GPHOTO2
#include "common/camera_control.h"
#endif
#include "common/cpuid.h"
#include "common/film.h"
#include "common/grealpath.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/imageio_module.h"
#include "common/mipmap_cache.h"
#include "common/noiseprofiles.h"
#include "common/opencl.h"
#include "common/points.h"
#include "develop/imageop.h"
#include "develop/blend.h"
#include "libs/lib.h"
#include "views/view.h"
#include "views/undo.h"
#include "control/control.h"
#include "control/crawler.h"
#include "control/jobs/control_jobs.h"
#include "control/signal.h"
#include "control/conf.h"
#include "gui/guides.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "lua/init.h"
#include "bauhaus/bauhaus.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>
#include <sys/types.h>
#ifndef __WIN32__
#include <sys/wait.h>
#endif
#include <locale.h>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif
#ifdef HAVE_GRAPHICSMAGICK
#include <magick/api.h>
#endif
#include "dbus.h"

#if defined(__SUNOS__)
#include <sys/varargs.h>
#endif
#ifdef _OPENMP
#include <omp.h>
#endif

#ifdef __linux__
#include <sys/prctl.h>
#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif
#endif

#ifdef USE_LUA
#include "lua/configuration.h"
#endif

darktable_t darktable;
const char dt_supported_extensions[] = "3fr,arw,bay,bmq,cap,cine,cr2,crw,cs1,dc2,dcr,dng,erf,exr,fff,hdr,ia,iiq,"
                                       "jpeg,jpg,k25,kc2,kdc,mdc,mef,mos,mrw,nef,nrw,orf,pef,pfm,pxn,qtk,raf,"
                                       "raw,rdc,rw2,rwl,sr2,srf,srw,sti,tif,tiff,x3f,png,ari"
#ifdef HAVE_OPENJPEG
                                       ",j2c,j2k,jp2,jpc"
#endif
#ifdef HAVE_GRAPHICSMAGICK
                                       ",gif,jpc,jp2,bmp,dcm,jng,miff,mng,pbm,pnm,ppm,pgm"
#endif
    ;

static int usage(const char *argv0)
{
  printf("usage: %s [-d "
         "{all,cache,camctl,camsupport,control,dev,fswatch,input,lighttable,masks,memory,nan,opencl,perf,pwstorage,print,sql}]"
         " [IMG_1234.{RAW,..}|image_folder/]",
         argv0);
#ifdef HAVE_OPENCL
  printf(" [--disable-opencl]");
#endif
  printf(" [--library <library file>]");
  printf(" [--datadir <data directory>]");
  printf(" [--moduledir <module directory>]");
  printf(" [--tmpdir <tmp directory>]");
  printf(" [--configdir <user config directory>]");
  printf(" [--cachedir <user cache directory>]");
  printf(" [--localedir <locale directory>]");
#ifdef USE_LUA
  printf(" [--luacmd <lua command>]");
#endif
  printf(" [--conf <key>=<value>]");
  printf(" [--noiseprofiles <noiseprofiles json file>]");
  printf("\n");
  return 1;
}

#if !defined __APPLE__ && !defined __WIN32__
typedef void(dt_signal_handler_t)(int);
// static dt_signal_handler_t *_dt_sigill_old_handler = NULL;
static dt_signal_handler_t *_dt_sigsegv_old_handler = NULL;
#endif

#if(defined(__FreeBSD_version) && (__FreeBSD_version < 800071)) || (defined(OpenBSD) && (OpenBSD < 201305))  \
    || defined(__SUNOS__)
static int dprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)))
{
  va_list ap;
  FILE *f = fdopen(fd, "a");
  va_start(ap, fmt);
  int rc = vfprintf(f, fmt, ap);
  fclose(f);
  va_end(ap);
  return rc;
}
#endif

#if !defined __APPLE__ && !defined __WIN32__
static void _dt_sigsegv_handler(int param)
{
  pid_t pid;
  gchar *name_used;
  int fout;
  gboolean delete_file = FALSE;
  char datadir[PATH_MAX] = { 0 };

  if((fout = g_file_open_tmp("darktable_bt_XXXXXX.txt", &name_used, NULL)) == -1)
    fout = STDOUT_FILENO; // just print everything to stdout

  dprintf(fout, "this is %s reporting a segfault:\n\n", PACKAGE_STRING);

  if(fout != STDOUT_FILENO) close(fout);

  dt_loc_get_datadir(datadir, sizeof(datadir));
  gchar *pid_arg = g_strdup_printf("%d", (int)getpid());
  gchar *comm_arg = g_strdup_printf("%s/gdb_commands", datadir);
  gchar *log_arg = g_strdup_printf("set logging on %s", name_used);

  if((pid = fork()) != -1)
  {
    if(pid)
    {
#ifdef __linux__
      // Allow the child to ptrace us
      prctl(PR_SET_PTRACER, pid, 0, 0, 0);
#endif
      waitpid(pid, NULL, 0);
      g_printerr("backtrace written to %s\n", name_used);
    }
    else
    {
      if(execlp("gdb", "gdb", darktable.progname, pid_arg, "-batch", "-ex", log_arg, "-x", comm_arg, NULL))
      {
        delete_file = TRUE;
        g_printerr("an error occurred while trying to execute gdb. please check if gdb is installed on your "
                   "system.\n");
      }
    }
  }
  else
  {
    delete_file = TRUE;
    g_printerr("an error occurred while trying to execute gdb.\n");
  }

  if(delete_file) g_unlink(name_used);
  g_free(pid_arg);
  g_free(comm_arg);
  g_free(log_arg);
  g_free(name_used);

  /* pass it further to the old handler*/
  _dt_sigsegv_old_handler(param);
}
#endif

gboolean dt_supported_image(const gchar *filename)
{
  gboolean supported = FALSE;
  char **extensions = g_strsplit(dt_supported_extensions, ",", 100);
  char *ext = g_strrstr(filename, ".");
  if(!ext)
  {
    g_strfreev(extensions);
    return FALSE;
  }
  ext++;
  for(char **i = extensions; *i != NULL; i++)
    if(!g_ascii_strncasecmp(ext, *i, strlen(*i)))
    {
      supported = TRUE;
      break;
    }
  g_strfreev(extensions);
  return supported;
}

static void strip_semicolons_from_keymap(const char *path)
{
  char pathtmp[PATH_MAX] = { 0 };
  FILE *fin = fopen(path, "r");
  FILE *fout;
  int i;
  int c = '\0';

  snprintf(pathtmp, sizeof(pathtmp), "%s_tmp", path);
  fout = fopen(pathtmp, "w");

  // First ignoring the first three lines
  for(i = 0; i < 3; i++)
  {
    c = fgetc(fin);
    while(c != '\n') c = fgetc(fin);
  }

  // Then ignore the first two characters of each line, copying the rest out
  while(c != EOF)
  {
    fseek(fin, 2, SEEK_CUR);
    do
    {
      c = fgetc(fin);
      if(c != EOF) fputc(c, fout);
    } while(c != '\n' && c != EOF);
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

static gchar *dt_make_path_absolute(const gchar *input)
{
  gchar *filename = NULL;

  if(g_str_has_prefix(input, "file://")) // in this case we should take care of %XX encodings in the string
                                         // (for example %20 = ' ')
  {
    input += strlen("file://");
    filename = g_uri_unescape_string(input, NULL);
  }
  else
    filename = g_strdup(input);

  if(g_path_is_absolute(filename) == FALSE)
  {
    char *current_dir = g_get_current_dir();
    char *tmp_filename = g_build_filename(current_dir, filename, NULL);
    g_free(filename);
    filename = g_realpath(tmp_filename);
    if(filename == NULL)
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

int dt_load_from_string(const gchar *input, gboolean open_image_in_dr, gboolean *single_image)
{
  int id = 0;
  if(input == NULL || input[0] == '\0') return 0;

  char *filename = dt_make_path_absolute(input);

  if(filename == NULL)
  {
    dt_control_log(_("found strange path `%s'"), input);
    return 0;
  }

  if(g_file_test(filename, G_FILE_TEST_IS_DIR))
  {
    // import a directory into a film roll
    unsigned int last_char = strlen(filename) - 1;
    if(filename[last_char] == '/') filename[last_char] = '\0';
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
    if(single_image) *single_image = FALSE;
  }
  else
  {
    // import a single image
    gchar *directory = g_path_get_dirname((const gchar *)filename);
    dt_film_t film;
    const int filmid = dt_film_new(&film, directory);
    id = dt_image_import(filmid, filename, TRUE);
    g_free(directory);
    if(id)
    {
      dt_film_open(filmid);
      // make sure buffers are loaded (load full for testing)
      dt_mipmap_buffer_t buf;
      dt_mipmap_cache_get(darktable.mipmap_cache, &buf, id, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');
      gboolean loaded = (buf.buf != NULL);
      dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
      if(!loaded)
      {
        id = 0;
        dt_control_log(_("file `%s' has unknown format!"), filename);
      }
      else
      {
        if(open_image_in_dr)
        {
          dt_control_set_mouse_over_id(id);
          dt_ctl_switch_mode_to(DT_DEVELOP);
        }
      }
    }
    else
    {
      dt_control_log(_("error loading file `%s'"), filename);
    }
    if(single_image) *single_image = TRUE;
  }
  g_free(filename);
  return id;
}

static void dt_codepaths_init()
{
#ifdef HAVE_BUILTIN_CPU_SUPPORTS
  __builtin_cpu_init();
#endif

  memset(&(darktable.codepath), 0, sizeof(darktable.codepath));

  // first, enable whatever codepath this CPU supports
  {
#ifdef HAVE_BUILTIN_CPU_SUPPORTS
    darktable.codepath.SSE2 = (__builtin_cpu_supports("sse") && __builtin_cpu_supports("sse2"));
#else
    dt_cpu_flags_t flags = dt_detect_cpu_features();
    darktable.codepath.SSE2 = ((flags & (CPU_FLAG_SSE)) && (flags & (CPU_FLAG_SSE2)));
#endif
  }

  // second, apply overrides from conf
  // NOTE: all intrinsics sets can only be overridden to OFF
  if(!dt_conf_get_bool("codepaths/sse2")) darktable.codepath.SSE2 = 0;

  // last: do we have any intrinsics sets enabled?
  darktable.codepath._no_intrinsics = !(darktable.codepath.SSE2);

// if there is no SSE, we must enable plain codepath by default,
// else, enable it conditionally.
#if defined(__SSE__)
  // disabled by default, needs to be manually enabled if needed.
  // disabling all optimized codepaths enables it automatically.
  if(dt_conf_get_bool("codepaths/openmp_simd") || darktable.codepath._no_intrinsics)
#endif
  {
    darktable.codepath.OPENMP_SIMD = 1;
    fprintf(stderr, "[dt_codepaths_init] will be using HIGHLY EXPERIMENTAL plain OpenMP SIMD codepath.\n");
  }

#if defined(__SSE__)
  if(darktable.codepath._no_intrinsics)
#endif
  {
    fprintf(stderr, "[dt_codepaths_init] SSE2-optimized codepath is disabled or unavailable.\n");
    fprintf(stderr,
            "[dt_codepaths_init] expect a LOT of functionality to be broken. you have been warned.\n");
  }
}

int dt_init(int argc, char *argv[], const int init_gui, lua_State *L)
{
#ifndef __WIN32__
  if(getuid() == 0 || geteuid() == 0)
    printf(
        "WARNING: either your user id or the effective user id are 0. are you running darktable as root?\n");
#endif

#if defined(__SSE__)
  // make everything go a lot faster.
  _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
#endif

#if !defined __APPLE__ && !defined __WIN32__
  _dt_sigsegv_old_handler = signal(SIGSEGV, &_dt_sigsegv_handler);
#endif

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "Unfortunately we only work on litte-endian systems."
#endif

#if(defined(__amd64__) || defined(__amd64) || defined(__x86_64__) || defined(__x86_64) || defined(__i386__)  \
    || defined(__i386))
#define DT_SUPPORTED_X86 1
#else
#define DT_SUPPORTED_X86 0
#endif

#if defined(__aarch64__) && defined(__ARM_64BIT_STATE) && defined(__ARM_ARCH) && defined(__ARM_ARCH_8A)
#define DT_SUPPORTED_ARMv8A 1
#else
#define DT_SUPPORTED_ARMv8A 0
#endif

#if !DT_SUPPORTED_X86 && !DT_SUPPORTED_ARMv8A
#error "Unfortunately we only work on amd64/x86 (64-bit and maybe 32-bit) and ARMv8-A (64-bit only)."
#endif

#if !DT_SUPPORTED_X86
#if !defined(__SIZEOF_POINTER__) || __SIZEOF_POINTER__ < 8
#error "On non-x86, we only support 64-bit."
#else
  if(sizeof(void *) < 8)
  {
    fprintf(stderr, "[dt_init] On non-x86, we only support 64-bit.\n");
    return 1;
  }
#endif
#endif

#undef DT_SUPPORTED_ARMv8A
#undef DT_SUPPORTED_X86

#if !defined(__SSE2__) || !defined(__SSE__)
#pragma message "Building without SSE2 is highly experimental."
#pragma message "Expect a LOT of functionality to be broken. You have been warned."
#endif

  int sse2_supported = 0;

#ifdef HAVE_BUILTIN_CPU_SUPPORTS
  // NOTE: _may_i_use_cpu_feature() looks better, but only avaliable in ICC
  __builtin_cpu_init();
  sse2_supported = __builtin_cpu_supports("sse2");
#else
  sse2_supported = dt_detect_cpu_features() & CPU_FLAG_SSE2;
#endif
  if(!sse2_supported)
  {
    fprintf(stderr, "[dt_init] SSE2 instruction set is unavailable.\n");
    fprintf(stderr, "[dt_init] expect a LOT of functionality to be broken. you have been warned.\n");
  }

#ifdef M_MMAP_THRESHOLD
  mallopt(M_MMAP_THRESHOLD, 128 * 1024); /* use mmap() for large allocations */
#endif

  // we have to have our share dir in XDG_DATA_DIRS,
  // otherwise GTK+ won't find our logo for the about screen (and maybe other things)
  {
    const gchar *xdg_data_dirs = g_getenv("XDG_DATA_DIRS");
    gchar *new_xdg_data_dirs = NULL;
    gboolean set_env = TRUE;
    if(xdg_data_dirs != NULL && *xdg_data_dirs != '\0')
    {
      // check if DARKTABLE_SHAREDIR is already in there
      gboolean found = FALSE;
      gchar **tokens = g_strsplit(xdg_data_dirs, ":", 0);
      // xdg_data_dirs is neither NULL nor empty => tokens != NULL
      for(char **iter = tokens; *iter != NULL; iter++)
        if(!strcmp(DARKTABLE_SHAREDIR, *iter))
        {
          found = TRUE;
          break;
        }
      g_strfreev(tokens);
      if(found)
        set_env = FALSE;
      else
        new_xdg_data_dirs = g_strjoin(":", DARKTABLE_SHAREDIR, xdg_data_dirs, NULL);
    }
    else
    {
      // see http://standards.freedesktop.org/basedir-spec/latest/ar01s03.html for a reason to use those as a
      // default
      if(!g_strcmp0(DARKTABLE_SHAREDIR, "/usr/local/share")
         || !g_strcmp0(DARKTABLE_SHAREDIR, "/usr/local/share/")
         || !g_strcmp0(DARKTABLE_SHAREDIR, "/usr/share") || !g_strcmp0(DARKTABLE_SHAREDIR, "/usr/share/"))
        new_xdg_data_dirs = g_strdup("/usr/local/share/:/usr/share/");
      else
        new_xdg_data_dirs = g_strdup_printf("%s:/usr/local/share/:/usr/share/", DARKTABLE_SHAREDIR);
    }

    if(set_env) g_setenv("XDG_DATA_DIRS", new_xdg_data_dirs, 1);
    g_free(new_xdg_data_dirs);
  }

  setlocale(LC_ALL, "");
  bindtextdomain(GETTEXT_PACKAGE, DARKTABLE_LOCALEDIR);
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
  textdomain(GETTEXT_PACKAGE);


  // init all pointers to 0:
  memset(&darktable, 0, sizeof(darktable_t));

  darktable.progname = argv[0];

  // database
  char *dbfilename_from_command = NULL;
  char *noiseprofiles_from_command = NULL;
  char *datadir_from_command = NULL;
  char *moduledir_from_command = NULL;
  char *tmpdir_from_command = NULL;
  char *configdir_from_command = NULL;
  char *cachedir_from_command = NULL;

#ifdef HAVE_OPENCL
  gboolean exclude_opencl = FALSE;
  gboolean print_statistics = strcmp(argv[0], "darktable-cltest");
#endif

#ifdef USE_LUA
  char *lua_command = NULL;
#endif

  darktable.num_openmp_threads = 1;
#ifdef _OPENMP
  darktable.num_openmp_threads = omp_get_num_procs();
#endif
  darktable.unmuted = 0;
  GSList *config_override = NULL;
  for(int k = 1; k < argc; k++)
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
#ifdef USE_LUA
        const char *lua_api_version = strcmp(LUA_API_VERSION_SUFFIX, "") ?
                                      STR(LUA_API_VERSION_MAJOR) "."
                                      STR(LUA_API_VERSION_MINOR) "."
                                      STR(LUA_API_VERSION_PATCH) "-"
                                      LUA_API_VERSION_SUFFIX :
                                      STR(LUA_API_VERSION_MAJOR) "."
                                      STR(LUA_API_VERSION_MINOR) "."
                                      STR(LUA_API_VERSION_PATCH);
#endif
        printf("this is " PACKAGE_STRING "\ncopyright (c) 2009-2015 johannes hanika\n" PACKAGE_BUGREPORT
               "\n\ncompile options:\n"
               "  bit depth is %s\n"
#ifdef _DEBUG
               "  debug build\n"
#else
               "  normal build\n"
#endif
#ifdef _OPENMP
               "  OpenMP support enabled\n"
#else
               "  OpenMP support disabled\n"
#endif

#ifdef HAVE_OPENCL
               "  OpenCL support enabled\n"
#else
               "  OpenCL support disabled\n"
#endif

#ifdef USE_LUA
               "  Lua support enabled, API version %s\n"
#else
               "  Lua support disabled\n"
#endif

#ifdef USE_COLORDGTK
               "  Colord support enabled\n"
#else
               "  Colord support disabled\n"
#endif

#ifdef HAVE_GPHOTO2
               "  gPhoto2 support enabled\n"
#else
               "  gPhoto2 support disabled\n"
#endif

#ifdef HAVE_GRAPHICSMAGICK
               "  GraphicsMagick support enabled\n"
#else
               "  GraphicsMagick support disabled\n"
#endif
               , (sizeof(void *) == 8 ? "64 bit" : sizeof(void *) == 4 ? "32 bit" : "unknown")
#if USE_LUA
               , lua_api_version
#endif
        );
        return 1;
      }
      else if(!strcmp(argv[k], "--library") && argc > k + 1)
      {
        dbfilename_from_command = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--datadir") && argc > k + 1)
      {
        datadir_from_command = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--moduledir") && argc > k + 1)
      {
        moduledir_from_command = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--tmpdir") && argc > k + 1)
      {
        tmpdir_from_command = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--configdir") && argc > k + 1)
      {
        configdir_from_command = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--cachedir") && argc > k + 1)
      {
        cachedir_from_command = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--localedir") && argc > k + 1)
      {
        bindtextdomain(GETTEXT_PACKAGE, argv[++k]);
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(argv[k][1] == 'd' && argc > k + 1)
      {
        if(!strcmp(argv[k + 1], "all"))
          darktable.unmuted = 0xffffffff; // enable all debug information
        else if(!strcmp(argv[k + 1], "cache"))
          darktable.unmuted |= DT_DEBUG_CACHE; // enable debugging for lib/film/cache module
        else if(!strcmp(argv[k + 1], "control"))
          darktable.unmuted |= DT_DEBUG_CONTROL; // enable debugging for scheduler module
        else if(!strcmp(argv[k + 1], "dev"))
          darktable.unmuted |= DT_DEBUG_DEV; // develop module
        else if(!strcmp(argv[k + 1], "fswatch"))
          darktable.unmuted |= DT_DEBUG_FSWATCH; // fswatch module
        else if(!strcmp(argv[k + 1], "input"))
          darktable.unmuted |= DT_DEBUG_INPUT; // input devices
        else if(!strcmp(argv[k + 1], "camctl"))
          darktable.unmuted |= DT_DEBUG_CAMCTL; // camera control module
        else if(!strcmp(argv[k + 1], "perf"))
          darktable.unmuted |= DT_DEBUG_PERF; // performance measurements
        else if(!strcmp(argv[k + 1], "pwstorage"))
          darktable.unmuted |= DT_DEBUG_PWSTORAGE; // pwstorage module
        else if(!strcmp(argv[k + 1], "opencl"))
          darktable.unmuted |= DT_DEBUG_OPENCL; // gpu accel via opencl
        else if(!strcmp(argv[k + 1], "sql"))
          darktable.unmuted |= DT_DEBUG_SQL; // SQLite3 queries
        else if(!strcmp(argv[k + 1], "memory"))
          darktable.unmuted |= DT_DEBUG_MEMORY; // some stats on mem usage now and then.
        else if(!strcmp(argv[k + 1], "lighttable"))
          darktable.unmuted |= DT_DEBUG_LIGHTTABLE; // lighttable related stuff.
        else if(!strcmp(argv[k + 1], "nan"))
          darktable.unmuted |= DT_DEBUG_NAN; // check for NANs when processing the pipe.
        else if(!strcmp(argv[k + 1], "masks"))
          darktable.unmuted |= DT_DEBUG_MASKS; // masks related stuff.
        else if(!strcmp(argv[k + 1], "lua"))
          darktable.unmuted |= DT_DEBUG_LUA; // lua errors are reported on console
        else if(!strcmp(argv[k + 1], "print"))
          darktable.unmuted |= DT_DEBUG_PRINT; // print errors are reported on console
        else if(!strcmp(argv[k + 1], "camsupport"))
          darktable.unmuted |= DT_DEBUG_CAMERA_SUPPORT; // camera support warnings are reported on console
        else
          return usage(argv[0]);
        k++;
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(argv[k][1] == 't' && argc > k + 1)
      {
        darktable.num_openmp_threads = CLAMP(atol(argv[k + 1]), 1, 100);
        printf("[dt_init] using %d threads for openmp parallel sections\n", darktable.num_openmp_threads);
        k++;
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--conf") && argc > k + 1)
      {
        gchar *keyval = g_strdup(argv[++k]), *c = keyval;
        argv[k-1] = NULL;
        argv[k] = NULL;
        gchar *end = keyval + strlen(keyval);
        while(*c != '=' && c < end) c++;
        if(*c == '=' && *(c + 1) != '\0')
        {
          *c++ = '\0';
          dt_conf_string_entry_t *entry = (dt_conf_string_entry_t *)g_malloc(sizeof(dt_conf_string_entry_t));
          entry->key = g_strdup(keyval);
          entry->value = g_strdup(c);
          config_override = g_slist_append(config_override, entry);
        }
        g_free(keyval);
      }
      else if(!strcmp(argv[k], "--noiseprofiles") && argc > k + 1)
      {
        noiseprofiles_from_command = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--luacmd") && argc > k + 1)
      {
#ifdef USE_LUA
        lua_command = argv[++k];
#else
        ++k;
#endif
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--disable-opencl"))
      {
#ifdef HAVE_OPENCL
        exclude_opencl = TRUE;
#endif
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--"))
      {
        // "--" confuses the argument parser of glib/gtk. remove it.
        argv[k] = NULL;
        break;
      }
      else
        return usage(argv[0]); // fail on unrecognized options
    }
  }

  // remove the NULLs to not confuse gtk_init() later.
  for(int i = 1; i < argc; i++)
  {
    int k;
    for(k = i; k < argc; k++)
      if(argv[k] != NULL) break;

    if(k > i)
    {
      k -= i;
      for(int j = i + k; j < argc; j++)
      {
        argv[j-k] = argv[j];
        argv[j] = NULL;
      }
      argc -= k;
    }
  }

  if(darktable.unmuted & DT_DEBUG_MEMORY)
  {
    fprintf(stderr, "[memory] at startup\n");
    dt_print_mem_usage();
  }

#ifdef _OPENMP
  omp_set_num_threads(darktable.num_openmp_threads);
#endif
  dt_loc_init_datadir(datadir_from_command);
  dt_loc_init_plugindir(moduledir_from_command);
  if(dt_loc_init_tmp_dir(tmpdir_from_command))
  {
    fprintf(stderr, "error: invalid temporary directory: %s\n", darktable.tmpdir);
    return usage(argv[0]);
  }
  dt_loc_init_user_config_dir(configdir_from_command);
  dt_loc_init_user_cache_dir(cachedir_from_command);

#ifdef USE_LUA
  dt_lua_init_early(L);
#endif

  // thread-safe init:
  dt_exif_init();
  char datadir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(datadir, sizeof(datadir));
  char filename[PATH_MAX] = { 0 };
  snprintf(filename, sizeof(filename), "%s/darktablerc", datadir);

  // initialize the config backend. this needs to be done first...
  darktable.conf = (dt_conf_t *)calloc(1, sizeof(dt_conf_t));
  dt_conf_init(darktable.conf, filename, config_override);
  g_slist_free_full(config_override, g_free);

  // set the interface language
  const gchar *lang = dt_conf_get_string("ui_last/gui_language");
  if(lang != NULL && lang[0] != '\0')
  {
    setenv("LANGUAGE", lang, 1);
    if(setlocale(LC_ALL, lang) != NULL) gtk_disable_setlocale();
    setlocale(LC_MESSAGES, lang);
    setenv("LANG", lang, 1);
  }
  g_free((gchar *)lang);

  // detect cpu features and decide which codepaths to enable
  dt_codepaths_init();

  // get the list of color profiles
  darktable.color_profiles = dt_colorspaces_init();

  // initialize the database
  darktable.db = dt_database_init(dbfilename_from_command);
  if(darktable.db == NULL)
  {
    printf("ERROR : cannot open database\n");
    return 1;
  }
  else if(!dt_database_get_lock_acquired(darktable.db))
  {
#ifndef MAC_INTEGRATION
    // send the images to the other instance via dbus
    fprintf(stderr, "trying to open the images in the running instance\n");

    GDBusConnection *connection = NULL;
    for(int i = 1; i < argc; i++)
    {
      // make the filename absolute ...
      if(argv[i] == NULL || *argv[i] == '\0') continue;
      gchar *filename = dt_make_path_absolute(argv[i]);
      if(filename == NULL) continue;
      if(!connection) connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
      // ... and send it to the running instance of darktable
      g_dbus_connection_call_sync(connection, "org.darktable.service", "/darktable",
                                  "org.darktable.service.Remote", "Open", g_variant_new("(s)", filename),
                                  NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
      g_free(filename);
    }
    if(connection) g_object_unref(connection);
#endif

    return 1;
  }

  // Initialize the signal system
  darktable.signals = dt_control_signal_init();

  // Make sure that the database and xmp files are in sync before starting the fswatch.
  // We need conf and db to be up and running for that which is the case here.
  // FIXME: is this also useful in non-gui mode?
  GList *changed_xmp_files = NULL;
  if(init_gui && dt_conf_get_bool("run_crawler_on_start"))
  {
    changed_xmp_files = dt_control_crawler_run();
  }

  // Initialize the filesystem watcher
  darktable.fswatch = dt_fswatch_new();

  // FIXME: move there into dt_database_t
  dt_pthread_mutex_init(&(darktable.db_insert), NULL);
  dt_pthread_mutex_init(&(darktable.plugin_threadsafe), NULL);
  dt_pthread_mutex_init(&(darktable.capabilities_threadsafe), NULL);
  darktable.control = (dt_control_t *)calloc(1, sizeof(dt_control_t));
  if(init_gui)
  {
    dt_control_init(darktable.control);
  }
  else
  {
    if(dbfilename_from_command && !strcmp(dbfilename_from_command, ":memory:"))
      dt_gui_presets_init(); // init preset db schema.
    darktable.control->running = 0;
    darktable.control->accelerators = NULL;
    dt_pthread_mutex_init(&darktable.control->run_mutex, NULL);
  }

  // initialize collection query
  darktable.collection = dt_collection_new(NULL);

  /* initialize selection */
  darktable.selection = dt_selection_new();

  /* capabilities set to NULL */
  darktable.capabilities = NULL;

  // Initialize the password storage engine
  darktable.pwstorage = dt_pwstorage_new();

  darktable.guides = dt_guides_init();

#ifdef HAVE_GRAPHICSMAGICK
  /* GraphicsMagick init */
  InitializeMagick(darktable.progname);
#endif

  darktable.opencl = (dt_opencl_t *)calloc(1, sizeof(dt_opencl_t));
#ifdef HAVE_OPENCL
  dt_opencl_init(darktable.opencl, exclude_opencl, print_statistics);
#endif

  darktable.blendop = (dt_blendop_t *)calloc(1, sizeof(dt_blendop_t));
  dt_develop_blend_init(darktable.blendop);

  darktable.points = (dt_points_t *)calloc(1, sizeof(dt_points_t));
  dt_points_init(darktable.points, dt_get_num_threads());

  darktable.noiseprofile_parser = dt_noiseprofile_init(noiseprofiles_from_command);

  // must come before mipmap_cache, because that one will need to access
  // image dimensions stored in here:
  darktable.image_cache = (dt_image_cache_t *)calloc(1, sizeof(dt_image_cache_t));
  dt_image_cache_init(darktable.image_cache);

  darktable.mipmap_cache = (dt_mipmap_cache_t *)calloc(1, sizeof(dt_mipmap_cache_t));
  dt_mipmap_cache_init(darktable.mipmap_cache);

  // The GUI must be initialized before the views, because the init()
  // functions of the views depend on darktable.control->accels_* to register
  // their keyboard accelerators

  if(init_gui)
  {
    darktable.gui = (dt_gui_gtk_t *)calloc(1, sizeof(dt_gui_gtk_t));
    if(dt_gui_gtk_init(darktable.gui, argc, argv)) return 1;
    dt_bauhaus_init();
  }
  else
    darktable.gui = NULL;

  darktable.view_manager = (dt_view_manager_t *)calloc(1, sizeof(dt_view_manager_t));
  dt_view_manager_init(darktable.view_manager);

  // check whether we were able to load darkroom view. if we failed, we'll crash everywhere later on.
  if(!darktable.develop) return 1;

  darktable.imageio = (dt_imageio_t *)calloc(1, sizeof(dt_imageio_t));
  dt_imageio_init(darktable.imageio);

  // load the darkroom mode plugins once:
  dt_iop_load_modules_so();

#ifdef HAVE_GPHOTO2
  // Initialize the camera control.
  // this is done late so that the gui can react to the signal sent but before switching to lighttable!
  darktable.camctl = dt_camctl_new();
#endif

  if(init_gui)
  {
    darktable.lib = (dt_lib_t *)calloc(1, sizeof(dt_lib_t));
    dt_lib_init(darktable.lib);

    dt_control_load_config(darktable.control);
  }

  dt_control_gui_mode_t mode = DT_LIBRARY;
  // april 1st: you have to earn using dt first! or know that you can switch views with keyboard shortcuts
  time_t now;
  time(&now);
  struct tm lt;
  localtime_r(&now, &lt);
  if(lt.tm_mon == 3 && lt.tm_mday == 1) mode = DT_KNIGHT;
  if(init_gui)
  {
    // init the gui part of views
    dt_view_manager_gui_init(darktable.view_manager);
    // Loading the keybindings
    char keyfile[PATH_MAX] = { 0 };

    // First dump the default keymapping
    snprintf(keyfile, sizeof(keyfile), "%s/keyboardrc_default", datadir);
    gtk_accel_map_save(keyfile);

    // Removing extraneous semi-colons from the default keymap
    strip_semicolons_from_keymap(keyfile);

    // Then load any modified keys if available
    snprintf(keyfile, sizeof(keyfile), "%s/keyboardrc", datadir);
    if(g_file_test(keyfile, G_FILE_TEST_EXISTS))
      gtk_accel_map_load(keyfile);
    else
      gtk_accel_map_save(keyfile); // Save the default keymap if none is present

    // I doubt that connecting to dbus for darktable-cli makes sense
    darktable.dbus = dt_dbus_init();

    // initialize undo struct
    darktable.undo = dt_undo_init();

#ifndef MAC_INTEGRATION
    // load image(s) specified on cmdline
    // If only one image is listed, attempt to load it in darkroom
    int last_id = 0;
    gboolean only_single_images = TRUE;
    int loaded_images = 0;

    for(int i = 1; i < argc; i++)
    {
      gboolean single_image = FALSE;
      if(argv[i] == NULL || *argv[i] == '\0') continue;
      int new_id = dt_load_from_string(argv[i], FALSE, &single_image);
      if(new_id > 0)
      {
        last_id = new_id;
        loaded_images++;
        if(!single_image) only_single_images = FALSE;
      }
    }

    if(loaded_images == 1 && only_single_images)
    {
      dt_control_set_mouse_over_id(last_id);
      mode = DT_DEVELOP;
    }
#endif
  }

  if(darktable.unmuted & DT_DEBUG_MEMORY)
  {
    fprintf(stderr, "[memory] after successful startup\n");
    dt_print_mem_usage();
  }

  dt_image_local_copy_synch();

/* init lua last, since it's user made stuff it must be in the real environment */
#ifdef USE_LUA
  dt_lua_init(darktable.lua_state.state, lua_command);
#endif

  if(init_gui) dt_ctl_switch_mode_to(mode);

  // last but not least construct the popup that asks the user about images whose xmp files are newer than the
  // db entry
  if(init_gui && changed_xmp_files)
  {
    dt_control_crawler_show_image_list(changed_xmp_files);
  }

  return 0;
}

void dt_cleanup()
{
  const int init_gui = (darktable.gui != NULL);

#ifdef HAVE_PRINT
  dt_printers_abort_discovery();
#endif

#ifdef USE_LUA
  dt_lua_finalize_early();
#endif
  if(init_gui)
  {
    dt_ctl_switch_mode_to(DT_MODE_NONE);
    dt_dbus_destroy(darktable.dbus);

    dt_control_write_config(darktable.control);
    dt_control_shutdown(darktable.control);

    dt_lib_cleanup(darktable.lib);
    free(darktable.lib);
  }
#ifdef USE_LUA
  dt_lua_finalize();
#endif
  dt_view_manager_cleanup(darktable.view_manager);
  free(darktable.view_manager);
  if(init_gui)
  {
    dt_imageio_cleanup(darktable.imageio);
    free(darktable.imageio);
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
    dt_undo_cleanup(darktable.undo);
  }
  dt_colorspaces_cleanup(darktable.color_profiles);
  dt_conf_cleanup(darktable.conf);
  free(darktable.conf);
  dt_points_cleanup(darktable.points);
  free(darktable.points);
  dt_iop_unload_modules_so();
  dt_opencl_cleanup(darktable.opencl);
  free(darktable.opencl);
#ifdef HAVE_GPHOTO2
  dt_camctl_destroy((dt_camctl_t *)darktable.camctl);
#endif
  dt_pwstorage_destroy(darktable.pwstorage);
  dt_fswatch_destroy(darktable.fswatch);

#ifdef HAVE_GRAPHICSMAGICK
  DestroyMagick();
#endif

  dt_guides_cleanup(darktable.guides);

  dt_database_destroy(darktable.db);

  if(init_gui)
  {
    dt_bauhaus_cleanup();
  }

  dt_capabilities_cleanup();

  dt_pthread_mutex_destroy(&(darktable.db_insert));
  dt_pthread_mutex_destroy(&(darktable.plugin_threadsafe));
  dt_pthread_mutex_destroy(&(darktable.capabilities_threadsafe));

  dt_exif_cleanup();
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

void dt_gettime_t(char *datetime, size_t datetime_len, time_t t)
{
  struct tm tt;
  (void)localtime_r(&t, &tt);
  strftime(datetime, datetime_len, "%Y:%m:%d %H:%M:%S", &tt);
}

void dt_gettime(char *datetime, size_t datetime_len)
{
  dt_gettime_t(datetime, datetime_len, time(NULL));
}

void *dt_alloc_align(size_t alignment, size_t size)
{
#if defined(__FreeBSD_version) && __FreeBSD_version < 700013
  return malloc(size);
#elif defined(__WIN32__)
  return _aligned_malloc(size, alignment);
#else
  void *ptr = NULL;
  if(posix_memalign(&ptr, alignment, size)) return NULL;
  return ptr;
#endif
}

#ifdef __WIN32__
void dt_free_align(void *mem)
{
  _aligned_free(mem);
}
#endif

void dt_show_times(const dt_times_t *start, const char *prefix, const char *suffix, ...)
{
  dt_times_t end;
  char buf[160]; /* Arbitrary size, should be lots big enough for everything used in DT */
  int i;

  /* Skip all the calculations an everything if -d perf isn't on */
  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_get_times(&end);
    i = snprintf(buf, sizeof(buf), "%s took %.3f secs (%.3f CPU)", prefix, end.clock - start->clock,
                 end.user - start->user);
    if(suffix != NULL)
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
  const int bits = (sizeof(void *) == 4) ? 32 : 64;
  fprintf(stderr, "[defaults] found a %d-bit system with %zu kb ram and %d cores (%d atom based)\n", bits,
          mem, threads, atom_cores);
  if(mem > (2u << 20) && threads > 4)
  {
    fprintf(stderr, "[defaults] setting high quality defaults\n");
    dt_conf_set_int("worker_threads", 8);
    dt_conf_set_bool("plugins/lighttable/low_quality_thumbnails", FALSE);
  }
  if(mem < (1u << 20) || threads <= 2 || bits < 64 || atom_cores > 0)
  {
    fprintf(stderr, "[defaults] setting very conservative defaults\n");
    dt_conf_set_int("worker_threads", 1);
    dt_conf_set_int("host_memory_limit", 500);
    dt_conf_set_int("singlebuffer_limit", 8);
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
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
