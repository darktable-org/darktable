/*
    This file is part of darktable,
    Copyright (C) 2013-2026 darktable developers.

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

#include "develop/lightroom.h"
#include "common/colorlabels.h"
#include "common/colorspaces.h"
#include "common/curve_tools.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/math.h"
#include "common/ratings.h"
#include "common/tags.h"
#include "common/metadata.h"
#include "control/control.h"
#include "develop/masks.h"

#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <stdlib.h>
#include <string.h>

// copy here the iop params struct with the actual version. This is so to
// be as independent as possible of any iop evolutions. Indeed, we create
// the iop params into the database for a specific version. We then ask
// for a reload of the history parameter. If the iop has evolved since then
// the legacy circuitry will be called to convert the parameters.
//
// to add a new iop:
// 1. copy the struct, with _v<version> added to its name and to the names of
//    the structs it contains, as legacy_params() does in the modules
// 2. add LRDT_<iop_name>_VERSION with corresponding module version
// 3. use this version to pass in dt_add_hist()

#define LRDT_CROP_VERSION 3
typedef struct dt_iop_crop_params_v3_t
{
  float cx, cy, cw, ch;
  int ratio_n, ratio_d;
} dt_iop_crop_params_v3_t;

#define LRDT_ASHIFT_VERSION 5
#define LRDT_ASHIFT_MAX_SAVED_LINES 50
typedef enum dt_iop_ashift_mode_t
{
  ASHIFT_MODE_GENERIC = 0,
  ASHIFT_MODE_SPECIFIC = 1
} dt_iop_ashift_mode_t;

typedef enum dt_iop_ashift_crop_t
{
  ASHIFT_CROP_OFF = 0,
  ASHIFT_CROP_LARGEST = 1,
  ASHIFT_CROP_ASPECT = 2
} dt_iop_ashift_crop_t;

typedef struct dt_iop_ashift_params_v5_t
{
  float rotation, lensshift_v, lensshift_h, shear;
  float f_length, crop_factor, orthocorr, aspect;
  dt_iop_ashift_mode_t mode;
  dt_iop_ashift_crop_t cropmode;
  float cl, cr, ct, cb;
  float last_drawn_lines[LRDT_ASHIFT_MAX_SAVED_LINES * 4];
  int last_drawn_lines_count;
  float last_quad_lines[8];
} dt_iop_ashift_params_v5_t;

#define LRDT_FLIP_VERSION 2
typedef struct dt_iop_flip_params_v2_t
{
  dt_image_orientation_t orientation;
} dt_iop_flip_params_v2_t;

#define LRDT_EXPOSURE_VERSION 7
typedef enum dt_iop_exposure_mode_t
{
  EXPOSURE_MODE_MANUAL = 0,
  EXPOSURE_MODE_DEFLICKER = 1
} dt_iop_exposure_mode_t;

typedef struct dt_iop_exposure_params_v7_t
{
  dt_iop_exposure_mode_t mode;
  float black;
  float exposure;
  float deflicker_percentile;
  float deflicker_target_level;
  gboolean compensate_exposure_bias;
  gboolean compensate_hilite_pres;
} dt_iop_exposure_params_v7_t;

#define LRDT_GRAIN_VERSION 1
typedef enum _dt_iop_grain_channel_t
{
  DT_GRAIN_CHANNEL_HUE = 0,
  DT_GRAIN_CHANNEL_SATURATION,
  DT_GRAIN_CHANNEL_LIGHTNESS,
  DT_GRAIN_CHANNEL_RGB
} _dt_iop_grain_channel_t;

typedef struct dt_iop_grain_params_v1_t
{
  _dt_iop_grain_channel_t channel;
  float scale;
  float strength;
} dt_iop_grain_params_v1_t;

typedef enum dt_iop_dither_t
{
  DITHER_OFF = 0,
  DITHER_8BIT = 1,
  DITHER_16BIT = 2
} dt_iop_dither_t;

typedef struct dt_iop_vignette_vector_2d_v3_t
{
  float x;
  float y;
} dt_iop_vignette_vector_2d_v3_t;

#define LRDT_VIGNETTE_VERSION 3
typedef struct dt_iop_vignette_params_v3_t
{
  float scale;               // 0 - 100 Inner radius, percent of largest image dimension
  float falloff_scale;       // 0 - 100 Radius for falloff -- outer radius = inner radius + falloff_scale
  float brightness;          // -1 - 1 Strength of brightness reduction
  float saturation;          // -1 - 1 Strength of saturation reduction
  dt_iop_vignette_vector_2d_v3_t center; // Center of vignette
  gboolean autoratio;        //
  float whratio;             // 0-1 = width/height ratio, 1-2 = height/width ratio + 1
  float shape;
  int dithering; // if and how to perform dithering
} dt_iop_vignette_params_v3_t;

#define LRDT_RETOUCH_VERSION 3
#define RETOUCH_NO_FORMS 300

typedef enum dt_iop_retouch_fill_modes_t
{
  DT_IOP_RETOUCH_FILL_ERASE = 0,
  DT_IOP_RETOUCH_FILL_COLOR = 1
} dt_iop_retouch_fill_modes_t;

typedef enum dt_iop_retouch_blur_types_t
{
  DT_IOP_RETOUCH_BLUR_GAUSSIAN = 0,
  DT_IOP_RETOUCH_BLUR_BILATERAL = 1
} dt_iop_retouch_blur_types_t;

typedef enum dt_iop_retouch_algo_type_t
{
  DT_IOP_RETOUCH_NONE = 0,
  DT_IOP_RETOUCH_CLONE = 1,
  DT_IOP_RETOUCH_HEAL = 2,
  DT_IOP_RETOUCH_BLUR = 3,
  DT_IOP_RETOUCH_FILL = 4
} dt_iop_retouch_algo_type_t;

typedef struct dt_iop_retouch_form_data_v3_t
{
  dt_mask_id_t formid;
  int scale;
  dt_iop_retouch_algo_type_t algorithm;
  dt_iop_retouch_blur_types_t blur_type;
  float blur_radius;
  dt_iop_retouch_fill_modes_t fill_mode;
  float fill_color[3];
  float fill_brightness;
  int distort_mode;
} dt_iop_retouch_form_data_v3_t;

typedef struct dt_iop_retouch_params_v3_t
{
  dt_iop_retouch_form_data_v3_t rt_forms[RETOUCH_NO_FORMS];
  dt_iop_retouch_algo_type_t algorithm;
  int num_scales;
  int curr_scale;
  int merge_from_scale;
  float preview_levels[3];
  dt_iop_retouch_blur_types_t blur_type;
  float blur_radius;
  dt_iop_retouch_fill_modes_t fill_mode;
  float fill_color[3];
  float fill_brightness;
  int max_heal_iter;
} dt_iop_retouch_params_v3_t;

#define LRDT_TONECURVE_VERSION 5
#define DT_IOP_TONECURVE_MAXNODES 20
typedef enum tonecurve_channel_t
{
  ch_L = 0,
  ch_a = 1,
  ch_b = 2,
  ch_max = 3
} tonecurve_channel_t;

typedef struct dt_iop_tonecurve_node_v5_t
{
  float x;
  float y;
} dt_iop_tonecurve_node_v5_t;

typedef struct dt_iop_tonecurve_params_v5_t
{
  dt_iop_tonecurve_node_v5_t tonecurve[3][DT_IOP_TONECURVE_MAXNODES]; // three curves (L, a, b) with max number
                                                                   // of nodes
  int tonecurve_nodes[3];
  int tonecurve_type[3];
  int tonecurve_autoscale_ab;
  int tonecurve_preset;
  int tonecurve_unbound_ab;
  int preserve_colors;
} dt_iop_tonecurve_params_v5_t;

#define LRDT_TONECURVE_AUTOMATIC_RGB 3
#define LRDT_RGB_NORM_AVERAGE 3

#define LRDT_COLORZONES_VERSION 2
#define DT_IOP_COLORZONES_BANDS 8

typedef enum dt_iop_colorzones_channel_t
{
  DT_IOP_COLORZONES_L = 0,
  DT_IOP_COLORZONES_C = 1,
  DT_IOP_COLORZONES_h = 2
} dt_iop_colorzones_channel_t;

typedef struct dt_iop_colorzones_params_v2_t
{
  int32_t channel;
  float equalizer_x[3][DT_IOP_COLORZONES_BANDS], equalizer_y[3][DT_IOP_COLORZONES_BANDS];
} dt_iop_colorzones_params_v2_t;

#define LRDT_SPLITTONING_VERSION 1
typedef struct dt_iop_splittoning_params_v1_t
{
  float shadow_hue;
  float shadow_saturation;
  float highlight_hue;
  float highlight_saturation;
  float balance;  // center luminance of gradient
  float compress; // Compress range
} dt_iop_splittoning_params_v1_t;

#define LRDT_BILAT_VERSION 3
typedef enum dt_iop_bilat_mode_t
{
  s_mode_bilateral = 0,
  s_mode_local_laplacian = 1
} dt_iop_bilat_mode_t;

typedef struct dt_iop_bilat_params_v3_t
{
  dt_iop_bilat_mode_t mode;
  float sigma_r;
  float sigma_s;
  float detail;
  float midtone;
} dt_iop_bilat_params_v3_t;


#define LRDT_COLORIN_VERSION 7
typedef enum dt_iop_color_normalize_t
{
  DT_NORMALIZE_OFF = 0
} dt_iop_color_normalize_t;

typedef struct dt_iop_colorin_params_v7_t
{
  dt_colorspaces_color_profile_type_t type;
  char filename[DT_IOP_COLOR_ICC_LEN];
  dt_iop_color_intent_t intent;
  dt_iop_color_normalize_t normalize;
  gboolean blue_mapping;
  dt_colorspaces_color_profile_type_t type_work;
  char filename_work[DT_IOP_COLOR_ICC_LEN];
} dt_iop_colorin_params_v7_t;

//
// end of iop structs
//

// the blend params for Lr import, not used in this mode (mode=0), as for iop generate the blend params for
// the
// version specified above.

#define LRDT_BLEND_VERSION 4
#define DEVELOP_BLENDIF_SIZE 16

typedef struct dt_lr_develop_blend_params_v4_t
{
  /** blending mode */
  uint32_t mode;
  /** mixing opacity */
  float opacity;
  /** id of mask in current pipeline */
  dt_mask_id_t mask_id;
  /** blendif mask */
  uint32_t blendif;
  /** blur radius */
  float radius;
  /** blendif parameters */
  float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
} dt_lr_develop_blend_params_v4_t;

