/*
    This file is part of darktable,
    Copyright (C) 2009-2024 darktable developers.

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
#include "common/dttypes.h"
#include "common/utility.h"
#include "common/wb_presets.h"
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

#ifndef dt_omp_sharedconst
#ifdef _OPENMP
#if defined(__clang__) || __GNUC__ > 8
# define dt_omp_sharedconst(...) shared(__VA_ARGS__)
#else
  // GCC 8.4 throws string of errors "'x' is predetermined 'shared' for 'shared'" if we explicitly declare
  //  'const' variables as shared
# define dt_omp_sharedconst(var, ...)
#endif
#endif /* _OPENMP */
#endif /* dt_omp_sharedconst */

#ifndef dt_omp_nontemporal
// Clang 10+ supports the nontemporal() OpenMP directive
// GCC 9 recognizes it as valid, but does not do anything with it
// GCC 10+ ???
#if (__clang__+0 >= 10 || __GNUC__ >= 9)
#  define dt_omp_nontemporal(...) nontemporal(__VA_ARGS__)
#else
// GCC7/8 only support OpenMP 4.5, which does not have the nontemporal() directive.
#  define dt_omp_nontemporal(var, ...)
#endif
#endif /* dt_omp_nontemporal */

#define DT_OMP_STRINGIFY(...) #__VA_ARGS__
#define DT_OMP_PRAGMA(...) _Pragma(DT_OMP_STRINGIFY(omp __VA_ARGS__))

#else /* _OPENMP */

# define omp_get_max_threads() 1
# define omp_get_thread_num() 0

#define DT_OMP_PRAGMA(...)

#endif /* _OPENMP */

#define DT_OMP_SIMD(clauses) DT_OMP_PRAGMA(simd clauses)
#define DT_OMP_DECLARE_SIMD(clauses) DT_OMP_PRAGMA(declare simd clauses)
#define DT_OMP_FOR(clauses) DT_OMP_PRAGMA(parallel for default(firstprivate) schedule(static) clauses)
#define DT_OMP_FOR_SIMD(clauses) DT_OMP_PRAGMA(parallel for simd default(firstprivate) schedule(simd:static) clauses)

#ifndef _RELEASE
#include "common/poison.h"
#endif

#include "common/usermanual_url.h"

// for signal debugging symbols
#include "control/signal.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* Create cloned functions for various CPU SSE generations */
/* See for instructions https://hannes.hauswedell.net/post/2017/12/09/fmv/ */
/* TL;DR : use only on SIMD functions containing low-level paralellized/vectorized loops */
#if __has_attribute(target_clones) && !defined(_WIN32) && !defined(NATIVE_ARCH) && !defined(__APPLE__) && defined(__GLIBC__)
# if defined(__amd64__) || defined(__amd64) || defined(__x86_64__) || defined(__x86_64)
#define __DT_CLONE_TARGETS__ __attribute__((target_clones("default", "sse2", "sse3", "sse4.1", "sse4.2", "popcnt", "avx", "avx2", "avx512f", "fma4")))
# elif defined(__PPC64__)
/* __PPC64__ is the only macro tested for in is_supported_platform.h, other macros would fail there anyway. */
#define __DT_CLONE_TARGETS__ __attribute__((target_clones("default","cpu=power9")))
# else
#define __DT_CLONE_TARGETS__
# endif
#else
#define __DT_CLONE_TARGETS__
#endif

typedef int32_t dt_imgid_t;
typedef int32_t dt_filmid_t;
#define NO_IMGID (0)
#define NO_FILMID (0)
#define dt_is_valid_imgid(n) ((n) > NO_IMGID)
#define dt_is_valid_filmid(n) ((n) > NO_FILMID)
/*
  A dt_mask_id_t can be
  0  -> a formless mask
  >0 -> having a form
  INVALID_MASKID is used while testing in mask manager
*/

#define DT_DEVICE_CPU -1
#define DT_DEVICE_NONE -2

