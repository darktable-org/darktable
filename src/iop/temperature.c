/*
    This file is part of darktable,
    copyright (c) 2009--2013 johannes hanika.
    copyright (c) 2015 LebedevRI.

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
#include <lcms2.h>

#include "common/darktable.h"
#include "develop/develop.h"
#include "develop/tiling.h"
#include "control/control.h"
#include "common/colorspaces.h"
#include "common/opencl.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "external/wb_presets.c"
#include "bauhaus/bauhaus.h"

// for Kelvin temperature and bogus WB
#include "external/adobe_coeff.c"
#include "external/cie_colorimetric_tables.c"
#include "common/colorspaces.h"

DT_MODULE_INTROSPECTION(2, dt_iop_temperature_params_t)

#define INITIALBLACKBODYTEMPERATURE 4000

#define DT_IOP_LOWEST_TEMPERATURE 1901
#define DT_IOP_HIGHEST_TEMPERATURE 25000

#define DT_IOP_LOWEST_TINT 0.135
#define DT_IOP_HIGHEST_TINT 2.326

#define DT_IOP_NUM_OF_STD_TEMP_PRESETS 3

typedef struct dt_iop_temperature_params_t
{
  float temp_out;
  float coeffs[3];
} dt_iop_temperature_params_t;

typedef struct dt_iop_temperature_gui_data_t
{
  GtkWidget *scale_k, *scale_tint, *scale_r, *scale_g, *scale_b;
  GtkWidget *presets;
  GtkWidget *finetune;
  int preset_cnt;
  int preset_num[50];
  double daylight_wb[3];
  double XYZ_to_CAM[3][3], CAM_to_XYZ[3][3];
} dt_iop_temperature_gui_data_t;

typedef struct dt_iop_temperature_data_t
{
  float coeffs[3];
} dt_iop_temperature_data_t;


typedef struct dt_iop_temperature_global_data_t
{
  int kernel_whitebalance_4f;
  int kernel_whitebalance_1f;
} dt_iop_temperature_global_data_t;

const char *name()
{
  return C_("modulename", "white balance");
}


int groups()
{
  return IOP_GROUP_BASIC;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "tint"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "temperature"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "red"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "green"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "blue"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;

  dt_accel_connect_slider_iop(self, "tint", GTK_WIDGET(g->scale_tint));
  dt_accel_connect_slider_iop(self, "temperature", GTK_WIDGET(g->scale_k));
  dt_accel_connect_slider_iop(self, "red", GTK_WIDGET(g->scale_r));
  dt_accel_connect_slider_iop(self, "green", GTK_WIDGET(g->scale_g));
  dt_accel_connect_slider_iop(self, "blue", GTK_WIDGET(g->scale_b));
}


int output_bpp(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  if(!dt_dev_pixelpipe_uses_downsampled_input(pipe) && (pipe->image.flags & DT_IMAGE_RAW))
    return sizeof(float);
  else
    return 4 * sizeof(float);
}

/*
 * Spectral power distribution functions
 * https://en.wikipedia.org/wiki/Spectral_power_distribution
 */
typedef double((*spd)(unsigned long int wavelength, double TempK));

/*
 * Bruce Lindbloom, "Spectral Power Distribution of a Blackbody Radiator"
 * http://www.brucelindbloom.com/Eqn_Blackbody.html
 */
static double spd_blackbody(unsigned long int wavelength, double TempK)
{
  // convert wavelength from nm to m
  const long double lambda = (double)wavelength * 1e-9;

/*
 * these 2 constants were computed using following Sage code:
 *
 * (from http://physics.nist.gov/cgi-bin/cuu/Value?h)
 * h = 6.62606957 * 10^-34 # Planck
 * c= 299792458 # speed of light in vacuum
 * k = 1.3806488 * 10^-23 # Boltzmann
 *
 * c_1 = 2 * pi * h * c^2
 * c_2 = h * c / k
 *
 * print 'c_1 = ', c_1, ' ~= ', RealField(128)(c_1)
 * print 'c_2 = ', c_2, ' ~= ', RealField(128)(c_2)
 */

#define c1 3.7417715246641281639549488324352159753e-16L
#define c2 0.014387769599838156481252937624049081933L

  return (double)(c1 / (powl(lambda, 5) * (expl(c2 / (lambda * TempK)) - 1.0L)));

#undef c2
#undef c1
}

/*
 * Bruce Lindbloom, "Spectral Power Distribution of a CIE D-Illuminant"
 * http://www.brucelindbloom.com/Eqn_DIlluminant.html
 * and https://en.wikipedia.org/wiki/Standard_illuminant#Illuminant_series_D
 */
static double spd_daylight(unsigned long int wavelength, double TempK)
{
  cmsCIExyY WhitePoint = { 0.3127, 0.3290, 1.0 };

  /*
   * Bruce Lindbloom, "TempK to xy"
   * http://www.brucelindbloom.com/Eqn_T_to_xy.html
   */
  cmsWhitePointFromTemp(&WhitePoint, TempK);

  const double M = (0.0241 + 0.2562 * WhitePoint.x - 0.7341 * WhitePoint.y),
               m1 = (-1.3515 - 1.7703 * WhitePoint.x + 5.9114 * WhitePoint.y) / M,
               m2 = (0.0300 - 31.4424 * WhitePoint.x + 30.0717 * WhitePoint.y) / M;

  const unsigned long int j
      = ((wavelength - cie_daylight_components[0].wavelength)
         / (cie_daylight_components[1].wavelength - cie_daylight_components[0].wavelength));

  return (cie_daylight_components[j].S[0] + m1 * cie_daylight_components[j].S[1]
          + m2 * cie_daylight_components[j].S[2]);
}

