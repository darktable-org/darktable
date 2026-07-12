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

/* spektra_sim.c — native port of the spektrafilm runtime (see spektra_sim.h).
 *
 * Ported from spektrafilm 0.3.3 (GPLv3, Andrea Volpato). Section markers
 * reference the Python files each block mirrors so future spektrafilm
 * releases can be diffed against this port:
 *
 *   [su]  utils/spectral_upsampling.py    tc_lut build, tri/quad transforms
 *   [gc]  utils/gamut_compression.py      Reinhard knee, xy radial, oklch
 *   [fi]  utils/fast_interp_lut.py        Mitchell 2D cubic, PCHIP 3D
 *   [dc]  model/density_curves.py         exposure->density interpolation
 *   [cp]  model/couplers.py               DIR coupler chemistry
 *   [mc]  utils/morph_curves.py           s023 print-curve morph (cdfs)
 *   [cf]  model/color_filters.py          dichroic enlarger filters
 *   [st]  runtime stage modules (stages dir)             stage orchestration & constants
 */

#include "spektra_sim.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SF_LOG_EPS 1e-10
#define SF_TC_KNEE_T 0.0 /* [gc] InputGamutCompressSpec.knee */
#define SF_TC_KNEE_L 1.0
#define SF_TC_KNEE_P 6.0
#define SF_OUT_KNEE_T 0.0 /* [gc] OutputGamutCompressSpec.knee */
#define SF_OUT_KNEE_L 1.0
#define SF_OUT_KNEE_P 6.0
#define SF_OUT_LIGHT_T 0.7 /* [gc] lightness_compression default */
#define SF_OUT_LIGHT_L 1.0
#define SF_OUT_LIGHT_P 2.2
#define SF_CMAX_NL 64  /* [gc] _OKLCH_CMAX_TABLE_N_L */
#define SF_CMAX_NH 720 /* [gc] _OKLCH_CMAX_TABLE_N_H */
#define SF_CMAX_NBISECT 18
#define SF_MIDGRAY 0.184

/* ------------------------------------------------------------------------ */
/* small linear algebra                                                     */
/* ------------------------------------------------------------------------ */

static void mat3_mul(double out[9], const double a[9], const double b[9])
{
  double r[9];
  for(int i = 0; i < 3; i++)
    for(int j = 0; j < 3; j++)
      r[3 * i + j] = a[3 * i + 0] * b[0 + j] + a[3 * i + 1] * b[3 + j] + a[3 * i + 2] * b[6 + j];
  memcpy(out, r, sizeof(r));
}

static void mat3_mulv(double out[3], const double m[9], const double v[3])
{
  double r0 = m[0] * v[0] + m[1] * v[1] + m[2] * v[2];
  double r1 = m[3] * v[0] + m[4] * v[1] + m[5] * v[2];
  double r2 = m[6] * v[0] + m[7] * v[1] + m[8] * v[2];
  out[0] = r0;
  out[1] = r1;
  out[2] = r2;
}

static int mat3_inv(double out[9], const double m[9])
{
  const double a = m[0], b = m[1], c = m[2];
  const double d = m[3], e = m[4], f = m[5];
  const double g = m[6], h = m[7], i = m[8];
  const double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
  const double det = a * A + b * B + c * C;
  if(fabs(det) < 1e-15) return 0;
  const double inv = 1.0 / det;
  out[0] = A * inv;
  out[1] = -(b * i - c * h) * inv;
  out[2] = (b * f - c * e) * inv;
  out[3] = B * inv;
  out[4] = (a * i - c * g) * inv;
  out[5] = -(a * f - c * d) * inv;
  out[6] = C * inv;
  out[7] = -(a * h - b * g) * inv;
  out[8] = (a * e - b * d) * inv;
  return 1;
}

/* whitepoint xy (Y=1) -> XYZ */
static void xy_to_XYZ(double out[3], const double xy[2])
{
  const double y = fmax(xy[1], 1e-10);
  out[0] = xy[0] / y;
  out[1] = 1.0;
  out[2] = (1.0 - xy[0] - xy[1]) / y;
}

/* von Kries chromatic adaptation matrix in a given cone space:
 * A = M^-1 · diag(cone_dst / cone_src) · M */
static void cat_matrix(double out[9], const double cone_m[9], const double src_xy[2],
                       const double dst_xy[2])
{
  double src_XYZ[3], dst_XYZ[3], cs[3], cd[3], minv[9], d[9] = { 0 };
  xy_to_XYZ(src_XYZ, src_xy);
  xy_to_XYZ(dst_XYZ, dst_xy);
  mat3_mulv(cs, cone_m, src_XYZ);
  mat3_mulv(cd, cone_m, dst_XYZ);
  d[0] = cd[0] / cs[0];
  d[4] = cd[1] / cs[1];
  d[8] = cd[2] / cs[2];
  mat3_inv(minv, cone_m);
  double tmp[9];
  mat3_mul(tmp, d, cone_m);
  mat3_mul(out, minv, tmp);
}

/* CAT16 cone matrix (Li et al. 2017) — used by spektrafilm's input side */
static const double SF_M_CAT16[9] = { 0.401288, 0.650173, -0.051461, -0.250268, 1.204414,
                                      0.045854, -0.002079, 0.048952, 0.953127 };
/* CAT02 cone matrix — colour.XYZ_to_RGB default, used by the scanning side */
static const double SF_M_CAT02[9] = { 0.7328, 0.4286, -0.1624, -0.7036, 1.6975,
                                      0.0061, 0.0030,  0.0136, 0.9834 };

/* OkLab matrices (Ottosson 2020), as used by colour-science */
static const double SF_OKLAB_M1[9]
    = { 0.8189330101, 0.3618667424, -0.1288597137, 0.0329845436, 0.9293118715,
        0.0361456387, 0.0482003018, 0.2643662691, 0.6338517070 };
static const double SF_OKLAB_M2[9]
    = { 0.2104542553, 0.7936177850, -0.0040720468, 1.9779984951, -2.4285922050,
        0.4505937099, 0.0259040371, 0.7827717662, -0.8086757660 };

/* ------------------------------------------------------------------------ */
/* internal structures                                                      */
/* ------------------------------------------------------------------------ */

struct sf_pack_t
{
  char *version;
  double wavelengths[SF_NWL];
  double log_exposure[SF_NLE];
  double cmfs[SF_NWL][3];
  /* spectral locus polygon (closed: first vertex repeated at the end) */
  int locus_n; /* number of vertices incl. the repeated closing vertex */
  double (*locus)[2];
  GHashTable *illuminants;      /* name -> double[SF_NWL] */
  GHashTable *dichroics;        /* brand -> double[SF_NWL*3] */
  JsonNode *neutral_filters;    /* nested object database */
  JsonNode *film_defaults;      /* per-film render defaults */
  JsonParser *parser;           /* keeps the JSON tree alive */
  /* hanatos2025 irradiance spectra LUT */
  int tc_n;                     /* 192 */
  float *spectra;               /* tc_n * tc_n * SF_NWL */
};

typedef struct sf_curves_model_t
{
  int n_layers;
  double centers[3][8], amplitudes[3][8], sigmas[3][8];
} sf_curves_model_t;

struct sf_profile_t
{
  char *stock, *name, *type, *support, *stage, *use, *antihalation;
  char *target_print, *channel_model;
  char *reference_illuminant, *viewing_illuminant;
  double log_sensitivity[SF_NWL][3];
  double channel_density[SF_NWL][3];
  double base_density[SF_NWL];
  double log_exposure[SF_NLE];
  double density_curves[SF_NLE][3];
  int window_n;
  double window_params[8];
  sf_curves_model_t curves_model;
};

struct sf_sim_t
{
  sf_sim_params_t p;
  int film_positive;
  int film_bw; /* single-emulsion stock widened to 3 channels: couple the grain */
  int print_positive;

  /* filming */
  double m_in[9];    /* input linear RGB -> XYZ adapted to film ref illuminant */
  double ev_scale;
  int tc_n;
  double *tc_lut;    /* tc_n*tc_n*3 raw CMY exposure */

  /* film develop */
  double le0, le_step; /* uniform log exposure grid */
  double curves_norm[SF_NLE][3];
  double curves_before[SF_NLE][3];
  double gamma[3];
  double couplers_M[3][3];
  /* Langmuir saturating couplers (spektrafilm dev/0.4+); K = INFINITY keeps
     the 0.3.x linear model. Donor side (negative film): inhibitor release
     g(D) = D (K + D_ref)/(K + D). Receiver side (positive/reversal film):
     response S(c) = c (Kr + c_ref)/(Kr + c), applied AFTER spatial diffusion.
     D_ref = d_max/2; c_ref from the amount-independent unit matrix. */
  /* DIR coupler inhibitor diffusion: gaussian core + exponential tail
     (upstream models the tail as a 3-gaussian mixture, amp/ratio identical
     to the halation tail constants). Per-film from film_render_defaults. */
  double coupler_diff_um, coupler_tail_um, coupler_tail_w;
  double couplers_donor_K[3], couplers_donor_Dref[3];
  double couplers_recv_Kr[3], couplers_recv_cref[3];
  int couplers_donor_lm, couplers_recv_lm; /* donor row -> receiver col, scaled by amount */
  int couplers_active;
  double film_dmax[3]; /* max of normalized film curves */
  double film_dmin[3]; /* the SAME curves' own floor (mn); grain's D_ref = 1+dmin
                           must use this, not an independently-sourced value, or
                           dmax_c+dmin_c no longer reconstructs the real absolute
                           D-max and the particle count silently drifts */
  /* per-film grain catalogue data (film_render_defaults[stock].grain); the
     density floor lives in p.grain_density_min (shared with the enlarger/scan
     table-range code below). Defaults to the legacy fixed constants when the
     pack has no per-film grain entry (see sf_sim_build). */
  double grain_rms[3], grain_uniformity[3];

  /* print exposure (exact spectral path) */
  int has_print;
  double illum_print[SF_NWL];    /* enlarger source × dichroic pack */
  double illum_preflash[SF_NWL];
  double print_sens[SF_NWL][3];
  double film_chan_density[SF_NWL][3];
  double film_base_density[SF_NWL];
  double midgray_factor;         /* scalar exposure factor (geomean logic) */
  double preflash_raw[3];
  double print_exposure;
  double enl_lo[3], enl_hi[3];

  /* print develop */
  double print_curves[SF_NLE][3];

  /* scanning (exact spectral path) */
  double scan_chan_density[SF_NWL][3];
  double scan_base_density[SF_NWL];
  double illum_view[SF_NWL];
  double cmfs[SF_NWL][3];
  double xyz_norm;
  double illum_view_xyz[3];
  double scan_lo[3], scan_hi[3];
  double m_out[9]; /* XYZ (viewing illum) -> linear output RGB (CAT02) */
  /* scanner black/white point correction (positive film scans only):
     xyz *= clip(bw_m*Y + bw_q, 0, 1)/Y after xyz = 10^log_xyz.
     Mirrors color_reference.black_white_xyz_correction with
     black_correction = white_correction = true (levels 0.01 / 0.98). */
  int scan_bw_on;
  double scan_bw_m, scan_bw_q;

  /* 3D tables + PCHIP preparation (NULL when lut_steps == 0) */
  int lut_steps;
  double *enl_lut, *enl_sx, *enl_sy, *enl_sz, *enl_cmin, *enl_cmax;
  double *scan_lut, *scan_sx, *scan_sy, *scan_sz, *scan_cmin, *scan_cmax;

  /* output gamut compression */
  sf_output_compress_t out_compress;
  double out_rgb2xyz[9], out_xyz2rgb[9];
  double oklab_m1inv[9], oklab_m2inv[9];
  float *cmax; /* SF_CMAX_NL × SF_CMAX_NH */
};

/* ------------------------------------------------------------------------ */
/* JSON helpers                                                             */
/* ------------------------------------------------------------------------ */

/* null JSON elements decode to NaN (JSON has no NaN literal; the exporter
 * writes null for non-finite values) */
static inline double json_elem_double(JsonArray *arr, int i)
{
  JsonNode *node = json_array_get_element(arr, i);
  if(!node || json_node_is_null(node)) return NAN;
  return json_node_get_double(node);
}

static gboolean json_read_darray(JsonObject *obj, const char *key, double *out, int n)
{
  if(!json_object_has_member(obj, key)) return FALSE;
  JsonNode *node = json_object_get_member(obj, key);
  if(!node || !JSON_NODE_HOLDS_ARRAY(node)) return FALSE; /* tolerate null values */
  JsonArray *arr = json_node_get_array(node);
  if(!arr || (int)json_array_get_length(arr) != n) return FALSE;
  for(int i = 0; i < n; i++) out[i] = json_elem_double(arr, i);
  return TRUE;
}

/* read an n×m nested array into row-major out */
static gboolean json_read_dmatrix(JsonObject *obj, const char *key, double *out, int n, int m)
{
  if(!json_object_has_member(obj, key)) return FALSE;
  JsonNode *node = json_object_get_member(obj, key);
  if(!node || !JSON_NODE_HOLDS_ARRAY(node)) return FALSE;
  JsonArray *arr = json_node_get_array(node);
  if(!arr || (int)json_array_get_length(arr) != n) return FALSE;
  for(int i = 0; i < n; i++)
  {
    JsonArray *row = json_array_get_array_element(arr, i);
    if(!row || (int)json_array_get_length(row) != m) return FALSE;
    for(int j = 0; j < m; j++) out[i * m + j] = json_elem_double(row, j);
  }
  return TRUE;
}

static char *json_dup_string(JsonObject *obj, const char *key)
{
  if(!json_object_has_member(obj, key)) return NULL;
  JsonNode *node = json_object_get_member(obj, key);
  if(json_node_is_null(node)) return NULL;
  return g_strdup(json_node_get_string(node));
}

static void set_error(char **errmsg, const char *fmt, ...)
{
  if(!errmsg) return;
  va_list ap;
  va_start(ap, fmt);
  *errmsg = g_strdup_vprintf(fmt, ap);
  va_end(ap);
}

/* ------------------------------------------------------------------------ */
/* pack loading                                                             */
/* ------------------------------------------------------------------------ */

