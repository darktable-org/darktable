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

#include "common/debug.h"
#include "common/metadata.h"
#include "control/conf.h"
#include "control/control.h"
#include "gui/import_metadata.h"
#include "gui/preferences.h"
#include "gui/gtk.h"

/*
    Metadata are displayed in a grid which follows this layout:
    Lines
    First line (DT_META_META_HEADER): Titles + metadata presets combobox
    Next lines (DT_META_META_VALUE): Metadata - visibility depending on metadata preferences
    Before last line (DT_META_TAGS_HEADER): tags presets combobox
    Last line (DT_META_TAGS_VALUE): tags
    Columns
    First column: metadata names
    Second column: value entry
    Last column: xmp flag - visibility depending on write xmp preferences
*/

typedef enum dt_import_grid_t
{
  DT_META_META_HEADER = 0,
  DT_META_META_VALUE,
  DT_META_TAGS_HEADER,
  DT_META_TAGS_VALUE
} dt_import_grid_t;


static void _import_metadata_presets_update(dt_import_metadata_t *metadata);
static void _fill_metadata_grid(dt_import_metadata_t *metadata);

static void _metadata_save(GtkWidget *widget,
                           dt_import_metadata_t *metadata)
{
  const char *name = dt_metadata_get_tag_subkey((char *)g_object_get_data(G_OBJECT(widget), "tagname"));
  gchar *setting = g_strdup_printf("ui_last/import_last_%s", name);
  dt_conf_set_string(setting, gtk_entry_get_text(GTK_ENTRY(widget)));
  g_free(setting);
}

static void _import_metadata_changed(GtkWidget *widget,
                                     dt_import_metadata_t *metadata)
{
  _metadata_save(widget, metadata);
  GtkWidget *w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), 1, DT_META_META_HEADER);
  gtk_combo_box_set_active(GTK_COMBO_BOX(w), -1);
}

static gboolean _import_metadata_reset(GtkWidget *label,
                                       GdkEventButton *event,
                                       GtkWidget *widget)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    gtk_entry_set_text(GTK_ENTRY(widget), "");
  }
  return FALSE;
}

static void _metadata_reset_all(dt_import_metadata_t *metadata,
                                const gboolean hard)
{
  for(unsigned int i = DT_META_META_VALUE; i < metadata->num_grid_rows + DT_META_TAGS_VALUE; i++)
  {
    GtkWidget *w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), 1, i);
    if(GTK_IS_ENTRY(w))
    {
      const gboolean visible = gtk_widget_get_visible(w);
      if(hard || visible)
        gtk_entry_set_text(GTK_ENTRY(w), "");
    }
  }
  if(hard)
  {
    // import module reset
    for(unsigned int i = DT_META_META_VALUE; i < metadata->num_grid_rows + DT_META_TAGS_VALUE; i++)
    {
      GtkWidget *w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), 2, i);
      if(GTK_IS_TOGGLE_BUTTON(w))
      {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), TRUE);
      }
    }
  }
}

static gboolean _import_metadata_reset_all(GtkWidget *label,
                                           GdkEventButton *event,
                                           dt_import_metadata_t *metadata)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    _metadata_reset_all(metadata, FALSE);
  }
  return FALSE;
}

static void _import_metadata_toggled(GtkWidget *widget,
                                     dt_import_metadata_t *metadata)
{
  const char *name = gtk_widget_get_name(widget);
  if(g_strcmp0(name, "tags"))
  {
    const char *tagname = g_object_get_data(G_OBJECT(widget), "tagname");
    const char *metadata_name = dt_metadata_get_tag_subkey(tagname);
    char *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", metadata_name);
    const gboolean imported = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
    const uint32_t flag = dt_conf_get_int(setting);
    dt_conf_set_int(setting, imported ? flag | DT_METADATA_FLAG_IMPORTED : flag & ~DT_METADATA_FLAG_IMPORTED);
    g_free(setting);
  }
  else
  {
    const gboolean imported = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
    dt_conf_set_bool("ui_last/import_last_tags_imported", imported);
  }
}

