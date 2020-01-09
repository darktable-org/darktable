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

#include "bauhaus/bauhaus.h"
#include "common/debug.h"
#include "common/image_cache.h"
#include "control/control.h"
#include "views/view.h"

G_DEFINE_TYPE(dt_thumbnail, dt_thumbnail, G_TYPE_OBJECT)


static gboolean _expose_again(gpointer user_data)
{
  if(!user_data || !GTK_IS_WIDGET(user_data)) return FALSE;

  GtkWidget *widget = (GtkWidget *)user_data;
  gtk_widget_queue_draw(widget);
  return FALSE;
}

static void _draw_background(cairo_t *cr, dt_thumbnail *thumb)
{
  // we draw the thumbtable background (the space between thumbnails)
  dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_LIGHTTABLE_BG);
  cairo_paint(cr);

  // we draw the thumbnail background
  const int delimiter = DT_PIXEL_APPLY_DPI(1.0);
  dt_gui_color_t bgcol = DT_GUI_COLOR_THUMBNAIL_BG;
  dt_gui_color_t fontcol = DT_GUI_COLOR_THUMBNAIL_FONT;
  dt_gui_color_t outlinecol = DT_GUI_COLOR_THUMBNAIL_OUTLINE;
  if(thumb->selected)
  {
    bgcol = DT_GUI_COLOR_THUMBNAIL_SELECTED_BG;
    fontcol = DT_GUI_COLOR_THUMBNAIL_SELECTED_FONT;
    outlinecol = DT_GUI_COLOR_THUMBNAIL_SELECTED_OUTLINE;
  }
  if(thumb->mouse_over)
  {
    bgcol = DT_GUI_COLOR_THUMBNAIL_HOVER_BG;
    fontcol = DT_GUI_COLOR_THUMBNAIL_HOVER_FONT;
    outlinecol = DT_GUI_COLOR_THUMBNAIL_HOVER_OUTLINE;
  }

  cairo_rectangle(cr, delimiter, delimiter, thumb->width - 2.0 * delimiter, thumb->height - 2.0 * delimiter);
  dt_gui_gtk_set_source_rgb(cr, bgcol);
  cairo_fill_preserve(cr);
  if(thumb->thumb_border)
  {
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.0));
    dt_gui_gtk_set_source_rgb(cr, outlinecol);
    cairo_stroke(cr);
  }

  // we try to acquire the image structure
  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, thumb->imgid, 'r');

  // we write the file extension
  if(img)
  {
    const char *ext = img->filename + strlen(img->filename);
    while(ext > img->filename && *ext != '.') ext--;
    ext++;
    gchar *upcase_ext = g_ascii_strup(ext, -1); // extension in capital letters to avoid character descenders
    dt_image_cache_read_release(darktable.image_cache, img);

    PangoLayout *layout;
    PangoRectangle ink;
    PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    const int fontsize = fminf(DT_PIXEL_APPLY_DPI(20.0), .09 * thumb->width);
    pango_font_description_set_absolute_size(desc, fontsize * PANGO_SCALE);
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, desc);

    dt_gui_gtk_set_source_rgb(cr, fontcol);

    if(thumb->img_height > thumb->img_width)
    {
      int max_chr_width = 0;
      for(int i = 0; upcase_ext[i] != 0; i++)
      {
        pango_layout_set_text(layout, &upcase_ext[i], 1);
        pango_layout_get_pixel_extents(layout, &ink, NULL);
        max_chr_width = MAX(max_chr_width, ink.width);
      }

      for(int i = 0, yoffs = fontsize; upcase_ext[i] != 0; i++, yoffs -= fontsize)
      {
        pango_layout_set_text(layout, &upcase_ext[i], 1);
        pango_layout_get_pixel_extents(layout, &ink, NULL);
        cairo_move_to(cr, .045 * thumb->width - ink.x + (max_chr_width - ink.width) / 2,
                      .045 * thumb->height - yoffs + fontsize);
        pango_cairo_show_layout(cr, layout);
      }
    }
    else
    {
      pango_layout_set_text(layout, upcase_ext, -1);
      pango_layout_get_pixel_extents(layout, &ink, NULL);
      cairo_move_to(cr, .045 * thumb->width - ink.x, .045 * thumb->height);
      pango_cairo_show_layout(cr, layout);
    }
    g_free(upcase_ext);
    pango_font_description_free(desc);
    g_object_unref(layout);
  }
}

static void _draw_image(cairo_t *cr, dt_thumbnail *thumb)
{
  if(!thumb->img_surf) return;

  cairo_set_source_surface(cr, thumb->img_surf, (thumb->width - thumb->img_width) / 2,
                           (thumb->height - thumb->img_height) / 2);
  cairo_paint(cr);
}

static void _draw_image_border(cairo_t *cr, dt_thumbnail *thumb)
{

}

