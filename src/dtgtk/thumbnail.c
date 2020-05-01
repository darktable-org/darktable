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
#include "common/collection.h"
#include "common/debug.h"
#include "common/focus_peaking.h"
#include "common/grouping.h"
#include "common/image_cache.h"
#include "common/ratings.h"
#include "common/selection.h"
#include "common/variables.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "dtgtk/icon.h"
#include "dtgtk/thumbnail_btn.h"
#include "gui/drag_and_drop.h"
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

// create a new extended infos line from strach
static void _thumb_update_extended_infos_line(dt_thumbnail_t *thumb)
{
  gchar *pattern = dt_conf_get_string("plugins/lighttable/extended_pattern");
  // we compute the info line (we reuse the function used in export to disk)
  char input_dir[1024] = { 0 };
  gboolean from_cache = TRUE;
  dt_image_full_path(thumb->imgid, input_dir, sizeof(input_dir), &from_cache);

  dt_variables_params_t *vp;
  dt_variables_params_init(&vp);

  vp->filename = input_dir;
  vp->jobcode = "infos";
  vp->imgid = thumb->imgid;
  vp->sequence = 0;

  gchar *msg = dt_variables_expand(vp, pattern, TRUE);

  dt_variables_params_destroy(vp);

  // we change the label
  g_snprintf(thumb->info_line, sizeof(thumb->info_line), "%s", msg);

  g_free(msg);
  g_free(pattern);
}

static void _image_get_infos(dt_thumbnail_t *thumb)
{
  if(thumb->imgid <= 0) return;
  if(thumb->over == DT_THUMBNAIL_OVERLAYS_NONE) return;

  // we only get here infos that might change, others(exif, ...) are cached on widget creation

  thumb->rating = 0;
  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, thumb->imgid, 'r');
  if(img)
  {
    thumb->has_localcopy = (img->flags & DT_IMAGE_LOCAL_COPY);
    thumb->rating = img->flags & DT_IMAGE_REJECTED ? DT_VIEW_REJECT : (img->flags & DT_VIEW_RATINGS_MASK);
    thumb->is_bw = dt_image_is_monochrome(img);
    thumb->is_hdr = dt_image_is_hdr(img);

    thumb->groupid = img->group_id;

    dt_image_cache_read_release(darktable.image_cache, img);
  }

  // colorlabels
  thumb->colorlabels = 0;
  DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.get_color);
  DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.get_color);
  DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.get_color, 1, thumb->imgid);
  while(sqlite3_step(darktable.view_manager->statements.get_color) == SQLITE_ROW)
  {
    const int col = sqlite3_column_int(darktable.view_manager->statements.get_color, 0);
    // we reuse CPF_* flags, as we'll pass them to the paint fct after
    if(col == 0)
      thumb->colorlabels |= CPF_DIRECTION_UP;
    else if(col == 1)
      thumb->colorlabels |= CPF_DIRECTION_DOWN;
    else if(col == 2)
      thumb->colorlabels |= CPF_DIRECTION_LEFT;
    else if(col == 3)
      thumb->colorlabels |= CPF_DIRECTION_RIGHT;
    else if(col == 4)
      thumb->colorlabels |= CPF_BG_TRANSPARENT;
  }
  if(thumb->w_color)
  {
    GtkDarktableThumbnailBtn *btn = (GtkDarktableThumbnailBtn *)thumb->w_color;
    btn->icon_flags = thumb->colorlabels;
  }

  // altered
  thumb->is_altered = dt_image_altered(thumb->imgid);

  // grouping
  DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.get_grouped);
  DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.get_grouped);
  DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.get_grouped, 1, thumb->imgid);
  DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.get_grouped, 2, thumb->imgid);
  thumb->is_grouped = (sqlite3_step(darktable.view_manager->statements.get_grouped) == SQLITE_ROW);
}

static gboolean _thumb_expose_again(gpointer user_data)
{
  if(!user_data || !GTK_IS_WIDGET(user_data)) return FALSE;

  GtkWidget *widget = (GtkWidget *)user_data;
  gtk_widget_queue_draw(widget);
  return FALSE;
}

