/*
    This file is part of darktable,
    Copyright (C) 2009-2022 darktable developers.

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

#include "is_supported_platform.h"

#if !defined(__APPLE__) && !defined(__FreeBSD__) && !defined(__OpenBSD__) && !defined(__DragonFly__)
#include <malloc.h>
#endif
#ifdef __APPLE__
#include <sys/malloc.h>
#endif

#include "common/collection.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/datetime.h"
#include "common/exif.h"
#include "common/pwstorage/pwstorage.h"
#include "common/selection.h"
#include "common/system_signal_handling.h"
#ifdef HAVE_GPHOTO2
#include "common/camera_control.h"
#endif
#include "bauhaus/bauhaus.h"
#include "common/action.h"
#include "common/cpuid.h"
#include "common/file_location.h"
#include "common/film.h"
#include "common/grealpath.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/imageio_module.h"
#include "common/iop_order.h"
#include "common/l10n.h"
#include "common/mipmap_cache.h"
#include "common/noiseprofiles.h"
#include "common/opencl.h"
#include "common/points.h"
#include "common/resource_limits.h"
#include "common/undo.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/crawler.h"
#include "control/jobs/control_jobs.h"
#include "control/jobs/film_jobs.h"
#include "control/signal.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/guides.h"
#include "gui/presets.h"
#include "libs/lib.h"
#include "lua/init.h"
#include "views/view.h"
#include "conf_gen.h"

#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/types.h>
#include <unistd.h>
#include <locale.h>
#include <limits.h>

#if defined(__SSE__)
#include <xmmintrin.h>
#endif

#ifdef HAVE_GRAPHICSMAGICK
#include <magick/api.h>
#elif defined HAVE_IMAGEMAGICK
#include <MagickWand/MagickWand.h>
#endif

#include "dbus.h"

#if defined(__SUNOS__)
#include <sys/varargs.h>
#endif

#ifdef _OPENMP
#include <omp.h>
#endif

#ifdef USE_LUA
#include "lua/configuration.h"
#endif

darktable_t darktable;

static int usage(const char *argv0)
{
#ifdef _WIN32
  char *logfile = g_build_filename(g_get_user_cache_dir(), "darktable", "darktable-log.txt", NULL);
#endif
  // clang-format off
  printf("usage: %s [options] [IMG_1234.{RAW,..}|image_folder/]\n", argv0);
  printf("\n");
  printf("options:\n");
  printf("\n");
  printf("  --cachedir <user cache directory>\n");
  printf("  --conf <key>=<value>\n");
  printf("  --configdir <user config directory>\n");
  printf("  -d {all,act_on,cache,camctl,camsupport,control,demosaic,dev,imageio,\n");
  printf("      input,ioporder,lighttable,lua,masks,memory,nan,opencl,params,\n");
  printf("      perf,print,pwstorage,signal,sql,tiling,undo,verbose}\n");
  printf("  --d-signal <signal> \n");
  printf("  --d-signal-act <all,raise,connect,disconnect");
  // clang-format on
#ifdef DT_HAVE_SIGNAL_TRACE
  printf(",print-trace");
#endif
  printf(">\n");
  printf("  --datadir <data directory>\n");
#ifdef HAVE_OPENCL
  printf("  --disable-opencl\n");
#endif
  printf("  -h, --help");
#ifdef _WIN32
  printf(", /?");
#endif
  printf("\n");
  printf("  --library <library file>\n");
  printf("  --localedir <locale directory>\n");
#ifdef USE_LUA
  printf("  --luacmd <lua command>\n");
#endif
  printf("  --moduledir <module directory>\n");
  printf("  --noiseprofiles <noiseprofiles json file>\n");
  printf("  -t <num openmp threads>\n");
  printf("  --tmpdir <tmp directory>\n");
  printf("  --version\n");
#ifdef _WIN32
  printf("\n");
  printf("  note: debug log and output will be written to this file:\n");
  printf("        %s\n", logfile);
#endif

#ifdef _WIN32
  g_free(logfile);
#endif

  return 1;
}

gboolean dt_is_dev_version()
{
  // a dev version as an odd number after the first dot
  char *p = (char *)darktable_package_string;
  while(*p && (*p != '.')) p++;
  if(p && (*p != '\0'))
  {
    p++;
    const int val = *p - '0';
    return val % 2 == 0 ? FALSE : TRUE;
  }
  return FALSE;
}

char *dt_version_major_minor()
{
  char ver[100] = { 0 };
  g_strlcpy(ver, darktable_package_string, sizeof(ver));
  int count = -1;
  char *start = ver;
  for(char *p = ver; *p; p++)
  {
    // first look for a number
    if(count == -1)
    {
      if(*p >= '0' && *p <= '9')
      {
        count++;
        start = p;
      }
    }
    // then check for <major>.<minor>
    else
    {
      if(*p == '.' || *p == '+') count++;
      if(count == 2)
      {
        *p = '\0';
        break;
      }
    }
  }
  return g_strdup(start);
}

gboolean dt_supported_image(const gchar *filename)
{
  gboolean supported = FALSE;
  char *ext = g_strrstr(filename, ".");
  if(!ext)
    return FALSE;
  ext++;
  for(const char **i = dt_supported_extensions; *i != NULL; i++)
    if(!g_ascii_strncasecmp(ext, *i, strlen(*i)))
    {
      supported = TRUE;
      break;
    }
  return supported;
}

int dt_load_from_string(const gchar *input, gboolean open_image_in_dr, gboolean *single_image)
{
  int32_t id = 0;
  if(input == NULL || input[0] == '\0') return 0;

  char *filename = dt_util_normalize_path(input);

  if(filename == NULL)
  {
    dt_control_log(_("found strange path `%s'"), input);
    return 0;
  }

  if(g_file_test(filename, G_FILE_TEST_IS_DIR))
  {
    // import a directory into a film roll
    id = dt_film_import(filename);
    if(id)
    {
      dt_film_open(id);
      dt_ctl_switch_mode_to("lighttable");
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
    id = dt_image_import(filmid, filename, TRUE, TRUE);
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
          dt_ctl_switch_mode_to("darkroom");
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
    fprintf(stderr, "[dt_codepaths_init] will be using experimental plain OpenMP SIMD codepath.\n");
  }

#if defined(__SSE__)
  if(darktable.codepath._no_intrinsics)
  {
    fprintf(stderr, "[dt_codepaths_init] SSE2-optimized codepath is disabled or unavailable.\n");
  }
#endif
}

static inline size_t _get_total_memory()
{
#if defined(__linux__)
  FILE *f = g_fopen("/proc/meminfo", "rb");
  if(!f) return 0;
  size_t mem = 0;
  char *line = NULL;
  size_t len = 0;
  int first = 1, found = 0;
  // return "MemTotal" or the value from the first line
  while(!found && getline(&line, &len, f) != -1)
  {
    char *colon = strchr(line, ':');
    if(!colon) continue;
    found = !strncmp(line, "MemTotal:", 9);
    if(found || first) mem = atol(colon + 1);
    first = 0;
  }
  fclose(f);
  if(len > 0) free(line);
  return mem;
#elif defined(__APPLE__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__NetBSD__)            \
    || defined(__OpenBSD__)
#if defined(__APPLE__)
  int mib[2] = { CTL_HW, HW_MEMSIZE };
#elif defined(HW_PHYSMEM64)
  int mib[2] = { CTL_HW, HW_PHYSMEM64 };
#else
  int mib[2] = { CTL_HW, HW_PHYSMEM };
#endif
  uint64_t physical_memory;
  size_t length = sizeof(uint64_t);
  sysctl(mib, 2, (void *)&physical_memory, &length, (void *)NULL, 0);
  return physical_memory / 1024;
#elif defined _WIN32
  MEMORYSTATUSEX memInfo;
  memInfo.dwLength = sizeof(MEMORYSTATUSEX);
  GlobalMemoryStatusEx(&memInfo);
  return memInfo.ullTotalPhys / (uint64_t)1024;
#else
  // assume 2GB until we have a better solution.
  fprintf(stderr, "Unknown memory size. Assuming 2GB\n");
  return 2097152;
#endif
}

static size_t _get_mipmap_size()
{
  dt_sys_resources_t *res = &darktable.dtresources;
  const int level = res->level;
  if(level < 0)
    return res->refresource[4*(-level-1) + 2] * 1024lu * 1024lu;
  const int fraction = res->fractions[res->group + 2];
  return res->total_memory / 1024lu * fraction;
}

void check_resourcelevel(const char *key, int *fractions, const int level)
{
  const int g = level * 4;
  gchar out[128] = { 0 };
  if(!dt_conf_key_exists(key))
  {
    g_snprintf(out, 126, "%i %i %i %i", fractions[g], fractions[g+1], fractions[g+2], fractions[g+3]);
    dt_conf_set_string(key, out);
  }
  else
  {
    const gchar *in = dt_conf_get_string_const(key);
    sscanf(in, "%i %i %i %i", &fractions[g], &fractions[g+1], &fractions[g+2], &fractions[g+3]);
  }
}

int dt_init(int argc, char *argv[], const gboolean init_gui, const gboolean load_data, lua_State *L)
{
  double start_wtime = dt_get_wtime();

#ifndef _WIN32
  if(getuid() == 0 || geteuid() == 0)
    printf(
        "WARNING: either your user id or the effective user id are 0. are you running darktable as root?\n");
#endif

#if defined(__SSE__)
  // make everything go a lot faster.
  _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
#endif

  dt_set_signal_handlers();

  int sse2_supported = 0;

#ifdef HAVE_BUILTIN_CPU_SUPPORTS
  // NOTE: _may_i_use_cpu_feature() looks better, but only available in ICC
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

  // make sure that stack/frame limits are good (musl)
  dt_set_rlimits();

  // init all pointers to 0:
  memset(&darktable, 0, sizeof(darktable_t));

  darktable.start_wtime = start_wtime;

  darktable.progname = argv[0];

  // FIXME: move there into dt_database_t
  pthread_mutexattr_t recursive_locking;
  pthread_mutexattr_init(&recursive_locking);
  pthread_mutexattr_settype(&recursive_locking, PTHREAD_MUTEX_RECURSIVE);
  for (int k=0; k<DT_IMAGE_DBLOCKS; k++)
  {
    dt_pthread_mutex_init(&(darktable.db_image[k]),&(recursive_locking));
  }
  dt_pthread_mutex_init(&(darktable.plugin_threadsafe), NULL);
  dt_pthread_mutex_init(&(darktable.dev_threadsafe), NULL);
  dt_pthread_mutex_init(&(darktable.capabilities_threadsafe), NULL);
  dt_pthread_mutex_init(&(darktable.exiv2_threadsafe), NULL);
  dt_pthread_mutex_init(&(darktable.readFile_mutex), NULL);
  darktable.control = (dt_control_t *)calloc(1, sizeof(dt_control_t));

  // database
  char *dbfilename_from_command = NULL;
  char *noiseprofiles_from_command = NULL;
  char *datadir_from_command = NULL;
  char *moduledir_from_command = NULL;
  char *localedir_from_command = NULL;
  char *tmpdir_from_command = NULL;
  char *configdir_from_command = NULL;
  char *cachedir_from_command = NULL;

#ifdef HAVE_OPENCL
  gboolean exclude_opencl = FALSE;
  gboolean print_statistics = (strstr(argv[0], "darktable-cltest") == NULL);
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
#ifdef _WIN32
    if(!strcmp(argv[k], "/?"))
    {
      return usage(argv[0]);
    }
#endif
    if(argv[k][0] == '-')
    {
      if(!strcmp(argv[k], "--help") || !strcmp(argv[k], "-h"))
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
        printf("this is %s\ncopyright (c) 2009-%s johannes hanika\n" PACKAGE_BUGREPORT "\n\ncompile options:\n"
               "  bit depth is %zu bit\n"
#ifdef _DEBUG
               "  debug build\n"
#else
               "  normal build\n"
#endif
#if defined(__SSE2__) && defined(__SSE__)
               "  SSE2 optimized codepath enabled\n"
#else
               "  SSE2 optimized codepath disabled\n"
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

#ifdef HAVE_IMAGEMAGICK
               "  ImageMagick support enabled\n"
#else
               "  ImageMagick support disabled\n"
#endif

#ifdef HAVE_OPENEXR
               "  OpenEXR support enabled\n"
#else
               "  OpenEXR support disabled\n"
#endif
               ,
               darktable_package_string,
               darktable_last_commit_year,
               CHAR_BIT * sizeof(void *)
#if USE_LUA
                   ,
               lua_api_version
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
        localedir_from_command = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(argv[k][1] == 'd' && argc > k + 1)
      {
        if(!strcmp(argv[k + 1], "all"))
          darktable.unmuted = 0xffffffff & ~DT_DEBUG_VERBOSE; // enable all debug information except verbose
        else if(!strcmp(argv[k + 1], "cache"))
          darktable.unmuted |= DT_DEBUG_CACHE; // enable debugging for lib/film/cache module
        else if(!strcmp(argv[k + 1], "control"))
          darktable.unmuted |= DT_DEBUG_CONTROL; // enable debugging for scheduler module
        else if(!strcmp(argv[k + 1], "dev"))
          darktable.unmuted |= DT_DEBUG_DEV; // develop module
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
        else if(!strcmp(argv[k + 1], "ioporder"))
          darktable.unmuted |= DT_DEBUG_IOPORDER; // iop order information are reported on console
        else if(!strcmp(argv[k + 1], "imageio"))
          darktable.unmuted |= DT_DEBUG_IMAGEIO; // image importing or exporting messages on console
        else if(!strcmp(argv[k + 1], "undo"))
          darktable.unmuted |= DT_DEBUG_UNDO; // undo/redo
        else if(!strcmp(argv[k + 1], "signal"))
          darktable.unmuted |= DT_DEBUG_SIGNAL; // signal information on console
        else if(!strcmp(argv[k + 1], "params"))
          darktable.unmuted |= DT_DEBUG_PARAMS; // iop module params checks on console
        else if(!strcmp(argv[k + 1], "demosaic"))
          darktable.unmuted |= DT_DEBUG_DEMOSAIC;
        else if(!strcmp(argv[k + 1], "act_on"))
          darktable.unmuted |= DT_DEBUG_ACT_ON;
        else if(!strcmp(argv[k + 1], "tiling"))
          darktable.unmuted |= DT_DEBUG_TILING;
        else if(!strcmp(argv[k + 1], "verbose"))
          darktable.unmuted |= DT_DEBUG_VERBOSE;
        else
          return usage(argv[0]);
        k++;
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--d-signal-act") && argc > k + 1)
      {
        if(!strcmp(argv[k + 1], "all"))
          darktable.unmuted_signal_dbg_acts = 0xffffffff; // enable all signal debug information
        else if(!strcmp(argv[k + 1], "raise"))
          darktable.unmuted_signal_dbg_acts |= DT_DEBUG_SIGNAL_ACT_RAISE; // enable debugging for signal raising
        else if(!strcmp(argv[k + 1], "connect"))
          darktable.unmuted_signal_dbg_acts |= DT_DEBUG_SIGNAL_ACT_CONNECT; // enable debugging for signal connection
        else if(!strcmp(argv[k + 1], "disconnect"))
          darktable.unmuted_signal_dbg_acts |= DT_DEBUG_SIGNAL_ACT_DISCONNECT; // enable debugging for signal disconnection
        else if(!strcmp(argv[k + 1], "print-trace"))
        {
#ifdef DT_HAVE_SIGNAL_TRACE
          darktable.unmuted_signal_dbg_acts |= DT_DEBUG_SIGNAL_ACT_PRINT_TRACE; // enable printing of signal tracing
#else
          fprintf(stderr, "[signal] print-trace not available, skipping\n");
#endif
        }
        else
          return usage(argv[0]);
        k++;
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--d-signal") && argc > k + 1)
      {
        gchar *str = g_ascii_strup(argv[k+1], -1);

        #define CHKSIGDBG(sig) else if(!g_strcmp0(str, #sig)) do {darktable.unmuted_signal_dbg[sig] = TRUE;} while (0)
        if(!g_strcmp0(str, "ALL"))
        {
          for(int sig=0; sig<DT_SIGNAL_COUNT; sig++)
            darktable.unmuted_signal_dbg[sig] = TRUE;
        }
        CHKSIGDBG(DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE);
        CHKSIGDBG(DT_SIGNAL_ACTIVE_IMAGES_CHANGE);
        CHKSIGDBG(DT_SIGNAL_CONTROL_REDRAW_ALL);
        CHKSIGDBG(DT_SIGNAL_CONTROL_REDRAW_CENTER);
        CHKSIGDBG(DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED);
        CHKSIGDBG(DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE);
        CHKSIGDBG(DT_SIGNAL_COLLECTION_CHANGED);
        CHKSIGDBG(DT_SIGNAL_SELECTION_CHANGED);
        CHKSIGDBG(DT_SIGNAL_TAG_CHANGED);
        CHKSIGDBG(DT_SIGNAL_METADATA_CHANGED);
        CHKSIGDBG(DT_SIGNAL_IMAGE_INFO_CHANGED);
        CHKSIGDBG(DT_SIGNAL_STYLE_CHANGED);
        CHKSIGDBG(DT_SIGNAL_IMAGES_ORDER_CHANGE);
        CHKSIGDBG(DT_SIGNAL_FILMROLLS_CHANGED);
        CHKSIGDBG(DT_SIGNAL_FILMROLLS_IMPORTED);
        CHKSIGDBG(DT_SIGNAL_FILMROLLS_REMOVED);
        CHKSIGDBG(DT_SIGNAL_DEVELOP_INITIALIZE);
        CHKSIGDBG(DT_SIGNAL_DEVELOP_MIPMAP_UPDATED);
        CHKSIGDBG(DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED);
        CHKSIGDBG(DT_SIGNAL_DEVELOP_PREVIEW2_PIPE_FINISHED);
        CHKSIGDBG(DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED);
        CHKSIGDBG(DT_SIGNAL_DEVELOP_HISTORY_WILL_CHANGE);
        CHKSIGDBG(DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
        CHKSIGDBG(DT_SIGNAL_DEVELOP_MODULE_REMOVE);
        CHKSIGDBG(DT_SIGNAL_DEVELOP_MODULE_MOVED);
        CHKSIGDBG(DT_SIGNAL_DEVELOP_IMAGE_CHANGED);
        CHKSIGDBG(DT_SIGNAL_CONTROL_PROFILE_CHANGED);
        CHKSIGDBG(DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED);
        CHKSIGDBG(DT_SIGNAL_IMAGE_IMPORT);
        CHKSIGDBG(DT_SIGNAL_IMAGE_EXPORT_TMPFILE);
        CHKSIGDBG(DT_SIGNAL_IMAGEIO_STORAGE_CHANGE);
        CHKSIGDBG(DT_SIGNAL_PREFERENCES_CHANGE);
        CHKSIGDBG(DT_SIGNAL_CAMERA_DETECTED);
        CHKSIGDBG(DT_SIGNAL_CONTROL_NAVIGATION_REDRAW);
        CHKSIGDBG(DT_SIGNAL_CONTROL_LOG_REDRAW);
        CHKSIGDBG(DT_SIGNAL_CONTROL_TOAST_REDRAW);
        CHKSIGDBG(DT_SIGNAL_CONTROL_PICKERDATA_READY);
        CHKSIGDBG(DT_SIGNAL_METADATA_UPDATE);
        else
        {
          fprintf(stderr, "unknown signal name: '%s'. use 'ALL' to enable debug for all or use full signal name\n", str);
          return usage(argv[0]);
        }
        g_free(str);
        #undef CHKSIGDBG
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
#ifdef __APPLE__
      else if(!strncmp(argv[k], "-psn_", 5))
      {
        // "-psn_*" argument is added automatically by macOS and should be ignored
        argv[k] = NULL;
      }
#endif
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

  // get valid directories
  dt_loc_init(datadir_from_command, moduledir_from_command, localedir_from_command, configdir_from_command, cachedir_from_command, tmpdir_from_command);

  if(darktable.unmuted & DT_DEBUG_MEMORY)
  {
    fprintf(stderr, "[memory] at startup\n");
    dt_print_mem_usage();
  }

  char sharedir[PATH_MAX] = { 0 };
  dt_loc_get_sharedir(sharedir, sizeof(sharedir));

  // we have to have our share dir in XDG_DATA_DIRS,
  // otherwise GTK+ won't find our logo for the about screen (and maybe other things)
  {
    const gchar *xdg_data_dirs = g_getenv("XDG_DATA_DIRS");
    gchar *new_xdg_data_dirs = NULL;
    gboolean set_env = TRUE;
    if(xdg_data_dirs != NULL && *xdg_data_dirs != '\0')
    {
      // check if sharedir is already in there
      gboolean found = FALSE;
      gchar **tokens = g_strsplit(xdg_data_dirs, G_SEARCHPATH_SEPARATOR_S, 0);
      // xdg_data_dirs is neither NULL nor empty => tokens != NULL
      for(char **iter = tokens; *iter != NULL; iter++)
        if(!strcmp(sharedir, *iter))
        {
          found = TRUE;
          break;
        }
      g_strfreev(tokens);
      if(found)
        set_env = FALSE;
      else
        new_xdg_data_dirs = g_strjoin(G_SEARCHPATH_SEPARATOR_S, sharedir, xdg_data_dirs, NULL);
    }
    else
    {
#ifndef _WIN32
      // see http://standards.freedesktop.org/basedir-spec/latest/ar01s03.html for a reason to use those as a
      // default
      if(!g_strcmp0(sharedir, "/usr/local/share")
         || !g_strcmp0(sharedir, "/usr/local/share/")
         || !g_strcmp0(sharedir, "/usr/share") || !g_strcmp0(sharedir, "/usr/share/"))
        new_xdg_data_dirs = g_strdup("/usr/local/share/" G_SEARCHPATH_SEPARATOR_S "/usr/share/");
      else
        new_xdg_data_dirs = g_strdup_printf("%s" G_SEARCHPATH_SEPARATOR_S "/usr/local/share/" G_SEARCHPATH_SEPARATOR_S
                                            "/usr/share/", sharedir);
#else
      set_env = FALSE;
#endif
    }

    if(set_env) g_setenv("XDG_DATA_DIRS", new_xdg_data_dirs, 1);
    dt_print(DT_DEBUG_DEV, "new_xdg_data_dirs: %s\n", new_xdg_data_dirs);
    g_free(new_xdg_data_dirs);
  }

  setlocale(LC_ALL, "");
  char localedir[PATH_MAX] = { 0 };
  dt_loc_get_localedir(localedir, sizeof(localedir));
  bindtextdomain(GETTEXT_PACKAGE, localedir);
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
  textdomain(GETTEXT_PACKAGE);

  if(init_gui)
  {
    // I doubt that connecting to dbus for darktable-cli makes sense
    darktable.dbus = dt_dbus_init();

    // make sure that we have no stale global progress bar visible. thus it's run as early as possible
    dt_control_progress_init(darktable.control);
  }

#ifdef _OPENMP
  omp_set_num_threads(darktable.num_openmp_threads);
#endif

#ifdef USE_LUA
  dt_lua_init_early(L);
#endif

  // thread-safe init:
  dt_exif_init();
  char datadir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(datadir, sizeof(datadir));
  char darktablerc[PATH_MAX] = { 0 };
  snprintf(darktablerc, sizeof(darktablerc), "%s/darktablerc", datadir);

  // initialize the config backend. this needs to be done first...
  darktable.conf = (dt_conf_t *)calloc(1, sizeof(dt_conf_t));
  dt_conf_init(darktable.conf, darktablerc, config_override);
  g_slist_free_full(config_override, g_free);

  // set the interface language and prepare selection for prefs
  darktable.l10n = dt_l10n_init(init_gui);

  dt_confgen_init();
  const int last_configure_version = dt_conf_get_int("performance_configuration_version_completed");

  // we need this REALLY early so that error messages can be shown, however after gtk_disable_setlocale
  if(init_gui)
  {
#ifdef GDK_WINDOWING_WAYLAND
    // There are currently bad interactions with Wayland (drop-downs
    // are very narrow, scroll events lost). Until this is fixed, give
    // priority to the XWayland backend for Wayland users.
    gdk_set_allowed_backends("x11,*");
#endif
    gtk_init(&argc, &argv);

    darktable.themes = NULL;
  }

  // detect cpu features and decide which codepaths to enable
  dt_codepaths_init();

  // get the list of color profiles
  darktable.color_profiles = dt_colorspaces_init();

  // initialize datetime data
  dt_datetime_init();

  // initialize the database
  darktable.db = dt_database_init(dbfilename_from_command, load_data, init_gui);
  if(darktable.db == NULL)
  {
    printf("ERROR : cannot open database\n");
    return 1;
  }
  else if(!dt_database_get_lock_acquired(darktable.db))
  {
    if (init_gui)
    {
      gboolean image_loaded_elsewhere = FALSE;
#ifndef MAC_INTEGRATION
      // send the images to the other instance via dbus
      fprintf(stderr, "trying to open the images in the running instance\n");

      GDBusConnection *connection = NULL;
      for(int i = 1; i < argc; i++)
      {
        // make the filename absolute ...
        if(argv[i] == NULL || *argv[i] == '\0') continue;
        gchar *filename = dt_util_normalize_path(argv[i]);
        if(filename == NULL) continue;
        if(!connection) connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
        // ... and send it to the running instance of darktable
        image_loaded_elsewhere = g_dbus_connection_call_sync(connection, "org.darktable.service", "/darktable",
                                                             "org.darktable.service.Remote", "Open",
                                                             g_variant_new("(s)", filename), NULL,
                                                             G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL) != NULL;
        g_free(filename);
      }
      if(connection) g_object_unref(connection);
#endif

      if(!image_loaded_elsewhere) dt_database_show_error(darktable.db);
    }
    fprintf(stderr, "ERROR: can't acquire database lock, aborting.\n");
    return 1;
  }

  //db maintenance on startup (if configured to do so)
  if(dt_database_maybe_maintenance(darktable.db, init_gui, FALSE))
  {
    dt_database_perform_maintenance(darktable.db);
  }

  // init darktable tags table
  dt_set_darktable_tags();

  // Initialize the signal system
  darktable.signals = dt_control_signal_init();

  // Make sure that the database and xmp files are in sync
  // We need conf and db to be up and running for that which is the case here.
  // FIXME: is this also useful in non-gui mode?
  GList *changed_xmp_files = NULL;
  if(init_gui && dt_conf_get_bool("run_crawler_on_start"))
  {
    changed_xmp_files = dt_control_crawler_run();
  }

  if(init_gui)
  {
    dt_control_init(darktable.control);
  }
  else
  {
    if(dbfilename_from_command && !strcmp(dbfilename_from_command, ":memory:"))
      dt_gui_presets_init(); // init preset db schema.
    darktable.control->running = 0;
    dt_pthread_mutex_init(&darktable.control->run_mutex, NULL);
  }

  // we initialize grouping early because it's needed for collection init
  // idem for folder reachability
  if(init_gui)
  {
    darktable.gui = (dt_gui_gtk_t *)calloc(1, sizeof(dt_gui_gtk_t));
    darktable.gui->grouping = dt_conf_get_bool("ui_last/grouping");
    memset(darktable.gui->scroll_to, 0, sizeof(darktable.gui->scroll_to));
    dt_film_set_folder_status();
  }

  /* for every resourcelevel we have 4 ints defined, either absolute or a fraction
     0 cpu available
     1 cpu singlebuffer
     2 mipmap size
     3 opencl available
  */
  /* special modes are meant to be used for debugging & testing,
     they are hidden in the ui menu and must be activated via --conf resourcelevel="xxx"
     here all values are absolutes in MB as we require fixed settings.
     reference, mini and notebook require a cl capable system with 16GB of ram and 2GB of free video ram
  */
  static int ref_resources[12] = {
      8192,  32,  512, 2048,   // reference
      1024,   2,  128,  200,   // mini system
      4096,  32,  512, 1024,   // simple notebook with integrated graphics
  };

  /* This is where the sync is to be done if the enum for pref resourcelevel in darktableconfig.xml.in is changed.
     all values are fractions val/1024 of total memory (0-2) or available OpenCL memory
  */
  static int fractions[16] = {
      128,    4,  64,  400, // small
      512,    8, 128,  700, // default
      700,   16, 128,  900, // large
    16384, 1024, 128,  900, // unrestricted
  };

  // Allow the settings for each UI performance level to be changed via darktablerc
  check_resourcelevel("resource_small", fractions, 0);
  check_resourcelevel("resource_default", fractions, 1);
  check_resourcelevel("resource_large", fractions, 2);
  check_resourcelevel("resource_unrestricted", fractions, 3);

  dt_sys_resources_t *res = &darktable.dtresources;
  res->fractions = fractions;
  res->refresource = ref_resources;
  res->total_memory = _get_total_memory() * 1024lu;

  char *config_info = calloc(1, DT_PERF_INFOSIZE);
  if(last_configure_version != DT_CURRENT_PERFORMANCE_CONFIGURE_VERSION)
    dt_configure_runtime_performance(last_configure_version, config_info);

  dt_get_sysresource_level();
  res->mipmap_memory = _get_mipmap_size();
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

  // *SIGH*
  dt_set_signal_handlers();
