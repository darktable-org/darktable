/*
    This file is part of darktable,
    Copyright (C) 2010-2020 darktable developers.

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
#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image_cache.h"
#include "common/ratings.h"
#include "common/colorlabels.h"
#include "common/grouping.h"
#include "common/undo.h"
#include "common/metadata.h"
#include "common/tags.h"
#include "control/control.h"
#include "control/jobs.h"
#include "control/jobs/control_jobs.h"
#include "dtgtk/button.h"
#include "dtgtk/paint.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <stdlib.h>
#ifdef USE_LUA
#include "lua/call.h"
#include "lua/image.h"
#endif
#include "libs/lib_api.h"

DT_MODULE(1)

typedef struct dt_lib_image_t
{
  GtkWidget *rotate_cw_button, *rotate_ccw_button, *remove_button, *delete_button, *create_hdr_button,
      *duplicate_button, *reset_button, *move_button, *copy_button, *group_button, *ungroup_button,
      *cache_button, *uncache_button, *refresh_button,
      *set_monochrome_button, *set_color_button,
      *copy_metadata_button, *paste_metadata_button, *clear_metadata_button,
      *ratings_flag, *colors_flag, *metadata_flag, *geotags_flag, *tags_flag;
  GtkWidget *page1; // saved here for lua extensions
  int imageid;
} dt_lib_image_t;

typedef enum dt_lib_metadata_id
{
  DT_LIB_META_NONE = 0,
  DT_LIB_META_RATING = 1 << 0,
  DT_LIB_META_COLORS = 1 << 1,
  DT_LIB_META_METADATA = 1 << 2,
  DT_LIB_META_GEOTAG = 1 << 3,
  DT_LIB_META_TAG = 1 << 4
} dt_lib_metadata_id;

const char *name(dt_lib_module_t *self)
{
  return _("selected image[s]");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"lighttable", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

/** merges all the selected images into a single group.
 * if there is an expanded group, then they will be joined there, otherwise a new one will be created. */
static void _group_helper_function(void)
{
  int new_group_id = darktable.gui->expanded_group_id;
  GList *imgs = NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images", -1,
                              &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int id = sqlite3_column_int(stmt, 0);
    if(new_group_id == -1) new_group_id = id;
    dt_grouping_add_to_group(new_group_id, id);
    imgs = g_list_append(imgs, GINT_TO_POINTER(id));
  }
  sqlite3_finalize(stmt);
  if(darktable.gui->grouping)
    darktable.gui->expanded_group_id = new_group_id;
  else
    darktable.gui->expanded_group_id = -1;
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, imgs);
  dt_control_queue_redraw_center();
}

/** removes the selected images from their current group. */
static void _ungroup_helper_function(void)
{
  GList *imgs = NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images", -1,
                              &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int id = sqlite3_column_int(stmt, 0);
    dt_grouping_remove_from_group(id);
    imgs = g_list_append(imgs, GINT_TO_POINTER(id));
  }
  sqlite3_finalize(stmt);
  darktable.gui->expanded_group_id = -1;
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, imgs);
  dt_control_queue_redraw_center();
}

static void button_clicked(GtkWidget *widget, gpointer user_data)
{
  const int i = GPOINTER_TO_INT(user_data);
  if(i == 0)
    dt_control_remove_images();
  else if(i == 1)
    dt_control_delete_images();
  // else if(i == 2) dt_control_write_sidecar_files();
  else if(i == 3)
    dt_control_duplicate_images();
  else if(i == 4)
    dt_control_flip_images(1);
  else if(i == 5)
    dt_control_flip_images(0);
  else if(i == 6)
    dt_control_flip_images(2);
  else if(i == 7)
    dt_control_merge_hdr();
  else if(i == 8)
    dt_control_move_images();
  else if(i == 9)
    dt_control_copy_images();
  else if(i == 10)
    _group_helper_function();
  else if(i == 11)
    _ungroup_helper_function();
  else if(i == 12)
    dt_control_set_local_copy_images();
  else if(i == 13)
    dt_control_reset_local_copy_images();
  else if(i == 14)
    dt_control_refresh_exif();
}

