#include "common/darktable.h"
#include "control/conf.h"
#include "libs/lib.h"

typedef struct dt_lib_collect_t
{
  GtkComboBox *combo;
  GtkComboBoxEntry *text;
  GtkTreeView *view;
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
}

static gboolean
entry_key_press (GtkEntry *entry, GdkEventKey *event, dt_lib_collect_t *d)
{ // update related list
  /*if(gtk_combo_box_get_active(GTK_COMBO_BOX(d->text)) == 0)
  {
    GtkTreeView *view = d->view;
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
    g_object_ref(model);
    gtk_tree_view_set_model(GTK_TREE_VIEW(view), NULL);
    gtk_list_store_clear(GTK_LIST_STORE(model));
    gtk_tree_view_set_model(GTK_TREE_VIEW(view), model);
    g_object_unref(model);
    return FALSE;
  }*/
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
  switch(property)
  {
    case 0: // film roll
      snprintf(query, 1024, "select distinct folder, id from film_rolls where folder like '%%%s%%'", text);
      break;
    case 1: // camera
      snprintf(query, 1024, "select distinct maker || ' ' || model, 1 from images where model like '%%%s%%' or maker like '%%%s%%'", text, text);
      break;
    case 2: // tag
      snprintf(query, 1024, "select distinct name, id from tags where name like '%%%s%%'", text);
      break;
    default: // case 3: // day
      snprintf(query, 1024, "select distinct datetime_taken, 1 from images where datetime_taken like '%%%s%%'", text);
      break;
  }
  g_free(text);
  printf("starting query `%s'\n", query);
  sqlite3_prepare_v2(darktable.db, query, -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    gtk_list_store_append(GTK_LIST_STORE(model), &iter);
    gtk_list_store_set (GTK_LIST_STORE(model), &iter,
                        DT_LIB_COLLECT_COL_TEXT, sqlite3_column_text(stmt, 0),
                        DT_LIB_COLLECT_COL_ID, sqlite3_column_int(stmt, 1),
                        -1);
  }
  sqlite3_finalize(stmt);
  gtk_tree_view_set_model(GTK_TREE_VIEW(view), model);
  g_object_unref(model);
  return FALSE;
}

#if 0
static void
update_related_list (dt_lib_module_t *self)
{
  dt_lib_collect_t *d   = (dt_lib_collect_t *)self->data;

  int rc;
  sqlite3_stmt *stmt;

  // film roll, camera, tag, day
  int property = gtk_combo_box_get_active(d->combo);

  if(gtk_combo_box_text_get_active(GTK_COMBO_BOX(d->text)) == 0)
  {
    int imgsel = -1;
    DT_CTL_GET_GLOBAL(imgsel, lib_image_mouse_over_id);
    // TODO: build query based on 
    if(imgsel > 0)
    {
      char query[1024];
      snprintf(query, 1024, "select distinct tags.id, tags.name from tagged_images "
          "join tags on tags.id = tagged_images.tagid where tagged_images.imgid = %d", imgsel);
      rc = sqlite3_prepare_v2(darktable.db, query, -1, &stmt, NULL);
    }
    else
    {
      rc = sqlite3_prepare_v2(darktable.db, "select distinct tags.id, tags.name from selected_images join tagged_images "
          "on selected_images.imgid = tagged_images.imgid join tags on tags.id = tagged_images.tagid", -1, &stmt, NULL);
    }
  }
  else
  {
    // get gtk text
    rc = sqlite3_exec(darktable.db, "create temp table tagquery1 (tagid integer, name varchar, count integer)", NULL, NULL, NULL);
    rc = sqlite3_exec(darktable.db, "create temp table tagquery2 (tagid integer, name varchar, count integer)", NULL, NULL, NULL);
    rc = sqlite3_exec(darktable.db, d->related_query, NULL, NULL, NULL);
    rc = sqlite3_exec(darktable.db, "insert into tagquery2 select distinct tagid, name, "
        "(select sum(count) from tagquery1 as b where b.tagid=a.tagid) from tagquery1 as a",
        NULL, NULL, NULL);
    rc = sqlite3_prepare_v2(darktable.db, "select tagid, name from tagquery2 order by count desc", -1, &stmt, NULL);
  }

  GtkTreeIter iter;
  GtkTreeView *view;
  if(which == 0) view = d->current;
  else           view = d->related;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
  g_object_ref(model);
  gtk_tree_view_set_model(GTK_TREE_VIEW(view), NULL);
  gtk_list_store_clear(GTK_LIST_STORE(model));
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    gtk_list_store_append(GTK_LIST_STORE(model), &iter);
    gtk_list_store_set (GTK_LIST_STORE(model), &iter,
                        DT_LIB_TAGGING_COL_TAG, sqlite3_column_text(stmt, 1),
                        DT_LIB_TAGGING_COL_ID, sqlite3_column_int(stmt, 0),
                        -1);
  }
  rc = sqlite3_finalize(stmt);
  if(which != 0)
  {
    sqlite3_exec(darktable.db, "delete from tagquery1", NULL, NULL, NULL);
    sqlite3_exec(darktable.db, "delete from tagquery2", NULL, NULL, NULL);
    sqlite3_exec(darktable.db, "drop table tagquery1", NULL, NULL, NULL);
    sqlite3_exec(darktable.db, "drop table tagquery2", NULL, NULL, NULL);
  }

  gtk_tree_view_set_model(GTK_TREE_VIEW(view), model);
  g_object_unref(model);
}
#endif

