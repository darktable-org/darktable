/*
    This file is part of darktable,
    Copyright (C) 2009-2020 darktable developers.

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

#pragma once

// just to be sure. the build system should set this for us already:
#if defined __DragonFly__ || defined __FreeBSD__ || defined __NetBSD__ || defined __OpenBSD__
#define _WITH_DPRINTF
#define _WITH_GETLINE
#elif !defined _XOPEN_SOURCE && !defined _WIN32
#define _XOPEN_SOURCE 700 // for localtime_r and dprintf
#endif

// needs to be defined before any system header includes for control/conf.h to work in C++ code
#define __STDC_FORMAT_MACROS

#if defined _WIN32
#include "win/win.h"
#endif

#if !defined(O_BINARY)
// To have portable g_open() on *nix and on Windows
#define O_BINARY 0
#endif

#include "external/ThreadSafetyAnalysis.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "common/database.h"
#include "common/dtpthread.h"
#include "common/utility.h"
#include <time.h>
#ifdef _WIN32
#include "win/getrusage.h"
#else
#include <sys/resource.h>
#endif
#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <inttypes.h>
#include <json-glib/json-glib.h>
#include <lua/lua.h>
#include <math.h>
#include <sqlite3.h>
#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

#if defined(__DragonFly__) || defined(__FreeBSD__)
typedef unsigned int u_int;
#include <sys/sysctl.h>
#include <sys/types.h>
#endif
#if defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/param.h>
#include <sys/sysctl.h>
#endif

#ifdef _OPENMP
# include <omp.h>

/* See https://redmine.darktable.org/issues/12568#note-14 */
# ifdef HAVE_OMP_FIRSTPRIVATE_WITH_CONST
   /* If the compiler correctly supports firstprivate, use it. */
#  define dt_omp_firstprivate(...) firstprivate(__VA_ARGS__)
# else /* HAVE_OMP_FIRSTPRIVATE_WITH_CONST */
   /* This is needed for clang < 7.0 */
#  define dt_omp_firstprivate(...)
# endif/* HAVE_OMP_FIRSTPRIVATE_WITH_CONST */

#else /* _OPENMP */

# define omp_get_max_threads() 1
# define omp_get_thread_num() 0

#endif /* _OPENMP */

/* Create cloned functions for various CPU SSE generations */
/* See for instructions https://hannes.hauswedell.net/post/2017/12/09/fmv/ */
/* TL;DR : use only on SIMD functions containing low-level paralellized/vectorized loops */
#if __has_attribute(target_clones) && !defined(_WIN32) && defined(__SSE__)
#define __DT_CLONE_TARGETS__ __attribute__((target_clones("default", "sse2", "sse3", "sse4.1", "sse4.2", "popcnt", "avx", "avx2", "avx512f", "fma4")))
#else
#define __DT_CLONE_TARGETS__
#endif

/* Helper to force heap vectors to be aligned on 64 bits blocks to enable AVX2 */
#define DT_ALIGNED_ARRAY __attribute__((aligned(64)))
#define DT_ALIGNED_PIXEL __attribute__((aligned(16)))

/* Helper to force stack vectors to be aligned on 64 bits blocks to enable AVX2 */
#define DT_IS_ALIGNED(x) __builtin_assume_aligned(x, 64);

#ifndef _RELEASE
#include "common/poison.h"
#endif

#include "common/usermanual_url.h"

// for signal debugging symbols
#include "control/signal.h"

#define DT_MODULE_VERSION 22 // version of dt's module interface

// version of current performance configuration version
// if you want to run an updated version of the performance configuration later
// bump this number and make sure you have an updated logic in dt_configure_performance()
#define DT_CURRENT_PERFORMANCE_CONFIGURE_VERSION 1

// every module has to define this:
#ifdef _DEBUG
#define DT_MODULE(MODVER)                                                                                    \
  int dt_module_dt_version()                                                                                 \
  {                                                                                                          \
    return -DT_MODULE_VERSION;                                                                               \
  }                                                                                                          \
  int dt_module_mod_version()                                                                                \
  {                                                                                                          \
    return MODVER;                                                                                           \
  }