/*
 * Bruce Lindbloom, "Computing XYZ From Spectral Data (Emissive Case)"
 * http://www.brucelindbloom.com/Eqn_Spect_to_XYZ.html
 */
static cmsCIEXYZ spectrum_to_XYZ(double TempK, spd I)
{
  cmsCIEXYZ Source = {.X = 0.0, .Y = 0.0, .Z = 0.0 };

  /*
   * Color matching functions
   * https://en.wikipedia.org/wiki/CIE_1931_color_space#Color_matching_functions
   */
  for(size_t i = 0; i < cie_1931_std_colorimetric_observer_count; i++)
  {
    const unsigned long int lambda = cie_1931_std_colorimetric_observer[0].wavelength
                                     + (cie_1931_std_colorimetric_observer[1].wavelength
                                        - cie_1931_std_colorimetric_observer[0].wavelength) * i;
    const double P = I(lambda, TempK);
    Source.X += P * cie_1931_std_colorimetric_observer[i].xyz.X;
    Source.Y += P * cie_1931_std_colorimetric_observer[i].xyz.Y;
    Source.Z += P * cie_1931_std_colorimetric_observer[i].xyz.Z;
  }

  // normalize so that each component is in [0.0, 1.0] range
  const double _max = MAX(MAX(Source.X, Source.Y), Source.Z);
  Source.X /= _max;
  Source.Y /= _max;
  Source.Z /= _max;

  return Source;
}

//
static cmsCIEXYZ temperature_to_XYZ(double TempK)
{
  if(TempK < DT_IOP_LOWEST_TEMPERATURE) TempK = DT_IOP_LOWEST_TEMPERATURE;
  if(TempK > DT_IOP_HIGHEST_TEMPERATURE) TempK = DT_IOP_HIGHEST_TEMPERATURE;

  if(TempK < INITIALBLACKBODYTEMPERATURE)
  {
    // if temperature is less than 4000K we use blackbody,
    // because there will be no Daylight reference below 4000K...
    return spectrum_to_XYZ(TempK, spd_blackbody);
  }
  else
  {
    return spectrum_to_XYZ(TempK, spd_daylight);
  }
}

// binary search inversion
static void XYZ_to_temperature(cmsCIEXYZ XYZ, double *TempK, double *tint)
{
  double maxtemp = DT_IOP_HIGHEST_TEMPERATURE, mintemp = DT_IOP_LOWEST_TEMPERATURE;
  cmsCIEXYZ _xyz;

  for(*TempK = (maxtemp + mintemp) / 2.0; (maxtemp - mintemp) > 1.0; *TempK = (maxtemp + mintemp) / 2.0)
  {
    _xyz = temperature_to_XYZ(*TempK);
    if(_xyz.Z / _xyz.X > XYZ.Z / XYZ.X)
      maxtemp = *TempK;
    else
      mintemp = *TempK;
  }

  *tint = (_xyz.Y / _xyz.X) / (XYZ.Y / XYZ.X);

  if(*TempK < DT_IOP_LOWEST_TEMPERATURE) *TempK = DT_IOP_LOWEST_TEMPERATURE;
  if(*TempK > DT_IOP_HIGHEST_TEMPERATURE) *TempK = DT_IOP_HIGHEST_TEMPERATURE;
  if(*tint < DT_IOP_LOWEST_TINT) *tint = DT_IOP_LOWEST_TINT;
  if(*tint > DT_IOP_HIGHEST_TINT) *tint = DT_IOP_HIGHEST_TINT;
}

static void temp2mul(dt_iop_module_t *self, double TempK, double tint, double mul[3])
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;

  cmsCIEXYZ _xyz = temperature_to_XYZ(TempK);

  double XYZ[3] = { _xyz.X, _xyz.Y / tint, _xyz.Z };

  double CAM[3];
  for(int k = 0; k < 3; k++)
  {
    CAM[k] = 0.0;
    for(int i = 0; i < 3; i++)
    {
      CAM[k] += g->XYZ_to_CAM[k][i] * XYZ[i];
    }
  }

  for(int k = 0; k < 3; k++) mul[k] = 1.0 / CAM[k];
}

static void mul2temp(dt_iop_module_t *self, float coeffs[3], double *TempK, double *tint)
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;

  double CAM[3];
  for(int k = 0; k < 3; k++) CAM[k] = 1.0 / coeffs[k];

  double XYZ[3];
  for(int k = 0; k < 3; k++)
  {
    XYZ[k] = 0.0;
    for(int i = 0; i < 3; i++)
    {
      XYZ[k] += g->CAM_to_XYZ[k][i] * CAM[i];
    }
  }

  XYZ_to_temperature((cmsCIEXYZ){ XYZ[0], XYZ[1], XYZ[2] }, TempK, tint);
}

