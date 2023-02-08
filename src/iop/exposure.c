/*
    This file is part of darktable,
    Copyright (C) 2009-2023 darktable developers.

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
#include "develop/imageop_gui.h"
#include "develop/pixelpipe.h"
#include "dtgtk/paint.h"
#include "dtgtk/resetlabel.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "gui/color_picker_proxy.h"
#include "iop/iop_api.h"

#define exposure2white(x) exp2f(-(x))
#define white2exposure(x) -dt_log2f(fmaxf(1e-20f, x))

DT_MODULE_INTROSPECTION(6, dt_iop_exposure_params_t)

typedef enum dt_iop_exposure_mode_t
{
  EXPOSURE_MODE_MANUAL,   // $DESCRIPTION: "manual"
  EXPOSURE_MODE_DEFLICKER // $DESCRIPTION: "automatic"
} dt_iop_exposure_mode_t;

typedef enum dt_spot_mode_t
{
  DT_SPOT_MODE_CORRECT = 0,
  DT_SPOT_MODE_MEASURE = 1,
  DT_SPOT_MODE_LAST
} dt_spot_mode_t;

// uint16_t pixel can have any value in range [0, 65535], thus, there is
// 65536 possible values.
#define DEFLICKER_BINS_COUNT (UINT16_MAX + 1)

typedef struct dt_iop_exposure_params_t
{
  dt_iop_exposure_mode_t mode; // $DEFAULT: EXPOSURE_MODE_MANUAL
  float black;    // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "black level correction"
  float exposure; // $MIN: -18.0 $MAX: 18.0 $DEFAULT: 0.0
  float deflicker_percentile;   // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 50.0 $DESCRIPTION: "percentile"
  float deflicker_target_level; // $MIN: -18.0 $MAX: 18.0 $DEFAULT: -4.0 $DESCRIPTION: "target level"
  gboolean compensate_exposure_bias; // $DEFAULT: FALSE $DESCRIPTION: "compensate exposure bias"
} dt_iop_exposure_params_t;

typedef struct dt_iop_exposure_gui_data_t
{
  GtkWidget *mode;
  GtkWidget *black;
  GtkStack *mode_stack;
  GtkWidget *exposure;
  GtkWidget *deflicker_percentile;
  GtkWidget *deflicker_target_level;
  uint32_t *deflicker_histogram; // used to cache histogram of source file
  dt_dev_histogram_stats_t deflicker_histogram_stats;
  GtkLabel *deflicker_used_EC;
  GtkWidget *compensate_exposure_bias;
  float deflicker_computed_exposure;

  GtkWidget *spot_mode;
  GtkWidget *lightness_spot;
  GtkWidget *origin_spot, *target_spot;
  GtkWidget *Lch_origin;

  dt_gui_collapsible_section_t cs;

  dt_aligned_pixel_t spot_RGB;

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

const char** description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self,
                                _("redo the exposure of the shot as if you were still in-camera\n"
                                  "using a color-safe brightening similar to increasing ISO setting"),
                                _("corrective and creative"),
                                _("linear, RGB, scene-referred"),
                                _("linear, RGB"),
                                _("linear, RGB, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_BASIC | IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

static void _exposure_proxy_set_exposure(struct dt_iop_module_t *self, const float exposure);
static float _exposure_proxy_get_exposure(struct dt_iop_module_t *self);
static void _exposure_proxy_set_black(struct dt_iop_module_t *self, const float black);
static float _exposure_proxy_get_black(struct dt_iop_module_t *self);
static void _paint_hue(dt_iop_module_t *self);
static void _exposure_set_black(struct dt_iop_module_t *self, const float black);

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 2 && new_version == 6)
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
    n->compensate_exposure_bias = FALSE;
    return 0;
  }
  if(old_version == 3 && new_version == 6)
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
    n->compensate_exposure_bias = FALSE;
    return 0;
  }
  if(old_version == 4 && new_version == 6)
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
    n->compensate_exposure_bias = FALSE;
    return 0;
  }
  if(old_version == 5 && new_version == 6)
  {
    typedef struct dt_iop_exposure_params_v5_t
    {
      dt_iop_exposure_mode_t mode;
      float black;
      float exposure;
      float deflicker_percentile, deflicker_target_level;
    } dt_iop_exposure_params_v5_t;

    dt_iop_exposure_params_v5_t *o = (dt_iop_exposure_params_v5_t *)old_params;
    dt_iop_exposure_params_t *n = (dt_iop_exposure_params_t *)new_params;
    dt_iop_exposure_params_t *d = (dt_iop_exposure_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    n->mode = o->mode;
    n->black = o->black;
    n->exposure = o->exposure;
    n->deflicker_percentile = o->deflicker_percentile;
    n->deflicker_target_level = o->deflicker_target_level;
    n->compensate_exposure_bias = FALSE;
    return 0;
  }
  return 1;
}

void init_presets (dt_iop_module_so_t *self)
{
  dt_gui_presets_add_generic(_("magic lantern defaults"), self->op,
                             self->version(),
                             &(dt_iop_exposure_params_t){.mode = EXPOSURE_MODE_DEFLICKER,
                                                         .black = 0.0f,
                                                         .exposure = 0.0f,
                                                         .deflicker_percentile = 50.0f,
                                                         .deflicker_target_level = -4.0f,
                                                         .compensate_exposure_bias = FALSE},
                             sizeof(dt_iop_exposure_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);


  // For scene-referred workflow, since filmic doesn't brighten as base curve does,
  // we need an initial exposure boost. This might be too much in some cases but…
  // (the preset name is used in develop.c)
  dt_gui_presets_add_generic(_("scene-referred default"), self->op, self->version(),
                             &(dt_iop_exposure_params_t){.mode = EXPOSURE_MODE_MANUAL,
                                                         .black = -0.000244140625f,
                                                         .exposure = 0.7f,
                                                         .deflicker_percentile = 50.0f,
                                                         .deflicker_target_level = -4.0f,
                                                         .compensate_exposure_bias = TRUE},
                             sizeof(dt_iop_exposure_params_t), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  dt_gui_presets_update_ldr(_("scene-referred default"), self->op,
                            self->version(), FOR_RAW);
}

static void _deflicker_prepare_histogram(dt_iop_module_t *self, uint32_t **histogram,
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
                                      .crop_right = image.crop_right,
                                      .crop_bottom = image.crop_bottom };

  histogram_params.roi = &histogram_roi;
  histogram_params.bins_count = DEFLICKER_BINS_COUNT;

  dt_histogram_helper(&histogram_params, histogram_stats, IOP_CS_RAW, IOP_CS_NONE,
                      buf.buf, histogram, NULL, FALSE, NULL);

  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
}

/* input: 0 - 65535 (valid range: from black level to white level) */
/* output: -16 ... 0 */
static double _raw_to_ev(uint32_t raw, uint32_t black_level, uint32_t white_level)
{
  const uint32_t raw_max = white_level - black_level;

  // we are working on data without black clipping,
  // so we can get values which are lower than the black level !!!
  const int64_t raw_val = MAX((int64_t)raw - (int64_t)black_level, 1);

  const double raw_ev = -log2(raw_max) + log2(raw_val);

  return raw_ev;
}

