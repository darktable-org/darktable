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
#include "iop/colorcorrection.h"
#include "develop/develop.h"
#include "control/control.h"
#include "gui/gtk.h"
#include "develop/imageop.h"

#define DT_COLORCORRECTION_INSET 5
#define DT_COLORCORRECTION_MAX 0.1

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, int x, int y, float scale, int width, int height)
{
  dt_iop_colorcorrection_data_t *d = (dt_iop_colorcorrection_data_t *)piece->data;
  float *in  = (float *)i;
  float *out = (float *)o;
  for(int k=0;k<width*height;k++)
  {
    out[0] = in[0];
    out[1] = d->saturation*(in[1] + in[0] * d->a_scale + d->a_base);
    out[2] = d->saturation*(in[2] + in[0] * d->b_scale + d->b_base);
    out += 3; in += 3;
  }
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorcorrection_params_t *p = (dt_iop_colorcorrection_params_t *)p1;
#ifdef HAVE_GEGL
  // pull in new params to gegl
  gegl_node_set(piece->input, "high_a_delta", p->hia, "high_b_delta", p->hib, "low_a_delta", p->loa, "low_b_delta", p->lob, "saturation", p->saturation, NULL);
#else
  dt_iop_colorcorrection_data_t *d = (dt_iop_colorcorrection_data_t *)piece->data;
  d->a_scale = p->hia - p->loa;
  d->a_base  = p->loa;
  d->b_scale = p->hib - p->lob;
  d->b_base  = p->lob;
  d->saturation = p->saturation;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
  dt_iop_colorcorrection_params_t *default_params = (dt_iop_colorcorrection_params_t *)self->default_params;
  piece->input = piece->output = gegl_node_new_child(pipe->gegl, "operation", "gegl:whitebalance", "high_a_delta", default_params->hia, "high_b_delta", default_params->hib, "low_a_delta", default_params->loa, "low_b_delta", default_params->lob, "saturation", default_params->saturation, NULL);
#else
  piece->data = malloc(sizeof(dt_iop_colorcorrection_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
#endif
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // clean up everything again.
  (void)gegl_node_remove_child(pipe->gegl, piece->input);
  // no free necessary, no data is alloc'ed
#else
  free(piece->data);
#endif
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_colorcorrection_gui_data_t *g = (dt_iop_colorcorrection_gui_data_t *)self->gui_data;
  dt_iop_colorcorrection_params_t *p = (dt_iop_colorcorrection_params_t *)module->params;
  gtk_range_set_value(GTK_RANGE(g->scale1), p->loa);
  gtk_range_set_value(GTK_RANGE(g->scale2), p->hia);
  gtk_range_set_value(GTK_RANGE(g->scale3), p->lob);
  gtk_range_set_value(GTK_RANGE(g->scale4), p->hib);
  gtk_range_set_value(GTK_RANGE(g->scale5), p->saturation);
  gtk_widget_queue_draw(self->widget);
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_colorcorrection_data_t));
  module->params = malloc(sizeof(dt_iop_colorcorrection_params_t));
  module->default_params = malloc(sizeof(dt_iop_colorcorrection_params_t));
  module->default_enabled = 0;
  module->params_size = sizeof(dt_iop_colorcorrection_params_t);
  module->gui_data = NULL;
  dt_iop_colorcorrection_params_t tmp = (dt_iop_colorcorrection_params_t){0., 0., 0., 0., 1.0};
  memcpy(module->params, &tmp, sizeof(dt_iop_colorcorrection_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_colorcorrection_params_t));
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
  self->gui_data = malloc(sizeof(dt_iop_colorcorrection_gui_data_t));
  dt_iop_colorcorrection_gui_data_t *g = (dt_iop_colorcorrection_gui_data_t *)self->gui_data;
  dt_iop_colorcorrection_params_t *p = (dt_iop_colorcorrection_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_vbox_new(FALSE, 0));
  g->area = GTK_DRAWING_AREA(gtk_drawing_area_new());
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->area), FALSE, FALSE, 5);
  gtk_drawing_area_size(g->area, 195, 195);
  gtk_widget_add_events(GTK_WIDGET(g->area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect (G_OBJECT (g->area), "expose-event",
                    G_CALLBACK (dt_iop_colorcorrection_expose), self);
  g_signal_connect (G_OBJECT (g->area), "button-press-event",
                    G_CALLBACK (dt_iop_colorcorrection_button_press), self);
  g_signal_connect (G_OBJECT (g->area), "button-release-event",
                    G_CALLBACK (dt_iop_colorcorrection_button_release), self);
  g_signal_connect (G_OBJECT (g->area), "motion-notify-event",
                    G_CALLBACK (dt_iop_colorcorrection_motion_notify), self);
  g_signal_connect (G_OBJECT (g->area), "leave-notify-event",
                    G_CALLBACK (dt_iop_colorcorrection_leave_notify), self);
  
  g->hbox = GTK_HBOX(gtk_hbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->hbox), FALSE, FALSE, 5);
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(g->hbox), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(g->hbox), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  g->label1 = GTK_LABEL(gtk_label_new("a low"));
  g->label2 = GTK_LABEL(gtk_label_new("a high"));
  g->label3 = GTK_LABEL(gtk_label_new("b low"));
  g->label4 = GTK_LABEL(gtk_label_new("b high"));
  g->label5 = GTK_LABEL(gtk_label_new("saturation"));
  gtk_misc_set_alignment(GTK_MISC(g->label1), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label2), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label3), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label4), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label5), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label3), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label4), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label5), TRUE, TRUE, 0);
  g->scale1 = GTK_HSCALE(gtk_hscale_new_with_range(-DT_COLORCORRECTION_MAX, DT_COLORCORRECTION_MAX, 0.001));
  g->scale2 = GTK_HSCALE(gtk_hscale_new_with_range(-DT_COLORCORRECTION_MAX, DT_COLORCORRECTION_MAX, 0.001));
  g->scale3 = GTK_HSCALE(gtk_hscale_new_with_range(-DT_COLORCORRECTION_MAX, DT_COLORCORRECTION_MAX, 0.001));
  g->scale4 = GTK_HSCALE(gtk_hscale_new_with_range(-DT_COLORCORRECTION_MAX, DT_COLORCORRECTION_MAX, 0.001));
  g->scale5 = GTK_HSCALE(gtk_hscale_new_with_range(-3.0, 3.0, 0.01));
  gtk_scale_set_digits(GTK_SCALE(g->scale1), 3);
  gtk_scale_set_digits(GTK_SCALE(g->scale2), 3);
  gtk_scale_set_digits(GTK_SCALE(g->scale3), 3);
  gtk_scale_set_digits(GTK_SCALE(g->scale4), 3);
  gtk_scale_set_digits(GTK_SCALE(g->scale5), 2);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale1), GTK_POS_LEFT);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale2), GTK_POS_LEFT);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale3), GTK_POS_LEFT);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale4), GTK_POS_LEFT);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale5), GTK_POS_LEFT);
  gtk_range_set_value(GTK_RANGE(g->scale1), p->loa);
  gtk_range_set_value(GTK_RANGE(g->scale2), p->hia);
  gtk_range_set_value(GTK_RANGE(g->scale3), p->lob);
  gtk_range_set_value(GTK_RANGE(g->scale4), p->hib);
  gtk_range_set_value(GTK_RANGE(g->scale5), p->saturation);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale3), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale4), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale5), TRUE, TRUE, 0);


  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (loa_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
                    G_CALLBACK (hia_callback), self);
  g_signal_connect (G_OBJECT (g->scale3), "value-changed",
                    G_CALLBACK (lob_callback), self);
  g_signal_connect (G_OBJECT (g->scale4), "value-changed",
                    G_CALLBACK (hib_callback), self);
  g_signal_connect (G_OBJECT (g->scale5), "value-changed",
                    G_CALLBACK (sat_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

void loa_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  // dt_iop_colorcorrection_gui_data_t *g = (dt_iop_colorcorrection_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorcorrection_params_t *p = (dt_iop_colorcorrection_params_t *)self->params;
  p->loa = gtk_range_get_value(range);
  // float hia = gtk_range_get_value(GTK_RANGE(g->scale2));
  // if(p->loa > hia) gtk_range_set_value(GTK_RANGE(g->scale2), p->loa);
  dt_dev_add_history_item(darktable.develop, self);
  gtk_widget_queue_draw(self->widget);
}

void hia_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  // dt_iop_colorcorrection_gui_data_t *g = (dt_iop_colorcorrection_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorcorrection_params_t *p = (dt_iop_colorcorrection_params_t *)self->params;
  p->hia = gtk_range_get_value(range);
  // float loa = gtk_range_get_value(GTK_RANGE(g->scale1));
  // if(loa > p->hia) gtk_range_set_value(GTK_RANGE(g->scale1), p->hia);
  dt_dev_add_history_item(darktable.develop, self);
  gtk_widget_queue_draw(self->widget);
}

