/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.
    copyright (c) 2011--2012 Henrik Andersson.
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
/** this is the view for the lighttable module.  */
#include "views/view.h"
#include "libs/lib.h"
#include "control/jobs.h"
#include "control/settings.h"
#include "control/control.h"
#include "control/conf.h"
#include "common/image_cache.h"
#include "common/darktable.h"
#include "common/collection.h"
#include "common/colorlabels.h"
#include "common/selection.h"
#include "common/debug.h"
#include "common/grouping.h"
#include "common/history.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/draw.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <assert.h>
#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

static gboolean star_key_accel_callback(GtkAccelGroup *accel_group,
                                        GObject *acceleratable, guint keyval,
                                        GdkModifierType modifier, gpointer data);
static gboolean go_up_key_accel_callback(GtkAccelGroup *accel_group,
    GObject *acceleratable, guint keyval,
    GdkModifierType modifier, gpointer data);
static gboolean go_down_key_accel_callback(GtkAccelGroup *accel_group,
    GObject *acceleratable, guint keyval,
    GdkModifierType modifier, gpointer data);
static gboolean
go_pgup_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                           guint keyval, GdkModifierType modifier,
                           gpointer data);
static gboolean
go_pgdown_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                             guint keyval, GdkModifierType modifier,
                             gpointer data);

/**
 * this organises the whole library:
 * previously imported film rolls..
 */
typedef struct dt_library_t
{
  // tmp mouse vars:
  float select_offset_x, select_offset_y;
  int32_t last_selected_idx, selection_origin_idx;
  int button;
  int layout;
  uint32_t modifiers;
  uint32_t center, pan;
  int32_t track, offset, first_visible_zoomable, first_visible_filemanager;
  float zoom_x, zoom_y;
  dt_view_image_over_t image_over;
  int full_preview;
  int32_t full_preview_id;
  gboolean offset_changed;
  GdkColor star_color;

  int32_t collection_count;

  /* prepared and reusable statements */
  struct
  {
    /* main query statment, should be update on listener signal of collection */
    sqlite3_stmt *main_query;
    /* select imgid from selected_images */
    sqlite3_stmt *select_imgid_in_selection;
    /* delete from selected_images where imgid != ?1 */
    sqlite3_stmt *delete_except_arg;
    /* check if the group of the image under the mouse has others, too, ?1: group_id, ?2: imgid */
    sqlite3_stmt *is_grouped;
  } statements;

}
dt_library_t;

// needed for drag&drop
static GtkTargetEntry target_list[] = { { "text/uri-list", GTK_TARGET_OTHER_APP, 0 } };
static guint n_targets = G_N_ELEMENTS (target_list);

const char *name(dt_view_t *self)
{
  return _("lighttable");
}


uint32_t view(dt_view_t *self)
{
  return DT_VIEW_LIGHTTABLE;
}

typedef enum direction
{
  UP = 0,
  DOWN = 1,
  LEFT = 2,
  RIGHT = 3,
  ZOOM_IN = 4,
  ZOOM_OUT = 5,
  TOP = 6,
  BOTTOM = 7,
  PGUP = 8,
  PGDOWN = 9
}direction;

void switch_layout_to(dt_library_t *lib, int new_layout)
{
  lib->layout = new_layout;
  
  if (new_layout == 1) // filemanager
  {
    if(lib->first_visible_zoomable >= 0)
    {
      lib->offset = lib->first_visible_zoomable;
    }
    lib->first_visible_zoomable = 0;
    
    if(lib->center) lib->offset = 0;
    lib->center = 0;
    
    lib->offset_changed = TRUE;
  }
}

static void move_view(dt_library_t *lib, direction dir)
{
  const int iir = dt_conf_get_int("plugins/lighttable/images_in_row");
 
  switch (dir)
  {
    case UP:
      {
        if (lib->offset >= 1)
          lib->offset = lib->offset - iir;
      }
      break;
    case DOWN:
      {
        lib->offset = lib->offset + iir;
        while(lib->offset > lib->collection_count)
          lib->offset -= iir;
      }
      break;
    case PGUP:
      {
          //TODO: this behavior has not been changed, but it really ought to be fixed so it scrolls a full page up or down.
          lib->offset -= 4*iir;
          while(lib->offset < 0)
            lib->offset += iir;
      }
      break;
    case PGDOWN:
      {
        //TODO: this behavior has not been changed, but it really ought to be fixed so it scrolls a full page up or down.
        lib->offset += 4*iir;
        while(lib->offset > lib->collection_count)
          lib->offset -= iir;
      }
      break;
    case TOP:
      {
        lib->offset = 0;
      }
      break;
    case BOTTOM:
      {
        lib->offset = lib->collection_count - iir;
      }
      break;
    default:
      break;
  }

  lib->first_visible_filemanager = lib->offset;
  lib->offset_changed = TRUE;
}

/* This function allows the file manager view to zoom "around" the image
 * currently under the mouse cursor, instead of around the top left image */
void zoom_around_image(dt_library_t *lib, double pointerx, double pointery, int width, int height, int old_images_in_row, int new_images_in_row)
{
  /* calculate which image number (relative to total collection)
   * is currently under the cursor, i.e. which image is the zoom anchor */
  float wd = width/(float)old_images_in_row;
  float ht = width/(float)old_images_in_row;
  int pi = pointerx / (float)wd;
  int pj = pointery / (float)ht;  
    
  int zoom_anchor_image = lib->offset + pi + (pj * old_images_in_row);
  
  // make sure that we don't try to zoom around an image that doesn't exist
  if (zoom_anchor_image > lib->collection_count)
    zoom_anchor_image = lib->collection_count;
  
    // make sure that we don't try to zoom around an image that doesn't exist
  if (zoom_anchor_image < 0)
    zoom_anchor_image = 0;
  
  /* calculate which image number (relative to offset) will be
   * under the cursor after zooming. Then subtract that value 
   * from the zoom anchor image number to see what the new offset should be */
  wd = width/(float)new_images_in_row;
  ht = width/(float)new_images_in_row;
  pi = pointerx / (float)wd;
  pj = pointery / (float)ht;
  
  lib->offset = zoom_anchor_image - pi - (pj * new_images_in_row);
  lib->first_visible_filemanager = lib->offset;
  lib->offset_changed = TRUE;
}

static void _view_lighttable_collection_listener_callback(gpointer instance, gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_library_t *lib = (dt_library_t *)self->data;

  /* check if we can get a query from collection */
  const gchar *query=dt_collection_get_query (darktable.collection);
  if(!query)
    return;

  /* if we have a statment lets clean it */
  if(lib->statements.main_query)
    sqlite3_finalize(lib->statements.main_query);

  /* prepare a new main query statement for collection */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &lib->statements.main_query, NULL);

  /* set the centerview scroll to top */
  if(instance != NULL)
    lib->offset=0;

  dt_control_queue_redraw_center();
}