typedef int32_t dt_mask_id_t;
#define INVALID_MASKID (-1)
#define NO_MASKID (0)
#define BLEND_RASTER_ID (0)
// testing for a valid form
#define dt_is_valid_maskid(n) ((n) > NO_MASKID)

/* Helper to force stack vectors to be aligned on DT_CACHELINE_BYTES blocks to enable AVX2 */
#define DT_IS_ALIGNED(x) __builtin_assume_aligned(x, DT_CACHELINE_BYTES)

/* Helper for 4-float pixel vectors */
#define DT_IS_ALIGNED_PIXEL(x) __builtin_assume_aligned(x, 16)

#define DT_MODULE_VERSION 25 // version of dt's module interface

// version of current performance configuration version
// if you want to run an updated version of the performance configuration later
// bump this number and make sure you have an updated logic in dt_configure_runtime_performance()
#define DT_CURRENT_PERFORMANCE_CONFIGURE_VERSION 17
#define DT_PERF_INFOSIZE 4096

// every module has to define this:
#ifdef _DEBUG
#define DT_MODULE(MODVER)                  \
  int dt_module_dt_version()               \
  {                                        \
    return -DT_MODULE_VERSION;             \
  }                                        \
  int dt_module_mod_version()              \
  {                                        \
    return MODVER;                         \
  }
#else
#define DT_MODULE(MODVER)                  \
  int dt_module_dt_version()               \
  {                                        \
    return DT_MODULE_VERSION;              \
  }                                        \
  int dt_module_mod_version()              \
  {                                        \
    return MODVER;                         \
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

// returns true if the running darktable corresponds to a dev version
gboolean dt_is_dev_version();
// returns the darktable version as <major>.<minor>
char *dt_version_major_minor();

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

// If platform supports hardware-accelerated fused-multiply-add
// This is not only faster but more accurate because rounding happens at the right place
#ifdef FP_FAST_FMAF
  #define DT_FMA(x, y, z) fmaf(x, y, z)
#else
  #define DT_FMA(x, y, z) ((x) * (y) + (z))
#endif

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

typedef float dt_boundingbox_t[4];  //(x,y) of upperleft, then (x,y) of lowerright
typedef float dt_pickerbox_t[8];

typedef enum dt_debug_thread_t
{
  // powers of two, masking
  DT_DEBUG_ALWAYS         = 0,       // special case tested by dt_print() variants
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
  DT_DEBUG_ACT_ON         = 1 << 22,
  DT_DEBUG_TILING         = 1 << 23,
  DT_DEBUG_VERBOSE        = 1 << 24,
  DT_DEBUG_PIPE           = 1 << 25,
  DT_DEBUG_EXPOSE         = 1 << 26,
  DT_DEBUG_PICKER         = 1 << 27,
  DT_DEBUG_ALL            = 0xffffffff & ~DT_DEBUG_VERBOSE,
  DT_DEBUG_COMMON         = DT_DEBUG_OPENCL | DT_DEBUG_DEV | DT_DEBUG_MASKS | DT_DEBUG_PARAMS | DT_DEBUG_IMAGEIO | DT_DEBUG_PIPE,
  DT_DEBUG_RESTRICT       = DT_DEBUG_VERBOSE | DT_DEBUG_PERF,
} dt_debug_thread_t;

typedef struct dt_codepath_t
{
  unsigned int _no_intrinsics : 1;
} dt_codepath_t;

typedef struct dt_sys_resources_t
{
  size_t total_memory;
  size_t mipmap_memory;
  int *fractions;   // fractions are calculated as res=input / 1024  * fraction
  int *refresource; // for the debug resource modes we use fixed settings
  int group;
  int level;
  gboolean tunehead;
} dt_sys_resources_t;

typedef struct dt_backthumb_t
{
  double time;
  double idle;
  gboolean service;
  gboolean running;
  gboolean capable;
  int32_t mipsize;
} dt_backthumb_t;

typedef struct dt_gimp_t
{
  int32_t size;
  char *mode;
  char *path;
  dt_imgid_t imgid;
  gboolean error;
} dt_gimp_t;

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
  char *dump_pfm_module;
  char *dump_pfm_pipe;
  char *dump_diff_pipe;
  char *tmp_directory;
  char *bench_module;
  dt_lua_state_t lua_state;
  GList *guides;
  double start_wtime;
  GList *themes;
  int32_t unmuted_signal_dbg_acts;
  gboolean unmuted_signal_dbg[DT_SIGNAL_COUNT];
  gboolean pipe_cache;
  int gui_running;		// atomic, access with g_atomic_int_*()
  GTimeZone *utc_tz;
  GDateTime *origin_gdt;
  struct dt_sys_resources_t dtresources;
  struct dt_backthumb_t backthumbs;
  struct dt_gimp_t gimp;
} darktable_t;