static gboolean _event_image_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  if(!user_data) return TRUE;
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  if(thumb->imgid <= 0)
  {
    dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_LIGHTTABLE_BG);
    cairo_paint(cr);
    return TRUE;
  }

  // if we have a rgbbuf but the thumb is not anymore the darkroom main one
  dt_develop_t *dev = darktable.develop;
  const dt_view_t *v = dt_view_manager_get_current_view(darktable.view_manager);
  if(thumb->img_surf_preview
     && (v->view(v) != DT_VIEW_DARKROOM || !dev->preview_pipe->output_backbuf
         || dev->preview_pipe->output_imgid != thumb->imgid))
  {
    if(thumb->img_surf && cairo_surface_get_reference_count(thumb->img_surf) > 0)
      cairo_surface_destroy(thumb->img_surf);
    thumb->img_surf = NULL;
    thumb->img_surf_dirty = TRUE;
  }

  // if image surface has no more ref. let's samitize it's value to NULL
  if(thumb->img_surf && cairo_surface_get_reference_count(thumb->img_surf) < 1) thumb->img_surf = NULL;

  // if we don't have it in memory, we want the image surface
  if(!thumb->img_surf || thumb->img_surf_dirty)
  {
    if(thumb->img_margin) gtk_border_free(thumb->img_margin);
    // we retrieve image margins from css
    GtkStateFlags state = gtk_widget_get_state_flags(thumb->w_image);
    thumb->img_margin = gtk_border_new();
    GtkStyleContext *context = gtk_widget_get_style_context(thumb->w_image);
    gtk_style_context_get_margin(context, state, thumb->img_margin);

    const float ratio_h = (float)(100 - thumb->img_margin->top - thumb->img_margin->bottom) / 100.0;
    const float ratio_w = (float)(100 - thumb->img_margin->left - thumb->img_margin->right) / 100.0;

    int image_w, image_h;
    if(thumb->over == DT_THUMBNAIL_OVERLAYS_ALWAYS_NORMAL || thumb->over == DT_THUMBNAIL_OVERLAYS_ALWAYS_EXTENDED)
    {
      image_w = thumb->width * ratio_w;
      int w = 0;
      int h = 0;
      gtk_widget_get_size_request(thumb->w_bottom, &w, &h);
      image_h = thumb->height - h;
      gtk_widget_get_size_request(thumb->w_altered, &w, &h);
      image_h -= h + gtk_widget_get_margin_top(thumb->w_altered);
      image_h *= ratio_h;
    }
    else if(thumb->over == DT_THUMBNAIL_OVERLAYS_MIXED)
    {
      image_w = thumb->width * ratio_w;
      int w = 0;
      int h = 0;
      gtk_widget_get_size_request(thumb->w_reject, &w, &h);
      image_h = thumb->height - (h + 2 * gtk_widget_get_margin_bottom(thumb->w_reject));
      gtk_widget_get_size_request(thumb->w_altered, &w, &h);
      image_h -= h + gtk_widget_get_margin_top(thumb->w_altered);
      image_h *= ratio_h;
    }
    else
    {
      image_w = thumb->width * ratio_w;
      image_h = thumb->height * ratio_h;
    }

    if(v->view(v) == DT_VIEW_DARKROOM && dev->preview_pipe->output_imgid == thumb->imgid
       && dev->preview_pipe->output_backbuf)
    {
      // the current thumb is the one currently developped in darkroom
      // better use the preview buffer for surface, in order to stay in sync
      if(thumb->img_surf && cairo_surface_get_reference_count(thumb->img_surf) > 0)
        cairo_surface_destroy(thumb->img_surf);
      thumb->img_surf = NULL;

      // get new surface with preview image
      const int buf_width = dev->preview_pipe->output_backbuf_width;
      const int buf_height = dev->preview_pipe->output_backbuf_height;
      uint8_t *rgbbuf = g_malloc0((size_t)buf_width * buf_height * 4 * sizeof(unsigned char));

      dt_pthread_mutex_t *mutex = &dev->preview_pipe->backbuf_mutex;
      dt_pthread_mutex_lock(mutex);
      memcpy(rgbbuf, dev->preview_pipe->output_backbuf, (size_t)buf_width * buf_height * 4 * sizeof(unsigned char));
      dt_pthread_mutex_unlock(mutex);

      const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, buf_width);
      cairo_surface_t *tmp_surface
          = cairo_image_surface_create_for_data(rgbbuf, CAIRO_FORMAT_RGB24, buf_width, buf_height, stride);

      // copy preview image into final surface
      if(tmp_surface)
      {
        const float scale = fminf(image_w / (float)buf_width, image_h / (float)buf_height);
        const int img_width = buf_width * scale;
        const int img_height = buf_height * scale;
        thumb->img_surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, img_width, img_height);
        cairo_t *cr2 = cairo_create(thumb->img_surf);
        cairo_scale(cr2, scale, scale);

        cairo_set_source_surface(cr2, tmp_surface, 0, 0);
        // set filter no nearest:
        // in skull mode, we want to see big pixels.
        // in 1 iir mode for the right mip, we want to see exactly what the pipe gave us, 1:1 pixel for pixel.
        // in between, filtering just makes stuff go unsharp.
        if((buf_width <= 8 && buf_height <= 8) || fabsf(scale - 1.0f) < 0.01f)
          cairo_pattern_set_filter(cairo_get_source(cr2), CAIRO_FILTER_NEAREST);
        else
          cairo_pattern_set_filter(cairo_get_source(cr2), CAIRO_FILTER_GOOD);

        cairo_paint(cr2);

        if(darktable.gui->show_focus_peaking)
          dt_focuspeaking(cr2, img_width, img_height, cairo_image_surface_get_data(thumb->img_surf),
                          cairo_image_surface_get_width(thumb->img_surf),
                          cairo_image_surface_get_height(thumb->img_surf));

        cairo_surface_destroy(tmp_surface);
        cairo_destroy(cr2);
      }
      if(rgbbuf) g_free(rgbbuf);
    }
    else
    {
      const gboolean res = dt_view_image_get_surface(thumb->imgid, image_w, image_h, &thumb->img_surf);
      if(res)
      {
        // if the image is missing, we reload it again
        g_timeout_add(250, _thumb_expose_again, widget);
        return TRUE;
      }
    }

    thumb->img_surf_dirty = FALSE;
    // let save thumbnail image size
    thumb->img_width = cairo_image_surface_get_width(thumb->img_surf);
    thumb->img_height = cairo_image_surface_get_height(thumb->img_surf);
    gtk_widget_set_size_request(widget, thumb->img_width, thumb->img_height);
    // and we set the position of the image
    int posx, posy;
    if(thumb->over == DT_THUMBNAIL_OVERLAYS_ALWAYS_NORMAL || thumb->over == DT_THUMBNAIL_OVERLAYS_ALWAYS_EXTENDED)
    {
      posx = thumb->width * thumb->img_margin->left / 100 + (image_w - thumb->img_width) / 2;
      int w = 0;
      int h = 0;
      gtk_widget_get_size_request(thumb->w_altered, &w, &h);
      posy = h + gtk_widget_get_margin_top(thumb->w_altered);

      gtk_widget_get_size_request(thumb->w_bottom, &w, &h);
      posy += (thumb->height - posy - h) * thumb->img_margin->top / 100 + (image_h - thumb->img_height) / 2;
    }
    else if(thumb->over == DT_THUMBNAIL_OVERLAYS_MIXED)
    {
      posx = thumb->width * thumb->img_margin->left / 100 + (image_w - thumb->img_width) / 2;
      int w = 0;
      int h = 0;
      gtk_widget_get_size_request(thumb->w_altered, &w, &h);
      posy = h + gtk_widget_get_margin_top(thumb->w_altered);

      gtk_widget_get_size_request(thumb->w_reject, &w, &h);
      posy += (thumb->height - posy - h - 2 * gtk_widget_get_margin_bottom(thumb->w_reject))
                  * thumb->img_margin->top / 100
              + (image_h - thumb->img_height) / 2;
    }
    else
    {
      posx = thumb->width * thumb->img_margin->left / 100 + (image_w - thumb->img_width) / 2;
      posy = thumb->height * thumb->img_margin->top / 100 + (image_h - thumb->img_height) / 2;
    }
    gtk_widget_set_margin_start(thumb->w_image, posx);
    gtk_widget_set_margin_top(thumb->w_image, posy);

    // now that we know image ratio, we can fill the extension label
    const char *ext = thumb->filename + strlen(thumb->filename);
    gchar *ext2 = NULL;
    while(ext > thumb->filename && *ext != '.') ext--;
    ext++;
    gchar *uext = dt_view_extend_modes_str(ext, thumb->is_hdr, thumb->is_bw);

    if(thumb->img_width < thumb->img_height)
    {
      // vertical disposition
      for(int i = 0; i < strlen(uext); i++) ext2 = dt_util_dstrcat(ext2, "%.1s\n", &uext[i]);
    }
    else
      ext2 = dt_util_dstrcat(ext2, "%s", uext);

    gtk_label_set_text(GTK_LABEL(thumb->w_ext), ext2);
    g_free(uext);
    g_free(ext2);

    return TRUE;
  }

  // Safety check to avoid possible error
  if(!thumb->img_surf || cairo_surface_get_reference_count(thumb->img_surf) < 1) return TRUE;

  // we draw the image
  cairo_set_source_surface(cr, thumb->img_surf, 0, 0);
  cairo_paint(cr);

  // and eventually the image border
  GtkStyleContext *context = gtk_widget_get_style_context(thumb->w_image);
  gtk_render_frame(context, cr, 0, 0, thumb->img_width, thumb->img_height);

  return TRUE;
}

