/*
    This file is part of darktable,
    copyright (c) 2009--2010 henrik andersson.

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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "common/darktable.h"
#include "common/styles.h"
#include "common/history.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "gui/styles.h"
#include "gui/gtk.h"

/* creates a styles dialog, if edit equals true id=styleid else id=imgid */
static void _gui_styles_dialog_run (gboolean edit,const char *name,int imgid);

typedef struct dt_gui_styles_dialog_t
{
  gboolean edit;
  int32_t imgid;
  GtkWidget *name,*description;
  GtkTreeView *items;
} dt_gui_styles_dialog_t;


typedef enum _style_items_columns_t
{
  DT_STYLE_ITEMS_COL_ENABLED=0,
  DT_STYLE_ITEMS_COL_NAME,
  DT_STYLE_ITEMS_COL_NUM,
  DT_STYLE_ITEMS_NUM_COLS
}
_styles_columns_t;

static GList *
_gui_styles_get_active_items (dt_gui_styles_dialog_t *sd)
{
  GList *result=NULL;

  /* run thru all items and add active ones to result */
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model (GTK_TREE_VIEW (sd->items));
  if (gtk_tree_model_get_iter_first(model,&iter))
  {
    do
    {
      gboolean active;
      guint num=0;
      gtk_tree_model_get (model, &iter, DT_STYLE_ITEMS_COL_ENABLED, &active, DT_STYLE_ITEMS_COL_NUM, &num, -1);
      if (active)
        result = g_list_append (result, (gpointer)(long unsigned int) num);

    }
    while (gtk_tree_model_iter_next (model,&iter));
  }

  //gtk_tree_model_get_iter (model, &iter, path);
// gtk_tree_model_get (model, &iter, DT_STYLE_ITEMS_COL_ENABLED, &toggle_item, -1);

  return result;
}

static void
_gui_styles_new_style_response(GtkDialog *dialog, gint response_id, dt_gui_styles_dialog_t *g)
{
  if (response_id == GTK_RESPONSE_ACCEPT)
  {
    /* get the filtered list from dialog */
    GList *result = _gui_styles_get_active_items(g);

    /* create the style from imageid */
    if (gtk_entry_get_text ( GTK_ENTRY (g->name)) && strlen(gtk_entry_get_text ( GTK_ENTRY (g->name)))>0)
      dt_styles_create_from_image(
        gtk_entry_get_text ( GTK_ENTRY (g->name)),
        gtk_entry_get_text ( GTK_ENTRY (g->description)),
        g->imgid,result);
  }
  gtk_widget_destroy(GTK_WIDGET(dialog));
  g_free(g);
}

static void
_gui_styles_edit_style_response(GtkDialog *dialog, gint response_id, dt_gui_styles_dialog_t *g)
{
  if (response_id == GTK_RESPONSE_ACCEPT)
  {
    /* get the filtered list from dialog */
    //GList *result = _gui_styles_get_active_items(g);


  }
  gtk_widget_destroy(GTK_WIDGET(dialog));
  g_free(g);
}

static void
_gui_styles_item_toggled (GtkCellRendererToggle *cell,
                          gchar                 *path_str,
                          gpointer               data)
{
  dt_gui_styles_dialog_t *sd = (dt_gui_styles_dialog_t *)data;

  GtkTreeModel *model = gtk_tree_view_get_model (GTK_TREE_VIEW (sd->items));
  GtkTreePath *path = gtk_tree_path_new_from_string (path_str);
  GtkTreeIter iter;
  gboolean toggle_item;

  gtk_tree_model_get_iter (model, &iter, path);
  gtk_tree_model_get (model, &iter, DT_STYLE_ITEMS_COL_ENABLED, &toggle_item, -1);

  toggle_item = (toggle_item==TRUE)?FALSE:TRUE;

  gtk_list_store_set (GTK_LIST_STORE (model), &iter, DT_STYLE_ITEMS_COL_ENABLED, toggle_item, -1);
  gtk_tree_path_free (path);

}

void
dt_gui_styles_dialog_new (int imgid)
{
  _gui_styles_dialog_run (FALSE,NULL,imgid);
}

void
dt_gui_styles_dialog_edit (const char *name)
{
  _gui_styles_dialog_run (TRUE,name,0);
}

static gint _g_list_find_module_by_name(gconstpointer a, gconstpointer b)
{
  return strncmp (((dt_iop_module_t*)a)->op, b, strlen (((dt_iop_module_t*)a)->op) );
}

