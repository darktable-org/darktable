/*
  This file is part of darktable,
  Copyright (C) 2010-2020 darktable developers.

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
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

#include <assert.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/** Crazy presets b&w ...
  Film Type     R   G   B           R G B
  AGFA 200X   18    41    41    Ilford Pan F    33  36  31
  Agfapan 25    25    39    36    Ilford SFX    36  31  33
  Agfapan 100   21    40    39    Ilford XP2 Super  21  42  37
  Agfapan 400   20    41    39    Kodak T-Max 100 24  37  39
  Ilford Delta 100  21    42    37    Kodak T-Max 400 27  36  37
  Ilford Delta 400  22    42    36    Kodak Tri-X 400 25  35  40
  Ilford Delta 3200 31    36    33    Normal Contrast 43  33  30
  Ilford FP4    28    41    31    High Contrast   40  34  60
  Ilford HP5    23    37    40    Generic B/W   24  68  8
*/

#define CLIP(x) ((x < 0) ? 0.0 : (x > 1.0) ? 1.0 : x)

DT_MODULE_INTROSPECTION(1, dt_iop_channelmixer_params_t)

typedef enum _channelmixer_output_t
{
  /** mixes into hue channel */
  CHANNEL_HUE = 0,
  /** mixes into lightness channel */
  CHANNEL_SATURATION,
  /** mixes into lightness channel */
  CHANNEL_LIGHTNESS,
  /** mixes into red channel of image */
  CHANNEL_RED,
  /** mixes into green channel of image */
  CHANNEL_GREEN,
  /** mixes into blue channel of image */
  CHANNEL_BLUE,
  /** mixes into gray channel of image = monochrome*/
  CHANNEL_GRAY,

  CHANNEL_SIZE
} _channelmixer_output_t;

typedef struct dt_iop_channelmixer_params_t
{
  //_channelmixer_output_t output_channel;
  /** amount of red to mix value */
  float red[CHANNEL_SIZE];   // $MIN: -1.0 $MAX: 1.0 
  /** amount of green to mix value */
  float green[CHANNEL_SIZE]; // $MIN: -1.0 $MAX: 1.0 
  /** amount of blue to mix value */
  float blue[CHANNEL_SIZE];  // $MIN: -1.0 $MAX: 1.0 
} dt_iop_channelmixer_params_t;

typedef struct dt_iop_channelmixer_gui_data_t
{
  GtkBox *vbox;
  GtkWidget *output_channel;                      // Output channel
  GtkWidget *scale_red, *scale_green, *scale_blue;    // red, green, blue
  GtkWidget *normalise;              // normalise inputs for greymix to sum to 1.0
} dt_iop_channelmixer_gui_data_t;

typedef struct dt_iop_channelmixer_data_t
{
  //_channelmixer_output_t output_channel;
  float red[CHANNEL_SIZE];
  float green[CHANNEL_SIZE];
  float blue[CHANNEL_SIZE];
} dt_iop_channelmixer_data_t;

typedef struct dt_iop_channelmixer_global_data_t
{
  int kernel_channelmixer;
} dt_iop_channelmixer_global_data_t;


const char *name()
{
  return _("channel mixer");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_group()
{
  return IOP_GROUP_COLOR;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_rgb;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "red"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "green"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "blue"));
  dt_accel_register_combobox_iop(self, FALSE, NC_("accel", "destination"));
  dt_accel_register_combobox_iop(self, FALSE, NC_("accel", "normalise"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_channelmixer_gui_data_t *g =
    (dt_iop_channelmixer_gui_data_t*)self->gui_data;

  dt_accel_connect_slider_iop(self, "red", GTK_WIDGET(g->scale_red));
  dt_accel_connect_slider_iop(self, "green", GTK_WIDGET(g->scale_green));
  dt_accel_connect_slider_iop(self, "blue", GTK_WIDGET(g->scale_blue));
  dt_accel_connect_combobox_iop(self, "destination", GTK_WIDGET(g->output_channel));
  dt_accel_connect_combobox_iop(self, "normalise", GTK_WIDGET(g->normalise));
}

const int which_channel(dt_iop_module_t *self)
{
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)self->params;
  const gboolean do_gray = (p->red[CHANNEL_GRAY] != 0.0 || p->green[CHANNEL_GRAY] != 0.0
                                  || p->blue[CHANNEL_GRAY] != 0.0);
  gboolean do_hsl = FALSE;
  for(int chan = CHANNEL_HUE; chan <= CHANNEL_LIGHTNESS; chan++)
      do_hsl |= (p->red[chan] != 0.0 || p->green[chan] != 0.0 || p->blue[chan] != 0.0);
  return do_gray ? CHANNEL_GRAY : do_hsl ? CHANNEL_HUE : CHANNEL_RED;
}

