/*
    This file is part of darktable,
    Copyright (C) 2010-2021 darktable developers.

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
#include <stdlib.h>
#include <string.h>

#include "common/darktable.h"
#include "common/gaussian.h"
#include "common/math.h"
#include "common/opencl.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "dtgtk/drawingarea.h"
#include "dtgtk/gradientslider.h"
#include "dtgtk/togglebutton.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

#include <librsvg/rsvg.h>
// ugh, ugly hack. why do people break stuff all the time?
#ifndef RSVG_CAIRO_H
#include <librsvg/rsvg-cairo.h>
#endif


DT_MODULE_INTROSPECTION(1, dt_iop_zonesystem_params_t)
#define MAX_ZONE_SYSTEM_SIZE 24

/** gui params. */
typedef struct dt_iop_zonesystem_params_t
{
  int size; // $DEFAULT: 10
  float zone[MAX_ZONE_SYSTEM_SIZE + 1]; // $DEFAULT: -1.0
} dt_iop_zonesystem_params_t;

/** and pixelpipe data is just the same */
typedef struct dt_iop_zonesystem_data_t
{
  dt_iop_zonesystem_params_t params;
  float rzscale;
  float zonemap_offset[MAX_ZONE_SYSTEM_SIZE];
  float zonemap_scale[MAX_ZONE_SYSTEM_SIZE];
} dt_iop_zonesystem_data_t;


/*
void init_presets (dt_iop_module_so_t *self)
{
//   DT_DEBUG_SQLITE3_EXEC(darktable.db, "begin", NULL, NULL, NULL);

  dt_gui_presets_add_generic(_("Fill-light 0.25EV with 4 zones"), self->op, self->version(),
&(dt_iop_zonesystem_params_t){0.25,0.25,4.0} , sizeof(dt_iop_zonesystem_params_t), 1);
  dt_gui_presets_add_generic(_("Fill-shadow -0.25EV with 4 zones"), self->op, self->version(),
&(dt_iop_zonesystem_params_t){-0.25,0.25,4.0} , sizeof(dt_iop_zonesystem_params_t), 1);

//   DT_DEBUG_SQLITE3_EXEC(darktable.db, "commit", NULL, NULL, NULL);
}
*/

typedef struct dt_iop_zonesystem_global_data_t
{
  int kernel_zonesystem;
} dt_iop_zonesystem_global_data_t;


typedef struct dt_iop_zonesystem_gui_data_t
{
  guchar *in_preview_buffer;
  guchar *out_preview_buffer;
  int preview_width, preview_height;
  GtkWidget *preview;
  GtkWidget *zones;
  float press_x, press_y, mouse_x, mouse_y;
  gboolean hilite_zone;
  gboolean is_dragging;
  int current_zone;
  int zone_under_mouse;
  int mouse_over_output_zones;

  cairo_surface_t *image;
  guint8 *image_buffer;
  int image_width, image_height;

} dt_iop_zonesystem_gui_data_t;


const char *name()
{
  return _("zone system");
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_ALLOW_TILING
         | IOP_FLAGS_PREVIEW_NON_OPENCL | IOP_FLAGS_DEPRECATED;
}

const char *deprecated_msg()
{
  return _("this module is deprecated. please use the tone equalizer module instead.");
}