#elif defined HAVE_IMAGEMAGICK
  /* ImageMagick init */
  MagickWandGenesis();
#endif

  darktable.opencl = (dt_opencl_t *)calloc(1, sizeof(dt_opencl_t));
#ifdef HAVE_OPENCL
  dt_opencl_init(darktable.opencl, exclude_opencl, print_statistics);
  dt_opencl_update_settings();
#endif

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
    if(dt_gui_gtk_init(darktable.gui))
    {
      fprintf(stderr, "ERROR: can't init gui, aborting.\n");
      return 1;
    }
    dt_bauhaus_init();
  }
  else
    darktable.gui = NULL;

  darktable.view_manager = (dt_view_manager_t *)calloc(1, sizeof(dt_view_manager_t));
  dt_view_manager_init(darktable.view_manager);

  // check whether we were able to load darkroom view. if we failed, we'll crash everywhere later on.
  if(!darktable.develop)
  {
    fprintf(stderr, "ERROR: can't init develop system, aborting.\n");
    return 1;
  }

  darktable.imageio = (dt_imageio_t *)calloc(1, sizeof(dt_imageio_t));
  dt_imageio_init(darktable.imageio);

  // load default iop order
  darktable.iop_order_list = dt_ioppr_get_iop_order_list(0, FALSE);
  // load iop order rules
  darktable.iop_order_rules = dt_ioppr_get_iop_order_rules();
  // load the darkroom mode plugins once:
  dt_iop_load_modules_so();
  // check if all modules have a iop order assigned
  if(dt_ioppr_check_so_iop_order(darktable.iop, darktable.iop_order_list))
  {
    fprintf(stderr, "ERROR: iop order looks bad, aborting.\n");
    return 1;
  }

  // set up memory.darktable_iop_names table
  dt_iop_set_darktable_iop_table();

  // set up the list of exiv2 metadata
  dt_exif_set_exiv2_taglist();

  // init metadata flags
  dt_metadata_init();

  if(init_gui)
  {
#ifdef HAVE_GPHOTO2
    // Initialize the camera control.
    // this is done late so that the gui can react to the signal sent but before switching to lighttable!
    darktable.camctl = dt_camctl_new();
#endif

    darktable.lib = (dt_lib_t *)calloc(1, sizeof(dt_lib_t));
    dt_lib_init(darktable.lib);

    dt_gui_gtk_load_config();

    // init the gui part of views
    dt_view_manager_gui_init(darktable.view_manager);

    // Save the default shortcuts
    dt_shortcuts_save(".defaults", FALSE);

    // Then load any shortcuts if available (wipe defaults first if requested)
    dt_shortcuts_load(NULL, !dt_conf_get_bool("accel/load_defaults"));

    // Save the shortcuts including defaults
    dt_shortcuts_save(NULL, TRUE);

    // initialize undo struct
    darktable.undo = dt_undo_init();
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

  if(init_gui)
  {
    const char *mode = "lighttable";
#ifdef HAVE_GAME
    // april 1st: you have to earn using dt first! or know that you can switch views with keyboard shortcuts
    time_t now;
    time(&now);
    struct tm lt;
    localtime_r(&now, &lt);
    if(lt.tm_mon == 3 && lt.tm_mday == 1)
    {
      const int current_year = lt.tm_year + 1900;
      const int last_year = dt_conf_get_int("ui_last/april1st");
      const gboolean kill_april1st = dt_conf_get_bool("ui_last/no_april1st");
      if(!kill_april1st && last_year < current_year)
      {
        dt_conf_set_int("ui_last/april1st", current_year);
        mode = "knight";
      }
    }
#endif
    // we have to call dt_ctl_switch_mode_to() here already to not run into a lua deadlock.
    // having another call later is ok
    dt_ctl_switch_mode_to(mode);

#ifndef MAC_INTEGRATION
    // load image(s) specified on cmdline.
    // this has to happen after lua is initialized as image import can run lua code
    if (argc == 2)
    {
      // If only one image is listed, attempt to load it in darkroom
      (void)dt_load_from_string(argv[1], TRUE, NULL);
    }
    else if (argc > 2)
    {
      // when multiple names are given, fire up a background job to import them
      dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_BG, dt_pathlist_import_create(argc,argv));
    }
#endif

    // there might be some info created in dt_configure_runtime_performance() for feedback
    gboolean not_again = TRUE;
    if(last_configure_version && config_info[0])
      not_again = dt_gui_show_standalone_yes_no_dialog
        (_("configuration information"),
         config_info,
         _("show this information again"), _("understood"));

    if(not_again || (last_configure_version == 0))
      dt_conf_set_int("performance_configuration_version_completed", DT_CURRENT_PERFORMANCE_CONFIGURE_VERSION);
  }
  free(config_info);

  // last but not least construct the popup that asks the user about images whose xmp files are newer than the
  // db entry
  if(init_gui && changed_xmp_files)
  {
    dt_control_crawler_show_image_list(changed_xmp_files);
  }

  dt_print(DT_DEBUG_CONTROL, "[init] startup took %f seconds\n", dt_get_wtime() - start_wtime);

  return 0;
}

