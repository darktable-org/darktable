/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.
    copyright (c) 2014 LebedevRI.

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

#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include <xmmintrin.h>

#include "common/opencl.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "common/mipmap_cache.h"
#include "common/image_cache.h"
#include "control/control.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "dtgtk/resetlabel.h"
#include "bauhaus/bauhaus.h"
#include "develop/pixelpipe.h"
#include "common/histogram.h"

#define exposure2white(x) exp2f(-(x))
#define white2exposure(x) -dt_log2f(fmaxf(0.001, x))

DT_MODULE_INTROSPECTION(4, dt_iop_exposure_params_t)

typedef enum dt_iop_exposure_mode_t
{
  EXPOSURE_MODE_MANUAL,
  EXPOSURE_MODE_DEFLICKER
} dt_iop_exposure_mode_t;

typedef enum dt_iop_exposure_deflicker_histogram_source_t
{
  DEFLICKER_HISTOGRAM_SOURCE_THUMBNAIL,
  DEFLICKER_HISTOGRAM_SOURCE_SOURCEFILE
} dt_iop_exposure_deflicker_histogram_source_t;

typedef struct dt_iop_exposure_params_t
{
  dt_iop_exposure_mode_t mode;
  float black;
  float exposure;
  float deflicker_percentile, deflicker_target_level;
  dt_iop_exposure_deflicker_histogram_source_t deflicker_histogram_source;
} dt_iop_exposure_params_t;

typedef struct dt_iop_exposure_gui_data_t
{
  GList *modes;
  GtkWidget *mode;
  GtkWidget *black;
  GtkWidget *vbox_manual;
  GtkWidget *exposure;
  GtkCheckButton *autoexp;
  GtkWidget *autoexpp;
  GtkWidget *vbox_deflicker;
  GtkWidget *deflicker_percentile;
  GtkWidget *deflicker_target_level;
  GList *deflicker_histogram_sources;
  GtkWidget *deflicker_histogram_source;
  uint32_t *deflicker_histogram; // used to cache histogram of source file
  dt_dev_histogram_stats_t deflicker_histogram_stats;
  gboolean reprocess_on_next_expose;
  GtkLabel *deflicker_used_EC;
  float deflicker_computed_exposure;
} dt_iop_exposure_gui_data_t;

typedef struct dt_iop_exposure_data_t
{
  dt_iop_exposure_mode_t mode;
  float black;
  float exposure;
  float deflicker_percentile, deflicker_target_level;
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
  return IOP_GROUP_BASIC;
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
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "histogram source"));
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
  dt_accel_connect_slider_iop(self, "histogram source", GTK_WIDGET(g->deflicker_histogram_source));
}

int output_bpp(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return 4 * sizeof(float);
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 2 && new_version == 4)
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
  if(old_version == 3 && new_version == 4)
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
  return 1;
}

void init_presets (dt_iop_module_so_t *self)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "begin", NULL, NULL, NULL);

  dt_gui_presets_add_generic(_("magic lantern defaults"), self->op, self->version(),
&(dt_iop_exposure_params_t)
  {
    EXPOSURE_MODE_DEFLICKER, 0.0f, 0.0f, 50.0f, -4.0f, DEFLICKER_HISTOGRAM_SOURCE_SOURCEFILE
  }, sizeof(dt_iop_exposure_params_t), 1);
  dt_gui_presets_add_generic(_("almost no clipping"), self->op, self->version(), &(dt_iop_exposure_params_t)
  {
    EXPOSURE_MODE_DEFLICKER, 0.0f, 0.0f, 100.0f, -1.0f, DEFLICKER_HISTOGRAM_SOURCE_THUMBNAIL
  }, sizeof(dt_iop_exposure_params_t), 1);

  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "commit", NULL, NULL, NULL);
}

