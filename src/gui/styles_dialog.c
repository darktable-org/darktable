/*
    This file is part of darktable,
    Copyright (C) 2010-2024 darktable developers.

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
#include "common/utility.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "gui/gtk.h"
#include "gui/draw.h"
#include "gui/styles.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

/* creates a styles dialog, if edit equals true id=styleid else id=imgid */
static void _gui_styles_dialog_run(gboolean edit,
                                   const char *name,
                                   const dt_imgid_t imgid,
                                   char **new_name);

typedef struct dt_gui_styles_dialog_t
{
  gboolean edit;
  dt_imgid_t imgid;
  gchar *nameorig;
  gchar **newname;
  GtkWidget *name, *description, *duplicate;
  GtkTreeView *items;
  GtkTreeView *items_new;
} dt_gui_styles_dialog_t;


typedef enum _style_items_columns_t
{
  DT_STYLE_ITEMS_COL_ENABLED = 0,
  DT_STYLE_ITEMS_COL_UPDATE,
  DT_STYLE_ITEMS_COL_ISACTIVE,
  DT_STYLE_ITEMS_COL_AUTOINIT,
  DT_STYLE_ITEMS_COL_NAME,
  DT_STYLE_ITEMS_COL_MASK,
  DT_STYLE_ITEMS_COL_NUM,
  DT_STYLE_ITEMS_COL_UPDATE_NUM,
  DT_STYLE_ITEMS_NUM_COLS
} _styles_columns_t;

static int _single_selected_imgid()
{
  dt_imgid_t imgid = NO_IMGID;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT imgid FROM main.selected_images",
                              -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    if(!dt_is_valid_imgid(imgid))
      imgid = sqlite3_column_int(stmt, 0);
    else
    {
      imgid = NO_IMGID;
      break;
    }
  }
  sqlite3_finalize(stmt);
  return imgid;
}

static gboolean _gui_styles_is_copy_module_order_set(dt_gui_styles_dialog_t *d)
{
  /* first item is the copy-module */
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->items));

  gboolean active = FALSE;
  gint num = 0;
  if(gtk_tree_model_get_iter_first(model, &iter))
    gtk_tree_model_get(model, &iter,
                       DT_STYLE_ITEMS_COL_ENABLED, &active,
                       DT_STYLE_ITEMS_COL_NUM, &num, -1);
  return active && (num == -1);
}

static gboolean _gui_styles_is_update_module_order_set(dt_gui_styles_dialog_t *d)
{
  /* first item is the copy-module */
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->items));

  gboolean active = FALSE;
  gint num = 0;
  if(gtk_tree_model_get_iter_first(model, &iter))
    gtk_tree_model_get(model, &iter,
                       DT_STYLE_ITEMS_COL_UPDATE, &active,
                       DT_STYLE_ITEMS_COL_NUM, &num, -1);
  return active && (num == -1);
}

void _gui_styles_get_active_items(dt_gui_styles_dialog_t *sd,
                                  GList **enabled,
                                  GList **update)
{
  /* run through all items and add active ones to result */
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(sd->items));
  gint num = 0, update_num = 0;
  gboolean active, uactive, autoinit;

  if(gtk_tree_model_get_iter_first(model, &iter))
  {
    do
    {
      gtk_tree_model_get(model, &iter,
                         DT_STYLE_ITEMS_COL_ENABLED, &active,
                         DT_STYLE_ITEMS_COL_UPDATE, &uactive,
                         DT_STYLE_ITEMS_COL_NUM, &num,
                         DT_STYLE_ITEMS_COL_UPDATE_NUM, &update_num,
                         DT_STYLE_ITEMS_COL_AUTOINIT, &autoinit,
                         -1);
      if((active || uactive) && num >= 0)
      {
        *enabled = g_list_append(*enabled, GINT_TO_POINTER(autoinit ? -num : num));
        if(update != NULL)
        {
          if(uactive)
            *update = g_list_append(*update, GINT_TO_POINTER(update_num));
          else
            *update = g_list_append(*update, GINT_TO_POINTER(0));
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
      gtk_tree_model_get(model, &iter,
                         DT_STYLE_ITEMS_COL_ENABLED, &active,
                         DT_STYLE_ITEMS_COL_NUM, &num,
                         DT_STYLE_ITEMS_COL_UPDATE_NUM, &update_num,
                         DT_STYLE_ITEMS_COL_AUTOINIT, &autoinit,
                         -1);
      if(active)
      {
        if(update_num == -1) // item from style
        {
          *enabled = g_list_append(*enabled, GINT_TO_POINTER(num));
          *update = g_list_append(*update, GINT_TO_POINTER(0));
        }
        else // item from image
        {
          *update = g_list_append(*update,
                                  GINT_TO_POINTER(autoinit ? -update_num : update_num));
          *enabled = g_list_append(*enabled, GINT_TO_POINTER(0));
        }
      }
    } while(gtk_tree_model_iter_next(model, &iter));
  }
}

static void _gui_styles_select_all_items(dt_gui_styles_dialog_t *d, const gboolean active)
{
  /* run through all items and set active status */
  GtkTreeView *items = (d->duplicate) ? d->items_new : d->items;
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(items));
  if(gtk_tree_model_get_iter_first(model, &iter))
  {
    do
    {
      gtk_list_store_set(GTK_LIST_STORE(model), &iter,
                         DT_STYLE_ITEMS_COL_ENABLED, active, -1);
    } while(gtk_tree_model_iter_next(model, &iter));
  }
}

