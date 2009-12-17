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
#include "gui/gtk.h"
#include "gui/draw.h"

/** rotate an image, then clip the buffer. */

typedef struct dt_iop_rawimport_params_t
{
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
  GtkCheckButton *wb_auto, *wb_cam, *cmatrix, *auto_bright, *four_color_rgb;
  GtkComboBox *demosaic_method, *highlight;
  GtkSpinButton *med_passes;
}
dt_iop_rawimport_gui_data_t;

/*typedef struct dt_iop_rawimport_data_t
{
}
dt_iop_rawimport_data_t;*/

const char *name()
{
  return _("raw import");
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  memcpy(o, i, sizeof(float)*3*roi_out->width*roi_out->height);
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // dt_iop_rawimport_params_t *p = (dt_iop_rawimport_params_t *)p1;
  // dt_iop_rawimport_data_t *d = (dt_iop_rawimport_data_t *)piece->data;
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = NULL; // malloc(sizeof(dt_iop_rawimport_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // free(piece->data);
}

#if 0
void cx_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_rawimport_params_t *p = (dt_iop_rawimport_params_t *)self->params;
  p->cx = gtk_range_get_value(range);
  dt_dev_add_history_item(darktable.develop, self);
}

void cy_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_rawimport_params_t *p = (dt_iop_rawimport_params_t *)self->params;
  p->cy = gtk_range_get_value(range);
  dt_dev_add_history_item(darktable.develop, self);
}

void cw_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_rawimport_params_t *p = (dt_iop_rawimport_params_t *)self->params;
  p->cw = gtk_range_get_value(range);
  dt_dev_add_history_item(darktable.develop, self);
}

void ch_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_rawimport_params_t *p = (dt_iop_rawimport_params_t *)self->params;
  p->ch = gtk_range_get_value(range);
  dt_dev_add_history_item(darktable.develop, self);
}

void angle_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_rawimport_params_t *p = (dt_iop_rawimport_params_t *)self->params;
  p->angle = gtk_range_get_value(range);
  dt_dev_add_history_item(darktable.develop, self);
}
#endif

void gui_update(struct dt_iop_module_t *self)
{
  // dt_iop_rawimport_gui_data_t *g = (dt_iop_rawimport_gui_data_t *)self->gui_data;
  // dt_iop_rawimport_params_t *p = (dt_iop_rawimport_params_t *)self->params;
  // TODO: update gui info from params.
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_rawimport_params_t));
  module->default_params = malloc(sizeof(dt_iop_rawimport_params_t));
  module->default_enabled = 0;
  module->params_size = sizeof(dt_iop_rawimport_params_t);
  module->gui_data = NULL;
  module->priority = 100;
  dt_iop_rawimport_params_t tmp = (dt_iop_rawimport_params_t)
    {0.0, 0.01, 0, 1, 1, 0, 2, 0, 0, 0, 0, -1};
  memcpy(module->params, &tmp, sizeof(dt_iop_rawimport_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_rawimport_params_t));
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
  self->gui_data = malloc(sizeof(dt_iop_rawimport_gui_data_t));
  dt_iop_rawimport_gui_data_t *g = (dt_iop_rawimport_gui_data_t *)self->gui_data;
  // dt_iop_rawimport_params_t *p = (dt_iop_rawimport_params_t *)self->params;

  self->widget = gtk_vbox_new(FALSE, 0);
  // g->vbox1 = GTK_VBOX(gtk_vbox_new(TRUE, 0));
  // g->vbox2 = GTK_VBOX(gtk_vbox_new(TRUE, 0));
  // gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), TRUE, TRUE, 5);
  // gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);

  g->wb_auto        = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("auto white balance")));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->wb_auto), TRUE, TRUE, 0);
  g->wb_cam         = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("camera white balance")));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->wb_cam), TRUE, TRUE, 0);
  g->auto_bright    = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("auto exposure")));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->auto_bright), TRUE, TRUE, 0);


  GtkWidget *hbox   = gtk_hbox_new(FALSE, 0);
  GtkWidget *vbox1  = gtk_vbox_new(TRUE, 0);
  GtkWidget *vbox2  = gtk_vbox_new(TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), vbox1, FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(hbox), vbox2, TRUE, TRUE, 5);

  GtkWidget *label = gtk_label_new(_("median passes"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(vbox1), label, TRUE, TRUE, 0);
  g->med_passes     = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 31, 1));
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
  gtk_combo_box_append_text(g->demosaic_method, _("linear"));
  gtk_combo_box_append_text(g->demosaic_method, _("VNG"));
  gtk_combo_box_append_text(g->demosaic_method, _("PPG"));
  gtk_combo_box_append_text(g->demosaic_method, _("AHD"));
  gtk_box_pack_start(GTK_BOX(vbox2), GTK_WIDGET(g->demosaic_method), TRUE, TRUE, 0);

  g->four_color_rgb = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("four color rgb")));
  gtk_box_pack_start(GTK_BOX(vbox1), gtk_label_new(""), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox2), GTK_WIDGET(g->four_color_rgb), TRUE, TRUE, 0);

  GtkWidget *reload = gtk_button_new_with_label(_("reload"));
  gtk_box_pack_start(GTK_BOX(vbox1), gtk_label_new(""), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox2), GTK_WIDGET(reload), TRUE, TRUE, 0);
  
#if 0
  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (cx_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
                    G_CALLBACK (cy_callback), self);
  g_signal_connect (G_OBJECT (g->scale3), "value-changed",
                    G_CALLBACK (cw_callback), self);
  g_signal_connect (G_OBJECT (g->scale4), "value-changed",
                    G_CALLBACK (ch_callback), self);
  g_signal_connect (G_OBJECT (g->scale5), "value-changed",
                    G_CALLBACK (angle_callback), self);
#endif
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

