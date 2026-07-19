/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

/* spektrafilm — native spectral film simulation.
 *
 * Film modeling powered by spektrafilm (https://github.com/andreavolpato/spektrafilm),
 * GPLv3, © Andrea Volpato. Film/paper profile data CC BY-SA 4.0.
 *
 * This module computes the full spektrafilm colour pipeline natively per pixel:
 *
 *   scene-linear work RGB
 *     -> CAT16 to the film's reference illuminant, xy -> spectral upsampling
 *        (hanatos2025 tc LUT) x film sensitivity            = camera exposure
 *     -> highlight boost / diffusion / halation             (linear, spatial)
 *     -> log exposure -> DIR coupler correction (blurred)   = film development
 *     -> CMY film density -> grain                          (density, spatial)
 *     -> enlarger (dichroic-filtered light through the negative,
 *        print paper sensitivity, midgray-balanced)         = print exposure
 *     -> print diffusion filter (optional)                  (density, spatial)
 *     -> print density curves (with optional contrast morph)
 *     -> viewing illuminant through the print, CMFs -> XYZ
 *        -> CAT02 -> work RGB -> OkLCh gamut compression    = scanning
 *
 * The per-pixel colour science lives in spektra_sim.c (a validated port of
 * spektrafilm 0.3.x, max deviation < 1e-4 vs the Python reference); the
 * spatial effects (grain / halation / diffusion / highlight boost) live in
 * spektra_core.h/.c, both shared with the OpenCL-side ports.
 *
 * Data: drop a data pack exported by tools/spektrafilm_export_data.py into
 *   <config>/spektrafilm/            (pack.json + spectra_lut.f32)
 *   <config>/spektrafilm/profiles/   (*.json film + paper profiles)
 * Upgrading to a new spektrafilm release = re-running the exporter.
 *
 * This is a scene-to-display view transform: enable it INSTEAD of
 * sigmoid / filmic / agx.
 *
 * Both CPU (process, OpenMP) and GPU (process_cl, data/kernels/spektrafilm.cl)
 * paths exist. The GPU kernels were validated against the CPU engine with
 * POCL to ~1e-6; exact-spectral quality stays CPU-only.
 */

#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/file_location.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "develop/tiling.h"
#include "develop/imageop_gui.h"
#include "develop/imageop_math.h"
#include "common/imagebuf.h"
#include "common/iop_profile.h"
#include "common/opencl.h"
#include "common/gaussian.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SPEKTRA_INLINE static inline
#define SF_READ_FILE(path, out, len) \
  (g_file_get_contents((path), (out), (len), NULL) ? 0 : -1)
#define SF_FREE_FILE(buf) g_free(buf)
#define SF_DIAG_LOG(...) dt_print(DT_DEBUG_DEV, __VA_ARGS__)
#define SF_STRTOD(s, end) g_ascii_strtod((s), (end))
#include "common/spektra_core.h"
#include "common/spektra_sim.h"

DT_MODULE_INTROSPECTION(5, dt_iop_spektrafilm_params_t)

/* Spatial-scale constants, micrometres on film unless noted (see the LUT
   module for the full rationale; these are shared with modify_roi_in() and
   tiling_callback() so the halo math stays in sync). */
#define SF_HALATION_FIRST_SIGMA_UM 65.0f
#define SF_HALATION_PSF_SIGMAS 1.7320508f /* sqrt(3) */
/* widest stage-1 scatter component: max(sc_tail)*max(tail_rat) from
   spektra_core.c's sf_halation() = 9.7 * 2.7684 um, rounded up */
#define SF_SCATTER_TAIL_MAX_UM 27.0f
#define SF_GRAIN_BLUR_FACTOR 0.8f
#define SF_GRAIN_SIZE_MIN 0.05f
#define SF_HALO_SIGMAS 4.0f
#define SF_DIFFUSION_BLOOM_LAMBDA_MAX_UM 950.0f
/* DIR coupler inhibitor diffusion; spektrafilm params_schema
   dir_couplers.diffusion_size_um default (a plain gaussian in the reference) */

#define SF_MAX_PROFILES 128
#define SF_NAME_LEN 128
#define SF_PATH_LEN 1024

typedef enum dt_iop_spektrafilm_quality_t
{
  DT_SPEKTRAFILM_Q_DRAFT = 0,    // $DESCRIPTION: "draft (17³ table)"
  DT_SPEKTRAFILM_Q_STANDARD = 1, // $DESCRIPTION: "standard (33³ table)"
  DT_SPEKTRAFILM_Q_HIGH = 2,     // $DESCRIPTION: "high (49³ table)"
  DT_SPEKTRAFILM_Q_EXACT = 3,    // $DESCRIPTION: "exact spectral (very slow)"
} dt_iop_spektrafilm_quality_t;

/* order must match SF_DIFF_FAMILIES[] in spektra_core.c */
typedef enum dt_iop_spektrafilm_diffusion_family_t
{
  DT_SPEKTRAFILM_DIFF_BLACK_PRO_MIST = 0, // $DESCRIPTION: "black pro-mist"
  DT_SPEKTRAFILM_DIFF_GLIMMERGLASS = 1,   // $DESCRIPTION: "glimmerglass"
  DT_SPEKTRAFILM_DIFF_PRO_MIST = 2,       // $DESCRIPTION: "pro-mist"
  DT_SPEKTRAFILM_DIFF_CINEBLOOM = 3,      // $DESCRIPTION: "cinebloom"
} dt_iop_spektrafilm_diffusion_family_t;

typedef struct dt_iop_spektrafilm_params_t
{
  uint32_t film_hash;       // $DEFAULT: 0  (0 = first available filming stock)
  uint32_t paper_hash;      // $DEFAULT: 0  (0 = the film's target print stock)
  float exposure_ev;        // $MIN: -4.0 $MAX: 4.0 $DEFAULT: 0.0 $DESCRIPTION: "film exposure"
  float print_exposure_ev;  // $MIN: -3.0 $MAX: 3.0 $DEFAULT: 0.0 $DESCRIPTION: "print exposure"
  gboolean print_auto_exposure; // $DEFAULT: TRUE $DESCRIPTION: "auto print exposure"
  float print_contrast;     // $MIN: 0.5 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "print contrast"
  float filter_m;           // $MIN: -60.0 $MAX: 60.0 $DEFAULT: 0.0 $DESCRIPTION: "filtration M"
  float filter_y;           // $MIN: -60.0 $MAX: 60.0 $DEFAULT: 0.0 $DESCRIPTION: "filtration Y"
  float couplers_amount;    // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "DIR couplers"
  float preflash_exposure;  // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 0.0 $DESCRIPTION: "preflash exposure"
  float preflash_m_shift;   // $MIN: -60.0 $MAX: 60.0 $DEFAULT: 0.0 $DESCRIPTION: "preflash M filter shift"
  float preflash_y_shift;   // $MIN: -60.0 $MAX: 60.0 $DEFAULT: 0.0 $DESCRIPTION: "preflash Y filter shift"
  gboolean scan_film;       // $DEFAULT: FALSE $DESCRIPTION: "scan the film (skip print)"
  dt_iop_spektrafilm_quality_t quality; // $DEFAULT: DT_SPEKTRAFILM_Q_STANDARD $DESCRIPTION: "quality"
  gboolean halation_on;     // $DEFAULT: TRUE $DESCRIPTION: "enable halation"
  float scatter_amount;     // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "scatter amount"
  float scatter_scale;      // $MIN: 0.2 $MAX: 4.0 $DEFAULT: 1.0 $DESCRIPTION: "scatter size"
  float halation_amount;    // $MIN: 0.0 $MAX: 8.0 $DEFAULT: 1.0 $DESCRIPTION: "halation strength"
  float halation_scale;     // $MIN: 0.2 $MAX: 4.0 $DEFAULT: 1.0 $DESCRIPTION: "halation size"
  float boost_ev;           // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 0.0 $DESCRIPTION: "highlight boost"
  float boost_range;        // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.3 $DESCRIPTION: "boost range"
  float protect_ev;         // $MIN: 0.0 $MAX: 6.0 $DEFAULT: 4.0 $DESCRIPTION: "boost protect"
  gboolean diffusion_on;    // $DEFAULT: FALSE $DESCRIPTION: "enable diffusion filter"
  dt_iop_spektrafilm_diffusion_family_t diffusion_filter_family; // $DEFAULT: DT_SPEKTRAFILM_DIFF_BLACK_PRO_MIST $DESCRIPTION: "diffusion filter type"
  float diffusion_strength; // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 0.5 $DESCRIPTION: "diffusion strength"
  float diffusion_scale;    // $MIN: 0.2 $MAX: 4.0 $DEFAULT: 1.0 $DESCRIPTION: "diffusion size"
  float diffusion_warmth;   // $MIN: -1.5 $MAX: 1.5 $DEFAULT: 0.0 $DESCRIPTION: "diffusion halo warmth"
  gboolean print_diffusion_on;    // $DEFAULT: FALSE $DESCRIPTION: "enable print diffusion"
  dt_iop_spektrafilm_diffusion_family_t print_diffusion_filter_family; // $DEFAULT: DT_SPEKTRAFILM_DIFF_BLACK_PRO_MIST $DESCRIPTION: "print diffusion filter type"
  float print_diffusion_strength; // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 0.5 $DESCRIPTION: "print diffusion strength"
  float print_diffusion_scale;    // $MIN: 0.2 $MAX: 4.0 $DEFAULT: 1.0 $DESCRIPTION: "print diffusion size"
  float print_diffusion_warmth;   // $MIN: -1.5 $MAX: 1.5 $DEFAULT: 0.0 $DESCRIPTION: "print diffusion halo warmth"
  gboolean grain_on;        // $DEFAULT: TRUE $DESCRIPTION: "enable grain"
  float grain_amount;       // $MIN: 0.0 $MAX: 8.0 $DEFAULT: 1.0 $DESCRIPTION: "grain strength"
  float grain_size;         // $MIN: 0.2 $MAX: 4.0 $DEFAULT: 1.0 $DESCRIPTION: "grain size"
  float film_format_mm;     // $MIN: 8.0 $MAX: 130.0 $DEFAULT: 36.0 $DESCRIPTION: "film format"
  float output_luminance_boost; // $MIN: 0.5 $MAX: 4.0 $DEFAULT: 1.0 $DESCRIPTION: "pre-compression boost"
} dt_iop_spektrafilm_params_t;

/* one discovered profile: stock (= file base name), display name, stage */
typedef struct sf_prof_entry_t
{
  char stock[SF_NAME_LEN];
  char name[SF_NAME_LEN];
  char target_print[SF_NAME_LEN];
  gboolean printing; /* stage == "printing" */
  gboolean positive; /* info.type == "positive" (slide / reversal) */
  gboolean bw;       /* channel_model == "bw" */
  uint32_t hash;
} sf_prof_entry_t;

typedef struct dt_iop_spektrafilm_gui_data_t
{
  GtkWidget *film, *paper;
  GtkWidget *exposure_ev, *print_exposure_ev, *print_auto_exposure, *print_contrast, *filter_m, *filter_y;
  GtkWidget *couplers_amount, *scan_film, *quality;
  GtkWidget *preflash_exposure, *preflash_m_shift, *preflash_y_shift;
  GtkWidget *halation_on, *scatter_amount, *scatter_scale, *halation_amount, *halation_scale;
  GtkWidget *boost_ev, *boost_range, *protect_ev;
  GtkWidget *diffusion_on, *diffusion_filter_family, *diffusion_strength, *diffusion_scale, *diffusion_warmth;
  GtkWidget *print_diffusion_on, *print_diffusion_filter_family, *print_diffusion_strength, *print_diffusion_scale, *print_diffusion_warmth;
  GtkWidget *grain_on, *grain_amount, *grain_size, *film_format_mm, *output_luminance_boost;
  sf_prof_entry_t entries[SF_MAX_PROFILES];
  int n_entries;
  int film_entry[SF_MAX_PROFILES], n_films;   /* indices into entries[] */
  int paper_entry[SF_MAX_PROFILES], n_papers;
  GtkNotebook *notebook;
} dt_iop_spektrafilm_gui_data_t;

/* per-piece data: parameter snapshot + a lazily (re)built simulation.
   The sim depends on the pipe's work profile, which is only reliably known in
   process(), so the build happens there guarded by a mutex. */
typedef struct dt_iop_spektrafilm_data_t
{
  dt_iop_spektrafilm_params_t p;
  /* engine cache */
  dt_pthread_mutex_t lock;
  sf_sim_t *sim;
  sf_sim_gpu_t *gpu; /* float tables for process_cl; NULL for exact quality */
  uint64_t sim_key;  /* hash of everything the sim build depends on */
  char sim_error[256];
} dt_iop_spektrafilm_data_t;

typedef struct dt_iop_spektrafilm_global_data_t
{
  int kernel_expose, kernel_lograw, kernel_develop_corr, kernel_develop;
  int kernel_grain_gen, kernel_grain_add;
  int kernel_print_expose, kernel_print_develop, kernel_scan, kernel_passthrough;
  int kernel_scatter_combine, kernel_accum, kernel_channel_extract, kernel_channel_accum, kernel_halation_apply;
  int kernel_gauss_row_4c, kernel_gauss_col_4c, kernel_gauss_row_1c, kernel_gauss_col_1c;
  int kernel_max_partials, kernel_max_reduce, kernel_boost, kernel_diffusion_accum, kernel_diffusion_mix;
} dt_iop_spektrafilm_global_data_t;

/* the data pack is large (spectra LUT ~12 MB) and shared by all pieces;
   load it once per process (lazily, under _pack_lock), freed in
   cleanup_global(). Kept in module-static storage rather than global_data so
   every pipe sees the same pack. */
static sf_pack_t *_pack = NULL;
static char _pack_error[256] = { 0 };
static dt_pthread_mutex_t _pack_lock;

