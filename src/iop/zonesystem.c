/*
    This file is part of darktable,
    copyright (c) 2010 Henrik Andersson.

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
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#ifdef HAVE_GEGL
#include <gegl.h>
#endif
#include "common/darktable.h"
#include "common/opencl.h"
#include "common/gaussian.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "control/conf.h"
#include "dtgtk/togglebutton.h"
#include "dtgtk/gradientslider.h"
#include "dtgtk/drawingarea.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include <xmmintrin.h>

#include <librsvg/rsvg.h>
// ugh, ugly hack. why do people break stuff all the time?
#ifndef RSVG_CAIRO_H
#include <librsvg/rsvg-cairo.h>
#endif


#define CLIP(x) (((x) >= 0) ? ((x) <= 1.0 ? (x) : 1.0) : 0.0)
DT_MODULE_INTROSPECTION(1, dt_iop_zonesystem_params_t)
#define MAX_ZONE_SYSTEM_SIZE 24

/** gui params. */
typedef struct dt_iop_zonesystem_params_t
{
  int size;
  float zone[MAX_ZONE_SYSTEM_SIZE + 1];
} dt_iop_zonesystem_params_t;

/** and pixelpipe data is just the same */
typedef struct dt_iop_zonesystem_params_t dt_iop_zonesystem_data_t;

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
  dt_pthread_mutex_t lock;

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
         | IOP_FLAGS_PREVIEW_NON_OPENCL;
}

int groups()
{
  return IOP_GROUP_TONE;
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

#define GAUSS(a, b, c, x) (a * pow(2.718281828, (-pow((x - b), 2) / (pow(c, 2)))))
void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid,
             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  float *in;
  float *out;
  dt_iop_zonesystem_gui_data_t *g = NULL;
  dt_iop_zonesystem_data_t *data = (dt_iop_zonesystem_data_t *)piece->data;

  const int width = roi_out->width;
  const int height = roi_out->height;

  if(self->dev->gui_attached && piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
  {
    g = (dt_iop_zonesystem_gui_data_t *)self->gui_data;
    dt_pthread_mutex_lock(&g->lock);
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
    dt_pthread_mutex_unlock(&g->lock);
  }

  /* calculate zonemap */
  const int size = data->size;
  float zonemap[MAX_ZONE_SYSTEM_SIZE] = { -1 };
  _iop_zonesystem_calculate_zonemap(data, zonemap);
  const int ch = piece->colors;


  /* process the image */
  in = (float *)ivoid;
  out = (float *)ovoid;

  const float rzscale = (size - 1) / 100.0f;

  float zonemap_offset[MAX_ZONE_SYSTEM_SIZE] = { -1 };
  float zonemap_scale[MAX_ZONE_SYSTEM_SIZE] = { -1 };

  // precompute scale and offset
  for(int k = 0; k < size - 1; k++) zonemap_scale[k] = (zonemap[k + 1] - zonemap[k]) * (size - 1);
  for(int k = 0; k < size - 1; k++) zonemap_offset[k] = 100.0f * ((k + 1) * zonemap[k] - k * zonemap[k + 1]);

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(in, out, zonemap_scale, zonemap_offset) schedule(static)
#endif
  for(int j = 0; j < height; j++)
    for(int i = 0; i < width; i++)
    {
      /* remap lightness into zonemap and apply lightness */
      const float *inp = in + ch * ((size_t)j * width + i);
      float *outp = out + ch * ((size_t)j * width + i);

      const int rz = CLAMPS(inp[0] * rzscale, 0, size - 2); // zone index

      const float zs = ((rz > 0) ? (zonemap_offset[rz] / inp[0]) : 0) + zonemap_scale[rz];

      _mm_stream_ps(outp, _mm_mul_ps(_mm_load_ps(inp), _mm_set1_ps(zs)));
    }

  _mm_sfence();

  if(piece->pipe->mask_display) dt_iop_alpha_copy(ivoid, ovoid, width, height);


  /* if gui and have buffer lets gaussblur and fill buffer with zone indexes */
  if(self->dev->gui_attached && g && g->in_preview_buffer && g->out_preview_buffer)
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
#pragma omp parallel for default(none) shared(ivoid, tmp) schedule(static)
#endif
      for(size_t k = 0; k < (size_t)width * height; k++) tmp[k] = ((float *)ivoid)[ch * k];

      dt_gaussian_blur(gauss, tmp, tmp);

      /* create zonemap preview for input */
      dt_pthread_mutex_lock(&g->lock);
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(tmp, g) schedule(static)
#endif
      for(size_t k = 0; k < (size_t)width * height; k++)
      {
        g->in_preview_buffer[k] = CLAMPS(tmp[k] * (size - 1) / 100.0f, 0, size - 2);
      }
      dt_pthread_mutex_unlock(&g->lock);


#ifdef _OPENMP
#pragma omp parallel for default(none) shared(ovoid, tmp) schedule(static)
#endif
      for(size_t k = 0; k < (size_t)width * height; k++) tmp[k] = ((float *)ovoid)[ch * k];

      dt_gaussian_blur(gauss, tmp, tmp);


      /* create zonemap preview for output */
      dt_pthread_mutex_lock(&g->lock);
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(tmp, g) schedule(static)
#endif
      for(size_t k = 0; k < (size_t)width * height; k++)
      {
        g->out_preview_buffer[k] = CLAMPS(tmp[k] * (size - 1) / 100.0f, 0, size - 2);
      }
      dt_pthread_mutex_unlock(&g->lock);
    }

    g_free(tmp);
    if(gauss) dt_gaussian_free(gauss);
  }
}


