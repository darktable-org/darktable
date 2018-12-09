/*
    This file is part of darktable,
    copyright (c) 2011 Henrik Andersson.

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
#include "common/colorspaces_inline_conversions.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"
#include "common/iop_group.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <assert.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE_INTROSPECTION(2, dt_iop_colorize_params_t)

// legacy parameters of version 1 of module
typedef struct dt_iop_colorize_params1_t
{
  float hue;
  float saturation;
  float source_lightness_mix;
  float lightness;
} dt_iop_colorize_params1_t;

typedef struct dt_iop_colorize_params_t
{
  float hue;
  float saturation;
  float source_lightness_mix;
  float lightness;
  int version;
} dt_iop_colorize_params_t;



typedef struct dt_iop_colorize_gui_data_t
{
  GtkWidget *scale1, *scale2; //  lightness, source_lightnessmix
  GtkWidget *gslider1, *gslider2; // hue, saturation
} dt_iop_colorize_gui_data_t;

typedef struct dt_iop_colorize_data_t
{
  float L;
  float a;
  float b;
  float mix;
} dt_iop_colorize_data_t;

typedef struct dt_iop_colorize_global_data_t
{
  int kernel_colorize;
} dt_iop_colorize_global_data_t;



const char *name()
{
  return _("colorize");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int groups()
{
  return dt_iop_get_group("colorize", IOP_GROUP_EFFECT);
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 2)
  {
    const dt_iop_colorize_params1_t *old = old_params;
    dt_iop_colorize_params_t *new = new_params;

    new->hue = old->hue;
    new->saturation = old->saturation;
    new->source_lightness_mix = old->source_lightness_mix;
    new->lightness = old->lightness;
    new->version = 1;
    return 0;
  }
  return 1;
}


void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "lightness"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "source mix"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_colorize_gui_data_t *g = (dt_iop_colorize_gui_data_t *)self->gui_data;

  dt_accel_connect_slider_iop(self, "lightness", GTK_WIDGET(g->scale1));
  dt_accel_connect_slider_iop(self, "source mix", GTK_WIDGET(g->scale2));
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  float *in, *out;
  dt_iop_colorize_data_t *d = (dt_iop_colorize_data_t *)piece->data;
  const int ch = piece->colors;

  const float L = d->L;
  const float a = d->a;
  const float b = d->b;
  const float mix = d->mix;
  const float Lmlmix = L - (mix * 100.0f) / 2.0f;

#ifdef _OPENMP
#pragma omp parallel for default(none) private(in, out) schedule(static)
#endif
  for(int k = 0; k < roi_out->height; k++)
  {

    int stride = ch * roi_out->width;

    in = (float *)ivoid + (size_t)k * stride;
    out = (float *)ovoid + (size_t)k * stride;

    for(int l = 0; l < stride; l += ch)
    {
      out[l + 0] = Lmlmix + in[l + 0] * mix;
      out[l + 1] = a;
      out[l + 2] = b;
      out[l + 3] = in[l + 3];
    }
  }
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_colorize_data_t *data = (dt_iop_colorize_data_t *)piece->data;
  dt_iop_colorize_global_data_t *gd = (dt_iop_colorize_global_data_t *)self->data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const float L = data->L;
  const float a = data->a;
  const float b = data->b;
  const float mix = data->mix;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };

  dt_opencl_set_kernel_arg(devid, gd->kernel_colorize, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorize, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorize, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorize, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorize, 4, sizeof(float), (void *)&mix);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorize, 5, sizeof(float), (void *)&L);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorize, 6, sizeof(float), (void *)&a);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorize, 7, sizeof(float), (void *)&b);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_colorize, sizes);
  if(err != CL_SUCCESS) goto error;
  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_colorize] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void init_global(dt_iop_module_so_t *module)
{
  const int program = 8; // extended.cl, from programs.conf
  dt_iop_colorize_global_data_t *gd
      = (dt_iop_colorize_global_data_t *)malloc(sizeof(dt_iop_colorize_global_data_t));
  module->data = gd;
  gd->kernel_colorize = dt_opencl_create_kernel(program, "colorize");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_colorize_global_data_t *gd = (dt_iop_colorize_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_colorize);
  free(module->data);
  module->data = NULL;
}

static inline void update_saturation_slider_end_color(GtkWidget *slider, float hue)
{
  float rgb[3];
  hsl2rgb(rgb, hue, 1.0, 0.5);
  dt_bauhaus_slider_set_stop(slider, 1.0, rgb[0], rgb[1], rgb[2]);
}

static void lightness_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorize_params_t *p = (dt_iop_colorize_params_t *)self->params;
  p->lightness = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void source_lightness_mix_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorize_params_t *p = (dt_iop_colorize_params_t *)self->params;
  p->source_lightness_mix = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void hue_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorize_params_t *p = (dt_iop_colorize_params_t *)self->params;
  dt_iop_colorize_gui_data_t *g = (dt_iop_colorize_gui_data_t *)self->gui_data;

  const float hue = dt_bauhaus_slider_get(g->gslider1);
  // fprintf(stderr," hue: %f, saturation: %f\n",hue,dtgtk_gradient_slider_get_value(g->gslider2));

  update_saturation_slider_end_color(g->gslider2, p->hue);

  gtk_widget_queue_draw(GTK_WIDGET(g->gslider2));

  p->hue = hue;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void saturation_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorize_params_t *p = (dt_iop_colorize_params_t *)self->params;

  p->saturation = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

#if 0 // TODO: could bind those to quad callback, if needed.
static void
colorpick_button_callback(GtkButton *button,gpointer user_data)
{
  GtkColorSelectionDialog  *csd=(GtkColorSelectionDialog  *)user_data;
  gtk_dialog_response(GTK_DIALOG(csd),(GTK_WIDGET(button)==csd->ok_button)?GTK_RESPONSE_ACCEPT:0);
}

static void
colorpick_callback (GtkDarktableButton *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorize_gui_data_t *g = (dt_iop_colorize_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorize_params_t *p = (dt_iop_colorize_params_t *)self->params;

  GtkColorSelectionDialog  *csd = GTK_COLOR_SELECTION_DIALOG(gtk_color_selection_dialog_new(_("select tone color")));
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(GTK_WIDGET(csd));
#endif
  g_signal_connect (G_OBJECT (csd->ok_button), "clicked",
                    G_CALLBACK (colorpick_button_callback), csd);
  g_signal_connect (G_OBJECT (csd->cancel_button), "clicked",
                    G_CALLBACK (colorpick_button_callback), csd);

  GtkColorSelection *cs = GTK_COLOR_SELECTION(gtk_color_selection_dialog_get_color_selection(csd));
  GdkRGBA c;
  float color[3],h,s,l;

  h = p->hue;
  s = p->saturation;
  l=0.5;
  hsl2rgb(color,h,s,l);

  c.red   = color[0];
  c.green = color[1];
  c.blue  = color[2];

  gtk_color_selection_set_current_color(cs,&c);

  if(gtk_dialog_run(GTK_DIALOG(csd))==GTK_RESPONSE_ACCEPT)
  {
    gtk_color_selection_get_current_color(cs,&c);
    color[0] = c.red;
    color[1] = c.green;
    color[2] = c.blue;
    rgb2hsl(color,&h,&s,&l);
    l=0.5;
    hsl2rgb(color,h,s,l);

    dtgtk_gradient_slider_set_value(g->gslider1, h);
    dtgtk_gradient_slider_set_value(g->gslider2, s);
  }

  gtk_widget_destroy(GTK_WIDGET(csd));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
#endif

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorize_params_t *p = (dt_iop_colorize_params_t *)p1;
  dt_iop_colorize_data_t *d = (dt_iop_colorize_data_t *)piece->data;

  /* create Lab */
  float rgb[3] = { 0 }, XYZ[3] = { 0 }, Lab[3] = { 0 };
  hsl2rgb(rgb, p->hue, p->saturation, p->lightness / 100.0);

  if(p->version == 1)
  {
    // the old matrix is a bit off. in fact it's the conversion matrix from AdobeRGB to XYZ@D65
    XYZ[0] = (rgb[0] * 0.5767309f) + (rgb[1] * 0.1855540f) + (rgb[2] * 0.1881852f);
    XYZ[1] = (rgb[0] * 0.2973769f) + (rgb[1] * 0.6273491f) + (rgb[2] * 0.0752741f);
    XYZ[2] = (rgb[0] * 0.0270343f) + (rgb[1] * 0.0706872f) + (rgb[2] * 0.9911085f);
  }
  else
  {
    // this fits better. conversion matrix from sRGB to XYZ@D50 - which is what dt_XYZ_to_Lab() expects as
    // input
    XYZ[0] = (rgb[0] * 0.4360747f) + (rgb[1] * 0.3850649f) + (rgb[2] * 0.1430804f);
    XYZ[1] = (rgb[0] * 0.2225045f) + (rgb[1] * 0.7168786f) + (rgb[2] * 0.0606169f);
    XYZ[2] = (rgb[0] * 0.0139322f) + (rgb[1] * 0.0971045f) + (rgb[2] * 0.7141733f);
  }

  dt_XYZ_to_Lab(XYZ, Lab);

  /* a/b components */
  d->L = Lab[0];
  d->a = Lab[1];
  d->b = Lab[2];
  d->mix = p->source_lightness_mix / 100.0f;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_colorize_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_colorize_gui_data_t *g = (dt_iop_colorize_gui_data_t *)self->gui_data;
  dt_iop_colorize_params_t *p = (dt_iop_colorize_params_t *)module->params;

  dt_bauhaus_slider_set(g->gslider1, p->hue);
  dt_bauhaus_slider_set(g->gslider2, p->saturation);
  dt_bauhaus_slider_set(g->scale1, p->lightness);
  dt_bauhaus_slider_set(g->scale2, p->source_lightness_mix);

  update_saturation_slider_end_color(g->gslider2, p->hue);

