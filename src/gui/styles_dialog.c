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
#include "common/debug.h"
#include "common/history.h"
#include "common/styles.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "gui/gtk.h"
#include "gui/styles.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

/* creates a styles dialog, if edit equals true id=styleid else id=imgid */
static void _gui_styles_dialog_run(gboolean edit, const char *name, int imgid);

typedef struct dt_gui_styles_dialog_t
{
  gboolean edit;
  int32_t imgid;
  gchar *nameorig;
  GtkWidget *name, *description, *duplicate;
  GtkTreeView *items;
  GtkTreeView *items_new;
} dt_gui_styles_dialog_t;


typedef enum _style_items_columns_t
{
  DT_STYLE_ITEMS_COL_ENABLED = 0,
  DT_STYLE_ITEMS_COL_UPDATE,
  DT_STYLE_ITEMS_COL_NAME,
  DT_STYLE_ITEMS_COL_NUM,
  DT_STYLE_ITEMS_COL_UPDATE_NUM,
  DT_STYLE_ITEMS_NUM_COLS
} _styles_columns_t;

static int _single_selected_imgid()
{
  int imgid = -1;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images", -1, &stmt,
                              NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    if(imgid == -1)
      imgid = sqlite3_column_int(stmt, 0);
    else
    {
      imgid = -1;
      break;
    }
  }
  sqlite3_finalize(stmt);
  return imgid;
}

void _gui_styles_get_active_items(dt_gui_styles_dialog_t *sd, GList **enabled, GList **update)
{
  /* run through all items and add active ones to result */
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(sd->items));
  int num = 0, update_num = 0;
  gboolean active, uactive;

  if(gtk_tree_model_get_iter_first(model, &iter))
  {
    do
    {
      gtk_tree_model_get(model, &iter, DT_STYLE_ITEMS_COL_ENABLED, &active, DT_STYLE_ITEMS_COL_UPDATE,
                         &uactive, DT_STYLE_ITEMS_COL_NUM, &num, DT_STYLE_ITEMS_COL_UPDATE_NUM, &update_num,
                         -1);
      if(active || uactive)
      {
        *enabled = g_list_append(*enabled, GINT_TO_POINTER(num));
        if(update != NULL)
        {
          if(uactive || num == -1)
            *update = g_list_append(*update, GINT_TO_POINTER(update_num));
          else
            *update = g_list_append(*update, GINT_TO_POINTER(-1));
        }
      }
    } while(gtk_tree_model_iter_next(model, &iter));
  }

  /* check for new items to be included */
  model = gtk_tree_view_get_model(GTK_TREE_VIEW(sd->items_new));
  if(gtk_tree_model_get_iter_first(model, &iter))
  {
    do
    {
      gtk_tree_model_get(model, &iter, DT_STYLE_ITEMS_COL_ENABLED, &active, DT_STYLE_ITEMS_COL_NUM, &num,
                         DT_STYLE_ITEMS_COL_UPDATE_NUM, &update_num, -1);
      if(active)
      {
        if(update_num == -1) // item from style
        {
          *enabled = g_list_append(*enabled, GINT_TO_POINTER(num));
          *update = g_list_append(*update, GINT_TO_POINTER(-1));
        }
        else // item from image
        {
          *update = g_list_append(*update, GINT_TO_POINTER(update_num));
          *enabled = g_list_append(*enabled, GINT_TO_POINTER(-1));
        }
      }
    } while(gtk_tree_model_iter_next(model, &iter));
  }
}

static void _gui_styles_new_style_response(GtkDialog *dialog, gint response_id, dt_gui_styles_dialog_t *g)
{
  if(response_id == GTK_RESPONSE_ACCEPT)
  {
    /* get the filtered list from dialog */
    GList *result = NULL;
    _gui_styles_get_active_items(g, &result, NULL);

    /* create the style from imageid */
    const gchar *name = gtk_entry_get_text(GTK_ENTRY(g->name));
    if(name && *name)
      if(dt_styles_create_from_image(name, gtk_entry_get_text(GTK_ENTRY(g->description)), g->imgid, result))
      {
        dt_control_log(_("style named '%s' successfully created"), name);
      };
  }
  gtk_widget_destroy(GTK_WIDGET(dialog));
  g_free(g->nameorig);
  g_free(g);
}