static void _gui_styles_new_style_response(GtkDialog *dialog,
                                           const gint response_id,
                                           dt_gui_styles_dialog_t *g)
{
  if(response_id == GTK_RESPONSE_YES)
  {
    _gui_styles_select_all_items(g, TRUE);
    return;
  }
  else if(response_id == GTK_RESPONSE_NONE)
  {
    _gui_styles_select_all_items(g, FALSE);
    return;
  }
  else if(response_id == GTK_RESPONSE_ACCEPT)
  {
    /* get the filtered list from dialog */
    GList *result = NULL;
    _gui_styles_get_active_items(g, &result, NULL);

    /* create the style from imageid */
    char *newname = g_strdup(gtk_entry_get_text(GTK_ENTRY(g->name)));

    if(g->newname)
      *g->newname = newname;

    if(newname && *newname)
    {
      /* show prompt dialog when style already exists */
      if(g->newname && (dt_styles_exists(newname)) != 0)
      {
        /* on button yes delete style name for overwriting */
        if(dt_gui_show_yes_no_dialog
           (_("overwrite style?"),
            _("style `%s' already exists.\ndo you want to overwrite?"), newname))
        {
          dt_styles_delete_by_name(newname);
        }
        else
        {
         /* on RESPONSE_NO and escape key return to dialog */
          return;
        }
      }

      if(dt_styles_create_from_image(newname,
                                     gtk_entry_get_text(GTK_ENTRY(g->description)),
                                     g->imgid,
                                     result,
                                     _gui_styles_is_copy_module_order_set(g)))
      {
        dt_control_log(_("style named '%s' successfully created"), newname);
      };
    }
    else
    {
      /* show dialog if name is missing from entry */
      GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
      GtkWidget *dlg_changename =
        gtk_message_dialog_new(GTK_WINDOW(window),
                               GTK_DIALOG_DESTROY_WITH_PARENT,
                               GTK_MESSAGE_WARNING,
                               GTK_BUTTONS_OK,
                               _("please give style a name"));
#ifdef GDK_WINDOWING_QUARTZ
      dt_osx_disallow_fullscreen(dlg_changename);
#endif
      gtk_window_set_title(GTK_WINDOW(dlg_changename), _("unnamed style"));
      gtk_dialog_run(GTK_DIALOG(dlg_changename));
      gtk_widget_destroy(dlg_changename);
      return;
    }
  }

  // finalize the dialog
  g_free(g->nameorig);
  g_free(g);

  gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void _gui_styles_edit_style_response(GtkDialog *dialog,
                                            const gint response_id,
                                            dt_gui_styles_dialog_t *g)
{
  if(response_id == GTK_RESPONSE_YES)
  {
    _gui_styles_select_all_items(g, TRUE);
    return;
  }
  else if(response_id == GTK_RESPONSE_NONE)
  {
    _gui_styles_select_all_items(g, FALSE);
    return;
  }

  char *newname = g_strdup(gtk_entry_get_text(GTK_ENTRY(g->name)));

  if(g->newname)
    *g->newname = newname;

  if(response_id == GTK_RESPONSE_ACCEPT)
  {
    /* get the filtered list from dialog */
    GList *result = NULL, *update = NULL;

    _gui_styles_get_active_items(g, &result, &update);

    if(newname && *newname)
    {
      if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->duplicate)))
      {
        dt_styles_create_from_style(g->nameorig,
                                    newname,
                                    gtk_entry_get_text(GTK_ENTRY(g->description)),
                                    result,
                                    g->imgid,
                                    update,
                                    _gui_styles_is_copy_module_order_set(g),
                                    _gui_styles_is_update_module_order_set(g));
      }
      else
      {
        dt_styles_update(g->nameorig,
                         newname,
                         gtk_entry_get_text(GTK_ENTRY(g->description)),
                         result,
                         g->imgid,
                         update,
                         _gui_styles_is_copy_module_order_set(g),
                         _gui_styles_is_update_module_order_set(g));
      }
      dt_control_log(_("style %s was successfully saved"), newname);
    }
    else
    {
      /* show dialog if name is missing from entry */
      GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
      GtkWidget *dlg_changename
                    = gtk_message_dialog_new(GTK_WINDOW(window),
                                             GTK_DIALOG_DESTROY_WITH_PARENT,
                                             GTK_MESSAGE_WARNING,
                                             GTK_BUTTONS_OK,
                                             _("please give style a name"));
#ifdef GDK_WINDOWING_QUARTZ
      dt_osx_disallow_fullscreen(dlg_changename);
#endif
      gtk_window_set_title(GTK_WINDOW(dlg_changename), _("unnamed style"));
      gtk_dialog_run(GTK_DIALOG(dlg_changename));
      gtk_widget_destroy(dlg_changename);
      return;
    }
  }

  // finalize the dialog
  g_free(g->nameorig);
  g_free(g);

  gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void _gui_styles_item_toggled(GtkCellRendererToggle *cell,
                                     gchar *path_str,
                                     gpointer data)
{
  dt_gui_styles_dialog_t *sd = (dt_gui_styles_dialog_t *)data;

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(sd->items));
  GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
  GtkTreeIter iter;
  gboolean toggle_item;
  gint num, update_num;

  gtk_tree_model_get_iter(model, &iter, path);
  gtk_tree_model_get(model, &iter,
                     DT_STYLE_ITEMS_COL_ENABLED,    &toggle_item,
                     DT_STYLE_ITEMS_COL_NUM,        &num,
                     DT_STYLE_ITEMS_COL_UPDATE_NUM, &update_num,
                     -1);

  toggle_item = (toggle_item == TRUE) ? FALSE : TRUE;

  if(update_num != -1 && toggle_item) // include so not updated
    gtk_list_store_set(GTK_LIST_STORE(model), &iter, DT_STYLE_ITEMS_COL_UPDATE, FALSE, -1);

  gtk_list_store_set(GTK_LIST_STORE(model), &iter,
                     DT_STYLE_ITEMS_COL_ENABLED, toggle_item, -1);
  gtk_tree_path_free(path);
}