void init(dt_view_t *self)
{
  self->data = malloc(sizeof(dt_library_t));
  dt_library_t *lib = (dt_library_t *)self->data;
  memset(self->data,0,sizeof(dt_library_t));

  lib->select_offset_x = lib->select_offset_y = 0.5f;
  lib->last_selected_idx = -1;
  lib->selection_origin_idx = -1;
  lib->first_visible_zoomable = lib->first_visible_filemanager = 0;
  lib->button = 0;
  lib->modifiers = 0;
  lib->center = lib->pan = lib->track = 0;
  lib->zoom_x = 0.0f;
  lib->zoom_y = 0.0f;
  lib->full_preview=0;
  lib->full_preview_id=-1;
    
  GtkStyle *style = gtk_rc_get_style_by_paths(gtk_settings_get_default(), "dt-stars", NULL, GTK_TYPE_NONE);

  lib->star_color.red = (255/ 65535) * style->fg[GTK_STATE_NORMAL].red;
  lib->star_color.blue = (255/ 65535) * style->fg[GTK_STATE_NORMAL].blue;
  lib->star_color.green = (255/ 65535) * style->fg[GTK_STATE_NORMAL].green;

  /* setup collection listener and initialize main_query statement */
  dt_control_signal_connect(darktable.signals,
                            DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_view_lighttable_collection_listener_callback),
                            (gpointer) self);

  _view_lighttable_collection_listener_callback(NULL,self);

  /* initialize reusable sql statements */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from selected_images where imgid != ?1", -1, &lib->statements.delete_except_arg, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from images where group_id = ?1 and id != ?2", -1, &lib->statements.is_grouped, NULL); //TODO: only check in displayed images?
}


void cleanup(dt_view_t *self)
{
  free(self->data);
}

/**
 * \brief A helper function to convert grid coordinates to an absolute index
 *
 * \param[in] row The row
 * \param[in] col The column
 * \param[in] stride The stride (number of columns per row)
 * \param[in] offset The zero-based index of the top-left image (aka the count of images above the viewport, minus 1)
 * \return The absolute, zero-based index of the specified grid location
 */

#if 0
static int
grid_to_index (int row, int col, int stride, int offset)
{
  return row * stride + col + offset;
}
#endif

