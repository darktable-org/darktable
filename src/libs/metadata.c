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

#include "common/darktable.h"
#include "common/metadata.h"
#include "common/debug.h"
#include "common/collection.h"
#include "control/control.h"
#include "control/signal.h"
#include "control/conf.h"
#include "libs/lib.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "dtgtk/button.h"

#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

typedef struct dt_lib_metadata_t
{
  int imgsel;
  GtkComboBox *title;
  GtkComboBox *description;
  GtkComboBox *creator;
  GtkComboBox *publisher;
  GtkComboBox *rights;
  gboolean multi_title;
  gboolean multi_description;
  gboolean multi_creator;
  gboolean multi_publisher;
  gboolean multi_rights;
  gboolean editing;
  GtkWidget *clear_button;
  GtkWidget *apply_button;
} dt_lib_metadata_t;

const char *name()
{
  return _("metadata editor");
}

uint32_t views()
{
  return DT_VIEW_LIGHTTABLE | DT_VIEW_TETHERING;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

static void fill_combo_box_entry(GtkComboBox **box, uint32_t count, GList **items, gboolean *multi)
{
  GList *iter;

  // FIXME: use gtk_combo_box_text_remove_all() in future (gtk 3.0)
  // https://bugzilla.gnome.org/show_bug.cgi?id=324899
  gtk_list_store_clear(GTK_LIST_STORE(gtk_combo_box_get_model(GTK_COMBO_BOX(*box))));

  // FIXME: how to make a nice empty combo box without the append/remove?
  if(count == 0)
  {
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(*box), "");
    gtk_combo_box_set_active(GTK_COMBO_BOX(*box), 0);
    gtk_combo_box_text_remove(GTK_COMBO_BOX_TEXT(*box), 0);

    *multi = FALSE;
    return;
  }

  if(count > 1)
  {
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(*box),
                                   _("<leave unchanged>")); // FIXME: should be italic!
    gtk_combo_box_set_button_sensitivity(GTK_COMBO_BOX(*box), GTK_SENSITIVITY_AUTO);
    *multi = TRUE;
  }
  else
  {
    gtk_combo_box_set_button_sensitivity(GTK_COMBO_BOX(*box), GTK_SENSITIVITY_OFF);
    *multi = FALSE;
  }
  if((iter = g_list_first(*items)) != NULL)
  {
    do
    {
      gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(*box), iter->data); // FIXME: dt segfaults when there
                                                                            // are illegal characters in the
                                                                            // string.
    } while((iter = g_list_next(iter)) != NULL);
  }
  gtk_combo_box_set_active(GTK_COMBO_BOX(*box), 0);
}

