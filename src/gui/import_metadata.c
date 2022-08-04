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
  DT_META_TAGS_HEADER = DT_META_META_VALUE + DT_METADATA_NUMBER,
  DT_META_TAGS_VALUE,
  DT_META_TOTAL_SIZE
} dt_import_grid_t;


static void _import_metadata_presets_update(dt_import_metadata_t *metadata);

static void _metadata_save(GtkWidget *widget, dt_import_metadata_t *metadata)
{
  const char *name = gtk_widget_get_name(widget);
  const int i = dt_metadata_get_keyid_by_name(name);
  if(i != -1)
  {
    char *setting = g_strdup_printf("ui_last/import_last_%s", name);
    dt_conf_set_string(setting, gtk_entry_get_text(GTK_ENTRY(widget)));
    g_free(setting);
  }
}

static void _import_metadata_changed(GtkWidget *widget, dt_import_metadata_t *metadata)
{
  _metadata_save(widget, metadata);
  GtkWidget *w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), 1, DT_META_META_HEADER);
  gtk_combo_box_set_active(GTK_COMBO_BOX(w), -1);
}

static gboolean _import_metadata_reset(GtkWidget *label, GdkEventButton *event, GtkWidget *widget)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    gtk_entry_set_text(GTK_ENTRY(widget), "");
  }
  return FALSE;
}

static void _metadata_reset_all(dt_import_metadata_t *metadata, const gboolean hard)
{
  for(unsigned int i = DT_META_META_VALUE; i < DT_META_TOTAL_SIZE; i++)
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
    for(unsigned int i = DT_META_META_VALUE; i < DT_META_TOTAL_SIZE; i++)
    {
      GtkWidget *w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), 2, i);
      if(GTK_IS_TOGGLE_BUTTON(w))
      {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), TRUE);
      }
    }
  }
}

static gboolean _import_metadata_reset_all(GtkWidget *label, GdkEventButton *event, dt_import_metadata_t *metadata)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    _metadata_reset_all(metadata, FALSE);
  }
  return FALSE;
}

static void _import_metadata_toggled(GtkWidget *widget, dt_import_metadata_t *metadata)
{
  const char *name = gtk_widget_get_name(widget);
  if(g_strcmp0(name, "tags"))
  {
    const int i = dt_metadata_get_keyid_by_name(name);
    if(i != -1)
    {
      char *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);
      const gboolean imported = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
      const uint32_t flag = dt_conf_get_int(setting);
      dt_conf_set_int(setting, imported ? flag | DT_METADATA_FLAG_IMPORTED : flag & ~DT_METADATA_FLAG_IMPORTED);
      g_free(setting);
    }
  }
  else
  {
    const gboolean imported = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
    dt_conf_set_bool("ui_last/import_last_tags_imported", imported);
  }
}

static void _import_tags_changed(GtkWidget *widget, dt_import_metadata_t *metadata)
{
  GtkWidget *w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), 1, DT_META_TAGS_HEADER);
  gtk_combo_box_set_active(GTK_COMBO_BOX(w), -1);
  w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), 1, DT_META_TAGS_VALUE);
  dt_conf_set_string("ui_last/import_last_tags", gtk_entry_get_text(GTK_ENTRY(w)));
}

static void _update_layout(dt_import_metadata_t *metadata)
{
  const gboolean write_xmp = (dt_image_get_xmp_mode() != DT_WRITE_XMP_NEVER);
  GtkWidget *w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), 2, DT_META_META_HEADER);
  gtk_widget_set_visible(w, !write_xmp);
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    const gboolean internal = dt_metadata_get_type_by_display_order(i) == DT_METADATA_TYPE_INTERNAL;
    const gchar *metadata_name = (gchar *)dt_metadata_get_name_by_display_order(i);
    char *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", metadata_name);
    const gboolean visible = !internal & !(dt_conf_get_int(setting) & DT_METADATA_FLAG_HIDDEN);
    g_free(setting);
    for(int j = 0; j < 3; j++)
    {
      w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), j, i + DT_META_META_VALUE);
      gtk_widget_set_visible(w, j < 2 ? visible : visible & !write_xmp);
    }
  }
  w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), 2, DT_META_TAGS_VALUE);
  gtk_widget_set_visible(w, !write_xmp);
}

