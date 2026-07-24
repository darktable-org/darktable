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

/* spektra_sim — native port of the spektrafilm runtime pipeline.
 *
 * This is a C implementation of the deterministic per-pixel model of
 * spektrafilm (https://github.com/andreavolpato/spektrafilm), the spectral
 * film simulation by Andrea Volpato (GPLv3; film modeling powered by
 * spektrafilm). It replaces the baked .cube-bundle approach: all colour
 * science is computed at parameter-commit time from a *data pack* exported
 * from a spektrafilm release (measured stock profiles, the hanatos2025
 * irradiance spectra LUT, illuminant SPDs, dichroic filter curves), so a new
 * spektrafilm release is adopted by re-running the exporter — no rebake, no
 * code changes as long as the model version matches.
 *
 * Model version tracked by this port: spektrafilm 0.3.x runtime
 * (SimulationPipeline: filming.expose → filming.develop → printing.expose →
 * printing.develop → scanning.scan). The stochastic / spatial effects
 * (grain, halation, scatter, diffusion filters, coupler diffusion blur) are
 * intentionally *not* in this engine — they act between the per-pixel
 * stages and stay in the caller (darktable already has fast gaussian
 * infrastructure; see spektra_core.c).
 *
 * Pipeline stages exposed here (all deterministic, all pure per-pixel):
 *
 *   rgb_in --expose--> raw (linear film exposure)          [caller: highlight
 *      boost, diffusion filter, scatter, halation in linear domain]
 *   raw --lograw--> log_raw
 *   log_raw --develop_corr--> DIR coupler correction       [caller: blur]
 *   (log_raw, corr) --develop--> cmy film density          [caller: grain]
 *   cmy --print_expose--> log_raw_print
 *   log_raw_print --print_develop--> cmy print density
 *   cmy --scan--> rgb_out (linear, output primaries, gamut compressed)
 *
 * The heavy spectral integrals (print exposure through the filtered
 * enlarger illuminant, scan through the viewing illuminant to XYZ) can run
 * either exactly per pixel or through runtime-built 3D tables with monotone
 * PCHIP interpolation — the same two paths the reference implementation
 * has (use_enlarger_lut / use_scanner_lut). The tables are built here from
 * profile data at sf_sim_build() time; nothing is pre-baked on disk.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SF_NWL 81  /* 380..780 nm in 5 nm steps — spektrafilm SPECTRAL_SHAPE */
#define SF_NLE 256 /* log-exposure grid — spektrafilm LOG_EXPOSURE */
/* max emulsion sub-layers a film's fitted density-curve model can have
   (matches sf_curves_model_t's centers/amplitudes/sigmas[3][8] in
   spektra_sim.c) and the max particle_scale_sublayers entries read below. */
#define SF_GRAIN_MAX_SUBLAYERS 8



typedef struct sf_pack_t sf_pack_t;
typedef struct sf_profile_t sf_profile_t;
typedef struct sf_sim_t sf_sim_t;

/* ---------------------------------------------------------------- pack -- */

/* Load a data pack directory (pack.json + spectra_lut.f32 + profiles/).
 * On failure returns NULL and sets *errmsg (caller frees with free()). */
sf_pack_t *sf_pack_load(const char *dir, char **errmsg);
void sf_pack_free(sf_pack_t *pack);
const char *sf_pack_version(const sf_pack_t *pack);

/* Neutral enlarger filter database lookup (Kodak CC units, CMY order).
 * Returns true and fills cmy[3] when a calibration exists for the triple. */
bool sf_pack_neutral_filters(const sf_pack_t *pack, const char *print_stock,
                             const char *illuminant, const char *film_stock,
                             double cmy[3]);

/* Per-film digested render defaults from the release (DIR coupler gamma
 * matrix, halation preset). Any pointer may be NULL. Returns false if the
 * stock has no entry (generic defaults are then left untouched). */
/* Langmuir K factors from film_render_defaults (dev/0.4+ packs); returns
   false and leaves outputs untouched when the pack predates them. */
bool sf_pack_film_langmuir(const sf_pack_t *pack, const char *film_stock,
                           double donor_k[3], double receiver_k[3]);

bool sf_pack_film_defaults(const sf_pack_t *pack, const char *film_stock,
                           double gamma_samelayer[3],
                           double gamma_inter_r_gb[2],
                           double gamma_inter_g_rb[2],
                           double gamma_inter_b_rg[2],
                           double halation_strength[3],
                           double halation_sigma_um[3],
                           double scatter_core_um[3],
                           double scatter_tail_um[3],
                           double scatter_tail_weight[3]);

