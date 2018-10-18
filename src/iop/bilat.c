/*
    This file is part of darktable,
    copyright (c) 2009--2016 johannes hanika.

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
// our includes go first:
#include "bauhaus/bauhaus.h"
#include "common/bilateral.h"
#include "common/bilateralcl.h"
#include "common/locallaplacian.h"
#include "common/locallaplaciancl.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/tiling.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"
#include "common/iop_group.h"

#include <gtk/gtk.h>
#include <stdlib.h>

// this is the version of the modules parameters,
// and includes version information about compile-time dt
DT_MODULE_INTROSPECTION(3, dt_iop_bilat_params_t)

typedef enum dt_iop_bilat_mode_t
{
  s_mode_bilateral = 0,
  s_mode_local_laplacian = 1,
}
dt_iop_bilat_mode_t;

typedef struct dt_iop_bilat_params_t
{
  uint32_t mode;
  float sigma_r;
  float sigma_s;
  float detail;
  float midtone;
}
dt_iop_bilat_params_t;

typedef struct dt_iop_bilat_params_v2_t
{
  uint32_t mode;
  float sigma_r;
  float sigma_s;
  float detail;
}
dt_iop_bilat_params_v2_t;

typedef struct dt_iop_bilat_params_v1_t
{
  float sigma_r;
  float sigma_s;
  float detail;
}
dt_iop_bilat_params_v1_t;

typedef dt_iop_bilat_params_t dt_iop_bilat_data_t;

typedef struct dt_iop_bilat_gui_data_t
{
  GtkWidget *highlights;
  GtkWidget *shadows;
  GtkWidget *midtone;
  GtkWidget *spatial;
  GtkWidget *range;
  GtkWidget *detail;
  GtkWidget *mode;

  local_laplacian_boundary_t ll_boundary;
  uint64_t hash;
  dt_pthread_mutex_t lock;
}
dt_iop_bilat_gui_data_t;

// this returns a translatable name
const char *name()
{
  return _("local contrast");
}

// some additional flags (self explanatory i think):
int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

// where does it appear in the gui?
int groups()
{
  return dt_iop_get_group("local contrast", IOP_GROUP_TONE);
}

int legacy_params(
    dt_iop_module_t *self, const void *const old_params, const int old_version,
    void *new_params, const int new_version)
{
  if(old_version == 2 && new_version == 3)
  {
    const dt_iop_bilat_params_v2_t *p2 = old_params;
    dt_iop_bilat_params_t *p = new_params;
    p->detail  = p2->detail;
    p->sigma_r = p2->sigma_r;
    p->sigma_s = p2->sigma_s;
    p->midtone = 0.2f;
    p->mode    = p2->mode;
    return 0;
  }
  else if(old_version == 1 && new_version == 3)
  {
    const dt_iop_bilat_params_v1_t *p1 = old_params;
    dt_iop_bilat_params_t *p = new_params;
    p->detail  = p1->detail;
    p->sigma_r = p1->sigma_r;
    p->sigma_s = p1->sigma_s;
    p->midtone = 0.2f;
    p->mode    = s_mode_bilateral;
    return 0;
  }
  return 1;
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_bilat_data_t *d = (dt_iop_bilat_data_t *)piece->data;

  if(d->mode == s_mode_bilateral)
  {
    // the total scale is composed of scale before input to the pipeline (iscale),
    // and the scale of the roi.
    const float scale = piece->iscale / roi_in->scale;
    const float sigma_r = d->sigma_r; // does not depend on scale
    const float sigma_s = d->sigma_s / scale;
    cl_int err = -666;

    dt_bilateral_cl_t *b
      = dt_bilateral_init_cl(piece->pipe->devid, roi_in->width, roi_in->height, sigma_s, sigma_r);
    if(!b) goto error;
    err = dt_bilateral_splat_cl(b, dev_in);
    if(err != CL_SUCCESS) goto error;
    err = dt_bilateral_blur_cl(b);
    if(err != CL_SUCCESS) goto error;
    err = dt_bilateral_slice_cl(b, dev_in, dev_out, d->detail);
    if(err != CL_SUCCESS) goto error;
    dt_bilateral_free_cl(b);
    return TRUE;
error:
    dt_bilateral_free_cl(b);
    dt_print(DT_DEBUG_OPENCL, "[opencl_bilateral] couldn't enqueue kernel! %d\n", err);
    return FALSE;
  }
  else // mode == s_mode_local_laplacian
  {
    dt_local_laplacian_cl_t *b = dt_local_laplacian_init_cl(piece->pipe->devid, roi_in->width, roi_in->height,
        d->midtone, d->sigma_s, d->sigma_r, d->detail);
    if(!b) goto error_ll;
    if(dt_local_laplacian_cl(b, dev_in, dev_out) != CL_SUCCESS) goto error_ll;
    dt_local_laplacian_free_cl(b);
    return TRUE;
error_ll:
    dt_local_laplacian_free_cl(b);
    return FALSE;
  }
}
#endif


void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  dt_iop_bilat_data_t *d = (dt_iop_bilat_data_t *)piece->data;
  // the total scale is composed of scale before input to the pipeline (iscale),
  // and the scale of the roi.

  if(d->mode == s_mode_bilateral)
  {
    const float scale = piece->iscale / roi_in->scale;
    const float sigma_r = d->sigma_r;
    const float sigma_s = d->sigma_s / scale;

    const int width = roi_in->width;
    const int height = roi_in->height;
    const int channels = piece->colors;

    const size_t basebuffer = width * height * channels * sizeof(float);

    tiling->factor = 2.0f + (float)dt_bilateral_memory_use(width, height, sigma_s, sigma_r) / basebuffer;
    tiling->maxbuf
        = fmax(1.0f, (float)dt_bilateral_singlebuffer_size(width, height, sigma_s, sigma_r) / basebuffer);
    tiling->overhead = 0;
    tiling->overlap = ceilf(4 * sigma_s);
    tiling->xalign = 1;
    tiling->yalign = 1;
  }
  else  // mode == s_mode_local_laplacian
  {
    const int width = roi_in->width;
    const int height = roi_in->height;
    const int channels = piece->colors;

    const size_t basebuffer = width * height * channels * sizeof(float);
    const int rad = MIN(roi_in->width, ceilf(256 * roi_in->scale / piece->iscale));

    tiling->factor = 2.0f + (float)local_laplacian_memory_use(width, height) / basebuffer;
    tiling->maxbuf
        = fmax(1.0f, (float)local_laplacian_singlebuffer_size(width, height) / basebuffer);
    tiling->overhead = 0;
    tiling->overlap = rad;
    tiling->xalign = 1;
    tiling->yalign = 1;
  }
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_bilat_params_t *p = (dt_iop_bilat_params_t *)p1;
  dt_iop_bilat_data_t *d = (dt_iop_bilat_data_t *)piece->data;
  *d = *p;

#ifdef HAVE_OPENCL
  if(d->mode == s_mode_bilateral)
    piece->process_cl_ready = (piece->process_cl_ready && !(darktable.opencl->avoid_atomics));
#endif
  if(d->mode == s_mode_local_laplacian)
    piece->process_tiling_ready = 0; // can't deal with tiles, sorry.
}


void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_bilat_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}


void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}


#if defined(__SSE2__)
void process_sse2(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const i, void *const o,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  // this is called for preview and full pipe separately, each with its own pixelpipe piece.
  // get our data struct:
  dt_iop_bilat_data_t *d = (dt_iop_bilat_data_t *)piece->data;
  dt_iop_bilat_gui_data_t *g = self->gui_data;
  // the total scale is composed of scale before input to the pipeline (iscale),
  // and the scale of the roi.
  const float scale = piece->iscale / roi_in->scale;
  const float sigma_r = d->sigma_r; // does not depend on scale
  const float sigma_s = d->sigma_s / scale;

  if(d->mode == s_mode_bilateral)
  {
    dt_bilateral_t *b = dt_bilateral_init(roi_in->width, roi_in->height, sigma_s, sigma_r);
    dt_bilateral_splat(b, (float *)i);
    dt_bilateral_blur(b);
    dt_bilateral_slice(b, (float *)i, (float *)o, d->detail);
    dt_bilateral_free(b);
  }
  else // s_mode_local_laplacian
  {
    local_laplacian_boundary_t b = {0};
    if(self->dev->gui_attached && g && piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
    {
      b.mode = 1;
    }
    else if(self->dev->gui_attached && g && piece->pipe->type == DT_DEV_PIXELPIPE_FULL)
    {
      // full pipeline working on ROI needs boundary conditions from preview pipe
      // only do this if roi covers less than 90% of full width
      if(MIN(roi_in->width/roi_in->scale / piece->buf_in.width,
             roi_in->height/roi_in->scale / piece->buf_in.height) < 0.9)
      {
        dt_pthread_mutex_lock(&g->lock);
        const uint64_t hash = g->hash;
        dt_pthread_mutex_unlock(&g->lock);
        if(hash == 0)
        {
          // Don't try grabbing anything from preview pipe.
        }
        else if(!dt_dev_sync_pixelpipe_hash(self->dev, piece->pipe, 0, self->priority, &g->lock, &g->hash))
        {
          // TODO: remove this debug output at some point:
          dt_control_log(_("local laplacian: inconsistent output"));
        }
        else
        {
          dt_pthread_mutex_lock(&g->lock);
          // grab preview pipe buffers here:
          b = g->ll_boundary;
          dt_pthread_mutex_unlock(&g->lock);
          if(b.wd > 0 && b.ht > 0) b.mode = 2;
        }
      }
    }

    b.roi = roi_in;
    b.buf = &piece->buf_in;
    // also lock the ll_boundary in case we're using it.
    // could get away without this if the preview pipe didn't also free the data below.
    const int lockit = self->dev->gui_attached && g && piece->pipe->type == DT_DEV_PIXELPIPE_FULL;
    if(lockit)
    {
      dt_pthread_mutex_lock(&g->lock);
      local_laplacian_sse2(i, o, roi_in->width, roi_in->height, d->midtone, d->sigma_s, d->sigma_r, d->detail, &b);
      dt_pthread_mutex_unlock(&g->lock);
    }
    else local_laplacian_sse2(i, o, roi_in->width, roi_in->height, d->midtone, d->sigma_s, d->sigma_r, d->detail, &b);

    // preview pixelpipe stores values.
    if(self->dev->gui_attached && g && piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
    {
      uint64_t hash = dt_dev_hash_plus(self->dev, piece->pipe, 0, self->priority);
      dt_pthread_mutex_lock(&g->lock);
      // store buffer pointers on gui struct. maybe need to swap/free old ones
      local_laplacian_boundary_free(&g->ll_boundary);
      g->ll_boundary = b;
      g->hash = hash;
      dt_pthread_mutex_unlock(&g->lock);
    }
  }

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(i, o, roi_in->width, roi_in->height);
}
#endif

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const i, void *const o,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  // this is called for preview and full pipe separately, each with its own pixelpipe piece.
  // get our data struct:
  dt_iop_bilat_data_t *d = (dt_iop_bilat_data_t *)piece->data;
  // the total scale is composed of scale before input to the pipeline (iscale),
  // and the scale of the roi.
  const float scale = piece->iscale / roi_in->scale;
  const float sigma_r = d->sigma_r; // does not depend on scale
  const float sigma_s = d->sigma_s / scale;

  if(d->mode == s_mode_bilateral)
  {
    dt_bilateral_t *b = dt_bilateral_init(roi_in->width, roi_in->height, sigma_s, sigma_r);
    dt_bilateral_splat(b, (float *)i);
    dt_bilateral_blur(b);
    dt_bilateral_slice(b, (float *)i, (float *)o, d->detail);
    dt_bilateral_free(b);
  }
  else // s_mode_local_laplacian
  {
    local_laplacian(i, o, roi_in->width, roi_in->height, d->midtone, d->sigma_s, d->sigma_r, d->detail, 0);
  }

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(i, o, roi_in->width, roi_in->height);
}

/** init, cleanup, commit to pipeline */
void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_bilat_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_bilat_params_t));
  // our module is disabled by default
  // by default:
  module->default_enabled = 0;
  // order has to be changed by editing the dependencies in tools/iop_dependencies.py
  module->priority = 585; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_bilat_params_t);
  module->gui_data = NULL;
  // init defaults:
  dt_iop_bilat_params_t tmp = (dt_iop_bilat_params_t){ s_mode_local_laplacian, 1.0, 1.0, 0.2, 0.2 };

  memcpy(module->params, &tmp, sizeof(dt_iop_bilat_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_bilat_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

static void spatial_callback(GtkWidget *w, dt_iop_module_t *self)
{
  dt_iop_bilat_params_t *p = (dt_iop_bilat_params_t *)self->params;
  p->sigma_s = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void range_callback(GtkWidget *w, dt_iop_module_t *self)
{
  dt_iop_bilat_params_t *p = (dt_iop_bilat_params_t *)self->params;
  p->sigma_r = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void highlights_callback(GtkWidget *w, dt_iop_module_t *self)
{
  dt_iop_bilat_params_t *p = (dt_iop_bilat_params_t *)self->params;
  p->sigma_r = dt_bauhaus_slider_get(w)/100.0f;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void shadows_callback(GtkWidget *w, dt_iop_module_t *self)
{
  dt_iop_bilat_params_t *p = (dt_iop_bilat_params_t *)self->params;
  p->sigma_s = dt_bauhaus_slider_get(w)/100.0f;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void midtone_callback(GtkWidget *w, dt_iop_module_t *self)
{
  dt_iop_bilat_params_t *p = self->params;
  p->midtone = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void detail_callback(GtkWidget *w, dt_iop_module_t *self)
{
  dt_iop_bilat_params_t *p = (dt_iop_bilat_params_t *)self->params;
  p->detail = (dt_bauhaus_slider_get(w)-100.0f)/100.0f;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void mode_callback(GtkWidget *w, dt_iop_module_t *self)
{
  dt_iop_bilat_params_t *p = (dt_iop_bilat_params_t *)self->params;
  p->mode = dt_bauhaus_combobox_get(w);
  dt_iop_bilat_gui_data_t *g = (dt_iop_bilat_gui_data_t *)self->gui_data;
  if(p->mode == s_mode_local_laplacian)
  {
    gtk_widget_set_visible(g->highlights, TRUE);
    gtk_widget_set_visible(g->shadows, TRUE);
    gtk_widget_set_visible(g->midtone, TRUE);
    gtk_widget_set_visible(g->range, FALSE);
    gtk_widget_set_visible(g->spatial, FALSE);
    dt_bauhaus_slider_set(g->highlights, 100.0f);
    dt_bauhaus_slider_set(g->shadows, 100.0f);
  }
  else
  {
    gtk_widget_set_visible(g->highlights, FALSE);
    gtk_widget_set_visible(g->shadows, FALSE);
    gtk_widget_set_visible(g->midtone, FALSE);
    gtk_widget_set_visible(g->range, TRUE);
    gtk_widget_set_visible(g->spatial, TRUE);
    dt_bauhaus_slider_set(g->range, 20.0f);
    dt_bauhaus_slider_set(g->spatial, 50.0f);
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

/** gui callbacks, these are needed. */
void gui_update(dt_iop_module_t *self)
{
  // let gui slider match current parameters:
  dt_iop_bilat_gui_data_t *g = (dt_iop_bilat_gui_data_t *)self->gui_data;
  dt_iop_bilat_params_t *p = (dt_iop_bilat_params_t *)self->params;
  dt_bauhaus_slider_set(g->detail, 100.0f*p->detail+100.0f);
  dt_bauhaus_combobox_set(g->mode, p->mode);
  if(p->mode == s_mode_local_laplacian)
  {
    dt_bauhaus_slider_set(g->shadows, p->sigma_s*100.0f);
    dt_bauhaus_slider_set(g->highlights, p->sigma_r*100.0f);
    dt_bauhaus_slider_set(g->midtone, p->midtone);
    gtk_widget_set_visible(g->range, FALSE);
    gtk_widget_set_visible(g->spatial, FALSE);
    gtk_widget_set_visible(g->highlights, TRUE);
    gtk_widget_set_visible(g->shadows, TRUE);
    gtk_widget_set_visible(g->midtone, TRUE);
    dt_pthread_mutex_lock(&g->lock);
    g->hash = 0;
    dt_pthread_mutex_unlock(&g->lock);
  }
  else
  {
    dt_bauhaus_slider_set(g->spatial, p->sigma_s);
    dt_bauhaus_slider_set(g->range, p->sigma_r);
    gtk_widget_set_visible(g->range, TRUE);
    gtk_widget_set_visible(g->spatial, TRUE);
    gtk_widget_set_visible(g->highlights, FALSE);
    gtk_widget_set_visible(g->shadows, FALSE);
    gtk_widget_set_visible(g->midtone, FALSE);
  }
}

void gui_init(dt_iop_module_t *self)
{
  // init the slider (more sophisticated layouts are possible with gtk tables and boxes):
  self->gui_data = malloc(sizeof(dt_iop_bilat_gui_data_t));
  dt_iop_bilat_gui_data_t *g = (dt_iop_bilat_gui_data_t *)self->gui_data;
  memset(&g->ll_boundary, 0, sizeof(local_laplacian_boundary_t));
  dt_pthread_mutex_init(&g->lock, NULL);
  g->hash = 0;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));

  g->mode = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->mode, NULL, _("mode"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->mode, TRUE, TRUE, 0);
  dt_bauhaus_combobox_add(g->mode, _("bilateral grid"));
  dt_bauhaus_combobox_add(g->mode, _("local laplacian filter"));
  dt_bauhaus_combobox_set_default(g->mode, s_mode_local_laplacian);
  dt_bauhaus_combobox_set(g->mode, s_mode_local_laplacian);
  gtk_widget_set_tooltip_text(g->mode, _("the filter used for local contrast enhancement. bilateral is faster but can lead to artifacts around edges for extreme settings."));

  g->detail = dt_bauhaus_slider_new_with_range(self, 0.0, 500.0, 1.0, 120.0, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->detail, TRUE, TRUE, 0);
  dt_bauhaus_widget_set_label(g->detail, NULL, _("detail"));
  dt_bauhaus_slider_set_format(g->detail, "%.0f%%");
  gtk_widget_set_tooltip_text(g->detail, _("changes the local contrast"));

  g->spatial = dt_bauhaus_slider_new_with_range(self, 1, 100, 1, 50, 0);
  dt_bauhaus_widget_set_label(g->spatial, NULL, _("coarseness"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->spatial, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->spatial, _("feature size of local details (spatial sigma of bilateral filter)"));

  g->range = dt_bauhaus_slider_new_with_range(self, 1, 100, 1, 20, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->range, TRUE, TRUE, 0);
  dt_bauhaus_widget_set_label(g->range, NULL, _("contrast"));
  gtk_widget_set_tooltip_text(g->range, _("L difference to detect edges (range sigma of bilateral filter)"));

  g->highlights = dt_bauhaus_slider_new_with_range(self, 0.0, 200.0, 1.0, 100.0, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->highlights, TRUE, TRUE, 0);
  dt_bauhaus_widget_set_label(g->highlights, NULL, _("highlights"));
  dt_bauhaus_slider_set_format(g->highlights, "%.0f%%");
  gtk_widget_set_tooltip_text(g->highlights, _("changes the local contrast of highlights"));

  g->shadows = dt_bauhaus_slider_new_with_range(self, 0.0, 200.0, 1.0, 100.0, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->shadows, TRUE, TRUE, 0);
  dt_bauhaus_widget_set_label(g->shadows, NULL, _("shadows"));
  gtk_widget_set_tooltip_text(g->shadows, _("changes the local contrast of shadows"));
  dt_bauhaus_slider_set_format(g->shadows, "%.0f%%");

  g->midtone = dt_bauhaus_slider_new_with_range(self, 0.001, 1.0, 0.001, 0.2, 3);
  gtk_box_pack_start(GTK_BOX(self->widget), g->midtone, TRUE, TRUE, 0);
  dt_bauhaus_widget_set_label(g->midtone, NULL, _("midtone range"));
  gtk_widget_set_tooltip_text(g->midtone, _("defines what counts as midtones. lower for better dynamic range compression (reduce shadow and highlight contrast), increase for more powerful local contrast"));

  // work around multi-instance issue which calls show all a fair bit:
  g_object_set(G_OBJECT(g->highlights), "no-show-all", TRUE, NULL);
  g_object_set(G_OBJECT(g->shadows), "no-show-all", TRUE, NULL);
  g_object_set(G_OBJECT(g->midtone), "no-show-all", TRUE, NULL);
  g_object_set(G_OBJECT(g->range), "no-show-all", TRUE, NULL);
  g_object_set(G_OBJECT(g->spatial), "no-show-all", TRUE, NULL);

  g_signal_connect(G_OBJECT(g->spatial), "value-changed", G_CALLBACK(spatial_callback), self);
  g_signal_connect(G_OBJECT(g->range), "value-changed", G_CALLBACK(range_callback), self);
  g_signal_connect(G_OBJECT(g->detail), "value-changed", G_CALLBACK(detail_callback), self);
  g_signal_connect(G_OBJECT(g->highlights), "value-changed", G_CALLBACK(highlights_callback), self);
  g_signal_connect(G_OBJECT(g->shadows), "value-changed", G_CALLBACK(shadows_callback), self);
  g_signal_connect(G_OBJECT(g->midtone), "value-changed", G_CALLBACK(midtone_callback), self);
  g_signal_connect(G_OBJECT(g->mode), "value-changed", G_CALLBACK(mode_callback), self);
}

void gui_cleanup(dt_iop_module_t *self)
{
  // nothing else necessary, gtk will clean up the slider.
  dt_iop_bilat_gui_data_t *g = (dt_iop_bilat_gui_data_t *)self->gui_data;
  local_laplacian_boundary_free(&g->ll_boundary);
  dt_pthread_mutex_destroy(&g->lock);
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