void sf_pack_free(sf_pack_t *pack)
{
  if(!pack) return;
  g_free(pack->version);
  g_free(pack->locus);
  if(pack->illuminants) g_hash_table_destroy(pack->illuminants);
  if(pack->dichroics) g_hash_table_destroy(pack->dichroics);
  if(pack->parser) g_object_unref(pack->parser);
  free(pack->spectra);
  g_free(pack);
}

sf_pack_t *sf_pack_load(const char *dir, char **errmsg)
{
  sf_pack_t *pack = g_new0(sf_pack_t, 1);
  char *json_path = g_build_filename(dir, "pack.json", NULL);
  char *lut_path = g_build_filename(dir, "spectra_lut.f32", NULL);

  pack->parser = json_parser_new();
  GError *gerr = NULL;
  if(!json_parser_load_from_file(pack->parser, json_path, &gerr))
  {
    set_error(errmsg, "spektra_sim: cannot parse %s: %s", json_path,
              gerr ? gerr->message : "unknown");
    g_clear_error(&gerr);
    goto fail;
  }
  JsonObject *root = json_node_get_object(json_parser_get_root(pack->parser));
  pack->version = json_dup_string(root, "spektrafilm_version");

  if(!json_read_darray(root, "wavelengths", pack->wavelengths, SF_NWL)
     || !json_read_darray(root, "log_exposure", pack->log_exposure, SF_NLE)
     || !json_read_dmatrix(root, "cmfs", &pack->cmfs[0][0], SF_NWL, 3))
  {
    set_error(errmsg, "spektra_sim: pack.json misses wavelengths/log_exposure/cmfs "
                      "or grid sizes changed (expected %d wavelengths, %d exposures)",
              SF_NWL, SF_NLE);
    goto fail;
  }

  /* spectral locus polygon */
  {
    JsonArray *arr = json_object_get_array_member(root, "spectral_locus_xy");
    if(!arr)
    {
      set_error(errmsg, "spektra_sim: pack.json misses spectral_locus_xy");
      goto fail;
    }
    pack->locus_n = json_array_get_length(arr);
    pack->locus = g_malloc0(sizeof(double) * 2 * pack->locus_n);
    for(int i = 0; i < pack->locus_n; i++)
    {
      JsonArray *row = json_array_get_array_element(arr, i);
      pack->locus[i][0] = json_array_get_double_element(row, 0);
      pack->locus[i][1] = json_array_get_double_element(row, 1);
    }
  }

  /* illuminants */
  pack->illuminants = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  {
    JsonObject *ill = json_object_get_object_member(root, "illuminants");
    GList *members = ill ? json_object_get_members(ill) : NULL;
    for(GList *m = members; m; m = m->next)
    {
      double *spd = g_new(double, SF_NWL);
      if(json_read_darray(ill, m->data, spd, SF_NWL))
        g_hash_table_insert(pack->illuminants, g_strdup(m->data), spd);
      else
        g_free(spd);
    }
    g_list_free(members);
  }

  /* dichroic filter curves */
  pack->dichroics = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  {
    JsonObject *df = json_object_get_object_member(root, "dichroic_filters");
    GList *members = df ? json_object_get_members(df) : NULL;
    for(GList *m = members; m; m = m->next)
    {
      double *f = g_new(double, SF_NWL * 3);
      if(json_read_dmatrix(df, m->data, f, SF_NWL, 3))
        g_hash_table_insert(pack->dichroics, g_strdup(m->data), f);
      else
        g_free(f);
    }
    g_list_free(members);
  }

  if(json_object_has_member(root, "neutral_print_filters"))
    pack->neutral_filters = json_object_get_member(root, "neutral_print_filters");
  if(json_object_has_member(root, "film_render_defaults"))
    pack->film_defaults = json_object_get_member(root, "film_render_defaults");

  /* hanatos2025 spectra LUT */
  {
    FILE *fh = g_fopen(lut_path, "rb");
    if(!fh)
    {
      set_error(errmsg, "spektra_sim: cannot open %s", lut_path);
      goto fail;
    }
    char magic[4];
    int32_t dims[3];
    if(fread(magic, 1, 4, fh) != 4 || memcmp(magic, "SFSL", 4) != 0
       || fread(dims, 4, 3, fh) != 3 || dims[0] != dims[1] || dims[2] != SF_NWL)
    {
      set_error(errmsg, "spektra_sim: bad spectra_lut header in %s", lut_path);
      fclose(fh);
      goto fail;
    }
    pack->tc_n = dims[0];
    const size_t count = (size_t)dims[0] * dims[1] * dims[2];
    pack->spectra = malloc(count * sizeof(float));
    if(!pack->spectra || fread(pack->spectra, sizeof(float), count, fh) != count)
    {
      set_error(errmsg, "spektra_sim: truncated spectra lut %s", lut_path);
      fclose(fh);
      goto fail;
    }
    fclose(fh);
  }

  g_free(json_path);
  g_free(lut_path);
  return pack;

fail:
  g_free(json_path);
  g_free(lut_path);
  sf_pack_free(pack);
  return NULL;
}

const char *sf_pack_version(const sf_pack_t *pack)
{
  return pack ? pack->version : NULL;
}

bool sf_pack_neutral_filters(const sf_pack_t *pack, const char *print_stock,
                             const char *illuminant, const char *film_stock, double cmy[3])
{
  if(!pack || !pack->neutral_filters) return false;
  JsonObject *db = json_node_get_object(pack->neutral_filters);
  if(!db || !json_object_has_member(db, print_stock)) return false;
  JsonObject *by_ill = json_object_get_object_member(db, print_stock);
  if(!by_ill || !json_object_has_member(by_ill, illuminant)) return false;
  JsonObject *by_film = json_object_get_object_member(by_ill, illuminant);
  if(!by_film || !json_object_has_member(by_film, film_stock)) return false;
  JsonArray *arr = json_object_get_array_member(by_film, film_stock);
  if(!arr || json_array_get_length(arr) != 3) return false;
  for(int i = 0; i < 3; i++) cmy[i] = json_array_get_double_element(arr, i);
  return true;
}

bool sf_pack_film_coupler_diffusion(const sf_pack_t *pack, const char *film_stock,
                                    double *size_um, double *tail_um, double *tail_w)
{
  if(!pack || !pack->film_defaults) return false;
  JsonObject *db = json_node_get_object(pack->film_defaults);
  if(!db || !json_object_has_member(db, film_stock)) return false;
  JsonObject *film = json_object_get_object_member(db, film_stock);
  if(!film || !json_object_has_member(film, "dir_couplers")) return false;
  JsonObject *dc = json_object_get_object_member(film, "dir_couplers");
  if(!dc) return false;
  gboolean ok = FALSE;
  if(json_object_has_member(dc, "diffusion_size_um"))
  {
    *size_um = json_object_get_double_member(dc, "diffusion_size_um");
    ok = TRUE;
  }
  if(json_object_has_member(dc, "diffusion_tail_um"))
    *tail_um = json_object_get_double_member(dc, "diffusion_tail_um");
  if(json_object_has_member(dc, "diffusion_tail_weight"))
    *tail_w = json_object_get_double_member(dc, "diffusion_tail_weight");
  return ok;
}

/* Per-film grain catalogue data: film_render_defaults[stock].grain in the
   pack, exported verbatim from spektrafilm's GrainParams (rms_granularity,
   uniformity, density_min — see spektrafilm_export_data.py's _grain_export).
   Any output pointer may be NULL. Returns false and leaves outputs untouched
   if the stock has no "grain" entry (older packs, or the stock predates
   per-film grain), so the caller can keep its fallback constants. */
bool sf_pack_film_grain(const sf_pack_t *pack, const char *film_stock,
                        double rms[3], double uniformity[3], double density_min[3])
{
  if(!pack || !pack->film_defaults) return false;
  JsonObject *db = json_node_get_object(pack->film_defaults);
  if(!db || !json_object_has_member(db, film_stock)) return false;
  JsonObject *film = json_object_get_object_member(db, film_stock);
  if(!film || !json_object_has_member(film, "grain")) return false;
  JsonObject *gr = json_object_get_object_member(film, "grain");
  if(!gr) return false;
  gboolean ok = FALSE;
  if(rms && json_object_has_member(gr, "rms_granularity"))
  {
    ok = json_read_darray(gr, "rms_granularity", rms, 3) || ok;
  }
  if(uniformity && json_object_has_member(gr, "uniformity"))
    ok = json_read_darray(gr, "uniformity", uniformity, 3) || ok;
  if(density_min && json_object_has_member(gr, "density_min"))
    ok = json_read_darray(gr, "density_min", density_min, 3) || ok;
  return ok;
}

bool sf_pack_film_langmuir(const sf_pack_t *pack, const char *film_stock,
                           double donor_k[3], double receiver_k[3])
{
  if(!pack || !pack->film_defaults) return false;
  JsonObject *db = json_node_get_object(pack->film_defaults);
  if(!db || !json_object_has_member(db, film_stock)) return false;
  JsonObject *film = json_object_get_object_member(db, film_stock);
  if(!film || !json_object_has_member(film, "dir_couplers")) return false;
  JsonObject *dc = json_object_get_object_member(film, "dir_couplers");
  if(!dc || !json_object_has_member(dc, "langmuir_donor_k_rgb")) return false;
  json_read_darray(dc, "langmuir_donor_k_rgb", donor_k, 3);
  json_read_darray(dc, "langmuir_receiver_k_rgb", receiver_k, 3);
  return true;
}

bool sf_pack_film_defaults(const sf_pack_t *pack, const char *film_stock,
                           double gamma_samelayer[3], double gamma_inter_r_gb[2],
                           double gamma_inter_g_rb[2], double gamma_inter_b_rg[2],
                           double halation_strength[3], double halation_sigma_um[3],
                           double scatter_core_um[3], double scatter_tail_um[3],
                           double scatter_tail_weight[3])
{
  if(!pack || !pack->film_defaults) return false;
  JsonObject *db = json_node_get_object(pack->film_defaults);
  if(!db || !json_object_has_member(db, film_stock)) return false;
  JsonObject *entry = json_object_get_object_member(db, film_stock);
  JsonObject *dc = json_object_get_object_member(entry, "dir_couplers");
  JsonObject *ha = json_object_get_object_member(entry, "halation");
  if(dc)
  {
    if(gamma_samelayer) json_read_darray(dc, "gamma_samelayer_rgb", gamma_samelayer, 3);
    if(gamma_inter_r_gb) json_read_darray(dc, "gamma_interlayer_r_to_gb", gamma_inter_r_gb, 2);
    if(gamma_inter_g_rb) json_read_darray(dc, "gamma_interlayer_g_to_rb", gamma_inter_g_rb, 2);
    if(gamma_inter_b_rg) json_read_darray(dc, "gamma_interlayer_b_to_rg", gamma_inter_b_rg, 2);
  }
  if(ha)
  {
    if(halation_strength) json_read_darray(ha, "strength", halation_strength, 3);
    if(halation_sigma_um) json_read_darray(ha, "first_sigma_um", halation_sigma_um, 3);
    if(scatter_core_um) json_read_darray(ha, "scatter_core_um", scatter_core_um, 3);
    if(scatter_tail_um) json_read_darray(ha, "scatter_tail_um", scatter_tail_um, 3);
    if(scatter_tail_weight) json_read_darray(ha, "scatter_tail_weight", scatter_tail_weight, 3);
  }
  return true;
}

/* ------------------------------------------------------------------------ */
/* profile loading                                                          */
/* ------------------------------------------------------------------------ */

void sf_profile_free(sf_profile_t *p)
{
  if(!p) return;
  g_free(p->stock);
  g_free(p->name);
  g_free(p->type);
  g_free(p->support);
  g_free(p->stage);
  g_free(p->use);
  g_free(p->antihalation);
  g_free(p->target_print);
  g_free(p->channel_model);
  g_free(p->reference_illuminant);
  g_free(p->viewing_illuminant);
  g_free(p);
}