int default_group()
{
  return IOP_GROUP_TONE | IOP_GROUP_GRADING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

/* get the zone index of pixel lightness from zonemap */
static inline int _iop_zonesystem_zone_index_from_lightness(float lightness, float *zonemap, int size)
{
  for(int k = 0; k < size - 1; k++)
    if(zonemap[k + 1] >= lightness) return k;
  return size - 1;
}

/* calculate a zonemap with scale values for each zone based on controlpoints from param */
static inline void _iop_zonesystem_calculate_zonemap(struct dt_iop_zonesystem_params_t *p, float *zonemap)
{
  int steps = 0;
  int pk = 0;

  for(int k = 0; k < p->size; k++)
  {
    if((k > 0 && k < p->size - 1) && p->zone[k] == -1)
      steps++;
    else
    {
      /* set 0 and 1.0 for first and last element in zonesystem size, or the actually parameter value */
      zonemap[k] = k == 0 ? 0.0 : k == (p->size - 1) ? 1.0 : p->zone[k];

      /* for each step from pk to k, calculate values
          for now this is linear distributed
      */
      for(int l = 1; l <= steps; l++)
        zonemap[pk + l] = zonemap[pk] + (((zonemap[k] - zonemap[pk]) / (steps + 1)) * l);

      /* store k into pk and reset zone steps for next range*/
      pk = k;
      steps = 0;
    }
  }
}

static void process_common_setup(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                 const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                                 const dt_iop_roi_t *const roi_out)
{
  const int width = roi_out->width;
  const int height = roi_out->height;

  if(self->dev->gui_attached && (piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW) == DT_DEV_PIXELPIPE_PREVIEW)
  {
    dt_iop_zonesystem_gui_data_t *g = (dt_iop_zonesystem_gui_data_t *)self->gui_data;
    dt_iop_gui_enter_critical_section(self);
    if(g->in_preview_buffer == NULL || g->out_preview_buffer == NULL || g->preview_width != width
       || g->preview_height != height)
    {
      g_free(g->in_preview_buffer);
      g_free(g->out_preview_buffer);
      g->in_preview_buffer = g_malloc_n((size_t)width * height, sizeof(guchar));
      g->out_preview_buffer = g_malloc_n((size_t)width * height, sizeof(guchar));
      g->preview_width = width;
      g->preview_height = height;
    }
    dt_iop_gui_leave_critical_section(self);
  }
}

static void process_common_cleanup(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                   const void *const ivoid, void *const ovoid,
                                   const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_zonesystem_data_t *d = (dt_iop_zonesystem_data_t *)piece->data;
  dt_iop_zonesystem_gui_data_t *g = (dt_iop_zonesystem_gui_data_t *)self->gui_data;

  const int width = roi_out->width;
  const int height = roi_out->height;
  const size_t ch = piece->colors;
  const int size = d->params.size;

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, width, height);

  /* if gui and have buffer lets gaussblur and fill buffer with zone indexes */
  if(self->dev->gui_attached
     && (piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW) == DT_DEV_PIXELPIPE_PREVIEW
     && g && g->in_preview_buffer
     && g->out_preview_buffer)
  {
    float Lmax[] = { 100.0f };
    float Lmin[] = { 0.0f };

    /* setup gaussian kernel */
    const int radius = 8;
    const float sigma = 2.5 * (radius * roi_in->scale / piece->iscale);

    dt_gaussian_t *gauss = dt_gaussian_init(width, height, 1, Lmax, Lmin, sigma, DT_IOP_GAUSSIAN_ZERO);

    float *tmp = g_malloc_n((size_t)width * height, sizeof(float));

    if(gauss && tmp)
    {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(ch, height, width, ivoid) \
      shared(tmp) \
      schedule(static)
#endif
      for(size_t k = 0; k < (size_t)width * height; k++) tmp[k] = ((float *)ivoid)[ch * k];

      dt_gaussian_blur(gauss, tmp, tmp);

      /* create zonemap preview for input */
      dt_iop_gui_enter_critical_section(self);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(height, size, width) \
      shared(tmp, g) \
      schedule(static)
#endif
      for(size_t k = 0; k < (size_t)width * height; k++)
      {
        g->in_preview_buffer[k] = CLAMPS(tmp[k] * (size - 1) / 100.0f, 0, size - 2);
      }
      dt_iop_gui_leave_critical_section(self);


#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(ch, height, ovoid, width) \
      shared(tmp) \
      schedule(static)
#endif
      for(size_t k = 0; k < (size_t)width * height; k++) tmp[k] = ((float *)ovoid)[ch * k];

      dt_gaussian_blur(gauss, tmp, tmp);


      /* create zonemap preview for output */
      dt_iop_gui_enter_critical_section(self);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(height, size, width) \
      shared(tmp, g) \
      schedule(static)
#endif
      for(size_t k = 0; k < (size_t)width * height; k++)
      {
        g->out_preview_buffer[k] = CLAMPS(tmp[k] * (size - 1) / 100.0f, 0, size - 2);
      }
      dt_iop_gui_leave_critical_section(self);
    }

    g_free(tmp);
    if(gauss) dt_gaussian_free(gauss);
  }
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return;

  const dt_iop_zonesystem_data_t *const d = (const dt_iop_zonesystem_data_t *const)piece->data;
  process_common_setup(self, piece, ivoid, ovoid, roi_in, roi_out);

  const int size = d->params.size;

  const float *const restrict in = (const float *const)ivoid;
  float *const restrict out = (float *const)ovoid;
  const size_t npixels = (size_t)roi_out->width * roi_out->height;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(d, in, out, npixels, size) \
  schedule(static)
#endif
  for(size_t k = 0; k < (size_t)4 * npixels; k += 4)
  {
    /* remap lightness into zonemap and apply lightness */
    const int rz = CLAMPS(in[k] * d->rzscale, 0, size - 2); // zone index
    const float zs = ((rz > 0) ? (d->zonemap_offset[rz] / in[k]) : 0) + d->zonemap_scale[rz];
    for_each_channel(c,aligned(in,out))
    {
      out[k+c] = in[k+c] * zs;
    }
  }

  process_common_cleanup(self, piece, ivoid, ovoid, roi_in, roi_out);
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_zonesystem_data_t *data = (dt_iop_zonesystem_data_t *)piece->data;
  dt_iop_zonesystem_global_data_t *gd = (dt_iop_zonesystem_global_data_t *)self->global_data;
  cl_mem dev_zmo, dev_zms = NULL;
  cl_int err = -999;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  /* calculate zonemap */
  const int size = data->params.size;
  float zonemap[MAX_ZONE_SYSTEM_SIZE] = { -1 };
  float zonemap_offset[ROUNDUP(MAX_ZONE_SYSTEM_SIZE, 16)] = { -1 };
  float zonemap_scale[ROUNDUP(MAX_ZONE_SYSTEM_SIZE, 16)] = { -1 };

  _iop_zonesystem_calculate_zonemap(&(data->params), zonemap);

  /* precompute scale and offset */
  for(int k = 0; k < size - 1; k++) zonemap_scale[k] = (zonemap[k + 1] - zonemap[k]) * (size - 1);
  for(int k = 0; k < size - 1; k++) zonemap_offset[k] = 100.0f * ((k + 1) * zonemap[k] - k * zonemap[k + 1]);

  dev_zmo = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * ROUNDUP(MAX_ZONE_SYSTEM_SIZE, 16),
                                                   zonemap_offset);
  if(dev_zmo == NULL) goto error;
  dev_zms = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * ROUNDUP(MAX_ZONE_SYSTEM_SIZE, 16),
                                                   zonemap_scale);
  if(dev_zms == NULL) goto error;

  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };

  dt_opencl_set_kernel_arg(devid, gd->kernel_zonesystem, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_zonesystem, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_zonesystem, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_zonesystem, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_zonesystem, 4, sizeof(int), (void *)&size);
  dt_opencl_set_kernel_arg(devid, gd->kernel_zonesystem, 5, sizeof(cl_mem), (void *)&dev_zmo);
  dt_opencl_set_kernel_arg(devid, gd->kernel_zonesystem, 6, sizeof(cl_mem), (void *)&dev_zms);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_zonesystem, sizes);

  if(err != CL_SUCCESS) goto error;
  dt_opencl_release_mem_object(dev_zmo);
  dt_opencl_release_mem_object(dev_zms);
  return TRUE;

