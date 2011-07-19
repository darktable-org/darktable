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
#include "iop/tonecurve.h"
#include "gui/histogram.h"
#include "gui/presets.h"
#include "develop/develop.h"
#include "control/control.h"
#include "gui/gtk.h"
#include "common/opencl.h"

#define DT_GUI_CURVE_EDITOR_INSET 5
#define DT_GUI_CURVE_INFL .3f

DT_MODULE(1)

const char *name()
{
  return _("tone curve");
}


int
groups ()
{
  return IOP_GROUP_CORRECT;
}

int
flags ()
{
  return IOP_FLAGS_SUPPORTS_BLENDING;
}


#ifdef HAVE_OPENCL
int
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)piece->data;
  dt_iop_tonecurve_global_data_t *gd = (dt_iop_tonecurve_global_data_t *)self->data;
  cl_mem dev_m = NULL;
  cl_mem dev_coeffs = NULL;
  cl_int err = -999;

  const int devid = piece->pipe->devid;
  size_t sizes[] = {roi_in->width, roi_in->height, 1};
  dev_m = dt_opencl_copy_host_to_device(d->table, 256, 256, devid, sizeof(float));
  if (dev_m == NULL) goto error;
  dev_coeffs = dt_opencl_copy_host_to_device_constant(sizeof(float)*2, devid, d->unbounded_coeffs);
  if (dev_coeffs == NULL) goto error;
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_tonecurve, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_tonecurve, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_tonecurve, 2, sizeof(cl_mem), (void *)&dev_m);
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_tonecurve, 3, sizeof(cl_mem), (void *)&dev_coeffs);
  err = dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_tonecurve, sizes);
  if(err != CL_SUCCESS) goto error;
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_coeffs);
  return TRUE;

error:
  if (dev_m != NULL) dt_opencl_release_mem_object(dev_m);
  if (dev_coeffs != NULL) dt_opencl_release_mem_object(dev_coeffs);
  dt_print(DT_DEBUG_OPENCL, "[opencl_tonecurve] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  const int ch = piece->colors;
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)(piece->data);
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_out, i, o, d) schedule(static)
#endif
  for(int k=0; k<roi_out->height; k++)
  {
    float *in = ((float *)i) + k*ch*roi_out->width;
    float *out = ((float *)o) + k*ch*roi_out->width;
    for (int j=0; j<roi_out->width; j++,in+=ch,out+=ch)
    {
      // in Lab: correct compressed Luminance for saturation:
      const float L_in = in[0]/100.0f;
      out[0] = (L_in < 1.0f) ? d->table[CLAMP((int)(L_in*0xfffful), 0, 0xffff)] :
        dt_iop_eval_exp(d->unbounded_coeffs, L_in);
        
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
    }
  }
}