#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_zonesystem_data_t *data = (dt_iop_zonesystem_data_t *)piece->data;
  dt_iop_zonesystem_global_data_t *gd = (dt_iop_zonesystem_global_data_t *)self->data;
  cl_mem dev_zmo, dev_zms = NULL;
  cl_int err = -999;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  /* calculate zonemap */
  const int size = data->size;
  float zonemap[MAX_ZONE_SYSTEM_SIZE] = { -1 };
  float zonemap_offset[ROUNDUP(MAX_ZONE_SYSTEM_SIZE, 16)] = { -1 };
  float zonemap_scale[ROUNDUP(MAX_ZONE_SYSTEM_SIZE, 16)] = { -1 };

  _iop_zonesystem_calculate_zonemap(data, zonemap);

  /* precompute scale and offset */
  for(int k = 0; k < size - 1; k++) zonemap_scale[k] = (zonemap[k + 1] - zonemap[k]) * (size - 1);
  for(int k = 0; k < size - 1; k++) zonemap_offset[k] = 100.0f * ((k + 1) * zonemap[k] - k * zonemap[k + 1]);

  dev_zmo = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * ROUNDUP(MAX_ZONE_SYSTEM_SIZE, 16),
                                                   zonemap_offset);
  if(dev_zmo == NULL) goto error;
  dev_zms = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * ROUNDUP(MAX_ZONE_SYSTEM_SIZE, 16),
                                                   zonemap_scale);
  if(dev_zms == NULL) goto error;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };

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
  if(dev_zmo != NULL) dt_opencl_release_mem_object(dev_zmo);
  if(dev_zms != NULL) dt_opencl_release_mem_object(dev_zms);
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
#ifdef HAVE_GEGL
  fprintf(stderr, "[zonesystem] TODO: implement gegl version!\n");
// pull in new params to gegl
#else
  dt_iop_zonesystem_data_t *d = (dt_iop_zonesystem_data_t *)piece->data;
  d->size = p->size;
  for(int i = 0; i <= MAX_ZONE_SYSTEM_SIZE; i++) d->zone[i] = p->zone[i];
