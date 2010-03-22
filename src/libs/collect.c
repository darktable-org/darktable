/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#include "common/film.h"
#include "control/conf.h"
#include "control/control.h"
#include "libs/lib.h"

DT_MODULE(1)

typedef struct dt_lib_collect_t
{
  GtkComboBox *combo;
  GtkComboBoxEntry *text;
  GtkTreeView *view;
  GtkScrolledWindow *scrolledwindow;
}
dt_lib_collect_t;

typedef enum dt_lib_collect_cols_t
{
  DT_LIB_COLLECT_COL_TEXT=0,
  DT_LIB_COLLECT_COL_ID,
  DT_LIB_COLLECT_NUM_COLS
}
dt_lib_collect_cols_t;

const char*
name ()
{
  return _("collect images");
}

void
gui_reset (dt_lib_module_t *self)
{
  int last_film = dt_conf_get_int ("ui_last/film_roll");
  dt_film_open(last_film);
}

static void
update_query(dt_lib_collect_t *d)
{
  char query[1024];
  int imgsel = -666;
  if(gtk_combo_box_get_active(GTK_COMBO_BOX(d->text)) != -1)
    DT_CTL_GET_GLOBAL(imgsel, lib_image_mouse_over_id);

  // film roll, camera, tag, day, history
  int property = gtk_combo_box_get_active(d->combo);
  gchar *text = gtk_combo_box_get_active_text(GTK_COMBO_BOX(d->text));
  switch(property)
  {
    case 0: // film roll
      if(imgsel == -666)
        snprintf(query, 1024, "select * from images where film_id in (select id from film_rolls where folder like '%%%s%%')", text);
      else if(imgsel > 0)
        snprintf(query, 1024, "select * from images where film_id in (select id from film_rolls where folder in "
                              "(select folder from film_rolls where id = (select film_id from images where id = %d)))", imgsel);
      else
        snprintf(query, 1024, "select * from images where film_id in (select id from film_rolls where id in "
                              "(select film_id from images as a join selected_images as b on a.id = b.imgid))");
      break;
      
    case 4: // history
      snprintf(query, 1024, "select * from images where id %s in (select imgid from history where imgid=images.id) ",(strcmp(text,_("altered"))==0)?"":"not");
    break;
      
    case 1: // camera
      if(imgsel == -666)
        snprintf(query, 1024, "select * from images where maker || ' ' || model like '%%%s%%'", text);
      else if(imgsel > 0)
        snprintf(query, 1024, "select * from images where maker || ' ' || model in "
                              "(select maker || ' ' || model from images where id = %d)", imgsel);
      else
        snprintf(query, 1024, "select * from images where maker || ' ' || model in "
                              "(select maker || ' ' || model from images as a join selected_images as b on a.id = b.imgid)");
      break;
    case 2: // tag
      if(imgsel == -666)
        snprintf(query, 1024, "select * from images where id in (select imgid from tagged_images as a join "
                              "tags as b on a.tagid = b.id where name like '%%%s%%')", text);
      else if(imgsel > 0)
        snprintf(query, 1024, "select * from images where id in "
                              "(select imgid from tagged_images as a join tags as b on a.tagid = b.id where "
                              "b.id in (select tagid from tagged_images where imgid = %d))", imgsel);
      else
        snprintf(query, 1024, "select * from images where id in "
                              "(select imgid from tagged_images as a join tags as b on a.tagid = b.id where "
                              "b.id in (select tagid from tagged_images as c join selected_images as d on c.imgid = d.imgid))");
      break;
 

    default: // case 3: // day
      if(imgsel == -666)
        snprintf(query, 1024, "select * from images where datetime_taken like '%%%s%%'", text);
      else if(imgsel > 0)
        snprintf(query, 1024, "select * from images where datetime_taken in (select datetime_taken from images where id = %d)", imgsel);
      else
        snprintf(query, 1024, "select * from images where datetime_taken in (select datetime_taken from images as a join selected_images as b on a.id = b.imgid");
      break;
  }
  g_free(text);
  // TODO: similar tagxtag query!
  dt_lib_sort_t   sort   = dt_conf_get_int ("ui_last/combo_sort");
  dt_lib_filter_t filter = dt_conf_get_int ("ui_last/combo_filter");
  char *sortstring[4] = {"datetime_taken", "flags & 7 desc", "filename", "id"};
  int sortindex = 3;
  if     (sort == DT_LIB_SORT_DATETIME) sortindex = 0;
  else if(sort == DT_LIB_SORT_RATING)   sortindex = 1;
  else if(sort == DT_LIB_SORT_FILENAME) sortindex = 2;
  // else (sort == DT_LIB_SORT_ID)

  if(filter == DT_LIB_FILTER_STAR_NO)
    sprintf(query+strlen(query), " and (flags & 7) < 1 order by %s limit ?1, ?2", sortstring[sortindex]);
  else
    sprintf(query+strlen(query), " and (flags & 7) >= %d order by %s limit ?1, ?2", filter-1, sortstring[sortindex]);
  // printf("query `%s'\n", query);
  dt_conf_set_string ("plugins/lighttable/query", query);
  dt_control_queue_draw_all();
}