sf_profile_t *sf_profile_load(const char *path, char **errmsg)
{
  JsonParser *parser = json_parser_new();
  GError *gerr = NULL;
  sf_profile_t *p = NULL;
  if(!json_parser_load_from_file(parser, path, &gerr))
  {
    set_error(errmsg, "spektra_sim: cannot parse profile %s: %s", path,
              gerr ? gerr->message : "unknown");
    g_clear_error(&gerr);
    g_object_unref(parser);
    return NULL;
  }
  JsonObject *root = json_node_get_object(json_parser_get_root(parser));
  JsonObject *info = json_object_get_object_member(root, "info");
  JsonObject *data = json_object_get_object_member(root, "data");
  if(!info || !data)
  {
    set_error(errmsg, "spektra_sim: profile %s misses info/data", path);
    g_object_unref(parser);
    return NULL;
  }

  p = g_new0(sf_profile_t, 1);
  p->stock = json_dup_string(info, "stock");
  p->name = json_dup_string(info, "name");
  p->type = json_dup_string(info, "type");
  p->support = json_dup_string(info, "support");
  p->stage = json_dup_string(info, "stage");
  p->use = json_dup_string(info, "use");
  p->antihalation = json_dup_string(info, "antihalation");
  p->target_print = json_dup_string(info, "target_print");
  p->channel_model = json_dup_string(info, "channel_model");
  p->reference_illuminant = json_dup_string(info, "reference_illuminant");
  p->viewing_illuminant = json_dup_string(info, "viewing_illuminant");

  gboolean ok = TRUE;
  double wavelengths[SF_NWL];
  ok &= json_read_darray(data, "wavelengths", wavelengths, SF_NWL);
  ok &= json_read_dmatrix(data, "log_sensitivity", &p->log_sensitivity[0][0], SF_NWL, 3);
  ok &= json_read_dmatrix(data, "channel_density", &p->channel_density[0][0], SF_NWL, 3);
  ok &= json_read_darray(data, "base_density", p->base_density, SF_NWL);
  ok &= json_read_darray(data, "log_exposure", p->log_exposure, SF_NLE);
  ok &= json_read_dmatrix(data, "density_curves", &p->density_curves[0][0], SF_NLE, 3);
  if(!ok)
  {
    set_error(errmsg, "spektra_sim: profile %s has unexpected data shapes "
                      "(model grid change? re-run the exporter and update the module)",
              path);
    sf_profile_free(p);
    g_object_unref(parser);
    return NULL;
  }

  /* optional pieces */
  if(json_object_has_member(data, "hanatos2025_adaptation_window_params"))
  {
    JsonNode *node = json_object_get_member(data, "hanatos2025_adaptation_window_params");
    JsonArray *arr = (node && JSON_NODE_HOLDS_ARRAY(node)) ? json_node_get_array(node) : NULL;
    p->window_n = arr ? MIN((int)json_array_get_length(arr), 8) : 0;
    for(int i = 0; i < p->window_n; i++)
      p->window_params[i] = json_array_get_double_element(arr, i);
  }
  if(json_object_has_member(data, "density_curves_model"))
  {
    JsonNode *mnode = json_object_get_member(data, "density_curves_model");
    JsonObject *m = (mnode && JSON_NODE_HOLDS_OBJECT(mnode)) ? json_node_get_object(mnode) : NULL;
    JsonNode *cnode = (m && json_object_has_member(m, "centers"))
                          ? json_object_get_member(m, "centers") : NULL;
    JsonArray *centers = (cnode && JSON_NODE_HOLDS_ARRAY(cnode)) ? json_node_get_array(cnode) : NULL;
    /* The reference package's own DensityCurvesModel stores centers/
       amplitudes/sigmas as (n_channels=3, n_layers) -- confirmed directly
       against its bundled kodak_portra_endura.json, byte-identical to our
       copy, and against apply_print_curves_morph's own
       "n_channels = model.centers.shape[0]". That's the normal,
       channel-major case (outer array length 3) and every profile checked
       against the reference package matches it.
       kodak_2302.json (not part of the reference package's own bundled
       profiles -- a separately-authored addition) stores it transposed:
       (n_layers=5, n_channels=3), outer length 5. Rather than assume one
       convention universally (an earlier version of this fix did, and
       broke every normal profile by mis-transposing correctly-shaped
       data), detect orientation per-profile from the one invariant that
       always holds: there are exactly 3 channels. */
    const int outer_len = centers ? MIN((int)json_array_get_length(centers), 8) : 0;
    JsonArray *row0 = (outer_len > 0) ? json_array_get_array_element(centers, 0) : NULL;
    const int inner_len = row0 ? (int)json_array_get_length(row0) : 0;
    const gboolean channel_major = (outer_len == 3);              /* centers[channel][layer]: normal */
    const gboolean layer_major = (!channel_major && inner_len == 3); /* centers[layer][channel]: e.g. kodak_2302 */
    if(channel_major || layer_major)
    {
      const int nl = channel_major ? inner_len : outer_len;
      p->curves_model.n_layers = nl;
      double c[24], a[24], s[24];
      if(json_read_dmatrix(m, "centers", c, outer_len, inner_len)
         && json_read_dmatrix(m, "amplitudes", a, outer_len, inner_len)
         && json_read_dmatrix(m, "sigmas", s, outer_len, inner_len))
      {
        for(int ch = 0; ch < 3; ch++)
          for(int l = 0; l < nl; l++)
          {
            const int idx = channel_major ? (ch * nl + l) : (l * 3 + ch);
            p->curves_model.centers[ch][l] = c[idx];
            p->curves_model.amplitudes[ch][l] = a[idx];
            p->curves_model.sigmas[ch][l] = s[idx];
          }
      }
      else
        p->curves_model.n_layers = 0; /* malformed data: don't leave partial state */
    }
  }

  g_object_unref(parser);
  return p;
}

const char *sf_profile_stock(const sf_profile_t *p) { return p->stock; }
const char *sf_profile_name(const sf_profile_t *p) { return p->name; }
const char *sf_profile_stage(const sf_profile_t *p) { return p->stage; }
const char *sf_profile_type(const sf_profile_t *p) { return p->type; }
const char *sf_profile_target_print(const sf_profile_t *p) { return p->target_print; }

/* ------------------------------------------------------------------------ */
/* parameter defaults & colour spaces                                       */
/* ------------------------------------------------------------------------ */

/* linear RGB -> XYZ matrices (source-white relative), colour-science values */
/* NOTE: colour-science ships the *published rounded* matrices for sRGB and
 * ProPhoto (not the primaries-derived ones); we match those exactly so the
 * numerics agree with the spektrafilm reference. */
static const double SF_M_SRGB_TO_XYZ[9]
    = { 0.4124, 0.3576, 0.1805, 0.2126, 0.7152, 0.0722, 0.0193, 0.1192, 0.9505 };
static const double SF_SRGB_WHITE_XY[2] = { 0.3127, 0.3290 };

static const double SF_M_PROPHOTO_TO_XYZ[9]
    = { 0.7977, 0.1352, 0.0313, 0.2880, 0.7119, 0.0001, 0.0, 0.0, 0.8249 };
static const double SF_D50_WHITE_XY[2] = { 0.3457, 0.3585 };

static const double SF_M_REC2020_TO_XYZ[9]
    = { 0.6369580483012913, 0.1446169035862083, 0.1688809751641721,
        0.2627002120112671, 0.6779980715188708, 0.0593017164698620,
        0.0000000000000000, 0.0280726930490874, 1.0609850577107909 };

void sf_sim_params_defaults(sf_sim_params_t *p)
{
  memset(p, 0, sizeof(*p));
  p->exposure_comp_ev = 0.0;
  p->density_curve_gamma = 1.0;
  p->couplers_active = true;
  p->couplers_amount = 1.0;
  /* generic negative-film gammas ([st] params_builder); overwritten from the
   * pack's per-film digested defaults in sf_sim_build() */
  const double gs[3] = { 0.336, 0.319, 0.273 };
  const double gr[2] = { 0.353, 0.302 }, gg[2] = { 0.154, 0.353 }, gb[2] = { 0.168, 0.226 };
  memcpy(p->gamma_samelayer, gs, sizeof(gs));
  memcpy(p->gamma_inter_r_gb, gr, sizeof(gr));
  memcpy(p->gamma_inter_g_rb, gg, sizeof(gg));
  memcpy(p->gamma_inter_b_rg, gb, sizeof(gb));
  p->inhibition_samelayer = 1.0;
  p->inhibition_interlayer = 1.0;
  p->grain_density_min[0] = p->grain_density_min[1] = p->grain_density_min[2] = 0.03;
  p->enlarger_illuminant = "TH-KG3";
  p->dichroic_brand = "custom";
  p->print_exposure = 1.0;
  p->print_exposure_compensation = true;
  p->normalize_print_exposure = true;
  p->c_filter_neutral = 0.0;
  p->m_filter_neutral = 65.0;
  p->y_filter_neutral = 55.0;
  p->neutral_from_db = true;
  p->morph_active = false;
  p->morph_gamma = p->morph_gamma_fast = p->morph_gamma_slow = 1.0;
  p->morph_gamma_r = p->morph_gamma_g = p->morph_gamma_b = 1.0;
  p->scan_film = false;
  p->lut_steps = 0;
  p->input_gamut_compress = true;
  p->output_compress = SF_OUTPUT_COMPRESS_OKLCH;
  sf_sim_params_set_input_prophoto(p); /* reference IOParams default */
  sf_sim_params_set_output_srgb(p);
}

void sf_sim_params_set_input_srgb(sf_sim_params_t *p)
{
  memcpy(p->input_rgb_to_xyz, SF_M_SRGB_TO_XYZ, sizeof(SF_M_SRGB_TO_XYZ));
  memcpy(p->input_white_xy, SF_SRGB_WHITE_XY, sizeof(SF_SRGB_WHITE_XY));
}

void sf_sim_params_set_input_prophoto(sf_sim_params_t *p)
{
  memcpy(p->input_rgb_to_xyz, SF_M_PROPHOTO_TO_XYZ, sizeof(SF_M_PROPHOTO_TO_XYZ));
  memcpy(p->input_white_xy, SF_D50_WHITE_XY, sizeof(SF_D50_WHITE_XY));
}

void sf_sim_params_set_input_rec2020(sf_sim_params_t *p)
{
  memcpy(p->input_rgb_to_xyz, SF_M_REC2020_TO_XYZ, sizeof(SF_M_REC2020_TO_XYZ));
  memcpy(p->input_white_xy, SF_SRGB_WHITE_XY, sizeof(SF_SRGB_WHITE_XY));
}

void sf_sim_params_set_output_srgb(sf_sim_params_t *p)
{
  memcpy(p->output_rgb_to_xyz, SF_M_SRGB_TO_XYZ, sizeof(SF_M_SRGB_TO_XYZ));
  mat3_inv(p->output_xyz_to_rgb, SF_M_SRGB_TO_XYZ);
  memcpy(p->output_white_xy, SF_SRGB_WHITE_XY, sizeof(SF_SRGB_WHITE_XY));
}

void sf_sim_params_set_output_rec2020(sf_sim_params_t *p)
{
  memcpy(p->output_rgb_to_xyz, SF_M_REC2020_TO_XYZ, sizeof(SF_M_REC2020_TO_XYZ));
  mat3_inv(p->output_xyz_to_rgb, SF_M_REC2020_TO_XYZ);
  memcpy(p->output_white_xy, SF_SRGB_WHITE_XY, sizeof(SF_SRGB_WHITE_XY));
}

/* ------------------------------------------------------------------------ */
/* [su] triangular <-> square chromaticity coordinates                      */
/* ------------------------------------------------------------------------ */

static inline void tri2quad(double out[2], const double tc[2])
{
  const double tx = tc[0], ty = tc[1];
  double y = ty / fmax(1.0 - tx, 1e-10);
  double x = (1.0 - tx) * (1.0 - tx);
  out[0] = CLAMP(x, 0.0, 1.0);
  out[1] = CLAMP(y, 0.0, 1.0);
}

static inline void quad2tri(double out[2], const double xy[2])
{
  const double sq = sqrt(xy[0]);
  out[0] = 1.0 - sq;
  out[1] = xy[1] * sq;
}

/* ------------------------------------------------------------------------ */
/* [gc] Reinhard knee, radial xy compression toward the spectral locus      */
/* ------------------------------------------------------------------------ */

static inline double reinhard_knee(double d, double threshold, double limit, double power)
{
  if(d <= threshold) return d;
  const double scale = limit - threshold;
  const double x = (d - threshold) / scale;
  const double y = x / pow(1.0 + pow(x, power), 1.0 / power);
  return threshold + scale * y;
}

/* distance from origin along unit direction to the first polygon crossing */
static double ray_polygon_distance(const double origin[2], const double dir[2],
                                   const double (*poly)[2], int n_vertices)
{
  double t_min = INFINITY;
  for(int k = 0; k + 1 < n_vertices; k++)
  {
    const double ax = poly[k][0], ay = poly[k][1];
    const double ex = poly[k + 1][0] - ax, ey = poly[k + 1][1] - ay;
    const double denom = dir[0] * ey - dir[1] * ex;
    if(fabs(denom) <= 1e-12) continue;
    const double ox = origin[0] - ax, oy = origin[1] - ay;
    const double t = (-ox * ey + oy * ex) / denom;
    const double s = (-ox * dir[1] + oy * dir[0]) / denom;
    if(t > 1e-9 && s >= 0.0 && s <= 1.0 && t < t_min) t_min = t;
  }
  return t_min;
}

static void compress_xy_radial(double out[2], const double xy[2], const double white[2],
                               const double (*locus)[2], int locus_n)
{
  const double dx = xy[0] - white[0], dy = xy[1] - white[1];
  const double dist = sqrt(dx * dx + dy * dy);
  if(dist < 1e-9)
  {
    out[0] = xy[0];
    out[1] = xy[1];
    return;
  }
  const double dir[2] = { dx / dist, dy / dist };
  const double boundary = ray_polygon_distance(white, dir, locus, locus_n);
  const double d_norm = dist / fmax(boundary, 1e-12);
  const double d_c = reinhard_knee(d_norm, SF_TC_KNEE_T, SF_TC_KNEE_L, SF_TC_KNEE_P);
  out[0] = white[0] + dir[0] * d_c * boundary;
  out[1] = white[1] + dir[1] * d_c * boundary;
}

/* ------------------------------------------------------------------------ */
/* [fi] Mitchell–Netravali 2D cubic LUT interpolation (reflected bounds)    */
/* ------------------------------------------------------------------------ */

static inline double mitchell_weight(double t)
{
  const double B = 1.0 / 3.0, C = 1.0 / 3.0;
  const double x = fabs(t);
  if(x < 1.0)
    return (1.0 / 6.0)
           * ((12.0 - 9.0 * B - 6.0 * C) * x * x * x + (-18.0 + 12.0 * B + 6.0 * C) * x * x
              + (6.0 - 2.0 * B));
  else if(x < 2.0)
    return (1.0 / 6.0)
           * ((-B - 6.0 * C) * x * x * x + (6.0 * B + 30.0 * C) * x * x
              + (-12.0 * B - 48.0 * C) * x + (8.0 * B + 24.0 * C));
  return 0.0;
}

static inline int safe_index(int idx, int L)
{
  if(idx < 0) return -idx;
  if(idx >= L) return 2 * (L - 1) - idx;
  return idx;
}

static inline void cubic_base_fraction(double coord, int L, int *base, double *frac)
{
  coord = CLAMP(coord, 0.0, (double)(L - 1));
  if(coord >= (double)(L - 1))
  {
    *base = L - 2;
    *frac = 1.0;
    return;
  }
  *base = (int)floor(coord);
  *frac = coord - *base;
}

