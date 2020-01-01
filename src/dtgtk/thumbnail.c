/*
    This file is part of darktable,
    copyright (c) 2019--2020 Aldric Renaudin.

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
/** this is the thumbnail class for the lighttable module.  */
#include "dtgtk/thumbnail.h"
#include "control/control.h"
#include "views/view.h"

G_DEFINE_TYPE(dt_thumbnail, dt_thumbnail, G_TYPE_OBJECT)

static void dt_thumbnail_init(dt_thumbnail *self)
{
}

static void dt_thumbnail_finalize(GObject *obj)
{
  G_OBJECT_CLASS(dt_thumbnail_parent_class)->finalize(obj);
}

static void dt_thumbnail_class_init(dt_thumbnailClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS(class);

  object_class->finalize = dt_thumbnail_finalize;
}

static gboolean _dt_thumbnail_expose_again(gpointer user_data)
{
  if(!user_data || !GTK_IS_WIDGET(user_data)) return FALSE;

  GtkWidget *widget = (GtkWidget *)user_data;
  gtk_widget_queue_draw(widget);
  return FALSE;
}
static gboolean _dt_thumbnail_draw_callback(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  dt_thumbnail *thumb = (dt_thumbnail *)user_data;
  if(thumb->imgid <= 0)
  {
    dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_LIGHTTABLE_BG);
    cairo_paint(cr);
    return TRUE;
  }

  thumb->mouse_over = (dt_control_get_mouse_over_id() == thumb->imgid);

  dt_view_image_expose_t params = { 0 };
  params.image_over = NULL;
  params.imgid = thumb->imgid;
  params.mouse_over = thumb->mouse_over;
  params.cr = cr;
  params.width = thumb->width;
  params.height = thumb->height;
  params.zoom = 7;
  const gboolean res = dt_view_image_expose(&params);

  if(res) g_timeout_add(250, _dt_thumbnail_expose_again, widget);
  return TRUE;
}

static gboolean _dt_thumbnail_enter_notify_callback(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_thumbnail *thumb = (dt_thumbnail *)user_data;
  dt_control_set_mouse_over_id(thumb->imgid);
  return TRUE;
}

static void _dt_thumbnail_mouse_over_image_callback(gpointer instance, gpointer user_data)
{
  GtkWidget *widget = (GtkWidget *)user_data;
  dt_thumbnail *thumb = (dt_thumbnail *)g_object_get_data(G_OBJECT(widget), "thumb");
  if(!thumb) return;
  if(thumb->mouse_over || dt_control_get_mouse_over_id() == thumb->imgid) gtk_widget_queue_draw(widget);
}

GtkWidget *dt_thumbnail_get_widget(gpointer item, gpointer user_data)
{
  dt_thumbnail *thumb = (dt_thumbnail *)item;

  GtkWidget *ret = gtk_drawing_area_new();
  g_object_set_data(G_OBJECT(ret), "thumb", thumb);
  gtk_widget_set_events(ret, GDK_EXPOSURE_MASK | GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                 | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_STRUCTURE_MASK
                                 | GDK_ENTER_NOTIFY_MASK);

  /* connect callbacks */
  gtk_widget_set_app_paintable(ret, TRUE);
  g_signal_connect(G_OBJECT(ret), "draw", G_CALLBACK(_dt_thumbnail_draw_callback), thumb);
  // g_signal_connect(G_OBJECT(ret), "size-allocate", G_CALLBACK(_dt_thumbnail_resize_callback), thumb);
  /*g_signal_connect(G_OBJECT(self->widget), "button-press-event",
                   G_CALLBACK(_lib_navigation_button_press_callback), self);
  g_signal_connect(G_OBJECT(self->widget), "button-release-event",
                   G_CALLBACK(_lib_navigation_button_release_callback), self);
  g_signal_connect(G_OBJECT(self->widget), "motion-notify-event",
                   G_CALLBACK(_lib_navigation_motion_notify_callback), self);*/
  g_signal_connect(G_OBJECT(ret), "enter-notify-event", G_CALLBACK(_dt_thumbnail_enter_notify_callback), thumb);

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(_dt_thumbnail_mouse_over_image_callback), ret);

  gtk_widget_set_size_request(ret, thumb->width, thumb->height);

  return ret;
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;