static void update(dt_lib_module_t *user_data, gboolean early_bark_out)
{
  //   early_bark_out = FALSE; // FIXME: when barking out early we don't update on ctrl-a/ctrl-shift-a. but
  //   otherwise it's impossible to edit text
  const dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;
  const int imgsel = dt_control_get_mouse_over_id();
  if(early_bark_out && imgsel == d->imgsel) return;

  d->imgsel = imgsel;

  sqlite3_stmt *stmt;

  GList *title = NULL;
  uint32_t title_count = 0;
  GList *description = NULL;
  uint32_t description_count = 0;
  GList *creator = NULL;
  uint32_t creator_count = 0;
  GList *publisher = NULL;
  uint32_t publisher_count = 0;
  GList *rights = NULL;
  uint32_t rights_count = 0;

  // using dt_metadata_get() is not possible here. we want to do all this in a single pass, everything else
  // takes ages.
  if(imgsel < 0) // selected images
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select key, value from meta_data where id in "
                                                               "(select imgid from selected_images) group by "
                                                               "key, value order by value",
                                -1, &stmt, NULL);
  }
  else // single image under mouse cursor
  {
    char query[1024];
    snprintf(query, sizeof(query),
             "select key, value from meta_data where id = %d group by key, value order by value", imgsel);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  }
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    char *value = g_strdup((char *)sqlite3_column_text(stmt, 1));
    if(sqlite3_column_bytes(stmt, 1))
    {
      switch(sqlite3_column_int(stmt, 0))
      {
        case DT_METADATA_XMP_DC_CREATOR:
          creator_count++;
          creator = g_list_append(creator, value);
          break;
        case DT_METADATA_XMP_DC_PUBLISHER:
          publisher_count++;
          publisher = g_list_append(publisher, value);
          break;
        case DT_METADATA_XMP_DC_TITLE:
          title_count++;
          title = g_list_append(title, value);
          break;
        case DT_METADATA_XMP_DC_DESCRIPTION:
          description_count++;
          description = g_list_append(description, value);
          break;
        case DT_METADATA_XMP_DC_RIGHTS:
          rights_count++;
          rights = g_list_append(rights, value);
          break;
      }
    }
  }
  sqlite3_finalize(stmt);

  fill_combo_box_entry(&(d->title), title_count, &title, &(d->multi_title));
  fill_combo_box_entry(&(d->description), description_count, &description, &(d->multi_description));
  fill_combo_box_entry(&(d->rights), rights_count, &rights, &(d->multi_rights));
  fill_combo_box_entry(&(d->creator), creator_count, &creator, &(d->multi_creator));
  fill_combo_box_entry(&(d->publisher), publisher_count, &publisher, &(d->multi_publisher));

  g_list_free(g_list_first(title));
  g_list_free(g_list_first(description));
  g_list_free(g_list_first(creator));
  g_list_free(g_list_first(publisher));
  g_list_free(g_list_first(rights));
}


static gboolean draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  if(!dt_control_running()) return FALSE;
  update((dt_lib_module_t *)user_data, TRUE);
  return FALSE;
}

static void clear_button_clicked(GtkButton *button, gpointer user_data)
{
  dt_metadata_clear(-1);
  dt_image_synch_xmp(-1);
  update(user_data, FALSE);
}

static void write_metadata(dt_lib_module_t *self)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;

  int32_t mouse_over_id;

  d->editing = FALSE;

  mouse_over_id = d->imgsel;

  gchar *title = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(d->title));
  gchar *description = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(d->description));
  gchar *rights = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(d->rights));
  gchar *creator = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(d->creator));
  gchar *publisher = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(d->publisher));

  if(title != NULL && (d->multi_title == FALSE || gtk_combo_box_get_active(GTK_COMBO_BOX(d->title)) != 0))
    dt_metadata_set(mouse_over_id, "Xmp.dc.title", title);
  if(description != NULL
     && (d->multi_description == FALSE || gtk_combo_box_get_active(GTK_COMBO_BOX(d->description)) != 0))
    dt_metadata_set(mouse_over_id, "Xmp.dc.description", description);
  if(rights != NULL && (d->multi_rights == FALSE || gtk_combo_box_get_active(GTK_COMBO_BOX(d->rights)) != 0))
    dt_metadata_set(mouse_over_id, "Xmp.dc.rights", rights);
  if(creator != NULL
     && (d->multi_creator == FALSE || gtk_combo_box_get_active(GTK_COMBO_BOX(d->creator)) != 0))
    dt_metadata_set(mouse_over_id, "Xmp.dc.creator", creator);
  if(publisher != NULL
     && (d->multi_publisher == FALSE || gtk_combo_box_get_active(GTK_COMBO_BOX(d->publisher)) != 0))
    dt_metadata_set(mouse_over_id, "Xmp.dc.publisher", publisher);

  g_free(title);
  g_free(description);
  g_free(rights);
  g_free(creator);
  g_free(publisher);

  dt_image_synch_xmp(mouse_over_id);
  update(self, FALSE);
}

static void apply_button_clicked(GtkButton *button, gpointer user_data)
{
  write_metadata(user_data);
}