static gboolean _event_main_motion(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  if(!user_data) return TRUE;
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  if(!thumb->mouse_over && !thumb->disable_mouseover) dt_control_set_mouse_over_id(thumb->imgid);
  return FALSE;
}

static gboolean _event_main_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  if(event->button == 1
     && ((event->type == GDK_2BUTTON_PRESS && !thumb->single_click)
         || (event->type == GDK_BUTTON_PRESS
             && (event->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_MOD1_MASK)) == 0 && thumb->single_click)))
  {
    dt_control_set_mouse_over_id(thumb->imgid); // to ensure we haven't lost imgid during double-click
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE, thumb->imgid);
  }
  return FALSE;
}
static gboolean _event_main_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;

  if(event->button == 1 && !thumb->moved && thumb->sel_mode != DT_THUMBNAIL_SEL_MODE_DISABLED)
  {
    if((event->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_MOD1_MASK)) == 0
       && thumb->sel_mode != DT_THUMBNAIL_SEL_MODE_MOD_ONLY)
      dt_selection_select_single(darktable.selection, thumb->imgid);
    else if((event->state & (GDK_MOD1_MASK)) == GDK_MOD1_MASK)
      dt_selection_select_single(darktable.selection, thumb->imgid);
    else if((event->state & (GDK_CONTROL_MASK)) == GDK_CONTROL_MASK)
      dt_selection_toggle(darktable.selection, thumb->imgid);
    else if((event->state & (GDK_SHIFT_MASK)) == GDK_SHIFT_MASK)
      dt_selection_select_range(darktable.selection, thumb->imgid);
  }
  return FALSE;
}

static gboolean _event_rating_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  return TRUE;
}
static gboolean _event_rating_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  if(dtgtk_thumbnail_btn_is_hidden(widget)) return FALSE;

  if(event->button == 1 && !thumb->moved)
  {
    dt_view_image_over_t rating = DT_VIEW_DESERT;
    if(widget == thumb->w_reject)
      rating = DT_VIEW_REJECT;
    else if(widget == thumb->w_stars[0])
      rating = DT_VIEW_STAR_1;
    else if(widget == thumb->w_stars[1])
      rating = DT_VIEW_STAR_2;
    else if(widget == thumb->w_stars[2])
      rating = DT_VIEW_STAR_3;
    else if(widget == thumb->w_stars[3])
      rating = DT_VIEW_STAR_4;
    else if(widget == thumb->w_stars[4])
      rating = DT_VIEW_STAR_5;

    if(rating != DT_VIEW_DESERT)
    {
      dt_ratings_apply_on_image(thumb->imgid, rating, TRUE, TRUE, TRUE);
      dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD,
                                 g_list_append(NULL, GINT_TO_POINTER(thumb->imgid)));
    }
  }
  return TRUE;
}

static gboolean _event_grouping_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  if(dtgtk_thumbnail_btn_is_hidden(widget)) return FALSE;

  if(event->button == 1 && !thumb->moved)
  {
    if(event->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)) // just add the whole group to the selection. TODO:
                                                           // make this also work for collapsed groups.
    {
      sqlite3_stmt *stmt;
      DT_DEBUG_SQLITE3_PREPARE_V2(
          dt_database_get(darktable.db),
          "INSERT OR IGNORE INTO main.selected_images SELECT id FROM main.images WHERE group_id = ?1", -1, &stmt,
          NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, thumb->groupid);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }
    else if(!darktable.gui->grouping
            || thumb->groupid == darktable.gui->expanded_group_id) // the group is already expanded, so ...
    {
      if(thumb->imgid == darktable.gui->expanded_group_id && darktable.gui->grouping) // ... collapse it
        darktable.gui->expanded_group_id = -1;
      else // ... make the image the new representative of the group
        darktable.gui->expanded_group_id = dt_grouping_change_representative(thumb->imgid);
    }
    else // expand the group
      darktable.gui->expanded_group_id = thumb->groupid;
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, NULL);
  }
  return FALSE;
}