#else
#define DT_MODULE(MODVER)                                                                                    \
  int dt_module_dt_version()                                                                                 \
  {                                                                                                          \
    return DT_MODULE_VERSION;                                                                                \
  }                                                                                                          \
  int dt_module_mod_version()                                                                                \
  {                                                                                                          \
    return MODVER;                                                                                           \
  }
#endif

#define DT_MODULE_INTROSPECTION(MODVER, PARAMSTYPE) DT_MODULE(MODVER)

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
#define M_PI 3.14159265358979323846F
#endif

// Golden number (1+sqrt(5))/2
#ifndef PHI
#define PHI 1.61803398874989479F
#endif

// 1/PHI
#ifndef INVPHI
#define INVPHI 0.61803398874989479F
#endif

// NaN-safe clamping (NaN compares false, and will thus result in H)
#define CLAMPS(A, L, H) ((A) > (L) ? ((A) < (H) ? (A) : (H)) : (L))

#undef STR_HELPER
#define STR_HELPER(x) #x

#undef STR
#define STR(x) STR_HELPER(x)

#define DT_IMAGE_DBLOCKS 64

struct dt_gui_gtk_t;
struct dt_control_t;
struct dt_develop_t;
struct dt_mipmap_cache_t;
struct dt_image_cache_t;
struct dt_lib_t;
struct dt_conf_t;
struct dt_points_t;
struct dt_imageio_t;
struct dt_bauhaus_t;
struct dt_undo_t;
struct dt_colorspaces_t;
struct dt_l10n_t;

typedef enum dt_debug_thread_t
{
  // powers of two, masking
  DT_DEBUG_CACHE          = 1 <<  0,
  DT_DEBUG_CONTROL        = 1 <<  1,
  DT_DEBUG_DEV            = 1 <<  2,
  DT_DEBUG_PERF           = 1 <<  4,
  DT_DEBUG_CAMCTL         = 1 <<  5,
  DT_DEBUG_PWSTORAGE      = 1 <<  6,
  DT_DEBUG_OPENCL         = 1 <<  7,
  DT_DEBUG_SQL            = 1 <<  8,
  DT_DEBUG_MEMORY         = 1 <<  9,
  DT_DEBUG_LIGHTTABLE     = 1 << 10,
  DT_DEBUG_NAN            = 1 << 11,
  DT_DEBUG_MASKS          = 1 << 12,
  DT_DEBUG_LUA            = 1 << 13,
  DT_DEBUG_INPUT          = 1 << 14,
  DT_DEBUG_PRINT          = 1 << 15,
  DT_DEBUG_CAMERA_SUPPORT = 1 << 16,
  DT_DEBUG_IOPORDER       = 1 << 17,
  DT_DEBUG_IMAGEIO        = 1 << 18,
  DT_DEBUG_UNDO           = 1 << 19,
  DT_DEBUG_SIGNAL         = 1 << 20,
  DT_DEBUG_PARAMS         = 1 << 21,
} dt_debug_thread_t;

typedef struct dt_codepath_t
{
  unsigned int SSE2 : 1;
  unsigned int _no_intrinsics : 1;
  unsigned int OPENMP_SIMD : 1; // always stays the last one
} dt_codepath_t;