error:
  dt_opencl_release_mem_object(dev_zmo);
  dt_opencl_release_mem_object(dev_zms);
  dt_print(DT_DEBUG_OPENCL, "[opencl_zonesystem] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif



void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_zonesystem_global_data_t *gd
      = (dt_iop_zonesystem_global_data_t *)malloc(sizeof(dt_iop_zonesystem_global_data_t));
  module->data = gd;
  gd->kernel_zonesystem = dt_opencl_create_kernel(program, "zonesystem");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_zonesystem_global_data_t *gd = (dt_iop_zonesystem_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_zonesystem);
  free(module->data);
  module->data = NULL;
}


void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_zonesystem_params_t *p = (dt_iop_zonesystem_params_t *)p1;

  dt_iop_zonesystem_data_t *d = (dt_iop_zonesystem_data_t *)piece->data;

  d->params = *p;
  d->rzscale = (d->params.size - 1) / 100.0f;

  /* calculate zonemap */
  float zonemap[MAX_ZONE_SYSTEM_SIZE] = { -1 };
  _iop_zonesystem_calculate_zonemap(&(d->params), zonemap);

  const int size = d->params.size;

  // precompute scale and offset
  for(int k = 0; k < size - 1; k++) d->zonemap_scale[k] = (zonemap[k + 1] - zonemap[k]) * (size - 1);
  for(int k = 0; k < size - 1; k++)
    d->zonemap_offset[k] = 100.0f * ((k + 1) * zonemap[k] - k * zonemap[k + 1]);
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_zonesystem_data_t));
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  //  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_zonesystem_gui_data_t *g = (dt_iop_zonesystem_gui_data_t *)self->gui_data;
  // dt_iop_zonesystem_params_t *p = (dt_iop_zonesystem_params_t *)module->params;
  gtk_widget_queue_draw(GTK_WIDGET(g->zones));
}

