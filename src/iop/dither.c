/*
    This file is part of darktable,
    Copyright (C) 2012-2023 darktable developers.

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
#include "common/imagebuf.h"
#include "common/math.h"
#include "common/opencl.h"
#include "common/tea.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "dtgtk/gradientslider.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "imageio/imageio_common.h"
#include "iop/iop_api.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>
#include <inttypes.h>

DT_MODULE_INTROSPECTION(1, dt_iop_dither_params_t)

typedef enum dt_iop_dither_type_t
{
  DITHER_RANDOM,      // $DESCRIPTION: "random"
  DITHER_FS1BIT,      // $DESCRIPTION: "Floyd-Steinberg 1-bit B&W"
  DITHER_FS4BIT_GRAY, // $DESCRIPTION: "Floyd-Steinberg 4-bit gray"
  DITHER_FS8BIT,      // $DESCRIPTION: "Floyd-Steinberg 8-bit RGB"
  DITHER_FS16BIT,     // $DESCRIPTION: "Floyd-Steinberg 16-bit RGB"
  DITHER_FSAUTO       // $DESCRIPTION: "Floyd-Steinberg auto"
} dt_iop_dither_type_t;


typedef struct dt_iop_dither_params_t
{
  dt_iop_dither_type_t dither_type; // $DEFAULT: DITHER_FSAUTO $DESCRIPTION: "method"
  int palette; // reserved for future extensions
  struct
  {
    float radius;   // reserved for future extensions
    float range[4]; // reserved for future extensions {0,0,1,1}
    float damping;  // $MIN: -200.0 $MAX: 0.0 $DEFAULT: -200.0 $DESCRIPTION: "damping"
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

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("reduce banding and posterization effects in output JPEGs by adding random noise"),
                                      _("corrective"),
                                      _("non-linear, RGB, display-referred"),
                                      _("non-linear, RGB"),
                                      _("non-linear, RGB, display-referred"));
}

int default_group()
{
  return IOP_GROUP_CORRECT | IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_ONE_INSTANCE;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_database_start_transaction(darktable.db);

  dt_iop_dither_params_t tmp
      = (dt_iop_dither_params_t){ DITHER_FSAUTO, 0, { 0.0f, { 0.0f, 0.0f, 1.0f, 1.0f }, -200.0f } };
  // add the preset.
  dt_gui_presets_add_generic(_("dither"), self->op,
                             self->version(), &tmp, sizeof(dt_iop_dither_params_t), 1,
                             DEVELOP_BLEND_CS_NONE);
  // make it auto-apply for all images:
  // dt_gui_presets_update_autoapply(_("dither"), self->op, self->version(), 1);

  dt_database_release_transaction(darktable.db);
}

#ifdef _OPENMP
#pragma omp declare simd simdlen(4)
#endif
static inline float _quantize(const float val, const float f, const float rf)
{
  return rf * ceilf((val * f) - 0.5); // round up only if frac(x) strictly greater than 0.5
  //originally (and more slowly):
  //const float tmp = val * f;
  //const float itmp = floorf(tmp);
  //return rf * (itmp + ((tmp - itmp > 0.5f) ? 1.0f : 0.0f));
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float _rgb_to_gray(const float *const restrict val)
{
  return 0.30f * val[0] + 0.59f * val[1] + 0.11f * val[2]; // RGB -> GRAY
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void _nearest_color(
        float *const restrict val,
        float *const restrict err,
        const int graymode,
        const float f,
        const float rf)
{
  if(graymode)
  {
    // dither pixel into gray, with f=levels-1 and rf=1/f, return err=old-new
    const float in = _rgb_to_gray(val);
    const float new = _quantize(in,f,rf);

#ifdef _OPENMP
#pragma omp simd aligned(val, err : 16)
#endif
    for(int c = 0; c < 4; c++)
    {
      err[c] = val[c] - new;
      val[c] = new;
    }
  }
  else
  {
    // dither pixel into RGB, with f=levels-1 and rf=1/f, return err=old-new
#ifdef _OPENMP
#pragma omp simd aligned(val, err : 16)
#endif
  for(int c = 0; c < 4; c++)
  {
    const float old = val[c];
    const float new = _quantize(old, f, rf);
    err[c] = old - new;
    val[c] = new;
  }
  }
}

static inline void _diffuse_error(
        float *const restrict val,
        const float *const restrict err,
        const float factor)
{
#ifdef _OPENMP
#pragma omp simd aligned(val, err)
#endif
  for(int c = 0; c < 4; c++)
  {
    val[c] += err[c] * factor;
  }
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float _clipnan(const float x)
{
  // convert NaN to 0.5, otherwise clamp to between 0.0 and 1.0
  return (x >= 0.0f) ? ((x < 1.0f) ? x    // 0 <= x < 1
                                   : 1.0f // x >= 1
                       )
         : dt_isnan(x) ? 0.5f  // x is NaN
                       : 0.0f; // x <= 0
}

static inline void _clipnan_pixel(
        float *const restrict out,
        const float *const restrict in)
{
#ifdef _OPENMP
#pragma omp simd aligned(in, out : 16)
#endif
  for(int c = 0; c < 4; c++)
    out[c] = _clipnan(in[c]);
}

static int _get_dither_parameters(
        const dt_iop_dither_data_t *const data,
        const dt_dev_pixelpipe_iop_t *const piece,
        const float scale,
        unsigned int *const restrict levels)
{
  int graymode = -1;
  *levels = 65536;
  const int l1 = floorf(1.0f + dt_log2f(1.0f / scale));
  const int bds = (piece->pipe->type & DT_DEV_PIXELPIPE_EXPORT) ? 1 : l1 * l1;

  switch(data->dither_type)
  {
    case DITHER_FS1BIT:
      graymode = 1;
      *levels = MAX(2, MIN(bds + 1, 256));
      break;
    case DITHER_FS4BIT_GRAY:
      graymode = 1;
      *levels = MAX(16, MIN(15 * bds + 1, 256));
      break;
    case DITHER_FS8BIT:
      graymode = 0;
      *levels = 256;
      break;
    case DITHER_FS16BIT:
      graymode = 0;
      *levels = 65536;
      break;
    case DITHER_FSAUTO:
      switch(piece->pipe->levels & IMAGEIO_CHANNEL_MASK)
      {
        case IMAGEIO_RGB:
          graymode = 0;
          break;
        case IMAGEIO_GRAY:
          graymode = 1;
          break;
      }

      switch(piece->pipe->levels & IMAGEIO_PREC_MASK)
      {
        case IMAGEIO_INT8:
          *levels = 256;
          break;
        case IMAGEIO_INT10:
          *levels = 1024;
          break;
        case IMAGEIO_INT12:
          *levels = 4096;
          break;
        case IMAGEIO_INT16:
          *levels = 65536;
          break;
        case IMAGEIO_BW:
          *levels = 2;
          break;
        case IMAGEIO_INT32:
        case IMAGEIO_FLOAT:
        default:
          graymode = -1;
          break;
      }
      // no automatic dithering for preview and thumbnail
      if(piece->pipe->type & (DT_DEV_PIXELPIPE_PREVIEW | DT_DEV_PIXELPIPE_PREVIEW2 | DT_DEV_PIXELPIPE_THUMBNAIL))
      {
        graymode = -1;
      }
      break;
    case DITHER_RANDOM:
      // this function won't ever be called for that type
      // instead, process_random() will be called
      __builtin_unreachable();
      break;
  }
  return graymode;
}

// what fraction of the error to spread to each neighbor pixel
#define RIGHT_WT      (7.0f/16.0f)
#define DOWNRIGHT_WT  (1.0f/16.0f)
#define DOWN_WT       (5.0f/16.0f)
#define DOWNLEFT_WT   (3.0f/16.0f)

#ifdef _OPENMP
#pragma omp declare simd aligned(ivoid, ovoid : 64)
#endif
static void process_floyd_steinberg(
        struct dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        const void *const ivoid,
        void *const ovoid,
        const dt_iop_roi_t *const roi_in,
        const dt_iop_roi_t *const roi_out,
        const gboolean fast_mode)
{
  const dt_iop_dither_data_t *const restrict data = (dt_iop_dither_data_t *)piece->data;

  const int width = roi_in->width;
  const int height = roi_in->height;
  const float scale = roi_in->scale / piece->iscale;

  const float *const restrict in = (const float *)ivoid;
  float *const restrict out = (float *)ovoid;

  unsigned int levels = 1;
  const int graymode = _get_dither_parameters(data,piece,scale,&levels);
  if(graymode < 0)
  {
    for(int j = 0; j < height * width; j++)
      _clipnan_pixel(out + 4*j, in + 4*j);
    return;
  }

  const float f = levels - 1;
  const float rf = 1.0 / f;
  dt_aligned_pixel_t err;

  // dither without error diffusion on very tiny images
  if(width < 3 || height < 3)
  {
    for(int j = 0; j < height * width; j++)
    {
      _clipnan_pixel(out + 4 * j, in + 4 * j);
      _nearest_color(out + 4 * j, err, graymode, f, rf);
    }
    return;
  }

  // offsets to neighboring pixels
  const size_t right = 4;
  const size_t downleft = 4 * (width-1);
  const size_t down = 4 * width;
  const size_t downright = 4 * (width+1);

#define PROCESS_PIXEL_FULL(_pixel, inpix)                               \
  {                                                                     \
    float *const pixel_ = (_pixel);                                     \
    _nearest_color(pixel_, err, graymode, f, rf);              /* quantize pixel */ \
    _clipnan_pixel(pixel_ + downright,(inpix) + downright);    /* prepare downright for first access */ \
    _diffuse_error(pixel_ + right, err, RIGHT_WT);            /* diffuse quantization error to neighbors */ \
    _diffuse_error(pixel_ + downleft, err, DOWNLEFT_WT);                \
    _diffuse_error(pixel_ + down, err, DOWN_WT);                        \
    _diffuse_error(pixel_ + downright, err, DOWNRIGHT_WT);              \
  }

