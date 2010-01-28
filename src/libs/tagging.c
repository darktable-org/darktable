#include "common/darktable.h"
#include "control/control.h"
#include "control/conf.h"
#include "libs/lib.h"
#include "gui/gtk.h"
#include <glade/glade.h>
#include <math.h>

#define MAX_TAGS_IN_LIST 14
#define EXPOSE_COLUMNS 2

typedef struct dt_lib_tagging_t
{
  char related_query[1024];
  GtkEntry *entry;
  GtkTreeView *current, *related;
  int imgsel;
}
dt_lib_tagging_t;

typedef enum dt_lib_tagging_cols_t
{
  DT_LIB_TAGGING_COL_TAG=0,
  DT_LIB_TAGGING_COL_ID,
  DT_LIB_TAGGING_NUM_COLS
}
dt_lib_tagging_cols_t;

const char*
name ()
{
  return _("tagging");
}

static void 
update (dt_lib_module_t *self, int which)
{
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;

  int rc;
  sqlite3_stmt *stmt;

  if(which == 0) // tags of selected images
  {
    int imgsel = -1;
    DT_CTL_GET_GLOBAL(imgsel, lib_image_mouse_over_id);
    d->imgsel = imgsel;
    if(imgsel > 0)
    {
      // draw affected image number in bg
      char query[1024];
#if 0
      cairo_set_source_rgb (cr, .3, .3, .3);
      cairo_set_font_size (cr, .8f*height);
      snprintf(query, 1024, "%d", imgsel);
      cairo_text_extents_t ext;
      cairo_text_extents (cr, query, &ext);
      cairo_move_to(cr, width-ext.width, height);
      cairo_show_text(cr, query);
#endif
      snprintf(query, 1024, "select distinct tags.id, tags.name from tagged_images "
          "join tags on tags.id = tagged_images.tagid where tagged_images.imgid = %d", imgsel);
      rc = sqlite3_prepare_v2(darktable.db, query, -1, &stmt, NULL);
    }
    else
    {
#if 0
      char nums[40], *p = nums;
      int cnt = 0;
      rc = sqlite3_prepare_v2(darktable.db, "select imgid from selected_images", -1, &stmt, NULL);
      while(sqlite3_step(stmt) == SQLITE_ROW)
      {
        if(p == nums) snprintf(p, 40 - (p-nums), "%d",  sqlite3_column_int(stmt, 0));
        else          snprintf(p, 40 - (p-nums), ",%d", sqlite3_column_int(stmt, 0));
        p = nums + strlen(nums);
        if(p >= nums + 40) break;
        cnt++;
      }
      rc = sqlite3_finalize(stmt);
      cairo_set_source_rgb (cr, .3, .3, .3);
      cairo_set_font_size (cr, .8*height/powf(cnt, .3));
      cairo_text_extents_t ext;
      cairo_text_extents (cr, nums, &ext);
      cairo_move_to(cr, MAX(5, width-ext.width), height);
      cairo_show_text(cr, nums);
#endif
      rc = sqlite3_prepare_v2(darktable.db, "select distinct tags.id, tags.name from selected_images join tagged_images "
          "on selected_images.imgid = tagged_images.imgid join tags on tags.id = tagged_images.tagid", -1, &stmt, NULL);
    }
  }
  else // related tags of typed text
  {
    rc = sqlite3_exec(darktable.db, "create temp table tagquery1 (tagid integer, name varchar, count integer)", NULL, NULL, NULL);
    rc = sqlite3_exec(darktable.db, "create temp table tagquery2 (tagid integer, name varchar, count integer)", NULL, NULL, NULL);
    rc = sqlite3_exec(darktable.db, d->related_query, NULL, NULL, NULL);
    rc = sqlite3_exec(darktable.db, "insert into tagquery2 select distinct tagid, name, "
        "(select sum(count) from tagquery1 as b where b.tagid=a.tagid) from tagquery1 as a",
        NULL, NULL, NULL);
    rc = sqlite3_prepare_v2(darktable.db, "select tagid, name from tagquery2 order by count desc", -1, &stmt, NULL);
  }
  int num = 0;
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
    num++;
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

static void
set_related_query(dt_lib_module_t *self, dt_lib_tagging_t *d)
{
  // sql query for filtered tags and (one bounce) for related tags
  snprintf(d->related_query, 1024,
    "insert into tagquery1 select related.id, related.name, cross.count from ( "
    "select * from tags join tagxtag on tags.id = tagxtag.id1 or tags.id = tagxtag.id2 "
    "where name like '%%%s%%') as cross join tags as related "
    "where (id2 = related.id or id1 = related.id) "
    "and (cross.id1 = cross.id2 or related.id != cross.id) "
    "and cross.count > 0",
    gtk_entry_get_text(d->entry));
  update (self, 1);
}

static gboolean
expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;
  int imgsel = -1;
  DT_CTL_GET_GLOBAL(imgsel, lib_image_mouse_over_id);
  if(imgsel != d->imgsel) update (self, 0);
  return FALSE;
}

