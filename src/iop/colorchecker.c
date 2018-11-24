/*
    This file is part of darktable,
    copyright (c) 2017 johannes hanika.

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
#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "common/opencl.h"
#include "common/exif.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/tiling.h"
#include "dtgtk/drawingarea.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"
#include "common/iop_group.h"
#include "iop/gaussian_elimination.h"
#include "libs/colorpicker.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>
#include <inttypes.h>

DT_MODULE_INTROSPECTION(2, dt_iop_colorchecker_params_t)

static const int colorchecker_patches = 24;
static const float colorchecker_Lab[] =
{ // from argyll ColorChecker.cie
 37.99,   13.56,  14.06, // dark skin
 65.71,   18.13,  17.81, // light skin
 49.93,   -4.88, -21.93, // blue sky
 43.14,  -13.10,  21.91, // foliage
 55.11,    8.84, -25.40, // blue flower
 70.72,  -33.40, -0.20 , // bluish green
 62.66,   36.07,  57.10, // orange
 40.02,   10.41, -45.96, // purple red
 51.12,   48.24,  16.25, // moderate red
 30.33,   22.98, -21.59, // purple
 72.53,  -23.71,  57.26, // yellow green
 71.94,  19.36 ,  67.86, // orange yellow
 28.78,  14.18 , -50.30, // blue
 55.26,  -38.34,  31.37, // green
 42.10,  53.38 ,  28.19, // red
 81.73,  4.04  ,  79.82, // yellow
 51.94,  49.99 , -14.57, // magenta
 51.04,  -28.63, -28.64, // cyan
 96.54,  -0.43 ,  1.19 , // white
 81.26,  -0.64 , -0.34 , // neutral 8
 66.77,  -0.73 , -0.50 , // neutral 65
 50.87,  -0.15 , -0.27 , // neutral 5
 35.66,  -0.42 , -1.23 , // neutral 35
 20.46,  -0.08 , -0.97   // black
};

// we came to the conclusion that more than 7x7 patches will not be
// manageable in the gui. the fitting experiments show however that you
// can do significantly better with 49 than you can with 24 patches,
// especially when considering max delta E.
#define MAX_PATCHES 49
typedef struct dt_iop_colorchecker_params_t
{
  float source_L[MAX_PATCHES];
  float source_a[MAX_PATCHES];
  float source_b[MAX_PATCHES];
  float target_L[MAX_PATCHES];
  float target_a[MAX_PATCHES];
  float target_b[MAX_PATCHES];
  int32_t num_patches;
} dt_iop_colorchecker_params_t;

typedef struct dt_iop_colorchecker_gui_data_t
{
  GtkWidget *area, *combobox_patch, *scale_L, *scale_a, *scale_b, *scale_C, *combobox_target;
  int patch, drawn_patch;
  cmsHTRANSFORM xform;
  int absolute_target; // 0: show relative offsets in sliders, 1: show absolute Lab values
} dt_iop_colorchecker_gui_data_t;

typedef struct dt_iop_colorchecker_data_t
{
  int32_t num_patches;
  float source_Lab[3*MAX_PATCHES];
  float coeff_L[MAX_PATCHES+4];
  float coeff_a[MAX_PATCHES+4];
  float coeff_b[MAX_PATCHES+4];
} dt_iop_colorchecker_data_t;

typedef struct dt_iop_colorchecker_global_data_t
{
  int kernel_colorchecker;
} dt_iop_colorchecker_global_data_t;


const char *name()
{
  return _("color look up table");
}

int groups()
{
  return dt_iop_get_group("color look up table", IOP_GROUP_COLOR);
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int legacy_params(
    dt_iop_module_t  *self,
    const void *const old_params,
    const int         old_version,
    void             *new_params,
    const int         new_version)
{
  static const float colorchecker_Lab_v1[] = {
    39.19, 13.76,  14.29,  // dark skin
    65.18, 19.00,  17.32,  // light skin
    49.46, -4.23,  -22.95, // blue sky
    42.85, -13.33, 22.12,  // foliage
    55.18, 9.44,   -24.94, // blue flower
    70.36, -32.77, -0.04,  // bluish green
    62.92, 35.49,  57.10,  // orange
    40.75, 11.41,  -46.03, // purple red
    52.10, 48.11,  16.89,  // moderate red
    30.67, 21.19,  -20.81, // purple
    73.08, -23.55, 56.97,  // yellow green
    72.43, 17.48,  68.20,  // orange yellow
    30.97, 12.67,  -46.30, // blue
    56.43, -40.66, 31.94,  // green
    43.40, 50.68,  28.84,  // red
    82.45, 2.41,   80.25,  // yellow
    51.98, 50.68,  -14.84, // magenta
    51.02, -27.63, -28.03, // cyan
    95.97, -0.40,  1.24,   // white
    81.10, -0.83,  -0.43,  // neutral 8
    66.81, -1.08,  -0.70,  // neutral 65
    50.98, -0.19,  -0.30,  // neutral 5
    35.72, -0.69,  -1.11,  // neutral 35
    21.46, 0.06,   -0.95,  // black
  };

  typedef struct dt_iop_colorchecker_params_v1_t
  {
    float target_L[24];
    float target_a[24];
    float target_b[24];
  } dt_iop_colorchecker_params_v1_t;

  if(old_version == 1 && new_version == 2)
  {
    dt_iop_colorchecker_params_v1_t *p1 = (dt_iop_colorchecker_params_v1_t *)old_params;
    dt_iop_colorchecker_params_t  *p2 = (dt_iop_colorchecker_params_t  *)new_params;

    p2->num_patches = 24;
    for(int k=0;k<24;k++)
    {
      p2->target_L[k] = p1->target_L[k];
      p2->target_a[k] = p1->target_a[k];
      p2->target_b[k] = p1->target_b[k];
      p2->source_L[k] = colorchecker_Lab_v1[3 * k + 0];
      p2->source_a[k] = colorchecker_Lab_v1[3 * k + 1];
      p2->source_b[k] = colorchecker_Lab_v1[3 * k + 2];
    }
    return 0;
  }
  return 1;
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_colorchecker_params_t p;
  memset(&p, 0, sizeof(p));
  p.num_patches = 24;
  p.target_L[ 0] = p.source_L[ 0] = 17.460945129394531;
  p.target_L[ 1] = p.source_L[ 1] = 26.878498077392578;
  p.target_L[ 2] = p.source_L[ 2] = 34.900054931640625;
  p.target_L[ 3] = p.source_L[ 3] = 21.692604064941406;
  p.target_L[ 4] = p.source_L[ 4] = 32.18853759765625;
  p.target_L[ 5] = p.source_L[ 5] = 62.531227111816406;
  p.target_L[ 6] = p.source_L[ 6] = 18.933284759521484;
  p.target_L[ 7] = p.source_L[ 7] = 53.936111450195312;
  p.target_L[ 8] = p.source_L[ 8] = 69.154266357421875;
  p.target_L[ 9] = p.source_L[ 9] = 43.381229400634766;
  p.target_L[10] = p.source_L[10] = 57.797889709472656;
  p.target_L[11] = p.source_L[11] = 73.27630615234375;
  p.target_L[12] = p.source_L[12] = 53.175498962402344;
  p.target_L[13] = p.source_L[13] = 49.111373901367188;
  p.target_L[14] = p.source_L[14] = 63.169830322265625;
  p.target_L[15] = p.source_L[15] = 61.896102905273438;
  p.target_L[16] = p.source_L[16] = 67.852409362792969;
  p.target_L[17] = p.source_L[17] = 72.489517211914062;
  p.target_L[18] = p.source_L[18] = 70.935714721679688;
  p.target_L[19] = p.source_L[19] = 70.173004150390625;
  p.target_L[20] = p.source_L[20] = 77.78826904296875;
  p.target_L[21] = p.source_L[21] = 76.070747375488281;
  p.target_L[22] = p.source_L[22] = 68.645004272460938;
  p.target_L[23] = p.source_L[23] = 74.502906799316406;
  p.target_a[ 0] = p.source_a[ 0] = 8.4928874969482422;
  p.target_a[ 1] = p.source_a[ 1] = 27.94782829284668;
  p.target_a[ 2] = p.source_a[ 2] = 43.8824462890625;
  p.target_a[ 3] = p.source_a[ 3] = 16.723676681518555;
  p.target_a[ 4] = p.source_a[ 4] = 39.174972534179688;
  p.target_a[ 5] = p.source_a[ 5] = 24.966419219970703;
  p.target_a[ 6] = p.source_a[ 6] = 8.8226642608642578;
  p.target_a[ 7] = p.source_a[ 7] = 34.451812744140625;
  p.target_a[ 8] = p.source_a[ 8] = 18.39008903503418;
  p.target_a[ 9] = p.source_a[ 9] = 28.272598266601562;
  p.target_a[10] = p.source_a[10] = 10.193824768066406;
  p.target_a[11] = p.source_a[11] = 13.241470336914062;
  p.target_a[12] = p.source_a[12] = 43.655307769775391;
  p.target_a[13] = p.source_a[13] = 23.247600555419922;
  p.target_a[14] = p.source_a[14] = 23.308664321899414;
  p.target_a[15] = p.source_a[15] = 11.138319969177246;
  p.target_a[16] = p.source_a[16] = 18.200069427490234;
  p.target_a[17] = p.source_a[17] = 15.363990783691406;
  p.target_a[18] = p.source_a[18] = 11.173545837402344;
  p.target_a[19] = p.source_a[19] = 11.313735961914062;
  p.target_a[20] = p.source_a[20] = 15.059500694274902;
  p.target_a[21] = p.source_a[21] = 4.7686996459960938;
  p.target_a[22] = p.source_a[22] = 3.0603706836700439;
  p.target_a[23] = p.source_a[23] = -3.687053918838501;
  p.target_b[ 0] = p.source_b[ 0] = -0.023579597473144531;
  p.target_b[ 1] = p.source_b[ 1] = 14.991056442260742;
  p.target_b[ 2] = p.source_b[ 2] = 26.443553924560547;
  p.target_b[ 3] = p.source_b[ 3] = 7.3905587196350098;
  p.target_b[ 4] = p.source_b[ 4] = 23.309671401977539;
  p.target_b[ 5] = p.source_b[ 5] = 19.262432098388672;
  p.target_b[ 6] = p.source_b[ 6] = 3.136211633682251;
  p.target_b[ 7] = p.source_b[ 7] = 31.949621200561523;
  p.target_b[ 8] = p.source_b[ 8] = 16.144514083862305;
  p.target_b[ 9] = p.source_b[ 9] = 25.893926620483398;
  p.target_b[10] = p.source_b[10] = 12.271202087402344;
  p.target_b[11] = p.source_b[11] = 16.763805389404297;
  p.target_b[12] = p.source_b[12] = 53.904998779296875;
  p.target_b[13] = p.source_b[13] = 36.537342071533203;
  p.target_b[14] = p.source_b[14] = 32.930683135986328;
  p.target_b[15] = p.source_b[15] = 19.008804321289062;
  p.target_b[16] = p.source_b[16] = 32.259223937988281;
  p.target_b[17] = p.source_b[17] = 25.815582275390625;
  p.target_b[18] = p.source_b[18] = 26.509498596191406;
  p.target_b[19] = p.source_b[19] = 40.572704315185547;
  p.target_b[20] = p.source_b[20] = 88.354469299316406;
  p.target_b[21] = p.source_b[21] = 33.434604644775391;
  p.target_b[22] = p.source_b[22] = 9.5750093460083008;
  p.target_b[23] = p.source_b[23] = 41.285167694091797;
  dt_gui_presets_add_generic(_("it8 skin tones"), self->op, self->version(), &p, sizeof(p), 1);

  // helmholtz/kohlrausch effect applied to black and white conversion.
  // implemented by wmader as an iop and matched as a clut for increased
  // flexibility. this was done using darktable-chart and this is copied
  // from the resulting dtstyle output file:
  const char *hk_params_input =
    "9738b84231c098426fb8814234a82d422ac41d422e3fa04100004843f7daa24257e09a422a1a984225113842f89cc9410836ca4295049542ad1c9242887370427cb32b427c512242b5a40742545bd141808740412cc6964262e484429604c44100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000ef6d3bc152c2acc1ef6566c093a522c2e7d4e4c1a87c7cc100000000b4c4dd407af09e40d060df418afc7d421dadd0413ec5124097d79041fcba2642fc9f484183eb92415d6b7040fcdcdc41b8fe2f42b64a1740fc8612c1276defc144432ec100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000d237eb4022a72842f5639742396d1442a2660d411c338b40000000006e35ca408df2054289658d4132327a4118427741d4cf08c0f8a4d5c03abed7c13fac36c23b41a6c03c2230c07d5088c26caff7c1e0e9c6bff14ecec073b028c29e0accc10000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000085f2b642a4ba9a423c9a8442a6493c428baf28425667b64100004843a836a142a84e9b4226719d421cb15d424c22ee4175fcca4211ae96426e6d9a4243878142ef45354222f82542629527420280ff416c2066417e3996420d838e424182e3410000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000fa370000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000c8b700000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000004837000000000000c8b60000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000018000000";
  int params_len = 0;
  uint8_t *hk_params = dt_exif_xmp_decode(
      hk_params_input, strlen(hk_params_input), &params_len);
  assert(params_len == sizeof(dt_iop_colorchecker_params_t));
  assert(hk_params);
  dt_gui_presets_add_generic(_("helmholtz/kohlrausch monochrome"), self->op, self->version(), hk_params, params_len, 1);
  free(hk_params);
}

// fast logarithms stolen from paul mineiro http://fastapprox.googlecode.com/svn/trunk/fastapprox/src/fastonebigheader.h
#if 0//def __SSE2__
#include <xmmintrin.h>

typedef __m128 v4sf;
typedef __m128i v4si;

#define v4si_to_v4sf _mm_cvtepi32_ps
#define v4sf_to_v4si _mm_cvttps_epi32

#define v4sfl(x) ((const v4sf) { (x), (x), (x), (x) })
#define v2dil(x) ((const v4si) { (x), (x) })
#define v4sil(x) v2dil((((unsigned long long) (x)) << 32) | (x))
static inline v4sf
vfastlog2 (v4sf x)
{
  union { v4sf f; v4si i; } vx = { x };
  union { v4si i; v4sf f; } mx; mx.i = (vx.i & v4sil (0x007FFFFF)) | v4sil (0x3f000000);
  v4sf y = v4si_to_v4sf (vx.i);
  y *= v4sfl (1.1920928955078125e-7f);

  const v4sf c_124_22551499 = v4sfl (124.22551499f);
  const v4sf c_1_498030302 = v4sfl (1.498030302f);
  const v4sf c_1_725877999 = v4sfl (1.72587999f);
  const v4sf c_0_3520087068 = v4sfl (0.3520887068f);

  return y - c_124_22551499
    - c_1_498030302 * mx.f
    - c_1_725877999 / (c_0_3520087068 + mx.f);
}

static inline v4sf
vfastlog (v4sf x)
{
  const v4sf c_0_69314718 = v4sfl (0.69314718f);
  return c_0_69314718 * vfastlog2 (x);
}

// thinplate spline kernel \phi(r) = 2 r^2 ln(r)
static inline v4sf kerneldist4(const float *x, const float *y)
{
  const float r2 =
      (x[0]-y[0])*(x[0]-y[0])+
      (x[1]-y[1])*(x[1]-y[1])+
      (x[2]-y[2])*(x[2]-y[2]);
  return r2 * fastlog(MAX(1e-8f,r2));
}
#endif

static inline float
fastlog2 (float x)
{
  union { float f; uint32_t i; } vx = { x };
  union { uint32_t i; float f; } mx = { (vx.i & 0x007FFFFF) | 0x3f000000 };
  float y = vx.i;
  y *= 1.1920928955078125e-7f;

  return y - 124.22551499f
    - 1.498030302f * mx.f
    - 1.72587999f / (0.3520887068f + mx.f);
}

static inline float
fastlog (float x)
{
  return 0.69314718f * fastlog2 (x);
}

// static inline float
// fasterlog(float x)
// {
//   union { float f; uint32_t i; } vx = { x };
//   float y = vx.i;
//   y *= 8.2629582881927490e-8f;
//   return y - 87.989971088f;
// }

// thinplate spline kernel \phi(r) = 2 r^2 ln(r)
#if defined(_OPENMP) && defined(OPENMP_SIMD_)
#pragma omp declare SIMD()
#endif
static inline float kernel(const float *x, const float *y)
{
  // return r*r*logf(MAX(1e-8f,r));
  // well damnit, this speedup thing unfortunately shows severe artifacts.
  // return r*r*fasterlog(MAX(1e-8f,r));
  // this one seems to be a lot better, let's see how it goes:
  const float r2 =
      (x[0]-y[0])*(x[0]-y[0])+
      (x[1]-y[1])*(x[1]-y[1])+
      (x[2]-y[2])*(x[2]-y[2]);
  return r2*fastlog(MAX(1e-8f,r2));
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorchecker_data_t *const data = (dt_iop_colorchecker_data_t *)piece->data;
  const int ch = piece->colors;
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) collapse(2)
#endif
  for(int j=0;j<roi_out->height;j++)
  {
    for(int i=0;i<roi_out->width;i++)
    {
      const float *in = ((float *)ivoid) + (size_t)ch * (j * roi_in->width + i);
      float *out = ((float *)ovoid) + (size_t)ch * (j * roi_in->width + i);
      out[0] = data->coeff_L[data->num_patches];
      out[1] = data->coeff_a[data->num_patches];
      out[2] = data->coeff_b[data->num_patches];
      // polynomial part:
      out[0] += data->coeff_L[data->num_patches+1] * in[0] +
                data->coeff_L[data->num_patches+2] * in[1] +
                data->coeff_L[data->num_patches+3] * in[2];
      out[1] += data->coeff_a[data->num_patches+1] * in[0] +
                data->coeff_a[data->num_patches+2] * in[1] +
                data->coeff_a[data->num_patches+3] * in[2];
      out[2] += data->coeff_b[data->num_patches+1] * in[0] +
                data->coeff_b[data->num_patches+2] * in[1] +
                data->coeff_b[data->num_patches+3] * in[2];
#if defined(_OPENMP) && defined(OPENMP_SIMD_) // <== nice try, i don't think this does anything here
#pragma omp SIMD()
#endif
      for(int k=0;k<data->num_patches;k++)
      { // rbf from thin plate spline
        const float phi = kernel(in, data->source_Lab + 3*k);
        out[0] += data->coeff_L[k] * phi;
        out[1] += data->coeff_a[k] * phi;
        out[2] += data->coeff_b[k] * phi;
      }
    }
  }
  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

#if 0 // TODO:
void process_sse2(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorchecker_data_t *const data = (dt_iop_colorchecker_data_t *)piece->data;
  const int ch = piece->colors;
  // TODO: swizzle this so we can eval the distance of one point
  // TODO: to four patches at the same time
  v4sf source_Lab[data->num_patches];
  for(int i=0;i<data->num_patches;i++)
    source_Lab[i] = _mm_set_ps(1.0,
        data->source_Lab[3*i+0],
        data->source_Lab[3*i+1],
        data->source_Lab[3*i+2]);
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) collapse(2)
#endif
  for(int j=0;j<roi_out->height;j++)
  {
    for(int i=0;i<roi_out->width;i++)
    {
      const float *in = ((float *)ivoid) + (size_t)ch * (j * roi_in->width + i);
      float *out = ((float *)ovoid) + (size_t)ch * (j * roi_in->width + i);
      // TODO: do this part in SSE (maybe need to store coeff_L in _mm128 on data struct)
      out[0] = data->coeff_L[data->num_patches];
      out[1] = data->coeff_a[data->num_patches];
      out[2] = data->coeff_b[data->num_patches];
      // polynomial part:
      out[0] += data->coeff_L[data->num_patches+1] * in[0] +
                data->coeff_L[data->num_patches+2] * in[1] +
                data->coeff_L[data->num_patches+3] * in[2];
      out[1] += data->coeff_a[data->num_patches+1] * in[0] +
                data->coeff_a[data->num_patches+2] * in[1] +
                data->coeff_a[data->num_patches+3] * in[2];
      out[2] += data->coeff_b[data->num_patches+1] * in[0] +
                data->coeff_b[data->num_patches+2] * in[1] +
                data->coeff_b[data->num_patches+3] * in[2];
      for(int k=0;k<data->num_patches;k+=4)
      { // rbf from thin plate spline
        const v4sf phi = kerneldist4(in, source_Lab[k]);
        // TODO: add up 4x output channels
        out[0] += data->coeff_L[k] * phi[0];
        out[1] += data->coeff_a[k] * phi[0];
        out[2] += data->coeff_b[k] * phi[0];
      }
    }
  }
  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}
#endif

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_colorchecker_data_t *d = (dt_iop_colorchecker_data_t *)piece->data;
  dt_iop_colorchecker_global_data_t *gd = (dt_iop_colorchecker_global_data_t *)self->data;

  const int devid = piece->pipe->devid;
  const int width = roi_out->width;
  const int height = roi_out->height;
  const int num_patches = d->num_patches;

  cl_int err = -999;
  cl_mem dev_params = NULL;

  const size_t params_size = (size_t)(4 * (2 * num_patches + 4)) * sizeof(float);
  float *params = malloc(params_size);
  float *idx = params;

  // re-arrange data->source_Lab and data->coeff_{L,a,b} into float4
  for(int n = 0; n < num_patches; n++, idx += 4)
  {
    idx[0] = d->source_Lab[3 * n];
    idx[1] = d->source_Lab[3 * n + 1];
    idx[2] = d->source_Lab[3 * n + 2];
    idx[3] = 0.0f;
  }

  for(int n = 0; n < num_patches + 4; n++, idx += 4)
  {
    idx[0] = d->coeff_L[n];
    idx[1] = d->coeff_a[n];
    idx[2] = d->coeff_b[n];
    idx[3] = 0.0f;
  }

  dev_params = dt_opencl_copy_host_to_device_constant(devid, params_size, params);
  if(dev_params == NULL) goto error;

  size_t sizes[3] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorchecker, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorchecker, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorchecker, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorchecker, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorchecker, 4, sizeof(int), (void *)&num_patches);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorchecker, 5, sizeof(cl_mem), (void *)&dev_params);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_colorchecker, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_params);
  free(params);
  return TRUE;

error:
  free(params);
  dt_opencl_release_mem_object(dev_params);
  dt_print(DT_DEBUG_OPENCL, "[opencl_colorchecker] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif


void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)p1;
  dt_iop_colorchecker_data_t *d = (dt_iop_colorchecker_data_t *)piece->data;

  d->num_patches = MIN(MAX_PATCHES, p->num_patches);
  const int N = d->num_patches, N4 = N + 4;
  for(int k = 0; k < N; k++)
  {
    d->source_Lab[3*k+0] = p->source_L[k];
    d->source_Lab[3*k+1] = p->source_a[k];
    d->source_Lab[3*k+2] = p->source_b[k];
  }

  // initialize coefficients with default values that will be
  // used for N<=4 and if coefficient matrix A is singular
  for(int i=0;i<4+N;i++)
  {
    d->coeff_L[i] = 0;
    d->coeff_a[i] = 0;
    d->coeff_b[i] = 0;
  }
  d->coeff_L[N + 1] = 1;
  d->coeff_a[N + 2] = 1;
  d->coeff_b[N + 3] = 1;

  /*
      Following

      K. Anjyo, J. P. Lewis, and F. Pighin, "Scattered data
      interpolation for computer graphics," ACM SIGGRAPH 2014 Courses
      on - SIGGRAPH ’14, 2014.
      http://dx.doi.org/10.1145/2614028.2615425
      http://scribblethink.org/Courses/ScatteredInterpolation/scatteredinterpcoursenotes.pdf

      construct the system matrix and the vector of function values and
      solve the set of linear equations

      / R   P \  / c \   / f \
      |       |  |   | = |   |
      \ P^t 0 /  \ d /   \ 0 /

      for the coefficient vector (c d)^t.

      By design of the interpolation scheme the interpolation
      coefficients c for radial non-linear basis functions (the kernel)
      must always vanish for N<=4.  For N<4 the (N+4)x(N+4) coefficient
      matrix A is singular, the linear system has non-unique solutions.
      Thus the cases with N<=4 need special treatment, unique solutions
      are found by setting some of the unknown coefficients to zero and
      solving a smaller linear system.
  */
  switch(N)
  {
  case 0:
    break;
  case 1:
    // interpolation via constant function
    d->coeff_L[N + 1] = p->target_L[0] / p->source_L[0];
    d->coeff_a[N + 2] = p->target_a[0] / p->source_a[0];
    d->coeff_b[N + 3] = p->target_b[0] / p->source_b[0];
    break;
  case 2:
    // interpolation via single constant function and the linear
    // function of the corresponding color channel
    {
      double A[2 * 2] = { 1, p->source_L[0],
                          1, p->source_L[1] };
      double b[2] = { p->target_L[0], p->target_L[1] };
      if(!gauss_solve(A, b, 2)) break;
      d->coeff_L[N + 0] = b[0];
      d->coeff_L[N + 1] = b[1];
    }
    {
      double A[2 * 2] = { 1, p->source_a[0],
                          1, p->source_a[1] };
      double b[2] = { p->target_a[0], p->target_a[1] };
      if(!gauss_solve(A, b, 2)) break;
      d->coeff_a[N + 0] = b[0];
      d->coeff_a[N + 2] = b[1];
    }
    {
      double A[2 * 2] = { 1, p->source_b[0],
                          1, p->source_b[1] };
      double b[2] = { p->target_b[0], p->target_b[1] };
      if(!gauss_solve(A, b, 2)) break;
      d->coeff_b[N + 0] = b[0];
      d->coeff_b[N + 3] = b[1];
    }
    break;
  case 3:
    // interpolation via single constant function, the linear function
    // of the corresponding color channel and the linear functions
    // of the other two color channels having both the same weight
    {
      double A[3 * 3] = { 1, p->source_L[0], p->source_a[0] + p->source_b[0],
                          1, p->source_L[1], p->source_a[1] + p->source_b[1],
                          1, p->source_L[2], p->source_a[2] + p->source_b[2] };
      double b[3] = { p->target_L[0], p->target_L[1], p->target_L[2] };
      if(!gauss_solve(A, b, 3)) break;
      d->coeff_L[N + 0] = b[0];
      d->coeff_L[N + 1] = b[1];
      d->coeff_L[N + 2] = b[2];
      d->coeff_L[N + 3] = b[2];
    }
    {
      double A[3 * 3] = { 1, p->source_a[0], p->source_L[0] + p->source_b[0],
                          1, p->source_a[1], p->source_L[1] + p->source_b[1],
                          1, p->source_a[2], p->source_L[2] + p->source_b[2] };
      double b[3] = { p->target_a[0], p->target_a[1], p->target_a[2] };
      if(!gauss_solve(A, b, 3)) break;
      d->coeff_a[N + 0] = b[0];
      d->coeff_a[N + 1] = b[2];
      d->coeff_a[N + 2] = b[1];
      d->coeff_a[N + 3] = b[2];
    }
    {
      double A[3 * 3] = { 1, p->source_b[0], p->source_L[0] + p->source_a[0],
                          1, p->source_b[1], p->source_L[1] + p->source_a[1],
                          1, p->source_b[2], p->source_L[2] + p->source_a[2] };
      double b[3] = { p->target_b[0], p->target_b[1], p->target_b[2] };
      if(!gauss_solve(A, b, 3)) break;
      d->coeff_b[N + 0] = b[0];
      d->coeff_b[N + 1] = b[2];
      d->coeff_b[N + 2] = b[2];
      d->coeff_b[N + 3] = b[1];
    }
    break;
  case 4:
  {
    // interpolation via constant function and 3 linear functions
    double A[4 * 4] = { 1, p->source_L[0], p->source_a[0], p->source_b[0],
                        1, p->source_L[1], p->source_a[1], p->source_b[1],
                        1, p->source_L[2], p->source_a[2], p->source_b[2],
                        1, p->source_L[3], p->source_a[3], p->source_b[3] };
    int pivot[4];
    if(!gauss_make_triangular(A, pivot, 4)) break;
    {
      double b[4] = { p->target_L[0], p->target_L[1], p->target_L[2], p->target_L[3] };
      gauss_solve_triangular(A, pivot, b, 4);
      d->coeff_L[N + 0] = b[0];
      d->coeff_L[N + 1] = b[1];
      d->coeff_L[N + 2] = b[2];
      d->coeff_L[N + 3] = b[3];
    }
    {
      double b[4] = { p->target_a[0], p->target_a[1], p->target_a[2], p->target_a[3] };
      gauss_solve_triangular(A, pivot, b, 4);
      d->coeff_a[N + 0] = b[0];
      d->coeff_a[N + 1] = b[1];
      d->coeff_a[N + 2] = b[2];
      d->coeff_a[N + 3] = b[3];
    }
    {
      double b[4] = { p->target_b[0], p->target_b[1], p->target_b[2], p->target_b[3] };
      gauss_solve_triangular(A, pivot, b, 4);
      d->coeff_b[N + 0] = b[0];
      d->coeff_b[N + 1] = b[1];
      d->coeff_b[N + 2] = b[2];
      d->coeff_b[N + 3] = b[3];
    }
    break;
  }
  default:
  {
    // setup linear system of equations
    double *A = malloc(N4 * N4 * sizeof(*A));
    double *b = malloc(N4 * sizeof(*b));
    // coefficients from nonlinear radial kernel functions
    for(int j=0;j<N;j++)
      for(int i=j;i<N;i++)
        A[j*N4+i] = A[i*N4+j] = kernel(d->source_Lab+3*i, d->source_Lab+3*j);
    // coefficients from constant and linear functions
    for(int i=0;i<N;i++) A[i*N4+N+0] = A[(N+0)*N4+i] = 1;
    for(int i=0;i<N;i++) A[i*N4+N+1] = A[(N+1)*N4+i] = d->source_Lab[3*i+0];
    for(int i=0;i<N;i++) A[i*N4+N+2] = A[(N+2)*N4+i] = d->source_Lab[3*i+1];
    for(int i=0;i<N;i++) A[i*N4+N+3] = A[(N+3)*N4+i] = d->source_Lab[3*i+2];
    // lower-right zero block
    for(int j=N;j<N4;j++)
      for(int i=N;i<N4;i++)
        A[j*N4+i] = 0;
    // make coefficient matrix triangular
    int *pivot = malloc(N4 * sizeof(*pivot));
    if (gauss_make_triangular(A, pivot, N4))
    {
      // calculate coefficients for L channel
      for(int i=0;i<N;i++) b[i] = p->target_L[i];
      for(int i=N;i<N+4;i++) b[i] = 0;
      gauss_solve_triangular(A, pivot, b, N4);
      for(int i=0;i<N+4;i++) d->coeff_L[i] = b[i];
      // calculate coefficients for a channel
      for(int i=0;i<N;i++) b[i] = p->target_a[i];
      for(int i=N;i<N+4;i++) b[i] = 0;
      gauss_solve_triangular(A, pivot, b, N4);
      for(int i=0;i<N+4;i++) d->coeff_a[i] = b[i];
      // calculate coefficients for b channel
      for(int i=0;i<N;i++) b[i] = p->target_b[i];
      for(int i=N;i<N+4;i++) b[i] = 0;
      gauss_solve_triangular(A, pivot, b, N4);
      for(int i=0;i<N+4;i++) d->coeff_b[i] = b[i];
    }
    // free resources
    free(pivot);
    free(b);
    free(A);
  }
  }
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_colorchecker_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_reset(struct dt_iop_module_t *self)
{
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  dt_bauhaus_widget_set_quad_active(g->combobox_patch, 0);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)module->params;
  if(g->patch >= p->num_patches || g->patch < 0) return;
  if(dt_bauhaus_combobox_length(g->combobox_patch) != p->num_patches)
  {
    dt_bauhaus_combobox_clear(g->combobox_patch);
    char cboxentry[1024];
    for(int k=0;k<p->num_patches;k++)
    {
      snprintf(cboxentry, sizeof(cboxentry), _("patch #%d"), k);
      dt_bauhaus_combobox_add(g->combobox_patch, cboxentry);
    }
    if(p->num_patches <= 24)
      dtgtk_drawing_area_set_aspect_ratio(g->area, 2.0/3.0);
    else
      dtgtk_drawing_area_set_aspect_ratio(g->area, 1.0);
  }
  if(g->absolute_target)
  {
    dt_bauhaus_slider_set(g->scale_L, p->target_L[g->patch]);
    dt_bauhaus_slider_set(g->scale_a, p->target_a[g->patch]);
    dt_bauhaus_slider_set(g->scale_b, p->target_b[g->patch]);
    const float Cout = sqrtf(
        p->target_a[g->patch]*p->target_a[g->patch]+
        p->target_b[g->patch]*p->target_b[g->patch]);
    dt_bauhaus_slider_set(g->scale_C, Cout);
  }
  else
  {
    dt_bauhaus_slider_set(g->scale_L, p->target_L[g->patch] - p->source_L[g->patch]);
    dt_bauhaus_slider_set(g->scale_a, p->target_a[g->patch] - p->source_a[g->patch]);
    dt_bauhaus_slider_set(g->scale_b, p->target_b[g->patch] - p->source_b[g->patch]);
    const float Cin = sqrtf(
        p->source_a[g->patch]*p->source_a[g->patch] +
        p->source_b[g->patch]*p->source_b[g->patch]);
    const float Cout = sqrtf(
        p->target_a[g->patch]*p->target_a[g->patch]+
        p->target_b[g->patch]*p->target_b[g->patch]);
    dt_bauhaus_slider_set(g->scale_C, Cout-Cin);
  }
  gtk_widget_queue_draw(g->area);

  if (self->request_color_pick == DT_REQUEST_COLORPICK_OFF)
    dt_bauhaus_widget_set_quad_active(g->combobox_patch, 0);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_colorchecker_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_colorchecker_params_t));
  module->default_enabled = 0;
  module->priority = 399; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_colorchecker_params_t);
  module->gui_data = NULL;
  dt_iop_colorchecker_params_t tmp;
  tmp.num_patches = 24;
  for(int k=0;k<tmp.num_patches;k++) tmp.source_L[k] = colorchecker_Lab[3*k+0];
  for(int k=0;k<tmp.num_patches;k++) tmp.source_a[k] = colorchecker_Lab[3*k+1];
  for(int k=0;k<tmp.num_patches;k++) tmp.source_b[k] = colorchecker_Lab[3*k+2];
  for(int k=0;k<tmp.num_patches;k++) tmp.target_L[k] = colorchecker_Lab[3*k+0];
  for(int k=0;k<tmp.num_patches;k++) tmp.target_a[k] = colorchecker_Lab[3*k+1];
  for(int k=0;k<tmp.num_patches;k++) tmp.target_b[k] = colorchecker_Lab[3*k+2];
  memcpy(module->params, &tmp, sizeof(dt_iop_colorchecker_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_colorchecker_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void init_global(dt_iop_module_so_t *module)
{
  dt_iop_colorchecker_global_data_t *gd
      = (dt_iop_colorchecker_global_data_t *)malloc(sizeof(dt_iop_colorchecker_global_data_t));
  module->data = gd;

  const int program = 8; // extended.cl, from programs.conf
  gd->kernel_colorchecker = dt_opencl_create_kernel(program, "colorchecker");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_colorchecker_global_data_t *gd = (dt_iop_colorchecker_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_colorchecker);
  free(module->data);
  module->data = NULL;
}

static void picker_callback(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;

  if(self->request_color_pick != DT_REQUEST_COLORPICK_MODULE)
    self->request_color_pick = DT_REQUEST_COLORPICK_MODULE;
  else
    self->request_color_pick = DT_REQUEST_COLORPICK_OFF;

  dt_iop_request_focus(self);

  if(self->request_color_pick != DT_REQUEST_COLORPICK_OFF)
    dt_dev_reprocess_all(self->dev);
  else
    dt_control_queue_redraw();

  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
}

static void target_L_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)self->params;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  if(g->patch >= p->num_patches || g->patch < 0) return;
  if(g->absolute_target)
    p->target_L[g->patch] = dt_bauhaus_slider_get(slider);
  else
    p->target_L[g->patch] = p->source_L[g->patch] + dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void target_a_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)self->params;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  if(g->patch >= p->num_patches || g->patch < 0) return;
  if(g->absolute_target)
  {
    p->target_a[g->patch] = CLAMP(dt_bauhaus_slider_get(slider), -128.0, 128.0);
    const float Cout = sqrtf(
        p->target_a[g->patch]*p->target_a[g->patch]+
        p->target_b[g->patch]*p->target_b[g->patch]);
    const int reset = darktable.gui->reset;
    darktable.gui->reset = 1; // avoid history item
    dt_bauhaus_slider_set(g->scale_C, Cout);
    darktable.gui->reset = reset;
  }
  else
  {
    p->target_a[g->patch] = CLAMP(p->source_a[g->patch] + dt_bauhaus_slider_get(slider), -128.0, 128.0);
    const float Cin = sqrtf(
        p->source_a[g->patch]*p->source_a[g->patch] +
        p->source_b[g->patch]*p->source_b[g->patch]);
    const float Cout = sqrtf(
        p->target_a[g->patch]*p->target_a[g->patch]+
        p->target_b[g->patch]*p->target_b[g->patch]);
    const int reset = darktable.gui->reset;
    darktable.gui->reset = 1; // avoid history item
    dt_bauhaus_slider_set(g->scale_C, Cout-Cin);
    darktable.gui->reset = reset;
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void target_b_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)self->params;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  if(g->patch >= p->num_patches || g->patch < 0) return;
  if(g->absolute_target)
  {
    p->target_b[g->patch] = CLAMP(dt_bauhaus_slider_get(slider), -128.0, 128.0);
    const float Cout = sqrtf(
        p->target_a[g->patch]*p->target_a[g->patch]+
        p->target_b[g->patch]*p->target_b[g->patch]);
    const int reset = darktable.gui->reset;
    darktable.gui->reset = 1; // avoid history item
    dt_bauhaus_slider_set(g->scale_C, Cout);
    darktable.gui->reset = reset;
  }
  else
  {
    p->target_b[g->patch] = CLAMP(p->source_b[g->patch] + dt_bauhaus_slider_get(slider), -128.0, 128.0);
    const float Cin = sqrtf(
        p->source_a[g->patch]*p->source_a[g->patch] +
        p->source_b[g->patch]*p->source_b[g->patch]);
    const float Cout = sqrtf(
        p->target_a[g->patch]*p->target_a[g->patch]+
        p->target_b[g->patch]*p->target_b[g->patch]);
    const int reset = darktable.gui->reset;
    darktable.gui->reset = 1; // avoid history item
    dt_bauhaus_slider_set(g->scale_C, Cout-Cin);
    darktable.gui->reset = reset;
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void target_C_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)self->params;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  if(g->patch >= p->num_patches || g->patch < 0) return;
  const float Cin = sqrtf(
      p->source_a[g->patch]*p->source_a[g->patch] +
      p->source_b[g->patch]*p->source_b[g->patch]);
  const float Cout = MAX(1e-4f, sqrtf(
      p->target_a[g->patch]*p->target_a[g->patch]+
      p->target_b[g->patch]*p->target_b[g->patch]));

  if(g->absolute_target)
  {
    const float Cnew = CLAMP(dt_bauhaus_slider_get(slider), 0.01, 128.0);
    p->target_a[g->patch] = CLAMP(p->target_a[g->patch]*Cnew/Cout, -128.0, 128.0);
    p->target_b[g->patch] = CLAMP(p->target_b[g->patch]*Cnew/Cout, -128.0, 128.0);
    const int reset = darktable.gui->reset;
    darktable.gui->reset = 1; // avoid history item
    dt_bauhaus_slider_set(g->scale_a, p->target_a[g->patch]);
    dt_bauhaus_slider_set(g->scale_b, p->target_b[g->patch]);
    darktable.gui->reset = reset;
  }
  else
  {
    const float Cnew = CLAMP(Cin + dt_bauhaus_slider_get(slider), 0.01, 128.0);
    p->target_a[g->patch] = CLAMP(p->target_a[g->patch]*Cnew/Cout, -128.0, 128.0);
    p->target_b[g->patch] = CLAMP(p->target_b[g->patch]*Cnew/Cout, -128.0, 128.0);
    const int reset = darktable.gui->reset;
    darktable.gui->reset = 1; // avoid history item
    dt_bauhaus_slider_set(g->scale_a, p->target_a[g->patch] - p->source_a[g->patch]);
    dt_bauhaus_slider_set(g->scale_b, p->target_b[g->patch] - p->source_b[g->patch]);
    darktable.gui->reset = reset;
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void target_callback(GtkWidget *combo, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  g->absolute_target = dt_bauhaus_combobox_get(combo);
  // switch off colour picker, it'll interfere with other changes of the patch:
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  self->gui_update(self);
}

static void patch_callback(GtkWidget *combo, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  g->patch = dt_bauhaus_combobox_get(combo);
  // switch off colour picker, it'll interfere with other changes of the patch:
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  self->gui_update(self);
}

static gboolean checker_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)self->params;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  // clear bg
  cairo_set_source_rgb(cr, .2, .2, .2);
  cairo_paint(cr);

  const float *picked_mean = self->picked_color;
  int besti = 0, bestj = 0;
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
  int cells_x = 6, cells_y = 4;
  if(p->num_patches > 24)
  {
    cells_x = 7;
    cells_y = 7;
  }
  for(int j = 0; j < cells_y; j++)
  {
    for(int i = 0; i < cells_x; i++)
    {
      double rgb[3] = { 0.5, 0.5, 0.5 }; // Lab: rgb grey converted to Lab
      cmsCIELab Lab;
      const int patch = i + j*cells_x;
      if(patch >= p->num_patches) continue;
      Lab.L = p->source_L[patch];
      Lab.a = p->source_a[patch];
      Lab.b = p->source_b[patch];
      if((self->request_color_pick == DT_REQUEST_COLORPICK_MODULE)
         && ((picked_mean[0] - Lab.L) * (picked_mean[0] - Lab.L)
                 + (picked_mean[1] - Lab.a) * (picked_mean[1] - Lab.a)
                 + (picked_mean[2] - Lab.b) * (picked_mean[2] - Lab.b)
             < (picked_mean[0] - p->source_L[cells_x * bestj + besti])
                       * (picked_mean[0] - p->source_L[cells_x * bestj + besti])
                   + (picked_mean[1] - p->source_a[cells_x * bestj + besti])
                         * (picked_mean[1] - p->source_a[cells_x * bestj + besti])
                   + (picked_mean[2] - p->source_b[cells_x * bestj + besti])
                         * (picked_mean[2] - p->source_b[cells_x * bestj + besti])))
      {
        besti = i;
        bestj = j;
      }
      cmsDoTransform(g->xform, &Lab, rgb, 1);
      cairo_set_source_rgb(cr, rgb[0], rgb[1], rgb[2]);
      cairo_rectangle(cr, width * i / (float)cells_x, height * j / (float)cells_y,
          width / (float)cells_x - DT_PIXEL_APPLY_DPI(1),
          height / (float)cells_y - DT_PIXEL_APPLY_DPI(1));
      cairo_fill(cr);
      if(fabsf(p->target_L[patch] - p->source_L[patch]) > 1e-5f ||
         fabsf(p->target_a[patch] - p->source_a[patch]) > 1e-5f ||
         fabsf(p->target_b[patch] - p->source_b[patch]) > 1e-5f)
      {
        cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
        cairo_set_source_rgb(cr, 0.8, 0.8, 0.8);
        cairo_rectangle(cr,
            width * i / (float)cells_x + DT_PIXEL_APPLY_DPI(1),
            height * j / (float)cells_y + DT_PIXEL_APPLY_DPI(1),
            width / (float)cells_x - DT_PIXEL_APPLY_DPI(3),
            height / (float)cells_y - DT_PIXEL_APPLY_DPI(3));
        cairo_stroke(cr);
        cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
        cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
        cairo_rectangle(cr,
            width * i / (float)cells_x + DT_PIXEL_APPLY_DPI(2),
            height * j / (float)cells_y + DT_PIXEL_APPLY_DPI(2),
            width / (float)cells_x - DT_PIXEL_APPLY_DPI(5),
            height / (float)cells_y - DT_PIXEL_APPLY_DPI(5));
        cairo_stroke(cr);
      }
    }
  }

  dt_bauhaus_widget_set_quad_paint(
      g->combobox_patch, dtgtk_cairo_paint_colorpicker,
      (self->request_color_pick == DT_REQUEST_COLORPICK_MODULE ? CPF_ACTIVE : CPF_NONE), NULL);

  // highlight patch that is closest to picked colour,
  // or the one selected in the combobox.
  if(self->request_color_pick != DT_REQUEST_COLORPICK_MODULE)
  {
    int i = dt_bauhaus_combobox_get(g->combobox_patch);
    besti = i % cells_x;
    bestj = i / cells_x;
    g->drawn_patch = cells_x * bestj + besti;
  }
  else if(self->request_color_pick == DT_REQUEST_COLORPICK_MODULE)
  {
    // freshly picked, also select it in gui:
    int pick = self->request_color_pick;
    g->drawn_patch = cells_x * bestj + besti;
    darktable.gui->reset = 1;
    dt_bauhaus_combobox_set(g->combobox_patch, g->drawn_patch);
    g->patch = g->drawn_patch;
    self->gui_update(self);
    darktable.gui->reset = 0;
    self->request_color_pick = pick; // restore, the combobox will kill it
  }
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
  cairo_rectangle(cr,
      width * besti / (float)cells_x + DT_PIXEL_APPLY_DPI(5),
      height * bestj / (float)cells_y + DT_PIXEL_APPLY_DPI(5),
      width / (float)cells_x - DT_PIXEL_APPLY_DPI(11),
      height / (float)cells_y - DT_PIXEL_APPLY_DPI(11));
  cairo_stroke(cr);

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