//
// end of blend_params
//

typedef struct lr2dt
{
  float lr, dt;
} lr2dt_t;

char *dt_get_lightroom_xmp(const dt_imgid_t imgid)
{
  char pathname[DT_MAX_FILENAME_LEN];
  gboolean from_cache = TRUE;

  // Get full pathname
  dt_image_full_path(imgid, pathname, DT_MAX_FILENAME_LEN, &from_cache);

  // Look for extension
  char *pos = strrchr(pathname, '.');

  if(pos == NULL)
    return NULL;

  // If found, replace extension with xmp
  strncpy(pos + 1, "xmp", 4);
  if(g_file_test(pathname, G_FILE_TEST_EXISTS))
    return g_strdup(pathname);

  strncpy(pos + 1, "XMP", 4);
  if(g_file_test(pathname, G_FILE_TEST_EXISTS))
    return g_strdup(pathname);

  return NULL;
}

static float get_interpolate(lr2dt_t lr2dt_table[], const float value)
{
  int k = 0;

  while(lr2dt_table[k + 1].lr < value) k++;

  return lr2dt_table[k].dt
         + ((value - lr2dt_table[k].lr) / (lr2dt_table[k + 1].lr - lr2dt_table[k].lr))
           * (lr2dt_table[k + 1].dt - lr2dt_table[k].dt);
}

static float lr2dt_blacks(const float value)
{
  lr2dt_t lr2dt_blacks_table[]
      = { { -100, 0.020 }, { -50, 0.005 }, { 0, 0 }, { 50, -0.005 }, { 100, -0.010 } };

  return get_interpolate(lr2dt_blacks_table, value);
}

// the vignette mappings below were fitted to Lightroom's renders, with
// darktable's vignette applied to the same image. Lightroom's amount darkens
// by a fraction of gamma-encoded values, darktable's brightness of linear ones
static float lr2dt_vignette_gain(const float value)
{
  // beyond Lightroom's range the power would be of a negative number
  const float amount = CLAMP(value, -100.0f, 100.0f);
  return amount < 0.0f ? powf(1.0f + amount / 100.0f, 2.5f) - 1.0f : 0.74f * amount / 100.0f;
}

// the width of the falloff, and where its middle lies at midpoint 50, both
// in percent of the long side's half
static float lr2dt_vignette_falloff(const float feather)
{
  lr2dt_t lr2dt_vignette_table[] = { { 0, 5 }, { 50, 75.5 }, { 100, 82 } };

  return get_interpolate(lr2dt_vignette_table, feather);
}

static float lr2dt_vignette_center(const float feather)
{
  lr2dt_t lr2dt_vignette_table[] = { { 0, 99.5 }, { 50, 92.2 }, { 100, 106.9 } };

  return get_interpolate(lr2dt_vignette_table, feather);
}

static float lr2dt_grain_amount(const float value)
{
  lr2dt_t lr2dt_grain_table[] = { { 0, 0 }, { 25, 20 }, { 50, 40 }, { 100, 80 } };

  return get_interpolate(lr2dt_grain_table, value);
}

static float lr2dt_grain_frequency(const float value)
{
  lr2dt_t lr2dt_grain_table[] = { { 0, 100 }, { 50, 100 }, { 75, 400 }, { 100, 800 } };

  return get_interpolate(lr2dt_grain_table, value) / 53.3;
}

// darktable's split toning measures lightness on linear display-referred
// RGB, Lightroom's balance a perceptual one; a positive balance favors the
// highlights, which moves the crossover down. The exponent was fitted to
// Lightroom's renders
static float lr2dt_splittoning_balance(const float value)
{
  return powf(CLAMP(0.5f - value / 200.0f, 0.0f, 1.0f), 1.5f);
}

// at equal saturation darktable tones more strongly than Lightroom
static float lr2dt_splittoning_saturation(const float value)
{
  return 0.7f * value / 100.0f;
}

static float lr2dt_clarity(const float value)
{
  lr2dt_t lr2dt_clarity_table[] = { { -100, -.650 }, { 0, 0 }, { 100, .650 } };

  return get_interpolate(lr2dt_clarity_table, value);
}

// Lightroom's parametric tone curve, following RawTherapee's model of it
// (DCT_Parametric in rtengine/diagonalcurves.cc), which has the same four
// regions and three splits. A region value of 0 leaves its region alone
static double _param_lower(const double x)
{
  return (2.0 - x) * x * x * x;
}

static double _param_upper(const double x)
{
  return x * (2.0 + (x - 2.0) * x * x);
}

// bends the lower half of [0, 1] by sh and the upper half by hl, both in
// [0, 1] with 0.5 for no change
static double _param_bend(const double x, const double sh, const double hl)
{
  const double t = (1.0 - sh) * (1.0 - hl) * _param_lower(x) + sh * hl * _param_upper(x);
  if(x <= 0.5)
    return t + (1.0 - sh) * hl * _param_lower(2.0 * x) * 0.5
             + sh * (1.0 - hl) * _param_upper(2.0 * x) * 0.5;
  return t + (1.0 - sh) * hl * (0.5 + _param_upper(2.0 * x - 1.0) * 0.5)
           + sh * (1.0 - hl) * (0.5 + _param_lower(2.0 * x - 1.0) * 0.5);
}

// value[] is shadows, darks, lights, highlights in [-100, 100]
static float _lr_parametric_curve(const float x, const float split[3], const int value[4])
{
  if(x <= 0.0f) return 0.0f;

  const double s1 = CLAMP(split[0], 0.001, 0.99);
  const double s2 = CLAMP(split[1], 0.001, 0.99);
  const double s3 = CLAMP(split[2], 0.001, 0.99);
  const double shadows = (value[0] + 100.0) / 200.0;
  const double darks = (value[1] + 100.0) / 200.0;
  const double lights = (value[2] + 100.0) / 200.0;
  const double highlights = (value[3] + 100.0) / 200.0;

  // gamma that moves the midtone split to 0.5, where darks and lights meet
  const double mc = -log(2.0) / log(s2);
  const double mid = pow(_param_bend(0.5, darks, lights), 1.0 / mc);
  const double base = pow(_param_bend(pow(x, mc), darks, lights), 1.0 / mc);

  if(x < s2)
  {
    const double sc = -log(2.0) / log(s1 / s2);
    return mid * pow(_param_bend(pow(base / mid, sc), shadows, 0.5), 1.0 / sc);
  }

  const double hc = -log(2.0) / log((s3 - s2) / (1.0 - s2));
  const double ht = pow((base - mid) / (1.0 - mid), hc);
  if(ht < 1e-6) return base;
  return mid + (1.0 - mid) * pow(_param_bend(ht, 0.5, highlights), 1.0 / hc);
}

// a curve with fewer than two nodes, or with every node on the diagonal,
// leaves the image unchanged
static gboolean _tonecurve_is_identity(const dt_iop_tonecurve_params_v5_t *p)
{
  if(p->tonecurve_nodes[ch_L] < 2) return TRUE;

  for(int k = 0; k < p->tonecurve_nodes[ch_L]; k++)
    if(fabsf(p->tonecurve[ch_L][k].x - p->tonecurve[ch_L][k].y) > 1e-4f)
      return FALSE;

  return TRUE;
}

// returns the history num of the new entry, for masks that belong to it
static int32_t _add_hist_with_mask(const dt_imgid_t imgid, const char *operation, const dt_iop_params_t *params,
                                   const int params_size, char *imported, const size_t imported_len,
                                   const int version, int *import_count, const dt_mask_id_t mask_id)
{
  int32_t num = 0;
  const dt_lr_develop_blend_params_v4_t blend_params = { .mask_id = mask_id };

  //  get current num if any
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT COUNT(*) FROM main.history WHERE imgid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    num = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);

  // add new history info
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO main.history"
                              "  (imgid, num, module, operation, op_params, enabled,"
                              "   blendop_params, blendop_version, multi_priority, multi_name)"
                              " VALUES (?1, ?2, ?3, ?4, ?5, 1, ?6, ?7, 0, ' ')",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, num);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 5, params, params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 6, &blend_params, sizeof(dt_lr_develop_blend_params_v4_t), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, LRDT_BLEND_VERSION);

  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // also bump history_end
  dt_image_sync_history_end(imgid);

  if(imported[0]) g_strlcat(imported, ", ", imported_len);
  g_strlcat(imported, dt_iop_get_localized_name(operation), imported_len);
  (*import_count)++;

  return num;
}

