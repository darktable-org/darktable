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

#include "common/debug.h"
#include "common/metadata.h"
#include "control/conf.h"
#include "control/control.h"
#include "gui/import_metadata.h"
#include "gui/preferences.h"
#include "gui/gtk.h"

static void _import_metadata_changed(GtkWidget *widget, dt_import_metadata_t *metadata)
{
  const char *name = gtk_widget_get_name(widget);
  const int i = dt_metadata_get_keyid_by_name(name);
  if(i != -1)
  {
    char *setting = dt_util_dstrcat(NULL, "ui_last/import_last_%s", name);
    dt_conf_set_string(setting, gtk_entry_get_text(GTK_ENTRY(widget)));
    g_free(setting);
  }
  gtk_combo_box_set_active(GTK_COMBO_BOX(metadata->presets), -1);
}

static void _import_metadata_reset(GtkWidget *label, GdkEventButton *event, GtkWidget *widget)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    gtk_entry_set_text(GTK_ENTRY(widget), "");
  }
}

static void _import_metadata_toggled(GtkWidget *widget, dt_import_metadata_t *metadata)
{
  const char *name = gtk_widget_get_name(widget);
  const int i = dt_metadata_get_keyid_by_name(name);
  if(i != -1)
  {
    char *setting = dt_util_dstrcat(NULL, "plugins/lighttable/metadata/%s_flag", name);
    const gboolean imported = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
    const uint32_t flag = dt_conf_get_int(setting);
    dt_conf_set_int(setting, imported ? flag | DT_METADATA_FLAG_IMPORTED : flag & ~DT_METADATA_FLAG_IMPORTED);
    g_free(setting);
  }
}

static void _import_tags_changed(GtkWidget *widget, dt_import_metadata_t *metadata)
{
  dt_conf_set_string("ui_last/import_last_tags", gtk_entry_get_text(GTK_ENTRY(metadata->tags)));
}

static void _apply_metadata_toggled(GtkWidget *widget, gpointer user_data)
{
  GtkWidget *grid = GTK_WIDGET(user_data);
  // get the number of lines of the grid - the last one is for tags
  int i = 0;
  GtkWidget *w = gtk_grid_get_child_at(GTK_GRID(grid), 0, i);
  while(w)
  {
    i++;
    w = gtk_grid_get_child_at(GTK_GRID(grid), 0, i);
  }
  // activate widgets as needed
  const gboolean default_metadata = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  for(int j = 0; j < i; j++)
  {
    w = gtk_grid_get_child_at(GTK_GRID(grid), 1, j);
    gtk_widget_set_sensitive(w, default_metadata);
  }
  w = gtk_grid_get_child_at(GTK_GRID(grid), 0, 0);
  gtk_widget_set_sensitive(w, default_metadata);
  w = gtk_grid_get_child_at(GTK_GRID(grid), 0, i - 1);
  gtk_widget_set_sensitive(w, default_metadata);
}

static void _metadata_presets_changed(GtkWidget *widget, dt_import_metadata_t *metadata)
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
        const char *name = dt_metadata_get_name(keyid);
        char *setting = dt_util_dstrcat(NULL, "plugins/lighttable/metadata/%s_flag", name);
        const gboolean hidden = dt_conf_get_int(setting) & DT_METADATA_FLAG_HIDDEN;
        g_free(setting);
        // make sure we don't provide values for hidden stuff
        if(!hidden)
        {
          g_signal_handlers_block_by_func(metadata->metadata[keyid], _import_metadata_changed, metadata);
          gtk_entry_set_text(GTK_ENTRY(metadata->metadata[keyid]), sv);
          g_signal_handlers_unblock_by_func(metadata->metadata[keyid], _import_metadata_changed, metadata);
        }
      }
      g_value_unset(&value);
    }
  }
}

