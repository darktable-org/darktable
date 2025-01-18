/*
    This file is part of darktable,
    Copyright (C) 2009-2025 darktable developers.

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
#include "common/file_location.h"
#include "common/film.h"
#include "common/grealpath.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/iop_order.h"
#include "common/l10n.h"
#include "common/mipmap_cache.h"
#include "common/noiseprofiles.h"
#include "common/opencl.h"
#include "common/points.h"
#include "common/resource_limits.h"
#include "common/undo.h"
#include "common/gimp.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/crawler.h"
#include "control/jobs/control_jobs.h"
#include "control/jobs/film_jobs.h"
#include "control/jobs/sidecar_jobs.h"
#include "control/signal.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/guides.h"
#include "gui/presets.h"
#include "gui/styles.h"
#include "gui/splash.h"
#include "imageio/imageio_module.h"
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

#ifdef HAVE_GRAPHICSMAGICK
#include <magick/api.h>
#elif defined HAVE_IMAGEMAGICK
  #ifdef HAVE_IMAGEMAGICK7
  #include <MagickWand/MagickWand.h>
  #else
  #include <wand/MagickWand.h>
  #endif
#endif

#ifdef HAVE_LIBHEIF
#include <libheif/heif.h>
#endif

#ifdef HAVE_LIBRAW
#include <libraw/libraw_version.h>
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
  char *logfile = g_build_filename(g_get_home_dir(), "Documents", "Darktable", "darktable-log.txt", NULL);
#endif
  // clang-format off

// Rewriting this following GNU Standards for Command Line Interfaces and other best practices
// ref. https://www.gnu.org/prep/standards/standards.html#Command_002dLine-Interfaces
// ref. https://clig.dev/#introduction
//
// Trying to keep the length of the text within 80 columns
// Using 2-4 spaces for the indentation of the inline help

  printf("darktable %s\n"
         "Copyright (C) 2012-%s Johannes Hanika and other contributors.\n\n"
         "<https://www.darktable.org>\n"
         "darktable is an open source photography workflow application and\n"
         "non-destructive raw developer for photographers.\n"
         "GNU GPL version 3 or later <https://gnu.org/licenses/gpl.html>\n"
         "This is free software: you are free to change and redistribute it.\n"
         "There is NO WARRANTY, to the extent permitted by law.\n\n",
         darktable_package_version,
         darktable_last_commit_year);

  printf("Usage:\n"
         "  darktable [OPTIONS] [IMAGE_FILE | IMAGE_FOLDER]\n"
         "\n"
         "Options:\n"
         "\n"
         "--cachedir DIR\n"
         "    darktable keeps a cache of image thumbnails for fast image preview\n"
         "    and precompiled OpenCL binaries for fast startup. By default the\n"
         "    cache is located in $HOME/.cache/darktable/. Multiple thumbnail\n"
         "    caches may exist in parallel, one for each library file.\n"
         "\n"
         "--conf KEY=VALUE\n"
         "    Temporarily overwrite individual settings on the command line with\n"
         "    this option - these settings will not be stored in darktablerc\n"
         "    on exit.\n"
         "\n"
         "--configdir DIR\n"
         "    Where darktable stores user-specific configuration.\n"
         "    The default location is $HOME/.config/darktable/\n"
         "\n"
         "--datadir DIR\n"
         "    Define the directory where darktable finds its runtime data.\n"
         "    The default location depends on your installation.\n"
         "    Typical locations are /opt/darktable/share/darktable/ \n"
         "    and /usr/share/darktable/\n"
         "\n"
         "--library FILE\n"
         "    Specifies an alternate location for darktable's image information database,\n"
         "    which is stored in an SQLite file by default (library.db) in the directory\n"
         "    specified by --configdir or $HOME/.config/darktable/. You can use this\n"
         "    option for experimentation without affecting your original library.db.\n"
         "    If the specified database file doesn't exist, darktable will create it.\n"
         "\n"
         "    When darktable starts, it locks the library to the current user by writing\n"
         "    the process identifier (PID) to a lock file named FILE.lock next to the\n"
         "    specified library. If a lock file already exists, darktable will exit.\n"
         "\n"
         "    :memory: -> Use this option as FILE to keep the database in system memory,\n"
         "    discarding changes on darktable termination.\n"
         "\n"
         "--localedir DIR\n"
         "    Define where darktable can find its language-specific text\n"
         "    strings. The default location depends on your installation.\n"
         "    Typical locations are /opt/darktable/share/locale/\n"
         "    and /usr/share/locale/\n"
         "\n"
#ifdef USE_LUA
         "--luacmd COMMAND\n"
         "    A string containing lua commands to execute after lua\n"
         "    initialization. These commands will be run after your “luarc”\n"
         "    file. If lua is not compiled-in, this option will be accepted\n"
         "    but won't do anything.\n"
         "\n"
#endif
         "--moduledir DIR\n"
         "    darktable has a modular structure and organizes its modules as\n"
         "    shared libraries for loading at runtime.\n"
         "    This option tells darktable where to look for its shared libraries.\n"
         "    The default location depends on your installation.\n"
         "    Typical locations are /opt/darktable/lib64/darktable/\n"
         "    and /usr/lib64/darktable/\n"
         "\n"
         "--noiseprofiles FILE\n"
         "    Provide a json file that contains camera-specific noise profiles.\n"
         "    The default location depends on your installation.\n"
         "    Typical locations are /opt/darktable/share/darktable/noiseprofile.json\n"
         "    and /usr/share/darktable/noiseprofile.json\n"
         "\n"
         "-t, --threads NUM\n"
         "    Limit number of openmp threads to use in openmp parallel sections\n"
         "\n"
         "--tmpdir DIR\n"
         "    Define where darktable should store its temporary files.\n"
         "    If this option is not supplied darktable uses the system default.\n"
         "\n"
         "-v, --version\n"
         "    Print darktable version number\n"
         "\n"
#ifdef _WIN32
         "-h, --help, /?\n"
#else
         "-h, --help\n"
#endif
         "    Show this help text\n"
         "\n"
         "Debugging:\n\n"
         "--bench-module MODULE_A,MODULE_B\n"
         "\n"
#ifdef HAVE_OPENCL
         "--disable-opencl\n"
         "    Prevent darktable from initializing the OpenCL subsystem.\n"
         "\n"
#endif
         "--disable-pipecache\n"
         "    Disable the pixelpipe cache. This option allows only\n"
         "    two cachelines per pipe, and should be used for debugging\n"
         "    purposes only.\n"
         "\n"
         "--dump-pfm MODULE_A,MODULE_B\n"
         "\n"
         "--dump-pipe MODULE_A,MODULE_B\n"
         "\n"
         "--dump-diff-pipe MODULE_A,MODULE_B\n"
         "\n"
         "--dumpdir DIR\n"
         "\n"
         "-d SIGNAL\n"
         "    Enable debug output to the terminal. Valid signals are:\n\n"
         "    act_on, cache, camctl, camsupport, control, dev, expose,\n"
         "    imageio, input, ioporder, lighttable, lua, masks, memory,\n"
         "    nan, opencl, params, perf, pipe, print, pwstorage, signal,\n"
         "    sql, tiling, picker, undo\n"
         "\n"
         "    all     -> to debug all signals\n"
         "    common  -> to debug dev, imageio, masks, opencl, params, pipe\n"
         "    verbose -> when combined with debug options like '-d opencl'\n"
         "               provides more detailed output. To activate verbosity,\n"
         "               use the additional option '-d verbose'\n"
         "               even when using '-d all'.\n"
         "\n"
         "    There are several subsystems of darktable and each of them can be\n"
         "    debugged separately. You can use this option multiple times if you\n"
         "    want to debug more than one subsystem.\n"
         "\n"
         "    E.g. darktable -d opencl -d camctl -d perf\n"
         "\n"
         "--d-signal SIGNAL\n"
         "    if -d signal or -d all is specified, specify the signal to debug\n"
         "    using this option. Specify ALL to debug all signals or specify\n"
         "    signal using it's full name. Can be used multiple times.\n"
         "\n"
         "--d-signal-act SIGNAL_ACT\n"
         "\n"
         "    Valid SIGNAL_ACT are:\n"
         "    all, raise, connect, disconnect"
#ifdef DT_HAVE_SIGNAL_TRACE
         ", print-trace\n"
#endif
         "\n"
         "    If -d signal or -d all is specified, specify the signal action\n"
         "    to debug using this option.\n");
#ifdef _WIN32
  printf("\n\n");
  printf("Debug log and output will be written to this file:\n");
  printf("    %s\n", logfile);
#else
  printf("\n");
#endif

  printf("See %s for more detailed documentation.\n"
         "See %s to report bugs.\n",
         PACKAGE_DOCS,
         PACKAGE_BUGREPORT);

#ifdef _WIN32
  g_free(logfile);
#endif

  return 1;
}

// clang-format on
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

static gboolean _is_directory(const gchar *input)
{
  gboolean is_dir = FALSE;
  char *filename = dt_util_normalize_path(input);
  if(filename)
  {
    is_dir = g_file_test(filename, G_FILE_TEST_IS_DIR);
    free(filename);
  }
  return is_dir;
}

static void _switch_to_new_filmroll(const gchar *input)
{
  char *filename = dt_util_normalize_path(input);
  if(filename)
  {
    gchar *dirname;
    if(g_file_test(filename, G_FILE_TEST_IS_DIR))
      dirname = g_strdup(filename);
    else
      dirname = g_path_get_dirname((const gchar*)filename);
    dt_filmid_t filmid = NO_FILMID;
    /* initialize a film object*/
    dt_film_t *film = malloc(sizeof(dt_film_t));
    dt_film_init(film);
    dt_film_new(film, dirname);
    if(dt_is_valid_filmid(film->id))
    {
      dt_film_open(film->id);
      dt_ctl_switch_mode_to("lighttable");
    }
    free(film);
    g_free(dirname);
    free(filename);
  }
}

