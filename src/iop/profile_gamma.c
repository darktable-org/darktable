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
#include "common/colorspaces_inline_conversions.h"
#include "common/darktable.h"
#include "common/math.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "gui/color_picker_proxy.h"
#include "iop/iop_api.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE_INTROSPECTION(2, dt_iop_profilegamma_params_t)

typedef enum dt_iop_profilegamma_mode_t
{
  PROFILEGAMMA_LOG = 0,  // $DESCRIPTION: "logarithmic"
  PROFILEGAMMA_GAMMA = 1 // $DESCRIPTION: "gamma"
} dt_iop_profilegamma_mode_t;

typedef struct dt_iop_profilegamma_params_t
{
  dt_iop_profilegamma_mode_t mode; // $DEFAULT: PROFILEGAMMA_LOG
  float linear;          // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.1
  float gamma;           // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.45
  float dynamic_range;   // $MIN: 0.01 $MAX: 32.0 $DEFAULT: 10.0 $DESCRIPTION: "dynamic range"
  float grey_point;      // $MIN: 0.1 $MAX: 100.0 $DEFAULT: 18.0 $DESCRIPTION: "middle gray luma"
  float shadows_range;   // $MIN: -16.0 $MAX: 16.0 $DEFAULT: -5.0 $DESCRIPTION: "black relative exposure"
  float security_factor; // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "safety factor"
} dt_iop_profilegamma_params_t;

typedef struct dt_iop_profilegamma_gui_data_t
{
  GtkWidget *mode;
  GtkWidget *mode_stack;
  GtkWidget *linear;
  GtkWidget *gamma;
  GtkWidget *dynamic_range;
  GtkWidget *grey_point;
  GtkWidget *shadows_range;
  GtkWidget *security_factor;
  GtkWidget *auto_button;
} dt_iop_profilegamma_gui_data_t;

typedef struct dt_iop_profilegamma_data_t
{
  dt_iop_profilegamma_mode_t mode;
  float linear;
  float gamma;
  float table[0x10000];      // precomputed look-up table
  float unbounded_coeffs[3]; // approximation for extrapolation of curve
  float dynamic_range;
  float grey_point;
  float shadows_range;
  float security_factor;
} dt_iop_profilegamma_data_t;

typedef struct dt_iop_profilegamma_global_data_t
{
  int kernel_profilegamma;
  int kernel_profilegamma_log;
} dt_iop_profilegamma_global_data_t;


const char *name()
{
  return _("unbreak input profile");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("correct input color profiles meant to be applied on non-linear RGB"),
                                      _("corrective"),
                                      _("linear, RGB, display-referred"),
                                      _("non-linear, RGB"),
                                      _("non-linear, RGB, display-referred"));
}