/* lut: L×L×3 doubles, coords already scaled to [0, L-1] */
static void cubic_interp_2d(double out[3], const double *lut, int L, double x, double y)
{
  int xb, yb;
  double xf, yf;
  cubic_base_fraction(x, L, &xb, &xf);
  cubic_base_fraction(y, L, &yb, &yf);
  double wx[4], wy[4];
  for(int i = 0; i < 4; i++)
  {
    wx[i] = mitchell_weight(xf + 1.0 - i);
    wy[i] = mitchell_weight(yf + 1.0 - i);
  }
  double acc[3] = { 0, 0, 0 }, wsum = 0.0;
  for(int i = 0; i < 4; i++)
  {
    const int xi = safe_index(xb - 1 + i, L);
    for(int j = 0; j < 4; j++)
    {
      const int yj = safe_index(yb - 1 + j, L);
      const double w = wx[i] * wy[j];
      wsum += w;
      const double *px = lut + ((size_t)xi * L + yj) * 3;
      acc[0] += w * px[0];
      acc[1] += w * px[1];
      acc[2] += w * px[2];
    }
  }
  if(wsum != 0.0)
    for(int c = 0; c < 3; c++) acc[c] /= wsum;
  out[0] = acc[0];
  out[1] = acc[1];
  out[2] = acc[2];
}

/* bilinear sampling on the same layout with clamped ("nearest") bounds —
 * used only for the tc_lut compression remap at build time */
static void bilinear_2d_clamped(double out[3], const double *lut, int L, double x, double y)
{
  x = CLAMP(x, 0.0, (double)(L - 1));
  y = CLAMP(y, 0.0, (double)(L - 1));
  const int x0 = (int)floor(x), y0 = (int)floor(y);
  const int x1 = MIN(x0 + 1, L - 1), y1 = MIN(y0 + 1, L - 1);
  const double tx = x - x0, ty = y - y0;
  for(int c = 0; c < 3; c++)
  {
    const double v00 = lut[((size_t)x0 * L + y0) * 3 + c];
    const double v01 = lut[((size_t)x0 * L + y1) * 3 + c];
    const double v10 = lut[((size_t)x1 * L + y0) * 3 + c];
    const double v11 = lut[((size_t)x1 * L + y1) * 3 + c];
    out[c] = (v00 * (1 - ty) + v01 * ty) * (1 - tx) + (v10 * (1 - ty) + v11 * ty) * tx;
  }
}

/* ------------------------------------------------------------------------ */
/* [fi] monotone PCHIP 3D LUT interpolation                                 */
/* ------------------------------------------------------------------------ */

static void fill_monotone_slopes_1d(const double *values, double *slopes, int size)
{
  if(size == 1)
  {
    slopes[0] = 0.0;
    return;
  }
  double deltas[64] = { 0 }; /* zero-init: gcc -Wmaybe-uninitialized cannot prove size bounds */
  for(int i = 0; i < size - 1; i++) deltas[i] = values[i + 1] - values[i];
  if(size == 2)
  {
    slopes[0] = slopes[1] = deltas[0];
    return;
  }
  double left = 0.5 * (3.0 * deltas[0] - deltas[1]);
  if(left * deltas[0] <= 0.0)
    left = 0.0;
  else if(deltas[0] * deltas[1] < 0.0 && fabs(left) > fabs(3.0 * deltas[0]))
    left = 3.0 * deltas[0];
  slopes[0] = left;
  for(int i = 1; i < size - 1; i++)
  {
    const double dp = deltas[i - 1], dn = deltas[i];
    slopes[i] = (dp == 0.0 || dn == 0.0 || dp * dn <= 0.0) ? 0.0 : 2.0 * dp * dn / (dp + dn);
  }
  double right = 0.5 * (3.0 * deltas[size - 2] - deltas[size - 3]);
  if(right * deltas[size - 2] <= 0.0)
    right = 0.0;
  else if(deltas[size - 2] * deltas[size - 3] < 0.0 && fabs(right) > fabs(3.0 * deltas[size - 2]))
    right = 3.0 * deltas[size - 2];
  slopes[size - 1] = right;
}

typedef struct sf_pchip3d_t
{
  int n;
  const double *lut, *sx, *sy, *sz, *cmin, *cmax;
} sf_pchip3d_t;

/* precompute per-axis monotone slopes and per-cell bounds for an n³×3 LUT */
static void pchip3d_prepare(const double *lut, int n, double *sx, double *sy, double *sz,
                            double *cmin, double *cmax)
{
  double line[64], slopes[64];
#define LUT(i, j, k, c) lut[((((size_t)(i)) * n + (j)) * n + (k)) * 3 + (c)]
#define SLOT(arr, i, j, k, c) arr[((((size_t)(i)) * n + (j)) * n + (k)) * 3 + (c)]
  for(int j = 0; j < n; j++)
    for(int k = 0; k < n; k++)
      for(int c = 0; c < 3; c++)
      {
        for(int i = 0; i < n; i++) line[i] = LUT(i, j, k, c);
        fill_monotone_slopes_1d(line, slopes, n);
        for(int i = 0; i < n; i++) SLOT(sx, i, j, k, c) = slopes[i];
      }
  for(int i = 0; i < n; i++)
    for(int k = 0; k < n; k++)
      for(int c = 0; c < 3; c++)
      {
        for(int j = 0; j < n; j++) line[j] = LUT(i, j, k, c);
        fill_monotone_slopes_1d(line, slopes, n);
        for(int j = 0; j < n; j++) SLOT(sy, i, j, k, c) = slopes[j];
      }
  for(int i = 0; i < n; i++)
    for(int j = 0; j < n; j++)
      for(int c = 0; c < 3; c++)
      {
        for(int k = 0; k < n; k++) line[k] = LUT(i, j, k, c);
        fill_monotone_slopes_1d(line, slopes, n);
        for(int k = 0; k < n; k++) SLOT(sz, i, j, k, c) = slopes[k];
      }
  const int m = n - 1;
  for(int i = 0; i < m; i++)
    for(int j = 0; j < m; j++)
      for(int k = 0; k < m; k++)
        for(int c = 0; c < 3; c++)
        {
          double mn = LUT(i, j, k, c), mx = mn;
          for(int di = 0; di < 2; di++)
            for(int dj = 0; dj < 2; dj++)
              for(int dk = 0; dk < 2; dk++)
              {
                const double s = LUT(i + di, j + dj, k + dk, c);
                if(s < mn) mn = s;
                if(s > mx) mx = s;
              }
          const size_t idx = ((((size_t)i) * m + j) * m + k) * 3 + c;
          cmin[idx] = mn;
          cmax[idx] = mx;
        }
#undef LUT
#undef SLOT
}

static inline double hermite_value(double y0, double y1, double m0, double m1, double t)
{
  const double t2 = t * t, t3 = t2 * t;
  return (2.0 * t3 - 3.0 * t2 + 1.0) * y0 + (t3 - 2.0 * t2 + t) * m0
         + (-2.0 * t3 + 3.0 * t2) * y1 + (t3 - t2) * m1;
}

static inline double linear_mix(double v0, double v1, double t) { return v0 + t * (v1 - v0); }

/* r, g, b in [0, n-1] index units */
static void pchip3d_interp(const sf_pchip3d_t *P, double r, double g, double b, double out[3])
{
  const int n = P->n, m = n - 1;
  int i, j, k;
  double tr, tg, tb;
  cubic_base_fraction(r, n, &i, &tr);
  cubic_base_fraction(g, n, &j, &tg);
  cubic_base_fraction(b, n, &k, &tb);
#define AT(arr, ii, jj, kk, c) arr[((((size_t)(ii)) * n + (jj)) * n + (kk)) * 3 + (c)]
  for(int c = 0; c < 3; c++)
  {
    const double v000 = hermite_value(AT(P->lut, i, j, k, c), AT(P->lut, i + 1, j, k, c),
                                      AT(P->sx, i, j, k, c), AT(P->sx, i + 1, j, k, c), tr);
    const double v010
        = hermite_value(AT(P->lut, i, j + 1, k, c), AT(P->lut, i + 1, j + 1, k, c),
                        AT(P->sx, i, j + 1, k, c), AT(P->sx, i + 1, j + 1, k, c), tr);
    const double v001
        = hermite_value(AT(P->lut, i, j, k + 1, c), AT(P->lut, i + 1, j, k + 1, c),
                        AT(P->sx, i, j, k + 1, c), AT(P->sx, i + 1, j, k + 1, c), tr);
    const double v011
        = hermite_value(AT(P->lut, i, j + 1, k + 1, c), AT(P->lut, i + 1, j + 1, k + 1, c),
                        AT(P->sx, i, j + 1, k + 1, c), AT(P->sx, i + 1, j + 1, k + 1, c), tr);
    const double sy00 = linear_mix(AT(P->sy, i, j, k, c), AT(P->sy, i + 1, j, k, c), tr);
    const double sy10 = linear_mix(AT(P->sy, i, j + 1, k, c), AT(P->sy, i + 1, j + 1, k, c), tr);
    const double sy01 = linear_mix(AT(P->sy, i, j, k + 1, c), AT(P->sy, i + 1, j, k + 1, c), tr);
    const double sy11
        = linear_mix(AT(P->sy, i, j + 1, k + 1, c), AT(P->sy, i + 1, j + 1, k + 1, c), tr);
    const double vz0 = hermite_value(v000, v010, sy00, sy10, tg);
    const double vz1 = hermite_value(v001, v011, sy01, sy11, tg);
    const double sz0
        = linear_mix(linear_mix(AT(P->sz, i, j, k, c), AT(P->sz, i + 1, j, k, c), tr),
                     linear_mix(AT(P->sz, i, j + 1, k, c), AT(P->sz, i + 1, j + 1, k, c), tr), tg);
    const double sz1 = linear_mix(
        linear_mix(AT(P->sz, i, j, k + 1, c), AT(P->sz, i + 1, j, k + 1, c), tr),
        linear_mix(AT(P->sz, i, j + 1, k + 1, c), AT(P->sz, i + 1, j + 1, k + 1, c), tr), tg);
    double v = hermite_value(vz0, vz1, sz0, sz1, tb);
    const size_t cidx = ((((size_t)i) * m + j) * m + k) * 3 + c;
    v = CLAMP(v, P->cmin[cidx], P->cmax[cidx]);
    out[c] = v;
  }
#undef AT
}

/* ------------------------------------------------------------------------ */
/* [dc] density curve interpolation helpers                                 */
/* ------------------------------------------------------------------------ */

/* np.interp over an increasing xp of size n, endpoint-clamped */
static double interp_general(double x, const double *xp, const double *fp, int n)
{
  if(x <= xp[0]) return fp[0];
  if(x >= xp[n - 1]) return fp[n - 1];
  int lo = 0, hi = n - 1;
  while(hi - lo > 1)
  {
    const int mid = (lo + hi) >> 1;
    if(xp[mid] <= x)
      lo = mid;
    else
      hi = mid;
  }
  const double dx = xp[hi] - xp[lo];
  if(dx <= 0.0) return fp[hi];
  const double t = (x - xp[lo]) / dx;
  return fp[lo] + t * (fp[hi] - fp[lo]);
}

/* [dc] interpolate one channel of a (SF_NLE, 3) curve table over the uniform
 * log-exposure grid divided by the per-channel gamma factor:
 *   x-axis = le/gamma  ->  index t = (x*gamma - le0) / le_step               */
static inline double interp_curve_uniform(double x, double gammac, double le0,
                                          double le_step, const double (*curves)[3], int c)
{
  const double t = (x * gammac - le0) / le_step;
  if(t <= 0.0) return curves[0][c];
  if(t >= (double)(SF_NLE - 1)) return curves[SF_NLE - 1][c];
  const int i = (int)t;
  const double f = t - i;
  return curves[i][c] + f * (curves[i + 1][c] - curves[i][c]);
}

/* ------------------------------------------------------------------------ */
/* [mc] cdfs density curve model + s023 morph                               */
/* ------------------------------------------------------------------------ */

static inline double norm_cdf(double z) { return 0.5 * (1.0 + erf(z * M_SQRT1_2)); }

/* evaluate one channel of the cdfs model over the log-exposure grid.
 * signed z: negated for positive profiles ([mc] _signed_z) */
static void eval_cdfs_channel(double *out, const double *le, int nle, const double *centers,
                              const double *amps, const double *sigmas, int n_layers,
                              int positive)
{
  for(int i = 0; i < nle; i++) out[i] = 0.0;
  for(int l = 0; l < n_layers; l++)
  {
    for(int i = 0; i < nle; i++)
    {
      double z = (le[i] - centers[l]) / sigmas[l];
      if(positive) z = -z;
      out[i] += amps[l] * norm_cdf(z);
    }
  }
}

#define SF_SIGMA_FLOOR 0.05 /* [mc] NormCdfsFitConfig.sigma_floor */

/* [mc] apply_print_curves_morph without developer exhaustion.
 * With morph inactive this reduces to a plain model evaluation. */
static void build_print_curves(double (*curves)[3], const sf_profile_t *print,
                               const sf_sim_params_t *p)
{
  const int positive = (print->type && strcmp(print->type, "positive") == 0);
  const sf_curves_model_t *m = &print->curves_model;
  const int nl = m->n_layers;

  for(int c = 0; c < 3; c++)
  {
    double centers[8], amps[8], sigmas[8];
    memcpy(centers, m->centers[c], sizeof(centers));
    memcpy(amps, m->amplitudes[c], sizeof(amps));
    memcpy(sigmas, m->sigmas[c], sizeof(sigmas));

    if(p->morph_active && nl > 0)
    {
      /* speed-layer indices by ascending center ([mc] _speed_layer_indices) */
      int order[8];
      for(int i = 0; i < nl; i++) order[i] = i;
      for(int i = 0; i < nl; i++)
        for(int j = i + 1; j < nl; j++)
          if(centers[order[j]] < centers[order[i]])
          {
            const int t = order[i];
            order[i] = order[j];
            order[j] = t;
          }
      const int i_fast = order[0], i_mid = order[nl / 2], i_slow = order[nl - 1];
      const double gch = (c == 0) ? p->morph_gamma_r : (c == 1) ? p->morph_gamma_g
                                                                : p->morph_gamma_b;
      const double g_fast = p->morph_gamma * gch * p->morph_gamma_fast;
      /* [mc] note: the mid sub-layer intentionally uses gamma_factor_slow */
      const double g_mid = p->morph_gamma * gch * p->morph_gamma_slow;
      const double g_slow = g_mid;
      sigmas[i_fast] = fmax(sigmas[i_fast] / g_fast, SF_SIGMA_FLOOR);
      centers[i_fast] = centers[i_fast] / g_fast;
      sigmas[i_mid] = fmax(sigmas[i_mid] / g_mid, SF_SIGMA_FLOOR);
      centers[i_mid] = centers[i_mid] / g_mid;
      sigmas[i_slow] = fmax(sigmas[i_slow] / g_slow, SF_SIGMA_FLOOR);
      centers[i_slow] = centers[i_slow] / g_slow;
    }

    double column[SF_NLE];
    eval_cdfs_channel(column, print->log_exposure, SF_NLE, centers, amps, sigmas, nl, positive);
    for(int i = 0; i < SF_NLE; i++) curves[i][c] = column[i];
  }
}

