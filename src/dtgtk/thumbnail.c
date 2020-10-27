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
#include "common/focus.h"
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

static void _thumb_resize_overlays(dt_thumbnail_t *thumb);

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
  vp->escape_markup = TRUE;

  if(thumb->info_line) g_free(thumb->info_line);
  thumb->info_line = dt_variables_expand(vp, pattern, TRUE);

  dt_variables_params_destroy(vp);

  g_free(pattern);
}

static void _image_update_group_tooltip(dt_thumbnail_t *thumb)
{
  if(!thumb->w_group) return;
  if(!thumb->is_grouped)
  {
    gtk_widget_set_has_tooltip(thumb->w_group, FALSE);
    return;
  }

  gchar *tt = NULL;
  int nb = 0;

  // the group leader
  if(thumb->imgid == thumb->groupid)
    tt = dt_util_dstrcat(tt, "\n<b>%s (%s)</b>", _("current"), _("leader"));
  else
  {
    const dt_image_t *img = dt_image_cache_get(darktable.image_cache, thumb->groupid, 'r');
    if(img)
    {
      tt = dt_util_dstrcat(tt, "\n<b>%s (%s)</b>", img->filename, _("leader"));
      dt_image_cache_read_release(darktable.image_cache, img);
    }
  }

  // and the other images
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT id, version, filename FROM main.images WHERE group_id = ?1", -1, &stmt,
                              NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, thumb->groupid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    nb++;
    const int id = sqlite3_column_int(stmt, 0);
    const int v = sqlite3_column_int(stmt, 1);

    if(id != thumb->groupid)
    {
      if(id == thumb->imgid)
        tt = dt_util_dstrcat(tt, "\n%s", _("current"));
      else
      {
        tt = dt_util_dstrcat(tt, "\n%s", sqlite3_column_text(stmt, 2));
        if(v > 0) tt = dt_util_dstrcat(tt, " v%d", v);
      }
    }
  }
  sqlite3_finalize(stmt);

  // and the number of grouped images
  gchar *ttf = dt_util_dstrcat(NULL, "%d %s\n%s", nb, _("grouped images"), tt);
  g_free(tt);

  // let's apply the tooltip
  gtk_widget_set_tooltip_markup(thumb->w_group, ttf);
  g_free(ttf);
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
    thumb->is_bw = dt_image_monochrome_flags(img);
    thumb->is_bw_flow = dt_image_use_monochrome_workflow(img);
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

  // grouping tooltip
  _image_update_group_tooltip(thumb);
}

static gboolean _thumb_expose_again(gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  if(!thumb) return FALSE;
  gpointer w_image = thumb->w_image;
  if(!w_image || !GTK_IS_WIDGET(w_image)) return FALSE;

  thumb->expose_again_timeout_id = 0;
  gtk_widget_queue_draw((GtkWidget *)w_image);
  return FALSE;
}

static void _thumb_draw_image(dt_thumbnail_t *thumb, cairo_t *cr)
{
  // Safety check to avoid possible error
  if(!thumb->img_surf || cairo_surface_get_reference_count(thumb->img_surf) < 1) return;

  // we draw the image
  GtkStyleContext *context = gtk_widget_get_style_context(thumb->w_image_box);
  int w = 0;
  int h = 0;
  gtk_widget_get_size_request(thumb->w_image_box, &w, &h);

  const float scaler = 1.0f / darktable.gui->ppd_thb;
  cairo_scale(cr, scaler, scaler);

  cairo_set_source_surface(cr, thumb->img_surf, thumb->current_zx * darktable.gui->ppd, thumb->current_zy * darktable.gui->ppd);
  cairo_paint(cr);

  // and eventually the image border
  gtk_render_frame(context, cr, 0, 0, w * darktable.gui->ppd_thb, h * darktable.gui->ppd_thb);
}

static void _thumb_retrieve_margins(dt_thumbnail_t *thumb)
{
  if(thumb->img_margin) gtk_border_free(thumb->img_margin);
  // we retrieve image margins from css
  GtkStateFlags state = gtk_widget_get_state_flags(thumb->w_image);
  thumb->img_margin = gtk_border_new();
  GtkStyleContext *context = gtk_widget_get_style_context(thumb->w_image);
  gtk_style_context_get_margin(context, state, thumb->img_margin);

  // and we apply it to the thumb size
  int width, height;
  gtk_widget_get_size_request(thumb->w_main, &width, &height);
  thumb->img_margin->left = MAX(0, thumb->img_margin->left * width / 1000);
  thumb->img_margin->top = MAX(0, thumb->img_margin->top * height / 1000);
  thumb->img_margin->right = MAX(0, thumb->img_margin->right * width / 1000);
  thumb->img_margin->bottom = MAX(0, thumb->img_margin->bottom * height / 1000);
}

static void _thumb_write_extension(dt_thumbnail_t *thumb)
{
  // fill the file extension label
  const char *ext = thumb->filename + strlen(thumb->filename);
  gchar *ext2 = NULL;
  while(ext > thumb->filename && *ext != '.') ext--;
  ext++;
  gchar *uext = dt_view_extend_modes_str(ext, thumb->is_hdr, thumb->is_bw, thumb->is_bw_flow);
  ext2 = dt_util_dstrcat(ext2, "%s", uext);
  gtk_label_set_text(GTK_LABEL(thumb->w_ext), ext2);
  g_free(uext);
  g_free(ext2);
}