static void _compute_correction(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                                const uint32_t *const histogram,
                                const dt_dev_histogram_stats_t *const histogram_stats, float *correction)
{
  const dt_iop_exposure_params_t *const p = (const dt_iop_exposure_params_t *const)p1;

  *correction = NAN;

  if(histogram == NULL) return;

  const double thr
      = CLAMP(((double)histogram_stats->pixels * (double)p->deflicker_percentile
               / (double)100.0), 0.0, (double)histogram_stats->pixels);

  size_t n = 0;
  uint32_t raw = 0;

  for(size_t i = 0; i < histogram_stats->bins_count; i++)
  {
    n += histogram[i];

    if((double)n >= thr)
    {
      raw = i;
      break;
    }
  }

  const double ev
      = _raw_to_ev(raw, (uint32_t)pipe->dsc.rawprepare.raw_black_level, pipe->dsc.rawprepare.raw_white_point);

  *correction = p->deflicker_target_level - ev;
}

static void _process_common_setup(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t*)self->gui_data;
  dt_iop_exposure_data_t *d = piece->data;

  d->black = d->params.black;
  float exposure = d->params.exposure;

  if(d->deflicker)
  {
    if(g)
    {
      // histogram is precomputed and cached
      _compute_correction(self, &d->params, piece->pipe, g->deflicker_histogram, &g->deflicker_histogram_stats,
                         &exposure);
    }
    else
    {
      uint32_t *histogram = NULL;
      dt_dev_histogram_stats_t histogram_stats;
      _deflicker_prepare_histogram(self, &histogram, &histogram_stats);
      _compute_correction(self, &d->params, piece->pipe, histogram, &histogram_stats, &exposure);
      dt_free_align(histogram);
    }

    // second, show computed correction in UI.
    if(g && (piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW))
    {
      dt_iop_gui_enter_critical_section(self);
      g->deflicker_computed_exposure = exposure;
      dt_iop_gui_leave_critical_section(self);
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
  dt_iop_exposure_global_data_t *gd = (dt_iop_exposure_global_data_t *)self->global_data;

  _process_common_setup(self, piece);

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_exposure, width, height,
    CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG((d->black)), CLARG((d->scale)));
  if(err != CL_SUCCESS) goto error;
  for(int k = 0; k < 3; k++) piece->pipe->dsc.processed_maximum[k] *= d->scale;

  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_exposure] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}