/* ------------------------------------------------------------------------ */
/* [cf] dichroic enlarger filters                                           */
/* ------------------------------------------------------------------------ */

/* filtered[l] = src[l] * prod_c (1 - (1 - F[l][c]) * (1 - 10^(-cc_c/100))) */
static void apply_dichroic_cc(double *out, const double *src, const double *filters,
                              const double cc[3])
{
  double dim[3];
  for(int c = 0; c < 3; c++) dim[c] = 1.0 - pow(10.0, -cc[c] / 100.0);
  for(int l = 0; l < SF_NWL; l++)
  {
    double total = 1.0;
    for(int c = 0; c < 3; c++) total *= 1.0 - (1.0 - filters[l * 3 + c]) * dim[c];
    out[l] = src[l] * total;
  }
}

/* ------------------------------------------------------------------------ */
/* exact spectral kernels shared by build (LUT fill) and per-pixel paths    */
/* ------------------------------------------------------------------------ */

/* [st] printing._film_cmy_to_print_log_raw — WITHOUT the print_exposure and
 * second log step, which run outside the (optional) 3D table */
static void cmy_to_print_lograw(const sf_sim_t *s, const double cmy[3], double out[3])
{
  double raw[3] = { 0.0, 0.0, 0.0 };
  for(int l = 0; l < SF_NWL; l++)
  {
    double ds = s->film_base_density[l];
    for(int c = 0; c < 3; c++) ds += s->film_chan_density[l][c] * cmy[c];
    /* [st] density_to_light zeroes NaN transmittance (missing spectral data) */
    double light = s->illum_print[l] * pow(10.0, -ds);
    if(!isfinite(light)) light = 0.0;
    for(int m = 0; m < 3; m++) raw[m] += light * s->print_sens[l][m];
  }
  for(int m = 0; m < 3; m++)
  {
    double r = raw[m] * s->midgray_factor + s->preflash_raw[m];
    out[m] = log10(fmax(r, 0.0) + SF_LOG_EPS);
  }
}

/* np.interp equivalent over xp = -curve[i] (ascending for positive film),
   fp = le[i]; endpoint-clamped exactly like numpy */
static double interp_ascending(double x, const double *curve, const double *le, int n)
{
  if(x <= -curve[0]) return le[0];
  if(x >= -curve[n - 1]) return le[n - 1];
  for(int i = 0; i < n - 1; i++)
  {
    const double x0 = -curve[i], x1 = -curve[i + 1];
    if(x >= x0 && x <= x1)
    {
      const double t = (x1 > x0) ? (x - x0) / (x1 - x0) : 0.0;
      return le[i] + t * (le[i + 1] - le[i]);
    }
  }
  return le[n - 1];
}

/* [st] scanning cmy_to_log_xyz */
static void cmy_to_log_xyz(const sf_sim_t *s, const double cmy[3], double out[3])
{
  double xyz[3] = { 0.0, 0.0, 0.0 };
  for(int l = 0; l < SF_NWL; l++)
  {
    double ds = s->scan_base_density[l];
    for(int c = 0; c < 3; c++) ds += s->scan_chan_density[l][c] * cmy[c];
    double light = s->illum_view[l] * pow(10.0, -ds);
    if(!isfinite(light)) light = 0.0;
    for(int m = 0; m < 3; m++) xyz[m] += light * s->cmfs[l][m];
  }
  for(int m = 0; m < 3; m++)
    out[m] = log10(fmax(xyz[m] / s->xyz_norm, 0.0) + SF_LOG_EPS);
}

/* ------------------------------------------------------------------------ */
/* [gc] OkLab conversions and output C_max(L, h) table                      */
/* ------------------------------------------------------------------------ */

static inline void xyz_to_oklab(const double xyz[3], double lab[3])
{
  double lms[3];
  mat3_mulv(lms, SF_OKLAB_M1, xyz);
  for(int i = 0; i < 3; i++) lms[i] = cbrt(lms[i]);
  mat3_mulv(lab, SF_OKLAB_M2, lms);
}

static inline void oklab_to_xyz(const sf_sim_t *s, const double lab[3], double xyz[3])
{
  double lms[3];
  mat3_mulv(lms, s->oklab_m2inv, lab);
  for(int i = 0; i < 3; i++) lms[i] = lms[i] * lms[i] * lms[i];
  mat3_mulv(xyz, s->oklab_m1inv, lms);
}

#define SF_CMAX_L_LO 0.02 /* [gc] _get_output_c_max_table oklch L_grid */
#define SF_CMAX_L_HI 1.0

/* bisect the max in-cube OkLch chroma per (L, h) ([gc] _build_polar_..._table) */
static void build_cmax_table(sf_sim_t *s)
{
  s->cmax = malloc(sizeof(float) * SF_CMAX_NL * SF_CMAX_NH);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for(int i = 0; i < SF_CMAX_NL; i++)
  {
    const double L = SF_CMAX_L_LO
                     + (SF_CMAX_L_HI - SF_CMAX_L_LO) * i / (double)(SF_CMAX_NL - 1);
    for(int j = 0; j < SF_CMAX_NH; j++)
    {
      const double h = -M_PI + 2.0 * M_PI * j / (double)SF_CMAX_NH;
      const double ch = cos(h), sh = sin(h);
      double lo = 0.0, hi = 0.5;
      for(int b = 0; b < SF_CMAX_NBISECT; b++)
      {
        const double mid = 0.5 * (lo + hi);
        const double lab[3] = { L, mid * ch, mid * sh };
        double xyz[3], rgb[3];
        oklab_to_xyz(s, lab, xyz);
        mat3_mulv(rgb, s->out_xyz2rgb, xyz);
        const int in_gamut = rgb[0] >= -1e-6 && rgb[0] <= 1.0 + 1e-6 && rgb[1] >= -1e-6
                             && rgb[1] <= 1.0 + 1e-6 && rgb[2] >= -1e-6 && rgb[2] <= 1.0 + 1e-6;
        if(in_gamut)
          lo = mid;
        else
          hi = mid;
      }
      s->cmax[(size_t)i * SF_CMAX_NH + j] = (float)lo;
    }
  }
}

/* [gc] _c_max_lookup — bilinear, L clamped, hue wrapped */
static inline double cmax_lookup(const sf_sim_t *s, double L, double h)
{
  L = CLAMP(L, SF_CMAX_L_LO, SF_CMAX_L_HI);
  const double h_step = 2.0 * M_PI / SF_CMAX_NH;
  const double h_idx = (h + M_PI) / h_step;
  const double h_floor = floor(h_idx);
  int h_lo = ((int)h_floor) % SF_CMAX_NH;
  if(h_lo < 0) h_lo += SF_CMAX_NH;
  const int h_hi = (h_lo + 1) % SF_CMAX_NH;
  const double h_frac = h_idx - h_floor;

  const double L_idx
      = (L - SF_CMAX_L_LO) / (SF_CMAX_L_HI - SF_CMAX_L_LO) * (double)(SF_CMAX_NL - 1);
  int L_lo = (int)floor(L_idx);
  L_lo = CLAMP(L_lo, 0, SF_CMAX_NL - 2);
  const int L_hi = L_lo + 1;
  const double L_frac = L_idx - L_lo;

  const float *T = s->cmax;
  const double v00 = T[(size_t)L_lo * SF_CMAX_NH + h_lo];
  const double v01 = T[(size_t)L_lo * SF_CMAX_NH + h_hi];
  const double v10 = T[(size_t)L_hi * SF_CMAX_NH + h_lo];
  const double v11 = T[(size_t)L_hi * SF_CMAX_NH + h_hi];
  return v00 * (1 - L_frac) * (1 - h_frac) + v01 * (1 - L_frac) * h_frac
         + v10 * L_frac * (1 - h_frac) + v11 * L_frac * h_frac;
}

/* [gc] compress_rgb_oklch_chroma with lightness_compression (0.7, 1, 2.2) */
static void compress_rgb_oklch(const sf_sim_t *s, double rgb[3])
{
  double xyz[3], lab[3];
  mat3_mulv(xyz, s->out_rgb2xyz, rgb);
  xyz_to_oklab(xyz, lab);
  double L = lab[0];
  const double a = lab[1], b = lab[2];
  /* lightness first, so C_max is looked up at the corrected L */
  L = reinhard_knee(L, SF_OUT_LIGHT_T, SF_OUT_LIGHT_L, SF_OUT_LIGHT_P);
  const double C = hypot(a, b);
  const double h = atan2(b, a);
  const double C_max = fmax(cmax_lookup(s, L, h), 1e-9);
  const double d = reinhard_knee(C / C_max, SF_OUT_KNEE_T, SF_OUT_KNEE_L, SF_OUT_KNEE_P);
  const double C_new = d * C_max;
  const double lab_new[3] = { L, C_new * cos(h), C_new * sin(h) };
  oklab_to_xyz(s, lab_new, xyz);
  mat3_mulv(rgb, s->out_xyz2rgb, xyz);
}

/* [gc] compress_rgb_aces_rgc — per-channel knee on achromatic distance */
static void compress_rgb_aces(double rgb[3])
{
  const double ach = fmax(rgb[0], fmax(rgb[1], rgb[2]));
  if(ach <= 1e-12) return;
  for(int c = 0; c < 3; c++)
  {
    const double d = (ach - rgb[c]) / ach;
    const double dc = reinhard_knee(d, SF_OUT_KNEE_T, SF_OUT_KNEE_L, SF_OUT_KNEE_P);
    rgb[c] = ach * (1.0 - dc);
  }
}

/* ------------------------------------------------------------------------ */
/* build                                                                    */
/* ------------------------------------------------------------------------ */

static void illuminant_xy_from_spd(double out[2], const double *spd,
                                   const double cmfs[][3])
{
  double xyz[3] = { 0.0, 0.0, 0.0 };
  for(int l = 0; l < SF_NWL; l++)
    for(int c = 0; c < 3; c++) xyz[c] += spd[l] * cmfs[l][c];
  const double sum = xyz[0] + xyz[1] + xyz[2];
  out[0] = xyz[0] / sum;
  out[1] = xyz[1] / sum;
}

/* [su] one 2D LUT lookup of the filming stage: linear RGB -> raw exposure */
static void expose_pixel(const double m_in[9], const double *tc_lut, int tc_n,
                         const double rgb[3], double raw[3])
{
  double xyz[3];
  mat3_mulv(xyz, m_in, rgb);
  const double b = xyz[0] + xyz[1] + xyz[2];
  const double xy[2] = { xyz[0] / fmax(b, 1e-10), xyz[1] / fmax(b, 1e-10) };
  double tc[2];
  tri2quad(tc, xy);
  const double scale = (double)(tc_n - 1);
  cubic_interp_2d(raw, tc_lut, tc_n, tc[0] * scale, tc[1] * scale);
  const double bb = isfinite(b) ? b : 0.0;
  for(int c = 0; c < 3; c++) raw[c] *= bb;
}

/* [st] filming._simple_rgb_to_density_spectral: the gray reference used to
 * balance the print exposure. NOTE the reference computes this in *sRGB*
 * (the _rgb_to_film_raw defaults), independent of the io input space. */
static void midgray_density_spectral(const sf_sim_t *s, const sf_profile_t *film,
                                     const double film_ref_xy[2], double gray,
                                     double ds[SF_NWL])
{
  double m_srgb[9], cat[9];
  cat_matrix(cat, SF_M_CAT16, SF_SRGB_WHITE_XY, film_ref_xy);
  mat3_mul(m_srgb, cat, SF_M_SRGB_TO_XYZ);

  const double rgb[3] = { gray, gray, gray };
  double raw[3];
  expose_pixel(m_srgb, s->tc_lut, s->tc_n, rgb, raw);

  double cmy[3];
  for(int c = 0; c < 3; c++)
  {
    const double lograw = log10(raw[c] + SF_LOG_EPS);
    /* develop_simple: UNNORMALIZED stock curves */
    cmy[c] = interp_curve_uniform(lograw, s->gamma[c], s->le0, s->le_step,
                                  film->density_curves, c);
  }
  for(int l = 0; l < SF_NWL; l++)
  {
    ds[l] = film->base_density[l];
    for(int c = 0; c < 3; c++) ds[l] += film->channel_density[l][c] * cmy[c];
  }
}

/* [st] printing._exposure_factor: 1 / geomean of the midgray print raw */
static double exposure_factor(const sf_sim_t *s, const double ds[SF_NWL])
{
  double raw[3] = { 0.0, 0.0, 0.0 };
  for(int l = 0; l < SF_NWL; l++)
  {
    double light = s->illum_print[l] * pow(10.0, -ds[l]);
    if(!isfinite(light)) light = 0.0;
    for(int m = 0; m < 3; m++) raw[m] += light * s->print_sens[l][m];
  }
  double log_sum = 0.0;
  for(int m = 0; m < 3; m++) log_sum += log(fmax(raw[m], 1e-10));
  return 1.0 / exp(log_sum / 3.0);
}

/* fill a steps^3 table by sampling fn over [lo, hi]^3 and prepare PCHIP */
typedef void (*sf_cell_fn)(const sf_sim_t *, const double[3], double[3]);