static gboolean
tag_name_changed (GtkEntry *entry, GdkEventKey *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;
  set_related_query(self, d);
  return FALSE;
}

static void
attach_selected_tag(dt_lib_module_t *self, dt_lib_tagging_t *d)
{
  int rc;
  sqlite3_stmt *stmt;
  
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeView *view = d->related;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;
  guint tag;
  gtk_tree_model_get (model, &iter, 
                      DT_LIB_TAGGING_COL_ID, &tag,
                      -1);

  int imgsel = -1;
  if(tag <= 0) return;

  DT_CTL_GET_GLOBAL(imgsel, lib_image_mouse_over_id);
  if(imgsel > 0)
  {
    rc = sqlite3_prepare_v2(darktable.db, "insert or replace into tagged_images (imgid, tagid) values (?1, ?2)", -1, &stmt, NULL);
    rc = sqlite3_bind_int(stmt, 1, imgsel);
    rc = sqlite3_bind_int(stmt, 2, tag);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize(stmt);

    rc = sqlite3_prepare_v2(darktable.db, "update tagxtag set count = count + 1 where "
        "(id1 = ?1 and id2 in (select tagid from tagged_images where imgid = ?2)) or "
        "(id2 = ?1 and id1 in (select tagid from tagged_images where imgid = ?2))", -1, &stmt, NULL);
    rc = sqlite3_bind_int(stmt, 1, tag);
    rc = sqlite3_bind_int(stmt, 2, imgsel);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize(stmt);
  }
  else
  { // insert into tagged_images if not there already.
    rc = sqlite3_prepare_v2(darktable.db, "insert or replace into tagged_images select imgid, ?1 from selected_images", -1, &stmt, NULL);
    rc = sqlite3_bind_int(stmt, 1, tag);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize(stmt);

    rc = sqlite3_prepare_v2(darktable.db, "update tagxtag set count = count + 1 where "
        "(id1 = ?1 and id2 in (select tagid from selected_images join tagged_images)) or "
        "(id2 = ?1 and id1 in (select tagid from selected_images join tagged_images))", -1, &stmt, NULL);
    rc = sqlite3_bind_int(stmt, 1, tag);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize(stmt);
  }
}

static void
detach_selected_tag(dt_lib_module_t *self, dt_lib_tagging_t *d)
{
  int rc;
  sqlite3_stmt *stmt;

  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeView *view = d->current;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;
  guint tag;
  gtk_tree_model_get (model, &iter, 
                      DT_LIB_TAGGING_COL_ID, &tag,
                      -1);

  int imgsel = -1;
  if(tag <= 0) return;

  DT_CTL_GET_GLOBAL(imgsel, lib_image_mouse_over_id);
  if(imgsel > 0)
  {
    rc = sqlite3_prepare_v2(darktable.db, "update tagxtag set count = count - 1 where "
        "(id1 = ?1 and id2 in (select tagid from tagged_images where imgid = ?2)) or "
        "(id2 = ?1 and id1 in (select tagid from tagged_images where imgid = ?2))", -1, &stmt, NULL);
    rc = sqlite3_bind_int(stmt, 1, tag);
    rc = sqlite3_bind_int(stmt, 2, imgsel);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize(stmt);

    // remove from tagged_images
    rc = sqlite3_prepare_v2(darktable.db, "delete from tagged_images where tagid = ?1 and imgid = ?2", -1, &stmt, NULL);
    rc = sqlite3_bind_int(stmt, 1, tag);
    rc = sqlite3_bind_int(stmt, 2, imgsel);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize(stmt);
  }
  else
  {
    rc = sqlite3_prepare_v2(darktable.db, "update tagxtag set count = count - 1 where "
        "(id1 = ?1 and id2 in (select tagid from selected_images join tagged_images)) or "
        "(id2 = ?1 and id1 in (select tagid from selected_images join tagged_images))", -1, &stmt, NULL);
    rc = sqlite3_bind_int(stmt, 1, tag);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize(stmt);

    // remove from tagged_images
    rc = sqlite3_prepare_v2(darktable.db, "delete from tagged_images where tagid = ?1 and imgid in (select imgid from selected_images)", -1, &stmt, NULL);
    rc = sqlite3_bind_int(stmt, 1, tag);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize(stmt);
  }
}