static void deflicker_prepare_histogram(dt_iop_module_t *self, uint32_t **histogram,
                                        dt_dev_histogram_stats_t *histogram_stats)
{
  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, self->dev->image_storage.id, DT_MIPMAP_FULL,
                      DT_MIPMAP_BLOCKING, 'r');
  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, self->dev->image_storage.id, 'r');
  dt_image_t image = *img;
  dt_image_cache_read_release(darktable.image_cache, img);
  if(!buf.buf)
  {
    dt_control_log(_("failed to get raw buffer from image `%s'"), image.filename);
    dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
    return;
  }

  dt_dev_histogram_collection_params_t histogram_params;
  memcpy(&histogram_params, &self->histogram_params, sizeof(dt_dev_histogram_collection_params_t));

  dt_iop_roi_t roi = { 0, 0, image.width, image.height, 1.0f };
  histogram_params.roi = &roi;

  dt_histogram_worker(&histogram_params, histogram_stats, buf.buf, histogram,
                      dt_histogram_helper_cs_RAW_uint16);
  histogram_stats->ch = 1u;

  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
}

/* input: 0 - 16384 (valid range: from black level to white level) */
/* output: -14 ... 0 */
static float raw_to_ev(uint32_t raw, uint32_t black_level, uint32_t white_level)
{
  uint32_t raw_max = white_level - black_level;
  float raw_ev = -log2f(raw_max) + log2f(CLAMP(raw, 0.0f, 16384.0f));

  return raw_ev;
}

static int compute_correction(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                              const uint32_t *const histogram,
                              const dt_dev_histogram_stats_t *const histogram_stats, float *correction)
{
  dt_iop_exposure_data_t *d = (dt_iop_exposure_data_t *)piece->data;

  if(histogram == NULL) return 1;

  *correction = NAN;

  uint32_t total = histogram_stats->ch * histogram_stats->pixels;

  float thr = (total * d->deflicker_percentile / 100.0f) - 2; // 50% => median; allow up to 2 stuck pixels
  uint32_t n = 0;
  uint32_t raw = 0;
  gboolean found = FALSE;

  for(uint32_t i = 0; i < histogram_stats->bins_count; i++)
  {
    for(uint32_t k = 0; k < histogram_stats->ch; k++) n += histogram[4 * i + k];

    if(!found && (n >= thr))
    {
      raw = i;
      found = TRUE;
      break;
    }
  }

  if(found)
  {
    float ev = raw_to_ev(raw, self->dev->image_storage.raw_black_level + d->black,
                         self->dev->image_storage.raw_white_point);
    *correction = d->deflicker_target_level - ev;

    return 0;
  }

  return 1;
}

/*
 * WARNING: unlike commit_params, which is thread safe wrt gui thread and
 * pipes, this function lives in the pipeline thread, and NOT thread safe!
 */
