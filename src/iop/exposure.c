/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.
    copyright (c) 2014-2016 Roman Lebedev.

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

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif

#include "bauhaus/bauhaus.h"
#include "common/histogram.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/pixelpipe.h"
#include "dtgtk/paint.h"
#include "dtgtk/resetlabel.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"
#include "common/iop_group.h"

#define exposure2white(x) exp2f(-(x))
#define white2exposure(x) -dt_log2f(fmaxf(1e-20f, x))

DT_MODULE_INTROSPECTION(5, dt_iop_exposure_params_t)

typedef enum dt_iop_exposure_mode_t
{
  EXPOSURE_MODE_MANUAL,
  EXPOSURE_MODE_DEFLICKER
} dt_iop_exposure_mode_t;

// uint16_t pixel can have any value in range [0, 65535], thus, there is
// 65536 possible values.
#define DEFLICKER_BINS_COUNT (UINT16_MAX + 1)

typedef struct dt_iop_exposure_params_t
{
  dt_iop_exposure_mode_t mode;
  float black;
  float exposure;
  float deflicker_percentile, deflicker_target_level;
} dt_iop_exposure_params_t;

typedef struct dt_iop_exposure_gui_data_t
{
  GList *modes;
  GtkWidget *mode;
  GtkWidget *black;
  GtkWidget *mode_stack;
  GtkWidget *exposure;
  GtkWidget *autoexpp;
  GtkWidget *deflicker_percentile;
  GtkWidget *deflicker_target_level;
  uint32_t *deflicker_histogram; // used to cache histogram of source file
  dt_dev_histogram_stats_t deflicker_histogram_stats;
  GtkLabel *deflicker_used_EC;
  float deflicker_computed_exposure;
  dt_pthread_mutex_t lock;
} dt_iop_exposure_gui_data_t;

typedef struct dt_iop_exposure_data_t
{
  dt_iop_exposure_params_t params;
  int deflicker;
  float black;
  float scale;
} dt_iop_exposure_data_t;

typedef struct dt_iop_exposure_global_data_t
{
  int kernel_exposure;
} dt_iop_exposure_global_data_t;


const char *name()
{
  return _("exposure");
}

int groups()
{
  return dt_iop_get_group("exposure", IOP_GROUP_BASIC);
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_SUPPORTS_BLENDING;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "mode"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "black"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "exposure"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "auto-exposure"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "percentile"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "target level"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;

  dt_accel_connect_slider_iop(self, "mode", GTK_WIDGET(g->mode));
  dt_accel_connect_slider_iop(self, "black", GTK_WIDGET(g->black));
  dt_accel_connect_slider_iop(self, "exposure", GTK_WIDGET(g->exposure));
  dt_accel_connect_slider_iop(self, "auto-exposure", GTK_WIDGET(g->autoexpp));
  dt_accel_connect_slider_iop(self, "percentile", GTK_WIDGET(g->deflicker_percentile));
  dt_accel_connect_slider_iop(self, "target level", GTK_WIDGET(g->deflicker_target_level));
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 2 && new_version == 5)
  {
    typedef struct dt_iop_exposure_params_v2_t
    {
      float black, exposure, gain;
    } dt_iop_exposure_params_v2_t;

    dt_iop_exposure_params_v2_t *o = (dt_iop_exposure_params_v2_t *)old_params;
    dt_iop_exposure_params_t *n = (dt_iop_exposure_params_t *)new_params;
    dt_iop_exposure_params_t *d = (dt_iop_exposure_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    n->black = o->black;
    n->exposure = o->exposure;
    return 0;
  }
  if(old_version == 3 && new_version == 5)
  {
    typedef struct dt_iop_exposure_params_v3_t
    {
      float black, exposure;
      gboolean deflicker;
      float deflicker_percentile, deflicker_target_level;
    } dt_iop_exposure_params_v3_t;

    dt_iop_exposure_params_v3_t *o = (dt_iop_exposure_params_v3_t *)old_params;
    dt_iop_exposure_params_t *n = (dt_iop_exposure_params_t *)new_params;
    dt_iop_exposure_params_t *d = (dt_iop_exposure_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    n->mode = o->deflicker ? EXPOSURE_MODE_DEFLICKER : EXPOSURE_MODE_MANUAL;
    n->black = o->black;
    n->exposure = o->exposure;
    n->deflicker_percentile = o->deflicker_percentile;
    n->deflicker_target_level = o->deflicker_target_level;
    return 0;
  }
  if(old_version == 4 && new_version == 5)
  {
    typedef enum dt_iop_exposure_deflicker_histogram_source_t {
      DEFLICKER_HISTOGRAM_SOURCE_THUMBNAIL,
      DEFLICKER_HISTOGRAM_SOURCE_SOURCEFILE
    } dt_iop_exposure_deflicker_histogram_source_t;

    typedef struct dt_iop_exposure_params_v4_t
    {
      dt_iop_exposure_mode_t mode;
      float black;
      float exposure;
      float deflicker_percentile, deflicker_target_level;
      dt_iop_exposure_deflicker_histogram_source_t deflicker_histogram_source;
    } dt_iop_exposure_params_v4_t;

    dt_iop_exposure_params_v4_t *o = (dt_iop_exposure_params_v4_t *)old_params;
    dt_iop_exposure_params_t *n = (dt_iop_exposure_params_t *)new_params;
    dt_iop_exposure_params_t *d = (dt_iop_exposure_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    n->mode = o->mode;
    n->black = o->black;
    n->exposure = o->exposure;
    n->deflicker_percentile = o->deflicker_percentile;
    n->deflicker_target_level = o->deflicker_target_level;
    // deflicker_histogram_source is dropped. this does change output,
    // but deflicker still was not publicly released at that point
    return 0;
  }
  return 1;
}

void init_presets (dt_iop_module_so_t *self)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "BEGIN", NULL, NULL, NULL);

  dt_gui_presets_add_generic(_("magic lantern defaults"), self->op, self->version(),
                             &(dt_iop_exposure_params_t){.mode = EXPOSURE_MODE_DEFLICKER,
                                                         .black = 0.0f,
                                                         .exposure = 0.0f,
                                                         .deflicker_percentile = 50.0f,
                                                         .deflicker_target_level = -4.0f },
                             sizeof(dt_iop_exposure_params_t), 1);

  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "COMMIT", NULL, NULL, NULL);
}

