/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.
    copyright (c) 2011 henrik andersson.

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
#include "common/opencl.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop_math.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#if defined(__SSE__)
#include <xmmintrin.h>
#endif
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include <gdk/gdkkeysyms.h>

// max iccprofile file name length
#define DT_IOP_COLOR_ICC_LEN 100
#define LUT_SAMPLES 0x10000

DT_MODULE_INTROSPECTION(4, dt_iop_colorout_params_t)

typedef struct dt_iop_colorout_data_t
{
  dt_colorspaces_color_profile_type_t type;
  dt_colorspaces_color_mode_t mode;
  float lut[3][LUT_SAMPLES];
  float cmatrix[9];
  cmsHTRANSFORM *xform;
  float unbounded_coeffs[3][3]; // for extrapolation of shaper curves
} dt_iop_colorout_data_t;

typedef struct dt_iop_colorout_global_data_t
{
  int kernel_colorout;
} dt_iop_colorout_global_data_t;

typedef struct dt_iop_colorout_params_t
{
  dt_colorspaces_color_profile_type_t type;
  char filename[DT_IOP_COLOR_ICC_LEN];
  dt_iop_color_intent_t intent;
} dt_iop_colorout_params_t;

typedef struct dt_iop_colorout_gui_data_t
{
  GtkWidget *output_intent, *output_profile;
} dt_iop_colorout_gui_data_t;


const char *name()
{
  return _("output color profile");
}

int groups()
{
  return IOP_GROUP_COLOR;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  /*  if(old_version == 1 && new_version == 2)
  {
    dt_iop_colorout_params_t *o = (dt_iop_colorout_params_t *)old_params;
    dt_iop_colorout_params_t *n = (dt_iop_colorout_params_t *)new_params;
    memcpy(n,o,sizeof(dt_iop_colorout_params_t));
    n->seq = 0;
    return 0;
    }*/
  if((old_version == 2 || old_version == 3) && new_version == 4)
  {
    typedef struct dt_iop_colorout_params_v3_t
    {
      char iccprofile[DT_IOP_COLOR_ICC_LEN];
      char displayprofile[DT_IOP_COLOR_ICC_LEN];
      dt_iop_color_intent_t intent;
      dt_iop_color_intent_t displayintent;
      char softproof_enabled;
      char softproofprofile[DT_IOP_COLOR_ICC_LEN];
      dt_iop_color_intent_t softproofintent;
    } dt_iop_colorout_params_v3_t;


    dt_iop_colorout_params_v3_t *o = (dt_iop_colorout_params_v3_t *)old_params;
    dt_iop_colorout_params_t *n = (dt_iop_colorout_params_t *)new_params;
    memset(n, 0, sizeof(dt_iop_colorout_params_t));

    if(!strcmp(o->iccprofile, "sRGB"))
      n->type = DT_COLORSPACE_SRGB;
    else if(!strcmp(o->iccprofile, "linear_rec709_rgb") || !strcmp(o->iccprofile, "linear_rgb"))
      n->type = DT_COLORSPACE_LIN_REC709;
    else if(!strcmp(o->iccprofile, "linear_rec2020_rgb"))
      n->type = DT_COLORSPACE_LIN_REC2020;
    else if(!strcmp(o->iccprofile, "adobergb"))
      n->type = DT_COLORSPACE_ADOBERGB;
    else if(!strcmp(o->iccprofile, "X profile"))
      n->type = DT_COLORSPACE_DISPLAY;
    else
    {
      n->type = DT_COLORSPACE_FILE;
      g_strlcpy(n->filename, o->iccprofile, sizeof(n->filename));
    }

    n->intent = o->intent;

    return 0;
  }

  return 1;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_colorout_global_data_t *gd
      = (dt_iop_colorout_global_data_t *)malloc(sizeof(dt_iop_colorout_global_data_t));
  module->data = gd;
  gd->kernel_colorout = dt_opencl_create_kernel(program, "colorout");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_colorout_global_data_t *gd = (dt_iop_colorout_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_colorout);
  free(module->data);
  module->data = NULL;
}