void init_global(dt_iop_module_so_t *self)
{
  dt_pthread_mutex_init(&_pack_lock, NULL);
  const int program = 42; /* spektrafilm.cl in data/kernels/programs.conf */
  dt_iop_spektrafilm_global_data_t *gd = malloc(sizeof(dt_iop_spektrafilm_global_data_t));
  self->data = gd;
  gd->kernel_expose = dt_opencl_create_kernel(program, "spektrafilm_expose");
  gd->kernel_lograw = dt_opencl_create_kernel(program, "spektrafilm_lograw");
  gd->kernel_develop_corr = dt_opencl_create_kernel(program, "spektrafilm_develop_corr");
  gd->kernel_develop = dt_opencl_create_kernel(program, "spektrafilm_develop");
  gd->kernel_grain_gen = dt_opencl_create_kernel(program, "spektrafilm_grain_gen");
  gd->kernel_grain_add = dt_opencl_create_kernel(program, "spektrafilm_grain_add");
  gd->kernel_print_expose = dt_opencl_create_kernel(program, "spektrafilm_print_expose");
  gd->kernel_print_develop = dt_opencl_create_kernel(program, "spektrafilm_print_develop");
  gd->kernel_scan = dt_opencl_create_kernel(program, "spektrafilm_scan");
  gd->kernel_passthrough = dt_opencl_create_kernel(program, "spektrafilm_passthrough");
  gd->kernel_scatter_combine = dt_opencl_create_kernel(program, "spektrafilm_scatter_combine");
  gd->kernel_accum = dt_opencl_create_kernel(program, "spektrafilm_accum");
  gd->kernel_channel_extract = dt_opencl_create_kernel(program, "spektrafilm_channel_extract");
  gd->kernel_gauss_row_4c = dt_opencl_create_kernel(program, "spektrafilm_gauss_row_4c");
  gd->kernel_gauss_col_4c = dt_opencl_create_kernel(program, "spektrafilm_gauss_col_4c");
  gd->kernel_gauss_row_1c = dt_opencl_create_kernel(program, "spektrafilm_gauss_row_1c");
  gd->kernel_gauss_col_1c = dt_opencl_create_kernel(program, "spektrafilm_gauss_col_1c");
  gd->kernel_channel_accum = dt_opencl_create_kernel(program, "spektrafilm_channel_accum");
  gd->kernel_halation_apply = dt_opencl_create_kernel(program, "spektrafilm_halation_apply");
  gd->kernel_max_partials = dt_opencl_create_kernel(program, "spektrafilm_max_partials");
  gd->kernel_max_reduce = dt_opencl_create_kernel(program, "spektrafilm_max_reduce");
  gd->kernel_boost = dt_opencl_create_kernel(program, "spektrafilm_boost");
  gd->kernel_diffusion_accum = dt_opencl_create_kernel(program, "spektrafilm_diffusion_accum");
  gd->kernel_diffusion_mix = dt_opencl_create_kernel(program, "spektrafilm_diffusion_mix");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_spektrafilm_global_data_t *gd = (dt_iop_spektrafilm_global_data_t *)self->data;
  if(gd)
  {
    dt_opencl_free_kernel(gd->kernel_expose);
    dt_opencl_free_kernel(gd->kernel_lograw);
    dt_opencl_free_kernel(gd->kernel_develop_corr);
    dt_opencl_free_kernel(gd->kernel_develop);
    dt_opencl_free_kernel(gd->kernel_grain_gen);
    dt_opencl_free_kernel(gd->kernel_grain_add);
    dt_opencl_free_kernel(gd->kernel_print_expose);
    dt_opencl_free_kernel(gd->kernel_print_develop);
    dt_opencl_free_kernel(gd->kernel_scan);
    dt_opencl_free_kernel(gd->kernel_passthrough);
    dt_opencl_free_kernel(gd->kernel_scatter_combine);
    dt_opencl_free_kernel(gd->kernel_accum);
    dt_opencl_free_kernel(gd->kernel_channel_extract);
    dt_opencl_free_kernel(gd->kernel_gauss_row_4c);
    dt_opencl_free_kernel(gd->kernel_gauss_col_4c);
    dt_opencl_free_kernel(gd->kernel_gauss_row_1c);
    dt_opencl_free_kernel(gd->kernel_gauss_col_1c);
    dt_opencl_free_kernel(gd->kernel_channel_accum);
    dt_opencl_free_kernel(gd->kernel_halation_apply);
    dt_opencl_free_kernel(gd->kernel_max_partials);
    dt_opencl_free_kernel(gd->kernel_max_reduce);
    dt_opencl_free_kernel(gd->kernel_boost);
    dt_opencl_free_kernel(gd->kernel_diffusion_accum);
    dt_opencl_free_kernel(gd->kernel_diffusion_mix);
    free(self->data);
    self->data = NULL;
  }
  dt_pthread_mutex_lock(&_pack_lock);
  if(_pack)
  {
    sf_pack_free(_pack);
    _pack = NULL;
  }
  _pack_error[0] = 0;
  dt_pthread_mutex_unlock(&_pack_lock);
  dt_pthread_mutex_destroy(&_pack_lock);
}

const char *name(void)
{
  return _("spektrafilm");
}
const char *aliases(void)
{
  return _("film simulation|analog|spectral|grain|halation|print");
}
const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(
      self,
      _("simulates the physical process of developing and printing analog film,\n"
        "using spectral emulsion and paper data from the spektrafilm project"),
      _("creative"), _("linear, RGB, scene-referred"), _("non-linear, RGB"),
      _("non-linear, RGB, display-referred"));
}
int default_group(void)
{
  return IOP_GROUP_COLOR | IOP_GROUP_GRADING;
}
int flags(void)
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_ALLOW_TILING;
}
dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *p,
                                            dt_dev_pixelpipe_iop_t *pi)
{
  return IOP_CS_RGB;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void **new_params, int32_t *new_params_size, int *new_version)
{
  /* v1 -> v5, v2 -> v5, v3 -> v5, v4 -> v5: each case below produces the
     current (v5) struct directly, rather than chaining through the
     intermediate versions -- same convention darktable's own modules use for
     multi-version migrations (see e.g. exposure.c, where every old_version
     case sets *new_version straight to its latest, not to old_version+1).

     v1 is the module's true original params shape, confirmed directly
     against the live, unmodified upstream source -- covering every params
     layout this module shipped with before the version was first bumped
     (the introspection version was never bumped through several early
     field additions, print_auto_exposure among them, during this module's
     fast-moving initial development, so v1 recovers history saved against
     any of those shapes too: the struct has only ever grown by appending
     fields, so an old, smaller saved blob still matches the leading
     fields of the current struct).

     v2 is the shape after the first proper version bump (preflash_*,
     diffusion_filter_family, output_luminance_boost added) but before
     print diffusion existed.

     v3 adds print_diffusion_on/print_diffusion_filter_family/
     print_diffusion_strength/print_diffusion_scale/print_diffusion_warmth
     -- a second, independent diffusion filter applied at the print stage
     rather than the film stage.

     v4 splits what used to be one shared halation_amount/halation_scale
     pair -- driving BOTH the in-emulsion scatter stage (always fully
     engaged, s_amount==1 hardcoded) and the back-reflection halation stage
     -- into two independent pairs: scatter_amount/scatter_scale (new) and
     halation_amount/halation_scale (unchanged meaning, now stage-2 only).
     Every v1/v2/v3 case sets scatter_amount = 1.0 (previously the implicit,
     unconfigurable, always-on value) and scatter_scale = <old
     halation_scale> (the spatial multiplier that value used to feed into
     BOTH stages) so migrated params reproduce the old rendering exactly,
     pixel for pixel, before the user ever touches the new sliders.

     v5 is a params-VALUE change, not a struct-shape change: sf_halation()
     no longer applies eff = halation_amount^1.3 before scaling
     halation_strength -- halation_amount is now a direct linear multiplier,
     matching upstream's a_tot = halation_strength * halation_amount exactly.
     Every version prior to v5 (v1 through v4, all of which share today's
     dt_iop_spektrafilm_params_t layout for this field) rendered under the
     ^1.3 curve, so every case below remaps
     halation_amount := old_halation_amount^1.3 -- the exact inverse of
     dropping the curve -- so a_tot stays numerically identical and every
     migrated edit still renders pixel-for-pixel as before, even though nothing
     about the struct's field layout changed. This is why a params version
     bump is still required even for a pure algorithm/semantics change: without
     it, darktable has no reason to call this function at all, and old edits
     would silently double-apply/skip the curve on load.

     From this version onward, any further params struct or semantics change
     should bump the version and add another case here rather than silently
     drift again. */
  typedef struct dt_iop_spektrafilm_params_v1_t
  {
    uint32_t film_hash;
    uint32_t paper_hash;
    float exposure_ev;
    float print_exposure_ev;
    gboolean print_auto_exposure;
    float print_contrast;
    float filter_m;
    float filter_y;
    float couplers_amount;
    gboolean scan_film;
    dt_iop_spektrafilm_quality_t quality;
    gboolean halation_on;
    float halation_amount;
    float halation_scale;
    float boost_ev;
    float boost_range;
    float protect_ev;
    gboolean diffusion_on;
    float diffusion_strength;
    float diffusion_scale;
    float diffusion_warmth;
    gboolean grain_on;
    float grain_amount;
    float grain_size;
    float film_format_mm;
  } dt_iop_spektrafilm_params_v1_t;

  typedef struct dt_iop_spektrafilm_params_v2_t
  {
    uint32_t film_hash;
    uint32_t paper_hash;
    float exposure_ev;
    float print_exposure_ev;
    gboolean print_auto_exposure;
    float print_contrast;
    float filter_m;
    float filter_y;
    float couplers_amount;
    float preflash_exposure;
    float preflash_m_shift;
    float preflash_y_shift;
    gboolean scan_film;
    dt_iop_spektrafilm_quality_t quality;
    gboolean halation_on;
    float halation_amount;
    float halation_scale;
    float boost_ev;
    float boost_range;
    float protect_ev;
    gboolean diffusion_on;
    dt_iop_spektrafilm_diffusion_family_t diffusion_filter_family;
    float diffusion_strength;
    float diffusion_scale;
    float diffusion_warmth;
    gboolean grain_on;
    float grain_amount;
    float grain_size;
    float film_format_mm;
    float output_luminance_boost;
  } dt_iop_spektrafilm_params_v2_t;

  typedef struct dt_iop_spektrafilm_params_v3_t
  {
    uint32_t film_hash;
    uint32_t paper_hash;
    float exposure_ev;
    float print_exposure_ev;
    gboolean print_auto_exposure;
    float print_contrast;
    float filter_m;
    float filter_y;
    float couplers_amount;
    float preflash_exposure;
    float preflash_m_shift;
    float preflash_y_shift;
    gboolean scan_film;
    dt_iop_spektrafilm_quality_t quality;
    gboolean halation_on;
    float halation_amount;
    float halation_scale;
    float boost_ev;
    float boost_range;
    float protect_ev;
    gboolean diffusion_on;
    dt_iop_spektrafilm_diffusion_family_t diffusion_filter_family;
    float diffusion_strength;
    float diffusion_scale;
    float diffusion_warmth;
    gboolean print_diffusion_on;
    dt_iop_spektrafilm_diffusion_family_t print_diffusion_filter_family;
    float print_diffusion_strength;
    float print_diffusion_scale;
    float print_diffusion_warmth;
    gboolean grain_on;
    float grain_amount;
    float grain_size;
    float film_format_mm;
    float output_luminance_boost;
  } dt_iop_spektrafilm_params_v3_t;

  typedef struct dt_iop_spektrafilm_params_v4_t
  {
    uint32_t film_hash;
    uint32_t paper_hash;
    float exposure_ev;
    float print_exposure_ev;
    gboolean print_auto_exposure;
    float print_contrast;
    float filter_m;
    float filter_y;
    float couplers_amount;
    float preflash_exposure;
    float preflash_m_shift;
    float preflash_y_shift;
    gboolean scan_film;
    dt_iop_spektrafilm_quality_t quality;
    gboolean halation_on;
    float scatter_amount;
    float scatter_scale;
    float halation_amount;
    float halation_scale;
    float boost_ev;
    float boost_range;
    float protect_ev;
    gboolean diffusion_on;
    dt_iop_spektrafilm_diffusion_family_t diffusion_filter_family;
    float diffusion_strength;
    float diffusion_scale;
    float diffusion_warmth;
    gboolean print_diffusion_on;
    dt_iop_spektrafilm_diffusion_family_t print_diffusion_filter_family;
    float print_diffusion_strength;
    float print_diffusion_scale;
    float print_diffusion_warmth;
    gboolean grain_on;
    float grain_amount;
    float grain_size;
    float film_format_mm;
    float output_luminance_boost;
  } dt_iop_spektrafilm_params_v4_t;

  if(old_version == 1)
  {
    const dt_iop_spektrafilm_params_v1_t *o = (dt_iop_spektrafilm_params_v1_t *)old_params;
    dt_iop_spektrafilm_params_t *n = malloc(sizeof(dt_iop_spektrafilm_params_t));

    n->film_hash = o->film_hash;
    n->paper_hash = o->paper_hash;
    n->exposure_ev = o->exposure_ev;
    n->print_exposure_ev = o->print_exposure_ev;
    n->print_auto_exposure = o->print_auto_exposure;
    n->print_contrast = o->print_contrast;
    n->filter_m = o->filter_m;
    n->filter_y = o->filter_y;
    n->couplers_amount = o->couplers_amount;
    n->preflash_exposure = 0.0f; /* new in v2: neutral default, no-op (matches upstream) */
    n->preflash_m_shift = 0.0f;  /* new in v2: neutral default, no-op */
    n->preflash_y_shift = 0.0f;  /* new in v2: neutral default, no-op */
    n->scan_film = o->scan_film;
    n->quality = o->quality;
    n->halation_on = o->halation_on;
    /* new in v4: scatter used to be hardcoded fully-on and shared
       halation_scale as its spatial multiplier -- reproduce that exactly. */
    n->scatter_amount = 1.0f;
    n->scatter_scale = o->halation_scale;
    n->halation_amount = powf(o->halation_amount, 1.3f); /* v5: undo dropped ^1.3 curve */
    n->halation_scale = o->halation_scale;
    n->boost_ev = o->boost_ev;
    n->boost_range = o->boost_range;
    n->protect_ev = o->protect_ev;
    n->diffusion_on = o->diffusion_on;
    /* new in v2: the engine was hardcoded to Black Pro-Mist before the
       family selector existed, so this exactly reproduces old saved
       diffusion settings rather than just picking a neutral default. */
    n->diffusion_filter_family = DT_SPEKTRAFILM_DIFF_BLACK_PRO_MIST;
    n->diffusion_strength = o->diffusion_strength;
    n->diffusion_scale = o->diffusion_scale;
    n->diffusion_warmth = o->diffusion_warmth;
    /* new in v3: print diffusion never existed for anything saved against
       v1, so it defaults off, same $DEFAULT the param itself declares. */
    n->print_diffusion_on = FALSE;
    n->print_diffusion_filter_family = DT_SPEKTRAFILM_DIFF_BLACK_PRO_MIST;
    n->print_diffusion_strength = 0.5f;
    n->print_diffusion_scale = 1.0f;
    n->print_diffusion_warmth = 0.0f;
    n->grain_on = o->grain_on;
    n->grain_amount = o->grain_amount;
    n->grain_size = o->grain_size;
    n->film_format_mm = o->film_format_mm;
    n->output_luminance_boost = 1.0f; /* new in v2: neutral default, no-op (matches upstream) */

    *new_params = n;
    *new_params_size = sizeof(dt_iop_spektrafilm_params_t);
    *new_version = 5;
    return 0;
  }
  if(old_version == 2)
  {
    const dt_iop_spektrafilm_params_v2_t *o = (dt_iop_spektrafilm_params_v2_t *)old_params;
    dt_iop_spektrafilm_params_t *n = malloc(sizeof(dt_iop_spektrafilm_params_t));

    n->film_hash = o->film_hash;
    n->paper_hash = o->paper_hash;
    n->exposure_ev = o->exposure_ev;
    n->print_exposure_ev = o->print_exposure_ev;
    n->print_auto_exposure = o->print_auto_exposure;
    n->print_contrast = o->print_contrast;
    n->filter_m = o->filter_m;
    n->filter_y = o->filter_y;
    n->couplers_amount = o->couplers_amount;
    n->preflash_exposure = o->preflash_exposure;
    n->preflash_m_shift = o->preflash_m_shift;
    n->preflash_y_shift = o->preflash_y_shift;
    n->scan_film = o->scan_film;
    n->quality = o->quality;
    n->halation_on = o->halation_on;
    /* new in v4: scatter used to be hardcoded fully-on and shared
       halation_scale as its spatial multiplier -- reproduce that exactly. */
    n->scatter_amount = 1.0f;
    n->scatter_scale = o->halation_scale;
    n->halation_amount = powf(o->halation_amount, 1.3f); /* v5: undo dropped ^1.3 curve */
    n->halation_scale = o->halation_scale;
    n->boost_ev = o->boost_ev;
    n->boost_range = o->boost_range;
    n->protect_ev = o->protect_ev;
    n->diffusion_on = o->diffusion_on;
    n->diffusion_filter_family = o->diffusion_filter_family;
    n->diffusion_strength = o->diffusion_strength;
    n->diffusion_scale = o->diffusion_scale;
    n->diffusion_warmth = o->diffusion_warmth;
    /* new in v3: nothing saved against v2 ever had print diffusion, so it
       defaults off, same $DEFAULT the param itself declares. */
    n->print_diffusion_on = FALSE;
    n->print_diffusion_filter_family = DT_SPEKTRAFILM_DIFF_BLACK_PRO_MIST;
    n->print_diffusion_strength = 0.5f;
    n->print_diffusion_scale = 1.0f;
    n->print_diffusion_warmth = 0.0f;
    n->grain_on = o->grain_on;
    n->grain_amount = o->grain_amount;
    n->grain_size = o->grain_size;
    n->film_format_mm = o->film_format_mm;
    n->output_luminance_boost = o->output_luminance_boost;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_spektrafilm_params_t);
    *new_version = 5;
    return 0;
  }
  if(old_version == 3)
  {
    const dt_iop_spektrafilm_params_v3_t *o = (dt_iop_spektrafilm_params_v3_t *)old_params;
    dt_iop_spektrafilm_params_t *n = malloc(sizeof(dt_iop_spektrafilm_params_t));

    n->film_hash = o->film_hash;
    n->paper_hash = o->paper_hash;
    n->exposure_ev = o->exposure_ev;
    n->print_exposure_ev = o->print_exposure_ev;
    n->print_auto_exposure = o->print_auto_exposure;
    n->print_contrast = o->print_contrast;
    n->filter_m = o->filter_m;
    n->filter_y = o->filter_y;
    n->couplers_amount = o->couplers_amount;
    n->preflash_exposure = o->preflash_exposure;
    n->preflash_m_shift = o->preflash_m_shift;
    n->preflash_y_shift = o->preflash_y_shift;
    n->scan_film = o->scan_film;
    n->quality = o->quality;
    n->halation_on = o->halation_on;
    /* new in v4: scatter used to be hardcoded fully-on and shared
       halation_scale as its spatial multiplier -- reproduce that exactly. */
    n->scatter_amount = 1.0f;
    n->scatter_scale = o->halation_scale;
    n->halation_amount = powf(o->halation_amount, 1.3f); /* v5: undo dropped ^1.3 curve */
    n->halation_scale = o->halation_scale;
    n->boost_ev = o->boost_ev;
    n->boost_range = o->boost_range;
    n->protect_ev = o->protect_ev;
    n->diffusion_on = o->diffusion_on;
    n->diffusion_filter_family = o->diffusion_filter_family;
    n->diffusion_strength = o->diffusion_strength;
    n->diffusion_scale = o->diffusion_scale;
    n->diffusion_warmth = o->diffusion_warmth;
    n->print_diffusion_on = o->print_diffusion_on;
    n->print_diffusion_filter_family = o->print_diffusion_filter_family;
    n->print_diffusion_strength = o->print_diffusion_strength;
    n->print_diffusion_scale = o->print_diffusion_scale;
    n->print_diffusion_warmth = o->print_diffusion_warmth;
    n->grain_on = o->grain_on;
    n->grain_amount = o->grain_amount;
    n->grain_size = o->grain_size;
    n->film_format_mm = o->film_format_mm;
    n->output_luminance_boost = o->output_luminance_boost;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_spektrafilm_params_t);
    *new_version = 5;
    return 0;
  }
  if(old_version == 4)
  {
    const dt_iop_spektrafilm_params_v4_t *o = (dt_iop_spektrafilm_params_v4_t *)old_params;
    dt_iop_spektrafilm_params_t *n = malloc(sizeof(dt_iop_spektrafilm_params_t));

    n->film_hash = o->film_hash;
    n->paper_hash = o->paper_hash;
    n->exposure_ev = o->exposure_ev;
    n->print_exposure_ev = o->print_exposure_ev;
    n->print_auto_exposure = o->print_auto_exposure;
    n->print_contrast = o->print_contrast;
    n->filter_m = o->filter_m;
    n->filter_y = o->filter_y;
    n->couplers_amount = o->couplers_amount;
    n->preflash_exposure = o->preflash_exposure;
    n->preflash_m_shift = o->preflash_m_shift;
    n->preflash_y_shift = o->preflash_y_shift;
    n->scan_film = o->scan_film;
    n->quality = o->quality;
    n->halation_on = o->halation_on;
    /* v4 already had independent scatter_amount/scatter_scale -- carry them
       over unchanged, only halation_amount's curve is being removed here. */
    n->scatter_amount = o->scatter_amount;
    n->scatter_scale = o->scatter_scale;
    n->halation_amount = powf(o->halation_amount, 1.3f); /* v5: undo dropped ^1.3 curve */
    n->halation_scale = o->halation_scale;
    n->boost_ev = o->boost_ev;
    n->boost_range = o->boost_range;
    n->protect_ev = o->protect_ev;
    n->diffusion_on = o->diffusion_on;
    n->diffusion_filter_family = o->diffusion_filter_family;
    n->diffusion_strength = o->diffusion_strength;
    n->diffusion_scale = o->diffusion_scale;
    n->diffusion_warmth = o->diffusion_warmth;
    n->print_diffusion_on = o->print_diffusion_on;
    n->print_diffusion_filter_family = o->print_diffusion_filter_family;
    n->print_diffusion_strength = o->print_diffusion_strength;
    n->print_diffusion_scale = o->print_diffusion_scale;
    n->print_diffusion_warmth = o->print_diffusion_warmth;
    n->grain_on = o->grain_on;
    n->grain_amount = o->grain_amount;
    n->grain_size = o->grain_size;
    n->film_format_mm = o->film_format_mm;
    n->output_luminance_boost = o->output_luminance_boost;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_spektrafilm_params_t);
    *new_version = 5;
    return 0;
  }
  return 1;
}

