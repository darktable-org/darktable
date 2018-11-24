/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.
    copyright (c) 2011 henrik andersson.

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
#include "common/tags.h"
#include "control/conf.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "views/view.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#include <gdk/gdkkeysyms.h>
#include <math.h>

#define FLOATING_ENTRY_WIDTH DT_PIXEL_APPLY_DPI(150)

DT_MODULE(1)

static gboolean _lib_tagging_tag_show(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                      GdkModifierType modifier, dt_lib_module_t *self);

typedef struct dt_lib_tagging_t
{
  char keyword[1024];
  GtkEntry *entry;
  GtkTreeView *current, *related;
  int imgsel;

  GtkWidget *attach_button, *detach_button, *new_button, *delete_button, *import_button, *export_button;

  GtkWidget *floating_tag_window;
  int floating_tag_imgid;
} dt_lib_tagging_t;

typedef enum dt_lib_tagging_cols_t
{
  DT_LIB_TAGGING_COL_TAG = 0,
  DT_LIB_TAGGING_COL_ID,
  DT_LIB_TAGGING_NUM_COLS
} dt_lib_tagging_cols_t;

const char *name(dt_lib_module_t *self)
{
  return _("tagging");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v1[] = {"lighttable", "darkroom", "map", "tethering", NULL};
  static const char *v2[] = {"lighttable", "map", "tethering", NULL};

  if(dt_conf_get_bool("plugins/darkroom/tagging/visible"))
    return v1;
  else
    return v2;
}

uint32_t container(dt_lib_module_t *self)
{
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(cv->view((dt_view_t *)cv) == DT_VIEW_DARKROOM)
    return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
  else
    return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "attach"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "detach"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "new"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "delete"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "tag"), GDK_KEY_t, GDK_CONTROL_MASK);
}

void connect_key_accels(dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;

  dt_accel_connect_button_lib(self, "attach", d->attach_button);
  dt_accel_connect_button_lib(self, "detach", d->detach_button);
  dt_accel_connect_button_lib(self, "new", d->new_button);
  dt_accel_connect_button_lib(self, "delete", d->delete_button);
  dt_accel_connect_lib(self, "tag", g_cclosure_new(G_CALLBACK(_lib_tagging_tag_show), self, NULL));
}

static void update(dt_lib_module_t *self, int which)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  GList *tags = NULL;
  uint32_t count;

  if(which == 0) // tags of selected images
  {
    int imgsel = dt_control_get_mouse_over_id();
    d->imgsel = imgsel;
    count = dt_tag_get_attached(imgsel, &tags, FALSE);
  }
  else // related tags of typed text
    count = dt_tag_get_suggestions(d->keyword, &tags);

  GtkTreeIter iter;
  GtkTreeView *view;
  if(which == 0)
    view = d->current;
  else
    view = d->related;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
  g_object_ref(model);
  gtk_tree_view_set_model(GTK_TREE_VIEW(view), NULL);
  gtk_list_store_clear(GTK_LIST_STORE(model));

  if(count > 0 && tags)
  {
    GList *tag = tags;
    do
    {
      gtk_list_store_append(GTK_LIST_STORE(model), &iter);
      gtk_list_store_set(GTK_LIST_STORE(model), &iter, DT_LIB_TAGGING_COL_TAG, ((dt_tag_t *)tag->data)->tag,
                         DT_LIB_TAGGING_COL_ID, ((dt_tag_t *)tag->data)->id, -1);
    } while((tag = g_list_next(tag)) != NULL);
  }

  // Free result...
  dt_tag_free_result(&tags);

  gtk_tree_view_set_model(GTK_TREE_VIEW(view), model);
  g_object_unref(model);
}

static void set_keyword(dt_lib_module_t *self, dt_lib_tagging_t *d)
{
  const gchar *beg = g_strrstr(gtk_entry_get_text(d->entry), ",");
  if(!beg)
    beg = gtk_entry_get_text(d->entry);
  else
  {
    if(*beg == ',') beg++;
    if(*beg == ' ') beg++;
  }
  snprintf(d->keyword, sizeof(d->keyword), "%s", beg);
  update(self, 1);
}

static void attach_selected_tag(dt_lib_module_t *self, dt_lib_tagging_t *d)
{
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeView *view = d->related;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)
     && !gtk_tree_model_get_iter_first(model, &iter))
    return;
  guint tagid;
  gtk_tree_model_get(model, &iter, DT_LIB_TAGGING_COL_ID, &tagid, -1);

  int imgsel = -1;
  if(tagid <= 0) return;

  imgsel = dt_view_get_image_to_act_on();

  dt_tag_attach(tagid, imgsel);
  dt_image_synch_xmp(imgsel);

  dt_control_signal_raise(darktable.signals, DT_SIGNAL_TAG_CHANGED);
}