static void _iop_zonesystem_redraw_preview_callback(gpointer instance, gpointer user_data);

static gboolean dt_iop_zonesystem_preview_draw(GtkWidget *widget, cairo_t *crf, dt_iop_module_t *self);

static gboolean dt_iop_zonesystem_bar_draw(GtkWidget *widget, cairo_t *crf, dt_iop_module_t *self);
static gboolean dt_iop_zonesystem_bar_motion_notify(GtkWidget *widget, GdkEventMotion *event,
                                                    dt_iop_module_t *self);
static gboolean dt_iop_zonesystem_bar_leave_notify(GtkWidget *widget, GdkEventCrossing *event,
                                                   dt_iop_module_t *self);
static gboolean dt_iop_zonesystem_bar_button_press(GtkWidget *widget, GdkEventButton *event,
                                                   dt_iop_module_t *self);
static gboolean dt_iop_zonesystem_bar_button_release(GtkWidget *widget, GdkEventButton *event,
                                                     dt_iop_module_t *self);
static gboolean dt_iop_zonesystem_bar_scrolled(GtkWidget *widget, GdkEventScroll *event,
                                               dt_iop_module_t *self);


static void size_allocate_callback(GtkWidget *widget, GtkAllocation *allocation, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_zonesystem_gui_data_t *g = (dt_iop_zonesystem_gui_data_t *)self->gui_data;

  if(g->image) cairo_surface_destroy(g->image);
  free(g->image_buffer);

  /* load the dt logo as a background */
  g->image = dt_util_get_logo(MIN(allocation->width, allocation->height) * 0.75);
  if(g->image)
  {
    g->image_buffer = cairo_image_surface_get_data(g->image);
    g->image_width = cairo_image_surface_get_width(g->image);
    g->image_height = cairo_image_surface_get_height(g->image);
  }
  else
  {
    g->image_buffer = NULL;
    g->image_width = 0;
    g->image_height = 0;
  }
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_zonesystem_gui_data_t *g = IOP_GUI_ALLOC(zonesystem);
  g->in_preview_buffer = g->out_preview_buffer = NULL;
  g->is_dragging = FALSE;
  g->hilite_zone = FALSE;
  g->preview_width = g->preview_height = 0;
  g->mouse_over_output_zones = FALSE;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_IOP_MODULE_CONTROL_SPACING);

  g->preview = dtgtk_drawing_area_new_with_aspect_ratio(1.0);
  g_signal_connect(G_OBJECT(g->preview), "size-allocate", G_CALLBACK(size_allocate_callback), self);
  g_signal_connect(G_OBJECT(g->preview), "draw", G_CALLBACK(dt_iop_zonesystem_preview_draw), self);
  gtk_widget_add_events(GTK_WIDGET(g->preview), GDK_POINTER_MOTION_MASK
                                                | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                                | GDK_LEAVE_NOTIFY_MASK);

  /* create the zonesystem bar widget */
  g->zones = gtk_drawing_area_new();
  gtk_widget_set_tooltip_text(g->zones, _("lightness zones\nuse mouse scrollwheel to change the number of zones\n"
                                          "left-click on a border to create a marker\n"
                                          "right-click on a marker to delete it"));
  g_signal_connect(G_OBJECT(g->zones), "draw", G_CALLBACK(dt_iop_zonesystem_bar_draw), self);
  g_signal_connect(G_OBJECT(g->zones), "motion-notify-event", G_CALLBACK(dt_iop_zonesystem_bar_motion_notify),
                   self);
  g_signal_connect(G_OBJECT(g->zones), "leave-notify-event", G_CALLBACK(dt_iop_zonesystem_bar_leave_notify),
                   self);
  g_signal_connect(G_OBJECT(g->zones), "button-press-event", G_CALLBACK(dt_iop_zonesystem_bar_button_press),
                   self);
  g_signal_connect(G_OBJECT(g->zones), "button-release-event",
                   G_CALLBACK(dt_iop_zonesystem_bar_button_release), self);
  g_signal_connect(G_OBJECT(g->zones), "scroll-event", G_CALLBACK(dt_iop_zonesystem_bar_scrolled), self);
  gtk_widget_add_events(GTK_WIDGET(g->zones), GDK_POINTER_MOTION_MASK
                                              | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                              | GDK_LEAVE_NOTIFY_MASK | darktable.gui->scroll_mask);
  gtk_widget_set_size_request(g->zones, -1, DT_PIXEL_APPLY_DPI(40));

  gtk_box_pack_start(GTK_BOX(self->widget), g->preview, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->zones, TRUE, TRUE, 0);

  /* add signal handler for preview pipe finish to redraw the preview */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                            G_CALLBACK(_iop_zonesystem_redraw_preview_callback), self);


  g->image = NULL;
  g->image_buffer = NULL;
  g->image_width = 0;
  g->image_height = 0;
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_iop_zonesystem_redraw_preview_callback), self);

  dt_iop_zonesystem_gui_data_t *g = (dt_iop_zonesystem_gui_data_t *)self->gui_data;
  g_free(g->in_preview_buffer);
  g_free(g->out_preview_buffer);
  if(g->image) cairo_surface_destroy(g->image);
  free(g->image_buffer);

  IOP_GUI_FREE;
}