static const char* _image_get_delete_button_label()
{
if (dt_conf_get_bool("send_to_trash"))
  return _("trash");
else
  return _("delete");
}

static const char* _image_get_delete_button_tooltip()
{
if (dt_conf_get_bool("send_to_trash"))
  return _("send file to trash");
else
  return _("physically delete from disk");
}

static void _update(dt_lib_module_t *self)
{
  dt_lib_cancel_postponed_update(self);
  dt_lib_image_t *d = (dt_lib_image_t *)self->data;
  const GList *imgs = dt_view_get_images_to_act_on(FALSE, FALSE);

  const guint act_on_cnt = g_list_length((GList *)imgs);
  const uint32_t selected_cnt = dt_collection_get_selected_count(darktable.collection);
  const gboolean can_paste
      = d->imageid > 0 && (act_on_cnt > 1 || (act_on_cnt == 1 && (d->imageid != dt_view_get_image_to_act_on())));

  gtk_widget_set_sensitive(GTK_WIDGET(d->remove_button), act_on_cnt > 0);
  gtk_widget_set_sensitive(GTK_WIDGET(d->delete_button), act_on_cnt > 0);

  gtk_widget_set_sensitive(GTK_WIDGET(d->move_button), act_on_cnt > 0);
  gtk_widget_set_sensitive(GTK_WIDGET(d->copy_button), act_on_cnt > 0);

  gtk_widget_set_sensitive(GTK_WIDGET(d->create_hdr_button), act_on_cnt > 0);
  gtk_widget_set_sensitive(GTK_WIDGET(d->duplicate_button), act_on_cnt > 0);

  gtk_widget_set_sensitive(GTK_WIDGET(d->rotate_ccw_button), act_on_cnt > 0);
  gtk_widget_set_sensitive(GTK_WIDGET(d->rotate_cw_button), act_on_cnt > 0);
  gtk_widget_set_sensitive(GTK_WIDGET(d->reset_button), act_on_cnt > 0);

  gtk_widget_set_sensitive(GTK_WIDGET(d->cache_button), act_on_cnt > 0);
  gtk_widget_set_sensitive(GTK_WIDGET(d->uncache_button), act_on_cnt > 0);

  gtk_widget_set_sensitive(GTK_WIDGET(d->group_button), selected_cnt > 1);
  gtk_widget_set_sensitive(GTK_WIDGET(d->ungroup_button), selected_cnt > 0);

  gtk_widget_set_sensitive(GTK_WIDGET(d->copy_metadata_button), act_on_cnt == 1);
  gtk_widget_set_sensitive(GTK_WIDGET(d->paste_metadata_button), can_paste);
  gtk_widget_set_sensitive(GTK_WIDGET(d->clear_metadata_button), act_on_cnt > 0);

  gtk_widget_set_sensitive(GTK_WIDGET(d->refresh_button), act_on_cnt > 0);
  if(act_on_cnt > 1)
  {
    gtk_widget_set_sensitive(GTK_WIDGET(d->set_monochrome_button), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(d->set_color_button), TRUE);
  }
  else if(act_on_cnt == 0)
  {
    gtk_widget_set_sensitive(GTK_WIDGET(d->set_monochrome_button), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(d->set_color_button), FALSE);
  }
  else
  {
    const int imgid = dt_view_get_image_to_act_on();
    if(imgid >= 0)
    {
      dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
      const gboolean is_bw = (dt_image_monochrome_flags(img) != 0);
      dt_image_cache_read_release(darktable.image_cache, img);
      gtk_widget_set_sensitive(GTK_WIDGET(d->set_monochrome_button), !is_bw);
      gtk_widget_set_sensitive(GTK_WIDGET(d->set_color_button), is_bw);
    }
    else
    {
      gtk_widget_set_sensitive(GTK_WIDGET(d->set_monochrome_button), FALSE);
      gtk_widget_set_sensitive(GTK_WIDGET(d->set_color_button), FALSE);
    }
  }
}

