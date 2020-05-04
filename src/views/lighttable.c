/*
    This file is part of darktable,
    Copyright (C) 2009-2020 darktable developers.
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
#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/colorlabels.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/file_location.h"
#include "common/focus.h"
#include "common/grouping.h"
#include "common/history.h"
#include "common/image_cache.h"
#include "common/ratings.h"
#include "common/selection.h"
#include "common/undo.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "control/settings.h"
#include "dtgtk/button.h"
#include "dtgtk/thumbtable.h"
#include "gui/accelerators.h"
#include "gui/drag_and_drop.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "views/view.h"
#include "views/view_api.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <gdk/gdkkeysyms.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

DT_MODULE(1)

#define FULL_PREVIEW_IN_MEMORY_LIMIT 9

/* returns TRUE if lighttable is using the custom order filter */
static gboolean _is_custom_image_order_actif(const dt_view_t *self);

static void _force_expose_all(dt_view_t *self);

static void _preview_enter(dt_view_t *self, gboolean sticky, gboolean focus, int32_t mouse_over_id);
static void _preview_quit(dt_view_t *self);

static gboolean _culling_recreate_slots_at(dt_view_t *self, const int display_first_image);

typedef struct dt_preview_surface_t
{
  int mip;
  int32_t imgid;
  int32_t width;
  int32_t height;
  cairo_surface_t *surface;
  uint8_t *rgbbuf;
  int w_lock;

  float w_fit;
  float h_fit;
  float zoom_100;

  float zoom_delta;
  float dx_delta;
  float dy_delta;

  float max_dx;
  float max_dy;
} dt_preview_surface_t;

typedef struct dt_layout_image_t
{
  gint imgid;
  gint width, height, x, y;
  double aspect_ratio;
} dt_layout_image_t;


/**
 * this organises the whole library:
 * previously imported film rolls..
 */
typedef struct dt_library_t
{
  // tmp mouse vars:
  float pan_x, pan_y;
  uint32_t modifiers;
  uint32_t pan;
  dt_view_image_over_t activate_on_release;
  int32_t track;
  dt_view_image_over_t image_over;
  int full_preview_sticky;
  int32_t full_preview_id;
  int32_t full_preview_rowid;
  gboolean full_preview_follow_sel; // should the selection follow the active image ?
  gboolean full_preview_inside_sel; // do we navigate only in side the selection ?
  int display_focus;
  int images_in_row;
  dt_lighttable_layout_t current_layout;

  GHashTable *thumbs_table;

  uint8_t *full_res_thumb;
  int32_t full_res_thumb_id, full_res_thumb_wd, full_res_thumb_ht;
  dt_image_orientation_t full_res_thumb_orientation;
  dt_focus_cluster_t full_res_focus[49];

  int32_t collection_count;

  // stuff for the audio player
  GPid audio_player_pid;   // the pid of the child process
  int32_t audio_player_id; // the imgid of the image the audio is played for
  guint audio_player_event_source;

  // zoom in image preview (full)
  int missing_thumbnails;
  float full_zoom;
  float full_x;
  float full_y;
  dt_preview_surface_t fp_surf[FULL_PREVIEW_IN_MEMORY_LIMIT];
  dt_layout_image_t *slots, *slots_old;
  int slots_count, slots_count_old;
  gboolean slots_changed;
  dt_layout_image_t culling_previous, culling_next;
  gboolean culling_use_selection;
  gboolean culling_follow_selection;
  gboolean already_started;
  gboolean select_deactivate;
  int last_num_images, last_width, last_height;

  /* prepared and reusable statements */
  struct
  {
    /* main query statement, should be update on listener signal of collection */
    sqlite3_stmt *main_query;
    /* select imgid from selected_images */
    sqlite3_stmt *select_imgid_in_selection;
    /* delete from selected_images where imgid != ?1 */
    sqlite3_stmt *delete_except_arg;
    /* check if the group of the image under the mouse has others, too, ?1: group_id, ?2: imgid */
    sqlite3_stmt *is_grouped;
  } statements;

  GtkWidget *profile_floating_window;

  int thumbtable_offset;
} dt_library_t;

static inline float absmul(float a, float b) {
  return a > b ? a/b : b/a;
}


static void _stop_audio(dt_library_t *lib);

const char *name(const dt_view_t *self)
{
  return _("lighttable");
}


uint32_t view(const dt_view_t *self)
{
  return DT_VIEW_LIGHTTABLE;
}

static inline dt_lighttable_layout_t get_layout(void)
{
  return dt_view_lighttable_get_layout(darktable.view_manager);
}

static inline gint get_zoom(void)
{
  return dt_view_lighttable_get_zoom(darktable.view_manager);
}

static void _force_expose_all(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->slots_changed = TRUE;
  dt_control_queue_redraw_center();
}

static void _culling_destroy_slots(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  if(!lib->slots) return;

  free(lib->slots);
  lib->slots = NULL;
  lib->slots_count = 0;
}

static int _culling_get_selection_count()
{
  int nb = 0;
  gchar *query = dt_util_dstrcat(
      NULL,
      "SELECT count(*) FROM main.selected_images AS s, memory.collected_images as m WHERE s.imgid = m.imgid");
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  if(stmt != NULL)
  {
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      nb = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
  }
  g_free(query);

  return nb;
}

// init navigate in selection and follow selection and return first image to display
static int _culling_preview_init_values(dt_view_t *self, gboolean culling, gboolean preview)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  /** HOW it works :
   *
   * For the first image :
   *  image_over OR first selected OR first OR -1
   *
   * For the navigation in selection :
   *  culling dynamic mode                       => OFF
   *  first image in selection AND selection > 1 => ON
   *  otherwise                                  => OFF
   *
   * For the selection following :
   *  culling dynamic mode         => OFF
   *  first image(s) == selection  => ON
   */

  // init values
  if(preview)
  {
    lib->full_preview_follow_sel = FALSE;
    lib->full_preview_inside_sel = FALSE;
  }
  else if(culling)
  {
    lib->full_preview_follow_sel = FALSE;
    lib->full_preview_inside_sel = FALSE;
  }

  // get first id
  sqlite3_stmt *stmt;
  int first_id = -1;

  if(!lib->already_started)
  {
    // first start, we retrieve the registered offset
    const int offset = dt_conf_get_int("plugins/lighttable/recentcollect/pos0");
    gchar *query = dt_util_dstrcat(NULL, "SELECT imgid FROM memory.collected_images WHERE rowid=%d", offset);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      first_id = sqlite3_column_int(stmt, 0);
    }
    g_free(query);
    sqlite3_finalize(stmt);
    lib->already_started = TRUE;
  }
  else
    first_id = dt_control_get_mouse_over_id();

  if(first_id < 1)
  {
    // search the first selected image
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
        "SELECT col.imgid "
          "FROM memory.collected_images AS col, main.selected_images as sel "
          "WHERE col.imgid=sel.imgid "
          "ORDER BY col.rowid "
          "LIMIT 1",
        -1, &stmt, NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW) first_id = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }
  if(first_id < 1)
  {
    // search the first image shown in view (this is the offset of thumbtable)
    first_id = dt_ui_thumbtable(darktable.gui->ui)->offset_imgid;
  }
  if(first_id < 1 || (!culling && !preview))
  {
    // no need to go further
    return first_id;
  }

  // special culling dynamic mode
  if(!preview && culling
     && dt_view_lighttable_get_culling_zoom_mode(darktable.view_manager) == DT_LIGHTTABLE_ZOOM_DYNAMIC)
  {
    lib->culling_use_selection = TRUE;
    return first_id;
  }

  // selection count
  int sel_count = 0;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT count(*) "
                                "FROM memory.collected_images AS col, main.selected_images as sel "
                                "WHERE col.imgid=sel.imgid",
                              -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW) sel_count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  // is first_id inside selection ?
  gboolean inside = FALSE;
  gchar *query = dt_util_dstrcat(NULL,
                                "SELECT col.imgid "
                                  "FROM memory.collected_images AS col, main.selected_images AS sel "
                                  "WHERE col.imgid=sel.imgid AND col.imgid=%d",
                                first_id);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW) inside = TRUE;
  sqlite3_finalize(stmt);
  g_free(query);

  if(preview)
  {
    lib->full_preview_inside_sel = (sel_count > 1 && inside);
    lib->full_preview_follow_sel = (sel_count == 1 && inside);
  }
  else if(culling)
  {
    const int zoom = get_zoom();
    lib->culling_use_selection = (sel_count > zoom && inside);
    if(sel_count <= zoom && inside)
    {
      lib->culling_follow_selection = TRUE;
      // ensure that first_id is the first selected
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
          "SELECT col.imgid "
            "FROM memory.collected_images AS col, main.selected_images as sel "
            "WHERE col.imgid=sel.imgid "
            "ORDER BY col.rowid "
            "LIMIT 1",
          -1, &stmt, NULL);
      if(sqlite3_step(stmt) == SQLITE_ROW) first_id = sqlite3_column_int(stmt, 0);
      sqlite3_finalize(stmt);
    }
  }

  return first_id;
}

static void check_layout(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  const dt_lighttable_layout_t layout = get_layout();
  const dt_lighttable_layout_t layout_old = lib->current_layout;

  if(lib->current_layout == layout) return;
  lib->current_layout = layout;

  // layout has changed, let restore panels
  dt_ui_restore_panels(darktable.gui->ui);

  // make sure we reset culling layout
  _culling_destroy_slots(self);

  if(layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER || layout == DT_LIGHTTABLE_LAYOUT_ZOOMABLE)
  {
    dt_ui_thumbtable(darktable.gui->ui)->navigate_inside_selection = FALSE;

    // if we arrive from culling, we just need to ensure the offset is right
    if(layout_old == DT_LIGHTTABLE_LAYOUT_CULLING)
    {
      dt_thumbtable_set_offset(dt_ui_thumbtable(darktable.gui->ui), lib->thumbtable_offset, FALSE);
    }
    // we want to reacquire the thumbtable if needed
    if(layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
    {
      dt_thumbtable_set_parent(dt_ui_thumbtable(darktable.gui->ui), dt_ui_center_base(darktable.gui->ui),
                               DT_THUMBTABLE_MODE_FILEMANAGER);
    }
    else
    {
      dt_thumbtable_set_parent(dt_ui_thumbtable(darktable.gui->ui), dt_ui_center_base(darktable.gui->ui),
                               DT_THUMBTABLE_MODE_ZOOM);
    }
    dt_thumbtable_full_redraw(dt_ui_thumbtable(darktable.gui->ui), TRUE);
    gtk_widget_show(dt_ui_thumbtable(darktable.gui->ui)->widget);
  }
  else if(layout == DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    // record thumbtable offset
    lib->thumbtable_offset = dt_thumbtable_get_offset(dt_ui_thumbtable(darktable.gui->ui));
    // ensure that thumbtable is not visible in the main view
    gtk_widget_hide(dt_ui_thumbtable(darktable.gui->ui)->widget);
    _culling_recreate_slots_at(self, _culling_preview_init_values(self, TRUE, FALSE));
    dt_ui_thumbtable(darktable.gui->ui)->navigate_inside_selection = lib->culling_use_selection;
  }

  if(layout == DT_LIGHTTABLE_LAYOUT_CULLING || lib->full_preview_id != -1)
  {
    dt_lib_set_visible(darktable.view_manager->proxy.timeline.module, FALSE); // not available in this layouts
    dt_lib_set_visible(darktable.view_manager->proxy.filmstrip.module,
                       TRUE); // always on, visibility is driven by panel state
    dt_ui_scrollbars_show(darktable.gui->ui, FALSE);
  }
  else
  {
    dt_lib_set_visible(darktable.view_manager->proxy.filmstrip.module, FALSE); // not available in this layouts
    dt_lib_set_visible(darktable.view_manager->proxy.timeline.module,
                       TRUE); // always on, visibility is driven by panel state
  }
}

static inline void _destroy_preview_surface(dt_preview_surface_t *fp_surf)
{
  if(fp_surf->surface) cairo_surface_destroy(fp_surf->surface);
  fp_surf->surface = NULL;
  if(fp_surf->rgbbuf) free(fp_surf->rgbbuf);
  fp_surf->rgbbuf = NULL;
  fp_surf->mip = 0;
  fp_surf->width = 0;
  fp_surf->height = 0;
  fp_surf->imgid = -1;
  fp_surf->w_lock = 0;

  fp_surf->zoom_100 = 1001.0f; // dummy value to say it need recompute
  fp_surf->w_fit = 0.0f;
  fp_surf->h_fit = 0.0f;

  fp_surf->zoom_delta = 0.0f;
  fp_surf->dx_delta = 0.0f;
  fp_surf->dy_delta = 0.0f;

  fp_surf->max_dx = 0.0f;
  fp_surf->max_dy = 0.0f;
}

static void _full_preview_destroy(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;

  for(int i = 0; i < FULL_PREVIEW_IN_MEMORY_LIMIT; i++)
  {
    _destroy_preview_surface(lib->fp_surf + i);
  }
}

static void _lighttable_change_offset(dt_view_t *self, gboolean reset, gint imgid)
{
  dt_library_t *lib = (dt_library_t *)self->data;

  if(reset)
  {
    // we cache the collection count
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT COUNT(*) FROM memory.collected_images", -1,
                                &stmt, NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW) lib->collection_count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }

  // full_preview change
  if(lib->full_preview_id > 0)
  {
    if(reset)
    {
      // Full preview id doesn't means anything. Better exit.
      lib->full_preview_follow_sel = FALSE;
      lib->thumbtable_offset = 1;
      _preview_quit(self);
    }
    else
    {
      lib->full_preview_id = imgid;
      sqlite3_stmt *stmt;
      gchar *query = dt_util_dstrcat(NULL, "SELECT rowid FROM memory.collected_images WHERE imgid=%d", imgid);
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
      if(sqlite3_step(stmt) == SQLITE_ROW) lib->full_preview_rowid = sqlite3_column_int(stmt, 0);
      sqlite3_finalize(stmt);
      g_free(query);
      dt_control_queue_redraw_center();
    }
  }

  // culling change (note that full_preview can be combined with culling)
  if(lib->current_layout == DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    _culling_recreate_slots_at(self, imgid);
    dt_control_queue_redraw_center();
  }
  // for filmanager and zoomable, we just test for empty collection
  else if(lib->collection_count == 0)
  {
    dt_control_queue_redraw_center();
  }
}

static gint _list_compare_id(gconstpointer a, gconstpointer b)
{
  const int ida = GPOINTER_TO_INT(a);
  const int idb = GPOINTER_TO_INT(b);
  return (ida != idb);
}

// find the first valid image after (or before) imgid
// this take care of special modes (dynamic, follow_selection, use_selection)
// and ensure that we have enought images to display after this one
static int _culling_find_first_valid_imgid(dt_view_t *self, int imgid)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  sqlite3_stmt *stmt;
  int newid = imgid;

  if(dt_view_lighttable_get_culling_zoom_mode(darktable.view_manager) == DT_LIGHTTABLE_ZOOM_DYNAMIC)
  {
    // on dynamic mode, nb of image follow selection size
    // so we return first image in selection
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
        "SELECT col.imgid "
          "FROM main.selected_images as sel, memory.collected_images as col "
          "WHERE col.imgid=sel.imgid "
          "ORDER BY col.rowid "
          "LIMIT 1",
        -1, &stmt, NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW) newid = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }
  else
  {
    if(lib->culling_use_selection)
    {
      // we search the first still selected (this can be the current one)
      newid = -1;
      gchar *query = dt_util_dstrcat(NULL,
                            "SELECT col.imgid FROM memory.collected_images AS col, main.selected_images AS sel "
                            "WHERE col.imgid=sel.imgid "
                                  "AND col.rowid>=(SELECT rowid FROM memory.collected_images WHERE imgid=%d) "
                            "ORDER BY col.rowid "
                            "LIMIT 1",
                            imgid);
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
      if(sqlite3_step(stmt) == SQLITE_ROW) newid = sqlite3_column_int(stmt, 0);
      sqlite3_finalize(stmt);
      g_free(query);

      // if not found, revert to selection beginning
      if(newid < 0)
      {
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
            "SELECT col.imgid "
              "FROM main.selected_images as sel, memory.collected_images as col "
              "WHERE col.imgid=sel.imgid "
              "ORDER BY col.rowid "
              "LIMIT 1",
            -1, &stmt, NULL);
        if(sqlite3_step(stmt) == SQLITE_ROW) newid = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
      }

      // if still not found, that means that the selection is empty, so we just exit the mode and return imgid
      if(newid < 0)
      {
        lib->culling_use_selection = FALSE;
        newid = imgid;
      }
    }
  }
  return newid;
}