void init_presets (dt_iop_module_so_t *self)
{
  dt_iop_tonecurve_params_t p;
  p.tonecurve_preset = 0;

  float linear[6] = {0.0, 0.08, 0.4, 0.6, 0.92, 1.0};
  for(int k=0; k<6; k++) p.tonecurve_x[k] = linear[k];
  for(int k=0; k<6; k++) p.tonecurve_y[k] = linear[k];
  dt_gui_presets_add_generic(_("linear"), self->op, &p, sizeof(p), 1);

  for(int k=0; k<6; k++) p.tonecurve_x[k] = linear[k];
  for(int k=0; k<6; k++) p.tonecurve_y[k] = linear[k];
  p.tonecurve_y[1] -= 0.03;
  p.tonecurve_y[4] += 0.03;
  p.tonecurve_y[2] -= 0.03;
  p.tonecurve_y[3] += 0.03;
  for(int k=1; k<5; k++) p.tonecurve_y[k] = powf(p.tonecurve_y[k], 2.2f);
  for(int k=1; k<5; k++) p.tonecurve_x[k] = powf(p.tonecurve_x[k], 2.2f);
  dt_gui_presets_add_generic(_("med contrast"), self->op, &p, sizeof(p), 1);

  for(int k=0; k<6; k++) p.tonecurve_x[k] = linear[k];
  for(int k=0; k<6; k++) p.tonecurve_y[k] = linear[k];
  p.tonecurve_y[1] -= 0.06;
  p.tonecurve_y[4] += 0.06;
  p.tonecurve_y[2] -= 0.10;
  p.tonecurve_y[3] += 0.10;
  for(int k=1; k<5; k++) p.tonecurve_y[k] = powf(p.tonecurve_y[k], 2.2f);
  for(int k=1; k<5; k++) p.tonecurve_x[k] = powf(p.tonecurve_x[k], 2.2f);
  dt_gui_presets_add_generic(_("high contrast"), self->op, &p, sizeof(p), 1);
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // pull in new params to gegl
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)(piece->data);
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)p1;
  for(int k=0; k<6; k++)
    dt_draw_curve_set_point(d->curve, k, p->tonecurve_x[k], p->tonecurve_y[k]);
  dt_draw_curve_calc_values(d->curve, 0.0f, 1.0f, 0x10000, NULL, d->table);
  for(int k=0; k<0x10000; k++) d->table[k] *= 100.0f;

  // now the extrapolation stuff:
  const float x[4] = {0.7f, 0.8f, 0.9f, 1.0f};
  const float y[4] = {d->table[CLAMP((int)(x[0]*0x10000ul), 0, 0xffff)],
                      d->table[CLAMP((int)(x[1]*0x10000ul), 0, 0xffff)],
                      d->table[CLAMP((int)(x[2]*0x10000ul), 0, 0xffff)],
                      d->table[CLAMP((int)(x[3]*0x10000ul), 0, 0xffff)]};
  dt_iop_estimate_exp(x, y, 4, d->unbounded_coeffs);
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // create part of the gegl pipeline
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)malloc(sizeof(dt_iop_tonecurve_data_t));
  dt_iop_tonecurve_params_t *default_params = (dt_iop_tonecurve_params_t *)self->default_params;
  piece->data = (void *)d;
  d->curve = dt_draw_curve_new(0.0, 1.0, CUBIC_SPLINE);
  for(int k=0; k<6; k++) (void)dt_draw_curve_add_point(d->curve, default_params->tonecurve_x[k], default_params->tonecurve_y[k]);
#ifdef HAVE_GEGL
  piece->input = piece->output = gegl_node_new_child(pipe->gegl, "operation", "gegl:dt-contrast-curve", "sampling-points", 65535, "curve", d->curve, NULL);
#else
  for(int k=0; k<0x10000; k++) d->table[k] = 100.0f*k/0x10000; // identity
#endif
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // clean up everything again.
#ifdef HAVE_GEGL
  // dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)(piece->data);
  (void)gegl_node_remove_child(pipe->gegl, piece->input);
  // (void)gegl_node_remove_child(pipe->gegl, d->node);
  // (void)gegl_node_remove_child(pipe->gegl, piece->output);
#endif
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)(piece->data);
  dt_draw_curve_destroy(d->curve);
  free(piece->data);
}