static gboolean _event_cursor_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  if(!user_data || !widget) return TRUE;
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;

  GtkStateFlags state = gtk_widget_get_state_flags(thumb->w_cursor);
  GtkStyleContext *context = gtk_widget_get_style_context(thumb->w_cursor);
  GdkRGBA col;
  gtk_style_context_get_color(context, state, &col);

  cairo_set_source_rgba(cr, col.red, col.green, col.blue, col.alpha);
  cairo_line_to(cr, gtk_widget_get_allocated_width(widget), 0);
  cairo_line_to(cr, gtk_widget_get_allocated_width(widget) / 2, gtk_widget_get_allocated_height(widget));
  cairo_line_to(cr, 0, 0);
  cairo_close_path(cr);
  cairo_fill(cr);

  return TRUE;
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
    // let's ensure we have the right margins
    _thumb_retrieve_margins(thumb);

    int image_w, image_h;
    if(thumb->over == DT_THUMBNAIL_OVERLAYS_ALWAYS_NORMAL || thumb->over == DT_THUMBNAIL_OVERLAYS_ALWAYS_EXTENDED)
    {
      image_w = thumb->width - thumb->img_margin->left - thumb->img_margin->right;
      int w = 0;
      int h = 0;
      gtk_widget_get_size_request(thumb->w_bottom_eb, &w, &h);
      image_h = thumb->height - h;
      gtk_widget_get_size_request(thumb->w_altered, &w, &h);
      if (!thumb->zoomable) image_h -= h + gtk_widget_get_margin_top(thumb->w_altered);
      else
        image_h -= thumb->img_margin->bottom;
      image_h -= thumb->img_margin->top;
    }
    else if(thumb->over == DT_THUMBNAIL_OVERLAYS_MIXED)
    {
      image_w = thumb->width - thumb->img_margin->left - thumb->img_margin->right;
      int w = 0;
      int h = 0;
      gtk_widget_get_size_request(thumb->w_reject, &w, &h);
      image_h = thumb->height - (h + gtk_widget_get_margin_bottom(thumb->w_reject));
      gtk_widget_get_size_request(thumb->w_altered, &w, &h);
      image_h -= h + gtk_widget_get_margin_top(thumb->w_altered);
      image_h -= thumb->img_margin->top + thumb->img_margin->bottom;
    }
    else
    {
      image_w = thumb->width - thumb->img_margin->left - thumb->img_margin->right;
      image_h = thumb->height - thumb->img_margin->top - thumb->img_margin->bottom;
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
        const float scale = fminf(image_w / (float)buf_width, image_h / (float)buf_height) * darktable.gui->ppd_thb;
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
          cairo_pattern_set_filter(cairo_get_source(cr2), darktable.gui->filter_image);

        cairo_paint(cr2);

        if(darktable.gui->show_focus_peaking)
        {
          cairo_save(cr2);
          cairo_scale(cr2, 1.0f/scale, 1.0f/scale);
          dt_focuspeaking(cr2, img_width, img_height, cairo_image_surface_get_data(thumb->img_surf),
                          cairo_image_surface_get_width(thumb->img_surf),
                          cairo_image_surface_get_height(thumb->img_surf));
          cairo_restore(cr2);
        }

        cairo_surface_destroy(tmp_surface);
        cairo_destroy(cr2);
      }
      if(rgbbuf) g_free(rgbbuf);
    }
    else
    {
      gboolean res;
      cairo_surface_t *img_surf = NULL;
      if(thumb->zoomable)
      {
        if(thumb->zoom > 1.0f) thumb->zoom = MIN(thumb->zoom, dt_thumbnail_get_zoom100(thumb));
        res = dt_view_image_get_surface(thumb->imgid, image_w * thumb->zoom, image_h * thumb->zoom, &img_surf, FALSE);
      }
      else
      {
        res = dt_view_image_get_surface(thumb->imgid, image_w, image_h, &img_surf, FALSE);
      }

      if(res)
      {
        // if the image is missing, we reload it again
        if(!thumb->expose_again_timeout_id)
          thumb->expose_again_timeout_id = g_timeout_add(250, _thumb_expose_again, thumb);

        // we still draw the thumb to avoid flickering
        _thumb_draw_image(thumb, cr);
        return TRUE;
      }

      cairo_surface_t *tmp_surf = thumb->img_surf;
      thumb->img_surf = img_surf;
      if(tmp_surf && cairo_surface_get_reference_count(tmp_surf) > 0) cairo_surface_destroy(tmp_surf);

      if(thumb->display_focus)
      {
        uint8_t *full_res_thumb = NULL;
        int32_t full_res_thumb_wd, full_res_thumb_ht;
        dt_colorspaces_color_profile_type_t color_space;
        char path[PATH_MAX] = { 0 };
        gboolean from_cache = TRUE;
        dt_image_full_path(thumb->imgid, path, sizeof(path), &from_cache);
        if(!dt_imageio_large_thumbnail(path, &full_res_thumb, &full_res_thumb_wd, &full_res_thumb_ht, &color_space))
        {
          // we look for focus areas
          dt_focus_cluster_t full_res_focus[49];
          const int frows = 5, fcols = 5;
          dt_focus_create_clusters(full_res_focus, frows, fcols, full_res_thumb, full_res_thumb_wd,
                                   full_res_thumb_ht);
          // and we draw them on the image
          cairo_t *cri = cairo_create(thumb->img_surf);
          dt_focus_draw_clusters(cri, cairo_image_surface_get_width(thumb->img_surf),
                                 cairo_image_surface_get_height(thumb->img_surf), thumb->imgid, full_res_thumb_wd,
                                 full_res_thumb_ht, full_res_focus, frows, fcols, 1.0, 0, 0);
          cairo_destroy(cri);
        }
        dt_free_align(full_res_thumb);
      }
    }

    thumb->img_surf_dirty = FALSE;
    // let save thumbnail image size
    thumb->img_width = cairo_image_surface_get_width(thumb->img_surf);
    thumb->img_height = cairo_image_surface_get_height(thumb->img_surf);
    const int imgbox_w = MIN(image_w, thumb->img_width/darktable.gui->ppd_thb);
    const int imgbox_h = MIN(image_h, thumb->img_height/darktable.gui->ppd_thb);
    // if the imgbox size change, this should also change the panning values
    int hh = 0;
    int ww = 0;
    gtk_widget_get_size_request(thumb->w_image_box, &ww, &hh);
    thumb->zoomx = thumb->zoomx + (imgbox_w - ww) / 2.0;
    thumb->zoomy = thumb->zoomy + (imgbox_h - hh) / 2.0;
    gtk_widget_set_size_request(thumb->w_image_box, imgbox_w, imgbox_h);
    // and we set the position of the image
    int posx, posy;
    if(thumb->over == DT_THUMBNAIL_OVERLAYS_ALWAYS_NORMAL || thumb->over == DT_THUMBNAIL_OVERLAYS_ALWAYS_EXTENDED)
    {
      posx = thumb->img_margin->left + (image_w - imgbox_w) / 2;
      int w = 0;
      int h = 0;
      if(!thumb->zoomable)
      {
        gtk_widget_get_size_request(thumb->w_altered, &w, &h);
        posy = h + gtk_widget_get_margin_top(thumb->w_altered);
      }
      else
      {
        posy = 0;
      }

      gtk_widget_get_size_request(thumb->w_bottom_eb, &w, &h);
      posy += thumb->img_margin->top + (image_h - imgbox_h) / 2;
    }
    else if(thumb->over == DT_THUMBNAIL_OVERLAYS_MIXED)
    {
      posx = thumb->img_margin->left + (image_w - imgbox_w) / 2;
      int w = 0;
      int h = 0;
      gtk_widget_get_size_request(thumb->w_altered, &w, &h);
      posy = h + gtk_widget_get_margin_top(thumb->w_altered);

      posy += thumb->img_margin->top + (image_h - imgbox_h) / 2;
    }
    else
    {
      posx = thumb->img_margin->left + (image_w - imgbox_w) / 2;
      posy = thumb->img_margin->top + (image_h - imgbox_h) / 2;
    }
    gtk_widget_set_margin_start(thumb->w_image_box, posx);
    gtk_widget_set_margin_top(thumb->w_image_box, posy);

    // for overlay block, we need to resize it
    if(thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK) _thumb_resize_overlays(thumb);

    // and we can also set the zooming level if needed
    if(thumb->zoomable && thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK)
    {
      if(thumb->zoom_100 < 1.0 || thumb->zoom <= 1.0f)
      {
        gtk_label_set_text(GTK_LABEL(thumb->w_zoom), _("fit"));
      }
      else
      {
        gchar *z = dt_util_dstrcat(NULL, "%.0f%%", thumb->zoom * 100.0 / thumb->zoom_100);
        gtk_label_set_text(GTK_LABEL(thumb->w_zoom), z);
        g_free(z);
      }
    }

    // let's sanitize and apply panning values as we are sure the zoomed image is loaded now
    // here we have to make sure to properly align according to ppd
    thumb->zoomx = CLAMP(thumb->zoomx, (imgbox_w * darktable.gui->ppd_thb - thumb->img_width) / darktable.gui->ppd_thb, 0);
    thumb->zoomy = CLAMP(thumb->zoomy, (imgbox_h * darktable.gui->ppd_thb - thumb->img_height) / darktable.gui->ppd_thb, 0);
    thumb->current_zx = thumb->zoomx;
    thumb->current_zy = thumb->zoomy;
  }

  _thumb_draw_image(thumb, cr);

  return TRUE;
}