static void _gui_styles_item_autoinit_toggled(GtkCellRendererToggle *cell,
                                              gchar *path_str,
                                              gpointer data)
{
  dt_gui_styles_dialog_t *sd = (dt_gui_styles_dialog_t *)data;

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(sd->items));
  GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
  GtkTreeIter iter;
  gboolean toggle_item;

  gtk_tree_model_get_iter(model, &iter, path);
  gtk_tree_model_get(model, &iter,
                     DT_STYLE_ITEMS_COL_AUTOINIT,  &toggle_item,
                     -1);

  toggle_item = (toggle_item == TRUE) ? FALSE : TRUE;

  gtk_list_store_set(GTK_LIST_STORE(model), &iter,
                     DT_STYLE_ITEMS_COL_AUTOINIT, toggle_item, -1);

  // auto-init (reset) is only meaningful if the module is also updated
  if(toggle_item)
    gtk_list_store_set(GTK_LIST_STORE(model), &iter,
                       DT_STYLE_ITEMS_COL_ENABLED, !toggle_item,
                       DT_STYLE_ITEMS_COL_UPDATE, toggle_item, -1);

  gtk_tree_path_free(path);
}

static void _gui_styles_item_new_autoinit_toggled(GtkCellRendererToggle *cell,
                                                  gchar *path_str,
                                                  gpointer data)
{
  dt_gui_styles_dialog_t *sd = (dt_gui_styles_dialog_t *)data;

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(sd->items_new));
  GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
  GtkTreeIter iter;
  gboolean toggle_item;

  gtk_tree_model_get_iter(model, &iter, path);
  gtk_tree_model_get(model, &iter,
                     DT_STYLE_ITEMS_COL_AUTOINIT,  &toggle_item,
                     -1);

  toggle_item = (toggle_item == TRUE) ? FALSE : TRUE;

  gtk_list_store_set(GTK_LIST_STORE(model), &iter,
                     DT_STYLE_ITEMS_COL_AUTOINIT, toggle_item, -1);

  // auto-init (reset) is only meaningful if the module is also included
  if(toggle_item)
    gtk_list_store_set(GTK_LIST_STORE(model), &iter,
                       DT_STYLE_ITEMS_COL_ENABLED, toggle_item, -1);

  gtk_tree_path_free(path);
}

static void _gui_styles_item_new_toggled(GtkCellRendererToggle *cell,
                                         gchar *path_str,
                                         gpointer data)
{
  dt_gui_styles_dialog_t *sd = (dt_gui_styles_dialog_t *)data;

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(sd->items_new));
  GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
  GtkTreeIter iter;
  gboolean toggle_item;

  gtk_tree_model_get_iter(model, &iter, path);
  gtk_tree_model_get(model, &iter, DT_STYLE_ITEMS_COL_ENABLED, &toggle_item, -1);

  toggle_item = (toggle_item == TRUE) ? FALSE : TRUE;

  gtk_list_store_set(GTK_LIST_STORE(model), &iter,
                     DT_STYLE_ITEMS_COL_ENABLED, toggle_item, -1);

  // auto-init (reset) is only meaningful if the module is also included
  if(!toggle_item)
    gtk_list_store_set(GTK_LIST_STORE(model), &iter,
                       DT_STYLE_ITEMS_COL_AUTOINIT, toggle_item, -1);

  gtk_tree_path_free(path);
}