inline static void matrix3k(const float *in, float *out, const float *variable, int k)
{
  for(int out_i = 0; out_i < k; out_i ++)
  {
    float *ptr = out_i + out;
    *ptr = 0.0f;
    for(int in_i = 0; in_i < 3; in_i++)
    {
      *ptr += *(variable + out_i + in_i * CHANNEL_SIZE) * *(in_i + in);
    }
  }
  for(int out_i = k; out_i < 3; out_i ++)
  {
    float *ptr = out_i + out;
    *ptr = *out;
  }
}

inline static void run_process(const float *data_start, const int channel, const int mat_row, const float *ivoid,
                               float *ovoid, const dt_iop_roi_t *const roi_out, int ch)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(channel, mat_row, ch, ivoid, ovoid, roi_out) \
  shared(data_start) \
  schedule(static)
#endif
 for(int j = 0; j < roi_out->height; j++)
  {
    const float *in = ((float *)ivoid) + (size_t)ch * j * roi_out->width;
    float *out = ((float *)ovoid) + (size_t)ch * j * roi_out->width;
    for(int i = 0; i < roi_out->width; i++)
    {
      matrix3k(in, out, data_start + channel, mat_row);
      out += ch;
      in += ch;
    }
  }
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_channelmixer_data_t *data = (dt_iop_channelmixer_data_t *)piece->data;
  const int ch = piece->colors;
  const float *start_address = data->red;
  const gboolean gray_mix_mode = (data->red[CHANNEL_GRAY] != 0.0 || data->green[CHANNEL_GRAY] != 0.0
                                  || data->blue[CHANNEL_GRAY] != 0.0);
  gboolean hsl_mode = FALSE;
  for(int chan = CHANNEL_HUE; chan <= CHANNEL_LIGHTNESS; chan++)
      hsl_mode |= (data->red[chan] != 0.0 || data->green[chan] != 0.0 || data->blue[chan] != 0.0);

  if(gray_mix_mode)
      run_process(start_address, CHANNEL_GRAY, 1, ivoid, ovoid, roi_out, ch);
  else if(!hsl_mode)
      run_process(start_address, CHANNEL_RED, 3, ivoid, ovoid, roi_out, ch);
  else
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(start_address, ch, ivoid, ovoid, roi_out) \
  shared(data) \
  schedule(static)
