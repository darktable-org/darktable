#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include "iop/equalizer.h"
#include "gui/histogram.h"
#include "develop/develop.h"
#include "control/control.h"
#include "gui/gtk.h"

#define DT_GUI_EQUALIZER_INSET 5
#define DT_GUI_CURVE_INFL .3f

#ifndef M_PI
# define M_PI		3.14159265358979323846	/* pi */
#endif

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, int x, int y, float scale, int width, int height)
{
  //float *in = (float *)i;
  float *out = (float *)o;
  dt_iop_equalizer_data_t *d = (dt_iop_equalizer_data_t *)(piece->data);

  // 1 pixel in this buffer represents 1.0/scale pixels in original image:
  // so finest possible level here is
  // 1 pixel: level 1
  // 2 pixels: level 2
  // 4 pixels: level 3
  const float l1 = 1.0f + log2f(1.0/scale);               // finest level
  const float lm = 1.0f + log2f(MIN(width,height)/scale); // coarsest level
  printf("level range in %d %d: %f %f\n", 1, d->num_levels, l1, lm);
  // level 1 => full resolution
  int numl = 0; for(int k=MIN(width,height);k;k>>=1) numl++;

  for(int l=1;l<numl;l++)
  {
    // 1 => 1
    // 0 => num_levels
    const float coeff = dt_draw_curve_calc_value(d->curve, 1.0-((lm-l1)*l/(float)numl + l1)/(float)d->num_levels);
    printf("level %d => l: %f => x: %f\n", l, (lm-l1)*l/(float)numl + l1, 1.0-((lm-l1)*l/(float)numl + l1)/(float)d->num_levels);
    const int step = 1<<l;
    for(int j=0;j<height;j+=step) for(int i=step/2;i<width;i+=step) out[width*j + i] *= coeff;
    for(int j=step/2;j<height;j+=step) for(int i=0;i<width;i+=step) out[width*j + i] *= coeff;
    for(int j=step/2;j<height;j+=step) for(int i=step/2;i<width;i+=step) out[width*j + i] *= coeff;
  }


#if 0
  for(int k=0;k<width*height;k++)
  {
    // 
    in += 3; out += 3;
  }
#endif
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // pull in new params to gegl
  dt_iop_equalizer_data_t *d = (dt_iop_equalizer_data_t *)(piece->data);
  dt_iop_equalizer_params_t *p = (dt_iop_equalizer_params_t *)p1;
#ifdef HAVE_GEGL
  // TODO
#else
  for(int k=0;k<DT_IOP_EQUALIZER_BANDS;k++) dt_draw_curve_set_point(d->curve, k, p->equalizer_x[k], p->equalizer_y[k]);
  int l = 0; for(int k=(int)MIN(pipe->iwidth*pipe->iscale,pipe->iheight*pipe->iscale);k;k>>=1) l++;
  d->num_levels = l;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // create part of the gegl pipeline
  dt_iop_equalizer_data_t *d = (dt_iop_equalizer_data_t *)malloc(sizeof(dt_iop_equalizer_data_t));
  dt_iop_equalizer_params_t *default_params = (dt_iop_equalizer_params_t *)self->default_params;
  piece->data = (void *)d;
  d->curve = dt_draw_curve_new(0.0, 1.0);
  for(int k=0;k<DT_IOP_EQUALIZER_BANDS;k++) (void)dt_draw_curve_add_point(d->curve, default_params->equalizer_x[k], default_params->equalizer_y[k]);
#ifdef HAVE_GEGL
  piece->input = piece->output = gegl_node_new_child(pipe->gegl, "operation", "gegl:dt-contrast-curve", "sampling-points", 65535, "curve", d->curve, NULL);
#else
  d->num_levels = 1;
#endif
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // clean up everything again.
#ifdef HAVE_GEGL
#error "gegl version not implemented!"
  // (void)gegl_node_remove_child(pipe->gegl, piece->input);
#endif
  dt_iop_equalizer_data_t *d = (dt_iop_equalizer_data_t *)(piece->data);
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
  module->params = malloc(sizeof(dt_iop_equalizer_params_t));
  module->default_params = malloc(sizeof(dt_iop_equalizer_params_t));
  module->params_size = sizeof(dt_iop_equalizer_params_t);
  module->gui_data = NULL;
  dt_iop_equalizer_params_t tmp;
  for(int k=0;k<DT_IOP_EQUALIZER_BANDS;k++) tmp.equalizer_x[k] = k/(float)(DT_IOP_EQUALIZER_BANDS-1);
  for(int k=0;k<DT_IOP_EQUALIZER_BANDS;k++) tmp.equalizer_y[k] = 0.5f;
  memcpy(module->params, &tmp, sizeof(dt_iop_equalizer_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_equalizer_params_t));
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
  self->gui_data = malloc(sizeof(dt_iop_equalizer_gui_data_t));
  dt_iop_equalizer_gui_data_t *c = (dt_iop_equalizer_gui_data_t *)self->gui_data;
  dt_iop_equalizer_params_t *p = (dt_iop_equalizer_params_t *)self->params;

  c->minmax_curve = dt_draw_curve_new(0.0, 1.0);
  for(int k=0;k<DT_IOP_EQUALIZER_BANDS;k++) (void)dt_draw_curve_add_point(c->minmax_curve, p->equalizer_x[k], p->equalizer_y[k]);
  c->mouse_x = c->mouse_y = -1.0;
  c->dragging = 0;
  c->mouse_radius = 0.1f;
  self->widget = GTK_WIDGET(gtk_vbox_new(FALSE, 0));
  c->area = GTK_DRAWING_AREA(gtk_drawing_area_new());
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(c->area), TRUE, TRUE, 0);
  gtk_drawing_area_size(c->area, 195, 195);

  gtk_widget_add_events(GTK_WIDGET(c->area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect (G_OBJECT (c->area), "expose-event",
                    G_CALLBACK (dt_iop_equalizer_expose), self);
  g_signal_connect (G_OBJECT (c->area), "button-press-event",
                    G_CALLBACK (dt_iop_equalizer_button_press), self);
  g_signal_connect (G_OBJECT (c->area), "button-release-event",
                    G_CALLBACK (dt_iop_equalizer_button_release), self);
  g_signal_connect (G_OBJECT (c->area), "motion-notify-event",
                    G_CALLBACK (dt_iop_equalizer_motion_notify), self);
  g_signal_connect (G_OBJECT (c->area), "leave-notify-event",
                    G_CALLBACK (dt_iop_equalizer_leave_notify), self);
  g_signal_connect (G_OBJECT (c->area), "scroll-event",
                    G_CALLBACK (dt_iop_equalizer_scrolled), self);
  // init gtk stuff
  c->hbox = GTK_HBOX(gtk_hbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(c->hbox), FALSE, FALSE, 0);

  c->button_Y   = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Y"));
  c->button_Cb  = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Cb"));
  c->button_Cr  = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Cr"));
  c->button_all = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("all"));

  gtk_box_pack_end(GTK_BOX(c->hbox), GTK_WIDGET(c->button_all), FALSE, FALSE, 5);
  gtk_box_pack_end(GTK_BOX(c->hbox), GTK_WIDGET(c->button_Cr),  FALSE, FALSE, 5);
  gtk_box_pack_end(GTK_BOX(c->hbox), GTK_WIDGET(c->button_Cb),  FALSE, FALSE, 5);
  gtk_box_pack_end(GTK_BOX(c->hbox), GTK_WIDGET(c->button_Y),   FALSE, FALSE, 5);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_equalizer_gui_data_t *c = (dt_iop_equalizer_gui_data_t *)self->gui_data;
  dt_draw_curve_destroy(c->minmax_curve);
  free(self->gui_data);
  self->gui_data = NULL;
}