/* ------------------------------------------------------------- profile -- */

sf_profile_t *sf_profile_load(const char *path, char **errmsg);
void sf_profile_free(sf_profile_t *profile);
/* ---------------------------------------------------------- GPU export -- */

/* Float copies of everything a per-pixel GPU port needs. Buffers are malloc'd
 * and owned by this struct EXCEPT cmax_table, which borrows the sim's own
 * float table (keep the sim alive while using the export).
 * Only the table-based paths export: lut_steps must be >= 2 (exact spectral
 * has no GPU path) or sf_sim_gpu_export() returns NULL. */
typedef struct sf_sim_gpu_t
{
  /* expose: work RGB -> film raw exposure */
  float m_in[9];
  float ev_scale;
  int tc_n;
  float *tc_lut; /* tc_n * tc_n * 3 */
  /* film develop */
  float gamma[3];
  float le0, le_step;
  float *curves_norm;   /* SF_NLE*3 */
  float *curves_before; /* SF_NLE*3 (== curves_norm when couplers off) */
  float couplers_M[9];  /* row donor -> column receiver, amount-scaled */
  float film_dmax[3];
  int film_positive, couplers_active;
  /* printing (has_print == 0 in scan-film mode; buffers NULL then) */
  int has_print, steps;
  float enl_lo[3], enl_hi[3];
  float *enl_lut, *enl_sx, *enl_sy, *enl_sz; /* steps^3 * 3 */
  float *enl_cmin, *enl_cmax;                /* (steps-1)^3 * 3 */
  float print_exposure;
  float *print_curves; /* SF_NLE*3 */
  /* scanning */
  float scan_lo[3], scan_hi[3];
  float *scan_lut, *scan_sx, *scan_sy, *scan_sz, *scan_cmin, *scan_cmax;
  float m_out[9]; /* XYZ(view illuminant) -> output RGB, CAT02 included */
  int scan_bw_on;   /* scanner black/white point (positive film scans) */
  float scan_bw_m, scan_bw_q;
  /* Langmuir couplers (dev/0.4+ packs; flags 0 on 0.3.x = linear) */
  int film_bw; /* B&W stock: achromatic (channel-coupled) grain */
  float coupler_diff_um, coupler_tail_um, coupler_tail_w;
  int couplers_donor_lm, couplers_recv_lm;
  float couplers_donor_K[3], couplers_donor_Dref[3];
  float couplers_recv_Kr[3], couplers_recv_cref[3];
  /* per-film grain catalogue data (film_render_defaults[stock].grain in the
     pack): RMS-granularity, uniformity and density floor, replacing the
     earlier one-size-fits-all constants */
  float grain_rms[3], grain_uniformity[3], grain_dmin[3];
  /* multi-sublayer grain model (see sf_grain_layers_t / _sf_build_grain_layers
     in spektra_sim.c): n==1 for a single-layer curve fit, the existing
     single-layer behavior. The two per-exposure-grid tables are borrowed
     pointers into the sim's own storage (same convention as cmax_table
     below), not copied -- process_cl() turns them into a device constant
     buffer the same way it already does for cmax_table. */
  int grain_n_sublayers;
  float grain_particle_scale[SF_GRAIN_MAX_SUBLAYERS];
  float grain_layer_dmax[SF_GRAIN_MAX_SUBLAYERS][3];
  float grain_layer_npart[SF_GRAIN_MAX_SUBLAYERS][3];
  float grain_layer_dmin[SF_GRAIN_MAX_SUBLAYERS][3];
  const float *grain_layer_curve;       /* [SF_NLE][SF_GRAIN_MAX_SUBLAYERS][3], borrowed */
  const float *grain_layer_curve_total; /* [SF_NLE][3], borrowed */
  /* per-film halation preset (film_render_defaults[stock].halation in the
     pack): back-reflection strength per channel and first-bounce radius;
     falls back to SF_HALATION_STRENGTH_DEFAULT_* / SF_HALATION_SIGMA_DEFAULT_UM
     (spektra_sim.c) when the pack has no per-stock entry. See
     sf_sim_halation_params(). */
  float halation_strength[3], halation_first_sigma_um;
  /* output gamut compression */
  int out_compress; /* sf_output_compress_t */
  float out_luminance_boost;
  float out_rgb2xyz[9], out_xyz2rgb[9];
  float oklab_m1[9], oklab_m2[9], oklab_m1inv[9], oklab_m2inv[9];
  const float *cmax_table; /* cmax_nl * cmax_nh, borrowed from the sim */
  int cmax_nl, cmax_nh;
} sf_sim_gpu_t;