static void dt_add_hist(const dt_imgid_t imgid, const char *operation, const dt_iop_params_t *params, const int params_size, char *imported,
                        const size_t imported_len, const int version, int *import_count)
{
  _add_hist_with_mask(imgid, operation, params, params_size, imported, imported_len, version, import_count, 0);
}

#define MAX_PTS 20

// a spot removal shape, normalized to Lightroom's view of the raw: a circle
// or, with dabs, a brush stroke
typedef struct lr_spot_t
{
  // the center, or the first dab
  float x, y;
  float radius;
  // position x, y clones from
  float xc, yc;
  gboolean clone;
  float opacity;
  // the share of the radius the edge fades over, 0 to 1
  float feather;
  // n_dabs pairs of x, y; NULL for a circle
  float *dabs;
  int n_dabs;
} lr_spot_t;

typedef struct lr_data_t
{
  dt_iop_crop_params_v3_t pcr;
  dt_iop_ashift_params_v5_t pas;
  gboolean has_crop;

  dt_iop_flip_params_v2_t pf;
  gboolean has_flip;

  dt_iop_exposure_params_v7_t pe;
  gboolean has_exposure;

  dt_iop_vignette_params_v3_t pv;
  gboolean has_vignette;

  dt_iop_grain_params_v1_t pg;
  gboolean has_grain;

  lr_spot_t spots[RETOUCH_NO_FORMS];
  int n_spots;
  gboolean has_retouch_areas;

  dt_iop_tonecurve_params_v5_t ptc;
  int ptc_value[4];
  float ptc_split[3];
  int curve_pts[MAX_PTS][2];
  int n_pts;

  dt_iop_colorzones_params_v2_t pcz;
  gboolean has_colorzones;

  dt_iop_splittoning_params_v1_t pst;
  gboolean has_splittoning;

  dt_iop_bilat_params_v3_t pbl;
  gboolean has_bilat;

  gboolean has_tags;

  int rating;
  gboolean has_rating;

  gdouble lat, lon;
  gdouble lat_ref, lon_ref;
  gboolean has_gps;

  int color;
  gboolean has_colorlabel;

  float fratio;                // factor ratio image
  float crop_roundness;        // from lightroom
  float vignette_midpoint, vignette_feather;
  int iwidth, iheight;         // image width / height
  dt_exif_image_orientation_t orientation;
} lr_data_t;

// three helper functions for parsing RetouchInfo entries. sscanf doesn't work due to floats.
static gboolean _read_float(const char **startptr, const char *key, float *value)
{
  const char *iter = *startptr;
  while(*iter == ' ') iter++;
  if(!g_str_has_prefix(iter, key))
    return FALSE;
  iter += strlen(key);
  while(*iter == ' ') iter++;
  if(*iter++ != '=')
    return FALSE;
  while(*iter == ' ') iter++;
  *value = g_ascii_strtod(iter, (char **)startptr);
  return iter != *startptr;
}

static gboolean _read_word(const char **startptr, const char *key, const char **word)
{
  const char *iter = *startptr;
  while(*iter == ' ') iter++;
  if(!g_str_has_prefix(iter, key))
    return FALSE;
  iter += strlen(key);
  while(*iter == ' ') iter++;
  if(*iter++ != '=')
    return FALSE;
  while(*iter == ' ') iter++;
  *word = iter;
  while((*iter >= 'a' && *iter <= 'z') || (*iter >= 'A' && *iter <= 'Z')) iter++;
  *startptr = iter;
  return TRUE;
}

static gboolean _skip_comma(const char **startptr)
{
  if(**startptr != ',')
    return FALSE;
  (*startptr)++;
  return TRUE;
}

// helpers for the nested RDF of RetouchAreas, where a property can be an
// attribute or a child element, and a list item its own resource or the
// parent of an rdf:Description
static xmlNodePtr _rdf_child(const xmlNodePtr node, const char *name)
{
  for(xmlNodePtr child = node ? node->children : NULL; child; child = child->next)
    if(child->type == XML_ELEMENT_NODE && !xmlStrcmp(child->name, (const xmlChar *)name))
      return child;
  return NULL;
}

static xmlNodePtr _rdf_resource(const xmlNodePtr li)
{
  const xmlNodePtr desc = _rdf_child(li, "Description");
  return desc ? desc : li;
}

// the first rdf:li of the rdf:Seq in node
static xmlNodePtr _rdf_seq_first(const xmlNodePtr node)
{
  return _rdf_child(_rdf_child(node, "Seq"), "li");
}

static xmlNodePtr _rdf_seq_next(const xmlNodePtr li)
{
  for(xmlNodePtr next = li->next; next; next = next->next)
    if(next->type == XML_ELEMENT_NODE && !xmlStrcmp(next->name, (const xmlChar *)"li"))
      return next;
  return NULL;
}

static xmlChar *_rdf_prop(const xmlDocPtr doc, const xmlNodePtr node, const char *name)
{
  xmlChar *value = xmlGetProp(node, (const xmlChar *)name);
  if(value) return value;
  const xmlNodePtr child = _rdf_child(node, name);
  return child ? xmlNodeListGetString(doc, child->children, 1) : NULL;
}

static float _rdf_prop_float(const xmlDocPtr doc, const xmlNodePtr node, const char *name, const float fallback)
{
  xmlChar *value = _rdf_prop(doc, node, name);
  const float f = value ? g_ascii_strtod((const char *)value, NULL) : fallback;
  xmlFree(value);
  return f;
}

static gboolean _rdf_prop_is(const xmlDocPtr doc, const xmlNodePtr node, const char *name, const char *expected)
{
  xmlChar *value = _rdf_prop(doc, node, name);
  const gboolean is = value && !xmlStrcmp(value, (const xmlChar *)expected);
  xmlFree(value);
  return is;
}

// dabs are "d x y", one per list item
static float *_read_dabs(const xmlDocPtr doc, const xmlNodePtr mask, int *n_dabs)
{
  GArray *dabs = g_array_new(FALSE, FALSE, sizeof(float));
  for(xmlNodePtr li = _rdf_seq_first(_rdf_child(mask, "Dabs")); li; li = _rdf_seq_next(li))
  {
    xmlChar *cvalue = xmlNodeListGetString(doc, li->children, 1);
    if(!cvalue) continue;

    const char *iter = (const char *)cvalue;
    while(*iter == ' ' || g_ascii_isalpha(*iter)) iter++;
    char *end = NULL;
    float xy[2];
    xy[0] = g_ascii_strtod(iter, &end);
    const gboolean has_x = end != iter;
    iter = end;
    xy[1] = g_ascii_strtod(iter, &end);
    // a dab on top of the previous one adds nothing to a stroke
    if(has_x && end != iter
       && (dabs->len == 0
           || xy[0] != g_array_index(dabs, float, dabs->len - 2)
           || xy[1] != g_array_index(dabs, float, dabs->len - 1)))
      g_array_append_vals(dabs, xy, 2);
    xmlFree(cvalue);
  }
  *n_dabs = dabs->len / 2;
  return (float *)g_array_free(dabs, *n_dabs == 0);
}

static void _free_spots(lr_data_t *data)
{
  for(int k = 0; k < data->n_spots; k++)
    g_free(data->spots[k].dabs);
  data->n_spots = 0;
}

// one area per spot, each with one or more masks: circles are Mask/Ellipse,
// brushed spots Mask/Paint
static void _read_retouch_area(const xmlDocPtr doc, const xmlNodePtr li, lr_data_t *data)
{
  const xmlNodePtr area = _rdf_resource(li);
  // the remove tool, heal_patchmatch, fills from a patch only Lightroom
  // has, in an .acr sidecar, and records no source
  const gboolean clone = _rdf_prop_is(doc, area, "SpotType", "clone");
  if(!clone && !_rdf_prop_is(doc, area, "SpotType", "heal")) return;

  // the source position of the first mask's center or first dab, whose y
  // Lightroom calls OffsetY
  const float sx = _rdf_prop_float(doc, area, "SourceX", NAN);
  const float sy = _rdf_prop_float(doc, area, "OffsetY", NAN);
  if(isnan(sx) || isnan(sy)) return;

  const float opacity = CLAMP(_rdf_prop_float(doc, area, "Opacity", 1.0f), 0.0f, 1.0f);
  const float feather = CLAMP(_rdf_prop_float(doc, area, "Feather", 0.0f), 0.0f, 1.0f);
  float dx = NAN, dy = NAN;

  for(xmlNodePtr mli = _rdf_seq_first(_rdf_child(area, "Masks"));
      mli && data->n_spots < RETOUCH_NO_FORMS;
      mli = _rdf_seq_next(mli))
  {
    const xmlNodePtr mask = _rdf_resource(mli);
    // erasing strokes, which take away from the spot, are left out
    if(_rdf_prop_is(doc, mask, "MaskActive", "false")
       || _rdf_prop_is(doc, mask, "MaskInverted", "true")
       || _rdf_prop_float(doc, mask, "MaskValue", 1.0f) <= 0.0f)
      continue;

    lr_spot_t s = { .clone = clone, .opacity = opacity, .feather = feather };
    if(_rdf_prop_is(doc, mask, "What", "Mask/Ellipse"))
    {
      s.x = _rdf_prop_float(doc, mask, "X", NAN);
      s.y = _rdf_prop_float(doc, mask, "Y", NAN);
      // the spot tool only draws circles
      s.radius = _rdf_prop_float(doc, mask, "SizeX", NAN);
    }
    else if(_rdf_prop_is(doc, mask, "What", "Mask/Paint"))
    {
      s.radius = _rdf_prop_float(doc, mask, "Radius", NAN);
      s.dabs = _read_dabs(doc, mask, &s.n_dabs);
      if(s.n_dabs == 0) continue;
      s.x = s.dabs[0];
      s.y = s.dabs[1];
      // a single dab is a circle
      if(s.n_dabs == 1)
      {
        g_clear_pointer(&s.dabs, g_free);
        s.n_dabs = 0;
      }
    }
    else
      continue;

    if(isnan(s.x) || isnan(s.y) || !(s.radius > 0.0f))
    {
      g_free(s.dabs);
      continue;
    }

    if(isnan(dx))
    {
      dx = sx - s.x;
      dy = sy - s.y;
    }
    s.xc = s.x + dx;
    s.yc = s.y + dy;
    data->spots[data->n_spots++] = s;
  }
}

