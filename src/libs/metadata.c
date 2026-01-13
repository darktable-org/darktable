/*
    This file is part of darktable,
    Copyright (C) 2010-2026 darktable developers.

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
#include "control/signal.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/gtkentry.h"
#include "gui/metadata_tags.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#include <gdk/gdkkeysyms.h>

DT_MODULE(5)

typedef enum dt_metadata_pref_cols_t
{
  DT_METADATA_PREF_COL_KEY,         // key
  DT_METADATA_PREF_COL_TAGNAME,     // xmp tag name
  DT_METADATA_PREF_COL_NAME,        // displayed name
  DT_METADATA_PREF_COL_VISIBLE,     // visibility
  DT_METADATA_PREF_COL_PRIVATE,     // do not export
  DT_METADATA_PREF_NUM_COLS
} dt_metadata_pref_cols_t;

typedef struct dt_lib_metadata_t
{
  GHashTable *metadata_texts;
  GHashTable *metadata_counts;
  GList *setting_names;
  GtkWidget *grid, *button_box, *apply_button, *cancel_button, *delete_button;
  GtkWidget *dialog;
  GtkListStore *liststore;
  GtkTreeView *tree_view;
  GList *last_act_on;
  GList *metadata_to_delete;
  int num_grid_rows;
  gboolean needs_rebuild;
} dt_lib_metadata_t;

const char *name(dt_lib_module_t *self)
{
  return _("metadata editor");
}

const char *description(dt_lib_module_t *self)
{
  return _("modify text metadata fields of\n"
           "the currently selected images");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_LIGHTTABLE | DT_VIEW_TETHERING;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

static gchar *_get_buffer_text(GtkTextView *textview)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(textview);
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  return gtk_text_buffer_get_text(buffer, &start, &end, TRUE);
}

static gboolean _is_leave_unchanged(GtkTextView *textview)
{
  return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(textview), "tv_multiple"));
}

static gboolean _set_leave_unchanged(GtkTextView *textview, GtkWidget *label)
{
  gchar *text = _get_buffer_text(textview);
  const gchar *text_orig = (gchar *)g_object_get_data(G_OBJECT(textview), "text_orig");
  const gboolean leave_unchanged = _is_leave_unchanged(textview);
  const gboolean this_changed = text_orig && !leave_unchanged
                              ? strcmp(text, text_orig)
                              : text[0] != 0;
  g_free(text);

  gtk_widget_set_name(label, this_changed ? "dt-metadata-changed" : NULL);

  gtk_container_foreach(GTK_CONTAINER(textview),
                        (GtkCallback)gtk_widget_set_visible,
                        GINT_TO_POINTER(leave_unchanged && !this_changed));
  return this_changed;
}

static GtkLabel *_get_label_from_grid_at_row(const uint32_t row, dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;

  GtkWidget *cell = gtk_grid_get_child_at(GTK_GRID(d->grid), 0, row);
  GtkLabel *label = g_object_get_data(G_OBJECT(cell), "label");
  return label;
}

static GtkTextView *_get_textview_from_grid_at_row(const uint32_t row, dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;

  GtkWidget *cell = gtk_grid_get_child_at(GTK_GRID(d->grid), 1, row);
  GtkTextView *textview = g_object_get_data(G_OBJECT(cell), "textview");
  return textview;
}

static void _textbuffer_changed(GtkTextBuffer *buffer, dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;
  if(darktable.gui->reset) return;

  gboolean changed = FALSE;
  for(unsigned int row = 0; row < d->num_grid_rows; row++)
  {
    GtkLabel *label = _get_label_from_grid_at_row(row, self);
    GtkTextView *textview = _get_textview_from_grid_at_row(row, self);

    if(label)
      changed |= _set_leave_unchanged(textview, GTK_WIDGET(label));
  }

  gtk_widget_set_sensitive(d->button_box, changed);
}

static GtkTextView *_get_textview_by_key(const uint32_t key,
                                         dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = self->data;

  // find textview by metadata key
  for(int row = 0; row < d->num_grid_rows; row++)
  {
    GtkWidget *cell = gtk_grid_get_child_at(GTK_GRID(d->grid), 1, row);
    const uint32_t metadata_key = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cell), "key"));
    if(metadata_key == key)
    {
      GtkTextView *textview = g_object_get_data(G_OBJECT(cell), "textview");
      return textview;
    }
  }

  return NULL;
}

static void _clear_grid(dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;

  for(unsigned int row = 0; row < d->num_grid_rows; row++)
  {
    GtkWidget *cell = gtk_grid_get_child_at(GTK_GRID(d->grid), 1, row);
    GtkTextView *textview = g_object_get_data(G_OBJECT(cell), "textview");
    g_object_set_data(G_OBJECT(textview), "tv_multiple", GINT_TO_POINTER(FALSE));
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(textview);
    gtk_text_buffer_set_text(buffer, "", -1);
    gchar *text_orig = g_object_get_data(G_OBJECT(textview), "text_orig");
    g_free(text_orig);
    g_object_set_data(G_OBJECT(textview), "text_orig", g_strdup(""));
  }
}

static void _fill_textview(gpointer key, gpointer value, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;

  const uint32_t metadata_key = GPOINTER_TO_INT(key);
  GList *texts = value;
  const int count =  GPOINTER_TO_INT(g_hash_table_lookup(d->metadata_counts,
                                     GINT_TO_POINTER(key)));

  GtkTextView *textview = _get_textview_by_key(metadata_key, self);

  if(textview)
  {
    g_object_set_data(G_OBJECT(textview), "tv_multiple", GINT_TO_POINTER(count == 1));
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(textview);

    if(count <= 1)
      gtk_text_buffer_set_text(buffer, "", -1);
    else
    {
      gchar *text = texts? texts->data : "";
      gtk_text_buffer_set_text(buffer, text, -1);
      gchar *text_orig = (gchar *)g_object_get_data(G_OBJECT(textview), "text_orig");
      g_free(text_orig);
      g_object_set_data(G_OBJECT(textview), "text_orig", g_strdup(text));
    }
  }
}

static void _write_metadata(dt_lib_module_t *self);

static void _free_list_entry(gpointer key, gpointer value, gpointer user_data)
{
  g_list_free_full((GList *)value, g_free);

  if(user_data)
  {
    dt_lib_metadata_t *d = (dt_lib_metadata_t *)user_data;
    g_hash_table_replace(d->metadata_texts,
                         key,
                         NULL);
  }
}

static void _reset_count_entry(gpointer key, gpointer value, gpointer user_data)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)user_data;
  g_hash_table_replace(d->metadata_counts, key, GINT_TO_POINTER(0));
}

void gui_update(dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = self->data;

  GList *imgs = dt_act_on_get_images(FALSE, FALSE, FALSE);

  // first we want to make sure the list of images to act on has changed
  // this is not the case if mouse hover change but still stay in selection for ex.
  if(imgs && d->last_act_on && dt_list_length_equal(imgs, d->last_act_on))
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
    if(!changed)
    {
      g_list_free(imgs);
      return;
    }
  }

  _write_metadata(self);
  d->last_act_on = imgs;

  // using dt_metadata_get() is not possible here. we want to do all
  // this in a single pass, everything else takes ages.
  gchar *images = dt_act_on_get_query(FALSE);
  const uint32_t imgs_count = g_list_length((GList *)imgs);

  g_hash_table_foreach(d->metadata_texts, _free_list_entry, d);
  g_hash_table_foreach(d->metadata_counts, _reset_count_entry, d);

  if(images)
  {
    sqlite3_stmt *stmt;
    // clang-format off
    gchar *query = g_strdup_printf(
                            "SELECT key, value, COUNT(id)"
                            " FROM main.meta_data"
                            " WHERE id IN (%s)"
                            " GROUP BY key, value",
                            images);
    // clang-format on
    g_free(images);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);

    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      if(sqlite3_column_bytes(stmt, 1))
      {
        const uint32_t key = (uint32_t)sqlite3_column_int(stmt, 0);
        char *value = g_strdup((char *)sqlite3_column_text(stmt, 1));
        const uint32_t count = (uint32_t)sqlite3_column_int(stmt, 2);

        GList *texts = g_hash_table_lookup(d->metadata_texts, GINT_TO_POINTER(key));
        texts = g_list_prepend(texts, value);
        g_hash_table_replace(d->metadata_texts,
                             GINT_TO_POINTER(key),
                             texts);
        g_hash_table_replace(d->metadata_counts,
                             GINT_TO_POINTER(key),
                             GINT_TO_POINTER((count == imgs_count) ? 2 : 1));
      }
    }
    sqlite3_finalize(stmt);
    g_free(query);
  }

  ++darktable.gui->reset;
  _clear_grid(self);
  g_hash_table_foreach(d->metadata_texts, _fill_textview, self);
  --darktable.gui->reset;

  _textbuffer_changed(NULL, self);
  gtk_widget_set_sensitive(d->grid, imgs_count > 0);
}

static void _image_selection_changed_callback(gpointer instance,
                                              dt_lib_module_t *self)
{
  dt_lib_gui_queue_update(self);
}

static void _collection_updated_callback(gpointer instance,
                                         const dt_collection_change_t query_change,
                                         const dt_collection_properties_t changed_property,
                                         gpointer imgs,
                                         const int next,
                                         dt_lib_module_t *self)
{
  dt_lib_gui_queue_update(self);
}

static void _append_kv(GList **l,
                       const gchar *key,
                       const gchar *value)
{
  *l = g_list_append(*l, (gchar *)key);
  *l = g_list_append(*l, (gchar *)value);
}

static void _metadata_set_list(const int i,
                               GList **key_value,
                               dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;

  GtkWidget *cell = gtk_grid_get_child_at(GTK_GRID(d->grid), 1, i);
  GtkTextView *textview = g_object_get_data(G_OBJECT(cell), "textview");
  if(!textview) return;
  const char *tagname = (char *)g_object_get_data(G_OBJECT(textview), "tagname");

  gchar *metadata = _get_buffer_text(GTK_TEXT_VIEW(textview));
  const gchar *text_orig = g_object_get_data(G_OBJECT(textview), "text_orig");
  const gboolean this_changed = text_orig
                                && !_is_leave_unchanged(textview)
                              ? strcmp(metadata, text_orig)
                              : metadata[0] != 0;
  if(this_changed)
    _append_kv(key_value, tagname, metadata);
  else
    g_free(metadata);
}

static void _write_metadata(dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = self->data;

  GList *key_value = NULL;
  for(unsigned int i = 0; i < d->num_grid_rows; i++)
    _metadata_set_list(i, &key_value, self);

  if(key_value && d->last_act_on)
  {
    dt_gui_cursor_set_busy();
    dt_metadata_set_list(d->last_act_on, key_value, TRUE);

    // NOTE: we free only one item out of two
    for(GList *l = key_value; l; l = l->next->next)
      g_free(l->next->data);
    g_list_free(key_value);

    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE);
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_METADATA_CHANGED, DT_METADATA_SIGNAL_NEW_VALUE);

    dt_image_synch_xmps(d->last_act_on);
    dt_gui_cursor_clear_busy();
  }

  g_list_free(d->last_act_on);
  d->last_act_on = NULL;

  dt_lib_gui_queue_update(self);
}

static void _apply_button_clicked(GtkButton *button,
                                  dt_lib_module_t *self)
{
  _write_metadata(self);

  gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
}

static void _cancel_button_clicked(GtkButton *button,
                                   dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = self->data;
  g_list_free(d->last_act_on);
  d->last_act_on = NULL;

  dt_lib_gui_queue_update(self);
  gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
}

static gboolean _key_pressed(GtkWidget *textview,
                             GdkEventKey *event,
                             dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = self->data;

  switch(event->keyval)
  {
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
      if(!dt_modifier_is(event->state, GDK_CONTROL_MASK))
      {
        gtk_button_clicked(GTK_BUTTON(d->apply_button));
        return TRUE;
      }
      break;
    case GDK_KEY_Escape:
      if(dt_modifier_is(event->state, 0))
      {
        gtk_button_clicked(GTK_BUTTON(d->cancel_button));
        return TRUE;
      }
      break;
    default:
      break;
  }

  return gtk_text_view_im_context_filter_keypress(GTK_TEXT_VIEW(textview), event);
}

static gboolean _textview_focus(GtkWidget *widget,
                                GtkDirectionType d,
                                gpointer user_data)
{
  GtkWidget *target = g_object_get_data
    (G_OBJECT(widget),
     d == GTK_DIR_TAB_FORWARD ? "meta_next" : "meta_prev");
  gtk_widget_grab_focus(target);
  return TRUE;
}

int position(const dt_lib_module_t *self)
{
  return 510;
}

static void _update_layout(dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = self->data;

  GtkWidget *first = NULL, *previous = NULL;

  dt_pthread_mutex_lock(&darktable.metadata_threadsafe);
  for(GList *iter = dt_metadata_get_list(); iter; iter = iter->next)
  {
    dt_metadata_t *metadata = (dt_metadata_t *)iter->data;

    const gboolean visible = !metadata->internal && metadata->visible;

    int row = 0;
    while(row < d->num_grid_rows)
    {
      GtkWidget *lb_cell = gtk_grid_get_child_at(GTK_GRID(d->grid), 0, row);
      GtkWidget *tv_cell = gtk_grid_get_child_at(GTK_GRID(d->grid), 1, row);
      const uint32_t key = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(tv_cell), "key"));

      if(key == metadata->key)
      {
        gtk_widget_set_visible(lb_cell, visible);
        gtk_widget_set_visible(tv_cell, visible);

        GtkWidget *label = g_object_get_data(G_OBJECT(lb_cell), "label");
        gtk_label_set_label(GTK_LABEL(label), metadata->name);

        if(visible)
        {
          GtkWidget *current = g_object_get_data(G_OBJECT(tv_cell), "textview");
          _set_leave_unchanged(GTK_TEXT_VIEW(current), label);

          if(!first) first = previous = current;

          g_object_set_data(G_OBJECT(previous), "meta_next", current);
          g_object_set_data(G_OBJECT(current), "meta_prev", previous);

          g_object_set_data(G_OBJECT(current), "meta_next", first);
          g_object_set_data(G_OBJECT(first), "meta_prev", current);

          previous = current;
        }
        break;
      }
      row++;
    }
  }
  dt_pthread_mutex_unlock(&darktable.metadata_threadsafe);
}

void gui_reset(dt_lib_module_t *self)
{
  ++darktable.gui->reset;
  dt_pthread_mutex_lock(&darktable.metadata_threadsafe);
  for(GList *iter = dt_metadata_get_list(); iter; iter = iter->next)
  {
    dt_metadata_t *metadata = (dt_metadata_t *)iter->data;

    if(!metadata->internal && metadata->visible)
    {
      GtkTextView *textview = _get_textview_by_key(metadata->key, self);
      GtkTextBuffer *buffer = gtk_text_view_get_buffer(textview);
      gtk_text_buffer_set_text(buffer, "", -1);
    }
  }
  dt_pthread_mutex_unlock(&darktable.metadata_threadsafe);
  --darktable.gui->reset;

  _write_metadata(self);
}

static void _menu_line_activated(GtkMenuItem *menuitem, GtkTextView *textview)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(textview);
  gtk_text_buffer_set_text
    (buffer,
     gtk_label_get_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(menuitem)))), -1);
}

static void _populate_popup_multi(GtkTextView *textview,
                                  GtkWidget *popup,
                                  dt_lib_module_t *self)
{
  const dt_lib_metadata_t *d = self->data;

  if(!_is_leave_unchanged(textview)) return;

  // get keyid from textview
  const uint32_t key = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(textview), "key"));
  GList *texts = g_hash_table_lookup(d->metadata_texts, GINT_TO_POINTER(key));

  gtk_menu_shell_append(GTK_MENU_SHELL(popup),gtk_separator_menu_item_new());

  for(GList *item = texts; item; item = item->next)
  {
    GtkWidget *new_line = gtk_menu_item_new_with_label(item->data);
    g_signal_connect(G_OBJECT(new_line), "activate", G_CALLBACK(_menu_line_activated), textview);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup), new_line);
  }
  gtk_widget_show_all(popup);
}

static gboolean _metadata_reset(GtkWidget *label,
                                GdkEventButton *event,
                                GtkWidget *widget)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    g_object_set_data(G_OBJECT(widget), "tv_multiple", GINT_TO_POINTER(FALSE));
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(widget));
    if(gtk_text_buffer_get_char_count(buffer))
      gtk_text_buffer_set_text(buffer, "", -1);
    else
      g_signal_emit_by_name(G_OBJECT(buffer), "changed"); // even if unchanged
  }
  return TRUE;
}

static void _add_grid_row(dt_metadata_t *metadata, int row, dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;

  GtkGrid *grid = GTK_GRID(d->grid);
  gtk_grid_insert_row(GTK_GRID(d->grid), row);
  GtkWidget *label = dt_ui_label_new(metadata->name);
  gtk_widget_set_halign(label, GTK_ALIGN_FILL);
  gtk_widget_set_valign(label, GTK_ALIGN_START);
  GtkWidget *labelev = gtk_event_box_new();
  gtk_widget_set_tooltip_text(labelev, _("double-click to reset"));
  gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
  gtk_container_add(GTK_CONTAINER(labelev), label);
  g_object_set_data(G_OBJECT(labelev), "label", label);
  gtk_grid_attach(grid, labelev, 0, row, 1, 1);

  GtkWidget *textview = gtk_text_view_new();
  dt_action_define(DT_ACTION(self), NULL, metadata->name, textview, &dt_action_def_entry);
  gtk_widget_set_tooltip_text(textview,
            _("metadata text"
            "\nctrl+enter inserts a new line (caution, may not be compatible with standard metadata)"
            "\nif <leave unchanged> selected images have different metadata"
            "\nin that case, right-click gives the possibility to choose one of them"
            "\nescape to exit the popup window"));
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textview));
  g_object_set_data(G_OBJECT(textview), "tv_multiple", GINT_TO_POINTER(FALSE));
  g_object_set_data(G_OBJECT(textview), "text_orig", NULL);
  g_object_set_data(G_OBJECT(textview), "key", GINT_TO_POINTER(metadata->key));
  g_object_set_data(G_OBJECT(textview), "tagname", metadata->tagname);

  GtkWidget *unchanged = gtk_label_new(_("<leave unchanged>"));
  gtk_widget_set_name(unchanged, "dt-metadata-multi");
  gtk_widget_set_visible(unchanged, FALSE);
  gtk_text_view_add_child_in_window(GTK_TEXT_VIEW(textview), unchanged, GTK_TEXT_WINDOW_WIDGET, 0, 0);

  gchar *setting_name = g_strdup_printf("plugins/lighttable/metadata/%s_text_height", dt_metadata_get_tag_subkey(metadata->tagname));
  GtkWidget *swindow = dt_ui_resize_wrap(GTK_WIDGET(textview), 100, setting_name);
  d->setting_names = g_list_append(d->setting_names, setting_name);
  g_object_set_data(G_OBJECT(swindow), "key", GINT_TO_POINTER(metadata->key));
  g_object_set_data(G_OBJECT(swindow), "textview", textview);

  gtk_grid_attach(grid, swindow, 1, row, 1, 1);
  gtk_widget_set_hexpand(swindow, TRUE);

  //workaround for a Gtk issue where the textview does not wrap correctly
  //while resizing the panel or typing into the widget
  //reported upstream to https://gitlab.gnome.org/GNOME/gtk/-/issues/4042
  //see also discussions on https://github.com/darktable-org/darktable/pull/10584
  GtkScrolledWindow *realsw = GTK_SCROLLED_WINDOW(gtk_widget_get_parent(textview));
  gtk_scrolled_window_set_policy(realsw, GTK_POLICY_EXTERNAL, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_width(realsw, DT_PIXEL_APPLY_DPI(30));

  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(textview), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(textview), FALSE);
  gtk_widget_add_events(textview, GDK_FOCUS_CHANGE_MASK | GDK_ENTER_NOTIFY_MASK);
  g_signal_connect(textview, "key-press-event", G_CALLBACK(_key_pressed), self);
  g_signal_connect(textview, "focus", G_CALLBACK(_textview_focus), self);
  g_signal_connect(textview, "populate-popup", G_CALLBACK(_populate_popup_multi), self);
  g_signal_connect(labelev, "button-press-event", G_CALLBACK(_metadata_reset), textview);
  g_signal_connect(buffer, "changed", G_CALLBACK(_textbuffer_changed), self);
  gtk_widget_set_hexpand(textview, TRUE);
  gtk_widget_set_vexpand(textview, TRUE);
  d->num_grid_rows++;
}

static void _toggled_callback(gchar *path_str,
                              gpointer user_data,
                              const int column)
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

static void _visible_toggled_callback(GtkCellRendererToggle *cell_renderer,
                                      gchar *path_str,
                                      gpointer user_data)
{
  _toggled_callback(path_str, user_data, DT_METADATA_PREF_COL_VISIBLE);
}

static void _private_toggled_callback(GtkCellRendererToggle *cell_renderer,
                                      gchar *path_str,
                                      gpointer user_data)
{
  _toggled_callback(path_str, user_data, DT_METADATA_PREF_COL_PRIVATE);
}

static void _display_name_edited_callback(GtkCellRenderer *renderer,
                                          gchar *path_str,
                                          gchar *new_text,
                                          gpointer user_data)
{
  GtkListStore *store = (GtkListStore *)user_data;
  GtkTreeIter iter;
  GtkTreePath *path = gtk_tree_path_new_from_string(path_str);

  gtk_tree_model_get_iter(GTK_TREE_MODEL(store), &iter, path);
  gtk_list_store_set(store, &iter, DT_METADATA_PREF_COL_NAME, new_text, -1);
  gtk_tree_path_free(path);
}

static gboolean _find_metadata_iter_per_text(GtkTreeModel *model, GtkTreeIter *iter, gint col, const char *text)
{
  if(!text) return FALSE;
  GtkTreeIter it;
  gboolean valid = gtk_tree_model_get_iter_first(model, &it);
  char *name;
  while(valid)
  {
    gtk_tree_model_get(model, &it, col, &name, -1);
    const gboolean found = g_strcmp0(text, name) == 0;
    g_free(name);
    if(found)
    {
      if(iter) *iter = it;
      return TRUE;
    }
    valid = gtk_tree_model_iter_next(model, &it);
  }
  return FALSE;
}

static void _add_selected_metadata(gchar *tagname, dt_lib_metadata_t *d)
{
  GtkTreeIter iter;
  if(!_find_metadata_iter_per_text(GTK_TREE_MODEL(d->liststore), NULL, DT_METADATA_PREF_COL_TAGNAME, tagname))
  {
    gtk_list_store_insert_with_values(d->liststore, &iter, -1,
                       DT_METADATA_PREF_COL_KEY, -1,  // -1 indicates a new entry
                       DT_METADATA_PREF_COL_TAGNAME, tagname,
                       DT_METADATA_PREF_COL_NAME, dt_metadata_get_tag_subkey(tagname),
                       DT_METADATA_PREF_COL_VISIBLE, TRUE,
                       DT_METADATA_PREF_COL_PRIVATE, FALSE,
                       -1);

    GtkTreeSelection *selection = gtk_tree_view_get_selection(d->tree_view);
    gtk_tree_selection_select_iter(selection, &iter);
  }
  g_free(tagname);
}

static void _metadata_activated(GtkTreeView *tree_view,
                                GtkTreePath *path,
                                GtkTreeViewColumn *column,
                                dt_lib_metadata_t *d)
{
  gchar *tagname = dt_metadata_tags_get_selected();
  _add_selected_metadata(tagname, d);
}

// dialog to add metadata tag into the formula list
static void _add_tag_button_clicked(GtkButton *button, dt_lib_metadata_t *d)
{
  GtkWidget *dialog = dt_metadata_tags_dialog(d->dialog, TRUE, _metadata_activated, d);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif

  gtk_widget_show_all(dialog);
  while(gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *tagname = dt_metadata_tags_get_selected();
    _add_selected_metadata(tagname, d);
  }
  gtk_widget_destroy(dialog);
}

static void _delete_tag_button_clicked(GtkButton *button, dt_lib_metadata_t *d)
{
  GtkTreeIter iter;
  GtkTreeModel *model = GTK_TREE_MODEL(d->liststore);
  GtkTreeSelection *selection = gtk_tree_view_get_selection(d->tree_view);
  if(gtk_tree_selection_get_selected(selection, &model, &iter))
  {
    uint32_t key;
    gtk_tree_model_get(model, &iter,
                       DT_METADATA_PREF_COL_KEY, &key,
                       -1);
    if(key != -1)
      d->metadata_to_delete = g_list_prepend(d->metadata_to_delete,
                                             GINT_TO_POINTER(key));
    gtk_list_store_remove(d->liststore, &iter);
  }
}

static void _drag_data_inserted(GtkTreeModel *tree_model, GtkTreePath *path, GtkTreeIter *iter, dt_lib_metadata_t *d)
{
  d->needs_rebuild = TRUE;
}


static void _fill_grid(dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;

  for(int32_t i = d->num_grid_rows - 1; i >= 0; i--)
    gtk_grid_remove_row(GTK_GRID(d->grid), i);

  g_hash_table_foreach(d->metadata_texts, _free_list_entry, NULL);
  g_hash_table_destroy(d->metadata_texts);
  g_hash_table_destroy(d->metadata_counts);

  d->metadata_texts = g_hash_table_new(NULL, NULL);
  d->metadata_counts = g_hash_table_new(NULL, NULL);
  d->metadata_to_delete = NULL;

  d->num_grid_rows = 0;

  gtk_widget_set_no_show_all(d->grid, FALSE);

  int row = 0;
  dt_pthread_mutex_lock(&darktable.metadata_threadsafe);
  for(GList *iter = dt_metadata_get_list(); iter; iter = iter->next)
  {
    dt_metadata_t *metadata = (dt_metadata_t *)iter->data;

    if(metadata->internal)
      continue;

    g_hash_table_insert(d->metadata_texts,
                        GINT_TO_POINTER(metadata->key),
                        NULL);
    g_hash_table_insert(d->metadata_counts,
                        GINT_TO_POINTER(metadata->key),
                        GINT_TO_POINTER(0));

    _add_grid_row(metadata, row, self);
    row++;
  }
  dt_pthread_mutex_unlock(&darktable.metadata_threadsafe);

  gtk_widget_show_all(d->grid);
  gtk_widget_set_no_show_all(d->grid, TRUE);
  dt_lib_gui_queue_update(self);
}

static void _menuitem_preferences(GtkMenuItem *menuitem,
                                  dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;

  GtkCellEditable *active_editable = NULL;

  g_list_free(d->metadata_to_delete);
  d->metadata_to_delete = NULL;

  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("metadata settings"), GTK_WINDOW(win),
                                                  GTK_DIALOG_DESTROY_WITH_PARENT,
                                                  _("_cancel"), GTK_RESPONSE_NONE,
                                                  _("_save"), GTK_RESPONSE_ACCEPT, NULL);
  d->dialog = dialog;
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
  dt_gui_dialog_add_help(GTK_DIALOG(dialog), "metadata_preferences");
  dt_gui_dialog_restore_size(GTK_DIALOG(dialog), "metadata");
  g_signal_connect(dialog, "key-press-event", G_CALLBACK(dt_handle_dialog_enter), NULL);

  GtkListStore *store = gtk_list_store_new(DT_METADATA_PREF_NUM_COLS,
                                           G_TYPE_INT,      // key
                                           G_TYPE_STRING,   // xmp tag name
                                           G_TYPE_STRING,   // displayed name
                                           G_TYPE_BOOLEAN,  // visibility
                                           G_TYPE_BOOLEAN); // do not export

  d->liststore = store;
  GtkTreeModel *model = GTK_TREE_MODEL(store);
  GtkTreeIter iter;

  dt_pthread_mutex_lock(&darktable.metadata_threadsafe);
  for(GList *metadata_iter = dt_metadata_get_list(); metadata_iter; metadata_iter = metadata_iter->next)
  {
    dt_metadata_t *metadata = (dt_metadata_t *)metadata_iter->data;
    if(!metadata->internal)
    {
      gtk_list_store_insert_with_values(store, NULL, -1,
                         DT_METADATA_PREF_COL_KEY, metadata->key,
                         DT_METADATA_PREF_COL_TAGNAME, metadata->tagname,
                         DT_METADATA_PREF_COL_NAME, metadata->name,
                         DT_METADATA_PREF_COL_VISIBLE, metadata->visible,
                         DT_METADATA_PREF_COL_PRIVATE, metadata->priv,
                         -1);
    }
  }
  dt_pthread_mutex_unlock(&darktable.metadata_threadsafe);

  GtkWidget *view = gtk_tree_view_new_with_model(model);
  d->tree_view = GTK_TREE_VIEW(view);
  g_object_unref(model);
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes
    (_("XMP tag name"), renderer,
     "text", DT_METADATA_PREF_COL_TAGNAME, NULL);
  gtk_tree_view_column_set_expand(column, TRUE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);
  renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes
    (_("display name"), renderer,
     "text", DT_METADATA_PREF_COL_NAME, NULL);
  g_object_set(renderer, "editable", TRUE, NULL);
  g_signal_connect(G_OBJECT(renderer), "edited", G_CALLBACK(_display_name_edited_callback), store);
  dt_gui_commit_on_focus_loss(renderer, &active_editable);
  gtk_tree_view_column_set_expand(column, TRUE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);
  renderer = gtk_cell_renderer_toggle_new();
  g_signal_connect(renderer, "toggled", G_CALLBACK(_visible_toggled_callback), store);
  column = gtk_tree_view_column_new_with_attributes
    (_("visible"), renderer,
     "active", DT_METADATA_PREF_COL_VISIBLE, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);
  GtkTreePath *first = gtk_tree_path_new_first ();
  gtk_tree_view_set_cursor(GTK_TREE_VIEW(view), first, column, FALSE);
  gtk_tree_path_free(first);
  gtk_widget_set_tooltip_text(view,
                _("drag and drop one row at a time until you get the desired order"));

  // drag & drop
  gtk_tree_view_set_reorderable(GTK_TREE_VIEW(view), TRUE);
  g_signal_connect(G_OBJECT(model), "row-inserted", G_CALLBACK(_drag_data_inserted), d);

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
                _("tick if you want to keep this information private"
                  " (not exported with images)"));

  GtkWidget *w = dt_gui_scroll_wrap(view);
  gtk_widget_set_size_request(w, -1, DT_PIXEL_APPLY_DPI(100));
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  GtkWidget *plus = dtgtk_button_new(dtgtk_cairo_paint_plus_simple, 0, NULL);
  gtk_widget_set_tooltip_text(plus, _("add metadata tags"));
  g_signal_connect(G_OBJECT(plus), "clicked", G_CALLBACK(_add_tag_button_clicked), (gpointer)d);

  GtkWidget *minus = dtgtk_button_new(dtgtk_cairo_paint_minus_simple, 0, NULL);
  gtk_widget_set_tooltip_text(minus, _("delete metadata tag"));
  g_signal_connect(G_OBJECT(minus), "clicked", G_CALLBACK(_delete_tag_button_clicked), (gpointer)d);
  d->delete_button = minus;

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  dt_gui_dialog_add(GTK_DIALOG(dialog), w, dt_gui_hbox(dt_gui_expand(dt_gui_align_right(minus)), plus));
  gtk_widget_show_all(dialog);

  d->needs_rebuild = FALSE;

  int res = gtk_dialog_run(GTK_DIALOG(dialog));

  if(res == GTK_RESPONSE_ACCEPT)
  {
    if(active_editable)
      gtk_cell_editable_editing_done(active_editable);

    // delete metadata
    GList *keys_str = NULL;
    for(GList *key_iter = d->metadata_to_delete; key_iter; key_iter = key_iter->next)
      keys_str = g_list_prepend(keys_str, g_strdup_printf("%d", GPOINTER_TO_INT(key_iter->data)));
    gchar *keys = dt_util_glist_to_str(",", keys_str);
    g_list_free_full(keys_str, g_free);
    gboolean confirm_delete = TRUE;
    sqlite3_stmt *stmt = NULL;

    if(keys)
    {
      // check for images with that metadata assigned
      uint32_t count = 0;
      gchar *query = g_strdup_printf("SELECT COUNT(*)"
                                     " FROM main.meta_data"
                                     " WHERE key in (%s)",
                                     keys);
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  query,
                                  -1, &stmt, NULL);
      if(sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
      sqlite3_finalize(stmt);
      g_free(query);

      if(count > 0)
      {
        confirm_delete = dt_gui_show_yes_no_dialog(
          _("delete metadata"), "",
          _("you are about to delete metadata which is currently assigned to images.\n"
            "the assignments will be removed."));
      }

      if(!confirm_delete)
        goto finish;

      // delete the assignments
      query = g_strdup_printf("DELETE FROM main.meta_data WHERE key IN (%s)",
                              keys);
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  query,
                                  -1, &stmt, NULL);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
      g_free(query);

      // now the metadata entries
      query = g_strdup_printf("DELETE FROM data.meta_data WHERE key IN (%s)",
                              keys);
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  query,
                                  -1, &stmt, NULL);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
      g_free(query);

      g_free(keys);

      dt_pthread_mutex_lock(&darktable.metadata_threadsafe);
      // remove the metadata from the variables substitution list
      for(GList *key_iter = d->metadata_to_delete; key_iter; key_iter = key_iter->next)
      {
        uint32_t keyid = GPOINTER_TO_INT(key_iter->data);
        dt_metadata_t *metadata = dt_metadata_get_metadata_by_keyid(keyid);
        if(metadata)
          dt_gtkentry_variables_remove_metadata(metadata);
      }

      // re-initialze the metadata list
      dt_metadata_init();
      dt_pthread_mutex_unlock(&darktable.metadata_threadsafe);

      g_list_free(d->metadata_to_delete);
      d->metadata_to_delete = NULL;

      d->needs_rebuild = TRUE;
    }

    // process the remaining list
    gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
    uint32_t display_order = 0;

    dt_pthread_mutex_lock(&darktable.metadata_threadsafe);
    while(valid)
    {
      int32_t key = 0;
      gchar *tagname = NULL;
      gchar *name = NULL;
      gboolean visible = FALSE;
      gboolean private = FALSE;

      gtk_tree_model_get(model, &iter,
                         DT_METADATA_PREF_COL_KEY, &key,
                         DT_METADATA_PREF_COL_TAGNAME, &tagname,
                         DT_METADATA_PREF_COL_NAME, &name,
                         DT_METADATA_PREF_COL_VISIBLE, &visible,
                         DT_METADATA_PREF_COL_PRIVATE, &private,
                         -1);

      if(key == -1)
      {
        // new metadata entry
        dt_metadata_t *md = calloc(1, sizeof(dt_metadata_t));
        md->tagname = g_strdup(tagname);
        md->name = g_strdup(name);
        md->internal = FALSE;
        md->visible = visible;
        md->priv = private;
        md->display_order = display_order;
        dt_metadata_add_metadata(md);
        dt_gtkentry_variables_add_metadata(md);

        d->needs_rebuild = TRUE;
      }
      else
      {
        // update
        DT_DEBUG_SQLITE3_PREPARE_V2
          (dt_database_get(darktable.db),
          "UPDATE data.meta_data"
          " SET name = ?2"
          "   , visible = ?3"
          "   , private = ?4"
          "   , display_order = ?5"
          " WHERE key = ?1",
          -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, key);
        DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, name, -1, SQLITE_TRANSIENT);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, visible);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, private);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, display_order);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        dt_metadata_t *metadata = dt_metadata_get_metadata_by_keyid(key);
        g_free(metadata->name);
        metadata->name = g_strdup(name);
        metadata->visible = visible;
        metadata->priv = private;
        metadata->display_order = display_order;
      }

      g_free(tagname);
      g_free(name);
      display_order++;

      valid = gtk_tree_model_iter_next(model, &iter);
    }
    dt_metadata_sort();
    dt_pthread_mutex_unlock(&darktable.metadata_threadsafe);

    if(d->needs_rebuild)
      _fill_grid(self);
    _update_layout(self);

    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_METADATA_CHANGED, DT_METADATA_SIGNAL_PREF_CHANGED);
  }

finish:
  gtk_widget_destroy(dialog);
}

void set_preferences(void *menu, dt_lib_module_t *self)
{
  GtkWidget *mi = gtk_menu_item_new_with_label(_("preferences..."));
  g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_menuitem_preferences), self);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = calloc(1, sizeof(dt_lib_metadata_t));
  self->data = (void *)d;

  d->metadata_texts = g_hash_table_new(NULL, NULL);
  d->metadata_counts = g_hash_table_new(NULL, NULL);
  d->metadata_to_delete = NULL;

  GtkWidget *grid = gtk_grid_new();
  d->grid = grid;
  gtk_grid_set_row_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(0));
  gtk_grid_set_column_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(10));

  d->apply_button = dt_action_button_new(self, N_("apply"), _apply_button_clicked, self,
                                         _("write metadata for selected images"), 0, 0);
  d->cancel_button = dt_action_button_new(self, N_("cancel"), _cancel_button_clicked, self,
                                         _("ignore changed metadata"), 0, 0);
  d->button_box = dt_gui_hbox(d->apply_button, d->cancel_button);
  self->widget = dt_gui_vbox(grid, d->button_box);
  _fill_grid(self);

  /* lets signup for mouse over image change signals */
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE, _image_selection_changed_callback);

  // and 2 other interesting signals:
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_SELECTION_CHANGED, _image_selection_changed_callback);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_COLLECTION_CHANGED, _collection_updated_callback);

  gtk_widget_show_all(d->grid);
  gtk_widget_set_no_show_all(d->grid, TRUE);
  _update_layout(self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = self->data;
  for(unsigned int row = 0; row < d->num_grid_rows; row++)
  {
    GtkWidget *cell = gtk_grid_get_child_at(GTK_GRID(d->grid), 1, row);
    GtkTextView *textview = g_object_get_data(G_OBJECT(cell), "textview");
    gchar *text_orig = g_object_get_data(G_OBJECT(textview), "text_orig");
    g_free(text_orig);
    g_object_set_data(G_OBJECT(textview), "text_orig", NULL);
  }

  g_list_free_full(d->setting_names, g_free);
  g_list_free(d->last_act_on);
  g_hash_table_foreach(d->metadata_texts, _free_list_entry, NULL);
  g_hash_table_destroy(d->metadata_texts);
  g_hash_table_destroy(d->metadata_counts);
  free(self->data);
  self->data = NULL;
}

