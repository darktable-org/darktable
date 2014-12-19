/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.
    copyright (c) 2011 henrik andersson

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
#include "iop/color.h"
#include "develop/develop.h"
#include "control/control.h"
#include "gui/gtk.h"
#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "common/colormatrices.c"
#include "common/opencl.h"
#include "common/image_cache.h"
#ifdef HAVE_OPENJPEG
#include "common/imageio_j2k.h"
#endif
#include "common/imageio_jpeg.h"
#include "common/imageio_tiff.h"
#include "common/imageio_png.h"
#include "external/adobe_coeff.c"
#include <xmmintrin.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>

DT_MODULE_INTROSPECTION(3, dt_iop_colorin_params_t)

static void update_profile_list(dt_iop_module_t *self);

typedef enum dt_iop_color_normalize_t
{
  DT_NORMALIZE_OFF,
  DT_NORMALIZE_SRGB,
  DT_NORMALIZE_ADOBE_RGB,
  DT_NORMALIZE_LINEAR_REC709_RGB,
  DT_NORMALIZE_LINEAR_REC2020_RGB
} dt_iop_color_normalize_t;

typedef struct dt_iop_colorin_params_t
{
  char iccprofile[DT_IOP_COLOR_ICC_LEN];
  dt_iop_color_intent_t intent;
  int normalize;
  int blue_mapping;
} dt_iop_colorin_params_t;

typedef struct dt_iop_colorin_gui_data_t
{
  GtkWidget *cbox1, *cbox2, *cbox3;
  GList *image_profiles, *global_profiles;
  int n_image_profiles;
} dt_iop_colorin_gui_data_t;

typedef struct dt_iop_colorin_global_data_t
{
  int kernel_colorin_unbound;
  int kernel_colorin_clipping;
} dt_iop_colorin_global_data_t;

typedef struct dt_iop_colorin_data_t
{
  cmsHPROFILE input;
  cmsHPROFILE Lab;
  cmsHPROFILE nrgb;
  cmsHTRANSFORM *xform_cam_Lab;
  cmsHTRANSFORM *xform_cam_nrgb;
  cmsHTRANSFORM *xform_nrgb_Lab;
  float lut[3][LUT_SAMPLES];
  float cmatrix[9];
  float nmatrix[9];
  float lmatrix[9];
  float unbounded_coeffs[3][3]; // approximation for extrapolation of shaper curves
  int blue_mapping;
} dt_iop_colorin_data_t;


const char *name()
{
  return _("input color profile");
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
  if(old_version == 1 && new_version == 3)
  {
    typedef struct dt_iop_colorin_params_v1_t
    {
      char iccprofile[DT_IOP_COLOR_ICC_LEN];
      dt_iop_color_intent_t intent;
    } dt_iop_colorin_params_v1_t;

    const dt_iop_colorin_params_v1_t *old = (dt_iop_colorin_params_v1_t *)old_params;
    dt_iop_colorin_params_t *new = (dt_iop_colorin_params_t *)new_params;

    g_strlcpy(new->iccprofile, old->iccprofile, DT_IOP_COLOR_ICC_LEN);
    new->intent = old->intent;
    new->normalize = 0;
    new->blue_mapping = 1;
    return 0;
  }
  if(old_version == 2 && new_version == 3)
  {
    typedef struct dt_iop_colorin_params_v2_t
    {
      char iccprofile[DT_IOP_COLOR_ICC_LEN];
      dt_iop_color_intent_t intent;
      int normalize;
    } dt_iop_colorin_params_v2_t;

    const dt_iop_colorin_params_v2_t *old = (dt_iop_colorin_params_v2_t *)old_params;
    dt_iop_colorin_params_t *new = (dt_iop_colorin_params_t *)new_params;

    g_strlcpy(new->iccprofile, old->iccprofile, DT_IOP_COLOR_ICC_LEN);
    new->intent = old->intent;
    new->normalize = old->normalize;
    new->blue_mapping = 1;
    return 0;
  }
  return 1;
}


void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_colorin_global_data_t *gd
      = (dt_iop_colorin_global_data_t *)malloc(sizeof(dt_iop_colorin_global_data_t));
  module->data = gd;
  gd->kernel_colorin_unbound = dt_opencl_create_kernel(program, "colorin_unbound");
  gd->kernel_colorin_clipping = dt_opencl_create_kernel(program, "colorin_clipping");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_colorin_global_data_t *gd = (dt_iop_colorin_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_colorin_unbound);
  dt_opencl_free_kernel(gd->kernel_colorin_clipping);
  free(module->data);
  module->data = NULL;
}

#if 0
static void intent_changed (GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)self->params;
  p->intent = (dt_iop_color_intent_t)dt_bauhaus_combobox_get(widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
#endif

static void profile_changed(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_request_focus(self);
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)self->params;
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;
  int pos = dt_bauhaus_combobox_get(widget);
  GList *prof;
  if(pos < g->n_image_profiles)
    prof = g->image_profiles;
  else
  {
    prof = g->global_profiles;
    pos -= g->n_image_profiles;
  }
  while(prof)
  {
    // could use g_list_nth. this seems safer?
    dt_iop_color_profile_t *pp = (dt_iop_color_profile_t *)prof->data;
    if(pp->pos == pos)
    {
      g_strlcpy(p->iccprofile, pp->filename, sizeof(p->iccprofile));
      dt_dev_add_history_item(darktable.develop, self, TRUE);
      return;
    }
    prof = g_list_next(prof);
  }
  // should really never happen.
  fprintf(stderr, "[colorin] color profile %s seems to have disappeared!\n", p->iccprofile);
}


