/*
    This file is part of darktable,
    copyright (c) 2009--2012 johannes hanika.

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
#include <xmmintrin.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include "common/darktable.h"
#include "develop/develop.h"
#include "develop/tiling.h"
#include "control/control.h"
#include "common/colorspaces.h"
#include "common/opencl.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libraw/libraw.h"
#include "iop/wb_presets.c"
#include "bauhaus/bauhaus.h"

DT_MODULE(2)

#define DT_IOP_LOWEST_TEMPERATURE     3000
#define DT_IOP_HIGHEST_TEMPERATURE   12000

static const float dt_iop_temperature_rgb_r55[][12];

typedef struct dt_iop_temperature_params_t
{
  float temp_out;
  float coeffs[3];
}
dt_iop_temperature_params_t;

typedef struct dt_iop_temperature_gui_data_t
{
  GtkWidget *scale_k, *scale_tint, *scale_k_out, *scale_r, *scale_g, *scale_b;
  GtkWidget *presets;
  GtkWidget *finetune;
  int preset_cnt;
  int preset_num[50];
}
dt_iop_temperature_gui_data_t;

typedef struct dt_iop_temperature_data_t
{
  float coeffs[3];
}
dt_iop_temperature_data_t;


typedef struct dt_iop_temperature_global_data_t
{
  int kernel_whitebalance_1ui;
  int kernel_whitebalance_4f;
  int kernel_whitebalance_1f;
}
dt_iop_temperature_global_data_t;

/** this wraps gegl:temperature plus some additional whitebalance adjustments. */

/* Coefficients of rational functions of degree 5 fitted per color channel to
 * the linear RGB coordinates of the range 1000K-12000K of the Planckian locus
 * with the 20K step. Original CIE-xy data from
 *
 * http://www.aim-dtp.net/aim/technology/cie_xyz/k2xy.txt
 *
 * converted to the linear RGB space assuming the ITU-R BT.709-5/sRGB primaries
 */
static const float dt_iop_temperature_rgb_r55[][12] =
{
  {
    6.9389923563552169e-01,  2.7719388100974670e+03,
    2.0999316761104289e+07, -4.8889434162208414e+09,
    -1.1899785506796783e+07, -4.7418427686099203e+04,
    1.0000000000000000e+00,  3.5434394338546258e+03,
    -5.6159353379127791e+05,  2.7369467137870544e+08,
    1.6295814912940913e+08,  4.3975072422421846e+05
  },
  {
    9.5417426141210926e-01,  2.2041043287098860e+03,
    -3.0142332673634286e+06, -3.5111986367681120e+03,
    -5.7030969525354260e+00,  6.1810926909962016e-01,
    1.0000000000000000e+00,  1.3728609973644000e+03,
    1.3099184987576159e+06, -2.1757404458816318e+03,
    -2.3892456292510311e+00,  8.1079012401293249e-01
  },
  {
    -7.1151622540856201e+10,  3.3728185802339764e+16,
    -7.9396187338868539e+19,  2.9699115135330123e+22,
    -9.7520399221734228e+22, -2.9250107732225114e+20,
    1.0000000000000000e+00,  1.3888666482167408e+16,
    2.3899765140914549e+19,  1.4583606312383295e+23,
    1.9766018324502894e+22,  2.9395068478016189e+18
  }
};


const char *name()
{
  return C_("modulename", "white balance");
}


int
groups ()
{
  return IOP_GROUP_BASIC;
}

int
flags ()
{
  return IOP_FLAGS_ALLOW_TILING;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "tint"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "temperature in"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "temperature out"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "red"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "green"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "blue"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_temperature_gui_data_t *g =
    (dt_iop_temperature_gui_data_t*)self->gui_data;

  dt_accel_connect_slider_iop(self, "tint", GTK_WIDGET(g->scale_tint));
  dt_accel_connect_slider_iop(self, "temperature in", GTK_WIDGET(g->scale_k));
  dt_accel_connect_slider_iop(self, "temperature out", GTK_WIDGET(g->scale_k_out));
  dt_accel_connect_slider_iop(self, "red", GTK_WIDGET(g->scale_r));
  dt_accel_connect_slider_iop(self, "green", GTK_WIDGET(g->scale_g));
  dt_accel_connect_slider_iop(self, "blue", GTK_WIDGET(g->scale_b));
}


