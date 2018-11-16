/*
    This file is part of darktable,
    copyright (c) 2010-2012 Henrik Andersson.

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
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bauhaus/bauhaus.h"
#include "common/debug.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "dtgtk/gradientslider.h"
#include "dtgtk/togglebutton.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"
#include "common/iop_group.h"

#define CLIP(x) ((x < 0) ? 0.0 : (x > 1.0) ? 1.0 : x)

DT_MODULE_INTROSPECTION(1, dt_iop_relight_params_t)

typedef struct dt_iop_relight_params_t
{
  float ev;
  float center;
  float width;
} dt_iop_relight_params_t;

void init_presets(dt_iop_module_so_t *self)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "BEGIN", NULL, NULL, NULL);

  dt_gui_presets_add_generic(_("fill-light 0.25EV with 4 zones"), self->op, self->version(),
                             &(dt_iop_relight_params_t){ 0.25, 0.25, 4.0 }, sizeof(dt_iop_relight_params_t),
                             1);
  dt_gui_presets_add_generic(_("fill-shadow -0.25EV with 4 zones"), self->op, self->version(),
                             &(dt_iop_relight_params_t){ -0.25, 0.25, 4.0 }, sizeof(dt_iop_relight_params_t),
                             1);

  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "COMMIT", NULL, NULL, NULL);
}

typedef struct dt_iop_relight_gui_data_t
{
  GtkBox *vbox1, *vbox2;                // left and right controlboxes
  GtkLabel *label1, *label2, *label3;   // ev, center, width
  GtkWidget *scale1, *scale2;           // ev,width
  GtkDarktableGradientSlider *gslider1; // center
  GtkDarktableToggleButton *tbutton1;   // Pick median lightness
} dt_iop_relight_gui_data_t;

typedef struct dt_iop_relight_data_t
{
  float ev;     // The ev of relight -4 - +4 EV
  float center; // the center light value for relight
  float width;  // the width expressed in zones
} dt_iop_relight_data_t;

typedef struct dt_iop_relight_global_data_t
{
  int kernel_relight;
} dt_iop_relight_global_data_t;


const char *name()
{
  return _("fill light");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int groups()
{
  return dt_iop_get_group("fill light", IOP_GROUP_TONE);
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "exposure"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "width"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_relight_gui_data_t *g = (dt_iop_relight_gui_data_t *)self->gui_data;

  dt_accel_connect_slider_iop(self, "exposure", GTK_WIDGET(g->scale1));
  dt_accel_connect_slider_iop(self, "width", GTK_WIDGET(g->scale2));
}


#define GAUSS(a, b, c, x) (a * pow(2.718281828, (-pow((x - b), 2) / (pow(c, 2)))))

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_relight_data_t *data = (dt_iop_relight_data_t *)piece->data;
  const int ch = piece->colors;

  // Precalculate parameters for gauss function
  const float a = 1.0;                        // Height of top
  const float b = -1.0 + (data->center * 2);  // Center of top
  const float c = (data->width / 10.0) / 2.0; // Width

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(data) schedule(static)
#endif
  for(int k = 0; k < roi_out->height; k++)
  {
    float *in = ((float *)ivoid) + (size_t)ch * k * roi_out->width;
    float *out = ((float *)ovoid) + (size_t)ch * k * roi_out->width;
    for(int j = 0; j < roi_out->width; j++, in += ch, out += ch)
    {
      const float lightness = in[0] / 100.0;
      const float x = -1.0 + (lightness * 2.0);
      float gauss = GAUSS(a, b, c, x);

      if(isnan(gauss) || isinf(gauss)) gauss = 0.0;

      float relight = 1.0 / exp2f(-data->ev * CLIP(gauss));

      if(isnan(relight) || isinf(relight)) relight = 1.0;

      out[0] = 100.0 * CLIP(lightness * relight);
      out[1] = in[1];
      out[2] = in[2];
      out[3] = in[3];
    }
  }
}


#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_relight_data_t *data = (dt_iop_relight_data_t *)piece->data;
  dt_iop_relight_global_data_t *gd = (dt_iop_relight_global_data_t *)self->data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const float center = data->center;
  const float wings = data->width;
  const float ev = data->ev;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };

  dt_opencl_set_kernel_arg(devid, gd->kernel_relight, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_relight, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_relight, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_relight, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_relight, 4, sizeof(float), (void *)&center);
  dt_opencl_set_kernel_arg(devid, gd->kernel_relight, 5, sizeof(float), (void *)&wings);
  dt_opencl_set_kernel_arg(devid, gd->kernel_relight, 6, sizeof(float), (void *)&ev);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_relight, sizes);
  if(err != CL_SUCCESS) goto error;
  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_relight] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void init_global(dt_iop_module_so_t *module)
{
  const int program = 8; // extended.cl, from programs.conf
  dt_iop_relight_global_data_t *gd
      = (dt_iop_relight_global_data_t *)malloc(sizeof(dt_iop_relight_global_data_t));
  module->data = gd;
  gd->kernel_relight = dt_opencl_create_kernel(program, "relight");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_relight_global_data_t *gd = (dt_iop_relight_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_relight);
  free(module->data);
  module->data = NULL;
}

static void picker_callback(GtkDarktableToggleButton *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;

  self->request_color_pick
      = (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button)) ? DT_REQUEST_COLORPICK_MODULE
                                                                 : DT_REQUEST_COLORPICK_OFF);

  /* set the area sample size*/
  if(self->request_color_pick != DT_REQUEST_COLORPICK_OFF)
  {
    dt_lib_colorpicker_set_point(darktable.lib, 0.5, 0.5);
    dt_dev_reprocess_all(self->dev);
  }
  else
    dt_control_queue_redraw();

  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
  dt_iop_request_focus(self);
}