static void add_rights_preset(dt_lib_module_t *self, char *name, char *string)
{
  const char *tagname = "Xmp.dc.rights";
  const uint32_t tagname_len = strlen(tagname) + 1;
  const uint32_t string_len = strlen(string) + 1;

  const unsigned int params_size = tagname_len + string_len;

  char *params = calloc(sizeof(char), params_size);
  if(params)
  {
    size_t pos = 0;
    memcpy(params + pos, tagname, tagname_len);
    pos += tagname_len;
    memcpy(params + pos, string, string_len);
    dt_lib_presets_add(name, self->plugin_name, self->version(),
                       params, params_size, TRUE, 0);
    free(params);
  }
}

void init_presets(dt_lib_module_t *self)
{
  add_rights_preset(self, _("CC BY"),
                    _("Creative Commons Attribution (CC BY)"));
  add_rights_preset(self, _("CC BY-SA"),
                    _("Creative Commons Attribution-ShareAlike (CC BY-SA)"));
  add_rights_preset(self, _("CC BY-ND"),
                    _("Creative Commons Attribution-NoDerivs (CC BY-ND)"));
  add_rights_preset(self, _("CC BY-NC"),
                    _("Creative Commons Attribution-NonCommercial (CC BY-NC)"));
  add_rights_preset(self, _("CC BY-NC-SA"),
                    _("Creative Commons Attribution-NonCommercial-ShareAlike (CC BY-NC-SA)"));
  add_rights_preset(self, _("CC BY-NC-ND"),
                    _("Creative Commons Attribution-NonCommercial-NoDerivs (CC BY-NC-ND)"));
  add_rights_preset(self, _("all rights reserved"),
                    _("all rights reserved"));
}

