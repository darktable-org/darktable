/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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
#include "develop/imageop.h"
#include "develop/develop.h"
#include "control/control.h"
#include "gui/draw.h"
#include "gui/presets.h"
#include "gui/gtk.h"
#include "dtgtk/button.h"
#include "common/colorspaces.h"
#include "common/opencl.h"
#include "libs/colorpicker.h"

#define DT_GUI_CURVE_EDITOR_INSET 5
#define DT_GUI_CURVE_INFL .3f

DT_MODULE(1)

static gboolean dt_iop_levels_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data);
static gboolean dt_iop_levels_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);
static gboolean dt_iop_levels_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean dt_iop_levels_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean dt_iop_levels_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);
static gboolean dt_iop_levels_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data);
static void dt_iop_levels_pick_black_callback(GtkToggleButton *togglebutton, dt_iop_module_t *self);
static void dt_iop_levels_pick_grey_callback(GtkToggleButton *togglebutton, dt_iop_module_t *self);
static void dt_iop_levels_pick_white_callback(GtkToggleButton *togglebutton, dt_iop_module_t *self);
static void dt_iop_levels_autoadjust_callback(GtkRange *range, dt_iop_module_t *self);


typedef struct dt_iop_levels_params_t
{
  float levels[3];
  int levels_preset;
}
dt_iop_levels_params_t;

typedef enum dt_iop_levels_pick_t
{
  NONE,
  BLACK,
  GREY,
  WHITE
}
dt_iop_levels_pick_t;

typedef struct dt_iop_levels_gui_data_t
{
  GtkHBox *hbox;
  GtkDrawingArea *area;
  GtkLabel *label;
  double mouse_x, mouse_y;
  int dragging, handle_move;
  float drag_start_percentage;
  dt_iop_levels_pick_t current_pick;
  GtkToggleButton *activeToggleButton;
  float last_picked_color;
  double pick_xy_positions[3][2];
}
dt_iop_levels_gui_data_t;

typedef struct dt_iop_levels_data_t
{
  float in_low;
  float in_high;
  float in_inv_gamma;
  float lut[0x10000];
}
dt_iop_levels_data_t;

typedef struct dt_iop_levels_global_data_t
{
  int kernel_levels;
}
dt_iop_levels_global_data_t;


const char *name()
{
  return _("levels");
}


int
groups ()
{
  return IOP_GROUP_TONE;
}

int
flags ()
{
  return IOP_FLAGS_SUPPORTS_BLENDING;
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  const int ch = piece->colors;
  dt_iop_levels_data_t *d = (dt_iop_levels_data_t*)(piece->data);
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_out, i, o, d) schedule(static)
#endif
  for(int k=0; k<roi_out->height; k++)
  {
    float *in = ((float *)i) + (size_t)k*ch*roi_out->width;
    float *out = ((float *)o) + (size_t)k*ch*roi_out->width;
    for (int j=0; j<roi_out->width; j++,in+=ch,out+=ch)
    {
      float L_in = in[0] / 100.0;

      if(L_in <= d->in_low)
      {
        // Anything below the lower threshold just clips to zero
        out[0] = 0;
      }
      else if(L_in >= d->in_high)
      {
        float percentage = (L_in - d->in_low) / (d->in_high - d->in_low);
        out[0] = 100.0 * pow(percentage, d->in_inv_gamma);
      }
      else
      {
        // Within the expected input range we can use the lookup table
        float percentage = (L_in - d->in_low) / (d->in_high - d->in_low);
        //out[0] = 100.0 * pow(percentage, d->in_inv_gamma);
        out[0] = d->lut[CLAMP((int)(percentage * 0xfffful), 0, 0xffff)];
      }

      // Preserving contrast
      if(in[0] > 0.01f)
      {
        out[1] = in[1] * out[0]/in[0];
        out[2] = in[2] * out[0]/in[0];
      }
      else
      {
        out[1] = in[1] * out[0]/0.01f;
        out[2] = in[2] * out[0]/0.01f;
      }

      out[3] = in[3];

    }
  }
}

#ifdef HAVE_OPENCL
int
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_levels_data_t *d = (dt_iop_levels_data_t *)piece->data;
  dt_iop_levels_global_data_t *gd = (dt_iop_levels_global_data_t *)self->data;

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
  dt_opencl_set_kernel_arg(devid, gd->kernel_levels, 5, sizeof(float), &d->in_low);
  dt_opencl_set_kernel_arg(devid, gd->kernel_levels, 6, sizeof(float), &d->in_high);
  dt_opencl_set_kernel_arg(devid, gd->kernel_levels, 7, sizeof(float), &d->in_inv_gamma);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_levels, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_lut);
  return TRUE;

