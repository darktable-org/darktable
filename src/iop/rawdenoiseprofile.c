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
  float strength;   // noise level after equalization
  float a[3], b[3]; // fit for poissonian-gaussian noise per color channel.
} dt_iop_rawdenoiseprofile_params_t;

typedef struct dt_iop_rawdenoiseprofile_gui_data_t
{
  GtkWidget *profile;
  GtkWidget *strength;
  dt_noiseprofile_t interpolated; // don't use name, maker or model, they may point to garbage
  GList *profiles;
} dt_iop_rawdenoiseprofile_gui_data_t;

typedef dt_iop_rawdenoiseprofile_params_t dt_iop_rawdenoiseprofile_data_t;

static dt_noiseprofile_t dt_iop_rawdenoiseprofile_get_auto_profile(dt_iop_module_t *self);

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
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
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

// TODO: adjust those two functions to new noise model!
static inline void precondition(
    const uint16_t *const in,
    float *const buf,
    const int wd,
    const int ht)
{
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none)
#endif
  for(int j = 0; j < ht; j++)
  {
    float *buf2 = buf + (size_t)j * wd;
    const uint16_t *in2 = in + (size_t)j * wd;
    for(int i = 0; i < wd; i++)
    {
      *buf2 = *in2;
      buf2 ++;
      in2 ++;
    }
  }
}

static inline void backtransform(
    const float *const buf,
    uint16_t *const out,
    const int wd,
    const int ht)
{
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none)
#endif
  for(int j = 0; j < ht; j++)
  {
    const float *buf2 = buf + (size_t)j * wd;
    uint16_t *out2 = out + (size_t)j * wd;
    for(int i = 0; i < wd; i++)
    {
      *out2 = *buf2;
      buf2 ++;
      out2 ++;
    }
  }
}

static void decompose_g(
    float *const output,      // output buffer
    const float *const input, // const input buffer
    float *const detail,      // output detail buffer
    const int offx,           // select channel in bayer pattern
    const int scale,          // 0..max wavelet scale
    const int32_t width,      // width of buffers
    const int32_t height)     // height of buffers (all three same size)
{
  const int mult = 1 << scale;
  static const float filter[5] = { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };

  // separable, data-independent, shift-independent a-trous wavelet transform.
  // split in H/V passes, for green bayer channel (diagonal)

  // H pass, or (+1, +1) really, write to detail buf (iterate over all green pixels only)
  // for(int j=0;j<height;j++) for(int i=(j&1)?1-offx:offx;i<width;i+=2)
  for(int j=2*mult;j<height-2*mult;j++) for(int i=((j&1)?1-offx:offx)+2*mult;i<width-2*mult;i+=2)
  {
    detail[j*width+i] = 0.0f;
    for(int k=0;k<5;k++)
      detail[j*width+i] += filter[k] * input[(j + mult*(k-2))*width+i + mult*(k-2)];
  }

  // V pass, or (-1, +1), read detail write to output
  for(int j=2*mult;j<height-2*mult;j++) for(int i=((j&1)?1-offx:offx)+2*mult;i<width-2*mult;i+=2)
  {
    output[j*width+i] = 0.0f;
    for(int k=0;k<5;k++)
      output[j*width+i] += filter[k] * detail[(j - mult*(k-2))*width+i + mult*(k-2)];
  }

  // final pass, write detail coeffs
  for(int j=2*mult;j<height-2*mult;j++) for(int i=((j&1)?1-offx:offx)+2*mult;i<width-2*mult;i+=2)
    detail[j*width+i] = input[j*width+i] - output[j*width+i];
}

static void decompose_rb(
    float *const output,      // output buffer
    const float *const input, // const input buffer
    float *const detail,      // output detail buffer
    const int offx,           // select channel in bayer pattern
    const int offy,
    const int scale,          // 0..max wavelet scale
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
  for(int j=offy+2*mult;j<height-2*mult;j+=2) for(int i=offx+2*mult;i<width-2*mult;i+=2)
  {
    detail[j*width+i] = 0.0f;
    for(int k=0;k<5;k++)
      detail[j*width+i] += filter[k] * input[j*width+i + mult*(k-2)];
  }

  // V pass, read detail write to output
  // for(int j=offy;j<height;j+=2) for(int i=offx;i<width;i+=2)
  for(int j=offy+2*mult;j<height-2*mult;j+=2) for(int i=offx+2*mult;i<width-2*mult;i+=2)
  {
    output[j*width+i] = 0.0f;
    for(int k=0;k<5;k++)
      output[j*width+i] += filter[k] * detail[(j + mult*(k-2))*width+i];
  }

  // final pass, write detail coeffs
  // for(int j=offy;j<height;j+=2) for(int i=offx;i<width;i+=2)
  for(int j=offy+2*mult;j<height-2*mult;j+=2) for(int i=offx+2*mult;i<width-2*mult;i+=2)
    detail[j*width+i] = input[j*width+i] - output[j*width+i];
}