static void _image_selection_changed_callback(gpointer instance, dt_lib_module_t *self)
{
  _update(self);
}

static void _collection_updated_callback(gpointer instance, dt_collection_change_t query_change, gpointer imgs,
                                        int next, dt_lib_module_t *self)
{
  _update(self);
}

static void _mouse_over_image_callback(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_queue_postponed_update(self, _update);
}

static void _image_preference_changed(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t*)user_data;
  dt_lib_image_t *d = (dt_lib_image_t *)self->data;
  gtk_button_set_label(GTK_BUTTON(d->delete_button), _image_get_delete_button_label());
  gtk_widget_set_tooltip_text(d->delete_button, _image_get_delete_button_tooltip());
}

int position()
{
  return 700;
}

typedef enum dt_metadata_actions_t
{
  DT_MA_REPLACE = 0,
  DT_MA_MERGE,
  DT_MA_CLEAR
} dt_metadata_actions_t;


static void _execute_metadata(dt_lib_module_t *self, const int action)
{
  dt_lib_image_t *d = (dt_lib_image_t *)self->data;
  const gboolean rating_flag = dt_conf_get_bool("plugins/lighttable/copy_metadata/rating");
  const gboolean colors_flag = dt_conf_get_bool("plugins/lighttable/copy_metadata/colors");
  const gboolean dtmetadata_flag = dt_conf_get_bool("plugins/lighttable/copy_metadata/metadata");
  const gboolean geotag_flag = dt_conf_get_bool("plugins/lighttable/copy_metadata/geotags");
  const gboolean dttag_flag = dt_conf_get_bool("plugins/lighttable/copy_metadata/tags");
  const int imageid = d->imageid;
  const GList *imgs = dt_view_get_images_to_act_on(FALSE, TRUE);
  if(imgs)
  {
    // for all the above actions, we don't use the grpu_on tag, as grouped images have already been added to image
    // list
    const dt_undo_type_t undo_type = (rating_flag ? DT_UNDO_RATINGS : 0) |
                                    (colors_flag ? DT_UNDO_COLORLABELS : 0) |
                                    (dtmetadata_flag ? DT_UNDO_METADATA : 0) |
                                    (geotag_flag ? DT_UNDO_GEOTAG : 0) |
                                    (dttag_flag ? DT_UNDO_TAGS : 0);
    if(undo_type) dt_undo_start_group(darktable.undo, undo_type);

    if(rating_flag)
    {
      const int stars = (action == DT_MA_CLEAR) ? 0 : dt_ratings_get(imageid);
      dt_ratings_apply_on_list(imgs, stars, TRUE);
    }
    if(colors_flag)
    {
      const int colors = (action == DT_MA_CLEAR) ? 0 : dt_colorlabels_get_labels(imageid);
      dt_colorlabels_set_labels(imgs, colors, action != DT_MA_MERGE, TRUE);
    }
    if(dtmetadata_flag)
    {
      GList *metadata = (action == DT_MA_CLEAR) ? NULL : dt_metadata_get_list_id(imageid);
      dt_metadata_set_list_id(imgs, metadata, action != DT_MA_MERGE, TRUE);
      DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE);
      g_list_free_full(metadata, g_free);
    }
    if(geotag_flag)
    {
      dt_image_geoloc_t *geoloc = (dt_image_geoloc_t *)malloc(sizeof(dt_image_geoloc_t));
      if(action == DT_MA_CLEAR)
        geoloc->longitude = geoloc->latitude = geoloc->elevation = NAN;
      else
        dt_image_get_location(imageid, geoloc);
      dt_image_set_locations(imgs, geoloc, TRUE);
      g_free(geoloc);
    }
    if(dttag_flag)
    {
      // affect only user tags (not dt tags)
      GList *tags = (action == DT_MA_CLEAR) ? NULL : dt_tag_get_tags(imageid, TRUE);
      if(dt_tag_set_tags(tags, imgs, TRUE, action != DT_MA_MERGE, TRUE))
        DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);
      g_list_free(tags);
    }

    if(undo_type)
    {
      dt_undo_end_group(darktable.undo);
      dt_image_synch_xmps(imgs);
      dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD,
                                 g_list_copy((GList *)imgs));
      dt_control_queue_redraw_center();
    }
  }
}

