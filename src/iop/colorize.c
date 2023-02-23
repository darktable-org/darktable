/*
    This file is part of darktable,
    Copyright (C) 2011-2023 darktable developers.

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
#include "develop/imageop_gui.h"
#include "gui/accelerators.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"
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
  float hue;                  // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0
  float saturation;           // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5
  float source_lightness_mix; // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 50.0 $DESCRIPTION: "source mix"
  float lightness;            // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 50.0
  int version;
} dt_iop_colorize_params_t;

typedef struct dt_iop_colorize_gui_data_t
{
  GtkWidget *lightness, *source_mix; //  lightness, source_lightnessmix
  GtkWidget *hue, *saturation;       // hue, saturation
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

int default_group()
{
  return IOP_GROUP_EFFECT | IOP_GROUP_GRADING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("overlay a solid color on the image"),
                                      _("creative"),
                                      _("linear or non-linear, Lab, display-referred"),
                                      _("non-linear, Lab"),
                                      _("non-linear, Lab, display-referred"));
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

void process(struct dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return;

  const float *const in = (float*)ivoid;
  float *const out = (float*)ovoid;
  dt_iop_colorize_data_t *d = (dt_iop_colorize_data_t *)piece->data;

  const float L = d->L;
  const float a = d->a;
  const float b = d->b;
  const float mix = d->mix;
  const float Lmlmix = L - (mix * 100.0f) / 2.0f;
  const size_t npixels = (size_t)roi_out->height * roi_out->width;
  const dt_aligned_pixel_t color = { 0.0f, a, b, 0.0f };

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(a, b, in, out, Lmlmix, mix, npixels, color)  \
  schedule(static)
#endif
  for(size_t k = 0; k < npixels; k++)
  {
    const float mixed_L = Lmlmix + in[4*k + 0] * mix;
    copy_pixel(out + 4*k, color);
    out[4*k] = mixed_L;
  }
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_colorize_data_t *data = (dt_iop_colorize_data_t *)piece->data;
  dt_iop_colorize_global_data_t *gd = (dt_iop_colorize_global_data_t *)self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const float L = data->L;
  const float a = data->a;
  const float b = data->b;
  const float mix = data->mix;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_colorize, width, height,
    CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(mix), CLARG(L), CLARG(a), CLARG(b));
  if(err != CL_SUCCESS) goto error;
  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_colorize] couldn't enqueue kernel! %s\n", cl_errstr(err));
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
  dt_aligned_pixel_t rgb;
  hsl2rgb(rgb, hue, 1.0, 0.5);
  dt_bauhaus_slider_set_stop(slider, 1.0, rgb[0], rgb[1], rgb[2]);
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_colorize_params_t *p = (dt_iop_colorize_params_t *)self->params;
  dt_iop_colorize_gui_data_t *g = (dt_iop_colorize_gui_data_t *)self->gui_data;

  if(w == g->hue)
  {
    update_saturation_slider_end_color(g->saturation, p->hue);
    gtk_widget_queue_draw(g->saturation);
  }
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorize_gui_data_t *g = (dt_iop_colorize_gui_data_t *)self->gui_data;
  dt_iop_colorize_params_t *p = (dt_iop_colorize_params_t *)self->params;

  // convert picker RGB 2 HSL
  float H = .0f, S = .0f, L = .0f;
  dt_aligned_pixel_t XYZ;
  dt_aligned_pixel_t rgb;
  dt_Lab_to_XYZ(self->picked_color, XYZ);
  dt_XYZ_to_sRGB(XYZ, rgb);
  rgb2hsl(rgb, &H, &S, &L);

  if(fabsf(p->hue - H) < 0.0001f && fabsf(p->saturation - S) < 0.0001f)
  {
    // interrupt infinite loops
    return;
  }

  p->hue        = H;
  p->saturation = S;

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->hue, p->hue);
  dt_bauhaus_slider_set(g->saturation, p->saturation);
  update_saturation_slider_end_color(g->saturation, p->hue);
  --darktable.gui->reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_reset(struct dt_iop_module_t *self)
{
  dt_iop_color_picker_reset(self, TRUE);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorize_params_t *p = (dt_iop_colorize_params_t *)p1;
  dt_iop_colorize_data_t *d = (dt_iop_colorize_data_t *)piece->data;

  /* create Lab */
  dt_aligned_pixel_t rgb = { 0 };
  dt_aligned_pixel_t XYZ = { 0 };
  dt_aligned_pixel_t Lab = { 0 };
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
    dt_Rec709_to_XYZ_D50(rgb, XYZ);
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
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_colorize_gui_data_t *g = (dt_iop_colorize_gui_data_t *)self->gui_data;
  dt_iop_colorize_params_t *p = (dt_iop_colorize_params_t *)self->params;

  dt_iop_color_picker_reset(self, TRUE);

  update_saturation_slider_end_color(g->saturation, p->hue);
}

void init(dt_iop_module_t *module)
{
  dt_iop_default_init(module);

  ((dt_iop_colorize_params_t *)module->default_params)->version = module->version();
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_colorize_gui_data_t *g = IOP_GUI_ALLOC(colorize);

  g->hue = dt_color_picker_new(self, DT_COLOR_PICKER_POINT, dt_bauhaus_slider_from_params(self, N_("hue")));
  dt_bauhaus_slider_set_feedback(g->hue, 0);
  dt_bauhaus_slider_set_factor(g->hue, 360.0f);
  dt_bauhaus_slider_set_format(g->hue, "°");
  dt_bauhaus_slider_set_stop(g->hue, 0.0f  , 1.0f, 0.0f, 0.0f);
  dt_bauhaus_slider_set_stop(g->hue, 0.166f, 1.0f, 1.0f, 0.0f);
  dt_bauhaus_slider_set_stop(g->hue, 0.322f, 0.0f, 1.0f, 0.0f);
  dt_bauhaus_slider_set_stop(g->hue, 0.498f, 0.0f, 1.0f, 1.0f);
  dt_bauhaus_slider_set_stop(g->hue, 0.664f, 0.0f, 0.0f, 1.0f);
  dt_bauhaus_slider_set_stop(g->hue, 0.830f, 1.0f, 0.0f, 1.0f);
  dt_bauhaus_slider_set_stop(g->hue, 1.0f  , 1.0f, 0.0f, 0.0f);
  gtk_widget_set_tooltip_text(g->hue, _("select the hue tone"));

  g->saturation = dt_bauhaus_slider_from_params(self, N_("saturation"));
  dt_bauhaus_slider_set_format(g->saturation, "%");
  dt_bauhaus_slider_set_stop(g->saturation, 0.0f, 0.2f, 0.2f, 0.2f);
  dt_bauhaus_slider_set_stop(g->saturation, 1.0f, 1.0f, 1.0f, 1.0f);
  gtk_widget_set_tooltip_text(g->saturation, _("select the saturation shadow tone"));

  g->lightness = dt_bauhaus_slider_from_params(self, N_("lightness"));
  dt_bauhaus_slider_set_format(g->lightness, "%");
  gtk_widget_set_tooltip_text(g->lightness, _("lightness of color"));

  g->source_mix = dt_bauhaus_slider_from_params(self, "source_lightness_mix");
  dt_bauhaus_slider_set_format(g->source_mix, "%");
  gtk_widget_set_tooltip_text(g->source_mix, _("mix value of source lightness"));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

