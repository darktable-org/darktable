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
#include "common/styles.h"
#include "control/control.h"
#include "control/conf.h"
#include "control/jobs.h"
#include "gui/gtk.h"
#include "gui/styles.h"
#include "libs/lib.h"
#include <stdlib.h>
#include <gtk/gtk.h>
#include <glade/glade.h>
#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

typedef struct dt_lib_styles_t
{
  GtkEntry *entry;
  GtkWidget *duplicate;
  GtkTreeView *list;
}
dt_lib_styles_t;


const char*
name ()
{
  return _("styles");
}

uint32_t views() 
{
  return DT_LIGHTTABLE_VIEW;
}


void
gui_reset (dt_lib_module_t *self)
{
 // dt_lib_styles_t *d = (dt_lib_styles_t *)self->data;
}

int
position ()
{
  return 599;
}

typedef enum _styles_columns_t
{
  DT_STYLES_COL_NAME=0,
  DT_STYLES_COL_TOOLTIP,
  DT_STYLES_NUM_COLS
}
_styles_columns_t;

static int
get_font_height(GtkWidget *widget, const char *str)
{
  int width, height;

  PangoLayout *layout = pango_layout_new (gtk_widget_get_pango_context (widget));

  pango_layout_set_text(layout, str, -1);
  pango_layout_set_font_description(layout, NULL);
  pango_layout_get_pixel_size (layout, &width, &height);

  g_object_unref (layout);
  return height;
}


static void _gui_styles_update_view( dt_lib_styles_t *d)
{
  /* clear current list */
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->list));
  g_object_ref(model);
  gtk_tree_view_set_model(GTK_TREE_VIEW(d->list), NULL);
  gtk_list_store_clear(GTK_LIST_STORE(model));

  GList *result = dt_styles_get_list(gtk_entry_get_text(d->entry));
  if (result)
  {
    do {
      dt_style_t *style = (dt_style_t *)result->data;
      
      gtk_list_store_append (GTK_LIST_STORE(model), &iter);
      gtk_list_store_set (GTK_LIST_STORE(model), &iter,
                        DT_STYLES_COL_NAME, style->name,
                        DT_STYLES_COL_TOOLTIP, style->description,
                        -1);
      
      g_free(style->name);
      g_free(style->description);
      g_free(style);
    } while ((result=g_list_next(result))!=NULL);
  }

  gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(d->list), DT_STYLES_COL_TOOLTIP);
  gtk_tree_view_set_model(GTK_TREE_VIEW(d->list), model);
  g_object_unref(model);

}

static void
_styles_row_activated_callback (GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *col, gpointer user_data)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)user_data;
  
  GtkTreeModel *model;
  GtkTreeIter   iter;
  gtk_widget_set_size_request (GTK_WIDGET (d->list), -1, -1);
  model = gtk_tree_view_get_model (d->list);
  
  if (!gtk_tree_model_get_iter (model, &iter, path)) 
    return;

  gchar *name;
  gtk_tree_model_get (model, &iter, DT_STYLES_COL_NAME, &name, -1);
  
  if (name)
    dt_styles_apply_to_selection (name,gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (d->duplicate)));
  
}

#if 0
static void edit_clicked(GtkWidget *w,gpointer user_data)
{
    dt_lib_styles_t *d = (dt_lib_styles_t *)user_data;

  GtkTreeIter iter;
   GtkTreeModel *model;
  model = gtk_tree_view_get_model (d->list);
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->list));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;
  char *name=NULL;
  gtk_tree_model_get (model, &iter, 
                      DT_STYLES_COL_NAME, &name,
                      -1);
  if (name)
  {
    dt_gui_styles_dialog_edit (name);
     _gui_styles_update_view(d);
  }
}
#endif

static void delete_clicked(GtkWidget *w,gpointer user_data)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)user_data;

  GtkTreeIter iter;
  GtkTreeModel *model;
  model = gtk_tree_view_get_model (d->list);
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->list));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;
  char *name=NULL;
  gtk_tree_model_get (model, &iter, 
                      DT_STYLES_COL_NAME, &name,
                      -1);
  if (name)
  {
    dt_styles_delete_by_name (name);
    _gui_styles_update_view(d);
  }
}

static gboolean
entry_callback (GtkEntry *entry, gpointer user_data)
{
  _gui_styles_update_view(user_data);
  return FALSE;
}