static void deflicker_prepare_histogram(dt_iop_module_t *self, uint32_t **histogram,
                                        dt_dev_histogram_stats_t *histogram_stats)
{
  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, self->dev->image_storage.id, 'r');
  dt_image_t image = *img;
  dt_image_cache_read_release(darktable.image_cache, img);

  if(image.buf_dsc.channels != 1 || image.buf_dsc.datatype != TYPE_UINT16) return;

  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, self->dev->image_storage.id, DT_MIPMAP_FULL,
                      DT_MIPMAP_BLOCKING, 'r');
  if(!buf.buf)
  {
    dt_control_log(_("failed to get raw buffer from image `%s'"), image.filename);
    dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
    return;
  }

  dt_dev_histogram_collection_params_t histogram_params = { 0 };

  dt_histogram_roi_t histogram_roi = {.width = image.width,
                                      .height = image.height,

                                      // FIXME: get those from rawprepare IOP somehow !!!
                                      .crop_x = image.crop_x,
                                      .crop_y = image.crop_y,
                                      .crop_width = image.crop_width,
                                      .crop_height = image.crop_height };

  histogram_params.roi = &histogram_roi;
  histogram_params.bins_count = DEFLICKER_BINS_COUNT;

  dt_histogram_worker(&histogram_params, histogram_stats, buf.buf, histogram,
                      dt_histogram_helper_cs_RAW_uint16);
  histogram_stats->ch = 1u;

  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
}

/* input: 0 - 65535 (valid range: from black level to white level) */
/* output: -16 ... 0 */
static double raw_to_ev(uint32_t raw, uint32_t black_level, uint32_t white_level)
{
  const uint32_t raw_max = white_level - black_level;

  // we are working on data without black clipping,
  // so we can get values which are lower than the black level !!!
  const int64_t raw_val = MAX((int64_t)raw - (int64_t)black_level, 1);

  const double raw_ev = -log2(raw_max) + log2(raw_val);

  return raw_ev;
}

