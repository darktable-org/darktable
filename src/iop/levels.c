/*
    This file is part of darktable,
    Copyright (C) 2011-2024 darktable developers.

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
#include "develop/imageop_gui.h"
#include "develop/openmp_maths.h"
#include "dtgtk/drawingarea.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/accelerators.h"
#include "gui/presets.h"
#include "iop/iop_api.h"
#include "libs/colorpicker.h"

#define DT_GUI_CURVE_EDITOR_INSET DT_PIXEL_APPLY_DPI(5)
// special marker value for uninitialized (and thus invalid) levels.  Use this in preference
// to NAN so that we can enable optimizations from -ffinite-math-only.
#define DT_LEVELS_UNINIT (-FLT_MAX)

DT_MODULE_INTROSPECTION(2, dt_iop_levels_params_t)

static gboolean dt_iop_levels_area_draw(GtkWidget *widget, cairo_t *crf, dt_iop_module_t *self);
static gboolean dt_iop_levels_motion_notify(GtkWidget *widget, GdkEventMotion *event, dt_iop_module_t *self);
static gboolean dt_iop_levels_button_press(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self);
static gboolean dt_iop_levels_button_release(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self);
static gboolean dt_iop_levels_leave_notify(GtkWidget *widget, GdkEventCrossing *event, dt_iop_module_t *self);
static gboolean dt_iop_levels_scroll(GtkWidget *widget, GdkEventScroll *event, dt_iop_module_t *self);
static void dt_iop_levels_autoadjust_callback(GtkRange *range, dt_iop_module_t *self);
//static void dt_iop_levels_mode_callback(GtkWidget *combo, gpointer user_data);
//static void dt_iop_levels_percentiles_callback(GtkWidget *slider, gpointer user_data);

typedef enum dt_iop_levels_mode_t
{
  LEVELS_MODE_MANUAL,   // $DESCRIPTION: "manual"
  LEVELS_MODE_AUTOMATIC // $DESCRIPTION: "automatic"
} dt_iop_levels_mode_t;

typedef struct dt_iop_levels_params_t
{
  dt_iop_levels_mode_t mode; // $DEFAULT: LEVELS_MODE_MANUAL
  float black; // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 0.0
  float gray;  // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 50.0
  float white; // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 100.0
  float levels[3];
} dt_iop_levels_params_t;

typedef struct dt_iop_levels_gui_data_t
{
  GList *modes;
  GtkWidget *mode;
  GtkWidget *mode_stack;
  GtkDrawingArea *area;
  double mouse_x, mouse_y;
  int dragging, handle_move;
  float drag_start_percentage;
  GtkToggleButton *activeToggleButton;
  float last_picked_color;
  GtkWidget *percentile_black;
  GtkWidget *percentile_grey;
  GtkWidget *percentile_white;
  float auto_levels[3];
  dt_hash_t hash;
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


const char *deprecated_msg()
{
  return _("this module is deprecated. please use the RGB levels module instead.");
}

const char *name()
{
  return _("levels");
}

int default_group()
{
  return IOP_GROUP_TONE | IOP_GROUP_GRADING;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_DEPRECATED;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("adjust black, white and mid-gray points"),
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
  typedef struct dt_iop_levels_params_v2_t
  {
    dt_iop_levels_mode_t mode;
    float black;
    float gray;
    float white;
    float levels[3];
  } dt_iop_levels_params_v2_t;

  if(old_version == 1)
  {
    typedef struct dt_iop_levels_params_v1_t
    {
      float levels[3];
      int levels_preset;
    } dt_iop_levels_params_v1_t;

    const dt_iop_levels_params_v1_t *o = (dt_iop_levels_params_v1_t *)old_params;
    dt_iop_levels_params_v2_t *n = malloc(sizeof(dt_iop_levels_params_v2_t));

    n->mode = LEVELS_MODE_MANUAL;
    n->black = 0.0f;
    n->gray = 50.0f;
    n->white = 100.0f;
    n->levels[0] = o->levels[0];
    n->levels[1] = o->levels[1];
    n->levels[2] = o->levels[2];

    *new_params = n;
    *new_params_size = sizeof(dt_iop_levels_params_v2_t);
    *new_version = 2;
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
  dt_iop_levels_data_t *d = piece->data;

  uint32_t total = piece->histogram_stats.pixels;

  dt_aligned_pixel_t thr;
  for(int k = 0; k < 3; k++)
  {
    thr[k] = (float)total * d->percentiles[k] / 100.0f;
    d->levels[k] = DT_LEVELS_UNINIT;
  }

  if(piece->histogram == NULL) return;

  // find min and max levels
  size_t n = 0;
  for(uint32_t i = 0; i < piece->histogram_stats.bins_count; i++)
  {
    n += piece->histogram[4 * i];

    for(int k = 0; k < 3; k++)
    {
      if(d->levels[k] == DT_LEVELS_UNINIT && (n >= thr[k]))
      {
        d->levels[k] = (float)i / (float)(piece->histogram_stats.bins_count - 1);
      }
    }
  }
  // for numerical reasons sometimes the threshold is sharp but in float and n is size_t.
  // in this case we want to make sure we don't keep the marker that it is uninitialized:
  if(d->levels[2] == DT_LEVELS_UNINIT)
    d->levels[2] = 1.0f;

  // compute middle level from min and max levels
  float center = d->percentiles[1] / 100.0f;
  if(d->levels[0] != DT_LEVELS_UNINIT && d->levels[2] != DT_LEVELS_UNINIT)
    d->levels[1] = (1.0f - center) * d->levels[0] + center * d->levels[2];
}

static void compute_lut(dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_levels_data_t *d = piece->data;

  // Building the lut for values in the [0,1] range
  float delta = (d->levels[2] - d->levels[0]) / 2.0f;
  float mid = d->levels[0] + delta;
  float tmp = (d->levels[1] - mid) / delta;
  d->in_inv_gamma = pow(10, tmp);

  for(unsigned int i = 0; i < 0x10000; i++)
  {
    float percentage = (float)i / (float)0x10000ul;
    d->lut[i] = 100.0f * powf(percentage, d->in_inv_gamma);
  }
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker,
                        dt_dev_pixelpipe_t *pipe)
{
  dt_iop_levels_gui_data_t *g = self->gui_data;
  dt_iop_levels_params_t *p = self->params;

  /* we need to save the last picked color to prevent flickering when
   * changing from one picker to another, as the picked_color value does not
   * update as rapidly */

  float mean_picked_color = *self->picked_color / 100.0;

  if(mean_picked_color != g->last_picked_color)
  {
    dt_aligned_pixel_t previous_color;
    previous_color[0] = p->levels[0];
    previous_color[1] = p->levels[1];
    previous_color[2] = p->levels[2];

    g->last_picked_color = mean_picked_color;

    if(picker == g->blackpick)
    {
      if(mean_picked_color > p->levels[1])
      {
        p->levels[0] = p->levels[1] - FLT_EPSILON;
      }
      else
      {
        p->levels[0] = mean_picked_color;
      }
    }
    else if(picker == g->greypick)
    {
      if(mean_picked_color < p->levels[0] || mean_picked_color > p->levels[2])
      {
        p->levels[1] = p->levels[1];
      }
      else
      {
        p->levels[1] = mean_picked_color;
      }
    }
    else if(picker == g->whitepick)
    {
      if(mean_picked_color < p->levels[1])
      {
        p->levels[2] = p->levels[1] + FLT_EPSILON;
      }
      else
      {
        p->levels[2] = mean_picked_color;
      }
    }

    if(previous_color[0] != p->levels[0]
       || previous_color[1] != p->levels[1]
       || previous_color[2] != p->levels[2])
    {
      dt_dev_add_history_item(darktable.develop, self, TRUE);
    }
  }
}