static void _apply_metadata_toggled(GtkWidget *widget, GtkWidget *grid)
{
  // activate widgets as needed
  const gboolean default_metadata = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  for(int i = DT_META_META_HEADER; i < DT_META_TOTAL_SIZE; i++)
  {
    for(int j = 0; j < 2 ; j++)
    {
      GtkWidget *w = gtk_grid_get_child_at(GTK_GRID(grid), j, i);
      if(GTK_IS_WIDGET(w))
        gtk_widget_set_sensitive(w, default_metadata);
    }
  }
}

static void _metadata_prefs_changed(gpointer instance, dt_import_metadata_t *metadata)
{
  _update_layout(metadata);
}

static void _metadata_list_changed(gpointer instance, int type, dt_import_metadata_t *metadata)
{
  if(type == DT_METADATA_SIGNAL_HIDDEN || type == DT_METADATA_SIGNAL_SHOWN)
    _update_layout(metadata);
}

static void _import_metadata_presets_changed(GtkWidget *widget, dt_import_metadata_t *metadata)
{
  GtkTreeIter iter;

  if(gtk_combo_box_get_active_iter(GTK_COMBO_BOX(widget), &iter) == TRUE)
  {
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(widget));
    GValue value = {
      0,
    };
    gchar *sv;

    for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
    {
      gtk_tree_model_get_value(model, &iter, i+1, &value);
      if((sv = (gchar *)g_value_get_string(&value)) != NULL && sv[0] != '\0')
      {
        const uint32_t keyid = dt_metadata_get_keyid_by_display_order(i);
        GtkWidget *w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), 1, keyid + 1);
        const gboolean visible = gtk_widget_get_visible(w);
        // make sure we don't provide values for hidden stuff
        if(visible)
        {
          g_signal_handlers_block_by_func(w, _import_metadata_changed, metadata);
          gtk_entry_set_text(GTK_ENTRY(w), sv);
          g_signal_handlers_unblock_by_func(w, _import_metadata_changed, metadata);
          _metadata_save(w, metadata);
        }
      }
      g_value_unset(&value);
    }
  }
}

static void _import_metadata_presets_update(dt_import_metadata_t *metadata)
{
  gtk_list_store_clear(metadata->m_model);
  GtkTreeIter iter;
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

    char *buf = (char *)op_params;
    char *metadata_param[DT_METADATA_NUMBER];
    uint32_t metadata_len[DT_METADATA_NUMBER];
    uint32_t total_len = 0;
    for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
    {
      if(dt_metadata_get_type_by_display_order(i) == DT_METADATA_TYPE_INTERNAL)
        continue;
      metadata_param[i] = buf;
      metadata_len[i] = strlen(metadata_param[i]) + 1;
      buf += metadata_len[i];
      total_len +=  metadata_len[i];
    }

    if(op_params_size == total_len)
    {
      gtk_list_store_append(metadata->m_model, &iter);
      // column 0 give the name, the following ones the different metadata
      gtk_list_store_set(metadata->m_model, &iter, 0, (char *)sqlite3_column_text(stmt, 0), -1);
      for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
      {
        if(dt_metadata_get_type_by_display_order(i) == DT_METADATA_TYPE_INTERNAL)
          continue;
        gtk_list_store_set(metadata->m_model, &iter, i+1, metadata_param[i], -1);
      }
    }
  }
  sqlite3_finalize(stmt);
}

