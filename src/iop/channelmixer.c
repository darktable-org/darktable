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
#include "develop/imageop_math.h"
#include "develop/openmp_maths.h"
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

DT_MODULE_INTROSPECTION(2, dt_iop_channelmixer_params_t)

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

typedef enum _channelmixer_algorithm_t
{
   CHANNEL_MIXER_VERSION_1 = 0,
   CHANNEL_MIXER_VERSION_2 = 1,
} _channelmixer_algorithm_t;

typedef struct dt_iop_channelmixer_params_t
{
  /** amount of red to mix value */
  float red[CHANNEL_SIZE]; // $MIN: -1.0 $MAX: 1.0
  /** amount of green to mix value */
  float green[CHANNEL_SIZE]; // $MIN: -1.0 $MAX: 1.0
  /** amount of blue to mix value */
  float blue[CHANNEL_SIZE]; // $MIN: -1.0 $MAX: 1.0
  /** algorithm version */
  _channelmixer_algorithm_t algorithm_version;
} dt_iop_channelmixer_params_t;

typedef struct dt_iop_channelmixer_gui_data_t
{
  GtkBox *vbox;
  GtkWidget *output_channel;                          // Output channel
  GtkWidget *scale_red, *scale_green, *scale_blue;    // red, green, blue
} dt_iop_channelmixer_gui_data_t;

typedef enum _channelmixer_operation_mode_t
{
  OPERATION_MODE_RGB = 0,
  OPERATION_MODE_GRAY = 1,
  OPERATION_MODE_HSL_V1 = 2,
  OPERATION_MODE_HSL_V2 = 3,
} _channelmixer_operation_mode_t;

typedef struct dt_iop_channelmixer_data_t
{
  float hsl_matrix[9];
  float rgb_matrix[9];
  _channelmixer_operation_mode_t operation_mode;
} dt_iop_channelmixer_data_t;

typedef struct dt_iop_channelmixer_global_data_t
{
  int kernel_channelmixer;
} dt_iop_channelmixer_global_data_t;


const char *name()
{
  return _("channel mixer");
}

const char *deprecated_msg()
{
  return _("this module is deprecated. please use the color calibration module instead.");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("perform color space corrections\n"
                                        "such as white balance, channels mixing\n"
                                        "and conversions to monochrome emulating film"),
                                      _("corrective or creative"),
                                      _("linear, RGB, display-referred"),
                                      _("linear, RGB"),
                                      _("linear, RGB, display-referred"));
}


int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_DEPRECATED;
}

int default_group()
{
  return IOP_GROUP_COLOR | IOP_GROUP_GRADING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params,
                  const int new_version)
{
  if(old_version == 1 && new_version == 2)
  {
    typedef struct dt_iop_channelmixer_params_v1_t
    {
      float red[7];
      float green[7];
      float blue[7];
    } dt_iop_channelmixer_params_v1_t;

    const dt_iop_channelmixer_params_v1_t *old = (dt_iop_channelmixer_params_v1_t *)old_params;
    dt_iop_channelmixer_params_t *new = (dt_iop_channelmixer_params_t *)new_params;
    dt_iop_channelmixer_params_t *defaults = (dt_iop_channelmixer_params_t *)self->default_params;

    *new = *defaults; // start with a fresh copy of default parameters
    new->algorithm_version = CHANNEL_MIXER_VERSION_1;

    // copy gray mixing parameters
    new->red[CHANNEL_GRAY] = old->red[6];
    new->green[CHANNEL_GRAY] = old->green[6];
    new->blue[CHANNEL_GRAY] = old->blue[6];

    // version 1 does not use RGB mixing when gray is enabled
    if(new->red[CHANNEL_GRAY] == 0.0f && new->green[CHANNEL_GRAY] == 0.0f && new->blue[CHANNEL_GRAY] == 0.0f)
    {
      for(int i = 0; i < 3; i++)
      {
        new->red[CHANNEL_RED + i] = old->red[3 + i];
        new->green[CHANNEL_RED + i] = old->green[3 + i];
        new->blue[CHANNEL_RED + i] = old->blue[3 + i];
      }
    }

    // copy HSL mixing parameters
    for(int i = 0; i < 3; i++)
    {
      new->red[i] = old->red[i];
      new->green[i] = old->green[i];
      new->blue[i] = old->blue[i];
    }
    return 0;
  }
  return 1;
}