static gboolean checker_motion_notify(GtkWidget *widget, GdkEventMotion *event,
    gpointer user_data)
{
  // highlight?
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)self->params;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  const float mouse_x = CLAMP(event->x, 0, width);
  const float mouse_y = CLAMP(event->y, 0, height);
  int cells_x = 6, cells_y = 4;
  if(p->num_patches > 24)
  {
    cells_x = 7;
    cells_y = 7;
  }
  const float mx = mouse_x * cells_x / (float)width;
  const float my = mouse_y * cells_y / (float)height;
  const int patch = (int)mx + cells_x * (int)my;
  if(patch < 0 || patch >= p->num_patches) return FALSE;
  char tooltip[1024];
  snprintf(tooltip, sizeof(tooltip),
      _("(%2.2f %2.2f %2.2f)\n"
        "altered patches are marked with an outline\n"
        "click to select\n"
        "double click to reset\n"
        "right click to delete patch\n"
        "shift-click while color picking to replace patch"),
      p->source_L[patch], p->source_a[patch], p->source_b[patch]);
  gtk_widget_set_tooltip_text(g->area, tooltip);
  return TRUE;
}

static gboolean checker_button_press(GtkWidget *widget, GdkEventButton *event,
                                                    gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)self->params;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  const float mouse_x = CLAMP(event->x, 0, width);
  const float mouse_y = CLAMP(event->y, 0, height);
  int cells_x = 6, cells_y = 4;
  if(p->num_patches > 24)
  {
    cells_x = 7;
    cells_y = 7;
  }
  const float mx = mouse_x * cells_x / (float)width;
  const float my = mouse_y * cells_y / (float)height;
  int patch = (int)mx + cells_x*(int)my;
  if(event->button == 1 && event->type == GDK_2BUTTON_PRESS)
  { // reset on double click
    if(patch < 0 || patch >= p->num_patches) return FALSE;
    p->target_L[patch] = p->source_L[patch];
    p->target_a[patch] = p->source_a[patch];
    p->target_b[patch] = p->source_b[patch];
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    self->gui_update(self);
    return TRUE;
  }
  else if(event->button == 3 && (patch < p->num_patches))
  {
    // right click: delete patch, move others up
    if(patch < 0 || patch >= p->num_patches) return FALSE;
    memmove(p->target_L+patch, p->target_L+patch+1, sizeof(float)*(p->num_patches-1-patch));
    memmove(p->target_a+patch, p->target_a+patch+1, sizeof(float)*(p->num_patches-1-patch));
    memmove(p->target_b+patch, p->target_b+patch+1, sizeof(float)*(p->num_patches-1-patch));
    memmove(p->source_L+patch, p->source_L+patch+1, sizeof(float)*(p->num_patches-1-patch));
    memmove(p->source_a+patch, p->source_a+patch+1, sizeof(float)*(p->num_patches-1-patch));
    memmove(p->source_b+patch, p->source_b+patch+1, sizeof(float)*(p->num_patches-1-patch));
    p->num_patches--;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    self->gui_update(self);
    return TRUE;
  }
  else if((event->button == 1) &&
          ((event->state & GDK_SHIFT_MASK) == GDK_SHIFT_MASK) &&
          (self->request_color_pick == DT_REQUEST_COLORPICK_MODULE))
  {
    // shift-left while colour picking: replace source colour
    // if clicked outside the valid patches: add new one

    // color channels should be nonzero to avoid numerical issues
    int new_color_valid = fabsf(self->picked_color[0]) > 1.e-3f &&
                          fabsf(self->picked_color[1]) > 1.e-3f &&
                          fabsf(self->picked_color[2]) > 1.e-3f;
    // check if the new color is very close to some color already in the colorchecker
    for(int i=0;i<p->num_patches;++i)
    {
      float color[] = { p->source_L[i], p->source_a[i], p->source_b[i] };
      if(fabsf(self->picked_color[0] - color[0]) < 1.e-3f && fabsf(self->picked_color[1] - color[1]) < 1.e-3f
         && fabsf(self->picked_color[2] - color[2]) < 1.e-3f)
        new_color_valid = FALSE;
    }
    if(new_color_valid)
    {
      if(p->num_patches < 24 && (patch < 0 || patch >= p->num_patches))
      {
        p->num_patches = MIN(MAX_PATCHES, p->num_patches + 1);
        patch = p->num_patches - 1;
      }
      p->target_L[patch] = p->source_L[patch] = self->picked_color[0];
      p->target_a[patch] = p->source_a[patch] = self->picked_color[1];
      p->target_b[patch] = p->source_b[patch] = self->picked_color[2];
      dt_dev_add_history_item(darktable.develop, self, TRUE);
      self->gui_update(self);
    }
    return TRUE;
  }
  if(patch >= p->num_patches) patch = p->num_patches-1;
  dt_bauhaus_combobox_set(g->combobox_patch, patch);
  return FALSE;
}