static gboolean _draw_main_callback(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  if(!user_data) return TRUE;
  dt_thumbnail *thumb = (dt_thumbnail *)user_data;
  if(thumb->imgid <= 0)
  {
    dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_LIGHTTABLE_BG);
    cairo_paint(cr);
    return TRUE;
  }

  // if we don't have it in memory, we want the image surface
  if(!thumb->img_surf)
  {
    const gboolean res
        = dt_view_image_get_surface(thumb->imgid, thumb->width * 0.91, thumb->height * 0.91, &thumb->img_surf);
    if(res)
    {
      // if the image is missing, we reload it again
      g_timeout_add(250, _expose_again, widget);
      return TRUE;
    }

    // let save thumbnail image size
    thumb->img_width = cairo_image_surface_get_width(thumb->img_surf);
    thumb->img_height = cairo_image_surface_get_height(thumb->img_surf);
  }

  // we draw the background with thumb border and image type
  _draw_background(cr, thumb);

  // we draw the image
  _draw_image(cr, thumb);

  // we draw the image border
  _draw_image_border(cr, thumb);

  // we draw the "not bottom" decorations

  return TRUE;
}
static gboolean _draw_bottom_callback(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  return TRUE;
}

static gboolean _enter_notify_callback(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  if(!user_data) return TRUE;
  dt_thumbnail *thumb = (dt_thumbnail *)user_data;
  dt_control_set_mouse_over_id(thumb->imgid);
  return TRUE;
}

static void _mouse_over_image_callback(gpointer instance, gpointer user_data)
{
  if(!user_data) return;
  dt_thumbnail *thumb = (dt_thumbnail *)user_data;
  if(!thumb || !thumb->w_back || !GTK_IS_WIDGET(thumb->w_back)) return;

  int over_id = dt_control_get_mouse_over_id();
  if(thumb->mouse_over || over_id == thumb->imgid)
  {
    thumb->mouse_over = (over_id == thumb->imgid);
    gtk_widget_queue_draw(thumb->w_back);
  }
}

static void _selection_changed_callback(gpointer instance, gpointer user_data)
{
  if(!user_data) return;
  dt_thumbnail *thumb = (dt_thumbnail *)user_data;
  if(!thumb) return;

  gboolean selected = FALSE;
  /* clear and reset statements */
  DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.is_selected);
  DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.is_selected);
  /* bind imgid to prepared statements */
  DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.is_selected, 1, thumb->imgid);
  /* lets check if imgid is selected */
  if(sqlite3_step(darktable.view_manager->statements.is_selected) == SQLITE_ROW) selected = TRUE;

  // if there's a change, update the thumb
  if(selected == thumb->selected)
  {
    thumb->selected = selected;
    gtk_widget_queue_draw(thumb->w_back);
  }
}

GtkWidget *dt_thumbnail_get_widget(gpointer item, gpointer user_data)
{
  dt_thumbnail *thumb = (dt_thumbnail *)item;

  // main widget (overlay)
  thumb->w_main = gtk_overlay_new();

  if(thumb->imgid > 0)
  {
    g_object_set_data(G_OBJECT(thumb->w_main), "thumb", thumb);
    dt_control_signal_connect(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                              G_CALLBACK(_mouse_over_image_callback), thumb);
    dt_control_signal_connect(darktable.signals, DT_SIGNAL_SELECTION_CHANGED,
                              G_CALLBACK(_selection_changed_callback), thumb);
    gtk_widget_set_size_request(thumb->w_main, thumb->width, thumb->height);

    // the main drawing area
    thumb->w_back = gtk_drawing_area_new();
    gtk_widget_set_events(thumb->w_back, GDK_EXPOSURE_MASK | GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                             | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_STRUCTURE_MASK
                                             | GDK_ENTER_NOTIFY_MASK);

    gtk_widget_set_app_paintable(thumb->w_back, TRUE);
    g_signal_connect(G_OBJECT(thumb->w_back), "draw", G_CALLBACK(_draw_main_callback), thumb);
    g_signal_connect(G_OBJECT(thumb->w_back), "enter-notify-event", G_CALLBACK(_enter_notify_callback), thumb);
    gtk_widget_show(thumb->w_back);
    gtk_container_add(GTK_CONTAINER(thumb->w_main), thumb->w_back);

    // the bottom part
    thumb->w_bottom = gtk_drawing_area_new();
    gtk_widget_set_events(thumb->w_bottom, GDK_EXPOSURE_MASK | GDK_POINTER_MOTION_MASK
                                               | GDK_POINTER_MOTION_HINT_MASK | GDK_BUTTON_PRESS_MASK
                                               | GDK_BUTTON_RELEASE_MASK | GDK_STRUCTURE_MASK
                                               | GDK_ENTER_NOTIFY_MASK);

    gtk_widget_set_app_paintable(thumb->w_bottom, TRUE);
    g_signal_connect(G_OBJECT(thumb->w_bottom), "draw", G_CALLBACK(_draw_bottom_callback), thumb);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_bottom);
  }
  return thumb->w_main;
}

static void dt_thumbnail_init(dt_thumbnail *self)
{
}

static void dt_thumbnail_finalize(GObject *obj)
{
  dt_thumbnail *thumb = (dt_thumbnail *)obj;
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_mouse_over_image_callback), thumb);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_selection_changed_callback), thumb);
  if(thumb->img_surf) cairo_surface_destroy(thumb->img_surf);
  if(thumb->w_main) gtk_widget_destroy(thumb->w_main);

  G_OBJECT_CLASS(dt_thumbnail_parent_class)->finalize(obj);
}

static void dt_thumbnail_class_init(dt_thumbnailClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS(class);

  object_class->finalize = dt_thumbnail_finalize;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;