static size_t _build_tag(char *buf,
                         const char *tagname,
                         const char **tagname_ptr,
                         size_t *tagname_len_ptr,
                         const char **text_ptr,
                         size_t *text_len_ptr)
{
  *tagname_ptr = tagname;
  *tagname_len_ptr = strlen(tagname) + 1;
  *text_ptr = buf;
  *text_len_ptr = strlen(buf) + 1;
  return *text_len_ptr;
}

void *legacy_params(dt_lib_module_t *self,
                    const void *const old_params,
                    const size_t old_params_size,
                    const int old_version,
                    int *new_version,
                    size_t *new_size)
{
  if(old_version == 1)
  {
    const size_t new_params_size = old_params_size + 1;
    char *new_params = calloc(sizeof(char), new_params_size);

    const char *buf = (const char *)old_params;

    enum dt_metadata_v1_t
    {
      DT_METADATA_XMP_DC_CREATOR_V1,
      DT_METADATA_XMP_DC_PUBLISHER_V1,
      DT_METADATA_XMP_DC_TITLE_V1,
      DT_METADATA_XMP_DC_DESCRIPTION_V1,
      DT_METADATA_XMP_DC_RIGHTS_V1,
      DT_METADATA_XMP_ACDSEE_NOTES_V1,
      DT_METADATA_XMP_VERSION_NAME_V1,
      DT_METADATA_XMP_IMAGE_ID_V1,
      DT_METADATA_XMP_PRESERVED_FILENAME_V1,
      DT_METADATA_NUMBER_V1
    };


    // <title>\0<description>\0<rights>\0<creator>\0<publisher>
    const char *metadata[DT_METADATA_NUMBER_V1];
    size_t metadata_len[DT_METADATA_NUMBER_V1];
    for(unsigned int i = 0; i < DT_METADATA_NUMBER_V1; i++)
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
    const size_t new_params_size = old_params_size + 1;
    char *new_params = calloc(sizeof(char), new_params_size);

    memcpy(new_params, old_params, old_params_size);

    *new_size = new_params_size;
    *new_version = 3;
    return new_params;
  }
  else if(old_version == 3)
  {
    const size_t new_params_size = old_params_size + 1;
    char *new_params = calloc(sizeof(char), new_params_size);

    memcpy(new_params, old_params, old_params_size);

    *new_size = new_params_size;
    *new_version = 4;
    return new_params;
  }
  else if(old_version == 4)
  {
    enum dt_metadata_v4_t
    {
      DT_METADATA_XMP_DC_CREATOR_V4,
      DT_METADATA_XMP_DC_PUBLISHER_V4,
      DT_METADATA_XMP_DC_TITLE_V4,
      DT_METADATA_XMP_DC_DESCRIPTION_V4,
      DT_METADATA_XMP_DC_RIGHTS_V4,
      DT_METADATA_XMP_ACDSEE_NOTES_V4,
      DT_METADATA_XMP_VERSION_NAME_V4,
      DT_METADATA_XMP_IMAGE_ID_V4,
      DT_METADATA_XMP_PRESERVED_FILENAME_V4,
      DT_METADATA_NUMBER_V4
    };

    // new format:
    // <tagname>\0<tagvalue>\0<tagname>\0</tagvalue>\0 ...
    const char **metadata_tagnames = calloc(DT_METADATA_NUMBER_V4, sizeof(char *));
    size_t *metadata_tagname_len = calloc(DT_METADATA_NUMBER_V4, sizeof(size_t));
    const char **metadata_texts = calloc(DT_METADATA_NUMBER_V4, sizeof(char *));
    size_t *metadata_len = calloc(DT_METADATA_NUMBER_V4, sizeof(size_t));

    char *buf = (char *)old_params;
    int i = 0;

    // creator
    if(strlen(buf) > 0)
    {
      buf += _build_tag(buf,
                        "Xmp.dc.creator",
                        &metadata_tagnames[i],
                        &metadata_tagname_len[i],
                        &metadata_texts[i],
                        &metadata_len[i]);
      i++;
    }
    else
      buf++;

    // publisher
    if(strlen(buf) > 0)
    {
      buf += _build_tag(buf,
                        "Xmp.dc.publisher",
                        &metadata_tagnames[i],
                        &metadata_tagname_len[i],
                        &metadata_texts[i],
                        &metadata_len[i]);
      i++;
    }
    else
      buf++;

    // title
    if(strlen(buf) > 0)
    {
      buf += _build_tag(buf,
                        "Xmp.dc.title",
                        &metadata_tagnames[i],
                        &metadata_tagname_len[i],
                        &metadata_texts[i],
                        &metadata_len[i]);
      i++;
    }
    else
      buf++;

    // description
    if(strlen(buf) > 0)
    {
      buf += _build_tag(buf,
                        "Xmp.dc.description",
                        &metadata_tagnames[i],
                        &metadata_tagname_len[i],
                        &metadata_texts[i],
                        &metadata_len[i]);
      i++;
    }
    else
      buf++;

    // rights
    if(strlen(buf) > 0)
    {
      buf += _build_tag(buf,
                        "Xmp.dc.rights",
                        &metadata_tagnames[i],
                        &metadata_tagname_len[i],
                        &metadata_texts[i],
                        &metadata_len[i]);
      i++;
    }
    else
      buf++;

    // notes
    if(strlen(buf) > 0)
    {
      buf += _build_tag(buf,
                        "Xmp.acdsee.notes",
                        &metadata_tagnames[i],
                        &metadata_tagname_len[i],
                        &metadata_texts[i],
                        &metadata_len[i]);
      i++;
    }
    else
      buf++;

    // version name
    if(strlen(buf) > 0)
    {
      buf += _build_tag(buf,
                        "Xmp.darktable.version_name",
                        &metadata_tagnames[i],
                        &metadata_tagname_len[i],
                        &metadata_texts[i],
                        &metadata_len[i]);
      i++;
    }
    else
      buf++;

    // preserved filename
    if(strlen(buf) > 0)
    {
      buf += _build_tag(buf,
                        "Xmp.xmpMM.PerservedFileName",
                        &metadata_tagnames[i],
                        &metadata_tagname_len[i],
                        &metadata_texts[i],
                        &metadata_len[i]);
      i++;
    }
    else
      buf++;

    // calculate new buffer size
    size_t new_params_size = 0;
    for(int j = 0; j < i; j++)
      new_params_size += metadata_tagname_len[j] + metadata_len[j];

    char *new_params = calloc(sizeof(char), new_params_size);

    size_t pos = 0;
    for(int j = 0; j < i; j++)
    {
      memcpy(new_params + pos, metadata_tagnames[j], metadata_tagname_len[j]);
      pos += metadata_tagname_len[j];
      memcpy(new_params + pos, metadata_texts[j], metadata_len[j]);
      pos += metadata_len[j];
    }

    free(metadata_tagname_len);
    free(metadata_tagnames);
    free(metadata_texts);
    free(metadata_len);

    *new_size = new_params_size;
    *new_version = 5;
    return new_params;
  }
  return NULL;
}