/*
 * interpolate values from p1 and p2 into out.
 */
void dt_wb_preset_interpolate(const wb_data *const p1, // the smaller tuning
                              const wb_data *const p2, // the larger tuning (can't be == p1)
                              wb_data *out)            // has tuning initialized
{
  // stupid linear interpolation.
  // to be confirmed.
  const double t = CLAMP((double)(out->tuning - p1->tuning) / (double)(p2->tuning - p1->tuning), 0.0, 1.0);
  for(int k = 0; k < 3; k++)
  {
    out->channel[k] = (1.0 - t) * p1->channel[k] + t * p2->channel[k];
  }
}

static int FC(const int row, const int col, const unsigned int filters)
{
  return filters >> (((row << 1 & 14) + (col & 1)) << 1) & 3;
}

static uint8_t FCxtrans(const int row, const int col, const dt_iop_roi_t *const roi,
                        uint8_t (*const xtrans)[6])
{
  return xtrans[(row + roi->y) % 6][(col + roi->x) % 6];
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid,
             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  const int filters = dt_image_filter(&piece->pipe->image);
  uint8_t (*const xtrans)[6] = self->dev->image_storage.xtrans;
  dt_iop_temperature_data_t *d = (dt_iop_temperature_data_t *)piece->data;
  if(!dt_dev_pixelpipe_uses_downsampled_input(piece->pipe) && filters == 9u)
  { // xtrans float mosaiced
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(roi_out, ivoid, ovoid, d) schedule(static)
#endif
    for(int j = 0; j < roi_out->height; j++)
    {
      const float *in = ((float *)ivoid) + (size_t)j * roi_out->width;
      float *out = ((float *)ovoid) + (size_t)j * roi_out->width;
      for(int i = 0; i < roi_out->width; i++, out++, in++)
        *out = *in * d->coeffs[FCxtrans(j, i, roi_out, xtrans)];
    }
  }
  else if(!dt_dev_pixelpipe_uses_downsampled_input(piece->pipe) && filters)
  { // bayer float mosaiced
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(roi_out, ivoid, ovoid, d) schedule(static)
#endif
    for(int j = 0; j < roi_out->height; j++)
    {
      const float *in = ((float *)ivoid) + (size_t)j * roi_out->width;
      float *out = ((float *)ovoid) + (size_t)j * roi_out->width;

      int i = 0;
      int alignment = ((4 - (j * roi_out->width & (4 - 1))) & (4 - 1));

      // process unaligned pixels
      for(; i < alignment; i++, out++, in++)
        *out = *in * d->coeffs[FC(j + roi_out->y, i + roi_out->x, filters)];

      const __m128 coeffs = _mm_set_ps(d->coeffs[FC(j + roi_out->y, roi_out->x + i + 3, filters)],
                                       d->coeffs[FC(j + roi_out->y, roi_out->x + i + 2, filters)],
                                       d->coeffs[FC(j + roi_out->y, roi_out->x + i + 1, filters)],
                                       d->coeffs[FC(j + roi_out->y, roi_out->x + i, filters)]);

      // process aligned pixels with SSE
      for(; i < roi_out->width - (4 - 1); i += 4, in += 4, out += 4)
      {
        const __m128 input = _mm_load_ps(in);

        const __m128 multiplied = _mm_mul_ps(input, coeffs);

        _mm_stream_ps(out, multiplied);
      }

      // process the rest
      for(; i < roi_out->width; i++, out++, in++)
        *out = *in * d->coeffs[FC(j + roi_out->y, i + roi_out->x, filters)];
    }
    _mm_sfence();
  }
  else
  { // non-mosaiced
    const int ch = piece->colors;

    const __m128 coeffs = _mm_set_ps(1.0f, d->coeffs[2], d->coeffs[1], d->coeffs[0]);

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(roi_out, ivoid, ovoid, d) schedule(static)
#endif
    for(int k = 0; k < roi_out->height; k++)
    {
      const float *in = ((float *)ivoid) + (size_t)ch * k * roi_out->width;
      float *out = ((float *)ovoid) + (size_t)ch * k * roi_out->width;
      for(int j = 0; j < roi_out->width; j++, in += ch, out += ch)
      {
        const __m128 input = _mm_load_ps(in);
        const __m128 multiplied = _mm_mul_ps(input, coeffs);
        _mm_stream_ps(out, multiplied);
      }
    }
    _mm_sfence();

    if(piece->pipe->mask_display) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
  }
  for(int k = 0; k < 3; k++)
    piece->pipe->processed_maximum[k] = d->coeffs[k] * piece->pipe->processed_maximum[k];
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_temperature_data_t *d = (dt_iop_temperature_data_t *)piece->data;
  dt_iop_temperature_global_data_t *gd = (dt_iop_temperature_global_data_t *)self->data;

  const int devid = piece->pipe->devid;
  const int filters = dt_image_filter(&piece->pipe->image);
  cl_mem dev_coeffs = NULL;
  cl_int err = -999;
  int kernel = -1;

  if(!dt_dev_pixelpipe_uses_downsampled_input(piece->pipe) && filters)
  {
    kernel = gd->kernel_whitebalance_1f;
  }
  else
  {
    kernel = gd->kernel_whitebalance_4f;
  }

  dev_coeffs = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3, d->coeffs);
  if(dev_coeffs == NULL) goto error;

  const int width = roi_in->width;
  const int height = roi_in->height;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };
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
  for(int k = 0; k < 3; k++)
    piece->pipe->processed_maximum[k] = d->coeffs[k] * piece->pipe->processed_maximum[k];
  return TRUE;