error:
  if (dev_lut != NULL) dt_opencl_release_mem_object(dev_lut);
  dt_print(DT_DEBUG_OPENCL, "[opencl_levels] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif



//void init_presets (dt_iop_module_so_t *self)
//{
//  dt_iop_levels_params_t p;
//  p.levels_preset = 0;
//
//  p.levels[0] = 0;
//  p.levels[1] = 0.5;
//  p.levels[2] = 1;
//  dt_gui_presets_add_generic(_("unmodified"), self->op, self->version(), &p, sizeof(p), 1);
//}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1,
                    dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_levels_data_t *d = (dt_iop_levels_data_t*)(piece->data);
  dt_iop_levels_params_t *p = (dt_iop_levels_params_t*)p1;

  // Building the lut for values in the [0,1] range
  d->in_low = p->levels[0];
  d->in_high = p->levels[2];
  float delta = (p->levels[2] - p->levels[0]) / 2.;
  float mid = p->levels[0] + delta;
  float tmp = (p->levels[1] - mid) / delta;
  d->in_inv_gamma = pow(10, tmp);

  for(unsigned int i = 0; i < 0x10000; i++)
  {
    float percentage = (float)i / (float)0xfffful;
    d->lut[i] = 100.0 * pow(percentage, d->in_inv_gamma);
  }
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                dt_dev_pixelpipe_iop_t *piece)
{
  // create part of the gegl pipeline
  dt_iop_levels_data_t *d =
    (dt_iop_levels_data_t *)malloc(sizeof(dt_iop_levels_data_t));
  piece->data = (void *)d;
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // clean up everything again.
  free(piece->data);
}

void gui_update(struct dt_iop_module_t *self)
{
  // nothing to do, gui curve is read directly from params during expose event.
  gtk_widget_queue_draw(self->widget);
}

