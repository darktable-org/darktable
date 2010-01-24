#include "common/darktable.h"
#include "control/control.h"
#include "control/conf.h"
#include "libs/lib.h"
#include "gui/gtk.h"
#include <glade/glade.h>

#define MAX_TAGS_IN_LIST 14
#define EXPOSE_COLUMNS 2

// TODO: if view 1:1 in lighttable, use mouse over? or if no images selected? or trigger selection on 1:1 (no, stars)?

typedef struct dt_lib_tagging_t
{
  char related_query[1024];
  GtkEntry *entry;
  int current_taglist[MAX_TAGS_IN_LIST];
  int related_taglist[MAX_TAGS_IN_LIST];
  int current_showed_last;
  int current_offset;
  int current_selected;
  int related_showed_last;
  int related_offset;
  int related_selected;
}
dt_lib_tagging_t;

const char*
name ()
{
  return _("tagging");
}

static gboolean
expose_tags (GtkWidget *widget, GdkEventExpose *event, gpointer user_data, int which)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;

  const int offset = which == 0 ? d->current_offset : d->related_offset;
  const int num_tags = MAX_TAGS_IN_LIST/EXPOSE_COLUMNS;
  const int selected = which == 0 ? d->current_selected : d->related_selected;
  int width = widget->allocation.width, height = widget->allocation.height;
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  // clear bg
  cairo_set_source_rgb (cr, .2, .2, .2);
  cairo_paint(cr);

  int rc;
  sqlite3_stmt *stmt;

  if(which == 0) // tags of selected images
    rc = sqlite3_prepare_v2(darktable.db, "select distinct tags.id, tags.name from selected_images join tagged_images "
        "on selected_images.imgid = tagged_images.imgid join tags on tags.id = tagged_images.tagid limit ?1, ?2", -1, &stmt, NULL);
  else // related tags of typed text
  {
    rc = sqlite3_exec(darktable.db, "create temp table tagquery1 (tagid integer, name varchar, count integer)", NULL, NULL, NULL);
    rc = sqlite3_exec(darktable.db, "create temp table tagquery2 (tagid integer, name varchar, count integer)", NULL, NULL, NULL);
    rc = sqlite3_exec(darktable.db, d->related_query, NULL, NULL, NULL);
    rc = sqlite3_exec(darktable.db, "insert into tagquery2 select distinct tagid, name, "
        "(select sum(count) from tagquery1 as b where b.tagid=a.tagid) from tagquery1 as a",
        NULL, NULL, NULL);
    rc = sqlite3_prepare_v2(darktable.db, "select tagid, name from tagquery2 order by count desc limit ?1, ?2", -1, &stmt, NULL);
  }
  rc = sqlite3_bind_int(stmt, 1, offset);
  rc = sqlite3_bind_int(stmt, 2, MAX_TAGS_IN_LIST);
  int i = 0, j = 0, num = 0;
  cairo_set_source_rgb (cr, .7, .7, .7);
  cairo_select_font_face (cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size (cr, .7f*height/num_tags);
  for(int k=0;k<MAX_TAGS_IN_LIST;k++) if(which == 0) d->current_taglist[k] = -1; else d->related_taglist[k] = -1;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    if(num == MAX_TAGS_IN_LIST) break;
    int tag = sqlite3_column_int(stmt, 0);
    if     (which == 0) d->current_taglist[num] = tag;
    else if(which == 1) d->related_taglist[num] = tag;
    j = (num / EXPOSE_COLUMNS - 0.3 + 1)*height/num_tags;
    i = (num % EXPOSE_COLUMNS)*width/EXPOSE_COLUMNS + 5;
    if(selected == tag)
    {
      cairo_set_source_rgb (cr, .4, .4, .4);
      cairo_rectangle(cr, i-5, j-.7*height/num_tags, width/EXPOSE_COLUMNS, height/num_tags);
      cairo_fill(cr);
      cairo_set_source_rgb (cr, .7, .7, .7);
    }
    cairo_move_to (cr, i, j);
    cairo_show_text (cr, (const char *)sqlite3_column_text(stmt, 1));
    num++;
  }
  rc = sqlite3_finalize(stmt);
  if(which == 1)
  {
    sqlite3_exec(darktable.db, "drop table tagquery1", NULL, NULL, NULL);
    sqlite3_exec(darktable.db, "drop table tagquery2", NULL, NULL, NULL);
  }
  cairo_set_source_rgb (cr, .7, .7, .7);
  if(num == MAX_TAGS_IN_LIST)
  { // there's more in this list!
    if(which == 0) d->current_showed_last = 0;
    if(which == 1) d->related_showed_last = 0;
    cairo_move_to(cr, width - 5, height - 5);
    cairo_line_to(cr, width, height - 5);
    cairo_line_to(cr, width - 2.5, height);
    cairo_close_path(cr);
    cairo_fill(cr);
  }
  else
  {
    if(which == 0) d->current_showed_last = 1;
    if(which == 1) d->related_showed_last = 1;
  }
  if(offset > 0)
  {
    cairo_move_to(cr, width - 5, 5);
    cairo_line_to(cr, width, 5);
    cairo_line_to(cr, width - 2.5, 0);
    cairo_close_path(cr);
    cairo_fill(cr);
  }
  
  cairo_destroy(cr);
  cairo_t *cr_pixmap = gdk_cairo_create(gtk_widget_get_window(widget));
  cairo_set_source_surface (cr_pixmap, cst, 0, 0);
  cairo_paint(cr_pixmap);
  cairo_destroy(cr_pixmap);
  cairo_surface_destroy(cst);
  return TRUE;
}

