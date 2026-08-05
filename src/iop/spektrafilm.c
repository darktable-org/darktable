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
#include "develop/pixelpipe_cache.h"
#include "develop/tiling.h"
#include "develop/imageop_gui.h"
#include "develop/imageop_math.h"
#include "common/imagebuf.h"
#include "common/iop_profile.h"
#include "common/spektra_fetch.h"
#include "common/opencl.h"
#include "common/gaussian.h"
#include "gui/accelerators.h"
#include "dtgtk/button.h"
#include "dtgtk/paint.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SPEKTRA_INLINE static inline
#include "common/spektra_core.h"
#include "common/spektra_sim.h"

DT_MODULE_INTROSPECTION(1, dt_iop_spektrafilm_params_t)

/* Spatial-scale constants, micrometres on film unless noted (see the LUT
   module for the full rationale; these are shared with modify_roi_in() and
   tiling_callback() so the halo math stays in sync). */
#define SF_HALATION_FIRST_SIGMA_UM 65.0f
#define SF_HALATION_PSF_SIGMAS 1.7320508f /* sqrt(3) */
/* widest stage-1 scatter component: max(sc_tail)*max(tail_rat) from
   spektra_core.c's sf_halation() = 9.7 * 2.7684 um, rounded up */
#define SF_SCATTER_TAIL_MAX_UM 27.0f
/* Per-channel ceilings the ROI padding above is sized for. The scatter PSF is
   per-film pack data now, and modify_roi_in() runs before the sim exists, so it
   cannot measure the real values -- clamp them to what the padding covers
   instead, exactly as hal_sigma_um is clamped to SF_HALATION_FIRST_SIGMA_UM.
   9.7 * SF_EXPTAIL_R2 = 26.85, which is where the 27.0 above comes from. */
#define SF_SCATTER_CORE_CLAMP_UM 2.2f
#define SF_SCATTER_TAIL_CLAMP_UM 9.7f
/* [gl] GlareParams.roughness / .blur -- the reference exposes these but leaves
   them at these values for every profile, so only the amount gets a slider. */
#define SF_GLARE_ROUGHNESS 0.7f
#define SF_GLARE_BLUR_PX 0.5f
#define SF_GRAIN_BLUR_FACTOR 0.8f
#define SF_GRAIN_SIZE_MIN 0.05f
/* Upstream's GrainParams.blur_dye_clouds_um (params_schema.py): a SECOND,
 * per-sub-layer blur applied to the raw particle draw INSIDE the particle
 * sampler itself (layer_particle_model in grain.py), before the main
 * clump blur above ever runs -- sigma = SF_GRAIN_DYE_BLUR_UM *
 * sqrt(od_particle), where od_particle = dmax/npart is that sub-layer's
 * own per-particle optical density. Passed through verbatim (no
 * pixel_um conversion anywhere in the reference's own call chain,
 * despite the "_um" name) -- ported as literally as upstream computes it
 * rather than second-guessing the naming. No variance-restoration
 * afterward either, same as the main clump blur. */
#define SF_GRAIN_DYE_BLUR_UM 2.0f
/* Push/pull processing is really two things happening together: shooting
 * at an effective ISO different from box speed (already modeled via
 * exposure_ev), plus extended/reduced development time, which increases
 * or decreases contrast -- the gamma knob. There's no single fixed
 * physical constant for how much contrast one stop of push buys (it
 * depends on the specific film/developer combination, which isn't
 * modeled here), so this is a documented approximation: each stop
 * multiplies gamma by this factor, a commonly-cited rule of thumb
 * (roughly a 15% contrast increase per stop). Compounds naturally across
 * multiple stops (push 2 = factor^2), which suits gamma being a
 * multiplicative quantity in this model to begin with. */
#define SF_PUSH_PULL_GAMMA_PER_STOP 1.15f
#define SF_HALO_SIGMAS 4.0f
/* DIR coupler inhibitor diffusion; spektrafilm params_schema
   dir_couplers.diffusion_size_um default (a plain gaussian in the reference) */

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
  /* Identity of the spectral upsampling table this edit was developed against.
     Upstream revises it often and every revision changes the render, so an edit
     reopened against a different one is reported rather than silently rendering
     differently. 0 means "not recorded" (an edit older than this field, or one
     made while no pack was loaded) and never warns. Diagnostic only -- nothing
     downstream reads it. */
  uint32_t lut_hash;        // $DEFAULT: 0
  uint32_t paper_hash;      // $DEFAULT: 0  (0 = the film's target print stock)
  float exposure_ev;        // $MIN: -4.0 $MAX: 4.0 $DEFAULT: 0.0 $DESCRIPTION: "film exposure"
  /* "compensation" because it is an offset either way: with auto print
     exposure on it shifts the automatic result rather than being ignored, which
     the bare name implied. */
  float print_exposure_ev;  // $MIN: -3.0 $MAX: 3.0 $DEFAULT: 0.0 $DESCRIPTION: "print exposure compensation"
  gboolean print_auto_exposure; // $DEFAULT: FALSE $DESCRIPTION: "auto print exposure"
  float print_contrast;     // $MIN: 0.5 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "print contrast"
  float filter_m;           // $MIN: -60.0 $MAX: 60.0 $DEFAULT: 0.0 $DESCRIPTION: "filtration M"
  float filter_y;           // $MIN: -60.0 $MAX: 60.0 $DEFAULT: 0.0 $DESCRIPTION: "filtration Y"
  float couplers_amount;    // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 1.0 $DESCRIPTION: "DIR couplers"
  float preflash_exposure;  // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 0.0 $DESCRIPTION: "preflash exposure"
  float preflash_m_shift;   // $MIN: -60.0 $MAX: 60.0 $DEFAULT: 0.0 $DESCRIPTION: "preflash M filter shift"
  float preflash_y_shift;   // $MIN: -60.0 $MAX: 60.0 $DEFAULT: 0.0 $DESCRIPTION: "preflash Y filter shift"
  gboolean scan_film;       // $DEFAULT: FALSE $DESCRIPTION: "scan the film (skip print)"
  dt_iop_spektrafilm_quality_t quality; // $DEFAULT: DT_SPEKTRAFILM_Q_STANDARD $DESCRIPTION: "quality"
  gboolean halation_on;     // $DEFAULT: TRUE $DESCRIPTION: "enable halation"
  float scatter_amount;     // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 1.0 $DESCRIPTION: "scatter amount"
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
  /* The format combobox above picks a film GAUGE (35mm); this is the frame's
     LONG EDGE (36 mm). Both are right and both were called "format", which read
     as the preset contradicting the slider. */
  float film_format_mm;     // $MIN: 8.0 $MAX: 130.0 $DEFAULT: 36.0 $DESCRIPTION: "frame long edge"
  float output_luminance_boost; // $MIN: 0.5 $MAX: 4.0 $DEFAULT: 1.0 $DESCRIPTION: "pre-compression boost"
  float grain_usm_sigma;        // $MIN: 0.0 $MAX: 3.0 $DEFAULT: 0.7 $DESCRIPTION: "grain recovery sharpness"
  float grain_usm_amount;       // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.5 $DESCRIPTION: "grain recovery strength"
  float film_gamma_factor;      // $MIN: 0.25 $MAX: 4.0 $DEFAULT: 1.0 $DESCRIPTION: "development gamma"
  float film_gamma_factor_fast; // $MIN: 0.25 $MAX: 4.0 $DEFAULT: 1.0 $DESCRIPTION: "fast layer gamma"
  float film_gamma_factor_slow; // $MIN: 0.25 $MAX: 4.0 $DEFAULT: 1.0 $DESCRIPTION: "slow layer gamma"
  float film_developer_exhaustion; // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "developer exhaustion"
  float push_pull_stops; // $MIN: -4.0 $MAX: 4.0 $DEFAULT: 0.0 $DESCRIPTION: "push/pull"
  float scan_blur;          // $MIN: 0.0 $MAX: 4.0 $DEFAULT: 0.0 $DESCRIPTION: "scanner blur"
  float scan_usm_sigma;     // $MIN: 0.0 $MAX: 3.0 $DEFAULT: 0.7 $DESCRIPTION: "scanner sharpness"
  float scan_usm_amount;    // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 0.7 $DESCRIPTION: "scanner sharpen strength"
  /* Off by default. Glare is a viewing-condition simulation, not a film
     property, and it is the last thing in the chain: leaving it on lifts the
     black point of every render, which hides the real per-paper black points
     while you are still judging them. The reference defaults it on because it
     renders finished images; this is an editing tool. Note that this is NOT
     the same reasoning as scan_usm_amount, which does default to the
     reference's own 0.7 -- sharpening is part of what a scan looks like,
     whereas glare is a property of the room the print is viewed in. */
  float glare_percent;      // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "viewing glare"
  float development_min;    // $MIN: 0.0 $MAX: 15.0 $DEFAULT: 0.0 $DESCRIPTION: "development time"
  float print_development_min; // $MIN: 0.0 $MAX: 15.0 $DEFAULT: 0.0 $DESCRIPTION: "development time"
  /* The two halves of the hanatos2025 sensitivity adaptation, each defaulting
     the way the reference resolves it. Bandwidth on: the filming stage agrees
     with the reference to 4e-15 that way. Surface off: the film profiles carry
     the correction surface and enable it, but the reference runtime's own
     setting disables it and wins, so its renders are made without it. Both are
     left switchable because the coefficients are part of the profile data and
     the upstream defaults may flip; see spektra_sim.h. */
  gboolean adaptation_bandwidth; // $DEFAULT: TRUE $DESCRIPTION: "bandwidth adaptation"
  gboolean adaptation_surface; // $DEFAULT: FALSE $DESCRIPTION: "surface adaptation"
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
  /* development times this stock is characterised at; n_dev == 0 means a single
     characterisation, i.e. nothing for the development slider to choose */
  int n_dev;
  double dev_times[SF_MAX_DEV_TIMES];
  uint32_t hash;
} sf_prof_entry_t;

typedef struct dt_iop_spektrafilm_gui_data_t
{
  GtkWidget *film, *paper;
  GtkWidget *output_boost;
  GtkWidget *film_format_combo, *film_format_mm_slider;
  GtkWidget *exposure_ev, *scan_film;
  GtkWidget *push_pull_stops, *film_gamma_factor;
  GtkWidget *film_gamma_factor_fast, *film_gamma_factor_slow, *film_developer_exhaustion;
  GtkWidget *quality, *adaptation_bandwidth, *adaptation_surface;
  GtkWidget *print_exposure_ev, *print_auto_exposure, *print_contrast;
  GtkWidget *filter_m, *filter_y, *couplers_amount;
  GtkWidget *preflash_exposure, *preflash_m_shift, *preflash_y_shift;
  GtkWidget *grain_on, *grain_amount, *grain_size;
  GtkWidget *scan_blur, *scan_usm_sigma, *scan_usm_amount, *glare_percent;
  GtkWidget *development_min, *print_development_min;
  GtkWidget *grain_usm_sigma, *grain_usm_amount;
  GtkWidget *halation_on, *scatter_amount, *scatter_scale, *halation_amount, *halation_scale;
  GtkWidget *boost_ev, *boost_range, *protect_ev;
  GtkWidget *diffusion_on, *diffusion_filter_family, *diffusion_strength, *diffusion_scale, *diffusion_warmth;
  GtkWidget *print_diffusion_on, *print_diffusion_filter_family;
  GtkWidget *print_diffusion_strength, *print_diffusion_scale, *print_diffusion_warmth;

  /* every profile found on disk, sorted, films and papers together --
     e->printing separates them. Owns its sf_prof_entry_t nodes. */
  GList *entries;
  GtkNotebook *notebook;

  /* data-pack row in the header: a button and a status line, both hidden while
     the installed pack already satisfies the edit. Shown only when there is
     something to do, so the common case stays a clean two-combobox header. */
  /* main_box holds everything except the data-pack row, so the whole module can
     be collapsed to just that row while no usable pack exists. */
  GtkWidget *main_box;
  GtkWidget *data_box, *data_button, *data_status;
  guint data_poll;      /* g_timeout id while a download runs, 0 otherwise */
  uint32_t data_wanted; /* spectral table the button will ask for */
  sf_fetch_state_t data_last_state; /* to spot the moment a fetch finishes */
} dt_iop_spektrafilm_gui_data_t;

static const struct { const char *label; float mm; } _format_presets[] = {
  { "half-frame",  24.0f }, { "35mm",  36.0f }, { "6x6",   56.0f },
  { "6x7",         69.0f }, { "6x9",   84.0f }, { "4x5",  120.0f },
  { "8x10",       244.0f },
  { "Super 8",      5.79f }, { "16mm", 10.26f }, { "Super 16", 12.52f },
  { "Super 35",    24.89f }, { "VistaVision", 37.72f },
  { "65mm 5-perf", 52.63f }, { "IMAX 15-perf", 69.6f },
};
#define FORMAT_PRESETS_N ((int)(sizeof(_format_presets) / sizeof(_format_presets[0])))
#define FORMAT_PRESET_CUSTOM FORMAT_PRESETS_N

static int _format_mm_to_preset(float mm)
{
  for(int i = 0; i < FORMAT_PRESETS_N; i++)
    if(fabsf(_format_presets[i].mm - mm) < 0.01f) return i;
  return FORMAT_PRESET_CUSTOM;
}

static void _format_changed(GtkWidget *combo, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_spektrafilm_gui_data_t *g = (dt_iop_spektrafilm_gui_data_t *)self->gui_data;
  dt_iop_spektrafilm_params_t *p = (dt_iop_spektrafilm_params_t *)self->params;
  const int pi = GPOINTER_TO_INT(dt_bauhaus_combobox_get_data(g->film_format_combo));
  if(pi >= 0 && pi < FORMAT_PRESETS_N)
  {
    DT_ENTER_GUI_UPDATE();
    dt_bauhaus_slider_set(g->film_format_mm_slider, _format_presets[pi].mm);
    DT_LEAVE_GUI_UPDATE();
    p->film_format_mm = _format_presets[pi].mm;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
  /* The millimetre slider only means anything on "custom" -- on a preset it is
     a read-only echo, and one that reads as a contradiction ("35mm" setting
     "36 mm") because the preset names a film GAUGE while the slider is the
     frame's LONG EDGE. Showing it only when it is editable removes both the
     row and the confusion. */
  gtk_widget_set_visible(g->film_format_mm_slider, pi < 0 || pi >= FORMAT_PRESETS_N);
}

static void _format_slider_changed(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_spektrafilm_gui_data_t *g = (dt_iop_spektrafilm_gui_data_t *)self->gui_data;
  const float mm = dt_bauhaus_slider_get(g->film_format_mm_slider);
  dt_bauhaus_combobox_set_from_value(g->film_format_combo, _format_mm_to_preset(mm));
}

static void _populate_format_combo(dt_iop_module_t *self)
{
  dt_iop_spektrafilm_gui_data_t *g = (dt_iop_spektrafilm_gui_data_t *)self->gui_data;
  dt_bauhaus_combobox_clear(g->film_format_combo);
  dt_bauhaus_combobox_add_section(g->film_format_combo, C_("section", "still"));
  for(int i = 0; i < 7; i++)
    dt_bauhaus_combobox_add_full(g->film_format_combo, _( _format_presets[i].label ),
                                 DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, GINT_TO_POINTER(i), NULL, TRUE);
  dt_bauhaus_combobox_add_section(g->film_format_combo, C_("section", "cine"));
  for(int i = 7; i < FORMAT_PRESETS_N; i++)
    dt_bauhaus_combobox_add_full(g->film_format_combo, _( _format_presets[i].label ),
                                 DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, GINT_TO_POINTER(i), NULL, TRUE);
  dt_bauhaus_combobox_add_section(g->film_format_combo, C_("section", "custom"));
  dt_bauhaus_combobox_add_full(g->film_format_combo, _("custom"),
                               DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, GINT_TO_POINTER(FORMAT_PRESETS_N), NULL, TRUE);
}

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
  char sim_warning[256];
  /* multi-sublayer grain GPU constant buffers (see process_cl's grain
     stage): built from d->gpu's grain_layer_* tables, which only change
     when d->gpu itself is rebuilt (a new film/paper/quality choice), never
     per-tile. Cached here and keyed on the `gpu` pointer they were built
     from AND on the device they were built on, instead of being re-uploaded
     on every process_cl() call -- tiled processing calls process_cl() once
     per tile, so uploading these fresh every time was pure per-tile overhead
     for data that never changes between tiles of the same image.

     The device is part of the key because a cl_mem belongs to the context
     that created it. piece->data lives as long as the pipe, but pipe->devid
     is reassigned by dt_opencl_lock_device() on every run -- that is what the
     device pool is for -- so on a machine with more than one OpenCL device a
     pipe can upload these on one device and, next run, hand them to a kernel
     on another. The handles are still valid, just foreign, and clSetKernelArg
     dereferences them inside the driver: it segfaults there rather than
     returning an error, so there is nothing to check afterwards. */
  const sf_sim_gpu_t *grain_cl_built_for;
  int grain_cl_devid;
  cl_mem grain_cl_dmax, grain_cl_npart, grain_cl_dmin, grain_cl_total, grain_cl_curve;
} dt_iop_spektrafilm_data_t;