static void detach_selected_tag(dt_lib_module_t *self, dt_lib_tagging_t *d)
{
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeView *view = d->current;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;
  guint tagid;
  gtk_tree_model_get(model, &iter, DT_LIB_TAGGING_COL_ID, &tagid, -1);

  int imgsel = -1;
  if(tagid <= 0) return;

  imgsel = dt_view_get_image_to_act_on();
  GList *affected_images = dt_tag_get_images_from_selection(imgsel, tagid);

  dt_tag_detach(tagid, imgsel);

  // we have to check the conf option as dt_image_synch_xmp() doesn't when called for a single image
  if(dt_conf_get_bool("write_sidecar_files"))
  {
    for(GList *image_iter = affected_images; image_iter; image_iter = g_list_next(image_iter))
    {
      int imgid = GPOINTER_TO_INT(image_iter->data);
      dt_image_synch_xmp(imgid);
    }
  }

  g_list_free(affected_images);

  dt_control_signal_raise(darktable.signals, DT_SIGNAL_TAG_CHANGED);
}

static void attach_activated(GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *col, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  attach_selected_tag(self, d);
  update(self, 0);
}

static void detach_activated(GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *col, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  detach_selected_tag(self, d);
  update(self, 0);
}

static void attach_button_clicked(GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  attach_selected_tag(self, d);
  update(self, 0);
}

static void detach_button_clicked(GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  detach_selected_tag(self, d);
  update(self, 0);
}

static void new_button_clicked(GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  const gchar *tag = gtk_entry_get_text(d->entry);

  /** attach tag to selected images  */
  dt_tag_attach_string_list(tag, -1);
  dt_image_synch_xmp(-1);

  update(self, 1);
  update(self, 0);

  /** clear input box */
  gtk_entry_set_text(d->entry, "");

  dt_control_signal_raise(darktable.signals, DT_SIGNAL_TAG_CHANGED);
}

static void entry_activated(GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  const gchar *tag = gtk_entry_get_text(d->entry);
  if(!tag || tag[0] == '\0') return;

  /** attach tag to selected images  */
  dt_tag_attach_string_list(tag, -1);
  dt_image_synch_xmp(-1);

  update(self, 1);
  update(self, 0);
  gtk_entry_set_text(d->entry, "");

  dt_control_signal_raise(darktable.signals, DT_SIGNAL_TAG_CHANGED);
}

static void tag_name_changed(GtkEntry *entry, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  set_keyword(self, d);
}

static void delete_button_clicked(GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;

  int res = GTK_RESPONSE_YES;

  guint tagid;
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeView *view = d->related;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;
  gtk_tree_model_get(model, &iter, DT_LIB_TAGGING_COL_ID, &tagid, -1);

  // First check how many images are affected by the remove
  int count = dt_tag_remove(tagid, FALSE);
  if(count > 0 && dt_conf_get_bool("plugins/lighttable/tagging/ask_before_delete_tag"))
  {
    GtkWidget *dialog;
    GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
    gchar *tagname = dt_tag_get_name(tagid);
    dialog = gtk_message_dialog_new(
        GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        ngettext("do you really want to delete the tag `%s'?\n%d image is assigned this tag!",
                 "do you really want to delete the tag `%s'?\n%d images are assigned this tag!", count),
        tagname, count);
    gtk_window_set_title(GTK_WINDOW(dialog), _("delete tag?"));
    res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    free(tagname);
  }
  if(res != GTK_RESPONSE_YES) return;

  GList *tagged_images = NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.tagged_images WHERE tagid=?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    tagged_images = g_list_append(tagged_images, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
  }
  sqlite3_finalize(stmt);

  dt_tag_remove(tagid, TRUE);

  GList *list_iter;
  if((list_iter = g_list_first(tagged_images)) != NULL)
  {
    do
    {
      dt_image_synch_xmp(GPOINTER_TO_INT(list_iter->data));
    } while((list_iter = g_list_next(list_iter)) != NULL);
  }
  g_list_free(g_list_first(tagged_images));

  update(self, 0);
  update(self, 1);

  dt_control_signal_raise(darktable.signals, DT_SIGNAL_TAG_CHANGED);
}