static void _view_lighttable_selection_listener_callback(gpointer instance, gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_library_t *lib = (dt_library_t *)self->data;

  if(lib->select_deactivate) return;

  // we reset the culling layout
  if(lib->current_layout == DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    // on dynamic mode, nb of image follow selection size
    int nbsel = _culling_get_selection_count();
    if(dt_view_lighttable_get_culling_zoom_mode(darktable.view_manager) == DT_LIGHTTABLE_ZOOM_DYNAMIC)
    {
      const int nz = (nbsel <= 1) ? dt_conf_get_int("plugins/lighttable/culling_num_images") : nbsel;
      dt_view_lighttable_set_zoom(darktable.view_manager, nz);
    }
    else if(lib->slots_count > 0)
    {
      int newid = _culling_find_first_valid_imgid(self, lib->slots[0].imgid);
      if(lib->culling_follow_selection)
      {
        // the selection should follow active image !
        // if there's now some differences, quit this mode.
        if(nbsel != g_slist_length(darktable.view_manager->active_images))
          lib->culling_follow_selection = FALSE;
        else
        {
          sqlite3_stmt *stmt;
          DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
              "SELECT col.imgid FROM main.selected_images as sel, memory.collected_images as col "
                "WHERE col.imgid=sel.imgid",
              -1, &stmt, NULL);
          while(sqlite3_step(stmt) == SQLITE_ROW)
          {
            const int id = sqlite3_column_int(stmt, 0);
            if(!g_slist_find_custom(darktable.view_manager->active_images, GINT_TO_POINTER(id), _list_compare_id))
            {
              lib->culling_follow_selection = FALSE;
              break;
            }
          }
          sqlite3_finalize(stmt);
        }
      }
      // we recreate the slots at the right position
      // if it's the same, _culling_recreate will take care to only reload changed images
      _culling_recreate_slots_at(self, newid);
      dt_control_queue_redraw_center();
    }
  }
  else if(lib->full_preview_id != -1)
  {
    // if we navigate inside selection and the current image is outside, reset this param
    // same for follow sel
    if(lib->full_preview_inside_sel || lib->full_preview_follow_sel)
    {
      sqlite3_stmt *stmt;
      gchar *query
          = dt_util_dstrcat(NULL, "SELECT rowid FROM main.selected_images WHERE imgid = %d", lib->full_preview_id);
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
      if(sqlite3_step(stmt) != SQLITE_ROW)
      {
        lib->full_preview_inside_sel = FALSE;
        lib->full_preview_follow_sel = FALSE;
      }
      sqlite3_finalize(stmt);
      g_free(query);
    }
  }
}

static int _get_images_in_row(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  return lib->images_in_row;
}

static int _get_full_preview_id(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  return lib->full_preview_id;
}

static int _culling_is_image_visible(dt_view_t *self, gint imgid)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  if(lib->current_layout != DT_LIGHTTABLE_LAYOUT_CULLING) return FALSE;
  for(int i = 0; i < lib->slots_count; i++)
  {
    if(lib->slots[i].imgid == imgid) return TRUE;
  }
  return FALSE;
}

static inline int _get_max_in_memory_images()
{
  const int max_in_memory_images = dt_conf_get_int("plugins/lighttable/preview/max_in_memory_images");
  return MIN(max_in_memory_images, FULL_PREVIEW_IN_MEMORY_LIMIT);
}

static void _sort_preview_surface(dt_library_t *lib, dt_layout_image_t *images, const int sel_img_count,
                                  const int max_in_memory_images)
{
#define SWAP_PREVIEW_SURFACE(x1, x2)                                                                              \
  {                                                                                                               \
    dt_preview_surface_t surf_tmp = lib->fp_surf[x1];                                                             \
    lib->fp_surf[x1] = lib->fp_surf[x2];                                                                          \
    lib->fp_surf[x2] = surf_tmp;                                                                                  \
  }

  const int in_memory_limit = MIN(max_in_memory_images, FULL_PREVIEW_IN_MEMORY_LIMIT);

  // if nb of images > in_memory_limit, we shouldn't have surfaces created, so nothing to do
  if(sel_img_count > in_memory_limit) return;

  for(int i = 0; i < sel_img_count; i++)
  {
    // we assume that there's only one cache per image
    if(images[i].imgid != lib->fp_surf[i].imgid)
    {
      int j = 0;
      // search the image in cache
      while(j < in_memory_limit && lib->fp_surf[j].imgid != images[i].imgid) j++;
      // found one, swap it
      if(j < in_memory_limit)
        SWAP_PREVIEW_SURFACE(i, j)
      else if(lib->fp_surf[i].imgid >= 0)
      {
        // check if there's an empty entry so we can save this cache
        j = 0;
        while(j < in_memory_limit && lib->fp_surf[j].imgid >= 0) j++;
        // found one, swap it
        if(j < in_memory_limit)
          SWAP_PREVIEW_SURFACE(i, j)
        else
        {
          // cache is full, get rid of the farthest one
          const int offset_current = dt_collection_image_offset(images[i].imgid);
          int offset_max = -1;
          int max_i = -1;
          j = i;
          while(j < in_memory_limit)
          {
            const int offset = dt_collection_image_offset(lib->fp_surf[j].imgid);
            if(abs(offset_current - offset) > offset_max)
            {
              offset_max = abs(offset_current - offset);
              max_i = j;
            }
            j++;
          }
          if(max_i >= 0 && max_i != i) SWAP_PREVIEW_SURFACE(i, max_i)
        }
      }
    }
  }

  // keep only the first max_in_memory_images cache entries
  for(int i = max_in_memory_images; i < FULL_PREVIEW_IN_MEMORY_LIMIT; i++)
  {
    _destroy_preview_surface(lib->fp_surf + i);
  }

#undef SWAP_PREVIEW_SURFACE
}

void init(dt_view_t *self)
{
  self->data = calloc(1, sizeof(dt_library_t));
  dt_library_t *lib = (dt_library_t *)self->data;

  darktable.view_manager->proxy.lighttable.get_images_in_row = _get_images_in_row;
  darktable.view_manager->proxy.lighttable.get_full_preview_id = _get_full_preview_id;
  darktable.view_manager->proxy.lighttable.view = self;
  darktable.view_manager->proxy.lighttable.culling_is_image_visible = _culling_is_image_visible;
  darktable.view_manager->proxy.lighttable.change_offset = _lighttable_change_offset;

  lib->modifiers = 0;
  lib->pan = lib->track = 0;
  lib->activate_on_release = DT_VIEW_ERR;
  lib->full_preview_id = -1;
  lib->display_focus = 0;
  lib->full_res_thumb = 0;
  lib->full_res_thumb_id = -1;
  lib->audio_player_id = -1;

  lib->collection_count = -1;
  lib->missing_thumbnails = 0;
  lib->full_zoom = 1.0f;
  lib->full_x = 0;
  lib->full_y = 0;

  for(int i = 0; i < FULL_PREVIEW_IN_MEMORY_LIMIT; i++)
  {
    lib->fp_surf[i].mip = 0;
    lib->fp_surf[i].imgid = -1;
    lib->fp_surf[i].width = 0;
    lib->fp_surf[i].height = 0;
    lib->fp_surf[i].surface = NULL;
    lib->fp_surf[i].rgbbuf = NULL;
    lib->fp_surf[i].w_lock = 0;
    lib->fp_surf[i].zoom_100 = 40.0f;
    lib->fp_surf[i].w_fit = 0.0f;
    lib->fp_surf[i].h_fit = 0.0f;
  }

  lib->culling_next.imgid = lib->culling_previous.imgid = -1;

  lib->thumbs_table = g_hash_table_new(g_int_hash, g_int_equal);

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_SELECTION_CHANGED,
                            G_CALLBACK(_view_lighttable_selection_listener_callback), (gpointer)self);

  // ensure the memory table is up to date
  dt_collection_memory_update();

  /* initialize reusable sql statements */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM main.selected_images WHERE imgid != ?1",
                              -1, &lib->statements.delete_except_arg, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT id FROM main.images WHERE group_id = ?1 AND id != ?2", -1,
                              &lib->statements.is_grouped, NULL); // TODO: only check in displayed images?
}


void cleanup(dt_view_t *self)
{
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_view_lighttable_selection_listener_callback), self);

  dt_library_t *lib = (dt_library_t *)self->data;
  if(lib->audio_player_id != -1) _stop_audio(lib);
  g_hash_table_destroy(lib->thumbs_table);
  dt_free_align(lib->full_res_thumb);
  free(lib->slots);
  free(self->data);
}