#endif
    for(int j = 0; j < roi_out->height; j++)
    {
      const float *in = ((float *)ivoid) + (size_t)ch * j * roi_out->width;
      float *out = ((float *)ovoid) + (size_t)ch * j * roi_out->width;
      for(int i = 0; i < roi_out->width; i++)
      {
        float hsl_mix[3] = { 0.0f };
        float temp_out[3] = { 0.0f };
        matrix3k(in, hsl_mix, start_address + CHANNEL_HUE, 3);
        for(int k=0; k<3; k++)
          hsl_mix[k] = CLIP(hsl_mix[k]);
        hsl2rgb(temp_out, *hsl_mix, *(hsl_mix + 1), *(hsl_mix + 2));

        matrix3k(temp_out, out, start_address + CHANNEL_RED, 3);        
        out += ch;
        in += ch;
      }
    }
  }  
  
  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_channelmixer_data_t *data = (dt_iop_channelmixer_data_t *)piece->data;
  dt_iop_channelmixer_global_data_t *gd = (dt_iop_channelmixer_global_data_t *)self->global_data;

  cl_mem dev_red = NULL;
  cl_mem dev_green = NULL;
  cl_mem dev_blue = NULL;

  cl_int err = -999;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const int gray_mix_mode = (data->red[CHANNEL_GRAY] != 0.0f || data->green[CHANNEL_GRAY] != 0.0f
                             || data->blue[CHANNEL_GRAY] != 0.0f)
                                ? TRUE : FALSE;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };

  dev_red = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * CHANNEL_SIZE, data->red);
  if(dev_red == NULL) goto error;
  dev_green = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * CHANNEL_SIZE, data->green);
  if(dev_green == NULL) goto error;
  dev_blue = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * CHANNEL_SIZE, data->blue);
  if(dev_blue == NULL) goto error;

  dt_opencl_set_kernel_arg(devid, gd->kernel_channelmixer, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_channelmixer, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_channelmixer, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_channelmixer, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_channelmixer, 4, sizeof(int), (void *)&gray_mix_mode);
  dt_opencl_set_kernel_arg(devid, gd->kernel_channelmixer, 5, sizeof(cl_mem), (void *)&dev_red);
  dt_opencl_set_kernel_arg(devid, gd->kernel_channelmixer, 6, sizeof(cl_mem), (void *)&dev_green);
  dt_opencl_set_kernel_arg(devid, gd->kernel_channelmixer, 7, sizeof(cl_mem), (void *)&dev_blue);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_channelmixer, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_red);
  dt_opencl_release_mem_object(dev_green);
  dt_opencl_release_mem_object(dev_blue);

  return TRUE;

error:
  dt_opencl_release_mem_object(dev_red);
  dt_opencl_release_mem_object(dev_green);
  dt_opencl_release_mem_object(dev_blue);
  dt_print(DT_DEBUG_OPENCL, "[opencl_channelmixer] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void init_global(dt_iop_module_so_t *module)
{
  const int program = 8; // extended.cl, from programs.conf
  dt_iop_channelmixer_global_data_t *gd
      = (dt_iop_channelmixer_global_data_t *)malloc(sizeof(dt_iop_channelmixer_global_data_t));
  module->data = gd;
  gd->kernel_channelmixer = dt_opencl_create_kernel(program, "channelmixer");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_channelmixer_global_data_t *gd = (dt_iop_channelmixer_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_channelmixer);
  free(module->data);
  module->data = NULL;
}

static void setting_limits(dt_iop_module_t *self, const int color)
{
  if(darktable.gui->reset) return;

  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)self->params;
  dt_iop_channelmixer_gui_data_t *g = (dt_iop_channelmixer_gui_data_t *)self->gui_data;
  const int output = dt_bauhaus_combobox_get(g->output_channel);
  const float low_lim = dt_conf_get_float("channel_mixer_lower_limit");
  const float up_lim = dt_conf_get_float("channel_mixer_upper_limit");
  const float offset=fmaxf(0.1f, -2.0f * low_lim);
  
  float chan[] = { p->red[output], p->green[output], p->blue[output] };
  float delta[3] = { 0 };
  delta[0] = dt_bauhaus_slider_get(g->scale_red);
  delta[1] = dt_bauhaus_slider_get(g->scale_green);
  delta[2] = dt_bauhaus_slider_get(g->scale_blue);
  
  chan[color] = delta[color];
  float sum_p = 0;
  for(int i = 0; i < 3; i++)
  {
    chan[i] = CLAMP(chan[i], low_lim, up_lim) + offset;
    sum_p += chan[i];
  }

  for(int i = 0; i < 3; i++)
  {
    chan[i] *= (1.0f + 3.0f * offset) / sum_p;
    chan[i] -= offset;
  }

  p->red[output] = chan[0];
  p->green[output] = chan[1];
  p->blue[output] = chan[2];
  
  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->scale_red, chan[0]);
  dt_bauhaus_slider_set(g->scale_green, chan[1]);
  dt_bauhaus_slider_set(g->scale_blue, chan[2]);
  --darktable.gui->reset;
}