static gboolean checker_leave_notify(GtkWidget *widget, GdkEventCrossing *event,
                                                    gpointer user_data)
{
  return FALSE; // ?
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_colorchecker_gui_data_t));
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));

  // custom 24-patch widget in addition to combo box
  g->area = dtgtk_drawing_area_new_with_aspect_ratio(4.0/6.0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->area, TRUE, TRUE, 0);

  gtk_widget_add_events(GTK_WIDGET(g->area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                             | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                             | GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK);
  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(checker_draw), self);
  g_signal_connect(G_OBJECT(g->area), "button-press-event", G_CALLBACK(checker_button_press), self);
  g_signal_connect(G_OBJECT(g->area), "motion-notify-event", G_CALLBACK(checker_motion_notify), self);
  g_signal_connect(G_OBJECT(g->area), "leave-notify-event", G_CALLBACK(checker_leave_notify), self);

  g->patch = 0;
  g->drawn_patch = -1;
  g->combobox_patch = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->combobox_patch, NULL, _("patch"));
  gtk_widget_set_tooltip_text(g->combobox_patch, _("color checker patch"));
  char cboxentry[1024];
  for(int k=0;k<p->num_patches;k++)
  {
    snprintf(cboxentry, sizeof(cboxentry), _("patch #%d"), k);
    dt_bauhaus_combobox_add(g->combobox_patch, cboxentry);
  }
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  dt_bauhaus_widget_set_quad_paint(g->combobox_patch, dtgtk_cairo_paint_colorpicker, CPF_NONE, NULL);

  g->scale_L = dt_bauhaus_slider_new_with_range(self, -100.0, 200.0, 1.0, 0.0f, 2);
  gtk_widget_set_tooltip_text(g->scale_L, _("lightness offset"));
  dt_bauhaus_widget_set_label(g->scale_L, NULL, _("lightness"));

  g->scale_a = dt_bauhaus_slider_new_with_range(self, -256.0, 256.0, 1.0, 0.0f, 2);
  gtk_widget_set_tooltip_text(g->scale_a, _("chroma offset green/red"));
  dt_bauhaus_widget_set_label(g->scale_a, NULL, _("green/red"));
  dt_bauhaus_slider_set_stop(g->scale_a, 0.0, 0.0, 1.0, 0.2);
  dt_bauhaus_slider_set_stop(g->scale_a, 0.5, 1.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->scale_a, 1.0, 1.0, 0.0, 0.2);

  g->scale_b = dt_bauhaus_slider_new_with_range(self, -256.0, 256.0, 1.0, 0.0f, 2);
  gtk_widget_set_tooltip_text(g->scale_b, _("chroma offset blue/yellow"));
  dt_bauhaus_widget_set_label(g->scale_b, NULL, _("blue/yellow"));
  dt_bauhaus_slider_set_stop(g->scale_b, 0.0, 0.0, 0.0, 1.0);
  dt_bauhaus_slider_set_stop(g->scale_b, 0.5, 1.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->scale_b, 1.0, 1.0, 1.0, 0.0);

  g->scale_C = dt_bauhaus_slider_new_with_range(self, -128.0, 128.0, 1.0f, 0.0f, 2);
  gtk_widget_set_tooltip_text(g->scale_C, _("saturation offset"));
  dt_bauhaus_widget_set_label(g->scale_C, NULL, _("saturation"));

  g->absolute_target = 0;
  g->combobox_target = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->combobox_target, 0, _("target color"));
  gtk_widget_set_tooltip_text(g->combobox_target, _("control target color of the patches via relative offsets or via absolute Lab values"));
  dt_bauhaus_combobox_add(g->combobox_target, _("relative"));
  dt_bauhaus_combobox_add(g->combobox_target, _("absolute"));

  gtk_box_pack_start(GTK_BOX(self->widget), g->combobox_patch, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_L, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_a, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_b, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_C, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->combobox_target, TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(g->combobox_patch), "value-changed", G_CALLBACK(patch_callback), self);
  g_signal_connect(G_OBJECT(g->combobox_patch), "quad-pressed", G_CALLBACK(picker_callback), self);
  g_signal_connect(G_OBJECT(g->scale_L), "value-changed", G_CALLBACK(target_L_callback), self);
  g_signal_connect(G_OBJECT(g->scale_a), "value-changed", G_CALLBACK(target_a_callback), self);
  g_signal_connect(G_OBJECT(g->scale_b), "value-changed", G_CALLBACK(target_b_callback), self);
  g_signal_connect(G_OBJECT(g->scale_C), "value-changed", G_CALLBACK(target_C_callback), self);
  g_signal_connect(G_OBJECT(g->combobox_target), "value-changed", G_CALLBACK(target_callback), self);

  cmsHPROFILE hsRGB = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "", DT_PROFILE_DIRECTION_IN)->profile;
  cmsHPROFILE hLab = dt_colorspaces_get_profile(DT_COLORSPACE_LAB, "", DT_PROFILE_DIRECTION_ANY)->profile;
  g->xform = cmsCreateTransform(hLab, TYPE_Lab_DBL, hsRGB, TYPE_RGB_DBL, INTENT_PERCEPTUAL,
                                0); // cmsFLAGS_NOTPRECALC);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  cmsDeleteTransform(g->xform);
  free(self->gui_data);
  self->gui_data = NULL;
}

#undef MAX_PATCHES

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
