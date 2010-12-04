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
#include "common/collection.h"
#include "control/conf.h"
#include "control/control.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "common/metadata.h"
#include "common/utility.h"

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

uint32_t views() 
{
  return DT_LIGHTTABLE_VIEW;
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
  gchar *escaped_text = dt_util_str_escape(text, "'", "''");
  
  switch(property)
  {
    case 0: // film roll
      if(imgsel == -666)
        snprintf(query, 1024, "(film_id in (select id from film_rolls where folder like '%%%s%%'))", escaped_text);
      else if(imgsel > 0)
        snprintf(query, 1024, "(film_id in (select id from film_rolls where folder in "
                              "(select folder from film_rolls where id = (select film_id from images where id = %d))))", imgsel);
      else
        snprintf(query, 1024, "(film_id in (select id from film_rolls where id in "
                              "(select film_id from images as a join selected_images as b on a.id = b.imgid)))");
      break;

    case 5: // colorlabel
    {
      int color = 0;
      if(strcmp(text,_("red"))==0) color=0;
      else if(strcmp(text,_("yellow"))==0) color=1;
      else if(strcmp(text,_("green"))==0) color=2;
      snprintf(query, 1024, "(id in (select imgid from color_labels where color=%d))", color);
    } break;
    
    case 4: // history
      snprintf(query, 1024, "(id %s in (select imgid from history where imgid=images.id)) ",(strcmp(text,_("altered"))==0)?"":"not");
    break;
      
    case 1: // camera
      if(imgsel == -666)
        snprintf(query, 1024, "(maker || ' ' || model like '%%%s%%')", escaped_text);
      else if(imgsel > 0)
        snprintf(query, 1024, "(maker || ' ' || model in "
                              "(select maker || ' ' || model from images where id = %d))", imgsel);
      else
        snprintf(query, 1024, "(maker || ' ' || model in "
                              "(select maker || ' ' || model from images as a join selected_images as b on a.id = b.imgid))");
      break;
    case 2: // tag
      if(imgsel == -666)
        snprintf(query, 1024, "(id in (select imgid from tagged_images as a join "
                              "tags as b on a.tagid = b.id where name like '%%%s%%'))", escaped_text);
      else if(imgsel > 0)
        snprintf(query, 1024, "(id in "
                              "(select imgid from tagged_images as a join tags as b on a.tagid = b.id where "
                              "b.id in (select tagid from tagged_images where imgid = %d)))", imgsel);
      else
        snprintf(query, 1024, "(id in "
                              "(select imgid from tagged_images as a join tags as b on a.tagid = b.id where "
                              "b.id in (select tagid from tagged_images as c join selected_images as d on c.imgid = d.imgid)))");
      break;

    // TODO: How to handle images without metadata? In the moment they are not shown.
    // TODO: Autogenerate this code?
    case 6: // title
        if(imgsel == -666)
            snprintf(query, 1024, "(id in (select id from meta_data where key = %d and value like '%%%s%%'))",
                                  DT_METADATA_XMP_DC_TITLE, escaped_text);
        else if(imgsel > 0)
            snprintf(query, 1024, "(id in"
                                  "(select id from meta_data where key = %d and"
                                  "value in (select value from meta_data where id = %d and key = %d)))",
                                  DT_METADATA_XMP_DC_TITLE, imgsel, DT_METADATA_XMP_DC_TITLE);
        else
            snprintf(query, 1024, "(id in"
                                  "(select id from meta_data as a join selected_images as b on a.id = b.imgid where a.key = %d))",
                                  DT_METADATA_XMP_DC_TITLE);
        break;
    case 7: // description
        if(imgsel == -666)
            snprintf(query, 1024, "(id in (select id from meta_data where key = %d and value like '%%%s%%'))",
                                  DT_METADATA_XMP_DC_DESCRIPTION, escaped_text);
        else if(imgsel > 0)
            snprintf(query, 1024, "(id in"
                                  "(select id from meta_data where key = %d and"
                                  "value in (select value from meta_data where id = %d and key = %d)))",
                                  DT_METADATA_XMP_DC_DESCRIPTION, imgsel, DT_METADATA_XMP_DC_DESCRIPTION);
        else
            snprintf(query, 1024, "(id in"
                                  "(select id from meta_data as a join selected_images as b on a.id = b.imgid where a.key = %d))",
                                  DT_METADATA_XMP_DC_DESCRIPTION);
        break;
    case 8: // creator
        if(imgsel == -666)
            snprintf(query, 1024, "(id in (select id from meta_data where key = %d and value like '%%%s%%'))",
                                  DT_METADATA_XMP_DC_CREATOR, escaped_text);
        else if(imgsel > 0)
            snprintf(query, 1024, "(id in"
                                  "(select id from meta_data where key = %d and"
                                  "value in (select value from meta_data where id = %d and key = %d)))",
                                  DT_METADATA_XMP_DC_CREATOR, imgsel, DT_METADATA_XMP_DC_CREATOR);
        else
            snprintf(query, 1024, "(id in"
                                  "(select id from meta_data as a join selected_images as b on a.id = b.imgid where a.key = %d))",
                                  DT_METADATA_XMP_DC_CREATOR);
        break;
    case 9: // publisher
        if(imgsel == -666)
            snprintf(query, 1024, "(id in (select id from meta_data where key = %d and value like '%%%s%%'))",
                                  DT_METADATA_XMP_DC_PUBLISHER, escaped_text);
        else if(imgsel > 0)
            snprintf(query, 1024, "(id in"
                                  "(select id from meta_data where key = %d and"
                                  "value in (select value from meta_data where id = %d and key = %d)))",
                                  DT_METADATA_XMP_DC_PUBLISHER, imgsel, DT_METADATA_XMP_DC_PUBLISHER);
        else
            snprintf(query, 1024, "(id in"
                                  "(select id from meta_data as a join selected_images as b on a.id = b.imgid where a.key = %d))",
                                  DT_METADATA_XMP_DC_PUBLISHER);
        break;
    case 10: // rights
        if(imgsel == -666)
            snprintf(query, 1024, "(id in (select id from meta_data where key = %d and value like '%%%s%%'))",
                                  DT_METADATA_XMP_DC_RIGHTS, escaped_text);
        else if(imgsel > 0)
            snprintf(query, 1024, "(id in"
                                  "(select id from meta_data where key = %d and"
                                  "value in (select value from meta_data where id = %d and key = %d)))",
                                  DT_METADATA_XMP_DC_RIGHTS, imgsel, DT_METADATA_XMP_DC_RIGHTS);
        else
            snprintf(query, 1024, "(id in"
                                  "(select id from meta_data as a join selected_images as b on a.id = b.imgid where a.key = %d))",
                                  DT_METADATA_XMP_DC_RIGHTS);
        break;

    default: // case 3: // day
      if(imgsel == -666)
        snprintf(query, 1024, "(datetime_taken like '%%%s%%')", escaped_text);
      else if(imgsel > 0)
        snprintf(query, 1024, "(datetime_taken in (select datetime_taken from images where id = %d))", imgsel);
      else
        snprintf(query, 1024, "(datetime_taken in (select datetime_taken from images as a join selected_images as b on a.id = b.imgid)");
      break;
  }
  g_free(text);
  g_free(escaped_text);
  
  /* set the extended where and the use of it in the query */
  dt_collection_set_extended_where (darktable.collection,query);
  dt_collection_set_query_flags (darktable.collection, (dt_collection_get_query_flags (darktable.collection) | COLLECTION_QUERY_USE_WHERE_EXT));
  
  /* remove film id from default filter */
  dt_collection_set_filter_flags (darktable.collection, (dt_collection_get_filter_flags (darktable.collection) & ~COLLECTION_FILTER_FILM_ID));
  
  /* update query and at last the visual */
  dt_collection_update (darktable.collection);
  
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
  gchar *escaped_text = dt_util_str_escape(text, "'", "''");
  dt_conf_set_string("plugins/lighttable/collect/string", text);
  dt_conf_set_int ("plugins/lighttable/collect/item", property);
  
  switch(property)
  {
    case 0: // film roll
      snprintf(query, 1024, "select distinct folder, id from film_rolls where folder like '%%%s%%'", escaped_text);
      break;
    case 1: // camera
      snprintf(query, 1024, "select distinct maker || ' ' || model, 1 from images where maker || ' ' || model like '%%%s%%'", escaped_text);
      break;
    case 2: // tag
      snprintf(query, 1024, "select distinct name, id from tags where name like '%%%s%%'", escaped_text);
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
    
    case 5: // colorlabels
      gtk_list_store_append(GTK_LIST_STORE(model), &iter);
      gtk_list_store_set (GTK_LIST_STORE(model), &iter,
        DT_LIB_COLLECT_COL_TEXT,_("red"),
        DT_LIB_COLLECT_COL_ID, 0,
        -1);
      gtk_list_store_append(GTK_LIST_STORE(model), &iter);
      gtk_list_store_set (GTK_LIST_STORE(model), &iter,
        DT_LIB_COLLECT_COL_TEXT,_("yellow"),
        DT_LIB_COLLECT_COL_ID, 1,
        -1);
     gtk_list_store_append(GTK_LIST_STORE(model), &iter);
      gtk_list_store_set (GTK_LIST_STORE(model), &iter,
        DT_LIB_COLLECT_COL_TEXT,_("green"),
        DT_LIB_COLLECT_COL_ID, 2,
        -1);
      goto entry_key_press_exit;
    break;
    
    // TODO: Add empty string for metadata?
    // TODO: Autogenerate this code?
    case 6: // title
        snprintf(query, 1024, "select distinct value, 1 from meta_data where key = %d and value like '%%%s%%'",
        DT_METADATA_XMP_DC_TITLE, escaped_text);
        break;
    case 7: // description
        snprintf(query, 1024, "select distinct value, 1 from meta_data where key = %d and value like '%%%s%%'",
        DT_METADATA_XMP_DC_DESCRIPTION, escaped_text);
        break;
    case 8: // creator
        snprintf(query, 1024, "select distinct value, 1 from meta_data where key = %d and value like '%%%s%%'",
        DT_METADATA_XMP_DC_CREATOR, escaped_text);
        break;
    case 9: // publisher
        snprintf(query, 1024, "select distinct value, 1 from meta_data where key = %d and value like '%%%s%%'",
        DT_METADATA_XMP_DC_PUBLISHER, escaped_text);
        break;
    case 10: // rights
        snprintf(query, 1024, "select distinct value, 1 from meta_data where key = %d and value like '%%%s%%'",
        DT_METADATA_XMP_DC_RIGHTS, escaped_text);
        break;

    default: // case 3: // day
      snprintf(query, 1024, "select distinct datetime_taken, 1 from images where datetime_taken like '%%%s%%'", escaped_text);
      break;
  }
  g_free(text);
  g_free(escaped_text);
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

int
position ()
{
  return 400;
}




void
gui_init (dt_lib_module_t *self)
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)malloc(sizeof(dt_lib_collect_t));
  self->data = (void *)d;
  self->widget = gtk_vbox_new(FALSE, 5);
  gtk_widget_set_size_request(self->widget ,100,-1);

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
  gtk_combo_box_append_text(GTK_COMBO_BOX(w), _("colorlabel"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(w), _("title"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(w), _("description"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(w), _("creator"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(w), _("publisher"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(w), _("rights"));
  gtk_combo_box_set_active(GTK_COMBO_BOX(w), dt_conf_get_int("plugins/lighttable/collect/item"));
  g_signal_connect(G_OBJECT(w), "changed", G_CALLBACK(combo_changed), d);
  gtk_box_pack_start(box, w, FALSE, FALSE, 0);
  w = gtk_combo_box_entry_new_text();
  dt_gui_key_accel_block_on_focus (w);
  d->text = GTK_COMBO_BOX_ENTRY(w);

/* xgettext:no-c-format */
  gtk_object_set(GTK_OBJECT(d->text), "tooltip-text", _("type your query, use `%' as wildcard"), (char *)NULL);
  gchar *text = dt_conf_get_string("plugins/lighttable/collect/string");
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
  gtk_object_set(GTK_OBJECT(view), "tooltip-text", _("doubleclick to select"), (char *)NULL);
  g_signal_connect(G_OBJECT (view), "row-activated", G_CALLBACK (row_activated), d);
  entry_key_press (NULL, NULL, d);
}

void
gui_cleanup (dt_lib_module_t *self)
{
  free(self->data);
  self->data = NULL;
}