int default_group()
{
  return IOP_GROUP_COLOR | IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_INCLUDE_IN_STYLES
         | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_profilegamma_params_t p;
  memset(&p, 0, sizeof(p));

  p.mode = PROFILEGAMMA_LOG;
  p.grey_point = 18.00f;
  p.security_factor = 0.0f;

  // 16 EV preset
  p.dynamic_range = 16.0f;
  p.shadows_range = -12.0f;
  dt_gui_presets_add_generic(_("16 EV dynamic range (generic)"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // 14 EV preset
  p.dynamic_range = 14.0f;
  p.shadows_range = -10.50f;
  dt_gui_presets_add_generic(_("14 EV dynamic range (generic)"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // 12 EV preset
  p.dynamic_range = 12.0f;
  p.shadows_range = -9.0f;
  dt_gui_presets_add_generic(_("12 EV dynamic range (generic)"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // 10 EV preset
  p.dynamic_range = 10.0f;
  p.shadows_range = -7.50f;
  dt_gui_presets_add_generic(_("10 EV dynamic range (generic)"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // 08 EV preset
  p.dynamic_range = 8.0f;
  p.shadows_range = -6.0f;
  dt_gui_presets_add_generic(_("08 EV dynamic range (generic)"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
}


int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params,
                  const int new_version)
{
  if(old_version == 1 && new_version == 2)
  {
    typedef struct dt_iop_profilegamma_params_v1_t
    {
      float linear;
      float gamma;
    } dt_iop_profilegamma_params_v1_t;

    dt_iop_profilegamma_params_v1_t *o = (dt_iop_profilegamma_params_v1_t *)old_params;
    dt_iop_profilegamma_params_t *n = (dt_iop_profilegamma_params_t *)new_params;
    dt_iop_profilegamma_params_t *d = (dt_iop_profilegamma_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    n->linear = o->linear;
    n->gamma = o->gamma;
    n->mode = PROFILEGAMMA_GAMMA;
    return 0;
  }
  return 1;
}


#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_profilegamma_data_t *d = (dt_iop_profilegamma_data_t *)piece->data;
  dt_iop_profilegamma_global_data_t *gd = (dt_iop_profilegamma_global_data_t *)self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  cl_mem dev_table = NULL;
  cl_mem dev_coeffs = NULL;


  size_t sizes[3] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };

  if(d->mode == PROFILEGAMMA_LOG)
  {
    const float dynamic_range = d->dynamic_range;
    const float shadows_range = d->shadows_range;
    const float grey = d->grey_point / 100.0f;
    dt_opencl_set_kernel_args(devid, gd->kernel_profilegamma_log, 0, CLARG(dev_in), CLARG(dev_out),
      CLARG(width), CLARG(height), CLARG(dynamic_range), CLARG(shadows_range), CLARG(grey));

    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_profilegamma_log, sizes);
    if(err != CL_SUCCESS) goto error;
    return TRUE;
  }
  else if(d->mode == PROFILEGAMMA_GAMMA)
  {
    dev_table = dt_opencl_copy_host_to_device(devid, d->table, 256, 256, sizeof(float));
    if(dev_table == NULL) goto error;

    dev_coeffs = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3, d->unbounded_coeffs);
    if(dev_coeffs == NULL) goto error;

    dt_opencl_set_kernel_args(devid, gd->kernel_profilegamma, 0, CLARG(dev_in), CLARG(dev_out), CLARG(width),
      CLARG(height), CLARG(dev_table), CLARG(dev_coeffs));

    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_profilegamma, sizes);
    if(err != CL_SUCCESS)
    {
      dt_opencl_release_mem_object(dev_table);
      dt_opencl_release_mem_object(dev_coeffs);
      goto error;
    }

    dt_opencl_release_mem_object(dev_table);
    dt_opencl_release_mem_object(dev_coeffs);
    return TRUE;
  }

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_profilegamma] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}
#endif

void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_profilegamma_data_t *data = (dt_iop_profilegamma_data_t *)piece->data;

  const int ch = piece->colors;

  switch(data->mode)
  {
    case PROFILEGAMMA_LOG:
    {
      const float grey = data->grey_point / 100.0f;

      /** The log2(x) -> -INF when x -> 0
      * thus very low values (noise) will get even lower, resulting in noise negative amplification,
      * which leads to pepper noise in shadows. To avoid that, we need to clip values that are noise for sure.
      * Using 16 bits RAW data, the black value (known by rawspeed for every manufacturer) could be used as a threshold.
      * However, at this point of the pixelpipe, the RAW levels have already been corrected and everything can happen with black levels
      * in the exposure module. So we define the threshold as the first non-null 16 bit integer
      */
      const float noise = powf(2.0f, -16.0f);

#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
      dt_omp_firstprivate(ch, grey, ivoid, ovoid, roi_out, noise) \
      shared(data) \
      schedule(static)
#endif
      for(size_t k = 0; k < (size_t)ch * roi_out->width * roi_out->height; k++)
      {
        float tmp = ((const float *)ivoid)[k] / grey;
        if(tmp < noise) tmp = noise;
        tmp = (fastlog2(tmp) - data->shadows_range) / (data->dynamic_range);

        if(tmp < noise)
        {
          ((float *)ovoid)[k] = noise;
        }
        else
        {
          ((float *)ovoid)[k] = tmp;
        }
      }
      break;
    }

    case PROFILEGAMMA_GAMMA:
    {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(ch, ivoid, ovoid, roi_out) \
      shared(data) \
      schedule(static)
#endif
      for(int k = 0; k < roi_out->height; k++)
      {
        const float *in = ((float *)ivoid) + (size_t)ch * k * roi_out->width;
        float *out = ((float *)ovoid) + (size_t)ch * k * roi_out->width;

        for(int j = 0; j < roi_out->width; j++, in += ch, out += ch)
        {
          for(int i = 0; i < 3; i++)
          {
            // use base curve for values < 1, else use extrapolation.
            if(in[i] < 1.0f)
              out[i] = data->table[CLAMP((int)(in[i] * 0x10000ul), 0, 0xffff)];
            else
              out[i] = dt_iop_eval_exp(data->unbounded_coeffs, in[i]);
          }
        }
      }
      break;
    }
  }

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
    dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

static void apply_auto_grey(dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;

  float grey = fmax(fmax(self->picked_color[0], self->picked_color[1]), self->picked_color[2]);
  p->grey_point = 100.f * grey;

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->grey_point, p->grey_point);
  --darktable.gui->reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void apply_auto_black(dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;

  float noise = powf(2.0f, -16.0f);

  // Black
  float black = fmax(fmax(self->picked_color_min[0], self->picked_color_min[1]), self->picked_color_min[2]);
  float EVmin = Log2Thres(black / (p->grey_point / 100.0f), noise);
  EVmin *= (1.0f + p->security_factor / 100.0f);

  p->shadows_range = EVmin;

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->shadows_range, p->shadows_range);
  --darktable.gui->reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void apply_auto_dynamic_range(dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;

  float noise = powf(2.0f, -16.0f);

  // Black
  float EVmin = p->shadows_range;

  // White
  float white = fmax(fmax(self->picked_color_max[0], self->picked_color_max[1]), self->picked_color_max[2]);
  float EVmax = Log2Thres(white / (p->grey_point / 100.0f), noise);
  EVmax *= (1.0f + p->security_factor / 100.0f);

  p->dynamic_range = EVmax - EVmin;

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->dynamic_range, p->dynamic_range);
  --darktable.gui->reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void apply_autotune(dt_iop_module_t *self)
{
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;

  float noise = powf(2.0f, -16.0f);

  // Grey
  float grey = fmax(fmax(self->picked_color[0], self->picked_color[1]), self->picked_color[2]);
  p->grey_point = 100.f * grey;

  // Black
  float black = fmax(fmax(self->picked_color_min[0], self->picked_color_min[1]), self->picked_color_min[2]);
  float EVmin = Log2Thres(black / (p->grey_point / 100.0f), noise);
  EVmin *= (1.0f + p->security_factor / 100.0f);

  // White
  float white = fmax(fmax(self->picked_color_max[0], self->picked_color_max[1]), self->picked_color_max[2]);
  float EVmax = Log2Thres(white / (p->grey_point / 100.0f), noise);
  EVmax *= (1.0f + p->security_factor / 100.0f);

  p->shadows_range = EVmin;
  p->dynamic_range = EVmax - EVmin;

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->grey_point, p->grey_point);
  dt_bauhaus_slider_set(g->shadows_range, p->shadows_range);
  dt_bauhaus_slider_set(g->dynamic_range, p->dynamic_range);
  --darktable.gui->reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;

  if(w == g->mode)
  {
    if(p->mode == PROFILEGAMMA_LOG)
    {
      gtk_stack_set_visible_child_name(GTK_STACK(g->mode_stack), "log");
    }
    else
    {
      gtk_stack_set_visible_child_name(GTK_STACK(g->mode_stack), "gamma");
    }
  }
  else if(w == g->security_factor)
  {
    float prev = *(float *)previous;
    float ratio = (p->security_factor - prev) / (prev + 100.0f);

    float EVmin = p->shadows_range;
    EVmin = EVmin + ratio * EVmin;

    float EVmax = p->dynamic_range + p->shadows_range;
    EVmax = EVmax + ratio * EVmax;

    p->dynamic_range = EVmax - EVmin;
    p->shadows_range = EVmin;

    ++darktable.gui->reset;
    dt_bauhaus_slider_set(g->dynamic_range, p->dynamic_range);
    dt_bauhaus_slider_set(g->shadows_range, p->shadows_range);
    --darktable.gui->reset;
  }
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;
  if     (picker == g->grey_point)
    apply_auto_grey(self);
  else if(picker == g->shadows_range)
    apply_auto_black(self);
  else if(picker == g->dynamic_range)
    apply_auto_dynamic_range(self);
  else if(picker == g->auto_button)
    apply_autotune(self);
  else
    fprintf(stderr, "[profile_gamma] unknown color picker\n");
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)p1;
  dt_iop_profilegamma_data_t *d = (dt_iop_profilegamma_data_t *)piece->data;

  const float linear = p->linear;
  const float gamma = p->gamma;

  d->linear = p->linear;
  d->gamma = p->gamma;

  float a, b, c, g;
  if(gamma == 1.0)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(d) schedule(static)
#endif
    for(int k = 0; k < 0x10000; k++) d->table[k] = 1.0 * k / 0x10000;
  }
  else
  {
    if(linear == 0.0)
    {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(gamma) \
      shared(d) \
      schedule(static)
#endif
      for(int k = 0; k < 0x10000; k++) d->table[k] = powf(1.00 * k / 0x10000, gamma);
    }
    else
    {
      if(linear < 1.0)
      {
        g = gamma * (1.0 - linear) / (1.0 - gamma * linear);
        a = 1.0 / (1.0 + linear * (g - 1));
        b = linear * (g - 1) * a;
        c = powf(a * linear + b, g) / linear;
      }
      else
      {
        a = b = g = 0.0;
        c = 1.0;
      }
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(linear) \
      shared(d, a, b, c, g) \
      schedule(static)
#endif
      for(int k = 0; k < 0x10000; k++)
      {
        float tmp;
        if(k < 0x10000 * linear)
          tmp = c * k / 0x10000;
        else
          tmp = powf(a * k / 0x10000 + b, g);
        d->table[k] = tmp;
      }
    }
  }

  // now the extrapolation stuff:
  const float x[4] = { 0.7f, 0.8f, 0.9f, 1.0f };
  const float y[4]
      = { d->table[CLAMP((int)(x[0] * 0x10000ul), 0, 0xffff)],
          d->table[CLAMP((int)(x[1] * 0x10000ul), 0, 0xffff)],
          d->table[CLAMP((int)(x[2] * 0x10000ul), 0, 0xffff)],
          d->table[CLAMP((int)(x[3] * 0x10000ul), 0, 0xffff)] };
  dt_iop_estimate_exp(x, y, 4, d->unbounded_coeffs);

  d->dynamic_range = p->dynamic_range;
  d->grey_point = p->grey_point;
  d->shadows_range = p->shadows_range;
  d->security_factor = p->security_factor;
  d->mode = p->mode;

  //piece->process_cl_ready = 1;

  // no OpenCL for log yet.
  //if(d->mode == PROFILEGAMMA_LOG) piece->process_cl_ready = 0;
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_profilegamma_data_t));
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_reset(dt_iop_module_t *self)
{
  dt_iop_color_picker_reset(self, TRUE);
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;

  dt_iop_color_picker_reset(self, TRUE);

  gui_changed(self, g->mode, 0);
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_profilegamma_global_data_t *gd
      = (dt_iop_profilegamma_global_data_t *)malloc(sizeof(dt_iop_profilegamma_global_data_t));

  module->data = gd;
  gd->kernel_profilegamma = dt_opencl_create_kernel(program, "profilegamma");
  gd->kernel_profilegamma_log = dt_opencl_create_kernel(program, "profilegamma_log");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_profilegamma_global_data_t *gd = (dt_iop_profilegamma_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_profilegamma);
  dt_opencl_free_kernel(gd->kernel_profilegamma_log);
  free(module->data);
  module->data = NULL;
}