static void _gui_styles_update_toggled(GtkCellRendererToggle *cell,
                                       gchar *path_str,
                                       gpointer data)
{
  dt_gui_styles_dialog_t *sd = (dt_gui_styles_dialog_t *)data;

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(sd->items));
  GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
  GtkTreeIter iter;
  gboolean toggle_item;

  gtk_tree_model_get_iter(model, &iter, path);
  gtk_tree_model_get(model, &iter, DT_STYLE_ITEMS_COL_UPDATE, &toggle_item, -1);

  toggle_item = (toggle_item == TRUE) ? FALSE : TRUE;

  gtk_list_store_set(GTK_LIST_STORE(model), &iter,
                     DT_STYLE_ITEMS_COL_ENABLED, !toggle_item, -1);
  gtk_list_store_set(GTK_LIST_STORE(model), &iter,
                     DT_STYLE_ITEMS_COL_UPDATE, toggle_item, -1);
  gtk_tree_path_free(path);
}

void dt_gui_styles_dialog_new(const dt_imgid_t imgid)
{
  _gui_styles_dialog_run(FALSE, NULL, imgid, NULL);
}

void dt_gui_styles_dialog_edit(const char *name, char **new_name)
{
  _gui_styles_dialog_run(TRUE, name, _single_selected_imgid(), new_name);
}

static gint _g_list_find_module_by_name(gconstpointer a, gconstpointer b)
{
  return strncmp(((dt_iop_module_t *)a)->op, b, strlen(((dt_iop_module_t *)a)->op));
}

static void _name_changed(GtkEntry *entry,
                          GtkDialog *dialog)
{
  const gchar *name = gtk_entry_get_text(entry);
  gtk_dialog_set_response_sensitive(dialog, GTK_RESPONSE_ACCEPT, name && *name);
}

static void _gui_styles_dialog_run(gboolean edit,
                                   const char *name,
                                   const dt_imgid_t imgid,
                                   char **new_name)
{
  char title[512];

  /* check if style exists */
  if(name && (dt_styles_exists(name)) == 0) return;

  /* initialize the dialog */
  dt_gui_styles_dialog_t *sd = g_malloc0(sizeof(dt_gui_styles_dialog_t));

  sd->nameorig = g_strdup(name);
  sd->imgid = imgid;
  sd->newname = new_name;

  if(edit)
  {
    snprintf(title, sizeof(title), "%s \"%s\"", _("edit style"), name);
    sd->duplicate = gtk_check_button_new_with_label(_("duplicate style"));
    gtk_widget_set_tooltip_text
      (sd->duplicate,
       _("creates a duplicate of the style before applying changes"));
  }
  else
  {
    g_strlcpy(title, _("create new style"), sizeof(title));
    sd->duplicate = NULL;
  }
  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
  GtkDialog *dialog = GTK_DIALOG(
      gtk_dialog_new_with_buttons(title, GTK_WINDOW(window), GTK_DIALOG_DESTROY_WITH_PARENT,
                                  _("select _all"),  GTK_RESPONSE_YES,
                                  _("select _none"), GTK_RESPONSE_NONE,
                                  _("_cancel"), GTK_RESPONSE_REJECT,
                                  _("_save"), GTK_RESPONSE_ACCEPT, NULL));
  dt_gui_dialog_add_help(dialog, "styles");
  gtk_dialog_set_default_response(dialog, GTK_RESPONSE_ACCEPT);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(GTK_WIDGET(dialog));
#endif

  GtkContainer *content_area =
    GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog)));

  // label box
  GtkBox *box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));

  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll),
                                             DT_PIXEL_APPLY_DPI(450));