static void compute_correction(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                               const uint32_t *const histogram,
                               const dt_dev_histogram_stats_t *const histogram_stats, float *correction)
{
  const dt_iop_exposure_params_t *const p = (const dt_iop_exposure_params_t *const)p1;

  *correction = NAN;

  if(histogram == NULL) return;

  const size_t total = (size_t)histogram_stats->ch * histogram_stats->pixels;

  const double thr
      = CLAMP(((double)total * (double)p->deflicker_percentile / (double)100.0), 0.0, (double)total);

  size_t n = 0;
  uint32_t raw = 0;

  for(uint32_t i = 0; i < histogram_stats->bins_count; i++)
  {
    for(uint32_t k = 0; k < histogram_stats->ch; k++) n += histogram[4 * i + k];

    if((double)n >= thr)
    {
      raw = i;
      break;
    }
  }

  const double ev
      = raw_to_ev(raw, (uint32_t)pipe->dsc.rawprepare.raw_black_level, pipe->dsc.rawprepare.raw_white_point);

  *correction = p->deflicker_target_level - ev;
}

static void process_common_setup(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_exposure_gui_data_t *g = self->gui_data;
  dt_iop_exposure_data_t *d = piece->data;

  d->black = d->params.black;
  float exposure = d->params.exposure;

  if(d->deflicker)
  {
    if(g)
    {
      // histogram is precomputed and cached
      compute_correction(self, &d->params, piece->pipe, g->deflicker_histogram, &g->deflicker_histogram_stats,
                         &exposure);
    }
    else
    {
      uint32_t *histogram = NULL;
      dt_dev_histogram_stats_t histogram_stats;
      deflicker_prepare_histogram(self, &histogram, &histogram_stats);
      compute_correction(self, &d->params, piece->pipe, histogram, &histogram_stats, &exposure);
      free(histogram);
    }

    // second, show computed correction in UI.
    if(g && piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
    {
      dt_pthread_mutex_lock(&g->lock);
      g->deflicker_computed_exposure = exposure;
      dt_pthread_mutex_unlock(&g->lock);
    }
  }

  const float white = exposure2white(exposure);
  d->scale = 1.0 / (white - d->black);
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_exposure_data_t *d = (dt_iop_exposure_data_t *)piece->data;
  dt_iop_exposure_global_data_t *gd = (dt_iop_exposure_global_data_t *)self->data;

  process_common_setup(self, piece);

  cl_int err = -999;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };
  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 4, sizeof(float), (void *)&(d->black));
  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 5, sizeof(float), (void *)&(d->scale));
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_exposure, sizes);
  if(err != CL_SUCCESS) goto error;
  for(int k = 0; k < 3; k++) piece->pipe->dsc.processed_maximum[k] *= d->scale;

  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_exposure] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const i, void *const o,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_exposure_data_t *const d = (const dt_iop_exposure_data_t *const)piece->data;

  process_common_setup(self, piece);

  const int ch = piece->colors;

#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) schedule(static)
#endif
  for(size_t k = 0; k < (size_t)ch * roi_out->width * roi_out->height; k++)
  {
    ((float *)o)[k] = (((float *)i)[k] - d->black) * d->scale;
  }

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(i, o, roi_out->width, roi_out->height);

  for(int k = 0; k < 3; k++) piece->pipe->dsc.processed_maximum[k] *= d->scale;
}

#if defined(__SSE__)
void process_sse2(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const i,
                  void *const o, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_exposure_data_t *const d = (const dt_iop_exposure_data_t *const)piece->data;

  process_common_setup(self, piece);

  const int ch = piece->colors;
  const __m128 blackv = _mm_set1_ps(d->black);
  const __m128 scalev = _mm_set1_ps(d->scale);

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int k = 0; k < roi_out->height; k++)
  {
    const float *in = ((float *)i) + (size_t)ch * k * roi_out->width;
    float *out = ((float *)o) + (size_t)ch * k * roi_out->width;
    for(int j = 0; j < roi_out->width; j++, in += 4, out += 4)
      _mm_store_ps(out, _mm_mul_ps(_mm_sub_ps(_mm_load_ps(in), blackv), scalev));
  }

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(i, o, roi_out->width, roi_out->height);

  for(int k = 0; k < 3; k++) piece->pipe->dsc.processed_maximum[k] *= d->scale;
}
#endif

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)p1;
  dt_iop_exposure_data_t *d = (dt_iop_exposure_data_t *)piece->data;

  d->params = *p;

  d->deflicker = 0;

  if(p->mode == EXPOSURE_MODE_DEFLICKER && dt_image_is_raw(&self->dev->image_storage)
     && self->dev->image_storage.buf_dsc.channels == 1 && self->dev->image_storage.buf_dsc.datatype == TYPE_UINT16)
  {
    d->deflicker = 1;
  }
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_exposure_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