static void normalize_changed(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)self->params;
  p->normalize = dt_bauhaus_combobox_get(widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


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

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;
  dt_iop_colorin_global_data_t *gd = (dt_iop_colorin_global_data_t *)self->data;
  cl_mem dev_m = NULL, dev_l = NULL, dev_r = NULL, dev_g = NULL, dev_b = NULL, dev_coeffs = NULL;

  int kernel;
  float *cmat, *lmat;

  if(d->nrgb)
  {
    kernel = gd->kernel_colorin_clipping;
    cmat = d->nmatrix;
    lmat = d->lmatrix;
  }
  else
  {
    kernel = gd->kernel_colorin_unbound;
    cmat = d->cmatrix;
    lmat = d->lmatrix;
  }

  cl_int err = -999;
  const int blue_mapping = d->blue_mapping && piece->pipe->image.flags & DT_IMAGE_RAW;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };
  dev_m = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 9, cmat);
  if(dev_m == NULL) goto error;
  dev_l = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 9, lmat);
  if(dev_l == NULL) goto error;
  dev_r = dt_opencl_copy_host_to_device(devid, d->lut[0], 256, 256, sizeof(float));
  if(dev_r == NULL) goto error;
  dev_g = dt_opencl_copy_host_to_device(devid, d->lut[1], 256, 256, sizeof(float));
  if(dev_g == NULL) goto error;
  dev_b = dt_opencl_copy_host_to_device(devid, d->lut[2], 256, 256, sizeof(float));
  if(dev_b == NULL) goto error;
  dev_coeffs
      = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3 * 3, (float *)d->unbounded_coeffs);
  if(dev_coeffs == NULL) goto error;
  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), (void *)&dev_m);
  dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), (void *)&dev_l);
  dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(cl_mem), (void *)&dev_r);
  dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(cl_mem), (void *)&dev_g);
  dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(cl_mem), (void *)&dev_b);
  dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(cl_int), (void *)&blue_mapping);
  dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(cl_mem), (void *)&dev_coeffs);
  err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
  if(err != CL_SUCCESS) goto error;
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_l);
  dt_opencl_release_mem_object(dev_r);
  dt_opencl_release_mem_object(dev_g);
  dt_opencl_release_mem_object(dev_b);
  dt_opencl_release_mem_object(dev_coeffs);
  return TRUE;