static void _import_tags_changed(GtkWidget *widget,
                                 dt_import_metadata_t *metadata)
{
  GtkWidget *w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), 1, metadata->num_grid_rows + DT_META_META_VALUE);
  gtk_combo_box_set_active(GTK_COMBO_BOX(w), -1);
  w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), 1, metadata->num_grid_rows + DT_META_TAGS_HEADER);
  dt_conf_set_string("ui_last/import_last_tags", gtk_entry_get_text(GTK_ENTRY(w)));
}

static void _update_layout(dt_import_metadata_t *metadata)
{
  const gboolean write_xmp = (dt_image_get_xmp_mode() != DT_WRITE_XMP_NEVER);
  GtkWidget *w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), 2, DT_META_META_HEADER);
  gtk_widget_set_visible(w, !write_xmp);

  unsigned int i = 0;
  dt_pthread_mutex_lock(&darktable.metadata_threadsafe);
  for(GList *iter = dt_metadata_get_list(); iter; iter = iter->next)
  {
    dt_metadata_t *md = (dt_metadata_t *)iter->data;
    const gboolean visible = !md->internal && md->visible;

    // update the label
    w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), 0, i + DT_META_META_VALUE);
    GtkWidget *lbl = g_object_get_data(G_OBJECT(w), "label");
    if(lbl)
      gtk_label_set_text(GTK_LABEL(lbl), md->name);

    for(int j = 0; j < 3; j++)
    {
      w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), j, i + DT_META_META_VALUE);
      gtk_widget_set_visible(w, j < 2 ? visible : visible & !write_xmp);
    }

    i++;
  }
  dt_pthread_mutex_unlock(&darktable.metadata_threadsafe);

  w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), 2, metadata->num_grid_rows + DT_META_TAGS_HEADER);
  gtk_widget_set_visible(w, !write_xmp);
}

static void _apply_metadata_toggled(GtkWidget *widget,
                                    dt_import_metadata_t *metadata)
{
  // activate widgets as needed
  const gboolean default_metadata = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));

  for(int i = DT_META_META_HEADER; i < metadata->num_grid_rows + DT_META_TAGS_VALUE; i++)
  {
    for(int j = 0; j < 2 ; j++)
    {
      GtkWidget *w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), j, i);
      if(GTK_IS_WIDGET(w))
        gtk_widget_set_sensitive(w, default_metadata);
    }
  }
}

static void _metadata_prefs_changed(gpointer instance,
                                    dt_import_metadata_t *metadata)
{
  _update_layout(metadata);
}

static void _metadata_list_changed(gpointer instance,
                                   const int type,
                                   dt_import_metadata_t *metadata)
{
  if(type == DT_METADATA_SIGNAL_PREF_CHANGED)
  {
    _fill_metadata_grid(metadata);
    _update_layout(metadata);
  }
}

static void _fill_textview(gpointer key,
                           gpointer value,
                           gpointer user_data)
{
  dt_import_metadata_t *metadata = (dt_import_metadata_t *)user_data;

  for(unsigned int i = 0; i < metadata->num_grid_rows; i++)
  {
    GtkWidget *w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), 1, i + DT_META_META_VALUE);
    const char *tagname = (char *)g_object_get_data(G_OBJECT(w), "tagname");
    if(!g_strcmp0(tagname, (char *)key))
    {
      g_signal_handlers_block_by_func(w, _import_metadata_changed, metadata);
      gtk_entry_set_text(GTK_ENTRY(w), value);
      g_signal_handlers_unblock_by_func(w, _import_metadata_changed, metadata);
      _metadata_save(w, metadata);
      break;
    }
  }
}

static void _import_metadata_presets_changed(GtkWidget *widget,
                                             dt_import_metadata_t *metadata)
{
  GtkTreeIter iter;

  if(gtk_combo_box_get_active_iter(GTK_COMBO_BOX(widget), &iter) == TRUE)
  {
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(widget));
    GHashTable *metadata_kv = NULL;
    gtk_tree_model_get(model, &iter, 1, &metadata_kv, -1);
    g_hash_table_foreach(metadata_kv, _fill_textview, metadata);
  }
}