static void import_button_clicked(GtkButton *button, gpointer user_data)
{
  char *last_dirname = dt_conf_get_string("plugins/lighttable/tagging/last_import_export_location");
  if(!last_dirname || !*last_dirname)
  {
    g_free(last_dirname);
    last_dirname = g_strdup(g_get_home_dir());
  }

  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new(_("Select a keyword file"), GTK_WINDOW(win),
                                                       GTK_FILE_CHOOSER_ACTION_OPEN,
                                                       _("_cancel"), GTK_RESPONSE_CANCEL,
                                                       _("_import"), GTK_RESPONSE_ACCEPT, (char *)NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(filechooser);
#endif
  gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), last_dirname);
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);

  if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
    char *dirname = g_path_get_dirname(filename);
    dt_conf_set_string("plugins/lighttable/tagging/last_import_export_location", dirname);
    ssize_t count = dt_tag_import(filename);
    if(count < 0)
      dt_control_log(_("error importing tags"));
    else
      dt_control_log(_("%zd tags imported"), count);
    g_free(filename);
    g_free(dirname);
  }

  g_free(last_dirname);
  gtk_widget_destroy(filechooser);
}

static void export_button_clicked(GtkButton *button, gpointer user_data)
{
  GDateTime *now = g_date_time_new_now_local();
  char *export_filename = g_date_time_format(now, "darktable_tags_%F_%R.txt");
  char *last_dirname = dt_conf_get_string("plugins/lighttable/tagging/last_import_export_location");
  if(!last_dirname || !*last_dirname)
  {
    g_free(last_dirname);
    last_dirname = g_strdup(g_get_home_dir());
  }

  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new(_("Select file to export to"), GTK_WINDOW(win),
                                                       GTK_FILE_CHOOSER_ACTION_SAVE,
                                                       _("_cancel"), GTK_RESPONSE_CANCEL,
                                                       _("_export"), GTK_RESPONSE_ACCEPT, (char *)NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(filechooser);
#endif
  gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(filechooser), TRUE);
  gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), last_dirname);
  gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(filechooser), export_filename);

  if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
    char *dirname = g_path_get_dirname(filename);
    dt_conf_set_string("plugins/lighttable/tagging/last_import_export_location", dirname);
    ssize_t count = dt_tag_export(filename);
    if(count < 0)
      dt_control_log(_("error exporting tags"));
    else
      dt_control_log(_("%zd tags exported"), count);
    g_free(filename);
    g_free(dirname);
  }

  g_date_time_unref(now);
  g_free(last_dirname);
  g_free(export_filename);
  gtk_widget_destroy(filechooser);
}

void gui_reset(dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  // clear entry box and query
  gtk_entry_set_text(d->entry, "");
  set_keyword(self, d);
}

int position()
{
  return 500;
}

static void _lib_tagging_redraw_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  int imgsel = dt_control_get_mouse_over_id();
  if(imgsel != d->imgsel) update(self, 0);
}

