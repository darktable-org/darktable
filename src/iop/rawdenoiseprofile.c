/*
    This file is part of darktable,
    copyright (c) 2015 johannes hanika.

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
#include "develop/imageop.h"
#include "develop/tiling.h"
#include "bauhaus/bauhaus.h"
#include "control/control.h"
#include "common/noiseprofiles.h"
#include "common/opencl.h"
#include "gui/accelerators.h"
#include "gui/presets.h"
#include "gui/gtk.h"
#include "common/opencl.h"
#include <gtk/gtk.h>
#include <stdlib.h>
#include <xmmintrin.h>

// this is the version of the modules parameters,
// and includes version information about compile-time dt
DT_MODULE_INTROSPECTION(1, dt_iop_rawdenoiseprofile_params_t)

typedef struct dt_iop_rawdenoiseprofile_params_t
{
  // TODO: probably want something for every channel separately (or at least g vs rb)
  float strength;   // noise level after equalization
  float a, b, c;    // noise parameters

  // probably not here to stay, who knows:
  uint32_t mode;
  uint32_t algo;
} dt_iop_rawdenoiseprofile_params_t;

typedef struct dt_iop_rawdenoiseprofile_gui_data_t
{
  GtkWidget *profile;
  GtkWidget *mode;
  GtkWidget *algo;
  GtkWidget *strength;
  GtkWidget *a;
  GtkWidget *b;
  GtkWidget *c;
  // dt_noiseprofile_t interpolated; // don't use name, maker or model, they may point to garbage
  GList *profiles;

  // debug stuff:
  float stddev[512];
  float stddev_max;

} dt_iop_rawdenoiseprofile_gui_data_t;

typedef dt_iop_rawdenoiseprofile_params_t dt_iop_rawdenoiseprofile_data_t;

// static dt_noiseprofile_t dt_iop_rawdenoiseprofile_get_auto_profile(dt_iop_module_t *self);

const char *name()
{
  return _("raw denoise (profiled)");
}

int groups()
{
  return IOP_GROUP_CORRECT;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING;
}

#if 0 // i don't think we support blending for uint16_t:
int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_SUPPORTS_BLENDING;
}

int output_bpp(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return sizeof(uint16_t);
}
#endif

// very fast approximation for 2^-x (returns 0 for x > 126)
static inline float fast_mexp2f(const float x)
{
  typedef union floatint_t
  {
    float f;
    uint32_t i;
  } floatint_t;

  const float i1 = (float)0x3f800000u; // 2^0
  const float i2 = (float)0x3f000000u; // 2^-1
  const float k0 = i1 + x * (i2 - i1);
  floatint_t k;
  k.i = k0 >= (float)0x800000u ? k0 : 0;
  return k.f;
}

static inline float noise(const float level, const float black, const float white, const float a, const float b, const float c)
{
  const float v = MAX(1, level-black);
  return sqrtf(a*v + b*b + c*c*v*v);
}

#if 1 //def ANALYSE
static void analyse_g(
    const float *coarse,         // blurred buffer
    const uint16_t *const input, // const input buffer
    const int offx,              // select channel in bayer pattern
    const int32_t width,
    const int32_t height,
    const float black,
    const float white,
    const float aa,
    const float bb,
    const float cc,
    const uint32_t mode,
    dt_iop_rawdenoiseprofile_gui_data_t *g)
{
  // safety margin:
  const int mult = 32;
#if 0 // this has way too much variance, fits are pretty much arbitrary
  uint64_t cnt = 0;
  // const float sigma2 = (b/a)*(b/a);
  for(int j=2*mult;j<height-2*mult;j++) for(int i=((j&1)?1-offx:offx)+2*mult;i<width-2*mult;i+=2)
  {
    // const float inp = 2.0f * sqrtf(fmaxf(0.0f, input[width*j+i]/a + 3./8. + sigma2));
    const float inp = input[width*j+i];
    const float detail = inp - coarse[width*j+i];
    if(cnt++ % 1033 == 0)
      fprintf(stdout, "%g %g\n", coarse[width*j+i], fabsf(detail));
      // fprintf(stdout, "%u %g\n", input[width*j+i], fabsf(detail));
    // if(cnt++ > 10000) return;
  }
#endif
  // first bin into a couple brightness slots and average these:
  // use arithmetic mean to reduce memory cost :/
  const int N = 512;
  double sum[N], sum2[N];
  uint64_t num[N];
  memset(sum, 0, sizeof(double)*N);
  memset(sum2, 0, sizeof(double)*N);
  memset(num, 0, sizeof(uint64_t)*N);
  for(int j=2*mult;j<height-2*mult;j++) for(int i=((j&1)?1-offx:offx)+2*mult;i<width-2*mult;i+=2)
  {
    const float v = coarse[width*j+i];
#if 1
    uint16_t vb;
    float inp;
    inp = input[width*j+i];
    vb = v;
    const float d = fabsf(inp - v);
#endif
    // const float d = detail[width*j+i];
    // const int bin = CLAMP((v - black)/(white - black) * N, 0, N-1);
    // const float d = input[width*j+i];
    const int bin = CLAMP((vb - black)/(white - black) * N, 0, N-1);
    sum[bin] += d;
    sum2[bin] += d*d;
    num[bin]++;
  }
  if(g)
  {
    g->stddev_max = 0.0f;
    for(int k=0;k<N;k++)
    {
      // g->stddev[k] = sum[k] / (1.4826*num[k]);
      g->stddev[k] = sqrtf(sum2[k] / (num[k]+1.0f));
      // if(mode) g->stddev[k] *= aa;
      if(g->stddev[k] > g->stddev_max) g->stddev_max = g->stddev[k];
      // fprintf(stdout, "%g %g\n", b + (w-b)*k/(float)N, sum[k]/(1.4826*num[k]));
    }
  }
  // else
    // for(int k=0;k<N;k++)
      // fprintf(stdout, "%g %g\n", b + (w-b)*k/(float)N, (sum2[k] - sum[k]*sum[k]/N)/(N-1));
      // fprintf(stdout, "%g %g\n", b + (w-b)*k/(float)N, sum[k]/(1.4826*num[k]));
}

#if 0
static void analyse_rb(
    float *const coarse,         // blurred buffer
    const uint16_t *const input, // const input buffer
    const int offx,              // select channel in bayer pattern
    const int offy,              // select channel in bayer pattern
    const int32_t width,
    const int32_t height,
    const float black,
    const float white,
    const float a,
    const float b,
    const float c,
    const uint32_t mode)
{
#if 0
  const int mult = 32;
  uint64_t cnt = 0;
  // const float sigma2 = (b/a)*(b/a);
  for(int j=offy+2*mult;j<height-2*mult;j+=2) for(int i=offx+2*mult;i<width-2*mult;i+=2)
  {
    const float inp = input[width*j+i];
    // const float inp = 2.0f * sqrtf(fmaxf(0.0f, input[width*j+i]/a + 3./8. + sigma2));
    const float detail = inp - coarse[width*j+i];
    if(cnt++ % 1033 == 0)
      fprintf(stdout, "%g %g\n", coarse[width*j+i], fabsf(detail));
      // fprintf(stdout, "%u %g\n", input[width*j+i], fabsf(detail));
    // if(cnt++ > 10000) return;
  }
#endif
}
#endif
#endif

static inline float filter_eaw(
    const float v0,
    const float v1,
    const float b)
{
  // un-normalised ``gaussian'' for base 2, with sigma = 3 noise sigma
  return fast_mexp2f(.5f*(v1-v0)*(v1-v0)/(9.f*b*b));
}

static void decompose_eaw_g(
    float *const output,      // output buffer
    const float *const input, // const input buffer
    float *const detail,      // output detail buffer
    const int offx,           // select channel in bayer pattern
    const int scale,          // 0..max wavelet scale
    const float black,
    const float white,
    const float a,            // noise profile poissonian scale
    const float b,            // noise profile gaussian part
    const float c,
    const int32_t width,      // width of buffers
    const int32_t height,     // height of buffers (all three same size)
    const uint32_t mode)
{
  const int mult = 1 << scale;
  static const float filter[5] = { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };

  const float bs = b * powf(0.5f, scale);
  // TODO: borders!
  // non-separable, edge-avoiding, shift-independent a-trous wavelet transform.
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j=4*mult;j<height-4*mult;j++) for(int i=((j&1)?1-offx:offx)+4*mult;i<width-4*mult;i+=2)
  {
    output[j*width+i] = 0.0f;
    float sum = 0.0f;
    for(int jj=0;jj<5;jj++) for(int ii=0;ii<5;ii++)
    {
      const float v1 = input[(j + mult*(ii-2-jj+2))*width+i + mult*(ii-2+jj-2)];
      float w = filter[jj] * filter[ii] * filter_eaw(input[j*width+i], v1, bs);
      sum += w;
      output[j*width+i] += w * v1;
    }
    output[j*width+i] /= sum;
  }

  // final pass, write detail coeffs
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j=0;j<height;j++) for(int i=((j&1)?1-offx:offx);i<width;i+=2)
    if(mode == 1)
      // Fisz transform:
      detail[j*width+i] = (input[j*width+i] - output[j*width+i]) / noise(output[j*width+i], black, white, a, b, c);
    else
      detail[j*width+i] = input[j*width+i] - output[j*width+i];
}

static void decompose_eaw_rb(
    float *const output,      // output buffer
    const float *const input, // const input buffer
    float *const detail,      // output detail buffer
    const int offx,           // select channel in bayer pattern
    const int offy,
    const int scale,          // 0..max wavelet scale
    const float black,
    const float white,
    const float a,            // noise profile poissonian scale
    const float b,            // noise profile gaussian part
    const float c,
    const int32_t width,      // width of buffers
    const int32_t height,     // height of buffers (all three same size)
    const uint32_t mode)
{
  // jump scale+1 to jump over green channel at finest scale
  const int mult = 1 << (scale+1);
  static const float filter[5] = { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };

  // const float bs = b * powf(0.5f, scale);
  // TODO: borders!
  // non-separable, edge-avoiding, shift-independent a-trous wavelet transform.
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j=offy+2*mult;j<height-2*mult;j+=2) for(int i=offx+2*mult;i<width-2*mult;i+=2)
  {
    output[j*width+i] = 0.0f;
    // float sum = 0.0f;
    for(int jj=0;jj<5;jj++) for(int ii=0;ii<5;ii++)
    {
      const float v1 = input[(j + mult*(jj-2))*width+i + mult*(ii-2)];
      float w = filter[jj] * filter[ii];// * filter_eaw(input[j*width+i], v1, bs);
      // sum += w;
      output[j*width+i] += w * v1;
    }
    // output[j*width+i] /= sum;
  }

  // final pass, write detail coeffs
  // for(int j=offy;j<height;j+=2) for(int i=offx;i<width;i+=2)
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j=offy;j<height-2*mult;j+=2) for(int i=offx;i<width;i+=2)
    if(mode == 1)
      // Fisz transform:
      detail[j*width+i] = (input[j*width+i] - output[j*width+i]) / noise(output[j*width+i], black, white, a, b, c);
    else
      detail[j*width+i] = input[j*width+i] - output[j*width+i];
}

// separable
static void decompose_g(
    float *const output,      // output buffer
    const float *const input, // const input buffer
    float *const detail,      // output detail buffer
    const int offx,           // select channel in bayer pattern
    const int scale,          // 0..max wavelet scale
    const float black,
    const float white,
    const float a,            // noise profile poissonian scale
    const float b,            // noise profile gaussian part
    const float c,
    const int32_t width,      // width of buffers
    const int32_t height,     // height of buffers (all three same size)
    const uint32_t mode)
{
  const int mult = 1 << scale;
  static const float filter[5] = { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };

  // separable, data-independent, shift-independent a-trous wavelet transform.
  // split in H/V passes, for green bayer channel (diagonal)

  // H pass, or (+1, +1) really, write to detail buf (iterate over all green pixels only)
  // for(int j=0;j<height;j++) for(int i=(j&1)?1-offx:offx;i<width;i+=2)
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j=2*mult;j<height-2*mult;j++) for(int i=((j&1)?1-offx:offx)+2*mult;i<width-2*mult;i+=2)
  {
    detail[j*width+i] = 0.0f;
    for(int k=0;k<5;k++)
      detail[j*width+i] += filter[k] * input[(j + mult*(k-2))*width+i + mult*(k-2)];
  }

  // V pass, or (-1, +1), read detail write to output
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j=2*mult;j<height-2*mult;j++) for(int i=((j&1)?1-offx:offx)+2*mult;i<width-2*mult;i+=2)
  {
    output[j*width+i] = 0.0f;
    for(int k=0;k<5;k++)
      output[j*width+i] += filter[k] * detail[(j - mult*(k-2))*width+i + mult*(k-2)];
  }

  // final pass, write detail coeffs
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j=2*mult;j<height-2*mult;j++) for(int i=((j&1)?1-offx:offx)+2*mult;i<width-2*mult;i+=2)
    if(mode == 1)
      // Fisz transform:
      detail[j*width+i] = (input[j*width+i] - output[j*width+i]) / noise(output[j*width+i], black, white, a, b, c);
    else
      detail[j*width+i] = input[j*width+i] - output[j*width+i];
}

static void decompose_rb(
    float *const output,      // output buffer
    const float *const input, // const input buffer
    float *const detail,      // output detail buffer
    const int offx,           // select channel in bayer pattern
    const int offy,
    const int scale,          // 0..max wavelet scale
    const float black,
    const float white,
    const float a,            // noise profile poissonian scale
    const float b,            // noise profile gaussian part
    const float c,
    const int32_t width,      // width of buffers
    const int32_t height,     // height of buffers (all three same size)
    const uint32_t mode)
{
  // jump scale+1 to jump over green channel at finest scale
  const int mult = 1 << (scale+1);
  static const float filter[5] = { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };

  // separable, data-independent, shift-independent a-trous wavelet transform.
  // split in H/V passes, for bayer channels red and blue

  // H pass, write to detail buf
  // for(int j=offy;j<height;j+=2) for(int i=offx;i<width;i+=2)
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j=offy+2*mult;j<height-2*mult;j+=2) for(int i=offx+2*mult;i<width-2*mult;i+=2)
  {
    detail[j*width+i] = 0.0f;
    for(int k=0;k<5;k++)
      detail[j*width+i] += filter[k] * input[j*width+i + mult*(k-2)];
  }

  // V pass, read detail write to output
  // for(int j=offy;j<height;j+=2) for(int i=offx;i<width;i+=2)
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j=offy+2*mult;j<height-2*mult;j+=2) for(int i=offx+2*mult;i<width-2*mult;i+=2)
  {
    output[j*width+i] = 0.0f;
    for(int k=0;k<5;k++)
      output[j*width+i] += filter[k] * detail[(j + mult*(k-2))*width+i];
  }

  // final pass, write detail coeffs
  // for(int j=offy;j<height;j+=2) for(int i=offx;i<width;i+=2)
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j=offy+2*mult;j<height-2*mult;j+=2) for(int i=offx+2*mult;i<width-2*mult;i+=2)
    if(mode == 1)
      // Fisz transform:
      detail[j*width+i] = (input[j*width+i] - output[j*width+i]) / noise(output[j*width+i], black, white, a, b, c);
    else
      detail[j*width+i] = input[j*width+i] - output[j*width+i];
}

static int FC(const int row, const int col, const unsigned int filters)
{
  return filters >> (((row << 1 & 14) + (col & 1)) << 1) & 3;
}

static void synthesize(
    float *const out,
    const float *const in,
    const float *const detail,
    const float *const thrsf,
    const float *const boostf,
    const float black,
    const float white,
    const float a,
    const float b,
    const float c,
    const int32_t width,
    const int32_t height,
    const int32_t crop_x,
    const int32_t crop_y,
    const uint32_t filters,
    const uint32_t mode)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j = 0; j < height; j++)
  {
    const float *pin = in + (size_t)j * width;
    const float *pdetail = detail + (size_t)j * width;
    float *pout = out + (size_t)j * width;
    for(int i = 0; i < width; i++)
    {
      const int fc = FC(crop_y + j, crop_x + i, filters);
      assert(fc >= 0 && fc <= 2);
      // inverse Fisz transform:
      float d0 = *pdetail;
      if(mode == 1) d0 *= noise(*pin, black, white, a, b, c);
      // const float d0 = *pdetail;
      const float d = copysignf(MAX(fabsf(d0) - thrsf[fc], 0.0f), d0);
      *pout = *pin + boostf[fc] * d;
      pdetail++;
      pin++;
      pout++;
    }
  }
}

#define ELEM_SWAP(a,b) { uint16_t t=(a);(a)=(b);(b)=t; }
static inline uint16_t kth_smallest(uint16_t a[], int n, int k)
{
  int i,j,l,m ;
  uint16_t x ;

  l=0 ; m=n-1 ;
  while (l<m) {
    x=a[k] ;
    i=l ;
    j=m ;
    do {
      while (a[i]<x) i++ ;
      while (x<a[j]) j-- ;
      if (i<=j) {
        ELEM_SWAP(a[i],a[j]) ;
        i++ ; j-- ;
      }
    } while (i<=j) ;
    if (j<k) l=i ;
    if (k<i) m=j ;
  }
  return a[k] ;
}
#define median_n(a,n) kth_smallest(a,n,(((n)&1)?((n)/2):(((n)/2)-1)))
static inline uint16_t median5(uint16_t v1, uint16_t v2, uint16_t v3, uint16_t v4, uint16_t v5)
{
  uint16_t v[5] = {v1, v2, v3, v4, v5};
  return median_n(v, 5);
}
#undef median_n
#undef ELEM_SWAP

static inline void chop_outliers(
    uint16_t *const in,
    float    *const out,
    const int32_t black,
    const int32_t white,
    const int32_t width,
    const int32_t height,
    const int32_t offx)
{
  // TODO: fill borders!
  for(int j=2;j<height-2;j++)
  {
    // green:
    for(int i=2+((j&1)?1-offx:offx);i<width-2;i+=2)
    {
      const float v = in[j*width+i];
      const float m = median5(
        in[(j-1)*width+i-1],
        in[(j-1)*width+i+1],
        in[(j+1)*width+i-1],
        in[(j+1)*width+i+1],
        in[j*width+i]);
      if((fabsf(v-m)/(white-black) > 0.1) || // *(white-m)/white) ||
         (MAX(0, v-black)/(float)(white-black) < 0.05))// || // stuck black pixels
         // (v/(float)white > 0.50)) // stuck whites. doesn't work.
        out[j*width+i] = m;
      else out[j*width+i] = v;
    }

    // colour channels:
    for(int i=2+((j&1)?offx:1-offx);i<width-2;i+=2)
    {
      const float v = in[j*width+i];
#if 0
      const float m = median5(
        in[(j-1)*width+i],
        in[(j+1)*width+i],
        in[j*width+i-1],
        in[j*width+i+1],
        in[j*width+i]);
      if((fabsf(v-m)/(white-black) > 0.1) || // *(white-m)/white) ||
         (MAX(0, v-black)/(float)(white-black) < 0.05))// || // stuck black pixels
         // (v/(float)white > 0.50)) // stuck whites. doesn't work.
        out[j*width+i] = m;
      else
#endif
        out[j*width+i] = v;
      // decompose r-g g b-g:
      out[j*width+i] -= out[j*width + (i&~1) + ((j&1)?1-offx:offx)];
    }
  }
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid,
    const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  // now unfortunately we're supposed to work on 16-bit in- and output.

  const dt_image_t *img = &piece->pipe->image;
  // estimate gaussian (black sensor) noise:
  // TODO: alloc some memory and use MAD instead of empirical variance estimate here?
  dt_mipmap_buffer_t full;
  dt_mipmap_cache_get(darktable.mipmap_cache, &full, img->id, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');
  double x = 0.0, x2 = 0.0;
  uint64_t num = 0;
  for(int j=img->crop_y;j<full.height;j++)
  {
    for(int i=0;i<img->crop_x;i++)
    {
      const double v = ((uint16_t*)full.buf)[full.width*j + i];
      x += v;
      x2 += v*v;
      num++;
    }
  }
  dt_mipmap_cache_release(darktable.mipmap_cache, &full);
  const float black = img->raw_black_level;
  const float e_black = x / num;
  const float black_s = sqrtf(x2/num - e_black*e_black);
  // fprintf(stderr, "estimated black %g/%d s %g num %lu\n", e_black, self->dev->image_storage.raw_black_level, black_s, num);

  // this is called for preview and full pipe separately, each with its own pixelpipe piece.
  // get our data struct:
  dt_iop_rawdenoiseprofile_params_t *d = (dt_iop_rawdenoiseprofile_params_t *)piece->data;

  const int max_max_scale = 5; // hard limit
  int max_scale = 4;
  const float scale = roi_in->scale / piece->iscale;
#if 0
  // largest desired filter on input buffer (20% of input dim)
  const float supp0
      = MIN(2 * (2 << (max_max_scale - 1)) + 1,
            MAX(piece->buf_in.height * piece->iscale, piece->buf_in.width * piece->iscale) * 0.2f);
  const float i0 = dt_log2f((supp0 - 1.0f) * .5f);
  for(; max_scale < max_max_scale; max_scale++)
  {
    // actual filter support on scaled buffer
    const float supp = 2 * (2 << max_scale) + 1;
    // approximates this filter size on unscaled input image:
    const float supp_in = supp * (1.0f / scale);
    const float i_in = dt_log2f((supp_in - 1) * .5f) - 1.0f;
    // i_in = max_scale .. .. .. 0
    const float t = 1.0f - (i_in + .5f) / i0;
    if(t < 0.0f) break;
  }
#endif

  const int width = roi_in->width, height = roi_in->height;
  const size_t npixels = (size_t)width*height;

  // corner case of extremely small image. this is not really likely to happen but would cause issues later
  // when we divide by (n-1). so let's be prepared
  if(npixels < 2 || fabsf(scale - 1.0f) > 1e-4f)
  {
    memcpy(ovoid, ivoid, npixels * sizeof(uint16_t));
    return;
  }

  // find offset to beginning of gccg quad
  const int filters = dt_image_filter(img);
  int offx = 0;
  if(FC(img->crop_y, img->crop_x, filters) == 0) offx = 1;
  if(FC(img->crop_y, img->crop_x, filters) == 2) offx = 1;

  float *buf[max_max_scale];
  float *tmp1 = 0, *tmp2 = 0;
  float *buf1 = 0, *buf2 = 0;
  for(int k = 0; k < max_scale; k++)
    buf[k] = dt_alloc_align(64, sizeof(float) * npixels);
  tmp1 = dt_alloc_align(64, sizeof(float) * npixels);
  tmp2 = dt_alloc_align(64, sizeof(float) * npixels);

  // noise std dev ~= sqrt(b + a*input)
  // lacks pixel non-uniformity that kicks in for large values (do we care?)
  const float a = d->a, b = black_s, c = d->c;
  const float white = img->raw_white_point;

  chop_outliers((uint16_t *)ivoid, tmp1, black, white, width, height, offx);

  buf1 = tmp1;
  buf2 = tmp2;
memset(tmp2, 0, sizeof(float)*width*height);

#if 1
  for(int scale = 0; scale < max_scale; scale++)
  {
    if(d->algo == 0)
    {
      decompose_g (buf2, buf1, buf[scale],   offx,    scale, black, white, a, b, c, width, height, d->mode);  // green
      decompose_rb(buf2, buf1, buf[scale], 1-offx, 0, scale, black, white, a, b, c, width, height, d->mode);  // blue
      decompose_rb(buf2, buf1, buf[scale],   offx, 1, scale, black, white, a, b, c, width, height, d->mode);  // red
    }
    else
    {
      decompose_eaw_g (buf2, buf1, buf[scale],   offx,    scale, black, white, a, b, c, width, height, d->mode);  // green
      decompose_eaw_rb(buf2, buf1, buf[scale], 1-offx, 0, scale, black, white, a, b, c, width, height, d->mode);  // blue
      decompose_eaw_rb(buf2, buf1, buf[scale],   offx, 1, scale, black, white, a, b, c, width, height, d->mode);  // red
    }
// DEBUG: clean out temporary memory:
memset(buf1, 0, sizeof(float)*width*height);
    float *buf3 = buf2;
    buf2 = buf1;
    buf1 = buf3;
  }
#endif
  // DEBUG show coarsest buffer in output:
  // backtransform(buf1, (uint16_t *)ovoid, ref, width, height, a, b, d->mode);
#if 1//def ANALYSE
  dt_iop_rawdenoiseprofile_gui_data_t *g = (dt_iop_rawdenoiseprofile_gui_data_t *)self->gui_data;
  analyse_g (buf1, ivoid,   offx,    width, height, black, white, a, b, c, d->mode, g);
  // analyse_rb(buf1, ivoid, 1-offx, 0, width, height, black, white, a, b, c, d->mode);
  // analyse_rb(buf1, ivoid,   offx, 1, width, height, black, white, a, b, c, d->mode);
#endif

#if 1
  // now do everything backwards, so the result will end up in *ovoid
  for(int scale = max_scale - 1; scale >= 0; scale--)
  {
#if 1
    // TODO: separately per channel?
    // variance stabilizing transform maps sigma to unity.
    const float sigma = 1.0f;
    // it is then transformed by wavelet scales via the 5 tap a-trous filter:
    const float varf = sqrtf(2.0f + 2.0f * 4.0f * 4.0f + 6.0f * 6.0f) / 16.0f; // about 0.5
    const float sigma_band = powf(varf, scale) * sigma;
    // determine thrs as bayesshrink
    // TODO: parallelize!
    float sum_y2[3] = {0.0f};
    uint64_t cnt[3] = {0};
    for(int j=64;j<height-64;j++)
    {
      for(int i=64;i<width-64;i++)
      {
        const uint64_t k = j*width+i;
        // just add crop y and x, it's only about odd/even and we want to avoid negative numbers
        const int c = FC(img->crop_y + roi_in->y + j, img->crop_x + roi_in->x + i, filters);
        sum_y2[c] += buf[scale][k] * buf[scale][k];
        cnt[c]++;
      }
    }

    const float sb2 = sigma_band * sigma_band;
    float var_y[3] = {0.0f}, s_x[3] = {0.0f}, thrs[3] = {0.0f};
    for(int k=0;k<3;k++)
    {
      float nv = sb2 * d->strength * d->strength;
      // need this? also the spatial pattern is doubled (bayer)
      // if(k!=1) nv *= 2; // noise variance in colour channels is doubled (r-g, b-g)
      var_y[k] = sum_y2[k] / (cnt[k] - 1.0f);     // noisy signal variance
      s_x[k] = sqrtf(MAX(1e-12f, var_y[k] - nv)); // signal std deviation
      thrs[k] = nv / s_x[k];
      fprintf(stderr, "???[%d] y2 %g cnt %lu vy %g sx %g t %g\n", k, sum_y2[k], cnt[k], var_y[k], s_x[k], thrs[k]);
    }
    fprintf(stderr, "scale %d thrs %f %f %f = %f / %f\n", scale, thrs[0], thrs[1], thrs[2], sb2, s_x[0]);
    if(d->mode == 0) thrs[0] = thrs[1] = thrs[2] = 0.0f;
#else
    // const float thrs[3] = { 0.0, 0.0, 0.0 };
    const float t = powf(0.5f, scale) * d->strength * 10.0f;
    const float thrs[3] = { 2.0*t, t, 2.0*t };
    // const float thrs[3] = { 0, t, 0 };
    // fprintf(stderr, "scale %d thrs %g = .5 ^ %d\n", scale, t, scale);
#endif
    const float boost[3] = { 1.0f, 1.0f, 1.0f };
    // const float boost[3] = { 0.0f, 0.0f, 0.0f };
    synthesize(buf2, buf1, buf[scale], thrs, boost, black, white, a, b, c,
        width, height, img->crop_x + roi_in->x, img->crop_y + roi_in->y, filters, d->mode);
    // DEBUG: clean out temporary memory:
    // memset(buf1, 0, sizeof(float)*4*width*height);

    float *buf3 = buf2;
    buf2 = buf1;
    buf1 = buf3;
  }
#endif

#if 1 // recompose from r-g, g, b-g
  for(int j=0;j<height;j++)
  {
    for(int i=(j&1)?1-offx:offx;i<width;i+=2) 
      ((uint16_t *)ovoid)[j*width+i] = CLAMP(
        buf1[j*width+i],
        0, 0xffff); // copy green
    for(int i=(j&1)?offx:1-offx;i<width;i+=2)
      ((uint16_t *)ovoid)[j*width+i] = CLAMP(
        buf1[j*width+i] + buf1[j*width + (i&~1) + ((j&1)?1-offx:offx)],
        0, 0xffff);
  }
#else
  for(size_t k=0;k<npixels;k++) ((uint16_t*)ovoid)[k] = CLAMP(buf1[k], 0, 0xffff);
#endif

  for(int k = 0; k < max_scale; k++) dt_free_align(buf[k]);
  dt_free_align(tmp1);
  dt_free_align(tmp2);
}


/** this will be called to init new defaults if a new image is loaded from film strip mode. */
void reload_defaults(dt_iop_module_t *module)
{
  // our module is disabled by default
  module->default_enabled = 0;
  dt_iop_rawdenoiseprofile_params_t *p = (dt_iop_rawdenoiseprofile_params_t *)module->default_params;
  p->strength = 1.0f;
  p->a = 1.0f;
  p->b = 0.0f;
  p->c = 0.0f;
  p->mode = 0;
  p->algo = 0;
#if 0
  dt_iop_rawdenoiseprofile_gui_data_t *g = (dt_iop_rawdenoiseprofile_gui_data_t *)module->gui_data;
  if(g)
  {
    dt_bauhaus_combobox_clear(g->profile);

    // get matching profiles:
    char name[512];
    if(g->profiles) g_list_free_full(g->profiles, dt_noiseprofile_free);
    g->profiles = dt_noiseprofile_get_matching(&module->dev->image_storage);
    g->interpolated = dt_noiseprofile_generic; // default to generic poissonian
    g_strlcpy(name, _(g->interpolated.name), sizeof(name));

    const int iso = module->dev->image_storage.exif_iso;
    dt_noiseprofile_t *last = NULL;
    for(GList *iter = g->profiles; iter; iter = g_list_next(iter))
    {
      dt_noiseprofile_t *current = (dt_noiseprofile_t *)iter->data;

      if(current->iso == iso)
      {
        g->interpolated = *current;
        // signal later autodetection in commit_params:
        g->interpolated.a = -1.0;
        snprintf(name, sizeof(name), _("found match for ISO %d"), iso);
        break;
      }
      if(last && last->iso < iso && current->iso > iso)
      {
        dt_noiseprofile_interpolate(last, current, &g->interpolated);
        // signal later autodetection in commit_params:
        g->interpolated.a = -1.0;
        snprintf(name, sizeof(name), _("interpolated from ISO %d and %d"), last->iso, current->iso);
        break;
      }
      last = current;
    }

    dt_bauhaus_combobox_add(g->profile, name);
    for(GList *iter = g->profiles; iter; iter = g_list_next(iter))
    {
      dt_noiseprofile_t *profile = (dt_noiseprofile_t *)iter->data;
      dt_bauhaus_combobox_add(g->profile, profile->name);
    }

    ((dt_iop_rawdenoiseprofile_params_t *)module->default_params)->strength = 1.0f;
    ((dt_iop_rawdenoiseprofile_params_t *)module->default_params)->a = g->interpolated.a;
    ((dt_iop_rawdenoiseprofile_params_t *)module->default_params)->b = g->interpolated.b;
    memcpy(module->params, module->default_params, sizeof(dt_iop_rawdenoiseprofile_params_t));
  }
#endif
}