static void process_hsl_v1(dt_dev_pixelpipe_iop_t *piece, const float *const restrict in,
                           float *const restrict out, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_channelmixer_data_t *data = (dt_iop_channelmixer_data_t *)piece->data;
  const float *const restrict hsl_matrix = data->hsl_matrix;
  const float *const restrict rgb_matrix = data->rgb_matrix;
  const int ch = piece->colors;
  const size_t pixel_count = (size_t)ch * roi_out->width * roi_out->height;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, pixel_count, hsl_matrix, rgb_matrix, in, out) \
  schedule(static)
#endif
  for(size_t k = 0; k < pixel_count; k += ch)
  {
    float h, s, l, hmix, smix, lmix;
    dt_aligned_pixel_t rgb;

    // Calculate the HSL mix
    hmix = clamp_simd(in[k + 0] * hsl_matrix[0]) + (in[k + 1] * hsl_matrix[1]) + (in[k + 2] * hsl_matrix[2]);
    smix = clamp_simd(in[k + 0] * hsl_matrix[3]) + (in[k + 1] * hsl_matrix[4]) + (in[k + 2] * hsl_matrix[5]);
    lmix = clamp_simd(in[k + 0] * hsl_matrix[6]) + (in[k + 1] * hsl_matrix[7]) + (in[k + 2] * hsl_matrix[8]);

    // If HSL mix is used apply to out[]
    if(hmix != 0.0f || smix != 0.0f || lmix != 0.0f)
    {
      // mix into HSL output channels
      rgb2hsl(&(in[k]), &h, &s, &l);
      h = (hmix != 0.0f) ? hmix : h;
      s = (smix != 0.0f) ? smix : s;
      l = (lmix != 0.0f) ? lmix : l;
      hsl2rgb(rgb, h, s, l);
    }
    else // no HSL copy in[] to out[]
    {
      for_each_channel(c,aligned(rgb,in)) rgb[c] = in[k + c];
    }

    // Calculate RGB mix
    for(int i = 0, j = 0; i < 3; i++, j += 3)
    {
      out[k + i] = clamp_simd(rgb_matrix[j + 0] * rgb[0]
                              + rgb_matrix[j + 1] * rgb[1]
                              + rgb_matrix[j + 2] * rgb[2]);
    }
  }
}

static void process_hsl_v2(dt_dev_pixelpipe_iop_t *piece, const float *const restrict in,
                           float *const restrict out, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_channelmixer_data_t *data = (dt_iop_channelmixer_data_t *)piece->data;
  const float *const restrict hsl_matrix = data->hsl_matrix;
  const float *const restrict rgb_matrix = data->rgb_matrix;
  const int ch = piece->colors;
  const size_t pixel_count = (size_t)ch * roi_out->width * roi_out->height;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, pixel_count, hsl_matrix, rgb_matrix, in, out) \
  schedule(static)
