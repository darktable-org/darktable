/*
    This file is part of darktable,
    copyright (c) 2011 johannes hanika.

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
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <gdk/gdkkeysyms.h>
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "control/conf.h"
#include "common/debug.h"
#include "dtgtk/label.h"
#include "dtgtk/slider.h"
#include "dtgtk/resetlabel.h"
#include "dtgtk/togglebutton.h"
#include "dtgtk/button.h"
#include "gui/gtk.h"
#include "gui/draw.h"
#include "gui/presets.h"

DT_MODULE(1)

typedef struct dt_iop_borders_params_t
{
  float color[3]; // border color
  float aspect;   // aspect ratio of the outer frame w/h
  float size;     // border width relative to overal frame width
}
dt_iop_borders_params_t;

typedef struct dt_iop_borders_gui_data_t
{
  GtkDarktableSlider *size;
  GtkComboBoxEntry *aspect;
  float aspect_ratios[8];
}
dt_iop_borders_gui_data_t;

typedef struct dt_iop_borders_params_t dt_iop_borders_data_t;

const char *name()
{
  return _("framing");
}

int
groups ()
{
  return IOP_GROUP_EFFECT;
}

// 1st pass: how large would the output be, given this input roi?
// this is always called with the full buffer before processing.
void
modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in)
{
  *roi_out = *roi_in;
  dt_iop_borders_data_t *d = (dt_iop_borders_data_t *)piece->data;

  // min width: constant ratio based on size:
  roi_out->width = (float)roi_in->width / (1.0f - d->size);
  // corresponding height: determined by aspect ratio:
  roi_out->height = (float)roi_out->width / d->aspect;
  // insane settings used?
  if(roi_out->height < (float)roi_in->height / (1.0f - d->size))
  {
    roi_out->height = (float)roi_in->height / (1.0f - d->size);
    roi_out->width  = (float)roi_out->height * d->aspect;
  }

  // sanity check.
  if(roi_out->width  < 1) roi_out->width  = 1;
  if(roi_out->height < 1) roi_out->height = 1;
}

// 2nd pass: which roi would this operation need as input to fill the given output region?
void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  // dt_iop_borders_data_t *d = (dt_iop_borders_data_t *)piece->data;
  *roi_in = *roi_out;
  const int bw = (piece->buf_out.width  - piece->buf_in.width ) * roi_out->scale;
  const int bh = (piece->buf_out.height - piece->buf_in.height) * roi_out->scale;
  // don't request outside image (no px for borders)
  roi_in->x = MAX(roi_out->x - bw/2, 0);
  roi_in->y = MAX(roi_out->y - bh/2, 0);
  // subtract upper left border from dimensions
  roi_in->width  -= MAX(bw/2 - roi_out->x, 0);
  roi_in->height -= MAX(bh/2 - roi_out->y, 0);
  // subtract lower right border from dimensions
  roi_in->width  -= roi_out->scale * MAX((roi_in->x + roi_in->width )/roi_out->scale - (piece->buf_in.width ), 0);
  roi_in->height -= roi_out->scale * MAX((roi_in->y + roi_in->height)/roi_out->scale - (piece->buf_in.height), 0);
  // don't request nothing or outside roi
  roi_in->width  = MIN(roi_out->scale * piece->buf_in.width,  MAX(1, roi_in->width ));
  roi_in->height = MIN(roi_out->scale * piece->buf_in.height, MAX(1, roi_in->height));
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_borders_data_t *d = (dt_iop_borders_data_t *)piece->data;
  const int ch = piece->colors;
  const int in_stride  = ch*roi_in->width;
  const int out_stride = ch*roi_out->width;
  const int cp_stride = in_stride*sizeof(float);

  const int bw = (piece->buf_out.width  - piece->buf_in.width ) * roi_in->scale;
  const int bh = (piece->buf_out.height - piece->buf_in.height) * roi_in->scale;
  const int bx = MAX(bw/2 - roi_out->x, 0);
  const int by = MAX(bh/2 - roi_out->y, 0);

  // sse-friendly color copy (stupidly copy whole buffer, /me lazy ass)
  const float col[4] = {d->color[0], d->color[1], d->color[2], 0.0f};
  float *buf = (float *)ovoid;
  for(int k=0;k<roi_out->width*roi_out->height;k++, buf+=4) memcpy(buf, col, sizeof(float)*4);
  // blit image inside border and fill border with bg color
  for(int j=0;j<roi_in->height;j++)
  {
    float *out = ((float *)ovoid) + (j + by)*out_stride + ch * bx;
    const float *in  = ((float *)ivoid) + j*in_stride;
    memcpy(out, in, cp_stride);
  }
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_borders_params_t *p = (dt_iop_borders_params_t *)p1;
  dt_iop_borders_data_t *d = (dt_iop_borders_data_t *)piece->data;
  memcpy(d, p, sizeof(dt_iop_borders_params_t));
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_borders_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}

void init_presets (dt_iop_module_t *self)
{
  dt_iop_borders_params_t p = (dt_iop_borders_params_t)
  {
    {0.0f, 0.0f, 0.0f}, 3.0f/2.0f, 0.1f
  };
  dt_gui_presets_add_generic(_("15:10 postcard black"), self->op, &p, sizeof(p), 1);
  p.color[0] = p.color[1] = p.color[2] = 1.0f;
  dt_gui_presets_add_generic(_("15:10 postcard white"), self->op, &p, sizeof(p), 1);
}

static void
aspect_changed (GtkComboBox *combo, dt_iop_module_t *self)
{
  dt_iop_borders_gui_data_t *g = (dt_iop_borders_gui_data_t *)self->gui_data;
  dt_iop_borders_params_t *p = (dt_iop_borders_params_t *)self->params;
  int which = gtk_combo_box_get_active(combo);
  if (which < 0)
  {
    p->aspect = self->dev->image->width/(float)self->dev->image->height;
    gchar *text = gtk_combo_box_get_active_text(combo);
    if(text)
    {
      gchar *c = text;
      while(*c != ':' && *c != '/' && c < text + strlen(text)) c++;
      if(c < text + strlen(text))
      {
        *c = '\0';
        c++;
        p->aspect = atof(text) / atof(c);
      }
      g_free(text);
    }
  }
  else if (which < 8)
  {
    if(self->dev->image->height > self->dev->image->width)
      p->aspect = 1.0/g->aspect_ratios[which];
    else
      p->aspect = g->aspect_ratios[which];
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
size_callback (GtkDarktableSlider *slider, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_borders_params_t *p = (dt_iop_borders_params_t *)self->params;
  p->size = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// TODO: color callback

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_borders_gui_data_t *g = (dt_iop_borders_gui_data_t *)self->gui_data;
  dt_iop_borders_params_t *p = (dt_iop_borders_params_t *)self->params;
  dtgtk_slider_set_value(g->size, p->size);
  // TODO: find aspect ratio from float:
  gtk_combo_box_set_active(GTK_COMBO_BOX(g->aspect), 0);
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_borders_data_t));
  module->params = malloc(sizeof(dt_iop_borders_params_t));
  module->default_params = malloc(sizeof(dt_iop_borders_params_t));
  module->default_enabled = 0;
  module->params_size = sizeof(dt_iop_borders_params_t);
  module->gui_data = NULL;
  module->priority = 911;
  dt_iop_borders_params_t tmp = (dt_iop_borders_params_t)
  {
    {0.0f, 0.0f, 0.0f}, 3.0f/2.0f, 0.1f
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_borders_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_borders_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void gui_init(struct dt_iop_module_t *self)
{
  // TODO: clean out!
  // TODO: insert color picker!
  self->gui_data = malloc(sizeof(dt_iop_borders_gui_data_t));
  dt_iop_borders_gui_data_t *g = (dt_iop_borders_gui_data_t *)self->gui_data;
  dt_iop_borders_params_t *p = (dt_iop_borders_params_t *)self->params;

  self->widget = gtk_table_new(3, 2, FALSE);
  gtk_table_set_row_spacings(GTK_TABLE(self->widget), DT_GUI_IOP_MODULE_CONTROL_SPACING);
  gtk_table_set_col_spacings(GTK_TABLE(self->widget), DT_GUI_IOP_MODULE_CONTROL_SPACING);

  g->size = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR, 0.0f, 0.5f, 0.1, p->size, 2));
  dtgtk_slider_set_label(g->size, _("border size"));
  dtgtk_slider_set_unit(g->size, "%");
  g_signal_connect (G_OBJECT (g->size), "value-changed", G_CALLBACK (size_callback), self);
  g_object_set(G_OBJECT(g->size), "tooltip-text", _("size of the border in percent of the full image"), (char *)NULL);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->size), 0, 2, 0, 1, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  GtkWidget *label = gtk_label_new(_("aspect"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(label), 0, 1, 1, 2, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  g->aspect = GTK_COMBO_BOX_ENTRY(gtk_combo_box_entry_new_text());
  gtk_combo_box_append_text(GTK_COMBO_BOX(g->aspect), _("image"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(g->aspect), _("golden cut"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(g->aspect), _("1:2"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(g->aspect), _("3:2"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(g->aspect), _("4:3"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(g->aspect), _("square"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(g->aspect), _("DIN"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(g->aspect), _("16:9"));
  // dt_gui_key_accel_register(GDK_CONTROL_MASK, GDK_x, key_accel_callback, (void *)self);

  // TODO: get preset from float (..and maybe even just code it once in gui_update)
  gtk_combo_box_set_active(GTK_COMBO_BOX(g->aspect), 0);
  g_signal_connect (G_OBJECT (g->aspect), "changed", G_CALLBACK (aspect_changed), self);
  g_object_set(G_OBJECT(g->aspect), "tooltip-text", _("set the aspect ratio (w:h)\npress ctrl-x to swap sides"), (char *)NULL);

  GtkBox *hbox = GTK_BOX(gtk_hbox_new(FALSE, 5));
  gtk_box_pack_start(hbox, GTK_WIDGET(g->aspect), TRUE, TRUE, 0);
  GtkWidget *button = dtgtk_button_new(dtgtk_cairo_paint_aspectflip, CPF_STYLE_FLAT);
  // TODO: what about this?
  //g_signal_connect (G_OBJECT (button), "clicked", G_CALLBACK (aspect_flip), self);
  g_object_set(G_OBJECT(button), "tooltip-text", _("swap the aspect ratio (ctrl-x)"), (char *)NULL);
  gtk_box_pack_start(hbox, button, TRUE, FALSE, 0);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(hbox), 1, 2, 1, 2, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  g->aspect_ratios[0] = self->dev->image->width/(float)self->dev->image->height;
  if(g->aspect_ratios[0] < 1.0)
    g->aspect_ratios[0] = 1.0 / g->aspect_ratios[0];
  g->aspect_ratios[1] = 1.6280;
  g->aspect_ratios[2] = 2.0/1.0;
  g->aspect_ratios[3] = 3.0/2.0;
  g->aspect_ratios[4] = 4.0/3.0;
  g->aspect_ratios[5] = 1.0;
  g->aspect_ratios[6] = sqrtf(2.0);
  g->aspect_ratios[7] = 16.0f/9.0f;

#if 0 // gui_update
  if(self->dev->image->height > self->dev->image->width)
    g->current_aspect = 1.0/g->aspect_ratios[act];
  else
    g->current_aspect = g->aspect_ratios[act];
#endif
}

void reload_defaults(dt_iop_module_t *module)
{
  // TODO: only affect default_params
#if 0
  g->aspect_ratios[0] = self->dev->image->width/(float)self->dev->image->height;
  if(g->aspect_ratios[0] < 1.0f)
    g->aspect_ratios[0] = 1.0f / g->aspect_ratios[1];
  
  // TODO: no current aspc
  if(g->current_aspect > 1.0f && self->dev->image->height > self->dev->image->width)
    g->current_aspect = 1.0f/g->aspect_ratios[act];
  else
    g->current_aspect = g->aspect_ratios[act];
#endif
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