typedef struct darktable_t
{
  dt_codepath_t codepath;
  int32_t num_openmp_threads;

  int32_t unmuted;
  GList *iop;
  GList *iop_order_list;
  GList *iop_order_rules;
  GList *capabilities;
  JsonParser *noiseprofile_parser;
  struct dt_conf_t *conf;
  struct dt_develop_t *develop;
  struct dt_lib_t *lib;
  struct dt_view_manager_t *view_manager;
  struct dt_control_t *control;
  struct dt_control_signal_t *signals;
  struct dt_gui_gtk_t *gui;
  struct dt_mipmap_cache_t *mipmap_cache;
  struct dt_image_cache_t *image_cache;
  struct dt_bauhaus_t *bauhaus;
  const struct dt_database_t *db;
  const struct dt_pwstorage_t *pwstorage;
  const struct dt_camctl_t *camctl;
  const struct dt_collection_t *collection;
  struct dt_selection_t *selection;
  struct dt_points_t *points;
  struct dt_imageio_t *imageio;
  struct dt_opencl_t *opencl;
  struct dt_dbus_t *dbus;
  struct dt_undo_t *undo;
  struct dt_colorspaces_t *color_profiles;
  struct dt_l10n_t *l10n;
  dt_pthread_mutex_t db_image[DT_IMAGE_DBLOCKS];
  dt_pthread_mutex_t dev_threadsafe;
  dt_pthread_mutex_t plugin_threadsafe;
  dt_pthread_mutex_t capabilities_threadsafe;
  dt_pthread_mutex_t exiv2_threadsafe;
  dt_pthread_mutex_t readFile_mutex;
  char *progname;
  char *datadir;
  char *sharedir;
  char *plugindir;
  char *localedir;
  char *tmpdir;
  char *configdir;
  char *cachedir;
  dt_lua_state_t lua_state;
  GList *guides;
  double start_wtime;
  GList *themes;
  int32_t unmuted_signal_dbg_acts;
  gboolean unmuted_signal_dbg[DT_SIGNAL_COUNT];
} darktable_t;

typedef struct
{
  double clock;
  double user;
} dt_times_t;

extern darktable_t darktable;

int dt_init(int argc, char *argv[], const gboolean init_gui, const gboolean load_data, lua_State *L);
void dt_cleanup();
void dt_print(dt_debug_thread_t thread, const char *msg, ...) __attribute__((format(printf, 2, 3)));
void dt_gettime_t(char *datetime, size_t datetime_len, time_t t);
void dt_gettime(char *datetime, size_t datetime_len);
void *dt_alloc_align(size_t alignment, size_t size);
size_t dt_round_size(const size_t size, const size_t alignment);
size_t dt_round_size_sse(const size_t size);

#ifdef _WIN32
void dt_free_align(void *mem);
#define dt_free_align_ptr dt_free_align
#else
#define dt_free_align(A) free(A)
#define dt_free_align_ptr free
#endif

static inline void dt_lock_image(int32_t imgid) ACQUIRE(darktable.db_image[imgid & (DT_IMAGE_DBLOCKS-1)])
{
  dt_pthread_mutex_lock(&(darktable.db_image[imgid & (DT_IMAGE_DBLOCKS-1)]));
}

static inline void dt_unlock_image(int32_t imgid) RELEASE(darktable.db_image[imgid & (DT_IMAGE_DBLOCKS-1)])
{
  dt_pthread_mutex_unlock(&(darktable.db_image[imgid & (DT_IMAGE_DBLOCKS-1)]));
}

static inline void dt_lock_image_pair(int32_t imgid1, int32_t imgid2) ACQUIRE(darktable.db_image[imgid1 & (DT_IMAGE_DBLOCKS-1)], darktable.db_image[imgid2 & (DT_IMAGE_DBLOCKS-1)])
{
  if(imgid1 < imgid2)
  {
    dt_pthread_mutex_lock(&(darktable.db_image[imgid1 & (DT_IMAGE_DBLOCKS-1)]));
    dt_pthread_mutex_lock(&(darktable.db_image[imgid2 & (DT_IMAGE_DBLOCKS-1)]));
  }
  else
  {
    dt_pthread_mutex_lock(&(darktable.db_image[imgid2 & (DT_IMAGE_DBLOCKS-1)]));
    dt_pthread_mutex_lock(&(darktable.db_image[imgid1 & (DT_IMAGE_DBLOCKS-1)]));
  }
}

