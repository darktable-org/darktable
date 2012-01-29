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
#include "common/colorspaces.h"
#include "develop/develop.h"
#include "control/control.h"
#include "dtgtk/slider.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "develop/imageop.h"

DT_MODULE(1)

#define DT_COLORCORRECTION_INSET 5
#define DT_COLORCORRECTION_MAX 40.

typedef struct dt_iop_monochrome_params_t
{
  float a, b, size;
}
dt_iop_monochrome_params_t;

typedef struct dt_iop_monochrome_gui_data_t
{
  GtkDrawingArea *area;
  int dragging;
  cmsHPROFILE hsRGB;
  cmsHPROFILE hLab;
  cmsHTRANSFORM xform;
}
dt_iop_monochrome_gui_data_t;

typedef struct dt_iop_monochrome_data_t
{
  float a, b, size;
}
dt_iop_monochrome_data_t;


const char *name()
{
  return _("monochrome");
}

int
groups ()
{
  return IOP_GROUP_EFFECT;
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

void init_presets (dt_iop_module_so_t *self)
{
  dt_iop_monochrome_params_t p;

  p.size = 2.3f;

  p.a = 32.0f;
  p.b = 64.0f;
  dt_gui_presets_add_generic(_("red filter"), self->op, self->version(), &p, sizeof(p), 1);

  // p.a = 64.0f;
  // p.b = -32.0f;
  // dt_gui_presets_add_generic(_("purple filter"), self->op, self->version(), &p, sizeof(p), 1);

  // p.a = -32.0f;
  // p.b = -64.0f;
  // dt_gui_presets_add_generic(_("blue filter"), self->op, self->version(), &p, sizeof(p), 1);

  // p.a = -64.0f;
  // p.b = 32.0f;
  // dt_gui_presets_add_generic(_("green filter"), self->op, self->version(), &p, sizeof(p), 1);
}

static float
color_filter(const float L, const float ai, const float bi, const float a, const float b, const float size)
{
  return L * dt_fast_expf(-CLAMPS(((ai-a)*(ai-a) + (bi-b)*(bi-b))/(2.0*size), 0.0f, 1.0f));
}


void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_monochrome_data_t *d = (dt_iop_monochrome_data_t *)piece->data;
  const int ch = piece->colors;
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_out, i, o, d) schedule(static)
#endif
  for(int k=0; k<roi_out->height; k++)
  {
    const float *in = ((float *)i) + ch*k*roi_out->width;
    float *out = ((float *)o) + ch*k*roi_out->width;
    for (int j=0; j<roi_out->width; j++,in+=ch,out+=ch)
    {
      out[0] = color_filter(in[0], in[1], in[2], d->a, d->b, d->size);
      out[1] = out[2] = 0.0f;
    }
  }
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_monochrome_params_t *p = (dt_iop_monochrome_params_t *)p1;
  dt_iop_monochrome_data_t *d = (dt_iop_monochrome_data_t *)piece->data;
  d->a = p->a;
  d->b = p->b;
  const float sigma = 128.0 * p->size;
  d->size = sigma*sigma;
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_monochrome_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}

void gui_update(struct dt_iop_module_t *self)
{
  gtk_widget_queue_draw(self->widget);
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_monochrome_data_t));
  module->params = malloc(sizeof(dt_iop_monochrome_params_t));
  module->default_params = malloc(sizeof(dt_iop_monochrome_params_t));
  module->default_enabled = 0;
  module->priority = 540; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_monochrome_params_t);
  module->gui_data = NULL;
  dt_iop_monochrome_params_t tmp = (dt_iop_monochrome_params_t)
  {
    0., 0., 1.
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_monochrome_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_monochrome_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

static gboolean dt_iop_monochrome_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_monochrome_gui_data_t *g = (dt_iop_monochrome_gui_data_t *)self->gui_data;
  dt_iop_monochrome_params_t *p  = (dt_iop_monochrome_params_t *)self->params;

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
  // clip region to inside:
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_clip(cr);
  // flip y:
  cairo_translate(cr, 0, height);
  cairo_scale(cr, 1., -1.);
  const int cells = 8;
  for(int j=0; j<cells; j++) for(int i=0; i<cells; i++)
    {
      double rgb[3] = {0.5, 0.5, 0.5};
      cmsCIELab Lab;
      Lab.L = 53.390011;
      Lab.a = Lab.b = 0; // grey
      // dt_iop_sRGB_to_Lab(rgb, Lab, 0, 0, 1.0, 1, 1); // get grey in Lab
      Lab.a = (Lab.a + Lab.L * .05*DT_COLORCORRECTION_MAX*(i/(cells-1.0) - .5));
      Lab.b = (Lab.b + Lab.L * .05*DT_COLORCORRECTION_MAX*(j/(cells-1.0) - .5));
      Lab.L = color_filter(Lab.L, Lab.a, Lab.b, p->a, p->b, 40*40*p->size*p->size);
      cmsDoTransform(g->xform, &Lab, rgb, 1);
      cairo_set_source_rgb (cr, rgb[0], rgb[1], rgb[2]);
      cairo_rectangle(cr, width*i/(float)cells, height*j/(float)cells, width/(float)cells-1, height/(float)cells-1);
      cairo_fill(cr);
    }
  cairo_set_source_rgb(cr, .7, .7, .7);
  const float x = p->a * width/128.0 + width * .5f, y = p->b * width/128.0 + width * .5f;
  cairo_arc(cr, x, y, width*.22f*p->size, 0, 2.0*M_PI);
  cairo_stroke(cr);

  if(g->dragging) dt_dev_add_history_item(darktable.develop, self, TRUE);

  cairo_destroy(cr);
  cairo_t *cr_pixmap = gdk_cairo_create(gtk_widget_get_window(widget));
  cairo_set_source_surface (cr_pixmap, cst, 0, 0);
  cairo_paint(cr_pixmap);
  cairo_destroy(cr_pixmap);
  cairo_surface_destroy(cst);
  return TRUE;
}