#endif

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const i, void *const o,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_exposure_data_t *const d = (const dt_iop_exposure_data_t *const)piece->data;

  _process_common_setup(self, piece);

  const int ch = piece->colors;

  const float *const restrict in = (float*)i;
  float *const restrict out = (float*)o;
  const float black = d->black;
  const float scale = d->scale;
  const size_t npixels = (size_t)roi_out->width * roi_out->height;
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(ch, npixels, black, scale, in, out)  \
  schedule(simd:static) aligned(in, out : 64)
#endif
  for(size_t k = 0; k < ch * npixels; k++)
  {
    out[k] = (in[k] - black) * scale;
  }

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
    dt_iop_alpha_copy(i, o, roi_out->width, roi_out->height);

  for(int k = 0; k < 3; k++) piece->pipe->dsc.processed_maximum[k] *= d->scale;
}


static float _get_exposure_bias(const struct dt_iop_module_t *self)
{
  float bias = 0.0f;

  // just check that pointers exist and are initialized
  if(self->dev && self->dev->image_storage.exif_exposure_bias)
    bias = self->dev->image_storage.exif_exposure_bias;

  // sanity checks, don't trust exif tags too much
  if(!isnan(bias))
    return CLAMP(bias, -5.0f, 5.0f);
  else
    return 0.0f;
}


void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)p1;
  dt_iop_exposure_data_t *d = (dt_iop_exposure_data_t *)piece->data;

  d->params.black = p->black;
  d->params.exposure = p->exposure;
  d->params.deflicker_percentile = p->deflicker_percentile;
  d->params.deflicker_target_level = p->deflicker_target_level;

  // If exposure bias compensation has been required, add it on top of user exposure correction
  if(p->compensate_exposure_bias)
    d->params.exposure -= _get_exposure_bias(self);

  d->deflicker = 0;

  if(p->mode == EXPOSURE_MODE_DEFLICKER
     && dt_image_is_raw(&self->dev->image_storage)
     && self->dev->image_storage.buf_dsc.channels == 1
     && self->dev->image_storage.buf_dsc.datatype == TYPE_UINT16)
  {
    d->deflicker = 1;
  }
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_exposure_data_t));
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

