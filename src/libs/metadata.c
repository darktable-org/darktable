/*
    This file is part of darktable,
    Copyright (C) 2010-2021 darktable developers.

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

#include "common/metadata.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/signal.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#include <gdk/gdkkeysyms.h>

DT_MODULE(3)

typedef enum dt_metadata_pref_cols_t
{
  DT_METADATA_PREF_COL_INDEX = 0, // display index
  DT_METADATA_PREF_COL_NAME,      // displayed name
  DT_METADATA_PREF_COL_VISIBLE,   // visibility
  DT_METADATA_PREF_COL_PRIVATE,    // do not export
  DT_METADATA_PREF_NUM_COLS
} dt_metadata_pref_cols_t;

typedef struct dt_lib_metadata_t
{
  GtkTextView *textview[DT_METADATA_NUMBER];
  GtkWidget *swindow[DT_METADATA_NUMBER];
  GList *metadata_list[DT_METADATA_NUMBER];
  char *setting_name[DT_METADATA_NUMBER];
  GtkGrid *metadata_grid;
  gboolean editing;
  GtkWidget *apply_button;
  gboolean init_layout;
  GList *last_act_on;
} dt_lib_metadata_t;

const char *name(dt_lib_module_t *self)
{
  return _("metadata editor");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"lighttable", "tethering", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

static gboolean _is_leave_unchanged(const char *text)
{
  return g_strcmp0(text, _("<leave unchanged>")) == 0;
}

static gchar *_get_buffer_text(GtkTextView *textview)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(textview);
  GtkTextIter start;
  gtk_text_buffer_get_start_iter(buffer, &start);
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(buffer, &end);
  return gtk_text_buffer_get_text(buffer, &start, &end, TRUE);
}

static void _text_set_italic(GtkTextView *textview, const gboolean italic)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(textview);
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  if(italic)
    gtk_text_buffer_apply_tag_by_name(buffer, "italic", &start, &end);
  else
    gtk_text_buffer_remove_tag_by_name(buffer, "italic", &start, &end);
}

static void _fill_text_view(const uint32_t i, const uint32_t count, dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;
  gboolean multi = FALSE;

  GtkTextBuffer *buffer = gtk_text_view_get_buffer(d->textview[i]);
  if(count == 0)  // no metadata value
  {
    gtk_text_buffer_set_text(buffer, "", -1);
  }
  else if(count == 1) // images with different metadata values
  {
    gtk_text_buffer_set_text(buffer, _("<leave unchanged>"), -1);
    multi = TRUE;
  }
  else // one or several images with the same metadata value
  {
    gtk_text_buffer_set_text(buffer, (char *)d->metadata_list[i]->data, -1);
  }
  _text_set_italic(d->textview[i], multi);
}

static void _update(dt_lib_module_t *self)
{
  dt_lib_cancel_postponed_update(self);
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;

  const GList *imgs = dt_view_get_images_to_act_on(FALSE, FALSE, FALSE);

  // first we want to make sure the list of images to act on has changed
  // this is not the case if mouse hover change but still stay in selection for ex.
  if(!imgs && !d->last_act_on) return;
  if(imgs && d->last_act_on)
  {
    gboolean changed = FALSE;
    GList *l = d->last_act_on;
    GList *ll = (GList *)imgs;
    while(l && ll)
    {
      if(GPOINTER_TO_INT(l->data) != GPOINTER_TO_INT(ll->data))
      {
        changed = TRUE;
        break;
      }
      l = g_list_next(l);
      ll = g_list_next(ll);
    }
    if(!changed) return;
  }
  g_list_free(d->last_act_on);
  d->last_act_on = g_list_copy((GList *)imgs);

  GList *metadata[DT_METADATA_NUMBER];
  uint32_t metadata_count[DT_METADATA_NUMBER];
  uint32_t imgs_count = 0;
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    metadata[i] = NULL;
    metadata_count[i] = 0;
  }

  // using dt_metadata_get() is not possible here. we want to do all this in a single pass, everything else
  // takes ages.
  gchar *images = dt_view_get_images_to_act_on_query(FALSE);
  imgs_count = g_list_length((GList *)imgs);

  if(images)
  {
    sqlite3_stmt *stmt;
    gchar *query = g_strdup_printf(
                            "SELECT key, value, COUNT(id) AS ct FROM main.meta_data"
                            " WHERE id IN (%s)"
                            " GROUP BY key, value ORDER BY value",
                            images);
    g_free(images);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);

    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      if(sqlite3_column_bytes(stmt, 1))
      {
        const uint32_t key = (uint32_t)sqlite3_column_int(stmt, 0);
        char *value = g_strdup((char *)sqlite3_column_text(stmt, 1));
        const uint32_t count = (uint32_t)sqlite3_column_int(stmt, 2);
        metadata_count[key] = (count == imgs_count) ? 2 : 1;  // if = all images have the same metadata
        metadata[key] = g_list_append(metadata[key], value);
      }
    }
    sqlite3_finalize(stmt);
    g_free(query);
  }

  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    const uint32_t keyid = dt_metadata_get_keyid_by_display_order(i);
    g_list_free_full(d->metadata_list[i], g_free);
    d->metadata_list[i] = metadata[keyid];
    _fill_text_view(i, metadata_count[keyid], self);
  }

  gtk_widget_set_sensitive(GTK_WIDGET(d->apply_button), imgs_count > 0);
}

static void _image_selection_changed_callback(gpointer instance, dt_lib_module_t *self)
{
  _update(self);
}

static void _collection_updated_callback(gpointer instance, dt_collection_change_t query_change,
                                         dt_collection_properties_t changed_property, gpointer imgs, int next,
                                         dt_lib_module_t *self)
{
  _update(self);
}

static void _append_kv(GList **l, const gchar *key, const gchar *value)
{
  *l = g_list_append(*l, (gchar *)key);
  *l = g_list_append(*l, (gchar *)value);
}

static void _write_metadata(dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;
  d->editing = FALSE;

  gchar *metadata[DT_METADATA_NUMBER];
  GList *key_value = NULL;
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    const uint32_t keyid = dt_metadata_get_keyid_by_display_order(i);
    metadata[i] = _get_buffer_text((GtkTextView *)d->textview[i]);
    if(metadata[i] && !_is_leave_unchanged(metadata[i]))
      _append_kv(&key_value, dt_metadata_get_key(keyid), metadata[i]);
  }

  const GList *imgs = dt_view_get_images_to_act_on(FALSE, TRUE, FALSE);
  dt_metadata_set_list(imgs, key_value, TRUE);

  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    g_free(metadata[i]);
  }
  g_list_free(key_value);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_METADATA_CHANGED, DT_METADATA_SIGNAL_NEW_VALUE);

  dt_image_synch_xmps(imgs);
  _update(self);
}

static void _apply_button_clicked(GtkButton *button, dt_lib_module_t *self)
{
  _write_metadata(self);
}

static gboolean _key_pressed(GtkWidget *textview, GdkEventKey *event, dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;

  if(dt_modifier_is(event->state, GDK_CONTROL_MASK))
  {
    switch(event->keyval)
    {
      case GDK_KEY_Return:
      case GDK_KEY_KP_Enter:
        // insert new line
        event->state &= ~GDK_CONTROL_MASK;  //TODO: on Mac, remap Ctrl to Cmd key
      default:
        d->editing = TRUE;
    }
  }
  else
  {
    switch(event->keyval)
    {
      case GDK_KEY_Return:
      case GDK_KEY_KP_Enter:
        _write_metadata(self);
        // go to next field
        event->keyval = GDK_KEY_Tab;
        break;
      case GDK_KEY_Escape:
        _update(self);
        gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
        d->editing = FALSE;
        break;
      case GDK_KEY_Tab:
        _write_metadata(self);
        break;
      default:
        d->editing = TRUE;
    }
  }

  return gtk_text_view_im_context_filter_keypress(GTK_TEXT_VIEW(textview), event);
}

static gboolean _got_focus(GtkWidget *textview, dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;
  if(!d->editing)
  {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textview));
    gchar *text = _get_buffer_text(GTK_TEXT_VIEW(textview));
    if(_is_leave_unchanged(text))
    {
      gtk_text_buffer_set_text(buffer, "", -1);
      _text_set_italic(GTK_TEXT_VIEW(textview), FALSE);
    }
    g_free(text);
  }
  return FALSE;
}

static gboolean _lost_focus(GtkWidget *textview, GdkEventFocus *event, dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;
  d->editing = FALSE;
  return FALSE;
}

int position()
{
  return 510;
}

static void _update_layout(dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    const gchar *name = dt_metadata_get_name_by_display_order(i);
    gchar *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);
    const gboolean hidden = dt_conf_get_int(setting) & DT_METADATA_FLAG_HIDDEN;
    g_free(setting);
    const int type = dt_metadata_get_type_by_display_order(i);
    for(int j = 0; j < 2; j++)
    {
      GtkWidget *w = gtk_grid_get_child_at(d->metadata_grid,j,i);
      gtk_widget_show_all(w);
      gtk_widget_set_no_show_all(w, TRUE);
      gtk_widget_set_visible(w, (!hidden || type == DT_METADATA_TYPE_INTERNAL));
    }
  }
}

void gui_reset(dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;
  d->editing = FALSE;
  const GList *imgs = dt_view_get_images_to_act_on(FALSE, TRUE, FALSE);
  dt_metadata_clear(imgs, TRUE);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE);
  dt_image_synch_xmps(imgs);
  _update(self);
}

static void _mouse_over_image_callback(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;
  // if editing don't lose the current entry
  if (d->editing) return;

  dt_lib_queue_postponed_update(self, _update);
}

static gboolean _metadata_list_size_changed(GtkWidget *window, GdkEvent  *event, GtkCellRenderer *renderer)
{
  GdkEventConfigure *data = (GdkEventConfigure *)event;

//  GtkTreeView *listview = g_object_get_qdata(G_OBJECT(renderer), g_quark_from_static_string("listview"));
  g_object_set(G_OBJECT(renderer), "wrap-width", data->width, NULL);
  // TODO make it works. renderer should take in account the new wrap-width and calculate a new record height
  gtk_widget_queue_draw(GTK_WIDGET(window));
  return FALSE;
}

typedef struct dt_lib_metadata_dialog_t
{
  GtkTextView *textview;
  GtkTreeView *listview;
  GtkDialog *dialog;
} dt_lib_metadata_dialog_t;

static gboolean _metadata_selected(GtkWidget *listview, GdkEventButton *event, dt_lib_metadata_dialog_t *d)
{
  if(event->type == GDK_BUTTON_PRESS && event->button == 1)
  {
    GtkTreePath *path = NULL;
    // Get tree path for row that was clicked
    if(gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(listview), (gint)event->x, (gint)event->y, &path, NULL, NULL, NULL))
    {
      GtkTreeModel *liststore = gtk_tree_view_get_model(GTK_TREE_VIEW(d->listview));
      GtkTreeIter iter;
      if(gtk_tree_model_get_iter(liststore, &iter, path))
      {
        gchar *text;
        gtk_tree_model_get(liststore, &iter, 0, &text, -1);
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(d->textview);
        gtk_text_buffer_set_text(buffer, text, -1);
        g_free(text);
        gtk_tree_path_free(path);
        gtk_dialog_response(d->dialog, GTK_RESPONSE_YES);
        return TRUE;
      }
    }
    gtk_tree_path_free(path);
  }
  gtk_dialog_response(d->dialog, GTK_RESPONSE_NONE);
  return FALSE;
}

static void _toggled_callback(gchar *path_str, gpointer user_data, const int column)
{
  GtkListStore *store = (GtkListStore *)user_data;
  GtkTreeIter iter;
  GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
  gboolean toggle;

  gtk_tree_model_get_iter(GTK_TREE_MODEL(store), &iter, path);
  gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, column, &toggle, -1);
  gtk_list_store_set(store, &iter, column, !toggle, -1);

  gtk_tree_path_free(path);
}

static void _visible_toggled_callback(GtkCellRendererToggle *cell_renderer, gchar *path_str, gpointer user_data)
{
  _toggled_callback(path_str, user_data, DT_METADATA_PREF_COL_VISIBLE);
}

static void _private_toggled_callback(GtkCellRendererToggle *cell_renderer, gchar *path_str, gpointer user_data)
{
  _toggled_callback(path_str, user_data, DT_METADATA_PREF_COL_PRIVATE);
}

void _menuitem_preferences(GtkMenuItem *menuitem, dt_lib_module_t *self)
{
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("metadata settings"), GTK_WINDOW(win),
                                       GTK_DIALOG_DESTROY_WITH_PARENT, _("default"), GTK_RESPONSE_ACCEPT,
                                       _("cancel"), GTK_RESPONSE_NONE, _("save"), GTK_RESPONSE_YES, NULL);
  GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

  GtkWidget *w = gtk_scrolled_window_new(NULL, NULL);
  gtk_widget_set_size_request(w, -1, DT_PIXEL_APPLY_DPI(100));
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w), GTK_POLICY_NEVER, GTK_POLICY_NEVER);
  gtk_box_pack_start(GTK_BOX(area), w, TRUE, TRUE, 0);

  GtkListStore *store = gtk_list_store_new(DT_METADATA_PREF_NUM_COLS,
                                           G_TYPE_INT, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN);
  GtkTreeModel *model = GTK_TREE_MODEL(store);
  GtkTreeIter iter;

  char *name[DT_METADATA_NUMBER];
  gboolean visible[DT_METADATA_NUMBER];
  gboolean private[DT_METADATA_NUMBER];
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    const int type = dt_metadata_get_type_by_display_order(i);
    if(type != DT_METADATA_TYPE_INTERNAL)
    {
      name[i] = (gchar *)dt_metadata_get_name_by_display_order(i);
      gchar *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name[i]);
      const uint32_t flag = dt_conf_get_int(setting);
      g_free(setting);
      visible[i] = !(flag & DT_METADATA_FLAG_HIDDEN);
      private[i] = flag & DT_METADATA_FLAG_PRIVATE;
      gtk_list_store_append(store, &iter);
      gtk_list_store_set(store, &iter,
                         DT_METADATA_PREF_COL_INDEX, i,
                         DT_METADATA_PREF_COL_NAME, _(name[i]),
                         DT_METADATA_PREF_COL_VISIBLE, visible[i],
                         DT_METADATA_PREF_COL_PRIVATE, private[i],
                         -1);
    }
  }

  GtkWidget *view = gtk_tree_view_new_with_model(model);
  g_object_unref(model);
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(_("metadata"), renderer,
                                                    "text", DT_METADATA_PREF_COL_NAME, NULL);
  gtk_tree_view_column_set_expand(column, TRUE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);
  renderer = gtk_cell_renderer_toggle_new();
  g_signal_connect(renderer, "toggled", G_CALLBACK(_visible_toggled_callback), store);
  column = gtk_tree_view_column_new_with_attributes(_("visible"), renderer,
                                                    "active", DT_METADATA_PREF_COL_VISIBLE, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);
  GtkWidget *header = gtk_tree_view_column_get_button(column);
  gtk_widget_set_tooltip_text(header,
                _("tick if the corresponding metadata is of interest for you"
                "\nit will be visible from metadata editor, collection and import module"
                "\nit will be also exported"));
  renderer = gtk_cell_renderer_toggle_new();
  g_signal_connect(renderer, "toggled", G_CALLBACK(_private_toggled_callback), store);
  column = gtk_tree_view_column_new_with_attributes(_("private"), renderer,
                                                    "active", DT_METADATA_PREF_COL_PRIVATE, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);
  header = gtk_tree_view_column_get_button(column);
  gtk_widget_set_tooltip_text(header,
                _("tick if you want to keep this information private (not exported with images)"));

  gtk_container_add(GTK_CONTAINER(w), view);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_widget_show_all(dialog);

  int res = gtk_dialog_run(GTK_DIALOG(dialog));
  while(res == GTK_RESPONSE_ACCEPT)
  {
    gtk_tree_model_get_iter_first(model, &iter);
    for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
    {
      // mimic dt_metadata_init() without saving at this stage
      const int type = dt_metadata_get_type_by_display_order(i);
      if(type != DT_METADATA_TYPE_INTERNAL)
      {
        gtk_list_store_set(store, &iter,
                           DT_METADATA_PREF_COL_VISIBLE, type == DT_METADATA_TYPE_OPTIONAL ? FALSE : TRUE,
                           DT_METADATA_PREF_COL_PRIVATE, FALSE,
                           -1);
        gtk_tree_model_iter_next(model, &iter);
      }
    }
    res = gtk_dialog_run(GTK_DIALOG(dialog));
  }

  if(res == GTK_RESPONSE_YES)
  {
    gboolean meta_signal = FALSE;
    gboolean meta_remove = FALSE;
    gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
    while(valid)
    {
      gboolean new_visible;
      gboolean new_private;
      uint32_t i;
      gtk_tree_model_get(model, &iter,
                         DT_METADATA_PREF_COL_INDEX, &i,
                         DT_METADATA_PREF_COL_VISIBLE, &new_visible,
                         DT_METADATA_PREF_COL_PRIVATE, &new_private,
                         -1);
      if(i < DT_METADATA_NUMBER)
      {
        gchar *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name[i]);
        uint32_t flag = dt_conf_get_int(setting);
        if(new_visible !=  visible[i])
        {
          flag = !new_visible ? flag | DT_METADATA_FLAG_HIDDEN : flag & ~DT_METADATA_FLAG_HIDDEN;
          meta_signal = TRUE;
          meta_remove =  !new_visible ? TRUE : meta_remove;
        }
        if(new_private != private[i])
        {
          flag = new_private ? flag | DT_METADATA_FLAG_PRIVATE : flag & ~DT_METADATA_FLAG_PRIVATE;
        }
        dt_conf_set_int(setting, flag);
        g_free(setting);
      }
      valid = gtk_tree_model_iter_next(model, &iter);
    }
    if(meta_signal)
      DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_METADATA_CHANGED,
                              meta_remove ? DT_METADATA_SIGNAL_HIDDEN : DT_METADATA_SIGNAL_SHOWN);
  }
  _update_layout(self);
  gtk_widget_destroy(dialog);
}

void set_preferences(void *menu, dt_lib_module_t *self)
{
  GtkWidget *mi = gtk_menu_item_new_with_label(_("preferences..."));
  g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_menuitem_preferences), self);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "apply"), 0, 0);
}

void connect_key_accels(dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;

  dt_accel_connect_button_lib(self, "apply", d->apply_button);
}

static gboolean _click_on_textview(GtkWidget *textview, GdkEventButton *event, dt_lib_module_t *self)
{
  const dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;
  // get grid line number
  uint32_t i;
  for(i = 0; i < DT_METADATA_NUMBER; i++)
  {
    if(GTK_TEXT_VIEW(textview) == d->textview[i])
      break;
  }
  if(i >= DT_METADATA_NUMBER) return FALSE;

  if(!(event->type == GDK_BUTTON_PRESS && event->button == 3)) return FALSE;

  gchar *text = _get_buffer_text(GTK_TEXT_VIEW(textview));
  const gboolean leave_unchanged = _is_leave_unchanged(text);
  g_free(text);
  if (!leave_unchanged) return FALSE;

  GtkWidget *dialog = gtk_dialog_new();
  gtk_window_set_decorated (GTK_WINDOW(dialog), FALSE);
  gtk_window_set_modal (GTK_WINDOW(dialog), TRUE);
  gtk_window_set_title(GTK_WINDOW(dialog),_("metadata list"));
  GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

  GtkWidget *grid = gtk_grid_new();
  gtk_container_add(GTK_CONTAINER(area), grid);
  GtkWidget *w = gtk_scrolled_window_new(NULL, NULL);
  gtk_widget_set_hexpand(w, TRUE);
  gtk_widget_set_vexpand(w, TRUE);
  // popup position
  GdkWindow *parent_window = gtk_widget_get_window(GTK_WIDGET(d->swindow[i]));
  gint wx, wy;
  gdk_window_get_origin(parent_window, &wx, &wy);
  gtk_window_move(GTK_WINDOW(dialog), wx, wy);
  // popup width
  GtkAllocation metadata_allocation;
  gtk_widget_get_allocation(GTK_WIDGET(d->swindow[i]), &metadata_allocation);
  // popup height
  const gchar *name = dt_metadata_get_name_by_display_order(i);
  gchar *setting = g_strdup_printf("plugins/lighttable/metadata/%s_text_height", name);
  const gint height = dt_conf_get_int(setting) * 5;
  g_free(setting);

  gtk_widget_set_size_request(w, metadata_allocation.width, height);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_grid_attach(GTK_GRID(grid), w, 0, 0, 1, 1);

  GtkTreeView *listview = GTK_TREE_VIEW(gtk_tree_view_new());
  gtk_container_add(GTK_CONTAINER(w), GTK_WIDGET(listview));
  gtk_tree_view_set_headers_visible(listview, FALSE);
  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(listview), GTK_SELECTION_SINGLE);

  dt_lib_metadata_dialog_t *sd = (dt_lib_metadata_dialog_t *)calloc(1, sizeof(dt_lib_metadata_dialog_t));
  sd->dialog = GTK_DIALOG(dialog);
  sd->textview = d->textview[i];
  sd->listview = listview;
  g_signal_connect(G_OBJECT(listview), "button-press-event", G_CALLBACK(_metadata_selected), sd);

  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(_("metadata"), renderer, "text", 0, NULL);
  gtk_tree_view_append_column(listview, col);
  g_object_set(G_OBJECT(renderer), "wrap-mode", PANGO_WRAP_WORD, NULL);
  g_object_set(G_OBJECT(renderer), "wrap-width", metadata_allocation.width, NULL);
  g_signal_connect(GTK_WIDGET(dialog), "configure-event", G_CALLBACK(_metadata_list_size_changed), renderer);

  GtkListStore *liststore = gtk_list_store_new(1, G_TYPE_STRING);
  GtkTreeIter iter;
  for(GList *item = d->metadata_list[i]; item; item = g_list_next(item))
  {
    gtk_list_store_append(liststore, &iter);
    gtk_list_store_set(liststore, &iter, 0, (char *)item->data, -1);
  }
  gtk_tree_view_set_model(listview, GTK_TREE_MODEL(liststore));
  g_object_unref(liststore);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_widget_show_all(dialog);
  const int res = gtk_dialog_run(GTK_DIALOG(dialog));
  if(res == GTK_RESPONSE_YES)
  {
    gtk_widget_grab_focus(GTK_WIDGET(d->textview[i]));
  }
  g_free(sd);
  gtk_widget_destroy(dialog);
  return TRUE;
}

static gboolean _metadata_reset(GtkWidget *label, GdkEventButton *event, GtkWidget *widget)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(widget));
    gtk_text_buffer_set_text(buffer, "", -1);

    GdkEventKey e = {0};
    e.type = GDK_KEY_PRESS;
    e.keyval = GDK_KEY_KP_Enter;
    e.send_event = TRUE;
    e.window = gtk_text_view_get_window(GTK_TEXT_VIEW(widget), GTK_TEXT_WINDOW_TEXT);
    gboolean ret_val;
    g_signal_emit_by_name(G_OBJECT(widget), "key-press-event", &e, &ret_val);
  }
  return FALSE;
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)calloc(1, sizeof(dt_lib_metadata_t));
  self->data = (void *)d;

  self->timeout_handle = 0;

  GtkGrid *grid = (GtkGrid *)gtk_grid_new();
  self->widget = GTK_WIDGET(grid);
  gtk_grid_set_row_spacing(grid, DT_PIXEL_APPLY_DPI(5));
  grid = (GtkGrid *)gtk_grid_new();
  d->metadata_grid = grid;
  gtk_grid_attach(GTK_GRID(self->widget), GTK_WIDGET(grid), 0, 0, 1, 1);

  dt_gui_add_help_link(self->widget, dt_get_help_url("metadata"));
  gtk_grid_set_row_spacing(grid, DT_PIXEL_APPLY_DPI(5));
  gtk_grid_set_column_spacing(grid, DT_PIXEL_APPLY_DPI(10));

  for(int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    GtkWidget *label = dt_ui_label_new(_(dt_metadata_get_name_by_display_order(i)));
    GtkWidget *labelev = gtk_event_box_new();
    gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(labelev), label);
    gtk_grid_attach(grid, labelev, 0, i, 1, 1);
    gtk_widget_set_tooltip_text(GTK_WIDGET(label),
              _("metadata text. ctrl-wheel scroll to resize the text box"
              "\n ctrl-enter inserts a new line (caution, may not be compatible with standard metadata)."
              "\nif <leave unchanged> selected images have different metadata."
              "\nin that case, right-click gives the possibility to choose one of them."
              "\npress escape to exit the popup window"));

    GtkWidget *textview = gtk_text_view_new();
    gtk_text_buffer_create_tag (gtk_text_view_get_buffer(GTK_TEXT_VIEW(textview)),
                                "italic", "style", PANGO_STYLE_ITALIC, NULL);

    const char *name = (char *)dt_metadata_get_name_by_display_order(i);
    d->setting_name[i] = g_strdup_printf("plugins/lighttable/metadata/%s_text_height", name);

    GtkWidget *swindow = dt_ui_scroll_wrap(GTK_WIDGET(textview), 100, d->setting_name[i]);

    gtk_grid_attach(grid, swindow, 1, i, 1, 1);
    gtk_widget_set_hexpand(swindow, TRUE);
    d->swindow[i] = swindow;
    gtk_widget_set_size_request(d->swindow[i], -1, DT_PIXEL_APPLY_DPI(30));

    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(textview), GTK_WRAP_WORD);
    gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(textview), FALSE);
    gtk_widget_add_events(textview, GDK_FOCUS_CHANGE_MASK);
    g_signal_connect(textview, "key-press-event", G_CALLBACK(_key_pressed), self);
    g_signal_connect(G_OBJECT(textview), "button-press-event", G_CALLBACK(_click_on_textview), self);
    g_signal_connect(textview, "grab-focus", G_CALLBACK(_got_focus), self);
    g_signal_connect(textview, "focus-out-event", G_CALLBACK(_lost_focus), self);
    g_signal_connect(GTK_EVENT_BOX(labelev), "button-press-event",
                     G_CALLBACK(_metadata_reset), textview);
    d->textview[i] = GTK_TEXT_VIEW(textview);
    gtk_widget_set_hexpand(textview, TRUE);
    gtk_widget_set_vexpand(textview, TRUE);

    // doesn't work. Workaround => gui_post_expose
    // gtk_widget_set_no_show_all(GTK_WIDGET(label), TRUE);
    // gtk_widget_set_no_show_all(GTK_WIDGET(textview), TRUE);
  }

  d->init_layout = FALSE;

  GtkBox *hbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));

  // apply button
  d->apply_button = dt_ui_button_new(_("apply"), _("write metadata for selected images"), NULL);
  gtk_box_pack_start(hbox, d->apply_button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(d->apply_button), "clicked", G_CALLBACK(_apply_button_clicked), self);

  gtk_grid_attach(GTK_GRID(self->widget), GTK_WIDGET(hbox), 0, 1, 1, 1);

  /* lets signup for mouse over image change signals */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(_mouse_over_image_callback), self);

  // and 2 other interesting signals:
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_SELECTION_CHANGED,
                            G_CALLBACK(_image_selection_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_collection_updated_callback), self);
  _update(self);
  _update_layout(self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_cancel_postponed_update(self);
  const dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_mouse_over_image_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_image_selection_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_collection_updated_callback), self);

  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    g_free(d->setting_name[i]);
  }
  free(self->data);
  self->data = NULL;
}