static void red_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)self->params;
  dt_iop_channelmixer_gui_data_t *g = (dt_iop_channelmixer_gui_data_t *)self->gui_data;
  const int output_channel_index = dt_bauhaus_combobox_get(g->output_channel);
  const gboolean normal = dt_bauhaus_combobox_get(g->normalise);
  
  if(output_channel_index >= 0)
  {
    if((output_channel_index >= CHANNEL_LIGHTNESS) & normal)
      setting_limits(self, 0);
    else
      p->red[output_channel_index] = dt_bauhaus_slider_get(slider);
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void green_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)self->params;
  dt_iop_channelmixer_gui_data_t *g = (dt_iop_channelmixer_gui_data_t *)self->gui_data;
  const int output_channel_index = dt_bauhaus_combobox_get(g->output_channel);
  const gboolean normal = dt_bauhaus_combobox_get(g->normalise);
  
  if(output_channel_index >= 0)
  {
    if((output_channel_index >= CHANNEL_LIGHTNESS) & normal)
      setting_limits(self, 1);
    else
      p->green[output_channel_index] = dt_bauhaus_slider_get(slider);
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void blue_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)self->params;
  dt_iop_channelmixer_gui_data_t *g = (dt_iop_channelmixer_gui_data_t *)self->gui_data;
  const int output_channel_index = dt_bauhaus_combobox_get(g->output_channel);
  const gboolean normal = dt_bauhaus_combobox_get(g->normalise);
  
  if(output_channel_index >= 0)
  {
    if((output_channel_index >= CHANNEL_LIGHTNESS) & normal)
      setting_limits(self, 2);
    else
      p->blue[output_channel_index] = dt_bauhaus_slider_get(slider);
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void output_callback(GtkComboBox *combo, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)self->params;
  dt_iop_channelmixer_gui_data_t *g = (dt_iop_channelmixer_gui_data_t *)self->gui_data;
  const int output_channel_index = dt_bauhaus_combobox_get(g->output_channel);

  if(output_channel_index >= 0)
  {
    dt_bauhaus_slider_set(g->scale_red, p->red[output_channel_index]);
    dt_bauhaus_slider_set(g->scale_green, p->green[output_channel_index]);
    dt_bauhaus_slider_set(g->scale_blue, p->blue[output_channel_index]);
    dt_bauhaus_combobox_set(g->normalise, output_channel_index >= CHANNEL_LIGHTNESS);
    gtk_widget_set_visible(g->normalise, output_channel_index >= CHANNEL_LIGHTNESS);
  }
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)p1;
  dt_iop_channelmixer_data_t *d = (dt_iop_channelmixer_data_t *)piece->data;

  for(int i = 0; i < CHANNEL_SIZE; i++)
  {
    d->red[i] = p->red[i];
    d->blue[i] = p->blue[i];
    d->green[i] = p->green[i];
  }
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_channelmixer_data_t));
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
  dt_iop_channelmixer_gui_data_t *g = (dt_iop_channelmixer_gui_data_t *)self->gui_data;
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)module->params;

  const int output_channel_index = dt_bauhaus_combobox_get(g->output_channel);

  if(output_channel_index >= 0)
  {
    const int use_channel = which_channel(self);
    dt_bauhaus_combobox_set(g->output_channel, use_channel);
    dt_bauhaus_combobox_set(g->normalise, use_channel >= CHANNEL_LIGHTNESS);
    dt_bauhaus_slider_set(g->scale_red, p->red[use_channel]);
    dt_bauhaus_slider_set(g->scale_green, p->green[use_channel]);
    dt_bauhaus_slider_set(g->scale_blue, p->blue[use_channel]);
  }
}

