/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.
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

#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "dtgtk/drawingarea.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"
#include "common/iop_group.h"

#define DT_GUI_CURVE_EDITOR_INSET DT_PIXEL_APPLY_DPI(5)
#define DT_GUI_CURVE_INFL .3f

DT_MODULE_INTROSPECTION(2, dt_iop_levels_params_t)

static gboolean dt_iop_levels_area_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data);
static gboolean dt_iop_levels_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);
static gboolean dt_iop_levels_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean dt_iop_levels_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean dt_iop_levels_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);
static gboolean dt_iop_levels_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data);
static void dt_iop_levels_pick_black_callback(GtkToggleButton *togglebutton, dt_iop_module_t *self);
static void dt_iop_levels_pick_grey_callback(GtkToggleButton *togglebutton, dt_iop_module_t *self);
static void dt_iop_levels_pick_white_callback(GtkToggleButton *togglebutton, dt_iop_module_t *self);
static void dt_iop_levels_autoadjust_callback(GtkRange *range, dt_iop_module_t *self);
static void dt_iop_levels_mode_callback(GtkWidget *combo, gpointer user_data);
static void dt_iop_levels_percentiles_callback(GtkWidget *slider, gpointer user_data);

typedef enum dt_iop_levels_mode_t
{
  LEVELS_MODE_MANUAL,
  LEVELS_MODE_AUTOMATIC
} dt_iop_levels_mode_t;

typedef struct dt_iop_levels_params_t
{
  dt_iop_levels_mode_t mode;
  float percentiles[3];
  float levels[3];
} dt_iop_levels_params_t;

typedef enum dt_iop_levels_pick_t
{
  NONE,
  BLACK,
  GREY,
  WHITE
} dt_iop_levels_pick_t;

typedef struct dt_iop_levels_gui_data_t
{
  GList *modes;
  GtkWidget *mode;
  GtkWidget *mode_stack;
  GtkDrawingArea *area;
  double mouse_x, mouse_y;
  int dragging, handle_move;
  float drag_start_percentage;
  dt_iop_levels_pick_t current_pick;
  GtkToggleButton *activeToggleButton;
  float last_picked_color;
  double pick_xy_positions[3][2];
  GtkWidget *percentile_black;
  GtkWidget *percentile_grey;
  GtkWidget *percentile_white;
  float auto_levels[3];
  uint64_t hash;
  dt_pthread_mutex_t lock;
  GtkWidget *blackpick, *greypick, *whitepick;
} dt_iop_levels_gui_data_t;

typedef struct dt_iop_levels_data_t
{
  dt_iop_levels_mode_t mode;
  float percentiles[3];
  float levels[3];
  float in_inv_gamma;
  float lut[0x10000];
} dt_iop_levels_data_t;

typedef struct dt_iop_levels_global_data_t
{
  int kernel_levels;
} dt_iop_levels_global_data_t;


const char *name()
{
  return _("levels");
}

int groups()
{
  return dt_iop_get_group("levels", IOP_GROUP_TONE);
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 2)
  {
    typedef struct dt_iop_levels_params_v1_t
    {
      float levels[3];
      int levels_preset;
    } dt_iop_levels_params_v1_t;

    dt_iop_levels_params_v1_t *o = (dt_iop_levels_params_v1_t *)old_params;
    dt_iop_levels_params_t *n = (dt_iop_levels_params_t *)new_params;
    dt_iop_levels_params_t *d = (dt_iop_levels_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    n->levels[0] = o->levels[0];
    n->levels[1] = o->levels[1];
    n->levels[2] = o->levels[2];
    return 0;
  }
  return 1;
}

static void dt_iop_levels_compute_levels_manual(const uint32_t *histogram, float *levels)
{
  if(!histogram) return;

  // search histogram for min (search from bottom)
  for(int k = 0; k <= 4 * 255; k += 4)
  {
    if(histogram[k] > 1)
    {
      levels[0] = ((float)(k) / (4 * 256));
      break;
    }
  }
  // then for max (search from top)
  for(int k = 4 * 255; k >= 0; k -= 4)
  {
    if(histogram[k] > 1)
    {
      levels[2] = ((float)(k) / (4 * 256));
      break;
    }
  }
  levels[1] = levels[0] / 2 + levels[2] / 2;
}

static void dt_iop_levels_compute_levels_automatic(dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_levels_data_t *d = (dt_iop_levels_data_t *)piece->data;

  uint32_t total = piece->histogram_stats.pixels;

  float thr[3];
  for(int k = 0; k < 3; k++)
  {
    thr[k] = (float)total * d->percentiles[k] / 100.0f;
    d->levels[k] = NAN;
  }

  if(piece->histogram == NULL) return;

  // find min and max levels
  size_t n = 0;
  for(uint32_t i = 0; i < piece->histogram_stats.bins_count; i++)
  {
    n += piece->histogram[4 * i];

    for(int k = 0; k < 3; k++)
    {
      if(isnan(d->levels[k]) && (n >= thr[k]))
      {
        d->levels[k] = (float)i / (float)(piece->histogram_stats.bins_count - 1);
      }
    }
  }
  // for numerical reasons sometimes the threshold is sharp but in float and n is size_t.
  // in this case we want to make sure we don't keep nan:
  if(isnan(d->levels[2])) d->levels[2] = 1.0f;

  // compute middle level from min and max levels
  float center = d->percentiles[1] / 100.0f;
  if(!isnan(d->levels[0]) && !isnan(d->levels[2]))
    d->levels[1] = (1.0f - center) * d->levels[0] + center * d->levels[2];
}