static void build_lut3d(const sf_sim_t *s, sf_cell_fn fn, const double lo[3],
                        const double hi[3], int steps, double **lut, double **sx,
                        double **sy, double **sz, double **cmin, double **cmax_)
{
  const size_t n3 = (size_t)steps * steps * steps * 3;
  const size_t m3 = (size_t)(steps - 1) * (steps - 1) * (steps - 1) * 3;
  *lut = malloc(n3 * sizeof(double));
  *sx = malloc(n3 * sizeof(double));
  *sy = malloc(n3 * sizeof(double));
  *sz = malloc(n3 * sizeof(double));
  *cmin = malloc(m3 * sizeof(double));
  *cmax_ = malloc(m3 * sizeof(double));
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for(int i = 0; i < steps; i++)
    for(int j = 0; j < steps; j++)
      for(int k = 0; k < steps; k++)
      {
        const double cmy[3] = { lo[0] + (hi[0] - lo[0]) * i / (double)(steps - 1),
                                lo[1] + (hi[1] - lo[1]) * j / (double)(steps - 1),
                                lo[2] + (hi[2] - lo[2]) * k / (double)(steps - 1) };
        fn(s, cmy, *lut + ((((size_t)i) * steps + j) * steps + k) * 3);
      }
  pchip3d_prepare(*lut, steps, *sx, *sy, *sz, *cmin, *cmax_);
}

void sf_sim_free(sf_sim_t *s)
{
  if(!s) return;
  free(s->tc_lut);
  free(s->enl_lut); free(s->enl_sx); free(s->enl_sy); free(s->enl_sz);
  free(s->enl_cmin); free(s->enl_cmax);
  free(s->scan_lut); free(s->scan_sx); free(s->scan_sy); free(s->scan_sz);
  free(s->scan_cmin); free(s->scan_cmax);
  free(s->cmax);
  g_free(s);
}

double sf_sim_film_dmax(const sf_sim_t *sim, int ch)
{
  return sim->film_dmax[CLAMP(ch, 0, 2)];
}

sf_sim_t *sf_sim_build(const sf_pack_t *pack, const sf_profile_t *film,
                       const sf_profile_t *print, const sf_sim_params_t *params,
                       char **errmsg)
{
  if(!pack || !film || !params || (!print && !params->scan_film))
  {
    set_error(errmsg, "spektra_sim: build needs pack, film and (unless scan_film) print");
    return NULL;
  }
  sf_sim_t *s = g_new0(sf_sim_t, 1);
  s->p = *params;
  sf_sim_params_t *p = &s->p;
  s->film_positive = (film->type && strcmp(film->type, "positive") == 0);
  s->film_bw = (film->channel_model && strcmp(film->channel_model, "bw") == 0);
  s->print_positive = (print && print->type && strcmp(print->type, "positive") == 0);
  s->has_print = !p->scan_film;
  s->out_compress = p->output_compress;
  s->print_exposure = p->print_exposure;
  s->lut_steps = p->lut_steps;
  if(s->lut_steps == 1) s->lut_steps = 0;
  if(s->lut_steps > 64) s->lut_steps = 64; /* pchip line buffers are 64 wide */
  memcpy(s->cmfs, pack->cmfs, sizeof(s->cmfs));

  /* per-film digested coupler gammas from the pack — applied only when the
   * caller left the generic defaults untouched */
  {
    sf_sim_params_t generic;
    sf_sim_params_defaults(&generic);
    if(memcmp(p->gamma_samelayer, generic.gamma_samelayer, sizeof(p->gamma_samelayer)) == 0
       && memcmp(p->gamma_inter_r_gb, generic.gamma_inter_r_gb, sizeof(p->gamma_inter_r_gb)) == 0
       && memcmp(p->gamma_inter_g_rb, generic.gamma_inter_g_rb, sizeof(p->gamma_inter_g_rb)) == 0
       && memcmp(p->gamma_inter_b_rg, generic.gamma_inter_b_rg, sizeof(p->gamma_inter_b_rg)) == 0)
      sf_pack_film_defaults(pack, film->stock, p->gamma_samelayer, p->gamma_inter_r_gb,
                            p->gamma_inter_g_rb, p->gamma_inter_b_rg, NULL, NULL, NULL,
                            NULL, NULL);
  }
  /* neutral enlarger filters from the release database */
  if(s->has_print && p->neutral_from_db)
  {
    double cmy[3];
    if(sf_pack_neutral_filters(pack, print->stock, p->enlarger_illuminant, film->stock, cmy))
    {
      p->c_filter_neutral = cmy[0];
      p->m_filter_neutral = cmy[1];
      p->y_filter_neutral = cmy[2];
    }
  }

  /* ----- filming: input matrix and tc_lut ------------------------------- */
  const double *illu_ref = g_hash_table_lookup(pack->illuminants, film->reference_illuminant);
  if(!illu_ref)
  {
    set_error(errmsg, "spektra_sim: pack misses reference illuminant '%s'",
              film->reference_illuminant);
    sf_sim_free(s);
    return NULL;
  }
  double film_ref_xy[2];
  illuminant_xy_from_spd(film_ref_xy, illu_ref, pack->cmfs);
  {
    double cat[9];
    cat_matrix(cat, SF_M_CAT16, p->input_white_xy, film_ref_xy);
    mat3_mul(s->m_in, cat, p->input_rgb_to_xyz);
  }
  s->ev_scale = pow(2.0, p->exposure_comp_ev);

  /* [su] compute_hanatos2025_tc_lut: spectra × (sensitivity × window / norm) */
  const int n = pack->tc_n;
  s->tc_n = n;
  s->tc_lut = malloc((size_t)n * n * 3 * sizeof(double));
  {
    double sens_w[SF_NWL][3];
    for(int l = 0; l < SF_NWL; l++)
      for(int m = 0; m < 3; m++)
      {
        const double v = pow(10.0, film->log_sensitivity[l][m]);
        sens_w[l][m] = isfinite(v) ? v : 0.0;
      }
    if(film->window_n == 4) /* erf4 spectral bandpass, white-balance preserving */
    {
      const double c_uv = film->window_params[0], s_uv = film->window_params[1];
      const double c_ir = film->window_params[2], s_ir = film->window_params[3];
      double w[SF_NWL];
      for(int l = 0; l < SF_NWL; l++)
      {
        const double wl = pack->wavelengths[l];
        const double e_uv = 0.5 * (1.0 + erf((wl - c_uv) / (s_uv * M_SQRT2)));
        const double e_ir = 0.5 * (1.0 - erf((wl - c_ir) / (s_ir * M_SQRT2)));
        w[l] = e_uv * e_ir;
      }
      for(int m = 0; m < 3; m++)
      {
        double num = 0.0, den = 0.0;
        for(int l = 0; l < SF_NWL; l++)
        {
          num += sens_w[l][m] * illu_ref[l] * w[l];
          den += sens_w[l][m] * illu_ref[l];
        }
        const double norm = num / den;
        for(int l = 0; l < SF_NWL; l++) sens_w[l][m] *= w[l] / norm;
      }
    }
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for(int i = 0; i < n; i++)
      for(int j = 0; j < n; j++)
      {
        const float *spec = pack->spectra + ((size_t)i * n + j) * SF_NWL;
        double acc[3] = { 0.0, 0.0, 0.0 };
        for(int l = 0; l < SF_NWL; l++)
        {
          const double sp = spec[l];
          for(int m = 0; m < 3; m++) acc[m] += sp * sens_w[l][m];
        }
        double *dst = s->tc_lut + ((size_t)i * n + j) * 3;
        dst[0] = acc[0];
        dst[1] = acc[1];
        dst[2] = acc[2];
      }
    /* [gc] remap_tc_lut_for_compression: new_lut[tc] = old_lut[compress(tc)] */
    if(p->input_gamut_compress)
    {
      double *old = malloc((size_t)n * n * 3 * sizeof(double));
      memcpy(old, s->tc_lut, (size_t)n * n * 3 * sizeof(double));
      const double scale = (double)(n - 1);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
      for(int i = 0; i < n; i++)
        for(int j = 0; j < n; j++)
        {
          const double tc[2] = { i / scale, j / scale };
          double xy[2], cxy[2], ctc[2];
          quad2tri(xy, tc);
          compress_xy_radial(cxy, xy, film_ref_xy, pack->locus, pack->locus_n);
          tri2quad(ctc, cxy);
          bilinear_2d_clamped(s->tc_lut + ((size_t)i * n + j) * 3, old, n,
                              ctc[0] * scale, ctc[1] * scale);
        }
      free(old);
    }
  }

  /* ----- film develop ---------------------------------------------------- */
  s->le0 = film->log_exposure[0];
  s->le_step = (film->log_exposure[SF_NLE - 1] - film->log_exposure[0]) / (SF_NLE - 1);
  for(int c = 0; c < 3; c++) s->gamma[c] = p->density_curve_gamma;
  for(int c = 0; c < 3; c++)
  {
    double mn = INFINITY, mx = -INFINITY;
    for(int i = 0; i < SF_NLE; i++)
    {
      const double v = film->density_curves[i][c];
      if(v < mn) mn = v;
      if(v > mx) mx = v;
    }
    for(int i = 0; i < SF_NLE; i++) s->curves_norm[i][c] = film->density_curves[i][c] - mn;
    s->film_dmax[c] = mx - mn;
    s->film_dmin[c] = mn;
  }
  /* [cp] per-film grain catalogue data (film_render_defaults[stock].grain);
     falls back to spektrafilm's original single fixed profile when the pack
     predates per-film grain or the stock has no entry. density_min shares
     p->grain_density_min with the enlarger/scan table-range code below, so
     it is overwritten in place rather than kept as a separate sim field. */
  {
    /* matches SF_GRAIN_LEGACY_RMS / SF_GRAIN_LEGACY_UNIFORMITY in
       spektra_core.h — spektrafilm's original single fixed grain profile */
    const double legacy_rms[3] = { 6.0, 8.0, 10.0 };
    const double legacy_unif[3] = { 0.97, 0.97, 0.97 };
    for(int c = 0; c < 3; c++)
    {
      s->grain_rms[c] = legacy_rms[c];
      s->grain_uniformity[c] = legacy_unif[c];
    }
    sf_pack_film_grain(pack, film->stock, s->grain_rms, s->grain_uniformity,
                       p->grain_density_min);
  }
  /* [cp] coupler matrix: donor row -> receiver column, scaled by amount */
  s->couplers_active = p->couplers_active;
  {
    double M[3][3] = { { 0 } };
    M[0][0] = p->gamma_samelayer[0] * p->inhibition_samelayer;
    M[1][1] = p->gamma_samelayer[1] * p->inhibition_samelayer;
    M[2][2] = p->gamma_samelayer[2] * p->inhibition_samelayer;
    M[0][1] = p->gamma_inter_r_gb[0] * p->inhibition_interlayer;
    M[0][2] = p->gamma_inter_r_gb[1] * p->inhibition_interlayer;
    M[1][0] = p->gamma_inter_g_rb[0] * p->inhibition_interlayer;
    M[1][2] = p->gamma_inter_g_rb[1] * p->inhibition_interlayer;
    M[2][0] = p->gamma_inter_b_rg[0] * p->inhibition_interlayer;
    M[2][1] = p->gamma_inter_b_rg[1] * p->inhibition_interlayer;
    for(int i = 0; i < 3; i++)
      for(int j = 0; j < 3; j++) s->couplers_M[i][j] = M[i][j] * p->couplers_amount;

    /* [cp] Langmuir parameters (dev/0.4+ packs; absent -> linear 0.3.x).
       Negative: donor-side saturation, K = k*d_max, D_ref = d_max/2.
       Positive/reversal: linear donor, receiver-side saturation with
       c_ref[m] = sum_k D_ref[k]*M_unit[k][m] from the amount-INdependent
       matrix, Kr = k_recv * 2*c_ref. */
    for(int c = 0; c < 3; c++)
    {
      s->couplers_donor_K[c] = INFINITY;
      s->couplers_recv_Kr[c] = INFINITY;
      s->couplers_donor_Dref[c] = 0.5 * s->film_dmax[c];
      s->couplers_recv_cref[c] = 0.0;
    }
    s->couplers_donor_lm = 0;
    s->couplers_recv_lm = 0;
    s->coupler_diff_um = SF_COUPLER_BLUR_UM;
    s->coupler_tail_um = 0.0;
    s->coupler_tail_w = 0.0;
    sf_pack_film_coupler_diffusion(pack, film->stock, &s->coupler_diff_um,
                                   &s->coupler_tail_um, &s->coupler_tail_w);
    if(s->coupler_tail_w <= 0.0 || s->coupler_tail_um <= 0.0)
    {
      s->coupler_tail_um = 0.0;
      s->coupler_tail_w = 0.0;
    }
    double lm_donor[3], lm_recv[3];
    if(sf_pack_film_langmuir(pack, film->stock, lm_donor, lm_recv))
    {
      if(s->film_positive)
      {
        s->couplers_recv_lm = 1;
        for(int m = 0; m < 3; m++)
        {
          double cref = 0.0;
          for(int k = 0; k < 3; k++) cref += s->couplers_donor_Dref[k] * M[k][m];
          s->couplers_recv_cref[m] = cref;
          s->couplers_recv_Kr[m] = lm_recv[m] * 2.0 * cref;
        }
      }
      else
      {
        s->couplers_donor_lm = 1;
        for(int c = 0; c < 3; c++)
          s->couplers_donor_K[c] = lm_donor[c] * s->film_dmax[c];
      }
    }
  }
  /* [cp] compute_density_curves_before_dir_couplers */
  if(s->couplers_active)
  {
    double le_0[SF_NLE][3];
    for(int i = 0; i < SF_NLE; i++)
      for(int m = 0; m < 3; m++)
      {
        double cac = 0.0;
        for(int k = 0; k < 3; k++)
        {
          double silver = s->film_positive ? s->film_dmax[k] - s->curves_norm[i][k]
                                           : s->curves_norm[i][k];
          if(s->couplers_donor_lm)
            silver = silver * (s->couplers_donor_K[k] + s->couplers_donor_Dref[k])
                     / (s->couplers_donor_K[k] + silver);
          cac += silver * s->couplers_M[k][m];
        }
        if(s->couplers_recv_lm)
          cac = cac * (s->couplers_recv_Kr[m] + s->couplers_recv_cref[m])
                / (s->couplers_recv_Kr[m] + cac);
        le_0[i][m] = film->log_exposure[i] - cac;
      }
    for(int c = 0; c < 3; c++)
    {
      double xp[SF_NLE], fp[SF_NLE];
      for(int i = 0; i < SF_NLE; i++)
      {
        xp[i] = le_0[i][c];
        fp[i] = s->film_positive ? -s->curves_norm[i][c] : s->curves_norm[i][c];
      }
      for(int i = 0; i < SF_NLE; i++)
      {
        const double v = interp_general(film->log_exposure[i], xp, fp, SF_NLE);
        s->curves_before[i][c] = s->film_positive ? -v : v;
      }
    }
  }
  else
    memcpy(s->curves_before, s->curves_norm, sizeof(s->curves_before));

  /* ----- printing -------------------------------------------------------- */
  if(s->has_print)
  {
    const double *illu_src = g_hash_table_lookup(pack->illuminants, p->enlarger_illuminant);
    const double *filters = g_hash_table_lookup(pack->dichroics, p->dichroic_brand);
    if(!illu_src || !filters)
    {
      set_error(errmsg, "spektra_sim: pack misses enlarger illuminant '%s' or dichroic '%s'",
                p->enlarger_illuminant, p->dichroic_brand);
      sf_sim_free(s);
      return NULL;
    }
    const double cc_print[3] = { p->c_filter_neutral, p->m_filter_neutral + p->m_filter_shift,
                                 p->y_filter_neutral + p->y_filter_shift };
    const double cc_pre[3] = { p->c_filter_neutral, p->m_filter_neutral + p->preflash_m_shift,
                               p->y_filter_neutral + p->preflash_y_shift };
    apply_dichroic_cc(s->illum_print, illu_src, filters, cc_print);
    apply_dichroic_cc(s->illum_preflash, illu_src, filters, cc_pre);
    for(int l = 0; l < SF_NWL; l++)
      for(int m = 0; m < 3; m++)
      {
        const double v = pow(10.0, print->log_sensitivity[l][m]);
        s->print_sens[l][m] = isfinite(v) ? v : 0.0;
      }
    memcpy(s->film_chan_density, film->channel_density, sizeof(s->film_chan_density));
    memcpy(s->film_base_density, film->base_density, sizeof(s->film_base_density));

    /* [st] midgray print balance (geometric-mean normalization) */
    s->midgray_factor = 1.0;
    {
      double ds_mid[SF_NWL], ds_comp[SF_NWL];
      midgray_density_spectral(s, film, film_ref_xy, SF_MIDGRAY, ds_mid);
      const double f_mid = exposure_factor(s, ds_mid);
      double f_comp = 1.0;
      if(p->print_exposure_compensation)
      {
        midgray_density_spectral(s, film, film_ref_xy, SF_MIDGRAY * s->ev_scale, ds_comp);
        f_comp = exposure_factor(s, ds_comp);
      }
      if(p->print_exposure_compensation && !p->normalize_print_exposure)
        s->midgray_factor = f_comp / f_mid;
      else if(p->normalize_print_exposure && p->print_exposure_compensation)
        s->midgray_factor = f_comp;
      else if(p->normalize_print_exposure && !p->print_exposure_compensation)
        s->midgray_factor = f_mid;
      else
        s->midgray_factor = 1.0;
    }
    /* [st] preflash through the base density only */
    s->preflash_raw[0] = s->preflash_raw[1] = s->preflash_raw[2] = 0.0;
    if(p->preflash_exposure > 0.0)
      for(int l = 0; l < SF_NWL; l++)
      {
        double light = s->illum_preflash[l] * pow(10.0, -film->base_density[l]);
        if(!isfinite(light)) light = 0.0;
        for(int m = 0; m < 3; m++)
          s->preflash_raw[m] += light * s->print_sens[l][m] * p->preflash_exposure;
      }

    /* enlarger table range: [-grain density_min, nanmax(unnormalized curves)] */
    for(int c = 0; c < 3; c++)
    {
      double mx = -INFINITY;
      for(int i = 0; i < SF_NLE; i++)
        if(film->density_curves[i][c] > mx) mx = film->density_curves[i][c];
      s->enl_lo[c] = -p->grain_density_min[c];
      s->enl_hi[c] = mx;
    }
    build_print_curves(s->print_curves, print, p);
  }

  /* ----- scanning -------------------------------------------------------- */
  {
    const sf_profile_t *sp = s->has_print ? print : film;
    memcpy(s->scan_chan_density, sp->channel_density, sizeof(s->scan_chan_density));
    memcpy(s->scan_base_density, sp->base_density, sizeof(s->scan_base_density));
    const double *illu_view = g_hash_table_lookup(pack->illuminants, sp->viewing_illuminant);
    if(!illu_view)
    {
      set_error(errmsg, "spektra_sim: pack misses viewing illuminant '%s'",
                sp->viewing_illuminant);
      sf_sim_free(s);
      return NULL;
    }
    memcpy(s->illum_view, illu_view, sizeof(s->illum_view));
    s->xyz_norm = 0.0;
    for(int l = 0; l < SF_NWL; l++) s->xyz_norm += illu_view[l] * pack->cmfs[l][1];
    for(int c = 0; c < 3; c++)
    {
      s->illum_view_xyz[c] = 0.0;
      for(int l = 0; l < SF_NWL; l++) s->illum_view_xyz[c] += illu_view[l] * pack->cmfs[l][c];
      s->illum_view_xyz[c] /= s->xyz_norm;
    }
    /* scan table range */
    if(s->has_print)
      for(int c = 0; c < 3; c++)
      {
        double mn = INFINITY, mx = -INFINITY;
        for(int i = 0; i < SF_NLE; i++)
        {
          const double v = print->density_curves[i][c];
          if(v < mn) mn = v;
          if(v > mx) mx = v;
        }
        s->scan_lo[c] = mn;
        s->scan_hi[c] = mx;
      }
    else
      for(int c = 0; c < 3; c++)
      {
        s->scan_lo[c] = -p->grain_density_min[c];
        s->scan_hi[c] = s->film_dmax[c]; /* == nanmax(curves) - min; see below */
      }
    /* reference uses nanmax of the raw film curves for scan_film */
    if(!s->has_print)
      for(int c = 0; c < 3; c++)
      {
        double mx = -INFINITY;
        for(int i = 0; i < SF_NLE; i++)
          if(film->density_curves[i][c] > mx) mx = film->density_curves[i][c];
        s->scan_hi[c] = mx;
      }
    /* output matrix: CAT02 from the viewing illuminant to the output white */
    double view_xy[2] = { s->illum_view_xyz[0]
                              / (s->illum_view_xyz[0] + s->illum_view_xyz[1]
                                 + s->illum_view_xyz[2]),
                          s->illum_view_xyz[1]
                              / (s->illum_view_xyz[0] + s->illum_view_xyz[1]
                                 + s->illum_view_xyz[2]) };
    double cat[9];
    cat_matrix(cat, SF_M_CAT02, view_xy, p->output_white_xy);
    mat3_mul(s->m_out, p->output_xyz_to_rgb, cat);
  }

  /* ----- scanner black/white point for positive film scans ---------------- */
  /* A slide has base density and never reaches the paper's D-max; a real
     scanner sets black/white points. Reference: color_reference.py with
     scanner.black_correction = white_correction = true, which upstream's UI
     uses for slides -- off (upstream default) the scan is washed out. Only
     affects scan-film mode with positive film; negatives are untouched. */
  s->scan_bw_on = 0;
  s->scan_bw_m = 1.0;
  s->scan_bw_q = 0.0;
  if(!s->has_print && s->film_positive)
  {
    /* upstream treats the 0.98 / 0.01 scanner levels as sRGB-encoded and
       linearizes them (color_reference._remove_sRGB_cctf) */
    const double white_level = pow((0.98 + 0.055) / 1.055, 2.4);
    const double black_level = 0.01 / 12.92;
    double cmy_black[3], cmy_white[3] = { 0.0, 0.0, 0.0 };
    for(int c = 0; c < 3; c++)
    {
      double mx = -INFINITY;
      for(int i = 0; i < SF_NLE; i++)
      {
        const double v = film->density_curves[i][c];
        if(isfinite(v) && v > mx) mx = v;
      }
      cmy_black[c] = mx;
    }
    double lxb[3], lxw[3];
    cmy_to_log_xyz(s, cmy_black, lxb);
    cmy_to_log_xyz(s, cmy_white, lxw);
    const double y_black = pow(10.0, lxb[1]), y_white = pow(10.0, lxw[1]);
    const double m = (white_level - black_level) / (y_white - y_black + 1e-10);
    const double q = black_level - m * y_black;
    s->scan_bw_on = 1;
    s->scan_bw_m = m;
    s->scan_bw_q = q;

    /* film exposure correction so midgray still lands on midgray after the
       correction (reference: black_white_filming_exposure_correction) */
    const double midgray_corrected = (0.184 - q) / m;
    if(midgray_corrected > 0.0)
    {
      const double density_midgray = -log10(0.184);
      const double density_midgray_corrected = -log10(midgray_corrected);
      double dmin_av = 0.0;
      int nvalid = 0;
      for(int i = 0; i < SF_NWL; i++)
        if(isfinite(film->base_density[i]))
        {
          dmin_av += film->base_density[i];
          nvalid++;
        }
      dmin_av = nvalid ? dmin_av / nvalid : 0.0;
      double curve_av[SF_NLE];
      for(int i = 0; i < SF_NLE; i++)
      {
        double sum = 0.0;
        int nc = 0;
        for(int c = 0; c < 3; c++)
          if(isfinite(film->density_curves[i][c]))
          {
            sum += film->density_curves[i][c];
            nc++;
          }
        curve_av[i] = nc ? sum / nc : 0.0;
      }
      /* np.interp(x, -curve_av, log_exposure): -curve_av ascends for positive
         film (density falls with exposure); endpoint clamp like np.interp */
      const double le_mid_c = -interp_ascending(-(density_midgray_corrected - dmin_av),
                                                curve_av, film->log_exposure, SF_NLE);
      const double le_mid = -interp_ascending(-(density_midgray - dmin_av), curve_av,
                                              film->log_exposure, SF_NLE);
      const double exposure_correction = pow(10.0, le_mid_c - le_mid);
      s->ev_scale /= exposure_correction; /* raw *= 1/correction */
    }
  }

  /* ----- runtime 3D tables ------------------------------------------------ */
  if(s->lut_steps >= 2)
  {
    if(s->has_print)
      build_lut3d(s, cmy_to_print_lograw, s->enl_lo, s->enl_hi, s->lut_steps, &s->enl_lut,
                  &s->enl_sx, &s->enl_sy, &s->enl_sz, &s->enl_cmin, &s->enl_cmax);
    build_lut3d(s, cmy_to_log_xyz, s->scan_lo, s->scan_hi, s->lut_steps, &s->scan_lut,
                &s->scan_sx, &s->scan_sy, &s->scan_sz, &s->scan_cmin, &s->scan_cmax);
  }

  /* ----- output gamut compression ----------------------------------------- */
  memcpy(s->out_rgb2xyz, p->output_rgb_to_xyz, sizeof(s->out_rgb2xyz));
  memcpy(s->out_xyz2rgb, p->output_xyz_to_rgb, sizeof(s->out_xyz2rgb));
  mat3_inv(s->oklab_m1inv, SF_OKLAB_M1);
  mat3_inv(s->oklab_m2inv, SF_OKLAB_M2);
  if(s->out_compress == SF_OUTPUT_COMPRESS_OKLCH) build_cmax_table(s);

  return s;
}