#endif
  for(size_t k = 0; k < pixel_count; k += ch)
  {
    dt_aligned_pixel_t rgb = { in[k], in[k + 1], in[k + 2] };

    dt_aligned_pixel_t hsl_mix;
    for(int i = 0, j = 0; i < 3; i++, j += 3)
    {
      hsl_mix[i] = clamp_simd(hsl_matrix[j + 0] * rgb[0]
                               + hsl_matrix[j + 1] * rgb[1]
                               + hsl_matrix[j + 2] * rgb[2]);
    }

    // If HSL mix is used apply to out[]
    if(hsl_mix[0] != 0.0 || hsl_mix[1] != 0.0 || hsl_mix[2] != 0.0)
    {
      dt_aligned_pixel_t hsl;
      // rgb2hsl expects all values to be clipped
      for_each_channel(c)
      {
        rgb[c] = clamp_simd(rgb[c]);
      }
      // mix into HSL output channels
      rgb2hsl(rgb, &hsl[0], &hsl[1], &hsl[2]);
      for(int i = 0; i < 3; i++)
      {
        hsl[i] = (hsl_mix[i] != 0.0f) ? hsl_mix[i] : hsl[i];
      }
      hsl2rgb(rgb, hsl[0], hsl[1], hsl[2]);
    }

    // Calculate RGB mix
    for(int i = 0, j = 0; i < 3; i++, j += 3)
    {
      out[k + i] = fmaxf(rgb_matrix[j + 0] * rgb[0]
                         + rgb_matrix[j + 1] * rgb[1]
                         + rgb_matrix[j + 2] * rgb[2], 0.0f);
    }
  }
}

static void process_rgb(dt_dev_pixelpipe_iop_t *piece, const float *const restrict in,
                        float *const restrict out, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_channelmixer_data_t *data = (dt_iop_channelmixer_data_t *)piece->data;
  const float *const restrict rgb_matrix = data->rgb_matrix;
  const int ch = piece->colors;
  const size_t pixel_count = (size_t)ch * roi_out->width * roi_out->height;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, pixel_count, rgb_matrix, in, out) \
  schedule(static)
#endif
  for(size_t k = 0; k < pixel_count; k += ch)
  {
    for(int i = 0, j = 0; i < 3; i++, j += 3)
    {
      out[k + i] = fmaxf(rgb_matrix[j + 0] * in[k + 0]
                         + rgb_matrix[j + 1] * in[k + 1]
                         + rgb_matrix[j + 2] * in[k + 2], 0.0f);
    }
  }
}

static void process_gray(dt_dev_pixelpipe_iop_t *piece, const float *const restrict in,
                         float *const restrict out, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_channelmixer_data_t *data = (dt_iop_channelmixer_data_t *)piece->data;
  const float *const restrict rgb_matrix = data->rgb_matrix;
  const int ch = piece->colors;
  const size_t pixel_count = (size_t)ch * roi_out->width * roi_out->height;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, pixel_count, rgb_matrix, in, out) \
  schedule(static)
#endif
  for(size_t k = 0; k < pixel_count; k += ch)
  {
    float gray = fmaxf(rgb_matrix[0] * in[k + 0]
                       + rgb_matrix[1] * in[k + 1]
                       + rgb_matrix[2] * in[k + 2], 0.0f);
    out[k + 0] = gray;
    out[k + 1] = gray;
    out[k + 2] = gray;
  }
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_channelmixer_data_t *data = (dt_iop_channelmixer_data_t *)piece->data;
  switch(data->operation_mode)
  {
    case OPERATION_MODE_RGB:
      process_rgb(piece, (const float *const restrict)ivoid, (float *const restrict)ovoid, roi_out);
      break;
    case OPERATION_MODE_GRAY:
      process_gray(piece, (const float *const restrict)ivoid, (float *const restrict)ovoid, roi_out);
      break;
    case OPERATION_MODE_HSL_V1:
      process_hsl_v1(piece, (const float *const restrict)ivoid, (float *const restrict)ovoid, roi_out);
      break;
    case OPERATION_MODE_HSL_V2:
      process_hsl_v2(piece, (const float *const restrict)ivoid, (float *const restrict)ovoid, roi_out);
      break;
    default:
      break;
  }
  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_channelmixer_data_t *data = (dt_iop_channelmixer_data_t *)piece->data;
  dt_iop_channelmixer_global_data_t *gd = (dt_iop_channelmixer_global_data_t *)self->global_data;

  cl_mem dev_hsl_matrix = NULL;
  cl_mem dev_rgb_matrix = NULL;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const _channelmixer_operation_mode_t operation_mode = data->operation_mode;


  dev_hsl_matrix = dt_opencl_copy_host_to_device_constant(devid, sizeof(data->hsl_matrix), data->hsl_matrix);
  if(dev_hsl_matrix == NULL) goto error;
  dev_rgb_matrix = dt_opencl_copy_host_to_device_constant(devid, sizeof(data->rgb_matrix), data->rgb_matrix);
  if(dev_rgb_matrix == NULL) goto error;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_channelmixer, width, height,
    CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(operation_mode), CLARG(dev_hsl_matrix), CLARG(dev_rgb_matrix));
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_hsl_matrix);
  dt_opencl_release_mem_object(dev_rgb_matrix);

  return TRUE;

