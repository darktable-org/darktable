/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.
    copyright (c) 2011--2012 henrik andersson.

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
#include "common/tags.h"
#include "common/curve_tools.h"
#include "develop/lightroom.h"
#include "develop/develop.h"
#include "develop/blend.h"
#include "iop/clipping.h"
#include "iop/exposure.h"
#include "iop/grain.h"
#include "iop/vignette.h"
#include "iop/spots.h"
#include "iop/tonecurve.h"
#include "iop/colorzones.h"
#include "control/control.h"

#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <sys/stat.h>

typedef struct lr2dt
{
  float lr, dt;
} lr2dt_t;

char *dt_get_lightroom_xmp(int imgid)
{
  char pathname[DT_MAX_FILENAME_LEN];
  struct stat buf;

  // Get full pathname
  dt_image_full_path (imgid, pathname, DT_MAX_FILENAME_LEN);

  // Look for extension
  char *pos = strrchr(pathname, '.');

  if (pos==NULL) { return NULL; }

  // If found, replace extension with xmp
  strncpy(pos+1, "xmp", 4);

  if (!stat(pathname, &buf))
    return g_strdup(pathname);
  else
    return NULL;
}

static float get_interpolate (lr2dt_t lr2dt_table[], float value)
{
  int k=0;

  while (lr2dt_table[k+1].lr < value) k++;

  return lr2dt_table[k].dt +
    ((value - lr2dt_table[k].lr)
     / (lr2dt_table[k+1].lr - lr2dt_table[k].lr))
    * (lr2dt_table[k+1].dt - lr2dt_table[k].dt);
}

static float lr2dt_blacks(float value)
{
  lr2dt_t lr2dt_blacks_table[] =
    {{-100, 0.020}, {-50, 0.005}, {0, 0}, {50, -0.005}, {100, -0.010}};

  return get_interpolate (lr2dt_blacks_table, value);
}

static float lr2dt_exposure(float value)
{
  lr2dt_t lr2dt_exposure_table[] =
    {{-5, -4.5}, {0, 0}, {5, 4.5}};

  return get_interpolate (lr2dt_exposure_table, value);
}

static float lr2dt_vignette_gain(float value)
{
  lr2dt_t lr2dt_vignette_table[] =
    {{-100, -1}, {-50, -0.7}, {0, 0}, {50, 0.5}, {100, 1}};

  return get_interpolate (lr2dt_vignette_table, value);
}

static float lr2dt_vignette_midpoint(float value)
{
  lr2dt_t lr2dt_vignette_table[] =
    {{0, 74}, {4, 75}, {25, 85}, {50, 100}, {100, 100}};

  return get_interpolate (lr2dt_vignette_table, value);
}

static float lr2dt_grain_amount(float value)
{
  lr2dt_t lr2dt_grain_table[] =
    {{0, 0}, {25, 20}, {50, 40}, {100, 80}};

  return get_interpolate (lr2dt_grain_table, value);
}

static float lr2dt_grain_frequency(float value)
{
  lr2dt_t lr2dt_grain_table[] =
    {{0, 100}, {50, 100}, {75, 400}, {100, 800}};

  return get_interpolate (lr2dt_grain_table, value) / 53.3;
}

static dt_dev_history_item_t *_new_hist_for (dt_develop_t *dev, char *opname)
{
  GList *modules = dev->iop;
  const int multi_priority = 0;
  const char * multi_name = "";
  dt_dev_history_item_t *hist = (dt_dev_history_item_t *)malloc(sizeof(dt_dev_history_item_t));

  hist->enabled = 1;
  snprintf(hist->multi_name,128,"%s",multi_name);
  hist->multi_priority = 0;

  // look for module

  hist->module = NULL;
  dt_iop_module_t *find_op = NULL;

  //we have to add a new instance of this module and set index to modindex
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    if(!strcmp(module->op, opname))
    {
      if (module->multi_priority == multi_priority)
      {
        hist->module = module;
        break;
      }
      else if (multi_priority > 0)
      {
        //we just say that we find the name, so we just have to add new instance of this module
        find_op = module;
      }
    }
    modules = g_list_next(modules);
  }

  if (!hist->module && find_op)
  {
    dt_iop_module_t *new_module = (dt_iop_module_t *)malloc(sizeof(dt_iop_module_t));
    if (!dt_iop_load_module(new_module, find_op->so, dev))
    {
      new_module->multi_priority = 0;

      snprintf(new_module->multi_name,128,"%s","");

      dev->iop = g_list_insert_sorted(dev->iop, new_module, sort_plugins);

      new_module->instance = find_op->instance;
      hist->module = new_module;
    }
  }

  //  Initially set with default parameters

  hist->params = malloc(hist->module->params_size);
  memcpy(hist->params, hist->module->default_params, hist->module->params_size);
  hist->blend_params = malloc(sizeof(dt_develop_blend_params_t));
  memcpy(hist->blend_params, hist->module->default_blendop_params, sizeof(dt_develop_blend_params_t));

  return hist;
}