static void _autoexp_disable(dt_iop_module_t *self)
{
  dt_iop_color_picker_reset(self, TRUE);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  if(!dt_image_is_raw(&self->dev->image_storage)
     || self->dev->image_storage.buf_dsc.channels != 1
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

  dt_iop_color_picker_reset(self, TRUE);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->compensate_exposure_bias), p->compensate_exposure_bias);
  /* xgettext:no-c-format */
  gchar *label = g_strdup_printf(_("compensate camera exposure (%+.1f EV)"), _get_exposure_bias(self));
  gtk_button_set_label(GTK_BUTTON(g->compensate_exposure_bias), label);
  gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->compensate_exposure_bias))), PANGO_ELLIPSIZE_MIDDLE);
  g_free(label);

  g->spot_RGB[0] = 0.f;
  g->spot_RGB[1] = 0.f;
  g->spot_RGB[2] = 0.f;
  g->spot_RGB[3] = 0.f;

  // get the saved params
  dt_iop_gui_enter_critical_section(self);

  const float lightness = dt_conf_get_float("darkroom/modules/exposure/lightness");
  dt_bauhaus_slider_set(g->lightness_spot, lightness);

  dt_iop_gui_leave_critical_section(self);

  dt_free_align(g->deflicker_histogram);
  g->deflicker_histogram = NULL;

  gtk_label_set_text(g->deflicker_used_EC, "");
  dt_iop_gui_enter_critical_section(self);
  g->deflicker_computed_exposure = NAN;
  dt_iop_gui_leave_critical_section(self);

  switch(p->mode)
  {
    case EXPOSURE_MODE_DEFLICKER:
      _autoexp_disable(self);
      gtk_stack_set_visible_child_name(GTK_STACK(g->mode_stack), "deflicker");
      _deflicker_prepare_histogram(self, &g->deflicker_histogram, &g->deflicker_histogram_stats);
      break;
    case EXPOSURE_MODE_MANUAL:
    default:
      gtk_stack_set_visible_child_name(GTK_STACK(g->mode_stack), "manual");
      break;
  }

  dt_gui_update_collapsible_section(&g->cs);
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // from programs.conf: basic.cl
  dt_iop_exposure_global_data_t *gd
      = (dt_iop_exposure_global_data_t *)malloc(sizeof(dt_iop_exposure_global_data_t));
  module->data = gd;
  gd->kernel_exposure = dt_opencl_create_kernel(program, "exposure");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_exposure_global_data_t *gd = (dt_iop_exposure_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_exposure);
  free(module->data);
  module->data = NULL;
}

static void _exposure_set_white(struct dt_iop_module_t *self, const float white)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  const float exposure = white2exposure(white);
  if(p->exposure == exposure) return;

  p->exposure = exposure;
  if(p->black >= white) _exposure_set_black(self, white - 0.01);

  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->exposure, p->exposure);
  --darktable.gui->reset;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _exposure_proxy_set_exposure(struct dt_iop_module_t *self, const float exposure)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  if(p->mode == EXPOSURE_MODE_DEFLICKER)
  {
    dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;

    p->deflicker_target_level = exposure;

    ++darktable.gui->reset;
    dt_bauhaus_slider_set(g->deflicker_target_level, p->deflicker_target_level);
    --darktable.gui->reset;

    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
  else
  {
    const float white = exposure2white(exposure);
    _exposure_set_white(self, white);
    _autoexp_disable(self);
  }
}

static float _exposure_proxy_get_exposure(struct dt_iop_module_t *self)
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

static void _exposure_set_black(struct dt_iop_module_t *self, const float black)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  if(p->black == black) return;

  p->black = black;
  if(p->black >= exposure2white(p->exposure))
  {
    _exposure_set_white(self, p->black + 0.01);
  }

  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->black, p->black);
  --darktable.gui->reset;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _exposure_proxy_set_black(struct dt_iop_module_t *self, const float black)
{
  _autoexp_disable(self);
  _exposure_set_black(self, black);
}

static float _exposure_proxy_get_black(struct dt_iop_module_t *self)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;
  return p->black;
}