/** init, cleanup, commit to pipeline */
void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_rawdenoiseprofile_params_t));
  module->default_params = malloc(sizeof(dt_iop_rawdenoiseprofile_params_t));
  module->priority = 5; // DEBUG FIXME module order hardcoded for debugging (<10 which is rawprepare)
  module->params_size = sizeof(dt_iop_rawdenoiseprofile_params_t);
  module->gui_data = NULL;
  module->data = NULL;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL; // just to be sure
  free(module->params);
  module->params = NULL;
}

#if 0
static dt_noiseprofile_t dt_iop_rawdenoiseprofile_get_auto_profile(dt_iop_module_t *self)
{
  GList *profiles = dt_noiseprofile_get_matching(&self->dev->image_storage);
  dt_noiseprofile_t interpolated = dt_noiseprofile_generic; // default to generic poissonian

  const int iso = self->dev->image_storage.exif_iso;
  dt_noiseprofile_t *last = NULL;
  for(GList *iter = profiles; iter; iter = g_list_next(iter))
  {
    dt_noiseprofile_t *current = (dt_noiseprofile_t *)iter->data;
    if(current->iso == iso)
    {
      interpolated = *current;
      break;
    }
    if(last && last->iso < iso && current->iso > iso)
    {
      dt_noiseprofile_interpolate(last, current, &interpolated);
      break;
    }
    last = current;
  }
  g_list_free_full(profiles, dt_noiseprofile_free);
  return interpolated;
}
#endif