//  only available in 3.22, and not making the expected job anyway
//  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), DT_PIXEL_APPLY_DPI(700));
//  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);

  // box in scrollwindow containing the two possible trees
  GtkBox *sbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));

  gtk_box_pack_start(GTK_BOX(content_area), GTK_WIDGET(box), TRUE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(scroll), GTK_WIDGET(sbox));

  sd->name = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(sd->name), _("name"));
  gtk_widget_set_tooltip_text(sd->name, _("enter a name for the new style"));
  gtk_entry_set_activates_default(GTK_ENTRY(sd->name), TRUE);
  gtk_dialog_set_response_sensitive(dialog, GTK_RESPONSE_ACCEPT, FALSE);
  g_signal_connect(sd->name, "changed", G_CALLBACK(_name_changed), dialog);

  sd->description = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(sd->description), _("description"));
  gtk_widget_set_tooltip_text
    (sd->description,
     _("enter a description for the new style, this description is searchable"));
  gtk_entry_set_activates_default(GTK_ENTRY(sd->description), TRUE);

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

  gtk_box_pack_start(box, sd->name, FALSE, TRUE, 0);
  gtk_box_pack_start(box, sd->description, FALSE, TRUE, 0);
  gtk_box_pack_start(box, GTK_WIDGET(scroll), TRUE, TRUE, 0);

  /* create the list of items */
  sd->items = GTK_TREE_VIEW(gtk_tree_view_new());
  GtkListStore *liststore = gtk_list_store_new(
    DT_STYLE_ITEMS_NUM_COLS, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN,
    GDK_TYPE_PIXBUF, G_TYPE_BOOLEAN, G_TYPE_STRING,
    GDK_TYPE_PIXBUF, G_TYPE_INT, G_TYPE_INT);

  sd->items_new = GTK_TREE_VIEW(gtk_tree_view_new());
  GtkListStore *liststore_new = gtk_list_store_new
    (DT_STYLE_ITEMS_NUM_COLS, G_TYPE_BOOLEAN, G_TYPE_STRING,
     GDK_TYPE_PIXBUF, G_TYPE_BOOLEAN, G_TYPE_STRING,
     GDK_TYPE_PIXBUF, G_TYPE_INT, G_TYPE_INT);

  /* enabled */
  GtkCellRenderer *renderer = gtk_cell_renderer_toggle_new();
  gtk_cell_renderer_toggle_set_activatable(GTK_CELL_RENDERER_TOGGLE(renderer), TRUE);
  g_object_set_data(G_OBJECT(renderer), "column", (gint *)DT_STYLE_ITEMS_COL_ENABLED);
  g_signal_connect(renderer, "toggled", G_CALLBACK(_gui_styles_item_toggled), sd);
  gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(sd->items), -1,
                                              edit ? _("keep") : _("include"),
                                              renderer, "active",
                                              DT_STYLE_ITEMS_COL_ENABLED, NULL);

  /* auto-init */
  renderer = gtk_cell_renderer_toggle_new();
  gtk_cell_renderer_toggle_set_activatable(GTK_CELL_RENDERER_TOGGLE(renderer), TRUE);
  g_object_set_data(G_OBJECT(renderer), "column", (gint *)DT_STYLE_ITEMS_COL_AUTOINIT);
  g_signal_connect(renderer, "toggled",
                   G_CALLBACK(_gui_styles_item_autoinit_toggled), sd);
  gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(sd->items), -1, _("reset"),
                                              renderer, "active",
                                              DT_STYLE_ITEMS_COL_AUTOINIT, NULL);

  if(edit)
  {
    /* include */
    renderer = gtk_cell_renderer_toggle_new();
    gtk_cell_renderer_toggle_set_activatable(GTK_CELL_RENDERER_TOGGLE(renderer), TRUE);
    g_object_set_data(G_OBJECT(renderer), "column", (gint *)DT_STYLE_ITEMS_COL_ENABLED);
    g_signal_connect(renderer, "toggled", G_CALLBACK(_gui_styles_item_new_toggled), sd);
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(sd->items_new), -1,
                                                _("include"), renderer,
                                                "active", DT_STYLE_ITEMS_COL_ENABLED, NULL);

    /* auto-init */
    renderer = gtk_cell_renderer_toggle_new();
    gtk_cell_renderer_toggle_set_activatable(GTK_CELL_RENDERER_TOGGLE(renderer), TRUE);
    g_object_set_data(G_OBJECT(renderer), "column", (gint *)DT_STYLE_ITEMS_COL_AUTOINIT);
    g_signal_connect(renderer, "toggled",
                     G_CALLBACK(_gui_styles_item_new_autoinit_toggled), sd);
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(sd->items_new), -1,
                                                _("reset"),
                                                renderer, "active",
                                                DT_STYLE_ITEMS_COL_AUTOINIT, NULL);
  }

  /* update */
  if(edit && dt_is_valid_imgid(imgid))
  {
    renderer = gtk_cell_renderer_toggle_new();
    gtk_cell_renderer_toggle_set_activatable(GTK_CELL_RENDERER_TOGGLE(renderer), TRUE);
    g_object_set_data(G_OBJECT(renderer), "column", (gint *)DT_STYLE_ITEMS_COL_UPDATE);
    g_signal_connect(renderer, "toggled", G_CALLBACK(_gui_styles_update_toggled), sd);

    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(sd->items), -1,
                                                _("update"), renderer, "active",
                                                DT_STYLE_ITEMS_COL_UPDATE, NULL);
  }

  /* active */
  renderer = gtk_cell_renderer_pixbuf_new();
  GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes
    ("", renderer,
     "pixbuf",
     DT_STYLE_ITEMS_COL_ISACTIVE, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(sd->items), column);
  gtk_tree_view_column_set_alignment(column, 0.5);
  gtk_tree_view_column_set_clickable(column, FALSE);
  gtk_tree_view_column_set_min_width(column, DT_PIXEL_APPLY_DPI(30));

  if(edit)
  {
    column = gtk_tree_view_column_new_with_attributes("", renderer, "pixbuf",
                                                      DT_STYLE_ITEMS_COL_ISACTIVE, NULL);
    gtk_tree_view_column_set_alignment(column, 0.5);
    gtk_tree_view_column_set_clickable(column, FALSE);
    gtk_tree_view_column_set_min_width(column, DT_PIXEL_APPLY_DPI(30));
    gtk_tree_view_append_column(GTK_TREE_VIEW(sd->items_new), column);
  }

  /* name */
  renderer = gtk_cell_renderer_text_new();
  g_object_set_data(G_OBJECT(renderer), "column", (gint *)DT_STYLE_ITEMS_COL_NAME);
  g_object_set(renderer, "xalign", 0.0, (gchar *)0);
  gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(sd->items), -1,
                                              _("item"), renderer, "markup",
                                              DT_STYLE_ITEMS_COL_NAME, NULL);

  if(edit)
  {
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(sd->items_new), -1,
                                                _("item"), renderer, "markup",
                                                DT_STYLE_ITEMS_COL_NAME, NULL);
  }

  /* mask */
  renderer = gtk_cell_renderer_pixbuf_new();
  column = gtk_tree_view_column_new_with_attributes
    (_("mask"), renderer, "pixbuf",
     DT_STYLE_ITEMS_COL_MASK, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(sd->items), column);
  gtk_tree_view_column_set_alignment(column, 0.5);
  gtk_tree_view_column_set_clickable(column, FALSE);
  gtk_tree_view_column_set_min_width(column, DT_PIXEL_APPLY_DPI(30));

  if(edit)
  {
    column = gtk_tree_view_column_new_with_attributes(_("mask"), renderer, "pixbuf",
                                                      DT_STYLE_ITEMS_COL_MASK, NULL);
    gtk_tree_view_column_set_alignment(column, 0.5);
    gtk_tree_view_column_set_clickable(column, FALSE);
    gtk_tree_view_column_set_min_width(column, DT_PIXEL_APPLY_DPI(30));
    gtk_tree_view_append_column(GTK_TREE_VIEW(sd->items_new), column);
  }

  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(sd->items)), GTK_SELECTION_SINGLE);
  gtk_tree_view_set_model(GTK_TREE_VIEW(sd->items), GTK_TREE_MODEL(liststore));

  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(sd->items_new)), GTK_SELECTION_SINGLE);
  gtk_tree_view_set_model(GTK_TREE_VIEW(sd->items_new), GTK_TREE_MODEL(liststore_new));

  gboolean has_new_item = FALSE, has_item = FALSE;

  GdkPixbuf *is_active_pb =
    dt_draw_paint_to_pixbuf(GTK_WIDGET(dialog), 10, 0, dtgtk_cairo_paint_switch);
  GdkPixbuf *is_inactive_pb =
    dt_draw_paint_to_pixbuf(GTK_WIDGET(dialog), 10, 0, dtgtk_cairo_paint_switch_inactive);
  GdkPixbuf *mask =
    dt_draw_paint_to_pixbuf(GTK_WIDGET(dialog), 10, 0, dtgtk_cairo_paint_showmask);

  /* fill list with history items */
  GtkTreeIter iter;
  if(edit)
  {
    gtk_list_store_append(GTK_LIST_STORE(liststore), &iter);
    gtk_list_store_set(GTK_LIST_STORE(liststore), &iter,
                       DT_STYLE_ITEMS_COL_ENABLED,  dt_styles_has_module_order(name),
                       DT_STYLE_ITEMS_COL_ISACTIVE, is_active_pb,
                       DT_STYLE_ITEMS_COL_NAME,     _("module order"),
                       DT_STYLE_ITEMS_COL_NUM,      -1,
                       -1);
    /* get history items for named style and populate the items list */
    GList *items = dt_styles_get_item_list(name, TRUE, imgid, TRUE);
    if(items)
    {
      for(const GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
      {
        dt_style_item_t *item = items_iter->data;
        const dt_develop_mask_mode_t mask_mode = item->blendop_params->mask_mode;

        if(item->num != -1 && item->selimg_num != -1) // defined in style and image
        {
          gtk_list_store_append(GTK_LIST_STORE(liststore), &iter);
          gtk_list_store_set(GTK_LIST_STORE(liststore), &iter,
                             DT_STYLE_ITEMS_COL_ENABLED,    TRUE,
                             DT_STYLE_ITEMS_COL_AUTOINIT,   FALSE,
                             DT_STYLE_ITEMS_COL_UPDATE,     FALSE,
                             DT_STYLE_ITEMS_COL_ISACTIVE,   item->enabled ? is_active_pb : is_inactive_pb,
                             DT_STYLE_ITEMS_COL_NAME,       item->name,
                             DT_STYLE_ITEMS_COL_MASK,       mask_mode > 0 ? mask : NULL,
                             DT_STYLE_ITEMS_COL_NUM,        item->num,
                             DT_STYLE_ITEMS_COL_UPDATE_NUM, item->selimg_num,
                             -1);
          has_item = TRUE;
        }
        else if(item->num != -1
                || item->selimg_num != -1) // defined in one or the other, let a way to select it or not
        {
          gtk_list_store_append(GTK_LIST_STORE(liststore_new), &iter);
          gtk_list_store_set(GTK_LIST_STORE(liststore_new), &iter,
                             DT_STYLE_ITEMS_COL_ENABLED,    item->num != -1 ? TRUE : FALSE,
                             DT_STYLE_ITEMS_COL_AUTOINIT,   FALSE,
                             DT_STYLE_ITEMS_COL_ISACTIVE,   item->enabled ? is_active_pb : is_inactive_pb,
                             DT_STYLE_ITEMS_COL_NAME,       item->name,
                             DT_STYLE_ITEMS_COL_MASK,       mask_mode > 0 ? mask : NULL,
                             DT_STYLE_ITEMS_COL_NUM,        item->num,
                             DT_STYLE_ITEMS_COL_UPDATE_NUM, item->selimg_num,
                             -1);
          has_new_item = TRUE;
        }
      }
      g_list_free_full(items, dt_style_item_free);
    }
  }
  else
  {
    const dt_iop_order_t order = dt_ioppr_get_iop_order_version(imgid);
    char *label = g_strdup_printf("%s (%s)", _("module order"), dt_iop_order_string(order));
    gtk_list_store_append(GTK_LIST_STORE(liststore), &iter);
    gtk_list_store_set(GTK_LIST_STORE(liststore), &iter,
                       DT_STYLE_ITEMS_COL_ENABLED,  TRUE,
                       DT_STYLE_ITEMS_COL_ISACTIVE, is_active_pb,
                       DT_STYLE_ITEMS_COL_NAME,     label,
                       DT_STYLE_ITEMS_COL_NUM, -1,
                       -1);
    g_free(label);

    GList *items = dt_history_get_items(imgid, FALSE, TRUE, TRUE);
    if(items)
    {
      for(const GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
      {
        dt_history_item_t *item = items_iter->data;

        /* lookup history item module */
        gboolean enabled = TRUE;
        dt_iop_module_t *module = NULL;
        GList *modules = darktable.develop->iop;
        if(modules)
        {
          GList *result = g_list_find_custom(
              modules, item->op, _g_list_find_module_by_name);
          if(result)
          {
            module = (dt_iop_module_t *)(result->data);
            enabled = (module->flags() & IOP_FLAGS_INCLUDE_IN_STYLES) ? TRUE : FALSE;
          }
        }

        gtk_list_store_append(GTK_LIST_STORE(liststore), &iter);
        gtk_list_store_set
          (GTK_LIST_STORE(liststore), &iter,
           DT_STYLE_ITEMS_COL_ENABLED,  enabled,
           DT_STYLE_ITEMS_COL_AUTOINIT, FALSE,
           DT_STYLE_ITEMS_COL_ISACTIVE, item->enabled ? is_active_pb : is_inactive_pb,
           DT_STYLE_ITEMS_COL_NAME,     item->name,
           DT_STYLE_ITEMS_COL_MASK,     item->mask_mode > 0 ? mask : NULL,
           DT_STYLE_ITEMS_COL_NUM,      item->num,
           -1);

        has_item = TRUE;
      }
      g_list_free_full(items, dt_history_item_free);
    }
    else
    {
      dt_control_log(_("can't create style out of unaltered image"));
      return;
    }
  }

  if(has_item)
    gtk_box_pack_start(sbox, GTK_WIDGET(sd->items), TRUE, TRUE, 0);

  if(has_new_item)
    gtk_box_pack_start(sbox, GTK_WIDGET(sd->items_new), TRUE, TRUE, 0);

  if(edit)
    gtk_box_pack_start(GTK_BOX(content_area), GTK_WIDGET(sd->duplicate), FALSE, TRUE, 0);

  g_object_unref(liststore);
  g_object_unref(liststore_new);

  /* run dialog */
  if(edit)
    g_signal_connect(dialog, "response", G_CALLBACK(_gui_styles_edit_style_response), sd);
  else
    g_signal_connect(dialog, "response", G_CALLBACK(_gui_styles_new_style_response), sd);

  gtk_widget_show_all(GTK_WIDGET(dialog));
  gtk_dialog_run(GTK_DIALOG(dialog));

  g_object_unref(is_active_pb);
  g_object_unref(is_inactive_pb);
}

// style preview

typedef struct _preview_data_t
{
  char style_name[128];
  dt_imgid_t imgid;
  gboolean first_draw;
  cairo_surface_t *surface;
  guint8 *hash;
  int hash_len;
} _preview_data_t;

static gboolean _preview_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  _preview_data_t *data = (_preview_data_t *)user_data;

  if(dt_is_valid_imgid(data->imgid) && !data->first_draw && !data->surface)
    data->surface = dt_gui_get_style_preview(data->imgid, data->style_name);

  if(data->surface)
  {
    const int psize = dt_conf_get_int("ui/style/preview_size");
    const int swidth = cairo_image_surface_get_width(data->surface);
    const int sheight = cairo_image_surface_get_height(data->surface);
    cairo_set_source_surface(cr, data->surface, .5f * (psize - swidth), .5f * (psize - sheight));
    cairo_paint(cr);
  }
  else
  {
    data->first_draw = FALSE;
    gtk_widget_queue_draw(widget);
  }

  return FALSE;
}

