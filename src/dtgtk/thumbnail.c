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
#include "common/selection.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "dtgtk/icon.h"
#include "dtgtk/thumbnail_btn.h"
#include "views/view.h"

static void _set_flag(GtkWidget *w, GtkStateFlags flag, gboolean over)
{
  int flags = gtk_widget_get_state_flags(w);
  if(over)
    flags |= flag;
  else
    flags &= ~flag;

  gtk_widget_set_state_flags(w, flags, TRUE);
}

static gboolean _expose_again(gpointer user_data)
{
  if(!user_data || !GTK_IS_WIDGET(user_data)) return FALSE;

  GtkWidget *widget = (GtkWidget *)user_data;
  gtk_widget_queue_draw(widget);
  return FALSE;
}

static void _draw_background(cairo_t *cr, dt_thumbnail_t *thumb)
{
  return;
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

static void _draw_image(cairo_t *cr, dt_thumbnail_t *thumb)
{
  if(!thumb->img_surf) return;

  cairo_set_source_surface(cr, thumb->img_surf, 0, 0);
  cairo_paint(cr);
}

static void _draw_image_border(cairo_t *cr, dt_thumbnail_t *thumb)
{

}

static gboolean _back_draw_callback(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  if(!user_data) return TRUE;
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
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
    gtk_widget_set_size_request(widget, thumb->img_width, thumb->img_height);

    // now that we know image ratio, we can fill the extension label
    const dt_image_t *img = dt_image_cache_get(darktable.image_cache, thumb->imgid, 'r');
    if(img)
    {
      const char *ext = img->filename + strlen(img->filename);
      gchar *ext2 = NULL;
      while(ext > img->filename && *ext != '.') ext--;
      ext++;
      if(thumb->img_width < thumb->img_height)
      {
        // vertical disposition
        for(int i = 0; i < strlen(ext); i++) ext2 = dt_util_dstrcat(ext2, "%.1s\n", &ext[i]);
      }
      else
        ext2 = dt_util_dstrcat(ext2, "%s", ext);
      gchar *upcase_ext = g_ascii_strup(ext2, -1); // extension in capital letters to avoid character descenders
      dt_image_cache_read_release(darktable.image_cache, img);
      const int fsize = fminf(DT_PIXEL_APPLY_DPI(20.0), .09 * thumb->width);
      gchar *ext_final = dt_util_dstrcat(NULL, "<span size=\"%d\">%s</span>", fsize * PANGO_SCALE, upcase_ext);
      gtk_label_set_markup(GTK_LABEL(thumb->w_ext), ext_final);
      g_free(upcase_ext);
      g_free(ext_final);
      g_free(ext2);
    }

    return TRUE;
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
/*static gboolean _draw_bottom_callback(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  return FALSE;
}*/

static gboolean _back_enter_notify_callback(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  if(!user_data) return TRUE;
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  dt_control_set_mouse_over_id(thumb->imgid);
  _set_flag(thumb->w_info_back_eb, GTK_STATE_FLAG_PRELIGHT, FALSE);
  return TRUE;
}

static gboolean _back_press_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(event->button == 1 && event->type == GDK_2BUTTON_PRESS)
  {
    dt_view_manager_switch(darktable.view_manager, "darkroom");
    return TRUE;
  }
  return FALSE;
}
static gboolean _back_release_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;

  if(event->button == 1)
  {
    if((event->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)) == 0)
      dt_selection_select_single(darktable.selection, thumb->imgid);
    else if((event->state & (GDK_CONTROL_MASK)) == GDK_CONTROL_MASK)
      dt_selection_toggle(darktable.selection, thumb->imgid);
    else if((event->state & (GDK_SHIFT_MASK)) == GDK_SHIFT_MASK)
      dt_selection_select_range(darktable.selection, thumb->imgid);
  }
  return FALSE;
}
static gboolean _reject_release_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  return TRUE;
}

static void _dt_mouse_over_image_callback(gpointer instance, gpointer user_data)
{
  if(!user_data) return;
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  if(!thumb || !thumb->w_back || !GTK_IS_WIDGET(thumb->w_back)) return;

  int over_id = dt_control_get_mouse_over_id();
  if(thumb->mouse_over || over_id == thumb->imgid)
  {
    thumb->mouse_over = (over_id == thumb->imgid);
    printf(" hover %d %d\n", thumb->imgid, thumb->mouse_over);
    gtk_widget_set_visible(thumb->w_info_back_eb, thumb->mouse_over);
    gtk_widget_set_visible(thumb->w_btn_reject, thumb->mouse_over);
    for(int i = 0; i < 4; i++) gtk_widget_set_visible(thumb->w_stars[i], thumb->mouse_over);

    _set_flag(thumb->w_back, GTK_STATE_FLAG_PRELIGHT, thumb->mouse_over);
    _set_flag(thumb->w_ext, GTK_STATE_FLAG_PRELIGHT, thumb->mouse_over);
    _set_flag(thumb->w_image, GTK_STATE_FLAG_PRELIGHT, thumb->mouse_over);
    if(!thumb->mouse_over) _set_flag(thumb->w_info_back_eb, GTK_STATE_FLAG_PRELIGHT, FALSE);
    gtk_widget_queue_draw(thumb->w_main);
  }
}