sf_sim_gpu_t *sf_sim_gpu_export(const sf_sim_t *sim);
void sf_sim_gpu_free(sf_sim_gpu_t *g);

int sf_sim_film_bw(const sf_sim_t *sim);
void sf_sim_film_dmax3(const sf_sim_t *sim, float dmax[3]);
/* per-film grain catalogue data (rms_granularity, uniformity, density_min);
   falls back to the legacy fixed constants (SF_GRAIN_LEGACY_* in
   spektra_core.h) when sim is NULL or the pack predates per-film grain. */
void sf_sim_film_grain3(const sf_sim_t *sim, float rms[3], float uniformity[3], float dmin[3]);

/* Multi-sublayer grain model (see _sf_build_grain_layers in spektra_sim.c):
 * n==1 for any stock whose own fitted density-curve model is single-layer
 * (or the pack has no particle_scale_sublayers for it) -- the existing
 * single-layer behavior, not a fallback approximation of a separate case.
 * layer_curve/layer_curve_total point into the sim's own storage (valid for
 * the sim's lifetime; not copied, since the table is a small but non-trivial
 * SF_NLE*SF_GRAIN_MAX_SUBLAYERS*3 floats). */
typedef struct sf_grain_layers_t
{
  int n;
  double particle_scale[SF_GRAIN_MAX_SUBLAYERS];
  double layer_dmax[SF_GRAIN_MAX_SUBLAYERS][3];
  double layer_npart[SF_GRAIN_MAX_SUBLAYERS][3];
  double layer_dmin[SF_GRAIN_MAX_SUBLAYERS][3];
  const float (*layer_curve)[SF_GRAIN_MAX_SUBLAYERS][3]; /* [SF_NLE][sublayer][channel] */
  const float (*layer_curve_total)[3];                   /* [SF_NLE][channel] */
} sf_grain_layers_t;
void sf_sim_grain_layers(const sf_sim_t *sim, sf_grain_layers_t *out);
/* Multi-sublayer grain delta (see _sf_build_grain_layers / sf_grain_layers_t
 * above): only call this when sf_sim_grain_layers()'s n > 1 -- for n==1,
 * calling the existing single-layer sf_grain_delta_dmax() (spektra_core.h)
 * directly is both simpler and avoids a redundant lookup round-trip.
 * npart_scale rescales the build-time-precomputed layer_npart (built at the
 * fixed SF_GRAIN_REF_UM reference scale, since it depends on curve/coupler
 * state baked in at sf_sim_build time, not just resolution) up to the live
 * pipe's real pixel_um: pass (pixel_um*pixel_um)/(SF_GRAIN_REF_UM*SF_GRAIN_REF_UM). */
void sf_grain_delta_ml(const sf_grain_layers_t *layers, const float dens[3], float amount,
                       float out_delta[3], uint32_t xi, uint32_t yi, int mono,
                       const float dmin_c[3], const float unif_c[3], float npart_scale);
/* Same per-sub-layer sampling as sf_grain_delta_ml, but returns each
 * sub-layer's raw (un-combined, pre-dmin-subtraction) sample into
 * raw_out[0..layers->n-1] instead, so the caller can dye-cloud-blur each
 * sub-layer's whole-image buffer independently (upstream's
 * layer_particle_model blur_particle) before summing them -- see the
 * definition in spektra_sim.c for the full rationale. raw_out must have
 * room for at least layers->n floats. */
void sf_grain_raw_samples_ml(const sf_grain_layers_t *layers, float density, int channel_idx,
                             int seed_ch, uint32_t xi, uint32_t yi, float unif_c,
                             float npart_scale, float *raw_out);
/* SF_GRAIN_MAX_SUBLAYERS is defined earlier in this header, next to SF_NLE
   (both are needed by sf_sim_gpu_t above, which comes before this point). */
bool sf_pack_film_grain(const sf_pack_t *pack, const char *film_stock,
                        double rms[3], double uniformity[3], double density_min[3],
                        double particle_scale[SF_GRAIN_MAX_SUBLAYERS], int *n_scale);