static void autoexp_disable(dt_iop_module_t *self)
{
  if(self->request_color_pick != DT_REQUEST_COLORPICK_MODULE) return;

  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  if(!dt_image_is_raw(&self->dev->image_storage) || self->dev->image_storage.buf_dsc.channels != 1
     || self->dev->image_storage.buf_dsc.datatype != TYPE_UINT16)
  {
    gtk_widget_set_sensitive(GTK_WIDGET(g->mode), FALSE);
    p->mode = EXPOSURE_MODE_MANUAL;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
  else
  {
    gtk_widget_set_sensitive(GTK_WIDGET(g->mode), TRUE);
  }

  dt_bauhaus_combobox_set(g->mode, g_list_index(g->modes, GUINT_TO_POINTER(p->mode)));

  dt_bauhaus_slider_set_soft(g->black, p->black);
  dt_bauhaus_slider_set_soft(g->exposure, p->exposure);

  dt_bauhaus_slider_set(g->autoexpp, 0.01);
  dt_bauhaus_widget_set_quad_active(g->autoexpp, FALSE);

  dt_bauhaus_slider_set(g->deflicker_percentile, p->deflicker_percentile);
  dt_bauhaus_slider_set(g->deflicker_target_level, p->deflicker_target_level);

  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;

  free(g->deflicker_histogram);
  g->deflicker_histogram = NULL;

  gtk_label_set_text(g->deflicker_used_EC, "");
  dt_pthread_mutex_lock(&g->lock);
  g->deflicker_computed_exposure = NAN;
  dt_pthread_mutex_unlock(&g->lock);

  switch(p->mode)
  {
    case EXPOSURE_MODE_DEFLICKER:
      autoexp_disable(self);
      gtk_stack_set_visible_child_name(GTK_STACK(g->mode_stack), "deflicker");
      deflicker_prepare_histogram(self, &g->deflicker_histogram, &g->deflicker_histogram_stats);
      break;
    case EXPOSURE_MODE_MANUAL:
    default:
      gtk_stack_set_visible_child_name(GTK_STACK(g->mode_stack), "manual");
      break;
  }
}

void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  // switch off auto exposure when we lose focus (switching images etc)
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;

  dt_bauhaus_slider_set(g->autoexpp, 0.01);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_exposure_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_exposure_params_t));
  module->default_enabled = 0;
  module->priority = 157; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_exposure_params_t);
  module->gui_data = NULL;
}