static void _thumb_update_icons(dt_thumbnail_t *thumb)
{
  gtk_widget_set_visible(thumb->w_local_copy, thumb->has_localcopy);
  gtk_widget_set_visible(thumb->w_altered, thumb->is_altered);
  gtk_widget_set_visible(thumb->w_group, thumb->is_grouped);
  gtk_widget_set_visible(thumb->w_audio, thumb->has_audio);
  gtk_widget_set_visible(thumb->w_color, thumb->colorlabels != 0);
  gtk_widget_set_visible(thumb->w_zoom_eb, (thumb->zoomable && thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK));
  gtk_widget_show(thumb->w_bottom_eb);
  gtk_widget_show(thumb->w_reject);
  gtk_widget_show(thumb->w_ext);
  gtk_widget_show(thumb->w_cursor);
  for(int i = 0; i < MAX_STARS; i++) gtk_widget_show(thumb->w_stars[i]);

  _set_flag(thumb->w_main, GTK_STATE_FLAG_PRELIGHT, thumb->mouse_over);
  _set_flag(thumb->w_main, GTK_STATE_FLAG_ACTIVE, thumb->active);

  _set_flag(thumb->w_reject, GTK_STATE_FLAG_ACTIVE, (thumb->rating == DT_VIEW_REJECT));
  for(int i = 0; i < MAX_STARS; i++)
    _set_flag(thumb->w_stars[i], GTK_STATE_FLAG_ACTIVE, (thumb->rating > i && thumb->rating < DT_VIEW_REJECT));
  _set_flag(thumb->w_group, GTK_STATE_FLAG_ACTIVE, (thumb->imgid == thumb->groupid));

  _set_flag(thumb->w_main, GTK_STATE_FLAG_SELECTED, thumb->selected);

  // and the tooltip
  gchar *pattern = dt_conf_get_string("plugins/lighttable/thumbnail_tooltip_pattern");
  if(!thumb->tooltip || strcmp(pattern, "") == 0)
  {
    gtk_widget_set_has_tooltip(thumb->w_main, FALSE);
  }
  else
  {
    // we compute the tooltip (we reuse the function used in export to disk)
    char input_dir[1024] = { 0 };
    gboolean from_cache = TRUE;
    dt_image_full_path(thumb->imgid, input_dir, sizeof(input_dir), &from_cache);

    dt_variables_params_t *vp;
    dt_variables_params_init(&vp);

    vp->filename = input_dir;
    vp->jobcode = "infos";
    vp->imgid = thumb->imgid;
    vp->sequence = 0;
    vp->escape_markup = TRUE;

    gchar *msg = dt_variables_expand(vp, pattern, TRUE);

    dt_variables_params_destroy(vp);

    // we change the label
    gtk_widget_set_tooltip_markup(thumb->w_main, msg);

    g_free(msg);
  }
  g_free(pattern);

  // we recompte the history tooltip if needed
  thumb->is_altered = dt_image_altered(thumb->imgid);
  gtk_widget_set_visible(thumb->w_altered, thumb->is_altered);
  if(thumb->is_altered)
  {
    char *tooltip = dt_history_get_items_as_string(thumb->imgid);
    if(tooltip)
    {
      gtk_widget_set_tooltip_text(thumb->w_altered, tooltip);
      g_free(tooltip);
    }
  }
}

static gboolean _thumbs_hide_overlays(gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  thumb->overlay_timeout_id = 0;
  // if the mouse is inside the infos block, we don't hide them
  if(gtk_widget_get_state_flags(thumb->w_bottom_eb) & GTK_STATE_FLAG_PRELIGHT) return FALSE;

  gtk_widget_hide(thumb->w_bottom_eb);
  gtk_widget_hide(thumb->w_reject);
  for(int i = 0; i < MAX_STARS; i++) gtk_widget_hide(thumb->w_stars[i]);
  gtk_widget_hide(thumb->w_color);
  gtk_widget_hide(thumb->w_local_copy);
  gtk_widget_hide(thumb->w_altered);
  gtk_widget_hide(thumb->w_group);
  gtk_widget_hide(thumb->w_audio);
  gtk_widget_hide(thumb->w_zoom_eb);
  gtk_widget_hide(thumb->w_ext);
  return G_SOURCE_REMOVE;
}
static gboolean _thumbs_show_overlays(gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  _thumb_update_icons(thumb);
  return G_SOURCE_REMOVE;
}

static gboolean _event_main_motion(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  if(!user_data) return TRUE;
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;

  // first, we hide the block overlays after a delay if the mouse hasn't move
  if(thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK)
  {
    if(thumb->overlay_timeout_id > 0)
    {
      g_source_remove(thumb->overlay_timeout_id);
      thumb->overlay_timeout_id = 0;
    }
    _thumbs_show_overlays(thumb);
    if(thumb->overlay_timeout_duration >= 0)
    {
      thumb->overlay_timeout_id
          = g_timeout_add_seconds(thumb->overlay_timeout_duration, _thumbs_hide_overlays, thumb);
    }
  }

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
  if(thumb->disable_actions) return FALSE;
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
  if(thumb->disable_actions) return FALSE;
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
  if(thumb->disable_actions) return FALSE;
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

// this is called each time the images info change
static void _dt_image_info_changed_callback(gpointer instance, gpointer imgs, gpointer user_data)
{
  if(!user_data || !imgs) return;
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  const GList *i = imgs;
  while(i)
  {
    if(GPOINTER_TO_INT(i->data) == thumb->imgid)
    {
      dt_thumbnail_update_infos(thumb);
      break;
    }
    i = g_list_next(i);
  }
}

// this is called each time collected images change
// we only use this because the image infos may have changed
static void _dt_collection_changed_callback(gpointer instance, dt_collection_change_t query_change, gpointer imgs,
                                            const int next, gpointer user_data)
{
  if(!user_data || !imgs) return;
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  const GList *i = imgs;
  while(i)
  {
    if(GPOINTER_TO_INT(i->data) == thumb->imgid)
    {
      dt_thumbnail_update_infos(thumb);
      break;
    }
    i = g_list_next(i);
  }
}

static void _dt_selection_changed_callback(gpointer instance, gpointer user_data)
{
  if(!user_data) return;
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  if(!thumb) return;
  if(!gtk_widget_is_visible(thumb->w_main)) return;

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
    if(gtk_widget_is_visible(thumb->w_main))
    {
      _thumb_update_icons(thumb);
      gtk_widget_queue_draw(thumb->w_main);
    }
  }
}