static void _lib_tagging_tags_changed_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  update(self, 1);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)malloc(sizeof(dt_lib_tagging_t));
  self->data = (void *)d;
  d->imgsel = -1;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->plugin_name));
  //   gtk_widget_set_size_request(self->widget, DT_PIXEL_APPLY_DPI(100), -1);

  GtkBox *box, *hbox;
  GtkWidget *button;
  GtkWidget *w;
  GtkListStore *liststore;

  // left side, current
  box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 5));

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(box), TRUE, TRUE, 0);
  w = gtk_scrolled_window_new(NULL, NULL);
  gtk_widget_set_size_request(w, -1, DT_PIXEL_APPLY_DPI(100));
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_box_pack_start(box, w, TRUE, TRUE, 0);
  d->current = GTK_TREE_VIEW(gtk_tree_view_new());
  gtk_tree_view_set_headers_visible(d->current, FALSE);
  liststore = gtk_list_store_new(DT_LIB_TAGGING_NUM_COLS, G_TYPE_STRING, G_TYPE_UINT);
  GtkTreeViewColumn *col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(d->current, col);
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_add_attribute(col, renderer, "text", DT_LIB_TAGGING_COL_TAG);
  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(d->current), GTK_SELECTION_SINGLE);
  gtk_tree_view_set_model(d->current, GTK_TREE_MODEL(liststore));
  g_object_unref(liststore);
  gtk_widget_set_tooltip_text(GTK_WIDGET(d->current), _("attached tags,\ndoubleclick to detach"));
  dt_gui_add_help_link(GTK_WIDGET(d->current), "tagging.html#tagging_usage");
  g_signal_connect(G_OBJECT(d->current), "row-activated", G_CALLBACK(detach_activated), (gpointer)self);
  gtk_container_add(GTK_CONTAINER(w), GTK_WIDGET(d->current));

  // attach/detach buttons
  hbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5));

  button = gtk_button_new_with_label(_("attach"));
  d->attach_button = button;
  gtk_widget_set_tooltip_text(button, _("attach tag to all selected images"));
  dt_gui_add_help_link(button, "tagging.html#tagging_usage");
  gtk_box_pack_start(hbox, button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(attach_button_clicked), (gpointer)self);

  button = gtk_button_new_with_label(_("detach"));
  d->detach_button = button;
  gtk_widget_set_tooltip_text(button, _("detach tag from all selected images"));
  dt_gui_add_help_link(button, "tagging.html#tagging_usage");
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(detach_button_clicked), (gpointer)self);
  gtk_box_pack_start(hbox, button, FALSE, TRUE, 0);

  gtk_box_pack_start(box, GTK_WIDGET(hbox), FALSE, TRUE, 0);

  // right side, related
  box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 5));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(box), TRUE, TRUE, 5);

  // text entry and new button
  w = gtk_entry_new();
  gtk_widget_set_tooltip_text(w, _("enter tag name"));
  dt_gui_add_help_link(w, "tagging.html#tagging_usage");
  gtk_box_pack_start(box, w, TRUE, TRUE, 0);
  gtk_widget_add_events(GTK_WIDGET(w), GDK_KEY_RELEASE_MASK);
  // g_signal_connect(G_OBJECT(w), "key-release-event",
  g_signal_connect(G_OBJECT(w), "changed", G_CALLBACK(tag_name_changed), (gpointer)self);
  g_signal_connect(G_OBJECT(w), "activate", G_CALLBACK(entry_activated), (gpointer)self);
  d->entry = GTK_ENTRY(w);
  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(d->entry));

  // related tree view
  w = gtk_scrolled_window_new(NULL, NULL);
  gtk_widget_set_size_request(w, -1, DT_PIXEL_APPLY_DPI(100));
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_box_pack_start(box, w, TRUE, TRUE, 0);
  d->related = GTK_TREE_VIEW(gtk_tree_view_new());
  gtk_tree_view_set_headers_visible(d->related, FALSE);
  liststore = gtk_list_store_new(DT_LIB_TAGGING_NUM_COLS, G_TYPE_STRING, G_TYPE_UINT);
  col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(d->related, col);
  renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_add_attribute(col, renderer, "text", DT_LIB_TAGGING_COL_TAG);
  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(d->related), GTK_SELECTION_SINGLE);
  gtk_tree_view_set_model(d->related, GTK_TREE_MODEL(liststore));
  g_object_unref(liststore);
  gtk_widget_set_tooltip_text(GTK_WIDGET(d->related), _("related tags,\ndoubleclick to attach"));
  dt_gui_add_help_link(GTK_WIDGET(d->related), "tagging.html#tagging_usage");
  g_signal_connect(G_OBJECT(d->related), "row-activated", G_CALLBACK(attach_activated), (gpointer)self);
  gtk_container_add(GTK_CONTAINER(w), GTK_WIDGET(d->related));

  // attach and delete buttons
  hbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5));

  button = gtk_button_new_with_label(_("new"));
  d->new_button = button;
  gtk_widget_set_tooltip_text(button, _("create a new tag with the\nname you entered"));
  dt_gui_add_help_link(button, "tagging.html#tagging_usage");
  gtk_box_pack_start(hbox, button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(new_button_clicked), (gpointer)self);

  button = gtk_button_new_with_label(_("delete"));
  d->delete_button = button;
  gtk_widget_set_tooltip_text(button, _("delete selected tag"));
  dt_gui_add_help_link(button, "tagging.html#tagging_usage");
  gtk_box_pack_start(hbox, button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(delete_button_clicked), (gpointer)self);

  button = gtk_button_new_with_label(C_("verb", "import"));
  d->import_button = button;
  gtk_widget_set_tooltip_text(button, _("import tags from a Lightroom keyword file"));
  dt_gui_add_help_link(button, "tagging.html#tagging_usage");
  gtk_box_pack_start(hbox, button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(import_button_clicked), (gpointer)self);

  button = gtk_button_new_with_label(C_("verb", "export"));
  d->export_button = button;
  gtk_widget_set_tooltip_text(button, _("export all tags to a Lightroom keyword file"));
  dt_gui_add_help_link(button, "tagging.html#tagging_usage");
  gtk_box_pack_start(hbox, button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(export_button_clicked), (gpointer)self);

  gtk_box_pack_start(box, GTK_WIDGET(hbox), FALSE, TRUE, 0);

  // add entry completion
  GtkEntryCompletion *completion = gtk_entry_completion_new();
  gtk_entry_completion_set_model(completion, gtk_tree_view_get_model(GTK_TREE_VIEW(d->related)));
  gtk_entry_completion_set_text_column(completion, 0);
  gtk_entry_completion_set_inline_completion(completion, TRUE);
  gtk_entry_set_completion(d->entry, completion);

  /* connect to mouse over id */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(_lib_tagging_redraw_callback), self);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_TAG_CHANGED,
                            G_CALLBACK(_lib_tagging_tags_changed_callback), self);

  update(self, 0);
  set_keyword(self, d);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(d->entry));
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_lib_tagging_redraw_callback), self);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_lib_tagging_tags_changed_callback), self);
  free(self->data);
  self->data = NULL;
}