static void _gui_styles_edit_style_response(GtkDialog *dialog, gint response_id, dt_gui_styles_dialog_t *g)
{
  if(response_id == GTK_RESPONSE_ACCEPT)
  {
    /* get the filtered list from dialog */
    GList *result = NULL, *update = NULL;

    _gui_styles_get_active_items(g, &result, &update);

    const gchar *name = gtk_entry_get_text(GTK_ENTRY(g->name));
    if(name && *name)
    {
      if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->duplicate)))
      {
        dt_styles_create_from_style(g->nameorig, name, gtk_entry_get_text(GTK_ENTRY(g->description)), result,
                                    g->imgid, update);
        dt_control_log(_("style %s was successfully saved"), name);
      }
      else
      {
        dt_styles_update(g->nameorig, name, gtk_entry_get_text(GTK_ENTRY(g->description)), result, g->imgid,
                         update);
        dt_control_log(_("style %s was successfully saved"), name);
      }
    }
  }
  gtk_widget_destroy(GTK_WIDGET(dialog));
  g_free(g->nameorig);
  g_free(g);
}

static void _gui_styles_item_toggled(GtkCellRendererToggle *cell, gchar *path_str, gpointer data)
{
  dt_gui_styles_dialog_t *sd = (dt_gui_styles_dialog_t *)data;

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(sd->items));
  GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
  GtkTreeIter iter;
  gboolean toggle_item;
  int num, update_num;

  gtk_tree_model_get_iter(model, &iter, path);
  gtk_tree_model_get(model, &iter, DT_STYLE_ITEMS_COL_ENABLED, &toggle_item, DT_STYLE_ITEMS_COL_NUM, &num,
                     DT_STYLE_ITEMS_COL_UPDATE_NUM, &update_num, -1);

  toggle_item = (toggle_item == TRUE) ? FALSE : TRUE;

  if(update_num != -1 && toggle_item) // include so not updated
    gtk_list_store_set(GTK_LIST_STORE(model), &iter, DT_STYLE_ITEMS_COL_UPDATE, FALSE, -1);

  gtk_list_store_set(GTK_LIST_STORE(model), &iter, DT_STYLE_ITEMS_COL_ENABLED, toggle_item, -1);
  gtk_tree_path_free(path);
}

static void _gui_styles_item_new_toggled(GtkCellRendererToggle *cell, gchar *path_str, gpointer data)
{
  dt_gui_styles_dialog_t *sd = (dt_gui_styles_dialog_t *)data;

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(sd->items_new));
  GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
  GtkTreeIter iter;
  gboolean toggle_item;

  gtk_tree_model_get_iter(model, &iter, path);
  gtk_tree_model_get(model, &iter, DT_STYLE_ITEMS_COL_ENABLED, &toggle_item, -1);

  toggle_item = (toggle_item == TRUE) ? FALSE : TRUE;

  gtk_list_store_set(GTK_LIST_STORE(model), &iter, DT_STYLE_ITEMS_COL_ENABLED, toggle_item, -1);
  gtk_tree_path_free(path);
}

static void _gui_styles_update_toggled(GtkCellRendererToggle *cell, gchar *path_str, gpointer data)
{
  dt_gui_styles_dialog_t *sd = (dt_gui_styles_dialog_t *)data;

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(sd->items));
  GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
  GtkTreeIter iter;
  gboolean toggle_item;

  gtk_tree_model_get_iter(model, &iter, path);
  gtk_tree_model_get(model, &iter, DT_STYLE_ITEMS_COL_UPDATE, &toggle_item, -1);

  toggle_item = (toggle_item == TRUE) ? FALSE : TRUE;

  gtk_list_store_set(GTK_LIST_STORE(model), &iter, DT_STYLE_ITEMS_COL_ENABLED, !toggle_item, -1);
  gtk_list_store_set(GTK_LIST_STORE(model), &iter, DT_STYLE_ITEMS_COL_UPDATE, toggle_item, -1);
  gtk_tree_path_free(path);
}

void dt_gui_styles_dialog_new(int imgid)
{
  _gui_styles_dialog_run(FALSE, NULL, imgid);
}

void dt_gui_styles_dialog_edit(const char *name)
{
  _gui_styles_dialog_run(TRUE, name, _single_selected_imgid());
}

static gint _g_list_find_module_by_name(gconstpointer a, gconstpointer b)
{
  return strncmp(((dt_iop_module_t *)a)->op, b, strlen(((dt_iop_module_t *)a)->op));
}

