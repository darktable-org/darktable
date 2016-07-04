/*
    This file is part of darktable,
    copyright (c) 2009--2012 johannes hanika.

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
#include "common/bilateral.h"
#include "common/bilateralcl.h"
#include "common/colorspaces.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/tiling.h"
#include "dtgtk/drawingarea.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE_INTROSPECTION(2, dt_iop_monochrome_params_t)

#define DT_COLORCORRECTION_INSET DT_PIXEL_APPLY_DPI(5)
#define DT_COLORCORRECTION_MAX 40.
#define PANEL_WIDTH 256.0f

typedef struct dt_iop_monochrome_params_t
{
  float a, b, size, highlights;
} dt_iop_monochrome_params_t;

typedef struct dt_iop_monochrome_data_t
{
  float a, b, size, highlights;
} dt_iop_monochrome_data_t;

typedef struct dt_iop_monochrome_gui_data_t
{
  GtkDrawingArea *area;
  GtkWidget *highlights;
  int dragging;
  cmsHTRANSFORM xform;
} dt_iop_monochrome_gui_data_t;

typedef struct dt_iop_monochrome_global_data_t
{
  int kernel_monochrome_filter, kernel_monochrome;
} dt_iop_monochrome_global_data_t;


const char *name()
{
  return _("monochrome");
}

int groups()
{
  return IOP_GROUP_COLOR;
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 2)
  {
    dt_iop_monochrome_params_t *p1 = (dt_iop_monochrome_params_t *)old_params;
    dt_iop_monochrome_params_t *p2 = (dt_iop_monochrome_params_t *)new_params;
    memcpy(p2, p1, sizeof(dt_iop_monochrome_params_t) - sizeof(float));
    p2->highlights = 0.0f;
    return 0;
  }
  return 1;
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_monochrome_params_t p;

  p.size = 2.3f;

  p.a = 32.0f;
  p.b = 64.0f;
  p.highlights = 0.0f;
  dt_gui_presets_add_generic(_("red filter"), self->op, self->version(), &p, sizeof(p), 1);

  // p.a = 64.0f;
  // p.b = -32.0f;
  // dt_gui_presets_add_generic(_("purple filter"), self->op, self->version(), &p, sizeof(p), 1);

  // p.a = -32.0f;
  // p.b = -64.0f;
  // dt_gui_presets_add_generic(_("blue filter"), self->op, self->version(), &p, sizeof(p), 1);

  // p.a = -64.0f;
  // p.b = 32.0f;
  // dt_gui_presets_add_generic(_("green filter"), self->op, self->version(), &p, sizeof(p), 1);
}

static float color_filter(const float ai, const float bi, const float a, const float b, const float size)
{
  return dt_fast_expf(-CLAMPS(((ai - a) * (ai - a) + (bi - b) * (bi - b)) / (2.0 * size), 0.0f, 1.0f));
}

static float envelope(const float L)
{
  const float x = CLAMPS(L / 100.0f, 0.0f, 1.0f);
  // const float alpha = 2.0f;
  const float beta = 0.6f;
  if(x < beta)
  {
    // return 1.0f-fabsf(x/beta-1.0f)^2
    const float tmp = fabsf(x / beta - 1.0f);
    return 1.0f - tmp * tmp;
  }
  else
  {
    const float tmp1 = (1.0f - x) / (1.0f - beta);
    const float tmp2 = tmp1 * tmp1;
    const float tmp3 = tmp2 * tmp1;
    return 3.0f * tmp2 - 2.0f * tmp3;
  }
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const i, void *const o,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_monochrome_data_t *d = (dt_iop_monochrome_data_t *)piece->data;
  const float sigma2 = (d->size * 128.0) * (d->size * 128.0f);
// first pass: evaluate color filter:
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(d) schedule(static)
#endif
  for(int k = 0; k < roi_out->height; k++)
  {
    const float *in = ((float *)i) + (size_t)4 * k * roi_out->width;
    float *out = ((float *)o) + (size_t)4 * k * roi_out->width;
    for(int j = 0; j < roi_out->width; j++, in += 4, out += 4)
    {
      out[0] = 100.0f * color_filter(in[1], in[2], d->a, d->b, sigma2);
      out[1] = out[2] = 0.0f;
      out[3] = in[3];
    }
  }

  // second step: blur filter contribution:
  const float scale = piece->iscale / roi_in->scale;
  const float sigma_r = 250.0f; // does not depend on scale
  const float sigma_s = 20.0f / scale;
  const float detail = -1.0f; // bilateral base layer

  dt_bilateral_t *b = dt_bilateral_init(roi_in->width, roi_in->height, sigma_s, sigma_r);
  dt_bilateral_splat(b, (float *)o);
  dt_bilateral_blur(b);
  dt_bilateral_slice(b, (float *)o, (float *)o, detail);
  dt_bilateral_free(b);

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(d) schedule(static)
#endif
  for(int k = 0; k < roi_out->height; k++)
  {
    const float *in = ((float *)i) + (size_t)4 * k * roi_out->width;
    float *out = ((float *)o) + (size_t)4 * k * roi_out->width;
    for(int j = 0; j < roi_out->width; j++, in += 4, out += 4)
    {
      const float tt = envelope(in[0]);
      const float t = tt + (1.0f - tt) * (1.0f - d->highlights);
      out[0] = (1.0f - t) * in[0]
               + t * out[0] * (1.0f / 100.0f) * in[0]; // normalized filter * input brightness
    }
  }
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_monochrome_data_t *d = (dt_iop_monochrome_data_t *)piece->data;
  dt_iop_monochrome_global_data_t *gd = (dt_iop_monochrome_global_data_t *)self->data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;

  const int width = roi_out->width;
  const int height = roi_out->height;
  const float sigma2 = (d->size * 128.0) * (d->size * 128.0f);

  // TODO: alloc new buffer, bilat filter, and go on with that
  const float scale = piece->iscale / roi_in->scale;
  const float sigma_r = 250.0f; // does not depend on scale
  const float sigma_s = 20.0f / scale;
  const float detail = -1.0f; // bilateral base layer

  cl_mem dev_tmp = NULL;
  dev_tmp = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, 4 * sizeof(float));

  dt_bilateral_cl_t *b = dt_bilateral_init_cl(devid, roi_in->width, roi_in->height, sigma_s, sigma_r);
  if(!b) goto error;

  size_t sizes[2] = { ROUNDUPWD(width), ROUNDUPHT(height) };
  dt_opencl_set_kernel_arg(devid, gd->kernel_monochrome_filter, 0, sizeof(cl_mem), &dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_monochrome_filter, 1, sizeof(cl_mem), &dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_monochrome_filter, 2, sizeof(int), &width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_monochrome_filter, 3, sizeof(int), &height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_monochrome_filter, 4, sizeof(float), &d->a);
  dt_opencl_set_kernel_arg(devid, gd->kernel_monochrome_filter, 5, sizeof(float), &d->b);
  dt_opencl_set_kernel_arg(devid, gd->kernel_monochrome_filter, 6, sizeof(float), &sigma2);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_monochrome_filter, sizes);
  if(err != CL_SUCCESS) goto error;

  err = dt_bilateral_splat_cl(b, dev_out);
  if(err != CL_SUCCESS) goto error;
  err = dt_bilateral_blur_cl(b);
  if(err != CL_SUCCESS) goto error;
  err = dt_bilateral_slice_cl(b, dev_out, dev_tmp, detail);
  if(err != CL_SUCCESS) goto error;
  dt_bilateral_free_cl(b);
  b = NULL; // make sure we don't do double cleanup in case the next few lines err out

  dt_opencl_set_kernel_arg(devid, gd->kernel_monochrome, 0, sizeof(cl_mem), &dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_monochrome, 1, sizeof(cl_mem), &dev_tmp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_monochrome, 2, sizeof(cl_mem), &dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_monochrome, 3, sizeof(int), &width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_monochrome, 4, sizeof(int), &height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_monochrome, 5, sizeof(float), &d->a);
  dt_opencl_set_kernel_arg(devid, gd->kernel_monochrome, 6, sizeof(float), &d->b);
  dt_opencl_set_kernel_arg(devid, gd->kernel_monochrome, 7, sizeof(float), &sigma2);
  dt_opencl_set_kernel_arg(devid, gd->kernel_monochrome, 8, sizeof(float), &d->highlights);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_monochrome, sizes);
  if(err != CL_SUCCESS) goto error;

  if(dev_tmp != NULL) dt_opencl_release_mem_object(dev_tmp);
  return TRUE;

error:
  if(dev_tmp != NULL) dt_opencl_release_mem_object(dev_tmp);
  dt_bilateral_free_cl(b);
  dt_print(DT_DEBUG_OPENCL, "[opencl_monochrome] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  const float scale = piece->iscale / roi_in->scale;
  const float sigma_s = 20.0f / scale;
  const float sigma_r = 250.0f;

  const int width = roi_in->width;
  const int height = roi_in->height;
  const int channels = piece->colors;

  const size_t basebuffer = width * height * channels * sizeof(float);

  tiling->factor = 3.0f + (float)dt_bilateral_memory_use(width, height, sigma_s, sigma_r) / basebuffer;
  tiling->maxbuf
      = fmax(1.0f, (float)dt_bilateral_singlebuffer_size(width, height, sigma_s, sigma_r) / basebuffer);
  tiling->overhead = 0;
  tiling->overlap = ceilf(4 * sigma_s);
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_monochrome_params_t *p = (dt_iop_monochrome_params_t *)p1;
  dt_iop_monochrome_data_t *d = (dt_iop_monochrome_data_t *)piece->data;
  d->a = p->a;
  d->b = p->b;
  d->size = p->size;
  d->highlights = p->highlights;

#ifdef HAVE_OPENCL
  piece->process_cl_ready = (piece->process_cl_ready && !(darktable.opencl->avoid_atomics));
#endif
}



void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl from programs.conf
  dt_iop_monochrome_global_data_t *gd
      = (dt_iop_monochrome_global_data_t *)malloc(sizeof(dt_iop_monochrome_global_data_t));
  module->data = gd;
  gd->kernel_monochrome_filter = dt_opencl_create_kernel(program, "monochrome_filter");
  gd->kernel_monochrome = dt_opencl_create_kernel(program, "monochrome");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_monochrome_global_data_t *gd = (dt_iop_monochrome_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_monochrome_filter);
  dt_opencl_free_kernel(gd->kernel_monochrome);
  free(module->data);
  module->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_monochrome_gui_data_t *g = (dt_iop_monochrome_gui_data_t *)self->gui_data;
  dt_iop_monochrome_params_t *p = (dt_iop_monochrome_params_t *)self->params;
  dt_bauhaus_slider_set(g->highlights, p->highlights);
  gtk_widget_queue_draw(self->widget);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_monochrome_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_monochrome_params_t));
  module->default_enabled = 0;
  module->priority = 630; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_monochrome_params_t);
  module->gui_data = NULL;
  dt_iop_monochrome_params_t tmp = (dt_iop_monochrome_params_t){ 0., 0., 2., 0. };
  memcpy(module->params, &tmp, sizeof(dt_iop_monochrome_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_monochrome_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_monochrome_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

static gboolean dt_iop_monochrome_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_monochrome_gui_data_t *g = (dt_iop_monochrome_gui_data_t *)self->gui_data;
  dt_iop_monochrome_params_t *p = (dt_iop_monochrome_params_t *)self->params;

  if(self->request_color_pick != DT_REQUEST_COLORPICK_OFF)
  {
    float old_a = p->a, old_b = p->b, old_size = p->size;
    p->a = self->picked_color[1];
    p->b = self->picked_color[2];
    float da = self->picked_color_max[1] - self->picked_color_min[1];
    float db = self->picked_color_max[2] - self->picked_color_min[2];
    p->size = CLAMP((da + db)/128.0, .5, 3.0);
    if(old_a != p->a || old_b != p->b || old_size != p->size)
      dt_dev_add_history_item(darktable.develop, self, TRUE);
  }

  const int inset = DT_COLORCORRECTION_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  // clear bg
  cairo_set_source_rgb(cr, .2, .2, .2);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
  width -= 2 * inset;
  height -= 2 * inset;
  // clip region to inside:
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_clip(cr);
  // flip y:
  cairo_translate(cr, 0, height);
  cairo_scale(cr, 1., -1.);
  const int cells = 8;
  for(int j = 0; j < cells; j++)
    for(int i = 0; i < cells; i++)
    {
      double rgb[3] = { 0.5, 0.5, 0.5 };
      cmsCIELab Lab;
      Lab.L = 53.390011;
      Lab.a = Lab.b = 0; // grey
      // dt_iop_sRGB_to_Lab(rgb, Lab, 0, 0, 1.0, 1, 1); // get grey in Lab
      Lab.a = PANEL_WIDTH * (i / (cells - 1.0) - .5);
      Lab.b = PANEL_WIDTH * (j / (cells - 1.0) - .5);
      const float f = color_filter(Lab.a, Lab.b, p->a, p->b, 40 * 40 * p->size * p->size);
      Lab.L *= f * f; // exaggerate filter a little
      cmsDoTransform(g->xform, &Lab, rgb, 1);
      cairo_set_source_rgb(cr, rgb[0], rgb[1], rgb[2]);
      cairo_rectangle(cr, width * i / (float)cells, height * j / (float)cells,
                      width / (float)cells - DT_PIXEL_APPLY_DPI(1),
                      height / (float)cells - DT_PIXEL_APPLY_DPI(1));
      cairo_fill(cr);
    }
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
  cairo_set_source_rgb(cr, .7, .7, .7);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.0));
  const float x = p->a * width / PANEL_WIDTH + width * .5f, y = p->b * height / PANEL_WIDTH + height * .5f;
  cairo_arc(cr, x, y, width * .22f * p->size, 0, 2.0 * M_PI);
  cairo_stroke(cr);

  if(g->dragging) dt_dev_add_history_item(darktable.develop, self, TRUE);

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

static gboolean dt_iop_monochrome_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_monochrome_gui_data_t *g = (dt_iop_monochrome_gui_data_t *)self->gui_data;
  dt_iop_monochrome_params_t *p = (dt_iop_monochrome_params_t *)self->params;
  if(g->dragging)
  {
    const int inset = DT_COLORCORRECTION_INSET;
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    int width = allocation.width - 2 * inset, height = allocation.height - 2 * inset;
    const float mouse_x = CLAMP(event->x - inset, 0, width);
    const float mouse_y = CLAMP(height - 1 - event->y + inset, 0, height);
    p->a = PANEL_WIDTH * (mouse_x - width * 0.5f) / (float)width;
    p->b = PANEL_WIDTH * (mouse_y - height * 0.5f) / (float)height;
    gtk_widget_queue_draw(self->widget);
  }
  gint x, y;
#if GTK_CHECK_VERSION(3, 20, 0)
  gdk_window_get_device_position(event->window,
      gdk_seat_get_pointer(gdk_display_get_default_seat(gtk_widget_get_display(widget))),
      &x, &y, 0);
#else
  gdk_window_get_device_position(event->window,
                                 gdk_device_manager_get_client_pointer(
                                     gdk_display_get_device_manager(gdk_window_get_display(event->window))),
                                 &x, &y, NULL);
#endif
  return TRUE;
}

static gboolean dt_iop_monochrome_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(event->button == 1)
  {
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    dt_iop_monochrome_gui_data_t *g = (dt_iop_monochrome_gui_data_t *)self->gui_data;
    dt_iop_monochrome_params_t *p = (dt_iop_monochrome_params_t *)self->params;
    self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
    if(event->type == GDK_2BUTTON_PRESS)
    {
      // reset
      dt_iop_monochrome_params_t *p0 = (dt_iop_monochrome_params_t *)self->default_params;
      p->a = p0->a;
      p->b = p0->b;
      p->size = p0->size;
    }
    else
    {
      const int inset = DT_COLORCORRECTION_INSET;
      GtkAllocation allocation;
      gtk_widget_get_allocation(widget, &allocation);
      int width = allocation.width - 2 * inset, height = allocation.height - 2 * inset;
      const float mouse_x = CLAMP(event->x - inset, 0, width);
      const float mouse_y = CLAMP(height - 1 - event->y + inset, 0, height);
      p->a = PANEL_WIDTH * (mouse_x - width * 0.5f) / (float)width;
      p->b = PANEL_WIDTH * (mouse_y - height * 0.5f) / (float)height;
      g->dragging = 1;
      g_object_set(G_OBJECT(widget), "has-tooltip", FALSE, (char *)NULL);
    }
    gtk_widget_queue_draw(self->widget);
    return TRUE;
  }
  return FALSE;
}

static gboolean dt_iop_monochrome_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(event->button == 1)
  {
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    dt_iop_monochrome_gui_data_t *g = (dt_iop_monochrome_gui_data_t *)self->gui_data;
    self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
    g->dragging = 0;
    g_object_set(G_OBJECT(widget), "has-tooltip", TRUE, (char *)NULL);
    return TRUE;
  }
  return FALSE;
}

static gboolean dt_iop_monochrome_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_monochrome_gui_data_t *g = (dt_iop_monochrome_gui_data_t *)self->gui_data;
  g->dragging = 0;
  gtk_widget_queue_draw(self->widget);
  return TRUE;
}

static gboolean dt_iop_monochrome_scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_monochrome_params_t *p = (dt_iop_monochrome_params_t *)self->params;
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  if(event->direction == GDK_SCROLL_UP && p->size > .5) p->size -= 0.1;
  if(event->direction == GDK_SCROLL_DOWN && p->size < 3.0) p->size += 0.1;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static void highlights_callback(GtkWidget *w, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_monochrome_params_t *p = (dt_iop_monochrome_params_t *)self->params;
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  p->highlights = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void picker_callback(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;

  if(self->request_color_pick == DT_REQUEST_COLORPICK_OFF)
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

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_monochrome_gui_data_t));
  dt_iop_monochrome_gui_data_t *g = (dt_iop_monochrome_gui_data_t *)self->gui_data;

  g->dragging = 0;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  g->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(1.0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->area), TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->area), _("drag and scroll mouse wheel to adjust the virtual color filter"));

  gtk_widget_add_events(GTK_WIDGET(g->area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                             | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                             | GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK);
  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(dt_iop_monochrome_draw), self);
  g_signal_connect(G_OBJECT(g->area), "button-press-event", G_CALLBACK(dt_iop_monochrome_button_press), self);
  g_signal_connect(G_OBJECT(g->area), "button-release-event", G_CALLBACK(dt_iop_monochrome_button_release),
                   self);
  g_signal_connect(G_OBJECT(g->area), "motion-notify-event", G_CALLBACK(dt_iop_monochrome_motion_notify),
                   self);
  g_signal_connect(G_OBJECT(g->area), "leave-notify-event", G_CALLBACK(dt_iop_monochrome_leave_notify), self);
  g_signal_connect(G_OBJECT(g->area), "scroll-event", G_CALLBACK(dt_iop_monochrome_scrolled), self);

  g->highlights = dt_bauhaus_slider_new_with_range(self, 0.0, 1.0, 0.01, 0.0, 2);
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  dt_bauhaus_widget_set_quad_paint(g->highlights, dtgtk_cairo_paint_colorpicker, CPF_ACTIVE);
  gtk_widget_set_tooltip_text(g->highlights, _("how much to keep highlights"));
  dt_bauhaus_widget_set_label(g->highlights, NULL, _("highlights"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->highlights, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->highlights), "value-changed", G_CALLBACK(highlights_callback), self);
  g_signal_connect(G_OBJECT(g->highlights), "quad-pressed", G_CALLBACK(picker_callback), self);

  cmsHPROFILE hsRGB = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "", DT_PROFILE_DIRECTION_IN)->profile;
  cmsHPROFILE hLab = dt_colorspaces_get_profile(DT_COLORSPACE_LAB, "", DT_PROFILE_DIRECTION_ANY)->profile;
  g->xform = cmsCreateTransform(hLab, TYPE_Lab_DBL, hsRGB, TYPE_RGB_DBL, INTENT_PERCEPTUAL,
                                0); // cmsFLAGS_NOTPRECALC);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_monochrome_gui_data_t *g = (dt_iop_monochrome_gui_data_t *)self->gui_data;
  cmsDeleteTransform(g->xform);
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