static void dt_add_hist (dt_develop_t *dev, dt_dev_history_item_t *hist, char *imported, char *error, int version, int *import_count)
{
  if (hist->module->version() == version)
  {
    //  Add clipping history to this dev
    hist->module->enabled = hist->enabled;

    dev->history = g_list_append(dev->history, hist);
    dev->history_end ++;

    if (imported[0]) strcat(imported, ", ");
    strcat(imported, hist->module->name());
    (*import_count)++;
  }
  else
  {
    if (error[0]) strcat(error, ", ");
    strcat(error, hist->module->name());
    free (hist);
  }
}

void dt_lightroom_import (dt_develop_t *dev)
{
  gboolean refresh_needed = FALSE;
  char imported[256] = {0};
  char error[256] = {0};

  // Get full pathname
  char *pathname = dt_get_lightroom_xmp(dev->image_storage.id);

  if (!pathname)
  {
    dt_control_log(_("cannot find lightroom xmp!"));
    g_free(pathname);
    return;
  }

  // Load LR xmp

  xmlDocPtr doc;
  xmlNodePtr entryNode;

  // Parse xml document

  doc = xmlParseEntity(pathname);

  // Enter first node, xmpmeta

  entryNode = xmlDocGetRootElement(doc);

  if (xmlStrcmp(entryNode->name, (const xmlChar *)"xmpmeta"))
  {
    dt_control_log(_("`%s' not a lightroom xmp!"), pathname);
    g_free(pathname);
    return;
  }

  // Go safely to Description node

  if (entryNode)
    entryNode = entryNode->xmlChildrenNode;
  if (entryNode)
    entryNode = entryNode->next;
  if (entryNode)
    entryNode = entryNode->xmlChildrenNode;
  if (entryNode)
    entryNode = entryNode->next;

  if (!entryNode || xmlStrcmp(entryNode->name, (const xmlChar *)"Description"))
  {
    dt_control_log(_("`%s' not a lightroom xmp!"), pathname);
    g_free(pathname);
    return;
  }
  g_free(pathname);

  //  Look for attributes in the Description

  dt_iop_clipping_params_t pc;
  memset(&pc, 0, sizeof(pc));
  gboolean has_crop = FALSE;
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
    string_contrast = 2,
    custom = 3
  } lr_curve_kind_t;

  #define MAX_PTS 20
  dt_iop_tonecurve_params_t ptc;
  memset(&ptc, 0, sizeof(ptc));
  int ptc_value[4] = {0, 0, 0, 0};
  float ptc_split[3] = {0.0, 0.0, 0.0};
  lr_curve_kind_t curve_kind = linear;
  int curve_pts[MAX_PTS][2];
  int n_pts = 0;

  dt_iop_colorzones_params_t pcz;
  memset(&pcz, 0, sizeof(pcz));
  gboolean has_colorzones = FALSE;

  gboolean has_tags = FALSE;

  float fratio = 0;         // factor ratio image
  int flip = 0;             // flip value
  float crop_roundness = 0; // from lightroom
  int n_import = 0;         // number of iop imported

  xmlAttr* attribute = entryNode->properties;

  while(attribute && attribute->name && attribute->children)
  {
    xmlChar* value = xmlNodeListGetString(entryNode->doc, attribute->children, 1);
    if (!xmlStrcmp(attribute->name, (const xmlChar *) "CropTop"))
      pc.cy = atof((char *)value);
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "CropRight"))
      pc.cw = atof((char *)value);
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "CropLeft"))
      pc.cx = atof((char *)value);
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "CropBottom"))
      pc.ch = atof((char *)value);
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "CropAngle"))
      pc.angle = -atof((char *)value);
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "Orientation"))
    {
      int v = atoi((char *)value);
      if (v == 1) flip = 0;
      else if (v == 2) flip = 1;
      else if (v == 3) flip = 3;
      else if (v == 4) flip = 2;
      if (flip) has_flip = TRUE;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "HasCrop"))
    {
      if (!xmlStrcmp(value, (const xmlChar *)"True"))
        has_crop = TRUE;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "Blacks2012"))
    {
      int v = atoi((char *)value);
      if (v != 0)
      {
        has_exposure = TRUE;
        pe.black = lr2dt_blacks((float)v);
      }
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "Exposure2012"))
    {
      float v = atof((char *)value);
      if (v != 0.0)
      {
        has_exposure = TRUE;
        pe.exposure = lr2dt_exposure(v);
      }
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "PostCropVignetteAmount"))
    {
      int v = atoi((char *)value);
      if (v != 0)
      {
        has_vignette = TRUE;
        pv.brightness = lr2dt_vignette_gain((float)v);
      }
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "PostCropVignetteMidpoint"))
    {
      int v = atoi((char *)value);
      pv.scale = lr2dt_vignette_midpoint((float)v);
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "PostCropVignetteStyle"))
    {
      int v = atoi((char *)value);
      if (v == 1) // Highlight Priority
        pv.saturation = -0.300;
      else // Color Priority & Paint Overlay
        pv.saturation = -0.200;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "PostCropVignetteFeather"))
    {
      int v = atoi((char *)value);
      if (v != 0)
        pv.falloff_scale = (float)v;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "PostCropVignetteRoundness"))
    {
      int v = atoi((char *)value);
      crop_roundness = (float)v;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "GrainAmount"))
    {
      int v = atoi((char *)value);
      if (v != 0)
      {
        has_grain = TRUE;
        pg.strength = lr2dt_grain_amount((float)v);
      }
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "GrainFrequency"))
    {
      int v = atoi((char *)value);
      if (v != 0)
        pg.scale = lr2dt_grain_frequency((float)v);
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "ParametricShadows"))
    {
      ptc_value[0] = atoi((char *)value);
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "ParametricDarks"))
    {
      ptc_value[1] = atoi((char *)value);
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "ParametricLights"))
    {
      ptc_value[2] = atoi((char *)value);
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "ParametricHighlights"))
    {
      ptc_value[3] = atoi((char *)value);
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "ParametricShadowSplit"))
    {
      ptc_split[0] = atof((char *)value) / 100.0;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "ParametricMidtoneSplit"))
    {
      ptc_split[1] = atof((char *)value) / 100.0;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "ParametricHighlightSplit"))
    {
      ptc_split[2] = atof((char *)value) / 100.0;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "ToneCurveName2012"))
    {
      if (!xmlStrcmp(value, (const xmlChar *)"Linear"))
        curve_kind = linear;
      else if (!xmlStrcmp(value, (const xmlChar *)"Medium Contrast"))
        curve_kind = medium_contrast;
      else if (!xmlStrcmp(value, (const xmlChar *)"Strong Contrast"))
        curve_kind = medium_contrast;
      else if (!xmlStrcmp(value, (const xmlChar *)"Custom"))
        curve_kind = custom;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "SaturationAdjustmentRed"))
    {
      int v = atoi((char *)value);
      if (v!=0)
        has_colorzones = TRUE;
      pcz.equalizer_y[1][0] = 0.5 + (float)v / 200.0;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "SaturationAdjustmentOrange"))
    {
      int v = atoi((char *)value);
      if (v!=0)
        has_colorzones = TRUE;
      pcz.equalizer_y[1][1] = 0.5 + (float)v / 200.0;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "SaturationAdjustmentYellow"))
    {
      int v = atoi((char *)value);
      if (v!=0)
        has_colorzones = TRUE;
      pcz.equalizer_y[1][2] = 0.5 + (float)v / 200.0;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "SaturationAdjustmentGreen"))
    {
      int v = atoi((char *)value);
      if (v!=0)
        has_colorzones = TRUE;
      pcz.equalizer_y[1][3] = 0.5 + (float)v / 200.0;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "SaturationAdjustmentAqua"))
    {
      int v = atoi((char *)value);
      if (v!=0)
        has_colorzones = TRUE;
      pcz.equalizer_y[1][4] = 0.5 + (float)v / 200.0;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "SaturationAdjustmentBlue"))
    {
      int v = atoi((char *)value);
      if (v!=0)
        has_colorzones = TRUE;
      pcz.equalizer_y[1][5] = 0.5 + (float)v / 200.0;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "SaturationAdjustmentPurple"))
    {
      int v = atoi((char *)value);
      if (v!=0)
        has_colorzones = TRUE;
      pcz.equalizer_y[1][6] = 0.5 + (float)v / 200.0;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "SaturationAdjustmentMagenta"))
    {
      int v = atoi((char *)value);
      if (v!=0)
        has_colorzones = TRUE;
      pcz.equalizer_y[1][7] = 0.5 + (float)v / 200.0;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "LuminanceAdjustmentRed"))
    {
      int v = atoi((char *)value);
      if (v!=0)
        has_colorzones = TRUE;
      pcz.equalizer_y[0][0] = 0.5 + (float)v / 200.0;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "LuminanceAdjustmentOrange"))
    {
      int v = atoi((char *)value);
      if (v!=0)
        has_colorzones = TRUE;
      pcz.equalizer_y[0][1] = 0.5 + (float)v / 200.0;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "LuminanceAdjustmentYellow"))
    {
      int v = atoi((char *)value);
      if (v!=0)
        has_colorzones = TRUE;
      pcz.equalizer_y[0][2] = 0.5 + (float)v / 200.0;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "LuminanceAdjustmentGreen"))
    {
      int v = atoi((char *)value);
      if (v!=0)
        has_colorzones = TRUE;
      pcz.equalizer_y[0][3] = 0.5 + (float)v / 200.0;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "LuminanceAdjustmentAqua"))
    {
      int v = atoi((char *)value);
      if (v!=0)
        has_colorzones = TRUE;
      pcz.equalizer_y[0][4] = 0.5 + (float)v / 200.0;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "LuminanceAdjustmentBlue"))
    {
      int v = atoi((char *)value);
      if (v!=0)
        has_colorzones = TRUE;
      pcz.equalizer_y[0][5] = 0.5 + (float)v / 200.0;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "LuminanceAdjustmentPurple"))
    {
      int v = atoi((char *)value);
      if (v!=0)
        has_colorzones = TRUE;
      pcz.equalizer_y[0][6] = 0.5 + (float)v / 200.0;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "LuminanceAdjustmentMagenta"))
    {
      int v = atoi((char *)value);
      if (v!=0)
        has_colorzones = TRUE;
      pcz.equalizer_y[0][7] = 0.5 + (float)v / 200.0;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "HueAdjustmentRed"))
    {
      int v = atoi((char *)value);
      if (v!=0)
        has_colorzones = TRUE;
      pcz.equalizer_y[2][0] = 0.5 + (float)v / 200.0;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "HueAdjustmentOrange"))
    {
      int v = atoi((char *)value);
      if (v!=0)
        has_colorzones = TRUE;
      pcz.equalizer_y[2][1] = 0.5 + (float)v / 200.0;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "HueAdjustmentYellow"))
    {
      int v = atoi((char *)value);
      if (v!=0)
        has_colorzones = TRUE;
      pcz.equalizer_y[2][2] = 0.5 + (float)v / 200.0;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "HueAdjustmentGreen"))
    {
      int v = atoi((char *)value);
      if (v!=0)
        has_colorzones = TRUE;
      pcz.equalizer_y[2][3] = 0.5 + (float)v / 200.0;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "HueAdjustmentAqua"))
    {
      int v = atoi((char *)value);
      if (v!=0)
        has_colorzones = TRUE;
      pcz.equalizer_y[2][4] = 0.5 + (float)v / 200.0;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "HueAdjustmentBlue"))
    {
      int v = atoi((char *)value);
      if (v!=0)
        has_colorzones = TRUE;
      pcz.equalizer_y[2][5] = 0.5 + (float)v / 200.0;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "HueAdjustmentPurple"))
    {
      int v = atoi((char *)value);
      if (v!=0)
        has_colorzones = TRUE;
      pcz.equalizer_y[2][6] = 0.5 + (float)v / 200.0;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "HueAdjustmentMagenta"))
    {
      int v = atoi((char *)value);
      if (v!=0)
        has_colorzones = TRUE;
      pcz.equalizer_y[2][7] = 0.5 + (float)v / 200.0;
    }

    xmlFree(value);
    attribute = attribute->next;
  }

  //  Look for tags (subject/Bag/* and RetouchInfo/seq/*)

  entryNode = entryNode->xmlChildrenNode;
  entryNode = entryNode->next;

  while (entryNode)
  {
    if (!xmlStrcmp(entryNode->name, (const xmlChar *) "subject"))
    {
      xmlNodePtr tagNode = entryNode;

      tagNode = tagNode->xmlChildrenNode;
      tagNode = tagNode->next;
      tagNode = tagNode->xmlChildrenNode;
      tagNode = tagNode->next;

      while (tagNode)
      {
        if (!xmlStrcmp(tagNode->name, (const xmlChar *) "li"))
        {
          xmlChar *value= xmlNodeListGetString(doc, tagNode->xmlChildrenNode, 1);
          guint tagid = 0;

          if (!dt_tag_exists((char *)value, &tagid))
            dt_tag_new((char *)value, &tagid);

          dt_tag_attach(tagid, dev->image_storage.id);
          has_tags = TRUE;
          xmlFree(value);
        }
        tagNode = tagNode->next;
      }
    }
    else if (!xmlStrcmp(entryNode->name, (const xmlChar *) "RetouchInfo"))
    {
      xmlNodePtr riNode = entryNode;

      riNode = riNode->xmlChildrenNode;
      riNode = riNode->next;
      riNode = riNode->xmlChildrenNode;
      riNode = riNode->next;

      while (riNode)
      {
        if (!xmlStrcmp(riNode->name, (const xmlChar *) "li"))
        {
          xmlChar *value= xmlNodeListGetString(doc, riNode->xmlChildrenNode, 1);
          spot_t *p = &ps.spot[ps.num_spots];
          if (sscanf((const char *)value, "centerX = %f, centerY = %f, radius = %f, sourceState = %*[a-zA-Z], sourceX = %f, sourceY = %f", &(p->x), &(p->y), &(p->radius), &(p->xc), &(p->yc)))
          {
            ps.num_spots++;
            has_spots = TRUE;
          }
          xmlFree(value);
        }
        if (ps.num_spots == MAX_SPOTS) break;
        riNode = riNode->next;
      }
    }
    else if (!xmlStrcmp(entryNode->name, (const xmlChar *) "ToneCurvePV2012"))
    {
      xmlNodePtr tcNode = entryNode;

      tcNode = tcNode->xmlChildrenNode;
      tcNode = tcNode->next;
      tcNode = tcNode->xmlChildrenNode;
      tcNode = tcNode->next;

      while (tcNode)
      {
        if (!xmlStrcmp(tcNode->name, (const xmlChar *) "li"))
        {
          xmlChar *value= xmlNodeListGetString(doc, tcNode->xmlChildrenNode, 1);

          if (sscanf((const char *)value, "%d, %d", &(curve_pts[n_pts][0]), &(curve_pts[n_pts][1])))
            n_pts++;
          xmlFree(value);
        }
        if (n_pts == MAX_PTS) break;
        tcNode = tcNode->next;
      }
    }
    entryNode = entryNode->next;
  }

  xmlFreeDoc(doc);

  //  Integrates into the history all the imported iop

  if (has_crop || has_flip)
  {
    dt_dev_history_item_t *hist = _new_hist_for (dev, "clipping");
    dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)hist->params;

    if (has_crop)
    {
      p->angle = pc.angle;
      p->crop_auto = 0;

      if (p->angle == 0)
      {
        p->cx = pc.cx;
        p->cy = pc.cy;
        p->cw = pc.cw;
        p->ch = pc.ch;
      }
      else
      {
        const float rangle = -pc.angle * (3.141592 / 180);
        float x, y;

        // do the rotation (rangle) using center of image (0.5, 0.5)

        x = pc.cx - 0.5;
        y = 0.5 - pc.cy;
        p->cx = 0.5 + x * cos(rangle) - y * sin(rangle);
        p->cy = 0.5 - (x * sin(rangle) + y * cos(rangle));

        x = pc.cw - 0.5;
        y = 0.5 - pc.ch;

        p->cw = 0.5 + x * cos(rangle) - y * sin(rangle);
        p->ch = 0.5 - (x * sin(rangle) + y * cos(rangle));
      }
    }
    else
    {
      p->angle = 0;
      p->crop_auto = 0;
      p->cx = 0;
      p->cy = 0;
      p->cw = 1;
      p->ch = 1;
    }

    fratio = (p->cw - p->cx) / (p->ch - p->cy);

    if (has_flip)
    {
      if (flip & 1)
      {
        float cx = p->cx;
        p->cx = 1.0 - p->cw;
        p->cw = (1.0 - cx) * -1.0;
      }
      if (flip & 2)
      {
        float cy = p->cy;
        p->cy = 1.0 - p->ch;
        p->ch = (1.0 - cy) * -1.0;
      }
    }

    dt_add_hist (dev, hist, imported, error, 4, &n_import);
    refresh_needed=TRUE;
  }

  if (has_exposure)
  {
    dt_dev_history_item_t *hist = _new_hist_for (dev, "exposure");
    dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)hist->params;

    p->black = pe.black;
    p->exposure = pe.exposure;

    dt_add_hist (dev, hist, imported, error, 2, &n_import);
    refresh_needed=TRUE;
  }

  if (has_grain)
  {
    dt_dev_history_item_t *hist = _new_hist_for (dev, "grain");
    dt_iop_grain_params_t *p = (dt_iop_grain_params_t *)hist->params;

    p->scale = pg.scale;
    p->strength = pg.strength;

    dt_add_hist (dev, hist, imported, error, 1, &n_import);
    refresh_needed=TRUE;
  }

  if (has_vignette)
  {
    dt_dev_history_item_t *hist = _new_hist_for (dev, "vignette");
    dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)hist->params;
    const float base_ratio = 1.325 / 1.5;

    p->brightness = pv.brightness;
    p->scale = pv.scale;
    p->falloff_scale = pv.falloff_scale;
    p->whratio = base_ratio * ((float)dev->pipe->iwidth / (float)dev->pipe->iheight);
    if (has_crop)
      p->whratio = p->whratio * fratio;
    p->autoratio = FALSE;
    p->saturation = pv.saturation;
    p->dithering = DITHER_8BIT;

    //  Adjust scale and ratio based on the roundness. On Lightroom changing
    //  the roundness change the width and the height of the vignette.

    if (crop_roundness > 0)
    {
      float newratio = p->whratio - (p->whratio - 1) * (crop_roundness / 100.0);
      float dscale = (1 - (newratio / p->whratio)) / 2.0;

      p->scale = p->scale - dscale * 100.0;
      p->whratio = newratio;
    }

    dt_add_hist (dev, hist, imported, error, 3, &n_import);
    refresh_needed=TRUE;
  }

  if (has_spots)
  {
    dt_dev_history_item_t *hist = _new_hist_for (dev, "spots");
    dt_iop_spots_params_t *p = (dt_iop_spots_params_t *)hist->params;

    for (int k=0; k<ps.num_spots; k++)
      p->spot[k] = ps.spot[k];

    p->num_spots = ps.num_spots;

    dt_add_hist (dev, hist, imported, error, 1, &n_import);
    refresh_needed=TRUE;
  }

  if (curve_kind != linear || ptc_value[0] != 0 || ptc_value[1] != 0 || ptc_value[2] != 0 || ptc_value[3] != 0)
  {
    dt_dev_history_item_t *hist = _new_hist_for (dev, "tonecurve");
    dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)hist->params;

    memset(p, 0, sizeof(dt_iop_tonecurve_params_t));

    p->tonecurve_nodes[ch_L] = 6;
    p->tonecurve_nodes[ch_a] = 7;
    p->tonecurve_nodes[ch_b] = 7;
    p->tonecurve_type[ch_L] = CUBIC_SPLINE;
    p->tonecurve_type[ch_a] = CUBIC_SPLINE;
    p->tonecurve_type[ch_b] = CUBIC_SPLINE;
    p->tonecurve_autoscale_ab = 1;
    p->tonecurve_preset = 0;

    float linear_ab[7] = {0.0, 0.08, 0.3, 0.5, 0.7, 0.92, 1.0};

    // linear a, b curves
    for(int k=0; k<7; k++) p->tonecurve[ch_a][k].x = linear_ab[k];
    for(int k=0; k<7; k++) p->tonecurve[ch_a][k].y = linear_ab[k];
    for(int k=0; k<7; k++) p->tonecurve[ch_b][k].x = linear_ab[k];
    for(int k=0; k<7; k++) p->tonecurve[ch_b][k].y = linear_ab[k];

    // Set the base tonecurve

    if (curve_kind == linear)
    {
      p->tonecurve[ch_L][0].x = 0.0;
      p->tonecurve[ch_L][0].y = 0.0;
      p->tonecurve[ch_L][1].x = ptc_split[0] / 2.0;
      p->tonecurve[ch_L][1].y = ptc_split[0] / 2.0;
      p->tonecurve[ch_L][2].x = ptc_split[1] - (ptc_split[1] - ptc_split[0]) / 2.0;
      p->tonecurve[ch_L][2].y = ptc_split[1] - (ptc_split[1] - ptc_split[0]) / 2.0;
      p->tonecurve[ch_L][3].x = ptc_split[1] + (ptc_split[2] - ptc_split[1]) / 2.0;
      p->tonecurve[ch_L][3].y = ptc_split[1] + (ptc_split[2] - ptc_split[1]) / 2.0;
      p->tonecurve[ch_L][4].x = ptc_split[2] + (1.0 - ptc_split[2]) / 2.0;
      p->tonecurve[ch_L][4].y = ptc_split[2] + (1.0 - ptc_split[2]) / 2.0;
      p->tonecurve[ch_L][5].x = 1.0;
      p->tonecurve[ch_L][5].y = 1.0;
    }
    else
    {
      for (int k=0; k<6; k++)
      {
        p->tonecurve[ch_L][k].x = curve_pts[k][0] / 255.0;
        p->tonecurve[ch_L][k].y = curve_pts[k][1] / 255.0;
      }
    }

    if (curve_kind != custom)
    {
      // set shadows/darks/lights/highlight adjustments

      p->tonecurve[ch_L][1].y += p->tonecurve[ch_L][1].y * ((float)ptc_value[0] / 100.0);
      p->tonecurve[ch_L][2].y += p->tonecurve[ch_L][1].y * ((float)ptc_value[1] / 100.0);
      p->tonecurve[ch_L][3].y += p->tonecurve[ch_L][1].y * ((float)ptc_value[2] / 100.0);
      p->tonecurve[ch_L][4].y += p->tonecurve[ch_L][1].y * ((float)ptc_value[3] / 100.0);

      if (p->tonecurve[ch_L][1].y > p->tonecurve[ch_L][2].y)
        p->tonecurve[ch_L][1].y = p->tonecurve[ch_L][2].y;
      if (p->tonecurve[ch_L][3].y > p->tonecurve[ch_L][4].y)
        p->tonecurve[ch_L][4].y = p->tonecurve[ch_L][3].y;
    }

    dt_add_hist (dev, hist, imported, error, 3, &n_import);
    refresh_needed=TRUE;
  }

  if (has_colorzones)
  {
    dt_dev_history_item_t *hist = _new_hist_for (dev, "colorzones");
    dt_iop_colorzones_params_t *p = (dt_iop_colorzones_params_t *)hist->params;

    for (int i=0; i<3; i++)
      for (int k=0; k<8; k++)
      {
        p->equalizer_x[i][k] = k/(DT_IOP_COLORZONES_BANDS-1.0);
        p->equalizer_y[i][k] = pcz.equalizer_y[i][k];
      }

    dt_add_hist (dev, hist, imported, error, 2, &n_import);
    refresh_needed=TRUE;
  }

  if (has_tags)
  {
    if (imported[0]) strcat(imported, ", ");
    strcat(imported, _("tags"));
    n_import++;
  }

  if(refresh_needed && dev->gui_attached)
  {
    char message[512];

    strcpy(message, imported);

    // some hist have been created, display them
    strcat(message, " ");
    if (n_import==1)
      strcat(message, _("has been imported"));
    else
      strcat(message, _("have been imported"));

    if (error[0] != '\0')
    {
      strcat (message, "; ");
      strcat(message, _("version mismatch"));
      strcat (message, ": ");
      strcat (message, error);
    }
    dt_control_log(message);

    /* signal history changed */
    dt_control_signal_raise(darktable.signals,DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
    dt_dev_reprocess_center(dev);
  }
}
