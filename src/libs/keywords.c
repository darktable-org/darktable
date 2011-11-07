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
#include "common/tags.h"
#include "libs/lib.h"

DT_MODULE(1)

typedef struct dt_lib_keywords_t
{
  GtkTreeStore *store;
  GtkTreeView *view;
}
dt_lib_keywords_t;


/* callback for drag and drop */
static void _lib_keywords_drag_data_received_callback(GtkWidget *w,
					  GdkDragContext *dctx,
					  guint x,
					  guint y,
					  GtkSelectionData *data,
					  guint info,
					  guint time,
					  gpointer user_data);

/* set the data for drag and drop, eg the treeview path of drag source */
static void _lib_keywords_drag_data_get_callback(GtkWidget *w,
						 GdkDragContext *dctx,
						 GtkSelectionData *data,
						 guint info,
						 guint time,
						 gpointer user_data);

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

#define UNCATEGORIZED_TAG "uncategorized"

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
  memset(&uncategorized,0,sizeof(GtkTreeIter));

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), 
			      "select name,icon,description from tags", -1, &stmt, NULL);
  while (sqlite3_step(stmt) == SQLITE_ROW)
  {
    if(strchr((const char *)sqlite3_column_text(stmt, 0),'|')==0)
    {
      /* add uncategorized root iter if not exists */
      if (!uncategorized.stamp)
      {
	gtk_tree_store_insert(store, &uncategorized, NULL,0);
	gtk_tree_store_set(store, &uncategorized, 0, _(UNCATEGORIZED_TAG), -1);
      }

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

  /* setup dnd source and destination within treeview */
  static const GtkTargetEntry dnd_target = { "keywords-reorganize",
					     GTK_TARGET_SAME_WIDGET, 0};

  gtk_tree_view_enable_model_drag_source(d->view, 
					 GDK_BUTTON1_MASK, 
					 &dnd_target, 1, GDK_ACTION_MOVE);
  
  gtk_tree_view_enable_model_drag_dest(d->view, &dnd_target, 1, GDK_ACTION_MOVE);

  /* setup drag and drop signals */
  g_signal_connect(G_OBJECT(d->view),"drag-data-received",
			  G_CALLBACK(_lib_keywords_drag_data_received_callback),
			  self);

    g_signal_connect(G_OBJECT(d->view),"drag-data-get",
			  G_CALLBACK(_lib_keywords_drag_data_get_callback),
			  self);


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


static void _gtk_tree_move_iter(GtkTreeStore *store, GtkTreeIter *source, GtkTreeIter *dest)
{
  /* create copy of iter and insert into destinatation */
  GtkTreeIter ni;
  GValue value;
  memset(&value,0,sizeof(GValue));

  gtk_tree_model_get_value(GTK_TREE_MODEL(store), source, 0, &value);
  gtk_tree_store_insert(store, &ni, dest,0);
  gtk_tree_store_set(store, &ni, 0, g_strdup(g_value_get_string(&value)), -1);   

  /* for each children recurse into */
  int children = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(store), source);
  for (int k=0;k<children;k++)
  {
    GtkTreeIter child;
    if (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(store), &child, source, k))
      _gtk_tree_move_iter(store, &child, &ni);
  
  }

  /* iter copied lets remove source */
  gtk_tree_store_remove(store, source);

}

static void _lib_keywords_drag_data_get_callback(GtkWidget *w,
						 GdkDragContext *dctx,
						 GtkSelectionData *data,
						 guint info,
						 guint time,
						 gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_keywords_t *d = (dt_lib_keywords_t*)self->data;
  
  /* get iter of item to drag to ssetup drag data */
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeSelection *s = gtk_tree_view_get_selection(d->view);
  
  if (gtk_tree_selection_get_selected(s,&model,&iter))
  {

    /* get tree path as string out of iter into selection data */
    GtkTreePath *path = NULL;
    path = gtk_tree_model_get_path(model,&iter);
    gchar *sp = gtk_tree_path_to_string(path);
   
    gtk_selection_data_set(data,data->target, 8, (const guchar *)sp, strlen(sp));
  }

}

/* builds a keyword string out of GtkTreePath */
static void _lib_keywords_string_from_path(char *dest,size_t ds, 
					   GtkTreeModel *model, 
					   GtkTreePath *path)
{
  g_assert(model!=NULL);
  g_assert(path!=NULL);

  GList *components = NULL;
  GtkTreePath *wp = gtk_tree_path_copy(path);
  GtkTreeIter iter;

  /* get components of path */
  while (1)
  {
    GValue value;
    memset(&value,0,sizeof(GValue));

    /* get iter from path, break out if fail */
    if (!gtk_tree_model_get_iter(model, &iter, wp))
      break;

    /* add component to begin of list */
    gtk_tree_model_get_value(model, &iter, 0, &value);
    if ( !(gtk_tree_path_get_depth(wp) == 1 &&
	   strcmp(g_value_get_string(&value), _(UNCATEGORIZED_TAG)) == 0))
    {
      components = g_list_insert(components, 
				 g_strdup(g_value_get_string(&value)), 
				 0);
    }
    g_value_unset(&value);

    /* get parent of current path break out if we are at root */
    if (!gtk_tree_path_up(wp) || gtk_tree_path_get_depth(wp) == 0)
      break;
  }

  /* build the tag string from components */
  int dcs = 0;
  for(int i=0;i<g_list_length(components);i++) 
  {
    dcs += g_snprintf(dest+dcs, ds-dcs,
		      "%s%s",
		      (gchar *)g_list_nth_data(components, i),
		      (i < g_list_length(components)-1) ? "|" : "");
  }

  /* free data */
  gtk_tree_path_free(wp);
  

}

static void _lib_keywords_drag_data_received_callback(GtkWidget *w,
					  GdkDragContext *dctx,
					  guint x,
					  guint y,
					  GtkSelectionData *data,
					  guint info,
					  guint time,
					  gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_keywords_t *d = (dt_lib_keywords_t*)self->data;

  GtkTreePath *dpath;
  GtkTreeViewDropPosition dpos;
  GtkTreeModel *model = gtk_tree_view_get_model(d->view);

  if (data->format == 8)
  {
    if (gtk_tree_view_get_dest_row_at_pos(d->view, x, y, &dpath, &dpos))
    {
      /* fetch tree iter of source and dest dnd operation */
      GtkTreePath *spath = gtk_tree_path_new_from_string((char *)data->data);      
     
      char dtag[1024];
      char stag[1024];
      
      _lib_keywords_string_from_path(dtag, 1024, model, dpath);
      _lib_keywords_string_from_path(stag, 1024, model, spath);
      
      /* updated tags in database */
      dt_tag_reorganize(stag,dtag);

      /* lets move the source iter into dest iter */
      GtkTreeIter sit,dit;
      gtk_tree_model_get_iter(model, &sit, spath);
      gtk_tree_model_get_iter(model, &dit, dpath);
     
      _gtk_tree_move_iter(GTK_TREE_STORE(model), &sit, &dit);
      
      /* accept drop */
      gtk_drag_finish(dctx, TRUE, FALSE, time);

      
    }

  }   
 
  /* reject drop */
  gtk_drag_finish (dctx, FALSE, FALSE, time);
}