static void _dt_preview_updated_callback(gpointer instance, gpointer user_data)
{
  if(!user_data) return;
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  if(!thumb) return;
  if(!gtk_widget_is_visible(thumb->w_main)) return;

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

  // we recompte the history tooltip if needed
  thumb->is_altered = dt_image_altered(thumb->imgid);
  gtk_widget_set_visible(thumb->w_altered, thumb->is_altered);
  if(thumb->is_altered)
  {
    char *tooltip = dt_history_get_items_as_string(thumb->imgid);
    if(tooltip)
    {
      gtk_widget_set_tooltip_text(thumb->w_altered, tooltip);
      g_free(tooltip);
    }
  }

  // reset surface
  thumb->img_surf_dirty = TRUE;
  gtk_widget_queue_draw(thumb->w_main);
}

static gboolean _event_box_enter_leave(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  // if we leave for ancestor, that means we leave for blank thumbtable area
  if(event->type == GDK_LEAVE_NOTIFY && event->detail == GDK_NOTIFY_ANCESTOR) dt_control_set_mouse_over_id(-1);

  if(!thumb->mouse_over && event->type == GDK_ENTER_NOTIFY && !thumb->disable_mouseover)
    dt_control_set_mouse_over_id(thumb->imgid);
  _set_flag(widget, GTK_STATE_FLAG_PRELIGHT, (event->type == GDK_ENTER_NOTIFY));
  _set_flag(thumb->w_image_box, GTK_STATE_FLAG_PRELIGHT, (event->type == GDK_ENTER_NOTIFY));
  return FALSE;
}

static gboolean _event_image_enter_leave(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  _set_flag(thumb->w_image_box, GTK_STATE_FLAG_PRELIGHT, (event->type == GDK_ENTER_NOTIFY));
  return FALSE;
}

static gboolean _event_btn_enter_leave(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  // if we leave for ancestor, that means we leave for blank thumbtable area
  if(event->type == GDK_LEAVE_NOTIFY && event->detail == GDK_NOTIFY_ANCESTOR) dt_control_set_mouse_over_id(-1);

  if(thumb->disable_actions) return TRUE;
  if(event->type == GDK_ENTER_NOTIFY) _set_flag(thumb->w_image_box, GTK_STATE_FLAG_PRELIGHT, TRUE);
  return FALSE;
}

static gboolean _event_star_enter(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  if(thumb->disable_actions) return TRUE;
  if(!thumb->mouse_over && !thumb->disable_mouseover) dt_control_set_mouse_over_id(thumb->imgid);
  _set_flag(thumb->w_bottom_eb, GTK_STATE_FLAG_PRELIGHT, TRUE);
  _set_flag(thumb->w_image_box, GTK_STATE_FLAG_PRELIGHT, TRUE);

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
  // if we leave for ancestor, that means we leave for blank thumbtable area
  if(event->type == GDK_LEAVE_NOTIFY && event->detail == GDK_NOTIFY_ANCESTOR) dt_control_set_mouse_over_id(-1);

  if(thumb->disable_actions) return TRUE;
  for(int i = 0; i < MAX_STARS; i++)
  {
    _set_flag(thumb->w_stars[i], GTK_STATE_FLAG_PRELIGHT, FALSE);
    gtk_widget_queue_draw(thumb->w_stars[i]);
  }
  return TRUE;
}

