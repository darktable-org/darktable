#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include "iop/tonecurve.h"
#include "gui/histogram.h"
#include "develop/develop.h"
#include "control/control.h"
#include "gui/gtk.h"

#define DT_GUI_CURVE_EDITOR_INSET 5
#define DT_GUI_CURVE_INFL .3f

#ifndef M_PI
# define M_PI		3.14159265358979323846	/* pi */
#endif

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  float *in = (float *)i;
  float *out = (float *)o;
  // uint16_t *out = (uint16_t *)o;
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)(piece->data);
  for(int k=0;k<roi_out->width*roi_out->height;k++)
  {
#if 1 // in YCbCr float:
    // uint32_t tmp = 0x3f800000ul | (d->table[CLAMP((int)(in[0]/100.0*0xfffful), 0, 0xffff)]<<7);
    // out[0] = *(float *)&tmp - 1.0f;
    // in Lab: correct compressed Luminance for saturation:
    const int t = CLAMP((int)(in[0]/100.0*0xfffful), 0, 0xffff);
    out[0] = d->table[t];
    out[1] = in[1];
    out[2] = in[2];
#else // in sRGB
    for(int c=0;c<3;c++) out[c] = d->table[CLAMP((int)(in[c]*0x10000ul), 0, 0xffff)];
#endif
    in += 3; out += 3;
  }
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // pull in new params to gegl
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)(piece->data);
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)p1;
#ifdef HAVE_GEGL
  for(int k=0;k<6;k++) dt_draw_curve_set_point(d->curve, k, p->tonecurve_x[k], p->tonecurve_y[k]);
  gegl_node_set(piece->input, "curve", d->curve, NULL);
#else
  for(int k=0;k<6;k++) dt_draw_curve_set_point(d->curve, k, p->tonecurve_x[k], p->tonecurve_y[k]);
  for(int k=0;k<0x10000;k++)
    // d->table[k] = (uint16_t)(0xffff*dt_draw_curve_calc_value(d->curve, (1.0/0x10000)*k));
    d->table[k] = 100.0*dt_draw_curve_calc_value(d->curve, (1.0/0x10000)*k);
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // create part of the gegl pipeline
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)malloc(sizeof(dt_iop_tonecurve_data_t));
  dt_iop_tonecurve_params_t *default_params = (dt_iop_tonecurve_params_t *)self->default_params;
  piece->data = (void *)d;
  d->curve = dt_draw_curve_new(0.0, 1.0);
  for(int k=0;k<6;k++) (void)dt_draw_curve_add_point(d->curve, default_params->tonecurve_x[k], default_params->tonecurve_y[k]);
#ifdef HAVE_GEGL
  piece->input = piece->output = gegl_node_new_child(pipe->gegl, "operation", "gegl:dt-contrast-curve", "sampling-points", 65535, "curve", d->curve, NULL);