#define PROCESS_PIXEL_LEFT(_pixel, inpix)                               \
  {                                                                     \
    float *const pixel_ = (_pixel);                                     \
    _nearest_color(pixel_, err, graymode, f, rf);              /* quantize pixel */ \
    _clipnan_pixel(pixel_ + down,(inpix) + down);              /* prepare down for first access */ \
    _clipnan_pixel(pixel_ + downright,(inpix) + downright);    /* prepare downright for first access */ \
    _diffuse_error(pixel_ + right, err, RIGHT_WT);            /* diffuse quantization error to neighbors */ \
    _diffuse_error(pixel_ + down, err, DOWN_WT);                        \
    _diffuse_error(pixel_ + downright, err, DOWNRIGHT_WT);              \
  }

#define PROCESS_PIXEL_RIGHT(pixel)                                      \
  _nearest_color(pixel, err, graymode, f, rf);             /* quantize pixel */ \
  _diffuse_error(pixel + downleft, err, DOWNLEFT_WT);     /* diffuse quantization error to neighbors */ \
  _diffuse_error(pixel + down, err, DOWN_WT);

  // once the FS dithering gets started, we can copy&clip the downright pixel, as that will be the first time
  // it will be accessed.  But to get the process started, we need to prepare the top row of pixels
#ifdef _OPENMP
#pragma omp simd aligned(in, out : 64)
#endif
  for(int j = 0; j < width; j++)
  {
    _clipnan_pixel(out + 4*j, in + 4*j);
  }

  // floyd-steinberg dithering follows here

  if(fast_mode)
  {
    // do the bulk of the image (all except the last one or two rows)
    for(int j = 0; j < height - 2; j += 2)
    {
      const float *const restrict inrow = in + (size_t)4 * j * width;
      float *const restrict outrow = out + (size_t)4 * j * width;

      // first two columns
      PROCESS_PIXEL_LEFT(outrow, inrow);                          // leftmost pixel in first (upper) row
      PROCESS_PIXEL_FULL(outrow + right, inrow + right);          // second pixel in first (upper) row
      PROCESS_PIXEL_LEFT(outrow + down, inrow + down);            // leftmost in second (lower) row

      // main part of the current pair of rows
      for(int i = 1; i < width - 1; i++)
      {
        float *const restrict pixel = outrow + 4 * i;
        PROCESS_PIXEL_FULL(pixel, inrow + 4 * i);
        PROCESS_PIXEL_FULL(pixel + downleft, inrow + 4 * i + downleft);
      }

      // last column of upper row
      float *const restrict lastpixel = outrow + 4 * (width-1);
      PROCESS_PIXEL_RIGHT(lastpixel);
      // we have two pixels left over in the lower row
      const float *const restrict lower_in = inrow + 4 * (width-1) + downleft;
      PROCESS_PIXEL_FULL(lastpixel + downleft, lower_in);
      // and now process the final pixel in the lower row
      PROCESS_PIXEL_RIGHT(lastpixel + down);
    }

    // next-to-last row, if the total number of rows is even
    if((height & 1) == 0)
    {
      const float *const restrict inrow = in + (size_t)4 * (height - 2) * width;
      float *const restrict outrow = out + (size_t)4 * (height - 2) * width;

      // first column
      PROCESS_PIXEL_LEFT(outrow, inrow);

      // main part of image
      for(int i = 1; i < width - 1; i++)
      {
        PROCESS_PIXEL_FULL(outrow + 4 * i, inrow + 4 * i);
      }

      // last column
      PROCESS_PIXEL_RIGHT(outrow + 4 * (width-1));
    }
  }
  else // use slower version which generates output identical to previous releases
  {
    // do the bulk of the image (all except the last row)
    for(int j = 0; j < height - 1; j++)
    {
      const float *const restrict inrow = in + (size_t)4 * j * width;
      float *const restrict outrow = out + (size_t)4 * j * width;

      // first two columns
      PROCESS_PIXEL_LEFT(outrow, inrow);                          // leftmost pixel in first (upper) row

      // main part of the current row
      for(int i = 1; i < width - 1; i++)
      {
        PROCESS_PIXEL_FULL(outrow + 4 * i, inrow + 4 * i);
      }

      // last column of upper row
      PROCESS_PIXEL_RIGHT(outrow + 4 * (width-1));
    }
  }

  // final row
  {
    float *const restrict outrow = out + (size_t)4 * (height - 1) * width;

    // last row except for the right-most pixel
    for(int i = 0; i < width - 1; i++)
    {
      float *const restrict pixel = outrow + 4 * i;
      _nearest_color(pixel, err, graymode, f, rf);              // quantize the pixel
      _diffuse_error(pixel + right, err, RIGHT_WT);            // spread error to only remaining neighbor
    }

    // lower right pixel
    _nearest_color(outrow + 4 * (width - 1), err, graymode, f, rf);  // quantize the last pixel, no neighbors left
  }
}

