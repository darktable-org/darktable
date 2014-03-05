/*
    This file is part of darktable,
    copyright (c) 2011 -- 2014 Tobias Ellinghaus, johannes hanika, henrik andersson.

    and the initial plugin `stuck pixels' was
    copyright (c) 2011 bruce guenter

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
#include "control/control.h"
#include "develop/imageop.h"
#include "dtgtk/resetlabel.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <stdlib.h>

DT_MODULE_INTROSPECTION(1, dt_iop_invert_params_t)

typedef struct dt_iop_invert_params_t
{
  float color[3]; // color of film material
}
dt_iop_invert_params_t;

typedef struct dt_iop_invert_gui_data_t
{
  GtkDarktableButton     *colorpicker;
  GtkDarktableResetLabel *label;
  GtkHBox                *pickerbuttons;
}
dt_iop_invert_gui_data_t;

typedef struct dt_iop_invert_params_t dt_iop_invert_data_t;

const char *name()
{
  return _("invert");
}

int
groups ()
{
  return IOP_GROUP_BASIC;
}

int flags ()
{
  return IOP_FLAGS_ONE_INSTANCE;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_iop(self, FALSE,
                        NC_("accel", "pick color of film material from image"),
                        0, 0);
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_invert_gui_data_t *g = (dt_iop_invert_gui_data_t*)self->gui_data;

  dt_accel_connect_button_iop(self, "pick color of film material from image",
                              GTK_WIDGET(g->colorpicker));
}

int
output_bpp(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // this is bytes per pixel, so it has to be 4*sizeof(float) or sizeof(float) for raw images.
  if(!dt_dev_pixelpipe_uses_downsampled_input(pipe) && piece->pipe->image.filters && piece->pipe->image.bpp != 4) return sizeof(uint16_t);
  if(!dt_dev_pixelpipe_uses_downsampled_input(pipe) && piece->pipe->image.filters && piece->pipe->image.bpp == 4) return sizeof(float);
  return 4*sizeof(float);
}

static void
request_pick_toggled(GtkToggleButton *togglebutton, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  self->request_color_pick = (gtk_toggle_button_get_active(togglebutton) ? 1 : 0);

  if(self->request_color_pick)
  {
    dt_lib_colorpicker_set_area(darktable.lib, 0.99);
    dt_dev_reprocess_all(self->dev);
  }
  else
    dt_control_queue_redraw();

  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
  dt_iop_request_focus(self);
}

static gboolean
expose (GtkWidget *widget, GdkEventExpose *event, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return FALSE;
  if(self->picked_color_max[0] < 0.0f) return FALSE;
  if(!self->request_color_pick) return FALSE;
  dt_iop_invert_gui_data_t *g = (dt_iop_invert_gui_data_t *)self->gui_data;
  dt_iop_invert_params_t *p = (dt_iop_invert_params_t *)self->params;

  if(fabsf(p->color[0] - self->picked_color[0]) < 0.0001f &&
      fabsf(p->color[1] - self->picked_color[1]) < 0.0001f &&
      fabsf(p->color[2] - self->picked_color[2]) < 0.0001f)
  {
    // interrupt infinite loops
    return FALSE;
  }

  p->color[0] = self->picked_color[0];
  p->color[1] = self->picked_color[1];
  p->color[2] = self->picked_color[2];
  GdkColor c;
  c.red   = p->color[0]*65535.0;
  c.green = p->color[1]*65535.0;
  c.blue  = p->color[2]*65535.0;
  gtk_widget_modify_fg(GTK_WIDGET(g->colorpicker), GTK_STATE_NORMAL, &c);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  return FALSE;
}

static void
colorpick_button_callback(GtkButton *button, GtkColorSelectionDialog *csd)
{
  GtkWidget *okButton = 0;
  g_object_get(G_OBJECT(csd), "ok-button", &okButton, NULL);

  gtk_dialog_response(GTK_DIALOG(csd), (GTK_WIDGET(button)==okButton)?GTK_RESPONSE_ACCEPT:0);
}

static void
colorpicker_callback (GtkDarktableButton *button, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_invert_gui_data_t *g = (dt_iop_invert_gui_data_t *)self->gui_data;
  dt_iop_invert_params_t *p = (dt_iop_invert_params_t *)self->params;

  GtkColorSelectionDialog  *csd = GTK_COLOR_SELECTION_DIALOG(gtk_color_selection_dialog_new(_("select color of film material")));
  gtk_window_set_transient_for(GTK_WINDOW(csd), GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));

  GtkWidget *okButton, *cancelButton = 0;
  g_object_get(G_OBJECT(csd), "ok-button", &okButton, NULL);
  g_object_get(G_OBJECT(csd), "cancel-button", &cancelButton, NULL);

  g_signal_connect (G_OBJECT (okButton), "clicked",
                    G_CALLBACK (colorpick_button_callback), csd);
  g_signal_connect (G_OBJECT (cancelButton), "clicked",
                    G_CALLBACK (colorpick_button_callback), csd);

  GtkColorSelection *cs = GTK_COLOR_SELECTION(gtk_color_selection_dialog_get_color_selection(csd));
  GdkColor c;
  c.red   = 65535 * p->color[0];
  c.green = 65535 * p->color[1];
  c.blue  = 65535 * p->color[2];
  gtk_color_selection_set_current_color(cs, &c);
  if(gtk_dialog_run(GTK_DIALOG(csd)) == GTK_RESPONSE_ACCEPT)
  {
    gtk_color_selection_get_current_color(cs, &c);
    p->color[0] = c.red  /65535.0;
    p->color[1] = c.green/65535.0;
    p->color[2] = c.blue /65535.0;
    gtk_widget_modify_fg(GTK_WIDGET(g->colorpicker), GTK_STATE_NORMAL, &c);
  }
  gtk_widget_destroy(GTK_WIDGET(csd));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static int
FC(const int row, const int col, const unsigned int filters)
{
  return filters >> (((row << 1 & 14) + (col & 1)) << 1) & 3;
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_invert_data_t *d = (dt_iop_invert_data_t *)piece->data;
  const float film_rgb[3] = {d->color[0], d->color[1], d->color[2]};

  //FIXME: it could be wise to make this a NOP when picking colors. not sure about that though.
//   if(self->request_color_pick){
  // do nothing
//   }

  const int filters = dt_image_flipped_filter(&piece->pipe->image);

  if(!dt_dev_pixelpipe_uses_downsampled_input(piece->pipe) && filters && piece->pipe->image.bpp != 4)
  {
    const float *const m = piece->pipe->processed_maximum;
    const int32_t film_rgb_i[3] = {m[0]*film_rgb[0]*65535, m[1]*film_rgb[1]*65535, m[2]*film_rgb[2]*65535};
#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(roi_out, ivoid, ovoid, /*film_rgb_i, min, max, res*/) schedule(static)
#endif
    for(int j=0; j<roi_out->height; j++)
    {
      const uint16_t *in = ((uint16_t*)ivoid) + (size_t)j*roi_out->width;
      uint16_t *out = ((uint16_t*)ovoid) + (size_t)j*roi_out->width;
      for(int i=0; i<roi_out->width; i++,out++,in++)
      {
        *out = CLAMP(film_rgb_i[FC(j+roi_out->x, i+roi_out->y, filters)] - (int32_t)in[0], 0, 0xffff);
      }
    }

    for(int k=0; k<3; k++)
      piece->pipe->processed_maximum[k] = 1.0f;
  }
  else if(!dt_dev_pixelpipe_uses_downsampled_input(piece->pipe) && filters && piece->pipe->image.bpp == 4)
  {
#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(roi_out, ivoid, ovoid) schedule(static)
#endif
    for(int j=0; j<roi_out->height; j++)
    {
      const float *in = ((float *)ivoid) + (size_t)j*roi_out->width;
      float *out = ((float*)ovoid) + (size_t)j*roi_out->width;
      for(int i=0; i<roi_out->width; i++,out++,in++)
      {
        *out = CLAMP(film_rgb[FC(j+roi_out->x, i+roi_out->y, filters)] - *in/(float)0xffff, 0, 1.0f);
      }
    }
    for(int k=0; k<3; k++)
      piece->pipe->processed_maximum[k] = 1.0f;
  }
  else
  {
    const int ch = piece->colors;
#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(roi_out, ivoid, ovoid) schedule(static)
#endif
    for(int k=0; k<roi_out->height; k++)
    {
      const float *in = ((float*)ivoid) + (size_t)ch*k*roi_out->width;
      float *out = ((float*)ovoid) + (size_t)ch*k*roi_out->width;
      for (int j=0; j<roi_out->width; j++,in+=ch,out+=ch)
        for(int c=0; c<3; c++) out[c] = film_rgb[c] - in[c];
    }
  }
}

