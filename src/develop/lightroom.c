/*
    This file is part of darktable,
    Copyright (C) 2013-2022 darktable developers.

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
#include "common/iop_order.h"
#include "common/ratings.h"
#include "common/tags.h"
#include "common/metadata.h"
#include "control/control.h"

#include <ctype.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// copy here the iop params struct with the actual version. This is so to
// be as independent as possible of any iop evolutions. Indeed, we create
// the iop params into the database for a specific version. We then ask
// for a reload of the history parameter. If the iop has evolved since then
// the legacy circuitry will be called to convert the parameters.
//
// to add a new iop:
// 1. copy the struct
// 2. add LRDT_<iop_name>_VERSION with corresponding module version
// 3. use this version to pass in dt_add_hist()

#define LRDT_CLIPPING_VERSION 5
typedef struct dt_iop_clipping_params_t
{
  float angle, cx, cy, cw, ch, k_h, k_v;
  float kxa, kya, kxb, kyb, kxc, kyc, kxd, kyd;
  int k_type, k_sym;
  int k_apply, crop_auto;
  int ratio_n, ratio_d;
} dt_iop_clipping_params_t;

#define LRDT_FLIP_VERSION 2
typedef struct dt_iop_flip_params_t
{
  dt_image_orientation_t orientation;
} dt_iop_flip_params_t;

#define LRDT_EXPOSURE_VERSION 2
typedef struct dt_iop_exposure_params_t
{
  float black, exposure, gain;
} dt_iop_exposure_params_t;

#define LRDT_GRAIN_VERSION 1
typedef enum _dt_iop_grain_channel_t
{
  DT_GRAIN_CHANNEL_HUE = 0,
  DT_GRAIN_CHANNEL_SATURATION,
  DT_GRAIN_CHANNEL_LIGHTNESS,
  DT_GRAIN_CHANNEL_RGB
} _dt_iop_grain_channel_t;

typedef struct dt_iop_grain_params_t
{
  _dt_iop_grain_channel_t channel;
  float scale;
  float strength;
} dt_iop_grain_params_t;

typedef enum dt_iop_dither_t
{
  DITHER_OFF = 0,
  DITHER_8BIT = 1,
  DITHER_16BIT = 2
} dt_iop_dither_t;

typedef struct dt_iop_fvector_2d_t
{
  float x;
  float y;
} dt_iop_vector_2d_t;

#define LRDT_VIGNETTE_VERSION 3
typedef struct dt_iop_vignette_params_t
{
  float scale;               // 0 - 100 Inner radius, percent of largest image dimension
  float falloff_scale;       // 0 - 100 Radius for falloff -- outer radius = inner radius + falloff_scale
  float brightness;          // -1 - 1 Strength of brightness reduction
  float saturation;          // -1 - 1 Strength of saturation reduction
  dt_iop_vector_2d_t center; // Center of vignette
  gboolean autoratio;        //
  float whratio;             // 0-1 = width/height ratio, 1-2 = height/width ratio + 1
  float shape;
  int dithering; // if and how to perform dithering
} dt_iop_vignette_params_t;

#define LRDT_SPOTS_VERSION 1
#define MAX_SPOTS 32

typedef struct spot_t
{
  // position of the spot
  float x, y;
  // position to clone from
  float xc, yc;
  float radius;
} spot_t;

typedef struct dt_iop_spots_params_t
{
  int num_spots;
  spot_t spot[MAX_SPOTS];
} dt_iop_spots_params_t;

#define LRDT_TONECURVE_VERSION 3
#define DT_IOP_TONECURVE_MAXNODES 20
typedef enum tonecurve_channel_t
{
  ch_L = 0,
  ch_a = 1,
  ch_b = 2,
  ch_max = 3
} tonecurve_channel_t;

typedef struct dt_iop_tonecurve_node_t
{
  float x;
  float y;
} dt_iop_tonecurve_node_t;

typedef struct dt_iop_tonecurve_params_t
{
  dt_iop_tonecurve_node_t tonecurve[3][DT_IOP_TONECURVE_MAXNODES]; // three curves (L, a, b) with max number
                                                                   // of nodes
  int tonecurve_nodes[3];
  int tonecurve_type[3];
  int tonecurve_autoscale_ab;
  int tonecurve_preset;
} dt_iop_tonecurve_params_t;

#define LRDT_COLORZONES_VERSION 2
#define DT_IOP_COLORZONES_BANDS 8

typedef enum dt_iop_colorzones_channel_t
{
  DT_IOP_COLORZONES_L = 0,
  DT_IOP_COLORZONES_C = 1,
  DT_IOP_COLORZONES_h = 2
} dt_iop_colorzones_channel_t;

typedef struct dt_iop_colorzones_params_t
{
  int32_t channel;
  float equalizer_x[3][DT_IOP_COLORZONES_BANDS], equalizer_y[3][DT_IOP_COLORZONES_BANDS];
} dt_iop_colorzones_params_t;

#define LRDT_SPLITTONING_VERSION 1
typedef struct dt_iop_splittoning_params_t
{
  float shadow_hue;
  float shadow_saturation;
  float highlight_hue;
  float highlight_saturation;
  float balance;  // center luminance of gradient
  float compress; // Compress range
} dt_iop_splittoning_params_t;

#define LRDT_BILAT_VERSION 1
typedef struct dt_iop_bilat_params_t
{
  float sigma_r;
  float sigma_s;
  float detail;
} dt_iop_bilat_params_t;


#define LRDT_COLORIN_VERSION 1
#define DT_IOP_COLOR_ICC_LEN_V1 100

typedef struct dt_iop_colorin_params_v1_t
{
  char iccprofile[DT_IOP_COLOR_ICC_LEN_V1];
  dt_iop_color_intent_t intent;
} dt_iop_colorin_params_v1_t;

#undef DT_IOP_COLOR_ICC_LEN_V1

//
// end of iop structs
//

// the blend params for Lr import, not used in this mode (mode=0), as for iop generate the blend params for
// the
// version specified above.

#define LRDT_BLEND_VERSION 4
#define DEVELOP_BLENDIF_SIZE 16

typedef struct dt_lr_develop_blend_params_t
{
  /** blending mode */
  uint32_t mode;
  /** mixing opacity */
  float opacity;
  /** id of mask in current pipeline */
  uint32_t mask_id;
  /** blendif mask */
  uint32_t blendif;
  /** blur radius */
  float radius;
  /** blendif parameters */
  float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
} dt_lr_develop_blend_params_t;