static void intent_changed(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)self->params;
  p->intent = (dt_iop_color_intent_t)dt_bauhaus_combobox_get(widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void output_profile_changed(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)self->params;
  int pos = dt_bauhaus_combobox_get(widget);

  for(GList *profiles = darktable.color_profiles->profiles; profiles; profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)profiles->data;
    if(pp->out_pos == pos)
    {
      p->type = pp->type;
      g_strlcpy(p->filename, pp->filename, sizeof(p->filename));
      dt_dev_add_history_item(darktable.develop, self, TRUE);
      return;
    }
  }

  fprintf(stderr, "[colorout] color profile %s seems to have disappeared!\n", dt_colorspaces_get_name(p->type, p->filename));
}

static void _signal_profile_changed(gpointer instance, gpointer user_data)
{
  dt_develop_t *dev = (dt_develop_t *)user_data;
  if(!dev->gui_attached || dev->gui_leaving) return;
  dt_dev_reprocess_center(dev);
}

#if 1
static float lerp_lut(const float *const lut, const float v)
{
  // TODO: check if optimization is worthwhile!
  const float ft = CLAMPS(v * (LUT_SAMPLES - 1), 0, LUT_SAMPLES - 1);
  const int t = ft < LUT_SAMPLES - 2 ? ft : LUT_SAMPLES - 2;
  const float f = ft - t;
  const float l1 = lut[t];
  const float l2 = lut[t + 1];
  return l1 * (1.0f - f) + l2 * f;
}
#endif

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_colorout_data_t *d = (dt_iop_colorout_data_t *)piece->data;
  dt_iop_colorout_global_data_t *gd = (dt_iop_colorout_global_data_t *)self->data;
  cl_mem dev_m = NULL, dev_r = NULL, dev_g = NULL, dev_b = NULL, dev_coeffs = NULL;

  cl_int err = -999;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  if(d->type == DT_COLORSPACE_LAB)
  {
    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { roi_in->width, roi_in->height, 1 };
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, region);
    if(err != CL_SUCCESS) goto error;
    return TRUE;
  }

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };

  dev_m = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 9, d->cmatrix);
  if(dev_m == NULL) goto error;
  dev_r = dt_opencl_copy_host_to_device(devid, d->lut[0], 256, 256, sizeof(float));
  if(dev_r == NULL) goto error;
  dev_g = dt_opencl_copy_host_to_device(devid, d->lut[1], 256, 256, sizeof(float));
  if(dev_g == NULL) goto error;
  dev_b = dt_opencl_copy_host_to_device(devid, d->lut[2], 256, 256, sizeof(float));
  if(dev_b == NULL) goto error;
  dev_coeffs
      = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3 * 3, (float *)d->unbounded_coeffs);
  if(dev_coeffs == NULL) goto error;
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorout, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorout, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorout, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorout, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorout, 4, sizeof(cl_mem), (void *)&dev_m);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorout, 5, sizeof(cl_mem), (void *)&dev_r);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorout, 6, sizeof(cl_mem), (void *)&dev_g);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorout, 7, sizeof(cl_mem), (void *)&dev_b);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorout, 8, sizeof(cl_mem), (void *)&dev_coeffs);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_colorout, sizes);
  if(err != CL_SUCCESS) goto error;
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_r);
  dt_opencl_release_mem_object(dev_g);
  dt_opencl_release_mem_object(dev_b);
  dt_opencl_release_mem_object(dev_coeffs);
  return TRUE;