int
output_bpp(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  if(pipe->type != DT_DEV_PIXELPIPE_PREVIEW && (pipe->image.flags & DT_IMAGE_RAW)) return sizeof(float);
  else return 4*sizeof(float);
}


static void
convert_k_to_rgb (float temperature, float *rgb)
{
  int channel;

  if (temperature < DT_IOP_LOWEST_TEMPERATURE)  temperature = DT_IOP_LOWEST_TEMPERATURE;
  if (temperature > DT_IOP_HIGHEST_TEMPERATURE) temperature = DT_IOP_HIGHEST_TEMPERATURE;

  /* Evaluation of an approximation of the Planckian locus in linear RGB space
   * by rational functions of degree 5 using Horner's scheme
   * f(x) =  (p1*x^5 + p2*x^4 + p3*x^3 + p4*x^2 + p5*x + p6) /
   *            (x^5 + q1*x^4 + q2*x^3 + q3*x^2 + q4*x + q5)
   */
  for (channel = 0; channel < 3; channel++)
  {
    float nomin, denom;
    int   deg;

    nomin = dt_iop_temperature_rgb_r55[channel][0];
    for (deg = 1; deg < 6; deg++)
      nomin = nomin * temperature + dt_iop_temperature_rgb_r55[channel][deg];

    denom = dt_iop_temperature_rgb_r55[channel][6];
    for (deg = 1; deg < 6; deg++)
      denom = denom * temperature + dt_iop_temperature_rgb_r55[channel][6 + deg];

    rgb[channel] = nomin / denom;
  }
}

// binary search inversion inspired by ufraw's RGB_to_Temperature:
static void
convert_rgb_to_k(float rgb[3], const float temp_out, float *temp, float *tint)
{
  float tmin, tmax, tmp[3], original_temperature_rgb[3], intended_temperature_rgb[3];
  for(int k=0; k<3; k++) tmp[k] = rgb[k];
  tmin = DT_IOP_LOWEST_TEMPERATURE;
  tmax = DT_IOP_HIGHEST_TEMPERATURE;
  convert_k_to_rgb (temp_out,  intended_temperature_rgb);
  for (*temp=(tmax+tmin)/2; tmax-tmin>1; *temp=(tmax+tmin)/2)
  {
    convert_k_to_rgb (*temp, original_temperature_rgb);

    tmp[0] = intended_temperature_rgb[0] / original_temperature_rgb[0];
    tmp[1] = intended_temperature_rgb[1] / original_temperature_rgb[1];
    tmp[2] = intended_temperature_rgb[2] / original_temperature_rgb[2];

    if (tmp[2]/tmp[0] < rgb[2]/rgb[0])
      tmax = *temp;
    else
      tmin = *temp;
  }
  *tint =  (rgb[1]/rgb[0]) / (tmp[1]/tmp[0]);
}