void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_exposure_params_t tmp = (dt_iop_exposure_params_t){.mode = EXPOSURE_MODE_MANUAL,
                                                            .black = 0.0f,
                                                            .exposure = 0.0f,
                                                            .deflicker_percentile = 50.0f,
                                                            .deflicker_target_level = -4.0f
  };

  memcpy(module->params, &tmp, sizeof(dt_iop_exposure_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_exposure_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // from programs.conf: basic.cl
  dt_iop_exposure_global_data_t *gd
      = (dt_iop_exposure_global_data_t *)malloc(sizeof(dt_iop_exposure_global_data_t));
  module->data = gd;
  gd->kernel_exposure = dt_opencl_create_kernel(program, "exposure");
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_exposure_global_data_t *gd = (dt_iop_exposure_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_exposure);
  free(module->data);
  module->data = NULL;
}

static void mode_callback(GtkWidget *combo, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;

  if(darktable.gui->reset) return;

  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  const dt_iop_exposure_mode_t new_mode
      = GPOINTER_TO_UINT(g_list_nth_data(g->modes, dt_bauhaus_combobox_get(combo)));

  free(g->deflicker_histogram);
  g->deflicker_histogram = NULL;

  switch(new_mode)
  {
    case EXPOSURE_MODE_DEFLICKER:
      autoexp_disable(self);
      if(!dt_image_is_raw(&self->dev->image_storage) || self->dev->image_storage.buf_dsc.channels != 1
         || self->dev->image_storage.buf_dsc.datatype != TYPE_UINT16)
      {
        dt_bauhaus_combobox_set(g->mode, g_list_index(g->modes, GUINT_TO_POINTER(EXPOSURE_MODE_MANUAL)));
        gtk_widget_set_sensitive(GTK_WIDGET(g->mode), FALSE);
        break;
      }
      p->mode = EXPOSURE_MODE_DEFLICKER;
      gtk_stack_set_visible_child_name(GTK_STACK(g->mode_stack), "deflicker");
      deflicker_prepare_histogram(self, &g->deflicker_histogram, &g->deflicker_histogram_stats);
      break;
    case EXPOSURE_MODE_MANUAL:
    default:
      p->mode = EXPOSURE_MODE_MANUAL;
      gtk_stack_set_visible_child_name(GTK_STACK(g->mode_stack), "manual");
      break;
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void exposure_set_black(struct dt_iop_module_t *self, const float black);

static void exposure_set_white(struct dt_iop_module_t *self, const float white)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  float exposure = white2exposure(white);
  if(p->exposure == exposure) return;

  p->exposure = exposure;
  if(p->black >= white) exposure_set_black(self, white - 0.01);

  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->exposure, p->exposure);
  darktable.gui->reset = 0;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void dt_iop_exposure_set_exposure(struct dt_iop_module_t *self, const float exposure)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  if(p->mode == EXPOSURE_MODE_DEFLICKER)
  {
    dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;

    p->deflicker_target_level = exposure;

    darktable.gui->reset = 1;
    dt_bauhaus_slider_set(g->deflicker_target_level, p->deflicker_target_level);
    darktable.gui->reset = 0;

    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
  else
  {
    float white = exposure2white(exposure);
    exposure_set_white(self, white);
    autoexp_disable(self);
  }
}

static float dt_iop_exposure_get_exposure(struct dt_iop_module_t *self)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  if(p->mode == EXPOSURE_MODE_DEFLICKER)
  {
    return p->deflicker_target_level;
  }
  else
  {
    return p->exposure;
  }
}

static void exposure_set_black(struct dt_iop_module_t *self, const float black)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  if(p->black == black) return;

  p->black = black;
  if(p->black >= exposure2white(p->exposure))
  {
    exposure_set_white(self, p->black + 0.01);
  }

  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->black, p->black);
  darktable.gui->reset = 0;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void dt_iop_exposure_set_black(struct dt_iop_module_t *self, const float black)
{
  autoexp_disable(self);
  exposure_set_black(self, black);
}

static float dt_iop_exposure_get_black(struct dt_iop_module_t *self)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;
  return p->black;
}

static void autoexp_callback(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;

  if(self->request_color_pick == DT_REQUEST_COLORPICK_OFF)
    self->request_color_pick = DT_REQUEST_COLORPICK_MODULE;
  else
    self->request_color_pick = DT_REQUEST_COLORPICK_OFF;

  dt_iop_request_focus(self);

  if(self->request_color_pick == DT_REQUEST_COLORPICK_MODULE)
  {
    dt_lib_colorpicker_set_area(darktable.lib, 0.99);
    dt_dev_reprocess_all(self->dev);
  }
  else
    dt_control_queue_redraw();

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void autoexpp_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  if(self->request_color_pick != DT_REQUEST_COLORPICK_MODULE || self->picked_color_max[0] < 0.0f) return;

  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  const float white = fmaxf(fmaxf(self->picked_color_max[0], self->picked_color_max[1]),
                            self->picked_color_max[2]) * (1.0 - dt_bauhaus_slider_get(g->autoexpp));
  exposure_set_white(self, white);
}

static void deflicker_params_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;

  if(!dt_image_is_raw(&self->dev->image_storage) || self->dev->image_storage.buf_dsc.channels != 1
     || self->dev->image_storage.buf_dsc.datatype != TYPE_UINT16)
    return;

  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  if(p->mode != EXPOSURE_MODE_DEFLICKER) return;

  p->deflicker_percentile = dt_bauhaus_slider_get(g->deflicker_percentile);
  p->deflicker_target_level = dt_bauhaus_slider_get(g->deflicker_target_level);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void exposure_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;

  autoexp_disable(self);

  const float exposure = dt_bauhaus_slider_get(slider);
  dt_iop_exposure_set_exposure(self, exposure);
}

static void black_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;

  const float black = dt_bauhaus_slider_get(slider);
  dt_iop_exposure_set_black(self, black);
}