static void compute_lut(dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_levels_data_t *d = (dt_iop_levels_data_t *)piece->data;

  // Building the lut for values in the [0,1] range
  float delta = (d->levels[2] - d->levels[0]) / 2.0f;
  float mid = d->levels[0] + delta;
  float tmp = (d->levels[1] - mid) / delta;
  d->in_inv_gamma = pow(10, tmp);

  for(unsigned int i = 0; i < 0x10000; i++)
  {
    float percentage = (float)i / (float)0x10000ul;
    d->lut[i] = 100.0f * pow(percentage, d->in_inv_gamma);
  }
}

/*
 * WARNING: unlike commit_params, which is thread safe wrt gui thread and
 * pipes, this function lives in the pipeline thread, and NOT thread safe!
 */
static void commit_params_late(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_levels_data_t *d = (dt_iop_levels_data_t *)piece->data;
  dt_iop_levels_gui_data_t *g = (dt_iop_levels_gui_data_t *)self->gui_data;

  if(d->mode == LEVELS_MODE_AUTOMATIC)
  {
    if(g && piece->pipe->type == DT_DEV_PIXELPIPE_FULL)
    {
      dt_pthread_mutex_lock(&g->lock);
      const uint64_t hash = g->hash;
      dt_pthread_mutex_unlock(&g->lock);

      // note that the case 'hash == 0' on first invocation in a session implies that d->levels[]
      // contains NANs which initiates special handling below to avoid inconsistent results. in all
      // other cases we make sure that the preview pipe has left us with proper readings for
      // g->auto_levels[]. if data are not yet there we need to wait (with timeout).
      if(hash != 0 && !dt_dev_sync_pixelpipe_hash(self->dev, piece->pipe, 0, self->priority, &g->lock, &g->hash))
        dt_control_log(_("inconsistent output"));

      dt_pthread_mutex_lock(&g->lock);
      d->levels[0] = g->auto_levels[0];
      d->levels[1] = g->auto_levels[1];
      d->levels[2] = g->auto_levels[2];
      dt_pthread_mutex_unlock(&g->lock);

      compute_lut(piece);
    }

    if(piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW || isnan(d->levels[0]) || isnan(d->levels[1])
       || isnan(d->levels[2]))
    {
      dt_iop_levels_compute_levels_automatic(piece);
      compute_lut(piece);
    }

    if(g && piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW && d->mode == LEVELS_MODE_AUTOMATIC)
    {
      uint64_t hash = dt_dev_hash_plus(self->dev, piece->pipe, 0, self->priority);
      dt_pthread_mutex_lock(&g->lock);
      g->auto_levels[0] = d->levels[0];
      g->auto_levels[1] = d->levels[1];
      g->auto_levels[2] = d->levels[2];
      g->hash = hash;
      dt_pthread_mutex_unlock(&g->lock);
    }
  }
}

void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const int ch = piece->colors;
  const dt_iop_levels_data_t *const d = (dt_iop_levels_data_t *)piece->data;

  if(d->mode == LEVELS_MODE_AUTOMATIC)
  {
    commit_params_late(self, piece);
  }

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int k = 0; k < roi_out->height; k++)
  {
    float *in = (float *)ivoid + (size_t)k * ch * roi_out->width;
    float *out = (float *)ovoid + (size_t)k * ch * roi_out->width;
    for(int j = 0; j < roi_out->width; j++, in += ch, out += ch)
    {
      float L_in = in[0] / 100.0f;

      if(L_in <= d->levels[0])
      {
        // Anything below the lower threshold just clips to zero
        out[0] = 0.0f;
      }
      else if(L_in >= d->levels[2])
      {
        float percentage = (L_in - d->levels[0]) / (d->levels[2] - d->levels[0]);
        out[0] = 100.0f * pow(percentage, d->in_inv_gamma);
      }
      else
      {
        // Within the expected input range we can use the lookup table
        float percentage = (L_in - d->levels[0]) / (d->levels[2] - d->levels[0]);
        // out[0] = 100.0 * pow(percentage, d->in_inv_gamma);
        out[0] = d->lut[CLAMP((int)(percentage * 0x10000ul), 0, 0xffff)];
      }

      // Preserving contrast
      if(in[0] > 0.01f)
      {
        out[1] = in[1] * out[0] / in[0];
        out[2] = in[2] * out[0] / in[0];
      }
      else
      {
        out[1] = in[1] * out[0] / 0.01f;
        out[2] = in[2] * out[0] / 0.01f;
      }
    }
  }

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_levels_data_t *d = (dt_iop_levels_data_t *)piece->data;
  dt_iop_levels_global_data_t *gd = (dt_iop_levels_global_data_t *)self->data;

  if(d->mode == LEVELS_MODE_AUTOMATIC)
  {
    commit_params_late(self, piece);
  }

  cl_mem dev_lut = NULL;
  cl_int err = -999;
  const int devid = piece->pipe->devid;

  const int width = roi_out->width;
  const int height = roi_out->height;

  dev_lut = dt_opencl_copy_host_to_device(devid, d->lut, 256, 256, sizeof(float));
  if(dev_lut == NULL) goto error;

  size_t sizes[2] = { ROUNDUPWD(width), ROUNDUPHT(height) };
  dt_opencl_set_kernel_arg(devid, gd->kernel_levels, 0, sizeof(cl_mem), &dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_levels, 1, sizeof(cl_mem), &dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_levels, 2, sizeof(int), &width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_levels, 3, sizeof(int), &height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_levels, 4, sizeof(cl_mem), &dev_lut);
  dt_opencl_set_kernel_arg(devid, gd->kernel_levels, 5, sizeof(float), &d->levels[0]);
  dt_opencl_set_kernel_arg(devid, gd->kernel_levels, 6, sizeof(float), &d->levels[2]);
  dt_opencl_set_kernel_arg(devid, gd->kernel_levels, 7, sizeof(float), &d->in_inv_gamma);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_levels, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_lut);

  return TRUE;