void reload_defaults(dt_iop_module_t *self)
{
  dt_iop_invert_params_t tmp = (dt_iop_invert_params_t)
  {
    {
      1.0f, 1.0f, 1.0f
    }
  };
  memcpy(self->params, &tmp, sizeof(dt_iop_invert_params_t));
  memcpy(self->default_params, &tmp, sizeof(dt_iop_invert_params_t));

  self->default_enabled = 0;
}

void init(dt_iop_module_t *module)
{
  // module->data = g_malloc(sizeof(dt_iop_invert_data_t));
  module->params = g_malloc(sizeof(dt_iop_invert_params_t));
  module->default_params = g_malloc(sizeof(dt_iop_invert_params_t));
  module->default_enabled = 0;
  module->params_size = sizeof(dt_iop_invert_params_t);
  module->gui_data = NULL;
  module->priority = 17; // module order created by iop_dependencies.py, do not edit!
}

void cleanup(dt_iop_module_t *module)
{
  g_free(module->gui_data);
  module->gui_data = NULL;
  g_free(module->params);
  module->params = NULL;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_invert_params_t *p = (dt_iop_invert_params_t *)params;
  dt_iop_invert_data_t *d = (dt_iop_invert_data_t *)piece->data;
  memcpy(d, p, sizeof(dt_iop_invert_params_t));
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = g_malloc(sizeof(dt_iop_invert_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  g_free(piece->data);
}

void gui_update(dt_iop_module_t *self)
{
  self->request_color_pick = 0;

  dt_iop_invert_gui_data_t *g = (dt_iop_invert_gui_data_t *)self->gui_data;
  dt_iop_invert_params_t *p = (dt_iop_invert_params_t *)self->params;

  gtk_widget_set_visible(GTK_WIDGET(g->pickerbuttons), TRUE);
  dtgtk_reset_label_set_text(g->label, _("color of film material"));

  GdkColor c;
  c.red   = p->color[0]*65535.0;
  c.green = p->color[1]*65535.0;
  c.blue  = p->color[2]*65535.0;
  gtk_widget_modify_fg(GTK_WIDGET(g->colorpicker), GTK_STATE_NORMAL, &c);
}

void gui_init(dt_iop_module_t *self)
{
  self->gui_data = g_malloc(sizeof(dt_iop_invert_gui_data_t));
  dt_iop_invert_gui_data_t *g = (dt_iop_invert_gui_data_t *)self->gui_data;
  dt_iop_invert_params_t *p = (dt_iop_invert_params_t *)self->params;

  self->widget = gtk_hbox_new(FALSE, 5);

  GtkWidget *tb;

  g->label = DTGTK_RESET_LABEL(dtgtk_reset_label_new ("", self, &p->color, 3*sizeof(float)));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->label), TRUE, TRUE, 0);

  g->pickerbuttons = GTK_HBOX(gtk_hbox_new(FALSE, 5));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->pickerbuttons), TRUE, TRUE, 0);

  g->colorpicker = DTGTK_BUTTON(dtgtk_button_new(dtgtk_cairo_paint_color, CPF_IGNORE_FG_STATE|CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER));
  gtk_widget_set_size_request(GTK_WIDGET(g->colorpicker), 75, 24);
  g_signal_connect (G_OBJECT (g->colorpicker), "clicked", G_CALLBACK (colorpicker_callback), self);
  gtk_box_pack_start(GTK_BOX(g->pickerbuttons), GTK_WIDGET(g->colorpicker), TRUE, TRUE, 0);

  tb = dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT);
  g_object_set(G_OBJECT(tb), "tooltip-text", _("pick color of film material from image"), (char *)NULL);
  gtk_widget_set_size_request(tb, 24, 24);
  g_signal_connect(G_OBJECT(tb), "toggled", G_CALLBACK(request_pick_toggled), self);
  gtk_box_pack_start(GTK_BOX(g->pickerbuttons), tb, TRUE, TRUE, 5);

  g_signal_connect (G_OBJECT(self->widget), "expose-event", G_CALLBACK(expose), self);
}

void gui_cleanup  (dt_iop_module_t *self)
{
  g_free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