#define SF_COUPLER_BLUR_UM 20.0 /* gaussian core default when pack lacks it */
/* exponential-tail gaussian mixture (upstream fit, n=3) — shared with halation */
#define SF_EXPTAIL_A0 0.1633
#define SF_EXPTAIL_A1 0.6496
#define SF_EXPTAIL_A2 0.1870
#define SF_EXPTAIL_R0 0.5360
#define SF_EXPTAIL_R1 1.5236
#define SF_EXPTAIL_R2 2.7684

void sf_sim_coupler_diffusion(const sf_sim_t *sim, double *size_um, double *tail_um,
                              double *tail_w);
bool sf_pack_film_coupler_diffusion(const sf_pack_t *pack, const char *film_stock,
                                    double *size_um, double *tail_um, double *tail_w);

/* per-film halation preset (film_render_defaults[stock].halation in the pack):
 * back-reflection strength per channel (R/G/B) and the first-bounce Gaussian
 * radius in micrometres. Both `strength` and `first_sigma_um` may be NULL if
 * the caller only wants one. Falls back to the generic still-film /
 * strong-antihalation baseline (SF_HALATION_STRENGTH_DEFAULT_* /
 * SF_HALATION_SIGMA_DEFAULT_UM in spektra_sim.c) when `sim` is NULL or the
 * pack predates per-stock halation data. */
void sf_sim_halation_params(const sf_sim_t *sim, double strength[3], double *first_sigma_um);

const char *sf_profile_stock(const sf_profile_t *p);
const char *sf_profile_name(const sf_profile_t *p);
const char *sf_profile_stage(const sf_profile_t *p);        /* "filming" / "printing" */
const char *sf_profile_type(const sf_profile_t *p);         /* "negative" / "positive" */
const char *sf_profile_target_print(const sf_profile_t *p); /* may be NULL */
const char *sf_profile_channel_model(const sf_profile_t *p); /* "color" / "bw" / NULL */

/* -------------------------------------------------------------- params -- */

typedef enum sf_output_compress_t
{
  SF_OUTPUT_COMPRESS_OFF = 0,
  SF_OUTPUT_COMPRESS_OKLCH = 1,    /* reference default */
  SF_OUTPUT_COMPRESS_ACES_RGC = 2,
} sf_output_compress_t;

typedef struct sf_sim_params_t
{
  /* camera / filming */
  double exposure_comp_ev;      /* 0 */
  double density_curve_gamma;   /* 1 */

  /* DIR couplers (matrix part; spatial diffusion is the caller's blur) */
  bool couplers_active;         /* true */
  double couplers_amount;       /* 1 */
  double gamma_samelayer[3];    /* filled from pack film defaults */
  double gamma_inter_r_gb[2], gamma_inter_g_rb[2], gamma_inter_b_rg[2];
  double inhibition_samelayer;  /* 1 */
  double inhibition_interlayer; /* 1 */

  /* grain reference floor — used for table ranges even when grain itself
   * runs in the caller (reference: GrainParams.density_min) */
  double grain_density_min[3];  /* (0.03, 0.03, 0.03) */

  /* enlarger */
  const char *enlarger_illuminant; /* "TH-KG3" */
  const char *dichroic_brand;      /* "custom" — reference color_enlarger default */
  double print_exposure;           /* 1 */
  bool print_exposure_compensation;/* true */
  bool normalize_print_exposure;   /* true */
  double c_filter_neutral, m_filter_neutral, y_filter_neutral; /* CC units;
      seeded from the pack database in sf_sim_build() when neutral_from_db */
  bool neutral_from_db;            /* true */
  double y_filter_shift, m_filter_shift; /* user CC shifts */
  double preflash_exposure;        /* 0 */
  double preflash_y_shift, preflash_m_shift;

  /* print curve morph (s023) — identity at defaults */
  bool morph_active;
  double morph_gamma, morph_gamma_fast, morph_gamma_slow;
  double morph_gamma_r, morph_gamma_g, morph_gamma_b;

  /* film curve chemistry (s023) — same morph as print's, applied to the
   * film's own fitted density-curve model instead, plus developer
   * exhaustion (not yet wired for print). Identity at defaults. */
  bool film_morph_active;
  double film_morph_gamma, film_morph_gamma_fast, film_morph_gamma_slow;
  double film_morph_developer_exhaustion;

  /* scanning / output */
  bool scan_film;                  /* false: full negative→print→scan chain */
  int lut_steps;                   /* 0 = exact spectral per pixel;
                                      >=2 = runtime 3D tables (ref default 17;
                                      33 recommended for production) */

  /* input colour handling: linear RGB -> XYZ (source-white relative) and the
   * source whitepoint xy. The engine appends a CAT16 adaptation to the film
   * reference illuminant, matching spektrafilm's _rgb_to_tc_b(). */
  double input_rgb_to_xyz[9];
  double input_white_xy[2];
  bool input_gamut_compress;       /* true — radial xy Reinhard (0,1,6) */

  /* output colour handling: XYZ (output-white relative) <-> linear output
   * RGB and the output whitepoint xy. The engine prepends a CAT02
   * adaptation from the viewing illuminant, matching colour.XYZ_to_RGB
   * defaults used by spektrafilm's scanning stage. */
  double output_rgb_to_xyz[9];
  double output_xyz_to_rgb[9];
  double output_white_xy[2];
  sf_output_compress_t output_compress; /* SF_OUTPUT_COMPRESS_OKLCH */
  double out_luminance_boost;  /* 1.0 = pre-gamut XYZ multiplier before OkLCh compressor */
} sf_sim_params_t;