static void ev_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_relight_params_t *p = (dt_iop_relight_params_t *)self->params;
  p->ev = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void width_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_relight_params_t *p = (dt_iop_relight_params_t *)self->params;
  p->width = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void center_callback(GtkDarktableGradientSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_relight_params_t *p = (dt_iop_relight_params_t *)self->params;

  {
    p->center = dtgtk_gradient_slider_get_value(slider);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
}



void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_relight_params_t *p = (dt_iop_relight_params_t *)p1;
  dt_iop_relight_data_t *d = (dt_iop_relight_data_t *)piece->data;

  d->ev = p->ev;
  d->width = p->width;
  d->center = p->center;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_relight_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_reset(struct dt_iop_module_t *self)
{
  dt_iop_relight_gui_data_t *g = (dt_iop_relight_gui_data_t *)self->gui_data;
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->tbutton1), 0);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;

  dt_iop_relight_gui_data_t *g = (dt_iop_relight_gui_data_t *)self->gui_data;
  dt_iop_relight_params_t *p = (dt_iop_relight_params_t *)module->params;
  dt_bauhaus_slider_set(g->scale1, p->ev);
  dt_bauhaus_slider_set(g->scale2, p->width);
  dtgtk_gradient_slider_set_value(g->gslider1, p->center);

  if (self->request_color_pick == DT_REQUEST_COLORPICK_OFF)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->tbutton1), 0);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_relight_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_relight_params_t));
  module->default_enabled = 0;
  module->priority = 714; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_relight_params_t);
  module->gui_data = NULL;
  dt_iop_relight_params_t tmp = (dt_iop_relight_params_t){ 0.33, 0, 4 };
  memcpy(module->params, &tmp, sizeof(dt_iop_relight_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_relight_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}


static gboolean draw(GtkWidget *widget, cairo_t *cr, dt_iop_module_t *self)
{
  // capture gui color picked event.
  if(darktable.gui->reset) return FALSE;

  float mean, min, max;

  if(self->request_color_pick != DT_REQUEST_COLORPICK_OFF && self->picked_color_max[0] >= 0.0f)
  {
    mean = fmin(fmax(self->picked_color[0] / 100.0f, 0.0f), 1.0f);
    min = fmin(fmax(self->picked_color_min[0] / 100.0f, 0.0f), 1.0f);
    max = fmin(fmax(self->picked_color_max[0] / 100.0f, 0.0f), 1.0f);
  }
  else
  {
    mean = min = max = NAN;
  }

  dt_iop_relight_gui_data_t *g = (dt_iop_relight_gui_data_t *)self->gui_data;
  dtgtk_gradient_slider_set_picker_meanminmax(DTGTK_GRADIENT_SLIDER(g->gslider1), mean, min, max);

  gtk_widget_queue_draw(GTK_WIDGET(g->gslider1));

  return FALSE;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_relight_gui_data_t));
  dt_iop_relight_gui_data_t *g = (dt_iop_relight_gui_data_t *)self->gui_data;
  dt_iop_relight_params_t *p = (dt_iop_relight_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));

  g_signal_connect(G_OBJECT(self->widget), "draw", G_CALLBACK(draw), self);

  /* exposure */
  g->scale1 = dt_bauhaus_slider_new_with_range(self, -2.0, 2.0, 0.05, p->ev, 2);
  dt_bauhaus_slider_set_format(g->scale1, "%.2fEV");
  dt_bauhaus_widget_set_label(g->scale1, NULL, _("exposure"));
  gtk_widget_set_tooltip_text(g->scale1, _("the fill-light in EV"));
  g_signal_connect(G_OBJECT(g->scale1), "value-changed", G_CALLBACK(ev_callback), self);
  /* width*/
  g->scale2 = dt_bauhaus_slider_new_with_range(self, 2, 10, 0.5, p->width, 1);
  dt_bauhaus_slider_set_format(g->scale2, "%.1f");
  dt_bauhaus_widget_set_label(g->scale2, NULL, _("width"));
  /* xgettext:no-c-format */
  gtk_widget_set_tooltip_text(g->scale2, _("width of fill-light area defined in zones"));
  g_signal_connect(G_OBJECT(g->scale2), "value-changed", G_CALLBACK(width_callback), self);

  /* lightnessslider */
  GtkBox *hbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2));