static void commit_params_late(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_exposure_data_t *d = (dt_iop_exposure_data_t *)piece->data;

  if(d->mode == EXPOSURE_MODE_DEFLICKER)
  {
    compute_correction(self, piece, self->histogram, &self->histogram_stats, &d->exposure);
  }
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_exposure_data_t *d = (dt_iop_exposure_data_t *)piece->data;
  dt_iop_exposure_global_data_t *gd = (dt_iop_exposure_global_data_t *)self->data;
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;

  if(d->mode == EXPOSURE_MODE_DEFLICKER)
  {
    commit_params_late(self, piece);
  }

  cl_int err = -999;
  const float black = d->black;
  const float white = exposure2white(d->exposure);
  const float scale = 1.0 / (white - black);
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };
  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 4, sizeof(float), (void *)&black);
  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 5, sizeof(float), (void *)&scale);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_exposure, sizes);
  if(err != CL_SUCCESS) goto error;
  for(int k = 0; k < 3; k++) piece->pipe->processed_maximum[k] *= scale;
  if(g != NULL && self->dev->gui_attached && piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
  {
    g->deflicker_computed_exposure = d->exposure;
  }
  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_exposure] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o,
             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_exposure_data_t *d = (dt_iop_exposure_data_t *)piece->data;
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;

  if(d->mode == EXPOSURE_MODE_DEFLICKER)
  {
    commit_params_late(self, piece);
  }

  const float black = d->black;
  const float white = exposure2white(d->exposure);
  const int ch = piece->colors;
  const float scale = 1.0 / (white - black);
  const __m128 blackv = _mm_set1_ps(black);
  const __m128 scalev = _mm_set1_ps(scale);
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(roi_out, i, o) schedule(static)
#endif
  for(int k = 0; k < roi_out->height; k++)
  {
    const float *in = ((float *)i) + (size_t)ch * k * roi_out->width;
    float *out = ((float *)o) + (size_t)ch * k * roi_out->width;
    for(int j = 0; j < roi_out->width; j++, in += 4, out += 4)
      _mm_store_ps(out, (_mm_load_ps(in) - blackv) * scalev);
  }

  if(piece->pipe->mask_display) dt_iop_alpha_copy(i, o, roi_out->width, roi_out->height);

  for(int k = 0; k < 3; k++) piece->pipe->processed_maximum[k] *= scale;

  if(g != NULL && self->dev->gui_attached && piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
  {
    g->deflicker_computed_exposure = d->exposure;
  }
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)p1;
  dt_iop_exposure_data_t *d = (dt_iop_exposure_data_t *)piece->data;
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;

  d->black = p->black;
  d->exposure = p->exposure;

  self->request_histogram &= ~(DT_REQUEST_ON);
  self->request_histogram |= (DT_REQUEST_ONLY_IN_GUI);
  self->request_histogram_source = (DT_DEV_PIXELPIPE_PREVIEW);

  if(self->dev->gui_attached)
  {
    g->reprocess_on_next_expose = FALSE;
  }

  gboolean histogram_is_good = ((self->histogram_stats.bins_count == 16384) && (self->histogram != NULL));

  d->deflicker_percentile = p->deflicker_percentile;
  d->deflicker_target_level = p->deflicker_target_level;

  if(p->mode == EXPOSURE_MODE_DEFLICKER && dt_image_is_raw(&self->dev->image_storage))
  {
    if(p->deflicker_histogram_source == DEFLICKER_HISTOGRAM_SOURCE_SOURCEFILE)
    {
      if(self->dev->gui_attached)
      {
        // histogram is precomputed and cached
        compute_correction(self, piece, g->deflicker_histogram, &g->deflicker_histogram_stats, &d->exposure);
      }
      else
      {
        uint32_t *histogram = NULL;
        dt_dev_histogram_stats_t histogram_stats;
        deflicker_prepare_histogram(self, &histogram, &histogram_stats);
        compute_correction(self, piece, histogram, &histogram_stats, &d->exposure);
        free(histogram);
      }
      d->mode = EXPOSURE_MODE_MANUAL;
    }
    else
    {
      if(p->deflicker_histogram_source == DEFLICKER_HISTOGRAM_SOURCE_THUMBNAIL)
      {
        self->request_histogram |= (DT_REQUEST_ON);

        gboolean failed = !histogram_is_good;

        if(self->dev->gui_attached && histogram_is_good)
        {
          /*
           * if in GUI, user might zoomed main view => we would get histogram of
           * only part of image, so if in GUI we must always use histogram of
           * preview pipe, which is always full-size and have biggest size
           */
          d->mode = EXPOSURE_MODE_DEFLICKER;
          commit_params_late(self, piece);
          d->mode = EXPOSURE_MODE_MANUAL;

          if(isnan(d->exposure)) failed = TRUE;
        }
        else if(failed || !(self->dev->gui_attached && histogram_is_good))
        {
          d->mode = EXPOSURE_MODE_DEFLICKER;
          // commit_params_late() will compute correct d->exposure later
          self->request_histogram &= ~(DT_REQUEST_ONLY_IN_GUI);
          self->request_histogram_source = (DT_DEV_PIXELPIPE_ANY);

          if(failed && self->dev->gui_attached)
          {
            /*
             * but sadly we do not yet have a histogram to do so, so this time
             * we process asif not in gui, and in expose() immediately
             * reprocess, thus everything works as expected
             */
            g->reprocess_on_next_expose = TRUE;
          }
        }
      }
    }
  }
  else
  {
    d->mode = EXPOSURE_MODE_MANUAL;
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

  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->autoexp), FALSE);

  gtk_widget_set_sensitive(GTK_WIDGET(g->autoexpp), FALSE);

  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  if(!dt_image_is_raw(&self->dev->image_storage))
  {
    gtk_widget_hide(GTK_WIDGET(g->mode));
    p->mode = EXPOSURE_MODE_MANUAL;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
  else
  {
    gtk_widget_show(GTK_WIDGET(g->mode));
  }

  dt_bauhaus_combobox_set(g->mode, g_list_index(g->modes, GUINT_TO_POINTER(p->mode)));

  dt_bauhaus_slider_set(g->black, p->black);
  dt_bauhaus_slider_set_soft(g->exposure, p->exposure);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->autoexp), FALSE);
  dt_bauhaus_slider_set(g->autoexpp, 0.01);
  gtk_widget_set_sensitive(GTK_WIDGET(g->autoexpp), FALSE);

  dt_bauhaus_slider_set(g->deflicker_percentile, p->deflicker_percentile);
  dt_bauhaus_slider_set(g->deflicker_target_level, p->deflicker_target_level);
  dt_bauhaus_combobox_set(
      g->deflicker_histogram_source,
      g_list_index(g->deflicker_histogram_sources, GUINT_TO_POINTER(p->deflicker_histogram_source)));

  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;

  free(g->deflicker_histogram);
  g->deflicker_histogram = NULL;

  g->reprocess_on_next_expose = FALSE;

  gtk_label_set_text(g->deflicker_used_EC, "");
  g->deflicker_computed_exposure = NAN;

  switch(p->mode)
  {
    case EXPOSURE_MODE_DEFLICKER:
      autoexp_disable(self);
      gtk_widget_hide(GTK_WIDGET(g->vbox_manual));
      gtk_widget_show(GTK_WIDGET(g->vbox_deflicker));

      if(p->deflicker_histogram_source == DEFLICKER_HISTOGRAM_SOURCE_SOURCEFILE)
        deflicker_prepare_histogram(self, &g->deflicker_histogram, &g->deflicker_histogram_stats);
      break;
    case EXPOSURE_MODE_MANUAL:
    default:
      gtk_widget_hide(GTK_WIDGET(g->vbox_deflicker));
      gtk_widget_show(GTK_WIDGET(g->vbox_manual));
      break;
  }
}

