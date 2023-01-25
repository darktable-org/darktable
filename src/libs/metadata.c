/*
    This file is part of darktable,
    Copyright (C) 2010-2022 darktable developers.

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
  char *edited[DT_METADATA_NUMBER];
  gboolean editing[DT_METADATA_NUMBER];
  char *name[DT_METADATA_NUMBER];
  GtkWidget *label[DT_METADATA_NUMBER];
  GtkWidget *apply_button;
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

void _textbuffer_changed(GtkTextBuffer *textbuffer, dt_lib_module_t *self);

static gboolean _is_leave_unchanged(GtkTextView *textview)
{
  return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(textview), "tv_multiple"));
}

static gchar *_get_buffer_text(GtkTextView *textview)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(textview);
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  return gtk_text_buffer_get_text(buffer, &start, &end, TRUE);
}

static void _text_set_all_selected(GtkTextView *textview, const gboolean selected)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(textview);
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  gtk_text_buffer_select_range(buffer, selected ? &start : &end, &end);
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

static void _set_text_buffer(GtkTextBuffer *buffer, const char *text)
{
  g_signal_handlers_block_matched(buffer, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, _textbuffer_changed, NULL);
  gtk_text_buffer_set_text(buffer, text, -1);
  g_signal_handlers_unblock_matched(buffer, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, _textbuffer_changed, NULL);
}

static int _textview_index(GtkTextView *textview)
{
  return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(textview), "tv_index"));
}

static void _set_textview_editing(const gboolean editing, const int i, dt_lib_metadata_t *d)
{
  if(!editing && d->editing[i] && d->last_act_on)
  {
    g_list_free(d->last_act_on);
    d->last_act_on = NULL;
  }
  d->editing[i] = editing;
  if(editing)
  {
    gchar *markup = g_strdup_printf("<u><i>%s</i></u>", d->name[i]);
    gtk_label_set_markup(GTK_LABEL(d->label[i]), markup);
    g_free(markup);
  }
  else if(d->label[i])
    gtk_label_set_text(GTK_LABEL(d->label[i]), d->name[i]);
}

static gboolean _is_textview_editing(const int i, dt_lib_metadata_t *d)
{
  return(d->editing[i]);
}

static void _save_edited_textview(const int i, dt_lib_metadata_t *d)
{
  if(d->editing[i])
  {
    if(d->edited[i])
      g_free(d->edited[i]);
    d->edited[i] = _get_buffer_text(GTK_TEXT_VIEW(d->textview[i]));
    _set_textview_editing(FALSE, i, d);
  }
}

static void _restore_edited_textview(const int i, dt_lib_metadata_t *d)
{
  if(d->edited[i])
  {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(d->textview[i]);
    _set_text_buffer(buffer, d->edited[i]);
    g_free(d->edited[i]);
    d->edited[i] = NULL;
    _set_textview_editing(TRUE, i, d);
  }
}

static void _reset_edited_state(const int i, dt_lib_metadata_t *d)
{
  _set_textview_editing(FALSE, i, d);
  if(d->edited[i])
  {
    g_free(d->edited[i]);
    d->edited[i] = NULL;
  }
}

static void _fill_text_view(const uint32_t i, const uint32_t count, dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;
  gboolean multi = FALSE;

  GtkTextBuffer *buffer = gtk_text_view_get_buffer(d->textview[i]);
  if(_is_textview_editing(i, d))
  {}
  else if(count == 0)  // no metadata value
  {
    _set_text_buffer(buffer, "");
  }
  else if(count == 1) // images with different metadata values
  {
    _set_text_buffer(buffer, _("<leave unchanged>"));
    multi = TRUE;
  }
  else // one or several images with the same metadata value
  {
    _set_text_buffer(buffer, (char *)d->metadata_list[i]->data);
  }
  g_object_set_data(G_OBJECT(d->textview[i]), "tv_multiple", GINT_TO_POINTER(multi));
  _text_set_italic(d->textview[i], multi);
}

static void _update(dt_lib_module_t *self)
{
  dt_lib_cancel_postponed_update(self);
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;

  GList *imgs = dt_act_on_get_images(FALSE, FALSE, FALSE);

  // first we want to make sure the list of images to act on has changed
  // this is not the case if mouse hover change but still stay in selection for ex.
  if(!imgs && !d->last_act_on) return;
  if(imgs && d->last_act_on && g_list_length(imgs) == g_list_length(d->last_act_on))
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
  g_list_free(d->last_act_on);
  d->last_act_on = imgs;

  GList *metadata[DT_METADATA_NUMBER];
  uint32_t metadata_count[DT_METADATA_NUMBER];

  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    metadata[i] = NULL;
    metadata_count[i] = 0;
  }

  // using dt_metadata_get() is not possible here. we want to do all this in a single pass, everything else
  // takes ages.
  gchar *images = dt_act_on_get_query(FALSE);
  const uint32_t imgs_count = g_list_length((GList *)imgs);

  if(images)
  {
    sqlite3_stmt *stmt;
    // clang-format off
    gchar *query = g_strdup_printf(
                            "SELECT key, value, COUNT(id) AS ct FROM main.meta_data"
                            " WHERE id IN (%s)"
                            " GROUP BY key, value ORDER BY value",
                            images);
    // clang-format on
    g_free(images);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);

    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      if(sqlite3_column_bytes(stmt, 1))
      {
        const uint32_t key = (uint32_t)sqlite3_column_int(stmt, 0);
        if(key >= DT_METADATA_NUMBER)
          continue;
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
    if(dt_metadata_get_type(keyid) == DT_METADATA_TYPE_INTERNAL)
      continue;
    g_list_free_full(d->metadata_list[i], g_free);
    d->metadata_list[i] = metadata[keyid];
    _fill_text_view(i, metadata_count[keyid], self);
  }

  gtk_widget_set_sensitive(GTK_WIDGET(d->apply_button), imgs_count > 0);
}

static void _image_selection_changed_callback(gpointer instance, dt_lib_module_t *self)
{
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
    _reset_edited_state(i, (dt_lib_metadata_t *)self->data);
  _update(self);
}

static void _collection_updated_callback(gpointer instance, dt_collection_change_t query_change,
                                         dt_collection_properties_t changed_property, gpointer imgs, int next,
                                         dt_lib_module_t *self)
{
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
    _reset_edited_state(i, (dt_lib_metadata_t *)self->data);
  _update(self);
}

static void _append_kv(GList **l, const gchar *key, const gchar *value)
{
  *l = g_list_append(*l, (gchar *)key);
  *l = g_list_append(*l, (gchar *)value);
}

static void _metadata_set_list(const int i, GList **key_value, dt_lib_metadata_t *d)
{
  const uint32_t keyid = dt_metadata_get_keyid_by_display_order(i);
  if(dt_metadata_get_type(i) == DT_METADATA_TYPE_INTERNAL)
    return;
  gchar *metadata = _get_buffer_text(GTK_TEXT_VIEW(d->textview[i]));
  if(metadata && !_is_leave_unchanged(GTK_TEXT_VIEW(d->textview[i])))
    _append_kv(key_value, dt_metadata_get_key(keyid), metadata);
}

static void _write_metadata(GtkTextView *textview, dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;

  GList *key_value = NULL;
  if(textview)
  {
    const int i = _textview_index(textview);
    _metadata_set_list(i, &key_value, d);
    _reset_edited_state(i, d);
  }
  else
  {
    for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
    {
      _metadata_set_list(i, &key_value, d);
      _reset_edited_state(i, d);
    }
  }

  GList *imgs = dt_act_on_get_images(FALSE, TRUE, FALSE);
  dt_metadata_set_list(imgs, key_value, TRUE);

  for(GList *l = key_value; l; l = l->next)
  {
    l = l->next;
    g_free(l->data);  // metadata value
  }
  g_list_free(key_value);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_METADATA_CHANGED, DT_METADATA_SIGNAL_NEW_VALUE);

  dt_image_synch_xmps(imgs);
  g_list_free(imgs);
  _update(self);
}

static void _apply_button_clicked(GtkButton *button, dt_lib_module_t *self)
{
  _write_metadata(NULL, self);
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
        _set_textview_editing(TRUE, _textview_index(GTK_TEXT_VIEW(textview)), d);
        break;
      default:
        break;
    }
  }
  else
  {
    switch(event->keyval)
    {
      case GDK_KEY_Return:
      case GDK_KEY_KP_Enter:
        _write_metadata(GTK_TEXT_VIEW(textview), self);
        _text_set_all_selected(GTK_TEXT_VIEW(textview), FALSE);
        return TRUE;
        break;
      case GDK_KEY_Tab:
      case GDK_KEY_KP_Tab:
      case GDK_KEY_ISO_Left_Tab:
        _write_metadata(GTK_TEXT_VIEW(textview), self);
        break;
      case GDK_KEY_Escape:
      {
        if(dt_modifier_is(event->state, 0))
        {
          for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
            _reset_edited_state(i, d);
          _update(self);
          gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
          return TRUE;
        }
        break;
      }
      default:
        break;
    }
  }

  return gtk_text_view_im_context_filter_keypress(GTK_TEXT_VIEW(textview), event);
}

void _textbuffer_changed(GtkTextBuffer *textbuffer, dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;
  GtkTextView *textview = GINT_TO_POINTER(g_object_get_data(G_OBJECT(textbuffer), "buffer_tv"));
  _set_textview_editing(TRUE, _textview_index(textview), d);
  g_object_set_data(G_OBJECT(textview), "tv_multiple", GINT_TO_POINTER(FALSE));
}

gboolean _textview_focus(GtkWidget *widget, GtkDirectionType d, gpointer user_data)
{
  GtkWidget *target = g_object_get_data(G_OBJECT(widget), d == GTK_DIR_TAB_FORWARD ? "meta_next" : "meta_prev");
  gtk_widget_grab_focus(target);
  return TRUE;
}

static gboolean _got_focus(GtkWidget *textview, dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;
  if(!_is_textview_editing(_textview_index(GTK_TEXT_VIEW(textview)), d))
  {
    if(_is_leave_unchanged(GTK_TEXT_VIEW(textview)))
    {
      GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textview));
      _set_text_buffer(buffer, "");
      _text_set_italic(GTK_TEXT_VIEW(textview), FALSE);
    }
    _text_set_all_selected(GTK_TEXT_VIEW(textview), TRUE);
  }
  return TRUE;
}

static gboolean _lost_focus(GtkWidget *textview, GdkEventFocus *event, dt_lib_module_t *self)
{
  if(_is_leave_unchanged(GTK_TEXT_VIEW(textview)))
  {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textview));
    _set_text_buffer(buffer, _("<leave unchanged>"));
    _text_set_italic(GTK_TEXT_VIEW(textview), TRUE);
  }
  return FALSE;
}

int position(const dt_lib_module_t *self)
{
  return 510;
}

static void _update_layout(dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;

  GtkWidget *first = NULL, *previous = NULL;
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    if(dt_metadata_get_type_by_display_order(i) == DT_METADATA_TYPE_INTERNAL)
      continue;
    const gchar *name = dt_metadata_get_name_by_display_order(i);
    const int type = dt_metadata_get_type_by_display_order(i);
    gchar *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);
    const gboolean hidden = type == DT_METADATA_TYPE_INTERNAL ||
                            dt_conf_get_int(setting) & DT_METADATA_FLAG_HIDDEN;
    g_free(setting);

    GtkWidget *label = gtk_grid_get_child_at(GTK_GRID(self->widget), 0, i);
    gtk_widget_set_visible(label, !hidden);
    GtkWidget *current = GTK_WIDGET(d->textview[i]);
    gtk_widget_set_visible(gtk_widget_get_parent(current), !hidden);

    if(!hidden)
    {
      if(!first) first = previous = current;

      g_object_set_data(G_OBJECT(previous), "meta_next", current);
      g_object_set_data(G_OBJECT(current), "meta_prev", previous);

      g_object_set_data(G_OBJECT(current), "meta_next", first);
      g_object_set_data(G_OBJECT(first), "meta_prev", current);

      previous = current;
    }
  }
}

void gui_reset(dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    const gchar *name = dt_metadata_get_name_by_display_order(i);
    gchar *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);
    const gboolean hidden = dt_conf_get_int(setting) & DT_METADATA_FLAG_HIDDEN;
    g_free(setting);
    const int type = dt_metadata_get_type_by_display_order(i);
    // we don't want to lose hidden information
    if(!hidden && type != DT_METADATA_TYPE_INTERNAL)
    {
      GtkTextBuffer *buffer = gtk_text_view_get_buffer(d->textview[i]);
      _set_text_buffer(buffer, "");
      _text_set_italic(d->textview[i], FALSE);
    }
  }
  _write_metadata(NULL, self);
}

static void _mouse_over_image_callback(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;
  // if editing don't lose the current entry
  const int32_t img = dt_control_get_mouse_over_id();

  if(img == -1)
  { // exits thumbnails zone
    for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
      _restore_edited_textview(i, d);
  }
  else
    for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
      _save_edited_textview(i, d);

  dt_lib_queue_postponed_update(self, _update);
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
                                       GTK_DIALOG_DESTROY_WITH_PARENT, _("default"), GTK_RESPONSE_YES,
                                       _("cancel"), GTK_RESPONSE_NONE, _("save"), GTK_RESPONSE_ACCEPT, NULL);
  g_signal_connect(dialog, "key-press-event", G_CALLBACK(dt_handle_dialog_enter), NULL);
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
  while(res == GTK_RESPONSE_YES)
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

  if(res == GTK_RESPONSE_ACCEPT)
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
      if(i < DT_METADATA_NUMBER && dt_metadata_get_type(i) != DT_METADATA_TYPE_INTERNAL)
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

void _menu_line_activated(GtkMenuItem *menuitem, GtkTextView *textview)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(textview);
  gtk_text_buffer_set_text(buffer, gtk_label_get_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(menuitem)))), -1);
}

static void _populate_popup_multi(GtkTextView *textview, GtkWidget *popup, dt_lib_module_t *self)
{
  const dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;

  // get grid line number
  const int i = _textview_index(textview);

  if(!d->metadata_list[i] || !_is_leave_unchanged(GTK_TEXT_VIEW(textview))) return;

  gtk_menu_shell_append(GTK_MENU_SHELL(popup),gtk_separator_menu_item_new());

  for(GList *item = d->metadata_list[i]; item; item = g_list_next(item))
  {
    GtkWidget *new_line = gtk_menu_item_new_with_label(item->data);
    g_signal_connect(G_OBJECT(new_line), "activate", G_CALLBACK(_menu_line_activated), textview);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup), new_line);
  }
  gtk_widget_show_all(popup);
}

static gboolean _metadata_reset(GtkWidget *label, GdkEventButton *event, GtkWidget *widget)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(widget));
    _set_text_buffer(buffer, "");

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

  GtkGrid *grid = GTK_GRID(gtk_grid_new());
  self->widget = GTK_WIDGET(grid);
  gtk_grid_set_row_spacing(grid, DT_PIXEL_APPLY_DPI(5));
  gtk_grid_set_column_spacing(grid, DT_PIXEL_APPLY_DPI(10));

  for(int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    if(dt_metadata_get_type_by_display_order(i) == DT_METADATA_TYPE_INTERNAL)
      continue;
    d->name[i] = _(dt_metadata_get_name_by_display_order(i));
    d->label[i] = dt_ui_label_new(d->name[i]);
    GtkWidget *labelev = gtk_event_box_new();
    gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(labelev), d->label[i]);
    gtk_grid_attach(grid, labelev, 0, i, 1, 1);

    GtkWidget *textview = gtk_text_view_new();
    gtk_widget_set_tooltip_text(textview,
              _("metadata text"
              "\nctrl+enter inserts a new line (caution, may not be compatible with standard metadata)"
              "\nif <leave unchanged> selected images have different metadata"
              "\nin that case, right-click gives the possibility to choose one of them"
              "\nescape to exit the popup window"));
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textview));
    g_object_set_data(G_OBJECT(buffer), "buffer_tv", GINT_TO_POINTER(textview));
    g_object_set_data(G_OBJECT(textview), "tv_index", GINT_TO_POINTER(i));
    g_object_set_data(G_OBJECT(textview), "tv_multiple", GINT_TO_POINTER(FALSE));
    gtk_text_buffer_create_tag(gtk_text_view_get_buffer(GTK_TEXT_VIEW(textview)),
                                "italic", "style", PANGO_STYLE_ITALIC, NULL);

    const char *name = (char *)dt_metadata_get_name_by_display_order(i);
    d->setting_name[i] = g_strdup_printf("plugins/lighttable/metadata/%s_text_height", name);

    GtkWidget *swindow = dt_ui_resize_wrap(GTK_WIDGET(textview), 100, d->setting_name[i]);

    gtk_grid_attach(grid, swindow, 1, i, 1, 1);
    gtk_widget_set_hexpand(swindow, TRUE);
    d->swindow[i] = swindow;

    //workaround for a Gtk issue where the textview does not wrap correctly
    //while resizing the panel or typing into the widget
    //reported upstream to https://gitlab.gnome.org/GNOME/gtk/-/issues/4042
    //see also discussions on https://github.com/darktable-org/darktable/pull/10584
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(gtk_bin_get_child(GTK_BIN(swindow))),
                                   GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);

    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(textview), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(textview), FALSE);
    gtk_widget_add_events(textview, GDK_FOCUS_CHANGE_MASK);
    g_signal_connect(textview, "key-press-event", G_CALLBACK(_key_pressed), self);
    g_signal_connect(textview, "focus", G_CALLBACK(_textview_focus), self);
    g_signal_connect(textview, "populate-popup", G_CALLBACK(_populate_popup_multi), self);
    g_signal_connect(textview, "grab-focus", G_CALLBACK(_got_focus), self);
    g_signal_connect(textview, "focus-out-event", G_CALLBACK(_lost_focus), self);
    g_signal_connect(labelev, "button-press-event", G_CALLBACK(_metadata_reset), textview);
    g_signal_connect(buffer, "changed", G_CALLBACK(_textbuffer_changed), self);
    d->textview[i] = GTK_TEXT_VIEW(textview);
    gtk_widget_set_hexpand(textview, TRUE);
    gtk_widget_set_vexpand(textview, TRUE);
  }

  // apply button
  d->apply_button = dt_action_button_new(self, N_("apply"), _apply_button_clicked, self,
                                         _("write metadata for selected images"), 0, 0);

  gtk_grid_attach(GTK_GRID(self->widget), GTK_WIDGET(d->apply_button), 0, DT_METADATA_NUMBER, 2, 1);

  /* lets signup for mouse over image change signals */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(_mouse_over_image_callback), self);

  // and 2 other interesting signals:
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_SELECTION_CHANGED,
                            G_CALLBACK(_image_selection_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_collection_updated_callback), self);

  gtk_widget_show_all(self->widget);
  gtk_widget_set_no_show_all(self->widget, TRUE);

  _update(self);
  _update_layout(self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_cancel_postponed_update(self);
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_mouse_over_image_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_image_selection_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_collection_updated_callback), self);

  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    if(dt_metadata_get_type_by_display_order(i) == DT_METADATA_TYPE_INTERNAL)
      continue;
    g_signal_handlers_block_by_func(d->textview[i], _lost_focus, self);
    g_free(d->setting_name[i]);
    _reset_edited_state(i, d);
  }
  free(self->data);
  self->data = NULL;
}