/*
 * WARNING: unlike commit_params, which is thread safe wrt gui thread and
 * pipes, this function lives in the pipeline thread, and NOT thread safe!
 */
static void commit_params_late(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_levels_data_t *d = piece->data;
  dt_iop_levels_gui_data_t *g = self->gui_data;

  if(d->mode == LEVELS_MODE_AUTOMATIC)
  {
    if(g && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL))
    {
      dt_iop_gui_enter_critical_section(self);
      const dt_hash_t hash = g->hash;
      dt_iop_gui_leave_critical_section(self);

      // note that the case 'hash == 0' on first invocation in a session implies that d->levels[]
      // contains NANs which initiates special handling below to avoid inconsistent results. in all
      // other cases we make sure that the preview pipe has left us with proper readings for
      // g->auto_levels[]. if data are not yet there we need to wait (with timeout).
      if(hash != 0 && !dt_dev_sync_pixelpipe_hash(self->dev, piece->pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, &self->gui_lock, &g->hash))
        dt_control_log(_("inconsistent output"));

      dt_iop_gui_enter_critical_section(self);
      d->levels[0] = g->auto_levels[0];
      d->levels[1] = g->auto_levels[1];
      d->levels[2] = g->auto_levels[2];
      dt_iop_gui_leave_critical_section(self);

      compute_lut(piece);
    }

    if((piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW)
       || d->levels[0] == DT_LEVELS_UNINIT || d->levels[1] == DT_LEVELS_UNINIT
       || d->levels[2] == DT_LEVELS_UNINIT)
    {
      dt_iop_levels_compute_levels_automatic(piece);
      compute_lut(piece);
    }

    if(g && (piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW) && d->mode == LEVELS_MODE_AUTOMATIC)
    {
      dt_hash_t hash = dt_dev_hash_plus(self->dev, piece->pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL);
      dt_iop_gui_enter_critical_section(self);
      g->auto_levels[0] = d->levels[0];
      g->auto_levels[1] = d->levels[1];
      g->auto_levels[2] = d->levels[2];
      g->hash = hash;
      dt_iop_gui_leave_critical_section(self);
    }
  }
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                        ivoid, ovoid, roi_in, roi_out))
    return;
  const dt_iop_levels_data_t *const d = piece->data;

  if(d->mode == LEVELS_MODE_AUTOMATIC)
  {
    commit_params_late(self, piece);
  }

  const float *const restrict in = (float*)ivoid;
  float *const restrict out = (float*)ovoid;
  const size_t npixels = (size_t)roi_out->width * roi_out->height;
  const float level_black = d->levels[0];
  const float level_range = d->levels[2] - d->levels[0];
  const float inv_gamma = d->in_inv_gamma;
  const float *lut = d->lut;

  DT_OMP_FOR()
  for(int i = 0; i < 4 * npixels; i += 4)
  {
    const float L_in = in[i] / 100.0f;
    float L_out;
    if(L_in <= level_black)
    {
      // Anything below the lower threshold just clips to zero
      L_out = 0.0f;
    }
    else
    {
      const float percentage = (L_in - level_black) / level_range;
      // Within the expected input range we can use the lookup table, else we need to compute from scratch
      L_out = percentage < 1.0f ? lut[(int)(percentage * 0x10000ul)] : 100.0f * powf(percentage, inv_gamma);
    }

    // Preserving contrast
    const float denom = (in[i] > 0.01f) ? in[i] : 0.01f;
    out[i] = L_out;
    out[i+1] = in[i+1] * L_out / denom;
    out[i+2] = in[i+2] * L_out / denom;
  }
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_levels_data_t *d = piece->data;
  dt_iop_levels_global_data_t *gd = self->global_data;

  if(d->mode == LEVELS_MODE_AUTOMATIC)
  {
    commit_params_late(self, piece);
  }

  cl_mem dev_lut = NULL;
  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  const int devid = piece->pipe->devid;

  const int width = roi_out->width;
  const int height = roi_out->height;

  dev_lut = dt_opencl_copy_host_to_device(devid, d->lut, 256, 256, sizeof(float));
  if(dev_lut == NULL) goto error;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_levels, width, height,
    CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(dev_lut), CLARG(d->levels[0]),
    CLARG(d->levels[2]), CLARG(d->in_inv_gamma));