void dt_get_sysresource_level()
{
  static int oldlevel = -999;
  static int oldtunecl = -999;

  dt_sys_resources_t *res = &darktable.dtresources;
  const int tunecl = dt_opencl_get_tuning_mode();
  int level = 1;
  const char *config = dt_conf_get_string_const("resourcelevel");
  /** These levels must correspond with preferences in xml.in
      modes available in the ui have levsls >= 0 **and** fractions
      modes available for debugging / reference have negative levels and **and** ref_resources
      If we want a new setting here, we must
        - add a string->level conversion here
        - add a line of fraction in int fractions[] or ref_resources[] above
        - add a line in darktableconfig.xml.in if available via UI
  */
  if(config)
  {
         if(!strcmp(config, "default"))      level = 1;
    else if(!strcmp(config, "small"))        level = 0;
    else if(!strcmp(config, "large"))        level = 2;
    else if(!strcmp(config, "unrestricted")) level = 3;
    else if(!strcmp(config, "reference"))    level = -1;
    else if(!strcmp(config, "mini"))         level = -2;
    else if(!strcmp(config, "notebook"))     level = -3;
  }
  const gboolean mod = ((level != oldlevel) || (oldtunecl != tunecl));
  res->level = oldlevel = level;
  oldtunecl = tunecl;
  res->tunemode = tunecl;
  if(mod && (darktable.unmuted & (DT_DEBUG_MEMORY | DT_DEBUG_OPENCL | DT_DEBUG_DEV)))
  {
    const int oldgrp = res->group;
    res->group = 4 * level;
    fprintf(stderr,"[dt_get_sysresource_level] switched to %i as `%s'\n", level, config);
    fprintf(stderr,"  total mem:       %luMB\n", res->total_memory / 1024lu / 1024lu);
    fprintf(stderr,"  mipmap cache:    %luMB\n", _get_mipmap_size() / 1024lu / 1024lu);
    fprintf(stderr,"  available mem:   %luMB\n", dt_get_available_mem() / 1024lu / 1024lu);
    fprintf(stderr,"  singlebuff:      %luMB\n", dt_get_singlebuffer_mem() / 1024lu / 1024lu);
    fprintf(stderr,"  iop cache:       %luMB\n", dt_get_iopcache_mem() / 1024lu / 1024lu);
#ifdef HAVE_OPENCL
    fprintf(stderr,"  OpenCL tune mem: %s\n", ((tunecl & DT_OPENCL_TUNE_MEMSIZE) && (level >= 0)) ? "WANTED" : "OFF");
    fprintf(stderr,"  OpenCL pinned:   %s\n", ((tunecl & DT_OPENCL_TUNE_PINNED) && (level >= 0)) ? "WANTED" : "OFF");
#endif
    res->group = oldgrp;
  }
}