void gui_init(dt_iop_module_t *self)
{
  dt_iop_profilegamma_gui_data_t *g = IOP_GUI_ALLOC(profilegamma);

  // prepare the modes widgets stack
  g->mode_stack = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(g->mode_stack), FALSE);

  /**** GAMMA MODE ***/

  GtkWidget *vbox_gamma = self->widget = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE));

  g->linear = dt_bauhaus_slider_from_params(self, N_("linear"));
  dt_bauhaus_slider_set_digits(g->linear, 4);
  gtk_widget_set_tooltip_text(g->linear, _("linear part"));

  g->gamma = dt_bauhaus_slider_from_params(self, N_("gamma"));
  dt_bauhaus_slider_set_digits(g->gamma, 4);
  gtk_widget_set_tooltip_text(g->gamma, _("gamma exponential factor"));

  gtk_stack_add_named(GTK_STACK(g->mode_stack), vbox_gamma, "gamma");

  /**** LOG MODE ****/

  GtkWidget *vbox_log = self->widget = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE));

  g->grey_point
      = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "grey_point"));
  dt_bauhaus_slider_set_format(g->grey_point, "%");
  gtk_widget_set_tooltip_text(g->grey_point, _("adjust to match the average luma of the subject"));

  g->shadows_range
      = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "shadows_range"));
  dt_bauhaus_slider_set_soft_max(g->shadows_range, 0.0);
  dt_bauhaus_slider_set_format(g->shadows_range, _(" EV"));
  gtk_widget_set_tooltip_text(g->shadows_range, _("number of stops between middle gray and pure black\nthis is a reading a posemeter would give you on the scene"));

  g->dynamic_range
      = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "dynamic_range"));
  dt_bauhaus_slider_set_soft_range(g->dynamic_range, 0.5, 16.0);
  dt_bauhaus_slider_set_format(g->dynamic_range, _(" EV"));
  gtk_widget_set_tooltip_text(g->dynamic_range, _("number of stops between pure black and pure white\nthis is a reading a posemeter would give you on the scene"));

  gtk_box_pack_start(GTK_BOX(vbox_log), dt_ui_section_label_new(_("optimize automatically")), FALSE, FALSE, 0);

  g->security_factor = dt_bauhaus_slider_from_params(self, "security_factor");
  dt_bauhaus_slider_set_format(g->security_factor, "%");
  gtk_widget_set_tooltip_text(g->security_factor, _("enlarge or shrink the computed dynamic range\nthis is useful when noise perturbates the measurements"));

  g->auto_button = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_combobox_new(self));
  dt_bauhaus_widget_set_label(g->auto_button, NULL, N_("auto tune levels"));
  gtk_widget_set_tooltip_text(g->auto_button, _("make an optimization with some guessing"));
  gtk_box_pack_start(GTK_BOX(vbox_log), g->auto_button, TRUE, TRUE, 0);

  gtk_stack_add_named(GTK_STACK(g->mode_stack), vbox_log, "log");

  // start building top level widget
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->mode = dt_bauhaus_combobox_from_params(self, N_("mode"));
  gtk_widget_set_tooltip_text(g->mode, _("tone mapping method"));

  gtk_box_pack_start(GTK_BOX(self->widget), g->mode_stack, TRUE, TRUE, 0);
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