static void _auto_set_exposure(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe)
{
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  // capture gui color picked event.
  if(self->picked_color_max[0] < self->picked_color_min[0]) return;
  const float *RGB = self->picked_color;

  // Get input profile, assuming we are before colorin
  const dt_iop_order_iccprofile_info_t *const input_profile = dt_ioppr_get_pipe_input_profile_info(pipe);
  if(input_profile == NULL) return;

  // Convert to XYZ
  dt_aligned_pixel_t XYZ;
  dt_aligned_pixel_t Lab;
  dot_product(RGB, input_profile->matrix_in, XYZ);
  dt_XYZ_to_Lab(XYZ, Lab);
  Lab[1] = Lab[2] = 0.f; // make color grey to get only the equivalent lighness
  dt_Lab_to_XYZ(Lab, XYZ);
  dt_XYZ_to_sRGB(XYZ, g->spot_RGB);

  // Convert to Lch for GUI feedback (input)
  dt_aligned_pixel_t Lch;
  dt_Lab_2_LCH(Lab, Lch);

  // Write report in GUI
  gchar *str = g_strdup_printf(_("L : \t%.1f %%"), Lch[0]);
  ++darktable.gui->reset;
  gtk_label_set_text(GTK_LABEL(g->Lch_origin), str);
  --darktable.gui->reset;
  g_free(str);

  const dt_spot_mode_t mode = dt_bauhaus_combobox_get(g->spot_mode);

  if(mode == DT_SPOT_MODE_MEASURE)
  {
    // get the exposure setting
    float expo = p->exposure;

    // If the exposure bias compensation is on, we need to add it to the user param
    if(p->compensate_exposure_bias)
      expo -= _get_exposure_bias(self);

    const float white = exposure2white(-expo);

    // apply the exposure compensation
    dt_aligned_pixel_t XYZ_out;
    for(int c = 0; c < 3; c++)
      XYZ_out[c] = XYZ[c] * white;

    // Convert to Lab for GUI feedback
    dt_aligned_pixel_t Lab_out;
    dt_XYZ_to_Lab(XYZ_out, Lab_out);
    Lab_out[1] = Lab_out[2] = 0.f; // make it grey

    // Return the values in sliders
    ++darktable.gui->reset;
    dt_bauhaus_slider_set(g->lightness_spot, Lab_out[0]);
    _paint_hue(self);
    --darktable.gui->reset;

    dt_conf_set_float("darkroom/modules/exposure/lightness", Lab_out[0]);
  }
  else if(mode == DT_SPOT_MODE_CORRECT)
  {
    // Get the target color in XYZ space
    dt_aligned_pixel_t Lch_target = { 0.f };
    dt_iop_gui_enter_critical_section(self);
    Lch_target[0] = dt_bauhaus_slider_get(g->lightness_spot);
    dt_iop_gui_leave_critical_section(self);

    dt_aligned_pixel_t Lab_target = { 0.f };
    dt_LCH_2_Lab(Lch_target, Lab_target);

    dt_aligned_pixel_t XYZ_target = { 0.f };
    dt_Lab_to_XYZ(Lab_target, XYZ_target);

    // Get the ratio
    float white =  XYZ[1] / XYZ_target[1];
    float expo = -white2exposure(white);

    // If the exposure bias compensation is on, we need to subtract it from the user param
    if(p->compensate_exposure_bias)
      expo -= _get_exposure_bias(self);

    white = exposure2white(-expo);
    _exposure_set_white(self, white);
  }
}


void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  if(darktable.gui->reset) return;
  _auto_set_exposure(self, piece->pipe);
}


void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  if(w == g->mode)
  {
    dt_free_align(g->deflicker_histogram);
    g->deflicker_histogram = NULL;

    switch(p->mode)
    {
      case EXPOSURE_MODE_DEFLICKER:
        _autoexp_disable(self);
        if(!dt_image_is_raw(&self->dev->image_storage)
           || self->dev->image_storage.buf_dsc.channels != 1
           || self->dev->image_storage.buf_dsc.datatype != TYPE_UINT16)
        {
          p->mode = EXPOSURE_MODE_MANUAL;
          dt_bauhaus_combobox_set(g->mode, p->mode);
          gtk_widget_set_sensitive(GTK_WIDGET(g->mode), FALSE);
          break;
        }
        gtk_stack_set_visible_child_name(GTK_STACK(g->mode_stack), "deflicker");
        _deflicker_prepare_histogram(self, &g->deflicker_histogram, &g->deflicker_histogram_stats);
        break;
      case EXPOSURE_MODE_MANUAL:
      default:
        gtk_stack_set_visible_child_name(GTK_STACK(g->mode_stack), "manual");
        break;
    }
  }
  else if(w == g->exposure)
  {
    const float white = exposure2white(p->exposure);
    if(p->black >= white)
      _exposure_set_black(self, white - 0.01);
  }
  else if(w == g->black)
  {
    const float white = exposure2white(p->exposure);
    if(p->black >= white)
      _exposure_set_white(self, p->black + 0.01);
  }
}