/* ---------------------------------------------------------------------- */
/* profile discovery                                                      */
/* ---------------------------------------------------------------------- */

/* stable string hash for profile identity in params (same as the LUT module
   used for bundles, so behaviour across machines/rescans is order-free) */
static uint32_t sf_name_hash(const char *s)
{
  uint32_t h = 2166136261u; /* FNV-1a */
  for(const unsigned char *p = (const unsigned char *)s; *p; p++)
  {
    h ^= *p;
    h *= 16777619u;
  }
  return h ? h : 1; /* 0 is reserved for "first available" */
}

static void sf_pack_dir(char *dst, size_t dstsz)
{
  char cfg[SF_PATH_LEN];
  dt_loc_get_user_config_dir(cfg, sizeof cfg);
  snprintf(dst, dstsz, "%s/spektrafilm", cfg);
}

/* natural (human) string compare: embedded numbers compared numerically
   so "Vision3 50D" < "Vision3 200T" < "Vision3 500T" */
static int sf_nat_cmp(const char *a, const char *b)
{
  for(;;)
  {
    if(*a == 0) return *b == 0 ? 0 : -1;
    if(*b == 0) return 1;
    int da = (unsigned)*a - '0' < 10u;
    int db = (unsigned)*b - '0' < 10u;
    if(da && db)
    {
      unsigned long va = 0, vb = 0;
      while((unsigned)*a - '0' < 10u) { va = va * 10 + (*a - '0'); a++; }
      while((unsigned)*b - '0' < 10u) { vb = vb * 10 + (*b - '0'); b++; }
      if(va != vb) return va < vb ? -1 : 1;
    }
    else if(da != db)
      return da ? -1 : 1;
    else
    {
      int ca = g_ascii_tolower(*a);
      int cb = g_ascii_tolower(*b);
      if(ca != cb) return ca < cb ? -1 : 1;
      a++; b++;
    }
  }
}

/* scan <config>/spektrafilm/profiles/ (all .json files); reads only the info header of
   each profile (stock / name / stage / target_print) */
static int sf_scan_profiles(sf_prof_entry_t *out, int maxn)
{
  char dir[SF_PATH_LEN];
  sf_pack_dir(dir, sizeof dir);
  char profdir[SF_PATH_LEN + 16];
  snprintf(profdir, sizeof profdir, "%s/profiles", dir);

  GDir *gd = g_dir_open(profdir, 0, NULL);
  if(!gd) return 0;
  int n = 0;
  const char *fn;
  while(n < maxn && (fn = g_dir_read_name(gd)))
  {
    if(!g_str_has_suffix(fn, ".json")) continue;
    char path[SF_PATH_LEN + 300];
    snprintf(path, sizeof path, "%s/%s", profdir, fn);
    char *err = NULL;
    sf_profile_t *prof = sf_profile_load(path, &err);
    if(!prof)
    {
      free(err);
      continue;
    }
    sf_prof_entry_t *e = &out[n];
    memset(e, 0, sizeof(*e));
    g_strlcpy(e->stock, sf_profile_stock(prof) ? sf_profile_stock(prof) : fn, SF_NAME_LEN);
    /* strip .json when falling back to the file name */
    char *dot = strstr(e->stock, ".json");
    if(dot) *dot = 0;
    g_strlcpy(e->name, sf_profile_name(prof) ? sf_profile_name(prof) : e->stock, SF_NAME_LEN);
    const char *stage = sf_profile_stage(prof);
    e->printing = (stage && !strcmp(stage, "printing"));
    const char *tp = sf_profile_target_print(prof);
    if(tp) g_strlcpy(e->target_print, tp, SF_NAME_LEN);
    const char *type = sf_profile_type(prof);
    e->positive = (type && !strcmp(type, "positive"));
    const char *cm = sf_profile_channel_model(prof);
    e->bw = (cm && !strcmp(cm, "bw"));
    e->hash = sf_name_hash(e->stock);
    sf_profile_free(prof);
    n++;
  }
  g_dir_close(gd);
  /* natural order by display name (numbers compared numerically,
     so "50D" < "200T" instead of lexicographic "200T" < "50D") */
  for(int i = 0; i < n; i++)
    for(int j = i + 1; j < n; j++)
      if(sf_nat_cmp(out[j].name, out[i].name) < 0)
      {
        sf_prof_entry_t t = out[i];
        out[i] = out[j];
        out[j] = t;
      }
  return n;
}

/* resolve a profile hash to its stock name. hash 0 -> default:
   for films the first filming stock, for papers prefer the film's
   target_print. Returns false when nothing matches. */
static gboolean sf_resolve_stock(const sf_prof_entry_t *entries, int n, uint32_t hash,
                                 gboolean want_printing, const char *prefer_stock,
                                 char *dst, size_t dstsz)
{
  if(hash)
    for(int i = 0; i < n; i++)
      if(entries[i].hash == hash && entries[i].printing == want_printing)
      {
        g_strlcpy(dst, entries[i].stock, dstsz);
        return TRUE;
      }
  if(prefer_stock && prefer_stock[0])
    for(int i = 0; i < n; i++)
      if(entries[i].printing == want_printing && !strcmp(entries[i].stock, prefer_stock))
      {
        g_strlcpy(dst, entries[i].stock, dstsz);
        return TRUE;
      }
  for(int i = 0; i < n; i++)
    if(entries[i].printing == want_printing)
    {
      g_strlcpy(dst, entries[i].stock, dstsz);
      return TRUE;
    }
  return FALSE;
}

/* ---------------------------------------------------------------------- */
/* pipeline plumbing                                                      */
/* ---------------------------------------------------------------------- */

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_spektrafilm_data_t *d = calloc(1, sizeof(dt_iop_spektrafilm_data_t));
  dt_pthread_mutex_init(&d->lock, NULL);
  piece->data = d;
}
void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_spektrafilm_data_t *d = (dt_iop_spektrafilm_data_t *)piece->data;
  if(d)
  {
    if(d->gpu) sf_sim_gpu_free(d->gpu);
    if(d->sim) sf_sim_free(d->sim);
    dt_pthread_mutex_destroy(&d->lock);
  }
  free(piece->data);
  piece->data = NULL;
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_spektrafilm_data_t *d = (dt_iop_spektrafilm_data_t *)piece->data;
  d->p = *(dt_iop_spektrafilm_params_t *)p1;
  /* the sim itself is (re)built lazily in process(), where the pipe's work
     profile is reliably known; a stale sim is detected via sim_key there. */
  /* exact-spectral quality has no GPU kernels: stay on the CPU path */
  if(d->p.quality == DT_SPEKTRAFILM_Q_EXACT) piece->process_cl_ready = FALSE;
}

static uint64_t _mix64(uint64_t h, const void *data, size_t len)
{
  const unsigned char *p = data;
  for(size_t i = 0; i < len; i++)
  {
    h ^= p[i];
    h *= 0x100000001b3ULL; /* FNV-1a 64 */
  }
  return h;
}

static int _quality_steps(dt_iop_spektrafilm_quality_t q)
{
  switch(q)
  {
    case DT_SPEKTRAFILM_Q_DRAFT: return 17;
    case DT_SPEKTRAFILM_Q_HIGH: return 49;
    case DT_SPEKTRAFILM_Q_EXACT: return 0; /* exact spectral, no table */
    case DT_SPEKTRAFILM_Q_STANDARD:
    default: return 33;
  }
}

/* make sure d->sim matches the current params + work profile; returns the sim
   or NULL (passthrough). Called from process() under no assumption of being
   single-threaded (full/preview pipes run concurrently). */