/* lrop handle the Lr operation and convert it as a dt iop */
static void _lrop(const dt_develop_t *dev, const xmlDocPtr doc, const dt_imgid_t imgid,
                  const xmlChar *name, const xmlChar *value, const xmlNodePtr node, lr_data_t *data)
{
  const float hfactor = 3.0 / 9.0; // hue factor adjustment (use 3 out of 9 boxes in colorzones)
  const float lfactor = 4.0 / 9.0; // lightness factor adjustment (use 4 out of 9 boxes in colorzones)

  if(value)
  {
    if(!xmlStrcmp(name, (const xmlChar *)"CropTop"))
      data->pcr.cy = g_ascii_strtod((char *)value, NULL);
    else if(!xmlStrcmp(name, (const xmlChar *)"CropRight"))
      data->pcr.cw = g_ascii_strtod((char *)value, NULL);
    else if(!xmlStrcmp(name, (const xmlChar *)"CropLeft"))
      data->pcr.cx = g_ascii_strtod((char *)value, NULL);
    else if(!xmlStrcmp(name, (const xmlChar *)"CropBottom"))
      data->pcr.ch = g_ascii_strtod((char *)value, NULL);
    else if(!xmlStrcmp(name, (const xmlChar *)"CropAngle"))
      data->pas.rotation = g_ascii_strtod((char *)value, NULL);
    else if(!xmlStrcmp(name, (const xmlChar *)"ImageWidth"))
      data->iwidth = atoi((char *)value);
    else if(!xmlStrcmp(name, (const xmlChar *)"ImageLength"))
      data->iheight = atoi((char *)value);
    else if(!xmlStrcmp(name, (const xmlChar *)"Orientation"))
    {
      data->orientation = atoi((char *)value);
      if(dev != NULL && ((dev->image_storage.orientation == ORIENTATION_NONE && data->orientation != EXIF_ORIENTATION_NONE)
                        || (dev->image_storage.orientation == ORIENTATION_ROTATE_CW_90_DEG && data->orientation != EXIF_ORIENTATION_ROTATE_CW_90_DEG)
                        || (dev->image_storage.orientation == ORIENTATION_ROTATE_CCW_90_DEG && data->orientation != EXIF_ORIENTATION_ROTATE_CCW_90_DEG)))
        data->has_flip = TRUE;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"HasCrop"))
    {
      if(!xmlStrcmp(value, (const xmlChar *)"True")) data->has_crop = TRUE;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"Blacks2012"))
    {
      const int v = atoi((char *)value);
      if(v != 0)
      {
        data->has_exposure = TRUE;
        data->pe.black = lr2dt_blacks((float)v);
      }
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"Exposure2012"))
    {
      const float v = g_ascii_strtod((char *)value, NULL);
      if(v != 0.0)
      {
        data->has_exposure = TRUE;
        data->pe.exposure = v;
      }
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"PostCropVignetteAmount"))
    {
      const int v = atoi((char *)value);
      if(v != 0)
      {
        data->has_vignette = TRUE;
        data->pv.brightness = lr2dt_vignette_gain((float)v);
      }
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"PostCropVignetteMidpoint"))
    {
      const int v = atoi((char *)value);
      data->vignette_midpoint = CLAMP(v, 0, 100);
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"PostCropVignetteStyle"))
    {
      const int v = atoi((char *)value);
      if(v == 1) // Highlight Priority
        data->pv.saturation = -0.300;
      else // Color Priority & Paint Overlay
        data->pv.saturation = -0.200;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"PostCropVignetteFeather"))
    {
      const int v = atoi((char *)value);
      data->vignette_feather = CLAMP(v, 0, 100);
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"PostCropVignetteRoundness"))
    {
      const int v = atoi((char *)value);
      data->crop_roundness = (float)v;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"GrainAmount"))
    {
      const int v = atoi((char *)value);
      if(v != 0)
      {
        data->has_grain = TRUE;
        data->pg.strength = lr2dt_grain_amount((float)v);
      }
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"GrainFrequency"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->pg.scale = lr2dt_grain_frequency((float)v);
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"ParametricShadows"))
    {
      data->ptc_value[0] = atoi((char *)value);
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"ParametricDarks"))
    {
      data->ptc_value[1] = atoi((char *)value);
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"ParametricLights"))
    {
      data->ptc_value[2] = atoi((char *)value);
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"ParametricHighlights"))
    {
      data->ptc_value[3] = atoi((char *)value);
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"ParametricShadowSplit"))
    {
      data->ptc_split[0] = g_ascii_strtod((char *)value, NULL) / 100.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"ParametricMidtoneSplit"))
    {
      data->ptc_split[1] = g_ascii_strtod((char *)value, NULL) / 100.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"ParametricHighlightSplit"))
    {
      data->ptc_split[2] = g_ascii_strtod((char *)value, NULL) / 100.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SaturationAdjustmentRed"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[1][0] = 0.5 + (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SaturationAdjustmentOrange"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[1][1] = 0.5 + (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SaturationAdjustmentYellow"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[1][2] = 0.5 + (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SaturationAdjustmentGreen"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[1][3] = 0.5 + (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SaturationAdjustmentAqua"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[1][4] = 0.5 + (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SaturationAdjustmentBlue"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[1][5] = 0.5 + (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SaturationAdjustmentPurple"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[1][6] = 0.5 + (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SaturationAdjustmentMagenta"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[1][7] = 0.5 + (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"LuminanceAdjustmentRed"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[0][0] = 0.5 + lfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"LuminanceAdjustmentOrange"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[0][1] = 0.5 + lfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"LuminanceAdjustmentYellow"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[0][2] = 0.5 + lfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"LuminanceAdjustmentGreen"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[0][3] = 0.5 + lfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"LuminanceAdjustmentAqua"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[0][4] = 0.5 + lfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"LuminanceAdjustmentBlue"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[0][5] = 0.5 + lfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"LuminanceAdjustmentPurple"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[0][6] = 0.5 + lfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"LuminanceAdjustmentMagenta"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[0][7] = 0.5 + lfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"HueAdjustmentRed"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[2][0] = 0.5 + hfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"HueAdjustmentOrange"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[2][1] = 0.5 + hfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"HueAdjustmentYellow"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[2][2] = 0.5 + hfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"HueAdjustmentGreen"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[2][3] = 0.5 + hfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"HueAdjustmentAqua"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[2][4] = 0.5 + hfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"HueAdjustmentBlue"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[2][5] = 0.5 + hfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"HueAdjustmentPurple"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[2][6] = 0.5 + hfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"HueAdjustmentMagenta"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[2][7] = 0.5 + hfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SplitToningShadowHue"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_splittoning = TRUE;
      data->pst.shadow_hue = (float)v / 360.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SplitToningShadowSaturation"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_splittoning = TRUE;
      data->pst.shadow_saturation = lr2dt_splittoning_saturation(v);
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SplitToningHighlightHue"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_splittoning = TRUE;
      data->pst.highlight_hue = (float)v / 360.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SplitToningHighlightSaturation"))
    {
      const int v = atoi((char *)value);
      if(v != 0) data->has_splittoning = TRUE;
      data->pst.highlight_saturation = lr2dt_splittoning_saturation(v);
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SplitToningBalance"))
    {
      const float v = g_ascii_strtod((char *)value, NULL);
      data->pst.balance = lr2dt_splittoning_balance(v);
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"Clarity2012"))
    {
      const int v = atoi((char *)value);
      if(v != 0)
      {
        data->has_bilat = TRUE;
        data->pbl.detail = lr2dt_clarity((float)v);
      }
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"Rating"))
    {
      const int v = atoi((char *)value);
      if(v != 0)
      {
        data->rating = v;
        data->has_rating = TRUE;
      }
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"GPSLatitudeRef"))
    {
      if(g_str_equal("N", (const char *)value))
      {
        data->lat_ref = 1;
        if(!isnan(data->lat))
        {
          data->lat = (data->lat > 0) ? data->lat : -data->lat;
        }
      }
      else
      {
        data->lat_ref = -1;
        if(!isnan(data->lat))
        {
          data->lat = (data->lat < 0) ? data->lat : -data->lat;
        }
      }
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"GPSLongitudeRef"))
    {
      if(g_str_equal("E", (const char *)value))
      {
        data->lon_ref = 1;
        if(!isnan(data->lon))
        {
          data->lon = (data->lon > 0) ? data->lon : -data->lon;
        }
      }
      else
      {
        data->lon_ref = -1;
        if(!isnan(data->lon))
        {
          data->lon = (data->lon < 0) ? data->lon : -data->lon;
        }
      }
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"GPSLatitude"))
    {
      const double latitude = dt_util_gps_string_to_number((const char *)value);
      if(!isnan(latitude))
      {
        if(!isnan(data->lat_ref))
        {
          data->lat =  ((latitude > 0) == (data->lat_ref > 0)) ? latitude : -latitude;
        }
        else
        {
          data->lat = latitude;
        }
        data->has_gps = TRUE;
      }
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"GPSLongitude"))
    {
      const double longitude = dt_util_gps_string_to_number((const char *)value);
      if(!isnan(longitude))
      {
        if(!isnan(data->lon_ref))
        {
          data->lon =  ((longitude > 0) == (data->lon_ref > 0)) ? longitude : -longitude;
        }
        else
        {
          data->lon = longitude;
        }
        data->has_gps = TRUE;
      }
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"Label"))
    {
      char *v = g_utf8_casefold((char *)value, -1);
      if(!g_strcmp0(v, _("red")))
        data->color = 0;
      else if(!g_strcmp0(v, _("yellow")))
        data->color = 1;
      else if(!g_strcmp0(v, _("green")))
        data->color = 2;
      else if(!g_strcmp0(v, _("blue")))
        data->color = 3;
      else
        // just an else here to catch all other cases as on lightroom one can
        // change the names of labels. So purple and the user's defined labels
        // will be mapped to purple on darktable.
        data->color = 4;

      data->has_colorlabel = TRUE;
      g_free(v);
    }
  }
  if(dev == NULL && (!xmlStrcmp(name, (const xmlChar *)"subject")
                     || !xmlStrcmp(name, (const xmlChar *)"hierarchicalSubject")))
  {
    xmlNodePtr tagNode = node;

    gboolean tag_change = FALSE;
    while(tagNode)
    {
      if(!xmlStrcmp(tagNode->name, (const xmlChar *)"li"))
      {
        xmlChar *cvalue = xmlNodeListGetString(doc, tagNode->xmlChildrenNode, 1);
        guint tagid = 0;
        if(!dt_tag_exists((char *)cvalue, &tagid)) dt_tag_new((char *)cvalue, &tagid);

        if(dt_tag_attach(tagid, imgid, FALSE, FALSE)) tag_change = TRUE;
        data->has_tags = TRUE;
        xmlFree(cvalue);
      }
      tagNode = tagNode->next;
    }
    if(tag_change) DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_TAG_CHANGED);
  }
  else if(dev != NULL && !xmlStrcmp(name, (const xmlChar *)"RetouchAreas"))
  {
    // RetouchAreas has every spot, so it replaces any read from RetouchInfo
    _free_spots(data);
    data->has_retouch_areas = TRUE;
    for(xmlNodePtr li = node; li; li = _rdf_seq_next(li))
      if(!xmlStrcmp(li->name, (const xmlChar *)"li"))
        _read_retouch_area(doc, li, data);
  }
  else if(dev != NULL && !data->has_retouch_areas && !xmlStrcmp(name, (const xmlChar *)"RetouchInfo"))
  {
    // RetouchInfo lists only the circular spots: Lightroom versions that
    // write RetouchAreas list the brushed ones there alone
    for(xmlNodePtr riNode = node; riNode && data->n_spots < RETOUCH_NO_FORMS; riNode = riNode->next)
    {
      if(xmlStrcmp(riNode->name, (const xmlChar *)"li")) continue;

      xmlChar *cvalue = xmlNodeListGetString(doc, riNode->xmlChildrenNode, 1);
      if(!cvalue) continue;

      lr_spot_t *s = &data->spots[data->n_spots];
      const char *startptr = (const char *)cvalue;
      const char *word = NULL;
      if(_read_float(&startptr, "centerX", &s->x)
         && _skip_comma(&startptr)
         && _read_float(&startptr, "centerY", &s->y)
         && _skip_comma(&startptr)
         && _read_float(&startptr, "radius", &s->radius)
         && _skip_comma(&startptr)
         && _read_word(&startptr, "sourceState", &word)
         && _skip_comma(&startptr)
         && _read_float(&startptr, "sourceX", &s->xc)
         && _skip_comma(&startptr)
         && _read_float(&startptr, "sourceY", &s->yc)
         && !isnan(s->x) && !isnan(s->y) && s->radius > 0.0f)
      {
        // without a spotType, heal, Lightroom's default tool
        s->clone = _skip_comma(&startptr)
                   && _read_word(&startptr, "spotType", &word)
                   && g_str_has_prefix(word, "clone");
        s->opacity = 1.0f;
        s->feather = 0.0f;
        s->dabs = NULL;
        s->n_dabs = 0;
        data->n_spots++;
      }
      xmlFree(cvalue);
    }
  }
  else if(dev != NULL && !xmlStrcmp(name, (const xmlChar *)"ToneCurvePV2012"))
  {
    xmlNodePtr tcNode = node;

    while(tcNode)
    {
      if(!xmlStrcmp(tcNode->name, (const xmlChar *)"li"))
      {
        xmlChar *cvalue = xmlNodeListGetString(doc, tcNode->xmlChildrenNode, 1);

        if(sscanf((const char *)cvalue, "%d, %d",
                  &(data->curve_pts[data->n_pts][0]), &(data->curve_pts[data->n_pts][1]))) data->n_pts++;
        xmlFree(cvalue);
      }
      if(data->n_pts == MAX_PTS) break;
      tcNode = tcNode->next;
    }
  }
  else if(dev == NULL && !xmlStrcmp(name, (const xmlChar *)"title"))
  {
    xmlNodePtr ttlNode = node;
    while(ttlNode)
    {
      if(!xmlStrncmp(ttlNode->name, (const xmlChar *)"li", 2))
      {
        xmlChar *cvalue = xmlNodeListGetString(doc, ttlNode->xmlChildrenNode, 1);
        dt_metadata_set_import_lock(imgid, "Xmp.dc.title", (char *)cvalue);
        xmlFree(cvalue);
      }
      ttlNode = ttlNode->next;
    }
  }
  else if(dev == NULL && !xmlStrcmp(name, (const xmlChar *)"description"))
  {
    xmlNodePtr desNode = node;
    while(desNode)
    {
      if(!xmlStrncmp(desNode->name, (const xmlChar *)"li", 2))
      {
        xmlChar *cvalue = xmlNodeListGetString(doc, desNode->xmlChildrenNode, 1);
        dt_metadata_set_import_lock(imgid, "Xmp.dc.description", (char *)cvalue);
        xmlFree(cvalue);
      }
      desNode = desNode->next;
    }
  }
  else if(dev == NULL && !xmlStrcmp(name, (const xmlChar *)"creator"))
  {
    xmlNodePtr creNode = node;
    while(creNode)
    {
      if(!xmlStrncmp(creNode->name, (const xmlChar *)"li", 2))
      {
        xmlChar *cvalue = xmlNodeListGetString(doc, creNode->xmlChildrenNode, 1);
        dt_metadata_set_import_lock(imgid, "Xmp.dc.creator", (char *)cvalue);
        xmlFree(cvalue);
      }
      creNode = creNode->next;
    }
  }
  else if(dev == NULL && !xmlStrcmp(name, (const xmlChar *)"rights"))
  {
    xmlNodePtr rigNode = node;
    while(rigNode)
    {
      if(!xmlStrncmp(rigNode->name, (const xmlChar *)"li", 2))
      {
        xmlChar *cvalue = xmlNodeListGetString(doc, rigNode->xmlChildrenNode, 1);
        dt_metadata_set_import_lock(imgid, "Xmp.dc.rights", (char *)cvalue);
        xmlFree(cvalue);
      }
      rigNode = rigNode->next;
    }
  }
}