error:
  if(dev_m != NULL) dt_opencl_release_mem_object(dev_m);
  if(dev_l != NULL) dt_opencl_release_mem_object(dev_l);
  if(dev_r != NULL) dt_opencl_release_mem_object(dev_r);
  if(dev_g != NULL) dt_opencl_release_mem_object(dev_g);
  if(dev_b != NULL) dt_opencl_release_mem_object(dev_b);
  if(dev_coeffs != NULL) dt_opencl_release_mem_object(dev_coeffs);
  dt_print(DT_DEBUG_OPENCL, "[opencl_colorin] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

static inline __m128 lab_f_m(const __m128 x)
{
  const __m128 epsilon = _mm_set1_ps(216.0f / 24389.0f);
  const __m128 kappa = _mm_set1_ps(24389.0f / 27.0f);

  // calculate as if x > epsilon : result = cbrtf(x)
  // approximate cbrtf(x):
  const __m128 a = _mm_castsi128_ps(
      _mm_add_epi32(_mm_cvtps_epi32(_mm_div_ps(_mm_cvtepi32_ps(_mm_castps_si128(x)), _mm_set1_ps(3.0f))),
                    _mm_set1_epi32(709921077)));
  const __m128 a3 = _mm_mul_ps(_mm_mul_ps(a, a), a);
  const __m128 res_big
      = _mm_div_ps(_mm_mul_ps(a, _mm_add_ps(a3, _mm_add_ps(x, x))), _mm_add_ps(_mm_add_ps(a3, a3), x));

  // calculate as if x <= epsilon : result = (kappa*x+16)/116
  const __m128 res_small
      = _mm_div_ps(_mm_add_ps(_mm_mul_ps(kappa, x), _mm_set1_ps(16.0f)), _mm_set1_ps(116.0f));

  // blend results according to whether each component is > epsilon or not
  const __m128 mask = _mm_cmpgt_ps(x, epsilon);
  return _mm_or_ps(_mm_and_ps(mask, res_big), _mm_andnot_ps(mask, res_small));
}

static inline __m128 dt_XYZ_to_Lab_SSE(const __m128 XYZ)
{
  const __m128 d50_inv = _mm_set_ps(0.0f, 1.0f / 0.8249f, 1.0f, 1.0f / 0.9642f);
  const __m128 coef = _mm_set_ps(0.0f, 200.0f, 500.0f, 116.0f);
  const __m128 f = lab_f_m(_mm_mul_ps(XYZ, d50_inv));
  // because d50_inv.z is 0.0f, lab_f(0) == 16/116, so Lab[0] = 116*f[0] - 16 equal to 116*(f[0]-f[3])
  return _mm_mul_ps(coef, _mm_sub_ps(_mm_shuffle_ps(f, f, _MM_SHUFFLE(3, 1, 0, 1)),
                                     _mm_shuffle_ps(f, f, _MM_SHUFFLE(3, 2, 1, 3))));
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid,
             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int ch = piece->colors;
  const int clipping = (d->nrgb != NULL);
  const int blue_mapping = d->blue_mapping && piece->pipe->image.flags & DT_IMAGE_RAW;

  if(!isnan(d->cmatrix[0]))
  {
    // only color matrix. use our optimized fast path!
    const float *const cmat = d->cmatrix;
    const float *const nmat = d->nmatrix;
    const float *const lmat = d->lmatrix;
    float *in = (float *)ivoid;
    float *out = (float *)ovoid;
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(roi_in, roi_out, out, in) schedule(static)
#endif
    for(int j = 0; j < roi_out->height; j++)
    {

      float *buf_in = in + (size_t)ch * roi_in->width * j;
      float *buf_out = out + (size_t)ch * roi_out->width * j;
      float cam[3];
      const __m128 cm0 = _mm_set_ps(0.0f, cmat[6], cmat[3], cmat[0]);
      const __m128 cm1 = _mm_set_ps(0.0f, cmat[7], cmat[4], cmat[1]);
      const __m128 cm2 = _mm_set_ps(0.0f, cmat[8], cmat[5], cmat[2]);

      const __m128 nm0 = _mm_set_ps(0.0f, nmat[6], nmat[3], nmat[0]);
      const __m128 nm1 = _mm_set_ps(0.0f, nmat[7], nmat[4], nmat[1]);
      const __m128 nm2 = _mm_set_ps(0.0f, nmat[8], nmat[5], nmat[2]);

      const __m128 lm0 = _mm_set_ps(0.0f, lmat[6], lmat[3], lmat[0]);
      const __m128 lm1 = _mm_set_ps(0.0f, lmat[7], lmat[4], lmat[1]);
      const __m128 lm2 = _mm_set_ps(0.0f, lmat[8], lmat[5], lmat[2]);

      for(int i = 0; i < roi_out->width; i++, buf_in += ch, buf_out += ch)
      {

        // memcpy(cam, buf_in, sizeof(float)*3);
        // avoid calling this for linear profiles (marked with negative entries), assures unbounded
        // color management without extrapolation.
        for(int i = 0; i < 3; i++)
          cam[i] = (d->lut[i][0] >= 0.0f)
                       ? ((buf_in[i] < 1.0f) ? lerp_lut(d->lut[i], buf_in[i])
                                             : dt_iop_eval_exp(d->unbounded_coeffs[i], buf_in[i]))
                       : buf_in[i];

        if(blue_mapping)
        {
          const float YY = cam[0] + cam[1] + cam[2];
          if(YY > 0.0f)
          {
            // manual gamut mapping. these values cause trouble when converting back from Lab to sRGB.
            // deeply saturated blues turn into purple fringes, so dampen them before conversion.
            // this is off for non-raw images, which don't seem to have this problem.
            // might be caused by too loose clipping bounds during highlight clipping?
            const float zz = cam[2] / YY;
            // lower amount and higher bound_z make the effect smaller.
            // the effect is weakened the darker input values are, saturating at bound_Y
            const float bound_z = 0.5f, bound_Y = 0.8f;
            const float amount = 0.11f;
            if(zz > bound_z)
            {
              const float t = (zz - bound_z) / (1.0f - bound_z) * fminf(1.0f, YY / bound_Y);
              cam[1] += t * amount;
              cam[2] -= t * amount;
            }
          }
        }

#if 0
        __attribute__((aligned(16))) float XYZ[4];
        _mm_store_ps(XYZ,_mm_add_ps(_mm_add_ps( _mm_mul_ps(m0,_mm_set1_ps(cam[0])), _mm_mul_ps(m1,_mm_set1_ps(cam[1]))), _mm_mul_ps(m2,_mm_set1_ps(cam[2]))));
        dt_XYZ_to_Lab(XYZ, buf_out);
#endif
        if(!clipping)
        {
          __m128 xyz = _mm_add_ps(
              _mm_add_ps(_mm_mul_ps(cm0, _mm_set1_ps(cam[0])), _mm_mul_ps(cm1, _mm_set1_ps(cam[1]))),
              _mm_mul_ps(cm2, _mm_set1_ps(cam[2])));
          _mm_stream_ps(buf_out, dt_XYZ_to_Lab_SSE(xyz));
        }
        else
        {
          __m128 nrgb = _mm_add_ps(
              _mm_add_ps(_mm_mul_ps(nm0, _mm_set1_ps(cam[0])), _mm_mul_ps(nm1, _mm_set1_ps(cam[1]))),
              _mm_mul_ps(nm2, _mm_set1_ps(cam[2])));
          __m128 crgb = _mm_min_ps(_mm_max_ps(nrgb, _mm_set1_ps(0.0f)), _mm_set1_ps(1.0f));
          __m128 xyz
              = _mm_add_ps(_mm_add_ps(_mm_mul_ps(lm0, _mm_shuffle_ps(crgb, crgb, _MM_SHUFFLE(0, 0, 0, 0))),
                                      _mm_mul_ps(lm1, _mm_shuffle_ps(crgb, crgb, _MM_SHUFFLE(1, 1, 1, 1)))),
                           _mm_mul_ps(lm2, _mm_shuffle_ps(crgb, crgb, _MM_SHUFFLE(2, 2, 2, 2))));
          _mm_stream_ps(buf_out, dt_XYZ_to_Lab_SSE(xyz));
        }
      }
    }
    _mm_sfence();
  }
  else
  {
// use general lcms2 fallback
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(ivoid, ovoid, roi_out)
#endif
    for(int k = 0; k < roi_out->height; k++)
    {
      const float *in = ((float *)ivoid) + (size_t)ch * k * roi_out->width;
      float *out = ((float *)ovoid) + (size_t)ch * k * roi_out->width;

      void *cam = NULL;
      const void *input = NULL;
      if(blue_mapping)
      {
        input = cam = dt_alloc_align(16, 4 * sizeof(float) * roi_out->width);
        float *camptr = (float *)cam;
        for(int j = 0; j < roi_out->width; j++, in += 4, camptr += 4)
        {
          camptr[0] = in[0];
          camptr[1] = in[1];
          camptr[2] = in[2];

          const float YY = camptr[0] + camptr[1] + camptr[2];
          const float zz = camptr[2] / YY;
          const float bound_z = 0.5f, bound_Y = 0.5f;
          const float amount = 0.11f;
          if(zz > bound_z)
          {
            const float t = (zz - bound_z) / (1.0f - bound_z) * fminf(1.0, YY / bound_Y);
            camptr[1] += t * amount;
            camptr[2] -= t * amount;
          }
        }
      }
      else
      {
        input = in;
      }

      // convert to (L,a/L,b/L) to be able to change L without changing saturation.
      if(!d->nrgb)
      {
        cmsDoTransform(d->xform_cam_Lab, input, out, roi_out->width);

        if(blue_mapping)
        {
          dt_free_align(cam);
          cam = NULL;
        }
      }
      else
      {
        void *rgb = dt_alloc_align(16, 4 * sizeof(float) * roi_out->width);
        cmsDoTransform(d->xform_cam_nrgb, input, rgb, roi_out->width);

        if(blue_mapping)
        {
          dt_free_align(cam);
          cam = NULL;
        }

        float *rgbptr = (float *)rgb;
        for(int j = 0; j < roi_out->width; j++, rgbptr += 4)
        {
          const __m128 min = _mm_setzero_ps();
          const __m128 max = _mm_set1_ps(1.0f);
          const __m128 input = _mm_load_ps(rgbptr);
          const __m128 result = _mm_max_ps(_mm_min_ps(input, max), min);
          _mm_store_ps(rgbptr, result);
        }

        cmsDoTransform(d->xform_nrgb_Lab, rgb, out, roi_out->width);
        dt_free_align(rgb);
      }
    }
  }

  if(piece->pipe->mask_display) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

static void mat3mul(float *dst, const float *const m1, const float *const m2)
{
  for(int k = 0; k < 3; k++)
  {
    for(int i = 0; i < 3; i++)
    {
      float x = 0.0f;
      for(int j = 0; j < 3; j++) x += m1[3 * k + j] * m2[3 * j + i];
      dst[3 * k + i] = x;
    }
  }
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)p1;
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;

  if(d->input) cmsCloseProfile(d->input);
  d->input = NULL;
  if(d->nrgb) cmsCloseProfile(d->nrgb);
  d->nrgb = NULL;

  d->blue_mapping = p->blue_mapping;

  switch(p->normalize)
  {
    case DT_NORMALIZE_SRGB:
      d->nrgb = dt_colorspaces_create_srgb_profile();
      break;
    case DT_NORMALIZE_ADOBE_RGB:
      d->nrgb = dt_colorspaces_create_adobergb_profile();
      break;
    case DT_NORMALIZE_LINEAR_REC709_RGB:
      d->nrgb = dt_colorspaces_create_linear_rec709_rgb_profile();
      break;
    case DT_NORMALIZE_LINEAR_REC2020_RGB:
      d->nrgb = dt_colorspaces_create_linear_rec2020_rgb_profile();
      break;
    case DT_NORMALIZE_OFF:
    default:
      d->nrgb = NULL;
  }

  if(d->xform_cam_Lab)
  {
    cmsDeleteTransform(d->xform_cam_Lab);
    d->xform_cam_Lab = NULL;
  }
  if(d->xform_cam_nrgb)
  {
    cmsDeleteTransform(d->xform_cam_nrgb);
    d->xform_cam_nrgb = NULL;
  }
  if(d->xform_nrgb_Lab)
  {
    cmsDeleteTransform(d->xform_nrgb_Lab);
    d->xform_nrgb_Lab = NULL;
  }

  d->cmatrix[0] = d->nmatrix[0] = d->lmatrix[0] = NAN;
  d->lut[0][0] = -1.0f;
  d->lut[1][0] = -1.0f;
  d->lut[2][0] = -1.0f;
  piece->process_cl_ready = 1;
  char datadir[PATH_MAX] = { 0 };
  char filename[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));

  char iccprofile[DT_IOP_COLOR_ICC_LEN];
  snprintf(iccprofile, sizeof(iccprofile), "%s", p->iccprofile);
  if(!strcmp(iccprofile, "Lab"))
  {
    piece->enabled = 0;
    return;
  }
  piece->enabled = 1;

  if(!strcmp(iccprofile, "darktable"))
  {
    char makermodel[1024];
    dt_colorspaces_get_makermodel(makermodel, sizeof(makermodel), pipe->image.exif_maker,
                                  pipe->image.exif_model);
    d->input = dt_colorspaces_create_darktable_profile(makermodel);
    if(!d->input) snprintf(iccprofile, sizeof(iccprofile), "eprofile");
  }
  if(!strcmp(iccprofile, "vendor"))
  {
    char makermodel[1024];
    dt_colorspaces_get_makermodel(makermodel, sizeof(makermodel), pipe->image.exif_maker,
                                  pipe->image.exif_model);
    d->input = dt_colorspaces_create_vendor_profile(makermodel);
    if(!d->input) snprintf(iccprofile, sizeof(iccprofile), "eprofile");
  }
  if(!strcmp(iccprofile, "alternate"))
  {
    char makermodel[1024];
    dt_colorspaces_get_makermodel(makermodel, sizeof(makermodel), pipe->image.exif_maker,
                                  pipe->image.exif_model);
    d->input = dt_colorspaces_create_alternate_profile(makermodel);
    if(!d->input) snprintf(iccprofile, sizeof(iccprofile), "eprofile");
  }
  if(!strcmp(iccprofile, "eprofile"))
  {
    // embedded color profile
    const dt_image_t *cimg = dt_image_cache_get(darktable.image_cache, pipe->image.id, 'r');
    if(cimg == NULL || cimg->profile == NULL)
      snprintf(iccprofile, sizeof(iccprofile), "ematrix");
    else
      d->input = cmsOpenProfileFromMem(cimg->profile, cimg->profile_size);
    dt_image_cache_read_release(darktable.image_cache, cimg);
  }
  if(!strcmp(iccprofile, "ematrix"))
  {
    // embedded matrix, hopefully D65
    if(isnan(pipe->image.d65_color_matrix[0]))
      snprintf(iccprofile, sizeof(iccprofile), "cmatrix");
    else
      d->input = dt_colorspaces_create_xyzimatrix_profile((float(*)[3])pipe->image.d65_color_matrix);
  }
  if(!strcmp(iccprofile, "cmatrix"))
  {
    // color matrix
    char makermodel[1024];
    dt_colorspaces_get_makermodel(makermodel, sizeof(makermodel), pipe->image.exif_maker,
                                  pipe->image.exif_model);
    float cam_xyz[12];
    cam_xyz[0] = NAN;
    dt_dcraw_adobe_coeff(makermodel, (float(*)[12])cam_xyz);
    if(isnan(cam_xyz[0]))
    {
      if(dt_image_is_raw(&pipe->image))
      {
        fprintf(stderr, "[colorin] `%s' color matrix not found!\n", makermodel);
        dt_control_log(_("`%s' color matrix not found!"), makermodel);
      }
      snprintf(iccprofile, sizeof(iccprofile), "linear_rec709_rgb");
    }
    else
      d->input = dt_colorspaces_create_xyzimatrix_profile((float(*)[3])cam_xyz);
  }

  if(!strcmp(iccprofile, "sRGB"))
  {
    d->input = dt_colorspaces_create_srgb_profile();
  }
  else if(!strcmp(iccprofile, "infrared"))
  {
    d->input = dt_colorspaces_create_linear_infrared_profile();
  }
  else if(!strcmp(iccprofile, "XYZ"))
  {
    d->input = dt_colorspaces_create_xyz_profile();
  }
  else if(!strcmp(iccprofile, "adobergb"))
  {
    d->input = dt_colorspaces_create_adobergb_profile();
  }
  else if(!strcmp(iccprofile, "linear_rec709_rgb") || !strcmp(iccprofile, "linear_rgb"))
  {
    d->input = dt_colorspaces_create_linear_rec709_rgb_profile();
  }
  else if(!strcmp(iccprofile, "linear_rec2020_rgb"))
  {
    d->input = dt_colorspaces_create_linear_rec2020_rgb_profile();
  }
  else if(!d->input)
  {
    dt_colorspaces_find_profile(filename, sizeof(filename), iccprofile, "in");
    d->input = cmsOpenProfileFromFile(filename, "r");
  }

  if(!d->input && strcmp(iccprofile, "sRGB"))
  {
    // use linear_rec709_rgb as fallback for missing non-sRGB profiles:
    d->input = dt_colorspaces_create_linear_rec709_rgb_profile();
  }

  // final resort: sRGB
  if(!d->input) d->input = dt_colorspaces_create_srgb_profile();

  // should never happen, but catch that case to avoid a crash
  if(!d->input)
  {
    dt_control_log(_("input profile could not be generated!"));
    piece->enabled = 0;
    return;
  }

  // prepare transformation matrix or lcms2 transforms as fallback
  if(d->nrgb)
  {
    // user wants us to clip to a given RGB profile
    if(dt_colorspaces_get_matrix_from_input_profile(d->input, d->cmatrix, d->lut[0], d->lut[1], d->lut[2],
                                                    LUT_SAMPLES))
    {
      piece->process_cl_ready = 0;
      d->cmatrix[0] = NAN;
      d->xform_cam_Lab = cmsCreateTransform(d->input, TYPE_RGBA_FLT, d->Lab, TYPE_LabA_FLT, p->intent, 0);
      d->xform_cam_nrgb = cmsCreateTransform(d->input, TYPE_RGBA_FLT, d->nrgb, TYPE_RGBA_FLT, p->intent, 0);
      d->xform_nrgb_Lab = cmsCreateTransform(d->nrgb, TYPE_RGBA_FLT, d->Lab, TYPE_LabA_FLT, p->intent, 0);
    }
    else
    {
      float lutr[1], lutg[1], lutb[1];
      float omat[9];
      dt_colorspaces_get_matrix_from_output_profile(d->nrgb, omat, lutr, lutg, lutb, 1);
      mat3mul(d->nmatrix, omat, d->cmatrix);
      dt_colorspaces_get_matrix_from_input_profile(d->nrgb, d->lmatrix, lutr, lutg, lutb, 1);
    }
  }
  else
  {
    // default mode: unbound processing
    if(dt_colorspaces_get_matrix_from_input_profile(d->input, d->cmatrix, d->lut[0], d->lut[1], d->lut[2],
                                                    LUT_SAMPLES))
    {
      piece->process_cl_ready = 0;
      d->cmatrix[0] = NAN;
      d->xform_cam_Lab = cmsCreateTransform(d->input, TYPE_RGBA_FLT, d->Lab, TYPE_LabA_FLT, p->intent, 0);
    }
  }

  // we might have failed generating the clipping transformations, check that:
  if(d->nrgb && ((!d->xform_cam_nrgb && isnan(d->nmatrix[0])) || (!d->xform_nrgb_Lab && isnan(d->lmatrix[0]))))
  {
    if(d->xform_cam_nrgb)
    {
      cmsDeleteTransform(d->xform_cam_nrgb);
      d->xform_cam_nrgb = NULL;
    }
    if(d->xform_nrgb_Lab)
    {
      cmsDeleteTransform(d->xform_nrgb_Lab);
      d->xform_nrgb_Lab = NULL;
    }
    dt_colorspaces_cleanup_profile(d->nrgb);
    d->nrgb = NULL;
  }

  // user selected a non-supported output profile, check that:
  if(!d->xform_cam_Lab && isnan(d->cmatrix[0]))
  {
    dt_control_log(_("unsupported input profile has been replaced by linear Rec709 RGB!"));
    if(d->input) dt_colorspaces_cleanup_profile(d->input);
    if(d->nrgb) dt_colorspaces_cleanup_profile(d->nrgb);
    d->nrgb = NULL;
    snprintf(iccprofile, sizeof(iccprofile), "linear_rec709_rgb");
    d->input = dt_colorspaces_create_linear_rec709_rgb_profile();
    if(dt_colorspaces_get_matrix_from_input_profile(d->input, d->cmatrix, d->lut[0], d->lut[1], d->lut[2],
                                                    LUT_SAMPLES))
    {
      piece->process_cl_ready = 0;
      d->cmatrix[0] = NAN;
      d->xform_cam_Lab = cmsCreateTransform(d->input, TYPE_RGBA_FLT, d->Lab, TYPE_LabA_FLT, p->intent, 0);
    }
  }

  // now try to initialize unbounded mode:
  // we do a extrapolation for input values above 1.0f.
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
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_colorin_data_t));
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;
  d->input = NULL;
  d->nrgb = NULL;
  d->xform_cam_Lab = NULL;
  d->xform_cam_nrgb = NULL;
  d->xform_nrgb_Lab = NULL;
  d->Lab = dt_colorspaces_create_lab_profile();
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;
  if(d->input) dt_colorspaces_cleanup_profile(d->input);
  dt_colorspaces_cleanup_profile(d->Lab);
  if(d->nrgb) dt_colorspaces_cleanup_profile(d->nrgb);
  if(d->xform_cam_Lab)
  {
    cmsDeleteTransform(d->xform_cam_Lab);
    d->xform_cam_Lab = NULL;
  }
  if(d->xform_cam_nrgb)
  {
    cmsDeleteTransform(d->xform_cam_nrgb);
    d->xform_cam_nrgb = NULL;
  }
  if(d->xform_nrgb_Lab)
  {
    cmsDeleteTransform(d->xform_nrgb_Lab);
    d->xform_nrgb_Lab = NULL;
  }

  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)module->params;
  // dt_bauhaus_combobox_set(g->cbox1, (int)p->intent);
  dt_bauhaus_combobox_set(g->cbox3, p->normalize);

  update_profile_list(self);

  // TODO: merge this into update_profile_list()
  GList *prof = g->image_profiles;
  while(prof)
  {
    dt_iop_color_profile_t *pp = (dt_iop_color_profile_t *)prof->data;
    if(!strcmp(pp->filename, p->iccprofile))
    {
      dt_bauhaus_combobox_set(g->cbox2, pp->pos);
      return;
    }
    prof = g_list_next(prof);
  }
  prof = g->global_profiles;
  while(prof)
  {
    dt_iop_color_profile_t *pp = (dt_iop_color_profile_t *)prof->data;
    if(!strcmp(pp->filename, p->iccprofile))
    {
      dt_bauhaus_combobox_set(g->cbox2, pp->pos + g->n_image_profiles);
      return;
    }
    prof = g_list_next(prof);
  }
  dt_bauhaus_combobox_set(g->cbox2, 0);

  if(strcmp(p->iccprofile, "darktable"))
    fprintf(stderr, "[colorin] could not find requested profile `%s'!\n", p->iccprofile);
}