static void _import_tags_presets_changed(GtkWidget *widget, dt_import_metadata_t *metadata)
{
  GtkTreeIter iter;

  if(gtk_combo_box_get_active_iter(GTK_COMBO_BOX(widget), &iter) == TRUE)
  {
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(widget));
    char *tags;
    gtk_tree_model_get(model, &iter, 1, &tags, -1);
    GtkWidget *w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), 1, DT_META_TAGS_VALUE);
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
  GtkTreeIter iter;
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
          tags = dt_util_dstrcat(tags, "%s,", tag);
          g_free(tag);
          entry++;
        }
        if(tags)
          tags[strlen(tags) - 1] = '\0';
        g_strfreev(tokens);
        gtk_list_store_append(metadata->t_model, &iter);
        gtk_list_store_set(metadata->t_model, &iter,
                           0, (char *)sqlite3_column_text(stmt, 0),
                           1, tags, -1);
        g_free(tags);
      }
    }
  }
  sqlite3_finalize(stmt);
}

static void _metadata_presets_changed(gpointer instance, gpointer module, dt_import_metadata_t *metadata)
{
  if(!g_strcmp0(module, "metadata"))
    _import_metadata_presets_update(metadata);
  else if(!g_strcmp0(module, "tagging"))
    _import_tags_presets_update(metadata);
}

static GtkWidget *_set_up_label(GtkWidget *label, const int align, const int line,
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

static GtkWidget *_set_up_combobox(GtkListStore *model, const int line, dt_import_metadata_t *metadata)
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

static void _set_up_entry(GtkWidget *entry, const char *str, const char *name,
                             const int line, dt_import_metadata_t *metadata)
{
  gtk_widget_set_name(entry, name);
  gtk_entry_set_text(GTK_ENTRY(entry), str);
  gtk_widget_set_halign(entry, GTK_ALIGN_FILL);
  gtk_entry_set_width_chars(GTK_ENTRY(entry), 5);
  gtk_widget_set_hexpand(entry, TRUE);
  gtk_grid_attach(GTK_GRID(metadata->grid), entry, 1, line, 1, 1);
}

static void _set_up_toggle_button(GtkWidget *button, const gboolean state, const char *name,
                                  const int line, dt_import_metadata_t *metadata)
{
  gtk_widget_set_name(button, name);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), state);
  gtk_grid_attach(GTK_GRID(metadata->grid), button, 2, line, 1, 1);
  gtk_widget_set_halign(button, GTK_ALIGN_CENTER);
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

  // presets from the metadata plugin
  GType types[DT_METADATA_NUMBER + 1];
  for(unsigned int i = 0; i < DT_METADATA_NUMBER + 1; i++)
  {
    types[i] = G_TYPE_STRING;
  }

  metadata->m_model = gtk_list_store_newv(DT_METADATA_NUMBER + 1, types);
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

  label = gtk_label_new(_("from xmp"));
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_tooltip_text(GTK_WIDGET(label),
                              _("selected metadata are imported from image and override the default value"
                                "\n this drives also the 'look for updated xmp files' and 'load sidecar file' actions"
                                "\n CAUTION: not selected metadata are cleaned up when xmp file is updated"
                              ));
  gtk_grid_attach(GTK_GRID(grid), label, 2, DT_META_META_HEADER, 1, 1);

  // grid content
  // metadata
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    const gchar *metadata_name = (gchar *)dt_metadata_get_name_by_display_order(i);
    gchar *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", metadata_name);
    const uint32_t flag = dt_conf_get_int(setting);
    g_free(setting);

    GtkWidget *metadata_label = gtk_label_new(_(metadata_name));
    labelev = _set_up_label(metadata_label, GTK_ALIGN_START, i + DT_META_META_VALUE, metadata);

    GtkWidget *metadata_entry = gtk_entry_new();
    setting = g_strdup_printf("ui_last/import_last_%s", metadata_name);
    const char *str = dt_conf_get_string_const(setting);
    _set_up_entry(metadata_entry, str, metadata_name, i + DT_META_META_VALUE, metadata);
    g_free(setting);
    g_signal_connect(GTK_ENTRY(metadata_entry), "changed",
                     G_CALLBACK(_import_metadata_changed), metadata);
    g_signal_connect(GTK_EVENT_BOX(labelev), "button-press-event",
                     G_CALLBACK(_import_metadata_reset), metadata_entry);

    GtkWidget *metadata_imported = gtk_check_button_new();
    _set_up_toggle_button(metadata_imported, flag & DT_METADATA_FLAG_IMPORTED,
                          metadata_name, i + DT_META_META_VALUE, metadata);
    g_signal_connect(GTK_TOGGLE_BUTTON(metadata_imported), "toggled",
                     G_CALLBACK(_import_metadata_toggled), metadata);
  }

  // tags
  label = gtk_label_new(_("tag presets"));
  gtk_widget_set_name(label, "import-presets");
  labelev =_set_up_label(label, GTK_ALIGN_START, DT_META_TAGS_HEADER, metadata);

  presets = _set_up_combobox(metadata->t_model, DT_META_TAGS_HEADER, metadata);
  g_signal_connect(presets, "changed", G_CALLBACK(_import_tags_presets_changed), metadata);

  label = gtk_label_new(_("tags"));
  labelev = _set_up_label(label, GTK_ALIGN_START, DT_META_TAGS_VALUE, metadata);

  GtkWidget *entry = gtk_entry_new();
  gtk_widget_set_visible(entry, TRUE);
  const char *str = dt_conf_get_string_const("ui_last/import_last_tags");
  _set_up_entry(entry, str, "tags", DT_META_TAGS_VALUE, metadata);
  gtk_widget_set_tooltip_text(entry, _("comma separated list of tags"));
  g_signal_connect(GTK_ENTRY(entry), "changed",
                   G_CALLBACK(_import_tags_changed), metadata);
  g_signal_connect(GTK_EVENT_BOX(labelev), "button-press-event",
                   G_CALLBACK(_import_metadata_reset), entry);

  GtkWidget *tags_imported = gtk_check_button_new();
  _set_up_toggle_button(tags_imported, dt_conf_get_bool("ui_last/import_last_tags_imported"),
                        "tags", DT_META_TAGS_VALUE, metadata);
  g_signal_connect(GTK_TOGGLE_BUTTON(tags_imported), "toggled",
                   G_CALLBACK(_import_metadata_toggled), metadata);

  // overall
  g_signal_connect(metadata->apply_metadata, "toggled",
                   G_CALLBACK(_apply_metadata_toggled), grid);
  // needed since the apply_metadata starts being turned off,
  // and setting it to off doesn't emit the 'toggled' signal ...
  _apply_metadata_toggled(metadata->apply_metadata, grid);

  // connect changed signal
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_PREFERENCES_CHANGE,
                            G_CALLBACK(_metadata_prefs_changed), metadata);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_METADATA_CHANGED,
                            G_CALLBACK(_metadata_list_changed), metadata);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_PRESETS_CHANGED,
                            G_CALLBACK(_metadata_presets_changed), metadata);
  _update_layout(metadata);
}