static gboolean
entry_key_press (GtkEntry *entry, GdkEventKey *event, dt_lib_collect_t *d)
{ // update related list
  sqlite3_stmt *stmt;
  GtkTreeIter iter;
  GtkTreeView *view = d->view;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
  g_object_ref(model);
  gtk_tree_view_set_model(GTK_TREE_VIEW(view), NULL);
  gtk_list_store_clear(GTK_LIST_STORE(model));
  char query[1024];
  int property = gtk_combo_box_get_active(d->combo);
  gchar *text = gtk_combo_box_get_active_text(GTK_COMBO_BOX(d->text));
  dt_conf_set_string("plugins/lighttable/collect/string", text);
  dt_conf_set_int ("plugins/lighttable/collect/item", property);
  switch(property)
  {
    case 0: // film roll
      snprintf(query, 1024, "select distinct folder, id from film_rolls where folder like '%%%s%%'", text);
      break;
    case 1: // camera
      snprintf(query, 1024, "select distinct maker || ' ' || model, 1 from images where maker || ' ' || model like '%%%s%%'", text);
      break;
    case 2: // tag
      snprintf(query, 1024, "select distinct name, id from tags where name like '%%%s%%'", text);
      break;
    case 4: // History, 2 hardcoded alternatives
      gtk_list_store_append(GTK_LIST_STORE(model), &iter);
      gtk_list_store_set (GTK_LIST_STORE(model), &iter,
        DT_LIB_COLLECT_COL_TEXT,_("altered"),
        DT_LIB_COLLECT_COL_ID, 0,
        -1);
      gtk_list_store_append(GTK_LIST_STORE(model), &iter);
      gtk_list_store_set (GTK_LIST_STORE(model), &iter,
        DT_LIB_COLLECT_COL_TEXT,_("not altered"),
        DT_LIB_COLLECT_COL_ID, 1,
        -1);
      goto entry_key_press_exit;
    break;
    default: // case 3: // day
      snprintf(query, 1024, "select distinct datetime_taken, 1 from images where datetime_taken like '%%%s%%'", text);
      break;
  }
  g_free(text);
  sqlite3_prepare_v2(darktable.db, query, -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    gtk_list_store_append(GTK_LIST_STORE(model), &iter);
    const char *folder = (const char*)sqlite3_column_text(stmt, 0);
    if(property == 0) // film roll
    {
      if(!strcmp("single images", folder)) folder = _("single images");
      else
      {
        const char *trunc = folder + strlen(folder);
        for(;*trunc != '/' && trunc > folder;trunc--);
        if(trunc != folder) trunc++;
        folder = trunc;
      }
    }
    gtk_list_store_set (GTK_LIST_STORE(model), &iter,
                        DT_LIB_COLLECT_COL_TEXT, folder,
                        DT_LIB_COLLECT_COL_ID, sqlite3_column_int(stmt, 1),
                        -1);
  }
  sqlite3_finalize(stmt);
entry_key_press_exit:
  gtk_tree_view_set_model(GTK_TREE_VIEW(view), model);
  g_object_unref(model);
  update_query(d);
  return FALSE;
}