static void
combo_changed (GtkComboBox *combo, dt_lib_collect_t *d)
{
  gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(d->text))), "");
  entry_key_press (NULL, NULL, d);
}

static void
combo_entry_changed (GtkComboBox *combo, GtkWidget *w)
{
  int active = gtk_combo_box_get_active(combo);
  gtk_widget_set_visible(w, active);
  if(active) gtk_widget_show_all(w);
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
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(box), FALSE, FALSE, 0);
  w = gtk_combo_box_new_text();
  d->combo = GTK_COMBO_BOX(w);
  gtk_combo_box_append_text(GTK_COMBO_BOX(w), _("film roll"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(w), _("camera"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(w), _("tag"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(w), _("date"));
  gtk_combo_box_set_active(GTK_COMBO_BOX(w), 0);
  g_signal_connect(G_OBJECT(w), "changed", G_CALLBACK(combo_changed), d);
  gtk_box_pack_start(box, w, FALSE, FALSE, 0);
  w = gtk_combo_box_entry_new_text();
  d->text = GTK_COMBO_BOX_ENTRY(w);
  gtk_combo_box_append_text(GTK_COMBO_BOX(w), _("matches selected images"));
  gtk_combo_box_set_active(GTK_COMBO_BOX(w), -1);
  g_signal_connect(G_OBJECT(w), "changed", G_CALLBACK(combo_entry_changed), sw);
  gtk_widget_set_events(w, GDK_KEY_PRESS_MASK);
  g_signal_connect(G_OBJECT(gtk_bin_get_child(GTK_BIN(w))), "key-press-event", G_CALLBACK(entry_key_press), d);
  gtk_box_pack_start(box, w, FALSE, TRUE, 0);

  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_container_add(GTK_CONTAINER(sw), GTK_WIDGET(view));
  // TODO: fix 5px border!
  // box = GTK_BOX(gtk_hbox_new(FALSE, 5));
  // gtk_box_pack_start(box, GTK_WIDGET(sw), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(sw), TRUE, TRUE, 0);
  // GTK_WIDGET_SET_FLAGS(sw, GTK_NO_SHOW_ALL);
  // gtk_widget_set_visible(sw, FALSE);
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
}

void
gui_cleanup (dt_lib_module_t *self)
{
  free(self->data);
  self->data = NULL;
}