void gui_update(struct dt_iop_module_t *self)
{
  // nothing to do, gui curve is read directly from params during expose event.
  gtk_widget_queue_draw(self->widget);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_tonecurve_params_t));
  module->default_params = malloc(sizeof(dt_iop_tonecurve_params_t));
  module->default_enabled = 0;
  module->priority = 577; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_tonecurve_params_t);
  module->gui_data = NULL;
  dt_iop_tonecurve_params_t tmp = (dt_iop_tonecurve_params_t)
  {
    {
      0.0, 0.08, 0.4, 0.6, 0.92, 1.0
    },
    {0.0, 0.08, 0.4, 0.6, 0.92, 1.0},
    0
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_tonecurve_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_tonecurve_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_tonecurve_global_data_t *gd = (dt_iop_tonecurve_global_data_t *)malloc(sizeof(dt_iop_tonecurve_global_data_t));
  module->data = gd;
  gd->kernel_tonecurve = dt_opencl_create_kernel(darktable.opencl, program, "tonecurve");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_tonecurve_global_data_t *gd = (dt_iop_tonecurve_global_data_t *)module->data;
  dt_opencl_free_kernel(darktable.opencl, gd->kernel_tonecurve);
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
  self->gui_data = malloc(sizeof(dt_iop_tonecurve_gui_data_t));
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;

  c->minmax_curve = dt_draw_curve_new(0.0, 1.0, CUBIC_SPLINE);
  for(int k=0; k<6; k++) (void)dt_draw_curve_add_point(c->minmax_curve, p->tonecurve_x[k], p->tonecurve_y[k]);
  c->mouse_x = c->mouse_y = -1.0;
  c->selected = -1;
  c->selected_offset = c->selected_y = c->selected_min = c->selected_max = 0.0;
  c->dragging = 0;
  c->x_move = -1;
  self->widget = GTK_WIDGET(gtk_vbox_new(FALSE, 5));
  c->area = GTK_DRAWING_AREA(gtk_drawing_area_new());
  GtkWidget *asp = gtk_aspect_frame_new(NULL, 0.5, 0.5, 1.0, TRUE);
  gtk_box_pack_start(GTK_BOX(self->widget), asp, TRUE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(asp), GTK_WIDGET(c->area));
  // gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(c->area), TRUE, TRUE, 0);
  gtk_drawing_area_size(c->area, 258, 258);
  g_object_set (GTK_OBJECT(c->area), "tooltip-text", _("abscissa: input, ordinate: output \nworks on L channel"), (char *)NULL);

  gtk_widget_add_events(GTK_WIDGET(c->area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect (G_OBJECT (c->area), "expose-event",
                    G_CALLBACK (dt_iop_tonecurve_expose), self);
  g_signal_connect (G_OBJECT (c->area), "button-press-event",
                    G_CALLBACK (dt_iop_tonecurve_button_press), self);
  g_signal_connect (G_OBJECT (c->area), "button-release-event",
                    G_CALLBACK (dt_iop_tonecurve_button_release), self);
  g_signal_connect (G_OBJECT (c->area), "motion-notify-event",
                    G_CALLBACK (dt_iop_tonecurve_motion_notify), self);
  g_signal_connect (G_OBJECT (c->area), "leave-notify-event",
                    G_CALLBACK (dt_iop_tonecurve_leave_notify), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_draw_curve_destroy(c->minmax_curve);
  free(self->gui_data);
  self->gui_data = NULL;
}


static gboolean dt_iop_tonecurve_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  c->mouse_x = c->mouse_y = -1.0;
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean dt_iop_tonecurve_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  for(int k=0; k<6; k++) dt_draw_curve_set_point(c->minmax_curve, k, p->tonecurve_x[k], p->tonecurve_y[k]);
  const int inset = DT_GUI_CURVE_EDITOR_INSET;
  int width = widget->allocation.width, height = widget->allocation.height;
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  // clear bg
  cairo_set_source_rgb (cr, .2, .2, .2);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2*inset;
  height -= 2*inset;

#if 0
  // draw shadow around
  float alpha = 1.0f;
  for(int k=0; k<inset; k++)
  {
    cairo_rectangle(cr, -k, -k, width + 2*k, height + 2*k);
    cairo_set_source_rgba(cr, 0, 0, 0, alpha);
    alpha *= 0.6f;
    cairo_fill(cr);
  }
#else
  cairo_set_line_width(cr, 1.0);
  cairo_set_source_rgb (cr, .1, .1, .1);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_stroke(cr);
#endif

  cairo_set_source_rgb (cr, .3, .3, .3);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  if(c->mouse_y > 0 || c->dragging)
  {
    float oldx1, oldy1;
    oldx1 = p->tonecurve_x[c->selected];
    oldy1 = p->tonecurve_y[c->selected];

    if(c->selected == 0) dt_draw_curve_set_point(c->minmax_curve, 1, p->tonecurve_x[1], fmaxf(c->selected_min, p->tonecurve_y[1]));
    if(c->selected == 2) dt_draw_curve_set_point(c->minmax_curve, 1, p->tonecurve_x[1], fminf(c->selected_min, fmaxf(0.0, p->tonecurve_y[1] + DT_GUI_CURVE_INFL*(c->selected_min - oldy1))));
    if(c->selected == 3) dt_draw_curve_set_point(c->minmax_curve, 4, p->tonecurve_x[4], fmaxf(c->selected_min, fminf(1.0, p->tonecurve_y[4] + DT_GUI_CURVE_INFL*(c->selected_min - oldy1))));
    if(c->selected == 5) dt_draw_curve_set_point(c->minmax_curve, 4, p->tonecurve_x[4], fminf(c->selected_min, p->tonecurve_y[4]));
    dt_draw_curve_set_point(c->minmax_curve, c->selected, oldx1, c->selected_min);
    dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, DT_IOP_TONECURVE_RES, c->draw_min_xs, c->draw_min_ys);

    if(c->selected == 0) dt_draw_curve_set_point(c->minmax_curve, 1, p->tonecurve_x[1], fmaxf(c->selected_max, p->tonecurve_y[1]));
    if(c->selected == 2) dt_draw_curve_set_point(c->minmax_curve, 1, p->tonecurve_x[1], fminf(c->selected_max, fmaxf(0.0, p->tonecurve_y[1] + DT_GUI_CURVE_INFL*(c->selected_max - oldy1))));
    if(c->selected == 3) dt_draw_curve_set_point(c->minmax_curve, 4, p->tonecurve_x[4], fmaxf(c->selected_max, fminf(1.0, p->tonecurve_y[4] + DT_GUI_CURVE_INFL*(c->selected_max - oldy1))));
    if(c->selected == 5) dt_draw_curve_set_point(c->minmax_curve, 4, p->tonecurve_x[4], fminf(c->selected_max, p->tonecurve_y[4]));
    dt_draw_curve_set_point  (c->minmax_curve, c->selected, oldx1, c->selected_max);
    dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, DT_IOP_TONECURVE_RES, c->draw_max_xs, c->draw_max_ys);

  }
  for(int k=0; k<6; k++) dt_draw_curve_set_point(c->minmax_curve, k, p->tonecurve_x[k], p->tonecurve_y[k]);
  dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, DT_IOP_TONECURVE_RES, c->draw_xs, c->draw_ys);

  // draw grid
  cairo_set_line_width(cr, .4);
  cairo_set_source_rgb (cr, .1, .1, .1);
  dt_draw_grid(cr, 4, 0, 0, width, height);

  // draw x positions
  cairo_set_line_width(cr, 1.);
  cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
  const float arrw = 7.0f;
  for(int k=1; k<5; k++)
  {
    cairo_move_to(cr, width*p->tonecurve_x[k], height+inset-1);
    cairo_rel_line_to(cr, -arrw*.5f, 0);
    cairo_rel_line_to(cr, arrw*.5f, -arrw);
    cairo_rel_line_to(cr, arrw*.5f, arrw);
    cairo_close_path(cr);
    if(c->x_move == k) cairo_fill(cr);
    else               cairo_stroke(cr);
  }

  // draw selected cursor
  cairo_set_line_width(cr, 1.);
  cairo_translate(cr, 0, height);

  // draw lum h istogram in background
  dt_develop_t *dev = darktable.develop;
  float *hist, hist_max;
  hist = dev->histogram_pre;
  hist_max = dev->histogram_pre_max;
  if(hist_max > 0)
  {
    cairo_save(cr);
    cairo_scale(cr, width/63.0, -(height-5)/(float)hist_max);
    cairo_set_source_rgba(cr, .2, .2, .2, 0.5);
    dt_gui_histogram_draw_8(cr, hist, 3);
    cairo_restore(cr);
  }

  if(c->mouse_y > 0 || c->dragging)
  {
    // draw min/max, if selected
    cairo_set_source_rgba(cr, .6, .6, .6, .5);
    cairo_move_to(cr, 0, - height*c->draw_min_ys[0]);
    for(int k=1; k<DT_IOP_TONECURVE_RES; k++)   cairo_line_to(cr, k*width/(DT_IOP_TONECURVE_RES-1.0), - height*c->draw_min_ys[k]);
    cairo_line_to(cr, width, - height*c->draw_min_ys[DT_IOP_TONECURVE_RES-1]);
    cairo_line_to(cr, width, - height*c->draw_max_ys[DT_IOP_TONECURVE_RES-1]);
    for(int k=DT_IOP_TONECURVE_RES-2; k>=0; k--) cairo_line_to(cr, k*width/(DT_IOP_TONECURVE_RES-1.0), - height*c->draw_max_ys[k]);
    cairo_close_path(cr);
    cairo_fill(cr);
    // draw mouse focus circle
    cairo_set_source_rgb(cr, .9, .9, .9);
    const float pos = MAX(0, (DT_IOP_TONECURVE_RES-1) * c->mouse_x/(float)width - 1);
    int k = (int)pos;
    const float f = k - pos;
    if(k >= DT_IOP_TONECURVE_RES-1) k = DT_IOP_TONECURVE_RES - 2;
    float ht = -height*(f*c->draw_ys[k] + (1-f)*c->draw_ys[k+1]);
    cairo_arc(cr, c->mouse_x, ht, 4, 0, 2.*M_PI);
    cairo_stroke(cr);
  }

  // draw curve
  cairo_set_line_width(cr, 2.);
  cairo_set_source_rgb(cr, .9, .9, .9);
  // cairo_set_line_cap  (cr, CAIRO_LINE_CAP_SQUARE);
  cairo_move_to(cr, 0, -height*c->draw_ys[0]);
  for(int k=1; k<DT_IOP_TONECURVE_RES; k++) cairo_line_to(cr, k*width/(DT_IOP_TONECURVE_RES-1.0), - height*c->draw_ys[k]);
  cairo_stroke(cr);

  cairo_destroy(cr);
  cairo_t *cr_pixmap = gdk_cairo_create(gtk_widget_get_window(widget));
  cairo_set_source_surface (cr_pixmap, cst, 0, 0);
  cairo_paint(cr_pixmap);
  cairo_destroy(cr_pixmap);
  cairo_surface_destroy(cst);
  return TRUE;
}