#if 0 // could update quad drawing color here
  float color[3];
  hsl2rgb(color,p->hue,p->saturation,0.5);

  GdkRGBA c;
  c.red = color[0];
  c.green = color[1];
  c.blue = color[2];

  gtk_widget_modify_fg(GTK_WIDGET(g->colorpick1),GTK_STATE_NORMAL,&c);
#endif
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_colorize_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_colorize_params_t));
  module->default_enabled = 0;
  module->priority = 471; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_colorize_params_t);
  module->gui_data = NULL;
  dt_iop_colorize_params_t tmp = (dt_iop_colorize_params_t){ 0, 0.5, 50, 50, module->version() };
  memcpy(module->params, &tmp, sizeof(dt_iop_colorize_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_colorize_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_colorize_gui_data_t));
  dt_iop_colorize_gui_data_t *g = (dt_iop_colorize_gui_data_t *)self->gui_data;
  dt_iop_colorize_params_t *p = (dt_iop_colorize_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));

  /* hue slider */
  g->gslider1 = dt_bauhaus_slider_new_with_range_and_feedback(self, 0.0f, 1.0f, 0.01f, 0.0f, 2, 0);
  dt_bauhaus_slider_set_stop(g->gslider1, 0.0f, 1.0f, 0.0f, 0.0f);
  // dt_bauhaus_slider_set_format(g->gslider1, "");
  dt_bauhaus_widget_set_label(g->gslider1, NULL, _("hue"));
  dt_bauhaus_slider_set_stop(g->gslider1, 0.166f, 1.0f, 1.0f, 0.0f);
  dt_bauhaus_slider_set_stop(g->gslider1, 0.322f, 0.0f, 1.0f, 0.0f);
  dt_bauhaus_slider_set_stop(g->gslider1, 0.498f, 0.0f, 1.0f, 1.0f);
  dt_bauhaus_slider_set_stop(g->gslider1, 0.664f, 0.0f, 0.0f, 1.0f);
  dt_bauhaus_slider_set_stop(g->gslider1, 0.830f, 1.0f, 0.0f, 1.0f);
  dt_bauhaus_slider_set_stop(g->gslider1, 1.0f, 1.0f, 0.0f, 0.0f);
  gtk_widget_set_tooltip_text(g->gslider1, _("select the hue tone"));

  gtk_box_pack_start(GTK_BOX(self->widget), g->gslider1, TRUE, TRUE, 0);

  /* saturation slider */
  g->gslider2 = dt_bauhaus_slider_new_with_range(self, 0.0f, 1.0f, 0.01f, 0.0f, 2);
  // dt_bauhaus_slider_set_format(g->gslider2, "");
  dt_bauhaus_widget_set_label(g->gslider2, NULL, _("saturation"));
  dt_bauhaus_slider_set_stop(g->gslider2, 0.0f, 0.2f, 0.2f, 0.2f);
  dt_bauhaus_slider_set_stop(g->gslider2, 1.0f, 1.0f, 1.0f, 1.0f);
  gtk_widget_set_tooltip_text(g->gslider2, _("select the saturation shadow tone"));

  gtk_box_pack_start(GTK_BOX(self->widget), g->gslider2, TRUE, TRUE, 0);

  // Additional parameters
  g->scale1 = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 0.1, p->lightness * 100.0, 2);
  dt_bauhaus_slider_set_format(g->scale1, "%.2f%%");
  dt_bauhaus_widget_set_label(g->scale1, NULL, _("lightness"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);

  g->scale2 = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 0.1, p->source_lightness_mix, 2);
  dt_bauhaus_slider_set_format(g->scale2, "%.2f%%");
  dt_bauhaus_widget_set_label(g->scale2, NULL, _("source mix"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);


  gtk_widget_set_tooltip_text(g->scale1, _("lightness of color"));
  gtk_widget_set_tooltip_text(g->scale2, _("mix value of source lightness"));

  g_signal_connect(G_OBJECT(g->gslider1), "value-changed", G_CALLBACK(hue_callback), self);
  g_signal_connect(G_OBJECT(g->gslider2), "value-changed", G_CALLBACK(saturation_callback), self);
  g_signal_connect(G_OBJECT(g->scale1), "value-changed", G_CALLBACK(lightness_callback), self);
  g_signal_connect(G_OBJECT(g->scale2), "value-changed", G_CALLBACK(source_lightness_mix_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