typedef struct
{
  double clock;
  double user;
} dt_times_t;

extern darktable_t darktable;

int dt_init(int argc, char *argv[],
            const gboolean init_gui,
            const gboolean load_data,
            lua_State *L);

void dt_get_sysresource_level();
void dt_cleanup();

/*
  for performance reasons the debug log functions should only be called,
  and their arguments evaluated, if flags in thread match what is requested.
*/
#define dt_debug_if(thread, func, ...)                            \
  do{ if( ( (~DT_DEBUG_RESTRICT & (thread)) == DT_DEBUG_ALWAYS    \
          || ~DT_DEBUG_RESTRICT & (thread) &  darktable.unmuted ) \
         && !(DT_DEBUG_RESTRICT & (thread) & ~darktable.unmuted)) \
        func(__VA_ARGS__); } while(0)

#define dt_print(thread, ...) dt_debug_if(thread, dt_print_ext, __VA_ARGS__)
#define dt_print_nts(thread, ...) dt_debug_if(thread, dt_print_nts_ext, __VA_ARGS__)
#define dt_print_pipe(thread, title, pipe, module, device, roi_in, roi_out, ...) \
  dt_debug_if(thread, dt_print_pipe_ext, title, pipe, module, device, roi_in, roi_out, " " __VA_ARGS__)

void dt_print_ext(const char *msg, ...)
  __attribute__((format(printf, 1, 2)));

/* same as above but without time stamp : nts = no time stamp */
void dt_print_nts_ext(const char *msg, ...)
  __attribute__((format(printf, 1, 2)));

int dt_worker_threads();
size_t dt_get_available_mem();
size_t dt_get_singlebuffer_mem();

void dt_dump_pfm_file(const char *pipe,
                      const void *data,
                      const int width,
                      const int height,
                      const int bpp,
                      const char *modname,
                      const char *head,
                      const gboolean input,
                      const gboolean output,
                      const gboolean cpu);

void dt_dump_pfm(const char *filename,
                 const void* data,
                 const int width,
                 const int height,
                 const int bpp,
                 const char *modname);

void dt_dump_pipe_pfm(const char *mod,
                      const void* data,
                      const int width,
                      const int height,
                      const int bpp,
                      const gboolean input,
                      const char *pipe);

void dt_dump_pipe_diff_pfm(const char *mod,
                          const float *a,
                          const float *b,
                          const int width,
                          const int height,
                          const int ch,
                          const char *pipe);

void *dt_alloc_aligned(const size_t size);

static inline void* dt_calloc_aligned(const size_t size)
{
  void *buf = dt_alloc_aligned(size);
  if(buf) memset(buf, 0, size);
  return buf;
}
#define dt_calloc1_align_type(TYPE) \
  ((TYPE*)__builtin_assume_aligned(dt_calloc_aligned(sizeof(TYPE)), DT_CACHELINE_BYTES))
#define dt_calloc_align_type(TYPE, count) \
  ((TYPE*)__builtin_assume_aligned(dt_calloc_aligned(count * sizeof(TYPE)), DT_CACHELINE_BYTES))
static inline int* dt_calloc_align_int(const size_t n_ints)
{
  return (int*)__builtin_assume_aligned(dt_calloc_aligned(n_ints * sizeof(int)), DT_CACHELINE_BYTES);
}