/* ------------------------------------------------------------------------ */
/* per-pixel stages                                                         */
/* ------------------------------------------------------------------------ */

void sf_sim_expose(const sf_sim_t *sim, const float *rgb_in, float *raw, size_t npix,
                   int nch_in, int nch_out)
{
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for(size_t px = 0; px < npix; px++)
  {
    const float *in = rgb_in + px * nch_in;
    float *out = raw + px * nch_out;
    const double rgb[3] = { in[0], in[1], in[2] };
    double r[3];
    expose_pixel(sim->m_in, sim->tc_lut, sim->tc_n, rgb, r);
    for(int c = 0; c < 3; c++) out[c] = (float)(r[c] * sim->ev_scale);
  }
}

void sf_sim_lograw(float *raw, size_t npix, int nch)
{
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for(size_t px = 0; px < npix; px++)
  {
    float *v = raw + px * nch;
    for(int c = 0; c < 3; c++)
      v[c] = (float)log10(fmax((double)v[c], 0.0) + SF_LOG_EPS);
  }
}

void sf_sim_develop_corr(const sf_sim_t *sim, const float *lograw, float *corr,
                         size_t npix, int nch_in)
{
  if(!sim->couplers_active)
  {
    memset(corr, 0, npix * 3 * sizeof(float));
    return;
  }
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for(size_t px = 0; px < npix; px++)
  {
    const float *in = lograw + px * nch_in;
    float *out = corr + px * 3;
    double silver[3];
    for(int c = 0; c < 3; c++)
    {
      const double d = interp_curve_uniform(in[c], sim->gamma[c], sim->le0, sim->le_step,
                                            sim->curves_norm, c);
      silver[c] = sim->film_positive ? sim->film_dmax[c] - d : d;
      if(sim->couplers_donor_lm)
        silver[c] = silver[c] * (sim->couplers_donor_K[c] + sim->couplers_donor_Dref[c])
                    / (sim->couplers_donor_K[c] + silver[c]);
    }
    for(int m = 0; m < 3; m++)
    {
      double acc = 0.0;
      for(int k = 0; k < 3; k++) acc += silver[k] * sim->couplers_M[k][m];
      out[m] = (float)acc;
    }
  }
}

void sf_sim_develop(const sf_sim_t *sim, const float *lograw, const float *corr,
                    float *cmy, size_t npix, int nch_in, int nch_out)
{
  const int use_corr = sim->couplers_active && corr != NULL;
  const double(*curves)[3] = use_corr ? sim->curves_before : sim->curves_norm;
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for(size_t px = 0; px < npix; px++)
  {
    const float *in = lograw + px * nch_in;
    const float *cr = use_corr ? corr + px * 3 : NULL;
    float *out = cmy + px * nch_out;
    for(int c = 0; c < 3; c++)
    {
      double crv = cr ? (double)cr[c] : 0.0;
      /* receiver-side Langmuir applies to the inhibitor that ARRIVES, i.e.
         after the spatial diffusion blur, hence here and not in _corr */
      if(cr && sim->couplers_recv_lm)
        crv = crv * (sim->couplers_recv_Kr[c] + sim->couplers_recv_cref[c])
              / (sim->couplers_recv_Kr[c] + crv);
      const double x = (double)in[c] - crv;
      out[c] = (float)interp_curve_uniform(x, sim->gamma[c], sim->le0, sim->le_step,
                                           curves, c);
    }
  }
}