static inline void dt_unlock_image_pair(int32_t imgid1, int32_t imgid2) RELEASE(darktable.db_image[imgid1 & (DT_IMAGE_DBLOCKS-1)], darktable.db_image[imgid2 & (DT_IMAGE_DBLOCKS-1)])
{
  dt_pthread_mutex_unlock(&(darktable.db_image[imgid1 & (DT_IMAGE_DBLOCKS-1)]));
  dt_pthread_mutex_unlock(&(darktable.db_image[imgid2 & (DT_IMAGE_DBLOCKS-1)]));
}

static inline gboolean dt_is_aligned(const void *pointer, size_t byte_count)
{
    return (uintptr_t)pointer % byte_count == 0;
}

static inline void * dt_alloc_sse_ps(size_t pixels)
{
  return __builtin_assume_aligned(dt_alloc_align(64, pixels * sizeof(float)), 64);
}

static inline void * dt_check_sse_aligned(void * pointer)
{
  if(dt_is_aligned(pointer, 64))
    return __builtin_assume_aligned(pointer, 64);
  else
    return NULL;
}

int dt_capabilities_check(char *capability);
void dt_capabilities_add(char *capability);
void dt_capabilities_remove(char *capability);
void dt_capabilities_cleanup();

static inline double dt_get_wtime(void)
{
  struct timeval time;
  gettimeofday(&time, NULL);
  return time.tv_sec - 1290608000 + (1.0 / 1000000.0) * time.tv_usec;
}

static inline void dt_get_times(dt_times_t *t)
{
  struct rusage ru;

  getrusage(RUSAGE_SELF, &ru);
  t->clock = dt_get_wtime();
  t->user = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * (1.0 / 1000000.0);
}

void dt_show_times(const dt_times_t *start, const char *prefix);

void dt_show_times_f(const dt_times_t *start, const char *prefix, const char *suffix, ...) __attribute__((format(printf, 3, 4)));

/** \brief check if file is a supported image */
gboolean dt_supported_image(const gchar *filename);

static inline int dt_get_num_threads()
{
#ifdef _OPENMP
  return omp_get_num_procs();
#else
  return 1;
#endif
}

static inline int dt_get_thread_num()
{
#ifdef _OPENMP
  return omp_get_thread_num();
#else
  return 0;
#endif
}

static inline float dt_log2f(const float f)
{
#ifdef __GLIBC__
  return log2f(f);
#else
  return logf(f) / logf(2.0f);
#endif
}

static inline float dt_fast_expf(const float x)
{
  // meant for the range [-100.0f, 0.0f]. largest error ~ -0.06 at 0.0f.
  // will get _a_lot_ worse for x > 0.0f (9000 at 10.0f)..
  const int i1 = 0x3f800000u;
  // e^x, the comment would be 2^x
  const int i2 = 0x402DF854u; // 0x40000000u;
  // const int k = CLAMPS(i1 + x * (i2 - i1), 0x0u, 0x7fffffffu);
  // without max clamping (doesn't work for large x, but is faster):
  const int k0 = i1 + x * (i2 - i1);
  union {
      float f;
      int k;
  } u;
  u.k = k0 > 0 ? k0 : 0;
  return u.f;
}

// fast approximation of 2^-x for 0<x<126
static inline float dt_fast_mexp2f(const float x)
{
  const int i1 = 0x3f800000; // bit representation of 2^0
  const int i2 = 0x3f000000; // bit representation of 2^-1
  const int k0 = i1 + (int)(x * (i2 - i1));
  union {
    float f;
    int i;
  } k;
  k.i = k0 >= 0x800000 ? k0 : 0;
  return k.f;
}

// The below version is incorrect, suffering from reduced precision.
// It is used by the non-local means code in both nlmeans.c and
// denoiseprofile.c, and fixing it would cause a change in output.
static inline float fast_mexp2f(const float x)
{
  const float i1 = (float)0x3f800000u; // 2^0
  const float i2 = (float)0x3f000000u; // 2^-1
  const float k0 = i1 + x * (i2 - i1);
  union {
    float f;
    int i;
  } k;
  k.i = k0 >= (float)0x800000u ? k0 : 0;
  return k.f;
}