static gboolean dt_iop_monochrome_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_monochrome_gui_data_t *g = (dt_iop_monochrome_gui_data_t *)self->gui_data;
  dt_iop_monochrome_params_t *p = (dt_iop_monochrome_params_t *)self->params;
  if(g->dragging)
  {
    const int inset = DT_COLORCORRECTION_INSET;
    int width = widget->allocation.width - 2*inset, height = widget->allocation.height - 2*inset;
    const float mouse_x = CLAMP(event->x - inset, 0, width);
    const float mouse_y = CLAMP(height - 1 - event->y + inset, 0, height);
    p->a = 128.0f*(mouse_x - width  * 0.5f)/(float)width;
    p->b = 128.0f*(mouse_y - height * 0.5f)/(float)height;
    gtk_widget_queue_draw(self->widget);
  }
  gint x, y;
  gdk_window_get_pointer(event->window, &x, &y, NULL);
  return TRUE;
}

static gboolean dt_iop_monochrome_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(event->button == 1)
  {
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    dt_iop_monochrome_gui_data_t *g = (dt_iop_monochrome_gui_data_t *)self->gui_data;
    dt_iop_monochrome_params_t *p = (dt_iop_monochrome_params_t *)self->params;
    const int inset = DT_COLORCORRECTION_INSET;
    int width = widget->allocation.width - 2*inset, height = widget->allocation.height - 2*inset;
    const float mouse_x = CLAMP(event->x - inset, 0, width);
    const float mouse_y = CLAMP(height - 1 - event->y + inset, 0, height);
    p->a = 128.0f*(mouse_x - width  * 0.5f)/(float)width;
    p->b = 128.0f*(mouse_y - height * 0.5f)/(float)height;
    g->dragging = 1;
    gtk_widget_queue_draw(self->widget);
    return TRUE;
  }
  return FALSE;
}

static gboolean dt_iop_monochrome_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(event->button == 1)
  {
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    dt_iop_monochrome_gui_data_t *g = (dt_iop_monochrome_gui_data_t *)self->gui_data;
    g->dragging = 0;
    return TRUE;
  }
  return FALSE;
}

static gboolean dt_iop_monochrome_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_monochrome_gui_data_t *g = (dt_iop_monochrome_gui_data_t *)self->gui_data;
  g->dragging = 0;
  gtk_widget_queue_draw(self->widget);
  return TRUE;
}

static gboolean dt_iop_monochrome_scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_monochrome_params_t *p = (dt_iop_monochrome_params_t *)self->params;
  if(event->direction == GDK_SCROLL_UP   && p->size >   .5) p->size -= 0.1;
  if(event->direction == GDK_SCROLL_DOWN && p->size <  3.0) p->size += 0.1;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_monochrome_gui_data_t));
  dt_iop_monochrome_gui_data_t *g = (dt_iop_monochrome_gui_data_t *)self->gui_data;

  g->dragging = 0;

  self->widget = GTK_WIDGET(gtk_vbox_new(FALSE, 0));
  g->area = GTK_DRAWING_AREA(gtk_drawing_area_new());
  GtkWidget *asp = gtk_aspect_frame_new(NULL, 0.5, 0.5, 1.0, TRUE);
  gtk_box_pack_start(GTK_BOX(self->widget), asp, TRUE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(asp), GTK_WIDGET(g->area));
  gtk_drawing_area_size(g->area, 258, 258);
  g_object_set(G_OBJECT(g->area), "tooltip-text", _("drag and scroll mouse wheel to adjust the virtual color filter"), (char *)NULL);

  gtk_widget_add_events(GTK_WIDGET(g->area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect (G_OBJECT (g->area), "expose-event",
                    G_CALLBACK (dt_iop_monochrome_expose), self);
  g_signal_connect (G_OBJECT (g->area), "button-press-event",
                    G_CALLBACK (dt_iop_monochrome_button_press), self);
  g_signal_connect (G_OBJECT (g->area), "button-release-event",
                    G_CALLBACK (dt_iop_monochrome_button_release), self);
  g_signal_connect (G_OBJECT (g->area), "motion-notify-event",
                    G_CALLBACK (dt_iop_monochrome_motion_notify), self);
  g_signal_connect (G_OBJECT (g->area), "leave-notify-event",
                    G_CALLBACK (dt_iop_monochrome_leave_notify), self);
  g_signal_connect (G_OBJECT (g->area), "scroll-event",
                    G_CALLBACK (dt_iop_monochrome_scrolled), self);

  g->hsRGB = dt_colorspaces_create_srgb_profile();
  g->hLab  = dt_colorspaces_create_lab_profile();
  g->xform = cmsCreateTransform(g->hLab, TYPE_Lab_DBL, g->hsRGB, TYPE_RGB_DBL,
                                INTENT_PERCEPTUAL, 0);//cmsFLAGS_NOTPRECALC);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_monochrome_gui_data_t *g = (dt_iop_monochrome_gui_data_t *)self->gui_data;
  dt_colorspaces_cleanup_profile(g->hsRGB);
  dt_colorspaces_cleanup_profile(g->hLab);
  cmsDeleteTransform(g->xform);
  free(self->gui_data);
  self->gui_data = NULL;
}