static int
FC(const int row, const int col, const unsigned int filters)
{
  return filters >> (((row << 1 & 14) + (col & 1)) << 1) & 3;
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  const int filters = dt_image_flipped_filter(&piece->pipe->image);
  dt_iop_temperature_data_t *d = (dt_iop_temperature_data_t *)piece->data;
  if(piece->pipe->type != DT_DEV_PIXELPIPE_PREVIEW && filters && piece->pipe->image.bpp != 4)
  {
    const float coeffsi[3] = {d->coeffs[0]/65535.0f, d->coeffs[1]/65535.0f, d->coeffs[2]/65535.0f};
#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(roi_out, ivoid, ovoid, d) schedule(static)
#endif
    for(int j=0; j<roi_out->height; j++)
    {
      int i=0;
      const uint16_t *in = ((uint16_t *)ivoid) + j*roi_out->width;
      float *out = ((float*)ovoid) + j*roi_out->width;

      // process unaligned pixels
      for ( ; i < ((4-(j*roi_out->width & 3)) & 3) ; i++,out++,in++)
        *out = *in * coeffsi[FC(j+roi_out->y, i+roi_out->x, filters)];

      const __m128 coeffs = _mm_set_ps(coeffsi[FC(j+roi_out->y, roi_out->x+i+3, filters)],
                                       coeffsi[FC(j+roi_out->y, roi_out->x+i+2, filters)],
                                       coeffsi[FC(j+roi_out->y, roi_out->x+i+1, filters)],
                                       coeffsi[FC(j+roi_out->y, roi_out->x+i  , filters)]);

      // process aligned pixels with SSE
      for( ; i < roi_out->width - 3 ; i+=4,out+=4,in+=4)
      {
        _mm_stream_ps(out,_mm_mul_ps(coeffs,_mm_set_ps(in[3],in[2],in[1],in[0])));
      }

      // process the rest
      for( ; i<roi_out->width; i++,out++,in++)
        *out = *in * coeffsi[FC(j+roi_out->y, i+roi_out->x, filters)];
    }
    _mm_sfence();
  }
  else if(piece->pipe->type != DT_DEV_PIXELPIPE_PREVIEW && filters && piece->pipe->image.bpp == 4)
  {
#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(roi_out, ivoid, ovoid, d) schedule(static)
#endif
    for(int j=0; j<roi_out->height; j++)
    {
      const float *in = ((float *)ivoid) + j*roi_out->width;
      float *out = ((float*)ovoid) + j*roi_out->width;
      for(int i=0; i<roi_out->width; i++,out++,in++)
        *out = *in * d->coeffs[FC(j+roi_out->x, i+roi_out->y, filters)];
    }
  }
  else
  {
    const int ch = piece->colors;
#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(roi_out, ivoid, ovoid, d) schedule(static)
#endif
    for(int k=0; k<roi_out->height; k++)
    {
      const float *in = ((float*)ivoid) + ch*k*roi_out->width;
      float *out = ((float*)ovoid) + ch*k*roi_out->width;
      for (int j=0; j<roi_out->width; j++,in+=ch,out+=ch)
        for(int c=0; c<3; c++) out[c] = in[c]*d->coeffs[c];
    }
  }
  for(int k=0; k<3; k++)
    piece->pipe->processed_maximum[k] = d->coeffs[k] * piece->pipe->processed_maximum[k];
}

#ifdef HAVE_OPENCL
int
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_temperature_data_t *d = (dt_iop_temperature_data_t *)piece->data;
  dt_iop_temperature_global_data_t *gd = (dt_iop_temperature_global_data_t *)self->data;

  const int devid = piece->pipe->devid;
  const int filters = dt_image_flipped_filter(&piece->pipe->image);
  float coeffs[3] = {d->coeffs[0], d->coeffs[1], d->coeffs[2]};
  cl_mem dev_coeffs = NULL;
  cl_int err = -999;
  int kernel = -1;

  if(piece->pipe->type != DT_DEV_PIXELPIPE_PREVIEW && filters && piece->pipe->image.bpp != 4)
  {
    kernel = gd->kernel_whitebalance_1ui;
    for(int k=0; k<3; k++) coeffs[k] /= 65535.0f;
  }
  else if(piece->pipe->type != DT_DEV_PIXELPIPE_PREVIEW && filters && piece->pipe->image.bpp == 4)
  {
    kernel = gd->kernel_whitebalance_1f;
  }
  else
  {
    kernel = gd->kernel_whitebalance_4f;
  }

  dev_coeffs = dt_opencl_copy_host_to_device_constant(devid, sizeof(float)*3, coeffs);
  if (dev_coeffs == NULL) goto error;

  const int width = roi_in->width;
  const int height = roi_in->height;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1};
  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), (void *)&dev_coeffs);
  dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(uint32_t), (void *)&filters);
  dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(uint32_t), (void *)&roi_out->x);
  dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(uint32_t), (void *)&roi_out->y);
  err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_coeffs);
  for(int k=0; k<3; k++)
    piece->pipe->processed_maximum[k] = d->coeffs[k] * piece->pipe->processed_maximum[k];
  return TRUE;

