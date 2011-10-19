/*
    This file is part of darktable,
    copyright (c) 2011 Henrik Andersson.

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
#include "common/debug.h"
#include "libs/lib.h"

DT_MODULE(1)

typedef struct dt_lib_keywords_t
{
  GtkTreeStore *store;
  GtkTreeView *view;
}
dt_lib_keywords_t;

const char* name()
{
  return _("keywords");
}


uint32_t views()
{
  return DT_VIEW_LIGHTTABLE;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position()
{
  return 399;
}

void init_key_accels(dt_lib_module_t *self)
{
  //  dt_accel_register_lib(self, NC_("accel", "scan for devices"), 0, 0);
}

void connect_key_accels(dt_lib_module_t *self)
{
  /*  dt_lib_import_t *d = (dt_lib_import_t*)self->data;

  dt_accel_connect_button_lib(self, "scan for devices",
                              GTK_WIDGET(d->scan_devices)); */
}


void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_keywords_t *d = (dt_lib_keywords_t *)g_malloc(sizeof(dt_lib_keywords_t));
  memset(d,0,sizeof(dt_lib_keywords_t));
  self->data = (void *)d;
  self->widget = gtk_vbox_new(FALSE, 5);

  GtkTreeStore *store = gtk_tree_store_new(1, G_TYPE_STRING);

  /* intialize the tree store with known tags */
  sqlite3_stmt *stmt;

  GtkTreeIter uncategorized, temp;  



  /* base tree iters */
  gtk_tree_store_insert(store, &uncategorized, NULL,0);
  gtk_tree_store_set(store, &uncategorized, 0, _("uncategorized"), -1);
  
  
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), 
			      "select name,icon,description from tags", -1, &stmt, NULL);
  while (sqlite3_step(stmt) == SQLITE_ROW)
  {
    if(strchr((const char *)sqlite3_column_text(stmt, 0),'|')==0)
    {
      /* adding a uncategorized tag */
      gtk_tree_store_insert(store, &temp, &uncategorized,0);
      gtk_tree_store_set(store, &temp, 0, sqlite3_column_text(stmt, 0), -1); 
    }
    else
    {
      int level = 0;
      char *value;
      GtkTreeIter current,iter;
      char *pch = strtok((char *)sqlite3_column_text(stmt, 0),"|");
      while (pch != NULL) 
      {
	gboolean found=FALSE;
	int children = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(store),level>0?&current:NULL);
	/* find child with name, if not found create and continue */
	for (int k=0;k<children;k++)
	{
	  if (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(store), &iter, level>0?&current:NULL, k))
	  {
	    gtk_tree_model_get (GTK_TREE_MODEL(store), &iter, 0, &value, -1);
	    
	    if (strcmp(value, pch)==0)
	    {
	      current = iter;
	      found = TRUE;
	      break;
	    }
	  }
	}

	/* lets add new keyword and assign current */
	if (!found)
	{
	  gtk_tree_store_insert(store, &iter, level>0?&current:NULL,0);
	  gtk_tree_store_set(store, &iter, 0, pch, -1);
	  current = iter;
	}

	level++;
	pch = strtok(NULL, "|");
      }
      
    }
    
  }

  /* add the treeview to show hirarchy tags*/
  GtkCellRenderer *renderer;

  d->view = GTK_TREE_VIEW (gtk_tree_view_new());

  renderer = gtk_cell_renderer_text_new ();
  gtk_tree_view_insert_column_with_attributes(d->view,
                                               -1,      
                                               "",  
                                               renderer,
                                               "text", 0,
                                               NULL);

  gtk_tree_view_set_headers_visible(d->view, FALSE);

  gtk_tree_view_set_model(d->view, GTK_TREE_MODEL(store));

  /* free store, treeview has its own storage now */
  g_object_unref(store);
  
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->view), TRUE,FALSE,0);

  gtk_widget_show_all(GTK_WIDGET(d->view));
  

}

void gui_cleanup(dt_lib_module_t *self)
{ 
  // dt_lib_import_t *d = (dt_lib_import_t*)self->data;

  /* cleanup mem */
  g_free(self->data);
  self->data = NULL;
}