static gboolean _event_audio_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  if(dtgtk_thumbnail_btn_is_hidden(widget)) return FALSE;

  if(event->button == 1 && !thumb->moved)
  {
    gboolean start_audio = TRUE;
    if(darktable.view_manager->audio.audio_player_id != -1)
    {
      // don't start the audio for the image we just killed it for
      if(darktable.view_manager->audio.audio_player_id == thumb->imgid) start_audio = FALSE;
      dt_view_audio_stop(darktable.view_manager);
    }

    if(start_audio)
    {
      dt_view_audio_start(darktable.view_manager, thumb->imgid);
    }
  }
  return FALSE;
}

static void _thumb_update_icons(dt_thumbnail_t *thumb)
{
  gtk_widget_set_visible(thumb->w_local_copy, thumb->has_localcopy);
  gtk_widget_set_visible(thumb->w_altered, thumb->is_altered);
  gtk_widget_set_visible(thumb->w_group, thumb->is_grouped);
  gtk_widget_set_visible(thumb->w_audio, thumb->has_audio);
  gtk_widget_set_visible(thumb->w_color, thumb->colorlabels != 0);

  _set_flag(thumb->w_main, GTK_STATE_FLAG_PRELIGHT, thumb->mouse_over);
  _set_flag(thumb->w_main, GTK_STATE_FLAG_ACTIVE, thumb->active);

  _set_flag(thumb->w_reject, GTK_STATE_FLAG_ACTIVE, (thumb->rating == DT_VIEW_REJECT));
  for(int i = 0; i < MAX_STARS; i++)
    _set_flag(thumb->w_stars[i], GTK_STATE_FLAG_ACTIVE, (thumb->rating > i && thumb->rating < DT_VIEW_REJECT));
  _set_flag(thumb->w_group, GTK_STATE_FLAG_ACTIVE, (thumb->imgid == thumb->groupid));

  _set_flag(thumb->w_main, GTK_STATE_FLAG_SELECTED, thumb->selected);

  // and the tooltip
  gchar *pattern = dt_conf_get_string("plugins/lighttable/thumbnail_tooltip_pattern");
  if(strcmp(pattern, "") == 0)
  {
    gtk_widget_set_has_tooltip(thumb->w_main, FALSE);
  }
  else
  {
    // we compute the info line (we reuse the function used in export to disk)
    char input_dir[1024] = { 0 };
    gboolean from_cache = TRUE;
    dt_image_full_path(thumb->imgid, input_dir, sizeof(input_dir), &from_cache);

    dt_variables_params_t *vp;
    dt_variables_params_init(&vp);

    vp->filename = input_dir;
    vp->jobcode = "infos";
    vp->imgid = thumb->imgid;
    vp->sequence = 0;

    gchar *msg = dt_variables_expand(vp, pattern, TRUE);

    dt_variables_params_destroy(vp);

    // we change the label
    gtk_widget_set_tooltip_markup(thumb->w_main, msg);

    g_free(msg);
  }
  g_free(pattern);
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
    _thumb_update_icons(thumb);
    gtk_widget_queue_draw(thumb->w_main);
  }
}

static void _dt_active_images_callback(gpointer instance, gpointer user_data)
{
  if(!user_data) return;
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  if(!thumb) return;

  gboolean active = FALSE;
  GSList *l = darktable.view_manager->active_images;
  while(l)
  {
    int id = GPOINTER_TO_INT(l->data);
    if(id == thumb->imgid)
    {
      active = TRUE;
      break;
    }
    l = g_slist_next(l);
  }

  // if there's a change, update the thumb
  if(active != thumb->active)
  {
    thumb->active = active;
    _thumb_update_icons(thumb);
    gtk_widget_queue_draw(thumb->w_main);
  }
}

static void _dt_preview_updated_callback(gpointer instance, gpointer user_data)
{
  if(!user_data) return;
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  if(!thumb) return;

  const dt_view_t *v = dt_view_manager_get_current_view(darktable.view_manager);
  if(v->view(v) == DT_VIEW_DARKROOM && darktable.develop->preview_pipe->output_imgid == thumb->imgid
     && darktable.develop->preview_pipe->output_backbuf)
  {
    // reset surface
    thumb->img_surf_dirty = TRUE;
    gtk_widget_queue_draw(thumb->w_main);
  }
}

static void _dt_mipmaps_updated_callback(gpointer instance, int imgid, gpointer user_data)
{
  if(!user_data) return;
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  if(!thumb || (imgid > 0 && thumb->imgid != imgid)) return;

  // reset surface
  thumb->img_surf_dirty = TRUE;
  gtk_widget_queue_draw(thumb->w_main);
}

static gboolean _event_box_enter_leave(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  if(!thumb->mouse_over && event->type == GDK_ENTER_NOTIFY && !thumb->disable_mouseover)
    dt_control_set_mouse_over_id(thumb->imgid);
  _set_flag(widget, GTK_STATE_FLAG_PRELIGHT, (event->type == GDK_ENTER_NOTIFY));
  return FALSE;
}

