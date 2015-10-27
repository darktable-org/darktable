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

typedef union floatint_t
{
  float f;
  uint32_t i;
} floatint_t;

// very fast approximation for 2^-x (returns 0 for x > 126)
static inline float fast_mexp2f(const float x)
{
  const float i1 = (float)0x3f800000u; // 2^0
  const float i2 = (float)0x3f000000u; // 2^-1
  const float k0 = i1 + x * (i2 - i1);
  floatint_t k;
  k.i = k0 >= (float)0x800000u ? k0 : 0;
  return k.f;
}

static inline float noise_stddev_gen_Fisz(const float level, const float black, const float white, const float a, const float b)
{
  return sqrtf(fmaxf(1.0f, fmaxf(0.0f, a*(level-black)) + b));
}

static inline float noise_stddev_dxo(
    const float level,   // input directly upsampled from uint16_t raw
    const float black,   // raw black point
    const float white,   // raw white level
    const float r,       // read noise
    const float G,       // gain, such that 100G equals the number of photoelectrons at white
    const float p)       // pixel non-uniformity
{
  // percentage of input level, 100 is white
  const float g = (level - black)/(white-black) * 100.0f;
  // noise scaled as g*G is, i.e. photoelectrons.
  // to compare to signal, uint16_t needs to be scaled (i-b)/(w-b)*100 * G
  const float n = sqrtf(fmaxf(1.0f, r*r + g*G + p*g*G * p*g*G));
  // or just do the reverse scaling here to keep the SNR to the input signal constant (ignore black offset for this matter):
  // return (white-black)/(100.0*G) * n;
  // return 100*G/(white-black) * n;
  // return 100*G * n;
  // return (white-black) * n;
  // return n/(white-black);
  return n;
  // unfortunately none of the above match the measured noise at all :/
}

static inline float noise(const float l, const float b, const float w, const float x, const float y, const float z)
{
  return noise_stddev_dxo(l, b, w, x, y, z);
  // return noise_stddev_gen_Fisz(l, b, w, x, y);
}

// TODO: adjust those two functions to new noise model!
static inline void precondition(
    const uint16_t *const in,
    float *const buf,
    const float *const ref,
    const int wd,
    const int ht,
    const float a,
    const float b,
    const uint32_t mode)
{
// #ifdef _OPENMP
// #pragma omp parallel for schedule(static) default(none)
// #endif
  for(int j = 0; j < ht; j++)
  {
    float *buf2 = buf + (size_t)j * wd;
    const float *ref2 = ref + (size_t)j * wd;
    const uint16_t *in2 = in + (size_t)j * wd;
    for(int i = 0; i < wd; i++)
    {
      if(mode == 0)
        *buf2 = *in2;
      else
#if 0 // science
        const float sigma2 = (b/a)*(b/a);
        *buf2 = 2.0f * sqrtf(fmaxf(0.0f, *in2/a + 3./8. + sigma2));
#else // custom pre-black point:
        *buf2 = *in2 / sqrtf(fmaxf(1.f, a* *ref2 + b));
#endif
      buf2 ++;
      in2 ++;
      ref2 ++;
    }
  }
}

static inline void backtransform(
    const float *const buf,
    uint16_t *const out,
    const float *const ref,
    const int wd,
    const int ht,
    const float a,
    const float b,
    const uint32_t mode)
{
// #ifdef _OPENMP
// #pragma omp parallel for schedule(static) default(none)
// #endif
  for(int j = 0; j < ht; j++)
  {
    const float *buf2 = buf + (size_t)j * wd;
    const float *ref2 = ref + (size_t)j * wd;
    uint16_t *out2 = out + (size_t)j * wd;
    for(int i = 0; i < wd; i++)
    {
      if(mode == 0)
      {
        *out2 = *buf2;
      }
      else
      {
        const float x = *buf2;
#if 0 // science
        const float sigma2 = (b/a)*(b/a);
        // closed form approximation to unbiased inverse (input range was 0..200 for fit, not 0..1)
        if(x < .5f)
          *out2 = 0.0f;
        else
          *out2 = 1. / 4. * x * x + 1. / 4. * sqrtf(3. / 2.) / x - 11. / 8. * 1.0 / (x * x)
            + 5. / 8. * sqrtf(3. / 2.) * 1.0 / (x * x * x) - 1. / 8. - sigma2;
        // asymptotic form:
        // *out2 = fmaxf(0.0f, 1./4.*x*x - 1./8. - sigma2);
        *out2 *= a;
#else // custom pre-blackpoint, algebraic inverse
        // *out2 = CLAMP(.5f * (a * x*x + sqrtf(a*a*x*x*x*x + 4.0f*b*x*x)), 0, 0xffff);
        *out2 = CLAMP(x * sqrtf(fmaxf(1.f, a* *ref2 + b)), 0, 0xffff);
#endif
      }
      buf2 ++;
      out2 ++;
      ref2 ++;
    }
  }
}

