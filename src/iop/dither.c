/*
    This file is part of darktable,
    copyright (c) 2012--2013 Ulrich Pegelow

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
#include "common/imageio.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/tiling.h"
#include "dtgtk/gradientslider.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"
#include "common/iop_group.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>
#include <inttypes.h>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif

#define CLIP(x) ((x < 0) ? 0.0 : (x > 1.0) ? 1.0 : x)
#define TEA_ROUNDS 8

DT_MODULE_INTROSPECTION(1, dt_iop_dither_params_t)

typedef void(_find_nearest_color)(float *val, float *err, const float f, const float rf);

#if defined(__SSE__)
typedef __m128(_find_nearest_color_sse)(float *val, const float f, const float rf);
#endif

typedef enum dt_iop_dither_type_t
{
  DITHER_RANDOM,
  DITHER_FS1BIT,
  DITHER_FS4BIT_GRAY,
  DITHER_FS8BIT,
  DITHER_FS16BIT,
  DITHER_FSAUTO
} dt_iop_dither_type_t;


typedef struct dt_iop_dither_params_t
{
  dt_iop_dither_type_t dither_type;
  int palette; // reserved for future extensions
  struct
  {
    float radius;   // reserved for future extensions
    float range[4]; // reserved for future extensions
    float damping;
  } random;
} dt_iop_dither_params_t;

typedef struct dt_iop_dither_gui_data_t
{
  GtkWidget *dither_type;
  GtkWidget *random;
  GtkWidget *radius;
  GtkWidget *range;
  GtkWidget *range_label;
  GtkWidget *damping;
} dt_iop_dither_gui_data_t;

typedef struct dt_iop_dither_data_t
{
  dt_iop_dither_type_t dither_type;
  struct
  {
    float radius;
    float range[4];
    float damping;
  } random;
} dt_iop_dither_data_t;


const char *name()
{
  return _("dithering");
}


int groups()
{
  return dt_iop_get_group("dithering", IOP_GROUP_CORRECT);
}

int flags()
{
  return IOP_FLAGS_ONE_INSTANCE;
}


void init_presets(dt_iop_module_so_t *self)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "BEGIN", NULL, NULL, NULL);

  dt_iop_dither_params_t tmp
      = (dt_iop_dither_params_t){ DITHER_FSAUTO, 0, { 0.0f, { 0.0f, 0.0f, 1.0f, 1.0f }, -200.0f } };
  // add the preset.
  dt_gui_presets_add_generic(_("dither"), self->op, self->version(), &tmp, sizeof(dt_iop_dither_params_t), 1);
  // make it auto-apply for all images:
  // dt_gui_presets_update_autoapply(_("dither"), self->op, self->version(), 1);

  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "COMMIT", NULL, NULL, NULL);
}


// dither pixel into gray, with f=levels-1 and rf=1/f, return err=old-new
static void _find_nearest_color_n_levels_gray(float *val, float *err, const float f, const float rf)
{
  const float in = 0.30f * val[0] + 0.59f * val[1] + 0.11f * val[2]; // RGB -> GRAY

  float tmp = in * f;
  int itmp = floorf(tmp);

  float new = (tmp - itmp > 0.5f ? (float)(itmp + 1) : (float)itmp) * rf;

  for(int c = 0; c < 4; c++)
  {
    err[c] = val[c] - new;
    val[c] = new;
  }
}

#if defined(__SSE2__)
// dither pixel into gray, with f=levels-1 and rf=1/f, return err=old-new
static __m128 _find_nearest_color_n_levels_gray_sse(float *val, const float f, const float rf)
{
  __m128 err;
  __m128 new;

  const float in = 0.30f * val[0] + 0.59f * val[1] + 0.11f * val[2]; // RGB -> GRAY

  float tmp = in * f;
  int itmp = floorf(tmp);

  new = _mm_set1_ps(tmp - itmp > 0.5f ? (float)(itmp + 1) * rf : (float)itmp * rf);
  err = _mm_sub_ps(_mm_load_ps(val), new);
  _mm_store_ps(val, new);

  return err;
}
#endif

// dither pixel into RGB, with f=levels-1 and rf=1/f, return err=old-new
static void _find_nearest_color_n_levels_rgb(float *val, float *err, const float f, const float rf)
{
  for(int c = 0; c < 4; c++)
  {
    float old = val[c];
    float tmp = old * f;
    float itmp = floorf(tmp);
    float new = (tmp - itmp > 0.5f ? itmp + 1 : itmp) * rf;

    val[c] = new;
    err[c] = old - new;
  }
}

#if defined(__SSE2__)
// dither pixel into RGB, with f=levels-1 and rf=1/f, return err=old-new
static __m128 _find_nearest_color_n_levels_rgb_sse2(float *val, const float f, const float rf)
{
  __m128 old = _mm_load_ps(val);
  __m128 tmp = _mm_mul_ps(old, _mm_set1_ps(f));        // old * f
  __m128 itmp = _mm_cvtepi32_ps(_mm_cvtps_epi32(tmp)); // floor(tmp)
  __m128 new = _mm_mul_ps(
      _mm_add_ps(itmp,
                 _mm_and_ps(_mm_cmpgt_ps(_mm_sub_ps(tmp, itmp), // (tmp - itmp > 0.5f ? itmp + 1 : itmp) * rf
                                         _mm_set1_ps(0.5f)),
                            _mm_set1_ps(1.0f))),
      _mm_set1_ps(rf));

  _mm_store_ps(val, new);

  return _mm_sub_ps(old, new);
}
#endif

static inline void _diffuse_error(float *val, const float *err, const float factor)
{
  for(int c = 0; c < 4; c++)
  {
    val[c] += err[c] * factor;
  }
}

#if defined(__SSE__)
static inline void _diffuse_error_sse(float *val, const __m128 err, const float factor)
{
  _mm_store_ps(val,
               _mm_add_ps(_mm_load_ps(val), _mm_mul_ps(err, _mm_set1_ps(factor)))); // *val += err * factor
}
#endif

static inline float clipnan(const float x)
{
  float r;

  if(isnan(x))
    r = 0.5f;
  else // normal number
    r = (isless(x, 0.0f) ? 0.0f : (isgreater(x, 1.0f) ? 1.0f : x));

  return r;
}

static void process_floyd_steinberg(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                    const void *const ivoid, void *const ovoid,
                                    const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_dither_data_t *data = (dt_iop_dither_data_t *)piece->data;

  const int width = roi_in->width;
  const int height = roi_in->height;
  const int ch = piece->colors;
  const float scale = roi_in->scale / piece->iscale;
  const int l1 = floorf(1.0f + dt_log2f(1.0f / scale));

  _find_nearest_color *nearest_color = NULL;
  unsigned int levels = 1;
  int bds = (piece->pipe->type != DT_DEV_PIXELPIPE_EXPORT) ? l1 * l1 : 1;

  switch(data->dither_type)
  {
    case DITHER_FS1BIT:
      nearest_color = _find_nearest_color_n_levels_gray;
      levels = MAX(2, MIN(bds + 1, 256));
      break;
    case DITHER_FS4BIT_GRAY:
      nearest_color = _find_nearest_color_n_levels_gray;
      levels = MAX(16, MIN(15 * bds + 1, 256));
      break;
    case DITHER_FS8BIT:
      nearest_color = _find_nearest_color_n_levels_rgb;
      levels = 256;
      break;
    case DITHER_FS16BIT:
      nearest_color = _find_nearest_color_n_levels_rgb;
      levels = 65536;
      break;
    case DITHER_FSAUTO:
      switch(piece->pipe->levels & IMAGEIO_CHANNEL_MASK)
      {
        case IMAGEIO_RGB:
          nearest_color = _find_nearest_color_n_levels_rgb;
          break;
        case IMAGEIO_GRAY:
          nearest_color = _find_nearest_color_n_levels_gray;
          break;
      }

      switch(piece->pipe->levels & IMAGEIO_PREC_MASK)
      {
        case IMAGEIO_INT8:
          levels = 256;
          break;
        case IMAGEIO_INT12:
          levels = 4096;
          break;
        case IMAGEIO_INT16:
          levels = 65536;
          break;
        case IMAGEIO_BW:
          levels = 2;
          break;
        case IMAGEIO_INT32:
        case IMAGEIO_FLOAT:
        default:
          nearest_color = NULL;
          break;
      }
      // no automatic dithering for preview and thumbnail
      if(piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW || piece->pipe->type == DT_DEV_PIXELPIPE_THUMBNAIL)
        nearest_color = NULL;
      break;
    case DITHER_RANDOM:
      // this function won't ever be called for that type
      // instead, process_random() will be called
      __builtin_unreachable();
      break;
  }

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j = 0; j < height; j++)
  {
    const float *in = (const float *)ivoid + (size_t)ch * width * j;
    float *out = (float *)ovoid + (size_t)ch * width * j;
    for(int i = 0; i < width; i++, in += ch, out += ch)
    {
      out[0] = clipnan(in[0]);
      out[1] = clipnan(in[1]);
      out[2] = clipnan(in[2]);
    }
  }

  if(nearest_color == NULL) return;

  const float f = levels - 1;
  const float rf = 1.0 / f;
  float err[4];

  // dither without error diffusion on very tiny images
  if(width < 3 || height < 3)
  {
    for(int j = 0; j < height; j++)
    {
      float *out = ((float *)ovoid) + (size_t)ch * j * width;
      for(int i = 0; i < width; i++) nearest_color(out + ch * i, err, f, rf);
    }

    if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
    return;
  }

  // floyd-steinberg dithering follows here

  // first height-1 rows
  for(int j = 0; j < height - 1; j++)
  {
    float *out = ((float *)ovoid) + (size_t)ch * j * width;

    // first column
    nearest_color(out, err, f, rf);
    _diffuse_error(out + ch, err, 7.0f / 16.0f);
    _diffuse_error(out + ch * width, err, 5.0f / 16.0f);
    _diffuse_error(out + ch * (width + 1), err, 1.0f / 16.0f);


    // main part of image
    for(int i = 1; i < width - 1; i++)
    {
      nearest_color(out + ch * i, err, f, rf);
      _diffuse_error(out + ch * (i + 1), err, 7.0f / 16.0f);
      _diffuse_error(out + ch * (i - 1) + ch * width, err, 3.0f / 16.0f);
      _diffuse_error(out + ch * i + ch * width, err, 5.0f / 16.0f);
      _diffuse_error(out + ch * (i + 1) + ch * width, err, 1.0f / 16.0f);
    }

    // last column
    nearest_color(out + ch * (width - 1), err, f, rf);
    _diffuse_error(out + ch * (width - 2) + ch * width, err, 3.0f / 16.0f);
    _diffuse_error(out + ch * (width - 1) + ch * width, err, 5.0f / 16.0f);
  }

  // last row
  do
  {
    float *out = ((float *)ovoid) + (size_t)ch * (height - 1) * width;

    // lower left pixel
    nearest_color(out, err, f, rf);
    _diffuse_error(out + ch, err, 7.0f / 16.0f);

    // main part of last row
    for(int i = 1; i < width - 1; i++)
    {
      nearest_color(out + ch * i, err, f, rf);
      _diffuse_error(out + ch * (i + 1), err, 7.0f / 16.0f);
    }

    // lower right pixel
    nearest_color(out + ch * (width - 1), err, f, rf);

  } while(0);

  // copy alpha channel if needed
  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

#if defined(__SSE2__)
static void process_floyd_steinberg_sse2(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                         const void *const ivoid, void *const ovoid,
                                         const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_dither_data_t *data = (dt_iop_dither_data_t *)piece->data;

  const int width = roi_in->width;
  const int height = roi_in->height;
  const int ch = piece->colors;
  const float scale = roi_in->scale / piece->iscale;
  const int l1 = floorf(1.0f + dt_log2f(1.0f / scale));

  _find_nearest_color_sse *nearest_color = NULL;
  unsigned int levels = 1;
  int bds = (piece->pipe->type != DT_DEV_PIXELPIPE_EXPORT) ? l1 * l1 : 1;

  switch(data->dither_type)
  {
    case DITHER_FS1BIT:
      nearest_color = _find_nearest_color_n_levels_gray_sse;
      levels = MAX(2, MIN(bds + 1, 256));
      break;
    case DITHER_FS4BIT_GRAY:
      nearest_color = _find_nearest_color_n_levels_gray_sse;
      levels = MAX(16, MIN(15 * bds + 1, 256));
      break;
    case DITHER_FS8BIT:
      nearest_color = _find_nearest_color_n_levels_rgb_sse2;
      levels = 256;
      break;
    case DITHER_FS16BIT:
      nearest_color = _find_nearest_color_n_levels_rgb_sse2;
      levels = 65536;
      break;
    case DITHER_FSAUTO:
      switch(piece->pipe->levels & IMAGEIO_CHANNEL_MASK)
      {
        case IMAGEIO_RGB:
          nearest_color = _find_nearest_color_n_levels_rgb_sse2;
          break;
        case IMAGEIO_GRAY:
          nearest_color = _find_nearest_color_n_levels_gray_sse;
          break;
      }

      switch(piece->pipe->levels & IMAGEIO_PREC_MASK)
      {
        case IMAGEIO_INT8:
          levels = 256;
          break;
        case IMAGEIO_INT12:
          levels = 4096;
          break;
        case IMAGEIO_INT16:
          levels = 65536;
          break;
        case IMAGEIO_BW:
          levels = 2;
          break;
        case IMAGEIO_INT32:
        case IMAGEIO_FLOAT:
        default:
          nearest_color = NULL;
          break;
      }
      // no automatic dithering for preview and thumbnail
      if(piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW || piece->pipe->type == DT_DEV_PIXELPIPE_THUMBNAIL)
        nearest_color = NULL;
      break;
    case DITHER_RANDOM:
      // this function won't ever be called for that type
      // instead, process_random() will be called
      __builtin_unreachable();
      break;
  }

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j = 0; j < height; j++)
  {
    const float *in = (const float *)ivoid + (size_t)ch * width * j;
    float *out = (float *)ovoid + (size_t)ch * width * j;
    for(int i = 0; i < width; i++, in += ch, out += ch)
    {
      out[0] = clipnan(in[0]);
      out[1] = clipnan(in[1]);
      out[2] = clipnan(in[2]);
    }
  }

  if(nearest_color == NULL) return;

  const float f = levels - 1;
  const float rf = 1.0 / f;
  __m128 err;

  // dither without error diffusion on very tiny images
  if(width < 3 || height < 3)
  {
    for(int j = 0; j < height; j++)
    {
      float *out = ((float *)ovoid) + (size_t)ch * j * width;
      for(int i = 0; i < width; i++) (void)nearest_color(out + ch * i, f, rf);
    }

    if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
    return;
  }

  // floyd-steinberg dithering follows here

  // first height-1 rows
  for(int j = 0; j < height - 1; j++)
  {
    float *out = ((float *)ovoid) + (size_t)ch * j * width;

    // first column
    err = nearest_color(out, f, rf);
    _diffuse_error_sse(out + ch, err, 7.0f / 16.0f);
    _diffuse_error_sse(out + ch * width, err, 5.0f / 16.0f);
    _diffuse_error_sse(out + ch * (width + 1), err, 1.0f / 16.0f);


    // main part of image
    for(int i = 1; i < width - 1; i++)
    {
      err = nearest_color(out + ch * i, f, rf);
      _diffuse_error_sse(out + ch * (i + 1), err, 7.0f / 16.0f);
      _diffuse_error_sse(out + ch * (i - 1) + ch * width, err, 3.0f / 16.0f);
      _diffuse_error_sse(out + ch * i + ch * width, err, 5.0f / 16.0f);
      _diffuse_error_sse(out + ch * (i + 1) + ch * width, err, 1.0f / 16.0f);
    }

    // last column
    err = nearest_color(out + ch * (width - 1), f, rf);
    _diffuse_error_sse(out + ch * (width - 2) + ch * width, err, 3.0f / 16.0f);
    _diffuse_error_sse(out + ch * (width - 1) + ch * width, err, 5.0f / 16.0f);
  }

  // last row
  do
  {
    float *out = ((float *)ovoid) + (size_t)ch * (height - 1) * width;

    // lower left pixel
    err = nearest_color(out, f, rf);
    _diffuse_error_sse(out + ch, err, 7.0f / 16.0f);

    // main part of last row
    for(int i = 1; i < width - 1; i++)
    {
      err = nearest_color(out + ch * i, f, rf);
      _diffuse_error_sse(out + ch * (i + 1), err, 7.0f / 16.0f);
    }

    // lower right pixel
    (void)nearest_color(out + ch * (width - 1), f, rf);

  } while(0);

  // copy alpha channel if needed
  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}
#endif


static void encrypt_tea(unsigned int *arg)
{
  const unsigned int key[] = { 0xa341316c, 0xc8013ea4, 0xad90777d, 0x7e95761e };
  unsigned int v0 = arg[0], v1 = arg[1];
  unsigned int sum = 0;
  unsigned int delta = 0x9e3779b9;
  for(int i = 0; i < TEA_ROUNDS; i++)
  {
    sum += delta;
    v0 += ((v1 << 4) + key[0]) ^ (v1 + sum) ^ ((v1 >> 5) + key[1]);
    v1 += ((v0 << 4) + key[2]) ^ (v0 + sum) ^ ((v0 >> 5) + key[3]);
  }
  arg[0] = v0;
  arg[1] = v1;
}


static float tpdf(unsigned int urandom)
{
  float frandom = (float)urandom / 0xFFFFFFFFu;

  return (frandom < 0.5f ? (sqrtf(2.0f * frandom) - 1.0f) : (1.0f - sqrtf(2.0f * (1.0f - frandom))));
}


static void process_random(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                           const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                           const dt_iop_roi_t *const roi_out)
{
  dt_iop_dither_data_t *data = (dt_iop_dither_data_t *)piece->data;

  const int width = roi_in->width;
  const int height = roi_in->height;
  const int ch = piece->colors;

  const float dither = powf(2.0f, data->random.damping / 10.0f);

  unsigned int *const tea_states = calloc(2 * dt_get_num_threads(), sizeof(unsigned int));

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j = 0; j < height; j++)
  {
    const size_t k = (size_t)ch * width * j;
    const float *in = (const float *)ivoid + k;
    float *out = (float *)ovoid + k;
    unsigned int *tea_state = tea_states + 2 * dt_get_thread_num();
    tea_state[0] = j * height + dt_get_thread_num();
    for(int i = 0; i < width; i++, in += ch, out += ch)
    {
      encrypt_tea(tea_state);
      float dith = dither * tpdf(tea_state[0]);

      out[0] = CLIP(in[0] + dith);
      out[1] = CLIP(in[1] + dith);
      out[2] = CLIP(in[2] + dith);
    }
  }

  free(tea_states);

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, width, height);
}


void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_dither_data_t *data = (dt_iop_dither_data_t *)piece->data;

  if(data->dither_type == DITHER_RANDOM)
    process_random(self, piece, ivoid, ovoid, roi_in, roi_out);
  else
    process_floyd_steinberg(self, piece, ivoid, ovoid, roi_in, roi_out);
}

#if defined(__SSE2__)
void process_sse2(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                  void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_dither_data_t *data = (dt_iop_dither_data_t *)piece->data;

  if(data->dither_type == DITHER_RANDOM)
    process_random(self, piece, ivoid, ovoid, roi_in, roi_out);
  else
    process_floyd_steinberg_sse2(self, piece, ivoid, ovoid, roi_in, roi_out);
}
#endif

static void method_callback(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_dither_params_t *p = (dt_iop_dither_params_t *)self->params;
  dt_iop_dither_gui_data_t *g = (dt_iop_dither_gui_data_t *)self->gui_data;
  p->dither_type = dt_bauhaus_combobox_get(widget);

  if(p->dither_type == DITHER_RANDOM)
    gtk_widget_show(GTK_WIDGET(g->random));
  else
    gtk_widget_hide(GTK_WIDGET(g->random));

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

#if 0
static void
radius_callback (GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_dither_params_t *p = (dt_iop_dither_params_t *)self->params;
  p->random.radius = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
#endif

static void damping_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_dither_params_t *p = (dt_iop_dither_params_t *)self->params;
  p->random.damping = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

#if 0
static void
range_callback (GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_dither_params_t *p = (dt_iop_dither_params_t *)self->params;
  p->random.range[0] = dtgtk_gradient_slider_multivalue_get_value(DTGTK_GRADIENT_SLIDER(slider), 0);
  p->random.range[1] = dtgtk_gradient_slider_multivalue_get_value(DTGTK_GRADIENT_SLIDER(slider), 1);
  p->random.range[2] = dtgtk_gradient_slider_multivalue_get_value(DTGTK_GRADIENT_SLIDER(slider), 2);
  p->random.range[3] = dtgtk_gradient_slider_multivalue_get_value(DTGTK_GRADIENT_SLIDER(slider), 3);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
#endif

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_dither_params_t *p = (dt_iop_dither_params_t *)p1;
  dt_iop_dither_data_t *d = (dt_iop_dither_data_t *)piece->data;

  d->dither_type = p->dither_type;
  memcpy(&(d->random.range), &(p->random.range), sizeof(p->random.range));
  d->random.radius = p->random.radius;
  d->random.damping = p->random.damping;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_dither_data_t));
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
  dt_iop_dither_gui_data_t *g = (dt_iop_dither_gui_data_t *)self->gui_data;
  dt_iop_dither_params_t *p = (dt_iop_dither_params_t *)module->params;
  dt_bauhaus_combobox_set(g->dither_type, p->dither_type);
#if 0
  dt_bauhaus_slider_set(g->radius, p->random.radius);

  dtgtk_gradient_slider_multivalue_set_value(DTGTK_GRADIENT_SLIDER(g->range), p->random.range[0], 0);
  dtgtk_gradient_slider_multivalue_set_value(DTGTK_GRADIENT_SLIDER(g->range), p->random.range[1], 1);
  dtgtk_gradient_slider_multivalue_set_value(DTGTK_GRADIENT_SLIDER(g->range), p->random.range[2], 2);
  dtgtk_gradient_slider_multivalue_set_value(DTGTK_GRADIENT_SLIDER(g->range), p->random.range[3], 3);
#endif

  dt_bauhaus_slider_set(g->damping, p->random.damping);

  if(p->dither_type == DITHER_RANDOM)
    gtk_widget_show(GTK_WIDGET(g->random));
  else
    gtk_widget_hide(GTK_WIDGET(g->random));
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_dither_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_dither_params_t));
  module->default_enabled = 0;
  module->priority = 985; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_dither_params_t);
  module->gui_data = NULL;
  dt_iop_dither_params_t tmp
      = (dt_iop_dither_params_t){ DITHER_FSAUTO, 0, { 0.0f, { 0.0f, 0.0f, 1.0f, 1.0f }, -200.0f } };
  memcpy(module->params, &tmp, sizeof(dt_iop_dither_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_dither_params_t));
}


void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_dither_gui_data_t));
  dt_iop_dither_gui_data_t *g = (dt_iop_dither_gui_data_t *)self->gui_data;
  dt_iop_dither_params_t *p = (dt_iop_dither_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));
  g->random = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->dither_type = dt_bauhaus_combobox_new(self);
  dt_bauhaus_combobox_add(g->dither_type, _("random"));
  dt_bauhaus_combobox_add(g->dither_type, _("floyd-steinberg 1-bit B&W"));
  dt_bauhaus_combobox_add(g->dither_type, _("floyd-steinberg 4-bit gray"));
  dt_bauhaus_combobox_add(g->dither_type, _("floyd-steinberg 8-bit RGB"));
  dt_bauhaus_combobox_add(g->dither_type, _("floyd-steinberg 16-bit RGB"));
  dt_bauhaus_combobox_add(g->dither_type, _("floyd-steinberg auto"));
  dt_bauhaus_widget_set_label(g->dither_type, NULL, _("method"));

#if 0
  g->radius = dt_bauhaus_slider_new_with_range(self, 0.0, 200.0, 0.1, p->random.radius, 2);
  gtk_widget_set_tooltip_text(g->radius, _("radius for blurring step"));
  dt_bauhaus_widget_set_label(g->radius, NULL, _("radius"));

  g->range = dtgtk_gradient_slider_multivalue_new(4);
  dtgtk_gradient_slider_multivalue_set_marker(DTGTK_GRADIENT_SLIDER(g->range), GRADIENT_SLIDER_MARKER_LOWER_OPEN_BIG, 0);
  dtgtk_gradient_slider_multivalue_set_marker(DTGTK_GRADIENT_SLIDER(g->range), GRADIENT_SLIDER_MARKER_UPPER_FILLED_BIG, 1);
  dtgtk_gradient_slider_multivalue_set_marker(DTGTK_GRADIENT_SLIDER(g->range), GRADIENT_SLIDER_MARKER_UPPER_FILLED_BIG, 2);
  dtgtk_gradient_slider_multivalue_set_marker(DTGTK_GRADIENT_SLIDER(g->range), GRADIENT_SLIDER_MARKER_LOWER_OPEN_BIG, 3);
  dtgtk_gradient_slider_multivalue_set_value(DTGTK_GRADIENT_SLIDER(g->range), p->random.range[0], 0);
  dtgtk_gradient_slider_multivalue_set_value(DTGTK_GRADIENT_SLIDER(g->range), p->random.range[1], 1);
  dtgtk_gradient_slider_multivalue_set_value(DTGTK_GRADIENT_SLIDER(g->range), p->random.range[2], 2);
  dtgtk_gradient_slider_multivalue_set_value(DTGTK_GRADIENT_SLIDER(g->range), p->random.range[3], 3);
  gtk_widget_set_tooltip_text(g->range, _("the gradient range where to apply random dither"));
  g->range_label = gtk_label_new(_("gradient range"));

  GtkWidget *rlabel = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
  gtk_box_pack_start(GTK_BOX(rlabel), GTK_WIDGET(g->range_label), FALSE, FALSE, 0);
#endif

  g->damping = dt_bauhaus_slider_new_with_range(self, -200.0, 0.0, 1.0, p->random.damping, 3);
  gtk_widget_set_tooltip_text(g->damping, _("damping level of random dither"));
  dt_bauhaus_widget_set_label(g->damping, NULL, _("damping"));
  dt_bauhaus_slider_set_format(g->damping, "%.0fdB");

#if 0
  gtk_box_pack_start(GTK_BOX(g->random), g->radius, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->random), rlabel, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->random), g->range, TRUE, TRUE, 0);
#endif
  gtk_box_pack_start(GTK_BOX(g->random), g->damping, TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), g->dither_type, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->random, TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(g->dither_type), "value-changed", G_CALLBACK(method_callback), self);
#if 0
  g_signal_connect (G_OBJECT (g->radius), "value-changed",
                    G_CALLBACK (radius_callback), self);
  g_signal_connect (G_OBJECT (g->range), "value-changed",
                    G_CALLBACK (range_callback), self);
#endif
  g_signal_connect(G_OBJECT(g->damping), "value-changed", G_CALLBACK(damping_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
