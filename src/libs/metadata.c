/*
    This file is part of darktable,
    copyright (c) 2010-2011 tobias ellinghaus, Henrik Andersson.

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
#include <gdk/gdkkeysyms.h>

DT_MODULE(2)

typedef struct dt_lib_metadata_t
{
  int imgsel;
  GtkComboBox *metadata_cb[DT_METADATA_NUMBER];
  gboolean multi_metadata[DT_METADATA_NUMBER];
  GtkGrid *metadata_grid;
  gboolean editing;
  GtkWidget *clear_button;
  GtkWidget *apply_button;
  GtkWidget *config_button;
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

static void _entry_set_style(GtkEntry *entry, PangoStyle style)
{
  PangoAttribute *attr = pango_attr_style_new(style);
  PangoLayout *layout = gtk_entry_get_layout(entry);
  PangoAttrList *attrs = pango_layout_get_attributes(layout);
  if (!attrs) attrs = pango_attr_list_new();
  pango_attr_list_insert(attrs, attr);
  gtk_entry_set_attributes(entry, attrs);
}

static void fill_combo_box_entry(GtkComboBox *combobox, uint32_t count, GList *items, gboolean *multi)
{
  GtkListStore *model = (GtkListStore *)gtk_combo_box_get_model(combobox);
  gtk_list_store_clear(GTK_LIST_STORE(model));
  GtkTreeIter iter;
  GtkEntry *entry = (GtkEntry *)gtk_bin_get_child(GTK_BIN(combobox));

  if(count == 0)  // no metadata value
  {
    gtk_entry_set_text(entry, "");
    _entry_set_style(entry, PANGO_STYLE_NORMAL);
    *multi = FALSE;
    return;
  }
  if(count == 1) // images with different metadata values
  {
    gtk_list_store_append(model, &iter);
    gtk_list_store_set(model, &iter, 0, _("<leave unchanged>"), -1);
    gtk_combo_box_set_button_sensitivity(combobox, GTK_SENSITIVITY_AUTO);
    _entry_set_style(entry, PANGO_STYLE_ITALIC);
    *multi = TRUE;
  }
  else // one or several images with the same metadata value
  {
    gtk_combo_box_set_button_sensitivity(combobox, GTK_SENSITIVITY_OFF);
    _entry_set_style(entry, PANGO_STYLE_NORMAL);
    *multi = FALSE;
  }
  for(GList *item = items; item; item = g_list_next(item))
  {
    gtk_list_store_append(model, &iter);
    gtk_list_store_set(model, &iter, 0, (char *)item->data, -1);
  }
  gtk_combo_box_set_active(combobox, 0);
}

static void update(dt_lib_module_t *self, gboolean early_bark_out)
{
  //   early_bark_out = FALSE; // FIXME: when barking out early we don't update on ctrl-a/ctrl-shift-a. but
  //   otherwise it's impossible to edit text
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;
  const int imgsel = dt_control_get_mouse_over_id();
  if(early_bark_out && imgsel == d->imgsel) return;

  d->imgsel = imgsel;

  sqlite3_stmt *stmt;

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
  if(imgsel < 0) // selected images
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT COUNT(*) FROM main.selected_images", -1, &stmt, NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW) imgs_count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT key, value, COUNT(id) AS ct FROM main.meta_data WHERE id IN "
                                                               "(SELECT imgid FROM main.selected_images) GROUP BY "
                                                               "key, value ORDER BY value",
                                -1, &stmt, NULL);
  }
  else // single image under mouse cursor
  {
    imgs_count = 1;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT key, value, COUNT(id) AS ct FROM main.meta_data "
                                                               "WHERE id = ?1 GROUP BY key, value ORDER BY value",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgsel);
  }
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    if(sqlite3_column_bytes(stmt, 1))
    {
      const uint32_t key = sqlite3_column_int(stmt, 0);
      char *value = g_strdup((char *)sqlite3_column_text(stmt, 1));
      const uint32_t count = sqlite3_column_int(stmt, 2);
      metadata_count[key] = (count == imgs_count) ? 2 : 1;  // if = all images have the same metadata
      metadata[key] = g_list_append(metadata[key], value);
    }
  }
  sqlite3_finalize(stmt);

  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    const uint32_t keyid = dt_metadata_get_keyid_by_display_order(i);
    fill_combo_box_entry(d->metadata_cb[i], metadata_count[keyid], metadata[keyid], &(d->multi_metadata[i]));
    g_list_free_full(metadata[keyid], g_free);
  }
}


static gboolean draw(GtkWidget *widget, cairo_t *cr, dt_lib_module_t *self)
{
  if(!dt_control_running()) return FALSE;
  update(self, TRUE);
  return FALSE;
}

static void clear_button_clicked(GtkButton *button, dt_lib_module_t *self)
{
  dt_metadata_clear(-1, TRUE, TRUE);
  dt_image_synch_xmp(-1);
  update(self, FALSE);
}

static void _append_kv(GList **l, const gchar *key, const gchar *value)
{
  *l = g_list_append(*l, (gchar *)key);
  *l = g_list_append(*l, (gchar *)value);
}

static void write_metadata(dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;

  int32_t mouse_over_id;

  d->editing = FALSE;

  mouse_over_id = d->imgsel;

  gchar *metadata[DT_METADATA_NUMBER];
  GList *key_value = NULL;
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    const uint32_t keyid = dt_metadata_get_keyid_by_display_order(i);
    metadata[i] = g_strdup(gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(d->metadata_cb[i])))));
    if(metadata[i] != NULL && (d->multi_metadata[i] == FALSE || gtk_combo_box_get_active(GTK_COMBO_BOX(d->metadata_cb[i])) != 0))
    {
      _append_kv(&key_value, dt_metadata_get_key(keyid), metadata[i]);
    }
  }

  dt_metadata_set_list(mouse_over_id, key_value, TRUE, TRUE);

  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    g_free(metadata[i]);
  }
  g_list_free(key_value);

  dt_control_signal_raise(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE);

  dt_image_synch_xmp(mouse_over_id);
  update(self, FALSE);
}

static void apply_button_clicked(GtkButton *button, dt_lib_module_t *self)
{
  write_metadata(self);
}

static gboolean key_pressed(GtkWidget *entry, GdkEventKey *event, dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;

  switch(event->keyval)
  {
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
      write_metadata(self);
      gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
      break;
    case GDK_KEY_Escape:
      update(self, FALSE);
      gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
      break;
    case GDK_KEY_Tab:
      write_metadata(self);
      break;
    default:
      d->editing = TRUE;
      _entry_set_style(GTK_ENTRY(entry), PANGO_STYLE_NORMAL);
  }
  return FALSE;
}

static void combobox_changed(GtkComboBox *combobox, dt_lib_module_t *self)
{
  GtkEntry *entry = (GtkEntry*)gtk_bin_get_child(GTK_BIN(combobox));
  gchar *value;
  GtkTreeModel *model = gtk_combo_box_get_model(combobox);
  GtkTreeIter iter;
  if(gtk_combo_box_get_active_iter(combobox, &iter))
  {
    gtk_tree_model_get(model, &iter, 0, &value, -1);
    const gboolean leave_unchanged = g_strcmp0(value, _("<leave unchanged>")) == 0;
    _entry_set_style(entry, leave_unchanged ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
  }
}

void gui_reset(dt_lib_module_t *self)
{
  update(self, FALSE);
}

int position()
{
  return 510;
}

static void mouse_over_image_callback(gpointer instace, dt_lib_module_t *self)
{
  const dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;
  /* lets trigger an expose for a redraw of widget */
  if(d->editing)
  {
    write_metadata(self);
    gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
  }
  update(self, FALSE);
}