typedef struct dt_iop_spektrafilm_global_data_t
{
  int kernel_expose, kernel_lograw, kernel_develop_corr, kernel_develop;
  int kernel_grain_gen_raw_sl, kernel_grain_accumulate_1c, kernel_grain_finalize_channel,
      kernel_grain_add, kernel_grain_usm;
  int kernel_print_expose, kernel_print_develop, kernel_scan, kernel_passthrough;
  int kernel_crop_out, kernel_scan_usm, kernel_glare_gen, kernel_glare_add;
  int kernel_scatter_combine, kernel_accum, kernel_channel_extract, kernel_channel_accum, kernel_halation_apply;
  int kernel_gauss_row_4c, kernel_gauss_col_4c, kernel_gauss_row_1c, kernel_gauss_col_1c;
  int kernel_yvv_row_4c, kernel_yvv_col_4c, kernel_yvv_row_1c, kernel_yvv_col_1c;
  int kernel_boost, kernel_diffusion_accum, kernel_diffusion_mix;
} dt_iop_spektrafilm_global_data_t;

/* the data pack is large (spectra LUT ~12 MB) and shared by all pieces;
   load it once per process (lazily, under _pack_lock), freed in
   cleanup_global(). Kept in module-static storage rather than global_data so
   every pipe sees the same pack. */
static sf_pack_t *_pack = NULL;
/* Directory _pack was loaded from. Which directory that is depends on the edit:
   an image developed against an older spectral table resolves to a different
   pack than one developed against the current one. Keeping the path lets
   _ensure_sim() notice the resolved directory has changed and reload, and lets
   _scan_profiles() read profiles out of the SAME pack rather than always out of
   the config directory -- profiles and spectral table have to come from one
   place or the film list will not match the data behind it. */
static char _pack_path[SF_PATH_LEN] = { 0 };
/* Value of sf_fetch_generation() when _pack / _pack_error were last decided.
   A download can install a pack into a directory this module already probed
   and rejected, which leaves the resolved path unchanged -- so a path
   comparison alone cannot tell that the answer has changed. */
static guint _pack_gen = 0;
static char _pack_error[256] = { 0 };
static dt_pthread_mutex_t _pack_lock;

void init_global(dt_iop_module_so_t *self)
{
  dt_pthread_mutex_init(&_pack_lock, NULL);
  sf_fetch_init();
  const int program = 42; /* spektrafilm.cl in data/kernels/programs.conf */
  dt_iop_spektrafilm_global_data_t *gd = malloc(sizeof(dt_iop_spektrafilm_global_data_t));
  self->data = gd;
  gd->kernel_expose = dt_opencl_create_kernel(program, "spektrafilm_expose");
  gd->kernel_lograw = dt_opencl_create_kernel(program, "spektrafilm_lograw");
  gd->kernel_develop_corr = dt_opencl_create_kernel(program, "spektrafilm_develop_corr");
  gd->kernel_develop = dt_opencl_create_kernel(program, "spektrafilm_develop");
  gd->kernel_grain_gen_raw_sl = dt_opencl_create_kernel(program, "spektrafilm_grain_gen_raw_sl");
  gd->kernel_grain_accumulate_1c = dt_opencl_create_kernel(program, "spektrafilm_grain_accumulate_1c");
  gd->kernel_grain_finalize_channel
      = dt_opencl_create_kernel(program, "spektrafilm_grain_finalize_channel");
  gd->kernel_grain_add = dt_opencl_create_kernel(program, "spektrafilm_grain_add");
  gd->kernel_grain_usm = dt_opencl_create_kernel(program, "spektrafilm_grain_usm");
  gd->kernel_print_expose = dt_opencl_create_kernel(program, "spektrafilm_print_expose");
  gd->kernel_print_develop = dt_opencl_create_kernel(program, "spektrafilm_print_develop");
  gd->kernel_scan = dt_opencl_create_kernel(program, "spektrafilm_scan");
  gd->kernel_crop_out = dt_opencl_create_kernel(program, "spektrafilm_crop_out");
  gd->kernel_scan_usm = dt_opencl_create_kernel(program, "spektrafilm_scan_usm");
  gd->kernel_glare_gen = dt_opencl_create_kernel(program, "spektrafilm_glare_gen");
  gd->kernel_glare_add = dt_opencl_create_kernel(program, "spektrafilm_glare_add");
  gd->kernel_passthrough = dt_opencl_create_kernel(program, "spektrafilm_passthrough");
  gd->kernel_scatter_combine = dt_opencl_create_kernel(program, "spektrafilm_scatter_combine");
  gd->kernel_accum = dt_opencl_create_kernel(program, "spektrafilm_accum");
  gd->kernel_channel_extract = dt_opencl_create_kernel(program, "spektrafilm_channel_extract");
  gd->kernel_yvv_row_4c = dt_opencl_create_kernel(program, "spektrafilm_yvv_row_4c");
  gd->kernel_yvv_col_4c = dt_opencl_create_kernel(program, "spektrafilm_yvv_col_4c");
  gd->kernel_yvv_row_1c = dt_opencl_create_kernel(program, "spektrafilm_yvv_row_1c");
  gd->kernel_yvv_col_1c = dt_opencl_create_kernel(program, "spektrafilm_yvv_col_1c");
  gd->kernel_gauss_row_4c = dt_opencl_create_kernel(program, "spektrafilm_gauss_row_4c");
  gd->kernel_gauss_col_4c = dt_opencl_create_kernel(program, "spektrafilm_gauss_col_4c");
  gd->kernel_gauss_row_1c = dt_opencl_create_kernel(program, "spektrafilm_gauss_row_1c");
  gd->kernel_gauss_col_1c = dt_opencl_create_kernel(program, "spektrafilm_gauss_col_1c");
  gd->kernel_channel_accum = dt_opencl_create_kernel(program, "spektrafilm_channel_accum");
  gd->kernel_halation_apply = dt_opencl_create_kernel(program, "spektrafilm_halation_apply");
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
    dt_opencl_free_kernel(gd->kernel_grain_gen_raw_sl);
    dt_opencl_free_kernel(gd->kernel_grain_accumulate_1c);
    dt_opencl_free_kernel(gd->kernel_grain_finalize_channel);
    dt_opencl_free_kernel(gd->kernel_grain_add);
    dt_opencl_free_kernel(gd->kernel_grain_usm);
    dt_opencl_free_kernel(gd->kernel_print_expose);
    dt_opencl_free_kernel(gd->kernel_print_develop);
    dt_opencl_free_kernel(gd->kernel_scan);
    dt_opencl_free_kernel(gd->kernel_crop_out);
    dt_opencl_free_kernel(gd->kernel_scan_usm);
    dt_opencl_free_kernel(gd->kernel_glare_gen);
    dt_opencl_free_kernel(gd->kernel_glare_add);
    dt_opencl_free_kernel(gd->kernel_passthrough);
    dt_opencl_free_kernel(gd->kernel_scatter_combine);
    dt_opencl_free_kernel(gd->kernel_accum);
    dt_opencl_free_kernel(gd->kernel_channel_extract);
    dt_opencl_free_kernel(gd->kernel_yvv_row_4c);
    dt_opencl_free_kernel(gd->kernel_yvv_col_4c);
    dt_opencl_free_kernel(gd->kernel_yvv_row_1c);
    dt_opencl_free_kernel(gd->kernel_yvv_col_1c);
    dt_opencl_free_kernel(gd->kernel_gauss_row_4c);
    dt_opencl_free_kernel(gd->kernel_gauss_col_4c);
    dt_opencl_free_kernel(gd->kernel_gauss_row_1c);
    dt_opencl_free_kernel(gd->kernel_gauss_col_1c);
    dt_opencl_free_kernel(gd->kernel_channel_accum);
    dt_opencl_free_kernel(gd->kernel_halation_apply);
    dt_opencl_free_kernel(gd->kernel_boost);
    dt_opencl_free_kernel(gd->kernel_diffusion_accum);
    dt_opencl_free_kernel(gd->kernel_diffusion_mix);
    free(self->data);
    self->data = NULL;
  }
  /* Before the pack lock goes away: a running fetch reprocesses the pipe on
     completion, so it has to be joined while there is still a pipe to touch. */
  sf_fetch_cleanup();
  dt_pthread_mutex_lock(&_pack_lock);
  if(_pack)
  {
    sf_pack_free(_pack);
    _pack = NULL;
  }
  _pack_path[0] = 0;
  _pack_gen = 0;
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
  /* Same grouping as the other display transforms (filmicrgb, sigmoid, agx),
     not the grading modules. It matters beyond tidiness: "only use one display
     transform" is the module's first piece of advice, and filing it under
     colour put it in a different group from every module it conflicts with. */
  return IOP_GROUP_TONE | IOP_GROUP_TECHNICAL;
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

/* ---------------------------------------------------------------------- */
/* profile discovery                                                      */
/* ---------------------------------------------------------------------- */

/* Stable identity for a profile in params: the stock name hashed, so a rescan
   or a different machine resolves the same stock regardless of directory order.
   Folded to 32 bits because params store it as uint32_t. */
static uint32_t _name_hash(const char *s)
{
  const dt_hash_t h = dt_hash(DT_INITHASH, s, strlen(s));
  const uint32_t h32 = (uint32_t)(h ^ (h >> 32));
  return h32 ? h32 : 1; /* 0 is reserved for "first available" */
}

/* Where a hand-installed pack lives. Still the preferred location, and the one
   named in the "no data pack" message, but no longer the only one: downloaded
   packs live under the cache directory, one per spectral table. */
static void _pack_dir(char *dst, size_t dstsz)
{
  char cfg[SF_PATH_LEN];
  dt_loc_get_user_config_dir(cfg, sizeof cfg);
  snprintf(dst, dstsz, "%s/spektrafilm", cfg);
}

/* Pick the pack directory for an edit that recorded wanted_lut_hash (0 = no
   preference). Local lookup only -- no network, safe on the pixelpipe. Falls
   back to the config directory so error text still names somewhere real. */
static void _resolve_pack_dir(uint32_t wanted_lut_hash, char *dst, size_t dstsz)
{
  gboolean exact = FALSE;
  const gboolean found = sf_fetch_resolve_pack_dir(wanted_lut_hash, dst, dstsz, &exact);
  if(!found) _pack_dir(dst, dstsz);
  /* Which directory was chosen, and why, is the first thing worth knowing when
     the module renders nothing: it separates "no pack anywhere" from "found a
     pack, but it failed to load". */
  dt_print(DT_DEBUG_DEV,
           "[spektrafilm] pack for table %08x -> %s (%s)\n", wanted_lut_hash, dst,
           !found      ? "nothing found, falling back"
           : exact     ? "exact match"
           : !wanted_lut_hash ? "no table recorded, taking what is installed"
                       : "table mismatch, using anyway");
}

/* natural (human) string compare: embedded numbers compared numerically
   so "Vision3 50D" < "Vision3 200T" < "Vision3 500T" */
static int _nat_cmp(const char *a, const char *b)
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

/* Trim filler out of a profile's display name, in place.

   The comboboxes are as wide as darktable's right panel and no wider, so a name
   like "Kodak Professional Portra Endura" truncates to "Kodak Professional ..."
   -- and four papers share that prefix, so the list became four identical
   entries. Only genuinely redundant words go: "Professional" says nothing, and
   "Negative Film" / "Reversal Film" duplicate the section heading the entry is
   already filed under. Everything that identifies the stock stays, including
   "Print Film 2302", where it is part of the name people know. */
static void _shorten_name(char *s, const size_t sz)
{
  static const char *const filler[] = { " Professional", " Negative Film", " Reversal Film" };
  for(size_t f = 0; f < sizeof(filler) / sizeof(*filler); f++)
  {
    char *at = strstr(s, filler[f]);
    if(!at) continue;
    const size_t flen = strlen(filler[f]);
    memmove(at, at + flen, strlen(at + flen) + 1);
  }
  (void)sz;
}

static gint _entry_name_cmp(gconstpointer a, gconstpointer b)
{
  return _nat_cmp(((const sf_prof_entry_t *)a)->name, ((const sf_prof_entry_t *)b)->name);
}

/* scan <packdir>/profiles/ (all .json files); reads only the info header of
   each profile (stock / name / stage / target_print). Returns a newly allocated
   list of sf_prof_entry_t, sorted by name, which the caller owns.

   packdir may be NULL, which means "whichever pack is currently loaded, or the
   one a fresh edit would resolve to". Profiles must come from the same pack as
   the spectral table: a profile names a stock, and the pack holds that stock's
   digested render defaults, so mixing the two silently drops per-film halation,
   grain and coupler data for any stock the other side has never heard of. */
static GList *_scan_profiles(const char *packdir)
{
  char dir[SF_PATH_LEN];
  if(packdir && *packdir)
    g_strlcpy(dir, packdir, sizeof dir);
  else
  {
    dt_pthread_mutex_lock(&_pack_lock);
    /* _pack_path also records directories that failed to load, so it only
       answers "which pack is in use" when one is actually held. */
    const gboolean have = _pack && _pack_path[0] != 0;
    if(have) g_strlcpy(dir, _pack_path, sizeof dir);
    dt_pthread_mutex_unlock(&_pack_lock);
    if(!have) _resolve_pack_dir(0, dir, sizeof dir);
  }
  char profdir[SF_PATH_LEN + 16];
  snprintf(profdir, sizeof profdir, "%s/profiles", dir);

  GDir *gd = g_dir_open(profdir, 0, NULL);
  if(!gd) return NULL;
  GList *list = NULL;
  const char *fn;
  while((fn = g_dir_read_name(gd)))
  {
    if(!g_str_has_suffix(fn, ".json")) continue;
    char path[SF_PATH_LEN + 300];
    snprintf(path, sizeof path, "%s/%s", profdir, fn);
    char *err = NULL;
    sf_profile_t *prof = sf_profile_load(path, 0.5f, &err); /* info header only */
    if(!prof)
    {
      free(err);
      continue;
    }
    sf_prof_entry_t *e = g_malloc0(sizeof(sf_prof_entry_t));
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
    _shorten_name(e->name, sizeof e->name);
    const char *cm = sf_profile_channel_model(prof);
    e->bw = (cm && !strcmp(cm, "bw"));
    e->n_dev = sf_profile_dev_times(prof, e->dev_times, SF_MAX_DEV_TIMES);
    e->hash = _name_hash(e->stock);
    sf_profile_free(prof);
    list = g_list_prepend(list, e);
  }
  g_dir_close(gd);
  /* natural order by display name (numbers compared numerically,
     so "50D" < "200T" instead of lexicographic "200T" < "50D") */
  return g_list_sort(list, _entry_name_cmp);
}

/* resolve a profile hash to its stock name. hash 0 -> default:
   for films the first filming stock, for papers prefer the film's
   target_print. Returns false when nothing matches. */