/* _has_list returns true if the node contains a list of value */
static int _has_list(const char *name)
{
  return !strcmp(name, "subject")
    || !strcmp(name, "hierarchicalSubject")
    || !strcmp(name, "RetouchInfo")
    || !strcmp(name, "RetouchAreas")
    || !strcmp(name, "ToneCurvePV2012")
    || !strcmp(name, "title")
    || !strcmp(name, "description")
    || !strcmp(name, "creator")
    || !strcmp(name, "publisher")
    || !strcmp(name, "rights");
};

/* handle a specific xpath */
static void _handle_xpath(const dt_develop_t *dev, xmlDoc *doc, const dt_imgid_t imgid, xmlXPathContext *ctx, const xmlChar *xpath, lr_data_t *data)
{
  xmlXPathObject *xpathObj = xmlXPathEvalExpression(xpath, ctx);

  if(xpathObj != NULL)
    {
      const xmlNodeSetPtr xnodes = xpathObj->nodesetval;
      const int n = xnodes->nodeNr;

      for(int k=0; k<n; k++)
        {
          const xmlNode *node = xnodes->nodeTab[k];

          if(_has_list((char *)node->name))
            {
              xmlNodePtr listnode = node->xmlChildrenNode;
              if(listnode) listnode = listnode->next;
              if(listnode) listnode = listnode->xmlChildrenNode;
              if(listnode) listnode = listnode->next;
              if(listnode) _lrop(dev, doc, imgid, node->name, NULL, listnode, data);
            }
          else
            {
              const xmlChar *value = xmlNodeListGetString(doc, node->children, 1);
              _lrop(dev, doc, imgid, node->name, value, NULL, data);
            }
        }

      xmlXPathFreeObject(xpathObj);
    }
}