error:
  dt_opencl_release_mem_object(dev_lut);
  return err;
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
  dt_iop_levels_data_t *d = piece->data;
  dt_iop_levels_params_t *p = (dt_iop_levels_params_t *)p1;

  if(pipe->type & DT_DEV_PIXELPIPE_PREVIEW)
    piece->request_histogram |= DT_REQUEST_ON;
  else
    piece->request_histogram &= ~DT_REQUEST_ON;

  piece->request_histogram |= DT_REQUEST_ONLY_IN_GUI;

  piece->histogram_params.bins_count = 256;

  if(p->mode == LEVELS_MODE_AUTOMATIC)
  {
    d->mode = LEVELS_MODE_AUTOMATIC;

    piece->request_histogram |= DT_REQUEST_ON;
    self->request_histogram &= ~DT_REQUEST_ON;

    if(!self->dev->gui_attached) piece->request_histogram &= ~DT_REQUEST_ONLY_IN_GUI;

    piece->histogram_params.bins_count = 16384;

    /*
     * in principle, we do not need/want histogram in FULL pipe
     * because we will use histogram from preview pipe there,
     * but it might happen that for some reasons we do not have
     * histogram of preview pipe yet - e.g. on first pipe run
     * (just after setting mode to automatic)
     */

    d->percentiles[0] = p->black;
    d->percentiles[1] = p->gray;
    d->percentiles[2] = p->white;

    d->levels[0] = DT_LEVELS_UNINIT;
    d->levels[1] = DT_LEVELS_UNINIT;
    d->levels[2] = DT_LEVELS_UNINIT;

    // commit_params_late() will compute LUT later
  }
  else
  {
    d->mode = LEVELS_MODE_MANUAL;

    self->request_histogram |= DT_REQUEST_ON;

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

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_levels_gui_data_t *g = self->gui_data;
  dt_iop_levels_params_t *p = self->params;

  if(w == g->mode)
  {
    if(p->mode == LEVELS_MODE_AUTOMATIC)
      gtk_stack_set_visible_child_name(GTK_STACK(g->mode_stack), "automatic");
    else
      gtk_stack_set_visible_child_name(GTK_STACK(g->mode_stack), "manual");
  }
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_levels_gui_data_t *g = self->gui_data;
  dt_iop_levels_params_t *p = self->params;

  dt_bauhaus_combobox_set(g->mode, p->mode);

  gui_changed(self, g->mode, 0);

  dt_iop_gui_enter_critical_section(self);
  g->auto_levels[0] = DT_LEVELS_UNINIT;
  g->auto_levels[1] = DT_LEVELS_UNINIT;
  g->auto_levels[2] = DT_LEVELS_UNINIT;
  g->hash = 0;
  dt_iop_gui_leave_critical_section(self);

  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

void init(dt_iop_module_t *self)
{
  dt_iop_default_init(self);

  self->request_histogram |= DT_REQUEST_ON;

  dt_iop_levels_params_t *d = self->default_params;

  d->levels[0] = 0.0f;
  d->levels[1] = 0.5f;
  d->levels[2] = 1.0f;
}

void init_global(dt_iop_module_so_t *self)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_levels_global_data_t *gd = malloc(sizeof(dt_iop_levels_global_data_t));
  self->data = gd;
  gd->kernel_levels = dt_opencl_create_kernel(program, "levels");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_levels_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_levels);
  free(self->data);
  self->data = NULL;
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_levels_gui_data_t *g = IOP_GUI_ALLOC(levels);

  dt_iop_gui_enter_critical_section(self);
  g->auto_levels[0] = DT_LEVELS_UNINIT;
  g->auto_levels[1] = DT_LEVELS_UNINIT;
  g->auto_levels[2] = DT_LEVELS_UNINIT;
  g->hash = 0;
  dt_iop_gui_leave_critical_section(self);

  g->modes = NULL;

  g->mouse_x = g->mouse_y = -1.0;
  g->dragging = 0;
  g->activeToggleButton = NULL;
  g->last_picked_color = -1;

  g->mode_stack = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(g->mode_stack),FALSE);

  g->area = GTK_DRAWING_AREA(dt_ui_resize_wrap(NULL,
                                               0,
                                               "plugins/darkroom/levels/graphheight"));
  GtkWidget *vbox_manual = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  gtk_box_pack_start(GTK_BOX(vbox_manual), GTK_WIDGET(g->area), TRUE, TRUE, 0);

  gtk_widget_set_tooltip_text(GTK_WIDGET(g->area),_("drag handles to set black, gray, and white points. "
                                                    "operates on L channel."));
  dt_action_define_iop(self, NULL, N_("levels"), GTK_WIDGET(g->area), NULL);

  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(dt_iop_levels_area_draw), self);
  g_signal_connect(G_OBJECT(g->area), "button-press-event", G_CALLBACK(dt_iop_levels_button_press), self);
  g_signal_connect(G_OBJECT(g->area), "button-release-event", G_CALLBACK(dt_iop_levels_button_release), self);
  g_signal_connect(G_OBJECT(g->area), "motion-notify-event", G_CALLBACK(dt_iop_levels_motion_notify), self);
  g_signal_connect(G_OBJECT(g->area), "leave-notify-event", G_CALLBACK(dt_iop_levels_leave_notify), self);
  g_signal_connect(G_OBJECT(g->area), "scroll-event", G_CALLBACK(dt_iop_levels_scroll), self);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  GtkWidget *autobutton = gtk_button_new_with_label(_("auto"));
  gtk_widget_set_tooltip_text(autobutton, _("apply auto levels"));
  g_signal_connect(G_OBJECT(autobutton), "clicked", G_CALLBACK(dt_iop_levels_autoadjust_callback), self);

  g->blackpick = dt_color_picker_new(self, DT_COLOR_PICKER_POINT, NULL);
  gtk_widget_set_tooltip_text(g->blackpick, _("pick black point from image"));
  gtk_widget_set_name(GTK_WIDGET(g->blackpick), "picker-black");

  g->greypick = dt_color_picker_new(self, DT_COLOR_PICKER_POINT, NULL);
  gtk_widget_set_tooltip_text(g->greypick, _("pick medium gray point from image"));
  gtk_widget_set_name(GTK_WIDGET(g->greypick), "picker-grey");

  g->whitepick = dt_color_picker_new(self, DT_COLOR_PICKER_POINT, NULL);
  gtk_widget_set_tooltip_text(g->whitepick, _("pick white point from image"));
  gtk_widget_set_name(GTK_WIDGET(g->whitepick), "picker-white");

  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(autobutton  ), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(g->blackpick), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(g->greypick ), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(g->whitepick), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox_manual), box, TRUE, TRUE, 0);

  gtk_stack_add_named(GTK_STACK(g->mode_stack), vbox_manual, "manual");

  GtkWidget *vbox_automatic = self->widget = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));

  g->percentile_black = dt_bauhaus_slider_from_params(self, N_("black"));
  gtk_widget_set_tooltip_text(g->percentile_black, _("black percentile"));
  dt_bauhaus_slider_set_format(g->percentile_black, "%");

  g->percentile_grey = dt_bauhaus_slider_from_params(self, N_("gray"));
  gtk_widget_set_tooltip_text(g->percentile_grey, _("gray percentile"));
  dt_bauhaus_slider_set_format(g->percentile_grey, "%");

  g->percentile_white = dt_bauhaus_slider_from_params(self, N_("white"));
  gtk_widget_set_tooltip_text(g->percentile_white, _("white percentile"));
  dt_bauhaus_slider_set_format(g->percentile_white, "%");

  gtk_stack_add_named(GTK_STACK(g->mode_stack), vbox_automatic, "automatic");

  // start building top level widget
  self->widget = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 5));

  g->mode = dt_bauhaus_combobox_from_params(self, N_("mode"));

  gtk_box_pack_start(GTK_BOX(self->widget), g->mode_stack, TRUE, TRUE, 0);
}