#if 1 //def ANALYSE
static void analyse_g(
    float *const coarse,         // blurred buffer
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
    uint16_t vb;
    float inp;
    if(mode == 0)
    {
      inp = input[width*j+i];
      vb = v;
    }
    else
    {
      // precondition(input+width*j+i, &inp, coarse+width*j+i, 1, 1, aa, bb, mode);
      inp = input[width*j+i];
      vb = input[width*j+i];
      //backtransform(coarse+width*j+i, &vb, coarse+width*j+i, 1, 1, aa, bb, mode);
    }
    const float d = fabsf(inp - v);
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
      if(mode) g->stddev[k] *= aa;
      if(g->stddev[k] > g->stddev_max) g->stddev_max = g->stddev[k];
      // fprintf(stdout, "%g %g\n", b + (w-b)*k/(float)N, sum[k]/(1.4826*num[k]));
    }
  }
  // else
    // for(int k=0;k<N;k++)
      // fprintf(stdout, "%g %g\n", b + (w-b)*k/(float)N, (sum2[k] - sum[k]*sum[k]/N)/(N-1));
      // fprintf(stdout, "%g %g\n", b + (w-b)*k/(float)N, sum[k]/(1.4826*num[k]));
}

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
    const int32_t height)     // height of buffers (all three same size)
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
    // detail[j*width+i] = input[j*width+i] - output[j*width+i];
    // Fisz transform:
    detail[j*width+i] = (input[j*width+i] - output[j*width+i]) / noise(input[j*width+i], black, white, a, b, c);
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
    const int32_t height)     // height of buffers (all three same size)
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
    // detail[j*width+i] = input[j*width+i] - output[j*width+i];
    // Fisz transform:
    detail[j*width+i] = (input[j*width+i] - output[j*width+i]) / noise(input[j*width+i], black, white, a, b, c);
}

#if 1// TODO: rewrite for single channel bayer patters (need to add channels for thrs+boost depending on pattern)
static void synthesize(
    float *const out,
    const float *const in,
    const float *const detail,
    const float *thrsf,
    const float *boostf,
    const float black,
    const float white,
    const float a,
    const float b,
    const float c,
    const int32_t width,
    const int32_t height)
{
  const float threshold = thrsf[0];
  const float boost = boostf[0];

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
      // inverse Fisz transform:
      const float d0 = *pdetail * noise(*pin, black, white, a, b, c);
      // const float d0 = *pdetail;
      const float d = copysignf(fmaxf(fabsf(d0) - threshold, 0.0f), d0);
      *pout = *pin + boost * d;
      pdetail++;
      pin++;
      pout++;
    }
  }
}
#endif

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid,
    const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  // now unfortunately we're supposed to work on 16-bit in- and output.


  // this is called for preview and full pipe separately, each with its own pixelpipe piece.
  // get our data struct:
  dt_iop_rawdenoiseprofile_params_t *d = (dt_iop_rawdenoiseprofile_params_t *)piece->data;

  const int max_max_scale = 5; // hard limit
  int max_scale = 3;
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

  // DEBUG: hardcoded offsets to beginning of cggc quad
  const int offx = 0;

  float *buf[max_max_scale];
  float *tmp1 = 0, *tmp2 = 0;
  float *buf1 = 0, *buf2 = 0;
  for(int k = 0; k < max_scale; k++)
    buf[k] = dt_alloc_align(64, sizeof(float) * npixels);
  tmp1 = dt_alloc_align(64, sizeof(float) * npixels);
  tmp2 = dt_alloc_align(64, sizeof(float) * npixels);

#if 0
  // create reference buffer for noise stabilisation (very wasteful on memory for now, sorry)
  float *ref = dt_alloc_align(64, sizeof(float) * npixels);
  for(size_t k=0;k<npixels;k++) tmp1[k] = ((uint16_t *)ivoid)[k];
  decompose_g (ref, tmp1, tmp2,   offx,    scale, width, height);  // green
  decompose_rb(ref, tmp1, tmp2, 1-offx, 0, scale, width, height);  // blue
  decompose_rb(ref, tmp1, tmp2,   offx, 1, scale, width, height);  // red
#endif

  // noise std dev ~= sqrt(b + a*input)
  // lacks pixel non-uniformity that kicks in for large values (do we care?)
  const float a = d->a, b = d->b, c = d->c;
  const float black = self->dev->image_storage.raw_black_level;
  const float white = self->dev->image_storage.raw_white_point;

  // precondition((uint16_t *)ivoid, tmp1, ref, width, height, a, b, d->mode);
  for(size_t k=0;k<npixels;k++) tmp1[k] = ((uint16_t *)ivoid)[k];