static void add_rights_preset(dt_lib_module_t *self, char *name, char *string)
{
  // to be adjusted the nb of metadata items changes
  const unsigned int metadata_nb = DT_METADATA_NUMBER;
  const unsigned int params_size = strlen(string) + metadata_nb;

  char *params = calloc(sizeof(char), params_size);
  memcpy(params + 4, string, params_size - metadata_nb);
  dt_lib_presets_add(name, self->plugin_name, self->version(), params, params_size, TRUE);
  free(params);
}

void init_presets(dt_lib_module_t *self)
{
  // <title>\0<description>\0<rights>\0<creator>\0<publisher>

  add_rights_preset(self, _("CC BY"), _("Creative Commons Attribution (CC BY)"));
  add_rights_preset(self, _("CC BY-SA"), _("Creative Commons Attribution-ShareAlike (CC BY-SA)"));
  add_rights_preset(self, _("CC BY-ND"), _("Creative Commons Attribution-NoDerivs (CC BY-ND)"));
  add_rights_preset(self, _("CC BY-NC"), _("Creative Commons Attribution-NonCommercial (CC BY-NC)"));
  add_rights_preset(self, _("CC BY-NC-SA"),
                    _("Creative Commons Attribution-NonCommercial-ShareAlike (CC BY-NC-SA)"));
  add_rights_preset(self, _("CC BY-NC-ND"),
                    _("Creative Commons Attribution-NonCommercial-NoDerivs (CC BY-NC-ND)"));
  add_rights_preset(self, _("all rights reserved"), _("all rights reserved"));
}