static inline void flip(float *x, float *y)
{
  const float tmp = *x;
  *x = 1.0 - *y;
  *y = 1.0 - tmp;
}

static inline void swap(float *x, float *y)
{
  const float tmp = *x;
  *x = *y;
  *y = tmp;
}

static inline double rotate_x(const double x, const double y, const double rangle)
{
  return x*cos(rangle) + y*sin(rangle);
}

static inline double rotate_y(const double x, const double y, const double rangle)
{
  return -x*sin(rangle) + y*cos(rangle);
}

static inline void rotate_xy(double *cx, double *cy, const double rangle)
{
  const double x = *cx;
  const double y = *cy;
  *cx = rotate_x(x, y, rangle);
  *cy = rotate_y(x, y, rangle);
}

static inline float round5(const double x)
{
  return round(x * 100000.f) / 100000.f;
}

// Lightroom normalizes to the raw as cropped by rawspeed and masks to the
// whole sensor. Both are in sensor orientation
static void _lr_to_mask(const dt_image_t *img, const float x, const float y, float out[2])
{
  out[0] = (x * (img->width - img->crop_x - img->crop_right) + img->crop_x) / img->width;
  out[1] = (y * (img->height - img->crop_y - img->crop_bottom) + img->crop_y) / img->height;
}

static dt_masks_form_t *_spot_to_brush(const dt_image_t *img, const lr_spot_t *s, const float border,
                                        const float hardness)
{
  dt_masks_form_t *form = dt_masks_create(DT_MASKS_BRUSH | DT_MASKS_CLONE);
  float *pts = g_new(float, 2 * s->n_dabs);
  for(int i = 0; i < s->n_dabs; i++)
    _lr_to_mask(img, s->dabs[2 * i], s->dabs[2 * i + 1], pts + 2 * i);

  // the control points of a catmull-rom spline through the dabs, as
  // _brush_init_ctrl_points() in brush.c sets them for a drawn stroke
  const int last = s->n_dabs - 1;
  for(int i = 0; i <= last; i++)
  {
    dt_masks_point_brush_t *p = calloc(1, sizeof(dt_masks_point_brush_t));
    for(int c = 0; c < 2; c++)
    {
      const float prev = i > 0 ? pts[2 * (i - 1) + c] : 2.0f * pts[c] - pts[2 + c];
      const float next = i < last ? pts[2 * (i + 1) + c] : 2.0f * pts[2 * last + c] - pts[2 * (last - 1) + c];
      p->corner[c] = pts[2 * i + c];
      p->ctrl1[c] = p->corner[c] - (next - prev) / 6.0f;
      p->ctrl2[c] = p->corner[c] + (next - prev) / 6.0f;
      p->border[c] = border;
    }
    p->density = 1.0f;
    p->hardness = hardness;
    p->state = DT_MASKS_POINT_STATE_NORMAL;
    form->points = g_list_append(form->points, p);
  }
  g_free(pts);
  return form;
}

// retouch takes its shapes from the masks history, not from its params: a
// clone group of one circle or brush per spot, recorded under the retouch
// history num
static void _add_retouch_hist(const dt_develop_t *dev, const dt_imgid_t imgid, const lr_data_t *data,
                              char *imported, const size_t imported_len, int *import_count)
{
  const dt_image_t *img = &dev->image_storage;
  const float cw = img->width - img->crop_x - img->crop_right;
  const float ch = img->height - img->crop_y - img->crop_bottom;
  if(cw <= 0.0f || ch <= 0.0f) return;
  // a Lightroom spot radius is relative to the long side, a circle radius or
  // brush border to the short side of the whole sensor
  const float rscale = MAX(cw, ch) / MIN(img->width, img->height);

  dt_iop_retouch_params_v3_t pr = { .algorithm = DT_IOP_RETOUCH_HEAL,
                                 .preview_levels = { -3.0f, 0.0f, 3.0f },
                                 .blur_radius = 10.0f,
                                 .max_heal_iter = 2000 };
  const dt_iop_module_t *retouch = dt_iop_get_module_by_op_priority(dev->iop, "retouch", 0);
  if(retouch && retouch->params_size == sizeof(pr))
    memcpy(&pr, retouch->default_params, sizeof(pr));

  dt_masks_form_t *grp = dt_masks_create(DT_MASKS_GROUP | DT_MASKS_CLONE);
  snprintf(grp->name, sizeof(grp->name), _("group `%s'"), dt_iop_get_localized_name("retouch"));
  GList *forms = NULL;
  int n_circles = 0, n_brushes = 0;

  for(int k = 0; k < data->n_spots; k++)
  {
    const lr_spot_t *s = &data->spots[k];
    const float radius = s->radius * rscale;
    // Lightroom feathers inside the radius and darktable outside it, over
    // a band that measured about half the feather wide
    const float soft = s->feather / 2.0f;
    dt_masks_form_t *form;
    if(s->dabs)
    {
      // a brush clones from relative to its first point, as a stroke does
      // from its first dab
      form = _spot_to_brush(img, s, radius, 1.0f - soft);
      form->functions->set_form_name(form, ++n_brushes);
    }
    else
    {
      form = dt_masks_create(DT_MASKS_CIRCLE | DT_MASKS_CLONE);
      form->functions->set_form_name(form, ++n_circles);
      dt_masks_point_circle_t *circle = calloc(1, sizeof(dt_masks_point_circle_t));
      _lr_to_mask(img, s->x, s->y, circle->center);
      circle->radius = radius * (1.0f - soft);
      circle->border = radius * soft;
      form->points = g_list_append(NULL, circle);
    }
    _lr_to_mask(img, s->xc, s->yc, form->source);
    forms = g_list_append(forms, form);

    // as dt_masks_group_insert_point() adds a retouch shape, but with the
    // spot's opacity rather than the preference for new shapes
    dt_masks_point_group_t *grpt = calloc(1, sizeof(dt_masks_point_group_t));
    grpt->formid = form->formid;
    grpt->parentid = grp->formid;
    grpt->state = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE;
    grpt->opacity = s->opacity;
    grpt->group_opacity = 1.0f;
    grp->points = g_list_append(grp->points, grpt);

    pr.rt_forms[k] = (dt_iop_retouch_form_data_v3_t){ .formid = form->formid,
                                                   .algorithm = s->clone ? DT_IOP_RETOUCH_CLONE
                                                                         : DT_IOP_RETOUCH_HEAL,
                                                   .blur_radius = pr.blur_radius,
                                                   .distort_mode = 2 };
  }
  forms = g_list_append(forms, grp);

  const int32_t num = _add_hist_with_mask(imgid, "retouch", (dt_iop_params_t *)&pr, sizeof(pr), imported,
                                          imported_len, LRDT_RETOUCH_VERSION, import_count, grp->formid);
  for(GList *l = forms; l; l = g_list_next(l))
    dt_masks_write_masks_history_item(imgid, num, l->data);

  g_list_free_full(forms, (GDestroyNotify)dt_masks_free_form);
}