static gboolean _event_star_enter(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  if(!thumb->mouse_over && !thumb->disable_mouseover) dt_control_set_mouse_over_id(thumb->imgid);
  _set_flag(thumb->w_bottom_eb, GTK_STATE_FLAG_PRELIGHT, TRUE);

  // we prelight all stars before the current one
  gboolean pre = TRUE;
  for(int i = 0; i < MAX_STARS; i++)
  {
    _set_flag(thumb->w_stars[i], GTK_STATE_FLAG_PRELIGHT, pre);
    gtk_widget_queue_draw(thumb->w_stars[i]);
    if(thumb->w_stars[i] == widget) pre = FALSE;
  }
  return TRUE;
}
static gboolean _event_star_leave(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  for(int i = 0; i < MAX_STARS; i++)
  {
    _set_flag(thumb->w_stars[i], GTK_STATE_FLAG_PRELIGHT, FALSE);
    gtk_widget_queue_draw(thumb->w_stars[i]);
  }
  return TRUE;
}

// we only want to specify that the mouse is hovereing the thumbnail
static gboolean _event_main_drag_motion(GtkWidget *widget, GdkDragContext *dc, gint x, gint y, guint time,
                                        gpointer user_data)
{
  _event_main_motion(widget, NULL, user_data);
  return TRUE;
}

