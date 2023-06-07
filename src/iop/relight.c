/*
    This file is part of darktable,
    Copyright (C) 2010-2023 darktable developers.

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

/** Note :
 * we use finite-math-only and fast-math because we have ensured no divisions by zero
 * fp-contract=fast enables hardware-accelerated Fused Multiply-Add
 **/
#if defined(__GNUC__)
#pragma GCC optimize ("finite-math-only", "no-math-errno", "fast-math", "fp-contract=fast")
#endif
 
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "bauhaus/bauhaus.h"
#include "common/opencl.h"
#include "develop/imageop_gui.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

DT_MODULE_INTROSPECTION(1, dt_iop_relight_params_t)

typedef struct dt_iop_relight_params_t
{
  float ev;     // $MIN: -2.0 $MAX: 2.0 $DEFAULT: 0.33 $DESCRIPTION: "exposure"
  float center; // $DEFAULT: 0.0
  float width; // $MIN: 2.0 $MAX: 10.0 $DEFAULT: 4.0
} dt_iop_relight_params_t;

void init_presets(dt_iop_module_so_t *self)
{
  dt_database_start_transaction(darktable.db);

  dt_gui_presets_add_generic(_("fill-light 0.25EV with 4 zones"), self->op, self->version(),
                             &(dt_iop_relight_params_t){ 0.25, 0.25, 4.0 }, sizeof(dt_iop_relight_params_t),
                             1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  dt_gui_presets_add_generic(_("fill-shadow -0.25EV with 4 zones"), self->op, self->version(),
                             &(dt_iop_relight_params_t){ -0.25, 0.25, 4.0 }, sizeof(dt_iop_relight_params_t),
                             1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  dt_database_release_transaction(darktable.db);
}

typedef struct dt_iop_relight_gui_data_t
{
  GtkWidget *exposure, *width;        // ev,width
  GtkDarktableGradientSlider *center; // center
  GtkWidget *colorpicker;             // Pick median lightness
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
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_DEPRECATED;
}

const char *deprecated_msg()
{
  return _("this module is deprecated. please use the tone equalizer module instead.");
}

int default_group()
{
  return IOP_GROUP_TONE | IOP_GROUP_GRADING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

//#define GAUSS(a, b, c, x) (a * powf(2.718281828f, (-powf((x - b), 2) / (powf(c, 2)))))
#define GAUSS(a, b, c, x) (a * expf(-(x-b)*(x-b) / (c*c)))

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

  dt_iop_relight_data_t *data = (dt_iop_relight_data_t *)piece->data;

  // Precalculate parameters for gauss function
  const float a = 1.0f;                           // Height of top
  const float b = -1.0f + (data->center * 2.0f);  // Center of top
  const float c = (data->width / 10.0f) / 2.0f;   // Width

  const size_t npixels = (size_t)roi_out->width * roi_out->height;
  const float ev = data->ev;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(a, b, c, ivoid, ovoid, npixels, ev) \
  schedule(static)
#endif
  for(size_t k = 0; k < npixels; k++)
  {
    const float *const restrict in = ((float *)ivoid) + 4*k;
    float *const restrict out = ((float *)ovoid) + 4*k;
    dt_aligned_pixel_t pixel;
    copy_pixel(pixel, in);
    const float lightness = pixel[0] / 100.0f;
    const float x = -1.0f + (lightness * 2.0f);
    float gauss = GAUSS(a, b, c, x);
    float relight = exp2f(ev * CLIP(gauss));
    pixel[0] = 100.0f * CLIP(lightness * relight);
    copy_pixel_nontemporal(out, pixel);
  }
  dt_omploop_sfence();
}


#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_relight_data_t *data = (dt_iop_relight_data_t *)piece->data;
  dt_iop_relight_global_data_t *gd = (dt_iop_relight_global_data_t *)self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const float center = data->center;
  const float wings = data->width;
  const float ev = data->ev;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_relight, width, height,
    CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(center), CLARG(wings), CLARG(ev));
  if(err != CL_SUCCESS) goto error;
  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_relight] couldn't enqueue kernel! %s\n", cl_errstr(err));
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

static void center_callback(GtkDarktableGradientSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_relight_params_t *p = (dt_iop_relight_params_t *)self->params;
  dt_iop_color_picker_reset(self, TRUE);
  p->center = dtgtk_gradient_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
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
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_relight_gui_data_t *g = (dt_iop_relight_gui_data_t *)self->gui_data;
  dt_iop_relight_params_t *p = (dt_iop_relight_params_t *)self->params;
  dtgtk_gradient_slider_set_value(g->center, p->center);
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_relight_gui_data_t *g = (dt_iop_relight_gui_data_t *)self->gui_data;
  float mean, min, max;

  if(self->picked_color_max[0] >= 0.0f)
  {
    mean = fmin(fmax(self->picked_color[0] / 100.0f, 0.0f), 1.0f);
    min = fmin(fmax(self->picked_color_min[0] / 100.0f, 0.0f), 1.0f);
    max = fmin(fmax(self->picked_color_max[0] / 100.0f, 0.0f), 1.0f);
  }
  else
  {
    mean = min = max = NAN;
  }

  dtgtk_gradient_slider_set_picker_meanminmax(DTGTK_GRADIENT_SLIDER(g->center), mean, min, max);
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_relight_gui_data_t *g = IOP_GUI_ALLOC(relight);

  g->exposure = dt_bauhaus_slider_from_params(self, "ev");
  dt_bauhaus_slider_set_format(g->exposure, _(" EV"));
  gtk_widget_set_tooltip_text(g->exposure, _("the fill-light in EV"));

  /* lightnessslider */
  GtkBox *sliderbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
#define NEUTRAL_GRAY 0.5
  static const GdkRGBA _gradient_L[]
      = { { 0, 0, 0, 1.0 }, { NEUTRAL_GRAY, NEUTRAL_GRAY, NEUTRAL_GRAY, 1.0 } };

  g->center = DTGTK_GRADIENT_SLIDER(dtgtk_gradient_slider_new_with_color_and_name(_gradient_L[0], _gradient_L[1], "gslider-relight"));
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->center), _("select the center of fill-light\nctrl+click to select an area"));
  g_signal_connect(G_OBJECT(g->center), "value-changed", G_CALLBACK(center_callback), self);
  gtk_box_pack_start(sliderbox, GTK_WIDGET(g->center), TRUE, TRUE, 0);
  g->colorpicker = dt_color_picker_new(self, DT_COLOR_PICKER_POINT_AREA, GTK_WIDGET(sliderbox));
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->colorpicker), _("toggle tool for picking median lightness in image"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(sliderbox), TRUE, FALSE, 0);

  g->width = dt_bauhaus_slider_from_params(self, N_("width"));
  gtk_widget_set_tooltip_text(g->width, _("width of fill-light area defined in zones"));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