void *legacy_params(dt_lib_module_t *self, const void *const old_params, const size_t old_params_size,
                    const int old_version, int *new_version, size_t *new_size)
{
  if(old_version == 1)
  {
    size_t new_params_size = old_params_size + 1;
    char *new_params = calloc(sizeof(char), new_params_size);

    const char *buf = (const char *)old_params;

    // <title>\0<description>\0<rights>\0<creator>\0<publisher>
    const char *metadata[DT_METADATA_NUMBER];
    size_t metadata_len[DT_METADATA_NUMBER];
    for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
    {
      metadata[i] = buf;
      if(!metadata[i])
      {
        free(new_params);
        return NULL;
      }
      metadata_len[i] = strlen(metadata[i]) + 1;
      buf += metadata_len[i];
    }

    // <creator>\0<publisher>\0<title>\0<description>\0<rights>
    size_t pos = 0;
    memcpy(new_params + pos, metadata[3], metadata_len[3]);
    pos += metadata_len[3];
    memcpy(new_params + pos, metadata[4], metadata_len[4]);
    pos += metadata_len[4];
    memcpy(new_params + pos, metadata[0], metadata_len[0]);
    pos += metadata_len[0];
    memcpy(new_params + pos, metadata[1], metadata_len[1]);
    pos += metadata_len[1];
    memcpy(new_params + pos, metadata[2], metadata_len[2]);

    *new_size = new_params_size;
    *new_version = 2;
    return new_params;
  }
  else if(old_version == 2)
  {
    size_t new_params_size = old_params_size + 1;
    char *new_params = calloc(sizeof(char), new_params_size);

    memcpy(new_params, old_params, old_params_size);

    *new_size = new_params_size;
    *new_version = 3;
    return new_params;
  }
  return NULL;
}