void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  // switch off auto exposure when we lose focus (switching images etc)
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->autoexp), FALSE);
  dt_bauhaus_slider_set(g->autoexpp, 0.01);
  gtk_widget_set_sensitive(GTK_WIDGET(g->autoexpp), FALSE);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_exposure_params_t));
  module->default_params = malloc(sizeof(dt_iop_exposure_params_t));
  module->default_enabled = 0;
  module->histogram_params.bins_count = 16384; // we neeed really maximally reliable histogrem
  module->priority = 183;                      // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_exposure_params_t);
  module->gui_data = NULL;
}

void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_exposure_params_t tmp = (dt_iop_exposure_params_t){ EXPOSURE_MODE_MANUAL, 0.0f, 0.0f, 100.0f, -1.0f,
                                                             DEFLICKER_HISTOGRAM_SOURCE_THUMBNAIL };

  tmp.mode = EXPOSURE_MODE_MANUAL;
  tmp.black = 0.0f;
  tmp.exposure = 0.0f;
  tmp.deflicker_percentile = 100.0f;
  tmp.deflicker_target_level = -1.0f;
  tmp.deflicker_histogram_source = DEFLICKER_HISTOGRAM_SOURCE_THUMBNAIL;

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
  free(module->gui_data);
  module->gui_data = NULL;
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
      if(!dt_image_is_raw(&self->dev->image_storage))
      {
        dt_bauhaus_combobox_set(g->mode, g_list_index(g->modes, GUINT_TO_POINTER(EXPOSURE_MODE_MANUAL)));
        gtk_widget_hide(GTK_WIDGET(g->mode));
        break;
      }
      p->mode = EXPOSURE_MODE_DEFLICKER;
      gtk_widget_hide(GTK_WIDGET(g->vbox_manual));
      gtk_widget_show(GTK_WIDGET(g->vbox_deflicker));
      if(p->deflicker_histogram_source == DEFLICKER_HISTOGRAM_SOURCE_SOURCEFILE)
        deflicker_prepare_histogram(self, &g->deflicker_histogram, &g->deflicker_histogram_stats);
      break;
    case EXPOSURE_MODE_MANUAL:
    default:
      p->mode = EXPOSURE_MODE_MANUAL;
      gtk_widget_hide(GTK_WIDGET(g->vbox_deflicker));
      gtk_widget_show(GTK_WIDGET(g->vbox_manual));
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