#else
  for(int k=0;k<0x10000;k++) d->table[k] = 100.0f*k/0x10000; // identity
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
  // TODO: reset combobox on preset changed event ?
  gtk_widget_queue_draw(self->widget);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_tonecurve_params_t));
  module->default_params = malloc(sizeof(dt_iop_tonecurve_params_t));
  module->default_enabled = 0;
  module->priority = 80;
  module->params_size = sizeof(dt_iop_tonecurve_params_t);
  module->gui_data = NULL;
  dt_iop_tonecurve_params_t tmp = (dt_iop_tonecurve_params_t) {{0.0, 0.08, 0.4, 0.6, 0.92, 1.0},
                                                {0.0, 0.08, 0.4, 0.6, 0.92, 1.0},
                                                 0};
  memcpy(module->params, &tmp, sizeof(dt_iop_tonecurve_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_tonecurve_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  // dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)module->data;
  // gegl_node_remove_child(module->dev->gegl, d->node);
  // gegl_node_remove_child(module->dev->gegl, d->node_preview);
  //..FIXME: ?? is this done by gegl?
  // free(d->curve);
  // g_unref(d->curve);
  // free(module->data);
  // module->data = NULL;
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

  c->minmax_curve = dt_draw_curve_new(0.0, 1.0);
  for(int k=0;k<6;k++) (void)dt_draw_curve_add_point(c->minmax_curve, p->tonecurve_x[k], p->tonecurve_y[k]);
  c->mouse_x = c->mouse_y = -1.0;
  c->selected = -1; c->selected_offset = 0.0;
  c->dragging = 0;
  self->widget = GTK_WIDGET(gtk_vbox_new(FALSE, 0));
  c->area = GTK_DRAWING_AREA(gtk_drawing_area_new());
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(c->area), TRUE, TRUE, 0);
  gtk_drawing_area_size(c->area, 195, 195);

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
  // init gtk stuff
  c->hbox = GTK_HBOX(gtk_hbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(c->hbox), FALSE, FALSE, 0);
  c->label = GTK_LABEL(gtk_label_new("presets"));
  gtk_box_pack_start(GTK_BOX(c->hbox), GTK_WIDGET(c->label), FALSE, FALSE, 5);
  c->presets = GTK_COMBO_BOX(gtk_combo_box_new_text());
  gtk_combo_box_append_text(GTK_COMBO_BOX(c->presets), "linear");
  gtk_combo_box_append_text(GTK_COMBO_BOX(c->presets), "med contrast");
  gtk_combo_box_append_text(GTK_COMBO_BOX(c->presets), "high contrast");
  gtk_box_pack_end(GTK_BOX(c->hbox), GTK_WIDGET(c->presets), FALSE, FALSE, 5);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_draw_curve_destroy(c->minmax_curve);
  free(self->gui_data);
  self->gui_data = NULL;
}


gboolean dt_iop_tonecurve_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  c->mouse_x = c->mouse_y = -1.0;
  gtk_widget_queue_draw(widget);
  return TRUE;
}

gboolean dt_iop_tonecurve_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  // dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)self->data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  for(int k=0;k<6;k++) dt_draw_curve_set_point(c->minmax_curve, k, p->tonecurve_x[k], p->tonecurve_y[k]);
  const int inset = DT_GUI_CURVE_EDITOR_INSET;
  int width = widget->allocation.width, height = widget->allocation.height;
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  // clear bg
  cairo_set_source_rgb (cr, .2, .2, .2);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2*inset; height -= 2*inset;