static void copy_metadata_callback(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_image_t *d = (dt_lib_image_t *)self->data;

  d->imageid = dt_view_get_image_to_act_on();

  _update(self);
}

static void paste_metadata_callback(GtkWidget *widget, dt_lib_module_t *self)
{
  const int mode = dt_conf_get_int("plugins/lighttable/copy_metadata/pastemode");
  _execute_metadata(self, mode == 0 ? DT_MA_MERGE : DT_MA_REPLACE);
}

static void clear_metadata_callback(GtkWidget *widget, dt_lib_module_t *self)
{
  _execute_metadata(self, DT_MA_CLEAR);
}

static void set_monochrome_callback(GtkWidget *widget, dt_lib_module_t *self)
{

  dt_control_monochrome_images(2);
}

static void set_color_callback(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_control_monochrome_images(0);
}

static void ratings_flag_callback(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_image_t *d = (dt_lib_image_t *)self->data;
  const gboolean flag = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->ratings_flag));
  dt_conf_set_bool("plugins/lighttable/copy_metadata/rating", flag);
}

static void colors_flag_callback(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_image_t *d = (dt_lib_image_t *)self->data;
  const gboolean flag = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->colors_flag));
  dt_conf_set_bool("plugins/lighttable/copy_metadata/colors", flag);
}

static void metadata_flag_callback(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_image_t *d = (dt_lib_image_t *)self->data;
  const gboolean flag = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->metadata_flag));
  dt_conf_set_bool("plugins/lighttable/copy_metadata/metadata", flag);
}

static void geotags_flag_callback(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_image_t *d = (dt_lib_image_t *)self->data;
  const gboolean flag = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->geotags_flag));
  dt_conf_set_bool("plugins/lighttable/copy_metadata/geotags", flag);
}

static void tags_flag_callback(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_image_t *d = (dt_lib_image_t *)self->data;
  const gboolean flag = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->tags_flag));
  dt_conf_set_bool("plugins/lighttable/copy_metadata/tags", flag);
}

static void pastemode_combobox_changed(GtkWidget *widget, gpointer user_data)
{
  const int mode = dt_bauhaus_combobox_get(widget);
  dt_conf_set_int("plugins/lighttable/copy_metadata/pastemode", mode);
}

