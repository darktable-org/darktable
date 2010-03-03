/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#include <assert.h>
#include <string.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#ifdef HAVE_GEGL
  #include <gegl.h>
#endif
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "common/image_cache.h"
#include "gui/gtk.h"
#include "gui/draw.h"

DT_MODULE(1)

/** core libraw functionality wrapper, for convenience in darkroom mode. */

typedef struct dt_iop_rawimport_params_t
{ // exactly matches dt_image_raw_parameters_t + two floats as in dt_image_t
  float raw_denoise_threshold, raw_auto_bright_threshold;
  unsigned raw_wb_auto : 1, raw_wb_cam : 1, raw_cmatrix : 1,
           raw_no_auto_bright : 1, raw_demosaic_method : 2,
           raw_med_passes : 4, raw_four_color_rgb : 1,
           raw_highlight : 4,
           fill1 : 9;
  int8_t raw_user_flip;
}
dt_iop_rawimport_params_t;

typedef struct dt_iop_rawimport_gui_data_t
{
  // GtkCheckButton *wb_auto, *cmatrix, *wb_cam;
  GtkCheckButton *auto_bright, *four_color_rgb;
  GtkComboBox *demosaic_method, *highlight;
  GtkScale *auto_bright_threshold, *denoise_threshold;
  GtkSpinButton *med_passes;
}
dt_iop_rawimport_gui_data_t;