#if 0// TODO: rewrite for single channel bayer patters:
static void synthesize(float *const out, const float *const in, const float *const detail,
                       const float *thrsf, const float *boostf, const int32_t width, const int32_t height)
{
  const __m128 threshold = _mm_set_ps(thrsf[3], thrsf[2], thrsf[1], thrsf[0]);
  const __m128 boost = _mm_set_ps(boostf[3], boostf[2], boostf[1], boostf[0]);

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j = 0; j < height; j++)
  {
    // TODO: prefetch? _mm_prefetch()
    const __m128 *pin = (__m128 *)in + (size_t)j * width;
    __m128 *pdetail = (__m128 *)detail + (size_t)j * width;
    float *pout = out + (size_t)4 * j * width;
    for(int i = 0; i < width; i++)
    {
#if 1
      const __m128i maski = _mm_set1_epi32(0x80000000u);
      const __m128 *mask = (__m128 *)&maski;
      const __m128 absamt
          = _mm_max_ps(_mm_setzero_ps(), _mm_sub_ps(_mm_andnot_ps(*mask, *pdetail), threshold));
      const __m128 amount = _mm_or_ps(_mm_and_ps(*pdetail, *mask), absamt);
      _mm_stream_ps(pout, _mm_add_ps(*pin, _mm_mul_ps(boost, amount)));
#endif
      // _mm_stream_ps(pout, _mm_add_ps(*pin, *pdetail));
      pdetail++;
      pin++;
      pout += 4;
    }
  }
  _mm_sfence();
}
#endif

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid,
    const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  // now unfortunately we're supposed to work on 16-bit in- and output.


  // this is called for preview and full pipe separately, each with its own pixelpipe piece.
  // get our data struct:
  // dt_iop_rawdenoiseprofile_params_t *d = (dt_iop_rawdenoiseprofile_params_t *)piece->data;

  const int max_max_scale = 5; // hard limit
  int max_scale = 1;
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

  float *buf[max_max_scale];
  float *tmp1 = 0, *tmp2 = 0;
  float *buf1 = 0, *buf2 = 0;
  for(int k = 0; k < max_scale; k++)
    buf[k] = dt_alloc_align(64, sizeof(float) * npixels);
  tmp1 = dt_alloc_align(64, sizeof(float) * npixels);
  tmp2 = dt_alloc_align(64, sizeof(float) * npixels);

  precondition((uint16_t *)ivoid, tmp1, width, height);

#if 0 // DEBUG: see what variance we have after transform
  if(piece->pipe->type != DT_DEV_PIXELPIPE_PREVIEW)
  {
    const int n = width*height;
    FILE *f = fopen("/tmp/transformed.pfm", "wb");
    fprintf(f, "PF\n%d %d\n-1.0\n", width, height);
    for(int k=0; k<n; k++)
      fwrite(((float*)ovoid)+4*k, sizeof(float), 3, f);
    fclose(f);
  }
#endif

  buf1 = tmp1;
  buf2 = tmp2;
memset(tmp2, 0, sizeof(float)*width*height);

  // debug: just do one step to see what it's doing:
  // decompose_g (buf2, buf1, buf[1], 1, 0, width, height);
  // decompose_rb(buf2, buf1, buf[1], 1, 0, 0, width, height);
  // decompose_rb(buf2, buf1, buf[1], 0, 1, 0, width, height);

  // DEBUG: hardcoded offsets to beginning of cggc quad
  const int offx = 0;