/** commit is the synch point between core and gui, so it copies params to pipe data. */
void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_rawdenoiseprofile_params_t *p = (dt_iop_rawdenoiseprofile_params_t *)params;
  dt_iop_rawdenoiseprofile_data_t *d = (dt_iop_rawdenoiseprofile_data_t *)piece->data;

  if(pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
  { // disable for preview pipeline
    piece->enabled = 0;
    return;
  }

  // copy everything first and make some changes later
  memcpy(d, p, sizeof(*d));

#if 0
  // compare if a[0] in params is set to "magic value" -1.0 for autodetection
  if(p->a[0] == -1.0)
  {
    // autodetect matching profile again, the same way as detecting their names,
    // this is partially duplicated code and data because we are not allowed to access
    // gui_data here ..
    dt_noiseprofile_t interpolated = dt_iop_rawdenoiseprofile_get_auto_profile(self);
    for(int k = 0; k < 3; k++)
    {
      d->a[k] = interpolated.a[k];
      d->b[k] = interpolated.b[k];
    }
  }
#endif
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_rawdenoiseprofile_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

static void mode_callback(GtkWidget *w, dt_iop_module_t *self)
{
  int i = dt_bauhaus_combobox_get(w);
  dt_iop_rawdenoiseprofile_params_t *p = (dt_iop_rawdenoiseprofile_params_t *)self->params;
  p->mode = i;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void algo_callback(GtkWidget *w, dt_iop_module_t *self)
{
  int i = dt_bauhaus_combobox_get(w);
  dt_iop_rawdenoiseprofile_params_t *p = (dt_iop_rawdenoiseprofile_params_t *)self->params;
  p->algo = i;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void profile_callback(GtkWidget *w, dt_iop_module_t *self)
{
#if 0 // FIXME: these are the wrong profiles
  int i = dt_bauhaus_combobox_get(w);
  dt_iop_rawdenoiseprofile_params_t *p = (dt_iop_rawdenoiseprofile_params_t *)self->params;
  dt_iop_rawdenoiseprofile_gui_data_t *g = (dt_iop_rawdenoiseprofile_gui_data_t *)self->gui_data;
  const dt_noiseprofile_t *profile = &(g->interpolated);
  if(i > 0) profile = (dt_noiseprofile_t *)g_list_nth_data(g->profiles, i - 1);
  for(int k = 0; k < 3; k++)
  {
    p->a[k] = profile->a[k];
    p->b[k] = profile->b[k];
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
#endif
}

static void a_callback(GtkWidget *w, dt_iop_module_t *self)
{
  dt_iop_rawdenoiseprofile_params_t *p = (dt_iop_rawdenoiseprofile_params_t *)self->params;
  p->a = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void b_callback(GtkWidget *w, dt_iop_module_t *self)
{
  dt_iop_rawdenoiseprofile_params_t *p = (dt_iop_rawdenoiseprofile_params_t *)self->params;
  // const float b = darktable.develop->image_storage.raw_black_level;
  // p->b = dt_bauhaus_slider_get(w) - b * p->a;
  p->b = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void c_callback(GtkWidget *w, dt_iop_module_t *self)
{
  dt_iop_rawdenoiseprofile_params_t *p = (dt_iop_rawdenoiseprofile_params_t *)self->params;
  p->c = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void strength_callback(GtkWidget *w, dt_iop_module_t *self)
{
  dt_iop_rawdenoiseprofile_params_t *p = (dt_iop_rawdenoiseprofile_params_t *)self->params;
  p->strength = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_update(dt_iop_module_t *self)
{
  // let gui slider match current parameters:
  dt_iop_rawdenoiseprofile_gui_data_t *g = (dt_iop_rawdenoiseprofile_gui_data_t *)self->gui_data;
  dt_iop_rawdenoiseprofile_params_t *p = (dt_iop_rawdenoiseprofile_params_t *)self->params;
  dt_bauhaus_slider_set(g->strength, p->strength);
  dt_bauhaus_slider_set(g->a, p->a);
  // const float b = darktable.develop->image_storage.raw_black_level;
  // dt_bauhaus_slider_set(g->b, p->b + b*p->a);
  dt_bauhaus_slider_set(g->b, p->b);
  dt_bauhaus_slider_set(g->c, p->c);
  dt_bauhaus_combobox_set(g->profile, -1);
  dt_bauhaus_combobox_set(g->mode, p->mode);
  dt_bauhaus_combobox_set(g->algo, p->algo);
#if 0
  if(p->a == -1.0)
  {
    dt_bauhaus_combobox_set(g->profile, 0);
  }
  else
  {
    int i = 1;
    for(GList *iter = g->profiles; iter; iter = g_list_next(iter), i++)
    {
      dt_noiseprofile_t *profile = (dt_noiseprofile_t *)iter->data;
      if(!memcmp(profile->a, p->a, sizeof(float) * 3)
         && !memcmp(profile->b, p->b, sizeof(float) * 3))
      {
        dt_bauhaus_combobox_set(g->profile, i);
        break;
      }
    }
  }
#endif
}

void gui_init(dt_iop_module_t *self)
{
  // init the slider (more sophisticated layouts are possible with gtk tables and boxes):
  self->gui_data = malloc(sizeof(dt_iop_rawdenoiseprofile_gui_data_t));
  dt_iop_rawdenoiseprofile_gui_data_t *g = (dt_iop_rawdenoiseprofile_gui_data_t *)self->gui_data;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  g->profiles = NULL;
  g->profile = dt_bauhaus_combobox_new(self);
  g->mode = dt_bauhaus_combobox_new(self);
  g->algo = dt_bauhaus_combobox_new(self);
  g->strength = dt_bauhaus_slider_new_with_range(self, 0.0f, 10.0f, .05, 1.f, 3);
  g->a = dt_bauhaus_slider_new_with_range(self, 0.0f, 200.0f, .05, 1.f, 4);
  g->b = dt_bauhaus_slider_new_with_range(self, 0.0f, 2000.0f, .05, 0.f, 3);
  g->c = dt_bauhaus_slider_new_with_range(self, 0.0f, 1.f, .00001, 0.f, 8);
  gtk_box_pack_start(GTK_BOX(self->widget), g->profile, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->mode, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->algo, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->strength, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->a, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->b, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->c, TRUE, TRUE, 0);
  dt_bauhaus_widget_set_label(g->profile, NULL, _("profile"));
  dt_bauhaus_widget_set_label(g->mode, NULL, _("mode"));
  dt_bauhaus_widget_set_label(g->algo, NULL, _("algorithm"));
  dt_bauhaus_widget_set_label(g->strength, NULL, _("strength"));
  dt_bauhaus_widget_set_label(g->a, NULL, _("shot / poissonian (a)"));
  dt_bauhaus_widget_set_label(g->b, NULL, _("sensor / gaussian (b)"));
  dt_bauhaus_widget_set_label(g->c, NULL, _("pixel non-uniformity (p)"));
  g_object_set(G_OBJECT(g->profile), "tooltip-text", _("profile used for variance stabilization"),
               (char *)NULL);
  g_object_set(G_OBJECT(g->strength), "tooltip-text", _("finetune denoising strength"), (char *)NULL);
  dt_bauhaus_combobox_add(g->mode, _("analyze"));
  dt_bauhaus_combobox_add(g->mode, _("denoise"));
  dt_bauhaus_combobox_add(g->algo, _("a-trous"));
  dt_bauhaus_combobox_add(g->algo, _("edge-aware"));
  g_signal_connect(G_OBJECT(g->profile), "value-changed", G_CALLBACK(profile_callback), self);
  g_signal_connect(G_OBJECT(g->mode), "value-changed", G_CALLBACK(mode_callback), self);
  g_signal_connect(G_OBJECT(g->algo), "value-changed", G_CALLBACK(algo_callback), self);
  g_signal_connect(G_OBJECT(g->strength), "value-changed", G_CALLBACK(strength_callback), self);
  g_signal_connect(G_OBJECT(g->a), "value-changed", G_CALLBACK(a_callback), self);
  g_signal_connect(G_OBJECT(g->b), "value-changed", G_CALLBACK(b_callback), self);
  g_signal_connect(G_OBJECT(g->c), "value-changed", G_CALLBACK(c_callback), self);
}

void gui_cleanup(dt_iop_module_t *self)
{
  dt_iop_rawdenoiseprofile_gui_data_t *g = (dt_iop_rawdenoiseprofile_gui_data_t *)self->gui_data;
  g_list_free_full(g->profiles, dt_noiseprofile_free);
  // nothing else necessary, gtk will clean up the slider.
  free(self->gui_data);
  self->gui_data = NULL;
}

void gui_post_expose(struct dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height,
                     int32_t pointerx, int32_t pointery)
{
  dt_iop_rawdenoiseprofile_gui_data_t *g = (dt_iop_rawdenoiseprofile_gui_data_t *)self->gui_data;
  dt_iop_rawdenoiseprofile_params_t *p = (dt_iop_rawdenoiseprofile_params_t *)self->params;

  if(p->mode != 0) return; // actually the stabilised variance is not easy to plot with a Fisz transform.

  const float b = darktable.develop->image_storage.raw_black_level;
  const float w = darktable.develop->image_storage.raw_white_point;
  // cairo_save(cr); // XXX save restore destroy the image, wtf.
  const float sx = width/w, sy = -height/(p->mode?3.0:g->stddev_max);
  // cairo_translate(cr, 0.0, height);
  // if(p->mode == 0)
    // cairo_scale(cr, width/w, -height/g->stddev_max);
  // else
    // cairo_scale(cr, width/w, -height/3.0);
  cairo_set_source_rgb(cr, .7, .7, .7);
  cairo_move_to(cr, b*sx, 0.0*sy+height);
  fprintf(stderr, "noise 0 %g\n", g->stddev[0]);
  for(int k=0;k<512;k++)
    if(g->stddev[k] == g->stddev[k])
      cairo_line_to(cr, (b + k/512.0*(w-b))*sx, g->stddev[k]*sy + height);
  // cairo_restore(cr);
  // cairo_set_line_width(cr, 2.0/height*g->stddev_max);
  cairo_set_line_width(cr, 2.0);
  cairo_stroke(cr);

  if(p->mode == 0)
  { // analysis stage, draw noise fit
    // cairo_save(cr);
    // cairo_translate(cr, 0.0, height);
    // cairo_scale(cr, width/w, -height/g->stddev_max);
    fprintf(stderr, "noise() 0 %g\n", noise(b, b, w, p->a, p->b, p->c));
    cairo_set_source_rgb(cr, .1, .7, .1);
    cairo_move_to(cr, b*sx, 0.0*sy+height);
    for(int k=0;k<512;k++)
      cairo_line_to(cr, (b + k/512.0*(w-b))*sx, noise(b + k/512.0*(w-b), b, w, p->a, p->b, p->c)*sy+height);
    // cairo_restore(cr);
    cairo_stroke(cr);
  }
  else
  { // denoise stage, draw stabilised unit variance (hopefully)
    cairo_set_source_rgb(cr, .7, .1, .1);
    cairo_move_to(cr, 0*sx, 1.0*sy + height);
    cairo_line_to(cr, w*sx, 1.0*sy + height);
    cairo_stroke(cr);
  }
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