static void process_random(
        struct dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        const void *const ivoid,
        void *const ovoid,
        const dt_iop_roi_t *const roi_in,
        const dt_iop_roi_t *const roi_out)
{
  const dt_iop_dither_data_t *const data = (dt_iop_dither_data_t *)piece->data;

  const int width = roi_in->width;
  const int height = roi_in->height;
  assert(piece->colors == 4);

  const float dither = powf(2.0f, data->random.damping / 10.0f);

  unsigned int *const tea_states = alloc_tea_states(dt_get_num_threads());

#ifdef _OPENMP
#pragma omp parallel default(none) \
  dt_omp_firstprivate(dither, height, width) \
  dt_omp_sharedconst(tea_states, ivoid, ovoid)
#endif
  {
    // get a pointer to each thread's private buffer *outside* the for loop, to avoid a function call per iteration
    unsigned int *const tea_state = get_tea_state(tea_states,dt_get_thread_num());
#ifdef _OPENMP
#pragma omp for schedule(static)
#endif
    for(int j = 0; j < height; j++)
    {
      const size_t k = (size_t)4 * width * j;
      const float *const in = (const float *)ivoid + k;
      float *const out = (float *)ovoid + k;
      tea_state[0] = j * height; /* + dt_get_thread_num() -- do not include, makes results unreproducible */
      for(int i = 0; i < width; i++)
      {
        encrypt_tea(tea_state);
        float dith = dither * tpdf(tea_state[0]);

#ifdef _OPENMP
#pragma omp simd aligned(in, out : 64)
#endif
        for(int c = 0; c < 4; c++)
        {
          out[4*i+c] = CLIP(in[4*i+c] + dith);
        }
      }
    }
  }
  free_tea_states(tea_states);
}