#define DT_ZONESYSTEM_INSET DT_PIXEL_APPLY_DPI(5)
#define DT_ZONESYSTEM_BAR_SPLIT_WIDTH 0.0
#define DT_ZONESYSTEM_REFERENCE_SPLIT 0.30
static gboolean dt_iop_zonesystem_bar_draw(GtkWidget *widget, cairo_t *crf, dt_iop_module_t *self)
{
  dt_iop_zonesystem_gui_data_t *g = (dt_iop_zonesystem_gui_data_t *)self->gui_data;
  dt_iop_zonesystem_params_t *p = (dt_iop_zonesystem_params_t *)self->params;

  const int inset = DT_ZONESYSTEM_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  /* clear background */
  cairo_set_source_rgb(cr, .15, .15, .15);
  cairo_paint(cr);


  /* translate and scale */
  width -= 2 * inset;
  height -= 2 * inset;
  cairo_save(cr);
  cairo_translate(cr, inset, inset);
  cairo_scale(cr, width, height);

  /* render the bars */
  float zonemap[MAX_ZONE_SYSTEM_SIZE] = { 0 };
  _iop_zonesystem_calculate_zonemap(p, zonemap);
  float s = (1. / (p->size - 2));
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
  for(int i = 0; i < p->size - 1; i++)
  {
    /* draw the reference zone */
    float z = s * i;
    cairo_rectangle(cr, (1. / (p->size - 1)) * i, 0, (1. / (p->size - 1)),
                    DT_ZONESYSTEM_REFERENCE_SPLIT - DT_ZONESYSTEM_BAR_SPLIT_WIDTH);
    cairo_set_source_rgb(cr, z, z, z);
    cairo_fill(cr);

    /* draw zone mappings */
    cairo_rectangle(cr, zonemap[i], DT_ZONESYSTEM_REFERENCE_SPLIT + DT_ZONESYSTEM_BAR_SPLIT_WIDTH,
                    (zonemap[i + 1] - zonemap[i]), 1.0 - DT_ZONESYSTEM_REFERENCE_SPLIT);
    cairo_set_source_rgb(cr, z, z, z);
    cairo_fill(cr);
  }
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
  cairo_restore(cr);

  /* render zonebar control lines */
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
  cairo_set_line_width(cr, 1.);
  cairo_rectangle(cr, inset, inset, width, height);
  cairo_set_source_rgb(cr, .1, .1, .1);
  cairo_stroke(cr);
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);

  /* render control points handles */
  cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  const float arrw = DT_PIXEL_APPLY_DPI(7.0f);
  for(int k = 1; k < p->size - 1; k++)
  {
    float nzw = zonemap[k + 1] - zonemap[k];
    float pzw = zonemap[k] - zonemap[k - 1];
    if((((g->mouse_x / width) > (zonemap[k] - (pzw / 2.0)))
        && ((g->mouse_x / width) < (zonemap[k] + (nzw / 2.0)))) || p->zone[k] != -1)
    {
      gboolean is_under_mouse = ((width * zonemap[k]) - arrw * .5f < g->mouse_x
                                 && (width * zonemap[k]) + arrw * .5f > g->mouse_x);

      cairo_move_to(cr, inset + (width * zonemap[k]), height + (2 * inset) - 1);
      cairo_rel_line_to(cr, -arrw * .5f, 0);
      cairo_rel_line_to(cr, arrw * .5f, -arrw);
      cairo_rel_line_to(cr, arrw * .5f, arrw);
      cairo_close_path(cr);

      if(is_under_mouse)
        cairo_fill(cr);
      else
        cairo_stroke(cr);
    }
  }


  /* push mem surface into widget */
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);

  return TRUE;
}

