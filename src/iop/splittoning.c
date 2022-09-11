/*
    This file is part of darktable,
    Copyright (C) 2010-2021 darktable developers.

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
#include "common/colorspaces.h"
#include "common/debug.h"
#include "common/math.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "dtgtk/button.h"
#include "dtgtk/gradientslider.h"
#include "dtgtk/resetlabel.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "gui/color_picker_proxy.h"
#include "iop/iop_api.h"
#include <assert.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE_INTROSPECTION(1, dt_iop_splittoning_params_t)

typedef struct dt_iop_splittoning_params_t
{
  float shadow_hue;           // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
  float shadow_saturation;    // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5 $DESCRIPTION: "saturation"
  float highlight_hue;        // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.2 $DESCRIPTION: "hue"
  float highlight_saturation; // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5 $DESCRIPTION: "saturation"
  float balance;              // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5 center luminance of gradient
  float compress;             // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 33.0 Compress range
} dt_iop_splittoning_params_t;

typedef struct dt_iop_splittoning_gui_data_t
{
  GtkWidget *balance_scale, *compress_scale;
  GtkWidget *shadow_colorpick, *highlight_colorpick;
  GtkWidget *shadow_hue_gslider, *shadow_sat_gslider;
  GtkWidget *highlight_hue_gslider, *highlight_sat_gslider;
} dt_iop_splittoning_gui_data_t;

typedef struct dt_iop_splittoning_data_t
{
  float shadow_hue;
  float shadow_saturation;
  float highlight_hue;
  float highlight_saturation;
  float balance;  // center luminance of gradient}
  float compress; // Compress range
} dt_iop_splittoning_data_t;

typedef struct dt_iop_splittoning_global_data_t
{
  int kernel_splittoning;
} dt_iop_splittoning_global_data_t;

const char *name()
{
  return _("split-toning");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_group()
{
  return IOP_GROUP_EFFECT | IOP_GROUP_GRADING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("use two specific colors for shadows and highlights and\n"
                                        "create a linear toning effect between them up to a pivot."),
                                      _("creative"),
                                      _("linear, RGB, scene-referred"),
                                      _("linear, RGB"),
                                      _("linear, RGB, scene-referred"));
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_database_start_transaction(darktable.db);

  // shadows: #ED7212
  // highlights: #ECA413
  // balance : 63
  // compress : 0
  dt_gui_presets_add_generic(
      _("authentic sepia"), self->op, self->version(),
      &(dt_iop_splittoning_params_t){ 26.0 / 360.0, 92.0 / 100.0, 40.0 / 360.0, 92.0 / 100.0, 0.63, 0.0 },
      sizeof(dt_iop_splittoning_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // shadows: #446CBB
  // highlights: #446CBB
  // balance : 0
  // compress : 5.22
  dt_gui_presets_add_generic(
      _("authentic cyanotype"), self->op, self->version(),
      &(dt_iop_splittoning_params_t){ 220.0 / 360.0, 64.0 / 100.0, 220.0 / 360.0, 64.0 / 100.0, 0.0, 5.22 },
      sizeof(dt_iop_splittoning_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // shadows : #A16C5E
  // highlights : #A16C5E
  // balance : 100
  // compress : 0
  dt_gui_presets_add_generic(
      _("authentic platinotype"), self->op, self->version(),
      &(dt_iop_splittoning_params_t){ 13.0 / 360.0, 42.0 / 100.0, 13.0 / 360.0, 42.0 / 100.0, 100.0, 0.0 },
      sizeof(dt_iop_splittoning_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // shadows: #211A14
  // highlights: #D9D0C7
  // balance : 60
  // compress : 0
  dt_gui_presets_add_generic(
      _("chocolate brown"), self->op, self->version(),
      &(dt_iop_splittoning_params_t){ 28.0 / 360.0, 39.0 / 100.0, 28.0 / 360.0, 8.0 / 100.0, 0.60, 0.0 },
      sizeof(dt_iop_splittoning_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  dt_database_release_transaction(darktable.db);
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return; // image has been copied through to output and module's trouble flag has been updated

  const dt_iop_splittoning_data_t *const data = (dt_iop_splittoning_data_t *)piece->data;
  const float compress = (data->compress / 110.0) / 2.0; // Don't allow 100% compression..

  const float *const restrict in = DT_IS_ALIGNED((float*)ivoid);
  float *const restrict out = DT_IS_ALIGNED((float*)ovoid);
  const int npixels = roi_out->width * roi_out->height;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(compress, npixels)  \
  dt_omp_sharedconst(data, in, out) \
  schedule(static)
#endif
  for(int k = 0; k < 4 * npixels; k += 4)
  {
    float h, s, l;
    rgb2hsl(in+k, &h, &s, &l);
    if(l < data->balance - compress)
    {
      dt_aligned_pixel_t mixrgb;
      hsl2rgb(mixrgb, data->shadow_hue, data->shadow_saturation, l);

      const float ra = CLIP((data->balance - compress - l) * 2.0f);
      const float la = (1.0f - ra);

      for_each_channel(c,aligned(in,out))
        out[k+c] = CLIP(in[k+c] * la + mixrgb[c] * ra);
    }
    else if(l > data->balance + compress)
    {
      dt_aligned_pixel_t mixrgb;
      hsl2rgb(mixrgb, data->highlight_hue, data->highlight_saturation, l);

      const float ra = CLIP((l - (data->balance + compress)) * 2.0f);
      const float la = (1.0f - ra);

      for_each_channel(c,aligned(in,out))
        out[k+c] = CLIP(in[k+c] * la + mixrgb[c] * ra);
    }
    else
    {
      copy_pixel(out + k, in +k);
    }

  }
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_splittoning_data_t *d = (dt_iop_splittoning_data_t *)piece->data;
  dt_iop_splittoning_global_data_t *gd = (dt_iop_splittoning_global_data_t *)self->global_data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;

  const int width = roi_out->width;
  const int height = roi_out->height;

  const float compress = (d->compress / 110.0) / 2.0; // Don't allow 100% compression..
  const float balance = d->balance;
  const float shadow_hue = d->shadow_hue;
  const float shadow_saturation = d->shadow_saturation;
  const float highlight_hue = d->highlight_hue;
  const float highlight_saturation = d->highlight_saturation;

  size_t sizes[2] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid) };
  dt_opencl_set_kernel_arg(devid, gd->kernel_splittoning, 0, sizeof(cl_mem), &dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_splittoning, 1, sizeof(cl_mem), &dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_splittoning, 2, sizeof(int), &width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_splittoning, 3, sizeof(int), &height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_splittoning, 4, sizeof(float), &compress);
  dt_opencl_set_kernel_arg(devid, gd->kernel_splittoning, 5, sizeof(float), &balance);
  dt_opencl_set_kernel_arg(devid, gd->kernel_splittoning, 6, sizeof(float), &shadow_hue);
  dt_opencl_set_kernel_arg(devid, gd->kernel_splittoning, 7, sizeof(float), &shadow_saturation);
  dt_opencl_set_kernel_arg(devid, gd->kernel_splittoning, 8, sizeof(float), &highlight_hue);
  dt_opencl_set_kernel_arg(devid, gd->kernel_splittoning, 9, sizeof(float), &highlight_saturation);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_splittoning, sizes);
  if(err != CL_SUCCESS) goto error;
  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_splittoning] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif


void init_global(dt_iop_module_so_t *module)
{
  const int program = 8; // extended.cl from programs.conf
  dt_iop_splittoning_global_data_t *gd
      = (dt_iop_splittoning_global_data_t *)malloc(sizeof(dt_iop_splittoning_global_data_t));
  module->data = gd;
  gd->kernel_splittoning = dt_opencl_create_kernel(program, "splittoning");
}


void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_splittoning_global_data_t *gd = (dt_iop_splittoning_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_splittoning);
  free(module->data);
  module->data = NULL;
}

static inline void update_colorpicker_color(GtkWidget *colorpicker, float hue, float sat)
{
  dt_aligned_pixel_t rgb;
  hsl2rgb(rgb, hue, sat, 0.5);

  GdkRGBA color = (GdkRGBA){.red = rgb[0], .green = rgb[1], .blue = rgb[2], .alpha = 1.0 };
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(colorpicker), &color);
}

static inline void update_saturation_slider_end_color(GtkWidget *slider, float hue)
{
  dt_aligned_pixel_t rgb;
  hsl2rgb(rgb, hue, 1.0, 0.5);
  dt_bauhaus_slider_set_stop(slider, 1.0, rgb[0], rgb[1], rgb[2]);
}

static inline void update_balance_slider_colors(
    GtkWidget *slider,
    float shadow_hue,
    float highlight_hue)
{
  dt_aligned_pixel_t rgb;
  if(shadow_hue != -1)
  {
    hsl2rgb(rgb, shadow_hue, 1.0, 0.5);
    dt_bauhaus_slider_set_stop(slider, 0.0, rgb[0], rgb[1], rgb[2]);
  }
  if(highlight_hue != -1)
  {
    hsl2rgb(rgb, highlight_hue, 1.0, 0.5);
    dt_bauhaus_slider_set_stop(slider, 1.0, rgb[0], rgb[1], rgb[2]);
  }

  gtk_widget_queue_draw(GTK_WIDGET(slider));
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)self->params;
  dt_iop_splittoning_gui_data_t *g = (dt_iop_splittoning_gui_data_t *)self->gui_data;

  if(w == g->shadow_sat_gslider || w == g->shadow_hue_gslider)
  {
    update_colorpicker_color(g->shadow_colorpick, p->shadow_hue, p->shadow_saturation);

    if(w == g->shadow_hue_gslider)
    {
      update_balance_slider_colors(g->balance_scale, p->shadow_hue, -1);
      update_saturation_slider_end_color(g->shadow_sat_gslider, p->shadow_hue);

      gtk_widget_queue_draw(GTK_WIDGET(g->shadow_sat_gslider));
    }
  }
  else if(w == g->highlight_sat_gslider || w == g->highlight_hue_gslider)
  {
    update_colorpicker_color(g->highlight_colorpick, p->highlight_hue, p->highlight_saturation);

    if(w == g->highlight_hue_gslider)
    {
      update_balance_slider_colors(g->balance_scale, -1, p->highlight_hue);
      update_saturation_slider_end_color(g->highlight_sat_gslider, p->highlight_hue);

      gtk_widget_queue_draw(GTK_WIDGET(g->highlight_sat_gslider));
    }
  }
}

static void colorpick_callback(GtkColorButton *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  dt_iop_splittoning_gui_data_t *g = (dt_iop_splittoning_gui_data_t *)self->gui_data;

  dt_aligned_pixel_t color;
  float h, s, l;

  GdkRGBA c;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &c);
  color[0] = c.red;
  color[1] = c.green;
  color[2] = c.blue;
  rgb2hsl(color, &h, &s, &l);

  if(GTK_WIDGET(widget) == g->shadow_colorpick)
  {
      dt_bauhaus_slider_set(g->shadow_hue_gslider, h);
      dt_bauhaus_slider_set(g->shadow_sat_gslider, s);
      update_balance_slider_colors(g->balance_scale, h, -1);
  }
  else
  {
      dt_bauhaus_slider_set(g->highlight_hue_gslider, h);
      dt_bauhaus_slider_set(g->highlight_sat_gslider, s);
      update_balance_slider_colors(g->balance_scale, -1,  h);
  }

  gtk_widget_queue_draw(GTK_WIDGET(g->balance_scale));

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_splittoning_gui_data_t *g = (dt_iop_splittoning_gui_data_t *)self->gui_data;
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)self->params;

  float *p_hue, *p_saturation;
  GtkWidget *sat, *hue, *colorpicker;

  // convert picker RGB 2 HSL
  float H = .0f, S = .0f, L = .0f;
  rgb2hsl(self->picked_color, &H, &S, &L);

  if(picker == g->highlight_hue_gslider)
  {
    p_hue = &p->highlight_hue;
    p_saturation = &p->highlight_saturation;
    hue = g->highlight_hue_gslider;
    sat = g->highlight_sat_gslider;
    colorpicker = g->highlight_colorpick;
    update_balance_slider_colors(g->balance_scale, -1, H);
  }
  else
  {
    p_hue = &p->shadow_hue;
    p_saturation = &p->shadow_saturation;
    hue = g->shadow_hue_gslider;
    sat = g->shadow_sat_gslider;
    colorpicker = g->shadow_colorpick;
    update_balance_slider_colors(g->balance_scale, H, -1);
  }

  if(fabsf(*p_hue - H) < 0.0001f && fabsf(*p_saturation - S) < 0.0001f)
  {
    // interrupt infinite loops
    return;
  }

  *p_hue        = H;
  *p_saturation = S;

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(hue, H);
  dt_bauhaus_slider_set(sat, S);
  update_colorpicker_color(colorpicker, H, S);
  update_saturation_slider_end_color(sat, H);
  --darktable.gui->reset;

  gtk_widget_queue_draw(GTK_WIDGET(g->balance_scale));

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)p1;
  dt_iop_splittoning_data_t *d = (dt_iop_splittoning_data_t *)piece->data;

  d->shadow_hue = p->shadow_hue;
  d->highlight_hue = p->highlight_hue;
  d->shadow_saturation = p->shadow_saturation;
  d->highlight_saturation = p->highlight_saturation;
  d->balance = p->balance;
  d->compress = p->compress;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_splittoning_data_t));
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_splittoning_gui_data_t *g = (dt_iop_splittoning_gui_data_t *)self->gui_data;
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)self->params;

  dt_bauhaus_slider_set(g->shadow_hue_gslider, p->shadow_hue);
  dt_bauhaus_slider_set(g->shadow_sat_gslider, p->shadow_saturation);
  dt_bauhaus_slider_set(g->highlight_hue_gslider, p->highlight_hue);
  dt_bauhaus_slider_set(g->highlight_sat_gslider, p->highlight_saturation);
  dt_bauhaus_slider_set(g->balance_scale, p->balance);
  dt_bauhaus_slider_set(g->compress_scale, p->compress);

  update_colorpicker_color(GTK_WIDGET(g->shadow_colorpick), p->shadow_hue, p->shadow_saturation);
  update_colorpicker_color(GTK_WIDGET(g->highlight_colorpick), p->highlight_hue, p->highlight_saturation);
  update_saturation_slider_end_color(g->shadow_sat_gslider, p->shadow_hue);
  update_saturation_slider_end_color(g->highlight_sat_gslider, p->highlight_hue);

  update_balance_slider_colors(g->balance_scale, p->shadow_hue, p->highlight_hue);
}

static inline void gui_init_section(struct dt_iop_module_t *self, char *section, GtkWidget *slider_box,
                                    GtkWidget *hue, GtkWidget *saturation, GtkWidget **picker, gboolean top)
{
  GtkWidget *label = dt_ui_section_label_new(_(section));

  gtk_box_pack_start(GTK_BOX(self->widget), label, FALSE, FALSE, 0);

  dt_bauhaus_slider_set_feedback(hue, 0);
  dt_bauhaus_slider_set_stop(hue, 0.0f  , 1.0f, 0.0f, 0.0f);
  dt_bauhaus_slider_set_stop(hue, 0.166f, 1.0f, 1.0f, 0.0f);
  dt_bauhaus_slider_set_stop(hue, 0.322f, 0.0f, 1.0f, 0.0f);
  dt_bauhaus_slider_set_stop(hue, 0.498f, 0.0f, 1.0f, 1.0f);
  dt_bauhaus_slider_set_stop(hue, 0.664f, 0.0f, 0.0f, 1.0f);
  dt_bauhaus_slider_set_stop(hue, 0.830f, 1.0f, 0.0f, 1.0f);
  dt_bauhaus_slider_set_stop(hue, 1.0f  , 1.0f, 0.0f, 0.0f);
  gtk_widget_set_tooltip_text(hue, _("select the hue tone"));
  dt_color_picker_new(self, DT_COLOR_PICKER_POINT, hue);

  dt_bauhaus_slider_set_stop(saturation, 0.0f, 0.2f, 0.2f, 0.2f);
  dt_bauhaus_slider_set_stop(saturation, 1.0f, 1.0f, 1.0f, 1.0f);
  gtk_widget_set_tooltip_text(saturation, _("select the saturation tone"));

  *picker = gtk_color_button_new();
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(*picker), FALSE);
  gtk_color_button_set_title(GTK_COLOR_BUTTON(*picker), _("select tone color"));
  g_signal_connect(G_OBJECT(*picker), "color-set", G_CALLBACK(colorpick_callback), self);

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(hbox), slider_box, TRUE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(hbox), *picker, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, FALSE, FALSE, 0);
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_splittoning_gui_data_t *g = IOP_GUI_ALLOC(splittoning);

  dt_iop_module_t *sect = DT_IOP_SECTION_FOR_PARAMS(self, N_("shadows"));
  GtkWidget *shadows_box = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  g->shadow_hue_gslider = dt_bauhaus_slider_from_params(sect, "shadow_hue");
  dt_bauhaus_slider_set_factor(g->shadow_hue_gslider, 360.0f);
  dt_bauhaus_slider_set_format(g->shadow_hue_gslider, "°");
  g->shadow_sat_gslider = dt_bauhaus_slider_from_params(sect, "shadow_saturation");

  sect = DT_IOP_SECTION_FOR_PARAMS(self, N_("highlights"));
  GtkWidget *highlights_box = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  g->highlight_hue_gslider = dt_bauhaus_slider_from_params(sect, "highlight_hue");
  dt_bauhaus_slider_set_factor(g->highlight_hue_gslider, 360.0f);
  dt_bauhaus_slider_set_format(g->highlight_hue_gslider, "°");
  g->highlight_sat_gslider = dt_bauhaus_slider_from_params(sect, "highlight_saturation");

  // start building top level widget
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  gui_init_section(self, N_("shadows"), shadows_box, g->shadow_hue_gslider, g->shadow_sat_gslider, &g->shadow_colorpick, TRUE);

  gui_init_section(self, N_("highlights"), highlights_box, g->highlight_hue_gslider, g->highlight_sat_gslider, &g->highlight_colorpick, FALSE);

  // Additional parameters
  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("properties")), FALSE, FALSE, 0);

  g->balance_scale = dt_bauhaus_slider_from_params(self, N_("balance"));
  dt_bauhaus_slider_set_feedback(g->balance_scale, 0);
  dt_bauhaus_slider_set_digits(g->balance_scale, 4);
  dt_bauhaus_slider_set_factor(g->balance_scale, -100.0);
  dt_bauhaus_slider_set_offset(g->balance_scale, +100.0);
  dt_bauhaus_slider_set_stop(g->balance_scale, 0.0f, 0.5f, 0.5f, 0.5f);
  dt_bauhaus_slider_set_stop(g->balance_scale, 1.0f, 0.5f, 0.5f, 0.5f);
  gtk_widget_set_tooltip_text(g->balance_scale, _("the balance of center of split-toning"));

  g->compress_scale = dt_bauhaus_slider_from_params(self, N_("compress"));
  dt_bauhaus_slider_set_format(g->compress_scale, "%");
  gtk_widget_set_tooltip_text(g->compress_scale, _("compress the effect on highlights/shadows and\npreserve mid-tones"));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