static gboolean _resolve_stock(GList *entries, uint32_t hash, gboolean want_printing,
                               const char *prefer_stock, char *dst, size_t dstsz)
{
  if(hash)
    for(GList *l = entries; l; l = l->next)
    {
      const sf_prof_entry_t *e = l->data;
      if(e->hash == hash && e->printing == want_printing)
      {
        g_strlcpy(dst, e->stock, dstsz);
        return TRUE;
      }
    }
  if(prefer_stock && prefer_stock[0])
    for(GList *l = entries; l; l = l->next)
    {
      const sf_prof_entry_t *e = l->data;
      if(e->printing == want_printing && !strcmp(e->stock, prefer_stock))
      {
        g_strlcpy(dst, e->stock, dstsz);
        return TRUE;
      }
    }
  for(GList *l = entries; l; l = l->next)
  {
    const sf_prof_entry_t *e = l->data;
    if(e->printing == want_printing)
    {
      g_strlcpy(dst, e->stock, dstsz);
      return TRUE;
    }
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
    if(d->grain_cl_dmax) dt_opencl_release_mem_object(d->grain_cl_dmax);
    if(d->grain_cl_npart) dt_opencl_release_mem_object(d->grain_cl_npart);
    if(d->grain_cl_dmin) dt_opencl_release_mem_object(d->grain_cl_dmin);
    if(d->grain_cl_total) dt_opencl_release_mem_object(d->grain_cl_total);
    if(d->grain_cl_curve) dt_opencl_release_mem_object(d->grain_cl_curve);
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
  key = _mix64(key, &p->lut_hash, sizeof p->lut_hash);
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
  /* selects which member of a B&W development-time family the curve model is
     built from, so it has to be part of the sim's cache key */
  key = _mix64(key, &p->development_min, sizeof p->development_min);
  key = _mix64(key, &p->print_development_min, sizeof p->print_development_min);
  key = _mix64(key, &p->quality, sizeof p->quality);
  /* both change the tc LUT, which is built once per sim */
  key = _mix64(key, &p->adaptation_bandwidth, sizeof p->adaptation_bandwidth);
  key = _mix64(key, &p->adaptation_surface, sizeof p->adaptation_surface);
  key = _mix64(key, &p->output_luminance_boost, sizeof p->output_luminance_boost);
  key = _mix64(key, &p->film_gamma_factor, sizeof p->film_gamma_factor);
  key = _mix64(key, &p->film_gamma_factor_fast, sizeof p->film_gamma_factor_fast);
  key = _mix64(key, &p->film_gamma_factor_slow, sizeof p->film_gamma_factor_slow);
  key = _mix64(key, &p->film_developer_exhaustion, sizeof p->film_developer_exhaustion);
  key = _mix64(key, &p->push_pull_stops, sizeof p->push_pull_stops);
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
    /* these were uploaded from this gpu's grain_layer_* tables; release them
       now so process_cl() re-uploads fresh ones from the new gpu instead of
       reusing now-stale content. */
    if(d->grain_cl_dmax) { dt_opencl_release_mem_object(d->grain_cl_dmax); d->grain_cl_dmax = NULL; }
    if(d->grain_cl_npart) { dt_opencl_release_mem_object(d->grain_cl_npart); d->grain_cl_npart = NULL; }
    if(d->grain_cl_dmin) { dt_opencl_release_mem_object(d->grain_cl_dmin); d->grain_cl_dmin = NULL; }
    if(d->grain_cl_total) { dt_opencl_release_mem_object(d->grain_cl_total); d->grain_cl_total = NULL; }
    if(d->grain_cl_curve) { dt_opencl_release_mem_object(d->grain_cl_curve); d->grain_cl_curve = NULL; }
    d->grain_cl_built_for = NULL;
  }
  if(d->sim)
  {
    sf_sim_free(d->sim);
    d->sim = NULL;
  }
  d->sim_key = key;
  d->sim_error[0] = 0;
  d->sim_warning[0] = 0;

  /* Global pack, loaded once per resolved directory.
     Which directory that is depends on the spectral table this edit recorded,
     so switching to an image developed against a different table reloads rather
     than silently rendering it with the wrong data. One pack is held at a time:
     the LUT is ~12 MB, and having two images from different table generations
     open in the same session is rare enough that the reload costs less than
     permanently carrying every pack the user has on disk. */
  char want_dir[SF_PATH_LEN];
  _resolve_pack_dir(p->lut_hash, want_dir, sizeof want_dir);

  const guint gen = sf_fetch_generation();

  dt_pthread_mutex_lock(&_pack_lock);
  /* The loaded pack and the error from failing to load both belong to one
     directory at one point in time, so both go stale on either axis: the
     resolved directory changing, or a download changing what that directory
     holds. Testing only the first, and only while a pack was actually loaded,
     left _pack_error latched forever after the first failure -- so a pack
     downloaded mid-session was never picked up and the image went on
     rendering against a failure recorded before the pack existed. */
  if(strcmp(_pack_path, want_dir) != 0 || _pack_gen != gen)
  {
    if(_pack) sf_pack_free(_pack);
    _pack = NULL;
    _pack_error[0] = 0;
    _pack_path[0] = 0;
  }
  if(!_pack && !_pack_error[0])
  {
    char *err = NULL;
    _pack = sf_pack_load(want_dir, &err);
    /* Record the attempt on failure too: _pack_path is what the staleness
       check above compares against, and leaving it empty after a failed load
       would make every later call look stale and retry the same doomed load
       once per pipe run. */
    g_strlcpy(_pack_path, want_dir, sizeof _pack_path);
    _pack_gen = gen;
    if(!_pack)
    {
      g_strlcpy(_pack_error, err ? err : "unknown", sizeof _pack_error);
      dt_print(DT_DEBUG_DEV, "[spektrafilm] %s\n", _pack_error);
      free(err);
    }
    else
      dt_print(DT_DEBUG_DEV, "[spektrafilm] loaded data pack %s (spektrafilm %s)\n",
               want_dir, sf_pack_version(_pack));
  }
  sf_pack_t *pack = _pack;
  char pack_dir[SF_PATH_LEN];
  char pack_error[sizeof _pack_error];
  g_strlcpy(pack_dir, _pack_path, sizeof pack_dir);
  g_strlcpy(pack_error, _pack_error, sizeof pack_error);
  dt_pthread_mutex_unlock(&_pack_lock);
  if(!pack)
  {
    /* Without this the module renders nothing behind an empty trouble banner
       and the reason only ever reaches the terminal. */
    g_strlcpy(d->sim_error, pack_error[0] ? pack_error : "no data pack found",
              sizeof d->sim_error);
    dt_pthread_mutex_unlock(&d->lock);
    return NULL;
  }

  /* resolve stocks */
  GList *entries = _scan_profiles(pack_dir);
  char film_stock[SF_NAME_LEN] = { 0 }, paper_stock[SF_NAME_LEN] = { 0 };
  if(!_resolve_stock(entries, p->film_hash, FALSE, "kodak_portra_400", film_stock,
                     sizeof film_stock))
  {
    g_strlcpy(d->sim_error, "no filming profiles found", sizeof d->sim_error);
    dt_print(DT_DEBUG_DEV, "[spektrafilm] no filming profiles under %s/profiles\n",
             pack_dir);
    g_list_free_full(entries, g_free);
    dt_pthread_mutex_unlock(&d->lock);
    return NULL;
  }
  const char *target_print = NULL;
  for(GList *l = entries; l; l = l->next)
  {
    const sf_prof_entry_t *e = l->data;
    if(!e->printing && !strcmp(e->stock, film_stock)) target_print = e->target_print;
  }
  if(!p->scan_film
     && !_resolve_stock(entries, p->paper_hash, TRUE, target_print, paper_stock,
                        sizeof paper_stock))
  {
    g_strlcpy(d->sim_error, "no printing profiles found", sizeof d->sim_error);
    dt_print(DT_DEBUG_DEV, "[spektrafilm] no printing profiles under %s/profiles\n",
             pack_dir);
    g_list_free_full(entries, g_free);
    dt_pthread_mutex_unlock(&d->lock);
    return NULL;
  }

  g_list_free_full(entries, g_free); /* stocks resolved; the list is done */

  /* Load from the pack that was actually resolved, NOT from the config
     directory. _scan_profiles() above lists the resolved pack's profiles, so
     using a different directory here means the stock names resolve against one
     pack and the files are read from another -- with a downloaded pack and
     nothing hand-installed, every load simply misses and the module goes quiet
     for want of a film. */
  char path[SF_PATH_LEN + 300];
  char *err = NULL;
  snprintf(path, sizeof path, "%s/profiles/%s.json", pack_dir, film_stock);
  sf_profile_t *film = sf_profile_load(path, p->development_min, &err);
  if(!film)
    dt_print(DT_DEBUG_DEV, "[spektrafilm] cannot load film profile %s: %s\n", path,
             err ? err : "unknown");
  sf_profile_t *paper = NULL;
  if(film && !p->scan_film)
  {
    snprintf(path, sizeof path, "%s/profiles/%s.json", pack_dir, paper_stock);
    paper = sf_profile_load(path, p->print_development_min, &err);
    if(!paper)
      dt_print(DT_DEBUG_DEV, "[spektrafilm] cannot load print profile %s: %s\n", path,
               err ? err : "unknown");
  }

  /* A profile that will not load left sim_error empty and printed nothing: the
     only branch that reports err sits inside the "film loaded" path below, so
     this failure rendered as a silently disabled module. */
  if(!film || (!paper && !p->scan_film))
    g_strlcpy(d->sim_error, err ? err : "profile could not be loaded",
              sizeof d->sim_error);

  if(film && (paper || p->scan_film))
  {
    sf_sim_params_t sp;
    sf_sim_params_defaults(&sp);
    sp.exposure_comp_ev = p->exposure_ev - p->push_pull_stops;
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
    sp.adaptation_bandwidth = p->adaptation_bandwidth;
    sp.adaptation_surface = p->adaptation_surface;
    sp.lut_steps = _quality_steps(p->quality);
    sp.out_luminance_boost = p->output_luminance_boost;
    if(p->print_contrast != 1.0f)
    {
      sp.morph_active = true;
      sp.morph_gamma = p->print_contrast;
    }
    if(p->film_gamma_factor != 1.0f || p->film_gamma_factor_fast != 1.0f
       || p->film_gamma_factor_slow != 1.0f || p->film_developer_exhaustion != 0.0f
       || p->push_pull_stops != 0.0f)
    {
      sp.film_morph_active = true;
      sp.film_morph_gamma = p->film_gamma_factor
                             * powf(SF_PUSH_PULL_GAMMA_PER_STOP, p->push_pull_stops);
      sp.film_morph_gamma_fast = p->film_gamma_factor_fast;
      sp.film_morph_gamma_slow = p->film_gamma_factor_slow;
      sp.film_morph_developer_exhaustion = p->film_developer_exhaustion;
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

    /* Compare what this edit was developed against with what is installed. Only
       when the edit actually recorded one -- a 0 means the field predates the
       edit, not that anything is wrong. */
    const uint32_t pack_lut = sf_pack_lut_hash(pack);
    if(p->lut_hash && pack_lut && p->lut_hash != pack_lut)
      /* Print the hash as well as the name. The name carries the spektrafilm
         version string, and that is not a reliable identifier: an editable dev
         install reports whatever pyproject.toml says, so two materially
         different checkouts can both call themselves the same thing. Without
         the hash the message reads as nonsense when they do. */
      snprintf(d->sim_warning, sizeof d->sim_warning,
               _("developed with a different spectral table\n"
                 "recorded: %08x    installed: %s (%08x)\n"
                 "the matching pack can be fetched with the button below"),
               p->lut_hash, sf_pack_lut_id(pack), pack_lut);

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

/* Widest Gaussian sigma (micrometres on film) one diffusion-filter stage will
   dispatch, or 0 when the stage is off / a no-op. Built from the same
   sf_diffusion_build_plan() the CPU and GPU paths run, so the ROI padding can
   never drift from the bank that is actually convolved. */
static float _diffusion_pad_sigma_um(const gboolean on, const int family, const float strength,
                                     const float warmth, const float scale)
{
  if(!on) return 0.0f;
  sf_diffusion_plan_t plan;
  if(!sf_diffusion_build_plan(family, strength, warmth, &plan) || plan.p_s <= 0.0f) return 0.0f;
  float smax = 0.0f;
  for(int j = 0; j < plan.n; j++) smax = fmaxf(smax, plan.sigma_um[j]);
  return smax * fmaxf(scale, 1e-3f);
}

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
                         /* SF_SCATTER_TAIL_MAX_UM is already the widest tail
                            component's sigma; SF_HALATION_PSF_SIGMAS is
                            sqrt(n_bounces) and belongs to the halation term
                            above, so applying it here over-padded scatter by
                            1.73x. Harmless but wasteful. */
                         ? SF_SCATTER_TAIL_MAX_UM * scat_scale * inv_um
                         : 0.0f;
  /* The widest of film-stage and print-stage diffusion determines the ROI
     padding — both must fit in the expanded tile. Take the widest component of
     the actual Gaussian bank each stage will dispatch rather than a single
     constant: the bloom scale differs by 2.6x across the four families (BPM
     380*2.5 um vs cinebloom 1000*2.5 um), so one constant either under-pads the
     wide families or over-pads the narrow ones. */
  const float diff = fmaxf(_diffusion_pad_sigma_um(p->diffusion_on,
                                                   (int)p->diffusion_filter_family,
                                                   p->diffusion_strength, p->diffusion_warmth,
                                                   p->diffusion_scale),
                           _diffusion_pad_sigma_um(p->print_diffusion_on,
                                                   (int)p->print_diffusion_filter_family,
                                                   p->print_diffusion_strength,
                                                   p->print_diffusion_warmth,
                                                   p->print_diffusion_scale))
                     * inv_um;
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
  /* scanner stage: already in pixels, so it does not go through inv_um */
  const float scan = fmaxf(p->scan_blur,
                           (p->scan_usm_amount > 0.0f) ? p->scan_usm_sigma : 0.0f);
  return fmaxf(fmaxf(fmaxf(hal, scat), fmaxf(diff, scan)), fmaxf(grain, coupler));
}

void modify_roi_in(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;
  const dt_iop_spektrafilm_data_t *const d = (const dt_iop_spektrafilm_data_t *)piece->data;
  if(!d) return;
  /* film_format_mm is the format's long-edge dimension; buf_in.width alone
     is the SHORT edge for portrait-oriented images (post-orientation), so
     using it directly here would under-scale pixel_um (and every halo/grain/
     halation size derived from it) by the aspect ratio for portrait shots. */
  const float full_long_edge
    = fmaxf(fmaxf((float)piece->buf_in.width, (float)piece->buf_in.height) * roi_out->scale, 1.0f);
  const float pixel_um = d->p.film_format_mm * 1000.0f / full_long_edge;
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
  /* see modify_roi_in: film_format_mm is the long-edge dimension */
  const float full_long_edge
    = fmaxf(fmaxf((float)piece->buf_in.width, (float)piece->buf_in.height) * roi_in->scale, 1.0f);
  const float pixel_um = d->p.film_format_mm * 1000.0f / full_long_edge;
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

  /* physical micrometres per pixel at this pipe resolution; film_format_mm
     is the long-edge dimension, see modify_roi_in */
  const float full_long_edge
    = fmaxf(fmaxf((float)piece->buf_in.width, (float)piece->buf_in.height) * roi_in->scale, 1.0f);
  const float pixel_um = d->p.film_format_mm * 1000.0f / full_long_edge;
  /* Fixed pixel radii (grain clumps, the two unsharp masks, the glare veil) were
     validated at export resolution; darktable's preview pipe renders the same
     image smaller, where the same nominal radius covers much more real scene
     detail. Shrink them there, but never grow them past 1:1. */
  const float preview_scale = fminf(roi_in->scale, 1.0f);

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
    /* per-film scatter PSF; clamped so a pack cannot outrun the ROI padding */
    double sc_core[3], sc_tail[3], sc_w[3];
    sf_sim_scatter_params(sim, sc_core, sc_tail, sc_w);
    for(int c = 0; c < 3; c++)
    {
      sc_core[c] = fmin(sc_core[c], (double)SF_SCATTER_CORE_CLAMP_UM);
      sc_tail[c] = fmin(sc_tail[c], (double)SF_SCATTER_TAIL_CLAMP_UM);
    }
    sf_halation(plane, w, h, (double)pixel_um, sc_core, sc_tail, sc_w, d->p.scatter_amount,
                d->p.scatter_scale, d->p.halation_amount, d->p.halation_scale, hal_strength,
                hal_sigma_um);
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
        if(csigma > 0.1f) sf_blur_plane3_fast(tmp, w, h, csigma, scratch);
        for(size_t i = 0; i < npix * 3; i++) mix[i] = wbase * tmp[i];
        for(int g3 = 0; g3 < 3; g3++)
        {
          memcpy(tmp, corr, sizeof(float) * npix * 3);
          const float ts = rat[g3] * tail_px;
          if(ts > 0.1f) sf_blur_plane3_fast(tmp, w, h, ts, scratch);
          const float wk = (float)ctail_w * amp[g3];
          for(size_t i = 0; i < npix * 3; i++) mix[i] += wk * tmp[i];
        }
        memcpy(corr, mix, sizeof(float) * npix * 3);
      }
      else if(csigma > 0.1f)
        sf_blur_plane3_fast(corr, w, h, csigma, scratch); /* alloc failed: core only */
      dt_free_align(mix);
      dt_free_align(tmp);
    }
    else if(csigma > 0.1f)
      sf_blur_plane3_fast(corr, w, h, csigma, scratch);
  }
  sf_sim_develop(sim, plane, couplers ? corr : NULL, plane, npix, 3, 3);

  /* 4) grain on the developed CMY film density: sample a grained density,
        take its difference from the clean one, scale by strength, add it
        back, then clump-blur the combined field and recover acutance with
        the multiplicative unsharp mask (upstream's blur/usm pair) */
  if(d->p.grain_on && d->p.grain_amount > 0.0f)
  {
    float *gbuf = corr; /* corr is free now — reuse as the grain delta buffer */
    const int roi_x = roi_in->x, roi_y = roi_in->y;
    const float amount = d->p.grain_amount;
    const int mono = sf_sim_film_bw(sim); /* B&W: achromatic grain */
    /* sf_grain_delta_ml's layer_npart is precomputed at sf_sim_build time
       against the fixed SF_GRAIN_REF_UM reference scale (it depends on
       curve/coupler state baked in at build time, not just resolution);
       rescale it live to the real pixel_um here. */
    const float npart_scale = (pixel_um * pixel_um) / (SF_GRAIN_REF_UM * SF_GRAIN_REF_UM);
    /* SF_GRAIN_BLUR_FACTOR/SF_GRAIN_DYE_BLUR_UM/grain_usm_sigma are fixed
       pixel radii, validated against upstream at whatever single
       resolution each of its own renders happens to use -- upstream has
       no notion of "the same image, but at a temporarily reduced preview
       resolution for interactive editing speed" the way darktable's
       preview pipe does. Without this, the SAME nominal pixel radius
       covers a much larger fraction of real scene detail in a
       downscaled preview than at full/export resolution (a real image
       edge that spans ~30px at full res might span ~4px in a heavily
       zoomed-out preview), producing visible over-sharpening ringing on
       actual scene content, not just grain texture, that isn't present
       at 1:1/export. Capped at 1.0 so zooming in PAST 100% doesn't grow
       the radii beyond what was actually validated. Particle density
       (npart_scale above) is NOT touched by this -- it's correctly
       resolution-dependent via pixel_um already, confirmed against the
       reference at multiple different resolutions. */
    float grms[3], gunif[3], gdmin[3];
    /* gdmin here is what the SAMPLER adds -- the sum of the per-sub-layer
       floors, not the film's single density_min. They coincide for a
       single-layer stock and differ for a multi-sub-layer one; using
       density_min there gave the delta a constant positive mean. */
    sf_sim_grain_dmin_total(sim, gdmin);
    float gdmin_unused[3];
    sf_sim_film_grain3(sim, grms, gunif, gdmin_unused); /* per-film catalogue grain
                                      (rms-granularity, uniformity, density
                                      floor) — Portra 400 no longer shares
                                      Tri-X's grain signature */
    sf_grain_layers_t layers;
    sf_sim_grain_layers(sim, &layers); /* n==1 for a single-layer curve fit
                                      is already valid data (see the
                                      function's own comment): every stock
                                      goes through this one mechanism. */
    const int nsub = layers.n;

    /* Upstream's per-sub-layer dye-cloud blur (layer_particle_model's
       blur_particle, grain.py) runs INSIDE the particle sampler, on each
       sub-layer's raw draw independently, before the main clump blur
       below ever sees it. Its sigma depends only on that sub-layer's own
       per-particle optical density (dmax/npart) -- constant across the
       whole image for a given (channel, sub-layer), so compute it once
       here rather than per pixel. */
    float dye_sigma[3][SF_GRAIN_MAX_SUBLAYERS];
    for(int c = 0; c < 3; c++)
      for(int sl = 0; sl < nsub; sl++)
      {
        const float npart_c = (float)layers.layer_npart[sl][c] * npart_scale;
        const float od_particle = (float)layers.layer_dmax[sl][c] / fmaxf(npart_c, 1e-6f);
        dye_sigma[c][sl] = SF_GRAIN_DYE_BLUR_UM * sqrtf(fmaxf(od_particle, 0.0f)) * preview_scale;
      }

    float *raw[SF_GRAIN_MAX_SUBLAYERS] = { 0 };
    gboolean raw_ok = TRUE;
    for(int sl = 0; sl < nsub; sl++)
    {
      raw[sl] = dt_alloc_align_float(npix);
      if(!raw[sl]) raw_ok = FALSE;
    }

    if(!raw_ok)
      memset(gbuf, 0, npix * 3 * sizeof(float)); /* allocation failed: skip grain gracefully */
    else
    {
      const int n_out_ch = mono ? 1 : 3;
      for(int oc = 0; oc < n_out_ch; oc++)
      {
        const int channel_idx = mono ? 1 : oc; /* mono uses channel 1's curve/params for the
                                                    achromatic draw, matching sf_grain_delta_ml */
        const int seed_ch = mono ? 0 : oc; /* mono seeds as sl*10 (no channel term), matching
                                               sf_grain_delta_ml's own convention exactly */
        const float unif_ch = gunif[channel_idx];
#ifdef _OPENMP
#pragma omp parallel for default(none)                                                          \
    shared(plane, raw, layers) firstprivate(w, npix, roi_x, roi_y, mono, channel_idx, seed_ch,   \
                                            unif_ch, npart_scale, nsub) schedule(static)
#endif
        for(size_t k = 0; k < npix; k++)
        {
          const int x = (int)(k % (size_t)w), y = (int)(k / (size_t)w);
          const float density = mono ? (plane[k * 3 + 0] + plane[k * 3 + 1] + plane[k * 3 + 2])
                                           / 3.0f
                                     : plane[k * 3 + channel_idx];
          float samp[SF_GRAIN_MAX_SUBLAYERS];
          sf_grain_raw_samples_ml(&layers, density, channel_idx, seed_ch, (uint32_t)(x + roi_x),
                                  (uint32_t)(y + roi_y), unif_ch, npart_scale, samp);
          for(int sl = 0; sl < nsub; sl++) raw[sl][k] = samp[sl];
        }
        /* dye-cloud blur: each sub-layer independently, no variance
           restoration (matching upstream: layer_particle_model doesn't
           renormalize after its blur_particle pass either). */
        for(int sl = 0; sl < nsub; sl++)
          sf_blur_plane1(raw[sl], w, h, dye_sigma[channel_idx][sl], NULL, scratch);
        /* combine: sum sub-layers, subtract the density floor and the
           original clean density, scale by strength. */
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(plane, gbuf, raw)                                  \
    firstprivate(npix, nsub, channel_idx, mono, oc, amount, gdmin) schedule(static)
#endif
        for(size_t k = 0; k < npix; k++)
        {
          float total = 0.0f;
          for(int sl = 0; sl < nsub; sl++) total += raw[sl][k];
          const float g = total - gdmin[channel_idx];
          const float density = mono ? (plane[k * 3 + 0] + plane[k * 3 + 1] + plane[k * 3 + 2])
                                           / 3.0f
                                     : plane[k * 3 + channel_idx];
          const float delta = (g - density) * amount;
          if(mono) gbuf[k * 3 + 0] = gbuf[k * 3 + 1] = gbuf[k * 3 + 2] = delta;
          else gbuf[k * 3 + oc] = delta;
        }
      }
    }
    for(int sl = 0; sl < nsub; sl++)
      if(raw[sl]) dt_free_align(raw[sl]);
    /* No DC-centring pass. The delta is zero-mean by construction now: the
       sampler draws an unbiased Poisson (spektra_core.h) and the combine above
       takes back exactly the floors the sampler added -- see
       sf_sim_grain_dmin_total(). Subtracting grain_density_min there instead
       left a constant +(sum - density_min) per unit strength, which a per-ROI
       mean was previously hiding. */
    /* Add the still-UNBLURRED delta onto the clean density first, so the clump
       blur below runs on the grained ABSOLUTE density -- image detail and grain
       together -- which is what upstream blurs (_finalize_grain in grain.py
       smooths density_cmy_out itself, not a separate grain layer).

       Blurring only the delta and adding it to an untouched clean signal leaves
       real image detail at full sharpness, and the multiplicative unsharp mask
       further down then sharpens it anyway. That mask exists solely to recover
       the acutance this blur takes away: the two are a tuned pair
       (params_schema.py annotates the blur "optimized to go with the mult usm
       below" and the usm "optimized to go with the blur above"). Running the
       recovery half without the loss half is over-sharpening by construction,
       and showed up as crunchy, over-defined edges on fine high-contrast
       texture. Softening genuine detail here is intended, not a side effect --
       it is what the emulsion does, and what upstream's own output shows. */
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(plane, gbuf) firstprivate(npix)              \
    schedule(static)
#endif
    for(size_t k = 0; k < npix * 3; k++) plane[k] += gbuf[k];
    /* Upstream's grain blur (GrainParams.blur, params_schema.py) is a
       literal FIXED pixel sigma (0.8), independent of pixel_um/resolution/
       film_format_mm -- confirmed empirically: measuring the real
       reference's noise autocorrelation at two different resolutions
       (87.5 and 35 um/px) gave near-identical radial profiles. Physical
       scaling lives entirely in particle DENSITY (now pixel_um-driven
       above), not in this smoothing pass. SF_GRAIN_SIZE_CAL is gone: it
       was calibrated against the old pixel_um-scaled formula and no
       longer applies. preview_scale is a SEPARATE, darktable-only
       correction (see its own comment above): upstream always renders
       one real resolution, but darktable's preview pipe renders the same
       image at a temporarily reduced resolution for interactive speed,
       so this fixed radius needs shrinking there or it over-affects real
       scene detail relative to what 1:1/export shows. */
    const float sigma = SF_GRAIN_BLUR_FACTOR * fmaxf(d->p.grain_size, SF_GRAIN_SIZE_MIN)
                         * preview_scale;
    /* No variance-restoration renorm here -- upstream's own grain
       finalization (_finalize_grain in grain.py) has none either; it just
       blurs and lets the natural contrast reduction stand, matching real
       optical clumping. Restoring full pre-blur variance made grain
       visibly higher-contrast, and therefore visually coarser, than
       upstream at any matching sigma. */
    sf_blur_plane3(plane, w, h, sigma, scratch);
    /* Acutance recovery for the blur above, and only meaningful because of it:
       these two are tuned together (defaults sigma 0.7 / amount 1.5). */
    if(d->p.grain_usm_sigma > 0.0f && d->p.grain_usm_amount > 0.0f)
      /* The reference's multiplicative USM (_finalize_grain in grain.py)
         runs on the ABSOLUTE density -- the grain sampler's floor is still
         present and is only removed afterwards (add_micro_structure -> blur
         -> USM -> -= density_min). The delta combine above already took the
         floor back out, so pass it back in here: without it the D/blur(D)
         ratio is ill-conditioned in the deepest shadows and the USM
         amplifies shadow noise the reference never does. */
      sf_multiplicative_unsharp_mask3(plane, w, h, d->p.grain_usm_sigma * preview_scale,
                                       d->p.grain_usm_amount, gdmin, corr, scratch);
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

  /* 6b) scanner optics + viewing glare, on the scanned RGB over the full padded
         ROI so the crop below never sees an edge artifact ([sc] ScannerParams
         lens_blur / unsharp_mask, [gl] add_glare -- the reference skips glare
         when the film itself is scanned rather than printed). */
  if(d->p.scan_blur > 0.0f)
    sf_blur_plane3(plane, w, h, d->p.scan_blur * preview_scale, scratch);
  if(d->p.scan_usm_sigma > 0.0f && d->p.scan_usm_amount > 0.0f)
    sf_unsharp_mask3(plane, w, h, d->p.scan_usm_sigma * preview_scale, d->p.scan_usm_amount,
                     corr, scratch);
  if(!d->p.scan_film && d->p.glare_percent > 0.0f)
    sf_glare(plane, w, h, d->p.glare_percent, SF_GLARE_ROUGHNESS,
             SF_GLARE_BLUR_PX * preview_scale, roi_in->x, roi_in->y, scratch);

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
/* Separable Young-van Vliet pass on the device, the GPU half of
   sf_gauss_yvv_coeffs / _sf_gauss_iir_1d. `tmp` is a scratch buffer of the same
   shape as `buf`. This replaces dt_gaussian_mean_blur_cl, which was being fed
   sigma * SF_GAUSS_SIGMA_CORRECTION -- a factor measured for the CPU's old
   Deriche recursion and meaningless for darktable's iterated-box blur, so the
   two paths were producing different halo widths for the same sigma. */
static cl_int _sf_yvv_blur_cl(const int devid, dt_iop_spektrafilm_global_data_t *gd,
                              cl_mem buf, cl_mem tmp, const int w, const int h,
                              const float sigma, const int ch)
{
  float b[4];
  sf_gauss_yvv_coeffs(sigma, b);
  const int krow = (ch == 4) ? gd->kernel_yvv_row_4c : gd->kernel_yvv_row_1c;
  const int kcol = (ch == 4) ? gd->kernel_yvv_col_4c : gd->kernel_yvv_col_1c;
  cl_int e = dt_opencl_enqueue_kernel_2d_args(devid, krow, h, 1, CLARG(buf), CLARG(tmp),
                                              CLARG(w), CLARG(h), CLARG(b[0]), CLARG(b[1]),
                                              CLARG(b[2]), CLARG(b[3]));
  if(e != CL_SUCCESS) return e;
  return dt_opencl_enqueue_kernel_2d_args(devid, kcol, w, 1, CLARG(tmp), CLARG(buf),
                                          CLARG(w), CLARG(h), CLARG(b[0]), CLARG(b[1]),
                                          CLARG(b[2]), CLARG(b[3]));
}

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

  /* film_format_mm is the long-edge dimension, see modify_roi_in */
  const float full_long_edge
    = fmaxf(fmaxf((float)piece->buf_in.width, (float)piece->buf_in.height) * roi_in->scale, 1.0f);
  const float pixel_um = d->p.film_format_mm * 1000.0f / full_long_edge;
  /* see the matching comment in process(): fixed pixel radii shrink with the
     preview pipe's reduced resolution, but never grow past 1:1 */
  const float preview_scale = fminf(roi_in->scale, 1.0f);

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
/* Same as SF_GAUSS_BLUR4, but above SF_GAUSS_EXACT_MAX_SIGMA falls back to
   the shared Young-van Vliet recursion (_sf_yvv_blur_cl above)
   instead of the exact row/col kernels: for callers with no downstream
   dependency on the exact kernel's shape (unlike grain's SF_GAUSS_BLUR4
   above, which stays on the exact path unconditionally -- the fast
   recursive approximation's own known ~18% effective-width error would
   reintroduce the same size mismatch against upstream that the exact
   kernel was adopted to fix), this recovers most of the O(radius) cost the
   exact kernel pays at large sigma. */
#define SF_GAUSS_BLUR4_FAST(buf, _sg, label) do { \
    if(err == CL_SUCCESS) \
    { \
      if((_sg) >= SF_GAUSS_EXACT_MAX_SIGMA) \
        err = _sf_yvv_blur_cl(devid, gd, (buf), gtmp4, w, h, (_sg), 4); \
      else \
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
    } \
    SF_CL_STEP(label); \
  } while(0)