// FIXME: update the gui when we add/remove the eprofile or ematrix
void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_colorin_params_t tmp = (dt_iop_colorin_params_t){ .iccprofile = "darktable",
                                                           .intent = DT_INTENT_PERCEPTUAL,
                                                           .normalize = DT_NORMALIZE_OFF,
                                                           .blue_mapping = 0 };

  // we might be called from presets update infrastructure => there is no image
  if(!module || !module->dev) goto end;

  gboolean use_eprofile = FALSE;
  // some file formats like jpeg can have an embedded color profile
  // currently we only support jpeg, j2k, tiff and png
  dt_image_t *img = dt_image_cache_get(darktable.image_cache, module->dev->image_storage.id, 'w');
  if(!img->profile)
  {
    char filename[PATH_MAX] = { 0 };
    gboolean from_cache = TRUE;
    dt_image_full_path(img->id, filename, sizeof(filename), &from_cache);
    const gchar *cc = filename + strlen(filename);
    for(; *cc != '.' && cc > filename; cc--)
      ;
    gchar *ext = g_ascii_strdown(cc + 1, -1);
    if(!strcmp(ext, "jpg") || !strcmp(ext, "jpeg"))
    {
      dt_imageio_jpeg_t jpg;
      if(!dt_imageio_jpeg_read_header(filename, &jpg))
      {
        img->profile_size = dt_imageio_jpeg_read_profile(&jpg, &img->profile);
        use_eprofile = (img->profile_size > 0);
      }
    }
#ifdef HAVE_OPENJPEG
    else if(!strcmp(ext, "jp2") || !strcmp(ext, "j2k") || !strcmp(ext, "j2c") || !strcmp(ext, "jpc"))
    {
      img->profile_size = dt_imageio_j2k_read_profile(filename, &img->profile);
      use_eprofile = (img->profile_size > 0);
    }
#endif
    else if(!strcmp(ext, "tif") || !strcmp(ext, "tiff"))
    {
      img->profile_size = dt_imageio_tiff_read_profile(filename, &img->profile);
      use_eprofile = (img->profile_size > 0);
    }
    else if(!strcmp(ext, "png"))
    {
      img->profile_size = dt_imageio_png_read_profile(filename, &img->profile);
      use_eprofile = (img->profile_size > 0);
    }
    g_free(ext);
  }
  else
    use_eprofile = TRUE; // the image has a profile assigned
  dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);

  if(use_eprofile)
    g_strlcpy(tmp.iccprofile, "eprofile", sizeof(tmp.iccprofile));
  else if(module->dev->image_storage.colorspace == DT_IMAGE_COLORSPACE_SRGB)
    g_strlcpy(tmp.iccprofile, "sRGB", sizeof(tmp.iccprofile));
  else if(module->dev->image_storage.colorspace == DT_IMAGE_COLORSPACE_ADOBE_RGB)
    g_strlcpy(tmp.iccprofile, "adobergb", sizeof(tmp.iccprofile));
  else if(dt_image_is_ldr(&module->dev->image_storage))
    g_strlcpy(tmp.iccprofile, "sRGB", sizeof(tmp.iccprofile));