static void
expose_filemanager (dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_library_t *lib = (dt_library_t *)self->data;

  gboolean offset_changed = FALSE;

  /* query new collection count */
  lib->collection_count = dt_collection_get_count (darktable.collection);

  if(darktable.gui->center_tooltip == 1)
    darktable.gui->center_tooltip = 2;

  /* get grid stride */
  const int iir = dt_conf_get_int("plugins/lighttable/images_in_row");

  /* get image over id */
  lib->image_over = DT_VIEW_DESERT;
  int32_t mouse_over_id, mouse_over_group = -1;
  DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);

  /* fill background */
  cairo_set_source_rgb (cr, .2, .2, .2);
  cairo_paint(cr);

  offset_changed = lib->offset_changed;

  static int oldpan = 0;
  const int pan = lib->pan;

  const float wd = width/(float)iir;
  const float ht = width/(float)iir;

  int pi = pointerx / (float)wd;
  int pj = pointery / (float)ht;
  if(pointerx < 0 || pointery < 0) pi = pj = -1;
  //const int pidx = grid_to_index(pj, pi, iir, offset);

  const int img_pointerx = iir == 1 ? pointerx : fmodf(pointerx, wd);
  const int img_pointery = iir == 1 ? pointery : fmodf(pointery, ht);

  const int max_rows = 1 + (int)((height)/ht + .5);
  const int max_cols = iir;
  
  int id;
  int clicked1 = (oldpan == 0 && pan == 1 && lib->button == 1);

  /* get the count of current collection */

  if(lib->collection_count == 0)
  {
    const float fs = 15.0f;
    const float ls = 1.5f*fs;
    const float offy = height*0.2f;
    const float offx = 60;
    const float at = 0.3f;
    cairo_set_font_size(cr, fs);
    cairo_set_source_rgba(cr, .7, .7, .7, 1.0f);
    cairo_select_font_face (cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_move_to(cr, offx, offy);
    cairo_show_text(cr, _("there are no images in this collection"));
    cairo_move_to(cr, offx, offy + 2*ls);
    cairo_show_text(cr, _("if you have not imported any images yet"));
    cairo_move_to(cr, offx, offy + 3*ls);
    cairo_show_text(cr, _("you can do so in the import module"));
    cairo_move_to(cr, offx - 10.0f, offy + 3*ls - ls*.25f);
    cairo_line_to(cr, 0.0f, 10.0f);
    cairo_set_source_rgba(cr, .7, .7, .7, at);
    cairo_stroke(cr);
    cairo_move_to(cr, offx, offy + 5*ls);
    cairo_set_source_rgba(cr, .7, .7, .7, 1.0f);
    cairo_show_text(cr, _("try to relax the filter settings in the top panel"));
    cairo_rel_move_to(cr, 10.0f, -ls*.25f);
    cairo_line_to(cr, width*0.5f, 0.0f);
    cairo_set_source_rgba(cr, .7, .7, .7, at);
    cairo_stroke(cr);
    cairo_move_to(cr, offx, offy + 6*ls);
    cairo_set_source_rgba(cr, .7, .7, .7, 1.0f);
    cairo_show_text(cr, _("or add images in the collection module in the left panel"));
    cairo_move_to(cr, offx - 10.0f, offy + 6*ls - ls*0.25f);
    cairo_rel_line_to(cr, - offx + 10.0f, 0.0f);
    cairo_set_source_rgba(cr, .7, .7, .7, at);
    cairo_stroke(cr);

    return;
  }

  /* do we have a main query collection statement */
  if(!lib->statements.main_query)
    return;
  
  int32_t offset = lib->offset = lib->first_visible_filemanager;

  int32_t drawing_offset = 0;
  if(offset < 0)
  {
    drawing_offset = offset;
    offset = 0;
  }
  
  /* update scroll borders */
  dt_view_set_scrollbar(self, 0, 1, 1, offset, lib->collection_count, max_rows*iir);

  /* let's reset and reuse the main_query statement */
  DT_DEBUG_SQLITE3_CLEAR_BINDINGS(lib->statements.main_query);
  DT_DEBUG_SQLITE3_RESET(lib->statements.main_query);

  /* setup offset and row for the main query */
  DT_DEBUG_SQLITE3_BIND_INT(lib->statements.main_query, 1, offset);
  DT_DEBUG_SQLITE3_BIND_INT(lib->statements.main_query, 2, max_rows*iir);

  if(mouse_over_id != -1)
  {
    const dt_image_t *mouse_over_image = dt_image_cache_read_get(darktable.image_cache, mouse_over_id);
    mouse_over_group = mouse_over_image->group_id;
    dt_image_cache_read_release(darktable.image_cache, mouse_over_image);
    DT_DEBUG_SQLITE3_CLEAR_BINDINGS(lib->statements.is_grouped);
    DT_DEBUG_SQLITE3_RESET(lib->statements.is_grouped);
    DT_DEBUG_SQLITE3_BIND_INT(lib->statements.is_grouped, 1, mouse_over_group);
    DT_DEBUG_SQLITE3_BIND_INT(lib->statements.is_grouped, 2, mouse_over_id);
    if(sqlite3_step(lib->statements.is_grouped) != SQLITE_ROW)
      mouse_over_group = -1;
  }

  // prefetch the ids so that we can peek into the future to see if there are adjacent images in the same group.
  int *query_ids = (int*)calloc(max_rows*max_cols, sizeof(int));
  if(!query_ids) goto after_drawing;
  for(int row = 0; row < max_rows; row++)
  {
    for(int col = 0; col < max_cols; col++)
    {
      if(sqlite3_step(lib->statements.main_query) == SQLITE_ROW)
        query_ids[row*iir+col] = sqlite3_column_int(lib->statements.main_query, 0);
      else goto end_query_cache;
    }
  }

end_query_cache:
  mouse_over_id = -1;
  cairo_save(cr);
  int current_image =0;
  
  for(int row = 0; row < max_rows; row++)
  {
    for(int col = 0; col < max_cols; col++)
    {
      //curidx = grid_to_index(row, col, iir, offset);

      /* skip drawing images until we reach a non-negative offset. 
       * This is needed for zooming, so that the image under the 
       * mouse cursor can stay there. */
      if (drawing_offset < 0)
      {
        drawing_offset++;
        cairo_translate(cr, wd, 0.0f);
        continue; 
      }

      id = query_ids[current_image];
      current_image++;
      
      if(id > 0)
      {
        if (iir == 1 && row)
          continue;

        /* set mouse over id if pointer is in current row / col */
        if(pi == col && pj == row)
        {
          mouse_over_id = id;
        }

        /* handle mouse click on current row / col
           this could easily and preferable be moved to button_pressed()
         */
        if (clicked1 && (pi == col && pj == row))
        {
          if ((lib->modifiers & (GDK_SHIFT_MASK|GDK_CONTROL_MASK)) == 0)
            dt_selection_select_single(darktable.selection, id);
          else if ((lib->modifiers & (GDK_CONTROL_MASK)) == GDK_CONTROL_MASK)
            dt_selection_toggle(darktable.selection, id);
          else if ((lib->modifiers & (GDK_SHIFT_MASK)) == GDK_SHIFT_MASK)
            dt_selection_select_range(darktable.selection, id);
        }

        cairo_save(cr);
        // if(iir == 1) dt_image_prefetch(image, DT_IMAGE_MIPF);
        dt_view_image_expose(&(lib->image_over), id, cr, wd, iir == 1 ? height : ht, iir, img_pointerx, img_pointery, FALSE);

        cairo_restore(cr);
      }
      else
        goto escape_image_loop;

      cairo_translate(cr, wd, 0.0f);
    }
    cairo_translate(cr, -max_cols*wd, ht);
  }
escape_image_loop:
  cairo_restore(cr);

  DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, mouse_over_id);

  // and now the group borders
  cairo_save(cr);
  current_image = 0;
  if(lib->offset < 0)
  {
    drawing_offset = lib->offset;
    offset = 0;
  }
  for(int row = 0; row < max_rows; row++)
  {
    for(int col = 0; col < max_cols; col++)
    {
      /* skip drawing images until we reach a non-negative offset. 
       * This is needed for zooming, so that the image under the 
       * mouse cursor can be stay there. */
      if (drawing_offset < 0)
      {
        drawing_offset++;
        cairo_translate(cr, wd, 0.0f);
        continue;
      }

      id = query_ids[current_image];
     
      
      if(id > 0)
      {
        const dt_image_t *image = dt_image_cache_read_get(darktable.image_cache, id);
        int group_id = -1;
        if(image)
          group_id = image->group_id;
        dt_image_cache_read_release(darktable.image_cache, image);

        if (iir == 1 && row)
          continue;

        cairo_save(cr);

        gboolean paint_border = FALSE;
        // regular highlight border
        if(group_id != -1)
        {
          if(mouse_over_group == group_id && iir > 1 && ((!darktable.gui->grouping && dt_conf_get_bool("plugins/lighttable/draw_group_borders")) || group_id == darktable.gui->expanded_group_id))
          {
            cairo_set_source_rgb(cr, 1, 0.8, 0);
            paint_border = TRUE;
          }
          // border of expanded group
          else if(darktable.gui->grouping && group_id == darktable.gui->expanded_group_id && iir > 1)
          {
            cairo_set_source_rgb(cr, 0, 0, 1);
            paint_border = TRUE;
          }
        }

        if(paint_border)
        {
          int neighbour_group = -1;
          // top border
          if(row > 0)
          {
            int _id = query_ids[current_image - iir];
            if(_id > 0)
            {
              const dt_image_t *_img = dt_image_cache_read_get(darktable.image_cache, _id);
              neighbour_group = _img->group_id;
              dt_image_cache_read_release(darktable.image_cache, _img);
            }
          }
          if(neighbour_group != group_id)
          {
            cairo_move_to(cr, 0, 0);
            cairo_line_to(cr, wd, 0);
          }
          // left border
          neighbour_group = -1;
          if(col > 0)
          {
            int _id = query_ids[current_image-1];
            if(_id > 0)
            {
              const dt_image_t *_img = dt_image_cache_read_get(darktable.image_cache, _id);
              neighbour_group = _img->group_id;
              dt_image_cache_read_release(darktable.image_cache, _img);
            }
          }
          if(neighbour_group != group_id)
          {
            cairo_move_to(cr, 0, 0);
            cairo_line_to(cr, 0, ht);
          }
          // bottom border
          neighbour_group = -1;
          if(row < max_rows-1)
          {
            int _id = query_ids[current_image+iir];
            if(_id > 0)
            {
              const dt_image_t *_img = dt_image_cache_read_get(darktable.image_cache, _id);
              neighbour_group = _img->group_id;
              dt_image_cache_read_release(darktable.image_cache, _img);
            }
          }
          if(neighbour_group != group_id)
          {
            cairo_move_to(cr, 0, ht);
            cairo_line_to(cr, wd, ht);
          }
          // right border
          neighbour_group = -1;
          if(col < max_cols-1)
          {
            int _id = query_ids[current_image+1];
            if(_id > 0)
            {
              const dt_image_t *_img = dt_image_cache_read_get(darktable.image_cache, _id);
              neighbour_group = _img->group_id;
              dt_image_cache_read_release(darktable.image_cache, _img);
            }
          }
          if(neighbour_group != group_id)
          {
            cairo_move_to(cr, wd, 0);
            cairo_line_to(cr, wd, ht);
          }
          cairo_set_line_width(cr, 0.01*wd);
          cairo_stroke(cr);
        }

        cairo_restore(cr);
        current_image ++;
      }
      else
        goto escape_border_loop;

      cairo_translate(cr, wd, 0.0f);
    }
    cairo_translate(cr, -max_cols*wd, ht);
  }
escape_border_loop:
  cairo_restore(cr);