#define NEUTRAL_GRAY 0.5
  static const GdkRGBA _gradient_L[]
      = { { 0, 0, 0, 1.0 }, { NEUTRAL_GRAY, NEUTRAL_GRAY, NEUTRAL_GRAY, 1.0 } };
  g->gslider1 = DTGTK_GRADIENT_SLIDER(dtgtk_gradient_slider_new_with_color(_gradient_L[0], _gradient_L[1]));

  gtk_widget_set_tooltip_text(GTK_WIDGET(g->gslider1), _("select the center of fill-light"));
  g_signal_connect(G_OBJECT(g->gslider1), "value-changed", G_CALLBACK(center_callback), self);
  g->tbutton1 = DTGTK_TOGGLEBUTTON(dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT, NULL));
  gtk_widget_set_size_request(GTK_WIDGET(g->tbutton1), DT_PIXEL_APPLY_DPI(22), DT_PIXEL_APPLY_DPI(22));

  g_signal_connect(G_OBJECT(g->tbutton1), "toggled", G_CALLBACK(picker_callback), self);

  gtk_box_pack_start(hbox, GTK_WIDGET(g->gslider1), TRUE, TRUE, 0);
  gtk_box_pack_start(hbox, GTK_WIDGET(g->tbutton1), FALSE, FALSE, 0);

  /* add controls to widget ui */
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale1), TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale2), TRUE, FALSE, 0);


  gtk_widget_set_tooltip_text(GTK_WIDGET(g->tbutton1), _("toggle tool for picking median lightness in image"));
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