static void _gui_styles_dialog_run(gboolean edit, const char *name, int imgid)
{
  char title[512];

  /* check if style exists */
  if(name && (dt_styles_exists(name)) == 0) return;

  /* initialize the dialog */
  dt_gui_styles_dialog_t *sd = (dt_gui_styles_dialog_t *)g_malloc(sizeof(dt_gui_styles_dialog_t));
  sd->nameorig = g_strdup(name);
  sd->imgid = imgid;

  if(edit)
  {
    snprintf(title, sizeof(title), "%s", _("edit style"));
    g_strlcat(title, " \"", sizeof(title));
    g_strlcat(title, name, sizeof(title));
    g_strlcat(title, "\"", sizeof(title));
    sd->duplicate = gtk_check_button_new_with_label(_("duplicate style"));
    gtk_widget_set_tooltip_text(sd->duplicate, _("creates a duplicate of the style before applying changes"));
  }
  else
  {
    snprintf(title, sizeof(title), "%s", _("create new style"));
    sd->duplicate = NULL;
  }
  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
  GtkDialog *dialog = GTK_DIALOG(
      gtk_dialog_new_with_buttons(title, GTK_WINDOW(window), GTK_DIALOG_DESTROY_WITH_PARENT, _("_cancel"),
                                  GTK_RESPONSE_REJECT, _("_save"), GTK_RESPONSE_ACCEPT, NULL));
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(GTK_WIDGET(dialog));
#endif

  GtkContainer *content_area = GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog)));
  GtkBox *box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_PIXEL_APPLY_DPI(5)));
  gtk_widget_set_margin_start(GTK_WIDGET(box), DT_PIXEL_APPLY_DPI(10));
  gtk_widget_set_margin_end(GTK_WIDGET(box), DT_PIXEL_APPLY_DPI(10));
  gtk_widget_set_margin_top(GTK_WIDGET(box), DT_PIXEL_APPLY_DPI(10));
  gtk_widget_set_margin_bottom(GTK_WIDGET(box), DT_PIXEL_APPLY_DPI(10));
  gtk_container_add(content_area, GTK_WIDGET(box));

  sd->name = gtk_entry_new();
  gtk_widget_set_tooltip_text(sd->name, _("enter a name for the new style"));

  sd->description = gtk_entry_new();
  gtk_widget_set_tooltip_text(sd->description,
                              _("enter a description for the new style, this description is searchable"));

  /*set values*/
  if(edit && name)
  {
    /* name */
    gtk_entry_set_text(GTK_ENTRY(sd->name), name);
    /* description */
    gchar *desc = dt_styles_get_description(name);
    if(desc)
    {
      gtk_entry_set_text(GTK_ENTRY(sd->description), desc);
      g_free(desc);
    }
  }

  gtk_box_pack_start(box, sd->name, FALSE, FALSE, 0);
  gtk_box_pack_start(box, sd->description, FALSE, FALSE, 0);

  /* create the list of items */
  sd->items = GTK_TREE_VIEW(gtk_tree_view_new());
  GtkListStore *liststore = gtk_list_store_new(DT_STYLE_ITEMS_NUM_COLS, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN,
                                               G_TYPE_STRING, G_TYPE_INT, G_TYPE_INT);

  sd->items_new = GTK_TREE_VIEW(gtk_tree_view_new());
  GtkListStore *liststore_new = gtk_list_store_new(DT_STYLE_ITEMS_NUM_COLS, G_TYPE_BOOLEAN, G_TYPE_STRING,
                                                   G_TYPE_STRING, G_TYPE_INT, G_TYPE_INT);

  /* enabled */
  GtkCellRenderer *renderer = gtk_cell_renderer_toggle_new();
  gtk_cell_renderer_toggle_set_activatable(GTK_CELL_RENDERER_TOGGLE(renderer), TRUE);
  g_object_set_data(G_OBJECT(renderer), "column", (gint *)DT_STYLE_ITEMS_COL_ENABLED);
  g_signal_connect(renderer, "toggled", G_CALLBACK(_gui_styles_item_toggled), sd);

  gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(sd->items), -1, _("include"), renderer, "active",
                                              DT_STYLE_ITEMS_COL_ENABLED, NULL);

  if(edit)
  {
    renderer = gtk_cell_renderer_toggle_new();
    gtk_cell_renderer_toggle_set_activatable(GTK_CELL_RENDERER_TOGGLE(renderer), TRUE);
    g_object_set_data(G_OBJECT(renderer), "column", (gint *)DT_STYLE_ITEMS_COL_ENABLED);
    g_signal_connect(renderer, "toggled", G_CALLBACK(_gui_styles_item_new_toggled), sd);
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(sd->items_new), -1, _("include"), renderer,
                                                "active", DT_STYLE_ITEMS_COL_ENABLED, NULL);
  }

  /* update */
  if(edit && imgid != -1)
  {
    renderer = gtk_cell_renderer_toggle_new();
    gtk_cell_renderer_toggle_set_activatable(GTK_CELL_RENDERER_TOGGLE(renderer), TRUE);
    g_object_set_data(G_OBJECT(renderer), "column", (gint *)DT_STYLE_ITEMS_COL_UPDATE);
    g_signal_connect(renderer, "toggled", G_CALLBACK(_gui_styles_update_toggled), sd);

    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(sd->items), -1, _("update"), renderer, "active",
                                                DT_STYLE_ITEMS_COL_UPDATE, NULL);
  }

  /* name */
  renderer = gtk_cell_renderer_text_new();
  g_object_set_data(G_OBJECT(renderer), "column", (gint *)DT_STYLE_ITEMS_COL_NAME);
  g_object_set(renderer, "xalign", 0.0, (gchar *)0);
  gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(sd->items), -1, _("item"), renderer, "text",
                                              DT_STYLE_ITEMS_COL_NAME, NULL);
  if(edit)
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(sd->items_new), -1, _("item"), renderer, "text",
                                                DT_STYLE_ITEMS_COL_NAME, NULL);

  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(sd->items)), GTK_SELECTION_SINGLE);
  gtk_tree_view_set_model(GTK_TREE_VIEW(sd->items), GTK_TREE_MODEL(liststore));

  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(sd->items_new)), GTK_SELECTION_SINGLE);
  gtk_tree_view_set_model(GTK_TREE_VIEW(sd->items_new), GTK_TREE_MODEL(liststore_new));

  gboolean has_new_item = FALSE, has_item = FALSE;

  /* fill list with history items */
  GtkTreeIter iter;
  if(edit)
  {
    /* get history items for named style and populate the items list */
    GList *items = dt_styles_get_item_list(name, FALSE, imgid);
    if(items)
    {
      do
      {
        dt_style_item_t *item = (dt_style_item_t *)items->data;

        if(item->num != -1 && item->selimg_num != -1) // defined in style and image
        {
          gtk_list_store_append(GTK_LIST_STORE(liststore), &iter);
          gtk_list_store_set(GTK_LIST_STORE(liststore), &iter, DT_STYLE_ITEMS_COL_ENABLED, TRUE,
                             DT_STYLE_ITEMS_COL_UPDATE, FALSE, DT_STYLE_ITEMS_COL_NAME, item->name,
                             DT_STYLE_ITEMS_COL_NUM, item->num, DT_STYLE_ITEMS_COL_UPDATE_NUM,
                             item->selimg_num, -1);
          has_item = TRUE;
        }
        else if(item->num != -1
                || item->selimg_num != -1) // defined in one or the other, let a way to select it or not
        {
          gtk_list_store_append(GTK_LIST_STORE(liststore_new), &iter);
          gtk_list_store_set(GTK_LIST_STORE(liststore_new), &iter, DT_STYLE_ITEMS_COL_ENABLED,
                             item->num != -1 ? TRUE : FALSE, DT_STYLE_ITEMS_COL_NAME, item->name,
                             DT_STYLE_ITEMS_COL_NUM, item->num, DT_STYLE_ITEMS_COL_UPDATE_NUM,
                             item->selimg_num, -1);
          has_new_item = TRUE;
        }
      } while((items = g_list_next(items)));
      g_list_free_full(items, dt_style_item_free);
    }
  }
  else
  {
    GList *items = dt_history_get_items(imgid, FALSE);
    if(items)
    {
      do
      {
        dt_history_item_t *item = (dt_history_item_t *)items->data;

        /* lookup history item module */
        gboolean enabled = TRUE;
        dt_iop_module_t *module = NULL;
        GList *modules = g_list_first(darktable.develop->iop);
        if(modules)
        {
          GList *result = g_list_find_custom(
              modules, item->op, _g_list_find_module_by_name); // (dt_iop_module_t *)(modules->data);
          if(result)
          {
            module = (dt_iop_module_t *)(result->data);
            enabled = (module->flags() & IOP_FLAGS_INCLUDE_IN_STYLES) ? TRUE : FALSE;
          }
        }

        gchar iname[256] = { 0 };
        g_snprintf(iname, sizeof(iname), "%s", item->name);

        gtk_list_store_append(GTK_LIST_STORE(liststore), &iter);
        gtk_list_store_set(GTK_LIST_STORE(liststore), &iter, DT_STYLE_ITEMS_COL_ENABLED, enabled,
                           DT_STYLE_ITEMS_COL_NAME, iname, DT_STYLE_ITEMS_COL_NUM, item->num, -1);

        has_item = TRUE;

      } while((items = g_list_next(items)));
      g_list_free_full(items, dt_history_item_free);
    }
    else
    {
      dt_control_log(_("can't create style out of unaltered image"));
      return;
    }
  }

  if(has_item) gtk_box_pack_start(box, GTK_WIDGET(sd->items), TRUE, TRUE, 0);

  if(has_new_item) gtk_box_pack_start(box, GTK_WIDGET(sd->items_new), TRUE, TRUE, 0);

  if(edit) gtk_box_pack_start(box, GTK_WIDGET(sd->duplicate), FALSE, FALSE, 0);

  g_object_unref(liststore);
  g_object_unref(liststore_new);

  /* run dialog */
  if(edit)
    g_signal_connect(dialog, "response", G_CALLBACK(_gui_styles_edit_style_response), sd);
  else
    g_signal_connect(dialog, "response", G_CALLBACK(_gui_styles_new_style_response), sd);

  gtk_widget_show_all(GTK_WIDGET(dialog));
  gtk_dialog_run(GTK_DIALOG(dialog));
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