GtkWidget *dt_gui_style_content_dialog(char *name, const dt_imgid_t imgid)
{
  static _preview_data_t data = { "", -1, FALSE, NULL, NULL, 0};

  dt_history_hash_values_t hash = { NULL, 0, NULL, 0, NULL, 0 };
  dt_history_hash_read(imgid, &hash);

  if(imgid != data.imgid
     || g_strcmp0(data.style_name, name)
     || data.hash_len != hash.current_len
     || memcmp(data.hash, hash.current, data.hash_len))
  {
    if(data.surface)
    {
      cairo_surface_destroy(data.surface);
      data.surface = NULL;
    }
    data.imgid = imgid;
    g_strlcpy(data.style_name, name, sizeof(data.style_name));
    g_free(data.hash);
    data.hash = g_malloc(hash.current_len);
    memcpy(data.hash, hash.current, hash.current_len);
    data.hash_len = hash.current_len;
  }

  dt_history_hash_free(&hash);

  if(!*name) return NULL;

  GtkWidget *ht = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  GtkWidget *label = NULL;

// Currently, some module names listed in the style tooltip are wider than the
// style thumbnail, so the actual width of the tooltip can sometimes "breathe".
// Given this, the width we specify here can also make the tooltip a little wider.
#define STYLE_TOOLTIP_MAX_WIDTH 30

  // Style name
  char *localized_name = dt_util_localize_segmented_name(name);
  gchar *esc_name = g_markup_printf_escaped("<b>%s</b>", localized_name);
  free(localized_name);
  label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(label), esc_name);
  gtk_label_set_max_width_chars(GTK_LABEL(label), STYLE_TOOLTIP_MAX_WIDTH);
  gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
  gtk_box_pack_start(GTK_BOX(ht), label, FALSE, FALSE, 0);
  g_free(esc_name);

  // Style description, it can be empty
  char *des = dt_styles_get_description(name);

  if(des && strlen(des) > 0)
  {
    // If the name and/or description are long and become multi-line, it will look
    // hard to understand what is what, so we add a horizontal separator between them.
    gtk_box_pack_start(GTK_BOX(ht), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), TRUE, TRUE, 0);

    gchar *esc_des = g_markup_printf_escaped("<b>%s</b>", des);
    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), esc_des);
    gtk_label_set_max_width_chars(GTK_LABEL(label), STYLE_TOOLTIP_MAX_WIDTH);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_box_pack_start(GTK_BOX(ht), label, FALSE, FALSE, 0);
    g_free(esc_des);
  }

  gtk_box_pack_start(GTK_BOX(ht), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), TRUE, TRUE, 0);

  GList *items = dt_styles_get_item_list(name, TRUE, -1, FALSE);
  GList *l = items;
  while(l)
  {
    char mn[64];
    dt_style_item_t *i = l->data;

    if(i->multi_name && strlen(i->multi_name) > 0)
    {
      snprintf(mn, sizeof(mn), "(%s)", i->multi_name);
    }
    else
    {
      snprintf(mn, sizeof(mn), "(%d)", i->multi_priority);
    }

    char buf[1024];
    snprintf(buf, sizeof(buf), "  %s %s %s",
             i->enabled ? "●" : "○",
             gettext(i->name),
             mn);

    label = gtk_label_new(buf);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(ht), label, FALSE, FALSE, 0);
    l = g_list_next(l);
  }

  g_list_free_full(items, dt_style_item_free);

  if(dt_is_valid_imgid(imgid))
  {
    gtk_box_pack_start(GTK_BOX(ht), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), TRUE, TRUE, 0);

    // style preview
    const int psize = dt_conf_get_int("ui/style/preview_size");
    GtkWidget *da = gtk_drawing_area_new();
    gtk_widget_set_size_request(da, psize, psize);
    gtk_widget_set_halign(da, GTK_ALIGN_CENTER);
    gtk_widget_set_app_paintable(da, TRUE);
    gtk_box_pack_start(GTK_BOX(ht), da, TRUE, TRUE, 0);
    data.first_draw = TRUE;
    g_signal_connect(G_OBJECT(da), "draw", G_CALLBACK(_preview_draw), &data);
  }

  return ht;
}

cairo_surface_t *dt_gui_get_style_preview(const dt_imgid_t imgid, const char *name)
{
  const int psize = dt_conf_get_int("ui/style/preview_size");
  cairo_surface_t *surface = dt_imageio_preview(imgid, psize, psize, -1, name);
  return surface;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