static inline void dt_print_mem_usage()
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

static inline int dt_get_num_atom_cores()
{
#if defined(__linux__)
  int count = 0;
  char line[256];
  FILE *f = g_fopen("/proc/cpuinfo", "r");
  if(f)
  {
    while(!feof(f))
    {
      if(fgets(line, sizeof(line), f))
      {
        if(!strncmp(line, "model name", 10))
        {
          if(strstr(line, "Atom"))
          {
            count++;
          }
        }
      }
    }
    fclose(f);
  }
  return count;
#elif defined(__DragonFly__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
  int ret, hw_ncpu;
  int mib[2] = { CTL_HW, HW_MODEL };
  char *hw_model, *index;
  size_t length;

  /* Query hw.model to get the required buffer length and allocate the
   * buffer. */
  ret = sysctl(mib, 2, NULL, &length, NULL, 0);
  if(ret != 0)
  {
    return 0;
  }

  hw_model = (char *)malloc(length + 1);
  if(hw_model == NULL)
  {
    return 0;
  }

  /* Query hw.model again, this time with the allocated buffer. */
  ret = sysctl(mib, 2, hw_model, &length, NULL, 0);
  if(ret != 0)
  {
    free(hw_model);
    return 0;
  }
  hw_model[length] = '\0';

  /* Check if the processor model name contains "Atom". */
  index = strstr(hw_model, "Atom");
  free(hw_model);
  if(index == NULL)
  {
    return 0;
  }

  /* Get the number of cores, using hw.ncpu sysctl. */
  mib[1] = HW_NCPU;
  hw_ncpu = 0;
  length = sizeof(hw_ncpu);
  ret = sysctl(mib, 2, &hw_ncpu, &length, NULL, 0);
  if(ret != 0)
  {
    return 0;
  }

  return hw_ncpu;
#else
  return 0;
#endif
}

static inline size_t dt_get_total_memory()
{
#if defined(__linux__)
  FILE *f = g_fopen("/proc/meminfo", "rb");
  if(!f) return 0;
  size_t mem = 0;
  char *line = NULL;
  size_t len = 0;
  if(getline(&line, &len, f) != -1) mem = atol(line + 10);
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

void dt_configure_performance();

// helper function which loads whatever image_to_load points to: single image files or whole directories
// it tells you if it was a single image or a directory in single_image (when it's not NULL)
int dt_load_from_string(const gchar *image_to_load, gboolean open_image_in_dr, gboolean *single_image);

#define dt_unreachable_codepath_with_desc(D)                                                                 \
  dt_unreachable_codepath_with_caller(D, __FILE__, __LINE__, __FUNCTION__)
#define dt_unreachable_codepath() dt_unreachable_codepath_with_caller("unreachable", __FILE__, __LINE__, __FUNCTION__)
static inline void dt_unreachable_codepath_with_caller(const char *description, const char *file,
                                                       const int line, const char *function)
{
  fprintf(stderr, "[dt_unreachable_codepath] {%s} %s:%d (%s) - we should not be here. please report this to "
                  "the developers.",
          description, file, line, function);
  __builtin_unreachable();
}

/** define for max path/filename length */
#define DT_MAX_FILENAME_LEN 256

#ifndef PATH_MAX
/*
 * from /usr/include/linux/limits.h (Linux 3.16.5)
 * Some systems might not define it (e.g. Hurd)
 *
 * We do NOT depend on any specific value of this env variable.
 * If you want constant value across all systems, use DT_MAX_PATH_FOR_PARAMS!
 */
#define PATH_MAX 4096
#endif

/*
 * ONLY TO BE USED FOR PARAMS!!! (e.g. dt_imageio_disk_t)
 *
 * WARNING: this should *NEVER* be changed, as it will break params,
 *          created with previous DT_MAX_PATH_FOR_PARAMS.
 */
#define DT_MAX_PATH_FOR_PARAMS 4096

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