void gui_cleanup(dt_iop_module_t *self)
{
  dt_iop_levels_gui_data_t *g = self->gui_data;
  g_list_free(g->modes);

  IOP_GUI_FREE;
}

static gboolean dt_iop_levels_leave_notify(GtkWidget *widget, GdkEventCrossing *event, dt_iop_module_t *self)
{
  dt_iop_levels_gui_data_t *g = self->gui_data;
  g->mouse_x = g->mouse_y = -1.0;
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean dt_iop_levels_area_draw(GtkWidget *widget, cairo_t *crf, dt_iop_module_t *self)
{
  dt_iop_levels_gui_data_t *g = self->gui_data;
  dt_iop_levels_params_t *p = self->params;

  const int inset = DT_GUI_CURVE_EDITOR_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(GTK_WIDGET(g->area), &allocation);
  int width = allocation.width, height = allocation.height - DT_RESIZE_HANDLE_SIZE;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

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
  dt_draw_vertical_lines(cr, 4, 0, 0, width, height);

  // Drawing the vertical line indicators
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));

  for(int k = 0; k < 3; k++)
  {
    if(k == g->handle_move && g->mouse_x > 0)
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
    if(g->handle_move == k && g->mouse_x > 0)
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
    const gboolean is_linear = darktable.lib->proxy.histogram.is_linear;
    float hist_max = is_linear ? self->histogram_max[0] : logf(1.0 + self->histogram_max[0]);
    if(hist && hist_max > 0.0f)
    {
      cairo_save(cr);
      cairo_scale(cr, width / 255.0, -(height - DT_PIXEL_APPLY_DPI(5)) / hist_max);
      cairo_set_source_rgba(cr, .2, .2, .2, 0.5);
      dt_draw_histogram_8(cr, hist, 4, 0, is_linear);
      cairo_restore(cr);
    }
  }

  // Cleaning up
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return FALSE;
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
  dt_iop_levels_gui_data_t *g = self->gui_data;
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

  if(g->activeToggleButton != NULL) gtk_toggle_button_set_active(g->activeToggleButton, FALSE);
  g->last_picked_color = -1;
}