#define dt_alloc1_align_type(TYPE) \
  ((TYPE*)__builtin_assume_aligned(dt_alloc_aligned(sizeof(TYPE)), DT_CACHELINE_BYTES))

#define dt_alloc_align_type(TYPE, count) \
  ((TYPE*)__builtin_assume_aligned(dt_alloc_aligned(count * sizeof(TYPE)), DT_CACHELINE_BYTES))

static inline int *dt_alloc_align_int(const size_t n_ints)
{
  return (int*)__builtin_assume_aligned(dt_alloc_aligned(n_ints * sizeof(int)), DT_CACHELINE_BYTES);
}
static inline uint8_t *dt_alloc_align_uint8(const size_t n_ints)
{
  return (uint8_t*)__builtin_assume_aligned(dt_alloc_aligned(n_ints * sizeof(uint8_t)), DT_CACHELINE_BYTES);
}
static inline float *dt_alloc_align_float(const size_t nfloats)
{
  return (float*)__builtin_assume_aligned(dt_alloc_aligned(nfloats * sizeof(float)),
                                          DT_CACHELINE_BYTES);
}
static inline double *dt_alloc_align_double(const size_t ndoubles)
{
  return (double*)__builtin_assume_aligned(dt_alloc_aligned(ndoubles * sizeof(double)),
                                           DT_CACHELINE_BYTES);
}
static inline float *dt_calloc_align_float(const size_t nfloats)
{
  float *const buf = (float*)dt_alloc_align_float(nfloats);
  if(buf) memset(buf, 0, nfloats * sizeof(float));
  return (float*)__builtin_assume_aligned(buf, DT_CACHELINE_BYTES);
}

static inline gboolean dt_check_aligned(void *addr)
{
  return ((uintptr_t)addr & (DT_CACHELINE_BYTES - 1)) == 0;
}

size_t dt_round_size(const size_t size, const size_t alignment);

#ifdef _WIN32
void dt_free_align(void *mem);
#define dt_free_align_ptr dt_free_align
#elif _DEBUG // debug build makes sure that we get a crash on using
             // plain free() on an aligned allocation
void dt_free_align(void *mem);
#define dt_free_align_ptr dt_free_align
#else
#define dt_free_align(A) free(A)
#define dt_free_align_ptr free
#endif

static inline void dt_lock_image(const dt_imgid_t imgid)
  ACQUIRE(darktable.db_image[imgid & (DT_IMAGE_DBLOCKS-1)])
{
  dt_pthread_mutex_lock(&(darktable.db_image[imgid & (DT_IMAGE_DBLOCKS-1)]));
}

static inline void dt_unlock_image(const dt_imgid_t imgid)
  RELEASE(darktable.db_image[imgid & (DT_IMAGE_DBLOCKS-1)])
{
  dt_pthread_mutex_unlock(&(darktable.db_image[imgid & (DT_IMAGE_DBLOCKS-1)]));
}

static inline void dt_lock_image_pair(const dt_imgid_t imgid1,
                                      const dt_imgid_t imgid2)
  ACQUIRE(darktable.db_image[imgid1 & (DT_IMAGE_DBLOCKS-1)],
          darktable.db_image[imgid2 & (DT_IMAGE_DBLOCKS-1)])
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

static inline void dt_unlock_image_pair(const dt_imgid_t imgid1,
                                        const dt_imgid_t imgid2)
  RELEASE(darktable.db_image[imgid1 & (DT_IMAGE_DBLOCKS-1)],
          darktable.db_image[imgid2 & (DT_IMAGE_DBLOCKS-1)])
{
  dt_pthread_mutex_unlock(&(darktable.db_image[imgid1 & (DT_IMAGE_DBLOCKS-1)]));
  dt_pthread_mutex_unlock(&(darktable.db_image[imgid2 & (DT_IMAGE_DBLOCKS-1)]));
}