error:
  dt_opencl_release_mem_object(dev_lut);
  dt_print(DT_DEBUG_OPENCL, "[opencl_levels] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

// void init_presets (dt_iop_module_so_t *self)
//{
//  dt_iop_levels_params_t p;
//  p.levels_preset = 0;
//
//  p.levels[0] = 0;
//  p.levels[1] = 0.5;
//  p.levels[2] = 1;
//  dt_gui_presets_add_generic(_("unmodified"), self->op, self->version(), &p, sizeof(p), 1);
//}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_levels_data_t *d = (dt_iop_levels_data_t *)piece->data;
  dt_iop_levels_params_t *p = (dt_iop_levels_params_t *)p1;

  if(pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
    piece->request_histogram |= (DT_REQUEST_ON);
  else
    piece->request_histogram &= ~(DT_REQUEST_ON);

  piece->request_histogram |= (DT_REQUEST_ONLY_IN_GUI);

  piece->histogram_params.bins_count = 256;

  if(p->mode == LEVELS_MODE_AUTOMATIC)
  {
    d->mode = LEVELS_MODE_AUTOMATIC;

    piece->request_histogram |= (DT_REQUEST_ON);
    self->request_histogram &= ~(DT_REQUEST_ON);

    if(!self->dev->gui_attached) piece->request_histogram &= ~(DT_REQUEST_ONLY_IN_GUI);

    piece->histogram_params.bins_count = 16384;

    /*
     * in principle, we do not need/want histogram in FULL pipe
     * because we will use histogram from preview pipe there,
     * but it might happen that for some reasons we do not have
     * histogram of preview pipe yet - e.g. on first pipe run
     * (just after setting mode to automatic)
     */

    for(int k = 0; k < 3; k++)
    {
      d->levels[k] = NAN;
      d->percentiles[k] = p->percentiles[k];
    }

    // commit_params_late() will compute LUT later
  }
  else
  {
    d->mode = LEVELS_MODE_MANUAL;

    self->request_histogram |= (DT_REQUEST_ON);

    d->levels[0] = p->levels[0];
    d->levels[1] = p->levels[1];
    d->levels[2] = p->levels[2];
    compute_lut(piece);
  }
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_levels_data_t));
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // clean up everything again.
  free(piece->data);
  piece->data = NULL;
}

void gui_reset(struct dt_iop_module_t *self)
{
  dt_iop_levels_gui_data_t *g = (dt_iop_levels_gui_data_t *)self->gui_data;
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->blackpick), 0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->greypick), 0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->whitepick), 0);
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_levels_gui_data_t *g = (dt_iop_levels_gui_data_t *)self->gui_data;
  dt_iop_levels_params_t *p = (dt_iop_levels_params_t *)self->params;

  dt_bauhaus_combobox_set(g->mode, g_list_index(g->modes, GUINT_TO_POINTER(p->mode)));
  dt_bauhaus_slider_set(g->percentile_black, p->percentiles[0]);
  dt_bauhaus_slider_set(g->percentile_grey, p->percentiles[1]);
  dt_bauhaus_slider_set(g->percentile_white, p->percentiles[2]);

  switch(p->mode)
  {
    case LEVELS_MODE_AUTOMATIC:
      gtk_stack_set_visible_child_name(GTK_STACK(g->mode_stack), "automatic");
      break;
    case LEVELS_MODE_MANUAL:
    default:
      gtk_stack_set_visible_child_name(GTK_STACK(g->mode_stack), "manual");
      break;
  }

  dt_pthread_mutex_lock(&g->lock);
  g->auto_levels[0] = NAN;
  g->auto_levels[1] = NAN;
  g->auto_levels[2] = NAN;
  g->hash = 0;
  dt_pthread_mutex_unlock(&g->lock);

  if (self->request_color_pick == DT_REQUEST_COLORPICK_OFF)
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->blackpick), 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->greypick), 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->whitepick), 0);
  }

  gtk_widget_queue_draw(self->widget);
}