static gboolean draw(GtkWidget *widget, cairo_t *cr, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return FALSE;

  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;

  dt_pthread_mutex_lock(&g->lock);
  if(!isnan(g->deflicker_computed_exposure))
  {
    gchar *str = g_strdup_printf("%.2fEV", g->deflicker_computed_exposure);

    darktable.gui->reset = 1;
    gtk_label_set_text(g->deflicker_used_EC, str);
    darktable.gui->reset = 0;

    g_free(str);
  }
  dt_pthread_mutex_unlock(&g->lock);

  if(self->request_color_pick != DT_REQUEST_COLORPICK_MODULE) return FALSE;

  if(self->picked_color_max[0] < 0.0f) return FALSE;

  const float white = fmaxf(fmaxf(self->picked_color_max[0], self->picked_color_max[1]),
                            self->picked_color_max[2]) * (1.0 - dt_bauhaus_slider_get(g->autoexpp));
  const float black
      = fminf(fminf(self->picked_color_min[0], self->picked_color_min[1]), self->picked_color_min[2]);

  exposure_set_white(self, white);
  exposure_set_black(self, black);
  return FALSE;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_exposure_gui_data_t));
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  g->modes = NULL;

  g->deflicker_histogram = NULL;

  dt_pthread_mutex_init(&g->lock, NULL);

  /* register hooks with current dev so that  histogram
     can interact with this module.
   */
  dt_dev_proxy_exposure_t *instance = g_malloc0(sizeof(dt_dev_proxy_exposure_t));
  instance->module = self;
  instance->set_exposure = dt_iop_exposure_set_exposure;
  instance->get_exposure = dt_iop_exposure_get_exposure;
  instance->set_black = dt_iop_exposure_set_black;
  instance->get_black = dt_iop_exposure_get_black;
  darktable.develop->proxy.exposure
      = g_list_insert_sorted(darktable.develop->proxy.exposure, instance, dt_dev_exposure_hooks_sort);

  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;

  self->widget = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE));
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));

  g->mode = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->mode, NULL, _("mode"));

  dt_bauhaus_combobox_add(g->mode, C_("mode", "manual"));
  g->modes = g_list_append(g->modes, GUINT_TO_POINTER(EXPOSURE_MODE_MANUAL));

  dt_bauhaus_combobox_add(g->mode, _("automatic"));
  g->modes = g_list_append(g->modes, GUINT_TO_POINTER(EXPOSURE_MODE_DEFLICKER));

  dt_bauhaus_combobox_set_default(g->mode, 0);
  dt_bauhaus_combobox_set(g->mode, g_list_index(g->modes, GUINT_TO_POINTER(p->mode)));

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->mode), TRUE, TRUE, 0);

  g->black = dt_bauhaus_slider_new_with_range(self, -0.1, 0.1, .001, p->black, 4);
  gtk_widget_set_tooltip_text(g->black, _("adjust the black level"));
  dt_bauhaus_slider_set_format(g->black, "%.4f");
  dt_bauhaus_widget_set_label(g->black, NULL, _("black"));
  dt_bauhaus_slider_enable_soft_boundaries(g->black, -1.0, 1.0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->black), TRUE, TRUE, 0);

  g->mode_stack = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(g->mode_stack),FALSE);
  gtk_box_pack_start(GTK_BOX(self->widget), g->mode_stack, TRUE, TRUE, 0);

  GtkWidget *vbox_manual = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE));

  g->exposure = dt_bauhaus_slider_new_with_range(self, -3.0, 3.0, .02, p->exposure, 3);
  gtk_widget_set_tooltip_text(g->exposure, _("adjust the exposure correction"));
  dt_bauhaus_slider_set_format(g->exposure, "%.2fEV");
  dt_bauhaus_widget_set_label(g->exposure, NULL, _("exposure"));
  dt_bauhaus_slider_enable_soft_boundaries(g->exposure, -18.0, 18.0);
  gtk_box_pack_start(GTK_BOX(vbox_manual), GTK_WIDGET(g->exposure), TRUE, TRUE, 0);

  g->autoexpp = dt_bauhaus_slider_new_with_range(self, 0.0, 0.2, .001, 0.01, 3);
  gtk_widget_set_tooltip_text(g->autoexpp, _("percentage of bright values clipped out, toggle color picker to activate"));
  dt_bauhaus_slider_set_format(g->autoexpp, "%.3f%%");
  dt_bauhaus_widget_set_label(g->autoexpp, NULL, _("clipping threshold"));
  dt_bauhaus_widget_set_quad_paint(g->autoexpp, dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->autoexpp, TRUE);
  gtk_box_pack_start(GTK_BOX(vbox_manual), GTK_WIDGET(g->autoexpp), TRUE, TRUE, 0);

  gtk_widget_show_all(vbox_manual);
  gtk_stack_add_named(GTK_STACK(g->mode_stack), vbox_manual, "manual");

  GtkWidget *vbox_deflicker = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE));

  g->deflicker_percentile = dt_bauhaus_slider_new_with_range(self, 0, 100, 1.0, p->deflicker_percentile, 3);
  dt_bauhaus_widget_set_label(g->deflicker_percentile, NULL, _("percentile"));
  dt_bauhaus_slider_set_format(g->deflicker_percentile, "%.2f%%");
  gtk_widget_set_tooltip_text(g->deflicker_percentile,
                              // xgettext:no-c-format
                              _("where in the histogram to meter for deflicking. E.g. 50% is median"));
  gtk_box_pack_start(GTK_BOX(vbox_deflicker), GTK_WIDGET(g->deflicker_percentile), TRUE, TRUE, 0);

  g->deflicker_target_level
      = dt_bauhaus_slider_new_with_range(self, -18.0, 18.0, .01, p->deflicker_target_level, 3);
  dt_bauhaus_widget_set_label(g->deflicker_target_level, NULL, _("target level"));
  dt_bauhaus_slider_set_format(g->deflicker_target_level, "%.2fEV");
  gtk_widget_set_tooltip_text(g->deflicker_target_level,
                              _("where to place the exposure level for processed pics, EV below overexposure."));
  gtk_box_pack_start(GTK_BOX(vbox_deflicker), GTK_WIDGET(g->deflicker_target_level), TRUE, TRUE, 0);

  GtkBox *hbox1 = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  GtkLabel *label = GTK_LABEL(gtk_label_new(_("computed EC: ")));
  gtk_box_pack_start(GTK_BOX(hbox1), GTK_WIDGET(label), FALSE, FALSE, 0);

  g->deflicker_used_EC = GTK_LABEL(gtk_label_new("")); // This gets filled in by process
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->deflicker_used_EC), _("what exposure correction has actually been used"));
  gtk_box_pack_start(GTK_BOX(hbox1), GTK_WIDGET(g->deflicker_used_EC), FALSE, FALSE, 0);

  dt_pthread_mutex_lock(&g->lock);
  g->deflicker_computed_exposure = NAN;
  dt_pthread_mutex_unlock(&g->lock);

  gtk_box_pack_start(GTK_BOX(vbox_deflicker), GTK_WIDGET(hbox1), FALSE, FALSE, 0);

  gtk_widget_show_all(vbox_deflicker);
  gtk_stack_add_named(GTK_STACK(g->mode_stack), vbox_deflicker, "deflicker");

  g_signal_connect(G_OBJECT(g->mode), "value-changed", G_CALLBACK(mode_callback), self);
  g_signal_connect(G_OBJECT(g->black), "value-changed", G_CALLBACK(black_callback), self);
  g_signal_connect(G_OBJECT(g->exposure), "value-changed", G_CALLBACK(exposure_callback), self);
  g_signal_connect(G_OBJECT(g->autoexpp), "value-changed", G_CALLBACK(autoexpp_callback), self);
  g_signal_connect(G_OBJECT(g->autoexpp), "quad-pressed", G_CALLBACK(autoexp_callback), self);
  g_signal_connect(G_OBJECT(g->deflicker_percentile), "value-changed", G_CALLBACK(deflicker_params_callback),
                   self);
  g_signal_connect(G_OBJECT(g->deflicker_target_level), "value-changed",
                   G_CALLBACK(deflicker_params_callback), self);
  g_signal_connect(G_OBJECT(self->widget), "draw", G_CALLBACK(draw), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;

  GList *instances = darktable.develop->proxy.exposure;
  while(instances != NULL)
  {
    GList *next = g_list_next(instances);
    dt_dev_proxy_exposure_t *instance = (dt_dev_proxy_exposure_t *)instances->data;
    if(instance->module == self)
    {
      g_free(instance);
      darktable.develop->proxy.exposure = g_list_delete_link(darktable.develop->proxy.exposure, instances);
    }
    instances = next;
  }

  free(g->deflicker_histogram);
  g->deflicker_histogram = NULL;
  g_list_free(g->modes);

  dt_pthread_mutex_destroy(&g->lock);

  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