void lob_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  // dt_iop_colorcorrection_gui_data_t *g = (dt_iop_colorcorrection_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorcorrection_params_t *p = (dt_iop_colorcorrection_params_t *)self->params;
  p->lob = gtk_range_get_value(range);
  // float hib = gtk_range_get_value(GTK_RANGE(g->scale4));
  // if(p->lob > hib) gtk_range_set_value(GTK_RANGE(g->scale4), p->lob);
  dt_dev_add_history_item(darktable.develop, self);
  gtk_widget_queue_draw(self->widget);
}

void hib_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  // dt_iop_colorcorrection_gui_data_t *g = (dt_iop_colorcorrection_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorcorrection_params_t *p = (dt_iop_colorcorrection_params_t *)self->params;
  p->hib = gtk_range_get_value(range);
  // float lob = gtk_range_get_value(GTK_RANGE(g->scale3));
  // if(lob > p->hib) gtk_range_set_value(GTK_RANGE(g->scale3), p->hib);
  dt_dev_add_history_item(darktable.develop, self);
  gtk_widget_queue_draw(self->widget);
}

void sat_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorcorrection_params_t *p = (dt_iop_colorcorrection_params_t *)self->params;
  p->saturation = gtk_range_get_value(range);
  dt_dev_add_history_item(darktable.develop, self);
  gtk_widget_queue_draw(self->widget);
}