void *get_params(dt_lib_module_t *self, int *size)
{
  *size = 0;

  dt_pthread_mutex_lock(&darktable.metadata_threadsafe);
  const int metadata_nb = g_list_length(dt_metadata_get_list());

  char **metadata_tagnames = calloc(metadata_nb, sizeof(char *));
  int32_t *metadata_tagname_len = calloc(metadata_nb, sizeof(int32_t));
  char **metadata_texts = calloc(metadata_nb, sizeof(char *));
  int32_t *metadata_len = calloc(metadata_nb, sizeof(int32_t));

  int i = 0;
  for(GList *iter = dt_metadata_get_list(); iter; iter = iter->next)
  {
    dt_metadata_t *metadata = (dt_metadata_t *)iter->data;

    if(metadata->internal)
      continue;

    GtkTextView *textview = _get_textview_by_key(metadata->key, self);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(textview);
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gchar *tagtext = gtk_text_buffer_get_text(buffer, &start, &end, TRUE);
    if(*tagtext != '\0')
    {
      const gchar *tagname = g_object_get_data(G_OBJECT(textview), "tagname");
      metadata_tagnames[i] = g_strdup(tagname);
      metadata_tagname_len[i] = strlen(metadata_tagnames[i]) + 1;
      metadata_texts[i] = tagtext;
      metadata_len[i] = strlen(metadata_texts[i]) + 1;
      *size = *size + metadata_tagname_len[i] + metadata_len[i];
      i++;
    }
    else
      g_free(tagtext);
  }
  dt_pthread_mutex_unlock(&darktable.metadata_threadsafe);

  char *params = malloc(*size);
  int pos = 0;

  for(int j = 0; j < i; j++)
  {
    memcpy(params + pos, metadata_tagnames[j], metadata_tagname_len[j]);
    pos += metadata_tagname_len[j];
    memcpy(params + pos, metadata_texts[j], metadata_len[j]);
    pos += metadata_len[j];
    g_free(metadata_texts[j]);
  }

  free(metadata_tagname_len);
  free(metadata_tagnames);
  free(metadata_texts);
  free(metadata_len);

  g_assert(pos == *size);

  return params;
}