void reload_defaults(dt_iop_module_t *self)
{
  dt_iop_levels_params_t tmp
      = (dt_iop_levels_params_t){ LEVELS_MODE_MANUAL, { 0.0f, 50.0f, 100.0f }, { 0.0f, 0.5f, 1.0f } };
  memcpy(self->params, &tmp, sizeof(dt_iop_levels_params_t));
  memcpy(self->default_params, &tmp, sizeof(dt_iop_levels_params_t));
}

void init(dt_iop_module_t *self)
{
  self->params = calloc(1, sizeof(dt_iop_levels_params_t));
  self->default_params = calloc(1, sizeof(dt_iop_levels_params_t));
  self->default_enabled = 0;
  self->request_histogram |= (DT_REQUEST_ON);
  self->priority = 699; // module order created by iop_dependencies.py, do not edit!
  self->params_size = sizeof(dt_iop_levels_params_t);
  self->gui_data = NULL;
}

void init_global(dt_iop_module_so_t *self)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_levels_global_data_t *gd
      = (dt_iop_levels_global_data_t *)malloc(sizeof(dt_iop_levels_global_data_t));
  self->data = gd;
  gd->kernel_levels = dt_opencl_create_kernel(program, "levels");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_levels_global_data_t *gd = (dt_iop_levels_global_data_t *)self->data;
  dt_opencl_free_kernel(gd->kernel_levels);
  free(self->data);
  self->data = NULL;
}

void cleanup(dt_iop_module_t *self)
{
  free(self->params);
  self->params = NULL;
}

