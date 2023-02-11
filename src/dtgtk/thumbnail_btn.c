/*
    This file is part of darktable,
    copyright (c)2020 Aldric Renaudin.

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
#include "thumbnail_btn.h"
#include "gui/gtk.h"
#include <string.h>

static void _thumbnail_btn_class_init(GtkDarktableThumbnailBtnClass *klass);
static void _thumbnail_btn_init(GtkDarktableThumbnailBtn *button);
static gboolean _thumbnail_btn_draw(GtkWidget *widget, cairo_t *cr);
static gboolean _thumbnail_btn_enter_leave_notify_callback(GtkWidget *widget, GdkEventCrossing *event);

static void _thumbnail_btn_class_init(GtkDarktableThumbnailBtnClass *klass)
{
  GtkWidgetClass *widget_class = (GtkWidgetClass *)klass;

  widget_class->draw = _thumbnail_btn_draw;
  widget_class->enter_notify_event = _thumbnail_btn_enter_leave_notify_callback;
  widget_class->leave_notify_event = _thumbnail_btn_enter_leave_notify_callback;
}

static void _thumbnail_btn_init(GtkDarktableThumbnailBtn *button)
{
}

static gboolean _thumbnail_btn_draw(GtkWidget *widget, cairo_t *cr)
{
  g_return_val_if_fail(DTGTK_IS_THUMBNAIL_BTN(widget), FALSE);

  if(gtk_widget_get_allocated_height(widget) < 2 || gtk_widget_get_allocated_width(widget) < 2) return TRUE;

  GtkStateFlags state = gtk_widget_get_state_flags(widget);

  GdkRGBA *fg_color, *bg_color;
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  gtk_style_context_get(context, state, GTK_STYLE_PROPERTY_COLOR, &fg_color, GTK_STYLE_PROPERTY_BACKGROUND_COLOR,
                        &bg_color, NULL);
  if(fg_color->alpha == 0 && bg_color->alpha == 0)
  {
    DTGTK_THUMBNAIL_BTN(widget)->hidden = TRUE;
    gdk_rgba_free(fg_color);
    gdk_rgba_free(bg_color);
    return TRUE;
  }
  DTGTK_THUMBNAIL_BTN(widget)->hidden = FALSE;

  cairo_save(cr);
  gdk_cairo_set_source_rgba(cr, fg_color);

  /* draw icon */
  if(DTGTK_THUMBNAIL_BTN(widget)->icon)
  {
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);

    int flags = DTGTK_THUMBNAIL_BTN(widget)->icon_flags;
    if(state & GTK_STATE_FLAG_PRELIGHT)
      flags |= CPF_PRELIGHT;
    else
      flags &= ~CPF_PRELIGHT;

    if(state & GTK_STATE_FLAG_ACTIVE)
      flags |= CPF_ACTIVE;
    else
      flags &= ~CPF_ACTIVE;

    GtkBorder padding;
    gtk_style_context_get_padding(context, state, &padding);
    // padding is a percent of the full size
    const float icon_x = padding.left * allocation.width / 100.0f;
    const float icon_y = padding.top * allocation.height / 100.0f;
    const float icon_w = allocation.width - (padding.left + padding.right) * allocation.width / 100.0f;
    const float icon_h = allocation.height - (padding.top + padding.bottom) * allocation.height / 100.0f;
    DTGTK_THUMBNAIL_BTN(widget)->icon(
        cr, icon_x, icon_y, icon_w, icon_h, flags,
        DTGTK_THUMBNAIL_BTN(widget)->icon_data ? DTGTK_THUMBNAIL_BTN(widget)->icon_data : bg_color);
  }
  // and eventually the image border
  cairo_restore(cr);
  gtk_render_frame(context, cr, 0, 0, gtk_widget_get_allocated_width(widget),
                   gtk_widget_get_allocated_height(widget));

  gdk_rgba_free(fg_color);
  gdk_rgba_free(bg_color);
  return TRUE;
}

static gboolean _thumbnail_btn_enter_leave_notify_callback(GtkWidget *widget, GdkEventCrossing *event)
{
  g_return_val_if_fail(widget != NULL, FALSE);

  if(event->type == GDK_ENTER_NOTIFY)
    gtk_widget_set_state_flags(widget, GTK_STATE_FLAG_PRELIGHT, FALSE);
  else
    gtk_widget_unset_state_flags(widget, GTK_STATE_FLAG_PRELIGHT);

  gtk_widget_queue_draw(widget);
  return FALSE;
}

// Public functions
GtkWidget *dtgtk_thumbnail_btn_new(DTGTKCairoPaintIconFunc paint, gint paintflags, void *paintdata)
{
  GtkDarktableThumbnailBtn *button;
  button = g_object_new(dtgtk_thumbnail_btn_get_type(), NULL);
  dt_gui_add_class(GTK_WIDGET(button), "dt_thumb_btn");
  button->icon = paint;
  button->icon_flags = paintflags;
  button->icon_data = paintdata;
  gtk_widget_set_events(GTK_WIDGET(button), GDK_EXPOSURE_MASK
                                                | GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK
                                                | GDK_BUTTON_RELEASE_MASK | GDK_STRUCTURE_MASK
                                                | GDK_ENTER_NOTIFY_MASK | GDK_ALL_EVENTS_MASK);
  gtk_widget_set_app_paintable(GTK_WIDGET(button), TRUE);
  gtk_widget_set_name(GTK_WIDGET(button), "thumbnail_btn");
  return (GtkWidget *)button;
}

GType dtgtk_thumbnail_btn_get_type()
{
  static GType dtgtk_thumbnail_btn_type = 0;
  if(!dtgtk_thumbnail_btn_type)
  {
    static const GTypeInfo dtgtk_thumbnail_btn_info = {
      sizeof(GtkDarktableThumbnailBtnClass),
      (GBaseInitFunc)NULL,
      (GBaseFinalizeFunc)NULL,
      (GClassInitFunc)_thumbnail_btn_class_init,
      NULL, /* class_finalize */
      NULL, /* class_data */
      sizeof(GtkDarktableThumbnailBtn),
      0, /* n_preallocs */
      (GInstanceInitFunc)_thumbnail_btn_init,
    };
    dtgtk_thumbnail_btn_type
        = g_type_register_static(GTK_TYPE_DRAWING_AREA, "GtkDarktableThumbnailBtn", &dtgtk_thumbnail_btn_info, 0);
  }
  return dtgtk_thumbnail_btn_type;
}

gboolean dtgtk_thumbnail_btn_is_hidden(GtkWidget *widget)
{
  g_return_val_if_fail(DTGTK_IS_THUMBNAIL_BTN(widget), TRUE);

  return DTGTK_THUMBNAIL_BTN(widget)->hidden;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