static int expose_empty(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx,
                        int32_t pointery)
{
  const float fs = DT_PIXEL_APPLY_DPI(15.0f);
  const float ls = 1.5f * fs;
  const float offy = height * 0.2f;
  const float offx = DT_PIXEL_APPLY_DPI(60);
  const float at = 0.3f;
  dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_LIGHTTABLE_BG);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  PangoLayout *layout;
  PangoRectangle ink;
  PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
  pango_font_description_set_absolute_size(desc, fs * PANGO_SCALE);
  layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, desc);
  cairo_set_font_size(cr, fs);
  dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_LIGHTTABLE_FONT);
  pango_layout_set_text(layout, _("there are no images in this collection"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, offx, offy - ink.height - ink.x);
  pango_cairo_show_layout(cr, layout);
  pango_layout_set_text(layout, _("if you have not imported any images yet"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, offx, offy + 2 * ls - ink.height - ink.x);
  pango_cairo_show_layout(cr, layout);
  pango_layout_set_text(layout, _("you can do so in the import module"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, offx, offy + 3 * ls - ink.height - ink.x);
  pango_cairo_show_layout(cr, layout);
  cairo_move_to(cr, offx - DT_PIXEL_APPLY_DPI(10.0f), offy + 3 * ls - ls * .25f);
  cairo_line_to(cr, 0.0f, 10.0f);
  dt_gui_gtk_set_source_rgba(cr, DT_GUI_COLOR_LIGHTTABLE_FONT, at);
  cairo_stroke(cr);
  pango_layout_set_text(layout, _("try to relax the filter settings in the top panel"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, offx, offy + 5 * ls - ink.height - ink.x);
  dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_LIGHTTABLE_FONT);
  pango_cairo_show_layout(cr, layout);
  cairo_rel_move_to(cr, 10.0f + ink.width, ink.height * 0.5f);
  cairo_line_to(cr, width * 0.5f, 0.0f);
  dt_gui_gtk_set_source_rgba(cr, DT_GUI_COLOR_LIGHTTABLE_FONT, at);
  cairo_stroke(cr);
  pango_layout_set_text(layout, _("or add images in the collection module in the left panel"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, offx, offy + 6 * ls - ink.height - ink.x);
  dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_LIGHTTABLE_FONT);
  pango_cairo_show_layout(cr, layout);
  cairo_move_to(cr, offx - DT_PIXEL_APPLY_DPI(10.0f), offy + 6 * ls - ls * 0.25f);
  cairo_rel_line_to(cr, -offx + 10.0f, 0.0f);
  dt_gui_gtk_set_source_rgba(cr, DT_GUI_COLOR_LIGHTTABLE_FONT, at);
  cairo_stroke(cr);

  pango_font_description_free(desc);
  g_object_unref(layout);
  return 0;
}

static float _preview_get_zoom100(int32_t width, int32_t height, uint32_t imgid)
{
  int w, h;
  w = h = 0;
  dt_image_get_final_size(imgid, &w, &h);
  // 0.97f value come from dt_view_image_expose
  float zoom_100 = fmaxf((float)w / ((float)width * 0.97f), (float)h / ((float)height * 0.97f));
  if(zoom_100 < 1.0f) zoom_100 = 1.0f;

  return zoom_100;
}

static gboolean _culling_recreate_slots_at(dt_view_t *self, const int display_first_image)
{
  dt_library_t *lib = (dt_library_t *)self->data;

  int img_count = 0;
  gchar *query = NULL;

  // number of images to be displayed
  img_count = get_zoom();

  gchar *rowid_txt = NULL;
  if(display_first_image >= 0)
  {
    rowid_txt = dt_util_dstrcat(NULL, "(SELECT rowid FROM memory.collected_images WHERE imgid = %d)",
                                display_first_image);
  }
  else
    rowid_txt = dt_util_dstrcat(NULL, "%d", 0);

  if(lib->culling_use_selection)
  {
    query = dt_util_dstrcat(NULL,
                            "SELECT m.imgid, b.aspect_ratio "
                              "FROM memory.collected_images AS m, main.selected_images AS s, images AS b "
                              "WHERE m.imgid = b.id AND m.imgid = s.imgid AND m.rowid >= %s "
                              "ORDER BY m.rowid "
                              "LIMIT %d",
                            rowid_txt, img_count);
  }
  else
  {
    query = dt_util_dstrcat(NULL,
                            "SELECT m.imgid, b.aspect_ratio "
                              "FROM (SELECT rowid, imgid "
                                      "FROM memory.collected_images "
                                      "WHERE rowid < %s + %d "
                                      "ORDER BY rowid DESC "
                                      "LIMIT %d) AS m, "
                                    "images AS b "
                              "WHERE m.imgid = b.id "
                              "ORDER BY m.rowid",
                            rowid_txt, img_count, img_count);
  }

  // be sure we don't have some remaining config
  _culling_destroy_slots(self);
  lib->culling_next.imgid = lib->culling_previous.imgid = -1;

  /* prepare a new main query statement for collection */
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  if(stmt == NULL)
  {
    g_free(query);
    return FALSE;
  }

  lib->slots = calloc(img_count, sizeof(dt_layout_image_t));
  int i = 0;
  while(sqlite3_step(stmt) == SQLITE_ROW && i < img_count)
  {
    const int32_t id = sqlite3_column_int(stmt, 0);
    double aspect_ratio = sqlite3_column_double(stmt, 1);
    if(!aspect_ratio || aspect_ratio < 0.0001)
    {
      aspect_ratio = dt_image_set_aspect_ratio(id, FALSE);
      // if an error occurs, let's use 1:1 value
      if(aspect_ratio < 0.0001) aspect_ratio = 1.0;
    }
    lib->slots[i].imgid = id;
    lib->slots[i].aspect_ratio = aspect_ratio;
    i++;
  }
  sqlite3_finalize(stmt);
  g_free(query);
  lib->slots_count = i;

  // in rare cases, we can have less images than wanted
  // although there's images before
  if(lib->culling_use_selection && lib->slots_count < img_count
     && lib->slots_count < _culling_get_selection_count())
  {
    const int nb = img_count - lib->slots_count;
    query = dt_util_dstrcat(NULL,
                            "SELECT m.imgid, b.aspect_ratio "
                              "FROM memory.collected_images AS m, main.selected_images AS s, images AS b "
                              "WHERE m.imgid = b.id AND m.imgid = s.imgid AND m.rowid < %s "
                              "ORDER BY m.rowid DESC "
                              "LIMIT %d",
                            rowid_txt, nb);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    if(stmt != NULL)
    {
      while(sqlite3_step(stmt) == SQLITE_ROW && lib->slots_count <= img_count)
      {
        const int32_t id = sqlite3_column_int(stmt, 0);
        double aspect_ratio = sqlite3_column_double(stmt, 1);
        if(!aspect_ratio || aspect_ratio < 0.0001)
        {
          aspect_ratio = dt_image_set_aspect_ratio(id, FALSE);
          // if an error occurs, let's use 1:1 value
          if(aspect_ratio < 0.0001) aspect_ratio = 1.0;
        }

        // we shift everything up
        for(int j = img_count - 1; j > 0; j--)
        {
          lib->slots[j] = lib->slots[j - 1];
        }
        // we record the new one
        lib->slots[0].imgid = id;
        lib->slots[0].aspect_ratio = aspect_ratio;
        lib->slots_count++;
      }
      sqlite3_finalize(stmt);
    }
    g_free(query);
  }

  g_free(rowid_txt);
  lib->last_num_images = img_count;
  lib->slots_changed = TRUE;
  return TRUE;
}

static gboolean _culling_compute_slots(dt_view_t *self, int32_t width, int32_t height,
                                       const dt_lighttable_layout_t layout)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  if(lib->slots_count <= 0 || !lib->slots) return FALSE;

  int sum_w = 0, max_h = 0, max_w = 0;

  GList *slots = NULL;

  // reinit size and positions
  for(int i = 0; i < lib->slots_count; i++)
  {
    const double aspect_ratio = lib->slots[i].aspect_ratio;
    lib->slots[i].width = (gint)(sqrt(aspect_ratio) * 100);
    lib->slots[i].height = (gint)(1 / sqrt(aspect_ratio) * 100);
    lib->slots[i].x = lib->slots[i].y = 0;
  }

  unsigned int total_width = 0, total_height = 0;
  int distance = 1;
  float avg_ratio = 0;

  // Get total window width and max window width/height
  for(int i = 0; i < lib->slots_count; i++)
  {
    sum_w += lib->slots[i].width;
    max_w = MAX(max_w, lib->slots[i].width);
    max_h = MAX(max_h, lib->slots[i].height);
    avg_ratio += lib->slots[i].width / (float)lib->slots[i].height;
  }

  avg_ratio /= lib->slots_count;

  int per_row, tmp_per_row, per_col, tmp_per_col;
  per_row = tmp_per_row = ceil(sqrt(lib->slots_count));
  per_col = tmp_per_col = (lib->slots_count + per_row - 1) / per_row; // ceil(sel_img_count/per_row)

  float tmp_slot_ratio, slot_ratio;
  tmp_slot_ratio = slot_ratio = (width/ (float) per_row) / (height/ (float) per_col);

  do
  {
    per_row = tmp_per_row;
    per_col = tmp_per_col;
    slot_ratio = tmp_slot_ratio;

    if(avg_ratio > slot_ratio)
    {
      tmp_per_row = per_row - 1;
    }
    else
    {
      tmp_per_row = per_row + 1;
    }

    if(tmp_per_row == 0) break;

    tmp_per_col = (lib->slots_count + tmp_per_row - 1) / tmp_per_row; // ceil(sel_img_count / tmp_per_row);

    tmp_slot_ratio = (width/ (float) tmp_per_row) / (height/( float) tmp_per_col);

  } while(per_row > 0 && per_row <= lib->slots_count
          && absmul(tmp_slot_ratio, avg_ratio) < absmul(slot_ratio, avg_ratio));


  // Vertical layout
  for(int i = 0; i < lib->slots_count; i++)
  {
    GList *slot_iter = g_list_first(slots);
    for (; slot_iter; slot_iter = slot_iter->next)
    {
      GList *slot = (GList *) slot_iter->data;
      // Calculate current total height of slot
      int slot_h = distance;
      GList *slot_cw_iter = slot;
      while(slot_cw_iter != NULL)
      {
        dt_layout_image_t *slot_cw = (dt_layout_image_t *) slot_cw_iter->data;
        slot_h = slot_h + slot_cw->height + distance;
        slot_cw_iter = slot_cw_iter->next;
      }
      // Add window to slot if the slot height after adding the window
      // doesn't exceed max window height
      if(slot_h + distance + lib->slots[i].height < max_h)
      {
        slot_iter->data = g_list_append(slot, &(lib->slots[i]));
        break;
      }
    }
    // Otherwise, create a new slot with only this window
    if(!slot_iter) slots = g_list_append(slots, g_list_append(NULL, &(lib->slots[i])));
  }

  GList *rows = g_list_append(NULL, NULL);
  {
    int row_y = 0, x = 0, row_h = 0;
    int max_row_w = sum_w/per_col;//sqrt((float) sum_w * max_h);// * pow((float) width/height, 0.02);
    for (GList *slot_iter = slots; slot_iter != NULL; slot_iter = slot_iter->next)
    {
      GList *slot = (GList *) slot_iter->data;

      // Max width of windows in the slot
      int slot_max_w = 0;
      for (GList *slot_cw_iter = slot; slot_cw_iter != NULL; slot_cw_iter = slot_cw_iter->next)
      {
        dt_layout_image_t *cw = (dt_layout_image_t *) slot_cw_iter->data;
        slot_max_w = MAX(slot_max_w, cw->width);
      }

      int y = row_y;
      for (GList *slot_cw_iter = slot; slot_cw_iter != NULL; slot_cw_iter = slot_cw_iter->next)
      {
        dt_layout_image_t *cw = (dt_layout_image_t *) slot_cw_iter->data;
        cw->x = x + (slot_max_w - cw->width) / 2;
        cw->y = y;
        y += cw->height + distance;
        rows->data = g_list_append(rows->data, cw);
      }

      row_h = MAX(row_h, y - row_y);
      total_height = MAX(total_height, y);
      x += slot_max_w + distance;
      total_width = MAX(total_width, x);

      if (x > max_row_w)
      {
        x = 0;
        row_y += row_h;
        row_h = 0;
        rows = g_list_append(rows, 0);
        rows = rows->next;
      }
      g_list_free(slot);
    }
    g_list_free(slots);
    slots = NULL;
  }

  total_width -= distance;
  total_height -= distance;

  for(GList *iter = g_list_first(rows); iter != NULL; iter = iter->next)
  {
    GList *row = (GList *) iter->data;
    int row_w = 0, xoff;
    int max_rh = 0;

    for (GList *slot_cw_iter = row; slot_cw_iter != NULL; slot_cw_iter = slot_cw_iter->next)
    {
      dt_layout_image_t *cw = (dt_layout_image_t *) slot_cw_iter->data;
      row_w = MAX(row_w, cw->x + cw->width);
      max_rh = MAX(max_rh, cw->height);
    }

    xoff = (total_width - row_w) / 2;

    for (GList *cw_iter = row; cw_iter != NULL; cw_iter = cw_iter->next)
    {
      dt_layout_image_t *cw = (dt_layout_image_t *) cw_iter->data;
      cw->x += xoff;
      cw->height = max_rh;
    }
    g_list_free(row);
  }

  g_list_free(rows);

  float factor;
  factor = (float) (width - 1) / total_width;
  if (factor * total_height > height - 1)
    factor = (float) (height - 1) / total_height;

  int xoff = (width - (float) total_width * factor) / 2;
  int yoff = (height - (float) total_height * factor) / 2;

  for(int i = 0; i < lib->slots_count; i++)
  {
    lib->slots[i].width = lib->slots[i].width * factor;
    lib->slots[i].height = lib->slots[i].height * factor;
    lib->slots[i].x = lib->slots[i].x * factor + xoff;
    lib->slots[i].y = lib->slots[i].y * factor + yoff;
  }

  const int max_in_memory_images = _get_max_in_memory_images();

  // sort lib->fp_surf to re-use cached thumbs & surface
  if(layout == DT_LIGHTTABLE_LAYOUT_CULLING)
    _sort_preview_surface(lib, lib->slots, lib->slots_count, max_in_memory_images);

  // ensure fp_surf are in sync with slots
  if(lib->slots_count <= max_in_memory_images)
  {
    for(int i = 0; i < lib->slots_count; i++)
    {
      if(lib->slots[i].imgid != lib->fp_surf[i].imgid)
      {
        _destroy_preview_surface(lib->fp_surf + i);
        lib->fp_surf[i].imgid = lib->slots[i].imgid;
      }
    }
  }

  lib->last_num_images = get_zoom();
  lib->last_width = width;
  lib->last_height = height;

  // we want to be sure the filmstrip stay in synch
  if(layout == DT_LIGHTTABLE_LAYOUT_CULLING && lib->slots_count > 0)
  {
    // if the selection should follow active images
    if(lib->culling_follow_selection)
    {
      // deactivate selection_change event
      lib->select_deactivate = TRUE;
      // deselect all
      DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM main.selected_images", NULL, NULL, NULL);
      // select all active images
      GList *l = NULL;
      for(int i = 0; i < lib->slots_count; i++) l = g_list_append(l, GINT_TO_POINTER(lib->slots[i].imgid));
      dt_selection_select_list(darktable.selection, l);
      g_list_free(l);
      // reactivate selection_change event
      lib->select_deactivate = FALSE;
    }
    // move filmstrip
    dt_thumbtable_set_offset_image(dt_ui_thumbtable(darktable.gui->ui), lib->slots[0].imgid, TRUE);
  }

  // we save the current first id
  dt_conf_set_int("plugins/lighttable/culling_last_id", lib->slots[0].imgid);

  return TRUE;
}

static void _culling_prefetch(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  if(lib->slots_count == 0) return;

  const float imgwd = 0.97;
  const float fz = (lib->full_zoom > 1.0f) ? lib->full_zoom : 1.0f;

  // we get the previous & next images infos
  for(int i = 0; i < 2; i++)
  {
    dt_layout_image_t *img = (i == 0) ? &lib->culling_previous : &lib->culling_next;
    if(img->imgid < 0)
    {
      dt_layout_image_t sl = lib->slots[0];
      if(i == 1) sl = lib->slots[lib->slots_count - 1];
      gchar *query = NULL;
      if(lib->culling_use_selection)
      {
        query = dt_util_dstrcat(NULL,
                                "SELECT m.imgid, b.aspect_ratio "
                                  "FROM memory.collected_images AS m, main.selected_images AS s, images AS b "
                                  "WHERE m.rowid %s (SELECT rowid FROM memory.collected_images WHERE imgid = %d) "
                                        "AND m.imgid = s.imgid "
                                        "AND m.imgid = b.id "
                                  "ORDER BY m.rowid %s "
                                  "LIMIT 1",
                                (i == 0) ? "<" : ">", sl.imgid, (i == 0) ? "DESC" : "ASC");
      }
      else
      {
        query = dt_util_dstrcat(
            NULL,
            "SELECT m.imgid, b.aspect_ratio "
              "FROM memory.collected_images AS m, images AS b "
              "WHERE m.rowid %s (SELECT rowid FROM memory.collected_images WHERE imgid = %d) "
                    "AND m.imgid = b.id "
              "ORDER BY m.rowid %s "
              "LIMIT 1",
            (i == 0) ? "<" : ">", sl.imgid, (i == 0) ? "DESC" : "ASC");
      }
      sqlite3_stmt *stmt;
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);

      if(stmt != NULL)
      {
        if(sqlite3_step(stmt) == SQLITE_ROW)
        {
          img->imgid = sqlite3_column_int(stmt, 0);
          double aspect_ratio = sqlite3_column_double(stmt, 1);
          if(!aspect_ratio || aspect_ratio < 0.0001)
          {
            aspect_ratio = dt_image_set_aspect_ratio(img->imgid, FALSE);
            // if an error occurs, let's use 1:1 value
            if(aspect_ratio < 0.0001) aspect_ratio = 1.0;
          }
          img->aspect_ratio = aspect_ratio;
        }
        sqlite3_finalize(stmt);
      }
      g_free(query);

      // and we prefetch the image
      if(img->imgid >= 0)
      {
        dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, imgwd * sl.width * fz,
                                                                 imgwd * sl.height * fz);

        if(mip < DT_MIPMAP_8)
          dt_mipmap_cache_get(darktable.mipmap_cache, NULL, img->imgid, mip, DT_MIPMAP_PREFETCH, 'r');
      }
      else
        img->imgid = -2; // no image available
    }
  }
}

static int expose_culling(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx,
                          int32_t pointery, const dt_lighttable_layout_t layout)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  int missing = 0;

  lib->image_over = DT_VIEW_DESERT;

  dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_LIGHTTABLE_BG);
  cairo_paint(cr);

  // we recompute images sizes and positions if needed
  gboolean prefetch = FALSE;
  if(!lib->slots || lib->slots_count < 1) return 0;
  if(lib->last_num_images != get_zoom())
  {
    if(!_culling_recreate_slots_at(self, _culling_find_first_valid_imgid(self, lib->slots[0].imgid))) return 0;
  }
  if(lib->last_width != width || lib->last_height != height || lib->slots_changed)
  {
    if(!_culling_compute_slots(self, width, height, layout)) return 0;
    lib->slots_changed = FALSE;
    prefetch = TRUE;

    // we update the active images list
    g_slist_free(darktable.view_manager->active_images);
    darktable.view_manager->active_images = NULL;
    for(int i = 0; i < lib->slots_count; i++)
    {
      darktable.view_manager->active_images
          = g_slist_append(darktable.view_manager->active_images, GINT_TO_POINTER(lib->slots[i].imgid));
    }
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_ACTIVE_IMAGES_CHANGE);
  }

  const int max_in_memory_images = _get_max_in_memory_images();
  int mouse_over_id = -1;

  for(int i = 0; i < lib->slots_count; i++)
  {
    // set mouse over id
    if(pointerx > lib->slots[i].x && pointerx < lib->slots[i].x + lib->slots[i].width && pointery > lib->slots[i].y
       && pointery < lib->slots[i].y + lib->slots[i].height)
    {
      mouse_over_id = lib->slots[i].imgid;
      dt_control_set_mouse_over_id(mouse_over_id);
    }

    cairo_save(cr);
    // if(zoom == 1) dt_image_prefetch(image, DT_IMAGE_MIPF);
    cairo_translate(cr, lib->slots[i].x, lib->slots[i].y);
    int img_pointerx = pointerx > lib->slots[i].x && pointerx < lib->slots[i].x + lib->slots[i].width
                           ? pointerx - lib->slots[i].x
                           : lib->slots[i].width;
    int img_pointery = pointery > lib->slots[i].y && pointery < lib->slots[i].y + lib->slots[i].height
                           ? pointery - lib->slots[i].y
                           : lib->slots[i].height;

    dt_view_image_expose_t params = { 0 };
    params.image_over = &(lib->image_over);
    params.imgid = lib->slots[i].imgid;
    params.mouse_over = (mouse_over_id == lib->slots[i].imgid);
    params.cr = cr;
    params.width = lib->slots[i].width;
    params.height = lib->slots[i].height;
    params.px = img_pointerx;
    params.py = img_pointery;
    params.zoom = 1;
    params.full_preview = TRUE;

    if(lib->slots_count <= max_in_memory_images)
    {
      // we get the real zoom, taking eventual delta in account and sanitize it
      float fz = lib->full_zoom + lib->fp_surf[i].zoom_delta;
      if(fz < 1.0f && lib->fp_surf[i].zoom_delta < 0.0f)
      {
        lib->fp_surf[i].zoom_delta = 1.0f - lib->full_zoom;
        fz = 1.0f;
      }
      else if(fz > lib->fp_surf[i].zoom_100 && lib->fp_surf[i].zoom_delta > 0.0f)
      {
        lib->fp_surf[i].zoom_delta = lib->fp_surf[i].zoom_100 - lib->full_zoom;
        fz = lib->fp_surf[i].zoom_100;
      }

      if(fz > 1.0f)
      {
        params.full_zoom = fz;
        params.full_x = lib->full_x + lib->fp_surf[i].dx_delta;
        params.full_y = lib->full_y + lib->fp_surf[i].dy_delta;
        params.full_surface = &lib->fp_surf[i].surface;
        params.full_rgbbuf = &lib->fp_surf[i].rgbbuf;
        params.full_surface_mip = &lib->fp_surf[i].mip;
        params.full_surface_id = &lib->fp_surf[i].imgid;
        params.full_surface_wd = &lib->fp_surf[i].width;
        params.full_surface_ht = &lib->fp_surf[i].height;
        params.full_surface_w_lock = &lib->fp_surf[i].w_lock;
        if(lib->fp_surf[i].zoom_100 >= 1000.0f || lib->fp_surf[i].imgid != lib->slots[i].imgid)
          lib->fp_surf[i].zoom_100
              = _preview_get_zoom100(lib->slots[i].width, lib->slots[i].height, lib->slots[i].imgid);
        params.full_zoom100 = lib->fp_surf[i].zoom_100;
        params.full_w1 = &lib->fp_surf[i].w_fit;
        params.full_h1 = &lib->fp_surf[i].h_fit;
        params.full_maxdx = &lib->fp_surf[i].max_dx;
        params.full_maxdy = &lib->fp_surf[i].max_dy;
      }
    }

    missing += dt_view_image_expose(&params);
    cairo_restore(cr);
  }

  // if needed, we prefetch the next and previous images
  // note that we only guess their sizes so they may be computed anyway
  if(prefetch) _culling_prefetch(self);

  if(darktable.unmuted & DT_DEBUG_CACHE) dt_mipmap_cache_print(darktable.mipmap_cache);
  return missing;
}