static gboolean dt_iop_levels_motion_notify(GtkWidget *widget, GdkEventMotion *event, dt_iop_module_t *self)
{
  dt_iop_levels_gui_data_t *g = self->gui_data;
  dt_iop_levels_params_t *p = self->params;
  const int inset = DT_GUI_CURVE_EDITOR_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int height = allocation.height - 2 * inset - DT_RESIZE_HANDLE_SIZE, width = allocation.width - 2 * inset;
  if(!g->dragging)
  {
    g->mouse_x = CLAMP(event->x - inset, 0, width);
    g->drag_start_percentage = (p->levels[1] - p->levels[0]) / (p->levels[2] - p->levels[0]);
  }
  g->mouse_y = CLAMP(event->y - inset, 0, height);

  if(g->dragging)
  {
    if(g->handle_move >= 0 && g->handle_move < 3)
    {
      const float mx = (CLAMP(event->x - inset, 0, width)) / (float)width;

      dt_iop_levels_move_handle(self, g->handle_move, mx, p->levels, g->drag_start_percentage);
    }
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
  else
  {
    g->handle_move = 0;
    const float mx = CLAMP(event->x - inset, 0, width) / (float)width;
    float dist = fabsf(p->levels[0] - mx);
    for(int k = 1; k < 3; k++)
    {
      float d2 = fabsf(p->levels[k] - mx);
      if(d2 < dist)
      {
        g->handle_move = k;
        dist = d2;
      }
    }
  }
  gtk_widget_queue_draw(widget);

  return TRUE;
}

static gboolean dt_iop_levels_button_press(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  // set active point
  if(event->button == 1)
  {
    if(darktable.develop->gui_module != self) dt_iop_request_focus(self);

    if(event->type == GDK_2BUTTON_PRESS)
    {
      // Reset
      dt_iop_levels_gui_data_t *g = self->gui_data;
      memcpy(self->params, self->default_params, self->params_size);

      // Needed in case the user scrolls or drags immediately after a reset,
      // as drag_start_percentage is only updated when the mouse is moved.
      g->drag_start_percentage = 0.5;
      dt_dev_add_history_item(darktable.develop, self, TRUE);
      gtk_widget_queue_draw(GTK_WIDGET(g->area));
    }
    else
    {
      dt_iop_levels_gui_data_t *g = self->gui_data;
      g->dragging = 1;
    }
    return TRUE;
  }
  return FALSE;
}

static gboolean dt_iop_levels_button_release(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  if(event->button == 1)
  {
    dt_iop_levels_gui_data_t *g = self->gui_data;
    g->dragging = 0;
    return TRUE;
  }
  return FALSE;
}

static gboolean dt_iop_levels_scroll(GtkWidget *widget, GdkEventScroll *event, dt_iop_module_t *self)
{
  dt_iop_levels_gui_data_t *g = self->gui_data;
  dt_iop_levels_params_t *p = self->params;

  if(dt_gui_ignore_scroll(event)) return FALSE;

  dt_iop_color_picker_reset(self, TRUE);

  if(g->dragging)
  {
    return FALSE;
  }

  if(darktable.develop->gui_module != self) dt_iop_request_focus(self);

  const float interval = 0.002 * dt_accel_get_speed_multiplier(widget, event->state); // Distance moved for each scroll event
  int delta_y;
  if(dt_gui_get_scroll_unit_delta(event, &delta_y))
  {
    float new_position = p->levels[g->handle_move] - interval * delta_y;
    dt_iop_levels_move_handle(self, g->handle_move, new_position, p->levels, g->drag_start_percentage);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    return TRUE;
  }

  return TRUE; // Ensure that scrolling the widget cannot move side panel
}

static void dt_iop_levels_autoadjust_callback(GtkRange *range, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_levels_params_t *p = self->params;
  dt_iop_levels_gui_data_t *g = self->gui_data;

  dt_iop_color_picker_reset(self, TRUE);

  dt_iop_levels_compute_levels_manual(self->histogram, p->levels);

  if(g->activeToggleButton != NULL) gtk_toggle_button_set_active(g->activeToggleButton, FALSE);
  g->last_picked_color = -1;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