#endif
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = calloc(1, sizeof(dt_iop_zonesystem_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
#endif
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // clean up everything again.
  (void)gegl_node_remove_child(pipe->gegl, piece->input);
// no free necessary, no data is alloc'ed
#else
  free(piece->data);
  piece->data = NULL;
#endif
}

void gui_update(struct dt_iop_module_t *self)
{
  //  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_zonesystem_gui_data_t *g = (dt_iop_zonesystem_gui_data_t *)self->gui_data;
  // dt_iop_zonesystem_params_t *p = (dt_iop_zonesystem_params_t *)module->params;
  gtk_widget_queue_draw(GTK_WIDGET(g->zones));
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_zonesystem_params_t));
  module->default_params = malloc(sizeof(dt_iop_zonesystem_params_t));
  module->default_enabled = 0;
  module->priority = 650; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_zonesystem_params_t);
  module->gui_data = NULL;
  dt_iop_zonesystem_params_t tmp = (dt_iop_zonesystem_params_t){
    10, { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 }
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_zonesystem_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_zonesystem_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
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


void size_allocate_callback(GtkWidget *widget, GtkAllocation *allocation, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_zonesystem_gui_data_t *g = (dt_iop_zonesystem_gui_data_t *)self->gui_data;

  if(g->image) cairo_surface_destroy(g->image);
  free(g->image_buffer);

  /* load the dt logo as a brackground */
  char filename[PATH_MAX] = { 0 };
  char datadir[PATH_MAX] = { 0 };
  char *logo;
  dt_logo_season_t season = get_logo_season();
  if(season != DT_LOGO_SEASON_NONE)
    logo = g_strdup_printf("%%s/pixmaps/idbutton-%d.svg", (int)season);
  else
    logo = g_strdup("%s/pixmaps/idbutton.svg");

  dt_loc_get_datadir(datadir, sizeof(datadir));
  snprintf(filename, sizeof(filename), logo, datadir);
  g_free(logo);
  RsvgHandle *svg = rsvg_handle_new_from_file(filename, NULL);
  if(svg)
  {
    cairo_surface_t *surface;
    cairo_t *cr;

    RsvgDimensionData dimension;
    rsvg_handle_get_dimensions(svg, &dimension);

    float svg_size = MAX(dimension.width, dimension.height);
    float final_size = MIN(allocation->width, allocation->height) * 0.75;
    float factor = final_size / svg_size;
    float final_width = dimension.width * factor * darktable.gui->ppd,
          final_height = dimension.height * factor * darktable.gui->ppd;
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, final_width);

    g->image_buffer = (guint8 *)calloc(stride * final_height, sizeof(guint8));
    surface = dt_cairo_image_surface_create_for_data(g->image_buffer, CAIRO_FORMAT_ARGB32, final_width,
                                                     final_height, stride);
    if(cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
    {
      free(g->image_buffer);
      g->image_buffer = NULL;
    }
    else
    {
      cr = cairo_create(surface);
      cairo_scale(cr, factor, factor);
      rsvg_handle_render_cairo(svg, cr);
      cairo_surface_flush(surface);
      g->image = surface;
      g->image_width = final_width / darktable.gui->ppd;
      g->image_height = final_height / darktable.gui->ppd;
    }
    g_object_unref(svg);
  }
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_zonesystem_gui_data_t));
  dt_iop_zonesystem_gui_data_t *g = (dt_iop_zonesystem_gui_data_t *)self->gui_data;
  g->in_preview_buffer = g->out_preview_buffer = NULL;
  g->is_dragging = FALSE;
  g->hilite_zone = FALSE;
  g->preview_width = g->preview_height = 0;
  g->mouse_over_output_zones = FALSE;

  dt_pthread_mutex_init(&g->lock, NULL);

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_IOP_MODULE_CONTROL_SPACING);

  g->preview = dtgtk_drawing_area_new_with_aspect_ratio(1.0);
  g_signal_connect(G_OBJECT(g->preview), "size-allocate", G_CALLBACK(size_allocate_callback), self);
  g_signal_connect(G_OBJECT(g->preview), "draw", G_CALLBACK(dt_iop_zonesystem_preview_draw), self);
  gtk_widget_add_events(GTK_WIDGET(g->preview), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                                | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                                | GDK_LEAVE_NOTIFY_MASK);

  /* create the zonesystem bar widget */
  g->zones = gtk_drawing_area_new();
  g_object_set(G_OBJECT(g->zones), "tooltip-text",
               _("lightness zones\nuse mouse scrollwheel to change the number of zones\nleft-click on a "
                 "border to create a marker\nright-click on a marker to delete it"),
               (char *)NULL);
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
  gtk_widget_add_events(GTK_WIDGET(g->zones), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                              | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                              | GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK);
  gtk_widget_set_size_request(g->zones, -1, DT_PIXEL_APPLY_DPI(40));

  gtk_box_pack_start(GTK_BOX(self->widget), g->preview, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->zones, TRUE, TRUE, 0);

  /* add signal handler for preview pipe finish to redraw the preview */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                            G_CALLBACK(_iop_zonesystem_redraw_preview_callback), self);


  g->image = NULL;
  g->image_buffer = NULL;
  g->image_width = 0;
  g->image_height = 0;
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_iop_zonesystem_redraw_preview_callback), self);

  dt_iop_zonesystem_gui_data_t *g = (dt_iop_zonesystem_gui_data_t *)self->gui_data;
  g_free(g->in_preview_buffer);
  g_free(g->out_preview_buffer);
  if(g->image) cairo_surface_destroy(g->image);
  free(g->image_buffer);
  dt_pthread_mutex_destroy(&g->lock);
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  free(self->gui_data);
  self->gui_data = NULL;
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

  if(event->direction == GDK_SCROLL_UP)
    p->size += 1;
  else if(event->direction == GDK_SCROLL_DOWN)
    p->size -= 1;

  /* sanity checks */
  p->size = CLAMP(p->size, 4, MAX_ZONE_SYSTEM_SIZE);

  p->zone[cs] = -1;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
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
  GdkRGBA color;
  GtkStyleContext *context = gtk_widget_get_style_context(self->expander);
  gtk_style_context_get_background_color(context, gtk_widget_get_state_flags(self->expander), &color);

  gdk_cairo_set_source_rgba(cr, &color);
  cairo_paint(cr);

  width -= 2 * inset;
  height -= 2 * inset;
  cairo_translate(cr, inset, inset);

  dt_pthread_mutex_lock(&g->lock);
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
    dt_pthread_mutex_unlock(&g->lock);

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
    dt_pthread_mutex_unlock(&g->lock);
    // draw a big, subdued dt logo
    if(g->image)
    {
      cairo_set_source_surface(cr, g->image, (width - g->image_width) * 0.5, (height - g->image_height) * 0.5);
      cairo_rectangle(cr, 0, 0, width, height);
      cairo_set_operator(cr, CAIRO_OPERATOR_HSL_LUMINOSITY);
      cairo_fill_preserve(cr);
      cairo_set_operator(cr, CAIRO_OPERATOR_DARKEN);
      cairo_set_source_rgb(cr, color.red + 0.02, color.green + 0.02, color.blue + 0.02);
      cairo_fill_preserve(cr);
      cairo_set_operator(cr, CAIRO_OPERATOR_LIGHTEN);
      cairo_set_source_rgb(cr, color.red - 0.02, color.green - 0.02, color.blue - 0.02);
      cairo_fill(cr);
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

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