static sf_sim_t *_ensure_sim(dt_iop_spektrafilm_data_t *d,
                             const dt_iop_order_iccprofile_info_t *work_profile)
{
  const dt_iop_spektrafilm_params_t *p = &d->p;

  /* the work profile's RGB<->XYZ matrices feed the engine; include them in
     the cache key so a work-profile change rebuilds the sim */
  float m_in[9], m_out[9];
  for(int i = 0; i < 3; i++)
    for(int j = 0; j < 3; j++)
    {
      /* dt_colormatrix_t, row-major: XYZ_i = sum_j matrix_in[i][j] * RGB_j */
      m_in[i * 3 + j] = work_profile->matrix_in[i][j];
      m_out[i * 3 + j] = work_profile->matrix_out[i][j];
    }

  uint64_t key = 0xcbf29ce484222325ULL;
  key = _mix64(key, &p->film_hash, sizeof p->film_hash);
  key = _mix64(key, &p->paper_hash, sizeof p->paper_hash);
  key = _mix64(key, &p->exposure_ev, sizeof p->exposure_ev);
  key = _mix64(key, &p->print_exposure_ev, sizeof p->print_exposure_ev);
  key = _mix64(key, &p->print_auto_exposure, sizeof p->print_auto_exposure);
  key = _mix64(key, &p->print_contrast, sizeof p->print_contrast);
  key = _mix64(key, &p->filter_m, sizeof p->filter_m);
  key = _mix64(key, &p->filter_y, sizeof p->filter_y);
  key = _mix64(key, &p->couplers_amount, sizeof p->couplers_amount);
  key = _mix64(key, &p->preflash_exposure, sizeof p->preflash_exposure);
  key = _mix64(key, &p->preflash_m_shift, sizeof p->preflash_m_shift);
  key = _mix64(key, &p->preflash_y_shift, sizeof p->preflash_y_shift);
  key = _mix64(key, &p->scan_film, sizeof p->scan_film);
  key = _mix64(key, &p->quality, sizeof p->quality);
  key = _mix64(key, &p->output_luminance_boost, sizeof p->output_luminance_boost);
  key = _mix64(key, m_in, sizeof m_in);
  key = _mix64(key, m_out, sizeof m_out);

  dt_pthread_mutex_lock(&d->lock);
  if(d->sim && d->sim_key == key)
  {
    sf_sim_t *s = d->sim;
    dt_pthread_mutex_unlock(&d->lock);
    return s;
  }

  /* (re)build */
  if(d->gpu)
  {
    sf_sim_gpu_free(d->gpu);
    d->gpu = NULL;
  }
  if(d->sim)
  {
    sf_sim_free(d->sim);
    d->sim = NULL;
  }
  d->sim_key = key;
  d->sim_error[0] = 0;

  /* global pack, loaded once */
  dt_pthread_mutex_lock(&_pack_lock);
  if(!_pack && !_pack_error[0])
  {
    char dir[SF_PATH_LEN];
    sf_pack_dir(dir, sizeof dir);
    char *err = NULL;
    _pack = sf_pack_load(dir, &err);
    if(!_pack)
    {
      g_strlcpy(_pack_error, err ? err : "unknown", sizeof _pack_error);
      dt_print(DT_DEBUG_DEV, "[spektrafilm] %s\n", _pack_error);
      free(err);
    }
    else
      dt_print(DT_DEBUG_DEV, "[spektrafilm] loaded data pack %s (spektrafilm %s)\n", dir,
               sf_pack_version(_pack));
  }
  sf_pack_t *pack = _pack;
  dt_pthread_mutex_unlock(&_pack_lock);
  if(!pack)
  {
    dt_pthread_mutex_unlock(&d->lock);
    return NULL;
  }

  /* resolve stocks */
  sf_prof_entry_t entries[SF_MAX_PROFILES];
  const int n = sf_scan_profiles(entries, SF_MAX_PROFILES);
  char film_stock[SF_NAME_LEN] = { 0 }, paper_stock[SF_NAME_LEN] = { 0 };
  if(!sf_resolve_stock(entries, n, p->film_hash, FALSE, "kodak_portra_400", film_stock,
                       sizeof film_stock))
  {
    g_strlcpy(d->sim_error, "no filming profiles found", sizeof d->sim_error);
    dt_pthread_mutex_unlock(&d->lock);
    return NULL;
  }
  const char *target_print = NULL;
  for(int i = 0; i < n; i++)
    if(!entries[i].printing && !strcmp(entries[i].stock, film_stock))
      target_print = entries[i].target_print;
  if(!p->scan_film
     && !sf_resolve_stock(entries, n, p->paper_hash, TRUE, target_print, paper_stock,
                          sizeof paper_stock))
  {
    g_strlcpy(d->sim_error, "no printing profiles found", sizeof d->sim_error);
    dt_pthread_mutex_unlock(&d->lock);
    return NULL;
  }

  char dir[SF_PATH_LEN], path[SF_PATH_LEN + 300];
  sf_pack_dir(dir, sizeof dir);
  char *err = NULL;
  snprintf(path, sizeof path, "%s/profiles/%s.json", dir, film_stock);
  sf_profile_t *film = sf_profile_load(path, &err);
  sf_profile_t *paper = NULL;
  if(film && !p->scan_film)
  {
    snprintf(path, sizeof path, "%s/profiles/%s.json", dir, paper_stock);
    paper = sf_profile_load(path, &err);
  }

  if(film && (paper || p->scan_film))
  {
    sf_sim_params_t sp;
    sf_sim_params_defaults(&sp);
    sp.exposure_comp_ev = p->exposure_ev;
    sp.print_exposure = powf(2.0f, p->print_exposure_ev);
    sp.print_exposure_compensation = p->print_auto_exposure; /* normalize_print_exposure
                                       stays at sf_sim_params_defaults' true — that combination
                                       is what gives f_mid (a fixed reference midgray density)
                                       when this toggle is off, i.e. film exposure then has its
                                       full, uncompensated effect on brightness; see
                                       sf_sim_build's midgray_factor branches */
    sp.m_filter_shift = p->filter_m;
    sp.y_filter_shift = p->filter_y;
    sp.couplers_active = (p->couplers_amount > 0.0f);
    sp.couplers_amount = p->couplers_amount;
    sp.preflash_exposure = p->preflash_exposure;
    sp.preflash_m_shift = p->preflash_m_shift;
    sp.preflash_y_shift = p->preflash_y_shift;
    sp.scan_film = p->scan_film;
    sp.lut_steps = _quality_steps(p->quality);
    sp.out_luminance_boost = p->output_luminance_boost;
    if(p->print_contrast != 1.0f)
    {
      sp.morph_active = true;
      sp.morph_gamma = p->print_contrast;
    }
    /* darktable pipeline XYZ is D50-relative; the work profile matrices map
       work RGB <-> that XYZ, so both engine whites are D50 */
    static const double d50_xy[2] = { 0.3457, 0.3585 };
    for(int i = 0; i < 9; i++)
    {
      sp.input_rgb_to_xyz[i] = m_in[i];
      sp.output_rgb_to_xyz[i] = m_in[i];
      sp.output_xyz_to_rgb[i] = m_out[i];
    }
    sp.input_white_xy[0] = sp.output_white_xy[0] = d50_xy[0];
    sp.input_white_xy[1] = sp.output_white_xy[1] = d50_xy[1];

    d->sim = sf_sim_build(pack, film, paper, &sp, &err);
    if(!d->sim && err)
    {
      g_strlcpy(d->sim_error, err, sizeof d->sim_error);
      dt_print(DT_DEBUG_DEV, "[spektrafilm] %s\n", err);
    }
    else if(d->sim)
    {
      /* float tables for the GPU path (NULL for exact-spectral quality,
         which stays CPU-only) */
      d->gpu = sf_sim_gpu_export(d->sim);
      dt_print(DT_DEBUG_DEV, "[spektrafilm] built sim: %s -> %s (steps %d, gpu %s)\n",
               film_stock, p->scan_film ? "(scan film)" : paper_stock, sp.lut_steps,
               d->gpu ? "yes" : "no");
    }
  }
  free(err);
  if(film) sf_profile_free(film);
  if(paper) sf_profile_free(paper);

  sf_sim_t *s = d->sim;
  dt_pthread_mutex_unlock(&d->lock);
  return s;
}

/* ---------------------------------------------------------------------- */
/* ROI / tiling: expand the input by the spatial-effect halo               */
/* ---------------------------------------------------------------------- */

static float _max_halo_sigma(const dt_iop_spektrafilm_params_t *p, float pixel_um)
{
  const float inv_um = 1.0f / fmaxf(pixel_um, 1e-3f);
  /* halation stage: first-bounce radius, scaled by the user's halation_scale
     (previously this padding ignored halation_scale entirely, silently
     under-padding for anyone above the 1.0 default -- fixed here). */
  const float hal_scale = fmaxf(p->halation_scale, 1e-3f);
  const float hal = (p->halation_on && p->halation_amount > 0.0f)
                        ? SF_HALATION_FIRST_SIGMA_UM * SF_HALATION_PSF_SIGMAS * hal_scale * inv_um
                        : 0.0f;
  /* scatter stage: widest core+tail component, scaled by its own
     scatter_scale (independent from halation_scale since the scatter_amount/
     scatter_scale split). */
  const float scat_scale = fmaxf(p->scatter_scale, 1e-3f);
  const float scat = (p->halation_on && p->scatter_amount > 0.0f)
                         ? SF_SCATTER_TAIL_MAX_UM * SF_HALATION_PSF_SIGMAS * scat_scale * inv_um
                         : 0.0f;
  /* The widest of film-stage and print-stage diffusion determines the ROI
     padding — both must fit in the expanded tile. Both stages use the same
     bloom constant; only the user's scale slider differs between them. */
  const float diff_film = p->diffusion_on
                              ? SF_DIFFUSION_BLOOM_LAMBDA_MAX_UM * 1.41421356f * p->diffusion_scale * inv_um
                              : 0.0f;
  const float diff_print = p->print_diffusion_on
                               ? SF_DIFFUSION_BLOOM_LAMBDA_MAX_UM * 1.41421356f
                                     * p->print_diffusion_scale * inv_um
                               : 0.0f;
  const float diff = fmaxf(diff_film, diff_print);
  const float grain = (p->grain_on && p->grain_amount > 0.0f)
                          ? SF_GRAIN_BLUR_FACTOR * SF_GRAIN_REF_UM
                                * fmaxf(p->grain_size, SF_GRAIN_SIZE_MIN) * inv_um
                          : 0.0f;
  /* coupler halo: gaussian core plus the widest exponential-tail component;
     the per-film tail size is unknown before the sim exists, so assume the
     stock value all current profiles use (200 um) whenever couplers are on */
  const float coupler = (p->couplers_amount > 0.0f)
                            ? fmaxf((float)SF_COUPLER_BLUR_UM,
                                    (float)(SF_EXPTAIL_R2 * 200.0)) * inv_um
                            : 0.0f;
  return fmaxf(fmaxf(fmaxf(hal, scat), diff), fmaxf(grain, coupler));
}

void modify_roi_in(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;
  const dt_iop_spektrafilm_data_t *const d = (const dt_iop_spektrafilm_data_t *)piece->data;
  if(!d) return;
  const float full_w = fmaxf((float)piece->buf_in.width * roi_out->scale, 1.0f);
  const float pixel_um = d->p.film_format_mm * 1000.0f / full_w;
  const int halo = (int)ceilf(SF_HALO_SIGMAS * _max_halo_sigma(&d->p, pixel_um));
  if(halo <= 0) return;
  const int img_w = (int)roundf((float)piece->buf_in.width * roi_out->scale);
  const int img_h = (int)roundf((float)piece->buf_in.height * roi_out->scale);
  int x0 = roi_out->x - halo, y0 = roi_out->y - halo;
  int x1 = roi_out->x + roi_out->width + halo, y1 = roi_out->y + roi_out->height + halo;
  if(x0 < 0) x0 = 0;
  if(y0 < 0) y0 = 0;
  if(img_w > 0 && x1 > img_w) x1 = img_w;
  if(img_h > 0 && y1 > img_h) y1 = img_h;
  roi_in->x = x0;
  roi_in->y = y0;
  roi_in->width = x1 - x0;
  roi_in->height = y1 - y0;
}

void tiling_callback(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     dt_develop_tiling_t *tiling)
{
  const dt_iop_spektrafilm_data_t *const d = (const dt_iop_spektrafilm_data_t *)piece->data;
  const float full_w = fmaxf((float)piece->buf_in.width * roi_in->scale, 1.0f);
  const float pixel_um = d->p.film_format_mm * 1000.0f / full_w;
  tiling->factor = 2.5f; /* 4 float4 buffers, but they alias in practice */
  tiling->factor_cl = 4.0f; /* + gtmp4 (1 float4) + plane1 and gtmp1 (1ch each, 1/4 float4) */
  tiling->maxbuf = 1.0f;
  tiling->maxbuf_cl = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = (unsigned)ceilf(SF_HALO_SIGMAS * _max_halo_sigma(&d->p, pixel_um));
  tiling->align = 1;
}

/* ---------------------------------------------------------------------- */
/* process                                                                */
/* ---------------------------------------------------------------------- */

static void _passthrough(const float *in, float *out, int w, int oh, int ow, int ox, int oy)
{
  for(int y = 0; y < oh; y++)
    for(int x = 0; x < ow; x++)
    {
      const float *s = in + ((size_t)(y + oy) * w + (x + ox)) * 4;
      float *o = out + ((size_t)y * ow + x) * 4;
      o[0] = s[0];
      o[1] = s[1];
      o[2] = s[2];
      o[3] = s[3];
    }
}