void init(dt_iop_module_t *module)
{
  dt_iop_default_init(module);

  dt_iop_channelmixer_params_t *d = module->default_params;

  d->red[CHANNEL_RED] = d->green[CHANNEL_GREEN] = d->blue[CHANNEL_BLUE] = 1.0;

  memcpy(module->params, module->default_params, sizeof(dt_iop_channelmixer_params_t));
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_channelmixer_gui_data_t));
  dt_iop_channelmixer_gui_data_t *g = (dt_iop_channelmixer_gui_data_t *)self->gui_data;
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  /* output */
  g->output_channel = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->output_channel, NULL, _("destination"));
  dt_bauhaus_combobox_add(g->output_channel, _("hue"));
  dt_bauhaus_combobox_add(g->output_channel, _("saturation"));
  dt_bauhaus_combobox_add(g->output_channel, _("lightness"));
  dt_bauhaus_combobox_add(g->output_channel, _("red"));
  dt_bauhaus_combobox_add(g->output_channel, _("green"));
  dt_bauhaus_combobox_add(g->output_channel, _("blue"));
  dt_bauhaus_combobox_add(g->output_channel, C_("channelmixer", "gray"));
  dt_bauhaus_combobox_set(g->output_channel, CHANNEL_RED);
  
  g_signal_connect(G_OBJECT(g->output_channel), "value-changed", G_CALLBACK(output_callback), self);
  const int use_channel = which_channel(self);
  dt_bauhaus_combobox_set(g->output_channel, use_channel);
  
  /* normalise */
  g->normalise = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->normalise, NULL, _("normalise"));
  dt_bauhaus_combobox_add(g->normalise, _("no"));
  dt_bauhaus_combobox_add(g->normalise, _("yes"));
  dt_bauhaus_combobox_set(g->normalise, use_channel >= CHANNEL_LIGHTNESS);
  gtk_widget_set_visible(g->normalise, use_channel >= CHANNEL_LIGHTNESS);
  gtk_widget_set_tooltip_text(g->normalise, _("inputs sum to one"));
  
  const float low_lim = dt_conf_get_float("channel_mixer_lower_limit");
  const float up_lim = dt_conf_get_float("channel_mixer_upper_limit");
  const float step = 0.01f;

  /* red */
  g->scale_red = dt_bauhaus_slider_new_with_range(self, low_lim, up_lim, step, p->red[use_channel], 2);
  gtk_widget_set_tooltip_text(g->scale_red, _("amount of red channel in the output channel"));
  dt_bauhaus_widget_set_label(g->scale_red, NULL, _("red"));
  g_signal_connect(G_OBJECT(g->scale_red), "value-changed", G_CALLBACK(red_callback), self);

  /* green */
  g->scale_green = dt_bauhaus_slider_new_with_range(self, low_lim, up_lim, step, p->green[use_channel], 2);
  gtk_widget_set_tooltip_text(g->scale_green, _("amount of green channel in the output channel"));
  dt_bauhaus_widget_set_label(g->scale_green, NULL, _("green"));
  g_signal_connect(G_OBJECT(g->scale_green), "value-changed", G_CALLBACK(green_callback), self);

  /* blue */
  g->scale_blue = dt_bauhaus_slider_new_with_range(self, low_lim, up_lim, step, p->blue[use_channel], 2);
  gtk_widget_set_tooltip_text(g->scale_blue, _("amount of blue channel in the output channel"));
  dt_bauhaus_widget_set_label(g->scale_blue, NULL, _("blue"));
  g_signal_connect(G_OBJECT(g->scale_blue), "value-changed", G_CALLBACK(blue_callback), self);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->normalise), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->output_channel), TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale_red), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale_green), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale_blue), TRUE, TRUE, 0);
}

