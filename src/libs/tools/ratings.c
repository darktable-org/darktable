/*
    This file is part of darktable,
    Copyright (C) 2011-2021 darktable developers.

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
#include "common/collection.h"
#include "common/debug.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/accelerators.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

typedef struct dt_lib_ratings_t
{
  gint current;
  gint pointerx;
  gint pointery;
} dt_lib_ratings_t;

/* redraw the ratings */
static gboolean _lib_ratings_draw_callback(GtkWidget *widget, cairo_t *cr, gpointer user_data);
/* motion notify handler*/
static gboolean _lib_ratings_motion_notify_callback(GtkWidget *widget, GdkEventMotion *event,
                                                    gpointer user_data);
/* motion leavel handler */
static gboolean _lib_ratings_leave_notify_callback(GtkWidget *widget, GdkEventCrossing *event,
                                                   gpointer user_data);
/* button press handler */
static gboolean _lib_ratings_button_press_callback(GtkWidget *widget, GdkEventButton *event,
                                                   gpointer user_data);
/* button release handler */
static gboolean _lib_ratings_button_release_callback(GtkWidget *widget, GdkEventButton *event,
                                                     gpointer user_data);

const char *name(dt_lib_module_t *self)
{
  return _("Ratings");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"lighttable", "tethering", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_LEFT;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position(const dt_lib_module_t *self)
{
  return 1002;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_ratings_t *d = (dt_lib_ratings_t *)g_malloc0(sizeof(dt_lib_ratings_t));
  self->data = (void *)d;

  self->widget = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_widget_set_halign(self->widget, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(self->widget, GTK_ALIGN_CENTER);

  GtkWidget *drawing = gtk_drawing_area_new();

  gtk_widget_set_events(drawing, GDK_EXPOSURE_MASK     | GDK_POINTER_MOTION_MASK
                               | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK
                               | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                               | GDK_STRUCTURE_MASK);

  /* connect callbacks */
  gtk_widget_set_tooltip_text(drawing, _("Set star rating for selected images"));
  gtk_widget_set_app_paintable(drawing, TRUE);
  g_signal_connect(G_OBJECT(drawing), "draw", G_CALLBACK(_lib_ratings_draw_callback), self);
  g_signal_connect(G_OBJECT(drawing), "button-press-event", G_CALLBACK(_lib_ratings_button_press_callback), self);
  g_signal_connect(G_OBJECT(drawing), "button-release-event", G_CALLBACK(_lib_ratings_button_release_callback),
                   self);
  g_signal_connect(G_OBJECT(drawing), "motion-notify-event", G_CALLBACK(_lib_ratings_motion_notify_callback), self);
  g_signal_connect(G_OBJECT(drawing), "leave-notify-event", G_CALLBACK(_lib_ratings_leave_notify_callback), self);

  gtk_box_pack_start(GTK_BOX(self->widget), drawing, TRUE, TRUE, 0);

  /* set size of navigation draw area */
  gtk_widget_set_name(self->widget, "lib-rating-stars");
  dt_action_t *ac = dt_action_define(&darktable.control->actions_thumb, NULL, N_("Rating"), drawing, &dt_action_def_rating);
  dt_shortcut_register(ac, 0, 0, GDK_KEY_0, 0);
  dt_shortcut_register(ac, 1, 0, GDK_KEY_1, 0);
  dt_shortcut_register(ac, 2, 0, GDK_KEY_2, 0);
  dt_shortcut_register(ac, 3, 0, GDK_KEY_3, 0);
  dt_shortcut_register(ac, 4, 0, GDK_KEY_4, 0);
  dt_shortcut_register(ac, 5, 0, GDK_KEY_5, 0);
  dt_shortcut_register(ac, 6, 0, GDK_KEY_r, 0);

}

void gui_cleanup(dt_lib_module_t *self)
{
  g_free(self->data);
  self->data = NULL;
}

static gboolean _lib_ratings_draw_callback(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_ratings_t *d = (dt_lib_ratings_t *)self->data;

  if(!darktable.control->running) return TRUE;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);

  const float star_size = allocation.height;
  const float star_spacing = (allocation.width - 5.0 * star_size) / 4.0;

  cairo_surface_t *cst
      = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, allocation.width, allocation.height);
  cairo_t *cr = cairo_create(cst);

  GtkStyleContext *context = gtk_widget_get_style_context(widget);

  gtk_render_background(context, cr, 0, 0, allocation.width, allocation.height);

  /* get current style */
  GdkRGBA fg_color;
  gtk_style_context_get_color(context, gtk_widget_get_state_flags(widget), &fg_color);

  /* lets draw stars */
  int x = 0;
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1));
  gdk_cairo_set_source_rgba(cr, &fg_color);
  d->current = 0;
  for(int k = 0; k < 5; k++)
  {
    /* outline star */
    dt_draw_star(cr, star_size / 2.0 + x, star_size / 2.0, star_size / 2.0, star_size / (2.0 * 2.5));
    if(x < d->pointerx)
    {
      cairo_fill_preserve(cr);
      cairo_set_source_rgba(cr, fg_color.red, fg_color.green, fg_color.blue, fg_color.alpha * 0.5);
      cairo_stroke(cr);
      gdk_cairo_set_source_rgba(cr, &fg_color);
      if((k + 1) > d->current) d->current = darktable.control->element = (k + 1);
    }
    else
      cairo_stroke(cr);
    x += star_size + star_spacing;
  }

  /* blit memsurface onto widget*/
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);

  return TRUE;
}

static gboolean _lib_ratings_motion_notify_callback(GtkWidget *widget, GdkEventMotion *event,
                                                    gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_ratings_t *d = (dt_lib_ratings_t *)self->data;

  d->pointerx = event->x;
  d->pointery = event->y;
  gtk_widget_queue_draw(self->widget);
  return TRUE;
}

static gboolean _lib_ratings_button_press_callback(GtkWidget *widget, GdkEventButton *event,
                                                   gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_ratings_t *d = (dt_lib_ratings_t *)self->data;
  if(d->current > 0)
  {
    GList *imgs = dt_act_on_get_images(FALSE, TRUE, FALSE);
    dt_ratings_apply_on_list(imgs, d->current, TRUE);
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_RATING_RANGE, imgs);

    dt_control_queue_redraw_center();
  }
  return TRUE;
}

static gboolean _lib_ratings_button_release_callback(GtkWidget *widget, GdkEventButton *event,
                                                     gpointer user_data)
{
  return TRUE;
}

static gboolean _lib_ratings_leave_notify_callback(GtkWidget *widget, GdkEventCrossing *event,
                                                   gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_ratings_t *d = (dt_lib_ratings_t *)self->data;
  d->pointery = d->pointerx = 0;
  gtk_widget_queue_draw(self->widget);
  return TRUE;
}



// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