static gboolean
duplicate_callback (GtkEntry *entry, gpointer user_data)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)user_data;
  dt_conf_set_bool ("ui_last/styles_create_duplicate", gtk_toggle_button_get_active ( GTK_TOGGLE_BUTTON (d->duplicate)));
  return FALSE;
}

void
gui_init (dt_lib_module_t *self)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)malloc (sizeof (dt_lib_styles_t));
  self->data = (void *)d;
  self->widget = gtk_vbox_new (FALSE, 5);
  GtkWidget *w;

  /* list */
  d->list = GTK_TREE_VIEW (gtk_tree_view_new ());
  gtk_tree_view_set_headers_visible(d->list,FALSE);
  GtkListStore *liststore = gtk_list_store_new (DT_STYLES_NUM_COLS, G_TYPE_STRING, G_TYPE_STRING);
  GtkTreeViewColumn *col = gtk_tree_view_column_new ();
  gtk_tree_view_append_column (GTK_TREE_VIEW (d->list), col);
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new ();
  gtk_tree_view_column_pack_start (col, renderer, TRUE);
  gtk_tree_view_column_add_attribute (col, renderer, "text", DT_STYLES_COL_NAME);

  int ht = get_font_height( GTK_WIDGET (d->list), "Dreggn");
  gtk_widget_set_size_request (GTK_WIDGET (d->list), -1, 5*ht);
  
  gtk_tree_selection_set_mode (gtk_tree_view_get_selection(GTK_TREE_VIEW(d->list)), GTK_SELECTION_SINGLE);
  gtk_tree_view_set_model (GTK_TREE_VIEW(d->list), GTK_TREE_MODEL(liststore));
  g_object_unref (liststore);
  
  gtk_object_set(GTK_OBJECT(d->list), "tooltip-text", _("available styles,\ndoubleclick to apply"), (char *)NULL);
  g_signal_connect (d->list, "row-activated", G_CALLBACK(_styles_row_activated_callback), d);
 
  /* filter entry */
  w = gtk_entry_new();
  d->entry=GTK_ENTRY(w);
  gtk_object_set(GTK_OBJECT(w), "tooltip-text", _("enter style name"), (char *)NULL);
  g_signal_connect (d->entry, "changed", G_CALLBACK(entry_callback),d);
  dt_gui_key_accel_block_on_focus ( GTK_WIDGET (d->entry));
  
  gtk_box_pack_start(GTK_BOX (self->widget),GTK_WIDGET (d->entry),TRUE,FALSE,0);
  gtk_box_pack_start(GTK_BOX (self->widget),GTK_WIDGET (d->list),TRUE,FALSE,0);

  GtkWidget *hbox=gtk_hbox_new (FALSE,5);
 
  GtkWidget *widget;
   
  d->duplicate = gtk_check_button_new_with_label(_("create duplicate"));
  gtk_box_pack_start(GTK_BOX (self->widget),GTK_WIDGET (d->duplicate),TRUE,FALSE,0);
  g_signal_connect (d->duplicate, "toggled", G_CALLBACK(duplicate_callback),d);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (d->duplicate), dt_conf_get_bool("ui_last/styles_create_duplicate"));
  g_object_set (d->duplicate, "tooltip-text", _("creates a duplicate of the image before applying style"), (char *)NULL);
 
#if 0
  // TODO: Unfinished stuff
  GtkWidget *widget=gtk_button_new_with_label(_("edit"));
  g_signal_connect (widget, "clicked", G_CALLBACK(edit_clicked),d);
  gtk_box_pack_start(GTK_BOX (hbox),widget,TRUE,TRUE,0);
#endif
  
  widget=gtk_button_new_with_label(_("delete"));
  g_signal_connect (widget, "clicked", G_CALLBACK(delete_clicked),d);
  g_object_set (widget, "tooltip-text", _("deletes the selected style in list above"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX (hbox),widget,TRUE,TRUE,0);
  gtk_box_pack_start(GTK_BOX (self->widget),hbox,TRUE,FALSE,0);
 
  
  /* update filtered list */
  _gui_styles_update_view(d);
  
}

void
gui_cleanup (dt_lib_module_t *self)
{
  free(self->data);
  self->data = NULL;
}