void *get_params(dt_lib_module_t *self, int *size)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;

  *size = 0;
  char *metadata[DT_METADATA_NUMBER];
  int32_t metadata_len[DT_METADATA_NUMBER];

  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    const uint32_t keyid = dt_metadata_get_keyid_by_display_order(i);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer((GtkTextView *)d->textview[i]);
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    metadata[keyid] = gtk_text_buffer_get_text(buffer, &start, &end, TRUE);
    if(!metadata[keyid]) metadata[keyid] = g_strdup("");
    metadata_len[keyid] = strlen(metadata[keyid]) + 1;
    *size = *size + metadata_len[keyid];
  }

  char *params = (char *)malloc(*size);

  int pos = 0;

  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    memcpy(params + pos, metadata[i], metadata_len[i]);
    pos += metadata_len[i];
    g_free(metadata[i]);
  }

  g_assert(pos == *size);

  return params;
}

// WARNING: also change src/libs/import.c when changing this!
int set_params(dt_lib_module_t *self, const void *params, int size)
{
  if(!params) return 1;
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;

  char *buf = (char *)params;
  char *metadata[DT_METADATA_NUMBER];
  uint32_t metadata_len[DT_METADATA_NUMBER];
  uint32_t total_len = 0;
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    metadata[i] = buf;
    if(!metadata[i]) return 1;
    metadata_len[i] = strlen(metadata[i]) + 1;
    buf += metadata_len[i];
    total_len +=  metadata_len[i];
  }

  if(size != total_len)
    return 1;

  GList *key_value = NULL;

  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    if(metadata[i][0] != '\0') _append_kv(&key_value, dt_metadata_get_key(i), metadata[i]);
  }

  const GList *imgs = dt_view_get_images_to_act_on(FALSE, TRUE, FALSE);
  dt_metadata_set_list(imgs, key_value, TRUE);

  g_list_free(key_value);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE);
  dt_image_synch_xmps(imgs);
  // force the ui refresh to update the info from preset
  g_list_free(d->last_act_on);
  d->last_act_on = NULL;
  _update(self);
  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