static void dt_iop_exposure_set_white(struct dt_iop_module_t *self, const float white)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  if(p->mode == EXPOSURE_MODE_DEFLICKER)
  {
    dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;

    p->deflicker_target_level = white;

    darktable.gui->reset = 1;
    dt_bauhaus_slider_set(g->deflicker_target_level, p->deflicker_target_level);
    darktable.gui->reset = 0;

    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
  else
  {
    exposure_set_white(self, white);
    autoexp_disable(self);
  }
}

static float dt_iop_exposure_get_white(struct dt_iop_module_t *self)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  if(p->mode == EXPOSURE_MODE_DEFLICKER)
  {
    return p->deflicker_target_level;
  }
  else
  {
    return exposure2white(p->exposure);
  }
}

static void exposure_set_black(struct dt_iop_module_t *self, const float black)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  float b = black;
  if(p->black == b) return;

  p->black = b;
  if(p->black >= exposure2white(p->exposure))
  {
    exposure_set_white(self, p->black + 0.01);
  }

  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->black, p->black);
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

static void autoexp_callback(GtkToggleButton *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;

  self->request_color_pick = gtk_toggle_button_get_active(button) ? DT_REQUEST_COLORPICK_MODULE
                                                                  : DT_REQUEST_COLORPICK_OFF;

  dt_iop_request_focus(self);

  if(self->request_color_pick == DT_REQUEST_COLORPICK_MODULE)
  {
    dt_lib_colorpicker_set_area(darktable.lib, 0.99);
    dt_dev_reprocess_all(self->dev);
  }
  else
    dt_control_queue_redraw();

  gtk_widget_set_sensitive(GTK_WIDGET(g->autoexpp), gtk_toggle_button_get_active(button));

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

  if(!dt_image_is_raw(&self->dev->image_storage)) return;

  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  if(p->mode != EXPOSURE_MODE_DEFLICKER) return;

  p->deflicker_percentile = dt_bauhaus_slider_get(g->deflicker_percentile);
  p->deflicker_target_level = dt_bauhaus_slider_get(g->deflicker_target_level);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void deflicker_histogram_source_callback(GtkWidget *combo, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;

  if(self->dt->gui->reset) return;

  if(!dt_image_is_raw(&self->dev->image_storage)) return;

  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  const dt_iop_exposure_deflicker_histogram_source_t new_mode
      = GPOINTER_TO_UINT(g_list_nth_data(g->deflicker_histogram_sources, dt_bauhaus_combobox_get(combo)));

  switch(new_mode)
  {
    case DEFLICKER_HISTOGRAM_SOURCE_SOURCEFILE:
      p->deflicker_histogram_source = DEFLICKER_HISTOGRAM_SOURCE_SOURCEFILE;
      deflicker_prepare_histogram(self, &g->deflicker_histogram, &g->deflicker_histogram_stats);
      break;
    case DEFLICKER_HISTOGRAM_SOURCE_THUMBNAIL:
    default:
      p->deflicker_histogram_source = DEFLICKER_HISTOGRAM_SOURCE_THUMBNAIL;
      free(g->deflicker_histogram);
      g->deflicker_histogram = NULL;
      break;
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void exposure_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;

  autoexp_disable(self);

  const float exposure = dt_bauhaus_slider_get(slider);
  dt_iop_exposure_set_white(self, exposure2white(exposure));
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

  if(!isnan(g->deflicker_computed_exposure))
  {
    gchar *str = g_strdup_printf("%.2fEV", g->deflicker_computed_exposure);

    darktable.gui->reset = 1;
    gtk_label_set_text(g->deflicker_used_EC, str);
    darktable.gui->reset = 0;

    g_free(str);
    g->deflicker_computed_exposure = NAN;
  }

  if(self->enabled && g->reprocess_on_next_expose)
  {
    g->reprocess_on_next_expose = FALSE;
    // FIXME: or just use dev->pipe->changed |= DT_DEV_PIPE_SYNCH; ?
    dt_dev_reprocess_all(self->dev);
    return FALSE;
  }

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
  g->deflicker_histogram_sources = NULL;

  g->deflicker_histogram = NULL;

  g->reprocess_on_next_expose = FALSE;

  /* register hooks with current dev so that  histogram
     can interact with this module.
   */
  darktable.develop->proxy.exposure.module = self;
  darktable.develop->proxy.exposure.set_white = dt_iop_exposure_set_white;
  darktable.develop->proxy.exposure.get_white = dt_iop_exposure_get_white;
  darktable.develop->proxy.exposure.set_black = dt_iop_exposure_set_black;
  darktable.develop->proxy.exposure.get_black = dt_iop_exposure_get_black;

  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;

  self->widget = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE));

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
  g_object_set(G_OBJECT(g->black), "tooltip-text", _("adjust the black level"), (char *)NULL);
  dt_bauhaus_slider_set_format(g->black, "%.4f");
  dt_bauhaus_widget_set_label(g->black, NULL, _("black"));
  dt_bauhaus_slider_enable_soft_boundaries(g->black, -1.0, 1.0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->black), TRUE, TRUE, 0);

  g->vbox_manual = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE));

  g->exposure = dt_bauhaus_slider_new_with_range(self, -3.0, 3.0, .02, p->exposure, 3);
  g_object_set(G_OBJECT(g->exposure), "tooltip-text", _("adjust the exposure correction"), (char *)NULL);
  dt_bauhaus_slider_set_format(g->exposure, "%.2fEV");
  dt_bauhaus_widget_set_label(g->exposure, NULL, _("exposure"));
  dt_bauhaus_slider_enable_soft_boundaries(g->exposure, -18.0, 18.0);
  gtk_box_pack_start(GTK_BOX(g->vbox_manual), GTK_WIDGET(g->exposure), TRUE, TRUE, 0);

  GtkBox *hbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));

  g->autoexp = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("auto")));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->autoexp), FALSE);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->autoexp), FALSE, TRUE, 0);

  g->autoexpp = dt_bauhaus_slider_new_with_range(self, 0.0, 0.2, .001, 0.01, 3);
  g_object_set(G_OBJECT(g->autoexpp), "tooltip-text", _("percentage of bright values clipped out"),
               (char *)NULL);
  gtk_widget_set_sensitive(GTK_WIDGET(g->autoexpp), TRUE);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->autoexpp), TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(g->vbox_manual), GTK_WIDGET(hbox), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox_manual), TRUE, TRUE, 0);

  g->vbox_deflicker = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE));

  g->deflicker_percentile = dt_bauhaus_slider_new_with_range(self, 0, 100, .01, p->deflicker_percentile, 3);
  // FIXME: this needs a better tooltip!
  g_object_set(G_OBJECT(g->deflicker_percentile), "tooltip-text", _("percentile"), (char *)NULL);
  dt_bauhaus_slider_set_format(g->deflicker_percentile, "%.2f%%");
  dt_bauhaus_widget_set_label(g->deflicker_percentile, NULL, _("percentile"));
  gtk_box_pack_start(GTK_BOX(g->vbox_deflicker), GTK_WIDGET(g->deflicker_percentile), TRUE, TRUE, 0);

  g->deflicker_target_level
      = dt_bauhaus_slider_new_with_range(self, -18.0, 18.0, .01, p->deflicker_target_level, 3);
  g_object_set(G_OBJECT(g->deflicker_target_level), "tooltip-text", _("target level"), (char *)NULL);
  dt_bauhaus_slider_set_format(g->deflicker_target_level, "%.2fEV");
  dt_bauhaus_widget_set_label(g->deflicker_target_level, NULL, _("target level"));
  gtk_box_pack_start(GTK_BOX(g->vbox_deflicker), GTK_WIDGET(g->deflicker_target_level), TRUE, TRUE, 0);

  g->deflicker_histogram_source = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->deflicker_histogram_source, NULL, _("histogram of"));

  dt_bauhaus_combobox_add(g->deflicker_histogram_source, _("pre-processed image"));
  g->deflicker_histogram_sources
      = g_list_append(g->deflicker_histogram_sources, GUINT_TO_POINTER(DEFLICKER_HISTOGRAM_SOURCE_THUMBNAIL));

  dt_bauhaus_combobox_add(g->deflicker_histogram_source, _("source raw data"));
  g->deflicker_histogram_sources = g_list_append(g->deflicker_histogram_sources,
                                                 GUINT_TO_POINTER(DEFLICKER_HISTOGRAM_SOURCE_SOURCEFILE));

  dt_bauhaus_combobox_set_default(g->deflicker_histogram_source, DEFLICKER_HISTOGRAM_SOURCE_THUMBNAIL);
  dt_bauhaus_combobox_set(g->deflicker_histogram_source,
                          g_list_index(g->modes, GUINT_TO_POINTER(p->deflicker_histogram_source)));
  gtk_box_pack_start(GTK_BOX(g->vbox_deflicker), GTK_WIDGET(g->deflicker_histogram_source), TRUE, TRUE, 0);

  GtkBox *hbox1 = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  GtkLabel *label = GTK_LABEL(gtk_label_new(_("computed EC: ")));
  gtk_box_pack_start(GTK_BOX(hbox1), GTK_WIDGET(label), FALSE, FALSE, 0);

  g->deflicker_used_EC = GTK_LABEL(gtk_label_new("")); // This gets filled in by process
  g_object_set(G_OBJECT(g->deflicker_used_EC), "tooltip-text",
               _("what exposure correction have actually been used"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(hbox1), GTK_WIDGET(g->deflicker_used_EC), FALSE, FALSE, 0);
  g->deflicker_computed_exposure = NAN;

  gtk_box_pack_start(GTK_BOX(g->vbox_deflicker), GTK_WIDGET(hbox1), FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox_deflicker), TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(g->mode), "value-changed", G_CALLBACK(mode_callback), self);
  g_signal_connect(G_OBJECT(g->black), "value-changed", G_CALLBACK(black_callback), self);
  g_signal_connect(G_OBJECT(g->exposure), "value-changed", G_CALLBACK(exposure_callback), self);
  g_signal_connect(G_OBJECT(g->autoexpp), "value-changed", G_CALLBACK(autoexpp_callback), self);
  g_signal_connect(G_OBJECT(g->autoexp), "toggled", G_CALLBACK(autoexp_callback), self);
  g_signal_connect(G_OBJECT(g->deflicker_percentile), "value-changed", G_CALLBACK(deflicker_params_callback),
                   self);
  g_signal_connect(G_OBJECT(g->deflicker_target_level), "value-changed",
                   G_CALLBACK(deflicker_params_callback), self);
  g_signal_connect(G_OBJECT(g->deflicker_histogram_source), "value-changed",
                   G_CALLBACK(deflicker_histogram_source_callback), self);
  g_signal_connect(G_OBJECT(self->widget), "draw", G_CALLBACK(draw), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  free(g->deflicker_histogram);
  g->deflicker_histogram = NULL;
  g_list_free(g->deflicker_histogram_sources);
  g_list_free(g->modes);

  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