error:
  if(dev_m != NULL) dt_opencl_release_mem_object(dev_m);
  if(dev_r != NULL) dt_opencl_release_mem_object(dev_r);
  if(dev_g != NULL) dt_opencl_release_mem_object(dev_g);
  if(dev_b != NULL) dt_opencl_release_mem_object(dev_b);
  if(dev_coeffs != NULL) dt_opencl_release_mem_object(dev_coeffs);
  dt_print(DT_DEBUG_OPENCL, "[opencl_colorout] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

static void process_fastpath_apply_tonecurves(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                              const void *const ivoid, void *const ovoid,
                                              const dt_iop_roi_t *const roi_in,
                                              const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorout_data_t *const d = (dt_iop_colorout_data_t *)piece->data;
  const int ch = piece->colors;

  if(!isnan(d->cmatrix[0]))
  {
    // out is already converted to RGB from Lab.

    // do we have any lut to apply, or is this a linear profile?
    if((d->lut[0][0] >= 0.0f) && (d->lut[1][0] >= 0.0f) && (d->lut[2][0] >= 0.0f))
    { // apply profile
      float *const out = (float *const)ovoid;
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none)
#endif
      for(size_t k = 0; k < (size_t)ch * roi_out->width * roi_out->height; k += ch)
      {
        for(int c = 0; c < 3; c++)
        {
          out[k + c] = (out[k + c] < 1.0f) ? lerp_lut(d->lut[c], out[k + c])
                                           : dt_iop_eval_exp(d->unbounded_coeffs[c], out[k + c]);
        }
      }
    }
    else if((d->lut[0][0] >= 0.0f) || (d->lut[1][0] >= 0.0f) || (d->lut[2][0] >= 0.0f))
    { // apply profile
      float *const out = (float *const)ovoid;
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none)
#endif
      for(size_t k = 0; k < (size_t)ch * roi_out->width * roi_out->height; k += ch)
      {
        for(int c = 0; c < 3; c++)
        {
          if(d->lut[c][0] >= 0.0f)
          {
            out[k + c] = (out[k + c] < 1.0f) ? lerp_lut(d->lut[c], out[k + c])
                                             : dt_iop_eval_exp(d->unbounded_coeffs[c], out[k + c]);
          }
        }
      }
    }
  }
}

#if defined(_OPENMP) && defined(OPENMP_SIMD_)
#pragma omp declare SIMD()
#endif
static inline float lab_f_inv_m(const float x)
{
  const float epsilon = (0.20689655172413796f); // cbrtf(216.0f/24389.0f);
  const float kappa_rcp_x16 = (16.0f * 27.0f / 24389.0f);
  const float kappa_rcp_x116 = (116.0f * 27.0f / 24389.0f);

  // x > epsilon
  float res_big = x * x * x;

  // x <= epsilon
  float res_small = ((kappa_rcp_x116 * x) - kappa_rcp_x16);

  // blend results according to whether each component is > epsilon or not
  return ((x > epsilon) ? res_big : res_small);
}

#if defined(__SSE__)
static inline __m128 lab_f_inv_m_SSE(const __m128 x)
{
  const __m128 epsilon = _mm_set1_ps(0.20689655172413796f); // cbrtf(216.0f/24389.0f);
  const __m128 kappa_rcp_x16 = _mm_set1_ps(16.0f * 27.0f / 24389.0f);
  const __m128 kappa_rcp_x116 = _mm_set1_ps(116.0f * 27.0f / 24389.0f);

  // x > epsilon
  const __m128 res_big = _mm_mul_ps(_mm_mul_ps(x, x), x);
  // x <= epsilon
  const __m128 res_small = _mm_sub_ps(_mm_mul_ps(kappa_rcp_x116, x), kappa_rcp_x16);

  // blend results according to whether each component is > epsilon or not
  const __m128 mask = _mm_cmpgt_ps(x, epsilon);
  return _mm_or_ps(_mm_and_ps(mask, res_big), _mm_andnot_ps(mask, res_small));
}
#endif

static inline void _dt_Lab_to_XYZ(const float *const Lab, float *const xyz)
{
  const float d50[] = { 0.9642f, 1.0f, 0.8249f };
  const float coef[] = { 1.0f / 500.0f, 1.0f / 116.0f, -1.0f / 200.0f };
  const float offset = (0.137931034f);

  float _F[3];
  _F[0] = Lab[1];
  _F[1] = Lab[0];
  _F[2] = Lab[2];

  for(int c = 0; c < 3; c++)
  {
    _F[c] *= coef[c];
  }

  float _F1[3];
  _F1[0] = _F[1];
  _F1[1] = 0.0f;
  _F1[2] = _F[1];

  for(int c = 0; c < 3; c++)
  {
    const float f = _F[c] + _F1[c] + offset;
    xyz[c] = d50[c] * lab_f_inv_m(f);
  }
}