static void add_rights_preset(dt_lib_module_t *self, char *name, char *string)
{
  // to be adjusted the nb of metadata items changes
  const unsigned int metadata_nb = dt_metadata_get_nb_user_metadata();
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
    const size_t new_params_size = old_params_size + 1;
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
    const size_t new_params_size = old_params_size + 1;
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
    if(dt_metadata_get_type_by_display_order(i) == DT_METADATA_TYPE_INTERNAL)
      continue;
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
    if(dt_metadata_get_type_by_display_order(i) == DT_METADATA_TYPE_INTERNAL)
      continue;
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
    if(dt_metadata_get_type_by_display_order(i) == DT_METADATA_TYPE_INTERNAL)
      continue;
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
    if(dt_metadata_get_type_by_display_order(i) == DT_METADATA_TYPE_INTERNAL)
      continue;
    if(metadata[i][0] != '\0') _append_kv(&key_value, dt_metadata_get_key(i), metadata[i]);
  }

  GList *imgs = dt_act_on_get_images(FALSE, TRUE, FALSE);
  dt_metadata_set_list(imgs, key_value, TRUE);

  g_list_free(key_value);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE);
  dt_image_synch_xmps(imgs);
  g_list_free(imgs);
  // force the ui refresh to update the info from preset
  g_list_free(d->last_act_on);
  d->last_act_on = NULL;
  _update(self);
  return 0;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