//
// end of blend_params
//

typedef struct lr2dt
{
  float lr, dt;
} lr2dt_t;

char *dt_get_lightroom_xmp(dt_imgid_t imgid)
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

static float get_interpolate(lr2dt_t lr2dt_table[], float value)
{
  int k = 0;

  while(lr2dt_table[k + 1].lr < value) k++;

  return lr2dt_table[k].dt
         + ((value - lr2dt_table[k].lr) / (lr2dt_table[k + 1].lr - lr2dt_table[k].lr))
           * (lr2dt_table[k + 1].dt - lr2dt_table[k].dt);
}

static float lr2dt_blacks(float value)
{
  lr2dt_t lr2dt_blacks_table[]
      = { { -100, 0.020 }, { -50, 0.005 }, { 0, 0 }, { 50, -0.005 }, { 100, -0.010 } };

  return get_interpolate(lr2dt_blacks_table, value);
}

static float lr2dt_vignette_gain(float value)
{
  lr2dt_t lr2dt_vignette_table[] = { { -100, -1 }, { -50, -0.7 }, { 0, 0 }, { 50, 0.5 }, { 100, 1 } };

  return get_interpolate(lr2dt_vignette_table, value);
}

static float lr2dt_vignette_midpoint(float value)
{
  lr2dt_t lr2dt_vignette_table[] = { { 0, 74 }, { 4, 75 }, { 25, 85 }, { 50, 100 }, { 100, 100 } };

  return get_interpolate(lr2dt_vignette_table, value);
}

static float lr2dt_grain_amount(float value)
{
  lr2dt_t lr2dt_grain_table[] = { { 0, 0 }, { 25, 20 }, { 50, 40 }, { 100, 80 } };

  return get_interpolate(lr2dt_grain_table, value);
}

static float lr2dt_grain_frequency(float value)
{
  lr2dt_t lr2dt_grain_table[] = { { 0, 100 }, { 50, 100 }, { 75, 400 }, { 100, 800 } };

  return get_interpolate(lr2dt_grain_table, value) / 53.3;
}