static gboolean _event_main_leave(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  // if we leave for ancestor, that means we leave for blank thumbtable area
  if(event->detail == GDK_NOTIFY_ANCESTOR) dt_control_set_mouse_over_id(-1);
  return FALSE;
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
    DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_ACTIVE_IMAGES_CHANGE,
                              G_CALLBACK(_dt_active_images_callback), thumb);
    DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_SELECTION_CHANGED,
                              G_CALLBACK(_dt_selection_changed_callback), thumb);
    DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_MIPMAP_UPDATED,
                              G_CALLBACK(_dt_mipmaps_updated_callback), thumb);
    DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                              G_CALLBACK(_dt_preview_updated_callback), thumb);
    DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_IMAGE_INFO_CHANGED,
                              G_CALLBACK(_dt_image_info_changed_callback), thumb);
    DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                              G_CALLBACK(_dt_collection_changed_callback), thumb);

    // the background
    thumb->w_back = gtk_event_box_new();
    gtk_widget_set_events(thumb->w_back, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_STRUCTURE_MASK
                                             | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK
                                             | GDK_POINTER_MOTION_HINT_MASK | GDK_POINTER_MOTION_MASK);
    gtk_widget_set_name(thumb->w_back, "thumb_back");
    g_signal_connect(G_OBJECT(thumb->w_back), "motion-notify-event", G_CALLBACK(_event_main_motion), thumb);
    g_signal_connect(G_OBJECT(thumb->w_back), "leave-notify-event", G_CALLBACK(_event_main_leave), thumb);
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
    thumb->w_image_box = gtk_overlay_new();
    gtk_widget_set_name(thumb->w_image_box, "thumb_image");
    gtk_widget_set_size_request(thumb->w_image_box, thumb->width, thumb->height);
    gtk_widget_set_valign(thumb->w_image_box, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_image_box, GTK_ALIGN_START);
    gtk_widget_show(thumb->w_image_box);
    thumb->w_image = gtk_drawing_area_new();
    gtk_widget_set_name(thumb->w_image, "thumb_image");
    gtk_widget_set_valign(thumb->w_image, GTK_ALIGN_FILL);
    gtk_widget_set_halign(thumb->w_image, GTK_ALIGN_FILL);
    gtk_widget_set_events(thumb->w_image, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_STRUCTURE_MASK
                                              | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK
                                              | GDK_POINTER_MOTION_HINT_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(G_OBJECT(thumb->w_image), "draw", G_CALLBACK(_event_image_draw), thumb);
    g_signal_connect(G_OBJECT(thumb->w_image), "motion-notify-event", G_CALLBACK(_event_main_motion), thumb);
    g_signal_connect(G_OBJECT(thumb->w_image), "enter-notify-event", G_CALLBACK(_event_image_enter_leave), thumb);
    g_signal_connect(G_OBJECT(thumb->w_image), "leave-notify-event", G_CALLBACK(_event_image_enter_leave), thumb);
    gtk_widget_show(thumb->w_image);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_image_box), thumb->w_image);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_image_box);
    _thumb_retrieve_margins(thumb);

    // triangle to indicate current image(s) in filmstrip
    thumb->w_cursor = gtk_drawing_area_new();
    gtk_widget_set_name(thumb->w_cursor, "thumb_cursor");
    gtk_widget_set_valign(thumb->w_cursor, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_cursor, GTK_ALIGN_CENTER);
    g_signal_connect(G_OBJECT(thumb->w_cursor), "draw", G_CALLBACK(_event_cursor_draw), thumb);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_cursor);

    // determine the overlays parents
    GtkWidget *overlays_parent = thumb->w_main;
    if(thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK) overlays_parent = thumb->w_image_box;

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
       || thumb->over == DT_THUMBNAIL_OVERLAYS_MIXED || thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK)
    {
      gchar *lb = dt_util_dstrcat(NULL, "%s", thumb->info_line);
      thumb->w_bottom = gtk_label_new(NULL);
      gtk_label_set_markup(GTK_LABEL(thumb->w_bottom), lb);
      g_free(lb);
    }
    else
    {
      thumb->w_bottom = gtk_label_new(NULL);
      gtk_label_set_markup(GTK_LABEL(thumb->w_bottom), "");
    }
    gtk_widget_set_name(thumb->w_bottom, "thumb_bottom_label");
    gtk_widget_show(thumb->w_bottom);
    gtk_label_set_yalign(GTK_LABEL(thumb->w_bottom), 0.05);
    gtk_label_set_ellipsize(GTK_LABEL(thumb->w_bottom), PANGO_ELLIPSIZE_MIDDLE);
    gtk_container_add(GTK_CONTAINER(thumb->w_bottom_eb), thumb->w_bottom);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlays_parent), thumb->w_bottom_eb);

    // the reject icon
    thumb->w_reject = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_reject, 0, NULL);
    gtk_widget_set_name(thumb->w_reject, "thumb_reject");
    gtk_widget_set_valign(thumb->w_reject, GTK_ALIGN_END);
    gtk_widget_set_halign(thumb->w_reject, GTK_ALIGN_START);
    gtk_widget_show(thumb->w_reject);
    g_signal_connect(G_OBJECT(thumb->w_reject), "button-press-event", G_CALLBACK(_event_rating_press), thumb);
    g_signal_connect(G_OBJECT(thumb->w_reject), "button-release-event", G_CALLBACK(_event_rating_release), thumb);
    g_signal_connect(G_OBJECT(thumb->w_reject), "enter-notify-event", G_CALLBACK(_event_btn_enter_leave), thumb);
    g_signal_connect(G_OBJECT(thumb->w_reject), "leave-notify-event", G_CALLBACK(_event_btn_enter_leave), thumb);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlays_parent), thumb->w_reject);

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
      gtk_overlay_add_overlay(GTK_OVERLAY(overlays_parent), thumb->w_stars[i]);
    }

    // the color labels
    thumb->w_color = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_label_flower, thumb->colorlabels, NULL);
    gtk_widget_set_name(thumb->w_color, "thumb_colorlabels");
    gtk_widget_set_valign(thumb->w_color, GTK_ALIGN_END);
    gtk_widget_set_halign(thumb->w_color, GTK_ALIGN_END);
    gtk_widget_set_no_show_all(thumb->w_color, TRUE);
    g_signal_connect(G_OBJECT(thumb->w_color), "enter-notify-event", G_CALLBACK(_event_btn_enter_leave), thumb);
    g_signal_connect(G_OBJECT(thumb->w_color), "leave-notify-event", G_CALLBACK(_event_btn_enter_leave), thumb);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlays_parent), thumb->w_color);

    // the local copy indicator
    thumb->w_local_copy = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_local_copy, CPF_DO_NOT_USE_BORDER, NULL);
    gtk_widget_set_name(thumb->w_local_copy, "thumb_localcopy");
    gtk_widget_set_valign(thumb->w_local_copy, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_local_copy, GTK_ALIGN_END);
    gtk_widget_set_no_show_all(thumb->w_local_copy, TRUE);
    g_signal_connect(G_OBJECT(thumb->w_local_copy), "enter-notify-event", G_CALLBACK(_event_btn_enter_leave),
                     thumb);
    g_signal_connect(G_OBJECT(thumb->w_local_copy), "leave-notify-event", G_CALLBACK(_event_btn_enter_leave),
                     thumb);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlays_parent), thumb->w_local_copy);

    // the altered icon
    thumb->w_altered = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_altered, CPF_DO_NOT_USE_BORDER, NULL);
    gtk_widget_set_name(thumb->w_altered, "thumb_altered");
    gtk_widget_set_valign(thumb->w_altered, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_altered, GTK_ALIGN_END);
    gtk_widget_set_no_show_all(thumb->w_altered, TRUE);
    g_signal_connect(G_OBJECT(thumb->w_altered), "enter-notify-event", G_CALLBACK(_event_btn_enter_leave), thumb);
    g_signal_connect(G_OBJECT(thumb->w_altered), "leave-notify-event", G_CALLBACK(_event_btn_enter_leave), thumb);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlays_parent), thumb->w_altered);

    // the group bouton
    thumb->w_group = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_grouping, CPF_DO_NOT_USE_BORDER, NULL);
    gtk_widget_set_name(thumb->w_group, "thumb_group");
    g_signal_connect(G_OBJECT(thumb->w_group), "button-release-event", G_CALLBACK(_event_grouping_release), thumb);
    g_signal_connect(G_OBJECT(thumb->w_group), "enter-notify-event", G_CALLBACK(_event_btn_enter_leave), thumb);
    g_signal_connect(G_OBJECT(thumb->w_group), "leave-notify-event", G_CALLBACK(_event_btn_enter_leave), thumb);
    gtk_widget_set_valign(thumb->w_group, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_group, GTK_ALIGN_END);
    gtk_widget_set_no_show_all(thumb->w_group, TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlays_parent), thumb->w_group);

    // the sound icon
    thumb->w_audio = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_audio, CPF_DO_NOT_USE_BORDER, NULL);
    gtk_widget_set_name(thumb->w_audio, "thumb_audio");
    g_signal_connect(G_OBJECT(thumb->w_audio), "button-release-event", G_CALLBACK(_event_audio_release), thumb);
    g_signal_connect(G_OBJECT(thumb->w_audio), "enter-notify-event", G_CALLBACK(_event_btn_enter_leave), thumb);
    g_signal_connect(G_OBJECT(thumb->w_audio), "leave-notify-event", G_CALLBACK(_event_btn_enter_leave), thumb);
    gtk_widget_set_valign(thumb->w_audio, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_audio, GTK_ALIGN_END);
    gtk_widget_set_no_show_all(thumb->w_audio, TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlays_parent), thumb->w_audio);

    // the zoom indicator
    thumb->w_zoom_eb = gtk_event_box_new();
    g_signal_connect(G_OBJECT(thumb->w_zoom_eb), "enter-notify-event", G_CALLBACK(_event_btn_enter_leave), thumb);
    gtk_widget_set_name(thumb->w_zoom_eb, "thumb_zoom");
    gtk_widget_set_valign(thumb->w_zoom_eb, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_zoom_eb, GTK_ALIGN_START);
    thumb->w_zoom = gtk_label_new("mini");
    gtk_widget_set_name(thumb->w_zoom, "thumb_zoom_label");
    gtk_widget_show(thumb->w_zoom);
    gtk_container_add(GTK_CONTAINER(thumb->w_zoom_eb), thumb->w_zoom);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlays_parent), thumb->w_zoom_eb);

    dt_thumbnail_resize(thumb, thumb->width, thumb->height, TRUE);
  }
  gtk_widget_show(thumb->w_main);
  g_object_ref(G_OBJECT(thumb->w_main));
  return thumb->w_main;
}

dt_thumbnail_t *dt_thumbnail_new(int width, int height, int imgid, int rowid, dt_thumbnail_overlay_t over,
                                 gboolean zoomable, gboolean tooltip)
{
  dt_thumbnail_t *thumb = calloc(1, sizeof(dt_thumbnail_t));
  thumb->width = width;
  thumb->height = height;
  thumb->imgid = imgid;
  thumb->rowid = rowid;
  thumb->over = over;
  thumb->zoomable = zoomable;
  thumb->zoom = 1.0f;
  thumb->overlay_timeout_duration = dt_conf_get_int("plugins/lighttable/overlay_timeout");
  thumb->tooltip = tooltip;
  thumb->expose_again_timeout_id = 0;

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
     || over == DT_THUMBNAIL_OVERLAYS_MIXED || thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK)
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
    char *tooltip_txt = dt_history_get_items_as_string(thumb->imgid);
    if(tooltip_txt)
    {
      gtk_widget_set_tooltip_text(thumb->w_altered, tooltip_txt);
      g_free(tooltip_txt);
    }
  }

  // grouping tooltip
  _image_update_group_tooltip(thumb);

  // get the file extension
  _thumb_write_extension(thumb);

  // ensure all icons are up to date
  _thumb_update_icons(thumb);

  return thumb;
}