static gboolean dt_iop_zonesystem_bar_button_press(GtkWidget *widget, GdkEventButton *event,
                                                   dt_iop_module_t *self)
{
  dt_iop_zonesystem_params_t *p = (dt_iop_zonesystem_params_t *)self->params;
  dt_iop_zonesystem_gui_data_t *g = (dt_iop_zonesystem_gui_data_t *)self->gui_data;
  const int inset = DT_ZONESYSTEM_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width - 2 * inset; /*, height = allocation.height - 2*inset;*/

  /* calculate zonemap */
  float zonemap[MAX_ZONE_SYSTEM_SIZE] = { -1 };
  _iop_zonesystem_calculate_zonemap(p, zonemap);

  /* translate mouse into zone index */
  int k = _iop_zonesystem_zone_index_from_lightness(g->mouse_x / width, zonemap, p->size);
  float zw = zonemap[k + 1] - zonemap[k];
  if((g->mouse_x / width) > zonemap[k] + (zw / 2)) k++;


  if(event->button == 1)
  {
    if(p->zone[k] == -1)
    {
      p->zone[k] = zonemap[k];
      dt_dev_add_history_item(darktable.develop, self, TRUE);
    }
    g->is_dragging = TRUE;
    g->current_zone = k;
  }
  else if(event->button == 3)
  {
    /* clear the controlpoint */
    p->zone[k] = -1;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }

  return TRUE;
}

