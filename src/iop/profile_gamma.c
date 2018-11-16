/*
   This file is part of darktable,
   copyright (c) 2009--2010 johannes hanika.
   copyright (c) 2014 LebedevRI.
   copyright (c) 2018 Aurélien Pierre.

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
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop_math.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "gui/color_picker_proxy.h"
#include "iop/iop_api.h"
#include "common/iop_group.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE_INTROSPECTION(2, dt_iop_profilegamma_params_t)

typedef enum dt_iop_profilegamma_mode_t
{
  PROFILEGAMMA_LOG = 0,
  PROFILEGAMMA_GAMMA = 1
} dt_iop_profilegamma_mode_t;

typedef enum dt_iop_profilegamma_pickcolor_type_t
{
  DT_PICKPROFLOG_NONE = 0,
  DT_PICKPROFLOG_GREY_POINT = 1,
  DT_PICKPROFLOG_SHADOWS_RANGE = 2,
  DT_PICKPROFLOG_DYNAMIC_RANGE = 3,
  DT_PICKPROFLOG_AUTOTUNE = 4
} dt_iop_profilegamma_pickcolor_type_t;

typedef struct dt_iop_profilegamma_params_t
{
  dt_iop_profilegamma_mode_t mode;
  float linear;
  float gamma;
  float dynamic_range;
  float grey_point;
  float shadows_range;
  float security_factor;
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
  int which_colorpicker;
  dt_iop_color_picker_t color_picker;
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

int groups()
{
  return dt_iop_get_group("unbreak input profile", IOP_GROUP_COLOR);
}

int flags()
{
  return IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_INCLUDE_IN_STYLES
         | IOP_FLAGS_SUPPORTS_BLENDING;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "mode"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "linear"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "gamma"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "dynamic range"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "grey point"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "shadows range"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;

  dt_accel_connect_slider_iop(self, "mode", GTK_WIDGET(g->mode));
  dt_accel_connect_slider_iop(self, "linear", GTK_WIDGET(g->linear));
  dt_accel_connect_slider_iop(self, "gamma", GTK_WIDGET(g->gamma));
  dt_accel_connect_slider_iop(self, "dynamic range", GTK_WIDGET(g->dynamic_range));
  dt_accel_connect_slider_iop(self, "grey point", GTK_WIDGET(g->grey_point));
  dt_accel_connect_slider_iop(self, "shadows range", GTK_WIDGET(g->shadows_range));
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
  dt_gui_presets_add_generic(_("16 EV dynamic range (generic)"), self->op, self->version(), &p, sizeof(p), 1);

  // 14 EV preset
  p.dynamic_range = 14.0f;
  p.shadows_range = -10.50f;
  dt_gui_presets_add_generic(_("14 EV dynamic range (generic)"), self->op, self->version(), &p, sizeof(p), 1);

  // 12 EV preset
  p.dynamic_range = 12.0f;
  p.shadows_range = -9.0f;
  dt_gui_presets_add_generic(_("12 EV dynamic range (generic)"), self->op, self->version(), &p, sizeof(p), 1);

  // 10 EV preset
  p.dynamic_range = 10.0f;
  p.shadows_range = -7.50f;
  dt_gui_presets_add_generic(_("10 EV dynamic range (generic)"), self->op, self->version(), &p, sizeof(p), 1);

  // 08 EV preset
  p.dynamic_range = 8.0f;
  p.shadows_range = -6.0f;
  dt_gui_presets_add_generic(_("08 EV dynamic range (generic)"), self->op, self->version(), &p, sizeof(p), 1);

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

static inline float Log2(float x)
{
  if(x > 0.)
  {
    return logf(x) / logf(2.f);
  }
  else
  {
    return x;
  }
}

static inline float Log2Thres(float x, float Thres)
{
  if(x > Thres)
  {
    return logf(x) / logf(2.f);
  }
  else
  {
    return logf(Thres) / logf(2.f);
  }
}


#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_profilegamma_data_t *d = (dt_iop_profilegamma_data_t *)piece->data;
  dt_iop_profilegamma_global_data_t *gd = (dt_iop_profilegamma_global_data_t *)self->data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  cl_mem dev_table = NULL;
  cl_mem dev_coeffs = NULL;


  size_t sizes[3] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };

  if(d->mode == PROFILEGAMMA_LOG)
  {
    const float dynamic_range = d->dynamic_range;
    const float shadows_range = d->shadows_range;
    const float grey = d->grey_point / 100.0f;
    dt_opencl_set_kernel_arg(devid, gd->kernel_profilegamma_log, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_profilegamma_log, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_profilegamma_log, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_profilegamma_log, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_profilegamma_log, 4, sizeof(float), (void *)&dynamic_range);
    dt_opencl_set_kernel_arg(devid, gd->kernel_profilegamma_log, 5, sizeof(float), (void *)&shadows_range);
    dt_opencl_set_kernel_arg(devid, gd->kernel_profilegamma_log, 6, sizeof(float), (void *)&grey);

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

    dt_opencl_set_kernel_arg(devid, gd->kernel_profilegamma, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_profilegamma, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_profilegamma, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_profilegamma, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_profilegamma, 4, sizeof(cl_mem), (void *)&dev_table);
    dt_opencl_set_kernel_arg(devid, gd->kernel_profilegamma, 5, sizeof(cl_mem), (void *)&dev_coeffs);

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
  dt_print(DT_DEBUG_OPENCL, "[opencl_profilegamma] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

// From data/kernels/extended.cl
static inline float fastlog2(float x)
{
  union { float f; unsigned int i; } vx = { x };
  union { unsigned int i; float f; } mx = { (vx.i & 0x007FFFFF) | 0x3f000000 };

  float y = vx.i;

  y *= 1.1920928955078125e-7f;

  return y - 124.22551499f
    - 1.498030302f * mx.f
    - 1.72587999f / (0.3520887068f + mx.f);
}

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
#pragma omp parallel for SIMD() default(none) shared(data) schedule(static)
#endif
      for(size_t k = 0; k < (size_t)ch * roi_out->width * roi_out->height; k++)
      {
        float tmp = ((const float *)ivoid)[k] / grey;
        if (tmp < noise) tmp = noise;
        tmp = (fastlog2(tmp) - data->shadows_range) / (data->dynamic_range);

        if (tmp < noise)
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
#pragma omp parallel for default(none) shared(data) schedule(static)
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
  if(self->dt->gui->reset) return;
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;

  float grey = fmax(fmax(self->picked_color[0], self->picked_color[1]), self->picked_color[2]);
  p->grey_point = 100.f * grey;

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->grey_point, p->grey_point);
  darktable.gui->reset = 0;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void apply_auto_black(dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;

  float noise = powf(2.0f, -16.0f);

  // Black
  float black = fmax(fmax(self->picked_color_min[0], self->picked_color_min[1]), self->picked_color_min[2]);
  float EVmin = Log2Thres(black / (p->grey_point / 100.0f), noise);
  EVmin *= (1.0f + p->security_factor / 100.0f);

  p->shadows_range = EVmin;

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->shadows_range, p->shadows_range);
  darktable.gui->reset = 0;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void apply_auto_dynamic_range(dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
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

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->dynamic_range, p->dynamic_range);
  darktable.gui->reset = 0;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void linear_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  p->linear = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void gamma_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  p->gamma = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void security_threshold_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;

  dt_iop_color_picker_reset(&g->color_picker, TRUE);

  float previous = p->security_factor;
  p->security_factor = dt_bauhaus_slider_get(slider);
  float ratio = (p->security_factor - previous) / (previous + 100.0f);

  float EVmin = p->shadows_range;
  EVmin = EVmin + ratio * EVmin;

  float EVmax = p->dynamic_range + p->shadows_range;
  EVmax = EVmax + ratio * EVmax;

  p->dynamic_range = EVmax - EVmin;
  p->shadows_range = EVmin;

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->dynamic_range, p->dynamic_range);
  dt_bauhaus_slider_set_soft(g->shadows_range, p->shadows_range);
  darktable.gui->reset = 0;

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

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->grey_point, p->grey_point);
  dt_bauhaus_slider_set(g->shadows_range, p->shadows_range);
  dt_bauhaus_slider_set(g->dynamic_range, p->dynamic_range);
  darktable.gui->reset = 0;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void grey_point_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;
  dt_iop_color_picker_reset(&g->color_picker, TRUE);

  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  p->grey_point = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void dynamic_range_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;

  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;
  dt_iop_color_picker_reset(&g->color_picker, TRUE);

  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  p->dynamic_range = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void shadows_range_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;

  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;
  dt_iop_color_picker_reset(&g->color_picker, TRUE);

  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  p->shadows_range = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void mode_callback(GtkWidget *combo, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;

  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  p->mode = dt_bauhaus_combobox_get(combo);

  dt_iop_color_picker_reset(&g->color_picker, TRUE);

  switch(p->mode)
  {
    case PROFILEGAMMA_LOG:
      gtk_stack_set_visible_child_name(GTK_STACK(g->mode_stack), "log");
      break;
    case PROFILEGAMMA_GAMMA:
      gtk_stack_set_visible_child_name(GTK_STACK(g->mode_stack), "gamma");
      break;
    default:
      p->mode = PROFILEGAMMA_LOG;
      gtk_stack_set_visible_child_name(GTK_STACK(g->mode_stack), "log");
      break;
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static int _iop_color_picker_get_set(dt_iop_module_t *self, GtkWidget *button)
{
  dt_iop_profilegamma_gui_data_t *g =  (dt_iop_profilegamma_gui_data_t *)self->gui_data;

  const int current_picker = g->which_colorpicker;

  g->which_colorpicker = DT_PICKPROFLOG_NONE;

  if(button == g->grey_point)
    g->which_colorpicker = DT_PICKPROFLOG_GREY_POINT;
  else if(button == g->shadows_range)
    g->which_colorpicker = DT_PICKPROFLOG_SHADOWS_RANGE;
  else if(button == g->dynamic_range)
    g->which_colorpicker = DT_PICKPROFLOG_DYNAMIC_RANGE;
  else if(button == g->auto_button)
    g->which_colorpicker = DT_PICKPROFLOG_AUTOTUNE;

  if (current_picker == g->which_colorpicker)
    return ALREADY_SELECTED;
  else
    return g->which_colorpicker;
}

static void _iop_color_picker_apply(struct dt_iop_module_t *self)
{
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;
  switch(g->which_colorpicker)
  {
     case DT_PICKPROFLOG_GREY_POINT:
       apply_auto_grey(self);
       break;
     case DT_PICKPROFLOG_SHADOWS_RANGE:
       apply_auto_black(self);
       break;
     case DT_PICKPROFLOG_DYNAMIC_RANGE:
       apply_auto_dynamic_range(self);
       break;
     case DT_PICKPROFLOG_AUTOTUNE:
       apply_autotune(self);
       break;
     default:
       break;
  }
}

static void _iop_color_picker_update(dt_iop_module_t *self)
{
  dt_iop_profilegamma_gui_data_t *g =  (dt_iop_profilegamma_gui_data_t *)self->gui_data;
  dt_bauhaus_widget_set_quad_active(g->grey_point, g->which_colorpicker == DT_PICKPROFLOG_GREY_POINT);
  dt_bauhaus_widget_set_quad_active(g->shadows_range, g->which_colorpicker == DT_PICKPROFLOG_SHADOWS_RANGE);
  dt_bauhaus_widget_set_quad_active(g->dynamic_range, g->which_colorpicker == DT_PICKPROFLOG_DYNAMIC_RANGE);
  dt_bauhaus_widget_set_quad_active(g->auto_button, g->which_colorpicker == DT_PICKPROFLOG_AUTOTUNE);
}

static void _iop_color_picker_reset(struct dt_iop_module_t *self)
{
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;
  g->which_colorpicker = DT_PICKPROFLOG_NONE;
}

void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;

  if(!in) dt_iop_color_picker_reset(&g->color_picker, TRUE);
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
#pragma omp parallel for default(none) shared(d) schedule(static)
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
#pragma omp parallel for default(none) shared(d, a, b, c, g) schedule(static)
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
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_reset(dt_iop_module_t *self)
{
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;
  dt_iop_color_picker_reset(&g->color_picker, TRUE);
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)module->params;

  self->color_picker_box[0] = self->color_picker_box[1] = .25f;
  self->color_picker_box[2] = self->color_picker_box[3] = .75f;
  self->color_picker_point[0] = self->color_picker_point[1] = 0.5f;

  dt_iop_color_picker_reset(&g->color_picker, TRUE);

  switch(p->mode)
  {
    case PROFILEGAMMA_LOG:
      gtk_stack_set_visible_child_name(GTK_STACK(g->mode_stack), "log");
      break;
    case PROFILEGAMMA_GAMMA:
      gtk_stack_set_visible_child_name(GTK_STACK(g->mode_stack), "gamma");
      break;
    default:
      p->mode = PROFILEGAMMA_LOG;
      gtk_stack_set_visible_child_name(GTK_STACK(g->mode_stack), "log");
      break;
  }

  dt_bauhaus_combobox_set(g->mode, p->mode);
  dt_bauhaus_slider_set(g->linear, p->linear);
  dt_bauhaus_slider_set(g->gamma, p->gamma);
  dt_bauhaus_slider_set_soft(g->dynamic_range, p->dynamic_range);
  dt_bauhaus_slider_set_soft(g->grey_point, p->grey_point);
  dt_bauhaus_slider_set_soft(g->shadows_range, p->shadows_range);
  dt_bauhaus_slider_set_soft(g->security_factor, p->security_factor);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_profilegamma_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_profilegamma_params_t));
  module->default_enabled = 0;
  module->priority = 342; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_profilegamma_params_t);
  module->gui_data = NULL;
  dt_iop_profilegamma_params_t tmp
      = (dt_iop_profilegamma_params_t){ 0, 0.1, 0.45, 10., 18., -5., 0. };
  memcpy(module->params, &tmp, sizeof(dt_iop_profilegamma_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_profilegamma_params_t));
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

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
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
  self->gui_data = malloc(sizeof(dt_iop_profilegamma_gui_data_t));
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));

  // mode choice
  g->mode = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->mode, NULL, _("mode"));
  dt_bauhaus_combobox_add(g->mode, _("logarithmic"));
  dt_bauhaus_combobox_add(g->mode, _("gamma"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->mode), TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->mode, _("tone mapping method"));
  g_signal_connect(G_OBJECT(g->mode), "value-changed", G_CALLBACK(mode_callback), self);

  // prepare the modes widgets stack
  g->mode_stack = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(g->mode_stack), FALSE);
  gtk_box_pack_start(GTK_BOX(self->widget), g->mode_stack, TRUE, TRUE, 0);


  /**** GAMMA MODE ***/
  GtkWidget *vbox_gamma = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE));
  // linear slider
  g->linear = dt_bauhaus_slider_new_with_range(self, 0.0, 1.0, 0.0001, p->linear, 4);
  dt_bauhaus_widget_set_label(g->linear, NULL, _("linear"));
  gtk_box_pack_start(GTK_BOX(vbox_gamma), g->linear, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->linear, _("linear part"));
  g_signal_connect(G_OBJECT(g->linear), "value-changed", G_CALLBACK(linear_callback), self);

  // gamma slider
  g->gamma = dt_bauhaus_slider_new_with_range(self, 0.0, 1.0, 0.0001, p->gamma, 4);
  dt_bauhaus_widget_set_label(g->gamma, NULL, _("gamma"));
  gtk_box_pack_start(GTK_BOX(vbox_gamma), g->gamma, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->gamma, _("gamma exponential factor"));
  g_signal_connect(G_OBJECT(g->gamma), "value-changed", G_CALLBACK(gamma_callback), self);

  gtk_widget_show_all(vbox_gamma);
  gtk_stack_add_named(GTK_STACK(g->mode_stack), vbox_gamma, "gamma");


  /**** LOG MODE ****/

  GtkWidget *vbox_log = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE));

  // grey_point slider
  g->grey_point = dt_bauhaus_slider_new_with_range(self, 0.1, 100., 0.5, p->grey_point, 2);
  dt_bauhaus_widget_set_label(g->grey_point, NULL, _("middle grey luma"));
  gtk_box_pack_start(GTK_BOX(vbox_log), g->grey_point, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->grey_point, "%.2f %%");
  gtk_widget_set_tooltip_text(g->grey_point, _("adjust to match the average luma of the subject"));
  g_signal_connect(G_OBJECT(g->grey_point), "value-changed", G_CALLBACK(grey_point_callback), self);
  dt_bauhaus_widget_set_quad_paint(g->grey_point, dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->grey_point, TRUE);
  g_signal_connect(G_OBJECT(g->grey_point), "quad-pressed", G_CALLBACK(dt_iop_color_picker_callback), &g->color_picker);

  // Shadows range slider
  g->shadows_range = dt_bauhaus_slider_new_with_range(self, -16.0, -0.0, 0.1, p->shadows_range, 2);
  dt_bauhaus_slider_enable_soft_boundaries(g->shadows_range, -16., 16.0);
  dt_bauhaus_widget_set_label(g->shadows_range, NULL, _("black relative exposure"));
  gtk_box_pack_start(GTK_BOX(vbox_log), g->shadows_range, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->shadows_range, "%.2f EV");
  gtk_widget_set_tooltip_text(g->shadows_range, _("number of stops between middle grey and pure black\nthis is a reading a posemeter would give you on the scene"));
  g_signal_connect(G_OBJECT(g->shadows_range), "value-changed", G_CALLBACK(shadows_range_callback), self);
  dt_bauhaus_widget_set_quad_paint(g->shadows_range, dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->shadows_range, TRUE);
  g_signal_connect(G_OBJECT(g->shadows_range), "quad-pressed", G_CALLBACK(dt_iop_color_picker_callback), &g->color_picker);

  // Dynamic range slider
  g->dynamic_range = dt_bauhaus_slider_new_with_range(self, 0.5, 16.0, 0.1, p->dynamic_range, 2);
  dt_bauhaus_slider_enable_soft_boundaries(g->dynamic_range, 0.01, 32.0);
  dt_bauhaus_widget_set_label(g->dynamic_range, NULL, _("dynamic range"));
  gtk_box_pack_start(GTK_BOX(vbox_log), g->dynamic_range, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->dynamic_range, "%.2f EV");
  gtk_widget_set_tooltip_text(g->dynamic_range, _("number of stops between pure black and pure white\nthis is a reading a posemeter would give you on the scene"));
  g_signal_connect(G_OBJECT(g->dynamic_range), "value-changed", G_CALLBACK(dynamic_range_callback), self);
  dt_bauhaus_widget_set_quad_paint(g->dynamic_range, dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->dynamic_range, TRUE);
  g_signal_connect(G_OBJECT(g->dynamic_range), "quad-pressed", G_CALLBACK(dt_iop_color_picker_callback), &g->color_picker);

  // Security factor
  gtk_box_pack_start(GTK_BOX(vbox_log), dt_ui_section_label_new(_("optimize automatically")), FALSE, FALSE, 5);
  g->security_factor = dt_bauhaus_slider_new_with_range(self, -100., 100., 0.1, p->security_factor, 2);
  dt_bauhaus_widget_set_label(g->security_factor, NULL, _("safety factor"));
  gtk_box_pack_start(GTK_BOX(vbox_log), g->security_factor, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->security_factor, "%.2f %%");
  gtk_widget_set_tooltip_text(g->security_factor, _("enlarge or shrink the computed dynamic range\nthis is usefull when noise perturbates the measurements"));
  g_signal_connect(G_OBJECT(g->security_factor), "value-changed", G_CALLBACK(security_threshold_callback), self);

  // Auto tune slider
  g->auto_button = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->auto_button, NULL, _("auto tune levels"));
  dt_bauhaus_widget_set_quad_paint(g->auto_button, dtgtk_cairo_paint_colorpicker,
                                   CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->auto_button, TRUE);
  g_signal_connect(G_OBJECT(g->auto_button), "quad-pressed", G_CALLBACK(dt_iop_color_picker_callback), &g->color_picker);
  gtk_widget_set_tooltip_text(g->auto_button, _("make an optimization with some guessing"));
  gtk_box_pack_start(GTK_BOX(vbox_log), g->auto_button, TRUE, TRUE, 0);

  gtk_widget_show_all(vbox_log);
  gtk_stack_add_named(GTK_STACK(g->mode_stack), vbox_log, "log");

  switch(p->mode)
  {
    case PROFILEGAMMA_LOG:
      gtk_stack_set_visible_child_name(GTK_STACK(g->mode_stack), "log");
      break;
    case PROFILEGAMMA_GAMMA:
      gtk_stack_set_visible_child_name(GTK_STACK(g->mode_stack), "gamma");
      break;
    default:
      p->mode = PROFILEGAMMA_LOG;
      gtk_stack_set_visible_child_name(GTK_STACK(g->mode_stack), "log");
      break;
  }

  init_picker(&g->color_picker,
              self,
              _iop_color_picker_get_set,
              _iop_color_picker_apply,
              _iop_color_picker_reset,
              _iop_color_picker_update);
}


void gui_cleanup(dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