void dt_thumbnail_destroy(dt_thumbnail_t *thumb)
{
  if(thumb->overlay_timeout_id > 0) g_source_remove(thumb->overlay_timeout_id);
  if(thumb->expose_again_timeout_id != 0) g_source_remove(thumb->expose_again_timeout_id);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_dt_selection_changed_callback), thumb);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_dt_active_images_callback), thumb);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_dt_mipmaps_updated_callback), thumb);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_dt_preview_updated_callback), thumb);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_dt_image_info_changed_callback), thumb);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_dt_collection_changed_callback), thumb);
  if(thumb->img_surf && cairo_surface_get_reference_count(thumb->img_surf) > 0)
    cairo_surface_destroy(thumb->img_surf);
  thumb->img_surf = NULL;
  if(thumb->w_main) gtk_widget_destroy(thumb->w_main);
  if(thumb->filename) g_free(thumb->filename);
  if(thumb->info_line) g_free(thumb->info_line);
  if(thumb->img_margin) gtk_border_free(thumb->img_margin);
  free(thumb);
}

void dt_thumbnail_update_infos(dt_thumbnail_t *thumb)
{
  if(!thumb) return;
  _image_get_infos(thumb);
  _thumb_write_extension(thumb);
  _thumb_update_icons(thumb);
  gtk_widget_queue_draw(thumb->w_main);
}