void sf_sim_params_defaults(sf_sim_params_t *p);
/* convenience: fill input or output side with sRGB / ProPhoto matrices */
void sf_sim_params_set_input_srgb(sf_sim_params_t *p);
void sf_sim_params_set_input_prophoto(sf_sim_params_t *p);
void sf_sim_params_set_input_rec2020(sf_sim_params_t *p);
void sf_sim_params_set_output_srgb(sf_sim_params_t *p);
void sf_sim_params_set_output_rec2020(sf_sim_params_t *p);

/* --------------------------------------------------------------- build -- */

/* Build all runtime tables. print may be NULL when params->scan_film.
 * Seeds params->gamma_* coupler defaults and neutral filters from the pack
 * unless the caller already customized them (see .neutral_from_db). */
sf_sim_t *sf_sim_build(const sf_pack_t *pack, const sf_profile_t *film,
                       const sf_profile_t *print, const sf_sim_params_t *params,
                       char **errmsg);
void sf_sim_free(sf_sim_t *sim);

/* info for the caller's spatial effects */
double sf_sim_film_dmax(const sf_sim_t *sim, int ch); /* normalized curve max */

/* ------------------------------------------------------ per-pixel API --- */
/* All buffers are interleaved float with `nch` floats per pixel (>= 3);
 * channels 0..2 are read/written, remaining channels are left untouched.
 * In-place operation (in == out) is allowed for every stage. */

/* linear input RGB -> linear film raw exposure (includes 2^ev) */
void sf_sim_expose(const sf_sim_t *sim, const float *rgb_in, float *raw,
                   size_t npix, int nch_in, int nch_out);

/* raw -> log10(max(raw,0) + 1e-10), in place */
void sf_sim_lograw(float *raw, size_t npix, int nch);

/* DIR coupler correction field (to be spatially blurred by the caller).
 * corr is a 3-channel interleaved buffer. No-op fill of zeros when couplers
 * are inactive. */
void sf_sim_develop_corr(const sf_sim_t *sim, const float *lograw, float *corr,
                         size_t npix, int nch_in);

/* (lograw, blurred corr) -> cmy film density. corr may be NULL (no couplers). */
void sf_sim_develop(const sf_sim_t *sim, const float *lograw, const float *corr,
                    float *cmy, size_t npix, int nch_in, int nch_out);

/* cmy film density -> log print raw exposure (through the enlarger) */
void sf_sim_print_expose(const sf_sim_t *sim, const float *cmy, float *lograw,
                         size_t npix, int nch_in, int nch_out);

/* log print raw -> cmy print density */
void sf_sim_print_develop(const sf_sim_t *sim, const float *lograw, float *cmy,
                          size_t npix, int nch_in, int nch_out);

/* cmy density (print, or film when scan_film) -> linear output RGB,
 * gamut compressed per params->output_compress */
void sf_sim_scan(const sf_sim_t *sim, const float *cmy, float *rgb_out,
                 size_t npix, int nch_in, int nch_out);

/* convenience: the full deterministic chain without spatial effects */
void sf_sim_process(const sf_sim_t *sim, const float *rgb_in, float *rgb_out,
                    size_t npix, int nch_in, int nch_out);

/* pre-compression OkLab lightness a single RGB triple would land at, using
 * boost_override in place of the sim's own out_luminance_boost -- see
 * spektra_sim.c for the full rationale (used by the precompression-boost
 * picker in spektrafilm.c). */
float sf_sim_probe_lightness(const sf_sim_t *sim, const float rgb_in[3], float boost_override);

#ifdef __cplusplus
}
#endif