dt_imgid_t dt_load_from_string(const gchar *input,
                               const gboolean open_image_in_dr,
                               gboolean *single_image)
{
  dt_imgid_t imgid = NO_IMGID;
  if(input == NULL || input[0] == '\0') return NO_IMGID;

  char *filename = dt_util_normalize_path(input);

  if(filename == NULL)
  {
    dt_control_log(_("found strange path `%s'"), input);
    return NO_IMGID;
  }

  if(g_file_test(filename, G_FILE_TEST_IS_DIR))
  {
    // import a directory into a film roll
    const dt_filmid_t filmid = dt_film_import(filename);
    if(dt_is_valid_filmid(filmid))
    {
      dt_film_open(filmid);
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
    const dt_filmid_t filmid = dt_film_new(&film, directory);
    imgid = dt_image_import(filmid, filename, TRUE, TRUE);
    g_free(directory);
    if(dt_is_valid_imgid(imgid))
    {
      dt_film_open(filmid);
      // make sure buffers are loaded (load full for testing)
      dt_mipmap_buffer_t buf;
      dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid,
                          DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');
      const gboolean loaded = (buf.buf != NULL);
      dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
      if(!loaded)
      {
        imgid = NO_IMGID;
        if(buf.loader_status == DT_IMAGEIO_UNSUPPORTED_FORMAT || buf.loader_status == DT_IMAGEIO_UNSUPPORTED_FEATURE)
          dt_control_log(_("file `%s' has unsupported format!"), filename);
        else
          dt_control_log(_("file `%s' has unknown format!"), filename);
      }
      else
      {
        if(open_image_in_dr)
        {
          dt_control_set_mouse_over_id(imgid);
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
  return imgid;
}

static void dt_codepaths_init()
{
  // we no longer do explicit runtime selection of code paths, so initialize to "none"
  // (there are still functions where the compiler creates runtime selection due to "target_clones"
  // directives, but those are not affected by this code)

  memset(&(darktable.codepath), 0, sizeof(darktable.codepath));

  // do we have any intrinsics sets enabled? (nope)
  darktable.codepath._no_intrinsics = 1;
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
  dt_print(DT_DEBUG_ALWAYS, "[get_total_memory] Unknown memory size. Assuming 2GB");
  return 2097152;
#endif
}

static size_t _get_mipmap_size()
{
  dt_sys_resources_t *res = &darktable.dtresources;
  const int level = res->level;
  if(level < 0)
    return res->refresource[4*(-level-1) + 2] * 1024lu * 1024lu;
  const int fraction = res->fractions[4*level + 2];
  return res->total_memory / 1024lu * fraction;
}

static void _check_resourcelevel(const char *key,
                                 int *fractions,
                                 const int level)
{
  const int g = level * 4;
  gchar out[128] = { 0 };
  if(!dt_conf_key_exists(key))
  {
    g_snprintf(out, 126, "%i %i %i %i",
               fractions[g], fractions[g+1], fractions[g+2], fractions[g+3]);
    dt_conf_set_string(key, out);
  }
  else
  {
    const gchar *in = dt_conf_get_string_const(key);
    sscanf(in, "%i %i %i %i", &fractions[g], &fractions[g+1], &fractions[g+2], &fractions[g+3]);
  }
}

void dt_dump_pfm_file(
        const char *pipe,
        const void* data,
        const int width,
        const int height,
        const int bpp,
        const char *modname,
        const char *head,
        const gboolean input,
        const gboolean output,
        const gboolean cpu)
{
  static int written = 0;

  char *path = g_build_filename(darktable.tmp_directory, pipe, NULL);
  char *fullname = NULL;

  if(!dt_util_test_writable_dir(path))
  {
    if(g_mkdir_with_parents(path, 0750))
    {
      dt_print(DT_DEBUG_ALWAYS, "%20s can't create directory '%s'", head, path);
      goto finalize;
    }
  }

  char fname[256]= { 0 };
  snprintf(fname, sizeof(fname), "%04d_%s_%s_%s%s.%s",
     written,
     modname,
     cpu ? "cpu" : "GPU",
     (input && output) ? "diff_" : ((!input && !output) ? "" : ((input) ? "in_" : "out_")),
     (bpp != 16) ? "M" : "C",
     (bpp==2) ? "ppm" : "pfm");

  if((width<1) || (height<1) || !data)
    goto finalize;

  fullname = g_build_filename(path, fname, NULL);

  FILE *f = g_fopen(fullname, "wb");
  if(f == NULL)
  {
    dt_print(DT_DEBUG_ALWAYS, "%20s can't write file '%s' in wb mode", head, fullname);
    goto finalize;
  }

  if(bpp==2)
    fprintf(f, "P5\n%d %d\n", width, height);
  else
    fprintf(f, "P%s\n%d %d\n-1.0\n", (bpp != 16) ? "f" : "F", width, height);

  for(int row = height - 1; row >= 0; row--)
  {
    for(int col = 0; col < width; col++)
    {
      const size_t blk = ((size_t)row * width + col) * bpp;
      fwrite(data + blk, (bpp==16) ? 12 : bpp, 1, f);
    }
  }

  dt_print(DT_DEBUG_ALWAYS, "%-20s %s,  %dx%d, bpp=%d", head, fullname, width, height, bpp);
  fclose(f);
  written += 1;

finalize:
  g_free(fullname);
  g_free(path);
}

void dt_dump_pfm(
        const char *filename,
        const void *data,
        const int width,
        const int height,
        const int bpp,
        const char *modname)
{
  if(!darktable.dump_pfm_module) return;
  if(!modname) return;
  if(!dt_str_commasubstring(darktable.dump_pfm_module, modname)) return;

  dt_dump_pfm_file(modname, data, width, height, bpp, filename, "[dt_dump_pfm]", FALSE, FALSE, TRUE);
}

void dt_dump_pipe_pfm(
        const char *mod,
        const void *data,
        const int width,
        const int height,
        const int bpp,
        const gboolean input,
        const char *pipe)
{
  if(!darktable.dump_pfm_pipe) return;
  if(!mod) return;
  if(!dt_str_commasubstring(darktable.dump_pfm_pipe, mod)) return;

  dt_dump_pfm_file(pipe, data, width, height, bpp, mod, "[dt_dump_pipe_pfm]", input, !input, TRUE);
}

void dt_dump_pipe_diff_pfm(
        const char *mod,
        const float *a,
        const float *b,
        const int width,
        const int height,
        const int ch,
        const char *pipe)
{
  if(!darktable.dump_diff_pipe) return;
  if(!mod) return;
  if(!dt_str_commasubstring(darktable.dump_diff_pipe, mod)) return;

  const size_t pk = (size_t)ch * width * height;
  float *o = dt_calloc_align_float(5 * pk);
  if(!o) return;

  DT_OMP_FOR()
  for(size_t p = 0; p < width * height; p++)
  {
    for(size_t c = 0; c < ch; c++)
    {
      const size_t k = ch * p +c;
      if(a[k] > NORM_MIN && b[k] > NORM_MIN)
      {
        o[k]      = 0.25f * a[k];
        o[1*pk+k] = CLIP(50.0f * CLIP(a[k] / b[k] - 1.0f));
        o[2*pk+k] = CLIP(100.0f * (a[k] - b[k]));
        o[3*pk+k] = CLIP(50.0f * CLIP(b[k] / a[k] - 1.0f));
        o[4*pk+k] = CLIP(100.0f * (b[k] - a[k]));
      }
    }
  }
  dt_dump_pfm_file(pipe, o, width, 5 * height, ch * sizeof(float), mod, "[dt_dump_CPU/GPU_diff_pfm]", TRUE, TRUE, TRUE);
  dt_free_align(o);
}

static int32_t _detect_opencl_job_run(dt_job_t *job)
{
  dt_opencl_init(darktable.opencl, GPOINTER_TO_INT(dt_control_job_get_params(job)), TRUE);
  return 0;
}

static dt_job_t *_detect_opencl_job_create(gboolean exclude_opencl)
{
  dt_job_t *job = dt_control_job_create(&_detect_opencl_job_run, "detect opencl devices");
  if(!job) return NULL;
  dt_control_job_set_params(job, GINT_TO_POINTER(exclude_opencl), NULL);
  return job;
}

static int32_t _backthumbs_job_run(dt_job_t *job)
{
  dt_update_thumbs_thread(dt_control_job_get_params(job));
  return 0;
}

static dt_job_t *_backthumbs_job_create(void)
{
  dt_job_t *job = dt_control_job_create(&_backthumbs_job_run, "generate mipmaps");
  if(!job) return NULL;
  dt_control_job_set_params(job, NULL, NULL);
  return job;
}

void dt_start_backtumbs_crawler(void)
{
  // don't write thumbs if using memory database or on a non-sufficient system
  if(!darktable.backthumbs.running && darktable.backthumbs.capable)
    dt_control_add_job(darktable.control, DT_JOB_QUEUE_SYSTEM_BG,
                   _backthumbs_job_create());
}

static char *_get_version_string(void)
{
#ifdef HAVE_LIBRAW
  const char *libraw_version = LIBRAW_VERSION_STR "\n";
#endif

#ifdef USE_LUA
  const char *lua_api_version = strcmp(LUA_API_VERSION_SUFFIX, "") ?
                                       STR(LUA_API_VERSION_MAJOR) "."
                                       STR(LUA_API_VERSION_MINOR) "."
                                       STR(LUA_API_VERSION_PATCH) "-"
                                       LUA_API_VERSION_SUFFIX :
                                       STR(LUA_API_VERSION_MAJOR) "."
                                       STR(LUA_API_VERSION_MINOR) "."
                                       STR(LUA_API_VERSION_PATCH) "\n";
#endif
char *version = g_strdup_printf(
               "darktable %s\n"
               "Copyright (C) 2012-%s Johannes Hanika and other contributors.\n\n"
               "Compile options:\n"
               "  Bit depth              -> %zu bit\n"
               "%s%s%s%s%s\n"
               "See %s for detailed documentation.\n"
               "See %s to report bugs.\n",
               darktable_package_version,
               darktable_last_commit_year,
               CHAR_BIT * sizeof(void *),

#ifdef _DEBUG
               "  Debug                  -> ENABLED\n"
#else
               "  Debug                  -> DISABLED\n"
#endif

#if defined(__SSE2__) && defined(__SSE__)
               "  SSE2 optimizations     -> ENABLED\n"
#else
               "  SSE2 optimizations     -> DISABLED\n"
#endif

#ifdef _OPENMP
               "  OpenMP                 -> ENABLED\n"
#else
               "  OpenMP                 -> DISABLED\n"
#endif

#ifdef HAVE_OPENCL
               "  OpenCL                 -> ENABLED\n"
#else
               "  OpenCL                 -> DISABLED\n"
#endif

#ifdef USE_LUA
               "  Lua                    -> ENABLED  - API version ", lua_api_version,
#else
               "  Lua                    -> DISABLED", "\n",
#endif

#ifdef USE_COLORDGTK
               "  Colord                 -> ENABLED\n"
#else
               "  Colord                 -> DISABLED\n"
#endif

#ifdef HAVE_GPHOTO2
               "  gPhoto2                -> ENABLED\n"
#else
               "  gPhoto2                -> DISABLED\n"
#endif

#ifdef HAVE_GMIC
               "  GMIC                   -> ENABLED  - Compressed LUTs are supported\n"
#else
               "  GMIC                   -> DISABLED - Compressed LUTs are NOT supported\n"
#endif

#ifdef HAVE_GRAPHICSMAGICK
               "  GraphicsMagick         -> ENABLED\n"
#else
               "  GraphicsMagick         -> DISABLED\n"
#endif

#ifdef HAVE_IMAGEMAGICK
               "  ImageMagick            -> ENABLED\n"
#else
               "  ImageMagick            -> DISABLED\n"
#endif

#ifdef HAVE_LIBAVIF
               "  libavif                -> ENABLED\n"
#else
               "  libavif                -> DISABLED\n"
#endif

#ifdef HAVE_LIBHEIF
               "  libheif                -> ENABLED\n"
#else
               "  libheif                -> DISABLED\n"
#endif

#ifdef HAVE_LIBJXL
               "  libjxl                 -> ENABLED\n"
#else
               "  libjxl                 -> DISABLED\n"
#endif

#ifdef HAVE_LIBRAW
               "  LibRaw                 -> ENABLED  - Version ", libraw_version,
#else
               "  LibRaw                 -> DISABLED", "\n",
#endif

#ifdef HAVE_OPENJPEG
               "  OpenJPEG               -> ENABLED\n"
#else
               "  OpenJPEG               -> DISABLED\n"
#endif

#ifdef HAVE_OPENEXR
               "  OpenEXR                -> ENABLED\n"
#else
               "  OpenEXR                -> DISABLED\n"
#endif

#ifdef HAVE_WEBP
               "  WebP                   -> ENABLED\n",
#else
               "  WebP                   -> DISABLED\n",
#endif

               PACKAGE_DOCS,
               PACKAGE_BUGREPORT);

  return version;
}

int dt_init(int argc, char *argv[], const gboolean init_gui, const gboolean load_data, lua_State *L)
{
  double start_wtime = dt_get_wtime();

#ifndef _WIN32
  if(getuid() == 0 || geteuid() == 0)
    dt_print(DT_DEBUG_ALWAYS,
        "WARNING: either your user id or the effective user id are 0. are you running darktable as root?");
#endif

  // make everything go a lot faster.
  dt_mm_enable_flush_zero();

  dt_set_signal_handlers();

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
  for(int k=0; k<DT_IMAGE_DBLOCKS; k++)
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

  darktable.dump_pfm_module = NULL;
  darktable.dump_pfm_pipe = NULL;
  darktable.dump_diff_pipe = NULL;
  darktable.tmp_directory = NULL;
  darktable.bench_module = NULL;

  gboolean exclude_opencl = TRUE;
  gboolean print_statistics = FALSE;
#ifdef HAVE_OPENCL
  exclude_opencl = FALSE;
  print_statistics = (strstr(argv[0], "darktable-cltest") == NULL);
#endif

#ifdef USE_LUA
  char *lua_command = NULL;
#endif

  darktable.num_openmp_threads = dt_get_num_procs();
  darktable.pipe_cache = TRUE;
  darktable.unmuted = 0;
  GSList *config_override = NULL;

  // keep a copy of argv array for possibly reporting later
  gchar **myoptions = init_gui && argc > 1 ? g_strdupv(argv) : NULL;

  for(int k = 1; k < argc; k++)
  {
#ifdef _WIN32
    if(!strcmp(argv[k], "/?"))
    {
      g_strfreev(myoptions);
      return usage(argv[0]);
    }
#endif
    if(argv[k][0] == '-')
    {
      if(!strcmp(argv[k], "--help") || !strcmp(argv[k], "-h"))
      {
        g_strfreev(myoptions);
        return usage(argv[0]);
      }

      else if(!strcmp(argv[k], "--version") || !strcmp(argv[k], "-v"))
      {
        char *theversion = _get_version_string();
        printf("%s", theversion);
        g_free(theversion);
        g_strfreev(myoptions);
        return 1;
      }
      else if(!strcmp(argv[k], "--dump-pfm") && argc > k + 1)
      {
        darktable.dump_pfm_module = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--bench-module") && argc > k + 1)
      {
        darktable.bench_module = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--dump-pipe") && argc > k + 1)
      {
        darktable.dump_pfm_pipe = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--dump-diff-pipe") && argc > k + 1)
      {
        darktable.dump_diff_pipe = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
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
      else if(!strcmp(argv[k], "--dumpdir") && argc > k + 1)
      {
        darktable.tmp_directory = g_strdup(argv[++k]);
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
        char *darg = argv[k + 1];
        dt_debug_thread_t dadd =
          !strcmp(darg, "common") ? DT_DEBUG_COMMON : // enable common processing options
          !strcmp(darg, "all") ? DT_DEBUG_ALL : // enable all debug information except verbose
          !strcmp(darg, "cache") ? DT_DEBUG_CACHE : // enable debugging for lib/film/cache module
          !strcmp(darg, "control") ? DT_DEBUG_CONTROL : // enable debugging for scheduler module
          !strcmp(darg, "dev") ? DT_DEBUG_DEV : // develop module
          !strcmp(darg, "input") ? DT_DEBUG_INPUT : // input devices
          !strcmp(darg, "camctl") ? DT_DEBUG_CAMCTL : // camera control module
          !strcmp(darg, "perf") ? DT_DEBUG_PERF : // performance measurements
          !strcmp(darg, "pwstorage") ? DT_DEBUG_PWSTORAGE : // pwstorage module
          !strcmp(darg, "opencl") ? DT_DEBUG_OPENCL : // gpu accel via opencl
          !strcmp(darg, "sql") ? DT_DEBUG_SQL : // SQLite3 queries
          !strcmp(darg, "memory") ? DT_DEBUG_MEMORY : // some stats on mem usage now and then.
          !strcmp(darg, "lighttable") ? DT_DEBUG_LIGHTTABLE : // lighttable related stuff.
          !strcmp(darg, "nan") ? DT_DEBUG_NAN : // check for NANs when processing the pipe.
          !strcmp(darg, "masks") ? DT_DEBUG_MASKS : // masks related stuff.
          !strcmp(darg, "lua") ? DT_DEBUG_LUA : // lua errors are reported on console
          !strcmp(darg, "print") ? DT_DEBUG_PRINT : // print errors are reported on console
          !strcmp(darg, "camsupport") ? DT_DEBUG_CAMERA_SUPPORT : // camera support warnings are reported on console
          !strcmp(darg, "ioporder") ? DT_DEBUG_IOPORDER : // iop order information are reported on console
          !strcmp(darg, "imageio") ? DT_DEBUG_IMAGEIO : // image importing or exporting messages on console
          !strcmp(darg, "undo") ? DT_DEBUG_UNDO : // undo/redo
          !strcmp(darg, "signal") ? DT_DEBUG_SIGNAL : // signal information on console
          !strcmp(darg, "params") ? DT_DEBUG_PARAMS : // iop module params checks on console
          !strcmp(darg, "act_on") ? DT_DEBUG_ACT_ON :
          !strcmp(darg, "tiling") ? DT_DEBUG_TILING :
          !strcmp(darg, "verbose") ? DT_DEBUG_VERBOSE :
          !strcmp(darg, "pipe") ? DT_DEBUG_PIPE :
          !strcmp(darg, "expose") ? DT_DEBUG_EXPOSE :
          !strcmp(darg, "picker") ? DT_DEBUG_PICKER :
          0;
        if(dadd)
          darktable.unmuted |= dadd;
        else
        {
          g_strfreev(myoptions);
          return usage(argv[0]);
        }
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
          dt_print(DT_DEBUG_ALWAYS, "[signal] print-trace not available, skipping");
#endif
        }
        else
        {
          g_strfreev(myoptions);
          return usage(argv[0]);
        }
        k++;
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--d-signal") && argc > k + 1)
      {
        gchar *str = g_ascii_strup(argv[k+1], -1);

        #define CHKSIGDBG(sig) else if(!g_strcmp0(str, #sig)) do {darktable.unmuted_signal_dbg[sig] = TRUE;} while(0)
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
          dt_print(DT_DEBUG_SIGNAL,
                   "[dt_init] unknown signal name: '%s'. use 'ALL'"
                   " to enable debug for all or use full signal name", str);
          g_strfreev(myoptions);
          return usage(argv[0]);
        }
        g_free(str);
        #undef CHKSIGDBG
        k++;
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if((argv[k][1] == 't' && argc > k + 1)
              || (!strcmp(argv[k], "--threads")
                  && argc > k + 1))
      {
        const int possible = dt_get_num_procs();
        const int desired = atol(argv[k + 1]);
        darktable.num_openmp_threads = CLAMP(desired, 1, possible);
        if(desired > possible)
          dt_print(DT_DEBUG_ALWAYS,
                   "[dt_init --threads] requested %d ompthreads restricted to %d",
            desired, possible);
        dt_print(DT_DEBUG_ALWAYS,
                 "[dt_init --threads] using %d threads for openmp parallel sections",
          darktable.num_openmp_threads);
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
          dt_conf_string_entry_t *entry = g_malloc(sizeof(dt_conf_string_entry_t));
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
      else if(!strcmp(argv[k], "--disable-pipecache"))
      {
        darktable.pipe_cache = FALSE;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--gimp"))
      {
        argv[k] = NULL;
        darktable.gimp.error = TRUE;

        if(argc > k + 1)
        {
          darktable.gimp.mode = argv[++k];
          argv[k-1] = NULL;
          argv[k] = NULL;

          if(dt_check_gimpmode("version"))
          {
            darktable.gimp.error = FALSE;
          }
          else if(dt_check_gimpmode("file") && (argc > k + 1))
          {
            darktable.gimp.path = argv[++k];
            argv[k-1] = NULL;
            argv[k] = NULL;

            if(g_file_test(darktable.gimp.path, G_FILE_TEST_IS_REGULAR))
              darktable.gimp.error = FALSE;
          }
          else if(dt_check_gimpmode("thumb") && (argc > k + 2))
          {
            darktable.gimp.path = argv[++k];
            argv[k-1] = NULL;
            argv[k] = NULL;

            if(g_file_test(darktable.gimp.path, G_FILE_TEST_IS_REGULAR))
            {
              darktable.gimp.size = atol(argv[k + 1]);
              k++;
              argv[k-1] = NULL;
              argv[k] = NULL;

              if(darktable.gimp.size > 0)
                darktable.gimp.error = FALSE;
            }
          }
        }
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
      {
        g_strfreev(myoptions);
        return usage(argv[0]); // fail on unrecognized options
      }
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

  /* We now have all command line options ready and check for gimp API questions.
      Return right now if we
        - check for API version
        - check for "file" or "thumb" and the file is not physically available
        - have an undefined gimp mode
      and let the caller check again and write out protocol messages.
  */
  if(dt_check_gimpmode("version")
    || (dt_check_gimpmode("file") && !dt_check_gimpmode_ok("file"))
    || (dt_check_gimpmode("thumb") && !dt_check_gimpmode_ok("thumb"))
    || darktable.gimp.error)
    return 0;

  if(darktable.unmuted)
  {
    char *theversion = _get_version_string();
    dt_print_nts(DT_DEBUG_ALWAYS, "%s\n", theversion);
    g_free(theversion);
  }

  if(myoptions)
  {
    dt_print(DT_DEBUG_ALWAYS, "[dt starting]");
    int k = 0;
    while(myoptions[k])
      dt_print_nts(DT_DEBUG_ALWAYS, " %s", myoptions[k++]);
    dt_print_nts(DT_DEBUG_ALWAYS, "\n");
    g_strfreev(myoptions);
  }

  if(darktable.dump_pfm_module
     || darktable.dump_pfm_pipe
     || darktable.dump_pfm_pipe
     || darktable.dump_diff_pipe)
  {
    if(darktable.tmp_directory == NULL)
      darktable.tmp_directory = g_dir_make_tmp("darktable_XXXXXX", NULL);
    dt_print(DT_DEBUG_ALWAYS,
             "[init] darktable dump directory is '%s'",
             (darktable.tmp_directory) ?: "NOT AVAILABLE");
  }

  // Set directories as requested or default.
  // Set a result flag so if we can't create certain directories, we can
  // later, after initializing the GUI, show the user a message and exit.
  const gboolean user_dirs_are_created = dt_loc_init(datadir_from_command,
                                                     moduledir_from_command,
                                                     localedir_from_command,
                                                     configdir_from_command,
                                                     cachedir_from_command,
                                                     tmpdir_from_command);

  dt_print_mem_usage("at startup");

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
        new_xdg_data_dirs =
          g_strjoin(G_SEARCHPATH_SEPARATOR_S, sharedir, xdg_data_dirs, NULL);
    }
    else
    {
#ifndef _WIN32
      // see http://standards.freedesktop.org/basedir-spec/latest/ar01s03.html for a reason to use those as a
      // default
      if(!g_strcmp0(sharedir, "/usr/local/share")
         || !g_strcmp0(sharedir, "/usr/local/share/")
         || !g_strcmp0(sharedir, "/usr/share") || !g_strcmp0(sharedir, "/usr/share/"))
        new_xdg_data_dirs = g_strdup("/usr/local/share/"
                                     G_SEARCHPATH_SEPARATOR_S "/usr/share/");
      else
        new_xdg_data_dirs =
          g_strdup_printf("%s" G_SEARCHPATH_SEPARATOR_S "/usr/local/share/"
                          G_SEARCHPATH_SEPARATOR_S "/usr/share/", sharedir);
#else
      set_env = FALSE;
#endif
    }

    if(set_env) g_setenv("XDG_DATA_DIRS", new_xdg_data_dirs, 1);
    dt_print(DT_DEBUG_DEV, "new_xdg_data_dirs: %s", new_xdg_data_dirs);
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

    // make sure that we have no stale global progress bar
    // visible. thus it's run as early as possible
    dt_control_progress_init(darktable.control);

    // ensure that we can load the Gtk theme early enough that the splash screen
    // doesn't change as we progress through startup
    darktable.gui = (dt_gui_gtk_t *)calloc(1, sizeof(dt_gui_gtk_t));
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

  // set the interface language and prepare selection for prefs & confgen
  darktable.l10n = dt_l10n_init(darktablerc, init_gui);

  // initialize the configuration default/min/max
  dt_confgen_init();

  // read actual configuration, needs confgen above for sanitizing values
  dt_conf_init(darktable.conf, darktablerc, config_override);

  g_slist_free_full(config_override, g_free);

  const int last_configure_version =
    dt_conf_get_int("performance_configuration_version_completed");

  // we need this REALLY early so that error messages can be shown,
  // however after gtk_disable_setlocale
  if(init_gui)
  {
#ifdef GDK_WINDOWING_WAYLAND
    // There are currently bad interactions with Wayland (drop-downs
    // are very narrow, scroll events lost). Until this is fixed, give
    // priority to the XWayland backend for Wayland users.
    // See also https://github.com/darktable-org/darktable/issues/13180
    gdk_set_allowed_backends("x11,*");
#endif
    gtk_init(&argc, &argv);

    darktable.themes = NULL;
    dt_gui_theme_init(darktable.gui);

    if(!user_dirs_are_created)
    {
      char *user_dirs_failure_text = g_markup_printf_escaped(
        _("you do not have write access to create one of the user directories\n"
          "\n"
          "see the log for more details\n"
          "\n"
          "please fix this and then run darktable again"));
      dt_gui_show_standalone_yes_no_dialog(_("darktable - unable to create directories"),
                                           user_dirs_failure_text,
                                           _("_quit darktable"),
                                           NULL);
      // There is no REAL need to free the string before exiting, but we do it
      // to avoid creating a code pattern that could be mistakenly copy-pasted
      // somewhere else where freeing memory would actually be needed.
      g_free(user_dirs_failure_text);
      exit(EXIT_FAILURE);
    }

    darktable_splash_screen_create(NULL, FALSE);
  }

  // detect cpu features and decide which codepaths to enable
  dt_codepaths_init();

  // get the list of color profiles
  darktable.color_profiles = dt_colorspaces_init();

  // initialize datetime data
  dt_datetime_init();

  // initialize the database
  darktable_splash_screen_set_progress(_("opening image library"));
  darktable.db = dt_database_init(dbfilename_from_command, load_data, init_gui);
  if(darktable.db == NULL)
  {
    dt_print(DT_DEBUG_ALWAYS, "ERROR : cannot open database");
    darktable_splash_screen_destroy();
    return 1;
  }
  else if(!dt_database_get_lock_acquired(darktable.db))
  {
    gboolean image_loaded_elsewhere = FALSE;
    if(init_gui && argc > 1)
    {
      darktable_splash_screen_set_progress(_("forwarding image(s) to running instance"));

      // send the images to the other instance via dbus
      dt_print(DT_DEBUG_ALWAYS,
               "[dt_init] trying to open the images in the running instance");

      GDBusConnection *connection = NULL;
      for(int i = 1; i < argc; i++)
      {
        // make the filename absolute ...
        if(argv[i] == NULL || *argv[i] == '\0') continue;
        gchar *filename = dt_util_normalize_path(argv[i]);
        if(filename == NULL) continue;
        if(!connection) connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
        // ... and send it to the running instance of darktable
        image_loaded_elsewhere =
          g_dbus_connection_call_sync(connection, "org.darktable.service", "/darktable",
                                      "org.darktable.service.Remote", "Open",
                                      g_variant_new("(s)", filename), NULL,
                                      G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL) != NULL;
        g_free(filename);
      }
      if(connection) g_object_unref(connection);
    }
    darktable_splash_screen_destroy(); // dismiss splash screen before potentially showing error dialog
    if(!image_loaded_elsewhere) dt_database_show_error(darktable.db);

    dt_print(DT_DEBUG_ALWAYS, "ERROR: can't acquire database lock, aborting.");
    return 1;
  }

  darktable_splash_screen_set_progress(_("preparing database"));
  dt_upgrade_maker_model(darktable.db);

  // init darktable tags table
  dt_set_darktable_tags();

  // Initialize the signal system
  darktable.signals = dt_control_signal_init();

  if(init_gui)
  {
    dt_control_init(darktable.control);

    // initialize undo struct
    darktable.undo = dt_undo_init();
  }
  else
  {
    if(dbfilename_from_command && !strcmp(dbfilename_from_command, ":memory:"))
      dt_gui_presets_init(); // init preset db schema.

    dt_atomic_set_int(&darktable.control->running, DT_CONTROL_STATE_DISABLED);
    dt_pthread_mutex_init(&darktable.control->log_mutex, NULL);
  }

  // import default styles from shared directory
  gchar *styledir = g_build_filename(sharedir, "darktable/styles", NULL);
  if(styledir)
    dt_import_default_styles(styledir);
  g_free(styledir);

  // we initialize grouping early because it's needed for collection init
  // idem for folder reachability
  if(init_gui)
  {
    darktable.gui->grouping = dt_conf_get_bool("ui_last/grouping");
    dt_film_set_folder_status();
  }

  // the update crawl needs to run after db and conf are up, but before LUA
  // starts any scripts
  GList *changed_xmp_files = NULL;
  if(init_gui)
  {
    if(dt_conf_get_bool("run_crawler_on_start") && !dt_gimpmode())
    {
      darktable_splash_screen_create(NULL, TRUE); // force the splash screen for the crawl even if user-disabled
      // scan for cases where the database and xmp files have different timestamps
      changed_xmp_files = dt_control_crawler_run();
      if(!dt_conf_get_bool("show_splash_screen"))
      {
        darktable_splash_screen_destroy();
        dt_gui_process_events(); // ensure that the splash screen is removed right away
      }
    }
  }

  /* for every resourcelevel we have 4 ints defined, either absolute or a fraction
     0 cpu available
     1 cpu singlebuffer
     2 mipmap size
     3 opencl available
  */
  /* special modes are meant to be used for debugging & testing, they
     are hidden in the ui menu and must be activated via --conf
     resourcelevel="xxx" here all values are absolutes in MB as we
     require fixed settings.  reference, mini and notebook require a
     cl capable system with 16GB of ram and 2GB of free video ram
  */
  static int ref_resources[12] = {
      8192,  32,  512, 2048,   // reference
      1024,   2,  128,  200,   // mini system
      4096,  32,  512, 1024,   // simple notebook with integrated graphics
  };

  /* This is where the sync is to be done if the enum for pref
     resourcelevel in darktableconfig.xml.in is changed.  all values
     are fractions val/1024 of total memory (0-2) or available OpenCL
     memory
  */
  static int fractions[16] = {
      128,    4,  64,  400, // small
      512,    8, 128,  700, // default
      700,   16, 128,  900, // large
    16384, 1024, 128,  900, // unrestricted
  };

  // Allow the settings for each UI performance level to be changed via darktablerc
  _check_resourcelevel("resource_small", fractions, 0);
  _check_resourcelevel("resource_default", fractions, 1);
  _check_resourcelevel("resource_large", fractions, 2);
  _check_resourcelevel("resource_unrestricted", fractions, 3);

  dt_sys_resources_t *res = &darktable.dtresources;
  res->fractions = fractions;
  res->refresource = ref_resources;
  res->total_memory = _get_total_memory() * 1024lu;

  char *config_info = calloc(1, DT_PERF_INFOSIZE);
  if(last_configure_version != DT_CURRENT_PERFORMANCE_CONFIGURE_VERSION
    && !dt_gimpmode())
  {
    dt_configure_runtime_performance(last_configure_version, config_info);
  }

  dt_get_sysresource_level();
  res->mipmap_memory = _get_mipmap_size();
  dt_print(DT_DEBUG_MEMORY | DT_DEBUG_DEV,
    "  mipmap cache:    %luMB", res->mipmap_memory / 1024lu / 1024lu);
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
  darktable_splash_screen_set_progress(_("initializing GraphicsMagick"));
  /* GraphicsMagick init */
#ifndef MAGICK_OPT_NO_SIGNAL_HANDER
  InitializeMagick(darktable.progname);

  // *SIGH*
  dt_set_signal_handlers();
#else
  InitializeMagickEx(darktable.progname, MAGICK_OPT_NO_SIGNAL_HANDER, NULL);
#endif
#elif defined HAVE_IMAGEMAGICK
  /* ImageMagick init */
  darktable_splash_screen_set_progress(_("initializing ImageMagick"));
  MagickWandGenesis();
#endif

#ifdef HAVE_LIBHEIF
  darktable_splash_screen_set_progress(_("initializing libheif"));
  heif_init(NULL);
#endif

  darktable_splash_screen_set_progress(_("starting OpenCL"));
  darktable.opencl = (dt_opencl_t *)calloc(1, sizeof(dt_opencl_t));
  if(init_gui)
    dt_control_add_job(darktable.control, DT_JOB_QUEUE_SYSTEM_BG,
                       _detect_opencl_job_create(exclude_opencl));
  else
    dt_opencl_init(darktable.opencl, exclude_opencl, print_statistics);

  darktable.points = (dt_points_t *)calloc(1, sizeof(dt_points_t));
  dt_points_init(darktable.points, dt_get_num_threads());

  dt_wb_presets_init(NULL);

  darktable_splash_screen_set_progress(_("loading noise profiles"));
  darktable.noiseprofile_parser = dt_noiseprofile_init(noiseprofiles_from_command);

  // must come before mipmap_cache, because that one will need to access
  // image dimensions stored in here:
  darktable.image_cache = (dt_image_cache_t *)calloc(1, sizeof(dt_image_cache_t));
  dt_image_cache_init(darktable.image_cache);

  darktable.mipmap_cache = (dt_mipmap_cache_t *)calloc(1, sizeof(dt_mipmap_cache_t));
  dt_mipmap_cache_init(darktable.mipmap_cache);

  // set up the list of exiv2 metadata
  dt_exif_set_exiv2_taglist();

  // init metadata flags
  dt_metadata_init();

  darktable_splash_screen_set_progress(_("synchronizing local copies"));
  dt_image_local_copy_synch();

#ifdef HAVE_GPHOTO2
  // Initialize the camera control.  this is done late so that the
  // gui can react to the signal sent but before switching to
  // lighttable!
  darktable_splash_screen_set_progress(_("initializing camera control"));
  darktable.camctl = dt_camctl_new();
#endif

  // The GUI must be initialized before the views, because the init()
  // functions of the views depend on darktable.control->accels_* to
  // register their keyboard accelerators

  if(init_gui)
  {
    darktable_splash_screen_set_progress(_("initializing GUI"));
    if(dt_gui_gtk_init(darktable.gui))
    {
      dt_print(DT_DEBUG_ALWAYS, "[dt_init] ERROR: can't init gui, aborting.");
      darktable_splash_screen_destroy();
      return 1;
    }
    dt_bauhaus_init();
  }
  else
    darktable.gui = NULL;

  darktable.view_manager = (dt_view_manager_t *)calloc(1, sizeof(dt_view_manager_t));
  dt_view_manager_init(darktable.view_manager);

  // check whether we were able to load darkroom view. if we failed,
  // we'll crash everywhere later on.
  if(!darktable.develop)
  {
    dt_print(DT_DEBUG_ALWAYS, "[dt_init] ERROR: can't init develop system, aborting.");
    darktable_splash_screen_destroy();
    return 1;
  }

  darktable_splash_screen_set_progress(_("loading processing modules"));
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
    dt_print(DT_DEBUG_ALWAYS, "[dt_init] ERROR: iop order looks bad, aborting.");
    darktable_splash_screen_destroy();
    return 1;
  }

  if(darktable.dump_pfm_module)
    dt_print(DT_DEBUG_ALWAYS,
             "[dt_init] writing intermediate pfm files for module '%s'",
      darktable.dump_pfm_module);

  if(darktable.dump_pfm_pipe)
    dt_print(DT_DEBUG_ALWAYS,
             "[dt_init] writing pfm files for module '%s' processing the pipeline",
      darktable.dump_pfm_pipe);

  if(darktable.dump_diff_pipe)
    dt_print(DT_DEBUG_ALWAYS,
             "[dt_init] writing CPU/GPU diff pfm files for module '%s' processing the pipeline",
      darktable.dump_diff_pipe);

  if(init_gui)
  {
    darktable.lib = (dt_lib_t *)calloc(1, sizeof(dt_lib_t));
    dt_lib_init(darktable.lib);

    // init the gui part of views
    dt_view_manager_gui_init(darktable.view_manager);
  }

/* init lua last, since it's user made stuff it must be in the real environment */
#ifdef USE_LUA
  darktable_splash_screen_set_progress(_("initializing Lua"));
  // after the following Lua startup call, we can no longer use dt_gui_process_events() or we hang;
  // this also means no more calls to darktable_splash_screen_set_progress()
  dt_lua_init(darktable.lua_state.state, lua_command);
#else
  darktable_splash_screen_set_progress(_(""));
#endif

  if(init_gui)
  {
    dt_ctl_switch_mode_to("lighttable");

    // Save the default shortcuts
    dt_shortcuts_save(".defaults", FALSE);

    // Then load any shortcuts if available (wipe defaults first if requested)
    dt_shortcuts_load(NULL, !dt_conf_get_bool("accel/load_defaults"));

    // Save the shortcuts including defaults
    dt_shortcuts_save(NULL, TRUE);

    // connect the shortcut dispatcher
    g_signal_connect(dt_ui_main_window(darktable.gui->ui), "event",
                     G_CALLBACK(dt_shortcut_dispatcher), NULL);

    // load image(s) specified on cmdline.  this has to happen after
    // lua is initialized as image import can run lua code
    if(argc == 2 && !_is_directory(argv[1]))
    {
      // If only one image is listed, attempt to load it in darkroom
#ifndef USE_LUA      // may cause UI hang since after LUA init
      darktable_splash_screen_set_progress(_("importing image"));
#endif
      dt_load_from_string(argv[1], TRUE, NULL);
    }
    else if(argc >= 2)
    {
      // when multiple names are given, or we are given a directory,
      // fire up a background job to import them after switching to
      // lighttable showing the filmroll for the first one
      _switch_to_new_filmroll(argv[1]);
      dt_control_add_job(darktable.control,
                         DT_JOB_QUEUE_USER_BG, dt_pathlist_import_create(argc,argv));
    }

    // there might be some info created in dt_configure_runtime_performance() for feedback
    if(!dt_gimpmode())
    {
      gboolean not_again = TRUE;
      if(last_configure_version && config_info[0])
      {
        not_again = dt_gui_show_standalone_yes_no_dialog(
          _("configuration information"),
            config_info,
          _("_show this message again"), _("_dismiss"));
      }

      if(not_again || (last_configure_version == 0))
      {
        dt_conf_set_int("performance_configuration_version_completed",
                      DT_CURRENT_PERFORMANCE_CONFIGURE_VERSION);
      }
    }
  }
  free(config_info);

  darktable.backthumbs.running = FALSE;
  darktable.backthumbs.capable =
      (dt_worker_threads() > 4)
      && !dt_gimpmode()
      && !(dbfilename_from_command && !strcmp(dbfilename_from_command, ":memory:"));

  if(init_gui)
  {
    // construct the popup that asks the user how to handle images whose xmp
    // files are newer than the db entry
    if(changed_xmp_files)
      dt_control_crawler_show_image_list(changed_xmp_files);

    if(!dt_gimpmode())
     dt_start_backtumbs_crawler();
  }

  // fire up a background job to perform sidecar writes
  dt_control_sidecar_synch_start();

#if defined(WIN32)
  dt_capabilities_add("windows");
  dt_capabilities_add("nonapple");
#elif defined(__APPLE__)
  dt_capabilities_add("apple");
#else
  dt_capabilities_add("linux");
  dt_capabilities_add("nonapple");
#endif

  if(init_gui)
  {
    // show the main window and restore its geometry to that saved in the config file
    gtk_widget_show_all(dt_ui_main_window(darktable.gui->ui));
    dt_gui_gtk_load_config();
    darktable_splash_screen_destroy();

    // finally set the cursor to be the default.
    // for some reason this is needed on some systems to pick up the correctly themed cursor
    dt_control_change_cursor(GDK_LEFT_PTR);
  }

  dt_print(DT_DEBUG_CONTROL,
           "[dt_init] startup took %f seconds", dt_get_wtime() - start_wtime);

  dt_print_mem_usage("after successful startup");

  return 0;
}

void dt_get_sysresource_level()
{
  static int oldlevel = -999;

  dt_sys_resources_t *res = &darktable.dtresources;
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
  if(config && !dt_gimpmode())
  {
         if(!strcmp(config, "default"))      level = 1;
    else if(!strcmp(config, "small"))        level = 0;
    else if(!strcmp(config, "large"))        level = 2;
    else if(!strcmp(config, "unrestricted")) level = 3;
    else if(!strcmp(config, "reference"))    level = -1;
    else if(!strcmp(config, "mini"))         level = -2;
    else if(!strcmp(config, "notebook"))     level = -3;
  }

  if(level != oldlevel)
  {
    oldlevel = res->level = level;
    dt_print(DT_DEBUG_MEMORY | DT_DEBUG_DEV,
             "[dt_get_sysresource_level] switched to `%s'", config);
    dt_print(DT_DEBUG_MEMORY | DT_DEBUG_DEV,
             "  total mem:       %luMB", res->total_memory / 1024lu / 1024lu);
    dt_print(DT_DEBUG_MEMORY | DT_DEBUG_DEV,
             "  available mem:   %luMB", dt_get_available_mem() / 1024lu / 1024lu);
    dt_print(DT_DEBUG_MEMORY | DT_DEBUG_DEV,
             "  singlebuff:      %luMB", dt_get_singlebuffer_mem() / 1024lu / 1024lu);
  }
}

void dt_cleanup()
{
  const gboolean init_gui = (darktable.gui != NULL);

//  if(init_gui)
//    darktable_exit_screen_create(NULL, FALSE);

  if(darktable.backthumbs.running)
  {
    // if the backthumbs crawler is running, stop it now and wait for it being terminated.
    darktable.backthumbs.running = FALSE;
    for(int i = 0; i < 1000 && darktable.backthumbs.capable; i++)
      g_usleep(10000);
  }
  // last chance to ask user for any input...

  const gboolean perform_maintenance = dt_database_maybe_maintenance(darktable.db);
  const gboolean perform_snapshot = dt_database_maybe_snapshot(darktable.db);
  gchar **snaps_to_remove = NULL;
  if(perform_snapshot)
  {
    snaps_to_remove = dt_database_snaps_to_remove(darktable.db);
    if(init_gui) dt_gui_process_events();
  }

#ifdef HAVE_PRINT
  dt_printers_abort_discovery();
  if(init_gui) dt_gui_process_events();
#endif

#ifdef USE_LUA
  dt_lua_finalize_early();
#endif

  // anything that asks user for input should be placed before this line

  if(init_gui)
  {
    // hide main window and do rest of the cleanup in the background
    gtk_widget_hide(dt_ui_main_window(darktable.gui->ui));
    dt_gui_process_events();

    dt_ctl_switch_mode_to("");
    dt_dbus_destroy(darktable.dbus);

    dt_lib_cleanup(darktable.lib);
    free(darktable.lib);
  }
#ifdef USE_LUA
  dt_lua_finalize();
#endif
  dt_view_manager_cleanup(darktable.view_manager);
  free(darktable.view_manager);
  darktable.view_manager = NULL;
  // we can no longer call dt_gui_process_events after this point, as that will cause a segfault
  // if some delayed event fires

  dt_image_cache_cleanup(darktable.image_cache);
  free(darktable.image_cache);
  darktable.image_cache = NULL;
  dt_mipmap_cache_cleanup(darktable.mipmap_cache);
  free(darktable.mipmap_cache);
  darktable.mipmap_cache = NULL;
  if(init_gui)
  {
    dt_imageio_cleanup(darktable.imageio);
    free(darktable.imageio);
    darktable.imageio = NULL;
    dt_control_shutdown(darktable.control);
    dt_control_cleanup(darktable.control);
    free(darktable.control);
    darktable.control = NULL;
    dt_undo_cleanup(darktable.undo);
    darktable.undo = NULL;
    free(darktable.gui);
    darktable.gui = NULL;
  }

  dt_colorspaces_cleanup(darktable.color_profiles);
  dt_conf_cleanup(darktable.conf);
  free(darktable.conf);
  darktable.conf = NULL;
  dt_points_cleanup(darktable.points);
  free(darktable.points);
  darktable.points = NULL;
  dt_iop_unload_modules_so();
  g_list_free_full(darktable.iop_order_list, free);
  darktable.iop_order_list = NULL;
  g_list_free_full(darktable.iop_order_rules, free);
  darktable.iop_order_rules = NULL;
  dt_opencl_cleanup(darktable.opencl);
  free(darktable.opencl);
  darktable.opencl = NULL;
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

#ifdef HAVE_LIBHEIF
  heif_deinit();
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
        g_chmod(snaps_to_remove[i],
                S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);

        dt_print(DT_DEBUG_SQL, "[db backup] removing old snap: %s... ", snaps_to_remove[i]);
        const int retunlink = g_remove(snaps_to_remove[i++]);
        dt_print(DT_DEBUG_SQL, "%s", retunlink == 0 ? "success" : "failed!");
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

  if(darktable.noiseprofile_parser)
  {
    g_object_unref(darktable.noiseprofile_parser);
    darktable.noiseprofile_parser = NULL;
  }

  dt_capabilities_cleanup();

  if(darktable.tmp_directory)
    g_free(darktable.tmp_directory);

  for(int k=0; k<DT_IMAGE_DBLOCKS; k++)
  {
    dt_pthread_mutex_destroy(&(darktable.db_image[k]));
  }
  dt_pthread_mutex_destroy(&(darktable.plugin_threadsafe));
  dt_pthread_mutex_destroy(&(darktable.dev_threadsafe));
  dt_pthread_mutex_destroy(&(darktable.capabilities_threadsafe));
  dt_pthread_mutex_destroy(&(darktable.exiv2_threadsafe));
  dt_pthread_mutex_destroy(&(darktable.readFile_mutex));

  dt_exif_cleanup();

  if(init_gui)
    darktable_exit_screen_destroy();
}

/* The dt_print variations can be used with a combination of DT_DEBUG_ flags.
   Two special cases are supported also:
   a) if you combine with DT_DEBUG_VERBOSE, output will only be written if dt had
      been started with -d verbose
   b) 'thread' may be identical to DT_DEBUG_ALWAYS to write output
*/
void dt_print_ext(const char *msg, ...)
{
  char vbuf[2048];
  va_list ap;
  va_start(ap, msg);
  vsnprintf(vbuf, sizeof(vbuf), msg, ap);
  va_end(ap);

  printf("%11.4f %s\n", dt_get_wtime() - darktable.start_wtime, vbuf);
  fflush(stdout);
}

void dt_print_nts_ext(const char *msg, ...)
{
  va_list ap;
  va_start(ap, msg);
  vprintf(msg, ap);
  va_end(ap);
  fflush(stdout);
}

void *dt_alloc_aligned(const size_t size)
{
  const size_t alignment = DT_CACHELINE_BYTES;
  const size_t aligned_size = dt_round_size(size, alignment);
#if defined(__FreeBSD_version) && __FreeBSD_version < 700013
  return malloc(aligned_size);
#elif defined(_WIN32)
  return _aligned_malloc(aligned_size, alignment);
#elif defined(_DEBUG)
  // for a debug build, ensure that we get a crash if we use plain
  // free() to release the allocated memory, by returning a pointer
  // which isn't a valid memory block address
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

#ifdef _WIN32
void dt_free_align(void *mem)
{
  _aligned_free(mem);
}
#elif defined(_DEBUG)
void dt_free_align(void *mem)
{
  // on a debug build, we deliberately offset the returned pointer
  // from dt_alloc_align, so eliminate the offset
  if(mem)
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
    char buf[140]; /* Arbitrary size, should be lots big enough for
                    * everything used in DT */
    snprintf(buf, sizeof(buf),
             "%s took %.3f secs (%.3f CPU)",
             prefix, end.clock - start->clock,
             end.user - start->user);
    dt_print(DT_DEBUG_PERF, "%s", buf);
  }
}

void dt_show_times_f(const dt_times_t *start,
                     const char *prefix,
                     const char *suffix, ...)
{
  /* Skip all the calculations an everything if -d perf isn't on */
  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_times_t end;
    dt_get_times(&end);
    char buf[160]; /* Arbitrary size, should be lots big enough for
                    * everything used in DT */
    const int n = snprintf(buf,
                           sizeof(buf),
                           "%s took %.3f secs (%.3f CPU) ",
                           prefix, end.clock - start->clock,
                           end.user - start->user);
    if(n < sizeof(buf) - 1)
    {
      va_list ap;
      va_start(ap, suffix);
      vsnprintf(buf + n, sizeof(buf) - n, suffix, ap);
      va_end(ap);
    }
    dt_print(DT_DEBUG_PERF, "%s", buf);
  }
}

int dt_worker_threads()
{
  const int wthreads = (_get_total_memory() >> 19) >= 15 && dt_get_num_threads() >= 4 ? 7 : 4;
  dt_print(DT_DEBUG_DEV, "[dt_worker_threads] using %i worker threads", wthreads);
  return wthreads;
}

size_t dt_get_available_mem()
{
  dt_sys_resources_t *res = &darktable.dtresources;
  const int level = res->level;
  if(level < 0)
    return res->refresource[4*(-level-1)] * 1024lu * 1024lu;

  const int fraction = res->fractions[4*level];
  return MAX(512lu * 1024lu * 1024lu, res->total_memory / 1024lu * fraction);
}

size_t dt_get_singlebuffer_mem()
{
  dt_sys_resources_t *res = &darktable.dtresources;
  const int level = res->level;
  if(level < 0)
    return res->refresource[4*(-level-1) + 1] * 1024lu * 1024lu;

  const int fraction = res->fractions[4*level + 1];
  return MAX(2lu * 1024lu * 1024lu, res->total_memory / 1024lu * fraction);
}

void dt_configure_runtime_performance(const int old, char *info)
{
  const size_t threads = dt_get_num_procs();
  const size_t mem = darktable.dtresources.total_memory / 1024lu / 1024lu;
  const size_t bits = CHAR_BIT * sizeof(void *);
  const gboolean sufficient = mem >= 4096 && threads >= 2;

  dt_print(DT_DEBUG_DEV,
           "[dt_configure_runtime_performance] found a %s %zu-bit"
           " system with %zu Mb ram and %zu cores",
           (sufficient) ? "sufficient" : "low performance",
           bits, mem, threads);

  // All runtime conf settings only write data if there is no valid
  // data found in conf

  if(!dt_conf_key_not_empty("resourcelevel"))
  {
    dt_conf_set_string("resourcelevel", (sufficient) ? "default" : "small");
    dt_print(DT_DEBUG_DEV,
             "[dt_configure_runtime_performance] resourcelevel=%s",
             (sufficient) ? "default" : "small");
  }

  if(!dt_conf_key_not_empty("cache_disk_backend_full"))
  {
    char cachedir[PATH_MAX] = { 0 };
    guint64 freecache = 0;
    dt_loc_get_user_cache_dir(cachedir, sizeof(cachedir));
    GFile *gfile = g_file_new_for_path(cachedir);
    GFileInfo *gfileinfo =
      g_file_query_filesystem_info(gfile, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, NULL, NULL);
    if(gfileinfo != NULL)
      freecache =
        g_file_info_get_attribute_uint64(gfileinfo, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
    g_object_unref(gfile);
    g_object_unref(gfileinfo);
    const gboolean largedisk = freecache > (8lu << 20);
    // enable cache_disk_backend_full when user has over 8gb free diskspace
    dt_conf_set_bool("cache_disk_backend_full", largedisk);
    dt_print(DT_DEBUG_DEV,
             "[dt_configure_runtime_performance] cache_disk_backend_full=%s",
             (largedisk) ? "TRUE" : "FALSE");
  }

  gboolean updated_mandatory = FALSE;
  if(!dt_conf_key_not_empty("opencl_mandatory_timeout"))
  {
    const int timeout = dt_conf_get_int("opencl_mandatory_timeout");
    if(timeout < 1000)
    {
      dt_conf_set_int("opencl_mandatory_timeout", 1000);
      updated_mandatory = TRUE;
    }
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
    g_strlcat(info, _("some global config parameters relevant for OpenCL performance are not used any longer."), DT_PERF_INFOSIZE);
    g_strlcat(info, "\n", DT_PERF_INFOSIZE);
    g_strlcat(info, _("instead you will find 'per device' data in 'cldevice_v5_canonical-name'. content is:"), DT_PERF_INFOSIZE);
    g_strlcat(info, "\n  ", DT_PERF_INFOSIZE);
    g_strlcat(info, _(" 'avoid_atomics' 'micro_nap' 'pinned_memory' 'roundupwd' 'roundupht' 'eventhandles' 'async' 'disable' 'magic' 'advantage' 'unified'"), DT_PERF_INFOSIZE);
    g_strlcat(info, "\n", DT_PERF_INFOSIZE);
    g_strlcat(info, _("you may tune as before except 'magic'"), DT_PERF_INFOSIZE);
    g_strlcat(info, "\n\n", DT_PERF_INFOSIZE);
  }
  else if(old < 13)
  {
    g_strlcat(info, INFO_HEADER, DT_PERF_INFOSIZE);
    g_strlcat(info, _("your OpenCL compiler settings for all devices have been reset to default."), DT_PERF_INFOSIZE);
    g_strlcat(info, "\n\n", DT_PERF_INFOSIZE);
  }

  else if(old < 14)
  {
    g_strlcat(info, INFO_HEADER, DT_PERF_INFOSIZE);
    g_strlcat(info, _("OpenCL global config parameters 'per device' data has been recreated with an updated name."), DT_PERF_INFOSIZE);
    g_strlcat(info, "\n", DT_PERF_INFOSIZE);
    g_strlcat(info, _("you will find 'per device' data in 'cldevice_v5_canonical-name'. content is:"), DT_PERF_INFOSIZE);
    g_strlcat(info, "\n  ", DT_PERF_INFOSIZE);
    g_strlcat(info, _(" 'avoid_atomics' 'micro_nap' 'pinned_memory' 'roundupwd' 'roundupht' 'eventhandles' 'async' 'disable' 'magic' 'advantage' 'unified'"), DT_PERF_INFOSIZE);
    g_strlcat(info, "\n", DT_PERF_INFOSIZE);
    g_strlcat(info, _("you may tune as before except 'magic'"), DT_PERF_INFOSIZE);
    g_strlcat(info, "\n", DT_PERF_INFOSIZE);
    g_strlcat(info, _("If you're using device names in 'opencl_device_priority' you should update them to the new names."), DT_PERF_INFOSIZE);
    g_strlcat(info, "\n\n", DT_PERF_INFOSIZE);
  }

  else if(old < 15)
  {
    g_strlcat(info, INFO_HEADER, DT_PERF_INFOSIZE);
    g_strlcat(info, _("OpenCL 'per device' config data have been automatically extended by 'unified-fraction'."), DT_PERF_INFOSIZE);
    g_strlcat(info, "\n", DT_PERF_INFOSIZE);
    g_strlcat(info, _("you will find 'per device' data in 'cldevice_v5_canonical-name'. content is:"), DT_PERF_INFOSIZE);
    g_strlcat(info, "\n  ", DT_PERF_INFOSIZE);
    g_strlcat(info, _(" 'avoid_atomics' 'micro_nap' 'pinned_memory' 'roundupwd' 'roundupht' 'eventhandles' 'async' 'disable' 'magic' 'advantage' 'unified'"), DT_PERF_INFOSIZE);
    g_strlcat(info, "\n\n", DT_PERF_INFOSIZE);
  }

  else if(old < 16)
  {
    g_strlcat(info, INFO_HEADER, DT_PERF_INFOSIZE);
    g_strlcat(info, _("OpenCL 'per device' compiler settings might have been updated.\n\n"), DT_PERF_INFOSIZE);
  }
  else if(old < 17 && updated_mandatory)
  {
    g_strlcat(info, INFO_HEADER, DT_PERF_INFOSIZE);
    g_strlcat(info, _("OpenCL mandatory timeout has been updated to 1000.\n\n"), DT_PERF_INFOSIZE);
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


void dt_print_mem_usage(char *info)
{
  if(!(darktable.unmuted & DT_DEBUG_MEMORY))
    return;
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

  dt_print(DT_DEBUG_ALWAYS,
                  "[memory] %s\n"
                  "             max address space (vmpeak): %15s"
                  "             cur address space (vmsize): %15s"
                  "             max used memory   (vmhwm ): %15s"
                  "             cur used memory   (vmrss ): %15s",
          info, vmpeak, vmsize, vmhwm, vmrss);

#elif defined(__APPLE__)
  struct task_basic_info t_info;
  mach_msg_type_number_t t_info_count = TASK_BASIC_INFO_COUNT;

  if(KERN_SUCCESS != task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t)&t_info, &t_info_count))
  {
    dt_print(DT_DEBUG_ALWAYS, "[memory] task memory info unknown");
    return;
  }

  // Report in kB, to match output of /proc on Linux.
  dt_print(DT_DEBUG_ALWAYS,
                  "[memory] %s\n"
                  "            max address space (vmpeak): %15s\n"
                  "            cur address space (vmsize): %12llu kB\n"
                  "            max used memory   (vmhwm ): %15s\n"
                  "            cur used memory   (vmrss ): %12llu kB",
          info, "unknown", (uint64_t)t_info.virtual_size / 1024, "unknown", (uint64_t)t_info.resident_size / 1024);
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


  dt_print(DT_DEBUG_ALWAYS,
                  "[memory] %s\n"
                  "            max address space (vmpeak): %12llu kB\n"
                  "            cur address space (vmsize): %12llu kB\n"
                  "            max used memory   (vmhwm ): %12llu kB\n"
                  "            cur used memory   (vmrss ): %12llu Kb",
          info, virtualMemUsedByMeMax / 1024, virtualMemUsedByMe / 1024, physMemUsedByMeMax / 1024,
          physMemUsedByMe / 1024);

#else
  dt_print(DT_DEBUG_ALWAYS, "dt_print_mem_usage() currently unsupported on this platform");
#endif
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