static gboolean _draw(GtkWidget *widget, cairo_t *cr, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return FALSE;

  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;

  dt_iop_gui_enter_critical_section(self);
  if(!isnan(g->deflicker_computed_exposure))
  {
    gchar *str = g_strdup_printf(_("%.2f EV"), g->deflicker_computed_exposure);

    ++darktable.gui->reset;
    gtk_label_set_text(g->deflicker_used_EC, str);
    --darktable.gui->reset;

    g_free(str);
  }
  dt_iop_gui_leave_critical_section(self);
  return FALSE;
}


static gboolean _target_color_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;

  // Init
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  // Margins
  const double INNER_PADDING = 4.0;
  const float margin = 2. * DT_PIXEL_APPLY_DPI(1.5);
  width -= 2* INNER_PADDING;
  height -= 2 * margin;

  // Paint target color
  dt_aligned_pixel_t RGB = { 0 };
  dt_aligned_pixel_t Lch = { 0 };
  dt_aligned_pixel_t Lab = { 0 };
  dt_aligned_pixel_t XYZ = { 0 };
  Lch[0] = dt_bauhaus_slider_get(g->lightness_spot);
  Lch[1] = 0.f;
  Lch[2] = 0.f;
  dt_LCH_2_Lab(Lch, Lab);
  dt_Lab_to_XYZ(Lab, XYZ);
  dt_XYZ_to_sRGB(XYZ, RGB);

  cairo_set_source_rgb(cr, RGB[0], RGB[1], RGB[2]);
  cairo_rectangle(cr, INNER_PADDING, margin, width, height);
  cairo_fill(cr);

  // Clean
  cairo_stroke(cr);
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}


static gboolean _origin_color_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;

  // Init
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width;
  int height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  // Margins
  const double INNER_PADDING = 4.0;
  const float margin = 2. * DT_PIXEL_APPLY_DPI(1.5);
  width -= 2* INNER_PADDING;
  height -= 2 * margin;

  cairo_set_source_rgb(cr, g->spot_RGB[0], g->spot_RGB[1], g->spot_RGB[2]);
  cairo_rectangle(cr, INNER_PADDING, margin, width, height);
  cairo_fill(cr);

  // Clean
  cairo_stroke(cr);
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

static void _paint_hue(dt_iop_module_t *self)
{
  // update the fill background color of LCh sliders
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;

  const float lightness_min = dt_bauhaus_slider_get_hard_min(g->lightness_spot);
  const float lightness_max = dt_bauhaus_slider_get_hard_max(g->lightness_spot);

  const float lightness_range = lightness_max - lightness_min;

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    const float x = lightness_min + stop * lightness_range;
    dt_aligned_pixel_t RGB = { 0 };
    const dt_aligned_pixel_t Lch = { x, 0.f, 0. };
    dt_aligned_pixel_t Lab = { 0 };
    dt_aligned_pixel_t XYZ = { 0 };

    dt_LCH_2_Lab(Lch, Lab);
    dt_Lab_to_XYZ(Lab, XYZ);
    dt_XYZ_to_sRGB(XYZ, RGB);

    dt_bauhaus_slider_set_stop(g->lightness_spot, stop, RGB[0], RGB[1], RGB[2]);
  }

  gtk_widget_queue_draw(g->lightness_spot);
  gtk_widget_queue_draw(g->target_spot);
}


static void _spot_settings_changed_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;

  dt_aligned_pixel_t Lch_target = { 0.f };

  Lch_target[0] = dt_bauhaus_slider_get(g->lightness_spot);

  // Save the color on change
  dt_conf_set_float("darkroom/modules/exposure/lightness", Lch_target[0]);

  ++darktable.gui->reset;
  _paint_hue(self);
  --darktable.gui->reset;

  // Re-run auto compute if color picker active and mode is correct
  const dt_spot_mode_t mode = dt_bauhaus_combobox_get(g->spot_mode);
  if(mode == DT_SPOT_MODE_CORRECT)
    _auto_set_exposure(self, darktable.develop->pipe);
  // else : just record new values and do nothing
}