static float lr2dt_splittoning_balance(float value)
{
  lr2dt_t lr2dt_splittoning_table[] = { { -100, 100 }, { 0, 0 }, { 100, 0 } };

  return get_interpolate(lr2dt_splittoning_table, value);
}

static float lr2dt_clarity(float value)
{
  lr2dt_t lr2dt_clarity_table[] = { { -100, -.650 }, { 0, 0 }, { 100, .650 } };

  return get_interpolate(lr2dt_clarity_table, value);
}

static void dt_add_hist(dt_imgid_t imgid, char *operation, dt_iop_params_t *params, int params_size, char *imported,
                        size_t imported_len, int version, int *import_count)
{
  int32_t num = 0;
  dt_lr_develop_blend_params_t blend_params = { 0 };

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
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 6, &blend_params, sizeof(dt_lr_develop_blend_params_t), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, LRDT_BLEND_VERSION);

  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // also bump history_end
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.images"
                              " SET history_end = (SELECT IFNULL(MAX(num) + 1, 0)"
                              "                    FROM main.history"
                              "                    WHERE imgid = ?1)"
                              " WHERE id = ?1", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if(imported[0]) g_strlcat(imported, ", ", imported_len);
  g_strlcat(imported, dt_iop_get_localized_name(operation), imported_len);
  (*import_count)++;
}

#define MAX_PTS 20

typedef enum lr_curve_kind_t
{
  linear = 0,
  medium_contrast = 1,
  strong_contrast = 2,
  custom = 3
} lr_curve_kind_t;

typedef struct lr_data_t
{
  dt_iop_clipping_params_t pc;
  gboolean has_crop;

  dt_iop_flip_params_t pf;
  gboolean has_flip;

  dt_iop_exposure_params_t pe;
  gboolean has_exposure;

  dt_iop_vignette_params_t pv;
  gboolean has_vignette;

  dt_iop_grain_params_t pg;
  gboolean has_grain;

  dt_iop_spots_params_t ps;
  gboolean has_spots;

  dt_iop_tonecurve_params_t ptc;
  int ptc_value[4];
  float ptc_split[3];
  lr_curve_kind_t curve_kind;
  int curve_pts[MAX_PTS][2];
  int n_pts;

  dt_iop_colorzones_params_t pcz;
  gboolean has_colorzones;

  dt_iop_splittoning_params_t pst;
  gboolean has_splittoning;

  dt_iop_bilat_params_t pbl;
  gboolean has_bilat;

  gboolean has_tags;

  int rating;
  gboolean has_rating;

  gdouble lat, lon;
  gboolean has_gps;

  int color;
  gboolean has_colorlabel;

  float fratio;                // factor ratio image
  float crop_roundness;        // from lightroom
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

static gboolean _skip_key_value_pair(const char **startptr, const char *key)
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
  while((*iter >= 'a' && *iter <= 'z') || (*iter >= 'A' && *iter <= 'Z')) iter++;
  *startptr = iter;
  return TRUE;
}