GtkWidget *dt_thumbnail_create_widget(dt_thumbnail_t *thumb)
{
  // main widget (overlay)
  thumb->w_main = gtk_overlay_new();
  gtk_widget_set_name(thumb->w_main, "thumb_main");

  if(thumb->imgid > 0)
  {
    // this is only here to ensure that mouse-over value is updated correctly
    // all dragging actions take place inside thumbatble.c
    gtk_drag_dest_set(thumb->w_main, GTK_DEST_DEFAULT_MOTION, target_list_all, n_targets_all, GDK_ACTION_COPY);
    g_signal_connect(G_OBJECT(thumb->w_main), "drag-motion", G_CALLBACK(_event_main_drag_motion), thumb);

    g_signal_connect(G_OBJECT(thumb->w_main), "button-press-event", G_CALLBACK(_event_main_press), thumb);
    g_signal_connect(G_OBJECT(thumb->w_main), "button-release-event", G_CALLBACK(_event_main_release), thumb);

    g_object_set_data(G_OBJECT(thumb->w_main), "thumb", thumb);
    dt_control_signal_connect(darktable.signals, DT_SIGNAL_ACTIVE_IMAGES_CHANGE,
                              G_CALLBACK(_dt_active_images_callback), thumb);
    dt_control_signal_connect(darktable.signals, DT_SIGNAL_SELECTION_CHANGED,
                              G_CALLBACK(_dt_selection_changed_callback), thumb);
    dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_MIPMAP_UPDATED,
                              G_CALLBACK(_dt_mipmaps_updated_callback), thumb);
    dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                              G_CALLBACK(_dt_preview_updated_callback), thumb);

    // the background
    thumb->w_back = gtk_event_box_new();
    gtk_widget_set_events(thumb->w_back, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_STRUCTURE_MASK
                                             | GDK_ENTER_NOTIFY_MASK | GDK_POINTER_MOTION_HINT_MASK
                                             | GDK_POINTER_MOTION_MASK);
    gtk_widget_set_name(thumb->w_back, "thumb_back");
    g_signal_connect(G_OBJECT(thumb->w_back), "motion-notify-event", G_CALLBACK(_event_main_motion), thumb);
    gtk_widget_show(thumb->w_back);
    gtk_container_add(GTK_CONTAINER(thumb->w_main), thumb->w_back);

    // the file extension label
    thumb->w_ext = gtk_label_new("");
    gtk_widget_set_name(thumb->w_ext, "thumb_ext");
    gtk_widget_set_valign(thumb->w_ext, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_ext, GTK_ALIGN_START);
    gtk_label_set_justify(GTK_LABEL(thumb->w_ext), GTK_JUSTIFY_CENTER);
    gtk_widget_show(thumb->w_ext);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_ext);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(thumb->w_main), thumb->w_ext, TRUE);

    // the image drawing area
    thumb->w_image = gtk_drawing_area_new();
    gtk_widget_set_name(thumb->w_image, "thumb_image");
    gtk_widget_set_size_request(thumb->w_image, thumb->width, thumb->height);
    gtk_widget_set_valign(thumb->w_image, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_image, GTK_ALIGN_START);
    gtk_widget_set_events(thumb->w_image, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_STRUCTURE_MASK
                                              | GDK_ENTER_NOTIFY_MASK | GDK_POINTER_MOTION_HINT_MASK
                                              | GDK_POINTER_MOTION_MASK);
    g_signal_connect(G_OBJECT(thumb->w_image), "draw", G_CALLBACK(_event_image_draw), thumb);
    g_signal_connect(G_OBJECT(thumb->w_image), "motion-notify-event", G_CALLBACK(_event_main_motion), thumb);
    gtk_widget_show(thumb->w_image);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_image);

    // the infos background
    thumb->w_bottom_eb = gtk_event_box_new();
    gtk_widget_set_name(thumb->w_bottom_eb, "thumb_bottom");
    g_signal_connect(G_OBJECT(thumb->w_bottom_eb), "enter-notify-event", G_CALLBACK(_event_box_enter_leave),
                     thumb);
    g_signal_connect(G_OBJECT(thumb->w_bottom_eb), "leave-notify-event", G_CALLBACK(_event_box_enter_leave),
                     thumb);
    gtk_widget_set_valign(thumb->w_bottom_eb, GTK_ALIGN_END);
    gtk_widget_set_halign(thumb->w_bottom_eb, GTK_ALIGN_CENTER);
    gtk_widget_show(thumb->w_bottom_eb);
    if(thumb->over == DT_THUMBNAIL_OVERLAYS_ALWAYS_EXTENDED || thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_EXTENDED
       || thumb->over == DT_THUMBNAIL_OVERLAYS_MIXED)
    {
      gchar *lb = dt_util_dstrcat(NULL, "%s", thumb->info_line);
      thumb->w_bottom = gtk_label_new(lb);
      g_free(lb);
    }
    else
      thumb->w_bottom = gtk_label_new("");
    gtk_widget_set_name(thumb->w_bottom, "thumb_bottom_label");
    gtk_widget_show(thumb->w_bottom);
    gtk_label_set_yalign(GTK_LABEL(thumb->w_bottom), 0.05);
    gtk_label_set_ellipsize(GTK_LABEL(thumb->w_bottom), PANGO_ELLIPSIZE_MIDDLE);
    gtk_container_add(GTK_CONTAINER(thumb->w_bottom_eb), thumb->w_bottom);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_bottom_eb);

    // the reject icon
    thumb->w_reject = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_reject, 0, NULL);
    gtk_widget_set_name(thumb->w_reject, "thumb_reject");
    gtk_widget_set_valign(thumb->w_reject, GTK_ALIGN_END);
    gtk_widget_set_halign(thumb->w_reject, GTK_ALIGN_START);
    gtk_widget_show(thumb->w_reject);
    g_signal_connect(G_OBJECT(thumb->w_reject), "button-release-event", G_CALLBACK(_event_rating_release), thumb);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_reject);

    // the stars
    for(int i = 0; i < MAX_STARS; i++)
    {
      thumb->w_stars[i] = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_star, 0, NULL);
      g_signal_connect(G_OBJECT(thumb->w_stars[i]), "enter-notify-event", G_CALLBACK(_event_star_enter), thumb);
      g_signal_connect(G_OBJECT(thumb->w_stars[i]), "leave-notify-event", G_CALLBACK(_event_star_leave), thumb);
      g_signal_connect(G_OBJECT(thumb->w_stars[i]), "button-press-event", G_CALLBACK(_event_rating_press), thumb);
      g_signal_connect(G_OBJECT(thumb->w_stars[i]), "button-release-event", G_CALLBACK(_event_rating_release),
                       thumb);
      gtk_widget_set_name(thumb->w_stars[i], "thumb_star");
      gtk_widget_set_valign(thumb->w_stars[i], GTK_ALIGN_END);
      gtk_widget_set_halign(thumb->w_stars[i], GTK_ALIGN_START);
      gtk_widget_show(thumb->w_stars[i]);
      gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_stars[i]);
    }

    // the color labels
    thumb->w_color = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_label_flower,
                                             CPF_DO_NOT_USE_BORDER | thumb->colorlabels, NULL);
    gtk_widget_set_name(thumb->w_color, "thumb_colorlabels");
    gtk_widget_set_valign(thumb->w_color, GTK_ALIGN_END);
    gtk_widget_set_halign(thumb->w_color, GTK_ALIGN_END);
    gtk_widget_set_no_show_all(thumb->w_color, TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_color);

    // the local copy indicator
    thumb->w_local_copy = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_local_copy, CPF_DO_NOT_USE_BORDER, NULL);
    gtk_widget_set_name(thumb->w_local_copy, "thumb_localcopy");
    gtk_widget_set_valign(thumb->w_local_copy, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_local_copy, GTK_ALIGN_END);
    gtk_widget_set_no_show_all(thumb->w_local_copy, TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_local_copy);

    // the altered icon
    thumb->w_altered = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_altered, CPF_DO_NOT_USE_BORDER, NULL);
    gtk_widget_set_name(thumb->w_altered, "thumb_altered");
    gtk_widget_set_valign(thumb->w_altered, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_altered, GTK_ALIGN_END);
    gtk_widget_set_no_show_all(thumb->w_altered, TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_altered);

    // the group bouton
    thumb->w_group = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_grouping, CPF_DO_NOT_USE_BORDER, NULL);
    gtk_widget_set_name(thumb->w_group, "thumb_group");
    g_signal_connect(G_OBJECT(thumb->w_group), "button-release-event", G_CALLBACK(_event_grouping_release), thumb);
    gtk_widget_set_valign(thumb->w_group, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_group, GTK_ALIGN_END);
    gtk_widget_set_no_show_all(thumb->w_group, TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_group);

    // the sound icon
    thumb->w_audio = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_audio, CPF_DO_NOT_USE_BORDER, NULL);
    gtk_widget_set_name(thumb->w_audio, "thumb_audio");
    g_signal_connect(G_OBJECT(thumb->w_audio), "button-release-event", G_CALLBACK(_event_audio_release), thumb);
    gtk_widget_set_valign(thumb->w_audio, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_audio, GTK_ALIGN_END);
    gtk_widget_set_no_show_all(thumb->w_audio, TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_audio);

    dt_thumbnail_resize(thumb, thumb->width, thumb->height, TRUE);
  }
  gtk_widget_show(thumb->w_main);
  g_object_ref(G_OBJECT(thumb->w_main));
  return thumb->w_main;
}