void dt_import_metadata_cleanup(dt_import_metadata_t *metadata)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_metadata_prefs_changed), metadata);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_metadata_list_changed), metadata);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_metadata_presets_changed), metadata);
}

void dt_import_metadata_update(dt_import_metadata_t *metadata)
{
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    GtkWidget *w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), 1, i + DT_META_META_VALUE);
    const gchar *metadata_name = dt_metadata_get_name_by_display_order(i);
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
  }

  GtkWidget *w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), 1, DT_META_TAGS_VALUE);
  const char *tags = dt_conf_get_string_const("ui_last/import_last_tags");
  g_signal_handlers_block_by_func(w, _import_tags_changed, metadata);
  gtk_entry_set_text(GTK_ENTRY(w), tags);
  g_signal_handlers_unblock_by_func(w, _import_tags_changed, metadata);
  w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), 2, DT_META_TAGS_VALUE);
  const gboolean imported = dt_conf_get_bool("ui_last/import_last_tags_imported");
  g_signal_handlers_block_by_func(w, _import_metadata_toggled, metadata);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), imported);
  g_signal_handlers_unblock_by_func(w, _import_metadata_toggled, metadata);

  w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), 1, DT_META_META_HEADER);
  gtk_combo_box_set_active(GTK_COMBO_BOX(w), -1);
  w = gtk_grid_get_child_at(GTK_GRID(metadata->grid), 1, DT_META_TAGS_HEADER);
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