static gboolean dt_iop_zonesystem_bar_button_release(GtkWidget *widget, GdkEventButton *event,
                                                     dt_iop_module_t *self)
{
  dt_iop_zonesystem_gui_data_t *g = (dt_iop_zonesystem_gui_data_t *)self->gui_data;
  if(event->button == 1)
  {
    g->is_dragging = FALSE;
  }
  return TRUE;
}

static gboolean dt_iop_zonesystem_bar_scrolled(GtkWidget *widget, GdkEventScroll *event, dt_iop_module_t *self)
{
  dt_iop_zonesystem_params_t *p = (dt_iop_zonesystem_params_t *)self->params;
  int cs = CLAMP(p->size, 4, MAX_ZONE_SYSTEM_SIZE);

  if(dt_gui_ignore_scroll(event)) return FALSE;

  int delta_y;
  if(dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y))
  {
    p->size = CLAMP(p->size - delta_y, 4, MAX_ZONE_SYSTEM_SIZE);
    p->zone[cs] = -1;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    gtk_widget_queue_draw(widget);
  }

  return TRUE;
}

static gboolean dt_iop_zonesystem_bar_leave_notify(GtkWidget *widget, GdkEventCrossing *event,
                                                   dt_iop_module_t *self)
{
  dt_iop_zonesystem_gui_data_t *g = (dt_iop_zonesystem_gui_data_t *)self->gui_data;
  g->hilite_zone = FALSE;
  gtk_widget_queue_draw(g->preview);
  return TRUE;
}

static gboolean dt_iop_zonesystem_bar_motion_notify(GtkWidget *widget, GdkEventMotion *event,
                                                    dt_iop_module_t *self)
{
  dt_iop_zonesystem_params_t *p = (dt_iop_zonesystem_params_t *)self->params;
  dt_iop_zonesystem_gui_data_t *g = (dt_iop_zonesystem_gui_data_t *)self->gui_data;
  const int inset = DT_ZONESYSTEM_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width - 2 * inset, height = allocation.height - 2 * inset;

  /* calculate zonemap */
  float zonemap[MAX_ZONE_SYSTEM_SIZE] = { -1 };
  _iop_zonesystem_calculate_zonemap(p, zonemap);

  /* record mouse position within control */
  g->mouse_x = CLAMP(event->x - inset, 0, width);
  g->mouse_y = CLAMP(height - 1 - event->y + inset, 0, height);

  if(g->is_dragging)
  {
    if((g->mouse_x / width) > zonemap[g->current_zone - 1]
       && (g->mouse_x / width) < zonemap[g->current_zone + 1])
    {
      p->zone[g->current_zone] = (g->mouse_x / width);
      dt_dev_add_history_item(darktable.develop, self, TRUE);
    }
  }
  else
  {
    /* decide which zone the mouse is over */
    if(g->mouse_y >= height * (1.0 - DT_ZONESYSTEM_REFERENCE_SPLIT))
    {
      g->zone_under_mouse = (g->mouse_x / width) / (1.0 / (p->size - 1));
      g->mouse_over_output_zones = TRUE;
    }
    else
    {
      float xpos = g->mouse_x / width;
      for(int z = 0; z < p->size - 1; z++)
      {
        if(xpos >= zonemap[z] && xpos < zonemap[z + 1])
        {
          g->zone_under_mouse = z;
          break;
        }
      }
      g->mouse_over_output_zones = FALSE;
    }
    g->hilite_zone = (g->mouse_y < height) ? TRUE : FALSE;
  }

  gtk_widget_queue_draw(self->widget);
  gtk_widget_queue_draw(g->preview);
  return TRUE;
}