static gboolean
expose_current_tags (GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  return expose_tags(widget, event, user_data, 0);
}

static gboolean
expose_related_tags (GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  return expose_tags(widget, event, user_data, 1);
}

static void set_related_query(dt_lib_module_t *self, dt_lib_tagging_t *d)
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
  gtk_widget_queue_draw(self->widget);
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
  int tag = d->related_selected;
  if(tag <= 0) return;

  // insert into tagged_images if not there already.
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

static void
detach_selected_tag(dt_lib_module_t *self, dt_lib_tagging_t *d)
{
  int rc;
  sqlite3_stmt *stmt;
  int tag = d->current_selected;
  if(tag <= 0) return;

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

static void
attach_button_clicked (GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;
  attach_selected_tag(self, d);
  gtk_widget_queue_draw(self->widget);
}

static void
detach_button_clicked (GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;
  detach_selected_tag(self, d);
  gtk_widget_queue_draw(self->widget);
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
  gtk_widget_queue_draw(self->widget);
}

static void
delete_button_clicked (GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;

  int rc, res = GTK_RESPONSE_YES;
  sqlite3_stmt *stmt;
  int id = d->related_selected;

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
}

static gboolean 
current_scrolled (GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;
  if(event->direction == GDK_SCROLL_UP)
    d->current_offset = MAX(0, d->current_offset - EXPOSE_COLUMNS);
  else if(!d->current_showed_last)
    d->current_offset += EXPOSE_COLUMNS;
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean 
related_scrolled (GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;
  if(event->direction == GDK_SCROLL_UP)
    d->related_offset = MAX(0, d->related_offset - EXPOSE_COLUMNS);
  else if(!d->related_showed_last)
    d->related_offset += EXPOSE_COLUMNS;
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean
current_button_pressed (GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t  *)user_data;
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;
  const int num_tags = MAX_TAGS_IN_LIST/EXPOSE_COLUMNS;
  const int width = widget->allocation.width, height = widget->allocation.height;
  if(event->x > width - 10 && event->y < 10) // scroll up/down
    d->current_offset = MAX(0, d->current_offset - EXPOSE_COLUMNS);
  else if(event->x > width - 10 && event->y > height - 10 && !d->current_showed_last)
    d->current_offset = MAX(0, d->current_offset - EXPOSE_COLUMNS);
  else
  { // select tag
    int y = (int)(num_tags * event->y / (float)height);
    int x = (int)(EXPOSE_COLUMNS * event->x / (float)width);
    int selected = y * EXPOSE_COLUMNS + x;
    selected = MAX(0, MIN(MAX_TAGS_IN_LIST-1, selected));
    if(d->current_taglist[selected] > 0) d->current_selected = d->current_taglist[selected];
    else d->current_selected = -1;
    if(event->type == GDK_2BUTTON_PRESS) detach_selected_tag(self, d);
  }
  gtk_widget_queue_draw(self->widget);
  return TRUE;
}

static gboolean
related_button_pressed (GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t  *)user_data;
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;
  const int num_tags = MAX_TAGS_IN_LIST/EXPOSE_COLUMNS;
  const int width = widget->allocation.width, height = widget->allocation.height;
  if(event->x > width - 10 && event->y < 10) // scroll up/down
    d->related_offset = MAX(0, d->related_offset - EXPOSE_COLUMNS);
  else if(event->x > width - 10 && event->y > height - 10 && !d->related_showed_last)
    d->related_offset = MAX(0, d->related_offset - EXPOSE_COLUMNS);
  else
  { // select tag
    int y = (int)(num_tags * event->y / (float)height);
    int x = (int)(EXPOSE_COLUMNS * event->x / (float)width);
    int selected = y * EXPOSE_COLUMNS + x;
    selected = MAX(0, MIN(MAX_TAGS_IN_LIST-1, selected));
    if(d->related_taglist[selected] > 0) d->related_selected = d->related_taglist[selected];
    else d->related_selected = -1;
    if(event->type == GDK_2BUTTON_PRESS) attach_selected_tag(self, d);
  }
  gtk_widget_queue_draw(self->widget);
  return TRUE;
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

  d->related_offset = d->current_offset = 0;
  d->related_showed_last = d->current_showed_last = 0;
  d->related_selected = d->current_selected = -1;

  self->widget = gtk_vbox_new(FALSE, 5);
  darktable.gui->redraw_widgets = g_list_append(darktable.gui->redraw_widgets, self->widget);

  GtkBox *hbox;
  GtkWidget *button;
  GtkWidget *w;

  w = gtk_drawing_area_new();
  GtkWidget *asp = gtk_aspect_frame_new(NULL, 0.5, 0.5, 1.0, TRUE);
  gtk_box_pack_start(GTK_BOX(self->widget), asp, TRUE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(asp), w);
  gtk_drawing_area_size(GTK_DRAWING_AREA(w), 258, 158);
  gtk_widget_add_events(w, GDK_BUTTON_PRESS_MASK);
  g_signal_connect(G_OBJECT(w), "expose-event",
                   G_CALLBACK(expose_current_tags), (gpointer)self);
	g_signal_connect (G_OBJECT (w), "scroll-event",
                    G_CALLBACK (current_scrolled), (gpointer)self);
	g_signal_connect (G_OBJECT (w), "button-press-event",
                    G_CALLBACK (current_button_pressed), (gpointer)self);
  gtk_object_set(GTK_OBJECT(w), "tooltip-text", _("attached tags,\ndoubleclick to detach"), NULL);

  hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
  w = gtk_entry_new();
  gtk_object_set(GTK_OBJECT(w), "tooltip-text", _("enter tag name"), NULL);
  gtk_box_pack_start(hbox, w, TRUE, TRUE, 5);
  g_signal_connect(G_OBJECT(w), "key-release-event",
                   G_CALLBACK(tag_name_changed), (gpointer)self);
  d->entry = GTK_ENTRY(w);
  set_related_query(self, d);

  button = gtk_button_new_with_label(_("new"));
  gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("create a new tag with the\nname you entered"), NULL);
  gtk_box_pack_start(hbox, button, FALSE, TRUE, 5);
  g_signal_connect(G_OBJECT (button), "clicked",
                   G_CALLBACK (new_button_clicked), (gpointer)self);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);


  w = gtk_drawing_area_new();
  asp = gtk_aspect_frame_new(NULL, 0.5, 0.5, 1.0, TRUE);
  gtk_box_pack_start(GTK_BOX(self->widget), asp, TRUE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(asp), w);
  gtk_drawing_area_size(GTK_DRAWING_AREA(w), 258, 158);
  gtk_widget_add_events(w, GDK_BUTTON_PRESS_MASK);
  g_signal_connect(G_OBJECT(w), "expose-event",
                   G_CALLBACK(expose_related_tags), (gpointer)self);
	g_signal_connect (G_OBJECT (w), "scroll-event",
                    G_CALLBACK (related_scrolled), (gpointer)self);
	g_signal_connect (G_OBJECT (w), "button-press-event",
                    G_CALLBACK (related_button_pressed), (gpointer)self);
  gtk_object_set(GTK_OBJECT(w), "tooltip-text", _("related tags,\ndoubleclick to attach"), NULL);

  hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));

  button = gtk_button_new_with_label(_("delete"));
  gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("delete selected tag"), NULL);
  gtk_box_pack_start(hbox, button, TRUE, TRUE, 5);
  g_signal_connect(G_OBJECT (button), "clicked",
                   G_CALLBACK (delete_button_clicked), (gpointer)self);

  button = gtk_button_new_with_label(_("attach"));
  gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("attach tag to all selected images"), NULL);
  gtk_box_pack_start(hbox, button, TRUE, TRUE, 5);
  g_signal_connect(G_OBJECT (button), "clicked",
                   G_CALLBACK (attach_button_clicked), (gpointer)self);

  button = gtk_button_new_with_label(_("detach"));
  gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("detach tag from all selected images"), NULL);
  gtk_box_pack_start(hbox, button, TRUE, TRUE, 5);
  g_signal_connect(G_OBJECT (button), "clicked",
                   G_CALLBACK (detach_button_clicked), (gpointer)self);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);
}

void
gui_cleanup (dt_lib_module_t *self)
{
  darktable.gui->redraw_widgets = g_list_remove(darktable.gui->redraw_widgets, self->widget);
  free(self->data);
  self->data = NULL;
}