static void _thumb_resize_overlays(dt_thumbnail_t *thumb)
{
  PangoAttrList *attrlist;
  PangoAttribute *attr;
  int width = 0;
  int height = 0;

  int max_size = darktable.gui->icon_size;
  if(max_size < 2) max_size = round(1.2f * darktable.bauhaus->line_height); // fallback if toolbar icons are not realized

  if(thumb->over != DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK)
  {
    gtk_widget_get_size_request(thumb->w_main, &width, &height);
    // we need to squeeze 5 stars + 1 reject + 1 colorlabels symbols on a thumbnail width
    // stars + reject having a width of 2 * r1 and spaced by r1 => 18 * r1
    // colorlabels => 3 * r1 + space r1
    // inner margins are defined in css (margin_* values)

    // retrieves the size of the main icons in the top panel, thumbtable overlays shall not exceed that
    const float r1 = fminf(max_size / 2.0f, (width - thumb->img_margin->left - thumb->img_margin->right) / 22.0f);
    const float icon_size = 2.5 * r1;

    // file extension
    gtk_widget_set_margin_top(thumb->w_ext, thumb->img_margin->top);
    gtk_widget_set_margin_start(thumb->w_ext, thumb->img_margin->left);

    // bottom background
    gtk_widget_set_margin_start(thumb->w_bottom, thumb->img_margin->left);
    gtk_widget_set_margin_end(thumb->w_bottom, thumb->img_margin->right);
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
      gtk_widget_set_size_request(thumb->w_bottom_eb, width, icon_size * 0.75 + h + 3 * thumb->img_margin->bottom);
    }
    else
      gtk_widget_set_size_request(thumb->w_bottom_eb, width, icon_size * 0.75 + 2 * thumb->img_margin->bottom);

    gtk_label_set_xalign(GTK_LABEL(thumb->w_bottom), 0.5);
    gtk_label_set_yalign(GTK_LABEL(thumb->w_bottom), 0);
    gtk_widget_set_margin_top(thumb->w_bottom, thumb->img_margin->bottom);
    gtk_widget_set_valign(thumb->w_bottom_eb, GTK_ALIGN_END);
    gtk_widget_set_halign(thumb->w_bottom_eb, GTK_ALIGN_CENTER);

    // reject icon
    const int margin_b_icons = MAX(0, thumb->img_margin->bottom - icon_size * 0.125 - 1);
    gtk_widget_set_size_request(thumb->w_reject, icon_size, icon_size);
    gtk_widget_set_valign(thumb->w_reject, GTK_ALIGN_END);
    int pos = MAX(0, MAX(thumb->img_margin->left - icon_size * 0.125, (width - 15.0 * r1) * 0.5 - 4 * 3.0 * r1));
    gtk_widget_set_margin_start(thumb->w_reject, pos);
    gtk_widget_set_margin_bottom(thumb->w_reject, margin_b_icons);

    // stars
    for(int i = 0; i < MAX_STARS; i++)
    {
      gtk_widget_set_size_request(thumb->w_stars[i], icon_size, icon_size);
      gtk_widget_set_valign(thumb->w_stars[i], GTK_ALIGN_END);
      gtk_widget_set_margin_bottom(thumb->w_stars[i], margin_b_icons);
      gtk_widget_set_margin_start(
          thumb->w_stars[i], thumb->img_margin->left
                                 + (width - thumb->img_margin->left - thumb->img_margin->right - 13.0 * r1) * 0.5
                                 + i * 2.5 * r1);
    }

    // the color labels
    gtk_widget_set_size_request(thumb->w_color, icon_size, icon_size);
    gtk_widget_set_valign(thumb->w_color, GTK_ALIGN_END);
    gtk_widget_set_halign(thumb->w_color, GTK_ALIGN_START);
    gtk_widget_set_margin_bottom(thumb->w_color, margin_b_icons);
    pos = MIN(width - (thumb->img_margin->right - icon_size * 0.125 + icon_size),
              (width - 15.0 * r1) * 0.5 + 8.25 * 3.0 * r1);
    gtk_widget_set_margin_start(thumb->w_color, pos);

    // the local copy indicator
    gtk_widget_set_size_request(thumb->w_local_copy, 1.618 * r1, 1.618 * r1);
    gtk_widget_set_halign(thumb->w_local_copy, GTK_ALIGN_END);

    // the altered icon
    gtk_widget_set_size_request(thumb->w_altered, 2.0 * r1, 2.0 * r1);
    gtk_widget_set_halign(thumb->w_altered, GTK_ALIGN_END);
    gtk_widget_set_margin_top(thumb->w_altered, thumb->img_margin->top);
    gtk_widget_set_margin_end(thumb->w_altered, thumb->img_margin->right);

    // the group bouton
    gtk_widget_set_size_request(thumb->w_group, 2.0 * r1, 2.0 * r1);
    gtk_widget_set_halign(thumb->w_group, GTK_ALIGN_END);
    gtk_widget_set_margin_top(thumb->w_group, thumb->img_margin->top);
    gtk_widget_set_margin_end(thumb->w_group, thumb->img_margin->right + 2.5 * r1);

    // the sound icon
    gtk_widget_set_size_request(thumb->w_audio, 2.0 * r1, 2.0 * r1);
    gtk_widget_set_halign(thumb->w_audio, GTK_ALIGN_END);
    gtk_widget_set_margin_top(thumb->w_audio, thumb->img_margin->top);
    gtk_widget_set_margin_end(thumb->w_audio, thumb->img_margin->right + 5.0 * r1);

    // the filmstrip cursor
    gtk_widget_set_size_request(thumb->w_cursor, 6.0 * r1, 1.5 * r1);
  }
  else
  {
    gtk_widget_get_size_request(thumb->w_image_box, &width, &height);

    // we need to squeeze 5 stars + 1 reject + 1 colorlabels symbols on a thumbnail width
    // all icons having a width of 3.0 * r1 => 21 * r1
    // we want r1 spaces at extremities, after reject, before colorlables => 4 * r1
    const float r1 = fminf(max_size / 2.0f, width / 25.0f);

    // file extension
    gtk_widget_set_margin_top(thumb->w_ext, 0.03 * width);
    gtk_widget_set_margin_start(thumb->w_ext, 0.03 * width);

    // bottom background
    attrlist = pango_attr_list_new();
    attr = pango_attr_size_new_absolute(1.5 * r1 * PANGO_SCALE);
    pango_attr_list_insert(attrlist, attr);
    gtk_label_set_attributes(GTK_LABEL(thumb->w_bottom), attrlist);
    gtk_label_set_attributes(GTK_LABEL(thumb->w_zoom), attrlist);
    pango_attr_list_unref(attrlist);
    int w = 0;
    int h = 0;
    pango_layout_get_pixel_size(gtk_label_get_layout(GTK_LABEL(thumb->w_bottom)), &w, &h);
    // for the position, we use css margin and use it as per thousand (and not pixels)
    GtkBorder *margins = gtk_border_new();
    GtkBorder *borders = gtk_border_new();
    GtkStateFlags state = gtk_widget_get_state_flags(thumb->w_bottom_eb);
    GtkStyleContext *context = gtk_widget_get_style_context(thumb->w_bottom_eb);
    GtkStateFlags statei = gtk_widget_get_state_flags(thumb->w_image);
    GtkStyleContext *contexti = gtk_widget_get_style_context(thumb->w_image);
    gtk_style_context_get_margin(context, state, margins);
    gtk_style_context_get_border(contexti, statei, borders);
    const int padding = r1;
    const int padding_t = 0.8 * r1; // reduced to compensate label top margin applied by gtk
    const int margin_t = height * margins->top / 1000;
    const int margin_l = width * margins->left / 1000;
    const int border_t = borders->top;
    const int border_l = borders->left;
    const float icon_size = 3.0 * r1;
    const float icon_size2 = 2.0 * r1;
    const int line2 = padding_t + h + padding - icon_size / 8.0 + margin_t + border_t;
    const int line3 = line2 + icon_size - icon_size / 8.0 + padding - icon_size / 8.0;
    gtk_border_free(margins);
    gtk_border_free(borders);

    const int min_width = 2.0 * padding - icon_size / 4.0 + 2 * r1 + 7 * icon_size;
    gtk_widget_set_size_request(thumb->w_bottom_eb, CLAMP(w + padding_t * 2.0, min_width, width),
                                line3 - margin_t - border_t + icon_size2 + padding);

    gtk_label_set_xalign(GTK_LABEL(thumb->w_bottom), 0);
    gtk_label_set_yalign(GTK_LABEL(thumb->w_bottom), 0);
    gtk_widget_set_valign(thumb->w_bottom_eb, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_bottom_eb, GTK_ALIGN_START);

    gtk_widget_set_margin_top(thumb->w_bottom_eb, margin_t + border_t);
    gtk_widget_set_margin_start(thumb->w_bottom_eb, margin_l + border_l);
    gtk_widget_set_margin_top(thumb->w_bottom, padding_t);
    gtk_widget_set_margin_start(thumb->w_bottom, padding_t);
    gtk_widget_set_margin_end(thumb->w_bottom, padding_t);

    // reject icon
    gtk_widget_set_size_request(thumb->w_reject, icon_size, icon_size);
    gtk_widget_set_valign(thumb->w_reject, GTK_ALIGN_START);
    gtk_widget_set_margin_start(thumb->w_reject, padding - icon_size / 8.0 + border_l);
    gtk_widget_set_margin_top(thumb->w_reject, line2);
    // stars
    for(int i = 0; i < MAX_STARS; i++)
    {
      gtk_widget_set_size_request(thumb->w_stars[i], icon_size, icon_size);
      gtk_widget_set_valign(thumb->w_stars[i], GTK_ALIGN_START);
      gtk_widget_set_margin_top(thumb->w_stars[i], line2);
      gtk_widget_set_margin_start(thumb->w_stars[i],
                                  padding - icon_size / 8.0 + border_l + r1 + (i + 1) * 3.0 * r1);
    }
    // the color labels
    gtk_widget_set_size_request(thumb->w_color, icon_size, icon_size);
    gtk_widget_set_valign(thumb->w_color, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_color, GTK_ALIGN_START);
    gtk_widget_set_margin_top(thumb->w_color, line2);
    gtk_widget_set_margin_start(thumb->w_color,
                                padding - icon_size / 8.0 + border_l + 2.0 * r1 + (MAX_STARS + 1) * 3.0 * r1);
    // the local copy indicator
    gtk_widget_set_size_request(thumb->w_local_copy, icon_size2, icon_size2);
    gtk_widget_set_halign(thumb->w_local_copy, GTK_ALIGN_START);
    gtk_widget_set_margin_top(thumb->w_altered, line3);
    gtk_widget_set_margin_start(thumb->w_altered, 10.0 * r1);
    // the altered icon
    gtk_widget_set_size_request(thumb->w_altered, icon_size2, icon_size2);
    gtk_widget_set_halign(thumb->w_altered, GTK_ALIGN_START);
    gtk_widget_set_margin_top(thumb->w_altered, line3);
    gtk_widget_set_margin_start(thumb->w_altered, 7.0 * r1);
    // the group bouton
    gtk_widget_set_size_request(thumb->w_group, icon_size2, icon_size2);
    gtk_widget_set_halign(thumb->w_group, GTK_ALIGN_START);
    gtk_widget_set_margin_top(thumb->w_group, line3);
    gtk_widget_set_margin_start(thumb->w_group, 4.0 * r1);
    // the sound icon
    gtk_widget_set_size_request(thumb->w_audio, icon_size2, icon_size2);
    gtk_widget_set_halign(thumb->w_audio, GTK_ALIGN_START);
    gtk_widget_set_margin_top(thumb->w_audio, line3);
    gtk_widget_set_margin_start(thumb->w_audio, r1);
    // the zoomming indicator
    gtk_widget_set_margin_top(thumb->w_zoom_eb, line3);
    gtk_widget_set_margin_start(thumb->w_zoom_eb, 18.0 * r1);
  }
}