// http://stackoverflow.com/questions/4631388/transparent-floating-gtkentry
static gboolean _lib_tagging_tag_key_press(GtkWidget *entry, GdkEventKey *event, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  switch(event->keyval)
  {
    case GDK_KEY_Escape:
      gtk_widget_destroy(d->floating_tag_window);
      return TRUE;
    case GDK_KEY_Tab:
      return TRUE;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    {
      const gchar *tag = gtk_entry_get_text(GTK_ENTRY(entry));
      // both these functions can deal with -1 for all selected images. no need for extra code in here!
      dt_tag_attach_string_list(tag, d->floating_tag_imgid);
      dt_image_synch_xmp(d->floating_tag_imgid);
      update(self, 1);
      update(self, 0);
      gtk_widget_destroy(d->floating_tag_window);
      dt_control_signal_raise(darktable.signals, DT_SIGNAL_TAG_CHANGED);
      return TRUE;
    }
  }
  return FALSE; /* event not handled */
}

static gboolean _lib_tagging_tag_destroy(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  gtk_widget_destroy(GTK_WIDGET(user_data));
  return FALSE;
}

static gboolean _match_selected_func(GtkEntryCompletion *completion, GtkTreeModel *model, GtkTreeIter *iter, gpointer user_data)
{
  char *tag = NULL;
  int column = gtk_entry_completion_get_text_column(completion);

  if(gtk_tree_model_get_column_type(model, column) != G_TYPE_STRING) return TRUE;

  GtkEditable *e = (GtkEditable *)gtk_entry_completion_get_entry(completion);
  if(!GTK_IS_EDITABLE(e))
  {
    return FALSE;
  }

  gtk_tree_model_get(model, iter, column, &tag, -1);
  
  gint cut_off, cur_pos = gtk_editable_get_position(e);

  gchar *currentText = gtk_editable_get_chars(e, 0, -1);
  const gchar *lastTag = g_strrstr(currentText, ",");
  if(lastTag == NULL)
  {
    cut_off = 0;
  }
  else
  {
    cut_off = (int)(g_utf8_strlen(currentText, -1) - g_utf8_strlen(lastTag, -1))+1;
  }
  free(currentText);

  gtk_editable_delete_text(e, cut_off, cur_pos);
  cur_pos = cut_off;
  gtk_editable_insert_text(e, tag, -1, &cur_pos);
  gtk_editable_set_position(e, cur_pos);
  return TRUE;
}      

static gboolean _completion_match_func(GtkEntryCompletion *completion, const gchar *key, GtkTreeIter *iter,
                                       gpointer user_data)
{
  gboolean res = FALSE;

  GtkEditable *e = (GtkEditable *)gtk_entry_completion_get_entry(completion);

  if(!GTK_IS_EDITABLE(e))
  {
    return FALSE;
  }
  
  gint cur_pos = gtk_editable_get_position(e);
  gboolean onLastTag = (g_strstr_len(&key[cur_pos], -1, ",") == NULL);
  if(!onLastTag)
  {
    return FALSE;
  }
  
  
  char *tag = NULL;
  GtkTreeModel *model = gtk_entry_completion_get_model(completion);
  int column = gtk_entry_completion_get_text_column(completion);

  if(gtk_tree_model_get_column_type(model, column) != G_TYPE_STRING) return FALSE;

  gtk_tree_model_get(model, iter, column, &tag, -1);

  const gchar *lastTag = g_strrstr(key, ",");
  if(lastTag != NULL)
  {
    lastTag++;
  }
  else
  {
    lastTag = key;
  }
  if(lastTag[0] == '\0' && key[0] != '\0')
  {
    return FALSE;
  }

  if(tag)
  {
    char *normalized = g_utf8_normalize(tag, -1, G_NORMALIZE_ALL);
    if(normalized)
    {
      char *casefold = g_utf8_casefold(normalized, -1);
      if(casefold)
      {
        res = g_strstr_len(casefold, -1, lastTag) != NULL;
      }
      g_free(casefold);
    }
    g_free(normalized);
    g_free(tag);
  }

  return res;
}