/**
 * Displays a full screen preview of the image currently under the mouse pointer.
 */
static int expose_full_preview(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx,
                               int32_t pointery)
{
  dt_library_t *lib = (dt_library_t *)self->data;

  int n_width = width * lib->full_zoom;
  int n_height = height * lib->full_zoom;
  // only look for images to preload or update the one shown when we moved to another image
  if(lib->track != 0)
  {
    /* How many images to preload in advance. */
    int preload_num = dt_conf_get_int("plugins/lighttable/preview/full_size_preload_count");
    gboolean preload = preload_num > 0;
    preload_num = CLAMPS(preload_num, 1, 99999);

    sqlite3_stmt *stmt;
    gchar *stmt_string
        = g_strdup_printf("SELECT col.imgid AS id, col.rowid FROM memory.collected_images AS col %s "
                          "WHERE col.rowid %s %d ORDER BY col.rowid %s LIMIT %d",
                          (!lib->full_preview_inside_sel) ?
                                                          /* We want to operate on the currently collected images,
                                                           * so there's no need to match against the selection */
                              ""
                                                          :
                                                          /* Limit the matches to the current selection */
                              "INNER JOIN main.selected_images AS sel ON col.imgid = sel.imgid",
                          (lib->track > 0) ? ">" : "<", lib->full_preview_rowid,
                          /* Direction of our navigation -- when showing for the first time,
                           * i.e. when offset == 0, assume forward navigation */
                          (lib->track > 0) ? "ASC" : "DESC", preload_num);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), stmt_string, -1, &stmt, NULL);

    /* Walk through the "next" images, activate preload and find out where to go if moving */
    int *preload_stack = malloc(preload_num * sizeof(int));
    for(int i = 0; i < preload_num; ++i)
    {
      preload_stack[i] = -1;
    }
    int count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
      /* Check if we're about to move */
      if(count == 0)
      {
        /* We're moving, so let's update the "next image" bits */
        lib->full_preview_id = sqlite3_column_int(stmt, 0);
        lib->full_preview_rowid = sqlite3_column_int(stmt, 1);
        dt_control_set_mouse_over_id(lib->full_preview_id);
        // set the active image
        g_slist_free(darktable.view_manager->active_images);
        darktable.view_manager->active_images = NULL;
        darktable.view_manager->active_images
            = g_slist_append(darktable.view_manager->active_images, GINT_TO_POINTER(lib->full_preview_id));
        dt_control_signal_raise(darktable.signals, DT_SIGNAL_ACTIVE_IMAGES_CHANGE);
        // selection_follow
        if(lib->full_preview_follow_sel)
        {
          lib->select_deactivate = TRUE;
          dt_selection_select_single(darktable.selection, lib->full_preview_id);
          lib->select_deactivate = FALSE;
        }
      }
      /* Store the image details for preloading, see below. */
      preload_stack[count] = sqlite3_column_int(stmt, 0);
      ++count;
    }
    g_free(stmt_string);
    sqlite3_finalize(stmt);

    if(preload)
    {
      dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, n_width, n_height);
      /* Preload these images.
      * The job queue is not a queue, but a stack, so we have to do it backwards.
      * Simply swapping DESC and ASC in the SQL won't help because we rely on the LIMIT clause, and
      * that LIMIT has to work with the "correct" sort order. One could use a subquery, but I don't
      * think that would be terribly elegant, either. */
      while(--count >= 0 && preload_stack[count] != -1 && mip != DT_MIPMAP_8)
      {
        dt_mipmap_cache_get(darktable.mipmap_cache, NULL, preload_stack[count], mip, DT_MIPMAP_PREFETCH, 'r');
      }
    }
    free(preload_stack);

    lib->track = 0;
  }

  lib->image_over = DT_VIEW_DESERT;
  dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_LIGHTTABLE_PREVIEW_BG);
  cairo_paint(cr);

  const int frows = 5, fcols = 5;
  if(lib->display_focus)
  {
    if(lib->full_res_thumb_id != lib->full_preview_id)
    {
      gboolean from_cache = TRUE;
      char filename[PATH_MAX] = { 0 };
      dt_image_full_path(lib->full_preview_id, filename, sizeof(filename), &from_cache);
      dt_free_align(lib->full_res_thumb);
      lib->full_res_thumb = NULL;
      dt_colorspaces_color_profile_type_t color_space;
      if(!dt_imageio_large_thumbnail(filename, &lib->full_res_thumb,
                                               &lib->full_res_thumb_wd,
                                               &lib->full_res_thumb_ht,
                                               &color_space))
      {
        lib->full_res_thumb_orientation = ORIENTATION_NONE;
        lib->full_res_thumb_id = lib->full_preview_id;
      }

      if(lib->full_res_thumb_id == lib->full_preview_id)
      {
        dt_focus_create_clusters(lib->full_res_focus, frows, fcols, lib->full_res_thumb,
                                 lib->full_res_thumb_wd, lib->full_res_thumb_ht);
      }
    }
  }

  if(!lib->slots || lib->slots_count != 1 || lib->slots[0].imgid != lib->full_preview_id
     || lib->slots[0].width != width || lib->slots[0].height != height)
  {
    _culling_destroy_slots(self);
    lib->slots_count = 1;
    lib->slots = calloc(lib->slots_count, sizeof(dt_layout_image_t));
    lib->slots[0].imgid = lib->full_preview_id;
    lib->slots[0].width = width;
    lib->slots[0].height = height;
  }

  dt_view_image_expose_t params = { 0 };
  params.image_over = &(lib->image_over);
  params.imgid = lib->full_preview_id;
  params.cr = cr;
  params.width = width;
  params.height = height;
  params.px = pointerx;
  params.py = pointery;
  params.zoom = 1;
  params.full_preview = TRUE;
  params.full_zoom = lib->full_zoom;
  if(lib->full_zoom > 1.0f)
  {
    if(lib->fp_surf[0].zoom_100 >= 1000.0f || lib->fp_surf[0].imgid != lib->full_preview_id)
      lib->fp_surf[0].zoom_100 = _preview_get_zoom100(width, height, lib->full_preview_id);
    params.full_zoom100 = lib->fp_surf[0].zoom_100;
    params.full_maxdx = &lib->fp_surf[0].max_dx;
    params.full_maxdy = &lib->fp_surf[0].max_dy;
    params.full_w1 = &lib->fp_surf[0].w_fit;
    params.full_h1 = &lib->fp_surf[0].h_fit;
    params.full_x = lib->full_x;
    params.full_y = lib->full_y;
    params.full_surface = &lib->fp_surf[0].surface;
    params.full_rgbbuf = &lib->fp_surf[0].rgbbuf;
    params.full_surface_mip = &lib->fp_surf[0].mip;
    params.full_surface_id = &lib->fp_surf[0].imgid;
    params.full_surface_wd = &lib->fp_surf[0].width;
    params.full_surface_ht = &lib->fp_surf[0].height;
    params.full_surface_w_lock = &lib->fp_surf[0].w_lock;
  }
  const int missing = dt_view_image_expose(&params);

  if(lib->display_focus && (lib->full_res_thumb_id == lib->full_preview_id))
    dt_focus_draw_clusters(cr, width, height, lib->full_preview_id, lib->full_res_thumb_wd, lib->full_res_thumb_ht,
                           lib->full_res_focus, frows, fcols, lib->full_zoom, lib->full_x, lib->full_y);
  return missing;
}

static gboolean _expose_again(gpointer user_data)
{
  // unfortunately there might have been images without thumbnails during expose.
  // this can have multiple reasons: not loaded yet (we'll receive a signal when done)
  // or still locked for writing.. we won't be notified when this changes.
  // so we just track whether there were missing images and expose again.
  dt_control_queue_redraw_center();
  return FALSE; // don't call again
}

void begin_pan(dt_library_t *lib, double x, double y)
{
  lib->pan_x = x;
  lib->pan_y = y;
  lib->pan = 1;
}

void expose(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_library_t *lib = (dt_library_t *)self->data;

  const double start = dt_get_wtime();
  const dt_lighttable_layout_t layout = get_layout();

  // Let's show full preview if in that state...

  lib->missing_thumbnails = 0;

  check_layout(self);

  if(!darktable.collection || darktable.collection->count <= 0)
  {
    if(layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER || layout == DT_LIGHTTABLE_LAYOUT_ZOOMABLE)
      gtk_widget_hide(dt_ui_thumbtable(darktable.gui->ui)->widget);
    expose_empty(self, cr, width, height, pointerx, pointery);
  }
  else if(lib->full_preview_id != -1)
  {
    lib->missing_thumbnails = expose_full_preview(self, cr, width, height, pointerx, pointery);
  }
  else // we do pass on expose to manager or zoomable
  {
    switch(layout)
    {
      case DT_LIGHTTABLE_LAYOUT_ZOOMABLE:
      case DT_LIGHTTABLE_LAYOUT_FILEMANAGER:
        if(!gtk_widget_get_visible(dt_ui_thumbtable(darktable.gui->ui)->widget))
          gtk_widget_show(dt_ui_thumbtable(darktable.gui->ui)->widget);
        break;
      case DT_LIGHTTABLE_LAYOUT_CULLING:
        lib->missing_thumbnails = expose_culling(self, cr, width, height, pointerx, pointery, layout);
        break;
      case DT_LIGHTTABLE_LAYOUT_FIRST:
      case DT_LIGHTTABLE_LAYOUT_LAST:
        break;
    }
  }

  // we have started the first expose
  lib->already_started = TRUE;

  if(layout != DT_LIGHTTABLE_LAYOUT_ZOOMABLE && !_is_custom_image_order_actif(self))
  {
    // file manager
    lib->activate_on_release = DT_VIEW_ERR;
  }

  const double end = dt_get_wtime();
  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_LIGHTTABLE, "[lighttable] expose took %0.04f sec\n", end - start);

  if(lib->missing_thumbnails)
    g_timeout_add(250, _expose_again, self);
  else
  {
    // clear hash map of thumb to redisplay, we are done
    g_hash_table_remove_all(lib->thumbs_table);
  }
}

static void _culling_move_start(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;

  // reset culling layout
  _culling_destroy_slots(self);
  // go to the last image on the collection / selection
  int imgid = -1;
  gchar *query = NULL;
  if(lib->culling_use_selection)
  {
    query = dt_util_dstrcat(NULL, "SELECT s.imgid "
                                  "FROM main.selected_images AS s, memory.collected_images AS m "
                                  "WHERE s.imgid = m.imgid "
                                  "ORDER BY m.rowid ASC LIMIT 1");
  }
  else
  {
    query = dt_util_dstrcat(NULL, "SELECT imgid "
                                  "FROM memory.collected_images "
                                  "ORDER BY rowid ASC LIMIT 1");
  }
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  if(stmt != NULL)
  {
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      imgid = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
  }
  g_free(query);

  // select this image
  if(imgid >= 0) _culling_recreate_slots_at(self, imgid);
}

static void _culling_move_end(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;

  // reset culling layout
  _culling_destroy_slots(self);
  // go to the last image on the collection / selection
  int imgid = -1;
  gchar *query = NULL;
  if(lib->culling_use_selection)
  {
    query = dt_util_dstrcat(NULL, "SELECT s.imgid "
                                  "FROM main.selected_images AS s, memory.collected_images AS m "
                                  "WHERE s.imgid = m.imgid "
                                  "ORDER BY m.rowid DESC LIMIT 1");
  }
  else
  {
    query = dt_util_dstrcat(NULL, "SELECT imgid "
                                  "FROM memory.collected_images "
                                  "ORDER BY rowid DESC LIMIT 1");
  }
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  if(stmt != NULL)
  {
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      imgid = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
  }
  g_free(query);

  // select this image
  if(imgid >= 0) _culling_recreate_slots_at(self, imgid);
}

static void _culling_move_pageup(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;

  if(dt_view_lighttable_get_culling_zoom_mode(darktable.view_manager) == DT_LIGHTTABLE_ZOOM_FIXED
     || !lib->culling_use_selection)
  {
    // jump to the previous page
    int imgid = -1;
    gchar *query = NULL;
    if(lib->culling_use_selection)
    {
      query = dt_util_dstrcat(NULL,
                              "SELECT nid FROM"
                              " (SELECT s.imgid AS nid, m.rowid AS nrowid"
                              " FROM main.selected_images AS s, memory.collected_images AS m"
                              " WHERE s.imgid = m.imgid AND m.rowid <"
                              " (SELECT rowid FROM memory.collected_images WHERE imgid = %d)"
                              " ORDER BY m.rowid DESC LIMIT %d) "
                              "ORDER BY nrowid ASC LIMIT 1",
                              lib->slots[0].imgid, lib->slots_count);
    }
    else
    {
      query = dt_util_dstrcat(NULL,
                              "SELECT imgid FROM"
                              " (SELECT imgid, rowid"
                              " FROM memory.collected_images"
                              " WHERE rowid < (SELECT rowid FROM memory.collected_images WHERE imgid = %d)"
                              " ORDER BY rowid DESC LIMIT %d) "
                              "ORDER BY rowid LIMIT 1",
                              lib->slots[0].imgid, lib->slots_count);
    }
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    if(stmt != NULL)
    {
      if(sqlite3_step(stmt) == SQLITE_ROW)
      {
        imgid = sqlite3_column_int(stmt, 0);
      }
      sqlite3_finalize(stmt);
    }
    g_free(query);

    // select this image
    if(imgid >= 0) _culling_recreate_slots_at(self, imgid);
  }
}

static void _culling_move_pagedown(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;

  if(dt_view_lighttable_get_culling_zoom_mode(darktable.view_manager) == DT_LIGHTTABLE_ZOOM_FIXED
     || !lib->culling_use_selection)
  {
    // jump to the first "not visible" image
    int imgid = -1;
    gchar *query = NULL;
    if(lib->culling_use_selection)
    {
      query = dt_util_dstrcat(NULL,
                              "SELECT s.imgid "
                              "FROM main.selected_images AS s, memory.collected_images AS m "
                              "WHERE s.imgid = m.imgid AND m.rowid >"
                              " (SELECT rowid FROM memory.collected_images WHERE imgid = %d) "
                              "ORDER BY m.rowid LIMIT 1",
                              lib->slots[lib->slots_count - 1].imgid);
    }
    else
    {
      query = dt_util_dstrcat(NULL,
                              "SELECT imgid "
                              "FROM memory.collected_images "
                              "WHERE rowid > (SELECT rowid FROM memory.collected_images WHERE imgid = %d) "
                              "ORDER BY rowid LIMIT 1",
                              lib->slots[lib->slots_count - 1].imgid);
    }
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    if(stmt != NULL)
    {
      if(sqlite3_step(stmt) == SQLITE_ROW)
      {
        imgid = sqlite3_column_int(stmt, 0);
      }
      sqlite3_finalize(stmt);
    }
    g_free(query);

    // select this image
    if(imgid >= 0) _culling_recreate_slots_at(self, imgid);
  }
}

static gboolean select_toggle_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                           GdkModifierType modifier, gpointer data)
{
  const uint32_t id = dt_control_get_mouse_over_id();
  dt_selection_toggle(darktable.selection, id);
  return TRUE;
}

static gboolean select_single_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                           GdkModifierType modifier, gpointer data)
{
  const uint32_t id = dt_control_get_mouse_over_id();
  dt_selection_select_single(darktable.selection, id);
  return TRUE;
}

static void _lighttable_mipmaps_updated_signal_callback(gpointer instance, int imgid, gpointer user_data)
{
  dt_control_queue_redraw_center();
}

