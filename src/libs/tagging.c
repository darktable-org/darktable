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
#include "common/tags.h"
#include "control/control.h"
#include "control/conf.h"
#include "libs/lib.h"
#include "gui/gtk.h"
#include <glade/glade.h>
#include <gdk/gdkkeysyms.h>
#include <math.h>

DT_MODULE(1)

typedef struct dt_lib_tagging_t
{
  char keyword[1024];
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

uint32_t views() 
{
  return DT_LIGHTTABLE_VIEW|DT_CAPTURE_VIEW;
}

static void 
update (dt_lib_module_t *self, int which)
{
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;
  GList *tags=NULL;
  uint32_t count;  

  if(which == 0) // tags of selected images
  {
    int imgsel = -1;
    DT_CTL_GET_GLOBAL(imgsel, lib_image_mouse_over_id);
    d->imgsel = imgsel;
    count = dt_tag_get_attached(imgsel,&tags);
  }
  else // related tags of typed text
    count = dt_tag_get_suggestions(d->keyword,&tags);
  
  GtkTreeIter iter;
  GtkTreeView *view;
  if(which == 0) view = d->current;
  else           view = d->related;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
  g_object_ref(model);
  gtk_tree_view_set_model(GTK_TREE_VIEW(view), NULL);
  gtk_list_store_clear(GTK_LIST_STORE(model));
  
  if( count >0 && tags )
  {
    do
    {
      gtk_list_store_append(GTK_LIST_STORE(model), &iter);
      gtk_list_store_set (GTK_LIST_STORE(model), &iter,
                        DT_LIB_TAGGING_COL_TAG, ((dt_tag_t*)tags->data)->tag,
                        DT_LIB_TAGGING_COL_ID, ((dt_tag_t*)tags->data)->id,
                        -1);
    } while( (tags=g_list_next(tags)) !=NULL );
    
    // Free result...
    dt_tag_free_result(&tags);
  }
  
  
  
  gtk_tree_view_set_model(GTK_TREE_VIEW(view), model);
  g_object_unref(model);
}

static void
set_keyword(dt_lib_module_t *self, dt_lib_tagging_t *d)
{
  sprintf(d->keyword,"%s",gtk_entry_get_text(d->entry));
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

static void
attach_selected_tag(dt_lib_module_t *self, dt_lib_tagging_t *d)
{
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeView *view = d->related;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;
  guint tagid;
  gtk_tree_model_get (model, &iter, 
                      DT_LIB_TAGGING_COL_ID, &tagid,
                      -1);

  int imgsel = -1;
  if(tagid <= 0) return;

  DT_CTL_GET_GLOBAL(imgsel, lib_image_mouse_over_id);
  
  dt_tag_attach(tagid,imgsel);
  
}

static void
detach_selected_tag(dt_lib_module_t *self, dt_lib_tagging_t *d)
{
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeView *view = d->current;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;
  guint tagid;
  gtk_tree_model_get (model, &iter, 
                      DT_LIB_TAGGING_COL_ID, &tagid,
                      -1);

  int imgsel = -1;
  if(tagid <= 0) return;

  DT_CTL_GET_GLOBAL(imgsel, lib_image_mouse_over_id);
  
  dt_tag_detach(tagid,imgsel);
  
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
  const gchar *tag = gtk_entry_get_text(d->entry);
  dt_tag_new(tag,NULL);
  update(self, 1);
}

static void
tag_name_changed (GtkEntry *entry, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;
  set_keyword(self, d);
}

static void
delete_button_clicked (GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;

  int res = GTK_RESPONSE_YES;

  guint tagid;
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeView *view = d->related;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;
  gtk_tree_model_get (model, &iter, 
                      DT_LIB_TAGGING_COL_ID, &tagid,
                      -1);

  // First check how many images are affected by the remove
  int count = dt_tag_remove(tagid,FALSE);
  if( count > 0 && dt_conf_get_bool("plugins/lighttable/tagging/ask_before_delete_tag") )
  {
    GtkWidget *dialog;
    GtkWidget *win = glade_xml_get_widget (darktable.gui->main_window, "main_window");
    const gchar *tagname=dt_tag_get_name(tagid);
    dialog = gtk_message_dialog_new(GTK_WINDOW(win),
        GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_YES_NO,
        ngettext("do you really want to delete the tag `%s'?\n%d image is assigned this tag!",
        "do you really want to delete the tag `%s'?\n%d images are assigned this tag!", count),
        tagname,count);
    gtk_window_set_title(GTK_WINDOW(dialog), _("delete tag?"));
    res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
  }
  if(res != GTK_RESPONSE_YES) return;
  dt_tag_remove(tagid,TRUE);
  
  
  update(self, 0);
  update(self, 1);
}

void
gui_reset (dt_lib_module_t *self)
{
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;
  // clear entry box and query
  gtk_entry_set_text(d->entry, "");
  set_keyword(self, d);
}

int
position ()
{
  return 500;
}



void
gui_init (dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)malloc(sizeof(dt_lib_tagging_t));
  self->data = (void *)d;
  d->imgsel = -1;

  self->widget = gtk_vbox_new(TRUE, 0);
  gtk_widget_set_size_request(self->widget,100,-1);
		
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
  gtk_object_set(GTK_OBJECT(d->current), "tooltip-text", _("attached tags,\ndoubleclick to detach"), (char *)NULL);
  g_signal_connect(G_OBJECT (d->current), "row-activated", G_CALLBACK (detach_activated), (gpointer)self);
  gtk_container_add(GTK_CONTAINER(w), GTK_WIDGET(d->current));

  // attach/detach buttons
  hbox = GTK_BOX(gtk_hbox_new(TRUE, 5));
 
  button = gtk_button_new_with_label(_("attach"));
  gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("attach tag to all selected images"), (char *)NULL);
  gtk_box_pack_start(hbox, button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT (button), "clicked",
                   G_CALLBACK (attach_button_clicked), (gpointer)self);

  button = gtk_button_new_with_label(_("detach"));
  gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("detach tag from all selected images"), (char *)NULL);
  g_signal_connect(G_OBJECT (button), "clicked",
                   G_CALLBACK (detach_button_clicked), (gpointer)self);
  gtk_box_pack_start(hbox, button, FALSE, TRUE, 0);

  gtk_box_pack_start(box, GTK_WIDGET(hbox), FALSE, TRUE, 0);

  // right side, related 
  box = GTK_BOX(gtk_vbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(box), TRUE, TRUE, 5);
  
  // text entry and new button
  w = gtk_entry_new();
  dt_gui_key_accel_block_on_focus (w);
  gtk_object_set(GTK_OBJECT(w), "tooltip-text", _("enter tag name"), (char *)NULL);
  gtk_box_pack_start(box, w, TRUE, TRUE, 0);
  gtk_widget_add_events(GTK_WIDGET(w), GDK_KEY_RELEASE_MASK);
  // g_signal_connect(G_OBJECT(w), "key-release-event",
  g_signal_connect(G_OBJECT(w), "changed",
                   G_CALLBACK(tag_name_changed), (gpointer)self);
  g_signal_connect(G_OBJECT (w), "activate",
                   G_CALLBACK (new_button_clicked), (gpointer)self);
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
  gtk_object_set(GTK_OBJECT(d->related), "tooltip-text", _("related tags,\ndoubleclick to attach"), (char *)NULL);
  g_signal_connect(G_OBJECT (d->related), "row-activated", G_CALLBACK (attach_activated), (gpointer)self);
  gtk_container_add(GTK_CONTAINER(w), GTK_WIDGET(d->related));

  // attach and delete buttons
  hbox = GTK_BOX(gtk_hbox_new(TRUE, 5));

  button = gtk_button_new_with_label(_("new"));
  gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("create a new tag with the\nname you entered"), (char *)NULL);
  gtk_box_pack_start(hbox, button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT (button), "clicked",
                   G_CALLBACK (new_button_clicked), (gpointer)self);

  button = gtk_button_new_with_label(_("delete"));
  gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("delete selected tag"), (char *)NULL);
  gtk_box_pack_start(hbox, button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT (button), "clicked",
                   G_CALLBACK (delete_button_clicked), (gpointer)self);

  gtk_box_pack_start(box, GTK_WIDGET(hbox), FALSE, TRUE, 0);

  set_keyword(self, d);
}

void
gui_cleanup (dt_lib_module_t *self)
{
  darktable.gui->redraw_widgets = g_list_remove(darktable.gui->redraw_widgets, self->widget);
  free(self->data);
  self->data = NULL;
}