static void _import_metadata_presets_update(dt_import_metadata_t *metadata)
{
  gtk_list_store_clear(metadata->m_model);
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT name, op_params FROM data.presets "
                              "WHERE operation = 'metadata' "
                              "ORDER BY writeprotect DESC, LOWER(name)",
                              -1, &stmt, NULL);
  // clang-format on
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    void *op_params = (void *)sqlite3_column_blob(stmt, 1);
    int32_t op_params_size = sqlite3_column_bytes(stmt, 1);

    // key/value list of metadata
    GHashTable *metadata_kv = g_hash_table_new(NULL, NULL);

    char *buf = (char *)op_params;
    uint32_t pos = 0;

    while(pos < op_params_size)
    {
      const char *tagname = g_strdup(buf + pos);
      pos += strlen(tagname) + 1;
      const char *value = g_strdup(buf + pos);
      pos += strlen(value) + 1;
      g_hash_table_insert(metadata_kv, (gpointer)tagname, (gpointer)value);
    }

    if(op_params_size == pos)
    {
      gtk_list_store_insert_with_values(metadata->m_model, NULL, -1,
                         0, (char *)sqlite3_column_text(stmt, 0),
                         1, metadata_kv,
                         -1);
    }
  }
  sqlite3_finalize(stmt);
}

static void _import_tags_presets_changed(GtkWidget *widget,
                                         dt_import_metadata_t *metadata)
{
  GtkTreeIter iter;

  if(gtk_combo_box_get_active_iter(GTK_COMBO_BOX(widget), &iter) == TRUE)
  {
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(widget));
    char *tags;
    gtk_tree_model_get(model, &iter, 1, &tags, -1);
    GtkWidget *w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), 1, metadata->num_grid_rows + DT_META_TAGS_HEADER);
    g_signal_handlers_block_by_func(w, _import_tags_changed, metadata);
    gtk_entry_set_text(GTK_ENTRY(w), tags);
    g_signal_handlers_unblock_by_func(w, _import_tags_changed, metadata);
    dt_conf_set_string("ui_last/import_last_tags", tags);
    g_free(tags);
  }
}

static void _import_tags_presets_update(dt_import_metadata_t *metadata)
{
  gtk_list_store_clear(metadata->t_model);
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT name, op_params FROM data.presets "
                              "WHERE operation = 'tagging' "
                              "ORDER BY writeprotect DESC, LOWER(name)",
                              -1, &stmt, NULL);
  // clang-format on
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    void *op_params = (void *)sqlite3_column_blob(stmt, 1);
    int32_t op_params_size = sqlite3_column_bytes(stmt, 1);
    if(op_params_size)
    {
      char *tags = NULL;
      gchar **tokens = g_strsplit(op_params, ",", 0);
      if(tokens)
      {
        gchar **entry = tokens;
        while(*entry)
        {
          const guint tagid = strtoul(*entry, NULL, 0);
          char *tag = dt_tag_get_name(tagid);
          dt_util_str_cat(&tags, "%s,", tag);
          g_free(tag);
          entry++;
        }
        if(tags)
          tags[strlen(tags) - 1] = '\0';
        g_strfreev(tokens);
        gtk_list_store_insert_with_values(metadata->t_model, NULL, -1,
                           0, (char *)sqlite3_column_text(stmt, 0),
                           1, tags, -1);
        g_free(tags);
      }
    }
  }
  sqlite3_finalize(stmt);
}

static void _metadata_presets_changed(gpointer instance,
                                      gpointer module,
                                      dt_import_metadata_t *metadata)
{
  if(!g_strcmp0(module, "metadata"))
    _import_metadata_presets_update(metadata);
  else if(!g_strcmp0(module, "tagging"))
    _import_tags_presets_update(metadata);
}