#if defined(__SSE__)
static inline __m128 dt_Lab_to_XYZ_SSE(const __m128 Lab)
{
  const __m128 d50 = _mm_set_ps(0.0f, 0.8249f, 1.0f, 0.9642f);
  const __m128 coef = _mm_set_ps(0.0f, -1.0f / 200.0f, 1.0f / 116.0f, 1.0f / 500.0f);
  const __m128 offset = _mm_set1_ps(0.137931034f);

  // last component ins shuffle taken from 1st component of Lab to make sure it is not nan, so it will become
  // 0.0f in f
  const __m128 f = _mm_mul_ps(_mm_shuffle_ps(Lab, Lab, _MM_SHUFFLE(0, 2, 0, 1)), coef);

  return _mm_mul_ps(
      d50, lab_f_inv_m_SSE(_mm_add_ps(_mm_add_ps(f, _mm_shuffle_ps(f, f, _MM_SHUFFLE(1, 1, 3, 1))), offset)));
}
#endif

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorout_data_t *const d = (dt_iop_colorout_data_t *)piece->data;
  const int ch = piece->colors;
  const int gamutcheck = (d->mode == DT_PROFILE_GAMUTCHECK);

  if(d->type == DT_COLORSPACE_LAB)
  {
    memcpy(ovoid, ivoid, sizeof(float)*4*roi_out->width*roi_out->height);
  }
  else if(!isnan(d->cmatrix[0]))
  {
// fprintf(stderr,"Using cmatrix codepath\n");
// convert to rgb using matrix
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none)
#endif
    for(size_t k = 0; k < (size_t)ch * roi_out->width * roi_out->height; k += ch)
    {
      const float *const in = (const float *const)ivoid + (size_t)k;
      float *out = (float *)ovoid + (size_t)k;

      float xyz[3];
      _dt_Lab_to_XYZ(in, xyz);

      for(int c = 0; c < 3; c++)
      {
        out[c] = 0.0f;
        for(int i = 0; i < 3; i++)
        {
          out[c] += d->cmatrix[3 * c + i] * xyz[i];
        }
      }
    }

    process_fastpath_apply_tonecurves(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
  else
  {
// fprintf(stderr,"Using xform codepath\n");
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none)
#endif
    for(int k = 0; k < roi_out->height; k++)
    {
      const float *in = ((float *)ivoid) + (size_t)ch * k * roi_out->width;
      float *out = ((float *)ovoid) + (size_t)ch * k * roi_out->width;

      cmsDoTransform(d->xform, in, out, roi_out->width);

      if(gamutcheck)
      {
        for(int j = 0; j < roi_out->width; j++, out += 4)
        {
          if(out[0] < 0.0f || out[1] < 0.0f || out[2] < 0.0f)
          {
            out[0] = 0.0f;
            out[1] = 1.0f;
            out[2] = 1.0f;
          }
        }
      }
    }
  }

  if(piece->pipe->mask_display) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

