/*
    This file is part of darktable,
    Copyright (C) 2012-2023 darktable developers.

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
#include "common/history.h"
#include "common/styles.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "gui/gtk.h"
#include "gui/hist_dialog.h"
#include "gui/styles.h"
#include "gui/draw.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

typedef enum _style_items_columns_t
{
  DT_HIST_ITEMS_COL_ENABLED = 0,
  DT_HIST_ITEMS_COL_ISACTIVE,
  DT_HIST_ITEMS_COL_AUTOINIT,
  DT_HIST_ITEMS_COL_NAME,
  DT_HIST_ITEMS_COL_MASK,
  DT_HIST_ITEMS_COL_NUM,
  DT_HIST_ITEMS_NUM_COLS
} _styles_columns_t;

static gboolean _gui_hist_is_copy_module_order_set(dt_history_copy_item_t *d)
{
  /* iterate through TreeModel to find if module order was copied
   * (num=-1 and active) */
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->items));

  gboolean active = FALSE;
  gboolean module_order_was_copied = FALSE;
  gint num = 0;

  gtk_tree_model_get_iter_first(model, &iter);
  do
  {
      gtk_tree_model_get(model, &iter,
                         DT_HIST_ITEMS_COL_ENABLED, &active,
                         DT_HIST_ITEMS_COL_NUM, &num,
                         -1);
      if(active && (num == -1)) module_order_was_copied = TRUE;
  }
  while(gtk_tree_model_iter_next(model, &iter));

  return module_order_was_copied;
}

static GList *_gui_hist_get_active_items(dt_history_copy_item_t *d)
{
  GList *result = NULL;

  /* run through all items and add active ones to result */
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->items));
  if(gtk_tree_model_get_iter_first(model, &iter))
  {
    do
    {
      gboolean active = FALSE;
      gboolean autoinit = FALSE;
      gint num = 0;
      gtk_tree_model_get(model, &iter,
                         DT_HIST_ITEMS_COL_ENABLED, &active,
                         DT_HIST_ITEMS_COL_AUTOINIT, &autoinit,
                         DT_HIST_ITEMS_COL_NUM, &num,
                         -1);

      if(active && num >= 0)
        result = g_list_prepend(result, GINT_TO_POINTER(autoinit ? -num : num));

    } while(gtk_tree_model_iter_next(model, &iter));
  }
  return g_list_reverse(result);  // list was built in reverse order, so un-reverse it
}

static void _gui_hist_set_items(dt_history_copy_item_t *d,
                                const gboolean active)
{
  /* run through all items and set active status */
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->items));
  if(gtk_tree_model_get_iter_first(model, &iter))
  {
    do
    {
      gtk_list_store_set(GTK_LIST_STORE(model), &iter,
                         DT_HIST_ITEMS_COL_ENABLED, active,
                         -1);
    } while(gtk_tree_model_iter_next(model, &iter));
  }
}

static void _gui_hist_copy_response(GtkDialog *dialog,
                                    const gint response_id,
                                    dt_history_copy_item_t *g)
{
  switch(response_id)
  {
    case GTK_RESPONSE_CANCEL:
      break;

    case GTK_RESPONSE_YES:
      _gui_hist_set_items(g, TRUE);
      break;

    case GTK_RESPONSE_NONE:
      _gui_hist_set_items(g, FALSE);
      break;

    case GTK_RESPONSE_OK:
      g->selops = _gui_hist_get_active_items(g);
      g->copy_iop_order = _gui_hist_is_copy_module_order_set(g);
      break;
  }
}

static void _gui_hist_item_toggled(GtkCellRendererToggle *cell,
                                   const gchar *path_str,
                                   gpointer data)
{
  dt_history_copy_item_t *d = (dt_history_copy_item_t *)data;

  const _styles_columns_t col =
    GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cell), "column"));

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->items));
  GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
  GtkTreeIter iter;
  gboolean toggle_item;

  gtk_tree_model_get_iter(model, &iter, path);
  gtk_tree_model_get(model, &iter, col, &toggle_item, -1);

  toggle_item = (toggle_item == TRUE) ? FALSE : TRUE;

  gtk_list_store_set(GTK_LIST_STORE(model), &iter, col, toggle_item, -1);
  gtk_tree_path_free(path);
}

static gboolean _gui_is_set(GList *selops,
                            const unsigned int num)
{
  /* nothing to filter */
  if(!selops)
    return TRUE;

  for(GList *l = selops; l; l = g_list_next(l))
  {
    if(l->data)
    {
      const unsigned int lnum = GPOINTER_TO_UINT(l->data);
      if(lnum == num)
        return TRUE;
    }
  }
  return FALSE;
}