void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_spektrafilm_data_t *const d = (dt_iop_spektrafilm_data_t *)piece->data;
  /* process the FULL input ROI (expanded by modify_roi_in), then crop roi_out */
  const int w = roi_in->width, h = roi_in->height;
  const int ow = roi_out->width, oh = roi_out->height;
  const int ox = roi_out->x - roi_in->x, oy = roi_out->y - roi_in->y;
  const size_t npix = (size_t)w * h;
  const float *const in = (const float *)ivoid;
  float *const out = (float *)ovoid;

  const dt_iop_order_iccprofile_info_t *const work_profile
      = dt_ioppr_get_pipe_work_profile_info(piece->pipe);
  sf_sim_t *sim = work_profile ? _ensure_sim(d, work_profile) : NULL;
  if(!sim)
  {
    _passthrough(in, out, w, oh, ow, ox, oy);
    return;
  }

  /* physical micrometres per pixel at this pipe resolution */
  const float full_w = fmaxf((float)piece->buf_in.width * roi_in->scale, 1.0f);
  const float pixel_um = d->p.film_format_mm * 1000.0f / full_w;

  float *plane = dt_alloc_align_float(npix * 3);  /* raw / lograw / cmy, in place */
  float *corr = dt_alloc_align_float(npix * 3);   /* DIR coupler correction field */
  float *scratch = dt_alloc_align_float(npix);    /* 1ch blur scratch */
  if(!plane || !corr || !scratch)
  {
    if(plane) dt_free_align(plane);
    if(corr) dt_free_align(corr);
    if(scratch) dt_free_align(scratch);
    _passthrough(in, out, w, oh, ow, ox, oy);
    return;
  }

  /* 1) camera exposure: work RGB -> spectral upsampling -> film raw exposure
        (includes the film-exposure EV) */
  sf_sim_expose(sim, in, plane, npix, 4, 3);

  /* 2) pre-film spatial effects on LINEAR exposure, spektrafilm's order:
        highlight boost -> diffusion filter -> halation */
  sf_boost_highlights(plane, w, h, d->p.boost_ev, d->p.boost_range, d->p.protect_ev);
  if(d->p.diffusion_on)
    sf_diffusion_filter(plane, w, h, (double)pixel_um, (int)d->p.diffusion_filter_family,
                        d->p.diffusion_strength, d->p.diffusion_scale, d->p.diffusion_warmth);
  if(d->p.halation_on && (d->p.scatter_amount > 0.0f || d->p.halation_amount > 0.0f))
  {
    double hal_strength[3], hal_sigma_um;
    sf_sim_halation_params(sim, hal_strength, &hal_sigma_um);
    /* modify_roi_in()/tiling_callback() already padded for at most
       SF_HALATION_FIRST_SIGMA_UM (see _max_halo_sigma); clamp so a future
       pack entry larger than that can't under-pad the halo. */
    hal_sigma_um = fmin(hal_sigma_um, (double)SF_HALATION_FIRST_SIGMA_UM);
    sf_halation(plane, w, h, (double)pixel_um, d->p.scatter_amount, d->p.scatter_scale,
               d->p.halation_amount, d->p.halation_scale, hal_strength, hal_sigma_um);
  }

  /* 3) film development: log exposure, DIR coupler inhibition (the correction
        field diffuses in the emulsion: gaussian, sigma 20 um as in the
        reference), density curves */
  sf_sim_lograw(plane, npix, 3);
  const int couplers = (d->p.couplers_amount > 0.0f);
  if(couplers)
  {
    sf_sim_develop_corr(sim, plane, corr, npix, 3);
    double cdiff_um, ctail_um, ctail_w;
    sf_sim_coupler_diffusion(sim, &cdiff_um, &ctail_um, &ctail_w);
    const float csigma = (float)cdiff_um / fmaxf(pixel_um, 1e-3f);
    if(ctail_w > 0.0)
    {
      /* corr = (1-w)*gauss(corr) + w*exptail(corr); exptail is upstream's
         3-gaussian mixture surrogate (fast_exponential_filter, n=3) */
      const float amp[3] = { SF_EXPTAIL_A0, SF_EXPTAIL_A1, SF_EXPTAIL_A2 };
      const float rat[3] = { SF_EXPTAIL_R0, SF_EXPTAIL_R1, SF_EXPTAIL_R2 };
      const float tail_px = (float)ctail_um / fmaxf(pixel_um, 1e-3f);
      float *mix = dt_alloc_align_float(npix * 3);
      float *tmp = dt_alloc_align_float(npix * 3);
      if(mix && tmp)
      {
        const float wbase = 1.0f - (float)ctail_w;
        memcpy(tmp, corr, sizeof(float) * npix * 3);
        if(csigma > 0.1f) sf_blur_plane3(tmp, w, h, csigma, scratch);
        for(size_t i = 0; i < npix * 3; i++) mix[i] = wbase * tmp[i];
        for(int g3 = 0; g3 < 3; g3++)
        {
          memcpy(tmp, corr, sizeof(float) * npix * 3);
          const float ts = rat[g3] * tail_px;
          if(ts > 0.1f) sf_blur_plane3(tmp, w, h, ts, scratch);
          const float wk = (float)ctail_w * amp[g3];
          for(size_t i = 0; i < npix * 3; i++) mix[i] += wk * tmp[i];
        }
        memcpy(corr, mix, sizeof(float) * npix * 3);
      }
      else if(csigma > 0.1f)
        sf_blur_plane3(corr, w, h, csigma, scratch); /* alloc failed: core only */
      dt_free_align(mix);
      dt_free_align(tmp);
    }
    else if(csigma > 0.1f)
      sf_blur_plane3(corr, w, h, csigma, scratch);
  }
  sf_sim_develop(sim, plane, couplers ? corr : NULL, plane, npix, 3, 3);

  /* 4) grain on the developed CMY film density (delta model + clump blur,
        renormalised so strength is stable across clump sizes) */
  if(d->p.grain_on && d->p.grain_amount > 0.0f)
  {
    float *gbuf = corr; /* corr is free now — reuse as the grain delta buffer */
    const int roi_x = roi_in->x, roi_y = roi_in->y;
    const float amount = d->p.grain_amount;
    const int mono = sf_sim_film_bw(sim); /* B&W: achromatic grain */
    float gdmax[3], grms[3], gunif[3], gdmin[3];
    sf_sim_film_dmax3(sim, gdmax); /* the emulsion's own D-max: slide film
                                      exceeds the colour-negative 2.2 default,
                                      which would bias (tint) dense areas */
    sf_sim_film_grain3(sim, grms, gunif, gdmin); /* per-film catalogue grain
                                      (rms-granularity, uniformity, density
                                      floor) — Portra 400 no longer shares
                                      Tri-X's grain signature */
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(plane, gbuf, gdmax, grms, gunif, gdmin)              \
    firstprivate(w, npix, roi_x, roi_y, amount, mono) schedule(static)
#endif
    for(size_t k = 0; k < npix; k++)
    {
      const int x = (int)(k % (size_t)w), y = (int)(k / (size_t)w);
      sf_grain_delta_dmax(plane + k * 3, amount, gbuf + k * 3, (uint32_t)(x + roi_x),
                          (uint32_t)(y + roi_y), mono, gdmax, gdmin, grms, gunif);
    }
    const float sigma = SF_GRAIN_BLUR_FACTOR * SF_GRAIN_REF_UM
                             * fmaxf(d->p.grain_size, SF_GRAIN_SIZE_MIN) / fmaxf(pixel_um, 1e-3f);
    sf_blur_plane3(gbuf, w, h, sigma, scratch);
    const float renorm = sf_gauss_grain_renorm(fmaxf(sigma, 0.3f));
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(plane, gbuf) firstprivate(npix, renorm)              \
    schedule(static)
#endif
    for(size_t k = 0; k < npix * 3; k++) plane[k] += gbuf[k] * renorm;
  }

  /* 5) print exposure + development (skipped in scan-film mode) */
  if(!d->p.scan_film)
  {
    sf_sim_print_expose(sim, plane, plane, npix, 3, 3);
    if(d->p.print_diffusion_on)
      sf_diffusion_filter(plane, w, h, (double)pixel_um, (int)d->p.print_diffusion_filter_family,
                          d->p.print_diffusion_strength, d->p.print_diffusion_scale,
                          d->p.print_diffusion_warmth);
    sf_sim_print_develop(sim, plane, plane, npix, 3, 3);
  }

  /* 6) scanning: viewing light through the print/film -> XYZ -> work RGB with
        OkLCh gamut compression. Write RGBA + carried alpha, then crop. */
  sf_sim_scan(sim, plane, plane, npix, 3, 3);

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(plane) firstprivate(out, in, w, ow, oh, ox, oy)      \
    schedule(static)
#endif
  for(int y = 0; y < oh; y++)
    for(int x = 0; x < ow; x++)
    {
      const size_t ks = (size_t)(y + oy) * w + (x + ox);
      const float *pl = plane + ks * 3;
      float *o = out + ((size_t)y * ow + x) * 4;
      o[0] = pl[0];
      o[1] = pl[1];
      o[2] = pl[2];
      o[3] = in[ks * 4 + 3];
    }

  dt_free_align(plane);
  dt_free_align(corr);
  dt_free_align(scratch);
}

#ifdef HAVE_OPENCL
/* GPU path: mirrors process(). Per-pixel stages run as kernels on the
   validated float tables from sf_sim_gpu_export() (POCL-checked to ~1e-6 vs
   the CPU engine); the Gaussian blurs (diffusion bank, halation bounces,
   coupler correction diffusion, grain clumps) use this file's own direct
   separable convolution (spektrafilm_gauss_row/col_*c, weights built
   host-side by sf_gauss_kernel_1d -- see spektra_core.c/.h), exactly as the
   CPU path uses sf_blur_plane3. */
int process_cl(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in,
               cl_mem dev_out, const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  dt_iop_spektrafilm_data_t *const d = (dt_iop_spektrafilm_data_t *)piece->data;
  dt_iop_spektrafilm_global_data_t *gd = (dt_iop_spektrafilm_global_data_t *)self->global_data;
  const int devid = piece->pipe->devid;
  const int w = roi_in->width, h = roi_in->height;
  const int ow = roi_out->width, oh = roi_out->height;
  const int ox = roi_out->x - roi_in->x, oy = roi_out->y - roi_in->y;
  const size_t npix = (size_t)w * h;
  cl_int err = DT_OPENCL_DEFAULT_ERROR;
#define SF_CL_STEP(label)                                                                          \
  do                                                                                               \
  {                                                                                                \
    if(err != CL_SUCCESS)                                                                          \
    {                                                                                              \
      dt_print(DT_DEBUG_OPENCL, "[spektrafilm] GPU step FAILED: %s (err=%d)\n", (label),          \
               (int)err);                                                                          \
      goto cleanup;                                                                                \
    }                                                                                              \
  } while(0)

  const dt_iop_order_iccprofile_info_t *const work_profile
      = dt_ioppr_get_pipe_work_profile_info(piece->pipe);
  sf_sim_t *sim = work_profile ? _ensure_sim(d, work_profile) : NULL;
  const sf_sim_gpu_t *g = d->gpu;

  if(!sim) /* no data pack / profiles: crop passthrough */
    return dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_passthrough, ow, oh,
                                            CLARG(dev_in), CLARG(dev_out), CLARG(ow),
                                            CLARG(oh), CLARG(ox), CLARG(oy));
  if(!g) return DT_OPENCL_DEFAULT_ERROR; /* exact quality etc. -> CPU fallback */

  const float full_w = fmaxf((float)piece->buf_in.width * roi_in->scale, 1.0f);
  const float pixel_um = d->p.film_format_mm * 1000.0f / full_w;

  /* ---- table uploads (read-only buffers) -------------------------------- */
  /* packed matrix block: layout must match the SF_M_* offsets in the .cl */
  float mats[93]; /* SF_M_* layout in spektrafilm.cl */
  memcpy(mats + 0, g->m_in, 9 * sizeof(float));
  memcpy(mats + 9, g->m_out, 9 * sizeof(float));
  memcpy(mats + 18, g->couplers_M, 9 * sizeof(float));
  memcpy(mats + 27, g->out_rgb2xyz, 9 * sizeof(float));
  memcpy(mats + 36, g->out_xyz2rgb, 9 * sizeof(float));
  memcpy(mats + 45, g->oklab_m1, 9 * sizeof(float));
  memcpy(mats + 54, g->oklab_m2, 9 * sizeof(float));
  memcpy(mats + 63, g->oklab_m1inv, 9 * sizeof(float));
  memcpy(mats + 72, g->oklab_m2inv, 9 * sizeof(float));
  memcpy(mats + 81, g->couplers_donor_K, 3 * sizeof(float));
  memcpy(mats + 84, g->couplers_donor_Dref, 3 * sizeof(float));
  memcpy(mats + 87, g->couplers_recv_Kr, 3 * sizeof(float));
  memcpy(mats + 90, g->couplers_recv_cref, 3 * sizeof(float));

  const int steps = g->steps;
  const size_t n3 = (size_t)steps * steps * steps * 3;
  const size_t m3 = (size_t)(steps - 1) * (steps - 1) * (steps - 1) * 3;
  const size_t f = sizeof(float);
  cl_mem mats_cl = dt_opencl_copy_host_to_device_constant(devid, 93 * f, mats);
  cl_mem tc_cl = dt_opencl_copy_host_to_device_constant(
      devid, (size_t)g->tc_n * g->tc_n * 3 * f, g->tc_lut);
  cl_mem cn_cl = dt_opencl_copy_host_to_device_constant(devid, 256 * 3 * f, g->curves_norm);
  cl_mem cb_cl = dt_opencl_copy_host_to_device_constant(devid, 256 * 3 * f,
                                                        g->couplers_active ? g->curves_before
                                                                           : g->curves_norm);
  cl_mem el_cl = NULL, ex_cl = NULL, ey_cl = NULL, ez_cl = NULL, en_cl = NULL, em_cl = NULL;
  cl_mem pc_cl = NULL;
  if(g->has_print)
  {
    el_cl = dt_opencl_copy_host_to_device_constant(devid, n3 * f, g->enl_lut);
    ex_cl = dt_opencl_copy_host_to_device_constant(devid, n3 * f, g->enl_sx);
    ey_cl = dt_opencl_copy_host_to_device_constant(devid, n3 * f, g->enl_sy);
    ez_cl = dt_opencl_copy_host_to_device_constant(devid, n3 * f, g->enl_sz);
    en_cl = dt_opencl_copy_host_to_device_constant(devid, m3 * f, g->enl_cmin);
    em_cl = dt_opencl_copy_host_to_device_constant(devid, m3 * f, g->enl_cmax);
    pc_cl = dt_opencl_copy_host_to_device_constant(devid, 256 * 3 * f, g->print_curves);
  }
  cl_mem sl_cl = dt_opencl_copy_host_to_device_constant(devid, n3 * f, g->scan_lut);
  cl_mem sx_cl = dt_opencl_copy_host_to_device_constant(devid, n3 * f, g->scan_sx);
  cl_mem sy_cl = dt_opencl_copy_host_to_device_constant(devid, n3 * f, g->scan_sy);
  cl_mem sz_cl = dt_opencl_copy_host_to_device_constant(devid, n3 * f, g->scan_sz);
  cl_mem sn_cl = dt_opencl_copy_host_to_device_constant(devid, m3 * f, g->scan_cmin);
  cl_mem sm_cl = dt_opencl_copy_host_to_device_constant(devid, m3 * f, g->scan_cmax);
  /* cmax_table is only used in oklch mode but the kernel arg must be valid */
  cl_mem cm_cl = dt_opencl_copy_host_to_device_constant(
      devid, (g->cmax_table ? (size_t)g->cmax_nl * g->cmax_nh : 1) * f,
      g->cmax_table ? (void *)g->cmax_table : (void *)mats);

  cl_mem plane = dt_opencl_alloc_device_buffer(devid, npix * f * 4);
  cl_mem plane2 = dt_opencl_alloc_device_buffer(devid, npix * f * 4);
  cl_mem tmpa = dt_opencl_alloc_device_buffer(devid, npix * f * 4);
  cl_mem acc = dt_opencl_alloc_device_buffer(devid, npix * f * 4);
  /* single-channel scratch for the scatter stage's genuinely per-channel
     blurs (spektrafilm_channel_extract + a 1ch Gaussian): 1/4 the size and
     1/4 the per-blur cost of running the equivalent work on a float4
     buffer, see the scatter stage below. */
  cl_mem plane1 = dt_opencl_alloc_device_buffer(devid, npix * f);
  /* row-pass intermediates for the direct (exact) separable convolution
     below: dedicated buffers, distinct from every buffer a blur might be
     called on in place, so the row pass never aliases its own input. */
  cl_mem gtmp4 = dt_opencl_alloc_device_buffer(devid, npix * f * 4);
  cl_mem gtmp1 = dt_opencl_alloc_device_buffer(devid, npix * f);
  /* kernel weights (2*SF_GAUSS_MAX_RADIUS+1 taps, built host-side by
     sf_gauss_kernel_1d and rewritten before each blur dispatch below) */
  cl_mem gauss_w = dt_opencl_alloc_device_buffer(devid, sizeof(float) * (2 * SF_GAUSS_MAX_RADIUS + 1));
  if(!mats_cl || !tc_cl || !cn_cl || !cb_cl || !sl_cl || !sx_cl || !sy_cl || !sz_cl || !sn_cl
     || !sm_cl || !cm_cl || !plane || !plane2 || !tmpa || !acc || !plane1 || !gtmp4 || !gtmp1
     || !gauss_w
     || (g->has_print && (!el_cl || !ex_cl || !ey_cl || !ez_cl || !en_cl || !em_cl || !pc_cl)))
  {
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }
/* Direct (exact) separable Gaussian blur: builds the exact kernel
   host-side (the same sf_gauss_kernel_1d() the CPU path convolves with),
   uploads it, then dispatches a row pass into a dedicated scratch buffer
   followed by a col pass into `dst` -- safe even when dst==buf (in-place),
   since the row pass fully consumes buf into scratch before the col pass
   writes buf. No sigma-correction factor: unlike a recursive/IIR
   approximation, a direct truncated kernel has no sigma-dependent error to
   correct for in the first place. */