end:
  memcpy(module->params, &tmp, sizeof(dt_iop_colorin_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_colorin_params_t));
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_colorin_data_t));
  module->params = malloc(sizeof(dt_iop_colorin_params_t));
  module->default_params = malloc(sizeof(dt_iop_colorin_params_t));
  module->params_size = sizeof(dt_iop_colorin_params_t);
  module->gui_data = NULL;
  module->priority = 350; // module order created by iop_dependencies.py, do not edit!
  module->hide_enable_button = 1;
  module->default_enabled = 1;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

static void update_profile_list(dt_iop_module_t *self)
{
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;

  // clear and refill the image profile list
  while(g->image_profiles)
  {
    g_free(g->image_profiles->data);
    g->image_profiles = g_list_delete_link(g->image_profiles, g->image_profiles);
  }
  g->image_profiles = NULL;
  g->n_image_profiles = 0;

  dt_iop_color_profile_t *prof;
  int pos = -1;
  // some file formats like jpeg can have an embedded color profile
  // currently we only support jpeg, j2k, tiff and png
  const dt_image_t *cimg = dt_image_cache_get(darktable.image_cache, self->dev->image_storage.id, 'r');
  if(cimg->profile)
  {
    prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
    g_strlcpy(prof->filename, "eprofile", sizeof(prof->filename));
    g_strlcpy(prof->name, "eprofile", sizeof(prof->name));
    g->image_profiles = g_list_append(g->image_profiles, prof);
    prof->pos = ++pos;
  }
  dt_image_cache_read_release(darktable.image_cache, cimg);
  // use the matrix embedded in some DNGs
  if(!isnan(self->dev->image_storage.d65_color_matrix[0]))
  {
    prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
    g_strlcpy(prof->filename, "ematrix", sizeof(prof->filename));
    g_strlcpy(prof->name, "ematrix", sizeof(prof->name));
    g->image_profiles = g_list_append(g->image_profiles, prof);
    prof->pos = ++pos;
  }
  // get color matrix from raw image:
  char makermodel[1024];
  dt_colorspaces_get_makermodel(makermodel, sizeof(makermodel), self->dev->image_storage.exif_maker,
                                self->dev->image_storage.exif_model);
  float cam_xyz[12];
  cam_xyz[0] = NAN;
  dt_dcraw_adobe_coeff(makermodel, (float(*)[12])cam_xyz);
  if(!isnan(cam_xyz[0]))
  {
    prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
    g_strlcpy(prof->filename, "cmatrix", sizeof(prof->filename));
    g_strlcpy(prof->name, "cmatrix", sizeof(prof->name));
    g->image_profiles = g_list_append(g->image_profiles, prof);
    prof->pos = ++pos;
  }

  // darktable built-in, if applicable
  for(int k = 0; k < dt_profiled_colormatrix_cnt; k++)
  {
    if(!strcasecmp(makermodel, dt_profiled_colormatrices[k].makermodel))
    {
      prof = (dt_iop_color_profile_t *)malloc(sizeof(dt_iop_color_profile_t));
      g_strlcpy(prof->filename, "darktable", sizeof(prof->filename));
      g_strlcpy(prof->name, "darktable", sizeof(prof->name));
      g->image_profiles = g_list_append(g->image_profiles, prof);
      prof->pos = ++pos;
      break;
    }
  }

  // darktable vendor matrix, if applicable
  for(int k = 0; k < dt_vendor_colormatrix_cnt; k++)
  {
    if(!strcmp(makermodel, dt_vendor_colormatrices[k].makermodel))
    {
      prof = (dt_iop_color_profile_t *)malloc(sizeof(dt_iop_color_profile_t));
      g_strlcpy(prof->filename, "vendor", sizeof(prof->filename));
      g_strlcpy(prof->name, "vendor", sizeof(prof->name));
      g->image_profiles = g_list_append(g->image_profiles, prof);
      prof->pos = ++pos;
      break;
    }
  }

  // darktable alternate matrix, if applicable
  for(int k = 0; k < dt_alternate_colormatrix_cnt; k++)
  {
    if(!strcmp(makermodel, dt_alternate_colormatrices[k].makermodel))
    {
      prof = (dt_iop_color_profile_t *)malloc(sizeof(dt_iop_color_profile_t));
      g_strlcpy(prof->filename, "alternate", sizeof(prof->filename));
      g_strlcpy(prof->name, "alternate", sizeof(prof->name));
      g->image_profiles = g_list_append(g->image_profiles, prof);
      prof->pos = ++pos;
      break;
    }
  }

  g->n_image_profiles = pos + 1;

  // update the gui
  dt_bauhaus_combobox_clear(g->cbox2);

  GList *l = g->image_profiles;
  while(l)
  {
    dt_iop_color_profile_t *prof = (dt_iop_color_profile_t *)l->data;
    if(!strcmp(prof->name, "eprofile"))
      dt_bauhaus_combobox_add(g->cbox2, _("embedded ICC profile"));
    else if(!strcmp(prof->name, "ematrix"))
      dt_bauhaus_combobox_add(g->cbox2, _("DNG embedded matrix"));
    else if(!strcmp(prof->name, "cmatrix"))
      dt_bauhaus_combobox_add(g->cbox2, _("standard color matrix"));
    else if(!strcmp(prof->name, "darktable"))
      dt_bauhaus_combobox_add(g->cbox2, _("enhanced color matrix"));
    else if(!strcmp(prof->name, "vendor"))
      dt_bauhaus_combobox_add(g->cbox2, _("vendor color matrix"));
    else if(!strcmp(prof->name, "alternate"))
      dt_bauhaus_combobox_add(g->cbox2, _("alternate color matrix"));
    else if(!strcmp(prof->name, "sRGB"))
      dt_bauhaus_combobox_add(g->cbox2, _("sRGB (e.g. JPG)"));
    else if(!strcmp(prof->name, "adobergb"))
      dt_bauhaus_combobox_add(g->cbox2, _("Adobe RGB (compatible)"));
    else if(!strcmp(prof->name, "linear_rec709_rgb") || !strcmp(prof->name, "linear_rgb"))
      dt_bauhaus_combobox_add(g->cbox2, _("linear Rec709 RGB"));
    else if(!strcmp(prof->name, "linear_rec2020_rgb"))
      dt_bauhaus_combobox_add(g->cbox2, _("linear Rec2020 RGB"));
    else if(!strcmp(prof->name, "infrared"))
      dt_bauhaus_combobox_add(g->cbox2, _("linear infrared BGR"));
    else if(!strcmp(prof->name, "XYZ"))
      dt_bauhaus_combobox_add(g->cbox2, _("linear XYZ"));
    else if(!strcmp(prof->name, "Lab"))
      dt_bauhaus_combobox_add(g->cbox2, _("Lab"));
    else
      dt_bauhaus_combobox_add(g->cbox2, prof->name);
    l = g_list_next(l);
  }
  l = g->global_profiles;
  while(l)
  {
    dt_iop_color_profile_t *prof = (dt_iop_color_profile_t *)l->data;
    if(!strcmp(prof->name, "eprofile"))
      dt_bauhaus_combobox_add(g->cbox2, _("embedded ICC profile"));
    else if(!strcmp(prof->name, "ematrix"))
      dt_bauhaus_combobox_add(g->cbox2, _("DNG embedded matrix"));
    else if(!strcmp(prof->name, "cmatrix"))
      dt_bauhaus_combobox_add(g->cbox2, _("standard color matrix"));
    else if(!strcmp(prof->name, "darktable"))
      dt_bauhaus_combobox_add(g->cbox2, _("enhanced color matrix"));
    else if(!strcmp(prof->name, "vendor"))
      dt_bauhaus_combobox_add(g->cbox2, _("vendor color matrix"));
    else if(!strcmp(prof->name, "alternate"))
      dt_bauhaus_combobox_add(g->cbox2, _("alternate color matrix"));
    else if(!strcmp(prof->name, "sRGB"))
      dt_bauhaus_combobox_add(g->cbox2, _("sRGB (e.g. JPG)"));
    else if(!strcmp(prof->name, "adobergb"))
      dt_bauhaus_combobox_add(g->cbox2, _("Adobe RGB (compatible)"));
    else if(!strcmp(prof->name, "linear_rec709_rgb") || !strcmp(prof->name, "linear_rgb"))
      dt_bauhaus_combobox_add(g->cbox2, _("linear Rec709 RGB"));
    else if(!strcmp(prof->name, "linear_rec2020_rgb"))
      dt_bauhaus_combobox_add(g->cbox2, _("linear Rec2020 RGB"));
    else if(!strcmp(prof->name, "infrared"))
      dt_bauhaus_combobox_add(g->cbox2, _("linear infrared BGR"));
    else if(!strcmp(prof->name, "XYZ"))
      dt_bauhaus_combobox_add(g->cbox2, _("linear XYZ"));
    else if(!strcmp(prof->name, "Lab"))
      dt_bauhaus_combobox_add(g->cbox2, _("Lab"));
    else
      dt_bauhaus_combobox_add(g->cbox2, prof->name);
    l = g_list_next(l);
  }
}