static gboolean key_pressed(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  const dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;

  switch(event->keyval)
  {
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
      write_metadata(user_data);
      gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
      break;
    case GDK_KEY_Escape:
      update(user_data, FALSE);
      gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
      break;
    case GDK_KEY_Tab:
      write_metadata(user_data);
      break;
    default:
      d->editing = TRUE;
  }
  return FALSE;
}

void gui_reset(dt_lib_module_t *self)
{
  update(self, FALSE);
}

int position()
{
  return 510;
}

static void _mouse_over_image_callback(gpointer instace, gpointer user_data)
{
  const dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  const dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;

  /* lets trigger an expose for a redraw of widget */
  if(d->editing)
  {
    write_metadata(user_data);
    gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
  }
  gtk_widget_queue_draw(GTK_WIDGET(self->widget));
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

void gui_init(dt_lib_module_t *self)
{
  GtkBox *hbox;
  GtkWidget *button;
  GtkWidget *label;
  GtkEntryCompletion *completion;
  int line = 0;

  dt_lib_metadata_t *d = (dt_lib_metadata_t *)calloc(1, sizeof(dt_lib_metadata_t));
  self->data = (void *)d;

  d->imgsel = -1;

  self->widget = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(self->widget), DT_PIXEL_APPLY_DPI(5));
  gtk_grid_set_column_spacing(GTK_GRID(self->widget), DT_PIXEL_APPLY_DPI(10));

  g_signal_connect(self->widget, "draw", G_CALLBACK(draw), self);

  struct
  {
    char *name;
    GtkComboBox **box;
  } entries[] = {
    // clang-format off
    {_("title"), &d->title},
    {_("description"), &d->description},
    {_("creator"), &d->creator},
    {_("publisher"), &d->publisher},
    {_("rights"), &d->rights}
    // clang-format on
  };

  for(line = 0; line < sizeof(entries) / sizeof(entries[0]); line++)
  {
    label = gtk_label_new(entries[line].name);
    g_object_set(G_OBJECT(label), "xalign", 0.0, NULL);

    GtkWidget *combobox = gtk_combo_box_text_new_with_entry();
    *(entries[line].box) = GTK_COMBO_BOX(combobox);

    // gtk_widget_set_hexpand(combobox, TRUE);

    GtkWidget *entry = gtk_bin_get_child(GTK_BIN(combobox));
    dt_gui_key_accel_block_on_focus_connect(entry);
    completion = gtk_entry_completion_new();
    gtk_entry_completion_set_model(completion, gtk_combo_box_get_model(GTK_COMBO_BOX(d->title)));
    gtk_entry_completion_set_text_column(completion, 0);
    gtk_entry_completion_set_inline_completion(completion, TRUE);
    gtk_entry_set_completion(GTK_ENTRY(entry), completion);

    g_signal_connect(entry, "key-press-event", G_CALLBACK(key_pressed), self);

    gtk_entry_set_width_chars(GTK_ENTRY(entry), 0);

    gtk_grid_attach(GTK_GRID(self->widget), label, 0, line, 1, 1);
    gtk_grid_attach_next_to(GTK_GRID(self->widget), combobox, label, GTK_POS_RIGHT, 1, 1);
  }

  g_object_unref(completion);

  // reset/apply buttons
  hbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5));

  button = gtk_button_new_with_label(_("clear"));
  d->clear_button = button;
  gtk_widget_set_hexpand(GTK_WIDGET(button), TRUE);
  g_object_set(G_OBJECT(button), "tooltip-text", _("remove metadata from selected images"), (char *)NULL);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(clear_button_clicked), (gpointer)self);
  gtk_box_pack_start(hbox, button, FALSE, TRUE, 0);

  button = gtk_button_new_with_label(_("apply"));
  d->apply_button = button;
  gtk_widget_set_hexpand(GTK_WIDGET(button), TRUE);
  g_object_set(G_OBJECT(button), "tooltip-text", _("write metadata for selected images"), (char *)NULL);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(apply_button_clicked), (gpointer)self);
  gtk_box_pack_start(hbox, button, FALSE, TRUE, 0);
  gtk_widget_set_margin_top(GTK_WIDGET(hbox), DT_PIXEL_APPLY_DPI(5));

  gtk_grid_attach(GTK_GRID(self->widget), GTK_WIDGET(hbox), 0, line, 2, 1);

  /* lets signup for mouse over image change signals */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(_mouse_over_image_callback), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  const dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_mouse_over_image_callback), self);
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(gtk_bin_get_child(GTK_BIN(d->publisher))));
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(gtk_bin_get_child(GTK_BIN(d->rights))));
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(gtk_bin_get_child(GTK_BIN(d->title))));
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(gtk_bin_get_child(GTK_BIN(d->description))));
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(gtk_bin_get_child(GTK_BIN(d->creator))));
  free(self->data);
  self->data = NULL;
}