void gui_init(dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_levels_gui_data_t));
  dt_iop_levels_gui_data_t *c = (dt_iop_levels_gui_data_t *)self->gui_data;
  dt_iop_levels_params_t *p = (dt_iop_levels_params_t *)self->params;

  dt_pthread_mutex_init(&c->lock, NULL);

  dt_pthread_mutex_lock(&c->lock);
  c->auto_levels[0] = NAN;
  c->auto_levels[1] = NAN;
  c->auto_levels[2] = NAN;
  c->hash = 0;
  dt_pthread_mutex_unlock(&c->lock);

  c->modes = NULL;

  c->mouse_x = c->mouse_y = -1.0;
  c->dragging = 0;
  c->activeToggleButton = NULL;
  c->current_pick = NONE;
  c->last_picked_color = -1;
  for(int i = 0; i < 3; i++)
    for(int j = 0; j < 2; j++) c->pick_xy_positions[i][j] = -1;
  self->widget = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 5));
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));

  c->mode = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(c->mode, NULL, _("mode"));

  dt_bauhaus_combobox_add(c->mode, C_("mode", "manual"));
  c->modes = g_list_append(c->modes, GUINT_TO_POINTER(LEVELS_MODE_MANUAL));

  dt_bauhaus_combobox_add(c->mode, _("automatic"));
  c->modes = g_list_append(c->modes, GUINT_TO_POINTER(LEVELS_MODE_AUTOMATIC));

  dt_bauhaus_combobox_set_default(c->mode, LEVELS_MODE_MANUAL);
  dt_bauhaus_combobox_set(c->mode, g_list_index(c->modes, GUINT_TO_POINTER(p->mode)));
  gtk_box_pack_start(GTK_BOX(self->widget), c->mode, TRUE, TRUE, 0);

  c->mode_stack = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(c->mode_stack),FALSE);
  gtk_box_pack_start(GTK_BOX(self->widget), c->mode_stack, TRUE, TRUE, 0);

  c->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(9.0 / 16.0));
  GtkWidget *vbox_manual = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 5));
  gtk_box_pack_start(GTK_BOX(vbox_manual), GTK_WIDGET(c->area), TRUE, TRUE, 0);

  gtk_widget_set_tooltip_text(GTK_WIDGET(c->area),_("drag handles to set black, gray, and white points. "
                                                    "operates on L channel."));

  gtk_widget_add_events(GTK_WIDGET(c->area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                             | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                             | GDK_LEAVE_NOTIFY_MASK | darktable.gui->scroll_mask);
  g_signal_connect(G_OBJECT(c->area), "draw", G_CALLBACK(dt_iop_levels_area_draw), self);
  g_signal_connect(G_OBJECT(c->area), "button-press-event", G_CALLBACK(dt_iop_levels_button_press), self);
  g_signal_connect(G_OBJECT(c->area), "button-release-event", G_CALLBACK(dt_iop_levels_button_release), self);
  g_signal_connect(G_OBJECT(c->area), "motion-notify-event", G_CALLBACK(dt_iop_levels_motion_notify), self);
  g_signal_connect(G_OBJECT(c->area), "leave-notify-event", G_CALLBACK(dt_iop_levels_leave_notify), self);
  g_signal_connect(G_OBJECT(c->area), "scroll-event", G_CALLBACK(dt_iop_levels_scroll), self);

  GtkWidget *autobutton = gtk_button_new_with_label(_("auto"));
  gtk_widget_set_tooltip_text(autobutton, _("apply auto levels"));
  gtk_widget_set_size_request(autobutton, -1, DT_PIXEL_APPLY_DPI(24));

  c->blackpick = dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT, NULL);
  gtk_widget_set_tooltip_text(c->blackpick, _("pick black point from image"));

  c->greypick = dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT, NULL);
  gtk_widget_set_tooltip_text(c->greypick, _("pick medium gray point from image"));

  c->whitepick = dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT, NULL);
  gtk_widget_set_tooltip_text(c->whitepick, _("pick white point from image"));

  GdkRGBA color = { 0 };
  color.alpha = 1.0;
  dtgtk_togglebutton_override_color(DTGTK_TOGGLEBUTTON(c->blackpick), &color);
  color.red = color.green = color.blue = 0.5;
  dtgtk_togglebutton_override_color(DTGTK_TOGGLEBUTTON(c->greypick), &color);
  color.red = color.green = color.blue = 1.0;
  dtgtk_togglebutton_override_color(DTGTK_TOGGLEBUTTON(c->whitepick), &color);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(10));
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(autobutton), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(c->blackpick), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(c->greypick), TRUE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(box), GTK_WIDGET(c->whitepick), TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(vbox_manual), box, TRUE, TRUE, 0);

  gtk_widget_show_all(vbox_manual);
  gtk_stack_add_named(GTK_STACK(c->mode_stack), vbox_manual, "manual");

  c->percentile_black = dt_bauhaus_slider_new_with_range(self, 0.0f, 100.0f, .1f, p->percentiles[0], 3);
  gtk_widget_set_tooltip_text(c->percentile_black, _("black percentile"));
  dt_bauhaus_slider_set_format(c->percentile_black, "%.1f%%");
  dt_bauhaus_widget_set_label(c->percentile_black, NULL, _("black"));

  c->percentile_grey = dt_bauhaus_slider_new_with_range(self, 0.0f, 100.0f, .1f, p->percentiles[1], 3);
  gtk_widget_set_tooltip_text(c->percentile_grey, _("gray percentile"));
  dt_bauhaus_slider_set_format(c->percentile_grey, "%.1f%%");
  dt_bauhaus_widget_set_label(c->percentile_grey, NULL, _("gray"));

  c->percentile_white = dt_bauhaus_slider_new_with_range(self, 0.0f, 100.0f, .1f, p->percentiles[2], 3);
  gtk_widget_set_tooltip_text(c->percentile_white, _("white percentile"));
  dt_bauhaus_slider_set_format(c->percentile_white, "%.1f%%");
  dt_bauhaus_widget_set_label(c->percentile_white, NULL, _("white"));

  GtkWidget *vbox_automatic = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 5));
  gtk_box_pack_start(GTK_BOX(vbox_automatic), GTK_WIDGET(c->percentile_black), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox_automatic), GTK_WIDGET(c->percentile_grey), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox_automatic), GTK_WIDGET(c->percentile_white), FALSE, FALSE, 0);

  gtk_widget_show_all(vbox_automatic);
  gtk_stack_add_named(GTK_STACK(c->mode_stack), vbox_automatic, "automatic");

  switch(p->mode)
  {
    case LEVELS_MODE_AUTOMATIC:
      gtk_stack_set_visible_child_name(GTK_STACK(c->mode_stack), "automatic");
      break;
    case LEVELS_MODE_MANUAL:
    default:
      gtk_stack_set_visible_child_name(GTK_STACK(c->mode_stack), "manual");
      break;
  }

  g_signal_connect(G_OBJECT(c->mode), "value-changed", G_CALLBACK(dt_iop_levels_mode_callback), self);
  g_signal_connect(G_OBJECT(c->percentile_black), "value-changed",
                   G_CALLBACK(dt_iop_levels_percentiles_callback), self);
  g_signal_connect(G_OBJECT(c->percentile_grey), "value-changed",
                   G_CALLBACK(dt_iop_levels_percentiles_callback), self);
  g_signal_connect(G_OBJECT(c->percentile_white), "value-changed",
                   G_CALLBACK(dt_iop_levels_percentiles_callback), self);

  g_signal_connect(G_OBJECT(autobutton), "clicked", G_CALLBACK(dt_iop_levels_autoadjust_callback),
                   (gpointer)self);
  g_signal_connect(G_OBJECT(c->blackpick), "toggled", G_CALLBACK(dt_iop_levels_pick_black_callback), self);
  g_signal_connect(G_OBJECT(c->greypick), "toggled", G_CALLBACK(dt_iop_levels_pick_grey_callback), self);
  g_signal_connect(G_OBJECT(c->whitepick), "toggled", G_CALLBACK(dt_iop_levels_pick_white_callback), self);
}

void gui_cleanup(dt_iop_module_t *self)
{
  dt_iop_levels_gui_data_t *g = (dt_iop_levels_gui_data_t *)self->gui_data;
  g_list_free(g->modes);

  dt_pthread_mutex_destroy(&g->lock);

  free(self->gui_data);
  self->gui_data = NULL;
}