static void
attach_activated (GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *col, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;
  attach_selected_tag(self, d);
  update(self, 0);
}

static void
detach_activated (GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *col, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;
  detach_selected_tag(self, d);
  update(self, 0);
}

static void
attach_button_clicked (GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;
  attach_selected_tag(self, d);
  update(self, 0);
}

static void
detach_button_clicked (GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;
  detach_selected_tag(self, d);
  update(self, 0);
}

static void
new_button_clicked (GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;
  int rc, rt, id;
  sqlite3_stmt *stmt;
  const gchar *tag = gtk_entry_get_text(d->entry);
  if(tag[0] == '\0') return; // no tag name.
  rc = sqlite3_prepare_v2(darktable.db, "select id from tags where name = ?1", -1, &stmt, NULL);
  rc = sqlite3_bind_text(stmt, 1, tag, strlen(tag), SQLITE_TRANSIENT);
  rt = sqlite3_step(stmt);
  rc = sqlite3_finalize(stmt);
  if(rt == SQLITE_ROW) return; // already there.
  rc = sqlite3_prepare_v2(darktable.db, "insert into tags (id, name) values (null, ?1)", -1, &stmt, NULL);
  rc = sqlite3_bind_text(stmt, 1, tag, strlen(tag), SQLITE_TRANSIENT);
  pthread_mutex_lock(&(darktable.db_insert));
  rc = sqlite3_step(stmt);
  rc = sqlite3_finalize(stmt);
  id = sqlite3_last_insert_rowid(darktable.db);
  pthread_mutex_unlock(&(darktable.db_insert));
  rc = sqlite3_prepare_v2(darktable.db, "insert into tagxtag select id, ?1, 0 from tags", -1, &stmt, NULL);
  rc = sqlite3_bind_int(stmt, 1, id);
  rc = sqlite3_step(stmt);
  rc = sqlite3_finalize(stmt);
  rc = sqlite3_prepare_v2(darktable.db, "update tagxtag set count = 1000000 where id1 = ?1 and id2 = ?1", -1, &stmt, NULL);
  rc = sqlite3_bind_int(stmt, 1, id);
  rc = sqlite3_step(stmt);
  rc = sqlite3_finalize(stmt);
  update(self, 1);
}

static void
delete_button_clicked (GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;

  int rc, res = GTK_RESPONSE_YES;
  sqlite3_stmt *stmt;
  guint id;
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeView *view = d->related;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;
  gtk_tree_model_get (model, &iter, 
                      DT_LIB_TAGGING_COL_ID, &id,
                      -1);


  rc = sqlite3_prepare_v2(darktable.db, "select name from tags where id=?1", -1, &stmt, NULL);
  rc = sqlite3_bind_int(stmt, 1, id);
  if(sqlite3_step(stmt) != SQLITE_ROW)
  {
    rc = sqlite3_finalize(stmt);
    return;
  }
  if(dt_conf_get_bool("plugins/lighttable/tagging/ask_before_delete_tag"))
  {
    GtkWidget *dialog;
    GtkWidget *win = glade_xml_get_widget (darktable.gui->main_window, "main_window");
    dialog = gtk_message_dialog_new(GTK_WINDOW(win),
        GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_YES_NO,
        _("do you really want to delete the tag `%s'?\nthis will also strip the tag off all tagged images!"),
        (const char *)sqlite3_column_text(stmt, 0));
    gtk_window_set_title(GTK_WINDOW(dialog), _("delete tag?"));
    res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
  }
  rc = sqlite3_finalize(stmt);
  if(res != GTK_RESPONSE_YES) return;

  rc = sqlite3_prepare_v2(darktable.db, "delete from tags where id=?1", -1, &stmt, NULL);
  rc = sqlite3_bind_int(stmt, 1, id);
  rc = sqlite3_step(stmt);
  rc = sqlite3_finalize(stmt);
  rc = sqlite3_prepare_v2(darktable.db, "delete from tagxtag where id1=?1 or id2=?1", -1, &stmt, NULL);
  rc = sqlite3_bind_int(stmt, 1, id);
  rc = sqlite3_step(stmt);
  rc = sqlite3_finalize(stmt);
  rc = sqlite3_prepare_v2(darktable.db, "delete from tagged_images where tagid=?1", -1, &stmt, NULL);
  rc = sqlite3_bind_int(stmt, 1, id);
  rc = sqlite3_step(stmt);
  rc = sqlite3_finalize(stmt);
  update(self, 0);
  update(self, 1);
}