error:
  dt_opencl_release_mem_object(dev_hsl_matrix);
  dt_opencl_release_mem_object(dev_rgb_matrix);
  dt_print(DT_DEBUG_OPENCL, "[opencl_channelmixer] couldn't enqueue kernel! %s\n", cl_errstr(err));
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

static void red_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)self->params;
  dt_iop_channelmixer_gui_data_t *g = (dt_iop_channelmixer_gui_data_t *)self->gui_data;
  const int output_channel_index = dt_bauhaus_combobox_get(g->output_channel);
  const float value = dt_bauhaus_slider_get(slider);
  if(output_channel_index >= 0 && value != p->red[output_channel_index])
  {
    p->red[output_channel_index] = value;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
}

static void green_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)self->params;
  dt_iop_channelmixer_gui_data_t *g = (dt_iop_channelmixer_gui_data_t *)self->gui_data;
  const int output_channel_index = dt_bauhaus_combobox_get(g->output_channel);
  const float value = dt_bauhaus_slider_get(slider);
  if(output_channel_index >= 0 && value != p->green[output_channel_index])
  {
    p->green[output_channel_index] = value;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
}

static void blue_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)self->params;
  dt_iop_channelmixer_gui_data_t *g = (dt_iop_channelmixer_gui_data_t *)self->gui_data;
  const int output_channel_index = dt_bauhaus_combobox_get(g->output_channel);
  const float value = dt_bauhaus_slider_get(slider);
  if(output_channel_index >= 0 && value != p->blue[output_channel_index])
  {
    p->blue[output_channel_index] = value;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
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
    dt_bauhaus_slider_set_default(g->scale_red, output_channel_index == CHANNEL_RED ? 1.0 : 0.0);
    dt_bauhaus_slider_set(g->scale_green, p->green[output_channel_index]);
    dt_bauhaus_slider_set_default(g->scale_green, output_channel_index == CHANNEL_GREEN ? 1.0 : 0.0);
    dt_bauhaus_slider_set(g->scale_blue, p->blue[output_channel_index]);
    dt_bauhaus_slider_set_default(g->scale_blue, output_channel_index == CHANNEL_BLUE ? 1.0 : 0.0);
  }
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)p1;
  dt_iop_channelmixer_data_t *d = (dt_iop_channelmixer_data_t *)piece->data;

  // HSL mixer matrix
  gboolean hsl_mix_mode = FALSE;
  for(int i = CHANNEL_HUE, k = 0; i <= CHANNEL_LIGHTNESS; i++, k += 3)
  {
    d->hsl_matrix[k + 0] = p->red[i];
    d->hsl_matrix[k + 1] = p->green[i];
    d->hsl_matrix[k + 2] = p->blue[i];
    hsl_mix_mode |= p->red[i] != 0.0f || p->green[i] != 0.0f || p->blue[i] != 0.0f;
  }

  // RGB mixer matrix
  for(int i = CHANNEL_RED, k = 0; i <= CHANNEL_BLUE; i++, k += 3)
  {
    d->rgb_matrix[k + 0] = p->red[i];
    d->rgb_matrix[k + 1] = p->green[i];
    d->rgb_matrix[k + 2] = p->blue[i];
  }

  // Gray
  dt_aligned_pixel_t graymix = { p->red[CHANNEL_GRAY], p->green[CHANNEL_GRAY], p->blue[CHANNEL_GRAY] };
  const gboolean gray_mix_mode = (graymix[0] != 0.0f || graymix[1] != 0.0f || graymix[2] != 0.0f) ? TRUE : FALSE;

  // Recompute the 3x3 RGB matrix
  if(gray_mix_mode)
  {
    dt_aligned_pixel_t mixed_gray;
    for(int i = 0; i < 3; i++)
    {
      mixed_gray[i] = (graymix[0] * d->rgb_matrix[i]
                       + graymix[1] * d->rgb_matrix[i + 3]
                       + graymix[2] * d->rgb_matrix[i + 6]);
    }
    for(int i = 0; i < 9; i += 3)
    {
      for(int j = 0; j < 3; j++)
      {
        d->rgb_matrix[i + j] = mixed_gray[j];
      }
    }
  }

  if(p->algorithm_version == CHANNEL_MIXER_VERSION_1)
  {
    d->operation_mode = OPERATION_MODE_HSL_V1;
  }
  else if(hsl_mix_mode)
  {
    d->operation_mode = OPERATION_MODE_HSL_V2;
  }
  else if(gray_mix_mode)
  {
    d->operation_mode = OPERATION_MODE_GRAY;
  }
  else
  {
    d->operation_mode = OPERATION_MODE_RGB;
  }
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_channelmixer_data_t));
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_channelmixer_gui_data_t *g = (dt_iop_channelmixer_gui_data_t *)self->gui_data;
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)self->params;

  const int output_channel_index = dt_bauhaus_combobox_get(g->output_channel);
  if(output_channel_index >= 0)
  {
    dt_bauhaus_slider_set(g->scale_red, p->red[output_channel_index]);
    dt_bauhaus_slider_set(g->scale_green, p->green[output_channel_index]);
    dt_bauhaus_slider_set(g->scale_blue, p->blue[output_channel_index]);
  }
}