error:
  if (dev_coeffs != NULL) dt_opencl_release_mem_object(dev_coeffs);
  dt_print(DT_DEBUG_OPENCL, "[opencl_white_balance] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void tiling_callback  (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out, struct dt_develop_tiling_t *tiling)
{
  tiling->factor = 2.0f; // in + out
  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 0;
  tiling->xalign = 2; // Bayer pattern
  tiling->yalign = 2; // Bayer pattern
  return;
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)p1;
  dt_iop_temperature_data_t *d = (dt_iop_temperature_data_t *)piece->data;
  for(int k=0; k<3; k++) d->coeffs[k]  = p->coeffs[k];
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_temperature_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update (struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  self->request_color_pick = 0;
  self->color_picker_box[0] = self->color_picker_box[1] = .25f;
  self->color_picker_box[2] = self->color_picker_box[3] = .75f;
  self->color_picker_point[0] = self->color_picker_point[1] = 0.5f;
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p  = (dt_iop_temperature_params_t *)module->params;
  dt_iop_temperature_params_t *fp = (dt_iop_temperature_params_t *)module->factory_params;
  float temp, tint, mul[3];
  for(int k=0; k<3; k++) mul[k] = p->coeffs[k]/fp->coeffs[k];
  convert_rgb_to_k(mul, p->temp_out, &temp, &tint);

  dt_bauhaus_slider_set(g->scale_k_out, p->temp_out);
  dt_bauhaus_slider_set(g->scale_r, p->coeffs[0]);
  dt_bauhaus_slider_set(g->scale_g, p->coeffs[1]);
  dt_bauhaus_slider_set(g->scale_b, p->coeffs[2]);
  dt_bauhaus_slider_set(g->scale_k, temp);
  dt_bauhaus_slider_set(g->scale_tint, tint);
  if(fabsf(p->coeffs[0]-fp->coeffs[0]) + fabsf(p->coeffs[1]-fp->coeffs[1]) + fabsf(p->coeffs[2]-fp->coeffs[2]) < 0.01)
    dt_bauhaus_combobox_set(g->presets, 0);
  else
    dt_bauhaus_combobox_set(g->presets, -1);
  dt_bauhaus_slider_set(g->finetune, 0);
}

void reload_defaults(dt_iop_module_t *module)
{
  // raw images need wb (to convert from uint16_t to float):
  if(dt_image_is_raw(&module->dev->image_storage))
  {
    module->default_enabled = 1;
    module->hide_enable_button = 1;
  }
  else module->default_enabled = 0;
  dt_iop_temperature_params_t tmp = (dt_iop_temperature_params_t)
  {
    5000.0, {1.0, 1.0, 1.0}
  };

  // get white balance coefficients, as shot
  char filename[1024];
  int ret=0;
  /* check if file is raw / hdr */
  if(dt_image_is_raw(&module->dev->image_storage))
  {
    dt_image_full_path(module->dev->image_storage.id, filename, 1024);
    libraw_data_t *raw = libraw_init(0);
  
    ret = libraw_open_file(raw, filename);
    if(!ret)
    {
      module->default_enabled = 1;
      for(int k=0; k<3; k++) tmp.coeffs[k] = raw->color.cam_mul[k];
      if(tmp.coeffs[0] <= 0.0)
      {
        for(int k=0; k<3; k++) tmp.coeffs[k] = raw->color.pre_mul[k];
      }
      if(tmp.coeffs[0] == 0 || tmp.coeffs[1] == 0 || tmp.coeffs[2] == 0)
      {
        // could not get useful info, try presets:
        char makermodel[1024];
        char *model = makermodel;
        dt_colorspaces_get_makermodel_split(makermodel, 1024, &model,
            module->dev->image_storage.exif_maker,
            module->dev->image_storage.exif_model);
        for(int i=0; i<wb_preset_count; i++)
        {
          if(!strcmp(wb_preset[i].make,  makermodel) &&
             !strcmp(wb_preset[i].model, model))
          {
            // just take the first preset we find for this camera
            for(int k=0; k<3; k++) tmp.coeffs[k] = wb_preset[i].channel[k];
            break;
          }
        }
        if(tmp.coeffs[0] == 0 || tmp.coeffs[1] == 0 || tmp.coeffs[2] == 0)
        {
          // final security net: hardcoded default that fits most cams.
          tmp.coeffs[0] = 2.0f;
          tmp.coeffs[1] = 1.0f;
          tmp.coeffs[2] = 1.5f;
        }
      }
      else
      {
        tmp.coeffs[0] /= tmp.coeffs[1];
        tmp.coeffs[2] /= tmp.coeffs[1];
        tmp.coeffs[1] = 1.0f;
      }
    }
    libraw_close(raw);
  }

  memcpy(module->params, &tmp, sizeof(dt_iop_temperature_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_temperature_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_temperature_global_data_t *gd = (dt_iop_temperature_global_data_t *)malloc(sizeof(dt_iop_temperature_global_data_t));
  module->data = gd;
  gd->kernel_whitebalance_1ui = dt_opencl_create_kernel(program, "whitebalance_1ui");
  gd->kernel_whitebalance_4f  = dt_opencl_create_kernel(program, "whitebalance_4f");
  gd->kernel_whitebalance_1f  = dt_opencl_create_kernel(program, "whitebalance_1f");
}

void init (dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_temperature_params_t));
  module->default_params = malloc(sizeof(dt_iop_temperature_params_t));
  module->priority = 39; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_temperature_params_t);
  module->gui_data = NULL;
}