void process(
        struct dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        const void *const ivoid,
        void *const ovoid,
        const dt_iop_roi_t *const roi_in,
        const dt_iop_roi_t *const roi_out)
{
  dt_iop_dither_data_t *data = (dt_iop_dither_data_t *)piece->data;

  if(data->dither_type == DITHER_RANDOM)
    process_random(self, piece, ivoid, ovoid, roi_in, roi_out);
  else
  {
    const gboolean fastmode = piece->pipe->type & DT_DEV_PIXELPIPE_FAST;
    process_floyd_steinberg(self, piece, ivoid, ovoid, roi_in, roi_out, fastmode);
  }
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_dither_params_t *p = (dt_iop_dither_params_t *)self->params;
  dt_iop_dither_gui_data_t *g = (dt_iop_dither_gui_data_t *)self->gui_data;

  if(w == g->dither_type)
  {
    gtk_widget_set_visible(g->random, p->dither_type == DITHER_RANDOM);
  }
}

#if 0
static void
radius_callback (GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_dither_params_t *p = (dt_iop_dither_params_t *)self->params;
  p->random.radius = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
#endif

#if 0
static void
range_callback (GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_dither_params_t *p = (dt_iop_dither_params_t *)self->params;
  p->random.range[0] = dtgtk_gradient_slider_multivalue_get_value(DTGTK_GRADIENT_SLIDER(slider), 0);
  p->random.range[1] = dtgtk_gradient_slider_multivalue_get_value(DTGTK_GRADIENT_SLIDER(slider), 1);
  p->random.range[2] = dtgtk_gradient_slider_multivalue_get_value(DTGTK_GRADIENT_SLIDER(slider), 2);
  p->random.range[3] = dtgtk_gradient_slider_multivalue_get_value(DTGTK_GRADIENT_SLIDER(slider), 3);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
#endif

void commit_params(
        struct dt_iop_module_t *self,
        dt_iop_params_t *p1,
        dt_dev_pixelpipe_t *pipe,
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
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}


void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_dither_gui_data_t *g = (dt_iop_dither_gui_data_t *)self->gui_data;
  dt_iop_dither_params_t *p = (dt_iop_dither_params_t *)self->params;
#if 0
  dt_bauhaus_slider_set(g->radius, p->random.radius);

  dtgtk_gradient_slider_multivalue_set_value(DTGTK_GRADIENT_SLIDER(g->range), p->random.range[0], 0);
  dtgtk_gradient_slider_multivalue_set_value(DTGTK_GRADIENT_SLIDER(g->range), p->random.range[1], 1);
  dtgtk_gradient_slider_multivalue_set_value(DTGTK_GRADIENT_SLIDER(g->range), p->random.range[2], 2);
  dtgtk_gradient_slider_multivalue_set_value(DTGTK_GRADIENT_SLIDER(g->range), p->random.range[3], 3);
#endif

  gtk_widget_set_visible(g->random, p->dither_type == DITHER_RANDOM);
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_dither_gui_data_t *g = IOP_GUI_ALLOC(dither);

  g->random = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

#if 0
  g->radius = dt_bauhaus_slider_new_with_range(self, 0.0, 200.0, 0.1, p->random.radius, 2);
  gtk_widget_set_tooltip_text(g->radius, _("radius for blurring step"));
  dt_bauhaus_widget_set_label(g->radius, NULL, N_("radius"));

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

  g->damping = dt_bauhaus_slider_from_params(self, "random.damping");

  gtk_widget_set_tooltip_text(g->damping, _("damping level of random dither"));
  dt_bauhaus_slider_set_digits(g->damping, 3);
  dt_bauhaus_slider_set_format(g->damping, " dB");

#if 0
  gtk_box_pack_start(GTK_BOX(g->random), g->radius, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->random), rlabel, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->random), g->range, TRUE, TRUE, 0);
#endif

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->dither_type = dt_bauhaus_combobox_from_params(self, "dither_type");

  gtk_box_pack_start(GTK_BOX(self->widget), g->random, TRUE, TRUE, 0);

#if 0
  g_signal_connect (G_OBJECT (g->radius), "value-changed",
                    G_CALLBACK (radius_callback), self);
  g_signal_connect (G_OBJECT (g->range), "value-changed",
                    G_CALLBACK (range_callback), self);
#endif
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