static void
combo_changed (GtkComboBox *combo, dt_lib_collect_t *d)
{
  gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(d->text))), "");
  entry_key_press (NULL, NULL, d);
}

static void
combo_entry_changed (GtkComboBox *combo, dt_lib_collect_t *d)
{
  int active = gtk_combo_box_get_active(combo);
  gtk_widget_set_visible(GTK_WIDGET(d->scrolledwindow), active);
  if(active) gtk_widget_show_all(GTK_WIDGET(d->scrolledwindow));
  entry_key_press (NULL, NULL, d);
}

static void
row_activated (GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *col, dt_lib_collect_t *d)
{
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;
  gchar *text;
  gtk_tree_model_get (model, &iter, 
                      DT_LIB_COLLECT_COL_TEXT, &text,
                      -1);
  gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(d->text))), text);
  entry_key_press (NULL, NULL, d);
  g_free(text);
}

void
gui_init (dt_lib_module_t *self)
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)malloc(sizeof(dt_lib_collect_t));
  self->data = (void *)d;
  self->widget = gtk_vbox_new(FALSE, 5);
  GtkBox *box;
  GtkWidget *w;
  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  GtkTreeView *view = GTK_TREE_VIEW(gtk_tree_view_new());
  d->view = view;
  GtkListStore *liststore;

  box = GTK_BOX(gtk_hbox_new(FALSE, 5));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(box), TRUE, TRUE, 0);
  w = gtk_combo_box_new_text();
  d->combo = GTK_COMBO_BOX(w);
  gtk_combo_box_append_text(GTK_COMBO_BOX(w), _("film roll"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(w), _("camera"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(w), _("tag"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(w), _("date"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(w), _("history"));
  gtk_combo_box_set_active(GTK_COMBO_BOX(w), dt_conf_get_int("plugins/lighttable/collect/item"));
  g_signal_connect(G_OBJECT(w), "changed", G_CALLBACK(combo_changed), d);
  gtk_box_pack_start(box, w, FALSE, FALSE, 0);
  w = gtk_combo_box_entry_new_text();
  d->text = GTK_COMBO_BOX_ENTRY(w);
  gchar *text = dt_conf_get_string("plugins/lighttable/collect/string");;
  if(text)
  {
    gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(d->text))), text);
    g_free(text);
  }
  d->scrolledwindow = GTK_SCROLLED_WINDOW(sw);
  gtk_combo_box_append_text(GTK_COMBO_BOX(w), _("matches selected images"));
  gtk_combo_box_set_active(GTK_COMBO_BOX(w), -1);
  g_signal_connect(G_OBJECT(w), "changed", G_CALLBACK(combo_entry_changed), d);
  gtk_widget_set_events(w, GDK_KEY_PRESS_MASK);
  g_signal_connect(G_OBJECT(gtk_bin_get_child(GTK_BIN(w))), "key-release-event", G_CALLBACK(entry_key_press), d);
  gtk_box_pack_start(box, w, TRUE, TRUE, 0);

  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_container_add(GTK_CONTAINER(sw), GTK_WIDGET(view));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(sw), TRUE, TRUE, 0);
  gtk_tree_view_set_headers_visible(view, FALSE);
  liststore = gtk_list_store_new(DT_LIB_COLLECT_NUM_COLS, G_TYPE_STRING, G_TYPE_UINT);
  GtkTreeViewColumn *col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(view, col);
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_add_attribute(col, renderer, "text", DT_LIB_COLLECT_COL_TEXT);
  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(view), GTK_SELECTION_SINGLE);
  gtk_tree_view_set_model(view, GTK_TREE_MODEL(liststore));
  gtk_object_set(GTK_OBJECT(view), "tooltip-text", _("doubleclick to select"), NULL);
  g_signal_connect(G_OBJECT (view), "row-activated", G_CALLBACK (row_activated), d);
  entry_key_press (NULL, NULL, d);
}

void
gui_cleanup (dt_lib_module_t *self)
{
  free(self->data);
  self->data = NULL;
}

