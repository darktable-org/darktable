/*
    This file is part of darktable,
    copyright (c) 2011 Henrik Andersson.

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

#include "common/ratings.h"
#include "common/debug.h"
#include "control/control.h"
#include "libs/lib.h"
#include "gui/gtk.h"
#include "gui/draw.h"
#include "dtgtk/button.h"

DT_MODULE(1)

typedef struct dt_lib_ratings_t
{
  gint current;
  gint pointerx;
  gint pointery;
}
dt_lib_ratings_t;

/* redraw the ratings */
static gboolean _lib_ratings_expose_callback(GtkWidget *widget, GdkEventExpose *event, gpointer user_data);
/* motion notify handler*/
static gboolean _lib_ratings_motion_notify_callback(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);
/* motion leavel handler */
static gboolean _lib_ratings_leave_notify_callback(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);
/* button press handler */
static gboolean _lib_ratings_button_press_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
/* button release handler */
static gboolean _lib_ratings_button_release_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data);

const char* name()
{
  return _("ratings");
}

uint32_t views()
{
  return DT_VIEW_LIGHTTABLE | DT_VIEW_TETHERING;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_LEFT;
}

int expandable()
{
  return 0;
}

int position()
{
  return 1002;
}

#define STAR_SIZE DT_PIXEL_APPLY_DPI(12)
#define STAR_SPACING DT_PIXEL_APPLY_DPI(6)

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_ratings_t *d = (dt_lib_ratings_t *)g_malloc0(sizeof(dt_lib_ratings_t));
  self->data = (void *)d;

  /* create a centered drawing area within alignment */
  self->widget = gtk_alignment_new(0.5, 0.5, 0, 0);

  GtkWidget *da = gtk_drawing_area_new();
  gtk_widget_set_events(da,
                        GDK_EXPOSURE_MASK
                        | GDK_POINTER_MOTION_MASK
                        | GDK_POINTER_MOTION_HINT_MASK
                        | GDK_LEAVE_NOTIFY_MASK
                        | GDK_BUTTON_PRESS_MASK
                        | GDK_BUTTON_RELEASE_MASK
                        | GDK_STRUCTURE_MASK);

  /* connect callbacks */
  gtk_widget_set_double_buffered(da, FALSE);
  gtk_widget_set_app_paintable(da, TRUE);
  g_signal_connect (G_OBJECT (da), "expose-event",
                    G_CALLBACK (_lib_ratings_expose_callback), self);
  g_signal_connect (G_OBJECT (da), "button-press-event",
                    G_CALLBACK (_lib_ratings_button_press_callback), self);
  g_signal_connect (G_OBJECT (da), "button-release-event",
                    G_CALLBACK (_lib_ratings_button_release_callback), self);
  g_signal_connect (G_OBJECT (da), "motion-notify-event",
                    G_CALLBACK (_lib_ratings_motion_notify_callback), self);
  g_signal_connect (G_OBJECT (da), "leave-notify-event",
                    G_CALLBACK (_lib_ratings_leave_notify_callback), self);

  /* set size of navigation draw area */
  gtk_widget_set_size_request(da, (STAR_SIZE*6)+(STAR_SPACING*5), STAR_SIZE);

  gtk_container_add(GTK_CONTAINER(self->widget),da);

}

void gui_cleanup(dt_lib_module_t *self)
{
  g_free(self->data);
  self->data = NULL;
}

static gboolean _lib_ratings_expose_callback(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_ratings_t *d = (dt_lib_ratings_t *)self->data;

  if(!darktable.control->running) return TRUE;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;

  /* get current style */
  GtkStyle *style=gtk_rc_get_style_by_paths(gtk_settings_get_default(), NULL,"GtkWidget", GTK_TYPE_WIDGET);
  if(!style) style = gtk_rc_get_style(widget);
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  /* fill background */
  cairo_set_source_rgb(cr, style->bg[0].red/65535.0, style->bg[0].green/65535.0, style->bg[0].blue/65535.0);
  cairo_paint(cr);

  /* lets draw stars */
  int x=0;
  cairo_set_line_width(cr, 1.5);
  cairo_set_source_rgba(cr, style->fg[0].red/65535.0, style->fg[0].green/65535.0, style->fg[0].blue/65535.0, 0.8);
  d->current = 0;
  for(int k=0; k<5; k++)
  {
    /* outline star */
    dt_draw_star(cr, STAR_SIZE/2.0+x, STAR_SIZE/2.0, STAR_SIZE/2.0, STAR_SIZE/4.0);
    if(x < d->pointerx)
    {
      cairo_fill_preserve(cr);
      cairo_set_source_rgba(cr, style->fg[0].red/65535.0, style->fg[0].green/65535.0, style->fg[0].blue/65535.0, 0.5);
      cairo_stroke(cr);
      cairo_set_source_rgba(cr, style->fg[0].red/65535.0, style->fg[0].green/65535.0, style->fg[0].blue/65535.0, 0.8);
      if((k+1) > d->current) d->current = (k+1);
    }
    else
      cairo_stroke(cr);
    x+=STAR_SIZE+STAR_SPACING;
  }

  /* blit memsurface onto widget*/
  cairo_destroy(cr);
  cairo_t *cr_pixmap = gdk_cairo_create(gtk_widget_get_window(widget));
  cairo_set_source_surface (cr_pixmap, cst, 0, 0);
  cairo_paint(cr_pixmap);
  cairo_destroy(cr_pixmap);
  cairo_surface_destroy(cst);

  return TRUE;
}

static gboolean _lib_ratings_motion_notify_callback(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_ratings_t *d = (dt_lib_ratings_t *)self->data;
  gdk_window_get_pointer(event->window, &d->pointerx, &d->pointery, NULL);
  gtk_widget_queue_draw(self->widget);
  return TRUE;
}

static gboolean _lib_ratings_button_press_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_ratings_t *d = (dt_lib_ratings_t *)self->data;
  if (d->current>0)
  {
    int32_t mouse_over_id;

    mouse_over_id = dt_view_get_image_to_act_on();

    if (mouse_over_id <= 0)
    {
      dt_ratings_apply_to_selection(d->current);
    }
    else
    {
      dt_ratings_apply_to_image(mouse_over_id, d->current);
      //dt_control_log(ngettext("applying rating %d to %d image", "applying rating %d to %d images", 1), d->current, 1); //FIXME: Change the message after release
    }

    dt_control_queue_redraw_center();
  }
  return TRUE;
}

static gboolean _lib_ratings_button_release_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  /*  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
    self=NULL;*/
  return TRUE;
}

static gboolean _lib_ratings_leave_notify_callback(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_ratings_t *d = (dt_lib_ratings_t *)self->data;
  d->pointery = d->pointerx = 0;
  gtk_widget_queue_draw(self->widget);
  return TRUE;
}



// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