const char *name()
{
  return _("raw import");
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{ // never called, this dummy module is always disabled.
  memcpy(o, i, sizeof(float)*3*roi_out->width*roi_out->height);
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{ // not necessary
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = NULL;
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
}

static void reimport_button_callback (GtkButton *button, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  if(module->dt->gui->reset) return;
  dt_iop_rawimport_params_t *p = (dt_iop_rawimport_params_t *)module->params;
  pthread_mutex_lock(&module->dev->history_mutex);
  // set image params
  module->dev->image->raw_denoise_threshold      = p->raw_denoise_threshold;
  module->dev->image->raw_auto_bright_threshold  = p->raw_auto_bright_threshold;
  module->dev->image->raw_params.wb_auto         = p->raw_wb_auto;
  module->dev->image->raw_params.wb_cam          = p->raw_wb_cam;
  module->dev->image->raw_params.cmatrix         = p->raw_cmatrix;
  module->dev->image->raw_params.no_auto_bright  = p->raw_no_auto_bright;
  module->dev->image->raw_params.demosaic_method = p->raw_demosaic_method;
  module->dev->image->raw_params.med_passes      = p->raw_med_passes;
  module->dev->image->raw_params.four_color_rgb  = p->raw_four_color_rgb;
  module->dev->image->raw_params.highlight       = p->raw_highlight;
  module->dev->image->raw_params.user_flip       = p->raw_user_flip;
  // also write to db
  dt_image_cache_flush(module->dev->image);
  // force reloading raw image using the params.
  dt_dev_raw_reload(module->dev);
  pthread_mutex_unlock(&module->dev->history_mutex);
  dt_control_gui_queue_draw();
}

static void togglebutton_callback (GtkToggleButton *toggle, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_rawimport_params_t *p = (dt_iop_rawimport_params_t *)self->params;
  dt_iop_rawimport_gui_data_t *g = (dt_iop_rawimport_gui_data_t *)self->gui_data;
  int active = gtk_toggle_button_get_active(toggle);
  /*if      (toggle == GTK_TOGGLE_BUTTON(g->wb_auto))
  {
    p->raw_wb_auto = active;
    p->raw_wb_cam  = !active;
    darktable.gui->reset = 1; gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->wb_cam), !active); darktable.gui->reset = 0;
  }
  else if (toggle == GTK_TOGGLE_BUTTON(g->wb_cam))
  {
    p->raw_wb_cam  = active;
    // p->raw_wb_auto = !active;
    // darktable.gui->reset = 1; gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->wb_auto), !active); darktable.gui->reset = 0;
  }*/
  // else if (toggle == GTK_TOGGLE_BUTTON(g->cmatrix))        p->raw_cmatrix = active;
  if      (toggle == GTK_TOGGLE_BUTTON(g->auto_bright))    p->raw_no_auto_bright = !active;
  else if (toggle == GTK_TOGGLE_BUTTON(g->four_color_rgb)) p->raw_four_color_rgb = active;
}

static void highlight_callback (GtkComboBox *box, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_rawimport_params_t *p = (dt_iop_rawimport_params_t *)self->params;
  p->raw_highlight = gtk_combo_box_get_active(box);
  if(p->raw_highlight == 3) p->raw_highlight = 5;
}

static void demosaic_callback (GtkComboBox *box, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_rawimport_params_t *p = (dt_iop_rawimport_params_t *)self->params;
  p->raw_demosaic_method = gtk_combo_box_get_active(box);
}

static void median_callback (GtkSpinButton *spin, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_rawimport_params_t *p = (dt_iop_rawimport_params_t *)self->params;
  p->raw_med_passes = gtk_spin_button_get_value(spin);
}

static void scale_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_rawimport_gui_data_t *g = (dt_iop_rawimport_gui_data_t *)self->gui_data;
  dt_iop_rawimport_params_t *p = (dt_iop_rawimport_params_t *)self->params;
  if      (range == GTK_RANGE(g->auto_bright_threshold)) p->raw_auto_bright_threshold = gtk_range_get_value(range)*0.01f;
  else if (range == GTK_RANGE(g->denoise_threshold))     p->raw_denoise_threshold = gtk_range_get_value(range);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_rawimport_gui_data_t *g = (dt_iop_rawimport_gui_data_t *)self->gui_data;
  dt_iop_rawimport_params_t *p = (dt_iop_rawimport_params_t *)self->params;
  // update gui info from params.
  gtk_range_set_value(GTK_RANGE(g->denoise_threshold), p->raw_denoise_threshold);
  gtk_range_set_value(GTK_RANGE(g->auto_bright_threshold), p->raw_auto_bright_threshold*100.0);
  // gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->wb_auto), p->raw_wb_auto);
  // gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->wb_cam), p->raw_wb_cam);
  // gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->cmatrix), p->raw_cmatrix);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->auto_bright), !p->raw_no_auto_bright);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->four_color_rgb), p->raw_four_color_rgb);
  gtk_combo_box_set_active(g->demosaic_method, p->raw_demosaic_method);
  gtk_combo_box_set_active(g->highlight, p->raw_highlight < 3 ? p->raw_highlight : 3);
  gtk_spin_button_set_value(g->med_passes, p->raw_med_passes);
}

static void resetbutton_callback (GtkButton *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_rawimport_params_t tmp = (dt_iop_rawimport_params_t)
     {0.0, 0.01, 0, 1, 1, 0, 2, 0, 0, 0, 0, -1};
  memcpy(self->params, &tmp, sizeof(dt_iop_rawimport_params_t));
  dt_iop_rawimport_gui_data_t *g = (dt_iop_rawimport_gui_data_t *)self->gui_data;
  dt_iop_rawimport_params_t *p = (dt_iop_rawimport_params_t *)self->params;
  gtk_range_set_value(GTK_RANGE(g->denoise_threshold), p->raw_denoise_threshold);
  gtk_range_set_value(GTK_RANGE(g->auto_bright_threshold), p->raw_auto_bright_threshold*100.0);
  // gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->wb_auto), p->raw_wb_auto);
  // gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->wb_cam), p->raw_wb_cam);
  // gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->cmatrix), p->raw_cmatrix);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->auto_bright), !p->raw_no_auto_bright);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->four_color_rgb), p->raw_four_color_rgb);
  gtk_combo_box_set_active(g->demosaic_method, p->raw_demosaic_method);
  gtk_combo_box_set_active(g->highlight, p->raw_highlight < 3 ? p->raw_highlight : 3);
  gtk_spin_button_set_value(g->med_passes, p->raw_med_passes);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_rawimport_params_t));
  dt_iop_rawimport_params_t *p = (dt_iop_rawimport_params_t *)module->params;
  module->default_params = malloc(sizeof(dt_iop_rawimport_params_t));
  module->default_enabled = 0;
  module->params_size = sizeof(dt_iop_rawimport_params_t);
  module->gui_data = NULL;
  module->priority = 100;
  module->hide_enable_button = 1;
  p->raw_denoise_threshold     = module->dev->image->raw_denoise_threshold;
  p->raw_auto_bright_threshold = module->dev->image->raw_auto_bright_threshold;
  p->raw_wb_auto               = module->dev->image->raw_params.wb_auto;
  p->raw_wb_cam                = module->dev->image->raw_params.wb_cam;
  p->raw_cmatrix               = module->dev->image->raw_params.cmatrix;
  p->raw_no_auto_bright        = module->dev->image->raw_params.no_auto_bright;
  p->raw_demosaic_method       = module->dev->image->raw_params.demosaic_method;
  p->raw_med_passes            = module->dev->image->raw_params.med_passes;
  p->raw_four_color_rgb        = module->dev->image->raw_params.four_color_rgb;
  p->raw_highlight             = module->dev->image->raw_params.highlight;
  p->raw_user_flip             = module->dev->image->raw_params.user_flip;
  memcpy(module->default_params, p, sizeof(dt_iop_rawimport_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

static gchar *fv_callback(GtkScale *scale, gdouble value)
{
  int digits = gtk_scale_get_digits(scale);
  return g_strdup_printf("%#*.*f", 4+1+digits, digits, value);
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_rawimport_gui_data_t));
  dt_iop_rawimport_gui_data_t *g = (dt_iop_rawimport_gui_data_t *)self->gui_data;
  // dt_iop_rawimport_params_t *p = (dt_iop_rawimport_params_t *)self->params;

  self->widget = gtk_vbox_new(FALSE, 0);

  GtkWidget *label;
  label = gtk_label_new(_("warning: these are cryptic low level\nsettings! you probably don't have to\nchange them."));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(self->widget), label, FALSE, FALSE, 5);

  // g->wb_auto        = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("auto white balance")));
  // gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->wb_auto), TRUE, TRUE, 0);
  // g->wb_cam         = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("camera white balance")));
  // gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->wb_cam), TRUE, TRUE, 0);
  g->auto_bright    = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("auto exposure")));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->auto_bright), TRUE, TRUE, 0);
  // g->cmatrix        = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("use color matrix")));
  // gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->cmatrix), TRUE, TRUE, 0);

  GtkWidget *hbox   = gtk_hbox_new(FALSE, 0);
  GtkWidget *vbox1  = gtk_vbox_new(TRUE, 0);
  GtkWidget *vbox2  = gtk_vbox_new(TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), vbox1, FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(hbox), vbox2, TRUE, TRUE, 5);

  label = gtk_label_new(_("exposure"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(vbox1), label, TRUE, TRUE, 0);
  g->auto_bright_threshold = GTK_SCALE(gtk_hscale_new_with_range(0.1, 2., 0.05));
  gtk_object_set(GTK_OBJECT(g->auto_bright_threshold), "tooltip-text", _("percentage of bright values\nto be clipped out"), NULL);
  gtk_scale_set_digits(g->auto_bright_threshold, 1);
  gtk_scale_set_value_pos(g->auto_bright_threshold, GTK_POS_LEFT);
  gtk_box_pack_start(GTK_BOX(vbox2), GTK_WIDGET(g->auto_bright_threshold), TRUE, TRUE, 0);

  label = gtk_label_new(_("denoise"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(vbox1), label, TRUE, TRUE, 0);
  g->denoise_threshold = GTK_SCALE(gtk_hscale_new_with_range(0.0, 1000.0, 1.0));
  gtk_scale_set_digits(g->denoise_threshold, 1);
  gtk_scale_set_value_pos(g->denoise_threshold, GTK_POS_LEFT);
  gtk_box_pack_start(GTK_BOX(vbox2), GTK_WIDGET(g->denoise_threshold), TRUE, TRUE, 0);

  label = gtk_label_new(_("median passes"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(vbox1), label, TRUE, TRUE, 0);
  g->med_passes = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 31, 1));
  gtk_object_set(GTK_OBJECT(g->med_passes), "tooltip-text", _("number of 3x3 median filter passes\non R-G and B-G after demosaicing"), NULL);
  gtk_box_pack_start(GTK_BOX(vbox2), GTK_WIDGET(g->med_passes), TRUE, TRUE, 0);

  label = gtk_label_new(_("highlight handling"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(vbox1), label, TRUE, TRUE, 0);
  g->highlight = GTK_COMBO_BOX(gtk_combo_box_new_text());
  gtk_combo_box_append_text(g->highlight, _("clip"));
  gtk_combo_box_append_text(g->highlight, _("unclip"));
  gtk_combo_box_append_text(g->highlight, _("blend"));
  gtk_combo_box_append_text(g->highlight, _("rebuild"));
  gtk_box_pack_start(GTK_BOX(vbox2), GTK_WIDGET(g->highlight), TRUE, TRUE, 0);

  label = gtk_label_new(_("demosaic method"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(vbox1), label, TRUE, TRUE, 0);
  g->demosaic_method = GTK_COMBO_BOX(gtk_combo_box_new_text());
  gtk_combo_box_append_text(g->demosaic_method, C_("demosaic", "linear"));
  gtk_combo_box_append_text(g->demosaic_method, _("VNG"));
  gtk_combo_box_append_text(g->demosaic_method, _("PPG"));
  gtk_combo_box_append_text(g->demosaic_method, _("AHD"));
  gtk_box_pack_start(GTK_BOX(vbox2), GTK_WIDGET(g->demosaic_method), TRUE, TRUE, 0);

  g->four_color_rgb = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("four color rgb")));
  gtk_object_set(GTK_OBJECT(g->four_color_rgb), "tooltip-text", _("demosaic treating RGGB as\nfour distinct colors,\nuse if you get mazing artifacts"), NULL);
  gtk_box_pack_start(GTK_BOX(vbox1), gtk_label_new(""), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox2), GTK_WIDGET(g->four_color_rgb), TRUE, TRUE, 0);

  GtkWidget *reset = gtk_button_new_with_label(_("reset"));
  gtk_object_set(GTK_OBJECT(reset), "tooltip-text", _("reset raw loading parameters\nto darktable's defaults"), NULL);
  gtk_box_pack_start(GTK_BOX(vbox1), GTK_WIDGET(reset), TRUE, TRUE, 0);
  GtkWidget *reload = gtk_button_new_with_label(_("re-import"));
  gtk_object_set(GTK_OBJECT(reload), "tooltip-text", _("trigger re-import of the raw image"), NULL);
  gtk_box_pack_start(GTK_BOX(vbox2), GTK_WIDGET(reload), TRUE, TRUE, 0);
  
  // gui_update(self);
  dt_iop_rawimport_params_t *p = (dt_iop_rawimport_params_t *)self->params;
  gtk_range_set_value(GTK_RANGE(g->denoise_threshold), p->raw_denoise_threshold);
  gtk_range_set_value(GTK_RANGE(g->auto_bright_threshold), p->raw_auto_bright_threshold*100.0);
  // gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->wb_auto), p->raw_wb_auto);
  // gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->wb_cam), p->raw_wb_cam);
  // gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->cmatrix), p->raw_cmatrix);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->auto_bright), !p->raw_no_auto_bright);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->four_color_rgb), p->raw_four_color_rgb);
  gtk_combo_box_set_active(g->demosaic_method, p->raw_demosaic_method);
  gtk_combo_box_set_active(g->highlight, p->raw_highlight < 3 ? p->raw_highlight : 3);
  gtk_spin_button_set_value(g->med_passes, p->raw_med_passes);

  // g_signal_connect (G_OBJECT (g->wb_auto),        "toggled", G_CALLBACK (togglebutton_callback), self);
  // g_signal_connect (G_OBJECT (g->wb_cam),         "toggled", G_CALLBACK (togglebutton_callback), self);
  // g_signal_connect (G_OBJECT (g->cmatrix),        "toggled", G_CALLBACK (togglebutton_callback), self);
  g_signal_connect (G_OBJECT (g->auto_bright),    "toggled", G_CALLBACK (togglebutton_callback), self);
  g_signal_connect (G_OBJECT (g->four_color_rgb), "toggled", G_CALLBACK (togglebutton_callback), self);
  g_signal_connect (G_OBJECT (g->demosaic_method), "changed", G_CALLBACK (demosaic_callback), self);
  g_signal_connect (G_OBJECT (g->highlight),       "changed", G_CALLBACK (highlight_callback), self);
  g_signal_connect (G_OBJECT (g->med_passes), "value-changed", G_CALLBACK (median_callback), self);
  g_signal_connect (G_OBJECT (g->auto_bright_threshold), "value-changed", G_CALLBACK (scale_callback), self);
  g_signal_connect (G_OBJECT (g->denoise_threshold),     "value-changed", G_CALLBACK (scale_callback), self);
  g_signal_connect (G_OBJECT (g->auto_bright_threshold), "format-value", G_CALLBACK (fv_callback), self);
  g_signal_connect (G_OBJECT (g->denoise_threshold),     "format-value", G_CALLBACK (fv_callback), self);
  g_signal_connect (G_OBJECT (reload),     "clicked", G_CALLBACK (reimport_button_callback), self);
  g_signal_connect (G_OBJECT (reset),      "clicked", G_CALLBACK (resetbutton_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