gboolean dt_iop_equalizer_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_equalizer_gui_data_t *c = (dt_iop_equalizer_gui_data_t *)self->gui_data;
  c->mouse_radius = 1.0/DT_IOP_EQUALIZER_BANDS;
  c->mouse_x = c->mouse_y = -1.0;
  gtk_widget_queue_draw(widget);
  return TRUE;
}

// fills in new parameters based on mouse position (in 0,1)
void dt_iop_equalizer_get_params(dt_iop_equalizer_params_t *p, const double mouse_x, const double mouse_y, const float rad)
{
  for(int k=0;k<DT_IOP_EQUALIZER_BANDS;k++)
  {
    const float f = fmaxf(0.0, rad*rad - (mouse_x - p->equalizer_x[k])*(mouse_x - p->equalizer_x[k]))/(rad*rad);
    p->equalizer_y[k] = (1-f)*p->equalizer_y[k] + f*mouse_y;
  }
}

gboolean dt_iop_equalizer_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_equalizer_gui_data_t *c = (dt_iop_equalizer_gui_data_t *)self->gui_data;
  dt_iop_equalizer_params_t p = *(dt_iop_equalizer_params_t *)self->params;
  for(int k=0;k<DT_IOP_EQUALIZER_BANDS;k++) dt_draw_curve_set_point(c->minmax_curve, k, p.equalizer_x[k], p.equalizer_y[k]);
  const int inset = DT_GUI_EQUALIZER_INSET;
  int width = widget->allocation.width, height = widget->allocation.height;
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  // clear bg
  cairo_set_source_rgb (cr, .2, .2, .2);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2*inset; height -= 2*inset;

  cairo_set_line_width(cr, 1.0);
  cairo_set_source_rgb (cr, .1, .1, .1);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_stroke(cr);

  cairo_set_source_rgb (cr, .3, .3, .3);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  if(c->mouse_y > 0 || c->dragging)
  {
    // draw min/max curves:
    dt_iop_equalizer_get_params(&p, c->mouse_x, 1.0, c->mouse_radius);
    for(int k=0;k<DT_IOP_EQUALIZER_BANDS;k++)
      dt_draw_curve_set_point(c->minmax_curve, k, p.equalizer_x[k], p.equalizer_y[k]);
    dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, DT_IOP_EQUALIZER_RES, c->draw_min_xs, c->draw_min_ys);

    p = *(dt_iop_equalizer_params_t *)self->params;
    dt_iop_equalizer_get_params(&p, c->mouse_x, .0, c->mouse_radius);
    for(int k=0;k<DT_IOP_EQUALIZER_BANDS;k++)
      dt_draw_curve_set_point(c->minmax_curve, k, p.equalizer_x[k], p.equalizer_y[k]);
    dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, DT_IOP_EQUALIZER_RES, c->draw_max_xs, c->draw_max_ys);

    p = *(dt_iop_equalizer_params_t *)self->params;
    for(int k=0;k<DT_IOP_EQUALIZER_BANDS;k++)
      dt_draw_curve_set_point(c->minmax_curve, k, p.equalizer_x[k], p.equalizer_y[k]);
  }
  dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, DT_IOP_EQUALIZER_RES, c->draw_xs, c->draw_ys);

  // draw grid
  cairo_set_line_width(cr, .4);
  cairo_set_source_rgb (cr, .1, .1, .1);
  dt_draw_grid(cr, 8, width, height);
  
  // draw selected cursor
  cairo_set_line_width(cr, 1.);
  cairo_translate(cr, 0, height);

  // TODO: draw frequency histogram in bg.