void dt_import_metadata_init(dt_import_metadata_t *metadata)
{
  // default metadata
  GtkWidget *grid = gtk_grid_new();
  gtk_box_pack_start(GTK_BOX(metadata->box), grid, FALSE, FALSE, 0);
//  gtk_widget_set_name(grid, "import_metadata");
  gtk_grid_set_column_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(5));

  // presets from the metadata plugin
  GtkTreeIter iter;
  GType types[DT_METADATA_NUMBER + 1];
  for(unsigned int i = 0; i < DT_METADATA_NUMBER+1; i++)
  {
    types[i] = G_TYPE_STRING;
  }
  GtkListStore *model = gtk_list_store_newv(DT_METADATA_NUMBER + 1, types);

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT name, op_params FROM data.presets WHERE operation = 'metadata'",
                              -1, &stmt, NULL);
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
      metadata_param[i] = buf;
      metadata_len[i] = strlen(metadata_param[i]) + 1;
      buf += metadata_len[i];
      total_len +=  metadata_len[i];
    }

    if(op_params_size == total_len)
    {
      gtk_list_store_append(model, &iter);
      // column 0 give the name, the following ones the different metadata
      gtk_list_store_set(model, &iter, 0, (char *)sqlite3_column_text(stmt, 0), -1);
      for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
      {
        gtk_list_store_set(model, &iter, i+1, metadata_param[i], -1);
      }
    }
  }
  sqlite3_finalize(stmt);

  const gboolean write_xmp = dt_conf_get_bool("write_sidecar_files");
  // grid headers
  int line = 0;

  GtkWidget *label = gtk_label_new(_("preset"));
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), label, 0, line++, 1, 1);
  gtk_widget_set_tooltip_text(GTK_WIDGET(label), _("metadata to be applied per default"));

  GtkWidget *presets = gtk_combo_box_new_with_model(GTK_TREE_MODEL(model));
  gtk_widget_set_hexpand(presets, TRUE);
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(presets), renderer, TRUE);
  gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(presets), renderer, "text", 0, NULL);
  g_object_set(G_OBJECT(renderer), "ellipsize", PANGO_ELLIPSIZE_END, (gchar *)0);
  gtk_grid_attach_next_to(GTK_GRID(grid), presets, label, GTK_POS_RIGHT, 1, 1);
  g_object_unref(model);
  metadata->presets = presets;

  if(!write_xmp)
  {
    label = gtk_label_new(_("from xmp"));
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_tooltip_text(GTK_WIDGET(label),
                                _("selected metadata are imported from image and override the default value"
                                  "\n this drives also the \'look for updated xmp files\' and \'load sidecar file\' actions"
                                  "\n CAUTION: not selected metadata are cleaned up when xmp file is updated"
                                ));
    gtk_grid_attach_next_to(GTK_GRID(grid), label, presets, GTK_POS_RIGHT, 1, 1);
  }
  // grid content
  // metadata
  GtkWidget *metadata_label[DT_METADATA_NUMBER];
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    metadata->metadata[i] = NULL;
    metadata->imported[i] = NULL;
    if(dt_metadata_get_type_by_display_order(i) != DT_METADATA_TYPE_INTERNAL)
    {
      const gchar*metadata_name = (gchar *)dt_metadata_get_name_by_display_order(i);
      char *setting = dt_util_dstrcat(NULL, "plugins/lighttable/metadata/%s_flag",
                                      metadata_name);
      const uint32_t flag = dt_conf_get_int(setting);
      g_free(setting);
      if(!(flag & DT_METADATA_FLAG_HIDDEN))
      {
        metadata_label[i] = gtk_label_new(_(metadata_name));
        gtk_widget_set_halign(metadata_label[i], GTK_ALIGN_START);
        gtk_label_set_ellipsize(GTK_LABEL(metadata_label[i]), PANGO_ELLIPSIZE_END);
        GtkWidget *labelev = gtk_event_box_new();
        gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
        gtk_container_add(GTK_CONTAINER(labelev), metadata_label[i]);
        gtk_grid_attach(GTK_GRID(grid), labelev, 0, line, 1, 1);

        metadata->metadata[i] = gtk_entry_new();
        gtk_widget_set_name(metadata->metadata[i], metadata_name);
        setting = dt_util_dstrcat(NULL, "ui_last/import_last_%s", metadata_name);
        gchar *str = dt_conf_get_string(setting);
        gtk_entry_set_text(GTK_ENTRY(metadata->metadata[i]), str);
        gtk_widget_set_halign(metadata->metadata[i], GTK_ALIGN_FILL);
        gtk_entry_set_width_chars(GTK_ENTRY(metadata->metadata[i]), 5);
        gtk_widget_set_hexpand(metadata->metadata[i], TRUE);
        dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(metadata->metadata[i]));
        g_free(str);
        g_free(setting);
        gtk_grid_attach(GTK_GRID(grid), metadata->metadata[i], 1, line, 1, 1);
        g_signal_connect(GTK_ENTRY(metadata->metadata[i]), "changed",
                         G_CALLBACK(_import_metadata_changed), metadata);
        g_signal_connect(GTK_EVENT_BOX(labelev), "button-press-event",
                         G_CALLBACK(_import_metadata_reset), metadata->metadata[i]);

        if(!write_xmp)
        {
          metadata->imported[i] = gtk_check_button_new();
          gtk_widget_set_name(metadata->imported[i], metadata_name);
          gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(metadata->imported[i]),
                                       flag & DT_METADATA_FLAG_IMPORTED);
          gtk_grid_attach(GTK_GRID(grid), metadata->imported[i], 2, line, 1, 1);
          gtk_widget_set_halign(metadata->imported[i], GTK_ALIGN_CENTER);
          g_signal_connect(GTK_TOGGLE_BUTTON(metadata->imported[i]), "toggled",
                           G_CALLBACK(_import_metadata_toggled), metadata);
        }
        line++;
      }
    }
  }

  //tags
  label = gtk_label_new(_("tags"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  GtkWidget *labelev = gtk_event_box_new();
  gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
  gtk_container_add(GTK_CONTAINER(labelev), label);
  gtk_grid_attach(GTK_GRID(grid), labelev, 0, line, 1, 1);

  metadata->tags = gtk_entry_new();
  gtk_widget_set_hexpand(metadata->tags, TRUE);
  gtk_entry_set_width_chars(GTK_ENTRY(metadata->tags), 5);
  gchar *str = dt_conf_get_string("ui_last/import_last_tags");
  gtk_widget_set_tooltip_text(metadata->tags, _("comma separated list of tags"));
  gtk_entry_set_text(GTK_ENTRY(metadata->tags), str);
  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(metadata->tags));
  g_free(str);
  gtk_grid_attach(GTK_GRID(grid), metadata->tags, 1, line, 1, 1);
  g_signal_connect(GTK_ENTRY(metadata->tags), "changed",
                   G_CALLBACK(_import_tags_changed), metadata);
  g_signal_connect(GTK_EVENT_BOX(labelev), "button-press-event",
                   G_CALLBACK(_import_metadata_reset), metadata->tags);

  g_signal_connect(metadata->apply_metadata, "toggled",
                   G_CALLBACK(_apply_metadata_toggled), grid);
  // needed since the apply_metadata starts being turned off,
  // and setting it to off doesn't emit the 'toggled' signal ...
  _apply_metadata_toggled(metadata->apply_metadata, grid);

  g_signal_connect(presets, "changed", G_CALLBACK(_metadata_presets_changed), metadata);
}