void dt_cleanup()
{
  const int init_gui = (darktable.gui != NULL);

  // last chance to ask user for any input...

  const gboolean perform_maintenance = dt_database_maybe_maintenance(darktable.db, init_gui, TRUE);
  const gboolean perform_snapshot = dt_database_maybe_snapshot(darktable.db);
  gchar **snaps_to_remove = NULL;
  if(perform_snapshot)
  {
    snaps_to_remove = dt_database_snaps_to_remove(darktable.db);
  }

#ifdef HAVE_PRINT
  dt_printers_abort_discovery();
#endif

#ifdef USE_LUA
  dt_lua_finalize_early();
#endif

  // anything that asks user for input should be placed before this line

  if(init_gui)
  {
    // hide main window and do rest of the cleanup in the background
    gtk_widget_hide(dt_ui_main_window(darktable.gui->ui));

    dt_ctl_switch_mode_to("");
    dt_dbus_destroy(darktable.dbus);

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
  g_list_free_full(darktable.iop_order_list, free);
  darktable.iop_order_list = NULL;
  g_list_free_full(darktable.iop_order_rules, free);
  darktable.iop_order_rules = NULL;
  dt_opencl_cleanup(darktable.opencl);
  free(darktable.opencl);
#ifdef HAVE_GPHOTO2
  dt_camctl_destroy((dt_camctl_t *)darktable.camctl);
  darktable.camctl = NULL;
#endif
  dt_pwstorage_destroy(darktable.pwstorage);

#ifdef HAVE_GRAPHICSMAGICK
  DestroyMagick();
#elif defined HAVE_IMAGEMAGICK
  MagickWandTerminus();
#endif

  dt_guides_cleanup(darktable.guides);

  if(perform_maintenance)
  {
    dt_database_cleanup_busy_statements(darktable.db);
    dt_database_perform_maintenance(darktable.db);
  }

  dt_database_optimize(darktable.db);
  if(perform_snapshot)
  {
    if(dt_database_snapshot(darktable.db) && snaps_to_remove)
    {
      int i = 0;
      while(snaps_to_remove[i])
      {
        // make file to remove writable, mostly problem on windows.
        g_chmod(snaps_to_remove[i], S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);

        dt_print(DT_DEBUG_SQL, "[db backup] removing old snap: %s... ", snaps_to_remove[i]);
        const int retunlink = g_remove(snaps_to_remove[i++]);
        dt_print(DT_DEBUG_SQL, "%s\n", retunlink == 0 ? "success" : "failed!");
      }
    }
  }
  if(snaps_to_remove)
  {
    g_strfreev(snaps_to_remove);
  }
  dt_database_destroy(darktable.db);

  if(init_gui)
  {
    dt_bauhaus_cleanup();
  }

  if (darktable.noiseprofile_parser)
  {
    g_object_unref(darktable.noiseprofile_parser);
    darktable.noiseprofile_parser = NULL;
  }

  dt_capabilities_cleanup();

  for (int k=0; k<DT_IMAGE_DBLOCKS; k++)
  {
    dt_pthread_mutex_destroy(&(darktable.db_image[k]));
  }
  dt_pthread_mutex_destroy(&(darktable.plugin_threadsafe));
  dt_pthread_mutex_destroy(&(darktable.dev_threadsafe));
  dt_pthread_mutex_destroy(&(darktable.capabilities_threadsafe));
  dt_pthread_mutex_destroy(&(darktable.exiv2_threadsafe));
  dt_pthread_mutex_destroy(&(darktable.readFile_mutex));

  dt_exif_cleanup();
}

void dt_print(dt_debug_thread_t thread, const char *msg, ...)
{
  if(darktable.unmuted & thread)
  {
    printf("%f ", dt_get_wtime() - darktable.start_wtime);
    va_list ap;
    va_start(ap, msg);
    vprintf(msg, ap);
    va_end(ap);
    fflush(stdout);
  }
}

void dt_print_nts(dt_debug_thread_t thread, const char *msg, ...)
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

void dt_vprint(dt_debug_thread_t thread, const char *msg, ...)
{
  if((darktable.unmuted & DT_DEBUG_VERBOSE) && (darktable.unmuted & thread))
  {
    printf("%f ", dt_get_wtime() - darktable.start_wtime);
    va_list ap;
    va_start(ap, msg);
    vprintf(msg, ap);
    va_end(ap);
    fflush(stdout);
  }
}

void *dt_alloc_align(size_t alignment, size_t size)
{
  const size_t aligned_size = dt_round_size(size, alignment);
#if defined(__FreeBSD_version) && __FreeBSD_version < 700013
  return malloc(aligned_size);
#elif defined(_WIN32)
  return _aligned_malloc(aligned_size, alignment);
#elif defined(_DEBUG)
  // for a debug build, ensure that we get a crash if we use plain free() to release the allocated memory, by
  // returning a pointer which isn't a valid memory block address
  void *ptr = NULL;
  if(posix_memalign(&ptr, alignment, aligned_size + alignment)) return NULL;
  short *offset = (short*)(((char*)ptr) + alignment - sizeof(short));
  *offset = alignment;
  return ((char*)ptr) + alignment ;
#else
  void *ptr = NULL;
  if(posix_memalign(&ptr, alignment, aligned_size)) return NULL;
  return ptr;
#endif
}

size_t dt_round_size(const size_t size, const size_t alignment)
{
  // Round the size of a buffer to the closest higher multiple
  return ((size % alignment) == 0) ? size : ((size - 1) / alignment + 1) * alignment;
}

size_t dt_round_size_sse(const size_t size)
{
  // Round the size of a buffer to the closest 64 higher multiple
  return dt_round_size(size, 64);
}


#ifdef _WIN32
void dt_free_align(void *mem)
{
  _aligned_free(mem);
}
#elif defined(_DEBUG)
void dt_free_align(void *mem)
{
  // on a debug build, we deliberately offset the returned pointer from dt_alloc_align, so eliminate the offset
  if (mem)
  {
    short offset = ((short*)mem)[-1];
    free(((char*)mem)-offset);
  }
}
#endif

void dt_show_times(const dt_times_t *start, const char *prefix)
{
  /* Skip all the calculations an everything if -d perf isn't on */
  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_times_t end;
    dt_get_times(&end);
    char buf[140]; /* Arbitrary size, should be lots big enough for everything used in DT */
    snprintf(buf, sizeof(buf), "%s took %.3f secs (%.3f CPU)", prefix, end.clock - start->clock,
             end.user - start->user);
    dt_print(DT_DEBUG_PERF, "%s\n", buf);
  }
}