error:
  if(dev_coeffs != NULL) dt_opencl_release_mem_object(dev_coeffs);
  dt_print(DT_DEBUG_OPENCL, "[opencl_white_balance] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  tiling->factor = 2.0f; // in + out
  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 0;
  tiling->xalign = 2; // Bayer pattern
  tiling->yalign = 2; // Bayer pattern
  return;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)p1;
  dt_iop_temperature_data_t *d = (dt_iop_temperature_data_t *)piece->data;
  for(int k = 0; k < 3; k++) d->coeffs[k] = p->coeffs[k];

  // x-trans images not implemented in OpenCL yet
  if(pipe->image.filters == 9u) piece->process_cl_ready = 0;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_temperature_data_t));
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
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  self->color_picker_box[0] = self->color_picker_box[1] = .25f;
  self->color_picker_box[2] = self->color_picker_box[3] = .75f;
  self->color_picker_point[0] = self->color_picker_point[1] = 0.5f;
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)module->params;
  dt_iop_temperature_params_t *fp = (dt_iop_temperature_params_t *)module->default_params;

  double TempK, tint;
  mul2temp(self, p->coeffs, &TempK, &tint);

  dt_bauhaus_slider_set(g->scale_r, p->coeffs[0]);
  dt_bauhaus_slider_set(g->scale_g, p->coeffs[1]);
  dt_bauhaus_slider_set(g->scale_b, p->coeffs[2]);
  dt_bauhaus_slider_set(g->scale_k, TempK);
  dt_bauhaus_slider_set(g->scale_tint, tint);

  dt_bauhaus_combobox_clear(g->presets);
  dt_bauhaus_combobox_add(g->presets, _("camera white balance"));
  dt_bauhaus_combobox_add(g->presets, _("camera neutral white balance"));
  dt_bauhaus_combobox_add(g->presets, _("spot white balance"));
  g->preset_cnt = DT_IOP_NUM_OF_STD_TEMP_PRESETS;
  memset(g->preset_num, 0, sizeof(g->preset_num));

  dt_bauhaus_combobox_set(g->presets, -1);
  dt_bauhaus_slider_set(g->finetune, 0);
  gtk_widget_set_sensitive(g->finetune, 0);

  const char *wb_name = NULL;
  if(!dt_image_is_ldr(&self->dev->image_storage))
    for(int i = 0; i < wb_preset_count; i++)
    {
      if(g->preset_cnt >= 50) break;
      if(!strcmp(wb_preset[i].make, self->dev->image_storage.camera_maker) 
         && !strcmp(wb_preset[i].model, self->dev->image_storage.camera_model))
      {
        if(!wb_name || strcmp(wb_name, wb_preset[i].name))
        {
          wb_name = wb_preset[i].name;
          dt_bauhaus_combobox_add(g->presets, _(wb_preset[i].name));
          g->preset_num[g->preset_cnt] = i;
          g->preset_cnt++;
        }
      }
    }

  gboolean found = FALSE;
  // is this a camera white balance?
  if(memcmp(p->coeffs, fp->coeffs, 3 * sizeof(float)) == 0)
  {
    dt_bauhaus_combobox_set(g->presets, 0);
    found = TRUE;
  }
  else
  {
    // is this a "camera neutral white balance"?
    if((p->coeffs[0] == (float)g->daylight_wb[0]) && (p->coeffs[1] == (float)g->daylight_wb[1])
       && (p->coeffs[2] == (float)g->daylight_wb[2]))
    {
      dt_bauhaus_combobox_set(g->presets, 1);
      found = TRUE;
    }
  }

  if(!found)
  {
    // look through all added presets
    for(int j = DT_IOP_NUM_OF_STD_TEMP_PRESETS; !found && (j < g->preset_cnt); j++)
    {
      // look through all variants of this preset, with different tuning
      for(int i = g->preset_num[j]; !found && (i < wb_preset_count) 
                                    && !strcmp(wb_preset[i].make, self->dev->image_storage.camera_maker)
                                    && !strcmp(wb_preset[i].model, self->dev->image_storage.camera_model)
                                    && !strcmp(wb_preset[i].name, wb_preset[g->preset_num[j]].name);
          i++)
      {
        float coeffs[3];
        for(int k = 0; k < 3; k++) coeffs[k] = wb_preset[i].channel[k];

        if(memcmp(coeffs, p->coeffs, 3 * sizeof(float)) == 0)
        {
          // got exact match!
          dt_bauhaus_combobox_set(g->presets, j);
          gtk_widget_set_sensitive(g->finetune, 1);
          dt_bauhaus_slider_set(g->finetune, wb_preset[i].tuning);
          found = TRUE;
          break;
        }
      }
    }

    if(!found)
    {
      // ok, we haven't found exact match, maybe this was interpolated?

      // look through all added presets
      for(int j = DT_IOP_NUM_OF_STD_TEMP_PRESETS; !found && (j < g->preset_cnt); j++)
      {
        // look through all variants of this preset, with different tuning
        int i = g->preset_num[j] + 1;
        while(!found && (i < wb_preset_count) && !strcmp(wb_preset[i].make, self->dev->image_storage.camera_maker)
              && !strcmp(wb_preset[i].model, self->dev->image_storage.camera_maker)
              && !strcmp(wb_preset[i].name, wb_preset[g->preset_num[j]].name))
        {
          // let's find gaps
          if(wb_preset[i - 1].tuning + 1 == wb_preset[i].tuning)
          {
            i++;
            continue;
          }

          // we have a gap!

          // we do not know what finetuning value was set, we need to bruteforce to find it
          for(int tune = wb_preset[i - 1].tuning + 1; !found && (tune < wb_preset[i].tuning); tune++)
          {
            wb_data interpolated = {.tuning = tune };
            dt_wb_preset_interpolate(&wb_preset[i - 1], &wb_preset[i], &interpolated);

            float coeffs[3];
            for(int k = 0; k < 3; k++) coeffs[k] = interpolated.channel[k];

            if(memcmp(coeffs, p->coeffs, 3 * sizeof(float)) == 0)
            {
              // got exact match!

              dt_bauhaus_combobox_set(g->presets, j);
              gtk_widget_set_sensitive(g->finetune, 1);
              dt_bauhaus_slider_set(g->finetune, tune);
              found = TRUE;
              break;
            }
          }
          i++;
        }
      }
    }
  }
}