dt_thumbnail_t *dt_thumbnail_new(int width, int height, int imgid, int rowid, dt_thumbnail_overlay_t over)
{
  dt_thumbnail_t *thumb = calloc(1, sizeof(dt_thumbnail_t));
  thumb->width = width;
  thumb->height = height;
  thumb->imgid = imgid;
  thumb->rowid = rowid;
  thumb->over = over;

  // we read and cache all the infos from dt_image_t that we need
  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, thumb->imgid, 'r');
  if(img)
  {
    thumb->filename = g_strdup(img->filename);
    if(thumb->over != DT_THUMBNAIL_OVERLAYS_NONE)
    {
      thumb->has_audio = (img->flags & DT_IMAGE_HAS_WAV);
      thumb->has_localcopy = (img->flags & DT_IMAGE_LOCAL_COPY);
    }
    dt_image_cache_read_release(darktable.image_cache, img);
  }
  if(thumb->over == DT_THUMBNAIL_OVERLAYS_ALWAYS_EXTENDED || thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_EXTENDED
     || over == DT_THUMBNAIL_OVERLAYS_MIXED)
    _thumb_update_extended_infos_line(thumb);

  // we read all other infos
  _image_get_infos(thumb);

  // we create the widget
  dt_thumbnail_create_widget(thumb);

  // let's see if the images are selected or active or mouse_overed
  _dt_active_images_callback(NULL, thumb);
  _dt_selection_changed_callback(NULL, thumb);
  if(dt_control_get_mouse_over_id() == thumb->imgid) dt_thumbnail_set_mouseover(thumb, TRUE);

  // set tooltip for altered icon if needed
  if(thumb->is_altered)
  {
    char *tooltip = dt_history_get_items_as_string(thumb->imgid);
    if(tooltip)
    {
      gtk_widget_set_tooltip_text(thumb->w_altered, tooltip);
      g_free(tooltip);
    }
  }

  // ensure all icons are up to date
  _thumb_update_icons(thumb);

  return thumb;
}

void dt_thumbnail_destroy(dt_thumbnail_t *thumb)
{
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_dt_selection_changed_callback), thumb);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_dt_active_images_callback), thumb);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_dt_mipmaps_updated_callback), thumb);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_dt_preview_updated_callback), thumb);
  if(thumb->img_surf && cairo_surface_get_reference_count(thumb->img_surf) > 0)
    cairo_surface_destroy(thumb->img_surf);
  thumb->img_surf = NULL;
  if(thumb->w_main) gtk_widget_destroy(thumb->w_main);
  if(thumb->filename) g_free(thumb->filename);
  if(thumb->img_margin) gtk_border_free(thumb->img_margin);
  free(thumb);
}

void dt_thumbnail_update_infos(dt_thumbnail_t *thumb)
{
  if(!thumb) return;
  _image_get_infos(thumb);
  _thumb_update_icons(thumb);
}

void dt_thumbnail_resize(dt_thumbnail_t *thumb, int width, int height, gboolean force)
{
  // first, we verify that there's something to change
  if(!force)
  {
    int w = 0;
    int h = 0;
    gtk_widget_get_size_request(thumb->w_main, &w, &h);
    if(w == width && h == height) return;
  }

  // we need to squeeze 5 stars + 1 reject + 1 colorlabels symbols on a thumbnail width
  // stars + reject having a width of 2 * r1 and spaced by r1 => 18 * r1
  // colorlabels => 3 * r1 + space r1
  // inner margins are 0.045 * width
  const float r1 = fminf(DT_PIXEL_APPLY_DPI(20.0f) / 2.0f, 0.91 * width / 22.0f);

  // widget resizing
  thumb->width = width;
  thumb->height = height;
  gtk_widget_set_size_request(thumb->w_main, width, height);
  // file extension
  gtk_widget_set_margin_start(thumb->w_ext, 0.045 * width);
  gtk_widget_set_margin_top(thumb->w_ext, 0.5 * r1);
  const int fsize = fminf(DT_PIXEL_APPLY_DPI(20.0), .09 * width);
  PangoAttrList *attrlist = pango_attr_list_new();
  PangoAttribute *attr = pango_attr_size_new_absolute(fsize * PANGO_SCALE);
  pango_attr_list_insert(attrlist, attr);
  // the idea is to reduce line-height, but it doesn't work for whatever reason...
  // PangoAttribute *attr2 = pango_attr_rise_new(-fsize * PANGO_SCALE);
  // pango_attr_list_insert(attrlist, attr2);
  gtk_label_set_attributes(GTK_LABEL(thumb->w_ext), attrlist);
  pango_attr_list_unref(attrlist);

  // bottom background
  if(thumb->over == DT_THUMBNAIL_OVERLAYS_ALWAYS_EXTENDED || thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_EXTENDED
     || thumb->over == DT_THUMBNAIL_OVERLAYS_MIXED)
  {
    attrlist = pango_attr_list_new();
    attr = pango_attr_size_new_absolute(1.5 * r1 * PANGO_SCALE);
    pango_attr_list_insert(attrlist, attr);
    gtk_label_set_attributes(GTK_LABEL(thumb->w_bottom), attrlist);
    pango_attr_list_unref(attrlist);
    int w = 0;
    int h = 0;
    pango_layout_get_pixel_size(gtk_label_get_layout(GTK_LABEL(thumb->w_bottom)), &w, &h);
    gtk_widget_set_size_request(thumb->w_bottom, width, 4.0 * r1 + h);
  }
  else
    gtk_widget_set_size_request(thumb->w_bottom, width, 4.0 * r1);
  // reject icon
  gtk_widget_set_size_request(thumb->w_reject, 3.0 * r1, 3.0 * r1);
  gtk_widget_set_margin_start(thumb->w_reject, 0.045 * width - r1 * 0.75);
  gtk_widget_set_margin_bottom(thumb->w_reject, 0.5 * r1);
  // stars
  for(int i = 0; i < MAX_STARS; i++)
  {
    gtk_widget_set_size_request(thumb->w_stars[i], 3.0 * r1, 3.0 * r1);
    gtk_widget_set_margin_bottom(thumb->w_stars[i], 0.5 * r1);
    gtk_widget_set_margin_start(thumb->w_stars[i], (width - 15.0 * r1) * 0.5 + i * 3.0 * r1);
  }
  // the color labels
  gtk_widget_set_size_request(thumb->w_color, 3.0 * r1, 3.0 * r1);
  gtk_widget_set_margin_bottom(thumb->w_color, 0.5 * r1);
  gtk_widget_set_margin_end(thumb->w_color, 0.045 * width);
  // the local copy indicator
  gtk_widget_set_size_request(thumb->w_local_copy, 2.0 * r1, 2.0 * r1);
  // the altered icon
  gtk_widget_set_size_request(thumb->w_altered, 2.0 * r1, 2.0 * r1);
  gtk_widget_set_margin_top(thumb->w_altered, 0.5 * r1);
  gtk_widget_set_margin_end(thumb->w_altered, 0.045 * width);
  // the group bouton
  gtk_widget_set_size_request(thumb->w_group, 2.0 * r1, 2.0 * r1);
  gtk_widget_set_margin_top(thumb->w_group, 0.5 * r1);
  gtk_widget_set_margin_end(thumb->w_group, 0.045 * width + 3.0 * r1);
  // the sound icon
  gtk_widget_set_size_request(thumb->w_audio, 2.0 * r1, 2.0 * r1);
  gtk_widget_set_margin_top(thumb->w_audio, 0.5 * r1);
  gtk_widget_set_margin_end(thumb->w_audio, 0.045 * width + 6.0 * r1);

  // update values
  thumb->width = width;
  thumb->height = height;

  // reset surface
  if(thumb->img_surf && cairo_surface_get_reference_count(thumb->img_surf) > 0)
    cairo_surface_destroy(thumb->img_surf);
  thumb->img_surf = NULL;
}