#if 0
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
#endif
 
  if(c->mouse_y > 0 || c->dragging)
  {
    // draw min/max, if selected
    cairo_set_source_rgba(cr, .6, .6, .6, .5);
    cairo_move_to(cr, 0, - height*c->draw_min_ys[0]);
    for(int k=1;k<DT_IOP_EQUALIZER_RES;k++)   cairo_line_to(cr, k*width/(float)DT_IOP_EQUALIZER_RES, - height*c->draw_min_ys[k]);
    for(int k=DT_IOP_EQUALIZER_RES-2;k>0;k--) cairo_line_to(cr, k*width/(float)DT_IOP_EQUALIZER_RES, - height*c->draw_max_ys[k]);
    cairo_close_path(cr);
    cairo_fill(cr);
    // draw mouse focus circle
    cairo_set_source_rgb(cr, .9, .9, .9);
    const float pos = DT_IOP_EQUALIZER_RES * c->mouse_x;
    int k = (int)pos; const float f = k - pos;
    if(k >= DT_IOP_EQUALIZER_RES-1) k = DT_IOP_EQUALIZER_RES - 2;
    float ht = -height*(f*c->draw_ys[k] + (1-f)*c->draw_ys[k+1]);
    cairo_arc(cr, c->mouse_x*width, ht, c->mouse_radius*width, 0, 2.*M_PI);
    cairo_stroke(cr);
  }

  // draw curve
  cairo_set_line_width(cr, 2.);
  cairo_set_source_rgb(cr, .9, .9, .9);
  // cairo_set_line_cap  (cr, CAIRO_LINE_CAP_SQUARE);
  cairo_move_to(cr, 0, - height*c->draw_ys[0]);
  for(int k=1;k<DT_IOP_EQUALIZER_RES;k++) cairo_line_to(cr, k*width/(float)DT_IOP_EQUALIZER_RES, - height*c->draw_ys[k]);
  cairo_stroke(cr);

  cairo_destroy(cr);
  cairo_t *cr_pixmap = gdk_cairo_create(gtk_widget_get_window(widget));
  cairo_set_source_surface (cr_pixmap, cst, 0, 0);
  cairo_paint(cr_pixmap);
  cairo_destroy(cr_pixmap);
  cairo_surface_destroy(cst);
  return TRUE;
}