gboolean dt_lightroom_import(dt_imgid_t imgid, dt_develop_t *dev, gboolean iauto)
{
  gboolean refresh_needed = FALSE;
  char imported[256] = { 0 };
  int n_import = 0;                // number of iop imported
  gboolean is_lr = FALSE;

  // Get full pathname
  char *pathname = dt_get_lightroom_xmp(imgid);

  if(!pathname)
  {
    if(!iauto) dt_control_log(_("cannot find Lightroom XMP!"));
    return FALSE;
  }

  // Load LR xmp

  xmlDocPtr doc;
  xmlNodePtr entryNode;

  // Parse xml document

  doc = xmlReadFile(pathname, NULL, 0);

  if(doc == NULL)
  {
    g_free(pathname);
    return FALSE ;
  }

  // Enter first node, xmpmeta

  entryNode = xmlDocGetRootElement(doc);

  if(entryNode == NULL)
  {
    g_free(pathname);
    xmlFreeDoc(doc);
    return FALSE;
  }

  if(xmlStrcmp(entryNode->name, (const xmlChar *)"xmpmeta"))
  {
    if(!iauto) dt_control_log(_("`%s' is not a Lightroom XMP!"), pathname);
    g_free(pathname);
    return FALSE;
  }

  // Check that this is really a Lightroom document

  xmlXPathContextPtr xpathCtx = xmlXPathNewContext(doc);

  if(xpathCtx == NULL)
  {
    g_free(pathname);
    xmlFreeDoc(doc);
    return FALSE;
  }

  xmlXPathRegisterNs(xpathCtx, BAD_CAST "stEvt", BAD_CAST "http://ns.adobe.com/xap/1.0/sType/ResourceEvent#");

  xmlXPathObjectPtr xpathObj = xmlXPathEvalExpression((const xmlChar *)"//@stEvt:softwareAgent", xpathCtx);

  if(xpathObj == NULL)
  {
    if(!iauto) dt_control_log(_("`%s' is not a Lightroom XMP!"), pathname);
    xmlXPathFreeContext(xpathCtx);
    g_free(pathname);
    xmlFreeDoc(doc);
    return FALSE;
  }

  xmlNodeSetPtr xnodes = xpathObj->nodesetval;

  if(xnodes != NULL && xnodes->nodeNr > 0)
  {
    xmlNodePtr xnode = xnodes->nodeTab[0];
    xmlChar *value = xmlNodeListGetString(doc, xnode->xmlChildrenNode, 1);

    if(!strstr((char *)value, "Lightroom") && !strstr((char *)value, "Camera Raw"))
    {
      xmlXPathFreeContext(xpathCtx);
      xmlXPathFreeObject(xpathObj);
      xmlFreeDoc(doc);
      xmlFree(value);
      if(!iauto) dt_control_log(_("`%s' is not a Lightroom XMP!"), pathname);
      g_free(pathname);
      return FALSE;
    }
    is_lr = TRUE;
    xmlFree(value);
  }
// we could bail out here if we ONLY wanted to load a file known to be from lightroom.
// if we don't know who created it we will just import it however.
//   else
//   {
//     xmlXPathFreeObject(xpathObj);
//     xmlXPathFreeContext(xpathCtx);
//     if(!iauto) dt_control_log(_("`%s' is not a Lightroom XMP!"), pathname);
//     g_free(pathname);
//     return;
//   }

  // let's now parse the needed data

  lr_data_t data;

  memset(&data, 0, sizeof(data));

  data.has_crop = FALSE;
  data.has_flip = FALSE;
  data.has_exposure = FALSE;
  data.has_vignette = FALSE;
  data.has_grain = FALSE;
  data.n_pts = 0;
  // Lightroom's defaults, which it leaves out of the XMP
  data.ptc_split[0] = 0.25f;
  data.ptc_split[1] = 0.50f;
  data.ptc_split[2] = 0.75f;
  data.has_colorzones = FALSE;
  data.has_splittoning = FALSE;
  data.pst.balance = lr2dt_splittoning_balance(0.0f);
  data.has_bilat = FALSE;
  data.has_tags = FALSE;
  data.rating = 0;
  data.has_rating = FALSE;
  data.lat = NAN;
  data.lon = NAN;
  data.lat_ref = NAN;
  data.lon_ref = NAN;
  data.has_gps = FALSE;
  data.color = 0;
  data.has_colorlabel = FALSE;
  data.fratio = NAN;                // factor ratio image
  data.crop_roundness = NAN;        // from lightroom
  data.vignette_midpoint = 50.0f;   // Lightroom's defaults
  data.vignette_feather = 50.0f;
  data.iwidth = 0;
  data.iheight = 0;                 // image width / height
  data.orientation = EXIF_ORIENTATION_NONE;

  // record the name-spaces needed for the parsing
  xmlXPathRegisterNs
    (xpathCtx,
     BAD_CAST "crs",
     BAD_CAST "http://ns.adobe.com/camera-raw-settings/1.0/");
  xmlXPathRegisterNs
    (xpathCtx,
     BAD_CAST "dc",
     BAD_CAST "http://purl.org/dc/elements/1.1/");
  xmlXPathRegisterNs
    (xpathCtx,
     BAD_CAST "tiff",
     BAD_CAST "http://ns.adobe.com/tiff/1.0/");
  xmlXPathRegisterNs
    (xpathCtx,
     BAD_CAST "xmp",
     BAD_CAST "http://ns.adobe.com/xap/1.0/");
  xmlXPathRegisterNs
    (xpathCtx,
     BAD_CAST "exif",
     BAD_CAST "http://ns.adobe.com/exif/1.0/");
  xmlXPathRegisterNs
    (xpathCtx,
     BAD_CAST "lr",
     BAD_CAST "http://ns.adobe.com/lightroom/1.0/");
  xmlXPathRegisterNs
    (xpathCtx,
     BAD_CAST "rdf",
     BAD_CAST "http://www.w3.org/1999/02/22-rdf-syntax-ns#");

  // All prefixes to parse from the XMP document
  static char *names[] = { "crs", "dc", "tiff", "xmp", "exif", "lr", NULL };

  for(int i=0; names[i]!=NULL; i++)
    {
      char expr[50];

      /* Lr 7.0 CC (nodes) */
      snprintf(expr, sizeof(expr), "//%s:*", names[i]);
      _handle_xpath(dev, doc, imgid, xpathCtx, (const xmlChar *)expr, &data);

      /* Lr up to 6.0 (attributes) */
      snprintf(expr, sizeof(expr), "//@%s:*", names[i]);
      _handle_xpath(dev, doc, imgid, xpathCtx, (const xmlChar *)expr, &data);
    }

  xmlXPathFreeObject(xpathObj);
  xmlXPathFreeContext(xpathCtx);
  xmlFreeDoc(doc);

  //  Integrates into the history all the imported iop

  if(dev != NULL && is_lr && dt_image_is_raw(&dev->image_storage))
  {
    // the standard color matrix is Adobe's, and Lightroom works in linear ProPhoto RGB
    const dt_iop_colorin_params_v7_t pci = { .type = DT_COLORSPACE_STANDARD_MATRIX,
                                          .intent = DT_INTENT_PERCEPTUAL,
                                          .normalize = DT_NORMALIZE_OFF,
                                          .type_work = DT_COLORSPACE_PROPHOTO_RGB };

    dt_add_hist(imgid, "colorin", (dt_iop_params_t *)&pci, sizeof(dt_iop_colorin_params_v7_t), imported,
                sizeof(imported), LRDT_COLORIN_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && data.has_crop)
  {
    double rangle;
    double cx, cw, cy, ch;
    double new_width, new_height;
    dt_image_orientation_t orientation = dt_image_orientation_to_flip_bits(data.orientation);

    // ashift runs before flip in the pipe, so it rotates in sensor
    // orientation, the same space Lightroom's CropAngle is expressed in
    if(data.pas.rotation != 0.0f)
    {
      data.pas.f_length = 28.0f;
      data.pas.crop_factor = 1.0f;
      data.pas.orthocorr = 100.0f;
      data.pas.aspect = 1.0f;
      data.pas.mode = ASHIFT_MODE_GENERIC;
      data.pas.cropmode = ASHIFT_CROP_OFF; // the crop module below does the cropping
      data.pas.cl = data.pas.ct = 0.0f;
      data.pas.cr = data.pas.cb = 1.0f;

      dt_add_hist(imgid, "ashift", (dt_iop_params_t *)&data.pas, sizeof(dt_iop_ashift_params_v5_t), imported,
                  sizeof(imported), LRDT_ASHIFT_VERSION, &n_import);
    }

    data.pcr.ratio_n = data.pcr.ratio_d = 0; // freehand

    // Convert image in image-centered coordinate system, [-image_size / 2; + image_size / 2]
    cx = (data.pcr.cx - 0.5f) * data.iwidth;
    cw = (data.pcr.cw - 0.5f) * data.iwidth;
    cy = (data.pcr.cy - 0.5f) * data.iheight;
    ch = (data.pcr.ch - 0.5f) * data.iheight;

    // Rotate the cropped zone according to rotation angle
    // All rotations done using center of the image
    rangle = deg2rad(-data.pas.rotation);
    rotate_xy(&cx, &cy, -rangle);
    rotate_xy(&cw, &ch, -rangle);

    // Calculate the new overall image size (black zone included) after rotation
    // rangle is limited to -45°;+45° by LR
    new_width  = rotate_x(+data.iwidth, -data.iheight, -fabs(rangle));
    new_height = rotate_y(+data.iwidth, +data.iheight, -fabs(rangle));

    // apply new size & convert image back in initial coordinate system [0.0 ; +1.0]
    data.pcr.cx = round5((cx / new_width)  + 0.5f);
    data.pcr.cw = round5((cw / new_width)  + 0.5f);
    data.pcr.cy = round5((cy / new_height) + 0.5f);
    data.pcr.ch = round5((ch / new_height) + 0.5f);

    // adjust crop data according to the orientation - Must be done after rotation
    if(orientation & ORIENTATION_FLIP_X)
      flip(&data.pcr.cx, &data.pcr.cw);
    if(orientation & ORIENTATION_FLIP_Y)
      flip(&data.pcr.cy, &data.pcr.ch);
    if(orientation & ORIENTATION_SWAP_XY)
    {
      swap(&data.pcr.cx, &data.pcr.cy);
      swap(&data.pcr.cw, &data.pcr.ch);
    }

    data.fratio = (data.pcr.cw - data.pcr.cx) / (data.pcr.ch - data.pcr.cy);

    dt_add_hist(imgid, "crop", (dt_iop_params_t *)&data.pcr, sizeof(dt_iop_crop_params_v3_t), imported,
                sizeof(imported), LRDT_CROP_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && data.has_flip)
  {
    data.pf.orientation = dt_image_orientation_to_flip_bits(data.orientation);

    dt_add_hist(imgid, "flip", (dt_iop_params_t *)&data.pf, sizeof(dt_iop_flip_params_v2_t), imported,
                sizeof(imported), LRDT_FLIP_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && data.has_exposure)
  {
    // Lightroom's adjustments are relative to its default rendering, so add
    // them to the module's defaults for this image, which in the
    // scene-referred workflow hold the baseline boost and bias compensation
    dt_iop_exposure_params_v7_t pe = { .mode = EXPOSURE_MODE_MANUAL,
                                    .deflicker_percentile = 50.0f,
                                    .deflicker_target_level = -4.0f,
                                    .compensate_hilite_pres = TRUE };
    const dt_iop_module_t *exposure =
      dt_iop_get_module_by_op_priority(dev->iop, "exposure", 0);
    if(exposure && exposure->params_size == sizeof(pe))
      memcpy(&pe, exposure->default_params, sizeof(pe));

    pe.exposure += data.pe.exposure;
    pe.black += data.pe.black;

    dt_add_hist(imgid, "exposure", (dt_iop_params_t *)&pe, sizeof(dt_iop_exposure_params_v7_t), imported,
                sizeof(imported), LRDT_EXPOSURE_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && data.has_grain)
  {
    data.pg.channel = 0;

    dt_add_hist(imgid, "grain", (dt_iop_params_t *)&data.pg, sizeof(dt_iop_grain_params_v1_t), imported,
                sizeof(imported), LRDT_GRAIN_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && data.has_vignette)
  {
    const float base_ratio = 1.325 / 1.5;

    data.pv.autoratio = FALSE;
    data.pv.dithering = DITHER_8BIT;
    data.pv.center.x = 0.0;
    data.pv.center.y = 0.0;
    data.pv.shape = 1.0;
    data.pv.falloff_scale = lr2dt_vignette_falloff(data.vignette_feather);
    // each step of the midpoint moves the falloff out by 0.576%
    data.pv.scale = lr2dt_vignette_center(data.vignette_feather)
                    + 0.576f * (data.vignette_midpoint - 50.0f) - data.pv.falloff_scale / 2.0f;

    // defensive code, should not happen, but just in case future Lr version
    // has not ImageWidth/ImageLength XML tag.
    if(data.iwidth == 0 || data.iheight == 0)
      data.pv.whratio = base_ratio;
    else
      data.pv.whratio = base_ratio * ((float)data.iwidth / (float)data.iheight);

    if(data.has_crop) data.pv.whratio = data.pv.whratio * data.fratio;

    //  Adjust scale and ratio based on the roundness. On Lightroom changing
    //  the roundness change the width and the height of the vignette.

    if(data.crop_roundness > 0)
    {
      float newratio = data.pv.whratio - (data.pv.whratio - 1) * (data.crop_roundness / 100.0);
      float dscale = (1 - (newratio / data.pv.whratio)) / 2.0;

      data.pv.scale -= dscale * 100.0;
      data.pv.whratio = newratio;
    }

    dt_add_hist(imgid, "vignette", (dt_iop_params_t *)&data.pv, sizeof(dt_iop_vignette_params_v3_t), imported,
                sizeof(imported), LRDT_VIGNETTE_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && data.n_spots > 0)
  {
    _add_retouch_hist(dev, imgid, &data, imported, sizeof(imported), &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && (data.n_pts >= 2 || data.ptc_value[0] != 0 || data.ptc_value[1] != 0
                     || data.ptc_value[2] != 0 || data.ptc_value[3] != 0))
  {
    data.ptc.tonecurve_nodes[ch_a] = 7;
    data.ptc.tonecurve_nodes[ch_b] = 7;
    data.ptc.tonecurve_type[ch_L] = CUBIC_SPLINE;
    data.ptc.tonecurve_type[ch_a] = CUBIC_SPLINE;
    data.ptc.tonecurve_type[ch_b] = CUBIC_SPLINE;
    data.ptc.tonecurve_autoscale_ab = LRDT_TONECURVE_AUTOMATIC_RGB;
    data.ptc.tonecurve_preset = 0;
    data.ptc.tonecurve_unbound_ab = 1;
    data.ptc.preserve_colors = LRDT_RGB_NORM_AVERAGE;

    float linear_ab[7] = { 0.0, 0.08, 0.3, 0.5, 0.7, 0.92, 1.0 };

    // linear a, b curves
    for(int k = 0; k < 7; k++) data.ptc.tonecurve[ch_a][k].x = linear_ab[k];
    for(int k = 0; k < 7; k++) data.ptc.tonecurve[ch_a][k].y = linear_ab[k];
    for(int k = 0; k < 7; k++) data.ptc.tonecurve[ch_b][k].x = linear_ab[k];
    for(int k = 0; k < 7; k++) data.ptc.tonecurve[ch_b][k].y = linear_ab[k];

    // Lightroom writes the point curve for its presets too, Linear included
    float px[MAX_PTS] = { 0.0f, 1.0f };
    float py[MAX_PTS] = { 0.0f, 1.0f };
    // a single point is no curve, so keep the identity rather than half of it
    const int n_pts = data.n_pts >= 2 ? data.n_pts : 2;
    for(int k = 0; data.n_pts >= 2 && k < n_pts; k++)
    {
      px[k] = data.curve_pts[k][0] / 255.0f;
      py[k] = data.curve_pts[k][1] / 255.0f;
    }

    const gboolean parametric = data.ptc_value[0] != 0 || data.ptc_value[1] != 0
                                || data.ptc_value[2] != 0 || data.ptc_value[3] != 0;

    if(!parametric)
    {
      data.ptc.tonecurve_nodes[ch_L] = n_pts;
      for(int k = 0; k < n_pts; k++)
      {
        data.ptc.tonecurve[ch_L][k].x = px[k];
        data.ptc.tonecurve[ch_L][k].y = py[k];
      }
    }
    else
    {
      // Lightroom applies the point curve on top of the parametric one;
      // sample the two together, as no handful of nodes follows both
      float *tangents = interpolate_set(n_pts, px, py, CUBIC_SPLINE);
      data.ptc.tonecurve_nodes[ch_L] = DT_IOP_TONECURVE_MAXNODES;
      for(int k = 0; k < DT_IOP_TONECURVE_MAXNODES; k++)
      {
        const float x = (float)k / (DT_IOP_TONECURVE_MAXNODES - 1);
        const float y = _lr_parametric_curve(x, data.ptc_split, data.ptc_value);
        data.ptc.tonecurve[ch_L][k].x = x;
        data.ptc.tonecurve[ch_L][k].y
            = tangents ? CLAMP(interpolate_val(n_pts, px, y, py, tangents, CUBIC_SPLINE), 0.0f, 1.0f) : y;
      }
      free(tangents);
    }

    if(!_tonecurve_is_identity(&data.ptc))
    {
      dt_add_hist(imgid, "tonecurve", (dt_iop_params_t *)&data.ptc,
                  sizeof(dt_iop_tonecurve_params_v5_t), imported, sizeof(imported),
                  LRDT_TONECURVE_VERSION, &n_import);
      refresh_needed = TRUE;
    }
  }

  if(dev != NULL && data.has_colorzones)
  {
    data.pcz.channel = DT_IOP_COLORZONES_h;

    for(int i = 0; i < 3; i++)
      for(int k = 0; k < 8; k++)
        data.pcz.equalizer_x[i][k] = k / (DT_IOP_COLORZONES_BANDS - 1.0);

    dt_add_hist(imgid, "colorzones", (dt_iop_params_t *)&data.pcz, sizeof(dt_iop_colorzones_params_v2_t), imported,
                sizeof(imported), LRDT_COLORZONES_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && data.has_splittoning)
  {
    // Lightroom leaves no untoned band between shadows and highlights
    data.pst.compress = 0.0;

    dt_add_hist(imgid, "splittoning", (dt_iop_params_t *)&data.pst, sizeof(dt_iop_splittoning_params_v1_t), imported,
                sizeof(imported), LRDT_SPLITTONING_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && data.has_bilat)
  {
    // shadows and highlights at 100% leave all but the midtone local
    // contrast alone, which is what Lightroom's clarity changes
    data.pbl.mode = s_mode_local_laplacian;
    data.pbl.sigma_r = 1.0f;
    data.pbl.sigma_s = 1.0f;
    data.pbl.midtone = 0.5f;

    dt_add_hist(imgid, "bilat", (dt_iop_params_t *)&data.pbl, sizeof(dt_iop_bilat_params_v3_t), imported,
                sizeof(imported), LRDT_BILAT_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(data.has_tags)
  {
    if(imported[0]) g_strlcat(imported, ", ", sizeof(imported));
    g_strlcat(imported, _("tags"), sizeof(imported));
    n_import++;
  }

  if(dev == NULL && data.has_rating)
  {
    dt_ratings_apply_on_image(imgid, data.rating, FALSE, FALSE, FALSE);

    if(imported[0]) g_strlcat(imported, ", ", sizeof(imported));
    g_strlcat(imported, _("rating"), sizeof(imported));
    n_import++;
  }

  if(dev == NULL && data.has_gps)
  {
    dt_image_geoloc_t geoloc;
    geoloc.longitude = data.lon;
    geoloc.latitude = data.lat;
    geoloc.elevation = NAN;
    dt_image_set_location(imgid, &geoloc, FALSE, FALSE);
    GList *imgs = NULL;
    imgs = g_list_prepend(imgs, GINT_TO_POINTER(imgid));
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_GEOTAG_CHANGED, imgs, 0);

    if(imported[0]) g_strlcat(imported, ", ", sizeof(imported));
    g_strlcat(imported, _("geotagging"), sizeof(imported));
    n_import++;
  }

  if(dev == NULL && data.has_colorlabel)
  {
    dt_colorlabels_set_label(imgid, data.color);

    if(imported[0]) g_strlcat(imported, ", ", sizeof(imported));
    g_strlcat(imported, _("color label"), sizeof(imported));
    n_import++;
  }

  if(dev != NULL && refresh_needed && dev->gui_attached)
  {
    dt_control_log(ngettext("%s has been imported", "%s have been imported", n_import), imported);

    if(!iauto)
    {
      /* signal history changed */
      dt_dev_reload_history_items(dev);
      dt_dev_modulegroups_set(darktable.develop, dt_dev_modulegroups_get(darktable.develop));
      /* update xmp file */
      dt_image_synch_xmp(imgid);
      DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
    }
  }

  _free_spots(&data);
  return TRUE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