#define SF_GAUSS_BLUR4(buf, _sg, label) do { \
    if(err == CL_SUCCESS) \
    { \
      float _kw[2 * SF_GAUSS_MAX_RADIUS + 1]; \
      const int _kr = sf_gauss_kernel_1d((_sg), _kw, SF_GAUSS_MAX_RADIUS); \
      err = dt_opencl_write_buffer_to_device(devid, _kw, gauss_w, 0, \
                                             sizeof(float) * (2 * _kr + 1), TRUE); \
      if(err == CL_SUCCESS) \
        err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_gauss_row_4c, w, h, \
                                               CLARG(buf), CLARG(gtmp4), CLARG(w), CLARG(h), \
                                               CLARG(gauss_w), CLARG(_kr)); \
      if(err == CL_SUCCESS) \
        err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_gauss_col_4c, w, h, \
                                               CLARG(gtmp4), CLARG(buf), CLARG(w), CLARG(h), \
                                               CLARG(gauss_w), CLARG(_kr)); \
    } \
    SF_CL_STEP(label); \
  } while(0)
/* loop-safe variant: sets err, caller checks err/breaks; src/dst may differ
   (e.g. accumulating several blurred copies of the same source). */
#define SF_GAUSS_BLUR4_OP_L(src, dst, _sg) do { \
    if(err == CL_SUCCESS) \
    { \
      float _kw[2 * SF_GAUSS_MAX_RADIUS + 1]; \
      const int _kr = sf_gauss_kernel_1d((_sg), _kw, SF_GAUSS_MAX_RADIUS); \
      err = dt_opencl_write_buffer_to_device(devid, _kw, gauss_w, 0, \
                                             sizeof(float) * (2 * _kr + 1), TRUE); \
      if(err == CL_SUCCESS) \
        err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_gauss_row_4c, w, h, \
                                               CLARG(src), CLARG(gtmp4), CLARG(w), CLARG(h), \
                                               CLARG(gauss_w), CLARG(_kr)); \
      if(err == CL_SUCCESS) \
        err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_gauss_col_4c, w, h, \
                                               CLARG(gtmp4), CLARG(dst), CLARG(w), CLARG(h), \
                                               CLARG(gauss_w), CLARG(_kr)); \
    } \
  } while(0)
/* single-channel in-place blur (scatter stage only, on plane1) */
#define SF_GAUSS_BLUR1_L(buf, _sg) do { \
    if(err == CL_SUCCESS) \
    { \
      float _kw[2 * SF_GAUSS_MAX_RADIUS + 1]; \
      const int _kr = sf_gauss_kernel_1d((_sg), _kw, SF_GAUSS_MAX_RADIUS); \
      err = dt_opencl_write_buffer_to_device(devid, _kw, gauss_w, 0, \
                                             sizeof(float) * (2 * _kr + 1), TRUE); \
      if(err == CL_SUCCESS) \
        err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_gauss_row_1c, w, h, \
                                               CLARG(buf), CLARG(gtmp1), CLARG(w), CLARG(h), \
                                               CLARG(gauss_w), CLARG(_kr)); \
      if(err == CL_SUCCESS) \
        err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_gauss_col_1c, w, h, \
                                               CLARG(gtmp1), CLARG(buf), CLARG(w), CLARG(h), \
                                               CLARG(gauss_w), CLARG(_kr)); \
    } \
  } while(0)

  /* ---- 1) expose: input image -> linear film raw exposure ---------------- */
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_expose, w, h, CLARG(dev_in),
                                         CLARG(plane), CLARG(w), CLARG(h), CLARG(mats_cl),
                                         CLARG(tc_cl), CLARG(g->tc_n), CLARG(g->ev_scale));
  SF_CL_STEP("expose");

  /* ---- 2) pre-film spatial effects on linear exposure -------------------- */
  if(d->p.boost_ev > 0.0f)
  {
    const int npartials = 256;
    cl_mem partials = dt_opencl_alloc_device_buffer(devid, npartials * sizeof(float));
    cl_mem maxv_buf = dt_opencl_alloc_device_buffer(devid, sizeof(float));
    if(partials && maxv_buf)
    {
      const int npix_i = (int)npix;
      err = dt_opencl_enqueue_kernel_1d_args(devid, gd->kernel_max_partials, npartials,
                                             CLARG(plane), CLARG(npix_i), CLARG(partials),
                                             CLARG(npartials));
      if(err == CL_SUCCESS)
        err = dt_opencl_enqueue_kernel_1d_args(devid, gd->kernel_max_reduce, 1, CLARG(partials),
                                               CLARG(maxv_buf), CLARG(npartials));
      if(err == CL_SUCCESS)
      {
        const float b_ev = d->p.boost_ev, b_rng = d->p.boost_range, b_prot = d->p.protect_ev;
        err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_boost, w, h, CLARG(plane),
                                               CLARG(w), CLARG(h), CLARG(b_ev), CLARG(b_rng),
                                               CLARG(b_prot), CLARG(maxv_buf));
      }
      SF_CL_STEP("boost");
    }
    dt_opencl_release_mem_object(partials);
    dt_opencl_release_mem_object(maxv_buf);
  }

  if(d->p.diffusion_on)
  {
    sf_diffusion_plan_t plan;
    if(sf_diffusion_build_plan((int)d->p.diffusion_filter_family, d->p.diffusion_strength,
                               d->p.diffusion_warmth, &plan)
       && plan.p_s > 0.0f)
    {
      const float dsc = fmaxf(d->p.diffusion_scale, 1e-6f);
      for(int j = 0; j < plan.n; j++)
      {
        const float sigma = fmaxf(plan.sigma_um[j] * dsc / pixel_um, 1e-3f);
        SF_GAUSS_BLUR4_OP_L(plane, tmpa, sigma);
        if(err != CL_SUCCESS) break;
        const int reset = (j == 0);
        const float wr = plan.wr[j], wg = plan.wg[j], wb = plan.wb[j];
        err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_diffusion_accum, w, h,
                                               CLARG(tmpa), CLARG(acc), CLARG(w), CLARG(h),
                                               CLARG(wr), CLARG(wg), CLARG(wb), CLARG(reset));
        if(err != CL_SUCCESS) break;
      }
      if(err == CL_SUCCESS)
      {
        const float ps = plan.p_s;
        err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_diffusion_mix, w, h,
                                               CLARG(plane), CLARG(acc), CLARG(w), CLARG(h),
                                               CLARG(ps));
      }
      SF_CL_STEP("diffusion");
    }
  }

  if(d->p.halation_on && (d->p.scatter_amount > 0.0f || d->p.halation_amount > 0.0f))
  {
    if(d->p.scatter_amount > 0.0f)
    {
      const float sscl = fmaxf(d->p.scatter_scale, 1e-3f);
      /* per-channel scatter radii (um on film) and tail mixture, identical to
         spektra_core.c's sf_halation() sc_core/sc_tail/tail_amp/tail_rat.
         Each channel needs its OWN sigma (R/G/B differ): extract that
         channel into the single-channel scratch buffer plane1, blur it
         alone (1x the work of a same-size float4 blur, not 4x), then
         kernel_channel_accum folds it into the target channel of tmpa/acc. */
      const float sc_core[3] = { 2.2f, 2.0f, 1.6f };
      const float sc_tail[3] = { 9.3f, 9.7f, 9.1f };
      const float amp[3] = { 0.1633f, 0.6496f, 0.1870f }, rat[3] = { 0.5360f, 1.5236f, 2.7684f };
      for(int c = 0; c < 3; c++)
      {
        err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_channel_extract, w, h,
                                               CLARG(plane), CLARG(plane1), CLARG(w), CLARG(h),
                                               CLARG(c));
        if(err != CL_SUCCESS) break;
        SF_GAUSS_BLUR1_L(plane1, fmaxf(sc_core[c] * sscl / pixel_um, 1e-6f));
        if(err != CL_SUCCESS) break;
        const float core_weight = 1.0f;
        const int core_reset = (c == 0);
        err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_channel_accum, w, h,
                                               CLARG(plane1), CLARG(tmpa), CLARG(w), CLARG(h),
                                               CLARG(core_weight), CLARG(c), CLARG(core_reset));
        SF_CL_STEP("scatter core blur");
      }
      for(int g3 = 0; g3 < 3 && err == CL_SUCCESS; g3++)
        for(int c = 0; c < 3; c++)
        {
          err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_channel_extract, w, h,
                                                 CLARG(plane), CLARG(plane1), CLARG(w), CLARG(h),
                                                 CLARG(c));
          if(err != CL_SUCCESS) break;
          const float sigma = fmaxf(rat[g3] * sc_tail[c] * sscl / pixel_um, 1e-6f);
          SF_GAUSS_BLUR1_L(plane1, sigma);
          if(err != CL_SUCCESS) break;
          const int reset = (g3 == 0 && c == 0);
          err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_channel_accum, w, h,
                                                 CLARG(plane1), CLARG(acc), CLARG(w), CLARG(h),
                                                 CLARG(amp[g3]), CLARG(c), CLARG(reset));
          SF_CL_STEP("scatter tail accum");
        }
      const float ws_r = 0.78f, ws_g = 0.65f, ws_b = 0.67f;
      /* (1-s)*raw + s*scattered, matching sf_halation()'s CPU blend; `plane`
         doubles as both the pre-scatter `raw` input and the `out` write
         target -- safe since this is a purely per-pixel elementwise op. */
      const float s_amount = d->p.scatter_amount;
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_scatter_combine, w, h, CLARG(plane),
                                             CLARG(tmpa), CLARG(acc), CLARG(plane), CLARG(w),
                                             CLARG(h), CLARG(s_amount), CLARG(ws_r), CLARG(ws_g),
                                             CLARG(ws_b));
      SF_CL_STEP("scatter combine");
    }

    if(d->p.halation_amount > 0.0f)
    {
      const float hscl = fmaxf(d->p.halation_scale, 1e-3f);
      const int N = 3;
      /* per-film first-bounce radius (still ~65um / cine ~50um on real
         stocks); clamped to what modify_roi_in()/tiling_callback() padded
         for, see the matching comment in process(). */
      const float first_sigma = fminf(g->halation_first_sigma_um, SF_HALATION_FIRST_SIGMA_UM);
      const float dec[3] = { 1.0f/1.75f, 0.5f/1.75f, 0.25f/1.75f };
      for(int k = 1; k <= N; k++)
      {
        SF_GAUSS_BLUR4_OP_L(plane, plane2, fmaxf(first_sigma * hscl * sqrtf((float)k) / pixel_um, 1e-3f));
        if(err != CL_SUCCESS) break;
        const int reset = (k == 1);
        err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_accum, w, h, CLARG(plane2),
                                               CLARG(acc), CLARG(w), CLARG(h), CLARG(dec[k - 1]),
                                               CLARG(reset));
        SF_CL_STEP("halation bounce accum");
      }
      /* halation_amount is a direct linear multiplier on strength, matching
         upstream's a_tot = halation_strength * halation_amount (no curve). */
      const float h_eff = d->p.halation_amount;
      /* per-film halation strength (e.g. a strong-AH stock stays near-zero on
         blue and much lower on red/green than a no-AH/redscale stock). */
      const float a_r = g->halation_strength[0] * h_eff, a_g = g->halation_strength[1] * h_eff,
                  a_b = g->halation_strength[2] * h_eff;
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_halation_apply, w, h, CLARG(plane),
                                             CLARG(acc), CLARG(w), CLARG(h), CLARG(a_r),
                                             CLARG(a_g), CLARG(a_b));
      SF_CL_STEP("halation apply");
    }
  }

  /* ---- 3) film development ------------------------------------------------ */
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_lograw, w, h, CLARG(plane), CLARG(w),
                                         CLARG(h));
  SF_CL_STEP("lograw");

  const int use_corr = g->couplers_active;
  if(use_corr)
  {
    err = dt_opencl_enqueue_kernel_2d_args(
        devid, gd->kernel_develop_corr, w, h, CLARG(plane), CLARG(acc), CLARG(w), CLARG(h),
        CLARG(cn_cl), CLARG(mats_cl), CLARG(g->gamma[0]), CLARG(g->gamma[1]), CLARG(g->gamma[2]),
        CLARG(g->le0), CLARG(g->le_step), CLARG(g->film_dmax[0]), CLARG(g->film_dmax[1]),
        CLARG(g->film_dmax[2]), CLARG(g->film_positive));
    SF_CL_STEP("develop_corr");
    /* DIR coupler inhibitor diffusion, gaussian sigma 20 um (reference value) */
    const float csigma = g->coupler_diff_um / fmaxf(pixel_um, 1e-3f);
    if(g->coupler_tail_w > 0.0f)
    {
      const float amp[4] = { 1.0f - g->coupler_tail_w, g->coupler_tail_w * SF_EXPTAIL_A0,
                             g->coupler_tail_w * SF_EXPTAIL_A1, g->coupler_tail_w * SF_EXPTAIL_A2 };
      const float sig[4] = { csigma, SF_EXPTAIL_R0 * g->coupler_tail_um / fmaxf(pixel_um, 1e-3f),
                             SF_EXPTAIL_R1 * g->coupler_tail_um / fmaxf(pixel_um, 1e-3f),
                             SF_EXPTAIL_R2 * g->coupler_tail_um / fmaxf(pixel_um, 1e-3f) };
      for(int g3 = 0; g3 < 4; g3++)
      {
        if(sig[g3] > 0.1f)
        {
          SF_GAUSS_BLUR4_OP_L(acc, plane2, sig[g3]);
          if(err != CL_SUCCESS) break;
        }
        else
        {
          err = dt_opencl_enqueue_copy_buffer_to_buffer(devid, acc, plane2, 0, 0, npix * f * 4);
          if(err != CL_SUCCESS) break;
        }
        const int reset = (g3 == 0);
        err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_diffusion_accum, w, h,
                                               CLARG(plane2), CLARG(tmpa), CLARG(w), CLARG(h),
                                               CLARG(amp[g3]), CLARG(amp[g3]), CLARG(amp[g3]),
                                               CLARG(reset));
        SF_CL_STEP("coupler tail accum");
      }
    }
    else if(csigma > 0.1f)
      SF_GAUSS_BLUR4(acc, csigma, "coupler blur");
  }
  cl_mem corr_buf = (g->coupler_tail_w > 0.0f) ? tmpa : acc;
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_develop, w, h, CLARG(plane),
                                         CLARG(corr_buf), CLARG(use_corr), CLARG(plane2), CLARG(w),
                                         CLARG(h), CLARG(cb_cl), CLARG(mats_cl), CLARG(g->gamma[0]),
                                         CLARG(g->gamma[1]), CLARG(g->gamma[2]), CLARG(g->le0),
                                         CLARG(g->le_step));
  SF_CL_STEP("develop");

  /* ---- 4) grain on the developed CMY density ----------------------------- */
  if(d->p.grain_on && d->p.grain_amount > 0.0f)
  {
    const int roi_x = roi_in->x, roi_y = roi_in->y;
    const float amount = d->p.grain_amount;
    const int mono = g->film_bw; /* B&W: achromatic grain */
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_grain_gen, w, h, CLARG(plane2),
                                           CLARG(tmpa), CLARG(w), CLARG(h), CLARG(amount),
                                           CLARG(roi_x), CLARG(roi_y), CLARG(mono),
                                           CLARG(g->film_dmax[0]), CLARG(g->film_dmax[1]),
                                           CLARG(g->film_dmax[2]), CLARG(g->grain_dmin[0]),
                                           CLARG(g->grain_dmin[1]), CLARG(g->grain_dmin[2]),
                                           CLARG(g->grain_rms[0]), CLARG(g->grain_rms[1]),
                                           CLARG(g->grain_rms[2]), CLARG(g->grain_uniformity[0]),
                                           CLARG(g->grain_uniformity[1]),
                                           CLARG(g->grain_uniformity[2]));
    SF_CL_STEP("grain gen");
    const float gsigma = SF_GRAIN_BLUR_FACTOR * SF_GRAIN_REF_UM
                              * fmaxf(d->p.grain_size, SF_GRAIN_SIZE_MIN) / fmaxf(pixel_um, 1e-3f);
    SF_GAUSS_BLUR4(tmpa, gsigma, "grain blur");
    const float grenorm = sf_gauss_grain_renorm(fmaxf(gsigma, 0.3f));
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_grain_add, w, h, CLARG(plane2),
                                           CLARG(tmpa), CLARG(w), CLARG(h), CLARG(grenorm));
    SF_CL_STEP("grain add");
  }

  /* ---- 5) print ----------------------------------------------------------- */
  if(g->has_print)
  {
    err = dt_opencl_enqueue_kernel_2d_args(
        devid, gd->kernel_print_expose, w, h, CLARG(plane2), CLARG(plane), CLARG(w), CLARG(h),
        CLARG(el_cl), CLARG(ex_cl), CLARG(ey_cl), CLARG(ez_cl), CLARG(en_cl), CLARG(em_cl),
        CLARG(steps), CLARG(g->enl_lo[0]), CLARG(g->enl_lo[1]), CLARG(g->enl_lo[2]),
        CLARG(g->enl_hi[0]), CLARG(g->enl_hi[1]), CLARG(g->enl_hi[2]), CLARG(g->print_exposure));
    SF_CL_STEP("print_expose");
    /* ---- print diffusion (optional, on the exposed print density) ---- */
    if(d->p.print_diffusion_on)
    {
      sf_diffusion_plan_t pplan;
      if(sf_diffusion_build_plan((int)d->p.print_diffusion_filter_family,
                                 d->p.print_diffusion_strength,
                                 d->p.print_diffusion_warmth, &pplan)
         && pplan.p_s > 0.0f)
      {
        const float pdsc = fmaxf(d->p.print_diffusion_scale, 1e-6f);
        for(int j = 0; j < pplan.n; j++)
        {
          const float sigma = fmaxf(pplan.sigma_um[j] * pdsc / pixel_um, 1e-3f);
          SF_GAUSS_BLUR4_OP_L(plane, tmpa, sigma);
          if(err != CL_SUCCESS) break;
          const int reset = (j == 0);
          const float wr = pplan.wr[j], wg = pplan.wg[j], wb = pplan.wb[j];
          err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_diffusion_accum, w, h,
                                                  CLARG(tmpa), CLARG(acc), CLARG(w), CLARG(h),
                                                  CLARG(wr), CLARG(wg), CLARG(wb), CLARG(reset));
          if(err != CL_SUCCESS) break;
        }
        if(err == CL_SUCCESS)
        {
          const float ps = pplan.p_s;
          err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_diffusion_mix, w, h,
                                                  CLARG(plane), CLARG(acc), CLARG(w), CLARG(h),
                                                  CLARG(ps));
        }
        SF_CL_STEP("print_diffusion");
      }
    }
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_print_develop, w, h, CLARG(plane),
                                           CLARG(plane2), CLARG(w), CLARG(h), CLARG(pc_cl),
                                           CLARG(g->le0), CLARG(g->le_step));
    SF_CL_STEP("print_develop");
  }

  /* ---- 6) scan: crop the roi_out window straight into dev_out ------------- */
  err = dt_opencl_enqueue_kernel_2d_args(
      devid, gd->kernel_scan, ow, oh, CLARG(plane2), CLARG(dev_in), CLARG(dev_out), CLARG(w),
      CLARG(ow), CLARG(oh), CLARG(ox), CLARG(oy), CLARG(sl_cl), CLARG(sx_cl), CLARG(sy_cl),
      CLARG(sz_cl), CLARG(sn_cl), CLARG(sm_cl), CLARG(steps), CLARG(g->scan_lo[0]),
      CLARG(g->scan_lo[1]), CLARG(g->scan_lo[2]), CLARG(g->scan_hi[0]), CLARG(g->scan_hi[1]),
      CLARG(g->scan_hi[2]), CLARG(mats_cl), CLARG(cm_cl), CLARG(g->cmax_nl), CLARG(g->cmax_nh),
      CLARG(g->out_compress), CLARG(g->out_luminance_boost), CLARG(g->scan_bw_on), CLARG(g->scan_bw_m),
      CLARG(g->scan_bw_q));
  SF_CL_STEP("scan");