static int calculate_bogus_daylight_wb(dt_iop_module_t *module, double bwb[3])
{
  if(!dt_image_is_raw(&module->dev->image_storage))
  {
    bwb[0] = 1.0;
    bwb[2] = 1.0;
    bwb[1] = 1.0;

    return 0;
  }

  // color matrix
  float XYZ_to_CAM[4][3];
  XYZ_to_CAM[0][0] = NAN;
  dt_dcraw_adobe_coeff(module->dev->image_storage.camera_makermodel, (float(*)[12])XYZ_to_CAM);
  if(!isnan(XYZ_to_CAM[0][0]))
  {
    // sRGB D65
    const double RGB_to_XYZ[3][3] = { { 0.4124564, 0.3575761, 0.1804375 },
                                      { 0.2126729, 0.7151522, 0.0721750 },
                                      { 0.0193339, 0.1191920, 0.9503041 } };

    double RGB_to_CAM[3][3];
    for(int i = 0; i < 3; i++)
    {
      for(int j = 0; j < 3; j++)
      {
        RGB_to_CAM[i][j] = 0.0;
        for(int k = 0; k < 3; k++) RGB_to_CAM[i][j] += XYZ_to_CAM[i][k] * RGB_to_XYZ[k][j];
      }
    }

    const double RGB[3] = { 1.0, 1.0, 1.0 };

    double CAM[3];
    for(int c = 0; c < 3; c++)
    {
      CAM[c] = 0.0;
      for(int i = 0; i < 3; i++)
      {
        CAM[c] += RGB_to_CAM[c][i] * RGB[i];
      }
    }

    double mul[3];
    for(int k = 0; k < 3; k++) mul[k] = 1.0 / CAM[k];

    // normalize green:
    bwb[0] = mul[0] / mul[1];
    bwb[2] = mul[2] / mul[1];
    bwb[1] = 1.0;

    return 0;
  }

  return 1;
}

static int prepare_wb_matrices(dt_iop_module_t *module)
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)module->gui_data;

  // sRGB D65
  const double RGB_to_XYZ[3][3] = { { 0.4124564, 0.3575761, 0.1804375 },
                                    { 0.2126729, 0.7151522, 0.0721750 },
                                    { 0.0193339, 0.1191920, 0.9503041 } };

  // sRGB D65
  const double XYZ_to_RGB[3][3] = { { 3.2404542, -1.5371385, -0.4985314 },
                                    { -0.9692660, 1.8760108, 0.0415560 },
                                    { 0.0556434, -0.2040259, 1.0572252 } };

  if(!dt_image_is_raw(&module->dev->image_storage))
  {
    // let's just assume for now(TM) that if it is not raw, it is sRGB
    memcpy(g->XYZ_to_CAM, XYZ_to_RGB, sizeof(g->XYZ_to_CAM));
    memcpy(g->CAM_to_XYZ, RGB_to_XYZ, sizeof(g->CAM_to_XYZ));

    return 0;
  }

  // prepare matrices for Kelvin temperature
  float XYZ_to_CAM[4][3];
  XYZ_to_CAM[0][0] = NAN;
  dt_dcraw_adobe_coeff(module->dev->image_storage.camera_makermodel, (float(*)[12])XYZ_to_CAM);
  if(!isnan(XYZ_to_CAM[0][0]))
  {
    for(int i = 0; i < 3; i++)
    {
      for(int j = 0; j < 3; j++)
      {
        g->XYZ_to_CAM[i][j] = (double)XYZ_to_CAM[i][j];
      }
    }

    // and inverse matrix
    mat3inv_double((double *)g->CAM_to_XYZ, (double *)g->XYZ_to_CAM);

    return 0;
  }

  return 1;
}