static void update_layout(dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;

  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    const gchar *metadata_name = dt_metadata_get_name_by_display_order(i);
    char *setting = dt_util_dstrcat(NULL, "plugins/lighttable/metadata/%s_hidden", metadata_name);
    const gboolean hidden = dt_conf_get_bool(setting);
    if(hidden)
    {
      gtk_widget_hide(gtk_grid_get_child_at(d->metadata_grid,0,i));
      gtk_widget_hide(gtk_grid_get_child_at(d->metadata_grid,1,i));
    }
    else
    {
      gtk_widget_show(gtk_grid_get_child_at(d->metadata_grid,0,i));
      gtk_widget_show(gtk_grid_get_child_at(d->metadata_grid,1,i));
    }
    g_free(setting);
  }
}

static void config_button_clicked(GtkButton *button, dt_lib_module_t *self)
{
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("metadata settings"), GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT,
                                       _("cancel"), GTK_RESPONSE_NONE, _("save"), GTK_RESPONSE_YES, NULL);
  GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

  GtkWidget *grid = gtk_grid_new();
  gtk_container_add(GTK_CONTAINER(area), grid);
  GtkWidget *label = gtk_label_new(_("metadata"));
  gtk_grid_attach(GTK_GRID(grid), label, 0, 0, 1, 1);
  label = gtk_label_new(_("hidden"));
  gtk_grid_attach(GTK_GRID(grid), label, 1, 0, 1, 1);
  label = gtk_label_new(_("private"));
  gtk_grid_attach(GTK_GRID(grid), label, 2, 0, 1, 1);
  GtkWidget *metadata[DT_METADATA_NUMBER];
  GtkWidget *private[DT_METADATA_NUMBER];
  gchar *metadata_name[DT_METADATA_NUMBER];
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    metadata_name[i] = (gchar *)dt_metadata_get_name_by_display_order(i);
    label = gtk_label_new(_(metadata_name[i]));
    gtk_grid_attach(GTK_GRID(grid), label, 0, i+1, 1, 1);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_hexpand(label, TRUE);
    metadata[i] = gtk_check_button_new();
    char *setting = dt_util_dstrcat(NULL, "plugins/lighttable/metadata/%s_hidden", metadata_name[i]);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(metadata[i]), dt_conf_get_bool(setting));
    gtk_grid_attach(GTK_GRID(grid), metadata[i], 1, i+1, 1, 1);
    gtk_widget_set_halign(metadata[i], GTK_ALIGN_CENTER);
    g_free(setting);
    private[i] = gtk_check_button_new();
    setting = dt_util_dstrcat(NULL, "plugins/lighttable/metadata/%s_private", metadata_name[i]);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(private[i]), dt_conf_get_bool(setting));
    gtk_grid_attach(GTK_GRID(grid), private[i], 2, i+1, 1, 1);
    gtk_widget_set_halign(private[i], GTK_ALIGN_CENTER);
    g_free(setting);
  }

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_widget_show_all(dialog);

  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_YES)
  {
    for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
    {
      char *setting = dt_util_dstrcat(NULL, "plugins/lighttable/metadata/%s_hidden", metadata_name[i]);
      dt_conf_set_bool(setting, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(metadata[i])));
      g_free(setting);
      setting = dt_util_dstrcat(NULL, "plugins/lighttable/metadata/%s_private", metadata_name[i]);
      dt_conf_set_bool(setting, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(private[i])));
      g_free(setting);
    }
  }
  update_layout(self);
  gtk_widget_destroy(dialog);
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "clear"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "apply"), 0, 0);
}