#if 0
  // draw shadow around
  float alpha = 1.0f;
  for(int k=0;k<inset;k++)
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
    oldx1 = p->tonecurve_x[c->selected]; oldy1 = p->tonecurve_y[c->selected];
    // dt_draw_curve_get_point(d->curve, c->selected, &oldx1, &oldy1);
    // if(c->selected == 2) dt_draw_curve_get_point(d->curve, 1, &oldx2, &oldy2);
    // if(c->selected == 3) dt_draw_curve_get_point(d->curve, 4, &oldx2, &oldy2);
    if(c->selected == 2) dt_draw_curve_set_point(c->minmax_curve, 1, p->tonecurve_x[1], fminf(c->selected_min, fmaxf(0.0, p->tonecurve_y[1] + DT_GUI_CURVE_INFL*(c->selected_min - oldy1))));
    if(c->selected == 3) dt_draw_curve_set_point(c->minmax_curve, 4, p->tonecurve_x[4], fmaxf(c->selected_min, fminf(1.0, p->tonecurve_y[4] + DT_GUI_CURVE_INFL*(c->selected_min - oldy1))));
    dt_draw_curve_set_point(c->minmax_curve, c->selected, oldx1, c->selected_min);
    dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, DT_IOP_TONECURVE_RES, c->draw_min_xs, c->draw_min_ys);
    if(c->selected == 2) dt_draw_curve_set_point(c->minmax_curve, 1, p->tonecurve_x[1], fminf(c->selected_max, fmaxf(0.0, p->tonecurve_y[1] + DT_GUI_CURVE_INFL*(c->selected_max - oldy1))));
    if(c->selected == 3) dt_draw_curve_set_point(c->minmax_curve, 4, p->tonecurve_x[4], fmaxf(c->selected_max, fminf(1.0, p->tonecurve_y[4] + DT_GUI_CURVE_INFL*(c->selected_max - oldy1))));
    dt_draw_curve_set_point  (c->minmax_curve, c->selected, oldx1, c->selected_max);
    dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, DT_IOP_TONECURVE_RES, c->draw_max_xs, c->draw_max_ys);
    dt_draw_curve_set_point(c->minmax_curve, c->selected, oldx1, oldy1);
    if(c->selected == 2) dt_draw_curve_set_point(c->minmax_curve, 1, p->tonecurve_x[1], p->tonecurve_y[1]);
    if(c->selected == 3) dt_draw_curve_set_point(c->minmax_curve, 4, p->tonecurve_x[4], p->tonecurve_y[4]);
  }
  dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, DT_IOP_TONECURVE_RES, c->draw_xs, c->draw_ys);

  // draw grid
  cairo_set_line_width(cr, .4);
  cairo_set_source_rgb (cr, .1, .1, .1);
  dt_draw_grid(cr, 4, width, height);
  
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
    cairo_move_to(cr, 0, 0);
    for(int k=0;k<DT_IOP_TONECURVE_RES;k++)   cairo_line_to(cr, k*width/(float)DT_IOP_TONECURVE_RES, - height*c->draw_min_ys[k]);
    for(int k=DT_IOP_TONECURVE_RES-2;k>0;k--) cairo_line_to(cr, k*width/(float)DT_IOP_TONECURVE_RES, - height*c->draw_max_ys[k]);
    cairo_close_path(cr);
    cairo_fill(cr);
    // draw mouse focus circle
    cairo_set_source_rgb(cr, .9, .9, .9);
    const float pos = DT_IOP_TONECURVE_RES * c->mouse_x/(float)width;
    int k = (int)pos; const float f = k - pos;
    if(k >= DT_IOP_TONECURVE_RES-1) k = DT_IOP_TONECURVE_RES - 2;
    float ht = -height*(f*c->draw_ys[k] + (1-f)*c->draw_ys[k+1]);
    cairo_arc(cr, c->mouse_x, ht+2.5, 4, 0, 2.*M_PI);
    cairo_stroke(cr);
  }

  // draw curve
  cairo_set_line_width(cr, 2.);
  cairo_set_source_rgb(cr, .9, .9, .9);
  // cairo_set_line_cap  (cr, CAIRO_LINE_CAP_SQUARE);
  cairo_move_to(cr, 0, 0);
  for(int k=0;k<DT_IOP_TONECURVE_RES;k++) cairo_line_to(cr, k*width/(float)DT_IOP_TONECURVE_RES, - height*c->draw_ys[k]);
  cairo_stroke(cr);

  cairo_destroy(cr);
  cairo_t *cr_pixmap = gdk_cairo_create(gtk_widget_get_window(widget));
  cairo_set_source_surface (cr_pixmap, cst, 0, 0);
  cairo_paint(cr_pixmap);
  cairo_destroy(cr_pixmap);
  cairo_surface_destroy(cst);
  return TRUE;
}