#if 1 // debug: write out preconditioned buffer for external analysis
  if(self->gui_data)
  {
      char filename[512];
      snprintf(filename, sizeof(filename), "/tmp/preconditioned.pfm");
      FILE *f = fopen(filename, "wb");
      fprintf(f, "PF\n%d %d\n-1.0\n", width, height);
      for(size_t k = 0; k < npixels; k++)
        for(int c=0;c<3;c++)
          fwrite(tmp1+k, sizeof(float), 1, f);
      fclose(f);
  }
#endif

  buf1 = tmp1;
  buf2 = tmp2;
memset(tmp2, 0, sizeof(float)*width*height);

#if 1
  for(int scale = 0; scale < max_scale; scale++)
  {
    // FIXME: hardcoded offsets for 5dm2
    decompose_g (buf2, buf1, buf[scale],   offx,    scale, black, white, a, b, c, width, height);  // green
    decompose_rb(buf2, buf1, buf[scale], 1-offx, 0, scale, black, white, a, b, c, width, height);  // blue
    decompose_rb(buf2, buf1, buf[scale],   offx, 1, scale, black, white, a, b, c, width, height);  // red
// DEBUG: clean out temporary memory:
memset(buf1, 0, sizeof(float)*width*height);
#if 0 // DEBUG: print wavelet scales:
    if(piece->pipe->type != DT_DEV_PIXELPIPE_PREVIEW)
    {
      char filename[512];
      snprintf(filename, sizeof(filename), "/tmp/coarse_%d.pfm", scale);
      FILE *f = fopen(filename, "wb");
      fprintf(f, "PF\n%d %d\n-1.0\n", width, height);
      for(size_t k = 0; k < npixels; k++)
        fwrite(buf2+4*k, sizeof(float), 3, f);
      fclose(f);
      snprintf(filename, sizeof(filename), "/tmp/detail_%d.pfm", scale);
      f = fopen(filename, "wb");
      fprintf(f, "PF\n%d %d\n-1.0\n", width, height);
      for(size_t k = 0; k < npixels; k++)
        fwrite(buf[scale]+4*k, sizeof(float), 3, f);
      fclose(f);
    }
#endif
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
  analyse_rb(buf1, ivoid, 1-offx, 0, width, height, black, white, a, b, c, d->mode);
  analyse_rb(buf1, ivoid,   offx, 1, width, height, black, white, a, b, c, d->mode);
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
    float sum_y2 = 0.0f;
    for(size_t k = 0; k < npixels; k++)
      sum_y2 += buf[scale][k] * buf[scale][k];

    const float sb2 = sigma_band * sigma_band;
    const float var_y = sum_y2 / (npixels - 1.0f);
    const float std_x = sqrtf(MAX(1e-6f, var_y - sb2));
    // adjust here because it seemed a little weak
    const float adjt = d->strength;
    const float thrs[4] = { adjt * sb2 / std_x };
// const float std = (std_x[0] + std_x[1] + std_x[2])/3.0f;
// const float thrs[4] = { adjt*sigma*sigma/std, adjt*sigma*sigma/std, adjt*sigma*sigma/std, 0.0f};
fprintf(stderr, "scale %d thrs %f = %f / %f\n", scale, thrs[0], sb2, std_x);
#else
    const float thrs[4] = { 0.0, 0.0, 0.0, 0.0 };
#endif
    const float boost[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    synthesize(buf2, buf1, buf[scale], thrs, boost, black, white, a, b, c, width, height);
    // DEBUG: clean out temporary memory:
    // memset(buf1, 0, sizeof(float)*4*width*height);

    float *buf3 = buf2;
    buf2 = buf1;
    buf1 = buf3;
  }

  // backtransform(buf1, (uint16_t *)ovoid, ref, width, height, a, b, d->mode);
  for(size_t k=0;k<npixels;k++) ((uint16_t*)ovoid)[k] = CLAMP(buf1[k], 0, 0xffff);
#endif

  for(int k = 0; k < max_scale; k++) dt_free_align(buf[k]);
  dt_free_align(tmp1);
  dt_free_align(tmp2);
  // dt_free_align(ref);

  // if(piece->pipe->mask_display) dt_iop_alpha_copy(ivoid, ovoid, width, height);
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
  g->strength = dt_bauhaus_slider_new_with_range(self, 0.0f, 400.0f, .05, 1.f, 3);
  g->a = dt_bauhaus_slider_new_with_range(self, 0.0001f, 10.0f, .05, 1.f, 4);
  g->b = dt_bauhaus_slider_new_with_range(self, 0.0f, 1000.0f, .05, 0.f, 3);
  g->c = dt_bauhaus_slider_new_with_range(self, 0.0f, .01f, .00001, 0.f, 8);
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