after_drawing:
  /* check if offset was changed and we need to prefetch thumbs */
  if (offset_changed)
  {
    int32_t imgids_num = 0;
    const int prefetchrows = .5*max_rows+1;
    int32_t imgids[prefetchrows*iir];
    /* clear and reset main query */
    DT_DEBUG_SQLITE3_CLEAR_BINDINGS(lib->statements.main_query);
    DT_DEBUG_SQLITE3_RESET(lib->statements.main_query);

    /* setup offest and row for prefetch */
    DT_DEBUG_SQLITE3_BIND_INT(lib->statements.main_query, 1, offset + max_rows*iir);
    DT_DEBUG_SQLITE3_BIND_INT(lib->statements.main_query, 2, prefetchrows*iir);

    // prefetch jobs in inverse order: supersede previous jobs: most important last
    while(sqlite3_step(lib->statements.main_query) == SQLITE_ROW && imgids_num < prefetchrows*iir)
      imgids[imgids_num++] = sqlite3_column_int(lib->statements.main_query, 0);

    float imgwd = iir == 1 ? 0.97 : 0.8;
    dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(
                             darktable.mipmap_cache,
                             imgwd*wd, imgwd*(iir==1?height:ht));
    while(imgids_num > 0)
    {
      imgids_num --;
      dt_mipmap_buffer_t buf;
      dt_mipmap_cache_read_get(
        darktable.mipmap_cache,
        &buf,
        imgids[imgids_num],
        mip,
        DT_MIPMAP_PREFETCH);
    }
  }

  if(query_ids)
    free(query_ids);
  oldpan = pan;
  if(darktable.unmuted & DT_DEBUG_CACHE)
    dt_mipmap_cache_print(darktable.mipmap_cache);

  if(darktable.gui->center_tooltip == 1) // set in this round
  {
    char* tooltip = dt_history_get_items_as_string(mouse_over_id);
    if(tooltip != NULL)
    {
      g_object_set(G_OBJECT(dt_ui_center(darktable.gui->ui)), "tooltip-text", tooltip, (char *)NULL);
      g_free(tooltip);
    }
  }
  else if(darktable.gui->center_tooltip == 2)   // not set in this round
  {
    darktable.gui->center_tooltip = 0;
    g_object_set(G_OBJECT(dt_ui_center(darktable.gui->ui)), "tooltip-text", "", (char *)NULL);
  }
}


// TODO: this is also defined in lib/tools/lighttable.c
//       fix so this value is shared.. DT_CTL_SET maybe ?

#define DT_LIBRARY_MAX_ZOOM 13

static void
expose_zoomable (dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  float zoom, zoom_x, zoom_y;
  int32_t mouse_over_id, pan, track, center;
  /* query new collection count */
  lib->collection_count = dt_collection_get_count (darktable.collection);

  DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
  zoom   = dt_conf_get_int("plugins/lighttable/images_in_row");
  zoom_x = lib->zoom_x;
  zoom_y = lib->zoom_y;
  pan    = lib->pan;
  center = lib->center;
  track  = lib->track;

  lib->image_over = DT_VIEW_DESERT;

  cairo_set_source_rgb (cr, .2, .2, .2);
  cairo_paint(cr);

  const float wd = width/zoom;
  const float ht = width/zoom;

  static int oldpan = 0;
  static float oldzoom = -1;
  if(oldzoom < 0) oldzoom = zoom;

  // TODO: exaggerate mouse gestures to pan when zoom == 1
  if(pan)// && mouse_over_id >= 0)
  {
    zoom_x = lib->select_offset_x - /* (zoom == 1 ? 2. : 1.)*/pointerx;
    zoom_y = lib->select_offset_y - /* (zoom == 1 ? 2. : 1.)*/pointery;
  }

  if(!lib->statements.main_query)
    return;

  if     (track == 0);
  else if(track >  1)  zoom_y += ht;
  else if(track >  0)  zoom_x += wd;
  else if(track > -2)  zoom_x -= wd;
  else                 zoom_y -= ht;
  if(zoom > DT_LIBRARY_MAX_ZOOM)
  {
    // double speed.
    if     (track == 0);
    else if(track >  1)  zoom_y += ht;
    else if(track >  0)  zoom_x += wd;
    else if(track > -2)  zoom_x -= wd;
    else                 zoom_y -= ht;
    if(zoom > 1.5*DT_LIBRARY_MAX_ZOOM)
    {
      // quad speed.
      if     (track == 0);
      else if(track >  1)  zoom_y += ht;
      else if(track >  0)  zoom_x += wd;
      else if(track > -2)  zoom_x -= wd;
      else                 zoom_y -= ht;
    }
  }

  if(oldzoom != zoom)
  {
    float oldx = (pointerx + zoom_x)*oldzoom/width;
    float oldy = (pointery + zoom_y)*oldzoom/width;
    if(zoom == 1)
    {
      zoom_x = (int)oldx*wd;
      zoom_y = (int)oldy*ht;
      lib->offset = 0x7fffffff;
    }
    else
    {
      zoom_x = oldx*wd - pointerx;
      zoom_y = oldy*ht - pointery;
    }
  }
  oldzoom = zoom;

  // TODO: replace this with center on top of selected/developed image
  if(center)
  {
    if(mouse_over_id >= 0)
    {
      zoom_x = wd*((int)(zoom_x)/(int)wd);
      zoom_y = ht*((int)(zoom_y)/(int)ht);
    }
    else zoom_x = zoom_y = 0.0;
    center = 0;
  }

  // mouse left the area, but we leave mouse over as it was, especially during panning
  // if(!pan && pointerx > 0 && pointerx < width && pointery > 0 && pointery < height) DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, -1);
  if(!pan && zoom != 1) DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, -1);

  // set scrollbar positions, clamp zoom positions

  if(lib->collection_count == 0)
  {
    zoom_x = zoom_y = 0.0f;
  }
  else if(zoom < 1.01)
  {
    if(zoom_x < 0)                         zoom_x = 0;
    if(zoom_x > wd*DT_LIBRARY_MAX_ZOOM-wd) zoom_x = wd*DT_LIBRARY_MAX_ZOOM-wd;
    if(zoom_y < 0)                         zoom_y = 0;
    if(zoom_y > ht*lib->collection_count/MIN(DT_LIBRARY_MAX_ZOOM, zoom)-ht)
      zoom_y =  ht*lib->collection_count/MIN(DT_LIBRARY_MAX_ZOOM, zoom)-ht;
  }
  else
  {
    if(zoom_x < -wd*DT_LIBRARY_MAX_ZOOM/2)  zoom_x = -wd*DT_LIBRARY_MAX_ZOOM/2;
    if(zoom_x >  wd*DT_LIBRARY_MAX_ZOOM-wd) zoom_x =  wd*DT_LIBRARY_MAX_ZOOM-wd;
    if(zoom_y < -height+ht)                 zoom_y = -height+ht;
    if(zoom_y >  ht*lib->collection_count/MIN(DT_LIBRARY_MAX_ZOOM, zoom)-ht)
      zoom_y =  ht*lib->collection_count/MIN(DT_LIBRARY_MAX_ZOOM, zoom)-ht;
  }


  int offset_i = (int)(zoom_x/wd);
  int offset_j = (int)(zoom_y/ht);
  if(lib->first_visible_filemanager >= 0)
  {
    offset_i = lib->first_visible_filemanager % DT_LIBRARY_MAX_ZOOM;
    offset_j = lib->first_visible_filemanager / DT_LIBRARY_MAX_ZOOM;
  }
  lib->first_visible_filemanager = -1;
  lib->first_visible_zoomable = offset_i + DT_LIBRARY_MAX_ZOOM*offset_j;
  // arbitrary 1000 to avoid bug due to round towards zero using (int)
  int seli = zoom == 1 ? 0 : ((int)(1000 + (pointerx + zoom_x)/wd) - MAX(offset_i, 0) - 1000);
  int selj = zoom == 1 ? 0 : ((int)(1000 + (pointery + zoom_y)/ht) - offset_j         - 1000);
  float offset_x = (zoom == 1) ? 0.0 : (zoom_x/wd - (int)(zoom_x/wd));
  float offset_y = (zoom == 1) ? 0.0 : (zoom_y/ht - (int)(zoom_y/ht));
  const int max_rows = (zoom == 1) ? 1 : (2 + (int)((height)/ht + .5));
  const int max_cols = (zoom == 1) ? 1 : (MIN(DT_LIBRARY_MAX_ZOOM - MAX(0, offset_i), 1 + (int)(zoom+.5)));

  int offset = MAX(0, offset_i) + DT_LIBRARY_MAX_ZOOM*offset_j;
  int img_pointerx = zoom == 1 ? pointerx : fmodf(pointerx + zoom_x, wd);
  int img_pointery = zoom == 1 ? pointery : fmodf(pointery + zoom_y, ht);

  // assure 1:1 is not switching images on resize/tab events:
  if(!track && lib->offset != 0x7fffffff && zoom == 1)
  {
    offset = lib->offset;
    zoom_x = wd*(offset % DT_LIBRARY_MAX_ZOOM);
    zoom_y = ht*(offset / DT_LIBRARY_MAX_ZOOM);
  }
  else lib->offset = offset;

  int id, clicked1, last_seli = 1<<30, last_selj = 1<<30;
  clicked1 = (oldpan == 0 && pan == 1 && lib->button == 1);

  dt_view_set_scrollbar(self, MAX(0, offset_i), DT_LIBRARY_MAX_ZOOM, zoom, DT_LIBRARY_MAX_ZOOM*offset_j,
                        lib->collection_count, DT_LIBRARY_MAX_ZOOM*max_cols);

  cairo_translate(cr, -offset_x*wd, -offset_y*ht);
  cairo_translate(cr, -MIN(offset_i*wd, 0.0), 0.0);

  for(int row = 0; row < max_rows; row++)
  {
    if(offset < 0)
    {
      cairo_translate(cr, 0, ht);
      offset += DT_LIBRARY_MAX_ZOOM;
      continue;
    }

    /* clear and reset main query */
    DT_DEBUG_SQLITE3_CLEAR_BINDINGS(lib->statements.main_query);
    DT_DEBUG_SQLITE3_RESET(lib->statements.main_query);

    DT_DEBUG_SQLITE3_BIND_INT(lib->statements.main_query, 1, offset);
    DT_DEBUG_SQLITE3_BIND_INT(lib->statements.main_query, 2, max_cols);
    for(int col = 0; col < max_cols; col++)
    {
      if(sqlite3_step(lib->statements.main_query) == SQLITE_ROW)
      {
        id = sqlite3_column_int(lib->statements.main_query, 0);

        // set mouse over id
        if((zoom == 1 && mouse_over_id < 0) || ((!pan || track) && seli == col && selj == row))
        {
          mouse_over_id = id;
          DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, mouse_over_id);
        }
        // add clicked image to selected table
        if(clicked1)
        {
          if((lib->modifiers & GDK_SHIFT_MASK) == 0 && (lib->modifiers & GDK_CONTROL_MASK) == 0 && seli == col && selj == row)
          {
            /* clear selection except id */

            /* clear and resest statement */
            DT_DEBUG_SQLITE3_CLEAR_BINDINGS(lib->statements.delete_except_arg);
            DT_DEBUG_SQLITE3_RESET(lib->statements.delete_except_arg);

            /* reuse statment */
            DT_DEBUG_SQLITE3_BIND_INT(lib->statements.delete_except_arg, 1, id);
            sqlite3_step(lib->statements.delete_except_arg);
          }
          // FIXME: whatever comes first assumption is broken!
          // if((lib->modifiers & GDK_SHIFT_MASK) && (last_seli == (1<<30)) &&
          //    (image->id == lib->last_selected_id || image->id == mouse_over_id)) { last_seli = col; last_selj = row; }
          // if(last_seli < (1<<30) && ((lib->modifiers & GDK_SHIFT_MASK) && (col >= MIN(last_seli,seli) && row >= MIN(last_selj,selj) &&
          //         col <= MAX(last_seli,seli) && row <= MAX(last_selj,selj)) && (col != last_seli || row != last_selj)) ||
          if((lib->modifiers & GDK_SHIFT_MASK) && id == lib->last_selected_idx)
          {
            last_seli = col;
            last_selj = row;
          }
          if((last_seli < (1<<30) && ((lib->modifiers & GDK_SHIFT_MASK) && (col >= last_seli && row >= last_selj &&
                                      col <= seli && row <= selj) && (col != last_seli || row != last_selj))) ||
              (seli == col && selj == row))
          {
            // insert all in range if shift, or only the one the mouse is over for ctrl or plain click.
            dt_view_toggle_selection(id);
            lib->last_selected_idx = id;
          }
        }
        cairo_save(cr);
        // if(zoom == 1) dt_image_prefetch(image, DT_IMAGE_MIPF);
        dt_view_image_expose(&(lib->image_over), id, cr, wd, zoom == 1 ? height : ht, zoom, img_pointerx, img_pointery, FALSE);
        cairo_restore(cr);
      }
      else goto failure;
      cairo_translate(cr, wd, 0.0f);
    }
    cairo_translate(cr, -max_cols*wd, ht);
    offset += DT_LIBRARY_MAX_ZOOM;
  }