// shitf the first select image by 1 with up direction
static void _culling_scroll(dt_library_t *lib, const int up)
{
  if(lib->slots_count <= 0) return;

  // we move the slots using in-memory previous/next images
  if(up)
  {
    if(lib->culling_previous.imgid >= 0)
    {
      lib->culling_next = lib->slots[lib->slots_count - 1];
      for(int i = lib->slots_count - 1; i > 0; i--)
      {
        lib->slots[i] = lib->slots[i - 1];
      }
      lib->slots[0] = lib->culling_previous;
      lib->culling_previous.imgid = -1;
      lib->slots_changed = TRUE;
      dt_control_queue_redraw_center();
    }
    else if(lib->culling_previous.imgid == -2
            && (dt_view_lighttable_get_culling_zoom_mode(darktable.view_manager) == DT_LIGHTTABLE_ZOOM_FIXED
                || !lib->culling_use_selection))
    {
      if(lib->culling_use_selection)
      {
        const int nbsel = _culling_get_selection_count();
        dt_control_log(ngettext("you have reached the start of your selection (%d image)",
                                "you have reached the start of your selection (%d images)", nbsel),
                       nbsel);
      }
      else
        dt_control_log(_("you have reached the start of your collection"));
    }
  }
  else if(!up)
  {
    if(lib->culling_next.imgid >= 0)
    {
      lib->culling_previous = lib->slots[0];
      for(int i = 0; i < lib->slots_count - 1; i++)
      {
        lib->slots[i] = lib->slots[i + 1];
      }
      lib->slots[lib->slots_count - 1] = lib->culling_next;
      lib->culling_next.imgid = -1;
      lib->slots_changed = TRUE;
      dt_control_queue_redraw_center();
    }
    else if(lib->culling_next.imgid == -2
            && (dt_view_lighttable_get_culling_zoom_mode(darktable.view_manager) == DT_LIGHTTABLE_ZOOM_FIXED
                || !lib->culling_use_selection))
    {
      if(lib->culling_use_selection)
      {
        const int nbsel = _culling_get_selection_count();
        dt_control_log(ngettext("you have reached the end of your selection (%d image)",
                                "you have reached the end of your selection (%d images)", nbsel),
                       nbsel);
      }
      else
        dt_control_log(_("you have reached the end of your collection"));
    }
  }
}

static void _lighttable_thumbtable_activate_signal_callback(gpointer instance, int imgid, gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_library_t *lib = (dt_library_t *)self->data;
  const dt_lighttable_layout_t layout = get_layout();

  if(lib->full_preview_id > 0)
  {
    if(lib->full_preview_id != imgid)
    {
      printf("coucou %d\n", imgid);
      lib->full_preview_id = imgid;
      // if we navigate inside selection and the current image is outside, reset this param
      // same for follow sel
      if(lib->full_preview_inside_sel || lib->full_preview_follow_sel)
      {
        sqlite3_stmt *stmt;
        gchar *query = dt_util_dstrcat(NULL, "SELECT imgid FROM main.selected_images WHERE imgid=%d", imgid);
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
        if(sqlite3_step(stmt) != SQLITE_ROW)
        {
          lib->full_preview_inside_sel = FALSE;
          lib->full_preview_follow_sel = FALSE;
        }
        sqlite3_finalize(stmt);
        g_free(query);
      }

      // follow selection if needed
      if(lib->full_preview_follow_sel) dt_selection_select_single(darktable.selection, imgid);

      dt_thumbtable_set_offset_image(dt_ui_thumbtable(darktable.gui->ui), lib->full_preview_id, TRUE);
      lib->full_preview_rowid = dt_ui_thumbtable(darktable.gui->ui)->offset;
      dt_control_set_mouse_over_id(lib->full_preview_id);
      // set the active image
      g_slist_free(darktable.view_manager->active_images);
      darktable.view_manager->active_images = NULL;
      darktable.view_manager->active_images
          = g_slist_append(darktable.view_manager->active_images, GINT_TO_POINTER(lib->full_preview_id));
      dt_control_signal_raise(darktable.signals, DT_SIGNAL_ACTIVE_IMAGES_CHANGE);
      dt_control_queue_redraw_center();
    }
  }
  else if(layout == DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    if(lib->slots_count > 0 && lib->slots[0].imgid != imgid)
    {
      if(dt_view_lighttable_get_culling_zoom_mode(darktable.view_manager) == DT_LIGHTTABLE_ZOOM_DYNAMIC)
      {
        // in dymamic mode, it's only selection that change displayed images. No way to do it by hand !
        return;
      }
      if(lib->culling_use_selection)
      {
        // if we navigate inside selection, we need to be sure that we stay inside selection...
        gboolean inside = FALSE;
        sqlite3_stmt *stmt;
        gchar *query = dt_util_dstrcat(NULL, "SELECT imgid FROM main.selected_images WHERE imgid=%d", imgid);
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
        if(sqlite3_step(stmt) == SQLITE_ROW)
        {
          inside = TRUE;
        }
        sqlite3_finalize(stmt);
        g_free(query);
        if(!inside) return;
      }
      _culling_recreate_slots_at(self, imgid);
      dt_control_queue_redraw_center();
    }
  }
  else if(layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER || layout == DT_LIGHTTABLE_LAYOUT_ZOOMABLE)
  {
    // we switch to darkroom
    dt_view_manager_switch(darktable.view_manager, "darkroom");
  }
}

void enter(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;

  // we want to reacquire the thumbtable if needed
  if(lib->full_preview_id < 1)
  {
    if(get_layout() == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
    {
      dt_thumbtable_set_parent(dt_ui_thumbtable(darktable.gui->ui), dt_ui_center_base(darktable.gui->ui),
                               DT_THUMBTABLE_MODE_FILEMANAGER);
      gtk_widget_show(dt_ui_thumbtable(darktable.gui->ui)->widget);
    }
    else if(get_layout() == DT_LIGHTTABLE_LAYOUT_ZOOMABLE)
    {
      dt_thumbtable_set_parent(dt_ui_thumbtable(darktable.gui->ui), dt_ui_center_base(darktable.gui->ui),
                               DT_THUMBTABLE_MODE_ZOOM);
      gtk_widget_show(dt_ui_thumbtable(darktable.gui->ui)->widget);
    }
  }

  // clean the undo list
  dt_undo_clear(darktable.undo, DT_UNDO_LIGHTTABLE);

  /* connect to signals */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_MIPMAP_UPDATED,
                            G_CALLBACK(_lighttable_mipmaps_updated_signal_callback), (gpointer)self);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE,
                            G_CALLBACK(_lighttable_thumbtable_activate_signal_callback), (gpointer)self);

  gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));

  // clear some state variables
  lib->pan = 0;
  lib->activate_on_release = DT_VIEW_ERR;
  dt_collection_hint_message(darktable.collection);

  // show/hide filmstrip & timeline when entering the view
  if(get_layout() == DT_LIGHTTABLE_LAYOUT_CULLING || lib->full_preview_id != -1)
  {
    dt_lib_set_visible(darktable.view_manager->proxy.timeline.module, FALSE); // not available in this layouts
    dt_lib_set_visible(darktable.view_manager->proxy.filmstrip.module,
                       TRUE); // always on, visibility is driven by panel state
  }
  else
  {
    dt_lib_set_visible(darktable.view_manager->proxy.filmstrip.module, FALSE); // not available in this layouts
    dt_lib_set_visible(darktable.view_manager->proxy.timeline.module,
                       TRUE); // always on, visibility is driven by panel state
  }

  // restore panels
  dt_ui_restore_panels(darktable.gui->ui);
}

static void _preview_enter(dt_view_t *self, gboolean sticky, gboolean focus, int32_t mouse_over_id)
{
  dt_library_t *lib = (dt_library_t *)self->data;

  if(lib->current_layout == DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    // save current slots
    lib->slots_old = lib->slots;
    lib->slots_count_old = lib->slots_count;
    lib->slots = NULL;
    lib->slots_count = 0;
  }
  else
  {
    // record current offset
    lib->thumbtable_offset = dt_thumbtable_get_offset(dt_ui_thumbtable(darktable.gui->ui));
    // ensure that thumbtable is not visible in the main view
    gtk_widget_hide(dt_ui_thumbtable(darktable.gui->ui)->widget);
  }

  lib->full_preview_sticky = sticky;
  lib->full_preview_id = _culling_preview_init_values(self, FALSE, TRUE);
  dt_control_set_mouse_over_id(lib->full_preview_id);

  // set corresponding rowid in the collected images
  {
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT rowid FROM memory.collected_images WHERE imgid=?1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, lib->full_preview_id);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      lib->full_preview_rowid = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
  }

  // update thumbtable, to indicate if we navigate inside selection or not
  // this is needed as collection change is handle there
  dt_ui_thumbtable(darktable.gui->ui)->navigate_inside_selection = lib->full_preview_inside_sel;

  // show/hide filmstrip & timeline when entering the view
  dt_lib_set_visible(darktable.view_manager->proxy.timeline.module, FALSE); // not available in this layouts
  dt_lib_set_visible(darktable.view_manager->proxy.filmstrip.module,
                     TRUE); // always on, visibility is driven by panel state
  dt_thumbtable_set_offset_image(dt_ui_thumbtable(darktable.gui->ui), lib->full_preview_id, TRUE);

  // set the active image
  g_slist_free(darktable.view_manager->active_images);
  darktable.view_manager->active_images = NULL;
  darktable.view_manager->active_images
      = g_slist_append(darktable.view_manager->active_images, GINT_TO_POINTER(lib->full_preview_id));
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_ACTIVE_IMAGES_CHANGE);

  // restore panels
  dt_ui_restore_panels(darktable.gui->ui);

  // we don't need the scrollbars
  dt_ui_scrollbars_show(darktable.gui->ui, FALSE);

  // preview with focus detection
  lib->display_focus = focus;

  // reset preview values
  lib->full_zoom = 1.0f;
  lib->full_x = 0.0f;
  lib->full_y = 0.0f;
  _full_preview_destroy(self);
}
static void _preview_quit(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  if(lib->full_preview_follow_sel)
  {
    dt_selection_select_single(darktable.selection, lib->full_preview_id);
  }
  lib->full_preview_id = -1;
  lib->full_preview_rowid = -1;

  lib->display_focus = 0;
  _full_preview_destroy(self);
  lib->full_zoom = 1.0f;
  lib->full_x = 0.0f;
  lib->full_y = 0.0f;

  // restore panels
  dt_ui_restore_panels(darktable.gui->ui);

  // show/hide filmstrip & timeline when entering the view
  if(lib->current_layout == DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    // update thumbtable, to indicate if we navigate inside selection or not
    // this is needed as collection change is handle there
    dt_ui_thumbtable(darktable.gui->ui)->navigate_inside_selection = lib->culling_use_selection;

    // retrieve saved slots
    _culling_destroy_slots(self);
    lib->slots = lib->slots_old;
    lib->slots_count = lib->slots_count_old;
    lib->slots_old = NULL;
    lib->slots_count_old = 0;

    dt_lib_set_visible(darktable.view_manager->proxy.timeline.module, FALSE); // not available in this layouts
    dt_lib_set_visible(darktable.view_manager->proxy.filmstrip.module,
                       TRUE); // always on, visibility is driven by panel state
  }
  else
  {
    dt_ui_thumbtable(darktable.gui->ui)->navigate_inside_selection = FALSE;
    dt_lib_set_visible(darktable.view_manager->proxy.filmstrip.module, FALSE); // not available in this layouts
    dt_lib_set_visible(darktable.view_manager->proxy.timeline.module,
                       TRUE); // always on, visibility is driven by panel state

    // set offset back
    dt_thumbtable_set_offset(dt_ui_thumbtable(darktable.gui->ui), lib->thumbtable_offset, TRUE);

    // we need to show thumbtable
    if(lib->current_layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
    {
      dt_thumbtable_set_parent(dt_ui_thumbtable(darktable.gui->ui), dt_ui_center_base(darktable.gui->ui),
                               DT_THUMBTABLE_MODE_FILEMANAGER);
    }
    else if(lib->current_layout == DT_LIGHTTABLE_LAYOUT_ZOOMABLE)
    {
      dt_thumbtable_set_parent(dt_ui_thumbtable(darktable.gui->ui), dt_ui_center_base(darktable.gui->ui),
                               DT_THUMBTABLE_MODE_ZOOM);
    }
    gtk_widget_show(dt_ui_thumbtable(darktable.gui->ui)->widget);
    dt_thumbtable_full_redraw(dt_ui_thumbtable(darktable.gui->ui), TRUE);
  }

  lib->slots_changed = TRUE;
}

void leave(dt_view_t *self)
{
  // we remove the thumbtable from main view
  dt_library_t *lib = (dt_library_t *)self->data;
  dt_thumbtable_set_parent(dt_ui_thumbtable(darktable.gui->ui), NULL, DT_THUMBTABLE_MODE_FILMSTRIP);
  // ensure we have no active image remaining
  if(darktable.view_manager->active_images)
  {
    g_slist_free(darktable.view_manager->active_images);
    darktable.view_manager->active_images = NULL;
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_ACTIVE_IMAGES_CHANGE);
  }

  /* disconnect from signals */
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_lighttable_mipmaps_updated_signal_callback),
                               (gpointer)self);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_lighttable_thumbtable_activate_signal_callback),
                               (gpointer)self);

  // clear some state variables
  lib->pan = 0;
  lib->activate_on_release = DT_VIEW_ERR;

  // exit preview mode if non-sticky
  if(lib->full_preview_id != -1 && lib->full_preview_sticky == 0)
  {
    _preview_quit(self);
  }

  // cleanup full preview image if any
  _full_preview_destroy(self);

  // cleanup culling layout if any
  _culling_destroy_slots(self);

  dt_ui_scrollbars_show(darktable.gui->ui, FALSE);
}

void reset(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->track = lib->pan = 0;
  lib->activate_on_release = DT_VIEW_ERR;
  dt_control_set_mouse_over_id(-1);
}

void mouse_enter(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  if(lib->full_preview_id > 0) dt_control_set_mouse_over_id(lib->full_preview_id);
}

void mouse_leave(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  if(lib->full_preview_id == -1 && get_layout() != DT_LIGHTTABLE_LAYOUT_CULLING) return;

  if(!lib->pan && get_zoom() != 1)
  {
    dt_control_set_mouse_over_id(-1);
    dt_control_queue_redraw_center();
  }
}


void scrollbar_changed(dt_view_t *self, double x, double y)
{
  const dt_lighttable_layout_t layout = get_layout();

  switch(layout)
  {
    case DT_LIGHTTABLE_LAYOUT_FILEMANAGER:
    case DT_LIGHTTABLE_LAYOUT_ZOOMABLE:
    {
      dt_thumbtable_scrollbar_changed(dt_ui_thumbtable(darktable.gui->ui), x, y);
      break;
    }
    default:
      break;
  }
}