gboolean dt_iop_colorcorrection_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorcorrection_params_t *p = (dt_iop_colorcorrection_params_t *)self->params;
  // dt_iop_colorcorrection_gui_data_t *g = (dt_iop_colorcorrection_gui_data_t *)self->gui_data;

  const int inset = DT_COLORCORRECTION_INSET;
  int width = widget->allocation.width, height = widget->allocation.height;
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  // clear bg
  cairo_set_source_rgb (cr, .2, .2, .2);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2*inset; height -= 2*inset;
  // flip y:
  cairo_translate(cr, 0, height);
  cairo_scale(cr, 1., -1.);
  const int cells = 8;
  for(int j=0;j<cells;j++) for(int i=0;i<cells;i++)
  {
    float yuv[3] = {0, 0, 0}, rgb[3] = {0.5, 0.5, 0.5};
    dt_iop_RGB_to_YCbCr(rgb, yuv); // get grey in yuv
    yuv[1] = p->saturation*(yuv[1] + yuv[0] * 4.0*DT_COLORCORRECTION_MAX*(i/(cells-1.0) - .5));
    yuv[2] = p->saturation*(yuv[2] + yuv[0] * 4.0*DT_COLORCORRECTION_MAX*(j/(cells-1.0) - .5));
    dt_iop_YCbCr_to_RGB(yuv, rgb); // get grey in yuv
    cairo_set_source_rgb (cr, rgb[0], rgb[1], rgb[2]);
    cairo_rectangle(cr, width*i/(float)cells, height*j/(float)cells, width/(float)cells-1, height/(float)cells-1);
    cairo_fill(cr);
  }
  const float loa = .5f*(width + width*p->loa/(float)DT_COLORCORRECTION_MAX),
              hia = .5f*(width + width*p->hia/(float)DT_COLORCORRECTION_MAX),
              lob = .5f*(height + height*p->lob/(float)DT_COLORCORRECTION_MAX),
              hib = .5f*(height + height*p->hib/(float)DT_COLORCORRECTION_MAX);
  cairo_rectangle(cr, loa, lob, hia-loa, hib-lob);
  cairo_set_source_rgb(cr, .9, .9, .9);
  cairo_set_line_width(cr, 2.);
  cairo_stroke(cr);

  cairo_destroy(cr);
  cairo_t *cr_pixmap = gdk_cairo_create(gtk_widget_get_window(widget));
  cairo_set_source_surface (cr_pixmap, cst, 0, 0);
  cairo_paint(cr_pixmap);
  cairo_destroy(cr_pixmap);
  cairo_surface_destroy(cst);
  return TRUE;
}

gboolean dt_iop_colorcorrection_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  return TRUE;
}

gboolean dt_iop_colorcorrection_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  return TRUE;
}

gboolean dt_iop_colorcorrection_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  return TRUE;
}

gboolean dt_iop_colorcorrection_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  return TRUE;
}