void dt_import_metadata_cleanup(dt_import_metadata_t *metadata)
{
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    if(metadata->metadata[i])
    {
      dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(metadata->metadata[i]));
    }
  }
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(metadata->tags));
}

void dt_import_metadata_update(dt_import_metadata_t *metadata)
{
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    if(metadata->metadata[i])
    {
      const gchar *metadata_name = dt_metadata_get_name_by_display_order(i);
      char *setting = dt_util_dstrcat(NULL, "ui_last/import_last_%s", metadata_name);
      char *meta = dt_conf_get_string(setting);
      g_signal_handlers_block_by_func(metadata->metadata[i], _import_metadata_changed, metadata);
      gtk_entry_set_text(GTK_ENTRY(metadata->metadata[i]), meta);
      g_signal_handlers_unblock_by_func(metadata->metadata[i], _import_metadata_changed, metadata);
      g_free(meta);
      g_free(setting);
      if(metadata->imported[i])
      {
        setting = dt_util_dstrcat(NULL, "plugins/lighttable/metadata/%s_flag", metadata_name);
        const uint32_t flag = dt_conf_get_int(setting);
        g_signal_handlers_block_by_func(metadata->imported[i], _import_metadata_toggled, metadata);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(metadata->imported[i]), flag & DT_METADATA_FLAG_IMPORTED);
        g_signal_handlers_unblock_by_func(metadata->imported[i], _import_metadata_toggled, metadata);
        g_free(setting);
      }
    }
  }
  char *tags = dt_conf_get_string("ui_last/import_last_tags");
  g_signal_handlers_block_by_func(metadata->tags, _import_tags_changed, metadata);
  gtk_entry_set_text(GTK_ENTRY(metadata->tags), tags);
  g_signal_handlers_unblock_by_func(metadata->tags, _import_tags_changed, metadata);
  g_free(tags);
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