#if defined(__SSE__)
void process_sse2(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                  void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorout_data_t *const d = (dt_iop_colorout_data_t *)piece->data;
  const int ch = piece->colors;
  const int gamutcheck = (d->mode == DT_PROFILE_GAMUTCHECK);

  if(d->type == DT_COLORSPACE_LAB)
  {
    memcpy(ovoid, ivoid, sizeof(float)*4*roi_out->width*roi_out->height);
  }
  else if(!isnan(d->cmatrix[0]))
  {
// fprintf(stderr,"Using cmatrix codepath\n");
// convert to rgb using matrix
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none)
#endif
    for(int j = 0; j < roi_out->height; j++)
    {

      float *in = (float *)ivoid + (size_t)ch * roi_in->width * j;
      float *out = (float *)ovoid + (size_t)ch * roi_out->width * j;
      const __m128 m0 = _mm_set_ps(0.0f, d->cmatrix[6], d->cmatrix[3], d->cmatrix[0]);
      const __m128 m1 = _mm_set_ps(0.0f, d->cmatrix[7], d->cmatrix[4], d->cmatrix[1]);
      const __m128 m2 = _mm_set_ps(0.0f, d->cmatrix[8], d->cmatrix[5], d->cmatrix[2]);

      for(int i = 0; i < roi_out->width; i++, in += ch, out += ch)
      {
        const __m128 xyz = dt_Lab_to_XYZ_SSE(_mm_load_ps(in));
        const __m128 t
            = _mm_add_ps(_mm_mul_ps(m0, _mm_shuffle_ps(xyz, xyz, _MM_SHUFFLE(0, 0, 0, 0))),
                         _mm_add_ps(_mm_mul_ps(m1, _mm_shuffle_ps(xyz, xyz, _MM_SHUFFLE(1, 1, 1, 1))),
                                    _mm_mul_ps(m2, _mm_shuffle_ps(xyz, xyz, _MM_SHUFFLE(2, 2, 2, 2)))));

        _mm_stream_ps(out, t);
      }
    }
    _mm_sfence();

    process_fastpath_apply_tonecurves(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
  else
  {
    // fprintf(stderr,"Using xform codepath\n");
    const __m128 outofgamutpixel = _mm_set_ps(0.0f, 1.0f, 1.0f, 0.0f);
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none)
#endif
    for(int k = 0; k < roi_out->height; k++)
    {
      const float *in = ((float *)ivoid) + (size_t)ch * k * roi_out->width;
      float *out = ((float *)ovoid) + (size_t)ch * k * roi_out->width;

      cmsDoTransform(d->xform, in, out, roi_out->width);

      if(gamutcheck)
      {
        for(int j = 0; j < roi_out->width; j++, out += 4)
        {
          const __m128 pixel = _mm_load_ps(out);
          __m128 ingamut = _mm_cmplt_ps(pixel, _mm_set_ps(-FLT_MAX, 0.0f, 0.0f, 0.0f));

          ingamut = _mm_or_ps(_mm_unpacklo_ps(ingamut, ingamut), _mm_unpackhi_ps(ingamut, ingamut));
          ingamut = _mm_or_ps(_mm_unpacklo_ps(ingamut, ingamut), _mm_unpackhi_ps(ingamut, ingamut));

          const __m128 result
              = _mm_or_ps(_mm_and_ps(ingamut, outofgamutpixel), _mm_andnot_ps(ingamut, pixel));
          _mm_stream_ps(out, result);
        }
      }
    }
    _mm_sfence();
  }

  if(piece->pipe->mask_display) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}
#endif