failure:

  oldpan = pan;
  lib->zoom_x = zoom_x;
  lib->zoom_y = zoom_y;
  lib->track  = 0;
  lib->center = center;
  if(darktable.unmuted & DT_DEBUG_CACHE)
    dt_mipmap_cache_print(darktable.mipmap_cache);
}

/**
 * Displays a full screen preview of the image currently under the mouse pointer.
 */
void expose_full_preview(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
    dt_library_t *lib = (dt_library_t *)self->data;
    int offset = 0;
    if(lib->track >  2) offset++;
    if(lib->track < -2) offset--;
    lib->track = 0;

    if (offset) {
      /* If more than one image is selected, iterate over these. */
      /* If only one image is selected, scroll through all known images. */

      int sel_img_count = 0;
      {
        sqlite3_stmt *stmt;
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select COUNT(*) from selected_images", -1, &stmt, NULL);
        if (sqlite3_step(stmt) == SQLITE_ROW)
        {
          sel_img_count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
      }

      const dt_image_t *img = dt_image_cache_read_get(darktable.image_cache, lib->full_preview_id);

      /* Build outer select criteria */
      gchar *filter_criteria = g_strdup_printf(
            "inner join images on s1.id=images.id WHERE ((images.filename = \"%s\") and (images.id %s %d)) or (images.filename %s \"%s\") ORDER BY images.filename %s, images.id %s LIMIT 1",
            img->filename,
            (offset > 0) ? ">" : "<",
            lib->full_preview_id,
            (offset > 0) ? ">" : "<",
            img->filename,
            (offset > 0) ? "" : "DESC",
            (offset > 0) ? "" : "DESC");

      dt_image_cache_read_release(darktable.image_cache, img);

      sqlite3_stmt *stmt;
      gchar *stmt_string = NULL;
      if (sel_img_count > 1)
      {
        stmt_string = g_strdup_printf(
            "select images.id as id from (select imgid as id from selected_images) as s1 %s",
            filter_criteria);

        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), stmt_string, -1, &stmt, NULL);
      }
      else
      {
        /* We need to augment the current main query a bit to fetch the
         * row we need. */
        const char *main_query = sqlite3_sql(lib->statements.main_query);
        stmt_string = g_strdup_printf(
                "select images.id as id from (%s) as s1 %s",
                main_query, filter_criteria);

        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), stmt_string, -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, 0);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, -1);

      }
      g_free(stmt_string);
      g_free(filter_criteria);

      if(sqlite3_step(stmt) == SQLITE_ROW) {
        lib->full_preview_id = sqlite3_column_int(stmt, 0);
      }

      sqlite3_finalize(stmt);
    }

    lib->image_over = DT_VIEW_DESERT;
    cairo_set_source_rgb (cr, .1, .1, .1);
    cairo_paint(cr);
    dt_view_image_expose(&(lib->image_over), lib->full_preview_id, cr, width, height, 1, pointerx, pointery, TRUE);
}