static void _dt_selection_changed_callback(gpointer instance, gpointer user_data)
{
  if(!user_data) return;
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
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
  if(selected != thumb->selected)
  {
    thumb->selected = selected;
    _set_flag(thumb->w_back, GTK_STATE_FLAG_SELECTED, thumb->selected);
    _set_flag(thumb->w_ext, GTK_STATE_FLAG_SELECTED, thumb->selected);
    _set_flag(thumb->w_image, GTK_STATE_FLAG_SELECTED, thumb->selected);
    gtk_widget_queue_draw(thumb->w_main);
  }
}

static gboolean _info_back_enter_notify_callback(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  if(!thumb->mouse_over) dt_control_set_mouse_over_id(thumb->imgid);
  _set_flag(thumb->w_info_back_eb, GTK_STATE_FLAG_PRELIGHT, TRUE);
  return FALSE;
}

GtkWidget *dt_thumbnail_create_widget(dt_thumbnail_t *thumb)
{
  // main widget (overlay)
  thumb->w_main = gtk_overlay_new();
  gtk_widget_set_size_request(thumb->w_main, thumb->width, thumb->height);
  g_signal_connect(G_OBJECT(thumb->w_main), "button-press-event", G_CALLBACK(_back_press_callback), thumb);
  g_signal_connect(G_OBJECT(thumb->w_main), "button-release-event", G_CALLBACK(_back_release_callback), thumb);

  if(thumb->imgid > 0)
  {
    g_object_set_data(G_OBJECT(thumb->w_main), "thumb", thumb);
    dt_control_signal_connect(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                              G_CALLBACK(_dt_mouse_over_image_callback), thumb);
    dt_control_signal_connect(darktable.signals, DT_SIGNAL_SELECTION_CHANGED,
                              G_CALLBACK(_dt_selection_changed_callback), thumb);

    // the background
    thumb->w_back = gtk_event_box_new();
    gtk_widget_set_name(thumb->w_back, "thumb_back");
    g_signal_connect(G_OBJECT(thumb->w_back), "enter-notify-event", G_CALLBACK(_back_enter_notify_callback), thumb);
    gtk_widget_show(thumb->w_back);
    gtk_container_add(GTK_CONTAINER(thumb->w_main), thumb->w_back);

    // the file extension label
    thumb->w_ext = gtk_label_new("");
    gtk_widget_set_name(thumb->w_ext, "thumb_ext");
    gtk_widget_set_valign(thumb->w_ext, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_ext, GTK_ALIGN_START);
    gtk_widget_set_margin_start(thumb->w_ext, 0.045 * thumb->width);
    gtk_widget_set_margin_top(thumb->w_ext, 0.045 * thumb->width);
    gtk_label_set_justify(GTK_LABEL(thumb->w_ext), GTK_JUSTIFY_CENTER);
    gtk_widget_show(thumb->w_ext);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_ext);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(thumb->w_main), thumb->w_ext, TRUE);

    // the main drawing area
    thumb->w_image = gtk_drawing_area_new();
    gtk_widget_set_name(thumb->w_image, "thumb_image");
    gtk_widget_set_size_request(thumb->w_image, thumb->width, thumb->height);
    gtk_widget_set_valign(thumb->w_image, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(thumb->w_image, GTK_ALIGN_CENTER);
    gtk_widget_set_events(thumb->w_image, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_STRUCTURE_MASK
                                              | GDK_ENTER_NOTIFY_MASK);
    g_signal_connect(G_OBJECT(thumb->w_image), "draw", G_CALLBACK(_back_draw_callback), thumb);
    g_signal_connect(G_OBJECT(thumb->w_image), "enter-notify-event", G_CALLBACK(_back_enter_notify_callback),
                     thumb);
    gtk_widget_show(thumb->w_image);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_image);

    // we need to squeeze 5 stars + 2 symbols on a thumbnail width
    // each of them having a width of 2 * r1 and spaced by r1
    // that's 14 * r1 of content + 6 * r1 of spacing
    // inner margins are 0.045 * width
    const float r1 = fminf(DT_PIXEL_APPLY_DPI(20.0f) / 2.0f, 0.91 * thumb->width / 20.0f);

    // the infos background
    thumb->w_info_back_eb = gtk_event_box_new();
    g_signal_connect(G_OBJECT(thumb->w_info_back_eb), "enter-notify-event",
                     G_CALLBACK(_info_back_enter_notify_callback), thumb);
    gtk_widget_set_valign(thumb->w_info_back_eb, GTK_ALIGN_END);
    gtk_widget_set_halign(thumb->w_info_back_eb, GTK_ALIGN_CENTER);
    thumb->w_info_back = gtk_label_new("");
    gtk_widget_set_name(thumb->w_info_back_eb, "thumb_info");
    gtk_widget_set_size_request(thumb->w_info_back, thumb->width - 2 * DT_PIXEL_APPLY_DPI(1.0),
                                0.147125 * thumb->height); // TODO Why this hardcoded ratio ?  prefer something
                                                           // dependent of fontsize ?
    gtk_widget_show(thumb->w_info_back);
    gtk_container_add(GTK_CONTAINER(thumb->w_info_back_eb), thumb->w_info_back);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_info_back_eb);

    // the reject icon
    thumb->w_btn_reject = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_reject, 0, NULL);
    gtk_widget_set_size_request(thumb->w_btn_reject, 4.0 * r1, 4.0 * r1);
    gtk_widget_set_valign(thumb->w_btn_reject, GTK_ALIGN_END);
    gtk_widget_set_halign(thumb->w_btn_reject, GTK_ALIGN_START);
    gtk_widget_set_margin_start(thumb->w_btn_reject, 0.045 * thumb->width - r1);
    gtk_widget_set_margin_bottom(thumb->w_btn_reject, 0.045 * thumb->width - r1);
    g_signal_connect(G_OBJECT(thumb->w_btn_reject), "enter-notify-event",
                     G_CALLBACK(_info_back_enter_notify_callback), thumb);
    g_signal_connect(G_OBJECT(thumb->w_btn_reject), "button-release-event", G_CALLBACK(_reject_release_callback),
                     thumb);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_btn_reject);

    // the stars
    for(int i = 0; i < 4; i++)
    {
      thumb->w_stars[i] = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_star, 0, NULL);
      gtk_widget_set_size_request(thumb->w_stars[i], 4.0 * r1, 4.0 * r1);
      g_signal_connect(G_OBJECT(thumb->w_stars[i]), "enter-notify-event",
                       G_CALLBACK(_info_back_enter_notify_callback), thumb);
      gtk_widget_set_name(thumb->w_stars[i], "thumb_star");
      gtk_widget_set_valign(thumb->w_stars[i], GTK_ALIGN_END);
      gtk_widget_set_halign(thumb->w_stars[i], GTK_ALIGN_START);
      gtk_widget_set_margin_bottom(thumb->w_stars[i], 0.045 * thumb->width - r1);
      gtk_widget_set_margin_start(thumb->w_stars[i], (thumb->width - 16.0 * r1) * 0.5 + i * 4.0 * r1);
      gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_stars[i]);
    }
  }
  gtk_widget_show(thumb->w_main);
  g_object_ref(G_OBJECT(thumb->w_main));
  return thumb->w_main;
}

