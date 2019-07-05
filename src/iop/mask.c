/*
    This file is part of darktable,
    copyright (c) 2019 Jacques Le Clerc

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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "bauhaus/bauhaus.h"
#include "develop/imageop.h"
#include "dtgtk/button.h"
#include "dtgtk/resetlabel.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <stdlib.h>

// this is the version of the modules parameters,
// and includes version information about compile-time dt
DT_MODULE_INTROSPECTION(1, dt_iop_mask_params_t)

typedef enum dt_iop_mask_t
{
  DT_IOP_MASK_RED = 0,
  DT_IOP_MASK_GREEN = 1,
  DT_IOP_MASK_BLUE = 2,
  DT_IOP_MASK_BLACK = 3,
  DT_IOP_MASK_WHITE = 4
} dt_iop_mask_t;

typedef struct dt_iop_mask_params_t
{
  dt_iop_mask_t mask_color;
} dt_iop_mask_params_t;

typedef struct dt_iop_mask_gui_data_t
{
  GtkWidget *mask_area;
  GtkWidget *mask_color;
  GtkWidget *color;
} dt_iop_mask_gui_data_t;

const char *name()
{
  return _("mask");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_group()
{
  return IOP_GROUP_CORRECT;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_rgb;
}


void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ibuf, void *const obuf,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const int width = roi_in->width;
  const int height = roi_in->height;
  const int ch = piece->colors;
  dt_iop_mask_params_t *p = (dt_iop_mask_params_t *)piece->data;
  float r, g, b;

  const dt_iop_order_iccprofile_info_t *const srgb_profile = dt_ioppr_add_profile_info_to_list(self->dev, DT_COLORSPACE_SRGB, "", INTENT_PERCEPTUAL);
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);

  // Work profile to SRGB profile
  if (work_profile && srgb_profile)
    dt_ioppr_transform_image_colorspace_rgb(ibuf, obuf, width, height, work_profile, srgb_profile, "mask");
  else
    memcpy(obuf, ibuf, width * height * ch * sizeof(float));

  switch(p->mask_color)
  {
   default:
   case DT_IOP_MASK_RED: r=+1.0; g=b=0.0; break;
   case DT_IOP_MASK_GREEN: g=+1.0; r=b=0.0; break;
   case DT_IOP_MASK_BLUE: b=+1.0; r=g=0.0; break;
   case DT_IOP_MASK_BLACK: b=r=g=0.0; break;
   case DT_IOP_MASK_WHITE: b=r=g=+1.0; break;
  }

  const float *pi = obuf;
  float *po = obuf;
  for (int h=0; h<height; h++)
    for (int w=0; w<width; w++)
    {
      *po++ = r;    // Red
      *po++ = g;    // Green
      *po++ = b;    // Blue
      pi+=3;
      *po++ = *pi++;  // Alpha
    }

  // SRGB profile to work profile
  if (work_profile && srgb_profile)
    dt_ioppr_transform_image_colorspace_rgb(obuf, obuf, width, height, srgb_profile, work_profile, "GMIC process");
}


/** init, cleanup, commit to pipeline */
void init(dt_iop_module_t *module)
{
  // we don't need global data:
  module->global_data = NULL; // malloc(sizeof(dt_iop_mask_global_data_t));
  module->params = calloc(1, sizeof(dt_iop_mask_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_mask_params_t));
  // our module is disabled by default
  module->default_enabled = 0;
  // order has to be changed by editing the dependencies in tools/iop_dependencies.py
  module->params_size = sizeof(dt_iop_mask_params_t);
  module->gui_data = NULL;
  // init defaults:
  dt_iop_mask_params_t tmp = (dt_iop_mask_params_t){ DT_IOP_MASK_RED };

  memcpy(module->params, &tmp, sizeof(dt_iop_mask_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_mask_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}


static void mask_color_callback(GtkWidget *w, dt_iop_module_t *self)
{
  // this is important to avoid cycles!
  if(darktable.gui->reset) return;

  dt_iop_mask_params_t *p = (dt_iop_mask_params_t *)self->params;
  p->mask_color = dt_bauhaus_combobox_get(w);

  // let core know of the changes
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_update(dt_iop_module_t *self)
{
  // let gui match current parameters:
  dt_iop_mask_gui_data_t *g = (dt_iop_mask_gui_data_t *)self->gui_data;
  dt_iop_mask_params_t *p = (dt_iop_mask_params_t *)self->params;
  dt_bauhaus_combobox_set(g->mask_color, p->mask_color);
}

void gui_init(dt_iop_module_t *self)
{
  // init the combobox
  self->gui_data = malloc(sizeof(dt_iop_mask_gui_data_t));
  dt_iop_mask_gui_data_t *g = (dt_iop_mask_gui_data_t *)self->gui_data;
//  dt_iop_mask_params_t *p = (dt_iop_mask_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));

  g->mask_area = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(self->widget), g->mask_area, TRUE, TRUE, 0);

  // Mask combobox
  g->mask_color = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->mask_color, NULL, _("mask"));
  gtk_box_pack_start(GTK_BOX(g->mask_area), g->mask_color, TRUE, TRUE, 0);
  dt_bauhaus_combobox_add(g->mask_color, _("Red"));
  dt_bauhaus_combobox_add(g->mask_color, _("Green"));
  dt_bauhaus_combobox_add(g->mask_color, _("Blue"));
  dt_bauhaus_combobox_add(g->mask_color, _("Black"));
  dt_bauhaus_combobox_add(g->mask_color, _("White"));
  gtk_widget_set_tooltip_text(g->mask_color, _("Mask"));
  g_signal_connect(G_OBJECT(g->mask_color), "value-changed", G_CALLBACK(mask_color_callback), self);

  g->color = dtgtk_togglebutton_new(dtgtk_cairo_paint_color, CPF_STYLE_BOX, NULL);
  gtk_box_pack_start(GTK_BOX(g->mask_area), g->color, FALSE, FALSE, 0);
}

void gui_cleanup(dt_iop_module_t *self)
{
  // nothing else necessary, gtk will clean up the slider.
  free(self->gui_data);
  self->gui_data = NULL;
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