void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_temperature_params_t tmp
      = (dt_iop_temperature_params_t){.temp_out = 5000.0, .coeffs = { 1.0, 1.0, 1.0 } };

  // we might be called from presets update infrastructure => there is no image
  if(!module->dev) goto end;

  if(module->gui_data) prepare_wb_matrices(module);

  /* check if file is raw / hdr */
  if(dt_image_is_raw(&module->dev->image_storage))
  {
    // raw images need wb:
    module->default_enabled = 1;

    int found = 1, is_monochrom = 0;

    for(int k = 0; k < 3; k++)
    {
      if(isnan(module->dev->image_storage.wb_coeffs[k]) || module->dev->image_storage.wb_coeffs[k] == 0.0f)
      {
        found = 0;
        break;
      }
    }

    if(found)
    {
      for(int k = 0; k < 3; k++) tmp.coeffs[k] = module->dev->image_storage.wb_coeffs[k];
    }
    else
    {
      if(!(!strncmp(module->dev->image_storage.exif_maker, "Leica Camera AG", 15)
           && !strncmp(module->dev->image_storage.exif_model, "M9 monochrom", 12)))
      {
        dt_control_log(_("failed to read camera white balance information!"));
        fprintf(stderr, "[temperature] failed to read camera white balance information from `%s'!\n",
                module->dev->image_storage.filename);

        // could not get useful info, try presets:
        for(int i = 0; i < wb_preset_count; i++)
        {
          if(!strcmp(wb_preset[i].make, module->dev->image_storage.camera_maker) 
             && !strcmp(wb_preset[i].model, module->dev->image_storage.camera_model))
          {
            // just take the first preset we find for this camera
            for(int k = 0; k < 3; k++) tmp.coeffs[k] = wb_preset[i].channel[k];
            found = 1;
            break;
          }
        }
      }
      else
      {
        // nop white balance is valid for monochrome sraws (like the leica monochrom produces)
        is_monochrom = 1;
      }
    }

    // did not find preset either?
    {
      double bwb[3];
      if(!is_monochrom && !found && !calculate_bogus_daylight_wb(module, bwb))
      {
        // found camera matrix and used it to calculate bogus daylight wb
        for(int c = 0; c < 3; c++) tmp.coeffs[c] = bwb[c];
        found = 1;
      }
    }

    // and no cam matrix too???
    if(!found && !is_monochrom)
    {
      // final security net: hardcoded default that fits most cams.
      tmp.coeffs[0] = 2.0f;
      tmp.coeffs[1] = 1.0f;
      tmp.coeffs[2] = 1.5f;
    }

    tmp.coeffs[0] /= tmp.coeffs[1];
    tmp.coeffs[2] /= tmp.coeffs[1];
    tmp.coeffs[1] = 1.0f;
  }

  // remember daylight wb used for temperature/tint conversion,
  // assuming it corresponds to CIE daylight (D65)
  if(module->gui_data)
  {
    dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)module->gui_data;

    dt_bauhaus_slider_set_default(g->scale_r, tmp.coeffs[0]);
    dt_bauhaus_slider_set_default(g->scale_g, tmp.coeffs[1]);
    dt_bauhaus_slider_set_default(g->scale_b, tmp.coeffs[2]);

    // to have at least something and definitely not crash
    for(int c = 0; c < 3; c++) g->daylight_wb[c] = tmp.coeffs[c];

    if(!calculate_bogus_daylight_wb(module, g->daylight_wb))
    {
      // found camera matrix and used it to calculate bogus daylight wb
    }
    else
    {
      // if we didn't find anything for daylight wb, look for a wb preset with appropriate name.
      // we're normalizing that to be D65
      for(int i = 0; i < wb_preset_count; i++)
      {
        if(!strcmp(wb_preset[i].make, module->dev->image_storage.camera_maker) 
           && !strcmp(wb_preset[i].model, module->dev->image_storage.camera_model)
           && !strcmp(wb_preset[i].name, Daylight) && wb_preset[i].tuning == 0)
        {
          for(int k = 0; k < 3; k++) g->daylight_wb[k] = wb_preset[i].channel[k];
          break;
        }
      }
    }

    double TempK, tint;
    mul2temp(module, tmp.coeffs, &TempK, &tint);

    dt_bauhaus_slider_set_default(g->scale_k, TempK);
    dt_bauhaus_slider_set_default(g->scale_tint, tint);
  }

end:
  memcpy(module->params, &tmp, sizeof(dt_iop_temperature_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_temperature_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_temperature_global_data_t *gd
      = (dt_iop_temperature_global_data_t *)malloc(sizeof(dt_iop_temperature_global_data_t));
  module->data = gd;
  gd->kernel_whitebalance_4f = dt_opencl_create_kernel(program, "whitebalance_4f");
  gd->kernel_whitebalance_1f = dt_opencl_create_kernel(program, "whitebalance_1f");
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_temperature_params_t));
  module->default_params = malloc(sizeof(dt_iop_temperature_params_t));
  module->priority = 50; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_temperature_params_t);
  module->gui_data = NULL;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_temperature_global_data_t *gd = (dt_iop_temperature_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_whitebalance_4f);
  dt_opencl_free_kernel(gd->kernel_whitebalance_1f);
  free(module->data);
  module->data = NULL;
}