static gboolean _lib_tagging_tag_show(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                      GdkModifierType modifier, dt_lib_module_t *self)
{
  int mouse_over_id = -1;
  int zoom = dt_conf_get_int("plugins/lighttable/images_in_row");

  // the order is:
  // if(zoom == 1) => currently shown image
  // else if(selection not empty) => selected images
  // else if(cursor over image) => hovered image
  // else => return
  if(zoom == 1 || dt_collection_get_selected_count(darktable.collection) == 0)
  {
    mouse_over_id = dt_control_get_mouse_over_id();
    if(mouse_over_id < 0) return TRUE;
  }

  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  d->floating_tag_imgid = mouse_over_id;

  gint x, y;
  gint px, py, w, h;
  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *center = dt_ui_center(darktable.gui->ui);
  gdk_window_get_origin(gtk_widget_get_window(center), &px, &py);

  w = gdk_window_get_width(gtk_widget_get_window(center));
  h = gdk_window_get_height(gtk_widget_get_window(center));

  x = px + 0.5 * (w - FLOATING_ENTRY_WIDTH);
  y = py + h - 50;

  /* put the floating box at the mouse pointer */
  //   gint pointerx, pointery;
  //   GdkDevice *device =
  //   gdk_device_manager_get_client_pointer(gdk_display_get_device_manager(gtk_widget_get_display(widget)));
  //   gdk_window_get_device_position (gtk_widget_get_window (widget), device, &pointerx, &pointery, NULL);
  //   x = px + pointerx + 1;
  //   y = py + pointery + 1;

  d->floating_tag_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(d->floating_tag_window);
#endif
  /* stackoverflow.com/questions/1925568/how-to-give-keyboard-focus-to-a-pop-up-gtk-window */
  gtk_widget_set_can_focus(d->floating_tag_window, TRUE);
  gtk_window_set_decorated(GTK_WINDOW(d->floating_tag_window), FALSE);
  gtk_window_set_type_hint(GTK_WINDOW(d->floating_tag_window), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
  gtk_window_set_transient_for(GTK_WINDOW(d->floating_tag_window), GTK_WINDOW(window));
  gtk_widget_set_opacity(d->floating_tag_window, 0.8);
  gtk_window_move(GTK_WINDOW(d->floating_tag_window), x, y);


  GtkWidget *entry = gtk_entry_new();
  gtk_widget_set_size_request(entry, FLOATING_ENTRY_WIDTH, -1);
  gtk_widget_add_events(entry, GDK_FOCUS_CHANGE_MASK);

  GtkEntryCompletion *completion = gtk_entry_completion_new();
  gtk_entry_completion_set_model(completion, gtk_tree_view_get_model(GTK_TREE_VIEW(d->related)));
  gtk_entry_completion_set_text_column(completion, 0);
  gtk_entry_completion_set_inline_completion(completion, TRUE);
  gtk_entry_completion_set_popup_set_width(completion, FALSE);
  g_signal_connect(G_OBJECT(completion), "match-selected", G_CALLBACK(_match_selected_func), self);
  gtk_entry_completion_set_match_func(completion, _completion_match_func, NULL, NULL);
  gtk_entry_set_completion(GTK_ENTRY(entry), completion);

  gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
  gtk_container_add(GTK_CONTAINER(d->floating_tag_window), entry);
  g_signal_connect(entry, "focus-out-event", G_CALLBACK(_lib_tagging_tag_destroy), d->floating_tag_window);
  g_signal_connect(entry, "key-press-event", G_CALLBACK(_lib_tagging_tag_key_press), self);

  gtk_widget_show_all(d->floating_tag_window);
  gtk_widget_grab_focus(entry);
  gtk_window_present(GTK_WINDOW(d->floating_tag_window));

  return TRUE;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