// WARNING: also change src/libs/import.c when changing this!
int set_params(dt_lib_module_t *self,
               const void *params, int size)
{
  if(!params) return 1;
  dt_lib_metadata_t *d = self->data;

  dt_pthread_mutex_lock(&darktable.metadata_threadsafe);
  const int metadata_nb = g_list_length(dt_metadata_get_list());
  dt_pthread_mutex_unlock(&darktable.metadata_threadsafe);

  char **metadata_tagnames = calloc(metadata_nb, sizeof(char *));
  int32_t *metadata_tagname_len = calloc(metadata_nb, sizeof(int32_t));
  char **metadata_texts = calloc(metadata_nb, sizeof(char *));
  int32_t *metadata_len = calloc(metadata_nb, sizeof(int32_t));

  char *buf = (char *)params;
  size_t pos = 0;

  int i = 0;
  while(pos < size)
  {
    char *tagname = buf + pos;
    pos += strlen(tagname) + 1;
    char *text = buf + pos;
    pos += strlen(text) + 1;

    metadata_tagnames[i] = tagname;
    metadata_tagname_len[i] = strlen(tagname) + 1;
    metadata_texts[i] = text;
    metadata_len[i] = strlen(text) + 1;
    i++;
  }

  if(pos != size)
  {
    free(metadata_tagname_len);
    free(metadata_tagnames);
    free(metadata_texts);
    free(metadata_len);
    return 1;
  }

  GList *key_value = NULL;

  for(unsigned int j = 0; j < i; j++)
  {
    _append_kv(&key_value, metadata_tagnames[j], metadata_texts[j]);
  }

  free(metadata_tagname_len);
  free(metadata_tagnames);
  free(metadata_texts);
  free(metadata_len);

  GList *imgs = dt_act_on_get_images(FALSE, TRUE, FALSE);
  dt_metadata_set_list(imgs, key_value, TRUE);

  g_list_free(key_value);

  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE);
  dt_image_synch_xmps(imgs);
  g_list_free(imgs);
  // force the ui refresh to update the info from preset
  g_list_free(d->last_act_on);
  d->last_act_on = NULL;
  dt_lib_gui_queue_update(self);
  return 0;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