static void gui_update_from_coeffs(dt_iop_module_t *self)
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;

  double TempK, tint;
  mul2temp(self, p->coeffs, &TempK, &tint);

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->scale_k, TempK);
  dt_bauhaus_slider_set(g->scale_tint, tint);
  dt_bauhaus_slider_set(g->scale_r, p->coeffs[0]);
  dt_bauhaus_slider_set(g->scale_g, p->coeffs[1]);
  dt_bauhaus_slider_set(g->scale_b, p->coeffs[2]);
  darktable.gui->reset = 0;
}


static gboolean draw(GtkWidget *widget, cairo_t *cr, dt_iop_module_t *self)
{
  // capture gui color picked event.
  if(darktable.gui->reset) return FALSE;
  if(self->picked_color_max[0] < self->picked_color_min[0]) return FALSE;
  if(self->request_color_pick == DT_REQUEST_COLORPICK_OFF) return FALSE;
  static float old[3] = { 0, 0, 0 };
  const float *grayrgb = self->picked_color;
  if(grayrgb[0] == old[0] && grayrgb[1] == old[1] && grayrgb[2] == old[2]) return FALSE;
  for(int k = 0; k < 3; k++) old[k] = grayrgb[k];
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;
  for(int k = 0; k < 3; k++) p->coeffs[k] = (grayrgb[k] > 0.001f) ? 1.0f / grayrgb[k] : 1.0f;
  // normalize green:
  p->coeffs[0] /= p->coeffs[1];
  p->coeffs[2] /= p->coeffs[1];
  p->coeffs[1] = 1.0;
  for(int k = 0; k < 3; k++) p->coeffs[k] = fmaxf(0.0f, fminf(8.0f, p->coeffs[k]));
  gui_update_from_coeffs(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  return FALSE;
}

static void temp_changed(dt_iop_module_t *self)
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;

  const double TempK = dt_bauhaus_slider_get(g->scale_k);
  const double tint = dt_bauhaus_slider_get(g->scale_tint);

  double rgb[3];
  temp2mul(self, TempK, tint, rgb);

  // normalize
  rgb[0] /= rgb[1];
  rgb[2] /= rgb[1];
  rgb[1] = 1.0;

  for(int c = 0; c < 3; c++) p->coeffs[c] = rgb[c];

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->scale_r, p->coeffs[0]);
  dt_bauhaus_slider_set(g->scale_g, p->coeffs[1]);
  dt_bauhaus_slider_set(g->scale_b, p->coeffs[2]);
  darktable.gui->reset = 0;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void tint_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  temp_changed(self);
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_bauhaus_combobox_set(g->presets, -1);
}

static void temp_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  temp_changed(self);
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_bauhaus_combobox_set(g->presets, -1);
}

static void rgb_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;
  const float value = dt_bauhaus_slider_get(slider);
  if(slider == g->scale_r)
    p->coeffs[0] = value;
  else if(slider == g->scale_g)
    p->coeffs[1] = value;
  else if(slider == g->scale_b)
    p->coeffs[2] = value;

  gui_update_from_coeffs(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  dt_bauhaus_combobox_set(g->presets, -1);
}