#if 1
  for(int scale = 0; scale < max_scale; scale++)
  {
    // FIXME: hardcoded offsets for 5dm2
    decompose_g (buf2, buf1, buf[scale],   offx,    scale, width, height);  // green
    decompose_rb(buf2, buf1, buf[scale], 1-offx, 0, scale, width, height);  // blue
    decompose_rb(buf2, buf1, buf[scale],   offx, 1, scale, width, height);  // red
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
  backtransform(buf1, (uint16_t *)ovoid, width, height);

#if 0
  // now do everything backwards, so the result will end up in *ovoid
  for(int scale = max_scale - 1; scale >= 0; scale--)
  {
#if 1
    // variance stabilizing transform maps sigma to unity.
    const float sigma = 1.0f;
    // it is then transformed by wavelet scales via the 5 tap a-trous filter:
    const float varf = sqrtf(2.0f + 2.0f * 4.0f * 4.0f + 6.0f * 6.0f) / 16.0f; // about 0.5
    const float sigma_band = powf(varf, scale) * sigma;
    // determine thrs as bayesshrink
    // TODO: parallelize!
    float sum_y2[3] = { 0.0f };
    for(size_t k = 0; k < npixels; k++)
      for(int c = 0; c < 3; c++) sum_y2[c] += buf[scale][4 * k + c] * buf[scale][4 * k + c];

    const float sb2 = sigma_band * sigma_band;
    const float var_y[3] = { sum_y2[0] / (npixels - 1.0f), sum_y2[1] / (npixels - 1.0f), sum_y2[2] / (npixels - 1.0f) };
    const float std_x[3] = { sqrtf(MAX(1e-6f, var_y[0] - sb2)), sqrtf(MAX(1e-6f, var_y[1] - sb2)),
                             sqrtf(MAX(1e-6f, var_y[2] - sb2)) };
    // add 8.0 here because it seemed a little weak
    const float adjt = 8.0f;
    const float thrs[4] = { adjt * sb2 / std_x[0], adjt * sb2 / std_x[1], adjt * sb2 / std_x[2], 0.0f };
// const float std = (std_x[0] + std_x[1] + std_x[2])/3.0f;
// const float thrs[4] = { adjt*sigma*sigma/std, adjt*sigma*sigma/std, adjt*sigma*sigma/std, 0.0f};
// fprintf(stderr, "scale %d thrs %f %f %f = %f / %f %f %f \n", scale, thrs[0], thrs[1], thrs[2], sb2,
// std_x[0], std_x[1], std_x[2]);
#endif
    const float boost[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    // const float thrs[4] = { 0.0, 0.0, 0.0, 0.0 };
    eaw_synthesize(buf2, buf1, buf[scale], thrs, boost, width, height);
    // DEBUG: clean out temporary memory:
    // memset(buf1, 0, sizeof(float)*4*width*height);

    float *buf3 = buf2;
    buf2 = buf1;
    buf1 = buf3;
  }

  backtransform(buf2, (uint16_t *)ovoid, width, height);
#endif

  for(int k = 0; k < max_scale; k++) dt_free_align(buf[k]);
  dt_free_align(tmp1);
  dt_free_align(tmp2);

  // if(piece->pipe->mask_display) dt_iop_alpha_copy(ivoid, ovoid, width, height);
}


/** this will be called to init new defaults if a new image is loaded from film strip mode. */
void reload_defaults(dt_iop_module_t *module)
{
  // our module is disabled by default
  module->default_enabled = 0;
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
        g->interpolated.a[0] = -1.0;
        snprintf(name, sizeof(name), _("found match for ISO %d"), iso);
        break;
      }
      if(last && last->iso < iso && current->iso > iso)
      {
        dt_noiseprofile_interpolate(last, current, &g->interpolated);
        // signal later autodetection in commit_params:
        g->interpolated.a[0] = -1.0;
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
    for(int k = 0; k < 3; k++)
    {
      ((dt_iop_rawdenoiseprofile_params_t *)module->default_params)->a[k] = g->interpolated.a[k];
      ((dt_iop_rawdenoiseprofile_params_t *)module->default_params)->b[k] = g->interpolated.b[k];
    }
    memcpy(module->params, module->default_params, sizeof(dt_iop_rawdenoiseprofile_params_t));
  }
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

/** commit is the synch point between core and gui, so it copies params to pipe data. */
void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_rawdenoiseprofile_params_t *p = (dt_iop_rawdenoiseprofile_params_t *)params;
  dt_iop_rawdenoiseprofile_data_t *d = (dt_iop_rawdenoiseprofile_data_t *)piece->data;

  // copy everything first and make some changes later
  memcpy(d, p, sizeof(*d));

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

static void profile_callback(GtkWidget *w, dt_iop_module_t *self)
{
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
  dt_bauhaus_combobox_set(g->profile, -1);
  if(p->a[0] == -1.0)
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
}

void gui_init(dt_iop_module_t *self)
{
  // init the slider (more sophisticated layouts are possible with gtk tables and boxes):
  self->gui_data = malloc(sizeof(dt_iop_rawdenoiseprofile_gui_data_t));
  dt_iop_rawdenoiseprofile_gui_data_t *g = (dt_iop_rawdenoiseprofile_gui_data_t *)self->gui_data;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  g->profiles = NULL;
  g->profile = dt_bauhaus_combobox_new(self);
  g->strength = dt_bauhaus_slider_new_with_range(self, 0.001f, 4.0f, .05, 1.f, 3);
  gtk_box_pack_start(GTK_BOX(self->widget), g->profile, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->strength, TRUE, TRUE, 0);
  dt_bauhaus_widget_set_label(g->profile, NULL, _("profile"));
  dt_bauhaus_widget_set_label(g->strength, NULL, _("strength"));
  g_object_set(G_OBJECT(g->profile), "tooltip-text", _("profile used for variance stabilization"),
               (char *)NULL);
  g_object_set(G_OBJECT(g->strength), "tooltip-text", _("finetune denoising strength"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->profile), "value-changed", G_CALLBACK(profile_callback), self);
  g_signal_connect(G_OBJECT(g->strength), "value-changed", G_CALLBACK(strength_callback), self);
}

void gui_cleanup(dt_iop_module_t *self)
{
  dt_iop_rawdenoiseprofile_gui_data_t *g = (dt_iop_rawdenoiseprofile_gui_data_t *)self->gui_data;
  g_list_free_full(g->profiles, dt_noiseprofile_free);
  // nothing else necessary, gtk will clean up the slider.
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
