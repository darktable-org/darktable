/*
    This file is part of darktable,
    Copyright (C) 2009-2024 darktable developers.

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
#include "common/math.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "dtgtk/drawingarea.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "gui/accelerators.h"
#include "iop/iop_api.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE_INTROSPECTION(2, dt_iop_monochrome_params_t)

#define DT_COLORCORRECTION_INSET DT_PIXEL_APPLY_DPI(5)
#define DT_COLORCORRECTION_MAX 40.
#define PANEL_WIDTH 256.0f

typedef struct dt_iop_monochrome_params_t
{
  float a; // $DEFAULT: 0.0
  float b; // $DEFAULT: 0.0
  float size; // $DEFAULT: 2.0
  float highlights; // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0
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

int default_group()
{
  return IOP_GROUP_COLOR | IOP_GROUP_EFFECTS;
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("quickly convert an image to black & white using a variable color filter"),
                                      _("creative"),
                                      _("linear or non-linear, Lab, display-referred"),
                                      _("non-linear, Lab"),
                                      _("non-linear, Lab, display-referred"));
}

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  typedef struct dt_iop_monochrome_params_v2_t
  {
    float a;
    float b;
    float size;
    float highlights;
  } dt_iop_monochrome_params_v2_t;

  if(old_version == 1)
  {
    typedef struct dt_iop_monochrome_params_v1_t
    {
      float a;
      float b;
      float size;
    } dt_iop_monochrome_params_v1_t;

    const dt_iop_monochrome_params_v1_t *o = (dt_iop_monochrome_params_v1_t *)old_params;
    dt_iop_monochrome_params_v2_t *n = malloc(sizeof(dt_iop_monochrome_params_v2_t));
    memcpy(n, o, sizeof(dt_iop_monochrome_params_v1_t));
    n->highlights = 0.0f;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_monochrome_params_v2_t);
    *new_version = 2;
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
  dt_gui_presets_add_generic(_("red filter"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

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

static float _color_filter(const float ai,
                           const float bi,
                           const float a,
                           const float b,
                           const float dbl_size)
{
  return dt_fast_expf(-CLAMPS(((ai - a) * (ai - a) + (bi - b) * (bi - b)) / dbl_size, 0.0f, 1.0f));
}

static float _envelope(const float L)
{
  const float x = CLAMPS(L / 100.0f, 0.0f, 1.0f);
  // const float alpha = 2.0f;
  const float beta = 0.6f;
  if(x < beta)
  {
    // return 1.0f-fabsf(x/beta-1.0f)^2
    const float tmp = (x / beta - 1.0f); // no need for fabsf since we square the value
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

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const i,
             void *const o,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  dt_iop_monochrome_data_t *d = piece->data;
  const float sigma2 = 2.0f * (d->size * 128.0f) * (d->size * 128.0f);
// first pass: evaluate color filter:
  const size_t npixels = (size_t)roi_out->height * roi_out->width;
  const float *const restrict in = (const float *)i;
  float *const restrict out = (float *)o;
  const float d_a = d->a;
  const float d_b = d->b;
  DT_OMP_FOR_SIMD(aligned(in, out:64))
  for(int k = 0; k < 4*npixels; k += 4)
  {
    out[k+0] = 100.0f * _color_filter(in[k+1], in[k+2], d_a, d_b, sigma2);
    out[k+1] = out[k+2] = 0.0f;
  }

  // second step: blur filter contribution:
  const float scale = fmaxf(piece->iscale / roi_in->scale, 1.f);
  const float sigma_r = 250.0f; // does not depend on scale
  const float sigma_s = 20.0f / scale;
  const float detail = -1.0f; // bilateral base layer

  dt_bilateral_t *b = dt_bilateral_init(roi_in->width, roi_in->height, sigma_s, sigma_r);
  dt_bilateral_splat(b, (float *)o);
  dt_bilateral_blur(b);
  dt_bilateral_slice(b, (float *)o, (float *)o, detail);
  dt_bilateral_free(b);

  const float highlights = d->highlights;
  DT_OMP_FOR_SIMD(aligned(in, out:64))
  for(int k = 0; k < 4*npixels; k += 4)
  {
    const float tt = _envelope(in[k]);
    const float t = tt + (1.0f - tt) * (1.0f - highlights);
    out[k] = (1.0f - t) * in[k] + t * out[k] * (1.0f / 100.0f) * in[k]; // normalized filter * input brightness
  }
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  dt_iop_monochrome_data_t *d = piece->data;
  dt_iop_monochrome_global_data_t *gd = self->global_data;

  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
  const int devid = piece->pipe->devid;

  const int width = roi_out->width;
  const int height = roi_out->height;
  const float sigma2 = (d->size * 128.0) * (d->size * 128.0f);

  // TODO: alloc new buffer, bilat filter, and go on with that
  const float scale = piece->iscale / roi_in->scale;
  const float sigma_r = 250.0f; // does not depend on scale
  const float sigma_s = 20.0f / scale;
  const float detail = -1.0f; // bilateral base layer

  cl_mem dev_tmp = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float) * 4);

  dt_bilateral_cl_t *b = dt_bilateral_init_cl(devid, roi_in->width, roi_in->height, sigma_s, sigma_r);
  if(!b || dev_tmp == NULL) goto error;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_monochrome_filter, width, height,
    CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(d->a), CLARG(d->b), CLARG(sigma2));
  if(err != CL_SUCCESS) goto error;

  err = dt_bilateral_splat_cl(b, dev_out);
  if(err != CL_SUCCESS) goto error;
  err = dt_bilateral_blur_cl(b);
  if(err != CL_SUCCESS) goto error;
  err = dt_bilateral_slice_cl(b, dev_out, dev_tmp, detail);
  if(err != CL_SUCCESS) goto error;
  dt_bilateral_free_cl(b);
  b = NULL; // make sure we don't do double cleanup in case the next few lines err out

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_monochrome, width, height,
    CLARG(dev_in), CLARG(dev_tmp), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(d->a), CLARG(d->b),
    CLARG(sigma2), CLARG(d->highlights));

error:
  dt_opencl_release_mem_object(dev_tmp);
  if(b) dt_bilateral_free_cl(b);
  return err;
}
#endif

void tiling_callback(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     dt_develop_tiling_t *tiling)
{
  const float scale = piece->iscale / roi_in->scale;
  const float sigma_s = 20.0f / scale;
  const float sigma_r = 250.0f;

  const int width = roi_in->width;
  const int height = roi_in->height;
  const int channels = piece->colors;

  const size_t basebuffer = sizeof(float) * channels * width * height;
  const size_t bilat_mem = dt_bilateral_memory_use(width, height, sigma_s, sigma_r);

  tiling->factor = 2.0f + (float)bilat_mem / basebuffer;
  tiling->factor_cl = 3.0f + (float)bilat_mem / basebuffer;
  tiling->maxbuf
      = fmax(1.0f, (float)dt_bilateral_singlebuffer_size(width, height, sigma_s, sigma_r) / basebuffer);
  tiling->maxbuf_cl = tiling->maxbuf;
  tiling->overhead = 0;
  tiling->overlap = ceilf(4 * sigma_s);
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_monochrome_params_t *p = (dt_iop_monochrome_params_t *)p1;
  dt_iop_monochrome_data_t *d = piece->data;
  d->a = p->a;
  d->b = p->b;
  d->size = p->size;
  d->highlights = p->highlights;

#ifdef HAVE_OPENCL
  piece->process_cl_ready = (piece->process_cl_ready && !dt_opencl_avoid_atomics(pipe->devid));
#endif
}



void init_global(dt_iop_module_so_t *self)
{
  const int program = 2; // basic.cl from programs.conf
  dt_iop_monochrome_global_data_t *gd = malloc(sizeof(dt_iop_monochrome_global_data_t));
  self->data = gd;
  gd->kernel_monochrome_filter = dt_opencl_create_kernel(program, "monochrome_filter");
  gd->kernel_monochrome = dt_opencl_create_kernel(program, "monochrome");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_monochrome_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_monochrome_filter);
  dt_opencl_free_kernel(gd->kernel_monochrome);
  free(self->data);
  self->data = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_monochrome_gui_data_t *g = self->gui_data;
  g->dragging = FALSE;
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_monochrome_data_t));
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

static gboolean _monochrome_draw(GtkWidget *widget, cairo_t *crf, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return FALSE;
  dt_iop_monochrome_gui_data_t *g = self->gui_data;
  dt_iop_monochrome_params_t *p = self->params;

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
      const float f = _color_filter(Lab.a, Lab.b, p->a, p->b, 40 * 40 * p->size * p->size);
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

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_t *pipe)
{
  dt_iop_monochrome_params_t *p = self->params;

  if(fabsf(p->a - self->picked_color[1]) < 0.0001f
     && fabsf(p->b - self->picked_color[2]) < 0.0001f)
  {
    // interrupt infinite loops
    return;
  }

  p->a = self->picked_color[1];
  p->b = self->picked_color[2];
  float da = self->picked_color_max[1] - self->picked_color_min[1];
  float db = self->picked_color_max[2] - self->picked_color_min[2];
  p->size = CLAMP((da + db)/128.0, .5, 3.0);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  dt_control_queue_redraw_widget(self->widget);
}

static gboolean _monochrome_motion_notify(GtkWidget *widget, GdkEventMotion *event, dt_iop_module_t *self)
{
  dt_iop_monochrome_gui_data_t *g = self->gui_data;
  dt_iop_monochrome_params_t *p = self->params;
  if(g->dragging)
  {
    const float old_a = p->a, old_b = p->b;
    const int inset = DT_COLORCORRECTION_INSET;
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    int width = allocation.width - 2 * inset, height = allocation.height - 2 * inset;
    const float mouse_x = CLAMP(event->x - inset, 0, width);
    const float mouse_y = CLAMP(height - 1 - event->y + inset, 0, height);
    p->a = PANEL_WIDTH * (mouse_x - width * 0.5f) / (float)width;
    p->b = PANEL_WIDTH * (mouse_y - height * 0.5f) / (float)height;

    if(old_a != p->a || old_b != p->b) dt_dev_add_history_item(darktable.develop, self, TRUE);
    gtk_widget_queue_draw(GTK_WIDGET(g->area));
  }
  return TRUE;
}

static gboolean _monochrome_button_press(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  if(event->button == 1)
  {
    dt_iop_monochrome_gui_data_t *g = self->gui_data;
    dt_iop_monochrome_params_t *p = self->params;
    dt_iop_color_picker_reset(self, TRUE);
    if(event->type == GDK_2BUTTON_PRESS)
    {
      // reset
      const dt_iop_monochrome_params_t *const p0 = self->default_params;
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
      g_object_set(G_OBJECT(widget), "has-tooltip", FALSE, (gchar *)0);
    }
    gtk_widget_queue_draw(GTK_WIDGET(g->area));
    return TRUE;
  }
  return FALSE;
}

static gboolean _monochrome_button_release(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  if(event->button == 1)
  {
    dt_iop_monochrome_gui_data_t *g = self->gui_data;
    dt_iop_color_picker_reset(self, TRUE);
    g->dragging = 0;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    g_object_set(G_OBJECT(widget), "has-tooltip", TRUE, (gchar *)0);
    return TRUE;
  }
  return FALSE;
}

static gboolean _monochrome_leave_notify(GtkWidget *widget, GdkEventCrossing *event, dt_iop_module_t *self)
{
  dt_iop_monochrome_gui_data_t *g = self->gui_data;
  g->dragging = 0;
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
  return TRUE;
}

static gboolean _monochrome_scrolled(GtkWidget *widget, GdkEventScroll *event, dt_iop_module_t *self)
{
  dt_iop_monochrome_params_t *p = self->params;

  if(dt_gui_ignore_scroll(event)) return FALSE;

  dt_iop_color_picker_reset(self, TRUE);

  int delta_y;
  if(dt_gui_get_scroll_unit_delta(event, &delta_y))
  {
    const float old_size = p->size;
    p->size = CLAMP(p->size + delta_y * 0.1, 0.5f, 3.0f);
    if(old_size != p->size) dt_dev_add_history_item(darktable.develop, self, TRUE);
    gtk_widget_queue_draw(widget);
  }

  return TRUE;
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_monochrome_gui_data_t *g = IOP_GUI_ALLOC(monochrome);

  g->dragging = 0;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_height(0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->area), TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->area), _("drag and scroll mouse wheel to adjust the virtual color filter"));
  dt_action_define_iop(self, NULL, N_("grid"), GTK_WIDGET(g->area), NULL);

  gtk_widget_add_events(GTK_WIDGET(g->area), GDK_POINTER_MOTION_MASK
                                             | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                             | GDK_LEAVE_NOTIFY_MASK | darktable.gui->scroll_mask);
  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(_monochrome_draw), self);
  g_signal_connect(G_OBJECT(g->area), "button-press-event", G_CALLBACK(_monochrome_button_press), self);
  g_signal_connect(G_OBJECT(g->area), "button-release-event", G_CALLBACK(_monochrome_button_release),
                   self);
  g_signal_connect(G_OBJECT(g->area), "motion-notify-event", G_CALLBACK(_monochrome_motion_notify),
                   self);
  g_signal_connect(G_OBJECT(g->area), "leave-notify-event", G_CALLBACK(_monochrome_leave_notify), self);
  g_signal_connect(G_OBJECT(g->area), "scroll-event", G_CALLBACK(_monochrome_scrolled), self);

  g->highlights
      = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, N_("highlights")));
  gtk_widget_set_tooltip_text(g->highlights, _("how much to keep highlights"));

  cmsHPROFILE hsRGB = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "", DT_PROFILE_DIRECTION_IN)->profile;
  cmsHPROFILE hLab = dt_colorspaces_get_profile(DT_COLORSPACE_LAB, "", DT_PROFILE_DIRECTION_ANY)->profile;
  g->xform = cmsCreateTransform(hLab, TYPE_Lab_DBL, hsRGB, TYPE_RGB_DBL, INTENT_PERCEPTUAL,
                                0); // cmsFLAGS_NOTPRECALC);
}

void gui_cleanup(dt_iop_module_t *self)
{
  dt_iop_monochrome_gui_data_t *g = self->gui_data;
  cmsDeleteTransform(g->xform);

  IOP_GUI_FREE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
