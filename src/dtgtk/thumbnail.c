/*
    This file is part of darktable,
    Copyright (C) 2019-2022 darktable developers.

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

#include "common/extra_optimizations.h"

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
#include "gui/accelerators.h"
#include "views/view.h"

static void _thumb_resize_overlays(dt_thumbnail_t *thumb);

static void _set_flag(GtkWidget *w, GtkStateFlags flag, gboolean activate)
{
  if(activate)
    gtk_widget_set_state_flags(w, flag, FALSE);
  else
    gtk_widget_unset_state_flags(w, flag);
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
    tt = g_strdup_printf("\n\u2022 <b>%s (%s)</b>", _("current"), _("leader"));
  else
  {
    const dt_image_t *img = dt_image_cache_get(darktable.image_cache, thumb->groupid, 'r');
    if(img)
    {
      tt = g_strdup_printf("%s\n\u2022 <b>%s (%s)</b>", _("\nclick here to set this image as group leader\n"), img->filename, _("leader"));
      dt_image_cache_read_release(darktable.image_cache, img);
    }
  }

  // and the other images
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT id, version, filename"
                              " FROM main.images"
                              " WHERE group_id = ?1", -1, &stmt,
                              NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, thumb->groupid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    nb++;
    const int id = sqlite3_column_int(stmt, 0);
    const int v = sqlite3_column_int(stmt, 1);

    if(id != thumb->groupid)
    {
      if(id == thumb->imgid)
        tt = dt_util_dstrcat(tt, "\n\u2022 %s", _("current"));
      else
      {
        tt = dt_util_dstrcat(tt, "\n\u2022 %s", sqlite3_column_text(stmt, 2));
        if(v > 0) tt = dt_util_dstrcat(tt, " v%d", v);
      }
    }
  }
  sqlite3_finalize(stmt);

  // and the number of grouped images
  gchar *ttf = g_strdup_printf("%d %s\n%s", nb, _("grouped images"), tt);
  g_free(tt);

  // let's apply the tooltip
  gtk_widget_set_tooltip_markup(thumb->w_group, ttf);
  g_free(ttf);
}

static void _thumb_update_rating_class(dt_thumbnail_t *thumb)
{
  if(!thumb->w_main) return;

  for(int i = DT_VIEW_DESERT; i <= DT_VIEW_REJECT; i++)
  {
    gchar *cn = g_strdup_printf("dt_thumbnail_rating_%d", i);
    if(thumb->rating == i)
      dt_gui_add_class(thumb->w_main, cn);
    else
      dt_gui_remove_class(thumb->w_main, cn);
    g_free(cn);
  }
}

static void _image_get_infos(dt_thumbnail_t *thumb)
{
  if(thumb->imgid <= 0) return;
  if(thumb->over == DT_THUMBNAIL_OVERLAYS_NONE) return;

  // we only get here infos that might change, others(exif, ...) are cached on widget creation

  const int old_rating = thumb->rating;
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
  // if the rating as changed, update the rejected
  if(old_rating != thumb->rating)
  {
    _thumb_update_rating_class(thumb);
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
      thumb->colorlabels |= CPF_LABEL_RED;
    else if(col == 1)
      thumb->colorlabels |= CPF_LABEL_YELLOW;
    else if(col == 2)
      thumb->colorlabels |= CPF_LABEL_GREEN;
    else if(col == 3)
      thumb->colorlabels |= CPF_LABEL_BLUE;
    else if(col == 4)
      thumb->colorlabels |= CPF_LABEL_PURPLE;
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

static void _thumb_set_image_size(dt_thumbnail_t *thumb, int image_w, int image_h)
{
  int imgbox_w = 0;
  int imgbox_h = 0;
  gtk_widget_get_size_request(thumb->w_image_box, &imgbox_w, &imgbox_h);

  gtk_widget_set_size_request(thumb->w_image, MIN(image_w, imgbox_w), MIN(image_h, imgbox_h));
}

static void _thumb_draw_image(dt_thumbnail_t *thumb, cairo_t *cr)
{
  if(!thumb->w_image) return;

  // we draw the image
  GtkStyleContext *context = gtk_widget_get_style_context(thumb->w_image);
  int w = 0;
  int h = 0;
  gtk_widget_get_size_request(thumb->w_image, &w, &h);

  // Safety check to avoid possible error
  if(thumb->img_surf && cairo_surface_get_reference_count(thumb->img_surf) >= 1)
  {
    cairo_save(cr);
    const float scaler = 1.0f / darktable.gui->ppd_thb;
    cairo_scale(cr, scaler, scaler);

    cairo_set_source_surface(cr, thumb->img_surf, thumb->zoomx * darktable.gui->ppd,
                             thumb->zoomy * darktable.gui->ppd);

    // get the transparency value
    GdkRGBA im_color;
    gtk_style_context_get_color(context, gtk_widget_get_state_flags(thumb->w_image), &im_color);
    cairo_paint_with_alpha(cr, im_color.alpha);

    // and eventually the image border
    gtk_render_frame(context, cr, 0, 0, w * darktable.gui->ppd_thb, h * darktable.gui->ppd_thb);
    cairo_restore(cr);
  }

  // if needed we draw the working msg too
  if(thumb->busy)
  {
    dt_control_draw_busy_msg(cr, w, h);
  }
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
  while(ext > thumb->filename && *ext != '.') ext--;
  ext++;
  gchar *uext = dt_view_extend_modes_str(ext, thumb->is_hdr, thumb->is_bw, thumb->is_bw_flow);
  gtk_label_set_text(GTK_LABEL(thumb->w_ext), uext);
  g_free(uext);
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

// zoom_ratio is 0-1 based, where 0 is "img to fit" and 1 "zoom to 100%". returns a thumb->zoom value
static float _zoom_ratio_to_thumb_zoom(float zoom_ratio, float zoom_100)
{
  return (zoom_100 - 1) * zoom_ratio + 1;
}

// converts a thumb->zoom value based on it's zoom_100 (max value) to a 0-1 based zoom_ratio.
static float _thumb_zoom_to_zoom_ratio(float zoom, float zoom_100)
{
  return (zoom - 1) / (zoom_100 - 1);
}

// given max_width & max_height, the width and height is calculated to fit an image in a "img to fit" mode
// (everything is visible)
static void _get_dimensions_for_img_to_fit(dt_thumbnail_t *thumb, int max_width, int max_height, float *width,
                                           float *height)
{
  float iw = max_width;
  float ih = max_height;

  // we can't rely on img->aspect_ratio as the value is round to 1 decimal, so not enough accurate
  // so we compute it from the larger available mipmap
  float ar = 0.0f;
  for(int k = DT_MIPMAP_7; k >= DT_MIPMAP_0; k--)
  {
    dt_mipmap_buffer_t tmp;
    dt_mipmap_cache_get(darktable.mipmap_cache, &tmp, thumb->imgid, k, DT_MIPMAP_TESTLOCK, 'r');
    if(tmp.buf)
    {
      const int mipw = tmp.width;
      const int miph = tmp.height;
      dt_mipmap_cache_release(darktable.mipmap_cache, &tmp);
      if(mipw > 0 && miph > 0)
      {
        ar = (float)mipw / miph;
        break;
      }
    }
  }

  if(ar < 0.001)
  {
    // let's try with the aspect_ratio store in image structure, even if it's less accurate
    const dt_image_t *img = dt_image_cache_get(darktable.image_cache, thumb->imgid, 'r');
    if(img)
    {
      ar = img->aspect_ratio;
      dt_image_cache_read_release(darktable.image_cache, img);
    }
  }

  if(ar > 0.001)
  {
    // we have a valid ratio, let's apply it
    if(ar < 1.0)
      iw = ih * ar;
    else
      ih = iw / ar;
    // rescale to ensure it stay in thumbnails bounds
    const float scale = fminf(1.0, fminf((float)max_width / iw, (float)max_height / ih));
    iw *= scale;
    ih *= scale;
  }

  *width = iw;
  *height = ih;
}

// retrieves image zoom100 and final_width/final_height to calculate the dimensions of the zoomed image.
static void _get_dimensions_for_zoomed_img(dt_thumbnail_t *thumb, int max_width, int max_height, float zoom_ratio,
                                           float *width, float *height)
{
  float iw = max_width;
  float ih = max_height;
  // we need to get proper dimensions for the image to determine the image_w size.
  // calling dt_thumbnail_get_zoom100 is used to get the max zoom, but also to ensure that final_width and
  // height are available.
  const float zoom_100 = dt_thumbnail_get_zoom100(thumb);
  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, thumb->imgid, 'r');
  if(img)
  {
    if(img->final_width > 0 && img->final_height > 0)
    {
      iw = img->final_width;
      ih = img->final_height;
    }
    dt_image_cache_read_release(darktable.image_cache, img);
  }

  // scale first to "img to fit", then apply the zoom ratio to get the resulting final (zoomed) image
  // dimensions, while making sure to still fit in the imagebox.
  const float scale_to_fit = fminf((float)max_width / iw, (float)max_height / ih);
  thumb->zoom = _zoom_ratio_to_thumb_zoom(zoom_ratio, zoom_100);
  *width = MIN(iw * scale_to_fit * thumb->zoom, max_width);
  *height = MIN(ih * scale_to_fit * thumb->zoom, max_height);
}

static void _thumb_set_image_area(dt_thumbnail_t *thumb, float zoom_ratio)
{
  // let's ensure we have the right margins
  _thumb_retrieve_margins(thumb);

  int image_w, image_h;
  int posy = 0;
  if(thumb->over == DT_THUMBNAIL_OVERLAYS_ALWAYS_NORMAL
     || thumb->over == DT_THUMBNAIL_OVERLAYS_ALWAYS_EXTENDED)
  {
    image_w = thumb->width - thumb->img_margin->left - thumb->img_margin->right;
    int w = 0;
    int h = 0;
    gtk_widget_get_size_request(thumb->w_bottom_eb, &w, &h);
    image_h = thumb->height - MAX(0, h);
    gtk_widget_get_size_request(thumb->w_altered, &w, &h);
    if(!thumb->zoomable)
    {
      posy = h + gtk_widget_get_margin_top(thumb->w_altered);
      image_h -= posy;
    }
    else
      image_h -= thumb->img_margin->bottom;
    image_h -= thumb->img_margin->top;
    posy += thumb->img_margin->top;
  }
  else if(thumb->over == DT_THUMBNAIL_OVERLAYS_MIXED)
  {
    image_w = thumb->width - thumb->img_margin->left - thumb->img_margin->right;
    int w = 0;
    int h = 0;
    gtk_widget_get_size_request(thumb->w_reject, &w, &h);
    image_h = thumb->height - (h + gtk_widget_get_margin_bottom(thumb->w_reject));
    gtk_widget_get_size_request(thumb->w_altered, &w, &h);
    posy = h + gtk_widget_get_margin_top(thumb->w_altered);
    image_h -= posy;
    image_h -= thumb->img_margin->top + thumb->img_margin->bottom;
    posy += thumb->img_margin->top;
  }
  else
  {
    image_w = thumb->width - thumb->img_margin->left - thumb->img_margin->right;
    image_h = thumb->height - thumb->img_margin->top - thumb->img_margin->bottom;
    posy = thumb->img_margin->top;
  }

  // we check that the image drawing area is not greater than the box
  int wi = 0;
  int hi = 0;
  gtk_widget_get_size_request(thumb->w_image, &wi, &hi);
  if(wi <= 0 || hi <= 0)
  {
    // we arrive here if we are inside the creation process
    float iw = image_w;
    float ih = image_h;

    if(zoom_ratio == IMG_TO_FIT)
      _get_dimensions_for_img_to_fit(thumb, image_w, image_h, &iw, &ih);
    else
      _get_dimensions_for_zoomed_img(thumb, image_w, image_h, zoom_ratio, &iw, &ih);

    gtk_widget_set_size_request(thumb->w_image, iw, ih);
  }
  else
  {
    const float scale = fminf((float)image_w / wi, (float)image_h / hi);
    if(scale < 1.0f) gtk_widget_set_size_request(thumb->w_image, wi * scale, hi * scale);
  }

  // and we set the size and margins of the imagebox
  gtk_widget_set_size_request(thumb->w_image_box, image_w, image_h);
  gtk_widget_set_margin_start(thumb->w_image_box, thumb->img_margin->left);
  gtk_widget_set_margin_top(thumb->w_image_box, posy);
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
     && (v->view(v) != DT_VIEW_DARKROOM
         || !dev->preview_pipe->output_backbuf
         || dev->preview_pipe->output_imgid != thumb->imgid))
  {
    if(thumb->img_surf && cairo_surface_get_reference_count(thumb->img_surf) > 0)
      cairo_surface_destroy(thumb->img_surf);
    thumb->img_surf = NULL;
    thumb->img_surf_dirty = TRUE;
    thumb->img_surf_preview = FALSE;
  }

  // if image surface has no more ref. let's samitize it's value to NULL
  if(thumb->img_surf
     && cairo_surface_get_reference_count(thumb->img_surf) < 1)
    thumb->img_surf = NULL;

  // if we don't have it in memory, we want the image surface
  dt_view_surface_value_t res = DT_VIEW_SURFACE_OK;
  if(!thumb->img_surf || thumb->img_surf_dirty)
  {
    int image_w = 0;
    int image_h = 0;
    _thumb_set_image_area(thumb, IMG_TO_FIT);
    gtk_widget_get_size_request(thumb->w_image_box, &image_w, &image_h);

    if(v->view(v) == DT_VIEW_DARKROOM
       && dev->preview_pipe->output_imgid == thumb->imgid
       && dev->preview_pipe->output_backbuf)
    {
      // the current thumb is the one currently developed in darkroom
      // better use the preview buffer for surface, in order to stay in sync
      if(thumb->img_surf && cairo_surface_get_reference_count(thumb->img_surf) > 0)
        cairo_surface_destroy(thumb->img_surf);
      thumb->img_surf = NULL;

      // get new surface with preview image
      const int buf_width = dev->preview_pipe->output_backbuf_width;
      const int buf_height = dev->preview_pipe->output_backbuf_height;
      uint8_t *rgbbuf = g_malloc0(sizeof(unsigned char) * 4 * buf_width * buf_height);

      dt_pthread_mutex_t *mutex = &dev->preview_pipe->backbuf_mutex;
      dt_pthread_mutex_lock(mutex);
      memcpy(rgbbuf, dev->preview_pipe->output_backbuf, sizeof(unsigned char) * 4 * buf_width * buf_height);
      dt_pthread_mutex_unlock(mutex);

      const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, buf_width);
      cairo_surface_t *tmp_surface
          = cairo_image_surface_create_for_data(rgbbuf, CAIRO_FORMAT_RGB24, buf_width, buf_height, stride);

      // copy preview image into final surface
      if(tmp_surface)
      {
        float scale = fminf(image_w / (float)buf_width, image_h / (float)buf_height) * darktable.gui->ppd_thb;
        const int img_width = roundf(buf_width * scale);
        const int img_height = roundf(buf_height * scale);
        scale = fmaxf(img_width / (float)buf_width, img_height / (float)buf_height);
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
          dt_focuspeaking(cr2, img_width, img_height, cairo_image_surface_get_data(thumb->img_surf));
          cairo_restore(cr2);
        }

        cairo_surface_destroy(tmp_surface);
        cairo_destroy(cr2);
      }
      if(rgbbuf) g_free(rgbbuf);

      thumb->img_surf_preview = TRUE;
    }
    else
    {
      cairo_surface_t *img_surf = NULL;
      if(thumb->zoomable)
      {
        if(thumb->zoom > 1.0f)
          thumb->zoom = MIN(thumb->zoom, dt_thumbnail_get_zoom100(thumb));
        res = dt_view_image_get_surface(thumb->imgid, image_w * thumb->zoom, image_h * thumb->zoom, &img_surf, FALSE);
      }
      else
      {
        res = dt_view_image_get_surface(thumb->imgid, image_w, image_h, &img_surf, FALSE);
      }

      if(res == DT_VIEW_SURFACE_OK || res == DT_VIEW_SURFACE_SMALLER)
      {
        // if we succeed to get an image (even a smaller one)
        cairo_surface_t *tmp_surf = thumb->img_surf;
        thumb->img_surf = img_surf;
        if(tmp_surf && cairo_surface_get_reference_count(tmp_surf) > 0)
          cairo_surface_destroy(tmp_surf);
      }
      thumb->img_surf_preview = FALSE;
    }

    if(thumb->img_surf)
    {
      thumb->img_width = cairo_image_surface_get_width(thumb->img_surf);
      thumb->img_height = cairo_image_surface_get_height(thumb->img_surf);
      // and we want to resize the imagebox to fit in the imagearea
      const int imgbox_w = MIN(image_w, thumb->img_width / darktable.gui->ppd_thb);
      const int imgbox_h = MIN(image_h, thumb->img_height / darktable.gui->ppd_thb);
      // we record the imagebox size before the change
      int hh = 0;
      int ww = 0;
      gtk_widget_get_size_request(thumb->w_image, &ww, &hh);
      // and we set the new size of the imagebox
      _thumb_set_image_size(thumb, imgbox_w, imgbox_h);
      // the imagebox size may have been slightly sanitized, so we get it again
      int nhi = 0;
      int nwi = 0;
      gtk_widget_get_size_request(thumb->w_image, &nwi, &nhi);

      // panning value need to be adjusted if the imagebox size as changed
      thumb->zoomx = thumb->zoomx + (nwi - ww) / 2.0;
      thumb->zoomy = thumb->zoomy + (nhi - hh) / 2.0;
      // let's sanitize and apply panning values as we are sure the zoomed image is loaded now
      // here we have to make sure to properly align according to ppd
      thumb->zoomx
          = CLAMP(thumb->zoomx, (nwi * darktable.gui->ppd_thb - thumb->img_width) / darktable.gui->ppd_thb, 0);
      thumb->zoomy
          = CLAMP(thumb->zoomy, (nhi * darktable.gui->ppd_thb - thumb->img_height) / darktable.gui->ppd_thb, 0);

      // for overlay block, we need to resize it
      if(thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK)
        _thumb_resize_overlays(thumb);
    }

    // if we don't have the right size of the image now, we reload it again
    if(res != DT_VIEW_SURFACE_OK)
    {
      thumb->busy = TRUE;
      if(!thumb->expose_again_timeout_id)
        thumb->expose_again_timeout_id = g_timeout_add(250, _thumb_expose_again, thumb);
    }

    // if needed we compute and draw here the big rectangle to show focused areas
    if(res == DT_VIEW_SURFACE_OK && thumb->display_focus)
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


    // here we are sure to have the right imagesurface
    if(res == DT_VIEW_SURFACE_OK)
    {
      thumb->img_surf_dirty = FALSE;
      thumb->busy = FALSE;
    }

    // and we can also set the zooming level if needed
    if(res == DT_VIEW_SURFACE_OK
       && thumb->zoomable
       && thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK)
    {
      if(thumb->zoom_100 < 1.0 || thumb->zoom <= 1.0f)
      {
        gtk_label_set_text(GTK_LABEL(thumb->w_zoom), _("fit"));
      }
      else
      {
        gchar *z = g_strdup_printf("%.0f%%", thumb->zoom * 100.0 / thumb->zoom_100);
        gtk_label_set_text(GTK_LABEL(thumb->w_zoom), z);
        g_free(z);
      }
    }
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

static void _thumbs_show_overlays(dt_thumbnail_t *thumb)
{
  // first, we hide the block overlays after a delay if the mouse hasn't move
  if(thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK)
  {
    if(thumb->overlay_timeout_id > 0)
    {
      g_source_remove(thumb->overlay_timeout_id);
      thumb->overlay_timeout_id = 0;
    }
    _thumb_update_icons(thumb);
    if(thumb->overlay_timeout_duration >= 0)
    {
      thumb->overlay_timeout_id
          = g_timeout_add_seconds(thumb->overlay_timeout_duration, _thumbs_hide_overlays, thumb);
    }
  }
  else
    _thumb_update_icons(thumb);
}

static gboolean _event_main_motion(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  if(!user_data) return TRUE;
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  // first, we hide the block overlays after a delay if the mouse hasn't move
  _thumbs_show_overlays(thumb);

  if(!thumb->mouse_over && !thumb->disable_mouseover)
    dt_control_set_mouse_over_id(thumb->imgid);
  return FALSE;
}

static gboolean _event_main_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  if(event->button == 1
     && ((event->type == GDK_2BUTTON_PRESS && !thumb->single_click)
         || (event->type == GDK_BUTTON_PRESS
             && dt_modifier_is(event->state, 0) && thumb->single_click)))
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
    if(dt_modifier_is(event->state, 0) && thumb->sel_mode != DT_THUMBNAIL_SEL_MODE_MOD_ONLY)
      dt_selection_select_single(darktable.selection, thumb->imgid);
    else if(dt_modifier_is(event->state, GDK_MOD1_MASK))
      dt_selection_select_single(darktable.selection, thumb->imgid);
    else if(dt_modifier_is(event->state, GDK_CONTROL_MASK))
      dt_selection_toggle(darktable.selection, thumb->imgid);
    else if(dt_modifier_is(event->state, GDK_SHIFT_MASK))
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
      dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_RATING_RANGE,
                                 g_list_prepend(NULL, GINT_TO_POINTER(thumb->imgid)));
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
    //TODO: will succeed if either or *both* of Shift and Control are pressed.  Do we want this?
    if(dt_modifier_is(event->state, GDK_SHIFT_MASK) | dt_modifier_is(event->state, GDK_CONTROL_MASK))
    {
      // just add the whole group to the selection. TODO: make this also work for collapsed groups.
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
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_GROUPING,
                               NULL);
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
  for(const GList *i = imgs; i; i = g_list_next(i))
  {
    if(GPOINTER_TO_INT(i->data) == thumb->imgid)
    {
      dt_thumbnail_update_infos(thumb);
      break;
    }
  }
}

// this is called each time collected images change
// we only use this because the image infos may have changed
static void _dt_collection_changed_callback(gpointer instance, dt_collection_change_t query_change,
                                            dt_collection_properties_t changed_property, gpointer imgs,
                                            const int next, gpointer user_data)
{
  if(!user_data || !imgs) return;
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  for(const GList *i = imgs; i; i = g_list_next(i))
  {
    if(GPOINTER_TO_INT(i->data) == thumb->imgid)
    {
      dt_thumbnail_update_infos(thumb);
      break;
    }
  }
}

void dt_thumbnail_update_selection(dt_thumbnail_t *thumb)
{
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

static void _dt_selection_changed_callback(gpointer instance, gpointer user_data)
{
  if(!user_data) return;
  dt_thumbnail_update_selection((dt_thumbnail_t *)user_data);
}

static void _dt_active_images_callback(gpointer instance, gpointer user_data)
{
  if(!user_data) return;
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;

  gboolean active = FALSE;
  for(GSList *l = darktable.view_manager->active_images; l; l = g_slist_next(l))
  {
    int id = GPOINTER_TO_INT(l->data);
    if(id == thumb->imgid)
    {
      active = TRUE;
      break;
    }
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
  if(!gtk_widget_is_visible(thumb->w_main)) return;

  const dt_view_t *v = dt_view_manager_get_current_view(darktable.view_manager);
  if(v->view(v) == DT_VIEW_DARKROOM
     && (thumb->img_surf_preview
         || darktable.develop->preview_pipe->output_imgid == thumb->imgid)
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
  if(imgid > 0 && thumb->imgid != imgid) return;

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
  if(event->type == GDK_LEAVE_NOTIFY
     && event->detail == GDK_NOTIFY_ANCESTOR)
    dt_control_set_mouse_over_id(-1);

  if(!thumb->mouse_over
     && event->type == GDK_ENTER_NOTIFY
     && !thumb->disable_mouseover)
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

  darktable.control->element = event->type == GDK_ENTER_NOTIFY && widget == thumb->w_reject ? DT_VIEW_REJECT : -1;

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
    if(thumb->w_stars[i] == widget)
    {
      darktable.control->element = i + 1;
      pre = FALSE;
    }
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

static void _event_image_style_updated(GtkWidget *w, dt_thumbnail_t *thumb)
{
  // for some reason the style has changed. We have to recompute margins and resize the overlays

  // we retrieve the eventual new margins
  const int oldt = thumb->img_margin->top;
  const int oldr = thumb->img_margin->right;
  const int oldb = thumb->img_margin->bottom;
  const int oldl = thumb->img_margin->left;
  _thumb_retrieve_margins(thumb);

  if(oldt != thumb->img_margin->top
     || oldr != thumb->img_margin->right
     || oldb != thumb->img_margin->bottom
     || oldl != thumb->img_margin->left)
  {
    _thumb_resize_overlays(thumb);
  }
}

GtkWidget *dt_thumbnail_create_widget(dt_thumbnail_t *thumb, float zoom_ratio)
{
  // main widget (overlay)
  thumb->w_main = gtk_overlay_new();
  gtk_widget_set_name(thumb->w_main, "thumb-main");
  _thumb_update_rating_class(thumb);
  gtk_widget_set_size_request(thumb->w_main, thumb->width, thumb->height);

  if(thumb->imgid > 0)
  {
    // this is only here to ensure that mouse-over value is updated correctly
    // all dragging actions take place inside thumbatble.c
    gtk_drag_dest_set(thumb->w_main, GTK_DEST_DEFAULT_MOTION, target_list_all, n_targets_all, GDK_ACTION_MOVE);
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
                                             | GDK_POINTER_MOTION_MASK);
    gtk_widget_set_name(thumb->w_back, "thumb-back");
    g_signal_connect(G_OBJECT(thumb->w_back), "motion-notify-event", G_CALLBACK(_event_main_motion), thumb);
    g_signal_connect(G_OBJECT(thumb->w_back), "leave-notify-event", G_CALLBACK(_event_main_leave), thumb);
    gtk_widget_show(thumb->w_back);
    gtk_container_add(GTK_CONTAINER(thumb->w_main), thumb->w_back);

    // the file extension label
    thumb->w_ext = gtk_label_new("");
    gtk_widget_set_name(thumb->w_ext, "thumb-ext");
    gtk_widget_set_valign(thumb->w_ext, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_ext, GTK_ALIGN_START);
    gtk_label_set_justify(GTK_LABEL(thumb->w_ext), GTK_JUSTIFY_CENTER);
    gtk_widget_show(thumb->w_ext);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_ext);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(thumb->w_main), thumb->w_ext, TRUE);

    // the image drawing area
    thumb->w_image_box = gtk_overlay_new();
    gtk_widget_set_name(thumb->w_image_box, "thumb-image");
    gtk_widget_set_size_request(thumb->w_image_box, thumb->width, thumb->height);
    gtk_widget_set_valign(thumb->w_image_box, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_image_box, GTK_ALIGN_START);
    gtk_widget_show(thumb->w_image_box);
    // we add a eventbox which cover all the w_image_box otherwise event don't work in areas not covered by w_image
    // itself
    GtkWidget *evt_image = gtk_event_box_new();
    gtk_widget_set_valign(evt_image, GTK_ALIGN_FILL);
    gtk_widget_set_halign(evt_image, GTK_ALIGN_FILL);
    gtk_widget_set_events(evt_image, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_STRUCTURE_MASK
                                         | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK
                                         | GDK_POINTER_MOTION_MASK);
    g_signal_connect(G_OBJECT(evt_image), "motion-notify-event", G_CALLBACK(_event_main_motion), thumb);
    g_signal_connect(G_OBJECT(evt_image), "enter-notify-event", G_CALLBACK(_event_image_enter_leave), thumb);
    g_signal_connect(G_OBJECT(evt_image), "leave-notify-event", G_CALLBACK(_event_image_enter_leave), thumb);
    gtk_widget_show(evt_image);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_image_box), evt_image);
    thumb->w_image = gtk_drawing_area_new();
    gtk_widget_set_name(thumb->w_image, "thumb-image");
    gtk_widget_set_valign(thumb->w_image, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(thumb->w_image, GTK_ALIGN_CENTER);
    // the size will be defined at the end, inside dt_thumbnail_resize
    gtk_widget_set_events(thumb->w_image, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_STRUCTURE_MASK
                                              | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK
                                              | GDK_POINTER_MOTION_MASK);
    g_signal_connect(G_OBJECT(thumb->w_image), "draw", G_CALLBACK(_event_image_draw), thumb);
    g_signal_connect(G_OBJECT(thumb->w_image), "motion-notify-event", G_CALLBACK(_event_main_motion), thumb);
    g_signal_connect(G_OBJECT(thumb->w_image), "enter-notify-event", G_CALLBACK(_event_image_enter_leave), thumb);
    g_signal_connect(G_OBJECT(thumb->w_image), "leave-notify-event", G_CALLBACK(_event_image_enter_leave), thumb);
    g_signal_connect(G_OBJECT(thumb->w_image), "style-updated", G_CALLBACK(_event_image_style_updated), thumb);
    gtk_widget_show(thumb->w_image);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_image_box), thumb->w_image);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_image_box);

    // triangle to indicate current image(s) in filmstrip
    thumb->w_cursor = gtk_drawing_area_new();
    gtk_widget_set_name(thumb->w_cursor, "thumb-cursor");
    gtk_widget_set_valign(thumb->w_cursor, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_cursor, GTK_ALIGN_CENTER);
    g_signal_connect(G_OBJECT(thumb->w_cursor), "draw", G_CALLBACK(_event_cursor_draw), thumb);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_cursor);

    // determine the overlays parents
    GtkWidget *overlays_parent = thumb->w_main;
    if(thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK)
      overlays_parent = thumb->w_image_box;

    // the infos background
    thumb->w_bottom_eb = gtk_event_box_new();
    gtk_widget_set_name(thumb->w_bottom_eb, "thumb-bottom");
    g_signal_connect(G_OBJECT(thumb->w_bottom_eb), "enter-notify-event", G_CALLBACK(_event_box_enter_leave),
                     thumb);
    g_signal_connect(G_OBJECT(thumb->w_bottom_eb), "leave-notify-event", G_CALLBACK(_event_box_enter_leave),
                     thumb);
    gtk_widget_set_valign(thumb->w_bottom_eb, GTK_ALIGN_END);
    gtk_widget_set_halign(thumb->w_bottom_eb, GTK_ALIGN_CENTER);
    gtk_widget_show(thumb->w_bottom_eb);
    if(thumb->over == DT_THUMBNAIL_OVERLAYS_ALWAYS_EXTENDED
       || thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_EXTENDED
       || thumb->over == DT_THUMBNAIL_OVERLAYS_MIXED
       || thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK)
    {
      gchar *lb = g_strdup(thumb->info_line);
      thumb->w_bottom = gtk_label_new(NULL);
      gtk_label_set_markup(GTK_LABEL(thumb->w_bottom), lb);
      g_free(lb);
    }
    else
    {
      thumb->w_bottom = gtk_label_new(NULL);
      gtk_label_set_markup(GTK_LABEL(thumb->w_bottom), "");
    }
    gtk_widget_set_name(thumb->w_bottom, "thumb-bottom-label");
    gtk_widget_show(thumb->w_bottom);
    gtk_label_set_yalign(GTK_LABEL(thumb->w_bottom), 0.05);
    gtk_label_set_ellipsize(GTK_LABEL(thumb->w_bottom), PANGO_ELLIPSIZE_MIDDLE);
    gtk_container_add(GTK_CONTAINER(thumb->w_bottom_eb), thumb->w_bottom);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlays_parent), thumb->w_bottom_eb);

    // the reject icon
    thumb->w_reject = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_reject, 0, NULL);
    gtk_widget_set_name(thumb->w_reject, "thumb-reject");
    dt_action_define(&darktable.control->actions_thumb, NULL, "rating", thumb->w_reject, &dt_action_def_rating);
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
      gtk_widget_set_name(thumb->w_stars[i], "thumb-star");
      dt_action_define(&darktable.control->actions_thumb, NULL, "rating", thumb->w_stars[i], &dt_action_def_rating);
      gtk_widget_set_valign(thumb->w_stars[i], GTK_ALIGN_END);
      gtk_widget_set_halign(thumb->w_stars[i], GTK_ALIGN_START);
      gtk_widget_show(thumb->w_stars[i]);
      gtk_overlay_add_overlay(GTK_OVERLAY(overlays_parent), thumb->w_stars[i]);
    }

    // the color labels
    thumb->w_color = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_label_flower, thumb->colorlabels, NULL);
    dt_action_define(&darktable.control->actions_thumb, NULL, N_("color label"), thumb->w_color, &dt_action_def_color_label);
    gtk_widget_set_name(thumb->w_color, "thumb-colorlabels");
    gtk_widget_set_valign(thumb->w_color, GTK_ALIGN_END);
    gtk_widget_set_halign(thumb->w_color, GTK_ALIGN_END);
    gtk_widget_set_no_show_all(thumb->w_color, TRUE);
    g_signal_connect(G_OBJECT(thumb->w_color), "enter-notify-event", G_CALLBACK(_event_btn_enter_leave), thumb);
    g_signal_connect(G_OBJECT(thumb->w_color), "leave-notify-event", G_CALLBACK(_event_btn_enter_leave), thumb);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlays_parent), thumb->w_color);

    // the local copy indicator
    thumb->w_local_copy = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_local_copy, 0, NULL);
    gtk_widget_set_name(thumb->w_local_copy, "thumb-localcopy");
    gtk_widget_set_tooltip_text(thumb->w_local_copy, _("local copy"));
    gtk_widget_set_valign(thumb->w_local_copy, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_local_copy, GTK_ALIGN_END);
    gtk_widget_set_no_show_all(thumb->w_local_copy, TRUE);
    g_signal_connect(G_OBJECT(thumb->w_local_copy), "enter-notify-event", G_CALLBACK(_event_btn_enter_leave),
                     thumb);
    g_signal_connect(G_OBJECT(thumb->w_local_copy), "leave-notify-event", G_CALLBACK(_event_btn_enter_leave),
                     thumb);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlays_parent), thumb->w_local_copy);

    // the altered icon
    thumb->w_altered = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_altered, 0, NULL);
    gtk_widget_set_name(thumb->w_altered, "thumb-altered");
    gtk_widget_set_valign(thumb->w_altered, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_altered, GTK_ALIGN_END);
    gtk_widget_set_no_show_all(thumb->w_altered, TRUE);
    g_signal_connect(G_OBJECT(thumb->w_altered), "enter-notify-event", G_CALLBACK(_event_btn_enter_leave), thumb);
    g_signal_connect(G_OBJECT(thumb->w_altered), "leave-notify-event", G_CALLBACK(_event_btn_enter_leave), thumb);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlays_parent), thumb->w_altered);

    // the group bouton
    thumb->w_group = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_grouping, 0, NULL);
    gtk_widget_set_name(thumb->w_group, "thumb-group-audio");
    g_signal_connect(G_OBJECT(thumb->w_group), "button-release-event", G_CALLBACK(_event_grouping_release), thumb);
    g_signal_connect(G_OBJECT(thumb->w_group), "enter-notify-event", G_CALLBACK(_event_btn_enter_leave), thumb);
    g_signal_connect(G_OBJECT(thumb->w_group), "leave-notify-event", G_CALLBACK(_event_btn_enter_leave), thumb);
    gtk_widget_set_valign(thumb->w_group, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_group, GTK_ALIGN_END);
    gtk_widget_set_no_show_all(thumb->w_group, TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlays_parent), thumb->w_group);

    // the sound icon
    thumb->w_audio = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_audio, 0, NULL);
    gtk_widget_set_name(thumb->w_audio, "thumb-group-audio");
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
    gtk_widget_set_name(thumb->w_zoom_eb, "thumb-zoom");
    gtk_widget_set_valign(thumb->w_zoom_eb, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_zoom_eb, GTK_ALIGN_START);
    if(zoom_ratio == IMG_TO_FIT)
      thumb->w_zoom = gtk_label_new(_("fit"));
    else
      thumb->w_zoom = gtk_label_new("mini");
    gtk_widget_set_name(thumb->w_zoom, "thumb-zoom-label");
    gtk_widget_show(thumb->w_zoom);
    gtk_container_add(GTK_CONTAINER(thumb->w_zoom_eb), thumb->w_zoom);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlays_parent), thumb->w_zoom_eb);

    dt_thumbnail_resize(thumb, thumb->width, thumb->height, TRUE, zoom_ratio);
  }
  gtk_widget_show(thumb->w_main);
  g_object_ref(G_OBJECT(thumb->w_main));
  return thumb->w_main;
}

dt_thumbnail_t *dt_thumbnail_new(int width, int height, float zoom_ratio, int imgid, int rowid,
                                 dt_thumbnail_overlay_t over, dt_thumbnail_container_t container, gboolean tooltip)
{
  dt_thumbnail_t *thumb = calloc(1, sizeof(dt_thumbnail_t));
  thumb->width = width;
  thumb->height = height;
  thumb->imgid = imgid;
  thumb->rowid = rowid;
  thumb->over = over;
  thumb->container = container;
  thumb->zoomable = (container == DT_THUMBNAIL_CONTAINER_CULLING
                     || container == DT_THUMBNAIL_CONTAINER_PREVIEW);
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
  if(thumb->over == DT_THUMBNAIL_OVERLAYS_ALWAYS_EXTENDED
     || thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_EXTENDED
     || over == DT_THUMBNAIL_OVERLAYS_MIXED
     || thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK)
    _thumb_update_extended_infos_line(thumb);

  // we read all other infos
  _image_get_infos(thumb);

  // we create the widget
  dt_thumbnail_create_widget(thumb, zoom_ratio);

  // let's see if the images are selected or active or mouse_overed
  _dt_active_images_callback(NULL, thumb);
  _dt_selection_changed_callback(NULL, thumb);
  if(dt_control_get_mouse_over_id() == thumb->imgid)
    dt_thumbnail_set_mouseover(thumb, TRUE);

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
  if(max_size < 2)
    max_size = round(1.2f * darktable.bauhaus->line_height); // fallback if toolbar icons are not realized

  if(thumb->over != DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK)
  {
    gtk_widget_get_size_request(thumb->w_main, &width, &height);
    // we need to squeeze reject + space + stars + space + colorlabels icons on a thumbnail width
    // that means a width of 4 + MAX_STARS icons size
    // all icons and spaces having a width of 2.5 * r1
    // inner margins are defined in css (margin_* values)

    // retrieves the size of the main icons in the top panel, thumbtable overlays shall not exceed that
    const float r1 = fminf(max_size / 2.0f,
                           (width - thumb->img_margin->left - thumb->img_margin->right) / (2.5 * (4 + MAX_STARS)));
    const int icon_size = roundf(2.5 * r1);

    // file extension
    gtk_widget_set_margin_top(thumb->w_ext, thumb->img_margin->top);
    gtk_widget_set_margin_start(thumb->w_ext, thumb->img_margin->left);

    // bottom background
    gtk_widget_set_margin_start(thumb->w_bottom, thumb->img_margin->left);
    gtk_widget_set_margin_end(thumb->w_bottom, thumb->img_margin->right);
    if(thumb->over == DT_THUMBNAIL_OVERLAYS_ALWAYS_EXTENDED
       || thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_EXTENDED
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
    gtk_widget_set_margin_start(thumb->w_bottom_eb, 0);

    // reject icon
    const int margin_b_icons = MAX(0, thumb->img_margin->bottom - icon_size * 0.125 - 1);
    gtk_widget_set_size_request(thumb->w_reject, icon_size, icon_size);
    gtk_widget_set_valign(thumb->w_reject, GTK_ALIGN_END);
    int pos = MAX(0, thumb->img_margin->left - icon_size * 0.125); // align on the left of the thumb
    gtk_widget_set_margin_start(thumb->w_reject, pos);
    gtk_widget_set_margin_bottom(thumb->w_reject, margin_b_icons);

    // stars
    for(int i = 0; i < MAX_STARS; i++)
    {
      gtk_widget_set_size_request(thumb->w_stars[i], icon_size, icon_size);
      gtk_widget_set_valign(thumb->w_stars[i], GTK_ALIGN_END);
      gtk_widget_set_margin_bottom(thumb->w_stars[i], margin_b_icons);
      gtk_widget_set_margin_start(
          thumb->w_stars[i],
          thumb->img_margin->left
              + (width - thumb->img_margin->left - thumb->img_margin->right - MAX_STARS * icon_size) * 0.5
              + i * icon_size);
    }

    // the color labels
    gtk_widget_set_size_request(thumb->w_color, icon_size, icon_size);
    gtk_widget_set_valign(thumb->w_color, GTK_ALIGN_END);
    gtk_widget_set_halign(thumb->w_color, GTK_ALIGN_START);
    gtk_widget_set_margin_bottom(thumb->w_color, margin_b_icons);
    pos = width - thumb->img_margin->right - icon_size + icon_size * 0.125; // align on the right
    gtk_widget_set_margin_start(thumb->w_color, pos);

    // the local copy indicator
    _set_flag(thumb->w_local_copy, GTK_STATE_FLAG_ACTIVE, FALSE);
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
    gtk_widget_get_size_request(thumb->w_image, &width, &height);
    int w = 0;
    int h = 0;
    gtk_widget_get_size_request(thumb->w_image_box, &w, &h);
    const int px = (w - width) / 2;
    const int py = (h - height) / 2;

    // we need to squeeze 5 stars + 1 reject + 1 colorlabels symbols on a thumbnail width
    // all icons having a width of 3.0 * r1 => 21 * r1
    // we want r1 spaces at extremities, after reject, before colorlables => 4 * r1
    const float r1 = fminf(max_size / 2.0f, width / 25.0f);

    // file extension
    gtk_widget_set_margin_top(thumb->w_ext, 0.03 * width + py);
    gtk_widget_set_margin_start(thumb->w_ext, 0.03 * width + px);

    // bottom background
    attrlist = pango_attr_list_new();
    attr = pango_attr_size_new_absolute(1.5 * r1 * PANGO_SCALE);
    pango_attr_list_insert(attrlist, attr);
    gtk_label_set_attributes(GTK_LABEL(thumb->w_bottom), attrlist);
    gtk_label_set_attributes(GTK_LABEL(thumb->w_zoom), attrlist);
    pango_attr_list_unref(attrlist);
    w = 0;
    h = 0;
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

    gtk_widget_set_margin_top(thumb->w_bottom_eb, margin_t + border_t + py);
    gtk_widget_set_margin_start(thumb->w_bottom_eb, margin_l + border_l + px);
    gtk_widget_set_margin_top(thumb->w_bottom, padding_t);
    gtk_widget_set_margin_start(thumb->w_bottom, padding_t);
    gtk_widget_set_margin_end(thumb->w_bottom, padding_t);

    // reject icon
    gtk_widget_set_size_request(thumb->w_reject, icon_size, icon_size);
    gtk_widget_set_valign(thumb->w_reject, GTK_ALIGN_START);
    gtk_widget_set_margin_start(thumb->w_reject, padding - icon_size / 8.0 + border_l + px);
    gtk_widget_set_margin_top(thumb->w_reject, line2 + py);
    // stars
    for(int i = 0; i < MAX_STARS; i++)
    {
      gtk_widget_set_size_request(thumb->w_stars[i], icon_size, icon_size);
      gtk_widget_set_valign(thumb->w_stars[i], GTK_ALIGN_START);
      gtk_widget_set_margin_top(thumb->w_stars[i], line2 + py);
      gtk_widget_set_margin_start(thumb->w_stars[i],
                                  padding - icon_size / 8.0 + border_l + r1 + (i + 1) * 3.0 * r1 + px);
    }
    // the color labels
    gtk_widget_set_size_request(thumb->w_color, icon_size, icon_size);
    gtk_widget_set_valign(thumb->w_color, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_color, GTK_ALIGN_START);
    gtk_widget_set_margin_top(thumb->w_color, line2 + py);
    gtk_widget_set_margin_start(thumb->w_color,
                                padding - icon_size / 8.0 + border_l + 2.0 * r1 + (MAX_STARS + 1) * 3.0 * r1 + px);
    // the local copy indicator
    _set_flag(thumb->w_local_copy, GTK_STATE_FLAG_ACTIVE, TRUE);
    gtk_widget_set_size_request(thumb->w_local_copy, icon_size2, icon_size2);
    gtk_widget_set_halign(thumb->w_local_copy, GTK_ALIGN_START);
    gtk_widget_set_margin_top(thumb->w_local_copy, line3 + py);
    gtk_widget_set_margin_start(thumb->w_local_copy, 10.0 * r1 + px);
    // the altered icon
    gtk_widget_set_size_request(thumb->w_altered, icon_size2, icon_size2);
    gtk_widget_set_halign(thumb->w_altered, GTK_ALIGN_START);
    gtk_widget_set_margin_top(thumb->w_altered, line3 + py);
    gtk_widget_set_margin_start(thumb->w_altered, 7.0 * r1 + px);
    // the group bouton
    gtk_widget_set_size_request(thumb->w_group, icon_size2, icon_size2);
    gtk_widget_set_halign(thumb->w_group, GTK_ALIGN_START);
    gtk_widget_set_margin_top(thumb->w_group, line3 + py);
    gtk_widget_set_margin_start(thumb->w_group, 4.0 * r1 + px);
    // the sound icon
    gtk_widget_set_size_request(thumb->w_audio, icon_size2, icon_size2);
    gtk_widget_set_halign(thumb->w_audio, GTK_ALIGN_START);
    gtk_widget_set_margin_top(thumb->w_audio, line3 + py);
    gtk_widget_set_margin_start(thumb->w_audio, r1 + px);
    // the zoomming indicator
    gtk_widget_set_margin_top(thumb->w_zoom_eb, line3 + py);
    gtk_widget_set_margin_start(thumb->w_zoom_eb, 18.0 * r1 + px);
  }
}

void dt_thumbnail_resize(dt_thumbnail_t *thumb, int width, int height, gboolean force, float zoom_ratio)
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

  // for thumbtable, we need to set the size class to the image widget
  if(thumb->container == DT_THUMBNAIL_CONTAINER_LIGHTTABLE)
  {
    // we get the corresponding size
    const char *txt = dt_conf_get_string_const("plugins/lighttable/thumbnail_sizes");
    gchar **ts = g_strsplit(txt, "|", -1);
    int i = 0;
    while(ts[i])
    {
      const int s = g_ascii_strtoll(ts[i], NULL, 10);
      if(thumb->width < s) break;
      i++;
    }
    g_strfreev(ts);

    gchar *cl = g_strdup_printf("dt_thumbnails_%d", i);
    GtkStyleContext *context = gtk_widget_get_style_context(thumb->w_image);
    if(!gtk_style_context_has_class(context, cl))
    {
      // we remove all previous size class if any
      GList *l = gtk_style_context_list_classes(context);
      for(GList *l_iter = l; l_iter; l_iter = g_list_next(l_iter))
      {
        gchar *ll = (gchar *)l_iter->data;
        if(g_str_has_prefix(ll, "dt_thumbnails_"))
        {
          gtk_style_context_remove_class(context, ll);
        }
      }
      g_list_free(l);

      // we set the new class
      gtk_style_context_add_class(context, cl);
    }
    g_free(cl);
  }

  // file extension
  _thumb_retrieve_margins(thumb);
  gtk_widget_set_margin_start(thumb->w_ext, thumb->img_margin->left);
  gtk_widget_set_margin_top(thumb->w_ext, thumb->img_margin->top);

  // retrieves the size of the main icons in the top panel, thumbtable overlays shall not exceed that
  int max_size = darktable.gui->icon_size;
  if(max_size < 2)
    max_size = round(1.2f * darktable.bauhaus->line_height); // fallback if toolbar icons are not realized

  const int fsize = fminf(max_size, (height - thumb->img_margin->top - thumb->img_margin->bottom) / 11.0f);

  PangoAttrList *attrlist = pango_attr_list_new();
  PangoAttribute *attr = pango_attr_size_new_absolute(fsize * PANGO_SCALE);
  pango_attr_list_insert(attrlist, attr);
  // the idea is to reduce line-height, but it doesn't work for whatever reason...
  // PangoAttribute *attr2 = pango_attr_rise_new(-fsize * PANGO_SCALE);
  // pango_attr_list_insert(attrlist, attr2);
  gtk_label_set_attributes(GTK_LABEL(thumb->w_ext), attrlist);
  pango_attr_list_unref(attrlist);

  // for overlays different than block, we compute their size here, so we have valid value for th image area compute
  if(thumb->over != DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK) _thumb_resize_overlays(thumb);
  // we change the size and margins according to the size change. This will be refined after
  _thumb_set_image_area(thumb, zoom_ratio);

  // and the overlays for the block only (the others have been done before)
  if(thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK) _thumb_resize_overlays(thumb);

  // reset surface
  dt_thumbnail_image_refresh(thumb);
}

void dt_thumbnail_set_group_border(dt_thumbnail_t *thumb, dt_thumbnail_border_t border)
{
  if(border == DT_THUMBNAIL_BORDER_NONE)
  {
    dt_gui_remove_class(thumb->w_main, "dt_group_left");
    dt_gui_remove_class(thumb->w_main, "dt_group_top");
    dt_gui_remove_class(thumb->w_main, "dt_group_right");
    dt_gui_remove_class(thumb->w_main, "dt_group_bottom");
    thumb->group_borders = DT_THUMBNAIL_BORDER_NONE;
    return;
  }
  else if(border & DT_THUMBNAIL_BORDER_LEFT)
    dt_gui_add_class(thumb->w_main, "dt_group_left");
  else if(border & DT_THUMBNAIL_BORDER_TOP)
    dt_gui_add_class(thumb->w_main, "dt_group_top");
  else if(border & DT_THUMBNAIL_BORDER_RIGHT)
    dt_gui_add_class(thumb->w_main, "dt_group_right");
  else if(border & DT_THUMBNAIL_BORDER_BOTTOM)
    dt_gui_add_class(thumb->w_main, "dt_group_bottom");

  thumb->group_borders |= border;
}

void dt_thumbnail_set_mouseover(dt_thumbnail_t *thumb, gboolean over)
{
  if(thumb->mouse_over == over) return;
  thumb->mouse_over = over;
  _thumbs_show_overlays(thumb);

  if(!thumb->mouse_over) _set_flag(thumb->w_bottom_eb, GTK_STATE_FLAG_PRELIGHT, FALSE);

  _set_flag(thumb->w_main, GTK_STATE_FLAG_PRELIGHT, thumb->mouse_over);
  _set_flag(thumb->w_image_box, GTK_STATE_FLAG_PRELIGHT, thumb->mouse_over);

  gtk_widget_queue_draw(thumb->w_main);
}

// set if the thumbnail should react (mouse_over) to drag and drop
// note that it's just cosmetic as dropping occurs in thumbtable in any case
void dt_thumbnail_set_drop(dt_thumbnail_t *thumb, gboolean accept_drop)
{
  if(accept_drop)
  {
    gtk_drag_dest_set(thumb->w_main, GTK_DEST_DEFAULT_MOTION, target_list_all, n_targets_all, GDK_ACTION_MOVE);
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

  // we ensure that the image is not completely outside the thumbnail, otherwise the image_draw is not triggered
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
  // if no change...
  if(thumb->over == over)
  {
    // eventual timeout change
    if(thumb->overlay_timeout_duration != timeout)
    {
      thumb->overlay_timeout_duration = timeout;
      if(thumb->overlay_timeout_id > 0)
      {
        g_source_remove(thumb->overlay_timeout_id);
        thumb->overlay_timeout_id = 0;
      }
      if(timeout < 0)
        _thumbs_show_overlays(thumb);
      else
        _thumbs_hide_overlays(thumb);
    }
    return;
  }

  thumb->overlay_timeout_duration = timeout;
  const dt_thumbnail_overlay_t old_over = thumb->over;
  thumb->over = over;

  // first, if we change from/to hover/block, we need to change some parent widgets
  if(old_over == DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK
     || over == DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK)
  {
    GtkOverlay *overlays_parent = GTK_OVERLAY(thumb->w_main);
    if(thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK)
      overlays_parent = GTK_OVERLAY(thumb->w_image_box);

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
  gtk_widget_get_size_request(thumb->w_image, &iw, &ih);
  thumb->zoomx = CLAMP(thumb->zoomx, (iw * darktable.gui->ppd_thb - thumb->img_width) / darktable.gui->ppd_thb, 0);
  thumb->zoomy = CLAMP(thumb->zoomy, (ih * darktable.gui->ppd_thb - thumb->img_height) / darktable.gui->ppd_thb, 0);
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

float dt_thumbnail_get_zoom_ratio(dt_thumbnail_t *thumb)
{
  if(thumb->zoom_100 < 1.0f) // we only compute the sizes if needed
    dt_thumbnail_get_zoom100(thumb);

  return _thumb_zoom_to_zoom_ratio(thumb->zoom, thumb->zoom_100);
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
  if(thumb->over == DT_THUMBNAIL_OVERLAYS_ALWAYS_EXTENDED
     || thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_EXTENDED
     || thumb->over == DT_THUMBNAIL_OVERLAYS_MIXED
     || thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK)
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
  if(thumb->over == DT_THUMBNAIL_OVERLAYS_ALWAYS_EXTENDED
     || thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_EXTENDED
     || thumb->over == DT_THUMBNAIL_OVERLAYS_MIXED
     || thumb->over == DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK)
    lb = g_strdup(thumb->info_line);

  // we set the text
  gtk_label_set_markup(GTK_LABEL(thumb->w_bottom), lb);
  g_free(lb);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