void connect_key_accels(dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;

  dt_accel_connect_button_lib(self, "clear", d->clear_button);
  dt_accel_connect_button_lib(self, "apply", d->apply_button);
}

static void _set_combobox_style(GtkCellLayout *cell_layout, GtkCellRenderer *renderer,
                          GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
  gchar *value;
  gtk_tree_model_get(model, iter, 0, &value, -1);
  const gboolean leave_unchanged = g_strcmp0(value, _("<leave unchanged>")) == 0;
  g_object_set(renderer, "style", leave_unchanged ? PANGO_STYLE_ITALIC
                                                  : PANGO_STYLE_NORMAL, NULL);
  g_free(value);
}

static gboolean _combobox_tooltip_setup(GtkWidget *combobox, gint x, gint y, gboolean kb_mode,
                                        GtkTooltip* tooltip, dt_lib_module_t *self)
{
  gboolean res = FALSE;
  GtkEntry *entry = (GtkEntry*)gtk_bin_get_child(GTK_BIN(combobox));
  PangoLayout *layout = gtk_entry_get_layout(entry);
  gint width;
  pango_layout_get_size(layout, &width, NULL);
  width = width / PANGO_SCALE;
  GtkAllocation allocation;
  gtk_widget_get_allocation(GTK_WIDGET(entry), &allocation);
  if(allocation.width < width)  // the full text is not visible
  {
    gchar *value;
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(combobox));
    GtkTreeIter iter;
    if(gtk_combo_box_get_active_iter(GTK_COMBO_BOX(combobox), &iter))
    {
      gtk_tree_model_get(model, &iter, 0, &value, -1);
      gtk_tooltip_set_text(tooltip, value);
      res = TRUE;
    }
  }
  return res;
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)calloc(1, sizeof(dt_lib_metadata_t));
  self->data = (void *)d;

  d->imgsel = -1;

  GtkGrid *grid = (GtkGrid *)gtk_grid_new();
  self->widget = GTK_WIDGET(grid);
  gtk_grid_set_row_spacing(grid, DT_PIXEL_APPLY_DPI(5));
  grid = (GtkGrid *)gtk_grid_new();
  d->metadata_grid = grid;
  gtk_grid_attach(GTK_GRID(self->widget), GTK_WIDGET(grid), 0, 0, 1, 1);

  dt_gui_add_help_link(self->widget, "metadata_editor.html#metadata_editor_usage");
  gtk_grid_set_row_spacing(grid, DT_PIXEL_APPLY_DPI(5));
  gtk_grid_set_column_spacing(grid, DT_PIXEL_APPLY_DPI(10));

  g_signal_connect(self->widget, "draw", G_CALLBACK(draw), self);

  for(int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    GtkWidget *label = gtk_label_new(_(dt_metadata_get_name_by_display_order(i)));
    gtk_widget_set_halign(label, GTK_ALIGN_START);

    GtkListStore *model = gtk_list_store_new(1, G_TYPE_STRING);
    GtkWidget *combobox = gtk_combo_box_new_with_model_and_entry(GTK_TREE_MODEL(model));
    g_signal_connect(combobox, "changed", G_CALLBACK(combobox_changed), self);
    GList *renderers = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(combobox));
    GtkCellRenderer *renderer = (GtkCellRenderer *)renderers->data;

    GtkWidget *entry = gtk_bin_get_child(GTK_BIN(combobox));
    dt_gui_key_accel_block_on_focus_connect(entry);
    g_signal_connect(entry, "key-press-event", G_CALLBACK(key_pressed), self);
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 0);

    g_object_set(G_OBJECT(combobox), "has-tooltip", TRUE, NULL);
    g_signal_connect(G_OBJECT(combobox), "query-tooltip", G_CALLBACK(_combobox_tooltip_setup), self);
    g_object_unref(model);
    gtk_combo_box_set_entry_text_column(GTK_COMBO_BOX(combobox), 0);
    gtk_cell_layout_set_cell_data_func(GTK_CELL_LAYOUT(combobox), renderer, _set_combobox_style, NULL, NULL);

    d->metadata_cb[i] = GTK_COMBO_BOX(combobox);

    gtk_widget_set_hexpand(combobox, TRUE);

    GtkEntryCompletion *completion = gtk_entry_completion_new();
    gtk_entry_completion_set_model(completion, gtk_combo_box_get_model(GTK_COMBO_BOX(combobox)));
    gtk_entry_completion_set_text_column(completion, 0);
    gtk_entry_completion_set_inline_completion(completion, TRUE);
    gtk_entry_set_completion(GTK_ENTRY(entry), completion);
    g_object_unref(completion);

    g_object_set(G_OBJECT(label), "no-show-all", TRUE, NULL);
    g_object_set(G_OBJECT(combobox), "no-show-all", TRUE, NULL);
    gtk_grid_attach(grid, label, 0, i, 1, 1);
    gtk_grid_attach_next_to(grid, combobox, label, GTK_POS_RIGHT, 1, 1);
  }
  update_layout(self);

  // clear/apply buttons

  grid = (GtkGrid *)gtk_grid_new();
  gtk_grid_set_column_homogeneous(grid, TRUE);

  GtkWidget *button = gtk_button_new_with_label(_("clear"));
  d->clear_button = button;
  gtk_widget_set_tooltip_text(button, _("remove metadata from selected images"));
  gtk_grid_attach(grid, button, 0, 0, 4, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(clear_button_clicked), (gpointer)self);

  button = gtk_button_new_with_label(_("apply"));
  d->apply_button = button;
  gtk_widget_set_tooltip_text(button, _("write metadata for selected images"));
  gtk_grid_attach(grid, button, 4, 0, 4, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(apply_button_clicked), (gpointer)self);

  button = dtgtk_button_new(dtgtk_cairo_paint_preferences,
      CPF_DO_NOT_USE_BORDER | CPF_STYLE_BOX, NULL);
  d->config_button = button;
  gtk_widget_set_tooltip_text(button, _("configure metadata"));
  gtk_grid_attach(grid, button, 8, 0, 1, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(config_button_clicked), (gpointer)self);

  gtk_grid_attach(GTK_GRID(self->widget), GTK_WIDGET(grid), 0, 1, 1, 1);
  gtk_widget_set_hexpand(GTK_WIDGET(grid), TRUE);

  /* lets signup for mouse over image change signals */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(mouse_over_image_callback), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  const dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(mouse_over_image_callback), self);
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(gtk_bin_get_child(GTK_BIN(d->metadata_cb[i]))));
  }
  free(self->data);
  self->data = NULL;
}