static GtkWidget *_set_up_label(GtkWidget *label,
                                const int align,
                                const int line,
                                dt_import_metadata_t *metadata)
{
  gtk_widget_set_visible(label, TRUE);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_halign(label, align);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0);
  GtkWidget *labelev = gtk_event_box_new();
  gtk_widget_set_visible(labelev, TRUE);
  gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
  gtk_container_add(GTK_CONTAINER(labelev), label);
  gtk_grid_attach(GTK_GRID(metadata->grid), labelev, 0, line, 1, 1);
  return labelev;
}

static GtkWidget *_set_up_combobox(GtkListStore *model,
                                   const int line,
                                   dt_import_metadata_t *metadata)
{
  GtkWidget *presets = gtk_combo_box_new_with_model(GTK_TREE_MODEL(model));
  gtk_widget_set_visible(presets, TRUE);
  gtk_widget_set_hexpand(presets, TRUE);
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(presets), renderer, TRUE);
  gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(presets), renderer, "text", 0, NULL);
  g_object_set(G_OBJECT(renderer), "ellipsize", PANGO_ELLIPSIZE_END, (gchar *)0);
  gtk_grid_attach(GTK_GRID(metadata->grid), presets, 1, line, 1, 1);
  g_object_unref(model);
  return presets;
}

static void _set_up_entry(GtkWidget *entry,
                          const char *str,
                          const char *name,
                          const int line,
                          dt_import_metadata_t *metadata)
{
  gtk_widget_set_name(entry, name);
  gtk_entry_set_text(GTK_ENTRY(entry), str);
  gtk_widget_set_halign(entry, GTK_ALIGN_FILL);
  gtk_entry_set_width_chars(GTK_ENTRY(entry), 5);
  gtk_widget_set_hexpand(entry, TRUE);
  gtk_grid_attach(GTK_GRID(metadata->grid), entry, 1, line, 1, 1);
}

static void _set_up_toggle_button(GtkWidget *button,
                                  const gboolean state,
                                  const char *name,
                                  const int line,
                                  dt_import_metadata_t *metadata)
{
  gtk_widget_set_name(button, name);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), state);
  gtk_grid_attach(GTK_GRID(metadata->grid), button, 2, line, 1, 1);
  gtk_widget_set_halign(button, GTK_ALIGN_CENTER);
}

static void _fill_metadata_grid(dt_import_metadata_t *metadata)
{
  for(uint32_t i = DT_META_META_VALUE + metadata->num_grid_rows - 1; i >= DT_META_META_VALUE; i--)
    gtk_grid_remove_row(GTK_GRID(metadata->grid), i);

  uint32_t i = 0;
  dt_pthread_mutex_lock(&darktable.metadata_threadsafe);
  for(GList *iter = dt_metadata_get_list(); iter; iter = iter->next)
  {
    dt_metadata_t *md = (dt_metadata_t *)iter->data;

    gtk_grid_insert_row(GTK_GRID(metadata->grid), i + DT_META_META_VALUE);

    const gchar *metadata_name = dt_metadata_get_tag_subkey(md->tagname);
    gchar *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", metadata_name);
    const uint32_t flag = dt_conf_get_int(setting);
    g_free(setting);

    GtkWidget *metadata_label = gtk_label_new(_(md->name));
    GtkWidget *labelev = _set_up_label(metadata_label, GTK_ALIGN_START, i + DT_META_META_VALUE, metadata);
    g_object_set_data(G_OBJECT(labelev), "label", metadata_label);
    g_object_set_data(G_OBJECT(labelev), "key", GINT_TO_POINTER(md->key));

    GtkWidget *metadata_entry = gtk_entry_new();
    g_object_set_data(G_OBJECT(metadata_entry), "tagname", md->tagname);
    setting = g_strdup_printf("ui_last/import_last_%s", metadata_name);
    const char *str = dt_conf_get_string_const(setting);
    _set_up_entry(metadata_entry, str, metadata_name, i + DT_META_META_VALUE, metadata);
    g_free(setting);
    g_signal_connect(GTK_ENTRY(metadata_entry), "changed",
                     G_CALLBACK(_import_metadata_changed), metadata);
    g_signal_connect(GTK_EVENT_BOX(labelev), "button-press-event",
                     G_CALLBACK(_import_metadata_reset), metadata_entry);

    GtkWidget *metadata_imported = gtk_check_button_new();
    g_object_set_data(G_OBJECT(metadata_imported), "tagname", md->tagname);
    _set_up_toggle_button(metadata_imported, flag & DT_METADATA_FLAG_IMPORTED,
                          metadata_name, i + DT_META_META_VALUE, metadata);
    g_signal_connect(GTK_TOGGLE_BUTTON(metadata_imported), "toggled",
                     G_CALLBACK(_import_metadata_toggled), metadata);
    i++;
  }
  dt_pthread_mutex_unlock(&darktable.metadata_threadsafe);
  metadata->num_grid_rows = i;
}