void gui_reset(struct dt_iop_module_t *self)
{
  dt_iop_color_picker_reset(self, TRUE);
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_exposure_gui_data_t *g = IOP_GUI_ALLOC(exposure);

  g->deflicker_histogram = NULL;

  g->mode_stack = GTK_STACK(gtk_stack_new());
  gtk_stack_set_homogeneous(GTK_STACK(g->mode_stack),FALSE);

  GtkWidget *vbox_manual = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_stack_add_named(GTK_STACK(g->mode_stack), vbox_manual, "manual");

  g->compensate_exposure_bias = dt_bauhaus_toggle_from_params(self, "compensate_exposure_bias");
  gtk_widget_set_tooltip_text(g->compensate_exposure_bias, _("automatically remove the camera exposure bias\n"
                                                             "this is useful if you exposed the image to the right."));

  g->exposure = dt_color_picker_new(self, DT_COLOR_PICKER_AREA,
                                    dt_bauhaus_slider_from_params(self, N_("exposure")));
  gtk_widget_set_tooltip_text(g->exposure, _("adjust the exposure correction"));
  dt_bauhaus_slider_set_digits(g->exposure, 3);
  dt_bauhaus_slider_set_format(g->exposure, _(" EV"));
  dt_bauhaus_slider_set_soft_range(g->exposure, -3.0, 4.0);

  GtkWidget *vbox_deflicker = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_stack_add_named(GTK_STACK(g->mode_stack), vbox_deflicker, "deflicker");

  g->deflicker_percentile = dt_bauhaus_slider_from_params(self, "deflicker_percentile");
  dt_bauhaus_slider_set_format(g->deflicker_percentile, "%");
  gtk_widget_set_tooltip_text(g->deflicker_percentile,
                              // xgettext:no-c-format
                              _("where in the histogram to meter for deflicking. E.g. 50% is median"));

  g->deflicker_target_level = dt_bauhaus_slider_from_params(self, "deflicker_target_level");
  dt_bauhaus_slider_set_format(g->deflicker_target_level, _(" EV"));
  gtk_widget_set_tooltip_text(g->deflicker_target_level,
                              _("where to place the exposure level for processed pics, EV below overexposure."));

  GtkBox *hbox1 = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_box_pack_start(GTK_BOX(hbox1), GTK_WIDGET(dt_ui_label_new(_("computed EC: "))), FALSE, FALSE, 0);
  g->deflicker_used_EC = GTK_LABEL(dt_ui_label_new("")); // This gets filled in by process
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->deflicker_used_EC), _("what exposure correction has actually been used"));
  gtk_box_pack_start(GTK_BOX(hbox1), GTK_WIDGET(g->deflicker_used_EC), FALSE, FALSE, 0);

  dt_iop_gui_enter_critical_section(self);
  g->deflicker_computed_exposure = NAN;
  dt_iop_gui_leave_critical_section(self);

  gtk_box_pack_start(GTK_BOX(vbox_deflicker), GTK_WIDGET(hbox1), FALSE, FALSE, 0);

  // Start building top level widget
  self->widget = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE));

  g->mode = dt_bauhaus_combobox_from_params(self, N_("mode"));

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->mode_stack), TRUE, TRUE, 0);

  g->black = dt_bauhaus_slider_from_params(self, "black");
  gtk_widget_set_tooltip_text(g->black, _("adjust the black level to unclip negative RGB values.\n"
                                          "you should never use it to add more density in blacks!\n"
                                          "if poorly set, it will clip near-black colors out of gamut\n"
                                          "by pushing RGB values into negatives."));
  dt_bauhaus_slider_set_digits(g->black, 4);
  dt_bauhaus_slider_set_soft_range(g->black, -0.1, 0.1);

  dt_gui_new_collapsible_section
    (&g->cs,
     "plugins/darkroom/exposure/mapping",
     _("spot exposure mapping"),
     GTK_BOX(self->widget));

  gtk_widget_set_tooltip_text(g->cs.expander, _("define a target brightness, in terms of exposure, for a selected region of the image (the control sample), which you then match against the same target brightness in other images. the control sample can either be a critical part of your subject or a non-moving and consistently-lit surface over your series of images."));

  DT_BAUHAUS_COMBOBOX_NEW_FULL(g->spot_mode, self, NULL, N_("spot mode"),
                                _("\"correction\" automatically adjust exposure\n"
                                  "such that the input lightness is mapped to the target.\n"
                                  "\"measure\" simply shows how an input color is mapped by the exposure compensation\n"
                                  "and can be used to define a target."),
                                0, _spot_settings_changed_callback, self,
                                N_("correction"),
                                N_("measure"));
  gtk_box_pack_start(GTK_BOX(g->cs.container), GTK_WIDGET(g->spot_mode), TRUE, TRUE, 0);

  GtkWidget *hhbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(darktable.bauhaus->quad_width));
  GtkWidget *vvbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  gtk_box_pack_start(GTK_BOX(vvbox), dt_ui_section_label_new(_("input")), FALSE, FALSE, 0);

  g->origin_spot = GTK_WIDGET(gtk_drawing_area_new());
  gtk_widget_set_size_request(g->origin_spot, 2 * DT_PIXEL_APPLY_DPI(darktable.bauhaus->quad_width),
                                              DT_PIXEL_APPLY_DPI(darktable.bauhaus->quad_width));
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->origin_spot),
                              _("the input color that should be mapped to the target"));

  g_signal_connect(G_OBJECT(g->origin_spot), "draw", G_CALLBACK(_origin_color_draw), self);
  gtk_box_pack_start(GTK_BOX(vvbox), g->origin_spot, TRUE, TRUE, 0);

  g->Lch_origin = gtk_label_new(_("L : \tN/A"));
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->Lch_origin),
                              _("these LCh coordinates are computed from CIE Lab 1976 coordinates"));
  gtk_box_pack_start(GTK_BOX(vvbox), GTK_WIDGET(g->Lch_origin), FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(hhbox), GTK_WIDGET(vvbox), FALSE, FALSE, DT_BAUHAUS_SPACE);

  vvbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  gtk_box_pack_start(GTK_BOX(vvbox), dt_ui_section_label_new(_("target")), FALSE, TRUE, 0);

  g->target_spot = GTK_WIDGET(gtk_drawing_area_new());
  gtk_widget_set_size_request(g->target_spot, 2 * DT_PIXEL_APPLY_DPI(darktable.bauhaus->quad_width),
                                              DT_PIXEL_APPLY_DPI(darktable.bauhaus->quad_width));
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->target_spot),
                              _("the desired target exposure after mapping"));

  g_signal_connect(G_OBJECT(g->target_spot), "draw", G_CALLBACK(_target_color_draw), self);
  gtk_box_pack_start(GTK_BOX(vvbox), g->target_spot, TRUE, TRUE, 0);

  g->lightness_spot = dt_bauhaus_slider_new_with_range(self, 0., 100., 0, 0, 1);
  dt_bauhaus_widget_set_label(g->lightness_spot, NULL, N_("lightness"));
  dt_bauhaus_slider_set_format(g->lightness_spot, "%");
  dt_bauhaus_slider_set_default(g->lightness_spot, 50.f);
  gtk_box_pack_start(GTK_BOX(vvbox), GTK_WIDGET(g->lightness_spot), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->lightness_spot), "value-changed", G_CALLBACK(_spot_settings_changed_callback), self);

  gtk_box_pack_start(GTK_BOX(hhbox), GTK_WIDGET(vvbox), TRUE, TRUE, DT_BAUHAUS_SPACE);

  gtk_box_pack_start(GTK_BOX(g->cs.container), GTK_WIDGET(hhbox), FALSE, FALSE, 0);

  g_signal_connect(G_OBJECT(self->widget), "draw", G_CALLBACK(_draw), self);

  /* register hooks with current dev so that  histogram
     can interact with this module.
  */
  dt_dev_proxy_exposure_t *instance = &darktable.develop->proxy.exposure;
  instance->module = self;
  instance->set_exposure = _exposure_proxy_set_exposure;
  instance->get_exposure = _exposure_proxy_get_exposure;
  instance->set_black = _exposure_proxy_set_black;
  instance->get_black = _exposure_proxy_get_black;
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;

  if(darktable.develop->proxy.exposure.module == self)
    darktable.develop->proxy.exposure.module = NULL;

  dt_free_align(g->deflicker_histogram);
  g->deflicker_histogram = NULL;

  IOP_GUI_FREE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