void expose(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  const double start = dt_get_wtime();

  // Let's show full preview if in that state...
  dt_library_t *lib = (dt_library_t *)self->data;
  
  /* TODO: instead of doing a check here, the call to switch_layout_to
     should be done in the place where the layout was actually changed. */
  const int new_layout = dt_conf_get_int("plugins/lighttable/layout");
  if (lib->layout != new_layout) switch_layout_to(lib, new_layout);
    
  if( lib->full_preview_id!=-1 )
  {
    expose_full_preview(self, cr, width, height, pointerx, pointery);
  }
  else // we do pass on expose to manager or zoomable
  {
    switch(new_layout)
    {
      case 1: // file manager
        expose_filemanager(self, cr, width, height, pointerx, pointery);
        break;
      default: // zoomable
        expose_zoomable(self, cr, width, height, pointerx, pointery);
        break;
    }
  }
  const double end = dt_get_wtime();
  if (darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_LIGHTTABLE, "[lighttable] expose took %0.04f sec\n", end-start);
}

static gboolean
go_up_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                         guint keyval, GdkModifierType modifier, gpointer data)
{
  const int layout = dt_conf_get_int("plugins/lighttable/layout");
  dt_view_t *self = (dt_view_t *)data;
  dt_library_t *lib = (dt_library_t *)self->data;

  if (layout == 1)
    move_view(lib, TOP);
  else
    lib->offset = 0;
  dt_control_queue_redraw_center();
  return TRUE;
}

static gboolean
go_down_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                           guint keyval, GdkModifierType modifier, gpointer data)
{
  const int layout = dt_conf_get_int("plugins/lighttable/layout");
  dt_view_t *self = (dt_view_t *)data;
  dt_library_t *lib = (dt_library_t *)self->data;
  
  if (layout == 1)
    move_view(lib, BOTTOM);
  else
     lib->offset = 0x1fffffff;
  dt_control_queue_redraw_center();
  return TRUE;
}

static gboolean
go_pgup_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                           guint keyval, GdkModifierType modifier,
                           gpointer data)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_library_t *lib = (dt_library_t *)self->data;
  const int layout = dt_conf_get_int("plugins/lighttable/layout");
  if (layout == 1)
    move_view(lib, PGUP);
  else
  {
    const int iir = dt_conf_get_int("plugins/lighttable/images_in_row");
    const int scroll_by_rows = 4; /* This should be the number of visible rows. */
    const int offset_delta = scroll_by_rows * iir;
    lib->offset = MAX(lib->offset - offset_delta, 0);
  }
  dt_control_queue_redraw_center();
  return TRUE;
}

static gboolean
go_pgdown_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                             guint keyval, GdkModifierType modifier,
                             gpointer data)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_library_t *lib = (dt_library_t *)self->data;
  const int layout = dt_conf_get_int("plugins/lighttable/layout");
  if (layout == 1)
  {
    move_view(lib, PGDOWN);
  }
  else
  {
    const int iir = dt_conf_get_int("plugins/lighttable/images_in_row");
    const int scroll_by_rows = 4; /* This should be the number of visible rows. */
    const int offset_delta = scroll_by_rows * iir;
    lib->offset = MIN(lib->offset + offset_delta, lib->collection_count);
  }
  dt_control_queue_redraw_center();
  return TRUE;
}

static gboolean
star_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                        guint keyval, GdkModifierType modifier, gpointer data)
{
  long int num = (long int)data;
  switch (num)
  {
    case DT_VIEW_REJECT:
    case DT_VIEW_DESERT:
    case DT_VIEW_STAR_1:
    case DT_VIEW_STAR_2:
    case DT_VIEW_STAR_3:
    case DT_VIEW_STAR_4:
    case DT_VIEW_STAR_5:
    case 666:
    {
      int32_t mouse_over_id;
      DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
      if(mouse_over_id <= 0)
      {
        sqlite3_stmt *stmt;
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select imgid from selected_images", -1, &stmt, NULL);
        while(sqlite3_step(stmt) == SQLITE_ROW)
        {
          const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, sqlite3_column_int(stmt, 0));
          dt_image_t *image = dt_image_cache_write_get(darktable.image_cache, cimg);
          if(num == 666 || num == DT_VIEW_DESERT) image->flags &= ~0xf;
          else if(num == DT_VIEW_STAR_1 && ((image->flags & 0x7) == 1)) image->flags &= ~0x7;
          else
          {
            image->flags &= ~0x7;
            image->flags |= num;
          }
          dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_SAFE);
          dt_image_cache_read_release(darktable.image_cache, cimg);
        }
        sqlite3_finalize(stmt);
      }
      else
      {
        const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, mouse_over_id);
        dt_image_t *image = dt_image_cache_write_get(darktable.image_cache, cimg);
        if(num == 666 || num == DT_VIEW_DESERT) image->flags &= ~0xf;
        else if(num == DT_VIEW_STAR_1 && ((image->flags & 0x7) == 1)) image->flags &= ~0x7;
        else
        {
          image->flags &= ~0x7;
          image->flags |= num;
        }
        dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_SAFE);
        dt_image_cache_read_release(darktable.image_cache, cimg);
      }
      dt_control_queue_redraw_center();
      break;
    }
    default:
      break;
  }
  return TRUE;
}

static void _lighttable_mipamps_updated_signal_callback(gpointer instance, gpointer user_data)
{
  dt_control_queue_redraw_center();
}

static void
drag_and_drop_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y, GtkSelectionData *selection_data,
                       guint target_type, guint time, gpointer data)
{
  gboolean success = FALSE;

  if((selection_data != NULL) && (selection_data->length >= 0))
  {
    gchar **uri_list = g_strsplit_set((gchar*)selection_data->data, "\r\n", 0);
    if(uri_list)
    {
      gchar **image_to_load = uri_list;
      while(*image_to_load)
      {
        dt_load_from_string(*image_to_load, FALSE); // TODO: do we want to open the image in darkroom mode? If yes -> set to TRUE.
        image_to_load++;
      }
    }
    g_strfreev(uri_list);
    success = TRUE;
  }
  gtk_drag_finish(context, success, FALSE, time);
}

void enter(dt_view_t *self)
{
  // init drag&drop of files/folders
  gtk_drag_dest_set(dt_ui_center(darktable.gui->ui), GTK_DEST_DEFAULT_ALL, target_list, n_targets, GDK_ACTION_COPY);
  g_signal_connect(dt_ui_center(darktable.gui->ui), "drag-data-received", G_CALLBACK(drag_and_drop_received), self);

  /* connect to signals */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_MIPMAP_UPDATED,
                            G_CALLBACK(_lighttable_mipamps_updated_signal_callback),
                            (gpointer)self);

  // clear some state variables
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->button = 0;
  lib->pan = 0;
}

void dt_lib_remove_child(GtkWidget *widget, gpointer data)
{
  gtk_container_remove(GTK_CONTAINER(data), widget);
}