static gboolean dt_iop_zonesystem_preview_draw(GtkWidget *widget, cairo_t *crf, dt_iop_module_t *self)
{
  const int inset = DT_PIXEL_APPLY_DPI(2);
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;

  dt_iop_zonesystem_gui_data_t *g = (dt_iop_zonesystem_gui_data_t *)self->gui_data;
  dt_iop_zonesystem_params_t *p = (dt_iop_zonesystem_params_t *)self->params;

  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  /* clear background */
  GtkStyleContext *context = gtk_widget_get_style_context(self->expander);
  gtk_render_background(context, cr, 0, 0, allocation.width, allocation.height);

  width -= 2 * inset;
  height -= 2 * inset;
  cairo_translate(cr, inset, inset);

  dt_iop_gui_enter_critical_section(self);
  if(g->in_preview_buffer && g->out_preview_buffer && self->enabled)
  {
    /* calculate the zonemap */
    float zonemap[MAX_ZONE_SYSTEM_SIZE] = { -1 };
    _iop_zonesystem_calculate_zonemap(p, zonemap);

    /* let's generate a pixbuf from pixel zone buffer */
    guchar *image = g_malloc_n((size_t)4 * g->preview_width * g->preview_height, sizeof(guchar));
    guchar *buffer = g->mouse_over_output_zones ? g->out_preview_buffer : g->in_preview_buffer;
    for(int k = 0; k < g->preview_width * g->preview_height; k++)
    {
      int zone = 255 * CLIP(((1.0 / (p->size - 1)) * buffer[k]));
      image[4 * k + 2] = (g->hilite_zone && buffer[k] == g->zone_under_mouse) ? 255 : zone;
      image[4 * k + 1] = (g->hilite_zone && buffer[k] == g->zone_under_mouse) ? 255 : zone;
      image[4 * k + 0] = (g->hilite_zone && buffer[k] == g->zone_under_mouse) ? 0 : zone;
    }
    dt_iop_gui_leave_critical_section(self);

    const int wd = g->preview_width, ht = g->preview_height;
    const float scale = fminf(width / (float)wd, height / (float)ht);
    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, wd);
    cairo_surface_t *surface = cairo_image_surface_create_for_data(image, CAIRO_FORMAT_RGB24, wd, ht, stride);
    cairo_translate(cr, width / 2.0, height / 2.0f);
    cairo_scale(cr, scale, scale);
    cairo_translate(cr, -.5f * wd, -.5f * ht);

    cairo_rectangle(cr, DT_PIXEL_APPLY_DPI(1), DT_PIXEL_APPLY_DPI(1), wd - DT_PIXEL_APPLY_DPI(2),
                    ht - DT_PIXEL_APPLY_DPI(2));
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_fill_preserve(cr);
    cairo_surface_destroy(surface);

    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0));
    cairo_set_source_rgb(cr, .1, .1, .1);
    cairo_stroke(cr);

    g_free(image);
  }
  else
  {
    dt_iop_gui_leave_critical_section(self);
    // draw a big, subdued dt logo
    if(g->image)
    {
      GdkRGBA *color;
      gtk_style_context_get(context, gtk_widget_get_state_flags(self->expander), "background-color", &color,
                            NULL);

      cairo_set_source_surface(cr, g->image, (width - g->image_width) * 0.5, (height - g->image_height) * 0.5);
      cairo_rectangle(cr, 0, 0, width, height);
      cairo_set_operator(cr, CAIRO_OPERATOR_HSL_LUMINOSITY);
      cairo_fill_preserve(cr);
      cairo_set_operator(cr, CAIRO_OPERATOR_DARKEN);
      cairo_set_source_rgb(cr, color->red + 0.02, color->green + 0.02, color->blue + 0.02);
      cairo_fill_preserve(cr);
      cairo_set_operator(cr, CAIRO_OPERATOR_LIGHTEN);
      cairo_set_source_rgb(cr, color->red - 0.02, color->green - 0.02, color->blue - 0.02);
      cairo_fill(cr);

      gdk_rgba_free(color);
    }
  }

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);

  return TRUE;
}

void _iop_zonesystem_redraw_preview_callback(gpointer instance, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_zonesystem_gui_data_t *g = (dt_iop_zonesystem_gui_data_t *)self->gui_data;

  dt_control_queue_redraw_widget(g->preview);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