dt_thumbnail_t *dt_thumbnail_new(int width, int height, int imgid, int rowid)
{
  dt_thumbnail_t *thumb = calloc(1, sizeof(dt_thumbnail_t));
  thumb->width = width;
  thumb->height = height;
  thumb->imgid = imgid;
  thumb->rowid = rowid;

  dt_thumbnail_create_widget(thumb);

  return thumb;
}

void dt_thumbnail_destroy(dt_thumbnail_t *thumb)
{
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_dt_mouse_over_image_callback), thumb);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_dt_selection_changed_callback), thumb);
  if(thumb->img_surf) cairo_surface_destroy(thumb->img_surf);
  if(thumb->w_main) gtk_widget_destroy(thumb->w_main);

  free(thumb);
}

void dt_thumbnail_resize(dt_thumbnail_t *thumb, int width, int height)
{
  // new size unit
  const float r1 = fminf(DT_PIXEL_APPLY_DPI(20.0f) / 2.0f, 0.91 * width / 20.0f);

  // widget resizing
  gtk_widget_set_size_request(thumb->w_main, width, height);
  gtk_widget_set_size_request(thumb->w_info_back, width - 2 * DT_PIXEL_APPLY_DPI(1.0), 0.147125 * height);
  gtk_widget_set_size_request(thumb->w_btn_reject, 4.0 * r1, 4.0 * r1);
  gtk_widget_set_margin_start(thumb->w_btn_reject, 0.045 * width - r1);
  gtk_widget_set_margin_bottom(thumb->w_btn_reject, 0.045 * width - r1);
  for(int i = 0; i < 4; i++)
  {
    gtk_widget_set_size_request(thumb->w_stars[i], 4.0 * r1, 4.0 * r1);
    gtk_widget_set_margin_bottom(thumb->w_stars[i], 0.045 * width - r1);
    gtk_widget_set_margin_start(thumb->w_stars[i], (thumb->width - 16.0 * r1) * 0.5 + i * 4.0 * r1);
  }

  // update values
  thumb->width = width;
  thumb->height = height;

  // reset surface
  if(thumb->img_surf) cairo_surface_destroy(thumb->img_surf);
  thumb->img_surf = NULL;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;