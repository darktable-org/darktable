/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#include "common/colorspaces.h"
#include "iop/colorcorrection.h"
#include "develop/develop.h"
#include "control/control.h"
#include "gui/gtk.h"
#include "develop/imageop.h"
#include "dtgtk/resetlabel.h"

DT_MODULE(1)

#define DT_COLORCORRECTION_INSET 5
#define DT_COLORCORRECTION_MAX 40.

const char *name()
{
  return _("color correction");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES|IOP_FLAGS_SUPPORTS_BLENDING;
}

int
groups ()
{
  return IOP_GROUP_COLOR;
}

void init_key_accels(dt_iop_module_t *self)
{
//  dtgtk_slider_init_accel(darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/colorcorrection/saturation");
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_colorcorrection_data_t *d = (dt_iop_colorcorrection_data_t *)piece->data;
  float *in  = (float *)i;
  float *out = (float *)o;
  const int ch = piece->colors;
  for(int k=0; k<roi_out->width*roi_out->height; k++)
  {
    out[0] = in[0];
    out[1] = d->saturation*(in[1] + in[0] * d->a_scale + d->a_base);
    out[2] = d->saturation*(in[2] + in[0] * d->b_scale + d->b_base);
    out += ch;
    in += ch;
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
  d->a_scale = (p->hia - p->loa)/100.0;
  d->a_base  = p->loa;
  d->b_scale = (p->hib - p->lob)/100.0;
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
  dtgtk_slider_set_value(g->scale5, p->saturation);
  gtk_widget_queue_draw(self->widget);
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_colorcorrection_data_t));
  module->params = malloc(sizeof(dt_iop_colorcorrection_params_t));
  module->default_params = malloc(sizeof(dt_iop_colorcorrection_params_t));
  module->default_enabled = 0;
  module->priority = 630; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_colorcorrection_params_t);
  module->gui_data = NULL;
  dt_iop_colorcorrection_params_t tmp = (dt_iop_colorcorrection_params_t)
  {
    0., 0., 0., 0., 1.0
  };
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

static void sat_callback (GtkDarktableSlider *slider, gpointer user_data);
static gboolean dt_iop_colorcorrection_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data);
static gboolean dt_iop_colorcorrection_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);
static gboolean dt_iop_colorcorrection_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean dt_iop_colorcorrection_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean dt_iop_colorcorrection_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);
static gboolean dt_iop_colorcorrection_scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data);

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_colorcorrection_gui_data_t));
  dt_iop_colorcorrection_gui_data_t *g = (dt_iop_colorcorrection_gui_data_t *)self->gui_data;
  dt_iop_colorcorrection_params_t *p = (dt_iop_colorcorrection_params_t *)self->params;

  g->selected = g->dragging = 0;
  g->press_x = g->press_y = -1;

  self->widget = GTK_WIDGET(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  g->area = GTK_DRAWING_AREA(gtk_drawing_area_new());
  GtkWidget *asp = gtk_aspect_frame_new(NULL, 0.5, 0.5, 1.0, TRUE);
  gtk_box_pack_start(GTK_BOX(self->widget), asp, TRUE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(asp), GTK_WIDGET(g->area));
  gtk_drawing_area_size(g->area, 258, 258);
  g_object_set (GTK_OBJECT(g->area), "tooltip-text", _("draw a rectangle to give a tint"), (char *)NULL);

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
  g_signal_connect (G_OBJECT (g->area), "scroll-event",
                    G_CALLBACK (dt_iop_colorcorrection_scrolled), self);

  g->hbox = GTK_HBOX(gtk_hbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->hbox), TRUE, TRUE, 0);
  GtkWidget *vbox = gtk_vbox_new(FALSE,0);
  gtk_box_pack_start(GTK_BOX(g->hbox), GTK_WIDGET(vbox), TRUE, TRUE, 5);
  g->scale5 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,-3.0, 3.0, 0.01, p->saturation,2));
  g_object_set (GTK_OBJECT(g->scale5), "tooltip-text", _("set the global saturation"), (char *)NULL);
  dtgtk_slider_set_label(g->scale5,_("saturation"));
//  dtgtk_slider_set_accel(g->scale5,darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/colorcorrection/saturation");
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g->scale5), TRUE, TRUE, 0);


  g_signal_connect (G_OBJECT (g->scale5), "value-changed",
                    G_CALLBACK (sat_callback), self);
  g->hsRGB = dt_colorspaces_create_srgb_profile();
  g->hLab  = dt_colorspaces_create_lab_profile();
  g->xform = cmsCreateTransform(g->hLab, TYPE_Lab_DBL, g->hsRGB, TYPE_RGB_DBL,
                                INTENT_PERCEPTUAL, 0);//cmsFLAGS_NOTPRECALC);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_colorcorrection_gui_data_t *g = (dt_iop_colorcorrection_gui_data_t *)self->gui_data;
  dt_colorspaces_cleanup_profile(g->hsRGB);
  dt_colorspaces_cleanup_profile(g->hLab);
  cmsDeleteTransform(g->xform);
  free(self->gui_data);
  self->gui_data = NULL;
}