void leave(dt_view_t *self)
{
  gtk_drag_dest_unset(dt_ui_center(darktable.gui->ui));

  /* disconnect from signals */
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_lighttable_mipamps_updated_signal_callback), (gpointer)self);

  // clear some state variables
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->button = 0;
  lib->pan = 0;
}

void reset(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->center = 1;
  lib->track = lib->pan = 0;
  lib->offset = 0x7fffffff;
  lib->first_visible_zoomable    = -1;
  lib->first_visible_filemanager = 0;
  DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, -1);
}


void mouse_enter(dt_view_t *self)
{
}

void mouse_leave(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  if(!lib->pan && dt_conf_get_int("plugins/lighttable/images_in_row") != 1)
  {
    DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, -1);
    dt_control_queue_redraw_center();
  }
}



int scrolled(dt_view_t *self, double x, double y, int up, int state)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  const int layout = dt_conf_get_int("plugins/lighttable/layout");
  if(layout == 1 && state == 0)
  {
    if(up) move_view(lib, UP);
    else   move_view(lib, DOWN);;
  }
  else
  {
    int zoom = dt_conf_get_int("plugins/lighttable/images_in_row");
    if(up)
    {
      zoom--;
      if(zoom < 1)
        zoom = 1;
      else if (layout == 1)
       zoom_around_image(lib, x, y, self->width, self->height, zoom+1, zoom);
    }
    else
    {
      zoom++;
      if(zoom > 2*DT_LIBRARY_MAX_ZOOM)
        zoom = 2*DT_LIBRARY_MAX_ZOOM;
      else if (layout == 1)
        zoom_around_image(lib, x, y, self->width, self->height, zoom-1, zoom);
    }
    dt_view_lighttable_set_zoom(darktable.view_manager, zoom);
  }
  return 0;
}


void mouse_moved(dt_view_t *self, double x, double y, int which)
{
  dt_control_queue_redraw_center();
}


int button_released(dt_view_t *self, double x, double y, int which, uint32_t state)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->pan = 0;
  if(which == 1) dt_control_change_cursor(GDK_LEFT_PTR);
  return 1;
}


int button_pressed(dt_view_t *self, double x, double y, int which, int type, uint32_t state)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->modifiers = state;
  lib->button = which;
  lib->select_offset_x = lib->zoom_x;
  lib->select_offset_y = lib->zoom_y;
  lib->select_offset_x += x;
  lib->select_offset_y += y;
  lib->pan = 1;
  if(which == 1) dt_control_change_cursor(GDK_HAND1);
  if(which == 1 && type == GDK_2BUTTON_PRESS) return 0;
  // image button pressed?
  if(which == 1)
  {
    switch(lib->image_over)
    {
      case DT_VIEW_DESERT:
        break;
      case DT_VIEW_REJECT:
      case DT_VIEW_STAR_1:
      case DT_VIEW_STAR_2:
      case DT_VIEW_STAR_3:
      case DT_VIEW_STAR_4:
      case DT_VIEW_STAR_5:
      {
        int32_t mouse_over_id;
        DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
        const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, mouse_over_id);
        dt_image_t *image = dt_image_cache_write_get(darktable.image_cache, cimg);
        if(image)
        {
          if(lib->image_over == DT_VIEW_STAR_1 && ((image->flags & 0x7) == 1)) image->flags &= ~0x7;
          else if(lib->image_over == DT_VIEW_REJECT && ((image->flags & 0x7) == 6)) image->flags &= ~0x7;
          else
          {
            image->flags &= ~0x7;
            image->flags |= lib->image_over;
          }
          dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_SAFE);
        }
        dt_image_cache_read_release(darktable.image_cache, image);
        break;
      }
      case DT_VIEW_GROUP:
      {
        int32_t mouse_over_id;
        DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
        const dt_image_t *image = dt_image_cache_read_get(darktable.image_cache, mouse_over_id);
        if(!image) return 0;
        int group_id = image->group_id;
        int id = image->id;
        dt_image_cache_read_release(darktable.image_cache, image);
        if(state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)) // just add the whole group to the selection. TODO: make this also work for collapsed groups.
        {
          sqlite3_stmt *stmt;
          DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "insert or ignore into selected_images select id from images where group_id = ?1", -1, &stmt, NULL);
          DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, group_id);
          sqlite3_step(stmt);
          sqlite3_finalize(stmt);
        }
        else if(group_id == darktable.gui->expanded_group_id) // the group is already expanded, so ...
        {
          if(id == darktable.gui->expanded_group_id) // ... collapse it
            darktable.gui->expanded_group_id = -1;
          else                                       // ... make the image the new representative of the group
            darktable.gui->expanded_group_id = dt_grouping_change_representative(id);
        }
        else // expand the group
          darktable.gui->expanded_group_id = group_id;
        dt_collection_update_query(darktable.collection);
        break;
      }
      default:
        return 0;
    }
  }
  return 1;
}

int key_released(dt_view_t *self, guint key, guint state)
{
  dt_control_accels_t *accels = &darktable.control->accels;
  dt_library_t *lib = (dt_library_t *)self->data;

  if(!darktable.control->key_accelerators_on)
    return 0;

  if(key == accels->lighttable_preview.accel_key
      && state == accels->lighttable_preview.accel_mods && lib->full_preview_id !=-1)
  {

    lib->full_preview_id = -1;

    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_LEFT,   ( lib->full_preview & 1));
    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_RIGHT,  ( lib->full_preview & 2));
    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_CENTER_BOTTOM, ( lib->full_preview & 4));
    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_CENTER_TOP,    ( lib->full_preview & 8));

    lib->full_preview = 0;
  }

  return 1;
}

int key_pressed(dt_view_t *self, guint key, guint state)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  dt_control_accels_t *accels = &darktable.control->accels;

  if(!darktable.control->key_accelerators_on)
    return 0;

  int zoom = dt_conf_get_int("plugins/lighttable/images_in_row");

  const int layout = dt_conf_get_int("plugins/lighttable/layout");

  if(key == accels->lighttable_preview.accel_key
      && state == accels->lighttable_preview.accel_mods)
  {
    int32_t mouse_over_id;
    DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
    if(lib->full_preview_id == -1 && mouse_over_id != -1 )
    {
      // encode panel visibility into full_preview
      lib->full_preview = 0;
      lib->full_preview_id = mouse_over_id;

      // let's hide some gui components
      lib->full_preview |= (dt_ui_panel_visible(darktable.gui->ui, DT_UI_PANEL_LEFT)&1) << 0;
      dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_LEFT, FALSE);
      lib->full_preview |= (dt_ui_panel_visible(darktable.gui->ui, DT_UI_PANEL_RIGHT)&1) << 1;
      dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_RIGHT, FALSE);
      lib->full_preview |= (dt_ui_panel_visible(darktable.gui->ui, DT_UI_PANEL_CENTER_BOTTOM)&1) << 2;
      dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_CENTER_BOTTOM, FALSE);
      lib->full_preview |= (dt_ui_panel_visible(darktable.gui->ui, DT_UI_PANEL_CENTER_TOP)&1) << 3;
      dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_CENTER_TOP, FALSE);

      //dt_dev_invalidate(darktable.develop);
    }
    return 1;
  }

  if(key == accels->lighttable_left.accel_key
      && state == accels->lighttable_left.accel_mods)
  {
    if(layout == 1 && zoom == 1) move_view(lib, UP);
    else lib->track = -1;
    return 1;
  }

  if(key == accels->lighttable_right.accel_key
      && state == accels->lighttable_right.accel_mods)
  {
    if(layout == 1 && zoom == 1) move_view(lib, DOWN);
    else lib->track = 1;
    return 1;
  }

  if(key == accels->lighttable_up.accel_key
      && state == accels->lighttable_up.accel_mods)
  {
    if(layout == 1) move_view(lib, UP);
    else lib->track = -DT_LIBRARY_MAX_ZOOM;
    return 1;
  }

  if(key == accels->lighttable_down.accel_key
      && state == accels->lighttable_down.accel_mods)
  {
    if(layout == 1) move_view(lib, DOWN);
    else lib->track = DT_LIBRARY_MAX_ZOOM;
    return 1;
  }

  if(key == accels->lighttable_center.accel_key
      && state == accels->lighttable_center.accel_mods)
  {
    lib->center = 1;
    return 1;
  }

  return 0;
}