void dt_thumbnail_resize(dt_thumbnail_t *thumb, int width, int height, gboolean force)
{
  int w = 0;
  int h = 0;
  gtk_widget_get_size_request(thumb->w_main, &w, &h);

  // first, we verify that there's something to change
  if(!force && w == width && h == height) return;

  // widget resizing
  thumb->width = width;
  thumb->height = height;
  gtk_widget_set_size_request(thumb->w_main, width, height);

  // we change the size and margins according to the size change. This will be refined after
  if(h > 0 && w > 0)
  {
    int wi = 0;
    int hi = 0;
    gtk_widget_get_size_request(thumb->w_image_box, &wi, &hi);
    const int nimg_w = width * wi / w;
    const int nimg_h = height * hi / h;
    gtk_widget_set_size_request(thumb->w_image_box, nimg_w, nimg_h);
  }

  _thumb_retrieve_margins(thumb);

  // file extension
  gtk_widget_set_margin_start(thumb->w_ext, thumb->img_margin->left);
  gtk_widget_set_margin_top(thumb->w_ext, thumb->img_margin->top);

  // retrieves the size of the main icons in the top panel, thumbtable overlays shall not exceed that
  int max_size = darktable.gui->icon_size;
  if(max_size < 2) max_size = round(1.2f * darktable.bauhaus->line_height); // fallback if toolbar icons are not realized

  const int fsize = fminf(max_size, (height - thumb->img_margin->top - thumb->img_margin->bottom) / 11.0f);

  PangoAttrList *attrlist = pango_attr_list_new();
  PangoAttribute *attr = pango_attr_size_new_absolute(fsize * PANGO_SCALE);
  pango_attr_list_insert(attrlist, attr);
  // the idea is to reduce line-height, but it doesn't work for whatever reason...
  // PangoAttribute *attr2 = pango_attr_rise_new(-fsize * PANGO_SCALE);
  // pango_attr_list_insert(attrlist, attr2);
  gtk_label_set_attributes(GTK_LABEL(thumb->w_ext), attrlist);
  pango_attr_list_unref(attrlist);

  // and the overlays
  _thumb_resize_overlays(thumb);

  // reset surface
  dt_thumbnail_image_refresh(thumb);
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

  // we ensure that the image is not completly outside the thumbnail, otherwise the image_draw is not triggered
  if(gtk_widget_get_margin_start(thumb->w_image_box) >= thumb->width
     || gtk_widget_get_margin_top(thumb->w_image_box) >= thumb->height)
  {
    gtk_widget_set_margin_start(thumb->w_image_box, 0);
    gtk_widget_set_margin_top(thumb->w_image_box, 0);
  }
  gtk_widget_queue_draw(thumb->w_main);
}

static void _widget_change_parent_overlay(GtkWidget *w, GtkOverlay *new_parent)
{
  g_object_ref(w);
  gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(w)), w);
  gtk_overlay_add_overlay(new_parent, w);
  gtk_widget_show(w);
  g_object_unref(w);
}
void dt_thumbnail_set_overlay(dt_thumbnail_t *thumb, dt_thumbnail_overlay_t over, int timeout)
{
  thumb->overlay_timeout_duration = timeout;
  // if no change, do nothing...
  if(thumb->over == over) return;
  dt_thumbnail_overlay_t old_over = thumb->over;
  thumb->over = over;

  // first, if we change from/to hover/block, we need to change some parent widgets
  if(old_over == DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK || over == DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK)
  {
    GtkOverlay *overlays_parent = GTK_OVERLAY(thumb->w_main);
    if(thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK) overlays_parent = GTK_OVERLAY(thumb->w_image_box);

    _widget_change_parent_overlay(thumb->w_bottom_eb, overlays_parent);
    _widget_change_parent_overlay(thumb->w_reject, overlays_parent);
    for(int i = 0; i < MAX_STARS; i++)
    {
      _widget_change_parent_overlay(thumb->w_stars[i], overlays_parent);
    }
    _widget_change_parent_overlay(thumb->w_color, overlays_parent);
    _widget_change_parent_overlay(thumb->w_local_copy, overlays_parent);
    _widget_change_parent_overlay(thumb->w_altered, overlays_parent);
    _widget_change_parent_overlay(thumb->w_group, overlays_parent);
    _widget_change_parent_overlay(thumb->w_audio, overlays_parent);
    _widget_change_parent_overlay(thumb->w_zoom_eb, overlays_parent);
  }

  // we read and cache all the infos from dt_image_t that we need, depending on the overlay level
  // note that when "downgrading" overlay level, we don't bother to remove the infos
  dt_thumbnail_reload_infos(thumb);

  // and we resize the overlays
  _thumb_resize_overlays(thumb);
}

// force the image to be redraw at the right position
void dt_thumbnail_image_refresh_position(dt_thumbnail_t *thumb)
{
  // let's sanitize and apply panning values
  // here we have to make sure to properly align according to ppd
  int iw = 0;
  int ih = 0;
  gtk_widget_get_size_request(thumb->w_image_box, &iw, &ih);
  thumb->zoomx = CLAMP(thumb->zoomx, (iw * darktable.gui->ppd_thb - thumb->img_width) / darktable.gui->ppd_thb, 0);
  thumb->zoomy = CLAMP(thumb->zoomy, (ih * darktable.gui->ppd_thb - thumb->img_height) / darktable.gui->ppd_thb, 0);
  thumb->current_zx = thumb->zoomx;
  thumb->current_zy = thumb->zoomy;
  gtk_widget_queue_draw(thumb->w_main);
}

// get the max zoom value of the thumb
float dt_thumbnail_get_zoom100(dt_thumbnail_t *thumb)
{
  if(thumb->zoom_100 < 1.0f) // we only compute the sizes if needed
  {
    int w = 0;
    int h = 0;
    dt_image_get_final_size(thumb->imgid, &w, &h);
    if(!thumb->img_margin) _thumb_retrieve_margins(thumb);

    const float used_h = (float)(thumb->height - thumb->img_margin->top - thumb->img_margin->bottom);
    const float used_w = (float)(thumb->width - thumb->img_margin->left - thumb->img_margin->right);
    thumb->zoom_100 = fmaxf((float)w / used_w, (float)h / used_h);
    if(thumb->zoom_100 < 1.0f) thumb->zoom_100 = 1.0f;
  }

  return thumb->zoom_100;
}

// force the reload of image infos
void dt_thumbnail_reload_infos(dt_thumbnail_t *thumb)
{
  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, thumb->imgid, 'r');
  if(img)
  {
    if(thumb->over != DT_THUMBNAIL_OVERLAYS_NONE)
    {
      thumb->filename = g_strdup(img->filename);
      thumb->has_audio = (img->flags & DT_IMAGE_HAS_WAV);
      thumb->has_localcopy = (img->flags & DT_IMAGE_LOCAL_COPY);
    }

    dt_image_cache_read_release(darktable.image_cache, img);
  }
  if(thumb->over == DT_THUMBNAIL_OVERLAYS_ALWAYS_EXTENDED || thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_EXTENDED
     || thumb->over == DT_THUMBNAIL_OVERLAYS_MIXED || thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK)
    _thumb_update_extended_infos_line(thumb);

  // we read all other infos
  if(thumb->over != DT_THUMBNAIL_OVERLAYS_NONE)
  {
    _image_get_infos(thumb);
    _thumb_update_icons(thumb);
  }

  _thumb_write_extension(thumb);

  // extended overlay text
  gchar *lb = NULL;
  if(thumb->over == DT_THUMBNAIL_OVERLAYS_ALWAYS_EXTENDED || thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_EXTENDED
     || thumb->over == DT_THUMBNAIL_OVERLAYS_MIXED || thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK)
    lb = dt_util_dstrcat(NULL, "%s", thumb->info_line);

  // we set the text
  gtk_label_set_markup(GTK_LABEL(thumb->w_bottom), lb);
  g_free(lb);
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