static gboolean _skip_comma(const char **startptr)
{
  return *(*startptr)++ == ',';
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
      data->pc.cy = g_ascii_strtod((char *)value, NULL);
    else if(!xmlStrcmp(name, (const xmlChar *)"CropRight"))
      data->pc.cw = g_ascii_strtod((char *)value, NULL);
    else if(!xmlStrcmp(name, (const xmlChar *)"CropLeft"))
      data->pc.cx = g_ascii_strtod((char *)value, NULL);
    else if(!xmlStrcmp(name, (const xmlChar *)"CropBottom"))
      data->pc.ch = g_ascii_strtod((char *)value, NULL);
    else if(!xmlStrcmp(name, (const xmlChar *)"CropAngle"))
      data->pc.angle = -g_ascii_strtod((char *)value, NULL);
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
      int v = atoi((char *)value);
      if(v != 0)
      {
        data->has_exposure = TRUE;
        data->pe.black = lr2dt_blacks((float)v);
      }
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"Exposure2012"))
    {
      float v = g_ascii_strtod((char *)value, NULL);
      if(v != 0.0)
      {
        data->has_exposure = TRUE;
        data->pe.exposure = v;
      }
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"PostCropVignetteAmount"))
    {
      int v = atoi((char *)value);
      if(v != 0)
      {
        data->has_vignette = TRUE;
        data->pv.brightness = lr2dt_vignette_gain((float)v);
      }
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"PostCropVignetteMidpoint"))
    {
      int v = atoi((char *)value);
      data->pv.scale = lr2dt_vignette_midpoint((float)v);
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"PostCropVignetteStyle"))
    {
      int v = atoi((char *)value);
      if(v == 1) // Highlight Priority
        data->pv.saturation = -0.300;
      else // Color Priority & Paint Overlay
        data->pv.saturation = -0.200;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"PostCropVignetteFeather"))
    {
      int v = atoi((char *)value);
      if(v != 0) data->pv.falloff_scale = (float)v;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"PostCropVignetteRoundness"))
    {
      int v = atoi((char *)value);
      data->crop_roundness = (float)v;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"GrainAmount"))
    {
      int v = atoi((char *)value);
      if(v != 0)
      {
        data->has_grain = TRUE;
        data->pg.strength = lr2dt_grain_amount((float)v);
      }
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"GrainFrequency"))
    {
      int v = atoi((char *)value);
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
    else if(!xmlStrcmp(name, (const xmlChar *)"ToneCurveName2012"))
    {
      if(!xmlStrcmp(value, (const xmlChar *)"Linear"))
        data->curve_kind = linear;
      else if(!xmlStrcmp(value, (const xmlChar *)"Medium Contrast"))
        data->curve_kind = medium_contrast;
      else if(!xmlStrcmp(value, (const xmlChar *)"Strong Contrast"))
        data->curve_kind = strong_contrast;
      else if(!xmlStrcmp(value, (const xmlChar *)"Custom"))
        data->curve_kind = custom;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SaturationAdjustmentRed"))
    {
      int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[1][0] = 0.5 + (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SaturationAdjustmentOrange"))
    {
      int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[1][1] = 0.5 + (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SaturationAdjustmentYellow"))
    {
      int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[1][2] = 0.5 + (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SaturationAdjustmentGreen"))
    {
      int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[1][3] = 0.5 + (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SaturationAdjustmentAqua"))
    {
      int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[1][4] = 0.5 + (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SaturationAdjustmentBlue"))
    {
      int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[1][5] = 0.5 + (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SaturationAdjustmentPurple"))
    {
      int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[1][6] = 0.5 + (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SaturationAdjustmentMagenta"))
    {
      int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[1][7] = 0.5 + (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"LuminanceAdjustmentRed"))
    {
      int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[0][0] = 0.5 + lfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"LuminanceAdjustmentOrange"))
    {
      int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[0][1] = 0.5 + lfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"LuminanceAdjustmentYellow"))
    {
      int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[0][2] = 0.5 + lfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"LuminanceAdjustmentGreen"))
    {
      int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[0][3] = 0.5 + lfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"LuminanceAdjustmentAqua"))
    {
      int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[0][4] = 0.5 + lfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"LuminanceAdjustmentBlue"))
    {
      int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[0][5] = 0.5 + lfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"LuminanceAdjustmentPurple"))
    {
      int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[0][6] = 0.5 + lfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"LuminanceAdjustmentMagenta"))
    {
      int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[0][7] = 0.5 + lfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"HueAdjustmentRed"))
    {
      int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[2][0] = 0.5 + hfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"HueAdjustmentOrange"))
    {
      int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[2][1] = 0.5 + hfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"HueAdjustmentYellow"))
    {
      int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[2][2] = 0.5 + hfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"HueAdjustmentGreen"))
    {
      int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[2][3] = 0.5 + hfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"HueAdjustmentAqua"))
    {
      int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[2][4] = 0.5 + hfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"HueAdjustmentBlue"))
    {
      int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[2][5] = 0.5 + hfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"HueAdjustmentPurple"))
    {
      int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[2][6] = 0.5 + hfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"HueAdjustmentMagenta"))
    {
      int v = atoi((char *)value);
      if(v != 0) data->has_colorzones = TRUE;
      data->pcz.equalizer_y[2][7] = 0.5 + hfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SplitToningShadowHue"))
    {
      int v = atoi((char *)value);
      if(v != 0) data->has_splittoning = TRUE;
      data->pst.shadow_hue = (float)v / 255.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SplitToningShadowSaturation"))
    {
      int v = atoi((char *)value);
      if(v != 0) data->has_splittoning = TRUE;
      data->pst.shadow_saturation = (float)v / 100.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SplitToningHighlightHue"))
    {
      int v = atoi((char *)value);
      if(v != 0) data->has_splittoning = TRUE;
      data->pst.highlight_hue = (float)v / 255.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SplitToningHighlightSaturation"))
    {
      int v = atoi((char *)value);
      if(v != 0) data->has_splittoning = TRUE;
      data->pst.highlight_saturation = (float)v / 100.0;
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"SplitToningBalance"))
    {
      float v = g_ascii_strtod((char *)value, NULL);
      data->pst.balance = lr2dt_splittoning_balance(v);
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"Clarity2012"))
    {
      int v = atoi((char *)value);
      if(v != 0)
      {
        data->has_bilat = TRUE;
        data->pbl.detail = lr2dt_clarity((float)v);
      }
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"Rating"))
    {
      int v = atoi((char *)value);
      if(v != 0)
      {
        data->rating = v;
        data->has_rating = TRUE;
      }
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"GPSLatitude"))
    {
      double latitude = dt_util_gps_string_to_number((const char *)value);
      if(!isnan(latitude))
      {
        data->lat = latitude;
        data->has_gps = TRUE;
      }
    }
    else if(!xmlStrcmp(name, (const xmlChar *)"GPSLongitude"))
    {
      double longitude = dt_util_gps_string_to_number((const char *)value);
      if(!isnan(longitude))
      {
        data->lon = longitude;
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
    if(tag_change) DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);
  }
  else if(dev != NULL && !xmlStrcmp(name, (const xmlChar *)"RetouchInfo"))
  {
    xmlNodePtr riNode = node;

    while(riNode)
    {
      if(!xmlStrcmp(riNode->name, (const xmlChar *)"li"))
      {
        xmlChar *cvalue = xmlNodeListGetString(doc, riNode->xmlChildrenNode, 1);
        spot_t *p = &data->ps.spot[data->ps.num_spots];
        float x, y, radius, xc, yc;
        const char *startptr = (const char *)cvalue;
        if(_read_float(&startptr, "centerX", &x) &&
           _skip_comma(&startptr) &&
           _read_float(&startptr, "centerY", &y) &&
           _skip_comma(&startptr) &&
           _read_float(&startptr, "radius", &radius) &&
           _skip_comma(&startptr) &&
           _skip_key_value_pair(&startptr, "sourceState") &&
           _skip_comma(&startptr) &&
           _read_float(&startptr, "sourceX", &xc) &&
           _skip_comma(&startptr) &&
           _read_float(&startptr, "sourceY", &yc))
        {
          p->x = x;
          p->y = y;
          p->radius = radius;
          p->xc = xc;
          p->yc = yc;
          data->ps.num_spots++;
          data->has_spots = TRUE;
        }
        xmlFree(cvalue);
      }
      if(data->ps.num_spots == MAX_SPOTS) break;
      riNode = riNode->next;
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
        dt_metadata_set_import(imgid, "Xmp.dc.title", (char *)cvalue);
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
        dt_metadata_set_import(imgid, "Xmp.dc.description", (char *)cvalue);
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
        dt_metadata_set_import(imgid, "Xmp.dc.creator", (char *)cvalue);
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
        dt_metadata_set_import(imgid, "Xmp.dc.rights", (char *)cvalue);
        xmlFree(cvalue);
      }
      rigNode = rigNode->next;
    }
  }
}