void dt_show_times_f(const dt_times_t *start, const char *prefix, const char *suffix, ...)
{
  /* Skip all the calculations an everything if -d perf isn't on */
  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_times_t end;
    dt_get_times(&end);
    char buf[160]; /* Arbitrary size, should be lots big enough for everything used in DT */
    const int n = snprintf(buf, sizeof(buf), "%s took %.3f secs (%.3f CPU) ", prefix, end.clock - start->clock,
                           end.user - start->user);
    if(n < sizeof(buf) - 1)
    {
      va_list ap;
      va_start(ap, suffix);
      vsnprintf(buf + n, sizeof(buf) - n, suffix, ap);
      va_end(ap);
    }
    dt_print(DT_DEBUG_PERF, "%s\n", buf);
  }
}

int dt_worker_threads()
{
  const size_t threads = dt_get_num_threads();
  const size_t mem = _get_total_memory();
  const int wthreads = (mem >= (8lu << 20) && threads >= 4) ? 4 : MIN(2, threads);
  dt_print(DT_DEBUG_DEV, "[dt_worker_threads] using %i worker threads\n", wthreads);
  return wthreads;
}

size_t dt_get_available_mem()
{
  dt_sys_resources_t *res = &darktable.dtresources;
  const int level = res->level;
  const size_t total_mem = res->total_memory;
  if(level < 0)
    return res->refresource[4*(-level-1)] * 1024lu * 1024lu;

  const int fraction = res->fractions[darktable.dtresources.group];
  return MAX(512lu * 1024lu * 1024lu, total_mem / 1024lu * fraction);
}