extern GdkModifierType dt_modifier_shortcuts;

// check whether the specified mask of modifier keys exactly matches,
// among the set Shift+Control+(Alt/Meta).  ignores the state of any
// other shifting keys
static inline gboolean dt_modifier_is(const GdkModifierType state,
                                      const GdkModifierType desired_modifier_mask)
{
  const GdkModifierType modifiers = gtk_accelerator_get_default_mod_mask();
//TODO: on Macs, remap the GDK_CONTROL_MASK bit in
//desired_modifier_mask to be the bit for the Cmd key
  return ((state | dt_modifier_shortcuts) & modifiers) == desired_modifier_mask;
}

// check whether the given modifier state includes AT LEAST the
// specified mask of modifier keys
static inline gboolean dt_modifiers_include(const GdkModifierType state,
                                            const GdkModifierType desired_modifier_mask)
{
//TODO: on Macs, remap the GDK_CONTROL_MASK bit in
//desired_modifier_mask to be the bit for the Cmd key
  const GdkModifierType modifiers = gtk_accelerator_get_default_mod_mask();
  // check whether all modifier bits of interest are turned on
  return ((state | dt_modifier_shortcuts)
          & (modifiers & desired_modifier_mask)) == desired_modifier_mask;
}


static inline gboolean dt_is_aligned(const void *pointer, const size_t byte_count)
{
    return (uintptr_t)pointer % byte_count == 0;
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

static inline double dt_get_debug_wtime(void)
{
  return darktable.unmuted ? dt_get_wtime() : 0.0;
}

static inline double dt_get_lap_time(double *time)
{
  double prev = *time;
  *time = dt_get_wtime();
  return *time - prev;
}

static inline double dt_get_utime(void)
{
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * (1.0 / 1000000.0);
}

static inline double dt_get_lap_utime(double *time)
{
  double prev = *time;
  *time = dt_get_utime();
  return *time - prev;
}

static inline void dt_get_times(dt_times_t *t)
{
  t->clock = dt_get_wtime();
  t->user = dt_get_utime();
}

static inline void dt_get_perf_times(dt_times_t *t)
{
  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_get_times(t);
  }
}

void dt_show_times(const dt_times_t *start, const char *prefix);

void dt_show_times_f(const dt_times_t *start,
                     const char *prefix,
                     const char *suffix, ...)
  __attribute__((format(printf, 3, 4)));

/** \brief check if file is a supported image */
gboolean dt_supported_image(const gchar *filename);

static inline size_t dt_get_num_threads()
{
#ifdef _OPENMP
  return (size_t)CLAMP(omp_get_num_procs(), 1, darktable.num_openmp_threads);
#else
  return 1;
#endif
}