cleanup:
  dt_opencl_release_mem_object(mats_cl);
  dt_opencl_release_mem_object(tc_cl);
  dt_opencl_release_mem_object(cn_cl);
  dt_opencl_release_mem_object(cb_cl);
  dt_opencl_release_mem_object(el_cl);
  dt_opencl_release_mem_object(ex_cl);
  dt_opencl_release_mem_object(ey_cl);
  dt_opencl_release_mem_object(ez_cl);
  dt_opencl_release_mem_object(en_cl);
  dt_opencl_release_mem_object(em_cl);
  dt_opencl_release_mem_object(pc_cl);
  dt_opencl_release_mem_object(sl_cl);
  dt_opencl_release_mem_object(sx_cl);
  dt_opencl_release_mem_object(sy_cl);
  dt_opencl_release_mem_object(sz_cl);
  dt_opencl_release_mem_object(sn_cl);
  dt_opencl_release_mem_object(sm_cl);
  dt_opencl_release_mem_object(cm_cl);
  dt_opencl_release_mem_object(plane);
  dt_opencl_release_mem_object(plane2);
  dt_opencl_release_mem_object(tmpa);
  dt_opencl_release_mem_object(acc);
  dt_opencl_release_mem_object(plane1);
  dt_opencl_release_mem_object(gtmp4);
  dt_opencl_release_mem_object(gtmp1);
  dt_opencl_release_mem_object(gauss_w);
  return err;
}
#endif /* HAVE_OPENCL */

/* ---------------------------------------------------------------------- */
/* GUI                                                                    */
/* ---------------------------------------------------------------------- */

static void _rescan(dt_iop_module_t *self)
{
  dt_iop_spektrafilm_gui_data_t *g = (dt_iop_spektrafilm_gui_data_t *)self->gui_data;
  g->n_entries = sf_scan_profiles(g->entries, SF_MAX_PROFILES);
  g->n_films = g->n_papers = 0;
  for(int i = 0; i < g->n_entries; i++)
  {
    if(g->entries[i].printing)
      g->paper_entry[g->n_papers++] = i;
    else
      g->film_entry[g->n_films++] = i;
  }
}

static void _update_print_sensitivity(dt_iop_module_t *self);

static void _film_changed(GtkWidget *w, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_spektrafilm_gui_data_t *g = (dt_iop_spektrafilm_gui_data_t *)self->gui_data;
  dt_iop_spektrafilm_params_t *p = (dt_iop_spektrafilm_params_t *)self->params;
  const int fi = GPOINTER_TO_INT(dt_bauhaus_combobox_get_data(g->film));
  if(fi < 0) return;
  const sf_prof_entry_t *e = &g->entries[fi];
  p->film_hash = e->hash;
  /* Keep the "default" scan_film following this film's own positive/negative
     type too, not just the live params -- so a double-click reset (which
     resets to self->default_params, whether via darktable's own bauhaus
     reset or a checkbox-reset mechanism for this field) means "what this
     film actually needs" rather than the module's one-size-fits-all factory
     default (FALSE). Without this, resetting scan_film on a positive/
     reversal film would silently break it, since that film has no print
     stage at all. */
  if(self->default_params)
    ((dt_iop_spektrafilm_params_t *)self->default_params)->scan_film = e->positive;
  /* The core checkbox-reset mechanism (darktable-core-toggle-reset.patch)
     doesn't re-read self->default_params live at reset time -- it captures
     each checkbox's default once, at widget-creation time, as opaque
     "dt-toggle-default" data on the button itself. So the update above
     alone doesn't reach it; poke the widget's own cached value too,
     otherwise a reset still uses whatever default_params->scan_film was
     when the module GUI first built (before any film was ever selected). */
  if(g->scan_film)
    g_object_set_data(G_OBJECT(g->scan_film), "dt-toggle-default", GINT_TO_POINTER(e->positive));
  /* scan-film follows the film's natural mode on a film switch: slides and
     reversal stocks are viewed directly (scan), negatives go through the
     print stage. The user can still toggle freely afterwards -- this only
     re-baselines when the film itself changes, like the paper auto-follow. */
  if(p->scan_film != e->positive)
  {
    p->scan_film = e->positive;
    ++darktable.gui->reset;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->scan_film), p->scan_film);
    --darktable.gui->reset;
    _update_print_sensitivity(self);
  }
  /* if the paper is still on "auto" (hash 0) keep it following the film's
     target print; otherwise leave the explicit user choice alone */
  if(p->paper_hash == 0 && e->target_print[0])
    for(int k = 0; k < g->n_papers; k++)
      if(!strcmp(g->entries[g->paper_entry[k]].stock, e->target_print))
      {
        ++darktable.gui->reset;
        dt_bauhaus_combobox_set_from_value(g->paper, g->paper_entry[k]);
        --darktable.gui->reset;
        break;
      }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _paper_changed(GtkWidget *w, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_spektrafilm_gui_data_t *g = (dt_iop_spektrafilm_gui_data_t *)self->gui_data;
  dt_iop_spektrafilm_params_t *p = (dt_iop_spektrafilm_params_t *)self->params;
  const int pi = GPOINTER_TO_INT(dt_bauhaus_combobox_get_data(g->paper));
  if(pi < 0) return;
  p->paper_hash = g->entries[pi].hash;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _update_print_sensitivity(dt_iop_module_t *self)
{
  dt_iop_spektrafilm_gui_data_t *g = (dt_iop_spektrafilm_gui_data_t *)self->gui_data;
  dt_iop_spektrafilm_params_t *p = (dt_iop_spektrafilm_params_t *)self->params;
  const gboolean printing = !p->scan_film;
  gtk_widget_set_sensitive(g->paper, printing);
  gtk_widget_set_sensitive(g->print_exposure_ev, printing);
  gtk_widget_set_sensitive(g->print_auto_exposure, printing);
  gtk_widget_set_sensitive(g->print_contrast, printing);
  gtk_widget_set_sensitive(g->filter_m, printing);
  gtk_widget_set_sensitive(g->filter_y, printing);
  gtk_widget_set_sensitive(g->print_diffusion_on, printing);
  gtk_widget_set_sensitive(g->print_diffusion_filter_family, printing);
  gtk_widget_set_sensitive(g->print_diffusion_strength, printing);
  gtk_widget_set_sensitive(g->print_diffusion_scale, printing);
  gtk_widget_set_sensitive(g->print_diffusion_warmth, printing);
  /* toggle_from_params checkboxes keep showing their tick even when made
     insensitive -- GTK just dims the whole widget, so a checked-but-grayed
     box can read as "this is still on" when it has no effect at all (no
     print stage on positive/reversal film). Blank the tick while
     insensitive and restore the real value once re-enabled. Wrapped in
     DT_ENTER/LEAVE_GUI_UPDATE -- the same guard dt_iop_gui_update's own
     programmatic widget syncs rely on -- so this is purely visual and
     never writes back into the param. */
  DT_ENTER_GUI_UPDATE();
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->print_auto_exposure),
                                printing && p->print_auto_exposure);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->print_diffusion_on),
                                printing && p->print_diffusion_on);
  DT_LEAVE_GUI_UPDATE();
}