gboolean dt_iop_tonecurve_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  // TODO: don't set gegl stuff, only overwrite params_t and update history item!
  // dt_dev history will come 
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  // dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)self->data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  // dt_iop_get_params(self, (void*)&p);
  const int inset = DT_GUI_CURVE_EDITOR_INSET;
  int height = widget->allocation.height - 2*inset, width = widget->allocation.width - 2*inset;
  if(!c->dragging) c->mouse_x = CLAMP(event->x - inset, 0, width);
  c->mouse_y = CLAMP(event->y - inset, 0, height);

  if(c->dragging)
  {
    float f = c->selected_y - (c->mouse_y-c->selected_offset)/height;
    f = fmaxf(c->selected_min, fminf(c->selected_max, f));
    // if(c->selected == 2) c->curve.m_anchors[1].y = fminf(f, fmaxf(0.0, c->curve.m_anchors[1].y + DT_GUI_CURVE_INFL*(f - c->curve.m_anchors[2].y)));
    // if(c->selected == 3) c->curve.m_anchors[4].y = fmaxf(f, fminf(1.0, c->curve.m_anchors[4].y + DT_GUI_CURVE_INFL*(f - c->curve.m_anchors[3].y)));
    // c->curve.m_anchors[c->selected].y = f;
    /*gdouble oldx1, oldx2, oldy1, oldy2;
    dt_draw_curve_get_point(d->curve, c->selected, &oldx1, &oldy1);
    if(c->selected == 2) dt_draw_curve_get_point(d->curve, 1, &oldx2, &oldy2);
    if(c->selected == 3) dt_draw_curve_get_point(d->curve, 4, &oldx2, &oldy2);
    if(c->selected == 2) oldy2 = fminf(f, fmaxf(0.0, oldy2 + DT_GUI_CURVE_INFL*(f - oldy1)));
    if(c->selected == 3) oldy2 = fmaxf(f, fminf(1.0, oldy2 + DT_GUI_CURVE_INFL*(f - oldy1)));
    if(c->selected == 2) dt_draw_curve_set_point(d->curve, 1, oldx2, oldy2);
    if(c->selected == 3) dt_draw_curve_set_point(d->curve, 4, oldx2, oldy2);
    dt_draw_curve_set_point(d->curve, c->selected, oldx1, f);*/
    if(c->selected == 2) p->tonecurve_y[1] = fminf(f, fmaxf(0.0, p->tonecurve_y[1] + DT_GUI_CURVE_INFL*(f - p->tonecurve_y[2])));
    if(c->selected == 3) p->tonecurve_y[4] = fmaxf(f, fminf(1.0, p->tonecurve_y[4] + DT_GUI_CURVE_INFL*(f - p->tonecurve_y[3])));
    // DT_CTL_SET_IMAGE(tonecurve_y[c->selected], f);
    // if(c->selected == 2) DT_CTL_SET_IMAGE(tonecurve_y[1], oldy2);
    // if(c->selected == 3) DT_CTL_SET_IMAGE(tonecurve_y[4], oldy2);
    p->tonecurve_y[c->selected] = f;
    // if(c->selected == 2) p->tonecurve_y[1] = oldy2;
    // if(c->selected == 3) p->tonecurve_y[4] = oldy2;
    // dt_iop_set_params(self, (void*)&p);
    dt_dev_add_history_item(darktable.develop, self);
  }
  else
  {
    float pos = (event->x - inset)/width;
    float min = 100.0;
    int nearest = 0;
    for(int k=1;k<5;k++)
    {
      float dist = (pos - p->tonecurve_x[k]); dist *= dist;
      if(dist < min) { min = dist; nearest = k; }
    }
    c->selected = nearest;
    c->selected_y = p->tonecurve_y[c->selected];
    c->selected_offset = c->mouse_y;
    // c->selected_min = .5f*(c->selected_y + c->curve.m_anchors[c->selected-1].y);
    // c->selected_max = .5f*(c->selected_y + c->curve.m_anchors[c->selected+1].y);
    const float f = 0.8f;
    c->selected_min = fmaxf(c->selected_y - 0.2f, (1.-f)*c->selected_y + f*p->tonecurve_y[c->selected-1]);
    c->selected_max = fminf(c->selected_y + 0.2f, (1.-f)*c->selected_y + f*p->tonecurve_y[c->selected+1]);
    if(c->selected == 1) c->selected_max *= 0.7;
    if(c->selected == 4) c->selected_min = 1.0 - 0.7*(1.0 - c->selected_min);
  }
  gtk_widget_queue_draw(widget);

  gint x, y;
  gdk_window_get_pointer(event->window, &x, &y, NULL);
  return TRUE;
}

gboolean dt_iop_tonecurve_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{ // set active point
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  c->dragging = 1;
  return TRUE;
}

gboolean dt_iop_tonecurve_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  c->dragging = 0;
  return TRUE;
}