void tree_on_row_activated(GtkTreeView       *treeview,
                           GtkTreePath       *path,
                           GtkTreeViewColumn *col,
                           gpointer           userdata)
{
  GtkDialog *dialog = GTK_DIALOG(userdata);
  GtkTreeModel *model = gtk_tree_view_get_model(treeview);
  GtkTreeIter   iter;

  // unselect all items

  if(gtk_tree_model_get_iter_first(model, &iter))
  {
    do
    {
      gtk_list_store_set(GTK_LIST_STORE(model), &iter,
                         DT_HIST_ITEMS_COL_ENABLED, FALSE,
                         -1);
    } while(gtk_tree_model_iter_next(model, &iter));
  }

  // select now the one that got double-clicked

  if(gtk_tree_model_get_iter(model, &iter, path))
  {
    gtk_list_store_set(GTK_LIST_STORE(model), &iter,
                       DT_HIST_ITEMS_COL_ENABLED, TRUE,
                       -1);
    // and finally close the dialog
    g_signal_emit_by_name(dialog, "response", GTK_RESPONSE_OK, NULL);
  }
}

int dt_gui_hist_dialog_new(dt_history_copy_item_t *d,
                           const dt_imgid_t imgid,
                           const gboolean iscopy)
{
  int res;
  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);

  GtkDialog *dialog = GTK_DIALOG
    (gtk_dialog_new_with_buttons(
      iscopy ? _("select parts to copy") : _("select parts to paste"),
      GTK_WINDOW(window), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
      _("select _all"),  GTK_RESPONSE_YES,
      _("select _none"), GTK_RESPONSE_NONE,
      _("_cancel"),      GTK_RESPONSE_CANCEL,
      _("_ok"),          GTK_RESPONSE_OK,
      NULL));
  dt_gui_dialog_add_help(dialog, "copy_history");

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(GTK_WIDGET(dialog));
#endif

  GtkContainer *content_area =
    GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog)));

  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll),
                                             DT_PIXEL_APPLY_DPI(450));

  /* create the list of items */
  d->items = GTK_TREE_VIEW(gtk_tree_view_new());
  gtk_container_add(GTK_CONTAINER(scroll), GTK_WIDGET(d->items));
  gtk_box_pack_start(GTK_BOX(content_area), GTK_WIDGET(scroll), TRUE, TRUE, 0);

  GtkListStore *liststore
    = gtk_list_store_new(DT_HIST_ITEMS_NUM_COLS,
                         G_TYPE_BOOLEAN, GDK_TYPE_PIXBUF, G_TYPE_BOOLEAN,
                         G_TYPE_STRING, GDK_TYPE_PIXBUF, G_TYPE_UINT);

  /* enabled */
  GtkCellRenderer *renderer = gtk_cell_renderer_toggle_new();
  gtk_cell_renderer_toggle_set_activatable(GTK_CELL_RENDERER_TOGGLE(renderer), TRUE);
  g_object_set_data(G_OBJECT(renderer), "column", (gint *)DT_HIST_ITEMS_COL_ENABLED);
  g_signal_connect(renderer, "toggled", G_CALLBACK(_gui_hist_item_toggled), d);

  gtk_tree_view_insert_column_with_attributes
    (GTK_TREE_VIEW(d->items), -1, _("include"), renderer, "active",
     DT_HIST_ITEMS_COL_ENABLED, NULL);

  /* auto-init */
  renderer = gtk_cell_renderer_toggle_new();
  gtk_cell_renderer_toggle_set_activatable(GTK_CELL_RENDERER_TOGGLE(renderer), TRUE);
  g_object_set_data(G_OBJECT(renderer), "column", (gint *)DT_HIST_ITEMS_COL_AUTOINIT);
  g_signal_connect(renderer, "toggled", G_CALLBACK(_gui_hist_item_toggled), d);

  gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(d->items), -1, _("reset"),
                                              renderer, "active",
                                              DT_HIST_ITEMS_COL_AUTOINIT, NULL);

  /* active */
  renderer = gtk_cell_renderer_pixbuf_new();
  GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes
    ("", renderer, "pixbuf",
     DT_HIST_ITEMS_COL_ISACTIVE, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(d->items), column);
  gtk_tree_view_column_set_alignment(column, 0.5);
  gtk_tree_view_column_set_clickable(column, FALSE);
  gtk_tree_view_column_set_min_width(column, DT_PIXEL_APPLY_DPI(30));

  /* name */
  renderer = gtk_cell_renderer_text_new();
  g_object_set_data(G_OBJECT(renderer), "column", (gint *)DT_HIST_ITEMS_COL_NAME);
  g_object_set(renderer, "xalign", 0.0, (gchar *)0);
  gtk_tree_view_insert_column_with_attributes
    (GTK_TREE_VIEW(d->items), -1, _("item"), renderer, "markup",
     DT_HIST_ITEMS_COL_NAME, NULL);

  /* mask */
  renderer = gtk_cell_renderer_pixbuf_new();
  column = gtk_tree_view_column_new_with_attributes
    (_("mask"), renderer, "pixbuf",
     DT_HIST_ITEMS_COL_MASK, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(d->items), column);
  gtk_tree_view_column_set_alignment(column, 0.5);
  gtk_tree_view_column_set_clickable(column, FALSE);
  gtk_tree_view_column_set_min_width(column, DT_PIXEL_APPLY_DPI(30));

  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(d->items)),
                              GTK_SELECTION_SINGLE);
  gtk_tree_view_set_model(GTK_TREE_VIEW(d->items), GTK_TREE_MODEL(liststore));

  GdkPixbuf *is_active_pb =
    dt_draw_paint_to_pixbuf(GTK_WIDGET(dialog), 10, 0, dtgtk_cairo_paint_switch);
  GdkPixbuf *is_inactive_pb =
    dt_draw_paint_to_pixbuf(GTK_WIDGET(dialog), 10, 0, dtgtk_cairo_paint_switch_inactive);
  GdkPixbuf *mask =
    dt_draw_paint_to_pixbuf(GTK_WIDGET(dialog), 10, 0, dtgtk_cairo_paint_showmask);

  /* fill list with history items */
  GList *items = dt_history_get_items(imgid, FALSE, TRUE, TRUE);
  if(items)
  {
    GtkTreeIter iter;

    for(const GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
    {
      const dt_history_item_t *item = items_iter->data;
      const int flags = dt_iop_get_module_flags(item->op);

      if(!(flags & IOP_FLAGS_HIDDEN))
      {
        gtk_list_store_append(GTK_LIST_STORE(liststore), &iter);
        gtk_list_store_set
          (GTK_LIST_STORE(liststore), &iter,
           DT_HIST_ITEMS_COL_ENABLED, iscopy ? FALSE : _gui_is_set(d->selops, item->num),
           DT_HIST_ITEMS_COL_AUTOINIT, FALSE,
           DT_HIST_ITEMS_COL_ISACTIVE, item->enabled ? is_active_pb : is_inactive_pb,
           DT_HIST_ITEMS_COL_NAME, item->name,
           DT_HIST_ITEMS_COL_MASK, item->mask_mode > 0 ? mask : NULL,
           DT_HIST_ITEMS_COL_NUM, (gint)item->num,
           -1);
      }
    }
    g_list_free_full(items, dt_history_item_free);

    /* last item is for copying the module order, or if paste and was selected */
    if(iscopy || d->copy_iop_order)
    {
      const dt_iop_order_t order = dt_ioppr_get_iop_order_version(imgid);
      char *label = g_strdup_printf("%s (%s)", _("module order"),
                                    dt_iop_order_string(order));
      gtk_list_store_append(GTK_LIST_STORE(liststore), &iter);
      gtk_list_store_set(GTK_LIST_STORE(liststore), &iter,
                         DT_HIST_ITEMS_COL_ENABLED, d->copy_iop_order,
                         DT_HIST_ITEMS_COL_ISACTIVE, is_active_pb,
                         DT_HIST_ITEMS_COL_NAME, label,
                         DT_HIST_ITEMS_COL_NUM, -1,
                         -1);
      g_free(label);
    }
  }
  else
  {
    dt_control_log(_("can't copy history out of unaltered image"));
    return GTK_RESPONSE_CANCEL;
  }

  g_signal_connect(GTK_TREE_VIEW(d->items), "row-activated",
                   (GCallback)tree_on_row_activated, GTK_WIDGET(dialog));
  g_object_unref(liststore);

  g_signal_connect(dialog, "response", G_CALLBACK(_gui_hist_copy_response), d);

  gtk_widget_show_all(GTK_WIDGET(dialog));

  while(1)
  {
    res = gtk_dialog_run(GTK_DIALOG(dialog));
    if(res == GTK_RESPONSE_CANCEL
       || res == GTK_RESPONSE_DELETE_EVENT
       || res == GTK_RESPONSE_OK) break;
  }

  gtk_widget_destroy(GTK_WIDGET(dialog));

  g_object_unref(is_active_pb);
  g_object_unref(is_inactive_pb);
  return res;
}

void dt_gui_hist_dialog_init(dt_history_copy_item_t *d)
{
  d->selops = NULL;
  d->copied_imageid = NO_IMGID;
  d->copy_iop_order = FALSE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
