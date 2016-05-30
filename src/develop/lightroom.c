/*
    This file is part of darktable,
    copyright (c) 2013--2015 pascal obry.

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

#include "common/darktable.h"
#include "common/colorspaces.h"
#include "common/tags.h"
#include "common/curve_tools.h"
#include "common/ratings.h"
#include "common/colorlabels.h"
#include "common/debug.h"
#include "develop/lightroom.h"
#include "control/control.h"

#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <sys/stat.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

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

#define LRDT_CLIPPING_VERSION 4
typedef struct dt_iop_clipping_params_t
{
  float angle, cx, cy, cw, ch, k_h, k_v;
  float kxa, kya, kxb, kyb, kxc, kyc, kxd, kyd;
  int k_type, k_sym;
  int k_apply, crop_auto;
} dt_iop_clipping_params_t;

#define LRDT_FLIP_VERSION 1
typedef struct dt_iop_flip_params_t
{
  int32_t orientation;
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
#define DT_IOP_COLOR_ICC_LEN 100

typedef struct dt_iop_colorin_params_t
{
  char iccprofile[DT_IOP_COLOR_ICC_LEN];
  dt_iop_color_intent_t intent;
} dt_iop_colorin_params_t;

//
// end of iop structs
//

// the blend params for Lr import, not used in this mode (mode=0), as for iop generate the blend params for
// the
// version specified above.

#define LRDT_BLEND_VERSION 4
#define DEVELOP_BLENDIF_SIZE 16

typedef struct dt_develop_blend_params_t
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
} dt_develop_blend_params_t;

//
// end of blend_params
//

typedef struct lr2dt
{
  float lr, dt;
} lr2dt_t;

char *dt_get_lightroom_xmp(int imgid)
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

static void dt_add_hist(int imgid, char *operation, dt_iop_params_t *params, int params_size, char *imported,
                        size_t imported_len, int version, int *import_count)
{
  int32_t num = 0;
  dt_develop_blend_params_t blend_params = { 0 };

  //  get current num if any
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT count(num) FROM history WHERE imgid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    num = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);

  // add new history info
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO history (imgid, num, module, operation, op_params, enabled, "
                              "blendop_params, blendop_version, multi_priority, multi_name) "
                              "VALUES (?1, ?2, ?3, ?4, ?5, 1, ?6, ?7, 0, ' ')",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, num);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 5, params, params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 6, &blend_params, sizeof(dt_develop_blend_params_t), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, LRDT_BLEND_VERSION);

  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // also bump history_end
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE images SET history_end = (SELECT IFNULL(MAX(num) + 1, 0) FROM history WHERE imgid = ?1) WHERE id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if(imported[0]) g_strlcat(imported, ", ", imported_len);
  g_strlcat(imported, dt_iop_get_localized_name(operation), imported_len);
  (*import_count)++;
}

void dt_lightroom_import(int imgid, dt_develop_t *dev, gboolean iauto)
{
  gboolean refresh_needed = FALSE;
  char imported[256] = { 0 };

  // Get full pathname
  char *pathname = dt_get_lightroom_xmp(imgid);

  if(!pathname)
  {
    if(!iauto) dt_control_log(_("cannot find lightroom XMP!"));
    return;
  }

  // Load LR xmp

  xmlDocPtr doc;
  xmlNodePtr entryNode;

  // Parse xml document

  doc = xmlParseEntity(pathname);

  if(doc == NULL)
  {
    g_free(pathname);
    return;
  }

  // Enter first node, xmpmeta

  entryNode = xmlDocGetRootElement(doc);

  if(entryNode == NULL)
  {
    g_free(pathname);
    xmlFreeDoc(doc);
    return;
  }

  if(xmlStrcmp(entryNode->name, (const xmlChar *)"xmpmeta"))
  {
    if(!iauto) dt_control_log(_("`%s' not a lightroom XMP!"), pathname);
    g_free(pathname);
    return;
  }

  // Check that this is really a Lightroom document

  xmlXPathContextPtr xpathCtx = xmlXPathNewContext(doc);

  if(xpathCtx == NULL)
  {
    g_free(pathname);
    xmlFreeDoc(doc);
    return;
  }

  xmlXPathRegisterNs(xpathCtx, BAD_CAST "stEvt", BAD_CAST "http://ns.adobe.com/xap/1.0/sType/ResourceEvent#");

  xmlXPathObjectPtr xpathObj = xmlXPathEvalExpression((const xmlChar *)"//@stEvt:softwareAgent", xpathCtx);

  if(xpathObj == NULL)
  {
    if(!iauto) dt_control_log(_("`%s' not a lightroom XMP!"), pathname);
    xmlXPathFreeContext(xpathCtx);
    g_free(pathname);
    xmlFreeDoc(doc);
    return;
  }

  xmlNodeSetPtr xnodes = xpathObj->nodesetval;

  if(xnodes != NULL && xnodes->nodeNr > 0)
  {
    xmlNodePtr xnode = xnodes->nodeTab[0];
    xmlChar *value = xmlNodeListGetString(doc, xnode->xmlChildrenNode, 1);

    if(!strstr((char *)value, "Lightroom"))
    {
      xmlXPathFreeContext(xpathCtx);
      xmlXPathFreeObject(xpathObj);
      xmlFreeDoc(doc);
      xmlFree(value);
      if(!iauto) dt_control_log(_("`%s' not a lightroom XMP!"), pathname);
      g_free(pathname);
      return;
    }
    xmlFree(value);
  }
// we could bail out here if we ONLY wanted to load a file known to be from lightroom.
// if we don't know who created it we will just import it however.
//   else
//   {
//     xmlXPathFreeObject(xpathObj);
//     xmlXPathFreeContext(xpathCtx);
//     if(!iauto) dt_control_log(_("`%s' not a lightroom XMP!"), pathname);
//     g_free(pathname);
//     return;
//   }

  xmlXPathFreeObject(xpathObj);
  xmlXPathFreeContext(xpathCtx);

  // Go safely to Description node

  if(entryNode) entryNode = entryNode->xmlChildrenNode;
  if(entryNode) entryNode = entryNode->next;
  if(entryNode) entryNode = entryNode->xmlChildrenNode;
  if(entryNode) entryNode = entryNode->next;

  if(!entryNode || xmlStrcmp(entryNode->name, (const xmlChar *)"Description"))
  {
    if(!iauto) dt_control_log(_("`%s' not a lightroom XMP!"), pathname);
    g_free(pathname);
    return;
  }
  g_free(pathname);

  //  Look for attributes in the Description

  dt_iop_clipping_params_t pc;
  memset(&pc, 0, sizeof(pc));
  gboolean has_crop = FALSE;

  dt_iop_flip_params_t pf;
  memset(&pf, 0, sizeof(pf));
  gboolean has_flip = FALSE;

  dt_iop_exposure_params_t pe;
  memset(&pe, 0, sizeof(pe));
  gboolean has_exposure = FALSE;

  dt_iop_vignette_params_t pv;
  memset(&pv, 0, sizeof(pv));
  gboolean has_vignette = FALSE;

  dt_iop_grain_params_t pg;
  memset(&pg, 0, sizeof(pg));
  gboolean has_grain = FALSE;

  dt_iop_spots_params_t ps;
  memset(&ps, 0, sizeof(ps));
  gboolean has_spots = FALSE;

  typedef enum lr_curve_kind_t
  {
    linear = 0,
    medium_contrast = 1,
    strong_contrast = 2,
    custom = 3
  } lr_curve_kind_t;

#define MAX_PTS 20
  dt_iop_tonecurve_params_t ptc;
  memset(&ptc, 0, sizeof(ptc));
  int ptc_value[4] = { 0, 0, 0, 0 };
  float ptc_split[3] = { 0.0, 0.0, 0.0 };
  lr_curve_kind_t curve_kind = linear;
  int curve_pts[MAX_PTS][2];
  int n_pts = 0;

  dt_iop_colorzones_params_t pcz;
  memset(&pcz, 0, sizeof(pcz));
  gboolean has_colorzones = FALSE;

  dt_iop_splittoning_params_t pst;
  memset(&pst, 0, sizeof(pst));
  gboolean has_splittoning = FALSE;

  dt_iop_bilat_params_t pbl;
  memset(&pbl, 0, sizeof(pbl));
  gboolean has_bilat = FALSE;

  gboolean has_tags = FALSE;

  int rating = 0;
  gboolean has_rating = FALSE;

  gdouble lat = 0, lon = 0;
  gboolean has_gps = FALSE;

  int color = 0;
  gboolean has_colorlabel = FALSE;

  float fratio = 0;                // factor ratio image
  float crop_roundness = 0;        // from lightroom
  int n_import = 0;                // number of iop imported
  const float hfactor = 3.0 / 9.0; // hue factor adjustment (use 3 out of 9 boxes in colorzones)
  const float lfactor = 4.0 / 9.0; // lightness factor adjustment (use 4 out of 9 boxes in colorzones)
  int iwidth = 0, iheight = 0;     // image width / height
  int orientation = 1;

  xmlAttr *attribute = entryNode->properties;

  while(attribute && attribute->name && attribute->children)
  {
    xmlChar *value = xmlNodeListGetString(entryNode->doc, attribute->children, 1);
    if(!xmlStrcmp(attribute->name, (const xmlChar *)"CropTop"))
      pc.cy = g_ascii_strtod((char *)value, NULL);
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"CropRight"))
      pc.cw = g_ascii_strtod((char *)value, NULL);
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"CropLeft"))
      pc.cx = g_ascii_strtod((char *)value, NULL);
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"CropBottom"))
      pc.ch = g_ascii_strtod((char *)value, NULL);
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"CropAngle"))
      pc.angle = -g_ascii_strtod((char *)value, NULL);
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"ImageWidth"))
      iwidth = atoi((char *)value);
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"ImageLength"))
      iheight = atoi((char *)value);
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"Orientation"))
    {
      orientation = atoi((char *)value);
      if(dev != NULL && ((dev->image_storage.orientation == 6 && orientation != 6)
                         || (dev->image_storage.orientation == 5 && orientation != 8)
                         || (dev->image_storage.orientation == 0 && orientation != 1)))
        has_flip = TRUE;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"HasCrop"))
    {
      if(!xmlStrcmp(value, (const xmlChar *)"True")) has_crop = TRUE;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"Blacks2012"))
    {
      int v = atoi((char *)value);
      if(v != 0)
      {
        has_exposure = TRUE;
        pe.black = lr2dt_blacks((float)v);
      }
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"Exposure2012"))
    {
      float v = g_ascii_strtod((char *)value, NULL);
      if(v != 0.0)
      {
        has_exposure = TRUE;
        pe.exposure = v;
      }
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"PostCropVignetteAmount"))
    {
      int v = atoi((char *)value);
      if(v != 0)
      {
        has_vignette = TRUE;
        pv.brightness = lr2dt_vignette_gain((float)v);
      }
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"PostCropVignetteMidpoint"))
    {
      int v = atoi((char *)value);
      pv.scale = lr2dt_vignette_midpoint((float)v);
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"PostCropVignetteStyle"))
    {
      int v = atoi((char *)value);
      if(v == 1) // Highlight Priority
        pv.saturation = -0.300;
      else // Color Priority & Paint Overlay
        pv.saturation = -0.200;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"PostCropVignetteFeather"))
    {
      int v = atoi((char *)value);
      if(v != 0) pv.falloff_scale = (float)v;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"PostCropVignetteRoundness"))
    {
      int v = atoi((char *)value);
      crop_roundness = (float)v;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"GrainAmount"))
    {
      int v = atoi((char *)value);
      if(v != 0)
      {
        has_grain = TRUE;
        pg.strength = lr2dt_grain_amount((float)v);
      }
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"GrainFrequency"))
    {
      int v = atoi((char *)value);
      if(v != 0) pg.scale = lr2dt_grain_frequency((float)v);
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"ParametricShadows"))
    {
      ptc_value[0] = atoi((char *)value);
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"ParametricDarks"))
    {
      ptc_value[1] = atoi((char *)value);
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"ParametricLights"))
    {
      ptc_value[2] = atoi((char *)value);
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"ParametricHighlights"))
    {
      ptc_value[3] = atoi((char *)value);
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"ParametricShadowSplit"))
    {
      ptc_split[0] = g_ascii_strtod((char *)value, NULL) / 100.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"ParametricMidtoneSplit"))
    {
      ptc_split[1] = g_ascii_strtod((char *)value, NULL) / 100.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"ParametricHighlightSplit"))
    {
      ptc_split[2] = g_ascii_strtod((char *)value, NULL) / 100.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"ToneCurveName2012"))
    {
      if(!xmlStrcmp(value, (const xmlChar *)"Linear"))
        curve_kind = linear;
      else if(!xmlStrcmp(value, (const xmlChar *)"Medium Contrast"))
        curve_kind = medium_contrast;
      else if(!xmlStrcmp(value, (const xmlChar *)"Strong Contrast"))
        curve_kind = strong_contrast;
      else if(!xmlStrcmp(value, (const xmlChar *)"Custom"))
        curve_kind = custom;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"SaturationAdjustmentRed"))
    {
      int v = atoi((char *)value);
      if(v != 0) has_colorzones = TRUE;
      pcz.equalizer_y[1][0] = 0.5 + (float)v / 200.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"SaturationAdjustmentOrange"))
    {
      int v = atoi((char *)value);
      if(v != 0) has_colorzones = TRUE;
      pcz.equalizer_y[1][1] = 0.5 + (float)v / 200.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"SaturationAdjustmentYellow"))
    {
      int v = atoi((char *)value);
      if(v != 0) has_colorzones = TRUE;
      pcz.equalizer_y[1][2] = 0.5 + (float)v / 200.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"SaturationAdjustmentGreen"))
    {
      int v = atoi((char *)value);
      if(v != 0) has_colorzones = TRUE;
      pcz.equalizer_y[1][3] = 0.5 + (float)v / 200.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"SaturationAdjustmentAqua"))
    {
      int v = atoi((char *)value);
      if(v != 0) has_colorzones = TRUE;
      pcz.equalizer_y[1][4] = 0.5 + (float)v / 200.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"SaturationAdjustmentBlue"))
    {
      int v = atoi((char *)value);
      if(v != 0) has_colorzones = TRUE;
      pcz.equalizer_y[1][5] = 0.5 + (float)v / 200.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"SaturationAdjustmentPurple"))
    {
      int v = atoi((char *)value);
      if(v != 0) has_colorzones = TRUE;
      pcz.equalizer_y[1][6] = 0.5 + (float)v / 200.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"SaturationAdjustmentMagenta"))
    {
      int v = atoi((char *)value);
      if(v != 0) has_colorzones = TRUE;
      pcz.equalizer_y[1][7] = 0.5 + (float)v / 200.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"LuminanceAdjustmentRed"))
    {
      int v = atoi((char *)value);
      if(v != 0) has_colorzones = TRUE;
      pcz.equalizer_y[0][0] = 0.5 + lfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"LuminanceAdjustmentOrange"))
    {
      int v = atoi((char *)value);
      if(v != 0) has_colorzones = TRUE;
      pcz.equalizer_y[0][1] = 0.5 + lfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"LuminanceAdjustmentYellow"))
    {
      int v = atoi((char *)value);
      if(v != 0) has_colorzones = TRUE;
      pcz.equalizer_y[0][2] = 0.5 + lfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"LuminanceAdjustmentGreen"))
    {
      int v = atoi((char *)value);
      if(v != 0) has_colorzones = TRUE;
      pcz.equalizer_y[0][3] = 0.5 + lfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"LuminanceAdjustmentAqua"))
    {
      int v = atoi((char *)value);
      if(v != 0) has_colorzones = TRUE;
      pcz.equalizer_y[0][4] = 0.5 + lfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"LuminanceAdjustmentBlue"))
    {
      int v = atoi((char *)value);
      if(v != 0) has_colorzones = TRUE;
      pcz.equalizer_y[0][5] = 0.5 + lfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"LuminanceAdjustmentPurple"))
    {
      int v = atoi((char *)value);
      if(v != 0) has_colorzones = TRUE;
      pcz.equalizer_y[0][6] = 0.5 + lfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"LuminanceAdjustmentMagenta"))
    {
      int v = atoi((char *)value);
      if(v != 0) has_colorzones = TRUE;
      pcz.equalizer_y[0][7] = 0.5 + lfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"HueAdjustmentRed"))
    {
      int v = atoi((char *)value);
      if(v != 0) has_colorzones = TRUE;
      pcz.equalizer_y[2][0] = 0.5 + hfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"HueAdjustmentOrange"))
    {
      int v = atoi((char *)value);
      if(v != 0) has_colorzones = TRUE;
      pcz.equalizer_y[2][1] = 0.5 + hfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"HueAdjustmentYellow"))
    {
      int v = atoi((char *)value);
      if(v != 0) has_colorzones = TRUE;
      pcz.equalizer_y[2][2] = 0.5 + hfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"HueAdjustmentGreen"))
    {
      int v = atoi((char *)value);
      if(v != 0) has_colorzones = TRUE;
      pcz.equalizer_y[2][3] = 0.5 + hfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"HueAdjustmentAqua"))
    {
      int v = atoi((char *)value);
      if(v != 0) has_colorzones = TRUE;
      pcz.equalizer_y[2][4] = 0.5 + hfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"HueAdjustmentBlue"))
    {
      int v = atoi((char *)value);
      if(v != 0) has_colorzones = TRUE;
      pcz.equalizer_y[2][5] = 0.5 + hfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"HueAdjustmentPurple"))
    {
      int v = atoi((char *)value);
      if(v != 0) has_colorzones = TRUE;
      pcz.equalizer_y[2][6] = 0.5 + hfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"HueAdjustmentMagenta"))
    {
      int v = atoi((char *)value);
      if(v != 0) has_colorzones = TRUE;
      pcz.equalizer_y[2][7] = 0.5 + hfactor * (float)v / 200.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"SplitToningShadowHue"))
    {
      int v = atoi((char *)value);
      if(v != 0) has_splittoning = TRUE;
      pst.shadow_hue = (float)v / 255.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"SplitToningShadowSaturation"))
    {
      int v = atoi((char *)value);
      if(v != 0) has_splittoning = TRUE;
      pst.shadow_saturation = (float)v / 100.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"SplitToningHighlightHue"))
    {
      int v = atoi((char *)value);
      if(v != 0) has_splittoning = TRUE;
      pst.highlight_hue = (float)v / 255.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"SplitToningHighlightSaturation"))
    {
      int v = atoi((char *)value);
      if(v != 0) has_splittoning = TRUE;
      pst.highlight_saturation = (float)v / 100.0;
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"SplitToningBalance"))
    {
      float v = g_ascii_strtod((char *)value, NULL);
      pst.balance = lr2dt_splittoning_balance(v);
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"Clarity2012"))
    {
      int v = atoi((char *)value);
      if(v != 0)
      {
        has_bilat = TRUE;
        pbl.detail = lr2dt_clarity((float)v);
      }
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"Rating"))
    {
      int v = atoi((char *)value);
      if(v != 0)
      {
        rating = v;
        has_rating = TRUE;
      }
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"GPSLatitude"))
    {
      int deg;
      double msec;
      char d;

      if(sscanf((const char *)value, "%d,%lf%c", &deg, &msec, &d))
      {
        lat = deg + msec / 60.0;
        if(d == 'S') lat = -lat;
        has_gps = TRUE;
      }
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"GPSLongitude"))
    {
      int deg;
      double msec;
      char d;

      if(sscanf((const char *)value, "%d,%lf%c", &deg, &msec, &d))
      {
        lon = deg + msec / 60.0;
        if(d == 'W') lon = -lon;
        has_gps = TRUE;
      }
    }
    else if(!xmlStrcmp(attribute->name, (const xmlChar *)"Label"))
    {
      for(int i = 0; value[i]; i++) value[i] = tolower(value[i]);

      if(!strcmp((char *)value, _("red")))
        color = 0;
      else if(!strcmp((char *)value, _("yellow")))
        color = 1;
      else if(!strcmp((char *)value, _("green")))
        color = 2;
      else if(!strcmp((char *)value, _("blue")))
        color = 3;
      else
        // just an else here to catch all other cases as on lightroom one can
        // change the names of labels. So purple and the user's defined labels
        // will be mapped to purple on darktable.
        color = 4;

      has_colorlabel = TRUE;
    }

    xmlFree(value);
    attribute = attribute->next;
  }

  //  Look for tags (subject/Bag/* and RetouchInfo/seq/*)

  entryNode = entryNode->xmlChildrenNode;
  if(entryNode) entryNode = entryNode->next;

  while(entryNode)
  {
    if(dev == NULL && (!xmlStrcmp(entryNode->name, (const xmlChar *)"subject")
                       || !xmlStrcmp(entryNode->name, (const xmlChar *)"hierarchicalSubject")))
    {
      xmlNodePtr tagNode = entryNode;

      tagNode = tagNode->xmlChildrenNode;
      tagNode = tagNode->next;
      tagNode = tagNode->xmlChildrenNode;
      tagNode = tagNode->next;

      while(tagNode)
      {
        if(!xmlStrcmp(tagNode->name, (const xmlChar *)"li"))
        {
          xmlChar *value = xmlNodeListGetString(doc, tagNode->xmlChildrenNode, 1);
          guint tagid = 0;

          if(!dt_tag_exists((char *)value, &tagid)) dt_tag_new((char *)value, &tagid);

          dt_tag_attach(tagid, imgid);
          has_tags = TRUE;
          xmlFree(value);
        }
        tagNode = tagNode->next;
      }
    }
    else if(dev != NULL && !xmlStrcmp(entryNode->name, (const xmlChar *)"RetouchInfo"))
    {
      xmlNodePtr riNode = entryNode;

      riNode = riNode->xmlChildrenNode;
      riNode = riNode->next;
      riNode = riNode->xmlChildrenNode;
      riNode = riNode->next;

      while(riNode)
      {
        if(!xmlStrcmp(riNode->name, (const xmlChar *)"li"))
        {
          xmlChar *value = xmlNodeListGetString(doc, riNode->xmlChildrenNode, 1);
          spot_t *p = &ps.spot[ps.num_spots];
          if(sscanf((const char *)value, "centerX = %f, centerY = %f, radius = %f, sourceState = %*[a-zA-Z], "
                                         "sourceX = %f, sourceY = %f",
                    &(p->x), &(p->y), &(p->radius), &(p->xc), &(p->yc)))
          {
            ps.num_spots++;
            has_spots = TRUE;
          }
          xmlFree(value);
        }
        if(ps.num_spots == MAX_SPOTS) break;
        riNode = riNode->next;
      }
    }
    else if(dev != NULL && !xmlStrcmp(entryNode->name, (const xmlChar *)"ToneCurvePV2012"))
    {
      xmlNodePtr tcNode = entryNode;

      tcNode = tcNode->xmlChildrenNode;
      tcNode = tcNode->next;
      tcNode = tcNode->xmlChildrenNode;
      tcNode = tcNode->next;

      while(tcNode)
      {
        if(!xmlStrcmp(tcNode->name, (const xmlChar *)"li"))
        {
          xmlChar *value = xmlNodeListGetString(doc, tcNode->xmlChildrenNode, 1);

          if(sscanf((const char *)value, "%d, %d", &(curve_pts[n_pts][0]), &(curve_pts[n_pts][1]))) n_pts++;
          xmlFree(value);
        }
        if(n_pts == MAX_PTS) break;
        tcNode = tcNode->next;
      }
    }
    entryNode = entryNode->next;
  }

  xmlFreeDoc(doc);

  //  Integrates into the history all the imported iop

  if(dev != NULL && dt_image_is_raw(&dev->image_storage))
  {
    // set colorin to cmatrix which is the default from Adobe (so closer to what Lightroom does)
    dt_iop_colorin_params_t pci = (dt_iop_colorin_params_t){ "cmatrix", DT_INTENT_PERCEPTUAL };

    dt_add_hist(imgid, "colorin", (dt_iop_params_t *)&pci, sizeof(dt_iop_colorin_params_t), imported,
                sizeof(imported), LRDT_COLORIN_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && has_crop)
  {
    pc.k_sym = 0;
    pc.k_apply = 0;
    pc.crop_auto = 0;
    pc.k_h = pc.k_v = 0;
    pc.k_type = 0;
    pc.kxa = pc.kxd = 0.2f;
    pc.kxc = pc.kxb = 0.8f;
    pc.kya = pc.kyb = 0.2f;
    pc.kyc = pc.kyd = 0.8f;
    float tmp;

    if(has_crop)
    {
      // adjust crop data according to the rotation

      switch(dev->image_storage.orientation)
      {
        case 5: // portrait - counter-clockwise
          tmp = pc.ch;
          pc.ch = 1.0 - pc.cx;
          pc.cx = pc.cy;
          pc.cy = 1.0 - pc.cw;
          pc.cw = tmp;
          break;
        case 6: // portrait - clockwise
          tmp = pc.ch;
          pc.ch = pc.cw;
          pc.cw = 1.0 - pc.cy;
          pc.cy = pc.cx;
          pc.cx = 1.0 - tmp;
          break;
        default:
          break;
      }

      if(pc.angle != 0)
      {
        const float rangle = -pc.angle * (3.141592 / 180);
        float x, y;

        // do the rotation (rangle) using center of image (0.5, 0.5)

        x = pc.cx - 0.5;
        y = 0.5 - pc.cy;
        pc.cx = 0.5 + x * cos(rangle) - y * sin(rangle);
        pc.cy = 0.5 - (x * sin(rangle) + y * cos(rangle));

        x = pc.cw - 0.5;
        y = 0.5 - pc.ch;
        pc.cw = 0.5 + x * cos(rangle) - y * sin(rangle);
        pc.ch = 0.5 - (x * sin(rangle) + y * cos(rangle));
      }
    }
    else
    {
      pc.angle = 0;
      pc.cx = 0;
      pc.cy = 0;
      pc.cw = 1;
      pc.ch = 1;
    }

    fratio = (pc.cw - pc.cx) / (pc.ch - pc.cy);

    dt_add_hist(imgid, "clipping", (dt_iop_params_t *)&pc, sizeof(dt_iop_clipping_params_t), imported,
                sizeof(imported), LRDT_CLIPPING_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && has_flip)
  {
    pf.orientation = 0;

    if(dev->image_storage.orientation == 5)
      // portrait
      switch(orientation)
      {
        case 8:
          pf.orientation = 0;
          break;
        case 3:
          pf.orientation = 5;
          break;
        case 6:
          pf.orientation = 3;
          break;
        case 1:
          pf.orientation = 6;
          break;

        // with horizontal flip
        case 7:
          pf.orientation = 1;
          break;
        case 2:
          pf.orientation = 4;
          break;
        case 5:
          pf.orientation = 2;
          break;
        case 4:
          pf.orientation = 7;
          break;
      }

    else if(dev->image_storage.orientation == 6)
      // portrait
      switch(orientation)
      {
        case 8:
          pf.orientation = 3;
          break;
        case 3:
          pf.orientation = 6;
          break;
        case 6:
          pf.orientation = 0;
          break;
        case 1:
          pf.orientation = 5;
          break;

        // with horizontal flip
        case 7:
          pf.orientation = 2;
          break;
        case 2:
          pf.orientation = 7;
          break;
        case 5:
          pf.orientation = 1;
          break;
        case 4:
          pf.orientation = 4;
          break;
      }

    else
      // landscape
      switch(orientation)
      {
        case 8:
          pf.orientation = 5;
          break;
        case 3:
          pf.orientation = 3;
          break;
        case 6:
          pf.orientation = 6;
          break;
        case 1:
          pf.orientation = 0;
          break;

        // with horizontal flip
        case 7:
          pf.orientation = 7;
          break;
        case 2:
          pf.orientation = 1;
          break;
        case 5:
          pf.orientation = 4;
          break;
        case 4:
          pf.orientation = 2;
          break;
      }

    dt_add_hist(imgid, "flip", (dt_iop_params_t *)&pf, sizeof(dt_iop_flip_params_t), imported,
                sizeof(imported), LRDT_FLIP_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && has_exposure)
  {
    dt_add_hist(imgid, "exposure", (dt_iop_params_t *)&pe, sizeof(dt_iop_exposure_params_t), imported,
                sizeof(imported), LRDT_EXPOSURE_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && has_grain)
  {
    pg.channel = 0;

    dt_add_hist(imgid, "grain", (dt_iop_params_t *)&pg, sizeof(dt_iop_grain_params_t), imported,
                sizeof(imported), LRDT_GRAIN_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && has_vignette)
  {
    const float base_ratio = 1.325 / 1.5;

    pv.autoratio = FALSE;
    pv.dithering = DITHER_8BIT;
    pv.center.x = 0.0;
    pv.center.y = 0.0;
    pv.shape = 1.0;

    // defensive code, should not happen, but just in case future Lr version
    // has not ImageWidth/ImageLength XML tag.
    if(iwidth == 0 || iheight == 0)
      pv.whratio = base_ratio;
    else
      pv.whratio = base_ratio * ((float)iwidth / (float)iheight);

    if(has_crop) pv.whratio = pv.whratio * fratio;

    //  Adjust scale and ratio based on the roundness. On Lightroom changing
    //  the roundness change the width and the height of the vignette.

    if(crop_roundness > 0)
    {
      float newratio = pv.whratio - (pv.whratio - 1) * (crop_roundness / 100.0);
      float dscale = (1 - (newratio / pv.whratio)) / 2.0;

      pv.scale -= dscale * 100.0;
      pv.whratio = newratio;
    }

    dt_add_hist(imgid, "vignette", (dt_iop_params_t *)&pv, sizeof(dt_iop_vignette_params_t), imported,
                sizeof(imported), LRDT_VIGNETTE_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && has_spots)
  {
    // Check for orientation, rotate when in portrait mode
    if(orientation > 4)
      for(int k = 0; k < ps.num_spots; k++)
      {
        float tmp = ps.spot[k].y;
        ps.spot[k].y = 1.0 - ps.spot[k].x;
        ps.spot[k].x = tmp;
        tmp = ps.spot[k].yc;
        ps.spot[k].yc = 1.0 - ps.spot[k].xc;
        ps.spot[k].xc = tmp;
      }

    dt_add_hist(imgid, "spots", (dt_iop_params_t *)&ps, sizeof(dt_iop_spots_params_t), imported,
                sizeof(imported), LRDT_SPOTS_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL &&
     (curve_kind != linear
      || ptc_value[0] != 0 || ptc_value[1] != 0 || ptc_value[2] != 0 || ptc_value[3] != 0))
  {
    ptc.tonecurve_nodes[ch_L] = 6;
    ptc.tonecurve_nodes[ch_a] = 7;
    ptc.tonecurve_nodes[ch_b] = 7;
    ptc.tonecurve_type[ch_L] = CUBIC_SPLINE;
    ptc.tonecurve_type[ch_a] = CUBIC_SPLINE;
    ptc.tonecurve_type[ch_b] = CUBIC_SPLINE;
    ptc.tonecurve_autoscale_ab = 1;
    ptc.tonecurve_preset = 0;

    float linear_ab[7] = { 0.0, 0.08, 0.3, 0.5, 0.7, 0.92, 1.0 };

    // linear a, b curves
    for(int k = 0; k < 7; k++) ptc.tonecurve[ch_a][k].x = linear_ab[k];
    for(int k = 0; k < 7; k++) ptc.tonecurve[ch_a][k].y = linear_ab[k];
    for(int k = 0; k < 7; k++) ptc.tonecurve[ch_b][k].x = linear_ab[k];
    for(int k = 0; k < 7; k++) ptc.tonecurve[ch_b][k].y = linear_ab[k];

    // Set the base tonecurve

    if(curve_kind == linear)
    {
      ptc.tonecurve[ch_L][0].x = 0.0;
      ptc.tonecurve[ch_L][0].y = 0.0;
      ptc.tonecurve[ch_L][1].x = ptc_split[0] / 2.0;
      ptc.tonecurve[ch_L][1].y = ptc_split[0] / 2.0;
      ptc.tonecurve[ch_L][2].x = ptc_split[1] - (ptc_split[1] - ptc_split[0]) / 2.0;
      ptc.tonecurve[ch_L][2].y = ptc_split[1] - (ptc_split[1] - ptc_split[0]) / 2.0;
      ptc.tonecurve[ch_L][3].x = ptc_split[1] + (ptc_split[2] - ptc_split[1]) / 2.0;
      ptc.tonecurve[ch_L][3].y = ptc_split[1] + (ptc_split[2] - ptc_split[1]) / 2.0;
      ptc.tonecurve[ch_L][4].x = ptc_split[2] + (1.0 - ptc_split[2]) / 2.0;
      ptc.tonecurve[ch_L][4].y = ptc_split[2] + (1.0 - ptc_split[2]) / 2.0;
      ptc.tonecurve[ch_L][5].x = 1.0;
      ptc.tonecurve[ch_L][5].y = 1.0;
    }
    else
    {
      for(int k = 0; k < 6; k++)
      {
        ptc.tonecurve[ch_L][k].x = curve_pts[k][0] / 255.0;
        ptc.tonecurve[ch_L][k].y = curve_pts[k][1] / 255.0;
      }
    }

    if(curve_kind != custom)
    {
      // set shadows/darks/lights/highlight adjustments

      ptc.tonecurve[ch_L][1].y += ptc.tonecurve[ch_L][1].y * ((float)ptc_value[0] / 100.0);
      ptc.tonecurve[ch_L][2].y += ptc.tonecurve[ch_L][2].y * ((float)ptc_value[1] / 100.0);
      ptc.tonecurve[ch_L][3].y += ptc.tonecurve[ch_L][3].y * ((float)ptc_value[2] / 100.0);
      ptc.tonecurve[ch_L][4].y += ptc.tonecurve[ch_L][4].y * ((float)ptc_value[3] / 100.0);

      if(ptc.tonecurve[ch_L][1].y > ptc.tonecurve[ch_L][2].y)
        ptc.tonecurve[ch_L][1].y = ptc.tonecurve[ch_L][2].y;
      if(ptc.tonecurve[ch_L][3].y > ptc.tonecurve[ch_L][4].y)
        ptc.tonecurve[ch_L][4].y = ptc.tonecurve[ch_L][3].y;
    }

    dt_add_hist(imgid, "tonecurve", (dt_iop_params_t *)&ptc, sizeof(dt_iop_tonecurve_params_t), imported,
                sizeof(imported), LRDT_TONECURVE_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && has_colorzones)
  {
    pcz.channel = DT_IOP_COLORZONES_h;

    for(int i = 0; i < 3; i++)
      for(int k = 0; k < 8; k++) pcz.equalizer_x[i][k] = k / (DT_IOP_COLORZONES_BANDS - 1.0);

    dt_add_hist(imgid, "colorzones", (dt_iop_params_t *)&pcz, sizeof(dt_iop_colorzones_params_t), imported,
                sizeof(imported), LRDT_COLORZONES_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && has_splittoning)
  {
    pst.compress = 50.0;

    dt_add_hist(imgid, "splittoning", (dt_iop_params_t *)&pst, sizeof(dt_iop_splittoning_params_t), imported,
                sizeof(imported), LRDT_SPLITTONING_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(dev != NULL && has_bilat)
  {
    pbl.sigma_r = 100.0;
    pbl.sigma_s = 100.0;

    dt_add_hist(imgid, "bilat", (dt_iop_params_t *)&pbl, sizeof(dt_iop_bilat_params_t), imported,
                sizeof(imported), LRDT_BILAT_VERSION, &n_import);
    refresh_needed = TRUE;
  }

  if(has_tags)
  {
    if(imported[0]) g_strlcat(imported, ", ", sizeof(imported));
    g_strlcat(imported, _("tags"), sizeof(imported));
    n_import++;
  }

  if(dev == NULL && has_rating)
  {
    dt_ratings_apply_to_image(imgid, rating);

    if(imported[0]) g_strlcat(imported, ", ", sizeof(imported));
    g_strlcat(imported, _("rating"), sizeof(imported));
    n_import++;
  }

  if(dev == NULL && has_gps)
  {
    dt_image_set_location(imgid, lon, lat);

    if(imported[0]) g_strlcat(imported, ", ", sizeof(imported));
    g_strlcat(imported, _("geotagging"), sizeof(imported));
    n_import++;
  }

  if(dev == NULL && has_colorlabel)
  {
    dt_colorlabels_set_label(imgid, color);

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
      dt_control_signal_raise(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
    }
  }
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