void gui_init(struct dt_iop_module_t *self)
{
  // pthread_mutex_lock(&darktable.plugin_threadsafe);
  self->gui_data = malloc(sizeof(dt_iop_colorin_gui_data_t));
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;

  g->image_profiles = g->global_profiles = NULL;
  dt_iop_color_profile_t *prof;

  // the profiles that are available for every image
  int pos = -1;

  // add linear Rec2020 RGB profile:
  prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
  g_strlcpy(prof->filename, "linear_rec2020_rgb", sizeof(prof->filename));
  g_strlcpy(prof->name, "linear_rec2020_rgb", sizeof(prof->name));
  g->global_profiles = g_list_append(g->global_profiles, prof);
  prof->pos = ++pos;

  // add linear Rec709 RGB profile:
  prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
  g_strlcpy(prof->filename, "linear_rec709_rgb", sizeof(prof->filename));
  g_strlcpy(prof->name, "linear_rec709_rgb", sizeof(prof->name));
  g->global_profiles = g_list_append(g->global_profiles, prof);
  prof->pos = ++pos;

  // sRGB for ldr image input
  prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
  g_strlcpy(prof->filename, "sRGB", sizeof(prof->filename));
  g_strlcpy(prof->name, "sRGB", sizeof(prof->name));
  g->global_profiles = g_list_append(g->global_profiles, prof);
  prof->pos = ++pos;

  // adobe rgb built-in
  prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
  g_strlcpy(prof->filename, "adobergb", sizeof(prof->filename));
  g_strlcpy(prof->name, "adobergb", sizeof(prof->name));
  g->global_profiles = g_list_append(g->global_profiles, prof);
  prof->pos = ++pos;

  // XYZ built-in
  prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
  g_strlcpy(prof->filename, "XYZ", sizeof(prof->filename));
  g_strlcpy(prof->name, "XYZ", sizeof(prof->name));
  g->global_profiles = g_list_append(g->global_profiles, prof);
  prof->pos = ++pos;

  // Lab built-in
  prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
  g_strlcpy(prof->filename, "Lab", sizeof(prof->filename));
  g_strlcpy(prof->name, "Lab", sizeof(prof->name));
  g->global_profiles = g_list_append(g->global_profiles, prof);
  prof->pos = ++pos;

  // infrared built-in
  prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
  g_strlcpy(prof->filename, "infrared", sizeof(prof->filename));
  g_strlcpy(prof->name, "infrared", sizeof(prof->name));
  g->global_profiles = g_list_append(g->global_profiles, prof);
  prof->pos = ++pos;

  // read {userconfig,datadir}/color/in/*.icc, in this order.
  char datadir[PATH_MAX] = { 0 };
  char confdir[PATH_MAX] = { 0 };
  char dirname[PATH_MAX] = { 0 };
  char filename[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(confdir, sizeof(confdir));
  dt_loc_get_datadir(datadir, sizeof(datadir));
  snprintf(dirname, sizeof(dirname), "%s/color/in", confdir);
  if(!g_file_test(dirname, G_FILE_TEST_IS_DIR)) snprintf(dirname, sizeof(dirname), "%s/color/in", datadir);
  cmsHPROFILE tmpprof;
  const gchar *d_name;
  GDir *dir = g_dir_open(dirname, 0, NULL);
  if(dir)
  {
    while((d_name = g_dir_read_name(dir)))
    {
      if(!strcmp(d_name, "linear_rec709_rgb") || !strcmp(d_name, "linear_rgb")) continue;
      snprintf(filename, sizeof(filename), "%s/%s", dirname, d_name);
      tmpprof = cmsOpenProfileFromFile(filename, "r");
      if(tmpprof)
      {
        char *lang = getenv("LANG");
        if(!lang) lang = "en_US";

        dt_iop_color_profile_t *prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
        dt_colorspaces_get_profile_name(tmpprof, lang, lang + 3, prof->name, sizeof(prof->name));

        g_strlcpy(prof->filename, d_name, sizeof(prof->filename));
        cmsCloseProfile(tmpprof);
        g->global_profiles = g_list_append(g->global_profiles, prof);
        prof->pos = ++pos;
      }
    }
    g_dir_close(dir);
  }

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  g->cbox2 = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->cbox2, NULL, _("profile"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->cbox2, TRUE, TRUE, 0);

  // now generate the list of profiles applicable to the current image and update the list
  update_profile_list(self);

  dt_bauhaus_combobox_set(g->cbox2, 0);

  char tooltip[1024];
  snprintf(tooltip, sizeof(tooltip), _("ICC profiles in %s/color/in or %s/color/in"), confdir, datadir);
  g_object_set(G_OBJECT(g->cbox2), "tooltip-text", tooltip, (char *)NULL);

  g_signal_connect(G_OBJECT(g->cbox2), "value-changed", G_CALLBACK(profile_changed), (gpointer)self);

  g->cbox3 = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->cbox3, NULL, _("gamut clipping"));

  dt_bauhaus_combobox_add(g->cbox3, _("off"));
  dt_bauhaus_combobox_add(g->cbox3, _("sRGB"));
  dt_bauhaus_combobox_add(g->cbox3, _("Adobe RGB (compatible)"));
  dt_bauhaus_combobox_add(g->cbox3, _("linear Rec709 RGB"));
  dt_bauhaus_combobox_add(g->cbox3, _("linear Rec2020 RGB"));

  g_object_set(G_OBJECT(g->cbox3), "tooltip-text", _("confine Lab values to gamut of RGB color space"),
               (char *)NULL);

  gtk_box_pack_start(GTK_BOX(self->widget), g->cbox3, TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(g->cbox3), "value-changed", G_CALLBACK(normalize_changed), (gpointer)self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;
  while(g->image_profiles)
  {
    g_free(g->image_profiles->data);
    g->image_profiles = g_list_delete_link(g->image_profiles, g->image_profiles);
  }
  while(g->global_profiles)
  {
    g_free(g->global_profiles->data);
    g->global_profiles = g_list_delete_link(g->global_profiles, g->global_profiles);
  }
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