static gboolean dt_iop_tonecurve_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  const int inset = DT_GUI_CURVE_EDITOR_INSET;
  int height = widget->allocation.height - 2*inset, width = widget->allocation.width - 2*inset;
  if(!c->dragging) c->mouse_x = CLAMP(event->x - inset, 0, width);
  c->mouse_y = CLAMP(event->y - inset, 0, height);

  if(c->dragging)
  {
    if(c->x_move >= 0)
    {
      const float mx = CLAMP(event->x - inset, 0, width)/(float)width;
      if(c->x_move > 0 && c->x_move < 6-1)
      {
        const float minx = p->tonecurve_x[c->x_move-1] + 0.001f;
        const float maxx = p->tonecurve_x[c->x_move+1] - 0.001f;
        p->tonecurve_x[c->x_move] = fminf(maxx, fmaxf(minx, mx));
      }
    }
    else
    {
      float f = c->selected_y - (c->mouse_y-c->selected_offset)/height;
      f = fmaxf(c->selected_min, fminf(c->selected_max, f));
      if(c->selected == 0) p->tonecurve_y[1] = fmaxf(f, p->tonecurve_y[1]);
      if(c->selected == 2) p->tonecurve_y[1] = fminf(f, fmaxf(0.0, p->tonecurve_y[1] + DT_GUI_CURVE_INFL*(f - p->tonecurve_y[2])));
      if(c->selected == 3) p->tonecurve_y[4] = fmaxf(f, fminf(1.0, p->tonecurve_y[4] + DT_GUI_CURVE_INFL*(f - p->tonecurve_y[3])));
      if(c->selected == 5) p->tonecurve_y[4] = fminf(f, p->tonecurve_y[4]);
      p->tonecurve_y[c->selected] = f;
    }
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
  else
  {
    if(event->y > height)
    {
      c->x_move = 1;
      const float mx = CLAMP(event->x - inset, 0, width)/(float)width;
      float dist = fabsf(p->tonecurve_x[1] - mx);
      for(int k=2; k<5; k++)
      {
        float d2 = fabsf(p->tonecurve_x[k] - mx);
        if(d2 < dist)
        {
          c->x_move = k;
          dist = d2;
        }
      }
    }
    else
    {
      c->x_move = -1;
    }
    float pos = (event->x - inset)/width;
    float min = 100.0;
    int nearest = 0;
    for(int k=0; k<6; k++)
    {
      float dist = (pos - p->tonecurve_x[k]);
      dist *= dist;
      if(dist < min)
      {
        min = dist;
        nearest = k;
      }
    }
    c->selected = nearest;
    c->selected_y = p->tonecurve_y[c->selected];
    c->selected_offset = c->mouse_y;
    const float f = 0.8f;
    if(c->selected == 0)
    {
      c->selected_min = 0.0f;
      c->selected_max = 0.2f;
    }
    else if(c->selected == 5)
    {
      c->selected_min = 0.8f;
      c->selected_max = 1.0f;
    }
    else
    {
      c->selected_min = fmaxf(c->selected_y - 0.2f, (1.-f)*c->selected_y + f*p->tonecurve_y[c->selected-1]);
      c->selected_max = fminf(c->selected_y + 0.2f, (1.-f)*c->selected_y + f*p->tonecurve_y[c->selected+1]);
    }
    if(c->selected == 1) c->selected_max *= 0.7;
    if(c->selected == 4) c->selected_min = 1.0 - 0.7*(1.0 - c->selected_min);
  }
  gtk_widget_queue_draw(widget);

  gint x, y;
  gdk_window_get_pointer(event->window, &x, &y, NULL);
  return TRUE;
}

static gboolean dt_iop_tonecurve_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  // set active point
  if(event->button == 1)
  {
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
    c->dragging = 1;
    return TRUE;
  }
  return FALSE;
}

static gboolean dt_iop_tonecurve_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(event->button == 1)
  {
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
    c->dragging = 0;
    return TRUE;
  }
  return FALSE;
}