size_t dt_get_singlebuffer_mem()
{
  dt_sys_resources_t *res = &darktable.dtresources;
  const int level = res->level;
  const size_t total_mem = res->total_memory;
  if(level < 0)
    return res->refresource[4*(-level-1) + 1] * 1024lu * 1024lu;

  const int fraction = res->fractions[res->group + 1];
  return MAX(2lu * 1024lu * 1024lu, total_mem / 1024lu * fraction);
}

size_t dt_get_iopcache_mem()
{
  dt_sys_resources_t *res = &darktable.dtresources;
  const size_t cachemb = res->total_memory / 1024lu / 1024lu / 20lu;
  return MIN(6000lu, MAX(400lu, cachemb)) * 1024lu * 1024lu;
}

void dt_configure_runtime_performance(const int old, char *info)
{
  const size_t threads = dt_get_num_threads();
  const size_t mem = darktable.dtresources.total_memory / 1024lu / 1024lu;
  const size_t bits = CHAR_BIT * sizeof(void *);
  const gboolean sufficient = mem >= 4096 && threads >= 2;

  dt_print(DT_DEBUG_DEV, "[dt_configure_runtime_performance] found a %s %zu-bit system with %zu Mb ram and %zu cores\n",
    (sufficient) ? "sufficient" : "low performance", bits, mem, threads);

  // All runtime conf settings only write data if there is no valid data found in conf
  if(!dt_conf_key_not_empty("ui/performance"))
  {
    dt_conf_set_bool("ui/performance", !sufficient);
    dt_print(DT_DEBUG_DEV, "[dt_configure_runtime_performance] ui/performance=%s\n", (sufficient) ? "FALSE" : "TRUE");
  }

  if(!dt_conf_key_not_empty("resourcelevel"))
  {
    dt_conf_set_string("resourcelevel", (sufficient) ? "default" : "small");
    dt_print(DT_DEBUG_DEV, "[dt_configure_runtime_performance] resourcelevel=%s\n", (sufficient) ? "default" : "small");
  }

  if(!dt_conf_key_not_empty("plugins/darkroom/demosaic/quality"))
  {
    dt_conf_set_string("plugins/darkroom/demosaic/quality", (sufficient) ? "at most RCD (reasonable)" : "always bilinear (fast)");
    dt_print(DT_DEBUG_DEV, "[dt_configure_runtime_performance] plugins/darkroom/demosaic/quality=%s",
      (sufficient) ? "at most RCD (reasonable)" : "always bilinear (fast)");
  }
  else if(old == 2)
  {
    const gchar *demosaic_quality = dt_conf_get_string_const("plugins/darkroom/demosaic/quality");
    if(!strcmp(demosaic_quality, "always bilinear (fast)"))
    {
      dt_conf_set_string("plugins/darkroom/demosaic/quality", "at most RCD (reasonable)");
      dt_print(DT_DEBUG_DEV, "[dt_configure_performance] override: plugins/darkroom/demosaic/quality=at most RCD (reasonable)\n");
    }
  }

  if(!dt_conf_key_not_empty("cache_disk_backend_full"))
  {
    char cachedir[PATH_MAX] = { 0 };
    guint64 freecache = 0;
    dt_loc_get_user_cache_dir(cachedir, sizeof(cachedir));
    GFile *gfile = g_file_new_for_path(cachedir);
    GFileInfo *gfileinfo = g_file_query_filesystem_info(gfile, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, NULL, NULL);
    if(gfileinfo != NULL)
      freecache = g_file_info_get_attribute_uint64(gfileinfo, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
    g_object_unref(gfile);
    g_object_unref(gfileinfo);
    const gboolean largedisk = freecache > (8lu << 20);
    // enable cache_disk_backend_full when user has over 8gb free diskspace
    dt_conf_set_bool("cache_disk_backend_full", largedisk);
    dt_print(DT_DEBUG_DEV, "[dt_configure_runtime_performance] cache_disk_backend_full=%s\n", (largedisk) ? "TRUE" : "FALSE");
  }

  // we might add some info now but only for non-fresh installs
  if(old == 0) return;

  #define INFO_HEADER "> "

  if(old < 2) // we introduced RCD as the default demosaicer in 2
  {
    g_strlcat(info, INFO_HEADER, DT_PERF_INFOSIZE);
    g_strlcat(info, _("the RCD demosaicer has been defined as default instead of PPG because of better quality and performance."), DT_PERF_INFOSIZE);
    g_strlcat(info, "\n", DT_PERF_INFOSIZE);
    g_strlcat(info, _("see preferences/darkroom/demosaicing for zoomed out darkroom mode"), DT_PERF_INFOSIZE);
    g_strlcat(info, "\n\n", DT_PERF_INFOSIZE);
  }
  if(old < 5)
  {
    g_strlcat(info, INFO_HEADER, DT_PERF_INFOSIZE);
    g_strlcat(info, _("the user interface and the underlying internals for tuning darktable performance have changed."), DT_PERF_INFOSIZE);
    g_strlcat(info, "\n", DT_PERF_INFOSIZE);
    g_strlcat(info, _("you won't find headroom and friends any longer, instead in preferences/processing use:"), DT_PERF_INFOSIZE);
    g_strlcat(info, "\n  ", DT_PERF_INFOSIZE);
    g_strlcat(info, _("1) darktable resources"), DT_PERF_INFOSIZE);
    g_strlcat(info, "\n  ", DT_PERF_INFOSIZE);
    g_strlcat(info, _("2) tune OpenCL performance"), DT_PERF_INFOSIZE);
    g_strlcat(info, "\n\n", DT_PERF_INFOSIZE);
  }

  if(old < 11)
  {
    g_strlcat(info, INFO_HEADER, DT_PERF_INFOSIZE);
    g_strlcat(info, _("some global config values relevant for OpenCL performance are not used any longer."), DT_PERF_INFOSIZE);
    g_strlcat(info, "\n", DT_PERF_INFOSIZE);
    g_strlcat(info, _("instead you will find 'per device' data in 'cl_device_v4_canonical-name'. content is:"), DT_PERF_INFOSIZE);
    g_strlcat(info, "\n  ", DT_PERF_INFOSIZE);
    g_strlcat(info, _(" 'avoid_atomics' 'micro_nap' 'pinned_memory' 'roundupwd' 'roundupht' 'eventhandles' 'async' 'disable' 'magic'"), DT_PERF_INFOSIZE);
    g_strlcat(info, "\n", DT_PERF_INFOSIZE);
    g_strlcat(info, _("you may tune as before except 'magic'"), DT_PERF_INFOSIZE);
    g_strlcat(info, "\n\n", DT_PERF_INFOSIZE);
  }

  #undef INFO_HEADER
}

int dt_capabilities_check(char *capability)
{
  for(GList *capabilities = darktable.capabilities; capabilities; capabilities = g_list_next(capabilities))
  {
    if(!strcmp(capabilities->data, capability))
    {
      return TRUE;
    }
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


void dt_print_mem_usage()
{
#if defined(__linux__)
  char *line = NULL;
  size_t len = 128;
  char vmsize[64];
  char vmpeak[64];
  char vmrss[64];
  char vmhwm[64];
  FILE *f;

  char pidstatus[128];
  snprintf(pidstatus, sizeof(pidstatus), "/proc/%u/status", (uint32_t)getpid());

  f = g_fopen(pidstatus, "r");
  if(!f) return;

  /* read memory size data from /proc/pid/status */
  while(getline(&line, &len, f) != -1)
  {
    if(!strncmp(line, "VmPeak:", 7))
      g_strlcpy(vmpeak, line + 8, sizeof(vmpeak));
    else if(!strncmp(line, "VmSize:", 7))
      g_strlcpy(vmsize, line + 8, sizeof(vmsize));
    else if(!strncmp(line, "VmRSS:", 6))
      g_strlcpy(vmrss, line + 8, sizeof(vmrss));
    else if(!strncmp(line, "VmHWM:", 6))
      g_strlcpy(vmhwm, line + 8, sizeof(vmhwm));
  }
  free(line);
  fclose(f);

  fprintf(stderr, "[memory] max address space (vmpeak): %15s"
                  "[memory] cur address space (vmsize): %15s"
                  "[memory] max used memory   (vmhwm ): %15s"
                  "[memory] cur used memory   (vmrss ): %15s",
          vmpeak, vmsize, vmhwm, vmrss);

#elif defined(__APPLE__)
  struct task_basic_info t_info;
  mach_msg_type_number_t t_info_count = TASK_BASIC_INFO_COUNT;

  if(KERN_SUCCESS != task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t)&t_info, &t_info_count))
  {
    fprintf(stderr, "[memory] task memory info unknown.\n");
    return;
  }

  // Report in kB, to match output of /proc on Linux.
  fprintf(stderr, "[memory] max address space (vmpeak): %15s\n"
                  "[memory] cur address space (vmsize): %12llu kB\n"
                  "[memory] max used memory   (vmhwm ): %15s\n"
                  "[memory] cur used memory   (vmrss ): %12llu kB\n",
          "unknown", (uint64_t)t_info.virtual_size / 1024, "unknown", (uint64_t)t_info.resident_size / 1024);
#elif defined (_WIN32)
  //Based on: http://stackoverflow.com/questions/63166/how-to-determine-cpu-and-memory-consumption-from-inside-a-process
  MEMORYSTATUSEX memInfo;
  memInfo.dwLength = sizeof(MEMORYSTATUSEX);
  GlobalMemoryStatusEx(&memInfo);
  // DWORDLONG totalVirtualMem = memInfo.ullTotalPageFile;

  // Virtual Memory currently used by current process:
  PROCESS_MEMORY_COUNTERS_EX pmc;
  GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc));
  size_t virtualMemUsedByMe = pmc.PagefileUsage;
  size_t virtualMemUsedByMeMax = pmc.PeakPagefileUsage;

  // Max Physical Memory currently used by current process
  size_t physMemUsedByMeMax = pmc.PeakWorkingSetSize;

  // Physical Memory currently used by current process
  size_t physMemUsedByMe = pmc.WorkingSetSize;


  fprintf(stderr, "[memory] max address space (vmpeak): %12llu kB\n"
                  "[memory] cur address space (vmsize): %12llu kB\n"
                  "[memory] max used memory   (vmhwm ): %12llu kB\n"
                  "[memory] cur used memory   (vmrss ): %12llu Kb\n",
          virtualMemUsedByMeMax / 1024, virtualMemUsedByMe / 1024, physMemUsedByMeMax / 1024,
          physMemUsedByMe / 1024);

#else
  fprintf(stderr, "dt_print_mem_usage() currently unsupported on this platform\n");
#endif
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