static void sat_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorcorrection_params_t *p = (dt_iop_colorcorrection_params_t *)self->params;
  p->saturation = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}

static gboolean dt_iop_colorcorrection_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorcorrection_gui_data_t *g = (dt_iop_colorcorrection_gui_data_t *)self->gui_data;
  dt_iop_colorcorrection_params_t *p  = (dt_iop_colorcorrection_params_t *)self->params;
  dt_iop_colorcorrection_params_t *p1 = &g->press_params;

  const int inset = DT_COLORCORRECTION_INSET;
  int width = widget->allocation.width, height = widget->allocation.height;
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  // clear bg
  cairo_set_source_rgb (cr, .2, .2, .2);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2*inset;
  height -= 2*inset;
  // flip y:
  cairo_translate(cr, 0, height);
  cairo_scale(cr, 1., -1.);
  const int cells = 8;
  for(int j=0; j<cells; j++) for(int i=0; i<cells; i++)
    {
      double rgb[3] = {0.5, 0.5, 0.5}; // Lab: rgb grey converted to Lab
      cmsCIELab Lab;
      Lab.L = 53.390011;
      Lab.a = Lab.b = 0; // grey
      // dt_iop_sRGB_to_Lab(rgb, Lab, 0, 0, 1.0, 1, 1); // get grey in Lab
      // printf("lab = %f %f %f\n", Lab[0], Lab[1], Lab[2]);
      Lab.a = p->saturation*(Lab.a + Lab.L * .05*DT_COLORCORRECTION_MAX*(i/(cells-1.0) - .5));
      Lab.b = p->saturation*(Lab.b + Lab.L * .05*DT_COLORCORRECTION_MAX*(j/(cells-1.0) - .5));
      cmsDoTransform(g->xform, &Lab, rgb, 1);
      // dt_iop_Lab_to_sRGB(Lab, rgb, 0, 0, 1.0, 1, 1);
      cairo_set_source_rgb (cr, rgb[0], rgb[1], rgb[2]);
      cairo_rectangle(cr, width*i/(float)cells, height*j/(float)cells, width/(float)cells-1, height/(float)cells-1);
      cairo_fill(cr);
    }
  float loa, hia, lob, hib;
  if(!g->dragging) p1 = p;
  loa = .5f*(width + width*p1->loa/(float)DT_COLORCORRECTION_MAX);
  hia = .5f*(width + width*p1->hia/(float)DT_COLORCORRECTION_MAX);
  lob = .5f*(height + height*p1->lob/(float)DT_COLORCORRECTION_MAX);
  hib = .5f*(height + height*p1->hib/(float)DT_COLORCORRECTION_MAX);
  cairo_set_line_width(cr, 2.);
  if(g->dragging)
  {
    cairo_rectangle(cr, loa, lob, hia-loa, hib-lob);
    if(g->selected & 1) loa = /*MIN(g->selected < 0xf ? hia :  INFINITY,*/ loa + g->mouse_x-g->press_x;//);
    if(g->selected & 2) lob = /*MIN(g->selected < 0xf ? hib :  INFINITY,*/ lob + g->mouse_y-g->press_y;//);
    if(g->selected & 4) hia = /*MAX(g->selected < 0xf ? loa : -INFINITY,*/ hia + g->mouse_x-g->press_x;//);
    if(g->selected & 8) hib = /*MAX(g->selected < 0xf ? lob : -INFINITY,*/ hib + g->mouse_y-g->press_y;//);
    p->loa = (2.0*loa - width) *DT_COLORCORRECTION_MAX/(float)width;
    p->hia = (2.0*hia - width) *DT_COLORCORRECTION_MAX/(float)width;
    p->lob = (2.0*lob - height)*DT_COLORCORRECTION_MAX/(float)height;
    p->hib = (2.0*hib - height)*DT_COLORCORRECTION_MAX/(float)height;
  }
  else
  {
    cairo_set_source_rgb(cr, .1, .1, .1);
    cairo_move_to(cr, loa, hib);
    cairo_line_to(cr, loa, lob);
    cairo_line_to(cr, hia, lob);
    cairo_stroke(cr);
    cairo_set_source_rgb(cr, .9, .9, .9);
    cairo_move_to(cr, hia, lob);
    cairo_line_to(cr, hia, hib);
    cairo_line_to(cr, loa, hib);
    cairo_stroke(cr);
    cairo_rectangle(cr, loa, lob, hia-loa, hib-lob);
    if(g->selected & 1) loa = loa < hia ? loa-7 : loa+7;
    if(g->selected & 2) lob = lob < hib ? lob-7 : lob+7;
    if(g->selected & 4) hia = loa < hia ? hia+7 : hia-7;
    if(g->selected & 8) hib = lob < hib ? hib+7 : hib-7;
  }
  cairo_set_fill_rule (cr, CAIRO_FILL_RULE_EVEN_ODD);
  cairo_set_source_rgba(cr, .9, .9, .9, .5);
  cairo_rectangle(cr, loa, lob, hia-loa, hib-lob);
  cairo_fill_preserve(cr);
  cairo_stroke(cr);
  if(g->dragging)
    dt_dev_add_history_item(darktable.develop, self, TRUE);

  cairo_destroy(cr);
  cairo_t *cr_pixmap = gdk_cairo_create(gtk_widget_get_window(widget));
  cairo_set_source_surface (cr_pixmap, cst, 0, 0);
  cairo_paint(cr_pixmap);
  cairo_destroy(cr_pixmap);
  cairo_surface_destroy(cst);
  return TRUE;
}

