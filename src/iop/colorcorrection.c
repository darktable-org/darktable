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
#include "common/colorspaces.h"
#include "common/opencl.h"
#include "develop/develop.h"
#include "control/control.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "bauhaus/bauhaus.h"
#include "develop/imageop.h"

#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>

DT_MODULE(1)

#define DT_COLORCORRECTION_INSET 5
#define DT_COLORCORRECTION_MAX 40.

#define ROUNDUP(a, n)		((a) % (n) == 0 ? (a) : ((a) / (n) + 1) * (n))

typedef struct dt_iop_colorcorrection_params_t
{
  float hia, hib, loa, lob, saturation;
}
dt_iop_colorcorrection_params_t;

typedef struct dt_iop_colorcorrection_gui_data_t
{
  GtkDrawingArea *area;
  GtkWidget *slider;
  float press_x, press_y, mouse_x, mouse_y;
  int selected, dragging;
  dt_iop_colorcorrection_params_t press_params;
  cmsHPROFILE hsRGB;
  cmsHPROFILE hLab;
  cmsHTRANSFORM xform;
}
dt_iop_colorcorrection_gui_data_t;

typedef struct dt_iop_colorcorrection_data_t
{
  float a_scale, a_base, b_scale, b_base, saturation;
}
dt_iop_colorcorrection_data_t;

typedef struct dt_iop_colorcorrection_global_data_t
{
  int kernel_colorcorrection;
}
dt_iop_colorcorrection_global_data_t;

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

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "saturation"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_colorcorrection_gui_data_t *g =
    (dt_iop_colorcorrection_gui_data_t*)self->gui_data;
  dt_accel_connect_slider_iop(self, "saturation", GTK_WIDGET(g->slider));
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

#ifdef HAVE_OPENCL
int
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_colorcorrection_data_t *d = (dt_iop_colorcorrection_data_t *)piece->data;
  dt_iop_colorcorrection_global_data_t *gd = (dt_iop_colorcorrection_global_data_t *)self->data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;

  const int width = roi_out->width;
  const int height = roi_out->height;

  size_t sizes[2] = { ROUNDUP(width, 4), ROUNDUP(height, 4) };
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorcorrection, 0, sizeof(cl_mem), &dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorcorrection, 1, sizeof(cl_mem), &dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorcorrection, 2, sizeof(int), &width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorcorrection, 3, sizeof(int), &height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorcorrection, 4, sizeof(float), &d->saturation);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorcorrection, 5, sizeof(float), &d->a_scale);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorcorrection, 6, sizeof(float), &d->a_base);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorcorrection, 7, sizeof(float), &d->b_scale);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorcorrection, 8, sizeof(float), &d->b_base);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_colorcorrection, sizes);
  if(err != CL_SUCCESS) goto error;

  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_colorcorrection] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif


void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl from programs.conf
  dt_iop_colorcorrection_global_data_t *gd = (dt_iop_colorcorrection_global_data_t *)malloc(sizeof(dt_iop_colorcorrection_global_data_t));
  module->data = gd;
  gd->kernel_colorcorrection = dt_opencl_create_kernel(program, "colorcorrection");
}


void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_colorcorrection_global_data_t *gd = (dt_iop_colorcorrection_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_colorcorrection);
  free(module->data);
  module->data = NULL;
}


void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorcorrection_params_t *p = (dt_iop_colorcorrection_params_t *)p1;
  dt_iop_colorcorrection_data_t *d = (dt_iop_colorcorrection_data_t *)piece->data;
  d->a_scale = (p->hia - p->loa)/100.0;
  d->a_base  = p->loa;
  d->b_scale = (p->hib - p->lob)/100.0;
  d->b_base  = p->lob;
  d->saturation = p->saturation;
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_colorcorrection_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_colorcorrection_gui_data_t *g = (dt_iop_colorcorrection_gui_data_t *)self->gui_data;
  dt_iop_colorcorrection_params_t *p = (dt_iop_colorcorrection_params_t *)module->params;
  dt_bauhaus_slider_set(g->slider, p->saturation);
  gtk_widget_queue_draw(self->widget);
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_colorcorrection_data_t));
  module->params = malloc(sizeof(dt_iop_colorcorrection_params_t));
  module->default_params = malloc(sizeof(dt_iop_colorcorrection_params_t));
  module->default_enabled = 0;
  module->priority = 666; // module order created by iop_dependencies.py, do not edit!
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

static void sat_callback (GtkWidget *slider, gpointer user_data);
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

  self->widget = gtk_vbox_new(FALSE, DT_BAUHAUS_SPACE);
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

  g->slider = dt_bauhaus_slider_new_with_range(self, -3.0f, 3.0f, 0.01f, p->saturation, 2);
  gtk_box_pack_start(GTK_BOX(self->widget), g->slider, TRUE, TRUE, 0);
  g_object_set (GTK_OBJECT(g->slider), "tooltip-text", _("set the global saturation"), (char *)NULL);
  dt_bauhaus_widget_set_label(g->slider,_("saturation"));

  g_signal_connect (G_OBJECT (g->slider), "value-changed",
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

static void sat_callback (GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorcorrection_params_t *p = (dt_iop_colorcorrection_params_t *)self->params;
  p->saturation = dt_bauhaus_slider_get(slider);
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
  gtk_widget_queue_draw(self->widget);
  return TRUE;
}

static gboolean dt_iop_colorcorrection_scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorcorrection_gui_data_t *g = (dt_iop_colorcorrection_gui_data_t *)self->gui_data;
  dt_iop_colorcorrection_params_t *p = (dt_iop_colorcorrection_params_t *)self->params;
  if(event->direction == GDK_SCROLL_UP   && p->saturation > -3.0) p->saturation += 0.1;
  if(event->direction == GDK_SCROLL_DOWN && p->saturation <  3.0) p->saturation -= 0.1;
  dt_bauhaus_slider_set(g->slider, p->saturation);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
