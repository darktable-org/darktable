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
#include "develop/lightroom.h"
#include "develop/develop.h"
#include "develop/blend.h"
#include "iop/clipping.h"
#include "iop/exposure.h"
#include "iop/vignette.h"
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
  static char pathname[DT_MAX_FILENAME_LEN];
  struct stat buf;

  // Get full pathname
  dt_image_full_path (imgid, pathname, DT_MAX_FILENAME_LEN);

  // Look for extension
  char *pos = strrchr(pathname, '.');

  if (pos==NULL) { return NULL; }

  // If found, replace extension with xmp
  strncpy(pos+1, "xmp", 4);

  if (!stat(pathname, &buf))
    return pathname;
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

static float lr2dt_exposure(float value)
{
  lr2dt_t lr2dt_exposure_table[] =
    {{-100, 0.020}, {-50, 0.005}, {0, 0}, {50, -0.005}, {100, -0.010}};

  return get_interpolate (lr2dt_exposure_table, value);
}

static float lr2dt_vignette_gain(float value)
{
  lr2dt_t lr2dt_vignette_table[] =
    {{-100, -0.900}, {-50, -0.650}, {-25, -0.400}, {0, 0},
     {25, 0.250}, {50, 0.500}, {100, 0.900}};

  return get_interpolate (lr2dt_vignette_table, value);
}

static float  lr2dt_vignette_scale(float value)
{
  lr2dt_t lr2dt_vignette_table[] =
    {{0, 100.0}, {50, 90.0}, {100, 50.0}};

  return get_interpolate (lr2dt_vignette_table, value);
}

static float  lr2dt_vignette_falloff_scale(float value)
{
  lr2dt_t lr2dt_vignette_table[] =
    {{0, 0.0}, {50, 70.0}, {100, 85.0}};

  return get_interpolate (lr2dt_vignette_table, value);
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

static void dt_add_hist (dt_develop_t *dev, dt_dev_history_item_t *hist, char *imported)
{
  //  Add clipping history to this dev
  hist->module->enabled = hist->enabled;

  dev->history = g_list_append(dev->history, hist);
  dev->history_end ++;

  if (imported[0]) strcat(imported, ", ");
  strcat(imported, hist->module->name());
}

void dt_lightroom_import (dt_develop_t *dev)
{
  gboolean refresh_needed = FALSE;
  char imported[256] = {0};

  // Get full pathname
  char *pathname = dt_get_lightroom_xmp(dev->image_storage.id);

  if (!pathname)
  {
    dt_control_log(_("cannot find Lightroom XMP!"));
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
    dt_control_log(_("(%s) not a Lightroom XMP!"), pathname);
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
    dt_control_log(_("(%s) not a Lightroom XMP!"), pathname);
    return;
  }

  //  Look for attributes in the Description

  dt_iop_clipping_params_t pc;
  memset(&pc, 0, sizeof(pc));
  gboolean has_crop = FALSE;

  dt_iop_exposure_params_t pe;
  memset(&pe, 0, sizeof(pe));
  gboolean has_exposure = FALSE;

  dt_iop_vignette_params_t pv;
  memset(&pv, 0, sizeof(pv));
  gboolean has_vignette = FALSE;

  gboolean has_tags = FALSE;

  int n_import = 0;

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
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "HasCrop"))
    {
      if (!xmlStrcmp(value, (const xmlChar *)"True"))
      {
        has_crop = TRUE;
        n_import++;
      }
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "Blacks2012"))
    {
      int v = atoi((char *)value);
      if (v != 0)
      {
        has_exposure = TRUE;
        pe.black = lr2dt_exposure((float)v);
        n_import++;
      }
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "PostCropVignetteAmount"))
    {
      int v = atoi((char *)value);
      if (v != 0)
      {
        has_vignette = TRUE;
        pv.brightness = lr2dt_vignette_gain((float)v);
        n_import++;
      }
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "PostCropVignetteStyle"))
    {
      int v = atoi((char *)value);
      if (v == 1) // Highlight Priority
        pv.saturation = -0.800;
      else // Color Priority & Paint Overlay
        pv.saturation = -0.300;
    }
    else if (!xmlStrcmp(attribute->name, (const xmlChar *) "PostCropVignetteFeather"))
    {
      int v = atoi((char *)value);
      if (v != 0)
      {
        pv.scale = lr2dt_vignette_scale((float)v);
        pv.falloff_scale = lr2dt_vignette_falloff_scale((float)v);
      }
    }

    xmlFree(value);
    attribute = attribute->next;
  }

  //  Look for tags (subject/Bag/*)

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
        }
        tagNode = tagNode->next;
      }
    }
    entryNode = entryNode->next;
  }

  xmlFreeDoc(doc);

  //  Integrates into the history all the imported iop

  if (has_crop)
  {
    dt_dev_history_item_t *hist = _new_hist_for (dev, "clipping");
    dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)hist->params;

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

    dt_add_hist (dev, hist, imported);
    refresh_needed=TRUE;
  }

  if (has_exposure)
  {
    dt_dev_history_item_t *hist = _new_hist_for (dev, "exposure");
    dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)hist->params;

    p->black = pe.black;

    dt_add_hist (dev, hist, imported);
    refresh_needed=TRUE;
  }

  if (has_vignette)
  {
    dt_dev_history_item_t *hist = _new_hist_for (dev, "vignette");
    dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)hist->params;

    p->brightness = pv.brightness;
    p->scale = pv.scale;
    p->falloff_scale = pv.falloff_scale;
    p->whratio = 1.350;
    p->autoratio = FALSE;
    p->saturation = pv.saturation;
    p->dithering = DITHER_8BIT;

    dt_add_hist (dev, hist, imported);
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
    // some hist have been created, display them
    strcat(imported, " ");
    if (n_import==1)
      strcat(imported, _("has been imported"));
    else
      strcat(imported, _("have been imported"));
    dt_control_log(imported);

    /* signal history changed */
    dt_control_signal_raise(darktable.signals,DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
    dt_dev_reprocess_center(dev);
  }
}