#define ellipsize_button(button) gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(button))), PANGO_ELLIPSIZE_END);
void gui_init(dt_lib_module_t *self)
{
  dt_lib_image_t *d = (dt_lib_image_t *)malloc(sizeof(dt_lib_image_t));
  self->data = (void *)d;
  self->timeout_handle = 0;
  self->widget = gtk_notebook_new();
  dt_gui_add_help_link(self->widget, "selected_images.html#selected_images_usage");

  GtkWidget *page1 = dt_ui_notebook_page(GTK_NOTEBOOK(self->widget), _("images"), NULL);
  GtkWidget *page2 = dt_ui_notebook_page(GTK_NOTEBOOK(self->widget), _("metadata"), NULL);

  // images operations
  d->page1 = gtk_grid_new();

  GtkGrid *grid = GTK_GRID(d->page1);
  gtk_container_add(GTK_CONTAINER(page1), d->page1);
  gtk_grid_set_column_homogeneous(grid, TRUE);
  int line = 0;


  d->remove_button = dt_ui_button_new(_("remove"), _("remove from the collection"), NULL);
  gtk_grid_attach(grid, d->remove_button, 0, line, 2, 1);
  g_signal_connect(G_OBJECT(d->remove_button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(0));

  d->delete_button = dt_ui_button_new(_image_get_delete_button_label(), _image_get_delete_button_tooltip(), NULL);
  gtk_grid_attach(grid, d->delete_button, 2, line++, 2, 1);
  g_signal_connect(G_OBJECT(d->delete_button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(1));

  d->move_button = dt_ui_button_new(_("move..."), _("move to other folder"), NULL);
  gtk_grid_attach(grid, d->move_button, 0, line, 2, 1);
  g_signal_connect(G_OBJECT(d->move_button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(8));

  d->copy_button = dt_ui_button_new(_("copy..."), _("copy to other folder"), NULL);
  gtk_grid_attach(grid, d->copy_button, 2, line++, 2, 1);
  g_signal_connect(G_OBJECT(d->copy_button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(9));

  d->create_hdr_button = dt_ui_button_new(_("create HDR"), _("create a high dynamic range image from selected shots"), NULL);
  gtk_grid_attach(grid, d->create_hdr_button, 0, line, 2, 1);
  g_signal_connect(G_OBJECT(d->create_hdr_button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(7));

  d->duplicate_button = dt_ui_button_new(_("duplicate"), _("add a duplicate to the collection, including its history stack"), NULL);
  gtk_grid_attach(grid, d->duplicate_button, 2, line++, 2, 1);
  g_signal_connect(G_OBJECT(d->duplicate_button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(3));

  d->rotate_ccw_button = dtgtk_button_new(dtgtk_cairo_paint_refresh, CPF_NONE, NULL);;
  gtk_widget_set_name(d->rotate_ccw_button, "non-flat");
  gtk_widget_set_tooltip_text(d->rotate_ccw_button, _("rotate selected images 90 degrees CCW"));
  gtk_grid_attach(grid, d->rotate_ccw_button, 0, line, 1, 1);
  g_signal_connect(G_OBJECT(d->rotate_ccw_button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(4));

  d->rotate_cw_button = dtgtk_button_new(dtgtk_cairo_paint_refresh, 1 | CPF_NONE, NULL);
  gtk_widget_set_name(d->rotate_cw_button, "non-flat");
  gtk_widget_set_tooltip_text(d->rotate_cw_button, _("rotate selected images 90 degrees CW"));
  gtk_grid_attach(grid, d->rotate_cw_button, 1, line, 1, 1);
  g_signal_connect(G_OBJECT(d->rotate_cw_button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(5));

  d->reset_button = dt_ui_button_new(_("reset rotation"), _("reset rotation to EXIF data"), NULL);
  gtk_grid_attach(grid, d->reset_button, 2, line++, 2, 1);
  g_signal_connect(G_OBJECT(d->reset_button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(6));

  d->cache_button = dt_ui_button_new(_("copy locally"), _("copy the image locally"), NULL);
  gtk_grid_attach(grid, d->cache_button, 0, line, 2, 1);
  g_signal_connect(G_OBJECT(d->cache_button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(12));

  d->uncache_button = dt_ui_button_new(_("resync local copy"), _("synchronize the image's XMP and remove the local copy"), NULL);
  gtk_grid_attach(grid, d->uncache_button, 2, line++, 2, 1);
  g_signal_connect(G_OBJECT(d->uncache_button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(13));

  d->group_button = dt_ui_button_new(_("group"), _("add selected images to expanded group or create a new one"), NULL);
  gtk_grid_attach(grid, d->group_button, 0, line, 2, 1);
  g_signal_connect(G_OBJECT(d->group_button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(10));

  d->ungroup_button = dt_ui_button_new(_("ungroup"), _("remove selected images from the group"), NULL);
  gtk_grid_attach(grid, d->ungroup_button, 2, line++, 2, 1);
  g_signal_connect(G_OBJECT(d->ungroup_button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(11));

  // metadata operations
  grid = GTK_GRID(gtk_grid_new());
  gtk_container_add(GTK_CONTAINER(page2), GTK_WIDGET(grid));
  gtk_grid_set_column_homogeneous(grid, TRUE);
  line = 0;

  GtkWidget *flag = gtk_check_button_new_with_label(_("ratings"));
  d->ratings_flag = flag;
  gtk_widget_set_tooltip_text(flag, _("select ratings metadata"));
  ellipsize_button(flag);
  gtk_grid_attach(grid, flag, 0, line, 3, 1);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(flag), dt_conf_get_bool("plugins/lighttable/copy_metadata/rating"));
  g_signal_connect(G_OBJECT(flag), "clicked", G_CALLBACK(ratings_flag_callback), self);

  flag = gtk_check_button_new_with_label(_("colors"));
  d->colors_flag = flag;
  gtk_widget_set_tooltip_text(flag, _("select colors metadata"));
  ellipsize_button(flag);
  gtk_grid_attach(grid, flag, 3, line++, 3, 1);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(flag), dt_conf_get_bool("plugins/lighttable/copy_metadata/colors"));
  g_signal_connect(G_OBJECT(flag), "clicked", G_CALLBACK(colors_flag_callback), self);

  flag = gtk_check_button_new_with_label(_("tags"));
  d->tags_flag = flag;
  gtk_widget_set_tooltip_text(flag, _("select tags metadata"));
  ellipsize_button(flag);
  gtk_grid_attach(grid, flag, 0, line, 3, 1);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(flag), dt_conf_get_bool("plugins/lighttable/copy_metadata/tags"));
  g_signal_connect(G_OBJECT(flag), "clicked", G_CALLBACK(tags_flag_callback), self);

  flag = gtk_check_button_new_with_label(_("geo tags"));
  d->geotags_flag = flag;
  gtk_widget_set_tooltip_text(flag, _("select geo tags metadata"));
  ellipsize_button(flag);
  gtk_grid_attach(grid, flag, 3, line++, 3, 1);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(flag), dt_conf_get_bool("plugins/lighttable/copy_metadata/geotags"));
  g_signal_connect(G_OBJECT(flag), "clicked", G_CALLBACK(geotags_flag_callback), self);

  flag = gtk_check_button_new_with_label(_("metadata"));
  d->metadata_flag = flag;
  gtk_widget_set_tooltip_text(flag, _("select dt metadata (from metadata editor module)"));
  ellipsize_button(flag);
  gtk_grid_attach(grid, flag, 0, line++, 3, 1);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(flag), dt_conf_get_bool("plugins/lighttable/copy_metadata/metadata"));
  g_signal_connect(G_OBJECT(flag), "clicked", G_CALLBACK(metadata_flag_callback), self);

  d->copy_metadata_button = dt_ui_button_new(_("copy"), _("set the (first) selected image as source of metadata"), NULL);
  d->imageid = 0;
  gtk_grid_attach(grid, d->copy_metadata_button, 0, line, 2, 1);
  g_signal_connect(G_OBJECT(d->copy_metadata_button), "clicked", G_CALLBACK(copy_metadata_callback), self);

  d->paste_metadata_button = dt_ui_button_new(_("paste"), _("paste selected metadata on selected images"), NULL);
  gtk_grid_attach(grid, d->paste_metadata_button, 2, line, 2, 1);
  g_signal_connect(G_OBJECT(d->paste_metadata_button), "clicked", G_CALLBACK(paste_metadata_callback), self);

  d->clear_metadata_button = dt_ui_button_new(_("clear"), _("clear selected metadata on selected images"), NULL);
  gtk_grid_attach(grid, d->clear_metadata_button, 4, line++, 2, 1);
  g_signal_connect(G_OBJECT(d->clear_metadata_button), "clicked", G_CALLBACK(clear_metadata_callback), self);

  GtkWidget *pastemode = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(pastemode, NULL, N_("mode"));
  dt_bauhaus_combobox_add(pastemode, _("merge"));
  dt_bauhaus_combobox_add(pastemode, _("overwrite"));
  gtk_widget_set_tooltip_text(pastemode, _("how to handle existing metadata"));
  gtk_grid_attach(grid, pastemode, 0, line++, 6, 1);
  dt_bauhaus_combobox_set(pastemode, dt_conf_get_int("plugins/lighttable/copy_metadata/pastemode"));
  g_signal_connect(G_OBJECT(pastemode), "value-changed", G_CALLBACK(pastemode_combobox_changed), self);

  d->refresh_button = dt_ui_button_new(_("refresh exif"), _("update image information to match changes to file"), NULL);
  gtk_grid_attach(grid, d->refresh_button, 0, line++, 6, 1);
  g_signal_connect(G_OBJECT(d->refresh_button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(14));

  d->set_monochrome_button = dt_ui_button_new(_("monochrome"), _("set selection as monochrome images and activate monochrome workflow"), NULL);
  gtk_grid_attach(grid, d->set_monochrome_button, 0, line, 3, 1);
  g_signal_connect(G_OBJECT(d->set_monochrome_button), "clicked", G_CALLBACK(set_monochrome_callback), self);

  d->set_color_button = dt_ui_button_new(_("color"), _("set selection as color images"), NULL);
  gtk_grid_attach(grid, d->set_color_button, 3, line++, 3, 1);
  g_signal_connect(G_OBJECT(d->set_color_button), "clicked", G_CALLBACK(set_color_callback), self);

  /* connect preference changed signal */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(
      darktable.signals,
      DT_SIGNAL_PREFERENCES_CHANGE,
      G_CALLBACK(_image_preference_changed),
      (gpointer)self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_SELECTION_CHANGED,
                            G_CALLBACK(_image_selection_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(_mouse_over_image_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_collection_updated_callback), self);

  _update(self);
}
#undef ellipsize_button

void gui_reset(dt_lib_module_t *self)
{
  dt_lib_image_t *d = (dt_lib_image_t *)self->data;
  d->imageid = 0;
  _update(self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_cancel_postponed_update(self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_image_preference_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_image_selection_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_mouse_over_image_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_collection_updated_callback), self);

  free(self->data);
  self->data = NULL;
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "remove from collection"), GDK_KEY_Delete, 0);
  dt_accel_register_lib(self, NC_("accel", "delete from disk or send to trash"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "move to other folder"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "copy to other folder"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "rotate selected images 90 degrees CW"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "rotate selected images 90 degrees CCW"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "create HDR"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "duplicate"), GDK_KEY_d, GDK_CONTROL_MASK);
  dt_accel_register_lib(self, NC_("accel", "reset rotation"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "copy the image locally"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "resync the local copy"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "refresh exif"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "set monochrome image"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "set color image"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "replace metadata"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "paste metadata"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "clear metadata"), 0, 0);
  // Grouping keys
  dt_accel_register_lib(self, NC_("accel", "group"), GDK_KEY_g, GDK_CONTROL_MASK);
  dt_accel_register_lib(self, NC_("accel", "ungroup"), GDK_KEY_g, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
}

void connect_key_accels(dt_lib_module_t *self)
{
  dt_lib_image_t *d = (dt_lib_image_t *)self->data;

  dt_accel_connect_button_lib(self, "remove from collection", d->remove_button);
  dt_accel_connect_button_lib(self, "delete from disk or send to trash", d->delete_button);
  dt_accel_connect_button_lib(self, "move to other folder", d->move_button);
  dt_accel_connect_button_lib(self, "copy to other folder", d->copy_button);
  dt_accel_connect_button_lib(self, "rotate selected images 90 degrees CW", d->rotate_cw_button);
  dt_accel_connect_button_lib(self, "rotate selected images 90 degrees CCW", d->rotate_ccw_button);
  dt_accel_connect_button_lib(self, "create HDR", d->create_hdr_button);
  dt_accel_connect_button_lib(self, "duplicate", d->duplicate_button);
  dt_accel_connect_button_lib(self, "reset rotation", d->reset_button);
  dt_accel_connect_button_lib(self, "copy the image locally", d->cache_button);
  dt_accel_connect_button_lib(self, "resync the local copy", d->uncache_button);
  dt_accel_connect_button_lib(self, "refresh exif", d->refresh_button);
  dt_accel_connect_button_lib(self, "set monochrome image", d->set_monochrome_button);
  dt_accel_connect_button_lib(self, "set color image", d->set_color_button);
  dt_accel_connect_button_lib(self, "copy metadata", d->copy_metadata_button);
  dt_accel_connect_button_lib(self, "paste metadata", d->paste_metadata_button);
  dt_accel_connect_button_lib(self, "clear metadata", d->clear_metadata_button);
  // Grouping keys
  dt_accel_connect_button_lib(self, "group", d->group_button);
  dt_accel_connect_button_lib(self, "ungroup", d->ungroup_button);
}

#ifdef USE_LUA
typedef struct {
  const char* key;
  dt_lib_module_t * self;
} lua_callback_data;


static int lua_button_clicked_cb(lua_State* L)
{
  lua_callback_data * data = lua_touserdata(L,1);
  dt_lua_module_entry_push(L,"lib",data->self->plugin_name);
  lua_getuservalue(L,-1);
  lua_getfield(L,-1,"callbacks");
  lua_getfield(L,-1,data->key);
  lua_pushstring(L,data->key);

  GList *image = dt_collection_get_selected(darktable.collection, -1);
  lua_newtable(L);
  while(image)
  {
    luaA_push(L, dt_lua_image_t, &image->data);
    luaL_ref(L, -2);
    image = g_list_delete_link(image, image);
  }

  lua_call(L,2,0);
  return 0;
}

static void lua_button_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_lua_async_call_alien(lua_button_clicked_cb,
      0,NULL,NULL,
      LUA_ASYNC_TYPENAME,"void*", user_data,
      LUA_ASYNC_DONE);
}

static int lua_register_action(lua_State *L)
{
  lua_settop(L,3);
  dt_lib_module_t *self = lua_touserdata(L, lua_upvalueindex(1));
  dt_lua_module_entry_push(L,"lib",self->plugin_name);
  lua_getuservalue(L,-1);
  const char* key = luaL_checkstring(L,1);
  luaL_checktype(L,2,LUA_TFUNCTION);

  lua_getfield(L,-1,"callbacks");
  lua_pushstring(L,key);
  lua_pushvalue(L,2);
  lua_settable(L,-3);

  GtkWidget* button = gtk_button_new_with_label(key);
  const char * tooltip = lua_tostring(L,3);
  if(tooltip)  {
    gtk_widget_set_tooltip_text(button, tooltip);
  }
  dt_lib_image_t *d = self->data;
  gtk_grid_attach_next_to(GTK_GRID(d->page1), button, NULL, GTK_POS_BOTTOM, 4, 1);


  lua_callback_data * data = malloc(sizeof(lua_callback_data));
  data->key = strdup(key);
  data->self = self;
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(lua_button_clicked), data);
  gtk_widget_show_all(button);
  return 0;
}

void init(struct dt_lib_module_t *self)
{

  lua_State *L = darktable.lua_state.state;
  int my_type = dt_lua_module_entry_get_type(L, "lib", self->plugin_name);
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, lua_register_action,1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "register_action");

  dt_lua_module_entry_push(L,"lib",self->plugin_name);
  lua_getuservalue(L,-1);
  lua_newtable(L);
  lua_setfield(L,-2,"callbacks");
  lua_pop(L,2);
}
#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