static gboolean dt_iop_colorcorrection_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorcorrection_gui_data_t *g = (dt_iop_colorcorrection_gui_data_t *)self->gui_data;
  dt_iop_colorcorrection_params_t *p = (dt_iop_colorcorrection_params_t *)self->params;
  const int inset = DT_COLORCORRECTION_INSET;
  int width = widget->allocation.width - 2*inset, height = widget->allocation.height - 2*inset;
  g->mouse_x = CLAMP(event->x - inset, 0, width);
  g->mouse_y = CLAMP(height - 1 - event->y + inset, 0, height);
  if(!g->dragging)
  {
    g->press_x = g->mouse_x;
    g->press_y = g->mouse_y;
    const float loa = .5f*(width + width*p->loa/(float)DT_COLORCORRECTION_MAX),
                hia = .5f*(width + width*p->hia/(float)DT_COLORCORRECTION_MAX),
                lob = .5f*(height + height*p->lob/(float)DT_COLORCORRECTION_MAX),
                hib = .5f*(height + height*p->hib/(float)DT_COLORCORRECTION_MAX);
    g->selected = 0;
    if(loa <= hia)
    {
      if(g->press_x <= loa) g->selected |= 1;
      if(g->press_x >= hia) g->selected |= 4;
    }
    else
    {
      if(g->press_x <= hia) g->selected |= 4;
      if(g->press_x >= loa) g->selected |= 1;
    }
    if(lob <= hib)
    {
      if(g->press_y <= lob) g->selected |= 2;
      if(g->press_y >= hib) g->selected |= 8;
    }
    else
    {
      if(g->press_y <= hib) g->selected |= 8;
      if(g->press_y >= lob) g->selected |= 2;
    }
    if(g->press_x > MIN(loa, hia) && g->press_x < MAX(hia,loa) && g->press_y > MIN(lob,hib) && g->press_y < MAX(hib,lob)) g->selected = 0xf;
    g->press_params = *p;
  }
  gtk_widget_queue_draw(self->widget);
  gint x, y;
  gdk_window_get_pointer(event->window, &x, &y, NULL);
  return TRUE;
}

static gboolean dt_iop_colorcorrection_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(event->button == 1)
  {
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    dt_iop_colorcorrection_gui_data_t *g = (dt_iop_colorcorrection_gui_data_t *)self->gui_data;
    g->dragging = 1;
    return TRUE;
  }
  return FALSE;
}

static gboolean dt_iop_colorcorrection_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(event->button == 1)
  {
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    dt_iop_colorcorrection_gui_data_t *g = (dt_iop_colorcorrection_gui_data_t *)self->gui_data;
    g->dragging = 0;
    return TRUE;
  }
  return FALSE;
}

static gboolean dt_iop_colorcorrection_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorcorrection_gui_data_t *g = (dt_iop_colorcorrection_gui_data_t *)self->gui_data;
  g->selected = g->dragging = 0;
  gtk_widget_queue_draw(self->widget);
  return TRUE;
}

static gboolean dt_iop_colorcorrection_scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorcorrection_gui_data_t *g = (dt_iop_colorcorrection_gui_data_t *)self->gui_data;
  dt_iop_colorcorrection_params_t *p = (dt_iop_colorcorrection_params_t *)self->params;
  if(event->direction == GDK_SCROLL_UP   && p->saturation > -3.0) p->saturation -= 0.1;
  if(event->direction == GDK_SCROLL_DOWN && p->saturation <  3.0) p->saturation += 0.1;
  dtgtk_slider_set_value(g->scale5, p->saturation);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