void dt_thumbnail_set_group_border(dt_thumbnail_t *thumb, dt_thumbnail_border_t border)
{
  GtkStyleContext *context = gtk_widget_get_style_context(thumb->w_main);
  if(border == DT_THUMBNAIL_BORDER_NONE)
  {
    gtk_style_context_remove_class(context, "dt_group_left");
    gtk_style_context_remove_class(context, "dt_group_top");
    gtk_style_context_remove_class(context, "dt_group_right");
    gtk_style_context_remove_class(context, "dt_group_bottom");
    thumb->group_borders = DT_THUMBNAIL_BORDER_NONE;
    return;
  }
  else if(border & DT_THUMBNAIL_BORDER_LEFT)
    gtk_style_context_add_class(context, "dt_group_left");
  else if(border & DT_THUMBNAIL_BORDER_TOP)
    gtk_style_context_add_class(context, "dt_group_top");
  else if(border & DT_THUMBNAIL_BORDER_RIGHT)
    gtk_style_context_add_class(context, "dt_group_right");
  else if(border & DT_THUMBNAIL_BORDER_BOTTOM)
    gtk_style_context_add_class(context, "dt_group_bottom");

  thumb->group_borders |= border;
}

void dt_thumbnail_set_mouseover(dt_thumbnail_t *thumb, gboolean over)
{
  if(thumb->mouse_over == over) return;
  thumb->mouse_over = over;
  _thumb_update_icons(thumb);

  if(!thumb->mouse_over)
  {
    _set_flag(thumb->w_bottom_eb, GTK_STATE_FLAG_PRELIGHT, FALSE);
  }
  gtk_widget_queue_draw(thumb->w_main);
}

// set if the thumbnail should react (mouse_over) to drag and drop
// note that it's just cosmetic as dropping occurs in thumbtable in any case
void dt_thumbnail_set_drop(dt_thumbnail_t *thumb, gboolean accept_drop)
{
  if(accept_drop)
  {
    gtk_drag_dest_set(thumb->w_main, GTK_DEST_DEFAULT_MOTION, target_list_all, n_targets_all, GDK_ACTION_COPY);
  }
  else
  {
    gtk_drag_dest_unset(thumb->w_main);
  }
}

// force the image to be reloaded from cache if any
void dt_thumbnail_image_refresh(dt_thumbnail_t *thumb)
{
  thumb->img_surf_dirty = TRUE;
  gtk_widget_queue_draw(thumb->w_main);
}

void dt_thumbnail_set_extended_overlay(dt_thumbnail_t *thumb, dt_thumbnail_overlay_t over)
{
  // if no change, do nothing...
  if(thumb->over == over) return;
  gchar *lb = NULL;
  dt_thumbnail_overlay_t old_over = thumb->over;
  thumb->over = over;

  // we read and cache all the infos from dt_image_t that we need, depending on the overlay level
  // note that when "downgrading" overlay level, we don't bother to remove the infos
  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, thumb->imgid, 'r');
  if(img)
  {
    if(old_over == DT_THUMBNAIL_OVERLAYS_NONE)
    {
      thumb->filename = g_strdup(img->filename);
      thumb->has_audio = (img->flags & DT_IMAGE_HAS_WAV);
      thumb->has_localcopy = (img->flags & DT_IMAGE_LOCAL_COPY);
    }

    dt_image_cache_read_release(darktable.image_cache, img);
  }
  if(over == DT_THUMBNAIL_OVERLAYS_ALWAYS_EXTENDED || over == DT_THUMBNAIL_OVERLAYS_HOVER_EXTENDED
     || over == DT_THUMBNAIL_OVERLAYS_MIXED)
  {
    _thumb_update_extended_infos_line(thumb);
  }

  // we read all other infos
  if(old_over == DT_THUMBNAIL_OVERLAYS_NONE)
  {
    _image_get_infos(thumb);
    _thumb_update_icons(thumb);
  }

  // extended overlay text
  if(over == DT_THUMBNAIL_OVERLAYS_ALWAYS_EXTENDED || over == DT_THUMBNAIL_OVERLAYS_HOVER_EXTENDED
     || over == DT_THUMBNAIL_OVERLAYS_MIXED)
    lb = dt_util_dstrcat(NULL, "%s", thumb->info_line);

  // we set the text
  gtk_label_set_text(GTK_LABEL(thumb->w_bottom), lb);
  g_free(lb);
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