static void apply_preset(dt_iop_module_t *self)
{
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  if(self->dt->gui->reset) return;
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;
  dt_iop_temperature_params_t *fp = (dt_iop_temperature_params_t *)self->default_params;
  const int tune = dt_bauhaus_slider_get(g->finetune);
  const int pos = dt_bauhaus_combobox_get(g->presets);
  switch(pos)
  {
    case -1: // just un-setting.
      return;
    case 0: // camera wb
      for(int k = 0; k < 3; k++) p->coeffs[k] = fp->coeffs[k];
      break;
    case 1: // camera neutral wb
      for(int k = 0; k < 3; k++) p->coeffs[k] = g->daylight_wb[k];
      break;
    case 2: // spot wb, expose callback will set p->coeffs.
      for(int k = 0; k < 3; k++) p->coeffs[k] = fp->coeffs[k];
      dt_iop_request_focus(self);
      self->request_color_pick = DT_REQUEST_COLORPICK_MODULE;

      /* set the area sample size*/
      if(self->request_color_pick != DT_REQUEST_COLORPICK_OFF)
        dt_lib_colorpicker_set_area(darktable.lib, 0.99);

      break;
    default: // camera WB presets
    {
      gboolean found = FALSE;
      // look through all variants of this preset, with different tuning
      for(int i = g->preset_num[pos]; (i < wb_preset_count) 
                                      && !strcmp(wb_preset[i].make, self->dev->image_storage.camera_maker)
                                      && !strcmp(wb_preset[i].model, self->dev->image_storage.camera_model)
                                      && !strcmp(wb_preset[i].name, wb_preset[g->preset_num[pos]].name);
          i++)
      {
        if(wb_preset[i].tuning == tune)
        {
          // got exact match!
          for(int k = 0; k < 3; k++) p->coeffs[k] = wb_preset[i].channel[k];
          found = TRUE;
          break;
        }
      }

      if(!found)
      {
        // ok, we haven't found exact match, need to interpolate

        // let's find 2 most closest tunings with needed_tuning in-between
        int min_id = INT_MIN, max_id = INT_MIN;

        // look through all variants of this preset, with different tuning, starting from second entry (if
        // any)
        int i = g->preset_num[pos] + 1;
        while((i < wb_preset_count) && !strcmp(wb_preset[i].make, self->dev->image_storage.camera_maker)
              && !strcmp(wb_preset[i].model, self->dev->image_storage.camera_model)
              && !strcmp(wb_preset[i].name, wb_preset[g->preset_num[pos]].name))
        {
          if(wb_preset[i - 1].tuning < tune && wb_preset[i].tuning > tune)
          {
            min_id = i - 1;
            max_id = i;
            break;
          }

          i++;
        }

        // have we found enough good data?
        if(min_id == INT_MIN || max_id == INT_MIN || min_id == max_id) break; // hysteresis

        wb_data interpolated = {.tuning = tune };
        dt_wb_preset_interpolate(&wb_preset[min_id], &wb_preset[max_id], &interpolated);
        for(int k = 0; k < 3; k++) p->coeffs[k] = interpolated.channel[k];
      }
    }
    break;
  }
  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
  gui_update_from_coeffs(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void presets_changed(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  apply_preset(self);
  const int pos = dt_bauhaus_combobox_get(widget);
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  gtk_widget_set_sensitive(g->finetune, pos >= DT_IOP_NUM_OF_STD_TEMP_PRESETS);
}

static void finetune_changed(GtkWidget *widget, gpointer user_data)
{
  apply_preset((dt_iop_module_t *)user_data);
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_temperature_gui_data_t));
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->default_params;

  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  g_signal_connect(G_OBJECT(self->widget), "draw", G_CALLBACK(draw), self);

  for(int k = 0; k < 3; k++) g->daylight_wb[k] = 1.0;
  g->scale_tint
      = dt_bauhaus_slider_new_with_range(self, DT_IOP_LOWEST_TINT, DT_IOP_HIGHEST_TINT, .01, 1.0, 3);
  g->scale_k = dt_bauhaus_slider_new_with_range(self, DT_IOP_LOWEST_TEMPERATURE, DT_IOP_HIGHEST_TEMPERATURE,
                                                10., 5000.0, 0);
  g->scale_r = dt_bauhaus_slider_new_with_range(self, 0.0, 8.0, .001, p->coeffs[0], 3);
  g->scale_g = dt_bauhaus_slider_new_with_range(self, 0.0, 8.0, .001, p->coeffs[1], 3);
  g->scale_b = dt_bauhaus_slider_new_with_range(self, 0.0, 8.0, .001, p->coeffs[2], 3);
  dt_bauhaus_slider_set_format(g->scale_k, "%.0fK");
  dt_bauhaus_widget_set_label(g->scale_tint, NULL, _("tint"));
  dt_bauhaus_widget_set_label(g->scale_k, NULL, _("temperature"));
  dt_bauhaus_widget_set_label(g->scale_r, NULL, _("red"));
  dt_bauhaus_widget_set_label(g->scale_g, NULL, _("green"));
  dt_bauhaus_widget_set_label(g->scale_b, NULL, _("blue"));

  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_tint, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_k, TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_r, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_g, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_b, TRUE, TRUE, 0);

  g->presets = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->presets, NULL, _("preset"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->presets, TRUE, TRUE, 0);
  g_object_set(G_OBJECT(g->presets), "tooltip-text", _("choose white balance preset from camera"),
               (char *)NULL);

  g->finetune = dt_bauhaus_slider_new_with_range(self, -9.0, 9.0, 1.0, 0.0, 0);
  dt_bauhaus_widget_set_label(g->finetune, NULL, _("finetune"));
  dt_bauhaus_slider_set_format(g->finetune, _("%.0f mired"));
  // initially doesn't have fine tuning stuff (camera wb)
  gtk_widget_set_sensitive(g->finetune, FALSE);
  gtk_box_pack_start(GTK_BOX(self->widget), g->finetune, TRUE, TRUE, 0);
  g_object_set(G_OBJECT(g->finetune), "tooltip-text", _("fine tune white balance preset"), (char *)NULL);

  self->gui_update(self);

  g_signal_connect(G_OBJECT(g->scale_tint), "value-changed", G_CALLBACK(tint_callback), self);
  g_signal_connect(G_OBJECT(g->scale_k), "value-changed", G_CALLBACK(temp_callback), self);
  g_signal_connect(G_OBJECT(g->scale_r), "value-changed", G_CALLBACK(rgb_callback), self);
  g_signal_connect(G_OBJECT(g->scale_g), "value-changed", G_CALLBACK(rgb_callback), self);
  g_signal_connect(G_OBJECT(g->scale_b), "value-changed", G_CALLBACK(rgb_callback), self);
  g_signal_connect(G_OBJECT(g->presets), "value-changed", G_CALLBACK(presets_changed), self);
  g_signal_connect(G_OBJECT(g->finetune), "value-changed", G_CALLBACK(finetune_changed), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