/* loop-safe variant: sets err, caller checks err/breaks; src/dst may differ
   (e.g. accumulating several blurred copies of the same source). Falls back
   to the fast recursive blur above SF_GAUSS_EXACT_MAX_SIGMA, same rationale
   as SF_GAUSS_BLUR4_FAST -- none of this macro's callers (halation bounce,
   coupler tail, both diffusion filters) renormalize against the exact
   kernel's shape. */
#define SF_GAUSS_BLUR4_OP_L(src, dst, _sg) do { \
    if(err == CL_SUCCESS) \
    { \
      if((_sg) >= SF_GAUSS_EXACT_MAX_SIGMA) \
      { \
        err = dt_opencl_enqueue_copy_buffer_to_buffer(devid, (src), (dst), 0, 0, npix * f * 4); \
        if(err == CL_SUCCESS) \
          err = _sf_yvv_blur_cl(devid, gd, (dst), gtmp4, w, h, (_sg), 4); \
      } \
      else \
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
    } \
  } while(0)
/* single-channel in-place blur (scatter stage only, on plane1). Same fast
   fallback above SF_GAUSS_EXACT_MAX_SIGMA -- scatter's core/tail sigmas are
   normally small (sub-few-px), but the fallback is here for whatever a user's
   scatter_scale slider can push them to. */