void
gui_reset (dt_lib_module_t *self)
{
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;
  // clear entry box and query
  gtk_entry_set_text(d->entry, "");
  set_related_query(self, d);
}

void
gui_init (dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)malloc(sizeof(dt_lib_tagging_t));
  self->data = (void *)d;
  d->imgsel = -1;

  self->widget = gtk_hbox_new(TRUE, 0);
  g_signal_connect(self->widget, "expose-event", G_CALLBACK(expose), (gpointer)self);
  darktable.gui->redraw_widgets = g_list_append(darktable.gui->redraw_widgets, self->widget);

  GtkBox *box, *hbox;
  GtkWidget *button;
  GtkWidget *w;
  GtkListStore *liststore;

  // left side, current
  box = GTK_BOX(gtk_vbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(box), TRUE, TRUE, 0);
  w = gtk_scrolled_window_new(NULL, NULL);
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
  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(d->current),
      GTK_SELECTION_SINGLE);
  gtk_tree_view_set_model(d->current, GTK_TREE_MODEL(liststore));
  gtk_object_set(GTK_OBJECT(d->current), "tooltip-text", _("attached tags,\ndoubleclick to detach"), NULL);
  g_signal_connect(G_OBJECT (d->current), "row-activated", G_CALLBACK (detach_activated), (gpointer)self);
  gtk_container_add(GTK_CONTAINER(w), GTK_WIDGET(d->current));

  // attach/detach buttons
  hbox = GTK_BOX(gtk_hbox_new(TRUE, 5));

  button = gtk_button_new_with_label(_("attach"));
  gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("attach tag to all selected images"), NULL);
  gtk_box_pack_start(hbox, button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT (button), "clicked",
                   G_CALLBACK (attach_button_clicked), (gpointer)self);

  button = gtk_button_new_with_label(_("detach"));
  gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("detach tag from all selected images"), NULL);
  g_signal_connect(G_OBJECT (button), "clicked",
                   G_CALLBACK (detach_button_clicked), (gpointer)self);
  gtk_box_pack_start(hbox, button, FALSE, TRUE, 0);

  gtk_box_pack_start(box, GTK_WIDGET(hbox), FALSE, TRUE, 0);

  // right side, related 
  box = GTK_BOX(gtk_vbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(box), TRUE, TRUE, 5);
  
  // text entry and new button
  w = gtk_entry_new();
  gtk_object_set(GTK_OBJECT(w), "tooltip-text", _("enter tag name"), NULL);
  gtk_box_pack_start(box, w, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(w), "key-release-event",
                   G_CALLBACK(tag_name_changed), (gpointer)self);
  d->entry = GTK_ENTRY(w);

  // related tree view
  w = gtk_scrolled_window_new(NULL, NULL);
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
  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(d->related),
      GTK_SELECTION_SINGLE);
  gtk_tree_view_set_model(d->related, GTK_TREE_MODEL(liststore));
  gtk_object_set(GTK_OBJECT(d->related), "tooltip-text", _("related tags,\ndoubleclick to attach"), NULL);
  g_signal_connect(G_OBJECT (d->related), "row-activated", G_CALLBACK (attach_activated), (gpointer)self);
  gtk_container_add(GTK_CONTAINER(w), GTK_WIDGET(d->related));

  // attach and delete buttons
  hbox = GTK_BOX(gtk_hbox_new(TRUE, 5));

  button = gtk_button_new_with_label(_("new"));
  gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("create a new tag with the\nname you entered"), NULL);
  gtk_box_pack_start(hbox, button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT (button), "clicked",
                   G_CALLBACK (new_button_clicked), (gpointer)self);

  button = gtk_button_new_with_label(_("delete"));
  gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("delete selected tag"), NULL);
  gtk_box_pack_start(hbox, button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT (button), "clicked",
                   G_CALLBACK (delete_button_clicked), (gpointer)self);

  gtk_box_pack_start(box, GTK_WIDGET(hbox), FALSE, TRUE, 0);

  set_related_query(self, d);
}

void
gui_cleanup (dt_lib_module_t *self)
{
  darktable.gui->redraw_widgets = g_list_remove(darktable.gui->redraw_widgets, self->widget);
  free(self->data);
  self->data = NULL;
}