void dt_import_metadata_init(dt_import_metadata_t *metadata)
{
  // default metadata
  GtkWidget *grid = gtk_grid_new();
  metadata->grid = grid;
  gtk_box_pack_start(GTK_BOX(metadata->box), grid, FALSE, FALSE, 0);
  gtk_grid_set_column_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(5));
  gtk_widget_show_all(grid);
  gtk_widget_set_no_show_all(grid, TRUE);

  metadata->m_model = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_POINTER);
  _import_metadata_presets_update(metadata);
  metadata->t_model = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
  _import_tags_presets_update(metadata);

  // grid headers
  GtkWidget *label = gtk_label_new(_("metadata presets"));
  gtk_widget_set_name(label, "import-presets");
  GtkWidget *labelev =_set_up_label(label, GTK_ALIGN_START, DT_META_META_HEADER, metadata);
  gtk_widget_set_tooltip_text(GTK_WIDGET(label), _("metadata to be applied per default"
                                                   "\ndouble-click on a label to clear the corresponding entry"
                                                   "\ndouble-click on 'preset' to clear all entries"));
  g_signal_connect(GTK_EVENT_BOX(labelev), "button-press-event",
                   G_CALLBACK(_import_metadata_reset_all), metadata);


  GtkWidget *presets = _set_up_combobox(metadata->m_model, DT_META_META_HEADER, metadata);
  g_signal_connect(presets, "changed", G_CALLBACK(_import_metadata_presets_changed), metadata);

  label = gtk_label_new(_("from XMP"));
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_tooltip_text(GTK_WIDGET(label),
                              _("selected metadata are imported from image and override the default value."
                                "\nthis drives also the \'look for updated XMP files\' and \'load sidecar file\' actions."
                                "\nCAUTION: not selected metadata are cleaned up when XMP file is updated."
                              ));
  gtk_grid_attach(GTK_GRID(grid), label, 2, DT_META_META_HEADER, 1, 1);

  // grid content
  // metadata
  _fill_metadata_grid(metadata);

  // tags
  label = gtk_label_new(_("tag presets"));
  gtk_widget_set_name(label, "import-presets");
  labelev =_set_up_label(label, GTK_ALIGN_START, metadata->num_grid_rows + DT_META_META_VALUE, metadata);

  presets = _set_up_combobox(metadata->t_model, metadata->num_grid_rows + DT_META_META_VALUE, metadata);
  g_signal_connect(presets, "changed", G_CALLBACK(_import_tags_presets_changed), metadata);

  label = gtk_label_new(_("tags"));
  labelev = _set_up_label(label, GTK_ALIGN_START, metadata->num_grid_rows + DT_META_TAGS_HEADER, metadata);

  GtkWidget *entry = gtk_entry_new();
  gtk_widget_set_visible(entry, TRUE);
  const char *str = dt_conf_get_string_const("ui_last/import_last_tags");
  _set_up_entry(entry, str, "tags", metadata->num_grid_rows + DT_META_TAGS_HEADER, metadata);
  gtk_widget_set_tooltip_text(entry, _("comma separated list of tags"));
  g_signal_connect(GTK_ENTRY(entry), "changed",
                   G_CALLBACK(_import_tags_changed), metadata);
  g_signal_connect(GTK_EVENT_BOX(labelev), "button-press-event",
                   G_CALLBACK(_import_metadata_reset), entry);

  GtkWidget *tags_imported = gtk_check_button_new();
  _set_up_toggle_button(tags_imported, dt_conf_get_bool("ui_last/import_last_tags_imported"),
                        "tags", metadata->num_grid_rows + DT_META_TAGS_HEADER, metadata);
  g_signal_connect(GTK_TOGGLE_BUTTON(tags_imported), "toggled",
                   G_CALLBACK(_import_metadata_toggled), metadata);

  // overall
  g_signal_connect(metadata->apply_metadata, "toggled",
                   G_CALLBACK(_apply_metadata_toggled), metadata);
  // needed since the apply_metadata starts being turned off,
  // and setting it to off doesn't emit the 'toggled' signal ...
  _apply_metadata_toggled(metadata->apply_metadata, metadata);

  // connect changed signal
  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_PREFERENCES_CHANGE, _metadata_prefs_changed, metadata);
  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_METADATA_CHANGED, _metadata_list_changed, metadata);
  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_PRESETS_CHANGED, _metadata_presets_changed, metadata);
  _update_layout(metadata);
}