#define SF_GAUSS_BLUR1_L(buf, _sg) do { \
    if(err == CL_SUCCESS) \
    { \
      if((_sg) >= SF_GAUSS_EXACT_MAX_SIGMA) \
        err = _sf_yvv_blur_cl(devid, gd, (buf), gtmp1, w, h, (_sg), 1); \
      else \
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
    /* The frame-maximum reduction that used to run here is gone: the curve is
       anchored to the exposure scale now, which is what makes the boost agree
       between the preview pipe, the export pipe and every tile. */
    const float b_ev = d->p.boost_ev, b_rng = d->p.boost_range, b_prot = d->p.protect_ev;
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_boost, w, h, CLARG(plane),
                                           CLARG(w), CLARG(h), CLARG(b_ev), CLARG(b_rng),
                                           CLARG(b_prot));
    SF_CL_STEP("boost");
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
      /* per-film scatter PSF, same clamps as the CPU path */
      float sc_core[3], sc_tail[3];
      for(int c = 0; c < 3; c++)
      {
        sc_core[c] = fminf(g->scatter_core_um[c], SF_SCATTER_CORE_CLAMP_UM);
        sc_tail[c] = fminf(g->scatter_tail_um[c], SF_SCATTER_TAIL_CLAMP_UM);
      }
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
      const float ws_r = g->scatter_tail_weight[0], ws_g = g->scatter_tail_weight[1],
                  ws_b = g->scatter_tail_weight[2];
      /* (1-s)*raw + s*scattered, matching sf_halation()'s CPU blend; `plane`
         doubles as both the pre-scatter `raw` input and the `out` write
         target -- safe since this is a purely per-pixel elementwise op. */
      /* convex blend weight -- see sf_halation() for why it cannot exceed 1 */
      const float s_amount = CLAMP(d->p.scatter_amount, 0.0f, 1.0f);
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
      SF_GAUSS_BLUR4_FAST(acc, csigma, "coupler blur");
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
    /* see the matching comment on the CPU path (process()) for the full
       rationale: darktable's preview pipe renders at a temporarily
       reduced resolution, unlike upstream which always renders one real
       resolution, so these fixed pixel radii need shrinking there to
       avoid over-affecting real scene detail. Capped at 1.0 so zooming
       in past 100% doesn't grow radii beyond what was validated. Does
       NOT apply to npart_scale below, which is correctly resolution-
       dependent via pixel_um already. */
    /* Unified through the multi-sublayer table for every stock (nsub can be
       1) rather than branching to a separate single-layer kernel -- see the
       matching comment in process()'s CPU path for why that's valid: the
       build-time layer table already has correct n==1 data for single-layer
       stocks. Built once per d->gpu (see d->grain_cl_built_for above)
       rather than re-uploaded on every process_cl() call: tiled processing
       calls this once per tile, and this data never changes between tiles
       of the same image, so re-uploading it per tile was pure overhead. */
    const int nsub = g->grain_n_sublayers, nle = SF_NLE, maxsub = SF_GRAIN_MAX_SUBLAYERS;
    if(d->grain_cl_built_for != g || d->grain_cl_devid != devid)
    {
      if(d->grain_cl_dmax) dt_opencl_release_mem_object(d->grain_cl_dmax);
      if(d->grain_cl_npart) dt_opencl_release_mem_object(d->grain_cl_npart);
      if(d->grain_cl_dmin) dt_opencl_release_mem_object(d->grain_cl_dmin);
      if(d->grain_cl_total) dt_opencl_release_mem_object(d->grain_cl_total);
      if(d->grain_cl_curve) dt_opencl_release_mem_object(d->grain_cl_curve);
      d->grain_cl_dmax = dt_opencl_copy_host_to_device_constant(
          devid, (size_t)maxsub * 3 * f, (void *)g->grain_layer_dmax);
      d->grain_cl_npart = dt_opencl_copy_host_to_device_constant(
          devid, (size_t)maxsub * 3 * f, (void *)g->grain_layer_npart);
      d->grain_cl_dmin = dt_opencl_copy_host_to_device_constant(
          devid, (size_t)maxsub * 3 * f, (void *)g->grain_layer_dmin);
      d->grain_cl_total = dt_opencl_copy_host_to_device_constant(
          devid, (size_t)nle * 3 * f, (void *)g->grain_layer_curve_total);
      d->grain_cl_curve = dt_opencl_copy_host_to_device_constant(
          devid, (size_t)nle * maxsub * 3 * f, (void *)g->grain_layer_curve);
      d->grain_cl_built_for = g;
      d->grain_cl_devid = devid;
    }
    if(!d->grain_cl_dmax || !d->grain_cl_npart || !d->grain_cl_dmin || !d->grain_cl_total
       || !d->grain_cl_curve)
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    else
    {
      /* layer_npart (uploaded above) is precomputed at sf_sim_build time
         against the fixed SF_GRAIN_REF_UM reference scale; rescale it live
         to the real pixel_um here. */
      const float npart_scale = (pixel_um * pixel_um) / (SF_GRAIN_REF_UM * SF_GRAIN_REF_UM);
      /* Upstream's per-sub-layer dye-cloud blur (layer_particle_model's
         blur_particle, grain.py): sigma depends only on that sub-layer's
         own per-particle optical density (dmax/npart), which is constant
         across the whole image -- computed host-side once here, same as
         the CPU path, rather than per-pixel on the device. */
      float dye_sigma[3][SF_GRAIN_MAX_SUBLAYERS];
      for(int c = 0; c < 3; c++)
        for(int sl = 0; sl < nsub; sl++)
        {
          const float npart_c = g->grain_layer_npart[sl][c] * npart_scale;
          const float od_particle = g->grain_layer_dmax[sl][c] / fmaxf(npart_c, 1e-6f);
          dye_sigma[c][sl] = SF_GRAIN_DYE_BLUR_UM * sqrtf(fmaxf(od_particle, 0.0f)) * preview_scale;
        }

      cl_mem raw_buf[SF_GRAIN_MAX_SUBLAYERS] = { NULL };
      cl_mem acc_buf = dt_opencl_alloc_device_buffer(devid, npix * f);
      gboolean raw_ok = acc_buf != NULL;
      for(int sl = 0; sl < nsub; sl++)
      {
        raw_buf[sl] = dt_opencl_alloc_device_buffer(devid, npix * f);
        if(!raw_buf[sl]) raw_ok = FALSE;
      }
      if(!raw_ok)
        err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      else
      {
        const int n_out_ch = mono ? 1 : 3;
        for(int oc = 0; oc < n_out_ch && err == CL_SUCCESS; oc++)
        {
          const int channel_idx = mono ? 1 : oc;
          const int seed_ch = mono ? 0 : oc;
          const float unif_ch = g->grain_uniformity[channel_idx];
          for(int sl = 0; sl < nsub && err == CL_SUCCESS; sl++)
          {
            err = dt_opencl_enqueue_kernel_2d_args(
                devid, gd->kernel_grain_gen_raw_sl, w, h, CLARG(plane2), CLARG(raw_buf[sl]),
                CLARG(w), CLARG(h), CLARG(roi_x), CLARG(roi_y), CLARG(mono), CLARG(channel_idx),
                CLARG(seed_ch), CLARG(sl), CLARG(nle), CLARG(maxsub), CLARG(unif_ch),
                CLARG(npart_scale), CLARG(d->grain_cl_dmax), CLARG(d->grain_cl_npart),
                CLARG(d->grain_cl_dmin), CLARG(d->grain_cl_total), CLARG(d->grain_cl_curve));
            if(err == CL_SUCCESS && dye_sigma[channel_idx][sl] > 1e-6f)
              SF_GAUSS_BLUR1_L(raw_buf[sl], dye_sigma[channel_idx][sl]);
          }
          for(int sl = 0; sl < nsub && err == CL_SUCCESS; sl++)
          {
            const int reset = sl == 0;
            err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_grain_accumulate_1c, w, h,
                                                   CLARG(acc_buf), CLARG(raw_buf[sl]), CLARG(w),
                                                   CLARG(h), CLARG(reset));
          }
          if(err == CL_SUCCESS)
          {
            const float dmin_ch = g->grain_dmin[channel_idx];
            err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_grain_finalize_channel, w, h,
                                                   CLARG(tmpa), CLARG(acc_buf), CLARG(plane2),
                                                   CLARG(w), CLARG(h), CLARG(mono),
                                                   CLARG(channel_idx), CLARG(oc), CLARG(dmin_ch),
                                                   CLARG(amount));
          }
        }
      }
      for(int sl = 0; sl < nsub; sl++)
        if(raw_buf[sl]) dt_opencl_release_mem_object(raw_buf[sl]);
      if(acc_buf) dt_opencl_release_mem_object(acc_buf);
    }
    SF_CL_STEP("grain gen");
    /* The DC-centring reduction is gone along with its CPU counterpart: the
       Poisson sampler is unbiased, so there is nothing to centre. That also
       retires the full device->host readback it needed, which stalled the queue
       once per grain stage. */
    /* Add the still-UNBLURRED delta, so the blur below sees the grained
       absolute density rather than an isolated grain layer -- same ordering as
       process(), see the long comment there for why the blur and the unsharp
       mask that follows it only make sense as a pair. */
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_grain_add, w, h, CLARG(plane2),
                                           CLARG(tmpa), CLARG(w), CLARG(h));
    SF_CL_STEP("grain add");
    /* fixed pixel sigma, matching process()'s CPU-side fix -- see comment
       there for the empirical validation. */
    const float gsigma = SF_GRAIN_BLUR_FACTOR * fmaxf(d->p.grain_size, SF_GRAIN_SIZE_MIN)
                          * preview_scale;
    SF_GAUSS_BLUR4(plane2, gsigma, "grain blur");
    if(d->p.grain_usm_sigma > 0.0f && d->p.grain_usm_amount > 0.0f)
    {
      err = dt_opencl_enqueue_copy_buffer_to_buffer(devid, plane2, acc, 0, 0, npix * f * 4);
      if(err != CL_SUCCESS) goto cleanup;
      const float usig = d->p.grain_usm_sigma * preview_scale;
      SF_GAUSS_BLUR4_FAST(plane2, usig, "grain USM blur");
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_grain_usm, w, h, CLARG(plane2),
                                             CLARG(acc), CLARG(w), CLARG(h),
                                             CLARG(d->p.grain_usm_amount),
                                             CLARG(g->grain_dmin[0]), CLARG(g->grain_dmin[1]),
                                             CLARG(g->grain_dmin[2]));
      SF_CL_STEP("grain USM");
    }
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

  /* ---- 6) scan over the full padded ROI into `plane` (free since print) ---- */
  err = dt_opencl_enqueue_kernel_2d_args(
      devid, gd->kernel_scan, w, h, CLARG(plane2), CLARG(plane), CLARG(w), CLARG(h),
      CLARG(sl_cl), CLARG(sx_cl), CLARG(sy_cl),
      CLARG(sz_cl), CLARG(sn_cl), CLARG(sm_cl), CLARG(steps), CLARG(g->scan_lo[0]),
      CLARG(g->scan_lo[1]), CLARG(g->scan_lo[2]), CLARG(g->scan_hi[0]), CLARG(g->scan_hi[1]),
      CLARG(g->scan_hi[2]), CLARG(mats_cl), CLARG(cm_cl), CLARG(g->cmax_nl), CLARG(g->cmax_nh),
      CLARG(g->out_compress), CLARG(g->out_luminance_boost), CLARG(g->scan_bw_on), CLARG(g->scan_bw_m),
      CLARG(g->scan_bw_q));
  SF_CL_STEP("scan");

  /* ---- 6b) scanner optics + viewing glare (mirrors process()) -------------- */
  if(d->p.scan_blur > 0.0f)
  {
    SF_GAUSS_BLUR4(plane, d->p.scan_blur * preview_scale, "scanner blur");
  }
  if(d->p.scan_usm_sigma > 0.0f && d->p.scan_usm_amount > 0.0f)
  {
    err = dt_opencl_enqueue_copy_buffer_to_buffer(devid, plane, acc, 0, 0, npix * f * 4);
    if(err != CL_SUCCESS) goto cleanup;
    SF_GAUSS_BLUR4_FAST(plane, d->p.scan_usm_sigma * preview_scale, "scanner USM blur");
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_scan_usm, w, h, CLARG(plane),
                                           CLARG(acc), CLARG(w), CLARG(h),
                                           CLARG(d->p.scan_usm_amount));
    SF_CL_STEP("scanner USM");
  }
  if(!d->p.scan_film && d->p.glare_percent > 0.0f)
  {
    const float gmean = d->p.glare_percent * 0.01f;
    const float sigma2 = logf(1.0f + SF_GLARE_ROUGHNESS * SF_GLARE_ROUGHNESS);
    const float gs = sqrtf(sigma2), gbias = -0.5f * sigma2;
    const int roi_x = roi_in->x, roi_y = roi_in->y;
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_glare_gen, w, h, CLARG(tmpa),
                                           CLARG(w), CLARG(h), CLARG(roi_x), CLARG(roi_y),
                                           CLARG(gmean), CLARG(gs), CLARG(gbias));
    SF_CL_STEP("glare gen");
    SF_GAUSS_BLUR4(tmpa, SF_GLARE_BLUR_PX * preview_scale, "glare blur");
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_glare_add, w, h, CLARG(plane),
                                           CLARG(tmpa), CLARG(w), CLARG(h));
    SF_CL_STEP("glare add");
  }

  /* ---- 7) crop the roi_out window into dev_out ----------------------------- */
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_crop_out, ow, oh, CLARG(plane),
                                         CLARG(dev_in), CLARG(dev_out), CLARG(w), CLARG(ow),
                                         CLARG(oh), CLARG(ox), CLARG(oy));
  SF_CL_STEP("crop out");

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
  g_list_free_full(g->entries, g_free);
  g->entries = _scan_profiles(NULL);
}

/* Entry at a list position, or NULL. The comboboxes carry the position as their
   data, so this is how a user selection resolves back to a profile. */
static const sf_prof_entry_t *_entry_at(const dt_iop_spektrafilm_gui_data_t *g, const int pos)
{
  return (pos >= 0) ? g_list_nth_data(g->entries, pos) : NULL;
}

static void _update_print_sensitivity(dt_iop_module_t *self);
static void _update_development_sensitivity(const dt_iop_spektrafilm_gui_data_t *g,
                                            const dt_iop_spektrafilm_params_t *p);
static float _development_default(const sf_prof_entry_t *e);

/* forward: needs _entry_by_hash(), which is defined below with the rest of the
   stock lookups */
static void _update_paper_auto_entry(dt_iop_module_t *self);