void sf_sim_print_expose(const sf_sim_t *sim, const float *cmy, float *lograw,
                         size_t npix, int nch_in, int nch_out)
{
  const int steps = sim->lut_steps;
  const sf_pchip3d_t P = { steps, sim->enl_lut, sim->enl_sx, sim->enl_sy, sim->enl_sz,
                           sim->enl_cmin, sim->enl_cmax };
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for(size_t px = 0; px < npix; px++)
  {
    const float *in = cmy + px * nch_in;
    float *out = lograw + px * nch_out;
    double l1[3];
    if(steps >= 2)
    {
      const double scale = (double)(steps - 1);
      const double r = (in[0] - sim->enl_lo[0]) / (sim->enl_hi[0] - sim->enl_lo[0]) * scale;
      const double g = (in[1] - sim->enl_lo[1]) / (sim->enl_hi[1] - sim->enl_lo[1]) * scale;
      const double b = (in[2] - sim->enl_lo[2]) / (sim->enl_hi[2] - sim->enl_lo[2]) * scale;
      pchip3d_interp(&P, r, g, b, l1);
    }
    else
    {
      const double c[3] = { in[0], in[1], in[2] };
      cmy_to_print_lograw(sim, c, l1);
    }
    /* [st] raw = 10^l1 * print_exposure; back to log10 */
    for(int m = 0; m < 3; m++)
    {
      const double r = pow(10.0, l1[m]) * sim->print_exposure;
      out[m] = (float)log10(fmax(r, 0.0) + SF_LOG_EPS);
    }
  }
}

void sf_sim_print_develop(const sf_sim_t *sim, const float *lograw, float *cmy,
                          size_t npix, int nch_in, int nch_out)
{
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for(size_t px = 0; px < npix; px++)
  {
    const float *in = lograw + px * nch_in;
    float *out = cmy + px * nch_out;
    for(int c = 0; c < 3; c++)
      out[c] = (float)interp_curve_uniform(in[c], 1.0, sim->le0, sim->le_step,
                                           sim->print_curves, c);
  }
}

void sf_sim_scan(const sf_sim_t *sim, const float *cmy, float *rgb_out, size_t npix,
                 int nch_in, int nch_out)
{
  const int steps = sim->lut_steps;
  const sf_pchip3d_t P = { steps, sim->scan_lut, sim->scan_sx, sim->scan_sy, sim->scan_sz,
                           sim->scan_cmin, sim->scan_cmax };
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for(size_t px = 0; px < npix; px++)
  {
    const float *in = cmy + px * nch_in;
    float *out = rgb_out + px * nch_out;
    double lx[3];
    if(steps >= 2)
    {
      const double scale = (double)(steps - 1);
      const double r = (in[0] - sim->scan_lo[0]) / (sim->scan_hi[0] - sim->scan_lo[0]) * scale;
      const double g = (in[1] - sim->scan_lo[1]) / (sim->scan_hi[1] - sim->scan_lo[1]) * scale;
      const double b = (in[2] - sim->scan_lo[2]) / (sim->scan_hi[2] - sim->scan_lo[2]) * scale;
      pchip3d_interp(&P, r, g, b, lx);
    }
    else
    {
      const double c[3] = { in[0], in[1], in[2] };
      cmy_to_log_xyz(sim, c, lx);
    }
    double xyz[3], rgb[3];
    for(int m = 0; m < 3; m++) xyz[m] = pow(10.0, lx[m]);
    if(sim->scan_bw_on)
    {
      /* scanner black/white point (positive film): scale toward Y in [0,1] */
      const double y = xyz[1];
      double yc = sim->scan_bw_m * y + sim->scan_bw_q;
      yc = yc < 0.0 ? 0.0 : (yc > 1.0 ? 1.0 : yc);
      const double sc = yc / (y + 1e-10);
      for(int m = 0; m < 3; m++) xyz[m] *= sc;
    }
    mat3_mulv(rgb, sim->m_out, xyz);
    if(sim->out_compress == SF_OUTPUT_COMPRESS_OKLCH)
      compress_rgb_oklch(sim, rgb);
    else if(sim->out_compress == SF_OUTPUT_COMPRESS_ACES_RGC)
      compress_rgb_aces(rgb);
    for(int c = 0; c < 3; c++) out[c] = (float)rgb[c];
  }
}

void sf_sim_process(const sf_sim_t *sim, const float *rgb_in, float *rgb_out, size_t npix,
                    int nch_in, int nch_out)
{
  float *tmp = malloc(npix * 3 * sizeof(float));
  float *corr = sim->couplers_active ? malloc(npix * 3 * sizeof(float)) : NULL;
  sf_sim_expose(sim, rgb_in, tmp, npix, nch_in, 3);
  sf_sim_lograw(tmp, npix, 3);
  if(corr) sf_sim_develop_corr(sim, tmp, corr, npix, 3);
  sf_sim_develop(sim, tmp, corr, tmp, npix, 3, 3);
  if(sim->has_print)
  {
    sf_sim_print_expose(sim, tmp, tmp, npix, 3, 3);
    sf_sim_print_develop(sim, tmp, tmp, npix, 3, 3);
  }
  sf_sim_scan(sim, tmp, rgb_out, npix, 3, nch_out);
  free(corr);
  free(tmp);
}

/* ------------------------------------------------------------------------ */
/* GPU export: float copies of the per-pixel tables                         */
/* ------------------------------------------------------------------------ */

static float *dup_f(const double *src, size_t n)
{
  float *dst = malloc(n * sizeof(float));
  if(dst)
    for(size_t i = 0; i < n; i++) dst[i] = (float)src[i];
  return dst;
}

static void cp9f(float dst[9], const double src[9])
{
  for(int i = 0; i < 9; i++) dst[i] = (float)src[i];
}

/* variants that keep the 2D array type so the compiler sees the full extent
   (a plain &a[0][0] decay trips -Werror=stringop-overread on gcc) */
static void cp33f(float dst[9], const double src[3][3])
{
  for(int i = 0; i < 3; i++)
    for(int j = 0; j < 3; j++) dst[i * 3 + j] = (float)src[i][j];
}

static float *dup_f3(const double (*src)[3], size_t rows)
{
  float *dst = malloc(rows * 3 * sizeof(float));
  if(dst)
    for(size_t i = 0; i < rows; i++)
      for(int c = 0; c < 3; c++) dst[i * 3 + c] = (float)src[i][c];
  return dst;
}

sf_sim_gpu_t *sf_sim_gpu_export(const sf_sim_t *s)
{
  if(!s || s->lut_steps < 2) return NULL; /* exact spectral: no GPU path */
  sf_sim_gpu_t *g = calloc(1, sizeof(sf_sim_gpu_t));
  if(!g) return NULL;

  cp9f(g->m_in, s->m_in);
  g->ev_scale = (float)s->ev_scale;
  g->tc_n = s->tc_n;
  g->tc_lut = dup_f(s->tc_lut, (size_t)s->tc_n * s->tc_n * 3);

  for(int c = 0; c < 3; c++) g->gamma[c] = (float)s->gamma[c];
  g->le0 = (float)s->le0;
  g->le_step = (float)s->le_step;
  g->curves_norm = dup_f3(s->curves_norm, SF_NLE);
  g->curves_before = dup_f3(s->curves_before, SF_NLE);
  cp33f(g->couplers_M, (const double (*)[3])s->couplers_M);
  for(int c = 0; c < 3; c++) g->film_dmax[c] = (float)s->film_dmax[c];
  for(int c = 0; c < 3; c++)
  {
    g->grain_rms[c] = (float)s->grain_rms[c];
    g->grain_uniformity[c] = (float)s->grain_uniformity[c];
    /* self-consistent with g->film_dmax: see sf_sim_film_grain3 */
    g->grain_dmin[c] = (float)s->film_dmin[c];
  }
  g->film_positive = s->film_positive;
  g->couplers_active = s->couplers_active;

  g->has_print = s->has_print;
  g->steps = s->lut_steps;
  const size_t n3 = (size_t)s->lut_steps * s->lut_steps * s->lut_steps * 3;
  const size_t m3 = (size_t)(s->lut_steps - 1) * (s->lut_steps - 1) * (s->lut_steps - 1) * 3;
  if(s->has_print)
  {
    for(int c = 0; c < 3; c++)
    {
      g->enl_lo[c] = (float)s->enl_lo[c];
      g->enl_hi[c] = (float)s->enl_hi[c];
    }
    g->enl_lut = dup_f(s->enl_lut, n3);
    g->enl_sx = dup_f(s->enl_sx, n3);
    g->enl_sy = dup_f(s->enl_sy, n3);
    g->enl_sz = dup_f(s->enl_sz, n3);
    g->enl_cmin = dup_f(s->enl_cmin, m3);
    g->enl_cmax = dup_f(s->enl_cmax, m3);
    g->print_exposure = (float)s->print_exposure;
    g->print_curves = dup_f3(s->print_curves, SF_NLE);
  }
  for(int c = 0; c < 3; c++)
  {
    g->scan_lo[c] = (float)s->scan_lo[c];
    g->scan_hi[c] = (float)s->scan_hi[c];
  }
  g->scan_lut = dup_f(s->scan_lut, n3);
  g->scan_sx = dup_f(s->scan_sx, n3);
  g->scan_sy = dup_f(s->scan_sy, n3);
  g->scan_sz = dup_f(s->scan_sz, n3);
  g->scan_cmin = dup_f(s->scan_cmin, m3);
  g->scan_cmax = dup_f(s->scan_cmax, m3);
  cp9f(g->m_out, s->m_out);
  g->scan_bw_on = s->scan_bw_on;
  g->scan_bw_m = (float)s->scan_bw_m;
  g->scan_bw_q = (float)s->scan_bw_q;
  g->film_bw = s->film_bw;
  g->coupler_diff_um = (float)s->coupler_diff_um;
  g->coupler_tail_um = (float)s->coupler_tail_um;
  g->coupler_tail_w = (float)s->coupler_tail_w;
  g->couplers_donor_lm = s->couplers_donor_lm;
  g->couplers_recv_lm = s->couplers_recv_lm;
  for(int c = 0; c < 3; c++)
  {
    /* INFINITY-safe: when linear, ship K large enough that the float
       formula degenerates to identity even without isinf checks */
    g->couplers_donor_K[c] = s->couplers_donor_lm ? (float)s->couplers_donor_K[c] : 1e30f;
    g->couplers_donor_Dref[c] = (float)s->couplers_donor_Dref[c];
    g->couplers_recv_Kr[c] = s->couplers_recv_lm ? (float)s->couplers_recv_Kr[c] : 1e30f;
    g->couplers_recv_cref[c] = (float)s->couplers_recv_cref[c];
  }

  g->out_compress = s->out_compress;
  cp9f(g->out_rgb2xyz, s->out_rgb2xyz);
  cp9f(g->out_xyz2rgb, s->out_xyz2rgb);
  cp9f(g->oklab_m1, SF_OKLAB_M1);
  cp9f(g->oklab_m2, SF_OKLAB_M2);
  cp9f(g->oklab_m1inv, s->oklab_m1inv);
  cp9f(g->oklab_m2inv, s->oklab_m2inv);
  g->cmax_table = s->cmax; /* borrowed; may be NULL when compression != oklch */
  g->cmax_nl = SF_CMAX_NL;
  g->cmax_nh = SF_CMAX_NH;
  return g;
}

void sf_sim_gpu_free(sf_sim_gpu_t *g)
{
  if(!g) return;
  free(g->tc_lut);
  free(g->curves_norm);
  free(g->curves_before);
  free(g->enl_lut); free(g->enl_sx); free(g->enl_sy); free(g->enl_sz);
  free(g->enl_cmin); free(g->enl_cmax);
  free(g->print_curves);
  free(g->scan_lut); free(g->scan_sx); free(g->scan_sy); free(g->scan_sz);
  free(g->scan_cmin); free(g->scan_cmax);
  free(g);
}

int sf_sim_film_bw(const sf_sim_t *sim) { return sim ? sim->film_bw : 0; }

void sf_sim_coupler_diffusion(const sf_sim_t *sim, double *size_um, double *tail_um,
                              double *tail_w)
{
  *size_um = sim ? sim->coupler_diff_um : SF_COUPLER_BLUR_UM;
  *tail_um = sim ? sim->coupler_tail_um : 0.0;
  *tail_w = sim ? sim->coupler_tail_w : 0.0;
}

void sf_sim_film_dmax3(const sf_sim_t *sim, float dmax[3])
{
  for(int c = 0; c < 3; c++) dmax[c] = sim ? (float)sim->film_dmax[c] : 2.2f;
}

void sf_sim_film_grain3(const sf_sim_t *sim, float rms[3], float uniformity[3], float dmin[3])
{
  /* matches SF_GRAIN_LEGACY_* in spektra_core.h */
  static const float legacy_rms[3] = { 6.0f, 8.0f, 10.0f };
  static const float legacy_unif[3] = { 0.97f, 0.97f, 0.97f };
  static const float legacy_dmin[3] = { 0.03f, 0.03f, 0.03f };
  for(int c = 0; c < 3; c++)
  {
    rms[c] = sim ? (float)sim->grain_rms[c] : legacy_rms[c];
    uniformity[c] = sim ? (float)sim->grain_uniformity[c] : legacy_unif[c];
    /* film_dmin (this module's own curve floor), NOT p.grain_density_min:
       the grain formula reconstructs dmax_abs = dmax_c + dmin, which is only
       the film's real absolute D-max when dmin is the SAME floor that
       produced dmax_c. An independently-sourced density_min (e.g. from a
       separate curve-fit pass upstream) breaks that identity and silently
       biases the particle count — see sf_grain_delta_dmax. */
    dmin[c] = sim ? (float)sim->film_dmin[c] : legacy_dmin[c];
  }
}