static gboolean _lighttable_preview_zoom_add(dt_view_t *self, float val, double posx, double posy, int state)
{
  dt_library_t *lib = (dt_library_t *)self->data;

  if(lib->full_preview_id > -1 || get_layout() == DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    const int max_in_memory_images = _get_max_in_memory_images();
    if(get_layout() == DT_LIGHTTABLE_LAYOUT_CULLING && lib->slots_count > max_in_memory_images)
    {
      dt_control_log(_("zooming is limited to %d images"), max_in_memory_images);
    }
    else
    {
      // we get the 100% zoom of the largest image
      float zmax = 1.0f;
      for(int i = 0; i < lib->slots_count; i++)
      {
        if(lib->fp_surf[i].zoom_100 >= 1000.0f || lib->fp_surf[i].imgid != lib->slots[i].imgid)
          lib->fp_surf[i].zoom_100
              = _preview_get_zoom100(lib->slots[i].width, lib->slots[i].height, lib->slots[i].imgid);
        if(lib->fp_surf[i].zoom_100 > zmax) zmax = lib->fp_surf[i].zoom_100;
      }

      float nz = fminf(zmax, lib->full_zoom + val);
      nz = fmaxf(nz, 1.0f);

      // if full preview, we center the zoom at mouse position
      if(lib->full_zoom != nz && lib->full_preview_id > -1 && posx >= 0.0f && posy >= 0.0f)
      {
        // we want to zoom "around" the pointer
        float dx = nz / lib->full_zoom
                       * (posx - (self->width - lib->fp_surf[0].w_fit * lib->full_zoom) * 0.5f - lib->full_x)
                   - posx + (self->width - lib->fp_surf[0].w_fit * nz) * 0.5f;
        float dy = nz / lib->full_zoom
                       * (posy - (self->height - lib->fp_surf[0].h_fit * lib->full_zoom) * 0.5f - lib->full_y)
                   - posy + (self->height - lib->fp_surf[0].h_fit * nz) * 0.5f;
        lib->full_x = -dx;
        lib->full_y = -dy;
      }

      // culling
      if(lib->full_preview_id < 0)
      {
        // if shift+ctrl, we only change the current image
        if((state & GDK_SHIFT_MASK) == GDK_SHIFT_MASK)
        {
          int mouseid = dt_control_get_mouse_over_id();
          for(int i = 0; i < lib->slots_count; i++)
          {
            if(lib->fp_surf[i].imgid == mouseid)
            {
              lib->fp_surf[i].zoom_delta += val;
              break;
            }
          }
        }
        else
        {
          // if global zoom doesn't change (we reach bounds) we may have to move individual values
          if(lib->full_zoom == nz && ((nz == 1.0f && val < 0.0f) || (nz == zmax && val > 0.0f)))
          {
            for(int i = 0; i < lib->slots_count; i++)
            {
              if(lib->fp_surf[i].zoom_delta != 0.0f) lib->fp_surf[i].zoom_delta += val;
            }
          }
          lib->full_zoom = nz;
        }
        // sanitize specific zoomming of individual images
        for(int i = 0; i < lib->slots_count; i++)
        {
          if(lib->full_zoom + lib->fp_surf[i].zoom_delta < 1.0f)
            lib->fp_surf[i].zoom_delta = 1.0f - lib->full_zoom;
          if(lib->full_zoom + lib->fp_surf[i].zoom_delta > lib->fp_surf[i].zoom_100)
            lib->fp_surf[i].zoom_delta = lib->fp_surf[i].zoom_100 - lib->full_zoom;
        }
      }
      else // full preview
      {
        lib->full_zoom = nz;
      }

      // redraw
      dt_control_queue_redraw_center();
    }
    return TRUE;
  }
  return FALSE;
}

void scrolled(dt_view_t *self, double x, double y, int up, int state)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  const dt_lighttable_layout_t layout = get_layout();

  if((lib->full_preview_id > -1 || layout == DT_LIGHTTABLE_LAYOUT_CULLING)
     && (state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK)
  {
    if(up)
      _lighttable_preview_zoom_add(self, 0.5f, x, y, state);
    else
      _lighttable_preview_zoom_add(self, -0.5f, x, y, state);
  }
  else if(lib->full_preview_id > -1)
  {
    if(up)
      lib->track = -DT_LIGHTTABLE_MAX_ZOOM;
    else
      lib->track = +DT_LIGHTTABLE_MAX_ZOOM;

    if(layout == DT_LIGHTTABLE_LAYOUT_CULLING && state == 0) _culling_scroll(lib, up);
  }
  else if(layout == DT_LIGHTTABLE_LAYOUT_CULLING && state == 0)
  {
    _culling_scroll(lib, up);
  }
}

void activate_control_element(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;

  switch(lib->image_over)
  {
    case DT_VIEW_REJECT:
    case DT_VIEW_STAR_1:
    case DT_VIEW_STAR_2:
    case DT_VIEW_STAR_3:
    case DT_VIEW_STAR_4:
    case DT_VIEW_STAR_5:
    {
      const int32_t mouse_over_id = dt_control_get_mouse_over_id();
      dt_ratings_apply_on_image(mouse_over_id, lib->image_over, TRUE, TRUE, TRUE);
      dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD,
                                 g_list_append(NULL, GINT_TO_POINTER(mouse_over_id)));
      break;
    }
    default:
      break;
  }
}

void mouse_moved(dt_view_t *self, double x, double y, double pressure, int which)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  const dt_lighttable_layout_t layout = get_layout();

  // get the max zoom of all images
  const int max_in_memory_images = _get_max_in_memory_images();
  float fz = lib->full_zoom;
  if(lib->pan && layout == DT_LIGHTTABLE_LAYOUT_CULLING && lib->slots_count <= max_in_memory_images)
  {
    for(int i = 0; i < lib->slots_count; i++)
    {
      fz = fmaxf(fz, lib->full_zoom + lib->fp_surf[i].zoom_delta);
    }
  }

  if(lib->pan && (lib->full_preview_id > -1 || layout == DT_LIGHTTABLE_LAYOUT_CULLING) && fz > 1.0f)
  {
    // we want the images to stay in the screen
    if(lib->full_preview_id != -1)
    {
      lib->full_x += x - lib->pan_x;
      lib->full_y += y - lib->pan_y;
      lib->full_x = fminf(lib->full_x, lib->fp_surf[0].max_dx);
      lib->full_x = fmaxf(lib->full_x, -lib->fp_surf[0].max_dx);
      lib->full_y = fminf(lib->full_y, lib->fp_surf[0].max_dy);
      lib->full_y = fmaxf(lib->full_y, -lib->fp_surf[0].max_dy);
    }
    else if(layout == DT_LIGHTTABLE_LAYOUT_CULLING && lib->slots_count <= max_in_memory_images)
    {
      const float valx = x - lib->pan_x;
      const float valy = y - lib->pan_y;

      float xmax = 0.0f;
      float ymax = 0.0f;
      for(int i = 0; i < lib->slots_count; i++)
      {
        xmax = fmaxf(xmax, lib->fp_surf[i].max_dx);
        ymax = fmaxf(ymax, lib->fp_surf[i].max_dy);
      }
      float nx = fminf(xmax, lib->full_x + valx);
      nx = fmaxf(nx, -xmax);
      float ny = fminf(ymax, lib->full_y + valy);
      ny = fmaxf(ny, -ymax);

      if((which & GDK_SHIFT_MASK) == GDK_SHIFT_MASK)
      {
        int mouseid = dt_control_get_mouse_over_id();
        for(int i = 0; i < lib->slots_count; i++)
        {
          if(lib->fp_surf[i].imgid == mouseid)
          {
            lib->fp_surf[i].dx_delta += valx;
            lib->fp_surf[i].dy_delta += valy;
            break;
          }
        }
      }
      else
      {
        // if global position doesn't change (we reach bounds) we may have to move individual values
        if(lib->full_x == nx && ((nx == -xmax && valx < 0.0f) || (nx == xmax && valx > 0.0f)))
        {
          for(int i = 0; i < lib->slots_count; i++)
          {
            if(lib->fp_surf[i].dx_delta != 0.0f) lib->fp_surf[i].dx_delta += valx;
          }
        }
        if(lib->full_y == ny && ((ny == -ymax && valy < 0.0f) || (ny == ymax && valy > 0.0f)))
        {
          for(int i = 0; i < lib->slots_count; i++)
          {
            if(lib->fp_surf[i].dy_delta != 0.0f) lib->fp_surf[i].dy_delta += valy;
          }
        }
        lib->full_x = nx;
        lib->full_y = ny;
      }
      // sanitize specific positions of individual images
      for(int i = 0; i < lib->slots_count; i++)
      {
        if(lib->full_x + lib->fp_surf[i].dx_delta < -lib->fp_surf[i].max_dx)
          lib->fp_surf[i].dx_delta = -lib->fp_surf[i].max_dx - lib->full_x;
        if(lib->full_x + lib->fp_surf[i].dx_delta > lib->fp_surf[i].max_dx)
          lib->fp_surf[i].dx_delta = lib->fp_surf[i].max_dx - lib->full_x;
        if(lib->full_y + lib->fp_surf[i].dy_delta < -lib->fp_surf[i].max_dy)
          lib->fp_surf[i].dy_delta = -lib->fp_surf[i].max_dy - lib->full_y;
        if(lib->full_y + lib->fp_surf[i].dy_delta > lib->fp_surf[i].max_dy)
          lib->fp_surf[i].dy_delta = lib->fp_surf[i].max_dy - lib->full_y;
      }
    }

    lib->pan_x = x;
    lib->pan_y = y;
  }

  if(layout == DT_LIGHTTABLE_LAYOUT_CULLING || lib->full_preview_id > 0) dt_control_queue_redraw_center();
}

int button_released(dt_view_t *self, double x, double y, int which, uint32_t state)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->pan = 0;
  // If a control element was activated by the button press and we decided to
  // defer action until release, then now it's time to act.
  if(lib->activate_on_release != DT_VIEW_ERR)
  {
    if(lib->activate_on_release == lib->image_over)
    {
      activate_control_element(self);
    }
    lib->activate_on_release = DT_VIEW_ERR;
  }
  if(which == 1 || which == GDK_BUTTON1_MASK) dt_control_change_cursor(GDK_LEFT_PTR);
  return 1;
}


static void _audio_child_watch(GPid pid, gint status, gpointer data)
{
  dt_library_t *lib = (dt_library_t *)data;
  lib->audio_player_id = -1;
  g_spawn_close_pid(pid);
}

static void _stop_audio(dt_library_t *lib)
{
  // make sure that the process didn't finish yet and that _audio_child_watch() hasn't run
  if(lib->audio_player_id == -1) return;
  // we don't want to trigger the callback due to a possible race condition
  g_source_remove(lib->audio_player_event_source);
#ifdef _WIN32
// TODO: add Windows code to actually kill the process
#else  // _WIN32
  if(lib->audio_player_id != -1)
  {
    if(getpgid(0) != getpgid(lib->audio_player_pid))
      kill(-lib->audio_player_pid, SIGKILL);
    else
      kill(lib->audio_player_pid, SIGKILL);
  }
#endif // _WIN32
  g_spawn_close_pid(lib->audio_player_pid);
  lib->audio_player_id = -1;
}

int button_pressed(dt_view_t *self, double x, double y, double pressure, int which, int type, uint32_t state)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  const dt_lighttable_layout_t layout = get_layout();

  lib->modifiers = state;
  lib->activate_on_release = DT_VIEW_ERR;

  if(which == 1 && type == GDK_2BUTTON_PRESS) return 0;
  // image button pressed?
  if(which == 1)
  {
    switch(lib->image_over)
    {
      case DT_VIEW_DESERT:
        // Here we begin to pan immediately, even though later we might decide
        // that the event was actually a click. For this reason we do not set
        // the pointer to GDK_HAND1 until we can exclude that it is a click,
        // namely until the pointer has moved a little distance. The code taking
        // care of this is in expose(). Pan only makes sense in zoomable lt.
        if(_is_custom_image_order_actif(self) || layout == DT_LIGHTTABLE_LAYOUT_ZOOMABLE
           || (lib->full_preview_id > -1 && lib->full_zoom > 1.0f))
          begin_pan(lib, x, y);

        // in culling mode, we allow to pan only if one image is zoomed
        if(layout == DT_LIGHTTABLE_LAYOUT_CULLING)
        {
          if(lib->slots_count <= _get_max_in_memory_images())
          {
            for(int i = 0; i < lib->slots_count; i++)
            {
              if(lib->full_zoom + lib->fp_surf[i].zoom_delta > 1.0f)
              {
                begin_pan(lib, x, y);
                break;
              }
            }
          }
        }
      // no break here intentionally
      case DT_VIEW_REJECT:
      case DT_VIEW_STAR_1:
      case DT_VIEW_STAR_2:
      case DT_VIEW_STAR_3:
      case DT_VIEW_STAR_4:
      case DT_VIEW_STAR_5:
        // In file manager we act immediately, in zoomable lt we defer action
        // until either the button is released or the pointer leaves the
        // activated control. In the second case, we cancel the action, and
        // instead we begin to pan. We do this for those users intending to
        // pan that accidentally hit a control element.
        if(layout != DT_LIGHTTABLE_LAYOUT_ZOOMABLE && !_is_custom_image_order_actif(self)) // filemanager/expose
          activate_control_element(self);
        else // zoomable lighttable --> defer action to check for pan
          lib->activate_on_release = lib->image_over;
        break;

      case DT_VIEW_GROUP:
      {
        const int32_t mouse_over_id = dt_control_get_mouse_over_id();
        const dt_image_t *image = dt_image_cache_get(darktable.image_cache, mouse_over_id, 'r');
        if(!image) return 0;
        const int group_id = image->group_id;
        const int id = image->id;
        dt_image_cache_read_release(darktable.image_cache, image);

        if(state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)) // just add the whole group to the selection. TODO:
                                                        // make this also work for collapsed groups.
        {
          sqlite3_stmt *stmt;
          DT_DEBUG_SQLITE3_PREPARE_V2(
              dt_database_get(darktable.db),
              "INSERT OR IGNORE INTO main.selected_images SELECT id FROM main.images WHERE group_id = ?1",
              -1, &stmt, NULL);
          DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, group_id);
          sqlite3_step(stmt);
          sqlite3_finalize(stmt);
        }
        else if(group_id == darktable.gui->expanded_group_id) // the group is already expanded, so ...
        {
          if(id == darktable.gui->expanded_group_id) // ... collapse it
            darktable.gui->expanded_group_id = -1;
          else // ... make the image the new representative of the group
            darktable.gui->expanded_group_id = dt_grouping_change_representative(id);
        }
        else // expand the group
          darktable.gui->expanded_group_id = group_id;
        dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD,
                                   g_list_append(NULL, GINT_TO_POINTER(id)));
        break;
      }
      case DT_VIEW_AUDIO:
      {
        const int32_t mouse_over_id = dt_control_get_mouse_over_id();
        gboolean start_audio = TRUE;
        if(lib->audio_player_id != -1)
        {
          // don't start the audio for the image we just killed it for
          if(lib->audio_player_id == mouse_over_id) start_audio = FALSE;

          _stop_audio(lib);
        }

        if(start_audio)
        {
          // if no audio is played at the moment -> play audio
          char *player = dt_conf_get_string("plugins/lighttable/audio_player");
          if(player && *player)
          {
            char *filename = dt_image_get_audio_path(mouse_over_id);
            if(filename)
            {
              char *argv[] = { player, filename, NULL };
              gboolean ret
                  = g_spawn_async(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH
                                                    | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                                  NULL, NULL, &lib->audio_player_pid, NULL);

              if(ret)
              {
                lib->audio_player_id = mouse_over_id;
                lib->audio_player_event_source
                    = g_child_watch_add(lib->audio_player_pid, (GChildWatchFunc)_audio_child_watch, lib);
              }
              else
                lib->audio_player_id = -1;

              g_free(filename);
            }
          }
          g_free(player);
        }

        break;
      }
      default:
        begin_pan(lib, x, y);
        dt_control_change_cursor(GDK_HAND1);
        return 0;
    }
  }
  return 1;
}

int key_released(dt_view_t *self, guint key, guint state)
{
  dt_control_accels_t *accels = &darktable.control->accels;
  dt_library_t *lib = (dt_library_t *)self->data;

  if(!darktable.control->key_accelerators_on) return 0;

  // we need a full expose
  if((key == accels->global_sideborders.accel_key
      && state == accels->global_sideborders.accel_mods)           // hide/show sideborders,
     || (key == accels->lighttable_timeline.accel_key
         && state == accels->lighttable_timeline.accel_mods)       // hide/show timeline,
     || (key == accels->global_focus_peaking.accel_key
         && (state == accels->global_focus_peaking.accel_mods)))   // hide/show focus peaking
  {
    _force_expose_all(self);
  }

  if(((key == accels->lighttable_preview.accel_key && state == accels->lighttable_preview.accel_mods)
      || (key == accels->lighttable_preview_display_focus.accel_key
          && state == accels->lighttable_preview_display_focus.accel_mods))
     && lib->full_preview_id != -1 && !lib->full_preview_sticky)
  {
    _preview_quit(self);
  }

  return 1;
}