static void _film_changed(GtkWidget *w, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_spektrafilm_gui_data_t *g = (dt_iop_spektrafilm_gui_data_t *)self->gui_data;
  dt_iop_spektrafilm_params_t *p = (dt_iop_spektrafilm_params_t *)self->params;
  const sf_prof_entry_t *e
      = _entry_at(g, GPOINTER_TO_INT(dt_bauhaus_combobox_get_data(g->film)));
  if(!e) return;
  p->film_hash = e->hash;
  /* A development time from the previous stock means nothing here -- the times
     differ per stock, and a stale value can sit above the new stock's longest
     (Double-X at 12 min, then 2302's family topping out at 9). Land on the new
     stock's own default, so the slider always shows a time it actually has. */
  p->development_min = _development_default(e);
  DT_ENTER_GUI_UPDATE();
  dt_bauhaus_slider_set(g->development_min, p->development_min);
  DT_LEAVE_GUI_UPDATE();
  /* A positive/reversal stock has no print stage, so its natural mode is
     scan_film. Point the widget's reset target at that, so a reset gesture on
     the checkbox lands on what THIS film wants rather than the compiled
     default. */
  dt_bauhaus_toggle_set_default(g->scan_film, e->positive);
  /* scan-film follows the film's natural mode on a film switch: slides and
     reversal stocks are viewed directly (scan), negatives go through the
     print stage. The user can still toggle freely afterwards -- this only
     re-baselines when the film itself changes, like the paper auto-follow. */
  if(p->scan_film != e->positive)
  {
    p->scan_film = e->positive;
    DT_ENTER_GUI_UPDATE();
    dt_bauhaus_toggle_set(g->scan_film, p->scan_film);
    DT_LEAVE_GUI_UPDATE();
    _update_print_sensitivity(self);
  }
  /* On "auto" (hash 0) the paper follows the film's target print, and the
     combobox keeps reading "auto" rather than jumping to the resolved stock:
     the state is the link, not the destination, and selecting a specific paper
     while the hash says auto made the two disagree. The entry names the stock
     it resolves to instead. Done on every film change, not only while auto is
     selected, so the entry is already correct if the paper is set back to auto
     later. The pipeline resolves it identically either way (_resolve_stock). */
  _update_paper_auto_entry(self);
  /* last, once scan_film and the auto-followed paper have settled: both
     development sliders are gated on their own stock, and the print one also on
     there being a print stage at all */
  _update_development_sensitivity(g, p);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _paper_changed(GtkWidget *w, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_spektrafilm_gui_data_t *g = (dt_iop_spektrafilm_gui_data_t *)self->gui_data;
  dt_iop_spektrafilm_params_t *p = (dt_iop_spektrafilm_params_t *)self->params;
  const int ppos = GPOINTER_TO_INT(dt_bauhaus_combobox_get_data(g->paper));
  if(ppos < 0)
  {
    /* back to auto: drop the explicit choice so the film resolves it again */
    p->paper_hash = 0;
    p->print_development_min = 0.0f;
    _update_development_sensitivity(g, p);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    return;
  }
  const sf_prof_entry_t *pe = _entry_at(g, ppos);
  if(!pe) return;
  p->paper_hash = pe->hash;
  /* same as _film_changed: a time from the previous paper does not transfer */
  p->print_development_min = _development_default(pe);
  DT_ENTER_GUI_UPDATE();
  dt_bauhaus_slider_set(g->print_development_min, p->print_development_min);
  DT_LEAVE_GUI_UPDATE();
  _update_development_sensitivity(g, p);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

/* Entry for a stock hash, or NULL. `printing` disambiguates, since the same
   stock name can exist as both a film and a paper. */
static const sf_prof_entry_t *_entry_by_hash(const dt_iop_spektrafilm_gui_data_t *g,
                                             const uint32_t hash, const gboolean printing)
{
  for(const GList *l = g->entries; l; l = l->next)
  {
    const sf_prof_entry_t *e = l->data;
    if(e->hash == hash && e->printing == printing) return e;
  }
  return NULL;
}

/* The film the pipeline is rendering with: the stock this edit names, or -- for
   a hash of 0, or a stock that has since left the pack -- the same fallback
   gui_update() selects the combobox on, so the label below never names a film
   the combobox does not show. */
static const sf_prof_entry_t *_current_film_entry(const dt_iop_spektrafilm_gui_data_t *g,
                                                  const dt_iop_spektrafilm_params_t *p)
{
  const sf_prof_entry_t *hit = _entry_by_hash(g, p->film_hash, FALSE);
  if(p->film_hash && hit) return hit;
  const sf_prof_entry_t *fallback = NULL;
  for(const GList *l = g->entries; l; l = l->next)
  {
    const sf_prof_entry_t *e = l->data;
    if(e->printing) continue;
    if(!fallback || !strcmp(e->stock, "kodak_portra_400")) fallback = e;
  }
  return fallback;
}

/* The paper the pipeline actually prints on while "auto" is selected: the film's
   own target print when the pack ships it as a paper, otherwise the first print
   stock in the list. That second half mirrors _resolve_stock()'s own last
   resort, and is what makes the label honest -- stopping at target_print left
   two cases reading "follow film stock" while the pipeline was quietly printing
   on a specific paper anyway: a film that names no target print, and Double-X,
   whose target 2302 the pack exports as a film rather than as a paper. */
static const sf_prof_entry_t *_auto_paper_entry(const dt_iop_spektrafilm_gui_data_t *g,
                                                const sf_prof_entry_t *film)
{
  const sf_prof_entry_t *first = NULL;
  for(const GList *l = g->entries; l; l = l->next)
  {
    const sf_prof_entry_t *pe = l->data;
    if(!pe->printing) continue;
    if(!first) first = pe;
    if(film && film->target_print[0] && !strcmp(pe->stock, film->target_print)) return pe;
  }
  return first;
}

/* Name the resolved paper in the "auto" entry itself rather than only in the
   tooltip. The combobox deliberately keeps reading "auto" while it follows a
   film (the state is the link, not the destination), but that left the paper
   actually being printed on invisible unless you hovered. Renaming the entry
   shows both at once and keeps the link intact -- the selection does not move,
   only its text changes, so paper_hash stays 0.

   With scan_film there is no print stage at all, and the widget is insensitive:
   the entry goes blank rather than naming a paper nothing will be printed on.
   Called from _update_print_sensitivity() as well as on a film change, so it
   follows that toggle. */
static void _update_paper_auto_entry(dt_iop_module_t *self)
{
  dt_iop_spektrafilm_gui_data_t *g = (dt_iop_spektrafilm_gui_data_t *)self->gui_data;
  const dt_iop_spektrafilm_params_t *p = (const dt_iop_spektrafilm_params_t *)self->params;
  if(!g || !g->paper) return;
  char label[SF_NAME_LEN + 32];
  char tip[SF_NAME_LEN + 256];
  const sf_prof_entry_t *re
      = p->scan_film ? NULL : _auto_paper_entry(g, _current_film_entry(g, p));
  if(p->scan_film)
  {
    label[0] = '\0';
    g_strlcpy(tip, _("print paper. not used: the film is being scanned directly."),
              sizeof tip);
  }
  else if(re)
  {
    snprintf(label, sizeof label, _("auto (%s)"), re->name);
    snprintf(tip, sizeof tip,
             _("print paper. \"auto\" follows the film stock's own target print,\n"
               "currently %s. picking a paper pins it until you select auto again."),
             re->name);
  }
  else
  {
    /* no print stock in the pack at all -- the list below reads "(none)" */
    g_strlcpy(label, _("auto"), sizeof label);
    g_strlcpy(tip, _("print paper. \"auto\" follows the film stock's own target print."),
              sizeof tip);
  }
  /* Position 0: the auto entry is added before any section header, so its list
     index is fixed whatever paper groups the pack turns out to contain. */
  dt_bauhaus_combobox_set_entry_label(g->paper, 0, label);
  /* The widget renders the active entry's label straight out of that array at
     draw time, so a relabel needs nothing but a redraw -- and does need one. */
  gtk_widget_queue_draw(g->paper);
  gtk_widget_set_tooltip_text(g->paper, tip);
}

/* Default development time for a stock, in minutes: the representative middle
   member of its family, which is what select_development_time(None) picks. 0 when
   the stock is characterised at a single development. */
static float _development_default(const sf_prof_entry_t *e)
{
  return (e && e->n_dev > 1) ? (float)e->dev_times[(e->n_dev - 1) / 2] : 0.0f;
}

/* Point one development slider at one stock: sensitive only where that stock is
   characterised at more than one development time, spanning exactly the times it
   offers, and naming them -- they differ per stock and the value snaps to them,
   so a bare 0-15 range would be guesswork. */
static void _development_widget_update(GtkWidget *w, const sf_prof_entry_t *e)
{
  if(!w) return;
  const gboolean have = (e && e->n_dev > 1);
  gtk_widget_set_sensitive(w, have);

  float hi = 15.0f;
  if(have)
  {
    hi = (float)e->dev_times[0];
    for(int i = 1; i < e->n_dev; i++) hi = fmaxf(hi, (float)e->dev_times[i]);
  }
  /* 0 stays reachable at the bottom -- it means "this stock's own default" -- and
     the hard 15 min ceiling covers the widest family in the release (Double-X,
     12 min) with room to spare. */
  dt_bauhaus_slider_set_soft_range(w, 0.0f, hi);
  /* Reset gestures (double-click, scroll-reset) go to the widget's own default,
     which introspection set to the compiled 0. That renders correctly -- 0 means
     "this stock's default" -- but leaves the slider reading 0 instead of the time
     it resolved to, on the one discontinuity in the range. Point it at the real
     number for the stock in hand, so a reset shows 6.5 min on Double-X and 5 min
     on 2302 rather than 0. */
  dt_bauhaus_slider_set_default(w, _development_default(e));

  if(have)
  {
    char times[128] = { 0 };
    for(int i = 0; i < e->n_dev; i++)
    {
      char one[24];
      snprintf(one, sizeof one, "%s%.3g", i ? ", " : "", e->dev_times[i]);
      g_strlcat(times, one, sizeof times);
    }
    char tip[320];
    snprintf(tip, sizeof tip,
             _("development time, in minutes. snaps to the nearest time %s is\n"
               "characterised at: %s min. 0 uses the stock's own default (%.3g min)."),
             e->name, times, (double)_development_default(e));
    gtk_widget_set_tooltip_text(w, tip);
  }
  else
    gtk_widget_set_tooltip_text(w,
                                _("development time. this stock is characterised at a single\n"
                                  "development, so there is nothing to choose.\n\n"
                                  "single-emulsion B&W stocks are the ones that carry a family:\n"
                                  "Double-X at 4/5/6.5/9/12 min, print film 2302 at 2/3.5/5/7/9 min."));
}

/* Film and print are separate chemistries developed for separate times, so they
   get a slider each, pointed at their own stock. Lives here rather than in
   gui_update() because this is the one function every film / paper / scan_film
   change already routes through; in gui_update() alone it went stale the moment a
   stock was switched. */
static void _update_development_sensitivity(const dt_iop_spektrafilm_gui_data_t *g,
                                            const dt_iop_spektrafilm_params_t *p)
{
  _development_widget_update(g->development_min, _entry_by_hash(g, p->film_hash, FALSE));
  _development_widget_update(g->print_development_min,
                             p->scan_film ? NULL : _entry_by_hash(g, p->paper_hash, TRUE));
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
  gtk_widget_set_sensitive(g->print_diffusion_filter_family, printing && p->print_diffusion_on);
  gtk_widget_set_sensitive(g->print_diffusion_strength, printing && p->print_diffusion_on);
  gtk_widget_set_sensitive(g->print_diffusion_scale, printing && p->print_diffusion_on);
  gtk_widget_set_sensitive(g->print_diffusion_warmth, printing && p->print_diffusion_on);
  gtk_widget_set_sensitive(g->preflash_exposure, printing);
  gtk_widget_set_sensitive(g->preflash_m_shift, printing);
  gtk_widget_set_sensitive(g->preflash_y_shift, printing);
  /* toggle_from_params checkboxes keep showing their tick even when made
     insensitive -- GTK just dims the whole widget, so a checked-but-grayed
     box can read as "this is still on" when it has no effect at all (no
     print stage on positive/reversal film). Blank the tick while
     insensitive and restore the real value once re-enabled. Wrapped in
     DT_ENTER/LEAVE_GUI_UPDATE -- the same guard dt_iop_gui_update's own
     programmatic widget syncs rely on -- so this is purely visual and
     never writes back into the param. */
  DT_ENTER_GUI_UPDATE();
  dt_bauhaus_toggle_set(g->print_auto_exposure,
                        printing && p->print_auto_exposure);
  dt_bauhaus_toggle_set(g->print_diffusion_on,
                        printing && p->print_diffusion_on);
  DT_LEAVE_GUI_UPDATE();

  /* The auto entry names the paper in use, and with no print stage there is
     none: relabel from here, the one place every scan_film change passes. */
  _update_paper_auto_entry(self);

  /* Also from here: gui_changed() sends the scan_film toggle to this function and
     not to _toggle_sensitivity(), so without this the print development slider
     stayed live after switching to a scan-the-film workflow that has no print
     stage at all. */
  _update_development_sensitivity(g, p);
}

/* Grays out each effect's own sub-controls when its master "enable" toggle
   is off -- previously only the print-related controls (scan_film ->
   _update_print_sensitivity above) got this treatment; halation/grain/
   diffusion sliders stayed clickable-but-inert when their own toggle was
   unchecked, which reads as "these still do something" when they don't. */
static void _toggle_sensitivity(dt_iop_spektrafilm_gui_data_t *g,
                                 dt_iop_spektrafilm_params_t *p)
{
  const gboolean hal = p->halation_on;
  gtk_widget_set_sensitive(g->scatter_amount, hal);
  gtk_widget_set_sensitive(g->scatter_scale, hal);
  gtk_widget_set_sensitive(g->halation_amount, hal);
  gtk_widget_set_sensitive(g->halation_scale, hal);
  gtk_widget_set_sensitive(g->boost_ev, hal);
  gtk_widget_set_sensitive(g->boost_range, hal);
  gtk_widget_set_sensitive(g->protect_ev, hal);

  const gboolean grn = p->grain_on;
  gtk_widget_set_sensitive(g->grain_amount, grn);
  gtk_widget_set_sensitive(g->grain_size, grn);
  gtk_widget_set_sensitive(g->grain_usm_sigma, grn);
  gtk_widget_set_sensitive(g->grain_usm_amount, grn);

  const gboolean dif = p->diffusion_on;
  gtk_widget_set_sensitive(g->diffusion_filter_family, dif);
  gtk_widget_set_sensitive(g->diffusion_strength, dif);
  gtk_widget_set_sensitive(g->diffusion_scale, dif);
  gtk_widget_set_sensitive(g->diffusion_warmth, dif);

  const gboolean pdif = p->print_diffusion_on;
  gtk_widget_set_sensitive(g->print_diffusion_filter_family, pdif);
  gtk_widget_set_sensitive(g->print_diffusion_strength, pdif);
  gtk_widget_set_sensitive(g->print_diffusion_scale, pdif);
  gtk_widget_set_sensitive(g->print_diffusion_warmth, pdif);

  /* last: the development sliders are gated on their own stock's family, and
     must not be re-enabled by any of the plain `printing` toggles above */
  _update_development_sensitivity(g, p);
}

void gui_reset(dt_iop_module_t *self)
{
  dt_iop_color_picker_reset(self, TRUE);
}

/* called by the core whenever a params-linked widget changed */
void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  /* Stamp the spectral table this edit is being made against. Done here because
     gui_changed() runs after the widget has written the param and before the
     history item is created, so the value lands in the same edit -- and only on
     a real user change, so merely opening an image never dirties one.

     Only stamp when there is nothing to lose: no table recorded yet, or the
     loaded pack is already the one recorded. Overwriting a DIFFERENT recorded
     hash would throw away the only record of which pack renders this edit as
     it was made, and it would happen on any incidental slider touch while the
     mismatch warning was on screen saying the data was wrong. That record is
     what the download button uses to fetch the right pack, so losing it turns
     a fixable mismatch into a permanent one. */
  {
    dt_iop_spektrafilm_params_t *p = (dt_iop_spektrafilm_params_t *)self->params;
    dt_pthread_mutex_lock(&_pack_lock);
    if(_pack)
    {
      const uint32_t cur = sf_pack_lut_hash(_pack);
      if(!p->lut_hash || p->lut_hash == cur) p->lut_hash = cur;
    }
    dt_pthread_mutex_unlock(&_pack_lock);
  }

  dt_iop_spektrafilm_gui_data_t *g = (dt_iop_spektrafilm_gui_data_t *)self->gui_data;
  dt_iop_spektrafilm_params_t *p = (dt_iop_spektrafilm_params_t *)self->params;
  if(!w || w == g->scan_film) _update_print_sensitivity(self);
  if(!w || w == g->halation_on || w == g->grain_on || w == g->diffusion_on
     || w == g->print_diffusion_on)
  {
    _toggle_sensitivity(g, p);
    if(w == g->print_diffusion_on) _update_print_sensitivity(self);
  }
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

/* ---------------------------------------------------------------------- */
/* data pack status row                                                   */
/* ---------------------------------------------------------------------- */

static gboolean _data_poll_cb(gpointer user_data);

/* Reflect pack state into the header row. Called whenever the module's trouble
   state is refreshed and once per second while a download runs. */
static void _update_data_row(dt_iop_module_t *self)
{
  dt_iop_spektrafilm_gui_data_t *g = (dt_iop_spektrafilm_gui_data_t *)self->gui_data;
  const dt_iop_spektrafilm_params_t *p =
      (const dt_iop_spektrafilm_params_t *)self->params;
  if(!g || !g->data_box) return;

  char msg[256] = { 0 };
  double progress = 0.0;
  const sf_fetch_state_t state = sf_fetch_status(msg, sizeof msg, &progress);

  /* Is any pack usable at all, and is it the one this edit was made with?
     Local-only, so this is cheap enough to answer on every refresh. */
  char dir[SF_PATH_LEN];
  gboolean exact = FALSE;
  const gboolean have_any =
      sf_fetch_resolve_pack_dir(p->lut_hash, dir, sizeof dir, &exact);

  /* With no pack there is nothing any of the controls could act on: a film
     list with no films, sliders driving a simulation that cannot be built.
     Collapse the module to the one control that changes that, and bring the
     rest back only once a pack is in place. */
  if(g->main_box) gtk_widget_set_visible(g->main_box, have_any);

  if(state == SF_FETCH_RUNNING)
  {
    g->data_last_state = state;
    char line[320];
    snprintf(line, sizeof line, "%s  %d%%", msg, (int)(progress * 100.0 + 0.5));
    gtk_label_set_text(GTK_LABEL(g->data_status), line);
    gtk_button_set_label(GTK_BUTTON(g->data_button), _("cancel"));
    gtk_widget_set_sensitive(g->data_button, TRUE);
    gtk_widget_set_visible(g->data_box, TRUE);
    if(!g->data_poll) g->data_poll = g_timeout_add(500, _data_poll_cb, self);
    return;
  }

  if(g->data_poll)
  {
    g_source_remove(g->data_poll);
    g->data_poll = 0;
  }

  /* A fetch just finished. Reprocessing alone is not enough to make the new
     pack usable: the film and paper comboboxes were filled by _rescan() at a
     time when no profiles existed, and nothing refills them on a pipe
     reprocess. Without this the module renders but every stock list stays
     empty, so no selection can be made and changing a slider appears to do
     nothing. dt_iop_gui_update() re-runs gui_update(), which rescans. */
  if(g->data_last_state == SF_FETCH_RUNNING && state == SF_FETCH_DONE)
  {
    g->data_last_state = state;
    dt_iop_gui_update(self);

    /* Drop the cached pipeline output from this module onwards before asking
       for a reprocess.

       The pixelpipe caches by a hash over module parameters, and installing a
       pack changes none of them -- so the cacheline computed while the module
       had no data (and therefore passed pixels through untouched) still
       matches and gets reused. The symptom is a module that stays inert after
       a successful download, starts working the moment any slider moves, and
       goes inert again the instant that slider returns to its original value,
       because that value hashes back onto the stale line. A restart looked
       like a fix only because it started with an empty cache. */
    dt_develop_t *dev = darktable.develop;
    if(dev)
    {
      dt_dev_pixelpipe_t *pipes[]
          = { dev->full.pipe, dev->preview_pipe, dev->preview2.pipe };
      for(size_t i = 0; i < sizeof(pipes) / sizeof(pipes[0]); i++)
        if(pipes[i])
          dt_dev_pixelpipe_cache_invalidate_later(pipes[i], self->iop_order,
                                                  "spektrafilm data pack installed");
      dt_dev_reprocess_all(dev);
    }
    return; /* gui_update() calls back into here with the settled state */
  }
  g->data_last_state = state;

  /* Nothing installed at all, or installed but not the table this edit wants.
     Those are the only two states worth offering a download for; anything else
     leaves the row hidden. */
  if(have_any && (exact || !p->lut_hash))
  {
    gtk_widget_set_visible(g->data_box, FALSE);
    return;
  }

  g->data_wanted = have_any ? p->lut_hash : 0;
  gtk_label_set_text(
      GTK_LABEL(g->data_status),
      have_any
          ? _("the table this edit was developed with is not installed")
          : _("no data pack installed -- the module's controls appear once one is"));
  gtk_button_set_label(GTK_BUTTON(g->data_button), _("download data pack"));

  /* The button stays visible but dead when downloads are switched off, so the
     reason the module cannot render is discoverable rather than silent. */
  const gboolean allowed = sf_fetch_downloads_enabled();
  gtk_widget_set_sensitive(g->data_button, allowed);
  gtk_widget_set_tooltip_text(
      g->data_button,
      allowed ? _("fetch the matching spectral data pack over the network")
              : _("enable \"allow spektrafilm to download data\" in preferences first"));
  gtk_widget_set_visible(g->data_box, TRUE);
}

static gboolean _data_poll_cb(gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_spektrafilm_gui_data_t *g = (dt_iop_spektrafilm_gui_data_t *)self->gui_data;
  if(!g || !g->data_box) return G_SOURCE_REMOVE;
  _update_data_row(self);
  /* _update_data_row clears data_poll when the fetch is no longer running, and
     that is also the signal to stop this timeout. */
  return g->data_poll ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static void _data_button_clicked(GtkButton *button, dt_iop_module_t *self)
{
  dt_iop_spektrafilm_gui_data_t *g = (dt_iop_spektrafilm_gui_data_t *)self->gui_data;
  if(!g) return;

  if(sf_fetch_status(NULL, 0, NULL) == SF_FETCH_RUNNING)
    sf_fetch_cancel();
  else
    sf_fetch_start(g->data_wanted);

  _update_data_row(self);
}

/* sim_error and sim_warning were being recorded and never shown -- a missing
   data pack, an unreadable profile or a spectral-table mismatch all produced a
   silently wrong or blank render. Route them to the module's trouble banner,
   which is darktable's own mechanism for exactly this. */
static void _update_trouble_message(dt_iop_module_t *self)
{
  const dt_iop_spektrafilm_data_t *d = (const dt_iop_spektrafilm_data_t *)self->data;
  _update_data_row(self);
  if(!d) return;
  if(d->sim_error[0])
    dt_iop_set_module_trouble_message(self, _("cannot render"), d->sim_error, NULL);
  else if(d->sim_warning[0])
    dt_iop_set_module_trouble_message(self, _("data mismatch"), d->sim_warning, NULL);
  else
    dt_iop_set_module_trouble_message(self, NULL, NULL, NULL);
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_spektrafilm_gui_data_t *g = (dt_iop_spektrafilm_gui_data_t *)self->gui_data;
  dt_iop_spektrafilm_params_t *p = (dt_iop_spektrafilm_params_t *)self->params;

  _rescan(self);

  /* Films and papers share one list; e->printing separates them, and each
     combobox entry carries its position in that list as its data. */
  static const struct { int pos; int bw; const char *label; } groups[] = {
    { 0, 0, N_("negative color") },
    { 0, 1, N_("negative monochrome") },
    { 1, 0, N_("positive color") },
    { 1, 1, N_("positive monochrome") },
  };
  static const struct { int bw; const char *label; } pgroups[] = {
    { 0, N_("color") },
    { 1, N_("monochrome") },
  };

  dt_bauhaus_combobox_clear(g->film);
  gboolean any_film = FALSE;
  for(int gi = 0; gi < 4; gi++)
  {
    gboolean first = TRUE;
    int pos = 0;
    for(const GList *l = g->entries; l; l = l->next, pos++)
    {
      const sf_prof_entry_t *e = l->data;
      if(e->printing || e->positive != groups[gi].pos || e->bw != groups[gi].bw) continue;
      if(first) { dt_bauhaus_combobox_add_section(g->film, _(groups[gi].label)); first = FALSE; }
      dt_bauhaus_combobox_add_full(g->film, e->name, DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,
                                   GINT_TO_POINTER(pos), NULL, TRUE);
      any_film = TRUE;
    }
  }
  if(!any_film) dt_bauhaus_combobox_add(g->film, _("(no profiles found)"));

  dt_bauhaus_combobox_clear(g->paper);
  /* paper_hash 0 means "follow the film's target print" -- the state a fresh
     edit starts in, and the one _film_changed() keeps updating. Picking a paper
     replaced it with an explicit choice and the link was then unreachable, which
     is what people have asked to get back. Give that state a name at the top of
     the list so it is both visible and selectable, rather than adding a separate
     reset button for something the combobox can already express. Data -1 keeps
     it clear of the list positions used below. */
  dt_bauhaus_combobox_add_full(g->paper, _("auto (follow film stock)"),
                               DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, GINT_TO_POINTER(-1), NULL, TRUE);
  gboolean any_paper = FALSE;
  for(int gi = 0; gi < 2; gi++)
  {
    gboolean first = TRUE;
    int pos = 0;
    for(const GList *l = g->entries; l; l = l->next, pos++)
    {
      const sf_prof_entry_t *e = l->data;
      if(!e->printing || e->bw != pgroups[gi].bw) continue;
      if(first) { dt_bauhaus_combobox_add_section(g->paper, _(pgroups[gi].label)); first = FALSE; }
      dt_bauhaus_combobox_add_full(g->paper, e->name, DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,
                                   GINT_TO_POINTER(pos), NULL, TRUE);
      any_paper = TRUE;
    }
  }
  if(!any_paper) dt_bauhaus_combobox_add(g->paper, _("(none)"));

  /* Select the saved film. On no hash match -- a fresh param with film_hash 0,
     or a stock that vanished from the pack -- mirror _resolve_stock's fallback
     so the combobox agrees with what the pipeline actually renders, instead of
     landing on whatever sorts first while the pipe renders the real default. */
  int fpos = -1, fallback = -1, pos = 0;
  const sf_prof_entry_t *fe = NULL;
  for(const GList *l = g->entries; l; l = l->next, pos++)
  {
    const sf_prof_entry_t *e = l->data;
    if(e->printing) continue;
    if(fallback < 0 || !strcmp(e->stock, "kodak_portra_400")) fallback = pos;
    if(p->film_hash && e->hash == p->film_hash) { fpos = pos; fe = e; }
  }
  if(fpos < 0) fpos = fallback;
  if(fpos >= 0)
  {
    if(!fe) fe = _entry_at(g, fpos);
    dt_bauhaus_combobox_set_from_value(g->film, fpos);
  }

  /* _film_changed() bails out under darktable.gui->reset, which gui_update runs
     under, so its reset target never gets set on a plain module load. Do it here
     too, or a reset gesture on a positive/reversal film would flip scan_film off.
     p->scan_film itself is deliberately not touched: the loaded value may be an
     intentional override and must survive the load. */
  if(fe) dt_bauhaus_toggle_set_default(g->scan_film, fe->positive);

  const char *target = fe ? fe->target_print : NULL;
  int ppos = -1, pfirst = -1;
  pos = 0;
  for(const GList *l = g->entries; l; l = l->next, pos++)
  {
    const sf_prof_entry_t *e = l->data;
    if(!e->printing) continue;
    if(pfirst < 0) pfirst = pos;
    if(p->paper_hash ? (e->hash == p->paper_hash) : (target && !strcmp(e->stock, target)))
      ppos = pos;
  }
  /* an edit that never picked a paper shows "auto", not the stock it happens to
     resolve to -- otherwise the link looks broken the moment it is displayed */
  if(!p->paper_hash) ppos = -1;
  else if(ppos < 0) ppos = pfirst;
  if(ppos >= -1) dt_bauhaus_combobox_set_from_value(g->paper, ppos);
  /* after the repopulation above, which reset the auto entry to its plain
     label: the entry names the paper this film resolves to, so a module that
     opens on auto shows the paper it is really printing on */
  _update_paper_auto_entry(self);

  {
    const int fpreset = _format_mm_to_preset(p->film_format_mm);
    dt_bauhaus_combobox_set_from_value(g->film_format_combo, fpreset);
    gtk_widget_set_visible(g->film_format_mm_slider,
                           fpreset < 0 || fpreset >= FORMAT_PRESETS_N);
  }

  /* toggle_from_params check buttons are NOT auto-synced by
     dt_bauhaus_update_from_field (it only handles sliders/combos), so set
     them here or they drift from the params: a stale box makes the first
     click a no-op (field already has that value -> no history item) and
     module reset never updates them. */
  dt_bauhaus_toggle_set(g->scan_film, p->scan_film);
  dt_bauhaus_toggle_set(g->adaptation_bandwidth, p->adaptation_bandwidth);
  dt_bauhaus_toggle_set(g->adaptation_surface, p->adaptation_surface);
  dt_bauhaus_toggle_set(g->print_auto_exposure, p->print_auto_exposure);
  dt_bauhaus_toggle_set(g->halation_on, p->halation_on);
  dt_bauhaus_toggle_set(g->diffusion_on, p->diffusion_on);
  dt_bauhaus_toggle_set(g->print_diffusion_on, p->print_diffusion_on);
  dt_bauhaus_toggle_set(g->grain_on, p->grain_on);

  _toggle_sensitivity(g, p);
  _update_print_sensitivity(self);

  _update_trouble_message(self);
}

/* Boost that puts the probe lightness of `rgb` at `target_L`.
 *
 * This used to be a closed form: L was taken to scale as boost^(1/3), so one
 * probe plus new = current * (target/measured)^3 was supposedly exact. That
 * identity holds only while the scan stage is a pure scale on XYZ. It is not:
 * sf_sim_scan applies the scanner black/white-point correction AFTER the boost,
 * and that correction is affine and clipped -- the delivered luminance is
 * clamp(m * boost * Y + q, 0, 1). So L goes as boost^a with a < 1/3, the update
 * degenerates to new = current^(1 - 3a) * const, and repeated picks crept
 * toward the right value instead of landing on it. (Negatives are unaffected,
 * scan_bw_on is only set for scan-film mode with positive stock -- which is
 * exactly where this control gets used.)
 *
 * Solve against the real transfer instead. boost -> L is monotone
 * non-decreasing and one probe is a single pixel through the sim, so a
 * geometric bisection over the slider's own range is both exact and free:
 * 20 steps pin the answer to ~2e-6 of the range. The result no longer depends
 * on the current slider value at all, so picking twice gives the same number. */
static float _solve_boost_for_lightness(const sf_sim_t *sim, const float rgb[3],
                                        const float target_L)
{
  float lo = 0.5f, hi = 4.0f; /* the slider's own $MIN / $MAX */
  if(sf_sim_probe_lightness(sim, rgb, lo) >= target_L) return lo;
  if(sf_sim_probe_lightness(sim, rgb, hi) <= target_L) return hi;
  for(int i = 0; i < 20; i++)
  {
    const float mid = sqrtf(lo * hi);
    if(sf_sim_probe_lightness(sim, rgb, mid) < target_L) lo = mid;
    else hi = mid;
  }
  return sqrtf(lo * hi);
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_t *pipe)
{
  dt_iop_spektrafilm_gui_data_t *g = self->gui_data;
  if(picker != g->output_boost) return;

  /* picked_color_min/max start at sentinel values (+FLT_MAX / -FLT_MAX)
     until a real area pick has actually landed; if this callback fires
     before that (e.g. some other programmatic trigger of the picker
     path), max stays below min and using it directly would feed garbage
     (-FLT_MAX for every channel) into the simulation -- that propagates
     into a wildly out-of-range LUT lookup and segfaults. Standard
     darktable idiom for this check, matching e.g. exposure.c's own
     color_picker_apply. */
  if(self->picked_color_max[0] < self->picked_color_min[0]) return;

  const dt_iop_order_iccprofile_info_t *work_profile = dt_ioppr_get_pipe_work_profile_info(pipe);
  if(!work_profile) return;

  /* Build a standalone sim from the module's current live params: the
     picker runs independently of any specific piece's cached data, so this
     is a one-off build for this measurement, not the pipe's own d->sim
     (which _ensure_sim also caches on -- see there for what gets set). */
  dt_iop_spektrafilm_data_t d_tmp;
  memset(&d_tmp, 0, sizeof(d_tmp));
  d_tmp.p = *(dt_iop_spektrafilm_params_t *)self->params;
  dt_pthread_mutex_init(&d_tmp.lock, NULL);
  sf_sim_t *sim = _ensure_sim(&d_tmp, work_profile);
  if(!sim)
  {
    dt_pthread_mutex_destroy(&d_tmp.lock);
    return;
  }

  /* the brightest tone in the picked area is what determines whether the
     compressor's knee engages usefully; picked_color_max is already an
     area-mode min/max/mean pick (see dt_color_picker_new(..., DT_COLOR_PICKER_AREA, ...)
     above in gui_init). */
  const float rgb_max[3] = { self->picked_color_max[0], self->picked_color_max[1],
                            self->picked_color_max[2] };
  /* Target: land well past the compressor's knee threshold (SF_OUT_LIGHT_T
     = 0.7 in spektra_sim.c), close to but not at its asymptotic limit
     (1.0). 0.80 (only 0.10 above the threshold) turned out too
     conservative in practice -- left visible unused headroom in the
     histogram and read as noticeably dark, since the knee's own
     compression only really starts doing useful work well above its
     threshold. 0.90 still left some headroom on further testing; 0.95
     (only 0.05 short of the limit) uses close to the full available
     range. The knee handles any input gracefully by design, so there's no
     hard-clipping risk in pushing this close to it. */
  const float target_L = 0.95f;
  const float new_boost = _solve_boost_for_lightness(sim, rgb_max, target_L);

  if(d_tmp.gpu) sf_sim_gpu_free(d_tmp.gpu);
  if(d_tmp.sim) sf_sim_free(d_tmp.sim);
  dt_pthread_mutex_destroy(&d_tmp.lock);

  dt_iop_spektrafilm_params_t *p = self->params;
  p->output_luminance_boost = new_boost;
  DT_ENTER_GUI_UPDATE();
  dt_bauhaus_slider_set(g->output_boost, new_boost);
  DT_LEAVE_GUI_UPDATE();
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

/* Section heading with its own reset button.

   Each tab holds several unrelated groups and darktable resets whole modules or
   single widgets, nothing in between -- so trying one idea in "chemistry" means
   either undoing every slider by hand or throwing away the rest of the tab. The
   button resets exactly the widgets between this heading and the next one.

   No bookkeeping: the widgets are packed into the page box in order, so the
   callback walks that box from its own header to the following one. A heading
   is marked with the "sf_section" data key rather than recognised by type. */
static void _section_reset_clicked(GtkButton *button, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  GtkWidget *hdr = gtk_widget_get_parent(GTK_WIDGET(button));
  GtkWidget *box = hdr ? gtk_widget_get_parent(hdr) : NULL;
  if(!box) return;

  /* Deliberately NOT wrapped in darktable.gui->reset: each widget's own
     value-changed handler is what writes the param, so suppressing it would
     move the sliders without changing the render. The cost is one history entry
     per widget rather than one per click -- correct, undoable, just chattier
     than ideal. */
  GList *kids = gtk_container_get_children(GTK_CONTAINER(box));
  gboolean after = FALSE;
  for(const GList *l = kids; l; l = l->next)
  {
    GtkWidget *w = l->data;
    if(w == hdr) { after = TRUE; continue; }
    if(!after) continue;
    if(g_object_get_data(G_OBJECT(w), "sf_section")) break; /* next section */
    if(DT_IS_BAUHAUS_WIDGET(w)) dt_bauhaus_widget_reset(w);
  }
  g_list_free(kids);
}

/* Pack a section heading carrying a reset button, and return it. */
static GtkWidget *_section_add(dt_iop_module_t *self, const char *label)
{
  GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  g_object_set_data(G_OBJECT(hdr), "sf_section", GINT_TO_POINTER(1));

  GtkWidget *lbl = dt_ui_section_label_new(label);
  gtk_box_pack_start(GTK_BOX(hdr), lbl, TRUE, TRUE, 0);

  GtkWidget *btn = dtgtk_button_new(dtgtk_cairo_paint_reset, 0, NULL);
  gtk_widget_set_tooltip_text(btn, _("reset only this section"));
  gtk_box_pack_end(GTK_BOX(hdr), btn, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(btn), "clicked", G_CALLBACK(_section_reset_clicked), self);

  dt_gui_box_add(self->widget, hdr);
  return hdr;
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_spektrafilm_gui_data_t *g = IOP_GUI_ALLOC(spektrafilm);
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  GtkWidget *sf_main_box = self->widget;

  /* ---- data pack row (packed first, so it reads as a precondition) ----
     Deliberately outside main_box: it is the one thing that must stay on
     screen when there is no pack, since it is what gets you one.

     Built with no_show_all set, because dt_iop_gui_init() runs a single
     gtk_widget_show_all() over the whole module at creation time. Without the
     flag that one call would reveal the row before _update_data_row() has had
     any chance to decide whether it should be there. */
  g->data_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_PIXEL_APPLY_DPI(2));
  gtk_widget_set_no_show_all(g->data_box, TRUE);

  g->data_status = gtk_label_new("");
  gtk_label_set_line_wrap(GTK_LABEL(g->data_status), TRUE);
  gtk_label_set_xalign(GTK_LABEL(g->data_status), 0.0);
  gtk_widget_set_name(g->data_status, "spektrafilm-data-status");
  gtk_box_pack_start(GTK_BOX(g->data_box), g->data_status, TRUE, TRUE, 0);

  g->data_button = gtk_button_new_with_label(_("download data pack"));
  g_signal_connect(G_OBJECT(g->data_button), "clicked",
                   G_CALLBACK(_data_button_clicked), self);
  gtk_box_pack_start(GTK_BOX(g->data_box), g->data_button, TRUE, TRUE, 0);

  gtk_widget_show(g->data_status);
  gtk_widget_show(g->data_button);
  gtk_box_pack_start(GTK_BOX(sf_main_box), g->data_box, TRUE, TRUE, 0);

  /* Everything else lives under main_box so a single set_visible() hides the
     lot. No no_show_all here: the creation-time show_all is what establishes
     the correct visibility of every child, including the ones with their own
     rules (the format slider is only shown for a custom format), and redoing
     that by hand on reveal would quietly override them. Since that show_all
     runs once at creation and gui_update() runs after it, hiding here sticks,
     and revealing later restores exactly the state show_all left behind. */
  g->main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  dt_gui_box_add(sf_main_box, g->main_box);

  /* ---- header ---- */
  GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  dt_gui_box_add(g->main_box, header_box);

  /* Inline labels, like every other control in the module. These were section
     headings for a while, to buy the value more width: the paper names used to
     truncate to a shared prefix, so "Kodak Professional Portra Endura" and three
     others all read alike. _shorten_name() has since stripped the filler words
     they shared, and they now diverge at the seventh character ("Kodak Endura
     Premier" / "Kodak Portra Endura" / "Kodak Supra Endura" / "Kodak Ultra
     Endura"), so a label beside them no longer costs anything worth having.
     A heading per single control also read as clutter once there were three. */
  g->film = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->film, NULL, N_("film stock"));
  gtk_widget_set_tooltip_text(g->film, _("film emulsion (spektrafilm filming profile)"));
  g_signal_connect(G_OBJECT(g->film), "value-changed", G_CALLBACK(_film_changed), self);
  gtk_box_pack_start(GTK_BOX(header_box), g->film, TRUE, TRUE, 0);

  g->paper = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->paper, NULL, N_("print paper"));
  gtk_widget_set_tooltip_text(g->paper,
                              _("print/paper stock; defaults to the film's target print"));
  g_signal_connect(G_OBJECT(g->paper), "value-changed", G_CALLBACK(_paper_changed), self);
  gtk_box_pack_start(GTK_BOX(header_box), g->paper, TRUE, TRUE, 0);

  /* redirect self->widget so from_params widgets pack into header_box */
  self->widget = header_box;

  g->film_format_combo = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->film_format_combo, NULL, N_("format"));
  gtk_widget_set_tooltip_text(g->film_format_combo,
                              _("common film/sensor gate presets; picking one sets the frame"
                                " long edge slider below (pick \"custom\" to dial in an exact"
                                " value).\nthe preset names a film gauge (35mm) while the"
                                " slider is the frame's long edge (36mm) -- both describe the"
                                " same format"));
  _populate_format_combo(self);
  g_signal_connect(G_OBJECT(g->film_format_combo), "value-changed",
                   G_CALLBACK(_format_changed), self);
  gtk_box_pack_start(GTK_BOX(header_box), g->film_format_combo, TRUE, TRUE, 0);

  g->film_format_mm_slider = dt_bauhaus_slider_from_params(self, "film_format_mm");
  dt_bauhaus_slider_set_format(g->film_format_mm_slider, _(" mm"));
  gtk_widget_set_tooltip_text(g->film_format_mm_slider,
                              _("physical frame size, long edge. sets the scale that grain, "
                                "scatter, halation and diffusion are all computed at, so a "
                                "smaller format shows every one of them proportionally larger "
                                "for the same print size"));
  g_signal_connect(G_OBJECT(g->film_format_mm_slider), "value-changed",
                   G_CALLBACK(_format_slider_changed), self);

  /* restore main widget for the notebook */
  self->widget = sf_main_box;

  /* ---- notebook / tabs ---- */
  static struct dt_action_def_t notebook_def = { };
  g->notebook = dt_ui_notebook_new(&notebook_def);
  dt_action_define_iop(self, NULL, N_("page"), GTK_WIDGET(g->notebook), &notebook_def);
  dt_gui_box_add(g->main_box, GTK_WIDGET(g->notebook));

  /* ---- tab 1: film (exposure + development) ---- */
  self->widget = dt_ui_notebook_page(g->notebook, N_("film"), NULL);

  _section_add(self, C_("section", "exposure"));

  g->exposure_ev = dt_bauhaus_slider_from_params(self, "exposure_ev");
  dt_bauhaus_slider_set_format(g->exposure_ev, _(" EV"));
  gtk_widget_set_tooltip_text(
      g->exposure_ev, _("film exposure compensation; with auto print exposure enabled, print"
                        " exposure follows automatically so this has no net brightness effect"
                        " (except on positive/reversal film, which has no print stage)"));
  g->scan_film = dt_bauhaus_toggle_from_params(self, "scan_film");
  gtk_widget_set_tooltip_text(g->scan_film,
                              _("view the developed film directly (no print stage)"));

  g->push_pull_stops = dt_bauhaus_slider_from_params(self, "push_pull_stops");
  dt_bauhaus_slider_set_format(g->push_pull_stops, _(" stops"));
  gtk_widget_set_tooltip_text(
      g->push_pull_stops,
      _("push (positive) or pull (negative) processing: shoot at an effective ISO"
        " different from box speed, then under- or over-develop to compensate --"
        " combines an exposure shift with a derived contrast increase/decrease"
        " (approximate: the exact relationship depends on the specific film/developer"
        " combination, which isn't modeled here). Stacks with the granular gamma"
        " controls below for further fine-tuning"));

  _section_add(self, C_("section", "chemistry"));

  g->development_min = dt_bauhaus_slider_from_params(self, "development_min");
  dt_bauhaus_slider_set_format(g->development_min, _(" min"));
  /* gui_update() replaces this with the selected stock's own times, and greys
     the slider out for stocks characterised at a single development */
  gtk_widget_set_tooltip_text(g->development_min,
                              _("development time. snaps to the nearest time the stock was\n"
                                "characterised at; 0 uses the stock's own default."));

  g->film_gamma_factor = dt_bauhaus_slider_from_params(self, "film_gamma_factor");
  dt_bauhaus_slider_set_soft_range(g->film_gamma_factor, 0.25f, 2.0f);
  gtk_widget_set_tooltip_text(
      g->film_gamma_factor,
      _("overall development contrast (morphs the film's density curves) -- extended or"
        " reduced development time, as in push/pull processing; 1.0 = normal development"));

  g->film_gamma_factor_fast = dt_bauhaus_slider_from_params(self, "film_gamma_factor_fast");
  dt_bauhaus_slider_set_soft_range(g->film_gamma_factor_fast, 0.25f, 2.0f);
  gtk_widget_set_tooltip_text(
      g->film_gamma_factor_fast,
      _("contrast of the fastest (most light-sensitive) emulsion sub-layer only --"
        " independent of the slow layer, since push/pull processing doesn't always affect"
        " every sub-layer equally"));

  g->film_gamma_factor_slow = dt_bauhaus_slider_from_params(self, "film_gamma_factor_slow");
  dt_bauhaus_slider_set_soft_range(g->film_gamma_factor_slow, 0.25f, 2.0f);
  gtk_widget_set_tooltip_text(
      g->film_gamma_factor_slow,
      _("contrast of the mid and slow emulsion sub-layers"));

  g->film_developer_exhaustion = dt_bauhaus_slider_from_params(self, "film_developer_exhaustion");
  gtk_widget_set_tooltip_text(
      g->film_developer_exhaustion,
      _("local developer depletion in dense (highly-exposed) areas: blends the highlight"
        " shoulder toward a self-limiting rolloff without shifting midgray (0 = off)"));

  /* "couplers and quality" named its first two controls, which stopped
     describing the section once the adaptation switches joined them -- and
     enumerating members does not scale anyway. What all four have in common is
     that they are the knobs you reach for last: the coupler strength and the
     two adaptation halves change how faithful the model is rather than what the
     look is, and the quality setting trades accuracy for speed. */
  _section_add(self, C_("section", "advanced"));

  g->couplers_amount = dt_bauhaus_slider_from_params(self, "couplers_amount");
  gtk_widget_set_tooltip_text(g->couplers_amount,
                              _("DIR coupler strength: inter-layer inhibition drives saturation"
                                " and edge effects (1.0 = film-accurate, 0 = off)"));

  g->quality = dt_bauhaus_combobox_from_params(self, "quality");
  gtk_widget_set_tooltip_text(g->quality,
                              _("spectral accuracy vs speed: the colour model is evaluated"
                                " on a table of this size and PCHIP-interpolated between the"
                                " points, so a finer table lands closer to the exact answer"
                                " and costs more to build.\n\"exact spectral\" skips the table"
                                " and runs the model per pixel -- CPU only, and slow"));

  g->adaptation_bandwidth = dt_bauhaus_toggle_from_params(self, "adaptation_bandwidth");
  gtk_widget_set_tooltip_text(
      g->adaptation_bandwidth,
      _("first half of the film's sensitivity adaptation: a spectral bandpass\n"
        "applied to the stock's own sensitivities, rolling off the UV and IR\n"
        "ends of each channel while preserving white balance.\non by default,\n"
        "and best left on: it is part of how the stock is characterised rather\n"
        "than a look.\nno effect on stocks whose profile carries no bandpass"));

  g->adaptation_surface = dt_bauhaus_toggle_from_params(self, "adaptation_surface");
  gtk_widget_set_tooltip_text(
      g->adaptation_surface,
      _("second half of the film's sensitivity adaptation: a per-colour exposure\n"
        "correction of up to two stops, zero at the film's own white point and\n"
        "growing with distance from it.\noff by default: it shifts saturated\n"
        "colours substantially.\nno effect on stocks whose profile carries no\n"
        "surface (the monochrome films and every print paper)"));

  /* ---- tab 2: print ---- */
  self->widget = dt_ui_notebook_page(g->notebook, N_("print"), NULL);

  GtkWidget *print_page = self->widget;

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

  _section_add(self, C_("section", "chemistry"));

  g->print_development_min = dt_bauhaus_slider_from_params(self, "print_development_min");
  dt_bauhaus_slider_set_format(g->print_development_min, _(" min"));
  /* _update_development_sensitivity() replaces this with the selected paper's own
     times, and greys it out for papers characterised at a single development */
  gtk_widget_set_tooltip_text(g->print_development_min,
                              _("print development time. snaps to the nearest time the paper\n"
                                "was characterised at; 0 uses its own default."));

  _section_add(self, C_("section", "filtration"));

  g->filter_m = dt_bauhaus_slider_from_params(self, "filter_m");
  dt_bauhaus_slider_set_format(g->filter_m, _(" CC"));
  gtk_widget_set_tooltip_text(g->filter_m,
                              _("magenta enlarger filtration, Kodak CC units from neutral"));

  g->filter_y = dt_bauhaus_slider_from_params(self, "filter_y");
  dt_bauhaus_slider_set_format(g->filter_y, _(" CC"));
  gtk_widget_set_tooltip_text(g->filter_y,
                              _("yellow enlarger filtration, Kodak CC units from neutral"));

  self->widget = print_page;

  _section_add(self, C_("section", "preflash"));

  g->preflash_exposure = dt_bauhaus_slider_from_params(self, "preflash_exposure");
  /* The effect is strong well before 0.5, so spreading 0..2 across the panel
     put every usable setting in the first quarter of the travel and made the
     step from off to barely-on larger than the whole range people work in.
     Higher values stay reachable by right-click, as elsewhere in the module. */
  dt_bauhaus_slider_set_soft_range(g->preflash_exposure, 0.0f, 0.5f);
  gtk_widget_set_tooltip_text(
      g->preflash_exposure,
      _("preflash exposure: a brief, uniform pre-exposure of the print through"
        " the film's base density, before the main print exposure -- lifts"
        " shadows and reduces contrast (0 = off). drag up to 0.5, right-click"
        " to enter higher values"));

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

  /* ---- tab 3: grain ---- */
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

  _section_add(self, C_("section", "acutance recovery"));

  g->grain_usm_sigma = dt_bauhaus_slider_from_params(self, "grain_usm_sigma");
  dt_bauhaus_slider_set_soft_range(g->grain_usm_sigma, 0.0f, 3.0f);
  gtk_widget_set_tooltip_text(g->grain_usm_sigma,
                              _("sharpening radius (0 = off). "
                                "higher = wider halos, lower = finer detail"));

  g->grain_usm_amount = dt_bauhaus_slider_from_params(self, "grain_usm_amount");
  dt_bauhaus_slider_set_soft_range(g->grain_usm_amount, 0.0f, 2.0f);
  gtk_widget_set_tooltip_text(g->grain_usm_amount,
                              _("sharpening strength (0 = off). "
                                "restores crispness that the grain blur softened; "
                                "overdo it and grain starts to look crunchy"));

  /* ---- tab 4: halation ---- */
  self->widget = dt_ui_notebook_page(g->notebook, N_("halation"), NULL);

  g->halation_on = dt_bauhaus_toggle_from_params(self, "halation_on");

  g->scatter_amount = dt_bauhaus_slider_from_params(self, "scatter_amount");
  gtk_widget_set_tooltip_text(g->scatter_amount,
                              _("fraction of light that scatters inside the emulsion,\n"
                                "before the halation bounce. 1.0 is film-accurate and is\n"
                                "also the maximum -- it means all of it, so nothing of the\n"
                                "unscattered image remains.\n\n"
                                "this is why the whole frame softens rather than just high\n"
                                "contrast edges: the scatter radius is small (a few um on\n"
                                "film) but it applies everywhere. lower this if you want a\n"
                                "sharper result than the film itself would give."));

  g->scatter_scale = dt_bauhaus_slider_from_params(self, "scatter_scale");
  /* Upstream fixes scatter_spatial_scale at 1.0 -- it is a schema field with no
     UI, no preset and no per-stock override. Exposing it is a darktable
     addition, so keep the drag range close to the value the film model actually
     claims and leave the rest reachable by right-click. At 4.0 on a 50 MP frame
     the effective blur is ~14 px across the whole image, far outside anything
     the reference produces. */
  dt_bauhaus_slider_set_soft_range(g->scatter_scale, 0.2f, 1.5f);
  gtk_widget_set_tooltip_text(g->scatter_scale,
                              _("scales the in-emulsion scatter radius. 1.0 is\n"
                                "film-accurate: the radius the film model itself\n"
                                "works at, and not normally something to change.\n\n"
                                "above 1.0 you are past what the film model claims, and the\n"
                                "whole frame softens quickly: the radius scales with the\n"
                                "value, so 4.0 is a four times wider blur everywhere.\n"
                                "drag up to 1.5, right-click to enter higher values"));

  g->halation_amount = dt_bauhaus_slider_from_params(self, "halation_amount");
  dt_bauhaus_slider_set_soft_range(g->halation_amount, 0.0f, 2.0f);
  gtk_widget_set_tooltip_text(g->halation_amount,
                              _("halation strength (1.0 = film-accurate; drag up to 2,"
                                " right-click to enter higher values)"));

  g->halation_scale = dt_bauhaus_slider_from_params(self, "halation_scale");
  gtk_widget_set_tooltip_text(g->halation_scale,
                              _("halation size: scales the glow radius (1.0 = film-accurate)"));

  _section_add(self, C_("section", "threshold"));

  g->boost_ev = dt_bauhaus_slider_from_params(self, "boost_ev");
  dt_bauhaus_slider_set_format(g->boost_ev, _(" EV"));
  gtk_widget_set_tooltip_text(g->boost_ev,
                              _("highlight boost: reconstructs clipped highlights so they bloom"
                                " into halation/diffusion (0 = off)"));

  g->boost_range = dt_bauhaus_slider_from_params(self, "boost_range");
  gtk_widget_set_tooltip_text(
      g->boost_range,
      _("widens or narrows the band of tones the highlight boost acts on. "
        "lower confines it to the brightest clipped highlights, higher pulls "
        "more of the upper midtones into the bloom"));

  g->protect_ev = dt_bauhaus_slider_from_params(self, "protect_ev");
  dt_bauhaus_slider_set_format(g->protect_ev, _(" EV"));
  gtk_widget_set_tooltip_text(g->protect_ev,
                              _("protect tones below this many stops over mid-grey from the boost"));

  /* ---- tab 5: diffusion ---- */
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
  gtk_widget_set_tooltip_text(
      g->diffusion_strength,
      _("sets how much light is diverted into the diffusion halo (0 = off). "
        "the halo is added on top of the unfiltered image, so raising this "
        "lifts shadows and lowers contrast as well as glowing the highlights"));

  g->diffusion_scale = dt_bauhaus_slider_from_params(self, "diffusion_scale");
  gtk_widget_set_tooltip_text(
      g->diffusion_scale,
      _("scales the radius of the diffusion halo. spreads the same amount of "
        "light further from each highlight rather than adding more of it -- "
        "use diffusion strength for that"));

  g->diffusion_warmth = dt_bauhaus_slider_from_params(self, "diffusion_warmth");
  gtk_widget_set_tooltip_text(g->diffusion_warmth,
                              _("diffusion halo warmth: >0 warm outer halo, <0 cool"
                                " (added on top of the selected filter's own warmth bias)"));

  g->print_diffusion_on = dt_bauhaus_toggle_from_params(self, "print_diffusion_on");

  g->print_diffusion_filter_family
      = dt_bauhaus_combobox_from_params(self, "print_diffusion_filter_family");
  gtk_widget_set_tooltip_text(
      g->print_diffusion_filter_family,
      _("print diffusion filter type (same presets as the film-stage filter)"));

  g->print_diffusion_strength = dt_bauhaus_slider_from_params(self, "print_diffusion_strength");
  gtk_widget_set_tooltip_text(
      g->print_diffusion_strength,
      _("sets how much light is diverted into the print diffusion halo "
        "(0 = off). acts at the enlarger rather than the camera, so it blooms "
        "the printed image instead of the scene"));

  g->print_diffusion_scale = dt_bauhaus_slider_from_params(self, "print_diffusion_scale");
  gtk_widget_set_tooltip_text(
      g->print_diffusion_scale,
      _("scales the radius of the print diffusion halo. spreads the same "
        "amount of light further from each highlight rather than adding more "
        "of it -- use print diffusion strength for that"));

  g->print_diffusion_warmth = dt_bauhaus_slider_from_params(self, "print_diffusion_warmth");
  gtk_widget_set_tooltip_text(g->print_diffusion_warmth,
                              _("print diffusion halo warmth: >0 warm outer halo, <0 cool"
                                " (added on top of the selected filter's own warmth bias)"));

  /* ---- scanner tab ---- */
  self->widget = dt_ui_notebook_page(g->notebook, N_("scanner"), NULL);

  /* Pre-compression boost lives here, not in the header. It acts in the scan
     stage, immediately before the OkLCh gamut compressor -- the last thing the
     module does, not the first. Its old position at the top implied an input
     control, which is why the picker "reading the processed look" was reported
     as a bug: the picker is right, the placement was misleading. */
  g->output_boost = dt_bauhaus_slider_from_params(self, "output_luminance_boost");
  gtk_widget_set_tooltip_text(g->output_boost,
                              _("multiplies XYZ luminance just before the OkLCh gamut\n"
                                "compressor, pushing the histogram right while preserving\n"
                                "the film's natural shoulder rolloff.\n\n"
                                "this acts at the END of the module, so the picker measures\n"
                                "the processed image rather than the input"));
  dt_color_picker_new(self, DT_COLOR_PICKER_AREA, g->output_boost);
  dt_bauhaus_widget_set_quad_tooltip(g->output_boost,
                                     _("pick brightest tone in the selected area and set the"
                                       " boost so it lands just past the compressor's knee"));

  g->scan_blur = dt_bauhaus_slider_from_params(self, "scan_blur");
  gtk_widget_set_tooltip_text(g->scan_blur,
                              _("scanner lens softness, in pixels (0 = off)"));

  g->scan_usm_sigma = dt_bauhaus_slider_from_params(self, "scan_usm_sigma");
  gtk_widget_set_tooltip_text(g->scan_usm_sigma,
                              _("scanner sharpening radius, in pixels"));

  g->scan_usm_amount = dt_bauhaus_slider_from_params(self, "scan_usm_amount");
  gtk_widget_set_tooltip_text(g->scan_usm_amount,
                              _("scanner sharpening strength (0 = off). "
                                "0.7 is what a scan of the film normally gets; "
                                "leave at 0 if you prefer to sharpen downstream"));

  g->glare_percent = dt_bauhaus_slider_from_params(self, "glare_percent");
  gtk_widget_set_tooltip_text(g->glare_percent,
                              _("viewing glare: a faint veil of the viewing light "
                                "reflected off the print surface, in percent. "
                                "lifts the deepest blacks slightly. "
                                "not applied when scanning the film directly"));

  /* restore root widget */
  self->widget = sf_main_box;
}

void gui_cleanup(dt_iop_module_t *self)
{
  dt_iop_spektrafilm_gui_data_t *g = (dt_iop_spektrafilm_gui_data_t *)self->gui_data;
  if(g)
  {
    /* The poll timeout closes over self and reads gui_data. Leaving it armed
       past teardown is a use-after-free on the next tick. */
    if(g->data_poll)
    {
      g_source_remove(g->data_poll);
      g->data_poll = 0;
    }
    g_list_free_full(g->entries, g_free);
    g->entries = NULL;
  }
}

// clang-format off
// modelines
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// clang-format on