static void add_rights_preset(dt_lib_module_t *self, char *name, char *string)
{
  const unsigned int params_size = strlen(string) + 5;

  char *params = calloc(sizeof(char), params_size);
  memcpy(params + 4, string, params_size - 5);
  dt_lib_presets_add(name, self->plugin_name, self->version(), params, params_size);
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
  add_rights_preset(self, _("all rights reserved"), _("all rights reserved."));
}

void *legacy_params(dt_lib_module_t *self, const void *const old_params, const size_t old_params_size,
                    const int old_version, int *new_version, size_t *new_size)
{
  if(old_version == 1)
  {
    size_t new_params_size = old_params_size;
    void *new_params = calloc(sizeof(char), new_params_size);

    char *buf = (char *)old_params;

    // <title>\0<description>\0<rights>\0<creator>\0<publisher>
    char *metadata[DT_METADATA_NUMBER];
    int32_t metadata_len[DT_METADATA_NUMBER];
    for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
    {
      metadata[i] = buf;
      if(!metadata[i]) return NULL;
      metadata_len[i] = strlen(metadata[i]) + 1;
      buf += metadata_len[i];
    }

    // <creator>\0<publisher>\0<title>\0<description>\0<rights>
    int pos = 0;
    memcpy(new_params + pos, metadata[3], metadata_len[3]);
    pos += metadata_len[3];
    memcpy(new_params + pos, metadata[4], metadata_len[4]);
    pos += metadata_len[4];
    memcpy(new_params + pos, metadata[0], metadata_len[0]);
    pos += metadata_len[0];
    memcpy(new_params + pos, metadata[1], metadata_len[1]);
    pos += metadata_len[1];
    memcpy(new_params + pos, metadata[2], metadata_len[2]);
    pos += metadata_len[2];

    *new_size = new_params_size;
    *new_version = 2;
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
    metadata[keyid] = g_strdup(gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(d->metadata_cb[i])))));
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

  dt_metadata_set_list(-1, key_value, TRUE, TRUE);

  g_list_free(key_value);

  dt_image_synch_xmp(-1);
  update(self, FALSE);
  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