int key_pressed(dt_view_t *self, guint key, guint state)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  dt_control_accels_t *accels = &darktable.control->accels;

  if(!darktable.control->key_accelerators_on) return 0;

  const dt_lighttable_layout_t layout = get_layout();

  if((key == accels->lighttable_preview.accel_key && state == accels->lighttable_preview.accel_mods)
     || (key == accels->lighttable_preview_display_focus.accel_key
         && state == accels->lighttable_preview_display_focus.accel_mods))
  {
    if(lib->full_preview_id > 0 && lib->full_preview_sticky)
    {
      _preview_quit(self);
      return TRUE;
    }
    const int32_t mouse_over_id = dt_control_get_mouse_over_id();
    if(lib->full_preview_id == -1 && mouse_over_id != -1)
    {
      gboolean focus = FALSE;
      if((key == accels->lighttable_preview_display_focus.accel_key
          && state == accels->lighttable_preview_display_focus.accel_mods))
      {
        focus = TRUE;
      }

      _preview_enter(self, FALSE, focus, mouse_over_id);
      return 1;
    }
    return 0;
  }

  // navigation accels for thumbtable layouts
  // this can't be "normal" key accels because it's usually arrow keys and lot of other widgets
  // will capture them before the usual accel is triggered
  if(lib->full_preview_id < 0
     && (layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER || layout == DT_LIGHTTABLE_LAYOUT_ZOOMABLE))
  {
    dt_thumbtable_move_t move = DT_THUMBTABLE_MOVE_NONE;
    gboolean select = FALSE;
    if(key == accels->lighttable_left.accel_key && state == accels->lighttable_left.accel_mods)
      move = DT_THUMBTABLE_MOVE_LEFT;
    else if(key == accels->lighttable_up.accel_key && state == accels->lighttable_up.accel_mods)
      move = DT_THUMBTABLE_MOVE_UP;
    else if(key == accels->lighttable_right.accel_key && state == accels->lighttable_right.accel_mods)
      move = DT_THUMBTABLE_MOVE_RIGHT;
    else if(key == accels->lighttable_down.accel_key && state == accels->lighttable_down.accel_mods)
      move = DT_THUMBTABLE_MOVE_DOWN;
    else if(key == accels->lighttable_pageup.accel_key && state == accels->lighttable_pageup.accel_mods)
      move = DT_THUMBTABLE_MOVE_PAGEUP;
    else if(key == accels->lighttable_pagedown.accel_key && state == accels->lighttable_pagedown.accel_mods)
      move = DT_THUMBTABLE_MOVE_PAGEDOWN;
    else if(key == accels->lighttable_start.accel_key && state == accels->lighttable_start.accel_mods)
      move = DT_THUMBTABLE_MOVE_START;
    else if(key == accels->lighttable_end.accel_key && state == accels->lighttable_end.accel_mods)
      move = DT_THUMBTABLE_MOVE_END;
    else
    {
      select = TRUE;
      if(key == accels->lighttable_sel_left.accel_key && state == accels->lighttable_sel_left.accel_mods)
        move = DT_THUMBTABLE_MOVE_LEFT;
      else if(key == accels->lighttable_sel_up.accel_key && state == accels->lighttable_sel_up.accel_mods)
        move = DT_THUMBTABLE_MOVE_UP;
      else if(key == accels->lighttable_sel_right.accel_key && state == accels->lighttable_sel_right.accel_mods)
        move = DT_THUMBTABLE_MOVE_RIGHT;
      else if(key == accels->lighttable_sel_down.accel_key && state == accels->lighttable_sel_down.accel_mods)
        move = DT_THUMBTABLE_MOVE_DOWN;
      else if(key == accels->lighttable_sel_pageup.accel_key && state == accels->lighttable_sel_pageup.accel_mods)
        move = DT_THUMBTABLE_MOVE_PAGEUP;
      else if(key == accels->lighttable_sel_pagedown.accel_key
              && state == accels->lighttable_sel_pagedown.accel_mods)
        move = DT_THUMBTABLE_MOVE_PAGEDOWN;
      else if(key == accels->lighttable_sel_start.accel_key && state == accels->lighttable_sel_start.accel_mods)
        move = DT_THUMBTABLE_MOVE_START;
      else if(key == accels->lighttable_sel_end.accel_key && state == accels->lighttable_sel_end.accel_mods)
        move = DT_THUMBTABLE_MOVE_END;
    }

    if(move != DT_THUMBTABLE_MOVE_NONE)
    {
      // for this layout navigation keys are managed directly by thumbtable
      dt_thumbtable_key_move(dt_ui_thumbtable(darktable.gui->ui), move, select);
      return TRUE;
    }
  }

  else if(lib->full_preview_id > 0)
  {
    if((key == accels->lighttable_left.accel_key && state == accels->lighttable_left.accel_mods)
       || (key == accels->lighttable_up.accel_key && state == accels->lighttable_up.accel_mods)
       || (key == accels->lighttable_pageup.accel_key && state == accels->lighttable_pageup.accel_mods))
    {
      lib->track = -1;
      if(layout == DT_LIGHTTABLE_LAYOUT_CULLING) _culling_scroll(lib, TRUE);
      dt_control_queue_redraw_center();
      return TRUE;
    }
    else if((key == accels->lighttable_right.accel_key && state == accels->lighttable_right.accel_mods)
            || (key == accels->lighttable_down.accel_key && state == accels->lighttable_down.accel_mods)
            || (key == accels->lighttable_pagedown.accel_key && state == accels->lighttable_pagedown.accel_mods))
    {
      lib->track = +1;
      if(layout == DT_LIGHTTABLE_LAYOUT_CULLING) _culling_scroll(lib, FALSE);
      return TRUE;
    }
    else if(key == accels->lighttable_start.accel_key && state == accels->lighttable_start.accel_mods)
    {
      // TODO
      return TRUE;
    }
    else if(key == accels->lighttable_end.accel_key && state == accels->lighttable_end.accel_mods)
    {
      // TODO
      return TRUE;
    }
  }

  else if(layout == DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    if((key == accels->lighttable_left.accel_key && state == accels->lighttable_left.accel_mods)
       || (key == accels->lighttable_up.accel_key && state == accels->lighttable_up.accel_mods))
    {
      lib->track = -1;
      _culling_scroll(lib, TRUE);
      return TRUE;
    }
    else if((key == accels->lighttable_right.accel_key && state == accels->lighttable_right.accel_mods)
            || (key == accels->lighttable_down.accel_key && state == accels->lighttable_down.accel_mods))
    {
      lib->track = 1;
      _culling_scroll(lib, FALSE);
      return TRUE;
    }
    else if(key == accels->lighttable_pageup.accel_key && state == accels->lighttable_pageup.accel_mods)
    {
      _culling_move_pageup(self);
      return TRUE;
    }
    else if(key == accels->lighttable_pagedown.accel_key && state == accels->lighttable_pagedown.accel_mods)
    {
      _culling_move_pagedown(self);
      return TRUE;
    }
    else if(key == accels->lighttable_start.accel_key && state == accels->lighttable_start.accel_mods)
    {
      _culling_move_start(self);
      return TRUE;
    }
    else if(key == accels->lighttable_end.accel_key && state == accels->lighttable_end.accel_mods)
    {
      _culling_move_end(self);
      return TRUE;
    }
  }
  return 0;
}

static gboolean zoom_in_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                           GdkModifierType modifier, gpointer data)
{
  int zoom = get_zoom();

  zoom--;
  if(zoom < 1) zoom = 1;

  dt_view_lighttable_set_zoom(darktable.view_manager, zoom);
  return TRUE;
}

static gboolean zoom_out_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                           GdkModifierType modifier, gpointer data)
{
  int zoom = get_zoom();

  zoom++;
  if(zoom > 2 * DT_LIGHTTABLE_MAX_ZOOM) zoom = 2 * DT_LIGHTTABLE_MAX_ZOOM;

  dt_view_lighttable_set_zoom(darktable.view_manager, zoom);
  return TRUE;
}

void init_key_accels(dt_view_t *self)
{
  // movement keys
  dt_accel_register_view(self, NC_("accel", "move page up"), GDK_KEY_Page_Up, 0);
  dt_accel_register_view(self, NC_("accel", "move page down"), GDK_KEY_Page_Down, 0);
  dt_accel_register_view(self, NC_("accel", "move up"), GDK_KEY_Up, 0);
  dt_accel_register_view(self, NC_("accel", "move down"), GDK_KEY_Down, 0);
  dt_accel_register_view(self, NC_("accel", "move left"), GDK_KEY_Left, 0);
  dt_accel_register_view(self, NC_("accel", "move right"), GDK_KEY_Right, 0);
  dt_accel_register_view(self, NC_("accel", "move start"), GDK_KEY_Home, 0);
  dt_accel_register_view(self, NC_("accel", "move end"), GDK_KEY_End, 0);

  // movement keys with selection
  dt_accel_register_view(self, NC_("accel", "move page up and select"), GDK_KEY_Page_Up, GDK_SHIFT_MASK);
  dt_accel_register_view(self, NC_("accel", "move page down and select"), GDK_KEY_Page_Down, GDK_SHIFT_MASK);
  dt_accel_register_view(self, NC_("accel", "move up and select"), GDK_KEY_Up, GDK_SHIFT_MASK);
  dt_accel_register_view(self, NC_("accel", "move down and select"), GDK_KEY_Down, GDK_SHIFT_MASK);
  dt_accel_register_view(self, NC_("accel", "move left and select"), GDK_KEY_Left, GDK_SHIFT_MASK);
  dt_accel_register_view(self, NC_("accel", "move right and select"), GDK_KEY_Right, GDK_SHIFT_MASK);
  dt_accel_register_view(self, NC_("accel", "move start and select"), GDK_KEY_Home, GDK_SHIFT_MASK);
  dt_accel_register_view(self, NC_("accel", "move end and select"), GDK_KEY_End, GDK_SHIFT_MASK);

  dt_accel_register_view(self, NC_("accel", "align images to grid"), 0, 0);
  dt_accel_register_view(self, NC_("accel", "reset first image offset"), 0, 0);
  dt_accel_register_view(self, NC_("accel", "select toggle image"), GDK_KEY_space, 0);
  dt_accel_register_view(self, NC_("accel", "select single image"), GDK_KEY_Return, 0);

  // Preview key
  dt_accel_register_view(self, NC_("accel", "preview"), GDK_KEY_w, 0);
  dt_accel_register_view(self, NC_("accel", "preview with focus detection"), GDK_KEY_w, GDK_CONTROL_MASK);
  dt_accel_register_view(self, NC_("accel", "sticky preview"), GDK_KEY_w, GDK_MOD1_MASK);
  dt_accel_register_view(self, NC_("accel", "sticky preview with focus detection"), GDK_KEY_w,
                         GDK_MOD1_MASK | GDK_CONTROL_MASK);

  // undo/redo
  dt_accel_register_view(self, NC_("accel", "undo"), GDK_KEY_z, GDK_CONTROL_MASK);
  dt_accel_register_view(self, NC_("accel", "redo"), GDK_KEY_y, GDK_CONTROL_MASK);

  // zoom for full preview
  dt_accel_register_view(self, NC_("accel", "preview zoom 100%"), 0, 0);
  dt_accel_register_view(self, NC_("accel", "preview zoom fit"), 0, 0);

  // zoom in/out
  dt_accel_register_view(self, NC_("accel", "zoom in"), GDK_KEY_plus, GDK_CONTROL_MASK);
  dt_accel_register_view(self, NC_("accel", "zoom out"), GDK_KEY_minus, GDK_CONTROL_MASK);
}

static gboolean _lighttable_undo_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                          GdkModifierType modifier, gpointer data)
{
  dt_undo_do_undo(darktable.undo, DT_UNDO_LIGHTTABLE);
  return TRUE;
}

static gboolean _lighttable_redo_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                          GdkModifierType modifier, gpointer data)
{
  dt_undo_do_redo(darktable.undo, DT_UNDO_LIGHTTABLE);
  return TRUE;
}

static gboolean _lighttable_preview_zoom_100(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                             GdkModifierType modifier, gpointer data)
{
  return _lighttable_preview_zoom_add(darktable.view_manager->proxy.lighttable.view, 100.0f, -1, -1, 0);
}

static gboolean _lighttable_preview_zoom_fit(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                             GdkModifierType modifier, gpointer data)
{
  dt_view_t *self = darktable.view_manager->proxy.lighttable.view;
  dt_library_t *lib = (dt_library_t *)self->data;

  if(lib->full_preview_id > -1 || get_layout() == DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    lib->full_zoom = 1.0f;
    lib->full_x = 0;
    lib->full_y = 0;
    dt_control_queue_redraw_center();
    return TRUE;
  }

  return FALSE;
}

static gboolean _accel_align_to_grid(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                     GdkModifierType modifier, gpointer data)
{
  const dt_lighttable_layout_t layout = get_layout();

  if(layout == DT_LIGHTTABLE_LAYOUT_ZOOMABLE)
  {
    return dt_thumbtable_key_move(dt_ui_thumbtable(darktable.gui->ui), DT_THUMBTABLE_MOVE_ALIGN, FALSE);
  }
  return FALSE;
}
static gboolean _accel_reset_first_offset(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                          GdkModifierType modifier, gpointer data)
{
  const dt_lighttable_layout_t layout = get_layout();

  if(layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER || layout == DT_LIGHTTABLE_LAYOUT_ZOOMABLE)
  {
    return dt_thumbtable_reset_first_offset(dt_ui_thumbtable(darktable.gui->ui));
  }
  return FALSE;
}

static gboolean _accel_sticky_preview(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                      GdkModifierType modifier, gpointer data)
{
  dt_view_t *self = darktable.view_manager->proxy.lighttable.view;
  dt_library_t *lib = (dt_library_t *)self->data;

  // if we are alredy in preview mode, we exit
  if(lib->full_preview_id > 0)
  {
    _preview_quit(self);
    return TRUE;
  }

  const int focus = GPOINTER_TO_INT(data);
  const int mouse_over_id = dt_control_get_mouse_over_id();
  if(mouse_over_id < 1) return TRUE;
  _preview_enter(self, TRUE, focus, mouse_over_id);

  return TRUE;
}

void connect_key_accels(dt_view_t *self)
{
  GClosure *closure;

  // Navigation keys
  closure = g_cclosure_new(G_CALLBACK(select_toggle_callback), (gpointer)self, NULL);
  dt_accel_connect_view(self, "select toggle image", closure);
  closure = g_cclosure_new(G_CALLBACK(select_single_callback), (gpointer)self, NULL);
  dt_accel_connect_view(self, "select single image", closure);
  closure = g_cclosure_new(G_CALLBACK(_accel_align_to_grid), (gpointer)self, NULL);
  dt_accel_connect_view(self, "align images to grid", closure);
  closure = g_cclosure_new(G_CALLBACK(_accel_reset_first_offset), (gpointer)self, NULL);
  dt_accel_connect_view(self, "reset first image offset", closure);

  // undo/redo
  closure = g_cclosure_new(G_CALLBACK(_lighttable_undo_callback), (gpointer)self, NULL);
  dt_accel_connect_view(self, "undo", closure);
  closure = g_cclosure_new(G_CALLBACK(_lighttable_redo_callback), (gpointer)self, NULL);
  dt_accel_connect_view(self, "redo", closure);

  // sticky preview (non sticky is managed inside key_pressed)
  closure = g_cclosure_new(G_CALLBACK(_accel_sticky_preview), GINT_TO_POINTER(FALSE), NULL);
  dt_accel_connect_view(self, "sticky preview", closure);
  closure = g_cclosure_new(G_CALLBACK(_accel_sticky_preview), GINT_TO_POINTER(TRUE), NULL);
  dt_accel_connect_view(self, "sticky preview with focus detection", closure);

  // full_preview zoom
  closure = g_cclosure_new(G_CALLBACK(_lighttable_preview_zoom_100), (gpointer)self, NULL);
  dt_accel_connect_view(self, "preview zoom 100%", closure);
  closure = g_cclosure_new(G_CALLBACK(_lighttable_preview_zoom_fit), (gpointer)self, NULL);
  dt_accel_connect_view(self, "preview zoom fit", closure);

  // zoom in/out
  closure = g_cclosure_new(G_CALLBACK(zoom_in_callback), (gpointer)self, NULL);
  dt_accel_connect_view(self, "zoom in", closure);
  closure = g_cclosure_new(G_CALLBACK(zoom_out_callback), (gpointer)self, NULL);
  dt_accel_connect_view(self, "zoom out", closure);
}

