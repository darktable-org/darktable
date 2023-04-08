/*
    This file is part of darktable,
    Copyright (C) 2015-2023 darktable developers.

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

#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/history.h"
#include "common/metadata.h"
#include "common/mipmap_cache.h"
#include "common/selection.h"
#include "common/styles.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "dtgtk/thumbnail.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/styles.h"
#include "libs/lib.h"

#define DUPLICATE_COMPARE_SIZE 40

DT_MODULE(1)

typedef struct dt_lib_duplicate_t
{
  GtkWidget *duplicate_box;
  dt_imgid_t imgid;

  cairo_surface_t *preview_surf;
  size_t processed_width;
  size_t processed_height;
  dt_view_context_t view_ctx;
  int preview_id;

  GList *thumbs;
} dt_lib_duplicate_t;

const char *name(dt_lib_module_t *self)
{
  return _("duplicate manager");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position(const dt_lib_module_t *self)
{
  return 850;
}

static void _lib_duplicate_init_callback(gpointer instance, dt_lib_module_t *self);

static gboolean _lib_duplicate_caption_out_callback(GtkWidget *widget,
                                                    GdkEvent *event,
                                                    dt_lib_module_t *self)
{
  const dt_imgid_t imgid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget),"imgid"));

  // we write the content of the textbox to the caption field
  dt_metadata_set(imgid, "Xmp.darktable.version_name",
                  gtk_entry_get_text(GTK_ENTRY(widget)), FALSE);
  dt_image_synch_xmp(imgid);

  return FALSE;
}

static void _lib_duplicate_new_clicked_callback(GtkWidget *widget,
                                                GdkEventButton *event,
                                                dt_lib_module_t *self)
{
  const dt_imgid_t imgid = darktable.develop->image_storage.id;
  const dt_imgid_t newid = dt_image_duplicate(imgid);
  if(!dt_is_valid_imgid(newid))
    return;
  dt_history_delete_on_image(newid);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);
  dt_collection_update_query(darktable.collection,
                             DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF, NULL);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals,
                                DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE, newid);
}
static void _lib_duplicate_duplicate_clicked_callback(GtkWidget *widget,
                                                      GdkEventButton *event,
                                                      dt_lib_module_t *self)
{
  const dt_imgid_t imgid = darktable.develop->image_storage.id;
  const dt_imgid_t newid = dt_image_duplicate(imgid);
  if(!dt_is_valid_imgid(newid))
    return;
  dt_history_copy_and_paste_on_image(imgid, newid, FALSE, NULL, TRUE, TRUE);
  dt_collection_update_query(darktable.collection,
                             DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF, NULL);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals,
                                DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE, newid);
}

static void _lib_duplicate_delete(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)self->data;
  const dt_imgid_t imgid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "imgid"));

  if(imgid == darktable.develop->image_storage.id)
  {
    // we find the duplicate image to show now
    for(GList *l = d->thumbs; l; l = g_list_next(l))
    {
      dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
      if(thumb->imgid == imgid)
      {
        GList *l2 = g_list_next(l);
        if(!l2) l2 = g_list_previous(l);
        if(l2)
        {
          dt_thumbnail_t *th2 = (dt_thumbnail_t *)l2->data;
          DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals,
                                        DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE,
                                        th2->imgid);
          break;
        }
      }
    }
  }

  // and we remove the image
  dt_control_delete_image(imgid);
  dt_collection_update_query(darktable.collection,
                             DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF,
                             g_list_prepend(NULL, GINT_TO_POINTER(imgid)));
}

static void _lib_duplicate_thumb_press_callback(GtkWidget *widget,
                                                GdkEventButton *event,
                                                dt_lib_module_t *self)
{
  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)self->data;
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)g_object_get_data(G_OBJECT(widget), "thumb");
  const dt_imgid_t imgid = thumb->imgid;

  if(event->button == 1)
  {
    if(event->type == GDK_BUTTON_PRESS)
    {
      d->imgid = imgid;
      dt_control_queue_redraw_center();
    }
    else if(event->type == GDK_2BUTTON_PRESS)
    {
      // let's switch to the new image
      DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals,
                                    DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE, imgid);
    }
  }
}

static void _lib_duplicate_thumb_release_callback(GtkWidget *widget,
                                                  GdkEventButton *event,
                                                  dt_lib_module_t *self)
{
  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)self->data;

  d->imgid = NO_IMGID;
  dt_control_queue_redraw_center();
}

void view_leave(struct dt_lib_module_t *self,
                struct dt_view_t *old_view,
                struct dt_view_t *new_view)
{
  // we leave the view. Let's destroy preview surf if any
  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)self->data;
  if(d->preview_surf)
  {
    cairo_surface_destroy(d->preview_surf);
    d->preview_surf = NULL;
  }
}
void gui_post_expose(dt_lib_module_t *self,
                     cairo_t *cri,
                     const int32_t width,
                     const int32_t height,
                     const int32_t pointerx,
                     const int32_t pointery)
{
  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)self->data;

  if(!dt_is_valid_imgid(d->imgid))
    return;

  const gboolean view_ok = dt_view_check_view_context(&d->view_ctx);

  if(!view_ok || d->preview_id != d->imgid)
  {
    uint8_t *buf = NULL;
    size_t processed_width;
    size_t processed_height;

    dt_dev_image(d->imgid, width, height, -1, &buf, &processed_width, &processed_height);

    d->preview_id = d->imgid;
    d->processed_width = processed_width;
    d->processed_height = processed_height;

    if(d->preview_surf)
      cairo_surface_destroy(d->preview_surf);
    d->preview_surf = dt_view_create_surface(buf, processed_width, processed_height);
  }

  if(d->preview_surf)
    dt_view_paint_surface(cri, width, height, d->preview_surf,
                          d->processed_width, d->processed_height, DT_WINDOW_MAIN);
}

static void _thumb_remove(gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(thumb->w_main)), thumb->w_main);
  dt_thumbnail_destroy(thumb);
}

static void _lib_duplicate_init_callback(gpointer instance, dt_lib_module_t *self)
{
  //block signals to avoid concurrent calls
  dt_control_signal_block_by_func(darktable.signals,
                                  G_CALLBACK(_lib_duplicate_init_callback), self);

  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)self->data;

  d->imgid = NO_IMGID;
  // we drop the preview if any
  if(d->preview_surf)
  {
    cairo_surface_destroy(d->preview_surf);
    d->preview_surf = NULL;
  }
  // we drop all the thumbs
  g_list_free_full(d->thumbs, _thumb_remove);
  d->thumbs = NULL;
  // and the other widgets too
  dt_gui_container_destroy_children(GTK_CONTAINER(d->duplicate_box));
  // retrieve all the versions of the image
  sqlite3_stmt *stmt;
  dt_develop_t *dev = darktable.develop;

  int count = 0;

  // we get a summarize of all versions of the image
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT i.version, i.id, m.value"
                              " FROM images AS i"
                              " LEFT JOIN meta_data AS m ON m.id = i.id AND m.key = ?3"
                              " WHERE film_id = ?1 AND filename = ?2"
                              " ORDER BY i.version",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dev->image_storage.film_id);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, dev->image_storage.filename, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, DT_METADATA_XMP_VERSION_NAME);

  GtkWidget *bt = NULL;

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    GtkWidget *hb = gtk_grid_new();
    const dt_imgid_t imgid = sqlite3_column_int(stmt, 1);
    dt_gui_add_class(hb, "dt_overlays_always");
    dt_thumbnail_t *thumb = dt_thumbnail_new(100, 100, IMG_TO_FIT, imgid, -1,
                                             DT_THUMBNAIL_OVERLAYS_ALWAYS_NORMAL,
                                             DT_THUMBNAIL_CONTAINER_LIGHTTABLE, TRUE);
    thumb->sel_mode = DT_THUMBNAIL_SEL_MODE_DISABLED;
    thumb->disable_mouseover = TRUE;
    thumb->disable_actions = TRUE;
    dt_thumbnail_set_mouseover(thumb, imgid == dev->image_storage.id);

    if(imgid != dev->image_storage.id)
    {
      g_signal_connect(G_OBJECT(thumb->w_main), "button-press-event",
                       G_CALLBACK(_lib_duplicate_thumb_press_callback), self);
      g_signal_connect(G_OBJECT(thumb->w_main), "button-release-event",
                       G_CALLBACK(_lib_duplicate_thumb_release_callback), self);
    }

    gchar chl[256];
    gchar *path = (gchar *)sqlite3_column_text(stmt, 2);
    g_snprintf(chl, sizeof(chl), "%d", sqlite3_column_int(stmt, 0));

    GtkWidget *tb = gtk_entry_new();
    if(path) gtk_entry_set_text(GTK_ENTRY(tb), path);
    gtk_entry_set_width_chars(GTK_ENTRY(tb), 0);
    gtk_widget_set_hexpand(tb, TRUE);
    g_object_set_data (G_OBJECT(tb), "imgid", GINT_TO_POINTER(imgid));
    gtk_widget_add_events(tb, GDK_FOCUS_CHANGE_MASK);
    g_signal_connect(G_OBJECT(tb), "focus-out-event",
                     G_CALLBACK(_lib_duplicate_caption_out_callback), self);
    GtkWidget *lb = gtk_label_new (g_strdup(chl));
    gtk_widget_set_hexpand(lb, TRUE);
    bt = dtgtk_button_new(dtgtk_cairo_paint_remove, 0, NULL);
    //    gtk_widget_set_halign(bt, GTK_ALIGN_END);
    g_object_set_data(G_OBJECT(bt), "imgid", GINT_TO_POINTER(imgid));
    g_signal_connect(G_OBJECT(bt), "clicked", G_CALLBACK(_lib_duplicate_delete), self);

    gtk_grid_attach(GTK_GRID(hb), thumb->w_main, 0, 0, 1, 2);
    gtk_grid_attach(GTK_GRID(hb), bt, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(hb), lb, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(hb), tb, 1, 1, 2, 1);

    gtk_widget_show_all(hb);

    gtk_box_pack_start(GTK_BOX(d->duplicate_box), hb, FALSE, FALSE, 0);
    d->thumbs = g_list_append(d->thumbs, thumb);
    count++;
  }
  sqlite3_finalize (stmt);

  gtk_widget_show(d->duplicate_box);

  // we have a single image, do not allow it to be removed so hide last bt
  if(count==1)
  {
    gtk_widget_set_sensitive(bt, FALSE);
    gtk_widget_set_visible(bt, FALSE);
  }

  //unblock signals
  dt_control_signal_unblock_by_func(darktable.signals,
                                    G_CALLBACK(_lib_duplicate_init_callback), self);
}

static void _lib_duplicate_collection_changed(gpointer instance,
                                              dt_collection_change_t query_change,
                                              dt_collection_properties_t changed_property,
                                              gpointer imgs,
                                              const int next,
                                              dt_lib_module_t *self)
{
  _lib_duplicate_init_callback(instance, self);
}

static void _lib_duplicate_mipmap_updated_callback(gpointer instance,
                                                   const dt_imgid_t imgid,
                                                   dt_lib_module_t *self)
{
  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)self->data;

  gtk_widget_queue_draw(d->duplicate_box);
  dt_control_queue_redraw_center();
}
static void _lib_duplicate_preview_updated_callback(gpointer instance,
                                                    dt_lib_module_t *self)
{
  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)self->data;

  gtk_widget_queue_draw (d->duplicate_box);
  dt_control_queue_redraw_center();
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)g_malloc0(sizeof(dt_lib_duplicate_t));
  self->data = (void *)d;

  d->imgid = NO_IMGID;
  d->preview_surf = NULL;
  d->processed_width = 0;
  d->processed_height = 0;
  d->view_ctx = 0;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  dt_gui_add_class(self->widget, "dt_duplicate_ui");

  d->duplicate_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *bt = dt_action_button_new
    (NULL, N_("original"),
     _lib_duplicate_new_clicked_callback, self,
     _("create a 'virgin' duplicate of the image without any development"), 0, 0);

  gtk_box_pack_end(GTK_BOX(hb), bt, TRUE, TRUE, 0);
  bt = dt_action_button_new
    (NULL, N_("duplicate"), _lib_duplicate_duplicate_clicked_callback, self,
     _("create a duplicate of the image with same history stack"), 0, 0);

  gtk_box_pack_end(GTK_BOX(hb), bt, TRUE, TRUE, 0);

  /* add duplicate list and buttonbox to widget */
  gtk_box_pack_start(GTK_BOX(self->widget),
                     dt_ui_resize_wrap
                       (d->duplicate_box, 1,
                        "plugins/darkroom/duplicate/windowheight"), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hb, TRUE, TRUE, 0);

  gtk_widget_show_all(self->widget);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals,
                                  DT_SIGNAL_DEVELOP_IMAGE_CHANGED,
                                  G_CALLBACK(_lib_duplicate_init_callback),
                                  self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals,
                                  DT_SIGNAL_DEVELOP_INITIALIZE,
                                  G_CALLBACK(_lib_duplicate_init_callback),
                                  self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals,
                                  DT_SIGNAL_COLLECTION_CHANGED,
                                  G_CALLBACK(_lib_duplicate_collection_changed),
                                  self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals,
                                  DT_SIGNAL_DEVELOP_MIPMAP_UPDATED,
                                  G_CALLBACK(_lib_duplicate_mipmap_updated_callback),
                                  (gpointer)self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals,
                                  DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                                  G_CALLBACK(_lib_duplicate_preview_updated_callback),
                                  self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals,
                                     G_CALLBACK(_lib_duplicate_init_callback),
                                     self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals,
                                     G_CALLBACK(_lib_duplicate_mipmap_updated_callback),
                                     self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals,
                                     G_CALLBACK(_lib_duplicate_preview_updated_callback),
                                     self);
  g_free(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