gboolean dt_iop_equalizer_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_equalizer_gui_data_t *c = (dt_iop_equalizer_gui_data_t *)self->gui_data;
  dt_iop_equalizer_params_t *p = (dt_iop_equalizer_params_t *)self->params;
  const int inset = DT_GUI_EQUALIZER_INSET;
  int height = widget->allocation.height - 2*inset, width = widget->allocation.width - 2*inset;
  if(!c->dragging) c->mouse_x = CLAMP(event->x - inset, 0, width)/(float)width;
  c->mouse_y = 1.0 - CLAMP(event->y - inset, 0, height)/(float)height;
  if(c->dragging)
  {
    dt_iop_equalizer_get_params(p, c->mouse_x, c->mouse_y, c->mouse_radius);
    dt_dev_add_history_item(darktable.develop, self);
  }
  gtk_widget_queue_draw(widget);
  gint x, y;
  gdk_window_get_pointer(event->window, &x, &y, NULL);
  return TRUE;
}

gboolean dt_iop_equalizer_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{ // set active point
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_equalizer_gui_data_t *c = (dt_iop_equalizer_gui_data_t *)self->gui_data;
  c->dragging = 1;
  return TRUE;
}

gboolean dt_iop_equalizer_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_equalizer_gui_data_t *c = (dt_iop_equalizer_gui_data_t *)self->gui_data;
  c->dragging = 0;
  return TRUE;
}

gboolean dt_iop_equalizer_scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_equalizer_gui_data_t *c = (dt_iop_equalizer_gui_data_t *)self->gui_data;
  if(event->direction == GDK_SCROLL_UP   && c->mouse_radius > 1.0/DT_IOP_EQUALIZER_BANDS) c->mouse_radius *= 0.7;
  if(event->direction == GDK_SCROLL_DOWN && c->mouse_radius < 1.0) c->mouse_radius *= 1.42;
  gtk_widget_queue_draw(widget);
  return TRUE;
}