static gboolean dt_iop_levels_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_levels_gui_data_t *c = (dt_iop_levels_gui_data_t *)self->gui_data;
  c->mouse_x = c->mouse_y = -1.0;
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean dt_iop_levels_area_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_levels_gui_data_t *c = (dt_iop_levels_gui_data_t *)self->gui_data;
  dt_iop_levels_params_t *p = (dt_iop_levels_params_t *)self->params;

  dt_develop_t *dev = darktable.develop;
  const int inset = DT_GUI_CURVE_EDITOR_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(GTK_WIDGET(c->area), &allocation);
  int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  float mean_picked_color = *self->picked_color / 100.0;

  /* we need to save the last picked color to prevent flickering when
   * changing from one picker to another, as the picked_color value does not
   * update as rapidly */
  if(self->request_color_pick != DT_REQUEST_COLORPICK_OFF && self->color_picker_point[0] >= 0.0f
     && self->color_picker_point[1] >= 0.0f && self->picked_color_max[0] >= 0.0f
     && mean_picked_color != c->last_picked_color)
  {
    float previous_color[3];
    previous_color[0] = p->levels[0];
    previous_color[1] = p->levels[1];
    previous_color[2] = p->levels[2];

    c->last_picked_color = mean_picked_color;

    if(BLACK == c->current_pick)
    {
      if(mean_picked_color > p->levels[1])
      {
        p->levels[0] = p->levels[1] - FLT_EPSILON;
      }
      else
      {
        p->levels[0] = mean_picked_color;
      }
      c->pick_xy_positions[0][0] = self->color_picker_point[0];
      c->pick_xy_positions[0][1] = self->color_picker_point[1];
    }
    else if(GREY == c->current_pick)
    {
      if(mean_picked_color < p->levels[0] || mean_picked_color > p->levels[2])
      {
        p->levels[1] = p->levels[1];
      }
      else
      {
        p->levels[1] = mean_picked_color;
      }
      c->pick_xy_positions[1][0] = self->color_picker_point[0];
      c->pick_xy_positions[1][1] = self->color_picker_point[1];
    }
    else if(WHITE == c->current_pick)
    {
      if(mean_picked_color < p->levels[1])
      {
        p->levels[2] = p->levels[1] + FLT_EPSILON;
      }
      else
      {
        p->levels[2] = mean_picked_color;
      }
      c->pick_xy_positions[2][0] = self->color_picker_point[0];
      c->pick_xy_positions[2][1] = self->color_picker_point[1];
    }

    if(previous_color[0] != p->levels[0] || previous_color[1] != p->levels[1]
       || previous_color[2] != p->levels[2])
    {
      dt_dev_add_history_item(darktable.develop, self, TRUE);
    }
  }

  // clear bg
  cairo_set_source_rgb(cr, .2, .2, .2);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2 * inset;
  height -= 2 * inset;

  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0));
  cairo_set_source_rgb(cr, .1, .1, .1);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_stroke(cr);

  cairo_set_source_rgb(cr, .3, .3, .3);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  // draw grid
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(.4));
  cairo_set_source_rgb(cr, .1, .1, .1);
  if(dev->histogram_type == DT_DEV_HISTOGRAM_WAVEFORM)
    dt_draw_waveform_lines(cr, 0, 0, width, height);
  else
    dt_draw_vertical_lines(cr, 4, 0, 0, width, height);

  // Drawing the vertical line indicators
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));

  for(int k = 0; k < 3; k++)
  {
    if(k == c->handle_move && c->mouse_x > 0)
      cairo_set_source_rgb(cr, 1, 1, 1);
    else
      cairo_set_source_rgb(cr, .7, .7, .7);

    cairo_move_to(cr, width * p->levels[k], height);
    cairo_rel_line_to(cr, 0, -height);
    cairo_stroke(cr);
  }

  // draw x positions
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  const float arrw = DT_PIXEL_APPLY_DPI(7.0f);
  for(int k = 0; k < 3; k++)
  {
    switch(k)
    {
      case 0:
        cairo_set_source_rgb(cr, 0, 0, 0);
        break;

      case 1:
        cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
        break;

      default:
        cairo_set_source_rgb(cr, 1, 1, 1);
        break;
    }

    cairo_move_to(cr, width * p->levels[k], height + inset - 1);
    cairo_rel_line_to(cr, -arrw * .5f, 0);
    cairo_rel_line_to(cr, arrw * .5f, -arrw);
    cairo_rel_line_to(cr, arrw * .5f, arrw);
    cairo_close_path(cr);
    if(c->handle_move == k && c->mouse_x > 0)
      cairo_fill(cr);
    else
      cairo_stroke(cr);
  }

  cairo_translate(cr, 0, height);

  // draw lum histogram in background
  // only if the module is enabled
  if(self->enabled)
  {
    uint32_t *hist = self->histogram;
    float hist_max = dev->histogram_type == DT_DEV_HISTOGRAM_LINEAR ? self->histogram_max[0]
                                                                    : logf(1.0 + self->histogram_max[0]);
    if(hist && hist_max > 0.0f)
    {
      cairo_save(cr);
      cairo_scale(cr, width / 255.0, -(height - DT_PIXEL_APPLY_DPI(5)) / hist_max);
      cairo_set_source_rgba(cr, .2, .2, .2, 0.5);
      dt_draw_histogram_8(cr, hist, 4, 0, dev->histogram_type == DT_DEV_HISTOGRAM_LINEAR); // TODO: make draw
                                                                                        // handle waveform
                                                                                        // histograms
      cairo_restore(cr);
    }
  }

  // Cleaning up
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