static void add_rights_preset(dt_lib_module_t *self, char *name, char *string)
{
  const unsigned int params_size = strlen(string) + 5;

  char *params = calloc(sizeof(char), params_size);
  memcpy(params + 2, string, params_size - 5);
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

void *get_params(dt_lib_module_t *self, int *size)
{
  dt_lib_metadata_t *d = (dt_lib_metadata_t *)self->data;

  const char *title = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(d->title));
  const char *description = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(d->description));
  const char *rights = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(d->rights));
  const char *creator = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(d->creator));
  const char *publisher = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(d->publisher));

  const int32_t title_len = strlen(title) + 1;
  const int32_t description_len = strlen(description) + 1;
  const int32_t rights_len = strlen(rights) + 1;
  const int32_t creator_len = strlen(creator) + 1;
  const int32_t publisher_len = strlen(publisher) + 1;

  *size = title_len + description_len + rights_len + creator_len + publisher_len;

  char *params = (char *)malloc(*size);

  int pos = 0;
  memcpy(params + pos, title, title_len);
  pos += title_len;
  memcpy(params + pos, description, description_len);
  pos += description_len;
  memcpy(params + pos, rights, rights_len);
  pos += rights_len;
  memcpy(params + pos, creator, creator_len);
  pos += creator_len;
  memcpy(params + pos, publisher, publisher_len);
  pos += publisher_len;

  g_assert(pos == *size);

  return params;
}

// WARNING: also change src/libs/import.c when changing this!
int set_params(dt_lib_module_t *self, const void *params, int size)
{
  if(!params) return 1;

  char *buf = (char *)params;

  const char *title = buf;
  if(!title) return 1;
  const int title_len = strlen(title) + 1;

  buf += title_len;
  const char *description = buf;
  if(!description) return 1;
  const int description_len = strlen(description) + 1;

  buf += description_len;
  const char *rights = buf;
  if(!rights) return 1;
  const int rights_len = strlen(rights) + 1;

  buf += rights_len;
  const char *creator = buf;
  if(!creator) return 1;
  const int creator_len = strlen(creator) + 1;

  buf += creator_len;
  const char *publisher = buf;
  if(!publisher) return 1;
  const int publisher_len = strlen(publisher) + 1;

  if(size != title_len + description_len + rights_len + creator_len + publisher_len)
    return 1;

  if(title != NULL && title[0] != '\0') dt_metadata_set(-1, "Xmp.dc.title", title);
  if(description != NULL && description[0] != '\0') dt_metadata_set(-1, "Xmp.dc.description", description);
  if(rights != NULL && rights[0] != '\0') dt_metadata_set(-1, "Xmp.dc.rights", rights);
  if(creator != NULL && creator[0] != '\0') dt_metadata_set(-1, "Xmp.dc.creator", creator);
  if(publisher != NULL && publisher[0] != '\0') dt_metadata_set(-1, "Xmp.dc.publisher", publisher);

  dt_image_synch_xmp(-1);
  update(self, FALSE);
  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