/* called by the core whenever a params-linked widget changed */
void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_spektrafilm_gui_data_t *g = (dt_iop_spektrafilm_gui_data_t *)self->gui_data;
  dt_iop_spektrafilm_params_t *p = (dt_iop_spektrafilm_params_t *)self->params;
  if(!w || w == g->scan_film) _update_print_sensitivity(self);
  if(w == g->print_auto_exposure && !*(gboolean *)previous && p->print_auto_exposure)
  {
    /* print_exposure_ev (manual) and print_auto_exposure (automatic) are
       independent, always-additive factors -- matching the reference app's
       own architecture (raw *= exposure_factor; raw *= enlarger.print_exposure,
       two separate multiplications) rather than a mutually-exclusive pair.
       Left alone, re-enabling auto stacks on top of whatever manual EV was
       dialed in while it was off, which reads as "auto exposure is now
       offset by the old manual value". Reset the manual slider on OFF->ON
       so re-enabling auto gives a clean auto result to fine-tune from. */
    p->print_exposure_ev = 0.0f;
    dt_bauhaus_slider_set(g->print_exposure_ev, 0.0f);
  }
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_spektrafilm_gui_data_t *g = (dt_iop_spektrafilm_gui_data_t *)self->gui_data;
  dt_iop_spektrafilm_params_t *p = (dt_iop_spektrafilm_params_t *)self->params;

  _rescan(self);
  dt_bauhaus_combobox_clear(g->film);
  if(g->n_films == 0)
    dt_bauhaus_combobox_add(g->film, _("(no profiles found)"));
  else
  {
    static const struct { int pos; int bw; const char *label; } groups[] = {
      { 0, 0, N_("negative color") },
      { 1, 0, N_("positive color") },
      { 0, 1, N_("negative monochrome") },
      { 1, 1, N_("positive monochrome") },
    };
    for(int gi = 0; gi < 4; gi++)
    {
      gboolean first = TRUE;
      for(int f = 0; f < g->n_films; f++)
      {
        const sf_prof_entry_t *e = &g->entries[g->film_entry[f]];
        if(e->positive != groups[gi].pos || e->bw != groups[gi].bw) continue;
        if(first) { dt_bauhaus_combobox_add_section(g->film, _(groups[gi].label)); first = FALSE; }
        dt_bauhaus_combobox_add_full(g->film, e->name,
                                     DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,
                                     GINT_TO_POINTER(g->film_entry[f]),
                                     NULL, TRUE);
      }
    }
  }
  dt_bauhaus_combobox_clear(g->paper);
  if(g->n_papers == 0)
    dt_bauhaus_combobox_add(g->paper, _("(none)"));
  else
  {
    static const struct { int bw; const char *label; } pgroups[] = {
      { 0, N_("color") },
      { 1, N_("monochrome") },
    };
    for(int gi = 0; gi < 2; gi++)
    {
      gboolean first = TRUE;
      for(int k = 0; k < g->n_papers; k++)
      {
        const sf_prof_entry_t *e = &g->entries[g->paper_entry[k]];
        if(e->bw != pgroups[gi].bw) continue;
        if(first) { dt_bauhaus_combobox_add_section(g->paper, _(pgroups[gi].label)); first = FALSE; }
        dt_bauhaus_combobox_add_full(g->paper, e->name,
                                     DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,
                                     GINT_TO_POINTER(g->paper_entry[k]),
                                     NULL, TRUE);
      }
    }
  }

  int fi = 0;
  gboolean film_matched = FALSE;
  for(int f = 0; f < g->n_films; f++)
    if(g->entries[g->film_entry[f]].hash == p->film_hash) { fi = f; film_matched = TRUE; }
  if(!film_matched)
  {
    /* no hash match (fresh param with film_hash==0, or the saved stock
       vanished from the pack) -- mirror sf_resolve_stock's fallback so the
       combobox agrees with what the pixel pipeline actually renders, instead
       of silently landing on whatever sorts first (e.g. "Fujifilm C200"
       alphabetically before "Kodak Portra 400") while the pipe renders the
       real default. */
    for(int f = 0; f < g->n_films; f++)
      if(!strcmp(g->entries[g->film_entry[f]].stock, "kodak_portra_400")) fi = f;
  }
  dt_bauhaus_combobox_set_from_value(g->film, g->film_entry[fi]);
  /* _film_changed() is a no-op during the combobox-set above (it bails out
     on darktable.gui->reset, which gui_update runs under, so programmatic
     loads don't get treated as user edits / spawn spurious history items).
     That means its scan_film "what should a reset target" bookkeeping --
     self->default_params->scan_film and the checkbox's own cached
     "dt-toggle-default" -- never gets re-baselined on a fresh module load,
     only when the user actually interacts with the film combobox. Left
     alone, both stay at the compiled FALSE default after e.g. closing and
     reopening darktable on an image using a positive/reversal film (which
     has no print stage and needs scan_film TRUE): the checkbox itself still
     shows correctly checked here (synced from p->scan_film below), but a
     later double-click reset on it would silently flip scan_film back off.
     Re-baseline both here too, exactly like _film_changed does -- but
     WITHOUT touching p->scan_film itself, since the just-loaded value may
     be a deliberate user override away from the film's natural mode and
     must be preserved on load; only the reset target needs fixing. */
  if(fi < g->n_films)
  {
    const sf_prof_entry_t *e = &g->entries[g->film_entry[fi]];
    if(self->default_params)
      ((dt_iop_spektrafilm_params_t *)self->default_params)->scan_film = e->positive;
    if(g->scan_film)
      g_object_set_data(G_OBJECT(g->scan_film), "dt-toggle-default", GINT_TO_POINTER(e->positive));
  }
  int pi = 0;
  const char *target = (fi < g->n_films) ? g->entries[g->film_entry[fi]].target_print : NULL;
  for(int k = 0; k < g->n_papers; k++)
  {
    const sf_prof_entry_t *e = &g->entries[g->paper_entry[k]];
    if(p->paper_hash ? (e->hash == p->paper_hash)
                     : (target && !strcmp(e->stock, target)))
      pi = k;
  }
  dt_bauhaus_combobox_set_from_value(g->paper, g->paper_entry[pi]);

  /* toggle_from_params check buttons are NOT auto-synced by
     dt_bauhaus_update_from_field (it only handles sliders/combos), so set
     them here or they drift from the params: a stale box makes the first
     click a no-op (field already has that value -> no history item) and
     module reset never updates them. */
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->scan_film), p->scan_film);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->print_auto_exposure), p->print_auto_exposure);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->halation_on), p->halation_on);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->diffusion_on), p->diffusion_on);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->print_diffusion_on), p->print_diffusion_on);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->grain_on), p->grain_on);
  _update_print_sensitivity(self);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_spektrafilm_gui_data_t *g = IOP_GUI_ALLOC(spektrafilm);
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  g->film = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->film, NULL, N_("film stock"));
  gtk_widget_set_tooltip_text(g->film, _("film emulsion (spektrafilm filming profile)"));
  g_signal_connect(G_OBJECT(g->film), "value-changed", G_CALLBACK(_film_changed), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->film, TRUE, TRUE, 0);

  g->paper = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->paper, NULL, N_("print paper"));
  gtk_widget_set_tooltip_text(g->paper,
                              _("print/paper stock; defaults to the film's target print"));
  g_signal_connect(G_OBJECT(g->paper), "value-changed", G_CALLBACK(_paper_changed), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->paper, TRUE, TRUE, 0);

  /* filmic-style tabbed notebook: everything else lives in tabs instead of a
     single flat, ever-growing list of sliders. */
  GtkWidget *sf_main_box = self->widget; /* restored after all tab pages below */
  static struct dt_action_def_t notebook_def = { };
  g->notebook = dt_ui_notebook_new(&notebook_def);
  dt_action_define_iop(self, NULL, N_("page"), GTK_WIDGET(g->notebook), &notebook_def);
  dt_gui_box_add(sf_main_box, GTK_WIDGET(g->notebook));

  /* ---- tab: film and print ---- */
  self->widget = dt_ui_notebook_page(g->notebook, N_("film and print"), NULL);
  dt_gui_box_add(self->widget, dt_ui_section_label_new(C_("section", "film")));
  g->exposure_ev = dt_bauhaus_slider_from_params(self, "exposure_ev");
  dt_bauhaus_slider_set_format(g->exposure_ev, _(" EV"));
  gtk_widget_set_tooltip_text(
      g->exposure_ev, _("film exposure compensation; with auto print exposure enabled, print"
                        " exposure follows automatically so this has no net brightness effect"
                        " (except on positive/reversal film, which has no print stage)"));
  g->scan_film = dt_bauhaus_toggle_from_params(self, "scan_film");
  gtk_widget_set_tooltip_text(g->scan_film,
                              _("view the developed film directly (no print stage)"));
  dt_gui_box_add(self->widget, dt_ui_section_label_new(C_("section", "print")));
  g->print_exposure_ev = dt_bauhaus_slider_from_params(self, "print_exposure_ev");
  dt_bauhaus_slider_set_format(g->print_exposure_ev, _(" EV"));
  gtk_widget_set_tooltip_text(g->print_exposure_ev, _("print brightness (enlarger exposure)"));
  g->print_auto_exposure = dt_bauhaus_toggle_from_params(self, "print_auto_exposure");
  gtk_widget_set_tooltip_text(
      g->print_auto_exposure,
      _("automatically compensate print exposure for film exposure changes, as a real"
        " printer would print to a fixed density; disable for film exposure to affect"
        " brightness directly, same as a fixed enlarger exposure time"));
  g->print_contrast = dt_bauhaus_slider_from_params(self, "print_contrast");
  gtk_widget_set_tooltip_text(g->print_contrast,
                              _("print contrast (morphs the paper's density curves)"));
  g->filter_m = dt_bauhaus_slider_from_params(self, "filter_m");
  dt_bauhaus_slider_set_format(g->filter_m, _(" CC"));
  gtk_widget_set_tooltip_text(g->filter_m,
                              _("magenta enlarger filtration, Kodak CC units from neutral"));
  g->filter_y = dt_bauhaus_slider_from_params(self, "filter_y");
  dt_bauhaus_slider_set_format(g->filter_y, _(" CC"));
  gtk_widget_set_tooltip_text(g->filter_y,
                              _("yellow enlarger filtration, Kodak CC units from neutral"));
  g->couplers_amount = dt_bauhaus_slider_from_params(self, "couplers_amount");
  gtk_widget_set_tooltip_text(g->couplers_amount,
                              _("DIR coupler strength: inter-layer inhibition drives saturation"
                                " and edge effects (1.0 = film-accurate, 0 = off)"));
  dt_gui_box_add(self->widget, dt_ui_section_label_new(C_("section", "preflash")));
  g->preflash_exposure = dt_bauhaus_slider_from_params(self, "preflash_exposure");
  gtk_widget_set_tooltip_text(
      g->preflash_exposure,
      _("preflash exposure: a brief, uniform pre-exposure of the print through"
        " the film's base density, before the main print exposure -- lifts"
        " shadows and reduces contrast (0 = off)"));
  g->preflash_m_shift = dt_bauhaus_slider_from_params(self, "preflash_m_shift");
  dt_bauhaus_slider_set_format(g->preflash_m_shift, _(" CC"));
  gtk_widget_set_tooltip_text(g->preflash_m_shift,
                              _("magenta filtration for the preflash exposure only, Kodak CC"
                                " units from neutral -- independent of the main enlarger"
                                " filtration above"));
  g->preflash_y_shift = dt_bauhaus_slider_from_params(self, "preflash_y_shift");
  dt_bauhaus_slider_set_format(g->preflash_y_shift, _(" CC"));
  gtk_widget_set_tooltip_text(g->preflash_y_shift,
                              _("yellow filtration for the preflash exposure only, Kodak CC"
                                " units from neutral -- independent of the main enlarger"
                                " filtration above"));
  dt_gui_box_add(self->widget, dt_ui_section_label_new(C_("section", "format")));
  g->film_format_mm = dt_bauhaus_slider_from_params(self, "film_format_mm");
  dt_bauhaus_slider_set_format(g->film_format_mm, _(" mm"));
  gtk_widget_set_tooltip_text(g->film_format_mm,
                              _("physical film width; sets the scale of grain and halation"));

  /* ---- tab: grain ---- */
  self->widget = dt_ui_notebook_page(g->notebook, N_("grain"), NULL);
  g->grain_on = dt_bauhaus_toggle_from_params(self, "grain_on");
  g->grain_amount = dt_bauhaus_slider_from_params(self, "grain_amount");
  dt_bauhaus_slider_set_soft_range(g->grain_amount, 0.0f, 2.0f);
  gtk_widget_set_tooltip_text(g->grain_amount,
                              _("grain strength (1.0 = film-accurate; drag up to 2,"
                                " right-click to enter higher values -- useful for pushing"
                                " naturally fine-grained stocks further than their"
                                " catalogue amount allows)"));
  g->grain_size = dt_bauhaus_slider_from_params(self, "grain_size");
  gtk_widget_set_tooltip_text(g->grain_size,
                              _("grain particle size (1.0 = film default; higher = coarser)"));

  /* ---- tab: halation ---- */
  self->widget = dt_ui_notebook_page(g->notebook, N_("halation"), NULL);
  g->halation_on = dt_bauhaus_toggle_from_params(self, "halation_on");
  g->scatter_amount = dt_bauhaus_slider_from_params(self, "scatter_amount");
  gtk_widget_set_tooltip_text(g->scatter_amount,
                              _("in-emulsion light scatter, before the halation bounce"
                                " (1.0 = film-accurate; 0 = off)"));
  g->scatter_scale = dt_bauhaus_slider_from_params(self, "scatter_scale");
  gtk_widget_set_tooltip_text(g->scatter_scale,
                              _("scatter size: scales the in-emulsion scatter radius"
                                " (1.0 = film-accurate)"));
  g->halation_amount = dt_bauhaus_slider_from_params(self, "halation_amount");
  dt_bauhaus_slider_set_soft_range(g->halation_amount, 0.0f, 2.0f);
  gtk_widget_set_tooltip_text(g->halation_amount,
                              _("halation strength (1.0 = film-accurate; drag up to 2,"
                                " right-click to enter higher values)"));
  g->halation_scale = dt_bauhaus_slider_from_params(self, "halation_scale");
  gtk_widget_set_tooltip_text(g->halation_scale,
                              _("halation size: scales the glow radius (1.0 = film-accurate)"));
  g->boost_ev = dt_bauhaus_slider_from_params(self, "boost_ev");
  dt_bauhaus_slider_set_format(g->boost_ev, _(" EV"));
  gtk_widget_set_tooltip_text(g->boost_ev,
                              _("highlight boost: reconstructs clipped highlights so they bloom"
                                " into halation/diffusion (0 = off)"));
  g->boost_range = dt_bauhaus_slider_from_params(self, "boost_range");
  gtk_widget_set_tooltip_text(g->boost_range, _("range of the highlight boost curve"));
  g->protect_ev = dt_bauhaus_slider_from_params(self, "protect_ev");
  dt_bauhaus_slider_set_format(g->protect_ev, _(" EV"));
  gtk_widget_set_tooltip_text(g->protect_ev,
                              _("protect tones below this many stops over mid-grey from the boost"));

  /* ---- tab: diffusion ---- */
  self->widget = dt_ui_notebook_page(g->notebook, N_("diffusion"), NULL);
  g->diffusion_on = dt_bauhaus_toggle_from_params(self, "diffusion_on");
  g->diffusion_filter_family = dt_bauhaus_combobox_from_params(self, "diffusion_filter_family");
  gtk_widget_set_tooltip_text(
      g->diffusion_filter_family,
      _("diffusion filter type: black pro-mist (concentrated, punchy halo, deep"
        " blacks) / glimmerglass (tight, subtle, sharp-preserving) / pro-mist"
        " (broader, pastel, atmospheric) / cinebloom (frame-wide, slow-decaying"
        " veil)"));
  g->diffusion_strength = dt_bauhaus_slider_from_params(self, "diffusion_strength");
  gtk_widget_set_tooltip_text(g->diffusion_strength, _("diffusion filter strength"));
  g->diffusion_scale = dt_bauhaus_slider_from_params(self, "diffusion_scale");
  gtk_widget_set_tooltip_text(g->diffusion_scale, _("diffusion halo/bloom size"));
  g->diffusion_warmth = dt_bauhaus_slider_from_params(self, "diffusion_warmth");
  gtk_widget_set_tooltip_text(g->diffusion_warmth,
                              _("diffusion halo warmth: >0 warm outer halo, <0 cool"
                                " (added on top of the selected filter's own warmth bias)"));

  dt_gui_box_add(self->widget, dt_ui_section_label_new(C_("section", "print diffusion")));
  g->print_diffusion_on = dt_bauhaus_toggle_from_params(self, "print_diffusion_on");
  g->print_diffusion_filter_family
      = dt_bauhaus_combobox_from_params(self, "print_diffusion_filter_family");
  gtk_widget_set_tooltip_text(
      g->print_diffusion_filter_family,
      _("print diffusion filter type (same presets as the film-stage filter)"));
  g->print_diffusion_strength = dt_bauhaus_slider_from_params(self, "print_diffusion_strength");
  gtk_widget_set_tooltip_text(g->print_diffusion_strength,
                              _("print diffusion filter strength"));
  g->print_diffusion_scale = dt_bauhaus_slider_from_params(self, "print_diffusion_scale");
  gtk_widget_set_tooltip_text(g->print_diffusion_scale,
                              _("print diffusion halo/bloom size"));
  g->print_diffusion_warmth = dt_bauhaus_slider_from_params(self, "print_diffusion_warmth");
  gtk_widget_set_tooltip_text(g->print_diffusion_warmth,
                              _("print diffusion halo warmth: >0 warm outer halo, <0 cool"
                                " (added on top of the selected filter's own warmth bias)"));

  /* ---- tab: advanced ---- */
  self->widget = dt_ui_notebook_page(g->notebook, N_("advanced"), NULL);
  g->quality = dt_bauhaus_combobox_from_params(self, "quality");
  gtk_widget_set_tooltip_text(g->quality,
                              _("spectral accuracy vs speed; the tables are PCHIP-interpolated"
                                " and validated against the reference"));
  g->output_luminance_boost = dt_bauhaus_slider_from_params(self, "output_luminance_boost");
  gtk_widget_set_tooltip_text(g->output_luminance_boost,
                              _("pre-compression boost: multiplies XYZ luminance before the"
                                " OkLCh gamut compressor, pushing the histogram right while"
                                " preserving the film's natural shoulder rolloff"));

  self->widget = sf_main_box;
}

// clang-format off
// modelines
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// clang-format on