/**
 * Move handler_move to new_pos, storing the value in handles,
 * while keeping new_pos within a valid range
 * and preserving the ratio between the three handles.
 *
 * @param self Pointer to this module to be able to access gui_data
 * @param handle_move Handle to move
 * @param new_pow New position (0..1)
 * @param levels Pointer to dt_iop_levels_params->levels.
 * @param drag_start_percentage Ratio between handle 1, 2 and 3.
 *
 * @return TRUE if the marker were given a new position. FALSE otherwise.
 */
static void dt_iop_levels_move_handle(dt_iop_module_t *self, int handle_move, float new_pos, float *levels,
                                      float drag_start_percentage)
{
  dt_iop_levels_gui_data_t *c = (dt_iop_levels_gui_data_t *)self->gui_data;
  float min_x = 0;
  float max_x = 1;

  if((handle_move < 0) || handle_move > 2) return;

  if(levels == NULL) return;

  // Determining the minimum and maximum bounds for the drag handles
  switch(handle_move)
  {
    case 0:
      max_x = fminf(levels[2] - (0.05 / drag_start_percentage), 1);
      max_x = fminf((levels[2] * (1 - drag_start_percentage) - 0.05) / (1 - drag_start_percentage), max_x);
      break;

    case 1:
      min_x = levels[0] + 0.05;
      max_x = levels[2] - 0.05;
      break;

    case 2:
      min_x = fmaxf((0.05 / drag_start_percentage) + levels[0], 0);
      min_x = fmaxf((levels[0] * (1 - drag_start_percentage) + 0.05) / (1 - drag_start_percentage), min_x);
      break;
  }

  levels[handle_move] = fminf(max_x, fmaxf(min_x, new_pos));

  if(handle_move != 1) levels[1] = levels[0] + (drag_start_percentage * (levels[2] - levels[0]));

  if(c->activeToggleButton != NULL) gtk_toggle_button_set_active(c->activeToggleButton, FALSE);
  c->last_picked_color = -1;
}

static gboolean dt_iop_levels_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_levels_gui_data_t *c = (dt_iop_levels_gui_data_t *)self->gui_data;
  dt_iop_levels_params_t *p = (dt_iop_levels_params_t *)self->params;
  const int inset = DT_GUI_CURVE_EDITOR_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int height = allocation.height - 2 * inset, width = allocation.width - 2 * inset;
  if(!c->dragging)
  {
    c->mouse_x = CLAMP(event->x - inset, 0, width);
    c->drag_start_percentage = (p->levels[1] - p->levels[0]) / (p->levels[2] - p->levels[0]);
  }
  c->mouse_y = CLAMP(event->y - inset, 0, height);

  if(c->dragging)
  {
    if(c->handle_move >= 0 && c->handle_move < 3)
    {
      const float mx = (CLAMP(event->x - inset, 0, width)) / (float)width;

      dt_iop_levels_move_handle(self, c->handle_move, mx, p->levels, c->drag_start_percentage);
    }
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
  else
  {
    c->handle_move = 0;
    const float mx = CLAMP(event->x - inset, 0, width) / (float)width;
    float dist = fabsf(p->levels[0] - mx);
    for(int k = 1; k < 3; k++)
    {
      float d2 = fabsf(p->levels[k] - mx);
      if(d2 < dist)
      {
        c->handle_move = k;
        dist = d2;
      }
    }
  }
  gtk_widget_queue_draw(widget);

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

static gboolean dt_iop_levels_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  // set active point
  if(event->button == 1)
  {
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;

    if(event->type == GDK_2BUTTON_PRESS)
    {
      // Reset
      dt_iop_levels_gui_data_t *c = (dt_iop_levels_gui_data_t *)self->gui_data;
      memcpy(self->params, self->default_params, self->params_size);

      // Needed in case the user scrolls or drags immediately after a reset,
      // as drag_start_percentage is only updated when the mouse is moved.
      c->drag_start_percentage = 0.5;
      dt_dev_add_history_item(darktable.develop, self, TRUE);
      gtk_widget_queue_draw(self->widget);
    }
    else
    {
      dt_iop_levels_gui_data_t *c = (dt_iop_levels_gui_data_t *)self->gui_data;
      c->dragging = 1;
    }
    return TRUE;
  }
  return FALSE;
}

static gboolean dt_iop_levels_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(event->button == 1)
  {
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    dt_iop_levels_gui_data_t *c = (dt_iop_levels_gui_data_t *)self->gui_data;
    c->dragging = 0;
    return TRUE;
  }
  return FALSE;
}