void reload_defaults(dt_iop_module_t *self)
{
  memcpy(self->params, self->default_params, sizeof(dt_iop_levels_params_t));
  self->default_enabled = 0;
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_levels_params_t));
  module->default_params = malloc(sizeof(dt_iop_levels_params_t));
  module->default_enabled = 0;
  module->request_histogram = 1;
  module->priority = 649; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_levels_params_t);
  module->gui_data = NULL;
  dt_iop_levels_params_t tmp = (dt_iop_levels_params_t)
  {
    {0, 0.5, 1},
    0
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_levels_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_levels_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_levels_global_data_t *gd = (dt_iop_levels_global_data_t *)malloc(sizeof(dt_iop_levels_global_data_t));
  module->data = gd;
  gd->kernel_levels = dt_opencl_create_kernel(program, "levels");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_levels_global_data_t *gd = (dt_iop_levels_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_levels);
  free(module->data);
  module->data = NULL;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_levels_gui_data_t));
  dt_iop_levels_gui_data_t *c = (dt_iop_levels_gui_data_t *)self->gui_data;

  c->mouse_x = c->mouse_y = -1.0;
  c->dragging = 0;
  c->activeToggleButton = NULL;
  c->current_pick = NONE;
  c->last_picked_color = -1;
  for (int i=0; i<3; i++)
    for (int j=0; j<2; j++) c->pick_xy_positions[i][j] = -1;
  self->widget = GTK_WIDGET(gtk_vbox_new(FALSE, 5));
  c->area = GTK_DRAWING_AREA(gtk_drawing_area_new());
  GtkWidget *asp = gtk_aspect_frame_new(NULL, 0.5, 0.5, 1.0, TRUE);
  gtk_box_pack_start(GTK_BOX(self->widget), asp, TRUE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(asp), GTK_WIDGET(c->area));
  gtk_widget_set_size_request(GTK_WIDGET(c->area), 258, 150);
  g_object_set (GTK_OBJECT(c->area), "tooltip-text", _("drag handles to set black, grey, and white points.  operates on L channel."), (char *)NULL);

  gtk_widget_add_events(GTK_WIDGET(c->area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect (G_OBJECT (c->area), "expose-event",
                    G_CALLBACK (dt_iop_levels_expose), self);
  g_signal_connect (G_OBJECT (c->area), "button-press-event",
                    G_CALLBACK (dt_iop_levels_button_press), self);
  g_signal_connect (G_OBJECT (c->area), "button-release-event",
                    G_CALLBACK (dt_iop_levels_button_release), self);
  g_signal_connect (G_OBJECT (c->area), "motion-notify-event",
                    G_CALLBACK (dt_iop_levels_motion_notify), self);
  g_signal_connect (G_OBJECT (c->area), "leave-notify-event",
                    G_CALLBACK (dt_iop_levels_leave_notify), self);
  g_signal_connect (G_OBJECT (c->area), "scroll-event",
                    G_CALLBACK (dt_iop_levels_scroll), self);

  GtkWidget *autobutton = dtgtk_button_new_with_label(_("auto"), NULL, CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER);
  g_object_set(G_OBJECT(autobutton), "tooltip-text", _("apply auto levels"), (char *)NULL);
  gtk_widget_set_size_request(autobutton, 70, 24);

  GtkWidget *blackpick = dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT);
  g_object_set(G_OBJECT(blackpick), "tooltip-text", _("pick blackpoint from image"), (char *)NULL);
  gtk_widget_set_size_request(blackpick, 24, 24);

  GtkWidget *greypick = dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT);
  g_object_set(G_OBJECT(greypick), "tooltip-text", _("pick medium greypoint from image"), (char *)NULL);
  gtk_widget_set_size_request(greypick, 24, 24);

  GtkWidget *whitepick = dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT);
  g_object_set(G_OBJECT(whitepick), "tooltip-text", _("pick whitepoint from image"), (char *)NULL);
  gtk_widget_set_size_request(whitepick, 24, 24);

  GdkColor col;
  col.red = col.green = col.blue = 0;
  gtk_widget_modify_fg(GTK_WIDGET(blackpick), GTK_STATE_NORMAL, &col);
  gtk_widget_modify_fg(GTK_WIDGET(blackpick), GTK_STATE_SELECTED, &col);
  col.red = col.green = col.blue = 32767;
  gtk_widget_modify_fg(GTK_WIDGET(greypick), GTK_STATE_NORMAL, &col);
  gtk_widget_modify_fg(GTK_WIDGET(greypick), GTK_STATE_SELECTED, &col);
  col.red = col.green = col.blue = 65535;
  gtk_widget_modify_fg(GTK_WIDGET(whitepick), GTK_STATE_NORMAL, &col);
  gtk_widget_modify_fg(GTK_WIDGET(whitepick), GTK_STATE_SELECTED, &col);
  col.red = col.green = col.blue = 4096;
  gtk_widget_modify_bg(GTK_WIDGET(blackpick), GTK_STATE_ACTIVE, &col);
  gtk_widget_modify_bg(GTK_WIDGET(greypick), GTK_STATE_ACTIVE, &col);
  gtk_widget_modify_bg(GTK_WIDGET(whitepick), GTK_STATE_ACTIVE, &col);

  GtkWidget *box = gtk_hbox_new(TRUE,0);
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(autobutton), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(blackpick), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(greypick), FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(box), GTK_WIDGET(whitepick), FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), box, TRUE, TRUE, 0);

  g_signal_connect (G_OBJECT(autobutton), "clicked",
                    G_CALLBACK(dt_iop_levels_autoadjust_callback), (gpointer)self);
  g_signal_connect (G_OBJECT(blackpick), "toggled",
                    G_CALLBACK (dt_iop_levels_pick_black_callback), self);
  g_signal_connect (G_OBJECT(greypick), "toggled",
                    G_CALLBACK (dt_iop_levels_pick_grey_callback), self);
  g_signal_connect (G_OBJECT(whitepick), "toggled",
                    G_CALLBACK (dt_iop_levels_pick_white_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
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

static gboolean dt_iop_levels_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_levels_gui_data_t *c = (dt_iop_levels_gui_data_t *)self->gui_data;
  dt_iop_levels_params_t *p = (dt_iop_levels_params_t *)self->params;
  dt_develop_t *dev = darktable.develop;
  const int inset = DT_GUI_CURVE_EDITOR_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  float mean_picked_color = *self->picked_color / 100.0;

  /* we need to save the last picked color to prevent flickering when
   * changing from one picker to another, as the picked_color value does not
   * update as rapidly */
  if(self->request_color_pick &&
      self->color_picker_point[0] >= 0.0f && self->color_picker_point[1] >= 0.0f &&
      self->picked_color_max[0] >= 0.0f &&
      mean_picked_color != c->last_picked_color)
  {
    float previous_color[3];
    previous_color[0] = p->levels[0];
    previous_color[1] = p->levels[1];
    previous_color[2] = p->levels[2];

    c->last_picked_color = mean_picked_color;

    if (BLACK == c->current_pick)
    {
      if (mean_picked_color > p->levels[1])
      {
        p->levels[0] = p->levels[1]-FLT_EPSILON;
      }
      else
      {
        p->levels[0] = mean_picked_color;
      }
      c->pick_xy_positions[0][0] = self->color_picker_point[0];
      c->pick_xy_positions[0][1] = self->color_picker_point[1];
    }
    else if (GREY == c->current_pick)
    {
      if (mean_picked_color < p->levels[0] || mean_picked_color > p->levels[2])
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
    else if (WHITE == c->current_pick)
    {
      if (mean_picked_color < p->levels[1])
      {
        p->levels[2] = p->levels[1]+FLT_EPSILON;
      }
      else
      {
        p->levels[2] = mean_picked_color;
      }
      c->pick_xy_positions[2][0] = self->color_picker_point[0];
      c->pick_xy_positions[2][1] = self->color_picker_point[1];
    }

    if (   previous_color[0] != p->levels[0]
           || previous_color[1] != p->levels[1]
           || previous_color[2] != p->levels[2] )
    {
      dt_dev_add_history_item(darktable.develop, self, TRUE);
    }
  }

  // clear bg
  cairo_set_source_rgb (cr, .2, .2, .2);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2*inset;
  height -= 2*inset;

  cairo_set_line_width(cr, 1.0);
  cairo_set_source_rgb (cr, .1, .1, .1);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_stroke(cr);

  cairo_set_source_rgb (cr, .3, .3, .3);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  // draw grid
  cairo_set_line_width(cr, .4);
  cairo_set_source_rgb (cr, .1, .1, .1);
  if(dev->histogram_type == DT_DEV_HISTOGRAM_WAVEFORM)
    dt_draw_waveform_lines(cr, 0, 0, width, height);
  else
    dt_draw_vertical_lines(cr, 4, 0, 0, width, height);

  // Drawing the vertical line indicators
  cairo_set_line_width(cr, 2.);

  for(int k = 0; k < 3; k++)
  {
    if(k == c->handle_move && c->mouse_x > 0)
      cairo_set_source_rgb(cr, 1, 1, 1);
    else
      cairo_set_source_rgb(cr, .7, .7, .7);

    cairo_move_to(cr, width*p->levels[k], height);
    cairo_rel_line_to(cr, 0, -height);
    cairo_stroke(cr);
  }

  // draw x positions
  cairo_set_line_width(cr, 1.);
  const float arrw = 7.0f;
  for(int k=0; k<3; k++)
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

    cairo_move_to(cr, width*p->levels[k], height+inset-1);
    cairo_rel_line_to(cr, -arrw*.5f, 0);
    cairo_rel_line_to(cr, arrw*.5f, -arrw);
    cairo_rel_line_to(cr, arrw*.5f, arrw);
    cairo_close_path(cr);
    if(c->handle_move == k && c->mouse_x > 0)
      cairo_fill(cr);
    else
      cairo_stroke(cr);
  }

  cairo_translate(cr, 0, height);

  // draw lum histogram in background
  // only if the module is enabled
  if (self->enabled)
  {
    float *hist, hist_max;
    hist = self->histogram;
    hist_max = dev->histogram_type == DT_DEV_HISTOGRAM_LINEAR?self->histogram_max[0]:logf(1.0 + self->histogram_max[0]);
    if(hist && hist_max > 0)
    {
      cairo_save(cr);
      cairo_scale(cr, width/63.0, -(height-5)/(float)hist_max);
      cairo_set_source_rgba(cr, .2, .2, .2, 0.5);
      dt_draw_histogram_8(cr, hist, 0, dev->histogram_type == DT_DEV_HISTOGRAM_WAVEFORM?DT_DEV_HISTOGRAM_LOGARITHMIC:dev->histogram_type); // TODO: make draw handle waveform histograms
      cairo_restore(cr);
    }
  }

  // Cleaning up
  cairo_destroy(cr);
  cairo_t *cr_pixmap = gdk_cairo_create(gtk_widget_get_window(widget));
  cairo_set_source_surface (cr_pixmap, cst, 0, 0);
  cairo_paint(cr_pixmap);
  cairo_destroy(cr_pixmap);
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
static void dt_iop_levels_move_handle(dt_iop_module_t *self, int handle_move, float new_pos, float *levels, float drag_start_percentage)
{
  dt_iop_levels_gui_data_t *c = (dt_iop_levels_gui_data_t *)self->gui_data;
  float min_x = 0;
  float max_x = 1;

  if ((handle_move < 0) || handle_move > 2)
    return;

  if (levels == NULL)
    return;

  // Determining the minimum and maximum bounds for the drag handles
  switch(handle_move)
  {
    case 0:
      max_x = fminf(levels[2] - (0.05 / drag_start_percentage),
                    1);
      max_x = fminf((levels[2] * (1 - drag_start_percentage) - 0.05)
                    / (1 - drag_start_percentage),
                    max_x);
      break;

    case 1:
      min_x = levels[0] + 0.05;
      max_x = levels[2] - 0.05;
      break;

    case 2:
      min_x = fmaxf((0.05 / drag_start_percentage) + levels[0],
                    0);
      min_x = fmaxf((levels[0] * (1 - drag_start_percentage) + 0.05)
                    / (1 - drag_start_percentage),
                    min_x);
      break;
  }

  levels[handle_move] =
    fminf(max_x, fmaxf(min_x, new_pos));

  if(handle_move != 1)
    levels[1] = levels[0] + (drag_start_percentage
                             * (levels[2] - levels[0]));

  if (c->activeToggleButton != NULL) gtk_toggle_button_set_active(c->activeToggleButton, FALSE);
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
  int height = allocation.height - 2*inset, width = allocation.width - 2*inset;
  if(!c->dragging)
  {
    c->mouse_x = CLAMP(event->x - inset, 0, width);
    c->drag_start_percentage = (p->levels[1] - p->levels[0])
                               / (p->levels[2] - p->levels[0]);
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
    const float mx = CLAMP(event->x - inset, 0, width)/(float)width;
    float dist = fabsf(p->levels[0] - mx);
    for(int k=1; k<3; k++)
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
  gdk_window_get_pointer(event->window, &x, &y, NULL);
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

  const float interval = 0.002; // Distance moved for each scroll event
  gboolean updated = FALSE;
  float new_position = 0;

  if (c->dragging)
  {
    return FALSE;
  }

  if(event->direction == GDK_SCROLL_UP)
  {
    new_position = p->levels[c->handle_move] + interval;
    updated = TRUE;
  }
  else if(event->direction == GDK_SCROLL_DOWN)
  {
    new_position = p->levels[c->handle_move] - interval;
    updated = TRUE;
  }

  if (updated)
  {
    dt_iop_levels_move_handle(self, c->handle_move, new_position,
                              p->levels, c->drag_start_percentage);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    return TRUE;
  }

  return FALSE;
}

static void dt_iop_levels_pick_general_handler(GtkToggleButton *togglebutton, dt_iop_module_t *self, double xpick, double ypick, dt_iop_levels_pick_t picklevel)
{
  dt_iop_levels_gui_data_t *c = (dt_iop_levels_gui_data_t *)self->gui_data;

  darktable.gui->reset = 1;
  // we do not require the callback if we deactivate it here
  if (c->activeToggleButton != NULL) gtk_toggle_button_set_active(c->activeToggleButton, FALSE);
  darktable.gui->reset = 0;

  gboolean toggle = gtk_toggle_button_get_active(togglebutton);
  if(darktable.gui->reset) return;

  if (TRUE == toggle)
  {
    self->request_color_pick = 1;
    dt_lib_colorpicker_set_point(darktable.lib, xpick, ypick);
    c->activeToggleButton = togglebutton;
    c->current_pick = picklevel;
    dt_dev_reprocess_all(self->dev);
  }
  else
  {
    self->request_color_pick = 0;
    c->activeToggleButton = NULL;
    c->current_pick = NONE;
    //gtk_widget_queue_draw(self->widget);
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

  float *hist = self->histogram;

  if(!hist) return;

  // search histogram for min (search from bottom)
  for(int k=0; k<=4*63; k+=4)
  {
    if (hist[k] > 1)
    {
      p->levels[0] = ((float)(k)/(4*64));
      break;
    }
  }
  // then for max (search from top)
  for(int k=4*63; k>=0; k-=4)
  {
    if (hist[k] > 1)
    {
      p->levels[2] = ((float)(k)/(4*64));
      break;
    }
  }
  p->levels[1] = p->levels[0]/2 + p->levels[2]/2;
  if (c->activeToggleButton != NULL) gtk_toggle_button_set_active(c->activeToggleButton, FALSE);
  c->last_picked_color = -1;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