static inline size_t dt_get_num_procs()
{
#ifdef _OPENMP
  // we can safely assume omp_get_num_procs is > 0
  return (size_t)omp_get_num_procs();
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

#define DT_INITHASH 5381
typedef uint64_t dt_hash_t;
static inline dt_hash_t dt_hash(dt_hash_t hash, const void *data, const size_t size)
{
  const uint8_t* str = (uint8_t*)data;
  // Scramble bits in str to create an (hopefully) unique hash representing the state of str
  // Dan Bernstein algo v2 http://www.cse.yorku.ca/~oz/hash.html
  // hash should be inited to DT_INITHASH if first run, or from a previous hash computed with this function.
  for(size_t i = 0; i < size; i++)
    hash = ((hash << 5) + hash) ^ str[i];

  return hash;
}

// Allocate a buffer for 'n' objects each of size 'objsize' bytes for
// each of the program's threads.  Ensures that there is no false
// sharing among threads by aligning and rounding up the allocation to
// a multiple of the cache line size.  Returns a pointer to the
// allocated pool and the adjusted number of objects in each thread's
// buffer.  Use dt_get_perthread or dt_get_bythread (see below) to
// access a specific thread's buffer.
static inline void *dt_alloc_perthread(const size_t n,
                                       const size_t objsize,
                                       size_t* padded_size)
{
  const size_t alloc_size = n * objsize;
  const size_t cache_lines = (alloc_size+DT_CACHELINE_BYTES-1)/DT_CACHELINE_BYTES;
  *padded_size = DT_CACHELINE_BYTES * cache_lines / objsize;
  const size_t total_bytes = DT_CACHELINE_BYTES * cache_lines * dt_get_num_threads();
  return __builtin_assume_aligned(dt_alloc_aligned(total_bytes), DT_CACHELINE_BYTES);
}
static inline void *dt_calloc_perthread(const size_t n,
                                        const size_t objsize,
                                        size_t* padded_size)
{
  void *const buf = (float*)dt_alloc_perthread(n, objsize, padded_size);
  memset(buf, 0, *padded_size * dt_get_num_threads() * objsize);
  return buf;
}
// Same as dt_alloc_perthread, but the object is a float.
static inline float *dt_alloc_perthread_float(const size_t n,
                                              size_t* padded_size)
{
  return (float*)dt_alloc_perthread(n, sizeof(float), padded_size);
}
// Allocate floats, cleared to zero
static inline float *dt_calloc_perthread_float(const size_t n,
                                               size_t* padded_size)
{
  float *const buf = (float*)dt_alloc_perthread(n, sizeof(float), padded_size);
  if (buf)
  {
    for (size_t i = 0; i < *padded_size * dt_get_num_threads(); i++)
      buf[i] = 0.0f;
  }
  return buf;
}

// Given the buffer and object count returned by dt_alloc_perthread,
// return the current thread's private buffer.
#define dt_get_perthread(buf, padsize) \
  DT_IS_ALIGNED((buf) + ((padsize) * dt_get_thread_num()))
// Given the buffer and object count returned by dt_alloc_perthread and a thread count in 0..dt_get_num_threads(),
// return a pointer to the indicated thread's private buffer.
#define dt_get_bythread(buf, padsize, tnum) \
  DT_IS_ALIGNED((buf) + ((padsize) * (tnum)))

// Most code in dt assumes that the compiler is capable of
// auto-vectorization.  In some cases, this will yield suboptimal code
// if the compiler in fact does NOT auto-vectorize.  Uncomment the
// following line for such a compiler.

//#define DT_NO_VECTORIZATION

// For some combinations of compiler and architecture, the compiler
// may actually emit inferior code if given a hint to vectorize a
// loop.  Uncomment the following line if such a combination is the
// compilation target.

//#define DT_NO_SIMD_HINTS

// copy the RGB channels of a pixel; includes the 'alpha' channel as
// well if faster due to vectorization, but subsequent code should
// ignore the value of the alpha unless explicitly set afterwards
// (since it might not have been copied)

// When writing sequentially to an output buffer, consider using
// copy_pixel_nontemporal (defined in develop/imageop.h) to avoid the
// overhead of loading the cache lines from RAM before then completely
// overwriting them
static inline void copy_pixel(float *const __restrict__ out,
                              const float *const __restrict__ in)
{
  for_each_channel(k,aligned(in,out:16)) out[k] = in[k];
}

// a few macros and helper functions to speed up certain frequently-used GLib operations
#define g_list_is_singleton(list) ((list) && (!(list)->next))
#define g_list_is_empty(list) (!list)

static inline gboolean g_list_shorter_than(const GList *list,
                                           unsigned len)
{
  // instead of scanning the full list to compute its length and then
  // comparing against the limit, bail out as soon as the limit is
  // reached.  Usage: g_list_shorter_than(l,4) instead of
  // g_list_length(l)<4
  while (len-- > 0)
  {
    if (!list) return TRUE;
    list = g_list_next(list);
  }
  return FALSE;
}

// advance the list by one position, unless already at the final node
static inline GList *g_list_next_bounded(GList *list)
{
  return g_list_next(list) ? g_list_next(list) : list;
}

static inline const GList *g_list_next_wraparound(const GList *list,
                                                  const GList *head)
{
  return g_list_next(list) ? g_list_next(list) : head;
}

static inline const GList *g_list_prev_wraparound(const GList *list)
{
  // return the prior element of the list, unless already on the first
  // element; in that case, return the last element of the list.
  return g_list_previous(list) ? g_list_previous(list) : g_list_last((GList*)list);
}

// returns true if the two GLists have the same length
static inline gboolean dt_list_length_equal(GList *l1, GList *l2)
{
  while (l1 && l2)
  {
    l1 = g_list_next(l1);
    l2 = g_list_next(l2);
  }
  return !l1 && !l2;
}

// returns true if the two GSLists have the same length
static inline gboolean dt_slist_length_equal(GSList *l1, GSList *l2)
{
  while (l1 && l2)
  {
    l1 = g_slist_next(l1);
    l2 = g_slist_next(l2);
  }
  return !l1 && !l2;
}

// checks internally for DT_DEBUG_MEMORY
void dt_print_mem_usage(char *info);

// try to start the backthumbs crawler
void dt_start_backtumbs_crawler();

void dt_configure_runtime_performance(const int version, char *config_info);
// helper function which loads whatever image_to_load points to:
// single image files or whole directories it tells you if it was a
// single image or a directory in single_image (when it's not NULL)
dt_imgid_t dt_load_from_string(const gchar *image_to_load,
                               const gboolean open_image_in_dr,
                               gboolean *single_image);

#define dt_unreachable_codepath_with_desc(D)                                                                 \
  dt_unreachable_codepath_with_caller(D, __FILE__, __LINE__, __FUNCTION__)
#define dt_unreachable_codepath() \
  dt_unreachable_codepath_with_caller("unreachable", __FILE__, __LINE__, __FUNCTION__)
static inline void dt_unreachable_codepath_with_caller(const char *description,
                                                       const char *file,
                                                       const int line,
                                                       const char *function)
{
  dt_print(DT_DEBUG_ALWAYS,
           "[dt_unreachable_codepath] {%s} %s:%d (%s) - we should not be here."
           " please report this to the developers",
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

/*
 * Helper functions for transition to gnome style xgettext translation context marking
 *
 * Many calls expect untranslated strings because they need to store
 * them as ids in a language independent way.  They then internally
 * before displaying call Q_ to translate, which allows an embedded
 * translation context to be specified.  The qnome format
 * "context|string" is used.  Intltool does not support this format
 * when it scans N_, so NC_("context","string") has to be used.  But
 * the standard NC_ does not propagate the context with the string. So
 * here it is overridden to combine both parts.
 *
 * A better solution would be to switch to a modern xgettext
 * https://wiki.gnome.org/MigratingFromIntltoolToGettext
 *
 *    xgettext --keyword=Q_:1g --keyword=N_:1g would allow using
 *    standard N_("context|string") to mark and pass on unchanged.
 *
 * This would also enable contextualised strings in introspection
 * markups, like
 *
 *    DT_INTENT_SATURATION = INTENT_SATURATION, // $DESCRIPTION: "rendering intent|saturation"
 *
 * Before storing in a language-indpendent format, like shortcutsrc,
 * NQ_ should be used to strip any context from the string.
 */
#undef NC_
#define NC_(Context, String) (Context "|" String)

static inline const gchar *NQ_(const gchar *String)
{
  const gchar *context_end = strchr(String, '|');
  return context_end ? context_end + 1 : String;
}

static inline gboolean dt_gimpmode(void)
{
  return darktable.gimp.mode ? TRUE : FALSE;
}

static inline gboolean dt_check_gimpmode(const char *mode)
{
  return darktable.gimp.mode ? strcmp(darktable.gimp.mode, mode) == 0 : FALSE;
}

static inline gboolean dt_check_gimpmode_ok(const char *mode)
{
  return darktable.gimp.mode ? !darktable.gimp.error && strcmp(darktable.gimp.mode, mode) == 0 : FALSE;
}

#ifdef __cplusplus
} // extern "C"
#endif /* __cplusplus */

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