static gboolean dt_iop_levels_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_levels_gui_data_t *c = (dt_iop_levels_gui_data_t *)self->gui_data;
  dt_iop_levels_params_t *p = (dt_iop_levels_params_t *)self->params;

  if(c->dragging)
  {
    return FALSE;
  }

  const float interval = 0.002; // Distance moved for each scroll event
  gdouble delta_y;
  if(dt_gui_get_scroll_deltas(event, NULL, &delta_y))
  {
    float new_position = p->levels[c->handle_move] - interval * delta_y;
    dt_iop_levels_move_handle(self, c->handle_move, new_position, p->levels, c->drag_start_percentage);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    return TRUE;
  }

  return FALSE;
}

static void dt_iop_levels_pick_general_handler(GtkToggleButton *togglebutton, dt_iop_module_t *self,
                                               double xpick, double ypick, dt_iop_levels_pick_t picklevel)
{
  dt_iop_levels_gui_data_t *c = (dt_iop_levels_gui_data_t *)self->gui_data;

  darktable.gui->reset = 1;
  // we do not require the callback if we deactivate it here
  if(c->activeToggleButton != NULL) gtk_toggle_button_set_active(c->activeToggleButton, FALSE);
  darktable.gui->reset = 0;

  gboolean toggle = gtk_toggle_button_get_active(togglebutton);
  if(darktable.gui->reset) return;

  if(TRUE == toggle)
  {
    self->request_color_pick = DT_REQUEST_COLORPICK_MODULE;
    dt_lib_colorpicker_set_point(darktable.lib, xpick, ypick);
    c->activeToggleButton = togglebutton;
    c->current_pick = picklevel;
    dt_dev_reprocess_all(self->dev);
  }
  else
  {
    self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
    c->activeToggleButton = NULL;
    c->current_pick = NONE;
    // gtk_widget_queue_draw(self->widget);
    dt_control_queue_redraw();
  }

  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), TRUE);
  dt_iop_request_focus(self);
}

static void dt_iop_levels_pick_black_callback(GtkToggleButton *togglebutton, dt_iop_module_t *self)
{
  dt_iop_levels_gui_data_t *c = (dt_iop_levels_gui_data_t *)self->gui_data;
  double xpick = c->pick_xy_positions[0][0];
  double ypick = c->pick_xy_positions[0][1];
  dt_iop_levels_pick_general_handler(togglebutton, self, xpick, ypick, BLACK);
}

static void dt_iop_levels_pick_grey_callback(GtkToggleButton *togglebutton, dt_iop_module_t *self)
{
  dt_iop_levels_gui_data_t *c = (dt_iop_levels_gui_data_t *)self->gui_data;
  double xpick = c->pick_xy_positions[1][0];
  double ypick = c->pick_xy_positions[1][1];
  dt_iop_levels_pick_general_handler(togglebutton, self, xpick, ypick, GREY);
}

static void dt_iop_levels_pick_white_callback(GtkToggleButton *togglebutton, dt_iop_module_t *self)
{
  dt_iop_levels_gui_data_t *c = (dt_iop_levels_gui_data_t *)self->gui_data;
  double xpick = c->pick_xy_positions[2][0];
  double ypick = c->pick_xy_positions[2][1];
  dt_iop_levels_pick_general_handler(togglebutton, self, xpick, ypick, WHITE);
}

static void dt_iop_levels_autoadjust_callback(GtkRange *range, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_levels_params_t *p = (dt_iop_levels_params_t *)self->params;
  dt_iop_levels_gui_data_t *c = (dt_iop_levels_gui_data_t *)self->gui_data;

  dt_iop_levels_compute_levels_manual(self->histogram, p->levels);

  if(c->activeToggleButton != NULL) gtk_toggle_button_set_active(c->activeToggleButton, FALSE);
  c->last_picked_color = -1;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void dt_iop_levels_mode_callback(GtkWidget *combo, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;

  if(darktable.gui->reset) return;

  dt_iop_levels_gui_data_t *g = (dt_iop_levels_gui_data_t *)self->gui_data;
  dt_iop_levels_params_t *p = (dt_iop_levels_params_t *)self->params;

  const dt_iop_levels_mode_t new_mode
      = GPOINTER_TO_UINT(g_list_nth_data(g->modes, dt_bauhaus_combobox_get(combo)));

  switch(new_mode)
  {
    case LEVELS_MODE_AUTOMATIC:
      p->mode = LEVELS_MODE_AUTOMATIC;
      gtk_stack_set_visible_child_name(GTK_STACK(g->mode_stack), "automatic");
      break;
    case LEVELS_MODE_MANUAL:
    default:
      p->mode = LEVELS_MODE_MANUAL;
      gtk_stack_set_visible_child_name(GTK_STACK(g->mode_stack), "manual");
      break;
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void dt_iop_levels_percentiles_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;

  dt_iop_levels_gui_data_t *g = (dt_iop_levels_gui_data_t *)self->gui_data;
  dt_iop_levels_params_t *p = (dt_iop_levels_params_t *)self->params;

  p->percentiles[0] = dt_bauhaus_slider_get(g->percentile_black);
  p->percentiles[1] = dt_bauhaus_slider_get(g->percentile_grey);
  p->percentiles[2] = dt_bauhaus_slider_get(g->percentile_white);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