static cmsHPROFILE _make_clipping_profile(cmsHPROFILE profile)
{
  cmsUInt32Number size;
  cmsHPROFILE old_profile = profile;
  profile = NULL;

  if(old_profile && cmsSaveProfileToMem(old_profile, NULL, &size))
  {
    char *data = malloc(size);

    if(cmsSaveProfileToMem(old_profile, data, &size))
      profile = cmsOpenProfileFromMem(data, size);

    free(data);
  }

  return profile;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)p1;
  dt_iop_colorout_data_t *d = (dt_iop_colorout_data_t *)piece->data;

  d->type = p->type;
  const dt_colorspaces_color_profile_type_t over_type = dt_conf_get_int("plugins/lighttable/export/icctype");
  gchar *over_filename = dt_conf_get_string("plugins/lighttable/export/iccprofile");
  const dt_iop_color_intent_t over_intent = dt_conf_get_int("plugins/lighttable/export/iccintent");

  const int force_lcms2 = dt_conf_get_bool("plugins/lighttable/export/force_lcms2");

  dt_colorspaces_color_profile_type_t out_type = DT_COLORSPACE_SRGB;
  gchar *out_filename = NULL;
  dt_iop_color_intent_t out_intent = DT_INTENT_PERCEPTUAL;

  const cmsHPROFILE Lab = dt_colorspaces_get_profile(DT_COLORSPACE_LAB, "", DT_PROFILE_DIRECTION_ANY)->profile;

  cmsHPROFILE output = NULL;
  cmsHPROFILE softproof = NULL;

  d->mode = pipe->type == DT_DEV_PIXELPIPE_FULL ? darktable.color_profiles->mode : DT_PROFILE_NORMAL;

  if(d->xform)
  {
    cmsDeleteTransform(d->xform);
    d->xform = NULL;
  }
  d->cmatrix[0] = NAN;
  d->lut[0][0] = -1.0f;
  d->lut[1][0] = -1.0f;
  d->lut[2][0] = -1.0f;
  piece->process_cl_ready = 1;

  /* if we are exporting then check and set usage of override profile */
  if(pipe->type == DT_DEV_PIXELPIPE_EXPORT)
  {
    if(over_type != DT_COLORSPACE_NONE)
    {
      p->type = over_type;
      g_strlcpy(p->filename, over_filename, sizeof(p->filename));
    }
    if((unsigned int)over_intent < DT_INTENT_LAST) p->intent = over_intent;

    out_type = p->type;
    out_filename = p->filename;
    out_intent = p->intent;
  }
  else if(pipe->type == DT_DEV_PIXELPIPE_THUMBNAIL)
  {
    out_type = dt_mipmap_cache_get_colorspace();
    out_filename = (out_type == DT_COLORSPACE_DISPLAY ? darktable.color_profiles->display_filename : "");
    out_intent = darktable.color_profiles->display_intent;
  }
  else
  {
    /* we are not exporting, using display profile as output */
    out_type = darktable.color_profiles->display_type;
    out_filename = darktable.color_profiles->display_filename;
    out_intent = darktable.color_profiles->display_intent;
  }

  /*
   * Setup transform flags
   */
  uint32_t transformFlags = 0;

  /* creating output profile */
  if(out_type == DT_COLORSPACE_DISPLAY) pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);

  const dt_colorspaces_color_profile_t *out_profile
        = dt_colorspaces_get_profile(out_type, out_filename, DT_PROFILE_DIRECTION_OUT | DT_PROFILE_DIRECTION_DISPLAY);
  if(out_profile)
    output = out_profile->profile;
  else
  {
    output = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "",
                                        DT_PROFILE_DIRECTION_OUT | DT_PROFILE_DIRECTION_DISPLAY)->profile;
    dt_control_log(_("missing output profile has been replaced by sRGB!"));
    fprintf(stderr, "missing output profile `%s' has been replaced by sRGB!\n",
            dt_colorspaces_get_name(out_type, out_filename));
  }

  /* creating softproof profile if softproof is enabled */
  if(d->mode != DT_PROFILE_NORMAL && pipe->type == DT_DEV_PIXELPIPE_FULL)
  {
    const dt_colorspaces_color_profile_t *p = dt_colorspaces_get_profile(darktable.color_profiles->softproof_type,
                                                                         darktable.color_profiles->softproof_filename,
                                                                         DT_PROFILE_DIRECTION_OUT |
                                                                         DT_PROFILE_DIRECTION_DISPLAY);
    if(p)
      softproof = p->profile;
    else
    {
      softproof = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "",
                                             DT_PROFILE_DIRECTION_OUT | DT_PROFILE_DIRECTION_DISPLAY)->profile;
      dt_control_log(_("missing softproof profile has been replaced by sRGB!"));
      fprintf(stderr, "missing softproof profile `%s' has been replaced by sRGB!\n",
              dt_colorspaces_get_name(darktable.color_profiles->softproof_type,
                                      darktable.color_profiles->softproof_filename));
    }

    // some of our internal profiles are what lcms considers ideal profiles as they have a parametric TRC so
    // taking a roundtrip through those profiles during softproofing has no effect. as a workaround we have to
    // make lcms quantisize those gamma tables to get the desired effect.
    // in case that fails we don't enable softproofing.
    softproof = _make_clipping_profile(softproof);
    if(softproof)
    {
      /* TODO: the use of bpc should be userconfigurable either from module or preference pane */
      /* softproof flag and black point compensation */
      transformFlags |= cmsFLAGS_SOFTPROOFING | cmsFLAGS_NOCACHE | cmsFLAGS_BLACKPOINTCOMPENSATION;

      if(d->mode == DT_PROFILE_GAMUTCHECK) transformFlags |= cmsFLAGS_GAMUTCHECK;
    }
  }

  /*
   * NOTE: theoretically, we should be passing
   * UsedDirection = LCMS_USED_AS_PROOF  into
   * dt_colorspaces_get_matrix_from_output_profile() so that
   * dt_colorspaces_get_matrix_from_profile() knows it, but since we do not try
   * to use our matrix codepath when softproof is enabled, this seemed redundant.
   */

  /* get matrix from profile, if softproofing or high quality exporting always go xform codepath */
  if(d->mode != DT_PROFILE_NORMAL || force_lcms2
     || dt_colorspaces_get_matrix_from_output_profile(output, d->cmatrix, d->lut[0], d->lut[1], d->lut[2],
                                                      LUT_SAMPLES, out_intent))
  {
    d->cmatrix[0] = NAN;
    piece->process_cl_ready = 0;
    d->xform = cmsCreateProofingTransform(Lab, TYPE_LabA_FLT, output, TYPE_RGBA_FLT, softproof,
                                          out_intent, INTENT_RELATIVE_COLORIMETRIC, transformFlags);
  }

  // user selected a non-supported output profile, check that:
  if(!d->xform && isnan(d->cmatrix[0]))
  {
    dt_control_log(_("unsupported output profile has been replaced by sRGB!"));
    fprintf(stderr, "unsupported output profile `%s' has been replaced by sRGB!\n", out_profile->name);
    output = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "", DT_PROFILE_DIRECTION_OUT)->profile;
    if(d->mode != DT_PROFILE_NORMAL
       || dt_colorspaces_get_matrix_from_output_profile(output, d->cmatrix, d->lut[0], d->lut[1],
                                                        d->lut[2], LUT_SAMPLES, out_intent))
    {
      d->cmatrix[0] = NAN;
      piece->process_cl_ready = 0;

      d->xform = cmsCreateProofingTransform(Lab, TYPE_LabA_FLT, output, TYPE_RGBA_FLT, softproof,
                                            out_intent, INTENT_RELATIVE_COLORIMETRIC, transformFlags);
    }
  }

  if(out_type == DT_COLORSPACE_DISPLAY) pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);

  // now try to initialize unbounded mode:
  // we do extrapolation for input values above 1.0f.
  // unfortunately we can only do this if we got the computation
  // in our hands, i.e. for the fast builtin-dt-matrix-profile path.
  for(int k = 0; k < 3; k++)
  {
    // omit luts marked as linear (negative as marker)
    if(d->lut[k][0] >= 0.0f)
    {
      const float x[4] = { 0.7f, 0.8f, 0.9f, 1.0f };
      const float y[4] = { lerp_lut(d->lut[k], x[0]), lerp_lut(d->lut[k], x[1]), lerp_lut(d->lut[k], x[2]),
                           lerp_lut(d->lut[k], x[3]) };
      dt_iop_estimate_exp(x, y, 4, d->unbounded_coeffs[k]);
    }
    else
      d->unbounded_coeffs[k][0] = -1.0f;
  }

  g_free(over_filename);
  // softproof is never the original but always a copy that went through _make_clipping_profile()
  dt_colorspaces_cleanup_profile(softproof);
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_colorout_data_t));
  dt_iop_colorout_data_t *d = (dt_iop_colorout_data_t *)piece->data;
  d->xform = NULL;
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorout_data_t *d = (dt_iop_colorout_data_t *)piece->data;
  if(d->xform)
  {
    cmsDeleteTransform(d->xform);
    d->xform = NULL;
  }

  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_colorout_gui_data_t *g = (dt_iop_colorout_gui_data_t *)self->gui_data;
  dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)module->params;
  dt_bauhaus_combobox_set(g->output_intent, (int)p->intent);

  for(GList *iter = darktable.color_profiles->profiles; iter; iter = g_list_next(iter))
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)iter->data;
    if(pp->out_pos > -1 &&
       p->type == pp->type && (p->type != DT_COLORSPACE_FILE || !strcmp(p->filename, pp->filename)))
    {
      dt_bauhaus_combobox_set(g->output_profile, pp->out_pos);
      return;
    }
  }

  dt_bauhaus_combobox_set(g->output_profile, 0);
  fprintf(stderr, "[colorout] could not find requested profile `%s'!\n", dt_colorspaces_get_name(p->type, p->filename));
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_colorout_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_colorout_params_t));
  module->params_size = sizeof(dt_iop_colorout_params_t);
  module->gui_data = NULL;
  module->priority = 815; // module order created by iop_dependencies.py, do not edit!
  module->hide_enable_button = 1;
  module->default_enabled = 1;
  dt_iop_colorout_params_t tmp = (dt_iop_colorout_params_t){ DT_COLORSPACE_SRGB, "", DT_INTENT_PERCEPTUAL};
  memcpy(module->params, &tmp, sizeof(dt_iop_colorout_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_colorout_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

static void _preference_changed(gpointer instance, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorout_gui_data_t *g = (dt_iop_colorout_gui_data_t *)self->gui_data;

  const int force_lcms2 = dt_conf_get_bool("plugins/lighttable/export/force_lcms2");
  if(force_lcms2)
  {
    gtk_widget_set_no_show_all(g->output_intent, FALSE);
    gtk_widget_set_visible(g->output_intent, TRUE);
  }
  else
  {
    gtk_widget_set_no_show_all(g->output_intent, TRUE);
    gtk_widget_set_visible(g->output_intent, FALSE);
  }
}

void gui_init(struct dt_iop_module_t *self)
{
  const int force_lcms2 = dt_conf_get_bool("plugins/lighttable/export/force_lcms2");

  self->gui_data = calloc(1, sizeof(dt_iop_colorout_gui_data_t));
  dt_iop_colorout_gui_data_t *g = (dt_iop_colorout_gui_data_t *)self->gui_data;

  char datadir[PATH_MAX] = { 0 };
  char confdir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));
  dt_loc_get_user_config_dir(confdir, sizeof(confdir));

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  // TODO:
  g->output_intent = dt_bauhaus_combobox_new(self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->output_intent, TRUE, TRUE, 0);
  dt_bauhaus_widget_set_label(g->output_intent, NULL, _("output intent"));
  dt_bauhaus_combobox_add(g->output_intent, _("perceptual"));
  dt_bauhaus_combobox_add(g->output_intent, _("relative colorimetric"));
  dt_bauhaus_combobox_add(g->output_intent, C_("rendering intent", "saturation"));
  dt_bauhaus_combobox_add(g->output_intent, _("absolute colorimetric"));

  if(!force_lcms2)
  {
    gtk_widget_set_no_show_all(g->output_intent, TRUE);
    gtk_widget_set_visible(g->output_intent, FALSE);
  }

  g->output_profile = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->output_profile, NULL, _("output profile"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->output_profile, TRUE, TRUE, 0);
  for(GList *l = darktable.color_profiles->profiles; l; l = g_list_next(l))
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)l->data;
    if(prof->out_pos > -1) dt_bauhaus_combobox_add(g->output_profile, prof->name);
  }

  char tooltip[1024];
  gtk_widget_set_tooltip_text(g->output_intent, _("rendering intent"));
  snprintf(tooltip, sizeof(tooltip), _("ICC profiles in %s/color/out or %s/color/out"), confdir, datadir);
  gtk_widget_set_tooltip_text(g->output_profile, tooltip);

  g_signal_connect(G_OBJECT(g->output_intent), "value-changed", G_CALLBACK(intent_changed), (gpointer)self);
  g_signal_connect(G_OBJECT(g->output_profile), "value-changed", G_CALLBACK(output_profile_changed), (gpointer)self);

  // reload the profiles when the display or softproof profile changed!
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_CHANGED,
                            G_CALLBACK(_signal_profile_changed), self->dev);
  // update the gui when the preferences changed (i.e. show intent when using lcms2)
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_PREFERENCES_CHANGE,
                            G_CALLBACK(_preference_changed), (gpointer)self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_signal_profile_changed), self->dev);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_preference_changed), self);

  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