GSList *mouse_actions(const dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  GSList *lm = NULL;
  dt_mouse_action_t *a = NULL;

  a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
  a->action = DT_MOUSE_ACTION_DOUBLE_LEFT;
  g_strlcpy(a->name, _("open image in darkroom"), sizeof(a->name));
  lm = g_slist_append(lm, a);

  if(lib->full_preview_id >= 0)
  {
    a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
    a->action = DT_MOUSE_ACTION_SCROLL;
    g_strlcpy(a->name, _("switch to next/previous image"), sizeof(a->name));
    lm = g_slist_append(lm, a);

    a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
    a->key.accel_mods = GDK_CONTROL_MASK;
    a->action = DT_MOUSE_ACTION_SCROLL;
    g_strlcpy(a->name, _("zoom in the image"), sizeof(a->name));
    lm = g_slist_append(lm, a);
  }
  else if(lib->current_layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
  {
    a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
    a->action = DT_MOUSE_ACTION_SCROLL;
    g_strlcpy(a->name, _("scroll the collection"), sizeof(a->name));
    lm = g_slist_append(lm, a);

    a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
    a->key.accel_mods = GDK_CONTROL_MASK;
    a->action = DT_MOUSE_ACTION_SCROLL;
    g_strlcpy(a->name, _("change number of images per row"), sizeof(a->name));
    lm = g_slist_append(lm, a);

    if(_is_custom_image_order_actif(self))
    {
      a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
      a->key.accel_mods = GDK_BUTTON1_MASK;
      a->action = DT_MOUSE_ACTION_DRAG_DROP;
      g_strlcpy(a->name, _("change image order"), sizeof(a->name));
      lm = g_slist_append(lm, a);
    }
  }
  else if(lib->current_layout == DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
    a->action = DT_MOUSE_ACTION_SCROLL;
    g_strlcpy(a->name, _("scroll the collection"), sizeof(a->name));
    lm = g_slist_append(lm, a);

    a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
    a->key.accel_mods = GDK_CONTROL_MASK;
    a->action = DT_MOUSE_ACTION_SCROLL;
    g_strlcpy(a->name, _("zoom all the images"), sizeof(a->name));
    lm = g_slist_append(lm, a);

    a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
    a->action = DT_MOUSE_ACTION_LEFT_DRAG;
    g_strlcpy(a->name, _("pan inside all the images"), sizeof(a->name));
    lm = g_slist_append(lm, a);

    a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
    a->key.accel_mods = GDK_CONTROL_MASK | GDK_SHIFT_MASK;
    a->action = DT_MOUSE_ACTION_SCROLL;
    g_strlcpy(a->name, _("zoom current image"), sizeof(a->name));
    lm = g_slist_append(lm, a);

    a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
    a->key.accel_mods = GDK_SHIFT_MASK;
    a->action = DT_MOUSE_ACTION_LEFT_DRAG;
    g_strlcpy(a->name, _("pan inside current image"), sizeof(a->name));
    lm = g_slist_append(lm, a);
  }
  else if(lib->current_layout == DT_LIGHTTABLE_LAYOUT_ZOOMABLE)
  {
    a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
    a->action = DT_MOUSE_ACTION_SCROLL;
    g_strlcpy(a->name, _("zoom the main view"), sizeof(a->name));
    lm = g_slist_append(lm, a);

    a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
    a->action = DT_MOUSE_ACTION_LEFT_DRAG;
    g_strlcpy(a->name, _("pan inside the main view"), sizeof(a->name));
    lm = g_slist_append(lm, a);
  }

  return lm;
}

static void display_intent_callback(GtkWidget *combo, gpointer user_data)
{
  const int pos = dt_bauhaus_combobox_get(combo);

  dt_iop_color_intent_t new_intent = darktable.color_profiles->display_intent;

  // we are not using the int value directly so it's robust against changes on lcms' side
  switch(pos)
  {
    case 0:
      new_intent = DT_INTENT_PERCEPTUAL;
      break;
    case 1:
      new_intent = DT_INTENT_RELATIVE_COLORIMETRIC;
      break;
    case 2:
      new_intent = DT_INTENT_SATURATION;
      break;
    case 3:
      new_intent = DT_INTENT_ABSOLUTE_COLORIMETRIC;
      break;
  }

  if(new_intent != darktable.color_profiles->display_intent)
  {
    darktable.color_profiles->display_intent = new_intent;
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);
    dt_colorspaces_update_display_transforms();
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
    dt_control_queue_redraw_center();
  }
}

static void display2_intent_callback(GtkWidget *combo, gpointer user_data)
{
  const int pos = dt_bauhaus_combobox_get(combo);

  dt_iop_color_intent_t new_intent = darktable.color_profiles->display2_intent;

  // we are not using the int value directly so it's robust against changes on lcms' side
  switch(pos)
  {
    case 0:
      new_intent = DT_INTENT_PERCEPTUAL;
      break;
    case 1:
      new_intent = DT_INTENT_RELATIVE_COLORIMETRIC;
      break;
    case 2:
      new_intent = DT_INTENT_SATURATION;
      break;
    case 3:
      new_intent = DT_INTENT_ABSOLUTE_COLORIMETRIC;
      break;
  }

  if(new_intent != darktable.color_profiles->display2_intent)
  {
    darktable.color_profiles->display2_intent = new_intent;
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);
    dt_colorspaces_update_display2_transforms();
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
    dt_control_queue_redraw_center();
  }
}

static void display_profile_callback(GtkWidget *combo, gpointer user_data)
{
  gboolean profile_changed = FALSE;
  const int pos = dt_bauhaus_combobox_get(combo);
  for(GList *profiles = darktable.color_profiles->profiles; profiles; profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)profiles->data;
    if(pp->display_pos == pos)
    {
      if(darktable.color_profiles->display_type != pp->type
        || (darktable.color_profiles->display_type == DT_COLORSPACE_FILE
        && strcmp(darktable.color_profiles->display_filename, pp->filename)))
      {
        darktable.color_profiles->display_type = pp->type;
        g_strlcpy(darktable.color_profiles->display_filename, pp->filename,
                  sizeof(darktable.color_profiles->display_filename));
        profile_changed = TRUE;
      }
      goto end;
    }
  }

  // profile not found, fall back to system display profile. shouldn't happen
  fprintf(stderr, "can't find display profile `%s', using system display profile instead\n", dt_bauhaus_combobox_get_text(combo));
  profile_changed = darktable.color_profiles->display_type != DT_COLORSPACE_DISPLAY;
  darktable.color_profiles->display_type = DT_COLORSPACE_DISPLAY;
  darktable.color_profiles->display_filename[0] = '\0';

end:
  if(profile_changed)
  {
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);
    dt_colorspaces_update_display_transforms();
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                            DT_COLORSPACES_PROFILE_TYPE_DISPLAY);
    dt_control_queue_redraw_center();
  }
}

static void display2_profile_callback(GtkWidget *combo, gpointer user_data)
{
  gboolean profile_changed = FALSE;
  const int pos = dt_bauhaus_combobox_get(combo);
  for(GList *profiles = darktable.color_profiles->profiles; profiles; profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)profiles->data;
    if(pp->display2_pos == pos)
    {
      if(darktable.color_profiles->display2_type != pp->type
         || (darktable.color_profiles->display2_type == DT_COLORSPACE_FILE
             && strcmp(darktable.color_profiles->display2_filename, pp->filename)))
      {
        darktable.color_profiles->display2_type = pp->type;
        g_strlcpy(darktable.color_profiles->display2_filename, pp->filename,
                  sizeof(darktable.color_profiles->display2_filename));
        profile_changed = TRUE;
      }
      goto end;
    }
  }

  // profile not found, fall back to system display2 profile. shouldn't happen
  fprintf(stderr, "can't find preview display profile `%s', using system display profile instead\n",
          dt_bauhaus_combobox_get_text(combo));
  profile_changed = darktable.color_profiles->display2_type != DT_COLORSPACE_DISPLAY2;
  darktable.color_profiles->display2_type = DT_COLORSPACE_DISPLAY2;
  darktable.color_profiles->display2_filename[0] = '\0';

end:
  if(profile_changed)
  {
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);
    dt_colorspaces_update_display2_transforms();
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                            DT_COLORSPACES_PROFILE_TYPE_DISPLAY2);
    dt_control_queue_redraw_center();
  }
}

static void _update_display_profile_cmb(GtkWidget *cmb_display_profile)
{
  GList *l = darktable.color_profiles->profiles;
  while(l)
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)l->data;
    if(prof->display_pos > -1)
    {
      if(prof->type == darktable.color_profiles->display_type
         && (prof->type != DT_COLORSPACE_FILE
             || !strcmp(prof->filename, darktable.color_profiles->display_filename)))
      {
        if(dt_bauhaus_combobox_get(cmb_display_profile) != prof->display_pos)
        {
          dt_bauhaus_combobox_set(cmb_display_profile, prof->display_pos);
          break;
        }
      }
    }
    l = g_list_next(l);
  }
}

static void _update_display2_profile_cmb(GtkWidget *cmb_display_profile)
{
  GList *l = darktable.color_profiles->profiles;
  while(l)
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)l->data;
    if(prof->display2_pos > -1)
    {
      if(prof->type == darktable.color_profiles->display2_type
         && (prof->type != DT_COLORSPACE_FILE
             || !strcmp(prof->filename, darktable.color_profiles->display2_filename)))
      {
        if(dt_bauhaus_combobox_get(cmb_display_profile) != prof->display2_pos)
        {
          dt_bauhaus_combobox_set(cmb_display_profile, prof->display2_pos);
          break;
        }
      }
    }
    l = g_list_next(l);
  }
}

static void _display_profile_changed(gpointer instance, uint8_t profile_type, gpointer user_data)
{
  GtkWidget *cmb_display_profile = GTK_WIDGET(user_data);

  _update_display_profile_cmb(cmb_display_profile);
}

static void _display2_profile_changed(gpointer instance, uint8_t profile_type, gpointer user_data)
{
  GtkWidget *cmb_display_profile = GTK_WIDGET(user_data);

  _update_display2_profile_cmb(cmb_display_profile);
}

void gui_init(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;

  // create display profile button
  GtkWidget *const profile_button = dtgtk_button_new(dtgtk_cairo_paint_display, CPF_STYLE_FLAT,
                                                     NULL);
  gtk_widget_set_tooltip_text(profile_button, _("set display profile"));
  dt_view_manager_module_toolbox_add(darktable.view_manager, profile_button, DT_VIEW_LIGHTTABLE);

  // and the popup window
  lib->profile_floating_window = gtk_popover_new(profile_button);

  gtk_widget_set_size_request(GTK_WIDGET(lib->profile_floating_window), 350, -1);
#if GTK_CHECK_VERSION(3, 16, 0)
  g_object_set(G_OBJECT(lib->profile_floating_window), "transitions-enabled", FALSE, NULL);
#endif
  g_signal_connect_swapped(G_OBJECT(profile_button), "button-press-event", G_CALLBACK(gtk_widget_show_all), lib->profile_floating_window);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  gtk_container_add(GTK_CONTAINER(lib->profile_floating_window), vbox);

  /** let's fill the encapsulating widgets */
  char datadir[PATH_MAX] = { 0 };
  char confdir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(confdir, sizeof(confdir));
  dt_loc_get_datadir(datadir, sizeof(datadir));

  GtkWidget *display_intent = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(display_intent, NULL, _("display intent"));
  gtk_box_pack_start(GTK_BOX(vbox), display_intent, TRUE, TRUE, 0);
  dt_bauhaus_combobox_add(display_intent, _("perceptual"));
  dt_bauhaus_combobox_add(display_intent, _("relative colorimetric"));
  dt_bauhaus_combobox_add(display_intent, C_("rendering intent", "saturation"));
  dt_bauhaus_combobox_add(display_intent, _("absolute colorimetric"));

  GtkWidget *display2_intent = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(display2_intent, NULL, _("preview display intent"));
  gtk_box_pack_start(GTK_BOX(vbox), display2_intent, TRUE, TRUE, 0);
  dt_bauhaus_combobox_add(display2_intent, _("perceptual"));
  dt_bauhaus_combobox_add(display2_intent, _("relative colorimetric"));
  dt_bauhaus_combobox_add(display2_intent, C_("rendering intent", "saturation"));
  dt_bauhaus_combobox_add(display2_intent, _("absolute colorimetric"));

  GtkWidget *display_profile = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(display_profile, NULL, _("display profile"));
  gtk_box_pack_start(GTK_BOX(vbox), display_profile, TRUE, TRUE, 0);

  GtkWidget *display2_profile = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(display2_profile, NULL, _("preview display profile"));
  gtk_box_pack_start(GTK_BOX(vbox), display2_profile, TRUE, TRUE, 0);

  for(GList *profiles = darktable.color_profiles->profiles; profiles; profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)profiles->data;
    if(prof->display_pos > -1)
    {
      dt_bauhaus_combobox_add(display_profile, prof->name);
      if(prof->type == darktable.color_profiles->display_type
        && (prof->type != DT_COLORSPACE_FILE
        || !strcmp(prof->filename, darktable.color_profiles->display_filename)))
      {
        dt_bauhaus_combobox_set(display_profile, prof->display_pos);
      }
    }
    if(prof->display2_pos > -1)
    {
      dt_bauhaus_combobox_add(display2_profile, prof->name);
      if(prof->type == darktable.color_profiles->display2_type
         && (prof->type != DT_COLORSPACE_FILE
             || !strcmp(prof->filename, darktable.color_profiles->display2_filename)))
      {
        dt_bauhaus_combobox_set(display2_profile, prof->display2_pos);
      }
    }
  }

  char *system_profile_dir = g_build_filename(datadir, "color", "out", NULL);
  char *user_profile_dir = g_build_filename(confdir, "color", "out", NULL);
  char *tooltip = g_strdup_printf(_("display ICC profiles in %s or %s"), user_profile_dir, system_profile_dir);
  gtk_widget_set_tooltip_text(display_profile, tooltip);
  g_free(tooltip);
  tooltip = g_strdup_printf(_("preview display ICC profiles in %s or %s"), user_profile_dir, system_profile_dir);
  gtk_widget_set_tooltip_text(display2_profile, tooltip);
  g_free(tooltip);
  g_free(system_profile_dir);
  g_free(user_profile_dir);

  g_signal_connect(G_OBJECT(display_intent), "value-changed", G_CALLBACK(display_intent_callback), NULL);
  g_signal_connect(G_OBJECT(display_profile), "value-changed", G_CALLBACK(display_profile_callback), NULL);

  g_signal_connect(G_OBJECT(display2_intent), "value-changed", G_CALLBACK(display2_intent_callback), NULL);
  g_signal_connect(G_OBJECT(display2_profile), "value-changed", G_CALLBACK(display2_profile_callback), NULL);

  // update the gui when profiles change
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                            G_CALLBACK(_display_profile_changed), (gpointer)display_profile);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                            G_CALLBACK(_display2_profile_changed), (gpointer)display2_profile);

  // proxy
  darktable.view_manager->proxy.lighttable.force_expose_all = _force_expose_all;
}

static gboolean _is_order_actif(const dt_view_t *self, dt_collection_sort_t sort)
{
  if (darktable.gui)
  {
    const dt_lighttable_layout_t layout = get_layout();

    // only in file manager
    // only in light table
    // only if custom image order is selected
    dt_view_t *current_view = darktable.view_manager->current_view;
    if (layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER
        && darktable.collection->params.sort == sort
        && current_view
        && current_view->view(self) == DT_VIEW_LIGHTTABLE)
    {
      // not in full_preview mode
      dt_library_t *lib = (dt_library_t *)self->data;
      if(lib->full_preview_id == -1) return TRUE;
    }
  }

  return FALSE;
}

static gboolean _is_custom_image_order_actif(const dt_view_t *self)
{
  return _is_order_actif(self, DT_COLLECTION_SORT_CUSTOM_ORDER);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