static void
_gui_styles_dialog_run (gboolean edit,const char *name,int imgid)
{
  char title[512];

  /* check if style exists */
  if (name && (dt_styles_exists (name))==0)
    return;

  /* initialize the dialog */
  dt_gui_styles_dialog_t *sd=(dt_gui_styles_dialog_t *)g_malloc (sizeof (dt_gui_styles_dialog_t));

  if (edit)
    sprintf (title,_("edit style"));
  else
  {
    sd->imgid = imgid;
    sprintf (title,"%s",_("create new style"));
  }
  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
  GtkDialog *dialog = GTK_DIALOG (gtk_dialog_new_with_buttons (title,
                                  GTK_WINDOW(window),
                                  GTK_DIALOG_DESTROY_WITH_PARENT,
                                  GTK_STOCK_CANCEL,
                                  GTK_RESPONSE_REJECT,
                                  GTK_STOCK_SAVE,
                                  GTK_RESPONSE_ACCEPT,
                                  NULL));

  GtkContainer *content_area = GTK_CONTAINER (gtk_dialog_get_content_area (GTK_DIALOG (dialog)));
  GtkWidget *alignment = gtk_alignment_new (0.5, 0.5, 1.0, 1.0);
  gtk_alignment_set_padding (GTK_ALIGNMENT(alignment), 5, 5, 5, 5);
  gtk_container_add (content_area, alignment);
  GtkBox *box = GTK_BOX (gtk_vbox_new(FALSE, 5));
  gtk_container_add (GTK_CONTAINER (alignment), GTK_WIDGET (box));

  sd->name = gtk_entry_new();
  g_object_set (sd->name, "tooltip-text", _("enter a name for the new style"), (char *)NULL);

  sd->description = gtk_entry_new();
  g_object_set (sd->description, "tooltip-text", _("enter a description for the new style, this description is searchable"), (char *)NULL);

  /*set values*/
  if (edit)
  {
    /* name */
    gtk_entry_set_text (GTK_ENTRY (sd->name),name);
    gtk_widget_set_sensitive (sd->name,FALSE);
    /* description */
    gchar *desc = dt_styles_get_description (name);
    if (desc)
    {
      gtk_entry_set_text (GTK_ENTRY (sd->description),desc);
      g_free (desc);
    }
  }

  gtk_box_pack_start (box,sd->name,FALSE,FALSE,0);
  gtk_box_pack_start (box,sd->description,FALSE,FALSE,0);

  /* create the list of items */
  sd->items = GTK_TREE_VIEW (gtk_tree_view_new ());
  GtkListStore *liststore = gtk_list_store_new (DT_STYLE_ITEMS_NUM_COLS, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_UINT);

  /* enabled */
  GtkCellRenderer *renderer = gtk_cell_renderer_toggle_new ();
  gtk_cell_renderer_toggle_set_activatable (GTK_CELL_RENDERER_TOGGLE (renderer), TRUE);
  g_object_set_data (G_OBJECT (renderer), "column", (gint *)DT_STYLE_ITEMS_COL_ENABLED);
  g_signal_connect (renderer, "toggled", G_CALLBACK (_gui_styles_item_toggled), sd);

  gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (sd->items),
      -1, _("include"),
      renderer,
      "active",
      DT_STYLE_ITEMS_COL_ENABLED,
      NULL);

  /* name */
  renderer = gtk_cell_renderer_text_new ();
  g_object_set_data (G_OBJECT (renderer), "column", (gint *)DT_STYLE_ITEMS_COL_NAME);
  g_object_set (renderer, "xalign", 0.0, NULL);
  gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (sd->items),
      -1, _("item"),
      renderer,
      "text",
      DT_STYLE_ITEMS_COL_NAME,
      NULL);


  gtk_tree_selection_set_mode (gtk_tree_view_get_selection(GTK_TREE_VIEW(sd->items)), GTK_SELECTION_SINGLE);
  gtk_tree_view_set_model (GTK_TREE_VIEW(sd->items), GTK_TREE_MODEL(liststore));

  gtk_box_pack_start (box,GTK_WIDGET (sd->items),TRUE,TRUE,0);


  /* fill list with history items */
  GtkTreeIter iter;
  if (edit)
  {
    /* get history items for named style and populate the items list */
    GList *items = dt_styles_get_item_list (name);
    if (items)
    {
      do
      {
        dt_style_item_t *item=(dt_style_item_t *)items->data;

        gtk_list_store_append (GTK_LIST_STORE(liststore), &iter);
        gtk_list_store_set (GTK_LIST_STORE(liststore), &iter,
                            DT_STYLE_ITEMS_COL_ENABLED, TRUE,
                            DT_STYLE_ITEMS_COL_NAME, item->name,
                            DT_STYLE_ITEMS_COL_NUM, (guint)item->num,
                            -1);

        g_free(item->name);
        g_free(item);
      }
      while ((items=g_list_next(items)));
    }
  }
  else
  {
    GList *items = dt_history_get_items (imgid);
    if (items)
    {
      do
      {
        dt_history_item_t *item = (dt_history_item_t *)items->data;

        /* lookup history item module */
        gboolean enabled = TRUE;
        dt_iop_module_t *module=NULL;
        GList *modules = g_list_first(darktable.develop->iop);
        if (modules)
        {
          GList *result = g_list_find_custom (modules, item->op, _g_list_find_module_by_name); // (dt_iop_module_t *)(modules->data);
          if( result )
          {
            module = (dt_iop_module_t *)(result->data);
            enabled  = (module->flags() & IOP_FLAGS_INCLUDE_IN_STYLES)?TRUE:FALSE;
          }
        }

        gchar name[256]= {0};
        g_snprintf(name,256,"%s",item->name);

        gtk_list_store_append (GTK_LIST_STORE(liststore), &iter);
        gtk_list_store_set (GTK_LIST_STORE(liststore), &iter,
                            DT_STYLE_ITEMS_COL_ENABLED, enabled,
                            DT_STYLE_ITEMS_COL_NAME, name,
                            DT_STYLE_ITEMS_COL_NUM, (guint)item->num,
                            -1);

        g_free(item->op);
        g_free(item->name);
        g_free(item);
      }
      while ((items=g_list_next(items)));
    }
    else
    {
      dt_control_log(_("can't create style out of unaltered image"));
      return;
    }
  }

  g_object_unref (liststore);


  /* run dialog */
  if (edit)
    g_signal_connect (dialog, "response", G_CALLBACK (_gui_styles_edit_style_response), sd);
  else
    g_signal_connect (dialog, "response", G_CALLBACK (_gui_styles_new_style_response), sd);

  gtk_widget_show_all (GTK_WIDGET (dialog));

}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