void dt_import_metadata_cleanup(dt_import_metadata_t *metadata)
{
  DT_CONTROL_SIGNAL_DISCONNECT_ALL(metadata, "metadata");
}

void dt_import_metadata_update(dt_import_metadata_t *metadata)
{
  unsigned int i = 0;
  dt_pthread_mutex_lock(&darktable.metadata_threadsafe);
  for(GList *iter = dt_metadata_get_list(); iter; iter = iter->next)
  {
    dt_metadata_t *md = iter->data;

    GtkWidget *w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), 1, i + DT_META_META_VALUE);
    const gchar *metadata_name = dt_metadata_get_tag_subkey(md->tagname);
    gchar *setting = g_strdup_printf("ui_last/import_last_%s", metadata_name);
    const char *meta = dt_conf_get_string_const(setting);
    g_signal_handlers_block_by_func(w, _import_metadata_changed, metadata);
    gtk_entry_set_text(GTK_ENTRY(w), meta);
    g_signal_handlers_unblock_by_func(w, _import_metadata_changed, metadata);
    g_free(setting);
    w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), 2, i + DT_META_META_VALUE);
    setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", metadata_name);
    const uint32_t flag = dt_conf_get_int(setting);
    g_signal_handlers_block_by_func(w, _import_metadata_toggled, metadata);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), flag & DT_METADATA_FLAG_IMPORTED);
    g_signal_handlers_unblock_by_func(w, _import_metadata_toggled, metadata);
    g_free(setting);
    i++;
  }
  dt_pthread_mutex_unlock(&darktable.metadata_threadsafe);

  GtkWidget *w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), 1, metadata->num_grid_rows + DT_META_TAGS_HEADER);
  const char *tags = dt_conf_get_string_const("ui_last/import_last_tags");
  g_signal_handlers_block_by_func(w, _import_tags_changed, metadata);
  gtk_entry_set_text(GTK_ENTRY(w), tags);
  g_signal_handlers_unblock_by_func(w, _import_tags_changed, metadata);
  w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), 2, metadata->num_grid_rows + DT_META_TAGS_HEADER);
  const gboolean imported = dt_conf_get_bool("ui_last/import_last_tags_imported");
  g_signal_handlers_block_by_func(w, _import_metadata_toggled, metadata);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), imported);
  g_signal_handlers_unblock_by_func(w, _import_metadata_toggled, metadata);

  w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), 1, DT_META_META_HEADER);
  gtk_combo_box_set_active(GTK_COMBO_BOX(w), -1);
  w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), 1, metadata->num_grid_rows + DT_META_META_VALUE);
  gtk_combo_box_set_active(GTK_COMBO_BOX(w), -1);
}

void dt_import_metadata_reset(dt_import_metadata_t *metadata)
{
  _metadata_reset_all(metadata, TRUE);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