void cleanup (dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_temperature_global_data_t *gd = (dt_iop_temperature_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_whitebalance_1ui);
  dt_opencl_free_kernel(gd->kernel_whitebalance_4f);
  dt_opencl_free_kernel(gd->kernel_whitebalance_1f);
  free(module->data);
  module->data = NULL;
}

static void
gui_update_from_coeffs (dt_iop_module_t *self)
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p  = (dt_iop_temperature_params_t *)self->params;
  dt_iop_temperature_params_t *fp = (dt_iop_temperature_params_t *)self->factory_params;
  // now get temp/tint from rgb. leave temp_out as it was:
  float temp, tint, mul[3];

  for(int k=0; k<3; k++) mul[k] = p->coeffs[k]/fp->coeffs[k];
  p->temp_out = dt_bauhaus_slider_get(g->scale_k_out);
  convert_rgb_to_k(mul, p->temp_out, &temp, &tint);

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->scale_k,    temp);
  dt_bauhaus_slider_set(g->scale_tint, tint);
  dt_bauhaus_slider_set(g->scale_r, p->coeffs[0]);
  dt_bauhaus_slider_set(g->scale_g, p->coeffs[1]);
  dt_bauhaus_slider_set(g->scale_b, p->coeffs[2]);
  darktable.gui->reset = 0;
}


static gboolean
expose (GtkWidget *widget, GdkEventExpose *event, dt_iop_module_t *self)
{
  // capture gui color picked event.
  if(darktable.gui->reset) return FALSE;
  if(self->picked_color_max[0] < self->picked_color_min[0]) return FALSE;
  if(!self->request_color_pick) return FALSE;
  static float old[3] = {0, 0, 0};
  const float *grayrgb = self->picked_color;
  if(grayrgb[0] == old[0] && grayrgb[1] == old[1] && grayrgb[2] == old[2]) return FALSE;
  for(int k=0; k<3; k++) old[k] = grayrgb[k];
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;
  for(int k=0; k<3; k++) p->coeffs[k] = (grayrgb[k] > 0.001f) ? 1.0f/grayrgb[k] : 1.0f;
  // normalize green:
  p->coeffs[0] /= p->coeffs[1];
  p->coeffs[2] /= p->coeffs[1];
  p->coeffs[1] = 1.0;
  for(int k=0; k<3; k++) p->coeffs[k] = fmaxf(0.0f, fminf(8.0f, p->coeffs[k]));
  gui_update_from_coeffs(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  return FALSE;
}

static void
temp_changed(dt_iop_module_t *self)
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p  = (dt_iop_temperature_params_t *)self->params;
  dt_iop_temperature_params_t *fp = (dt_iop_temperature_params_t *)self->factory_params;

  const float temp_out = dt_bauhaus_slider_get(g->scale_k_out);
  const float temp_in  = dt_bauhaus_slider_get(g->scale_k);
  const float tint     = dt_bauhaus_slider_get(g->scale_tint);

  float original_temperature_rgb[3], intended_temperature_rgb[3];
  convert_k_to_rgb (temp_in,  original_temperature_rgb);
  convert_k_to_rgb (temp_out, intended_temperature_rgb);

  p->temp_out = temp_out;
  p->coeffs[0] = fp->coeffs[0] *        intended_temperature_rgb[0] / original_temperature_rgb[0];
  p->coeffs[1] = fp->coeffs[1] * tint * intended_temperature_rgb[1] / original_temperature_rgb[1];
  p->coeffs[2] = fp->coeffs[2] *        intended_temperature_rgb[2] / original_temperature_rgb[2];
  // normalize:
  p->coeffs[0] /= p->coeffs[1];
  p->coeffs[2] /= p->coeffs[1];
  p->coeffs[1] = 1.0f;

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->scale_r, p->coeffs[0]);
  dt_bauhaus_slider_set(g->scale_g, p->coeffs[1]);
  dt_bauhaus_slider_set(g->scale_b, p->coeffs[2]);
  darktable.gui->reset = 0;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