void init_presets(dt_iop_module_so_t *self)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "BEGIN", NULL, NULL, NULL);

  dt_gui_presets_add_generic(_("swap R and B"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 0, 0, 1, 0 },
                                                              { 0, 0, 0, 0, 1, 0, 0 },
                                                              { 0, 0, 0, 1, 0, 0, 0 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("swap G and B"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 1, 0, 0, 0 },
                                                              { 0, 0, 0, 0, 0, 1, 0 },
                                                              { 0, 0, 0, 0, 1, 0, 0 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("color contrast boost"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0.8, 1, 0, 0, 0 },
                                                              { 0, 0, 0.1, 0, 1, 0, 0 },
                                                              { 0, 0, 0.1, 0, 0, 1, 0 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("color details boost"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0.1, 1, 0, 0, 0 },
                                                              { 0, 0, 0.8, 0, 1, 0, 0 },
                                                              { 0, 0, 0.1, 0, 0, 1, 0 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("color artifacts boost"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0.1, 1, 0, 0, 0 },
                                                              { 0, 0, 0.1, 0, 1, 0, 0 },
                                                              { 0, 0, 0.800, 0, 0, 1, 0 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("B/W luminance-based"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 1, 0, 0, 0.21 },
                                                              { 0, 0, 0, 0, 1, 0, 0.72 },
                                                              { 0, 0, 0, 0, 0, 1, 0.07 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("B/W artifacts boost"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 1, 0, 0, -0.275 },
                                                              { 0, 0, 0, 0, 1, 0, -0.275 },
                                                              { 0, 0, 0, 0, 0, 1, 1.275 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("B/W smooth skin"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 1, 0, 0, 1.0 },
                                                              { 0, 0, 0, 0, 0, 1, 0.325 },
                                                              { 0, 0, 0, 0, 0, 0, -0.4 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("B/W blue artifacts reduce"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 0, 0, 0, 0.4 },
                                                              { 0, 0, 0, 0, 0, 0, 0.750 },
                                                              { 0, 0, 0, 0, 0, 0, -0.15 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);

  dt_gui_presets_add_generic(_("B/W Ilford Delta 100-400"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 0, 0, 0, 0.21 },
                                                              { 0, 0, 0, 0, 0, 0, 0.42 },
                                                              { 0, 0, 0, 0, 0, 0, 0.37 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);

  dt_gui_presets_add_generic(_("B/W Ilford Delta 3200"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 0, 0, 0, 0.31 },
                                                              { 0, 0, 0, 0, 0, 0, 0.36 },
                                                              { 0, 0, 0, 0, 0, 0, 0.33 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);

  dt_gui_presets_add_generic(_("B/W Ilford FP4"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 0, 0, 0, 0.28 },
                                                              { 0, 0, 0, 0, 0, 0, 0.41 },
                                                              { 0, 0, 0, 0, 0, 0, 0.31 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);

  dt_gui_presets_add_generic(_("B/W Ilford HP5"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 0, 0, 0, 0.23 },
                                                              { 0, 0, 0, 0, 0, 0, 0.37 },
                                                              { 0, 0, 0, 0, 0, 0, 0.40 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);

  dt_gui_presets_add_generic(_("B/W Ilford SFX"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 0, 0, 0, 0.36 },
                                                              { 0, 0, 0, 0, 0, 0, 0.31 },
                                                              { 0, 0, 0, 0, 0, 0, 0.33 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);

  dt_gui_presets_add_generic(_("B/W Kodak T-Max 100"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 0, 0, 0, 0.24 },
                                                              { 0, 0, 0, 0, 0, 0, 0.37 },
                                                              { 0, 0, 0, 0, 0, 0, 0.39 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);

  dt_gui_presets_add_generic(_("B/W Kodak T-max 400"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 0, 0, 0, 0.27 },
                                                              { 0, 0, 0, 0, 0, 0, 0.36 },
                                                              { 0, 0, 0, 0, 0, 0, 0.37 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);

  dt_gui_presets_add_generic(_("B/W Kodak Tri-X 400"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 0, 0, 0, 0.25 },
                                                              { 0, 0, 0, 0, 0, 0, 0.35 },
                                                              { 0, 0, 0, 0, 0, 0, 0.40 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("Color"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 1, 0, 0, 0 },
                                                              { 0, 0, 0, 0, 1, 0, 0 },
                                                              { 0, 0, 0, 0, 0, 1, 0 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);


  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "COMMIT", NULL, NULL, NULL);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