void init(dt_iop_module_t *module)
{
  dt_iop_default_init(module);

  dt_iop_channelmixer_params_t *d = module->default_params;

  d->algorithm_version = CHANNEL_MIXER_VERSION_2;
  d->red[CHANNEL_RED] = d->green[CHANNEL_GREEN] = d->blue[CHANNEL_BLUE] = 1.0;
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_channelmixer_gui_data_t *g = IOP_GUI_ALLOC(channelmixer);
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)self->default_params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  /* output */
  g->output_channel = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->output_channel, NULL, N_("destination"));
  dt_bauhaus_combobox_add(g->output_channel, _("hue"));
  dt_bauhaus_combobox_add(g->output_channel, _("saturation"));
  dt_bauhaus_combobox_add(g->output_channel, _("lightness"));
  dt_bauhaus_combobox_add(g->output_channel, _("red"));
  dt_bauhaus_combobox_add(g->output_channel, _("green"));
  dt_bauhaus_combobox_add(g->output_channel, _("blue"));
  dt_bauhaus_combobox_add(g->output_channel, C_("channelmixer", "gray"));
  dt_bauhaus_combobox_set(g->output_channel, CHANNEL_RED);
  g_signal_connect(G_OBJECT(g->output_channel), "value-changed", G_CALLBACK(output_callback), self);

  /* red */
  g->scale_red = dt_bauhaus_slider_new_with_range(self, -2.0, 2.0, 0, p->red[CHANNEL_RED], 3);
  gtk_widget_set_tooltip_text(g->scale_red, _("amount of red channel in the output channel"));
  dt_bauhaus_widget_set_label(g->scale_red, NULL, N_("red"));
  g_signal_connect(G_OBJECT(g->scale_red), "value-changed", G_CALLBACK(red_callback), self);

  /* green */
  g->scale_green = dt_bauhaus_slider_new_with_range(self, -2.0, 2.0, 0, p->green[CHANNEL_RED], 3);
  gtk_widget_set_tooltip_text(g->scale_green, _("amount of green channel in the output channel"));
  dt_bauhaus_widget_set_label(g->scale_green, NULL, N_("green"));
  g_signal_connect(G_OBJECT(g->scale_green), "value-changed", G_CALLBACK(green_callback), self);

  /* blue */
  g->scale_blue = dt_bauhaus_slider_new_with_range(self, -2.0, 2.0, 0, p->blue[CHANNEL_RED], 3);
  gtk_widget_set_tooltip_text(g->scale_blue, _("amount of blue channel in the output channel"));
  dt_bauhaus_widget_set_label(g->scale_blue, NULL, N_("blue"));
  g_signal_connect(G_OBJECT(g->scale_blue), "value-changed", G_CALLBACK(blue_callback), self);


  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->output_channel), TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale_red), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale_green), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale_blue), TRUE, TRUE, 0);
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_database_start_transaction(darktable.db);

  dt_gui_presets_add_generic(_("swap R and B"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 0, 0, 1, 0 },
                                                              { 0, 0, 0, 0, 1, 0, 0 },
                                                              { 0, 0, 0, 1, 0, 0, 0 },
                                                              CHANNEL_MIXER_VERSION_2 },
                             sizeof(dt_iop_channelmixer_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  dt_gui_presets_add_generic(_("swap G and B"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 1, 0, 0, 0 },
                                                              { 0, 0, 0, 0, 0, 1, 0 },
                                                              { 0, 0, 0, 0, 1, 0, 0 },
                                                              CHANNEL_MIXER_VERSION_2 },
                             sizeof(dt_iop_channelmixer_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  dt_gui_presets_add_generic(_("color contrast boost"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0.8, 1, 0, 0, 0 },
                                                              { 0, 0, 0.1, 0, 1, 0, 0 },
                                                              { 0, 0, 0.1, 0, 0, 1, 0 },
                                                              CHANNEL_MIXER_VERSION_2 },
                             sizeof(dt_iop_channelmixer_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  dt_gui_presets_add_generic(_("color details boost"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0.1, 1, 0, 0, 0 },
                                                              { 0, 0, 0.8, 0, 1, 0, 0 },
                                                              { 0, 0, 0.1, 0, 0, 1, 0 },
                                                              CHANNEL_MIXER_VERSION_2 },
                             sizeof(dt_iop_channelmixer_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  dt_gui_presets_add_generic(_("color artifacts boost"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0.1, 1, 0, 0, 0 },
                                                              { 0, 0, 0.1, 0, 1, 0, 0 },
                                                              { 0, 0, 0.8, 0, 0, 1, 0 },
                                                              CHANNEL_MIXER_VERSION_2 },
                             sizeof(dt_iop_channelmixer_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  dt_gui_presets_add_generic(_("B/W luminance-based"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 1, 0, 0, 0.21 },
                                                              { 0, 0, 0, 0, 1, 0, 0.72 },
                                                              { 0, 0, 0, 0, 0, 1, 0.07 },
                                                              CHANNEL_MIXER_VERSION_2 },
                             sizeof(dt_iop_channelmixer_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  dt_gui_presets_add_generic(_("B/W artifacts boost"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 1, 0, 0, -0.275 },
                                                              { 0, 0, 0, 0, 1, 0, -0.275 },
                                                              { 0, 0, 0, 0, 0, 1, 1.275 },
                                                              CHANNEL_MIXER_VERSION_2 },
                             sizeof(dt_iop_channelmixer_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  dt_gui_presets_add_generic(_("B/W smooth skin"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 1, 0, 0, 1.0 },
                                                              { 0, 0, 0, 0, 1, 0, 0.325 },
                                                              { 0, 0, 0, 0, 0, 1, -0.4 },
                                                              CHANNEL_MIXER_VERSION_2 },
                             sizeof(dt_iop_channelmixer_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  dt_gui_presets_add_generic(_("B/W blue artifacts reduce"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 1, 0, 0, 0.4 },
                                                              { 0, 0, 0, 0, 1, 0, 0.750 },
                                                              { 0, 0, 0, 0, 0, 1, -0.15 },
                                                              CHANNEL_MIXER_VERSION_2 },
                             sizeof(dt_iop_channelmixer_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  dt_gui_presets_add_generic(_("B/W Ilford Delta 100-400"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 1, 0, 0, 0.21 },
                                                              { 0, 0, 0, 0, 1, 0, 0.42 },
                                                              { 0, 0, 0, 0, 0, 1, 0.37 },
                                                              CHANNEL_MIXER_VERSION_2 },
                             sizeof(dt_iop_channelmixer_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  dt_gui_presets_add_generic(_("B/W Ilford Delta 3200"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 1, 0, 0, 0.31 },
                                                              { 0, 0, 0, 0, 1, 0, 0.36 },
                                                              { 0, 0, 0, 0, 0, 1, 0.33 },
                                                              CHANNEL_MIXER_VERSION_2 },
                             sizeof(dt_iop_channelmixer_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  dt_gui_presets_add_generic(_("B/W Ilford FP4"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 1, 0, 0, 0.28 },
                                                              { 0, 0, 0, 0, 1, 0, 0.41 },
                                                              { 0, 0, 0, 0, 0, 1, 0.31 },
                                                              CHANNEL_MIXER_VERSION_2 },
                             sizeof(dt_iop_channelmixer_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  dt_gui_presets_add_generic(_("B/W Ilford HP5"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 1, 0, 0, 0.23 },
                                                              { 0, 0, 0, 0, 1, 0, 0.37 },
                                                              { 0, 0, 0, 0, 0, 1, 0.40 },
                                                              CHANNEL_MIXER_VERSION_2 },
                             sizeof(dt_iop_channelmixer_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  dt_gui_presets_add_generic(_("B/W Ilford SFX"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 1, 0, 0, 0.36 },
                                                              { 0, 0, 0, 0, 1, 0, 0.31 },
                                                              { 0, 0, 0, 0, 0, 1, 0.33 },
                                                              CHANNEL_MIXER_VERSION_2 },
                             sizeof(dt_iop_channelmixer_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  dt_gui_presets_add_generic(_("B/W Kodak T-Max 100"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 1, 0, 0, 0.24 },
                                                              { 0, 0, 0, 0, 1, 0, 0.37 },
                                                              { 0, 0, 0, 0, 0, 1, 0.39 },
                                                              CHANNEL_MIXER_VERSION_2 },
                             sizeof(dt_iop_channelmixer_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  dt_gui_presets_add_generic(_("B/W Kodak T-max 400"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 1, 0, 0, 0.27 },
                                                              { 0, 0, 0, 0, 1, 0, 0.36 },
                                                              { 0, 0, 0, 0, 0, 1, 0.37 },
                                                              CHANNEL_MIXER_VERSION_2 },
                             sizeof(dt_iop_channelmixer_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  dt_gui_presets_add_generic(_("B/W Kodak Tri-X 400"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 1, 0, 0, 0.25 },
                                                              { 0, 0, 0, 0, 1, 0, 0.35 },
                                                              { 0, 0, 0, 0, 0, 1, 0.40 },
                                                              CHANNEL_MIXER_VERSION_2 },
                             sizeof(dt_iop_channelmixer_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);


  dt_database_release_transaction(darktable.db);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