tint_callback (GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  temp_changed(self);
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_bauhaus_combobox_set(g->presets, -1);
}

static void
temp_callback (GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  temp_changed(self);
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_bauhaus_combobox_set(g->presets, -1);
}

static void
temp_out_callback (GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  temp_changed(self);
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_bauhaus_combobox_set(g->presets, -1);
}

static void
rgb_callback (GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;
  const float value = dt_bauhaus_slider_get( slider );
  if     (slider == g->scale_r) p->coeffs[0] = value;
  else if(slider == g->scale_g) p->coeffs[1] = value;
  else if(slider == g->scale_b) p->coeffs[2] = value;

  gui_update_from_coeffs(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  dt_bauhaus_combobox_set(g->presets, -1);
}

static void
apply_preset(dt_iop_module_t *self)
{
  self->request_color_pick = 0;
  if(self->dt->gui->reset) return;
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p  = (dt_iop_temperature_params_t *)self->params;
  dt_iop_temperature_params_t *fp = (dt_iop_temperature_params_t *)self->factory_params;
  const int tune = dt_bauhaus_slider_get(g->finetune);
  const int pos = dt_bauhaus_combobox_get(g->presets);
  switch(pos)
  {
    case -1: // just un-setting.
      return;
    case 0: // camera wb
      for(int k=0; k<3; k++) p->coeffs[k] = fp->coeffs[k];
      break;
    case 1: // spot wb, exposure callback will set p->coeffs.
      for(int k=0; k<3; k++) p->coeffs[k] = fp->coeffs[k];
      dt_iop_request_focus(self);
      self->request_color_pick = 1;

      /* set the area sample size*/
      if (self->request_color_pick)
        dt_lib_colorpicker_set_area(darktable.lib, 0.99);

      break;
    case 2: // passthrough mode, raw data
      for(int k=0; k<3; k++) p->coeffs[k] = 1.0;
      break;
    default:
      for(int i=g->preset_num[pos]; i<wb_preset_count; i++)
      {
        char makermodel[1024];
        char *model = makermodel;
        dt_colorspaces_get_makermodel_split(makermodel, 1024, &model, self->dev->image_storage.exif_maker, self->dev->image_storage.exif_model);
        if(!strcmp(wb_preset[i].make,  makermodel) &&
            !strcmp(wb_preset[i].model, model) && wb_preset[i].tuning == tune)
        {
          for(int k=0; k<3; k++) p->coeffs[k] = wb_preset[i].channel[k];
          break;
        }
      }
      break;
  }
  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
  gui_update_from_coeffs(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
presets_changed (GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  apply_preset(self);
  const int pos = dt_bauhaus_combobox_get(widget);
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  gtk_widget_set_sensitive(g->finetune, pos > 2);
}

static void
finetune_changed (GtkWidget *widget, gpointer user_data)
{
  apply_preset((dt_iop_module_t *)user_data);
}

void gui_init (struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_temperature_gui_data_t));
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t*)self->default_params;

  self->request_color_pick = 0;
  self->widget = gtk_vbox_new(TRUE, DT_BAUHAUS_SPACE);
  g_signal_connect(G_OBJECT(self->widget), "expose-event", G_CALLBACK(expose), self);

  g->scale_tint  = dt_bauhaus_slider_new_with_range(self,0.1, 8.0, .01,1.0,3);
  g->scale_k     = dt_bauhaus_slider_new_with_range(self,DT_IOP_LOWEST_TEMPERATURE, DT_IOP_HIGHEST_TEMPERATURE, 10.,5000.0,0);
  g->scale_k_out = dt_bauhaus_slider_new_with_range(self,DT_IOP_LOWEST_TEMPERATURE, DT_IOP_HIGHEST_TEMPERATURE, 10.,5000.0,0);
  g->scale_r     = dt_bauhaus_slider_new_with_range(self,0.0, 8.0, .001,p->coeffs[0],3);
  g->scale_g     = dt_bauhaus_slider_new_with_range(self,0.0, 8.0, .001,p->coeffs[1],3);
  g->scale_b     = dt_bauhaus_slider_new_with_range(self,0.0, 8.0, .001,p->coeffs[2],3);
  dt_bauhaus_slider_set_format(g->scale_k,"%.0fK");
  dt_bauhaus_slider_set_format(g->scale_k_out,"%.0fK");
  dt_bauhaus_widget_set_label(g->scale_tint,_("tint"));
  dt_bauhaus_widget_set_label(g->scale_k,_("temperature in"));
  dt_bauhaus_widget_set_label(g->scale_k_out,_("temperature out"));
  dt_bauhaus_widget_set_label(g->scale_r,_("red"));
  dt_bauhaus_widget_set_label(g->scale_g,_("green"));
  dt_bauhaus_widget_set_label(g->scale_b,_("blue"));

  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_tint, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_k, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_k_out, TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_r, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_g, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_b, TRUE, TRUE, 0);

  g->presets = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->presets,_("preset"));
  dt_bauhaus_combobox_add(g->presets, _("camera white balance"));
  dt_bauhaus_combobox_add(g->presets, _("spot white balance"));
  dt_bauhaus_combobox_add(g->presets, _("passthrough"));
  g->preset_cnt = 3;
  const char *wb_name = NULL;
  char makermodel[1024];
  char *model = makermodel;
  dt_colorspaces_get_makermodel_split(makermodel, 1024, &model, self->dev->image_storage.exif_maker, self->dev->image_storage.exif_model);
  if(!dt_image_is_ldr(&self->dev->image_storage)) for(int i=0; i<wb_preset_count; i++)
    {
      if(g->preset_cnt >= 50) break;
      if(!strcmp(wb_preset[i].make,  makermodel) &&
          !strcmp(wb_preset[i].model, model))
      {
        if(!wb_name || strcmp(wb_name, wb_preset[i].name))
        {
          wb_name = wb_preset[i].name;
          dt_bauhaus_combobox_add(g->presets, _(wb_preset[i].name));
          g->preset_num[g->preset_cnt++] = i;
        }
      }
    }
  gtk_box_pack_start(GTK_BOX(self->widget), g->presets, TRUE, TRUE, 0);
  g_object_set(G_OBJECT(g->presets), "tooltip-text", _("choose white balance preset from camera"), (char *)NULL);

  g->finetune = dt_bauhaus_slider_new_with_range(self,-9.0, 9.0, 1.0, 0.0, 0);
  dt_bauhaus_widget_set_label(g->finetune, _("finetune"));
  dt_bauhaus_slider_set_format(g->finetune, _("%.0f mired"));
  // initially doesn't have fine tuning stuff (camera wb)
  gtk_widget_set_sensitive(g->finetune, FALSE);
  gtk_box_pack_start(GTK_BOX(self->widget), g->finetune, TRUE, TRUE, 0);
  g_object_set(G_OBJECT(g->finetune), "tooltip-text", _("fine tune white balance preset"), (char *)NULL);

  self->gui_update(self);

  g_signal_connect (G_OBJECT (g->scale_tint), "value-changed",
                    G_CALLBACK (tint_callback), self);
  g_signal_connect (G_OBJECT (g->scale_k), "value-changed",
                    G_CALLBACK (temp_callback), self);
  g_signal_connect (G_OBJECT (g->scale_k_out), "value-changed",
                    G_CALLBACK (temp_out_callback), self);
  g_signal_connect (G_OBJECT (g->scale_r), "value-changed",
                    G_CALLBACK (rgb_callback), self);
  g_signal_connect (G_OBJECT (g->scale_g), "value-changed",
                    G_CALLBACK (rgb_callback), self);
  g_signal_connect (G_OBJECT (g->scale_b), "value-changed",
                    G_CALLBACK (rgb_callback), self);
  g_signal_connect (G_OBJECT (g->presets), "value-changed",
                    G_CALLBACK (presets_changed), self);
  g_signal_connect (G_OBJECT (g->finetune), "value-changed",
                    G_CALLBACK (finetune_changed), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  self->request_color_pick = 0;
  free(self->gui_data);
  self->gui_data = NULL;
}

// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