void border_scrolled(dt_view_t *view, double x, double y, int which, int up)
{
  dt_library_t *lib = (dt_library_t *)view->data;
  int layout = lib->layout;
  if (layout == 1)
  {
    move_view(lib, which);
  }
  else
  {
    if(which == 0 || which == 1)
    {
      if(up) lib->track = -DT_LIBRARY_MAX_ZOOM;
      else   lib->track =  DT_LIBRARY_MAX_ZOOM;
    }
    else if(which == 2 || which == 3)
    {
      if(up) lib->track = -1;
      else   lib->track =  1;
    }
  }
  
  dt_control_queue_redraw();
}

void init_key_accels(dt_view_t *self)
{
  // Initializing accelerators

  // Rating keys
  dt_accel_register_view(self, NC_("accel", "rate desert"), GDK_0, 0);
  dt_accel_register_view(self, NC_("accel", "rate 1"), GDK_1, 0);
  dt_accel_register_view(self, NC_("accel", "rate 2"), GDK_2, 0);
  dt_accel_register_view(self, NC_("accel", "rate 3"), GDK_3, 0);
  dt_accel_register_view(self, NC_("accel", "rate 4"), GDK_4, 0);
  dt_accel_register_view(self, NC_("accel", "rate 5"), GDK_5, 0);
  dt_accel_register_view(self, NC_("accel", "rate reject"), GDK_r, 0);

  // Navigation keys
  dt_accel_register_view(self, NC_("accel", "navigate up"),
                         GDK_g, 0);
  dt_accel_register_view(self, NC_("accel", "navigate down"),
                         GDK_g, GDK_SHIFT_MASK);
  dt_accel_register_view(self, NC_("accel", "navigate page up"),
                         GDK_Page_Up, 0);
  dt_accel_register_view(self, NC_("accel", "navigate page down"),
                         GDK_Page_Down, 0);

  // Color keys
  dt_accel_register_view(self, NC_("accel", "color red"), GDK_F1, 0);
  dt_accel_register_view(self, NC_("accel", "color yellow"), GDK_F2, 0);
  dt_accel_register_view(self, NC_("accel", "color green"), GDK_F3, 0);
  dt_accel_register_view(self, NC_("accel", "color blue"), GDK_F4, 0);
  dt_accel_register_view(self, NC_("accel", "color purple"), GDK_F5, 0);

  // Scroll keys
  dt_accel_register_view(self, NC_("accel", "scroll up"),
                         GDK_Up, 0);
  dt_accel_register_view(self, NC_("accel", "scroll down"),
                         GDK_Down, 0);
  dt_accel_register_view(self, NC_("accel", "scroll left"),
                         GDK_Left, 0);
  dt_accel_register_view(self, NC_("accel", "scroll right"),
                         GDK_Right, 0);
  dt_accel_register_view(self, NC_("accel", "scroll center"),
                         GDK_apostrophe, 0);

  // Preview key
  dt_accel_register_view(self, NC_("accel", "preview"), GDK_z, 0);
}

void connect_key_accels(dt_view_t *self)
{
  GClosure *closure;

  // Rating keys
  closure = g_cclosure_new(
              G_CALLBACK(star_key_accel_callback),
              (gpointer)DT_VIEW_DESERT, NULL);
  dt_accel_connect_view(self, "rate desert", closure);
  closure = g_cclosure_new(
              G_CALLBACK(star_key_accel_callback),
              (gpointer)DT_VIEW_STAR_1, NULL);
  dt_accel_connect_view(self, "rate 1", closure);
  closure = g_cclosure_new(
              G_CALLBACK(star_key_accel_callback),
              (gpointer)DT_VIEW_STAR_2, NULL);
  dt_accel_connect_view(self, "rate 2", closure);
  closure = g_cclosure_new(
              G_CALLBACK(star_key_accel_callback),
              (gpointer)DT_VIEW_STAR_3, NULL);
  dt_accel_connect_view(self, "rate 3", closure);
  closure = g_cclosure_new(
              G_CALLBACK(star_key_accel_callback),
              (gpointer)DT_VIEW_STAR_4, NULL);
  dt_accel_connect_view(self, "rate 4", closure);
  closure = g_cclosure_new(
              G_CALLBACK(star_key_accel_callback),
              (gpointer)DT_VIEW_STAR_5, NULL);
  dt_accel_connect_view(self, "rate 5", closure);
  closure = g_cclosure_new(
              G_CALLBACK(star_key_accel_callback),
              (gpointer)DT_VIEW_REJECT, NULL);
  dt_accel_connect_view(self, "rate reject", closure);

  // Navigation keys
  closure = g_cclosure_new(
              G_CALLBACK(go_up_key_accel_callback),
              (gpointer)self, NULL);
  dt_accel_connect_view(self, "navigate up", closure);
  closure = g_cclosure_new(
              G_CALLBACK(go_down_key_accel_callback),
              (gpointer)self, NULL);
  dt_accel_connect_view(self, "navigate down", closure);
  closure = g_cclosure_new(
              G_CALLBACK(go_pgup_key_accel_callback),
              (gpointer)self, NULL);
  dt_accel_connect_view(self, "navigate page up", closure);
  closure = g_cclosure_new(
              G_CALLBACK(go_pgdown_key_accel_callback),
              (gpointer)self, NULL);
  dt_accel_connect_view(self, "navigate page down", closure);

  // Color keys
  closure = g_cclosure_new(G_CALLBACK(dt_colorlabels_key_accel_callback),
                           (gpointer)0, NULL);
  dt_accel_connect_view(self, "color red", closure);
  closure = g_cclosure_new(G_CALLBACK(dt_colorlabels_key_accel_callback),
                           (gpointer)1, NULL);
  dt_accel_connect_view(self, "color yellow", closure);
  closure = g_cclosure_new(G_CALLBACK(dt_colorlabels_key_accel_callback),
                           (gpointer)2, NULL);
  dt_accel_connect_view(self, "color green", closure);
  closure = g_cclosure_new(G_CALLBACK(dt_colorlabels_key_accel_callback),
                           (gpointer)3, NULL);
  dt_accel_connect_view(self, "color blue", closure);
  closure = g_cclosure_new(G_CALLBACK(dt_colorlabels_key_accel_callback),
                           (gpointer)4, NULL);
  dt_accel_connect_view(self, "color purple", closure);

}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