/* _has_list returns true if the node contains a list of value */
static int _has_list(char *name)
{
  return !strcmp(name, "subject")
    || !strcmp(name, "hierarchicalSubject")
    || !strcmp(name, "RetouchInfo")
    || !strcmp(name, "ToneCurvePV2012")
    || !strcmp(name, "title")
    || !strcmp(name, "description")
    || !strcmp(name, "creator")
    || !strcmp(name, "publisher")
    || !strcmp(name, "rights");
};

/* handle a specific xpath */
static void _handle_xpath(dt_develop_t *dev, xmlDoc *doc, dt_imgid_t imgid, xmlXPathContext *ctx, const xmlChar *xpath, lr_data_t *data)
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

static inline double rotate_x(double x, double y, const double rangle)
{
  return x*cos(rangle) + y*sin(rangle);
}

static inline double rotate_y(double x, double y, const double rangle)
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

static inline float round5(double x)
{
  return round(x * 100000.f) / 100000.f;
}

gboolean dt_lightroom_import(dt_imgid_t imgid, dt_develop_t *dev, gboolean iauto)
{
  gboolean refresh_needed = FALSE;
  char imported[256] = { 0 };
  int n_import = 0;                // number of iop imported

  // Get full pathname
  char *pathname = dt_get_lightroom_xmp(imgid);

  if(!pathname)
  {
    if(!iauto) dt_control_log(_("cannot find lightroom XMP!"));
    return FALSE;
  }

  // Load LR xmp

  xmlDocPtr doc;
  xmlNodePtr entryNode;

  // Parse xml document

  doc = xmlParseEntity(pathname);

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
  data.has_spots = FALSE;
  data.curve_kind = linear;
  data.n_pts = 0;
  data.has_colorzones = FALSE;
  data.has_splittoning = FALSE;
  data.has_bilat = FALSE;
  data.has_tags = FALSE;
  data.rating = 0;
  data.has_rating = FALSE;
  data.lat = NAN;
  data.lon = NAN;
  data.has_gps = FALSE;
  data.color = 0;
  data.has_colorlabel = FALSE;
  data.fratio = NAN;                // factor ratio image
  data.crop_roundness = NAN;        // from lightroom
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

  if(dev != NULL && dt_image_is_raw(&dev->image_storage))
  {
    // set colorin to cmatrix which is the default from Adobe (so closer to what Lightroom does)
    dt_iop_colorin_params_v1_t pci = (dt_iop_colorin_params_v1_t){ "cmatrix", DT_INTENT_PERCEPTUAL };

    dt_add_hist(imgid, "colorin", (dt_iop_params_t *)&pci, sizeof(dt_iop_colorin_params_v1_t), imported,
                sizeof(imported), LRDT_COLORIN_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && data.has_crop)
  {
    double rangle;
    double cx, cw, cy, ch;
    double new_width, new_height;
    dt_image_orientation_t orientation = dt_image_orientation_to_flip_bits(data.orientation);

    data.pc.k_sym = 0;
    data.pc.k_apply = 0;
    data.pc.crop_auto = 0;  // Cannot use crop-auto=1 (the default at clipping GUI), as it does not allow to cover all cropping cases.
    data.pc.ratio_n = data.pc.ratio_d = -2;
    data.pc.k_h = data.pc.k_v = 0;
    data.pc.k_type = 0;
    data.pc.kxa = data.pc.kxd = 0.2f;
    data.pc.kxc = data.pc.kxb = 0.8f;
    data.pc.kya = data.pc.kyb = 0.2f;
    data.pc.kyc = data.pc.kyd = 0.8f;

    // Convert image in image-centered coordinate system, [-image_size / 2; + image_size / 2]
    cx = (data.pc.cx - 0.5f) * data.iwidth;
    cw = (data.pc.cw - 0.5f) * data.iwidth;
    cy = (data.pc.cy - 0.5f) * data.iheight;
    ch = (data.pc.ch - 0.5f) * data.iheight;

    // Rotate the cropped zone according to rotation angle
    // All rotations done using center of the image
    rangle = data.pc.angle * (M_PI / 180.0f);
    rotate_xy(&cx, &cy, -rangle);
    rotate_xy(&cw, &ch, -rangle);

    // Calculate the new overall image size (black zone included) after rotation
    // rangle is limited to -45°;+45° by LR
    new_width  = rotate_x(+data.iwidth, -data.iheight, -fabs(rangle));
    new_height = rotate_y(+data.iwidth, +data.iheight, -fabs(rangle));

    // apply new size & convert image back in initial coordinate system [0.0 ; +1.0]
    data.pc.cx = round5((cx / new_width)  + 0.5f);
    data.pc.cw = round5((cw / new_width)  + 0.5f);
    data.pc.cy = round5((cy / new_height) + 0.5f);
    data.pc.ch = round5((ch / new_height) + 0.5f);

    // adjust crop data according to the orientation - Must be done after rotation
    if(orientation & ORIENTATION_FLIP_X)
      flip(&data.pc.cx, &data.pc.cw);
    if(orientation & ORIENTATION_FLIP_Y)
      flip(&data.pc.cy, &data.pc.ch);
    if(orientation & ORIENTATION_SWAP_XY)
    {
      swap(&data.pc.cx, &data.pc.cy);
      swap(&data.pc.cw, &data.pc.ch);
    }

    // Invert angle when orientation is flipped
    if(orientation == ORIENTATION_FLIP_HORIZONTALLY
    || orientation == ORIENTATION_FLIP_VERTICALLY
    || orientation == ORIENTATION_TRANSPOSE
    || orientation == ORIENTATION_TRANSVERSE)
      data.pc.angle = -data.pc.angle;

    data.fratio = (data.pc.cw - data.pc.cx) / (data.pc.ch - data.pc.cy);

    dt_add_hist(imgid, "clipping", (dt_iop_params_t *)&data.pc, sizeof(dt_iop_clipping_params_t), imported,
                sizeof(imported), LRDT_CLIPPING_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && data.has_flip)
  {
    data.pf.orientation = dt_image_orientation_to_flip_bits(data.orientation);

    dt_add_hist(imgid, "flip", (dt_iop_params_t *)&data.pf, sizeof(dt_iop_flip_params_t), imported,
                sizeof(imported), LRDT_FLIP_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && data.has_exposure)
  {
    dt_add_hist(imgid, "exposure", (dt_iop_params_t *)&data.pe, sizeof(dt_iop_exposure_params_t), imported,
                sizeof(imported), LRDT_EXPOSURE_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && data.has_grain)
  {
    data.pg.channel = 0;

    dt_add_hist(imgid, "grain", (dt_iop_params_t *)&data.pg, sizeof(dt_iop_grain_params_t), imported,
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

    dt_add_hist(imgid, "vignette", (dt_iop_params_t *)&data.pv, sizeof(dt_iop_vignette_params_t), imported,
                sizeof(imported), LRDT_VIGNETTE_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && data.has_spots)
  {
    // Check for orientation, rotate when in portrait mode
    if(data.orientation > 4)
      for(int k = 0; k < data.ps.num_spots; k++)
      {
        float tmp = data.ps.spot[k].y;
        data.ps.spot[k].y = 1.0 - data.ps.spot[k].x;
        data.ps.spot[k].x = tmp;
        tmp = data.ps.spot[k].yc;
        data.ps.spot[k].yc = 1.0 - data.ps.spot[k].xc;
        data.ps.spot[k].xc = tmp;
      }

    dt_add_hist(imgid, "spots", (dt_iop_params_t *)&data.ps, sizeof(dt_iop_spots_params_t), imported,
                sizeof(imported), LRDT_SPOTS_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL &&
     (data.curve_kind != linear
      || data.ptc_value[0] != 0 || data.ptc_value[1] != 0 || data.ptc_value[2] != 0 || data.ptc_value[3] != 0))
  {
    const int total_pts = (data.curve_kind == custom) ? data.n_pts : 6;
    data.ptc.tonecurve_nodes[ch_L] = total_pts;
    data.ptc.tonecurve_nodes[ch_a] = 7;
    data.ptc.tonecurve_nodes[ch_b] = 7;
    data.ptc.tonecurve_type[ch_L] = CUBIC_SPLINE;
    data.ptc.tonecurve_type[ch_a] = CUBIC_SPLINE;
    data.ptc.tonecurve_type[ch_b] = CUBIC_SPLINE;
    data.ptc.tonecurve_autoscale_ab = 1;
    data.ptc.tonecurve_preset = 0;

    float linear_ab[7] = { 0.0, 0.08, 0.3, 0.5, 0.7, 0.92, 1.0 };

    // linear a, b curves
    for(int k = 0; k < 7; k++) data.ptc.tonecurve[ch_a][k].x = linear_ab[k];
    for(int k = 0; k < 7; k++) data.ptc.tonecurve[ch_a][k].y = linear_ab[k];
    for(int k = 0; k < 7; k++) data.ptc.tonecurve[ch_b][k].x = linear_ab[k];
    for(int k = 0; k < 7; k++) data.ptc.tonecurve[ch_b][k].y = linear_ab[k];

    // Set the base tonecurve

    if(data.curve_kind == linear)
    {
      data.ptc.tonecurve[ch_L][0].x = 0.0;
      data.ptc.tonecurve[ch_L][0].y = 0.0;
      data.ptc.tonecurve[ch_L][1].x = data.ptc_split[0] / 2.0;
      data.ptc.tonecurve[ch_L][1].y = data.ptc_split[0] / 2.0;
      data.ptc.tonecurve[ch_L][2].x = data.ptc_split[1] - (data.ptc_split[1] - data.ptc_split[0]) / 2.0;
      data.ptc.tonecurve[ch_L][2].y = data.ptc_split[1] - (data.ptc_split[1] - data.ptc_split[0]) / 2.0;
      data.ptc.tonecurve[ch_L][3].x = data.ptc_split[1] + (data.ptc_split[2] - data.ptc_split[1]) / 2.0;
      data.ptc.tonecurve[ch_L][3].y = data.ptc_split[1] + (data.ptc_split[2] - data.ptc_split[1]) / 2.0;
      data.ptc.tonecurve[ch_L][4].x = data.ptc_split[2] + (1.0 - data.ptc_split[2]) / 2.0;
      data.ptc.tonecurve[ch_L][4].y = data.ptc_split[2] + (1.0 - data.ptc_split[2]) / 2.0;
      data.ptc.tonecurve[ch_L][5].x = 1.0;
      data.ptc.tonecurve[ch_L][5].y = 1.0;
    }
    else
    {
      for(int k = 0; k < total_pts; k++)
      {
        data.ptc.tonecurve[ch_L][k].x = data.curve_pts[k][0] / 255.0;
        data.ptc.tonecurve[ch_L][k].y = data.curve_pts[k][1] / 255.0;
      }
    }

    if(data.curve_kind != custom)
    {
      // set shadows/darks/lights/highlight adjustments

      data.ptc.tonecurve[ch_L][1].y += data.ptc.tonecurve[ch_L][1].y * ((float)data.ptc_value[0] / 100.0);
      data.ptc.tonecurve[ch_L][2].y += data.ptc.tonecurve[ch_L][2].y * ((float)data.ptc_value[1] / 100.0);
      data.ptc.tonecurve[ch_L][3].y += data.ptc.tonecurve[ch_L][3].y * ((float)data.ptc_value[2] / 100.0);
      data.ptc.tonecurve[ch_L][4].y += data.ptc.tonecurve[ch_L][4].y * ((float)data.ptc_value[3] / 100.0);

      if(data.ptc.tonecurve[ch_L][1].y > data.ptc.tonecurve[ch_L][2].y)
        data.ptc.tonecurve[ch_L][1].y = data.ptc.tonecurve[ch_L][2].y;
      if(data.ptc.tonecurve[ch_L][3].y > data.ptc.tonecurve[ch_L][4].y)
        data.ptc.tonecurve[ch_L][4].y = data.ptc.tonecurve[ch_L][3].y;
    }

    dt_add_hist(imgid, "tonecurve", (dt_iop_params_t *)&data.ptc, sizeof(dt_iop_tonecurve_params_t), imported,
                sizeof(imported), LRDT_TONECURVE_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && data.has_colorzones)
  {
    data.pcz.channel = DT_IOP_COLORZONES_h;

    for(int i = 0; i < 3; i++)
      for(int k = 0; k < 8; k++)
        data.pcz.equalizer_x[i][k] = k / (DT_IOP_COLORZONES_BANDS - 1.0);

    dt_add_hist(imgid, "colorzones", (dt_iop_params_t *)&data.pcz, sizeof(dt_iop_colorzones_params_t), imported,
                sizeof(imported), LRDT_COLORZONES_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && data.has_splittoning)
  {
    data.pst.compress = 50.0;

    dt_add_hist(imgid, "splittoning", (dt_iop_params_t *)&data.pst, sizeof(dt_iop_splittoning_params_t), imported,
                sizeof(imported), LRDT_SPLITTONING_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && data.has_bilat)
  {
    data.pbl.sigma_r = 100.0;
    data.pbl.sigma_s = 100.0;

    dt_add_hist(imgid, "bilat", (dt_iop_params_t *)&data.pbl, sizeof(dt_iop_bilat_params_t), imported,
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
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_GEOTAG_CHANGED, imgs, 0);

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
      DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
    }
  }
  return TRUE;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
