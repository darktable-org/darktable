/*
    This file is part of darktable,
    Copyright (C) 2023 darktable developers.

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

/*
  This file contains the necessary routines to implement a filter for the filtering module
*/

typedef struct _widgets_camera_t
{
  dt_lib_filtering_rule_t *rule;

  GtkWidget *name;
  GtkWidget *pop;
  GtkWidget *name_tree;
  gboolean tree_ok;
  int internal_change;
} _widgets_camera_t;

static void _camera_synchronise(_widgets_camera_t *source)
{
  _widgets_camera_t *dest = NULL;
  if(source == source->rule->w_specific_top)
    dest = source->rule->w_specific;
  else
    dest = source->rule->w_specific_top;

  if(dest)
  {
    source->rule->manual_widget_set++;
    const gchar *txt1 = gtk_entry_get_text(GTK_ENTRY(source->name));
    gtk_entry_set_text(GTK_ENTRY(dest->name), txt1);
    source->rule->manual_widget_set--;
  }
}

static void _camera_changed(GtkWidget *widget,
                            gpointer user_data)
{
  _widgets_camera_t *camera = (_widgets_camera_t *)user_data;
  if(camera->rule->manual_widget_set) return;

  _rule_set_raw_text(camera->rule, gtk_entry_get_text(GTK_ENTRY(camera->name)), TRUE);
  _camera_synchronise(camera);
}

static gboolean _camera_focus_out(GtkWidget *entry,
                                  GdkEventFocus *event,
                                  gpointer user_data)
{
  _widgets_camera_t *camera = (_widgets_camera_t *)user_data;
  if(camera->rule->cleaning) return FALSE;
  _camera_changed(entry, user_data);
  return FALSE;
}

void _camera_tree_update(_widgets_camera_t *camera)
{
  dt_lib_filtering_t *d = camera->rule->lib;

  char query[1024] = { 0 };

  GtkTreeIter iter;
  GtkTreeModel *name_model = gtk_tree_view_get_model(GTK_TREE_VIEW(camera->name_tree));
  gtk_list_store_clear(GTK_LIST_STORE(name_model));

  // clang-format off
  g_snprintf(query, sizeof(query),
             "SELECT TRIM(cm.maker || ' ' || cm.model) AS camera, COUNT(*) AS count"
             " FROM main.images AS mi, main.cameras AS cm"
             " WHERE mi.camera_id = cm.id AND %s"
             " GROUP BY camera"
             " ORDER BY camera",
             d->last_where_ext);
  // clang-format on
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  int unset = 0;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    gchar *value = (gchar *)sqlite3_column_text(stmt, 0);
    const int count = sqlite3_column_int(stmt, 1);
    gchar *value_path = g_strdup_printf("\"%s\"", value);

    if(!value || !g_strcmp0(g_strstrip(value), ""))
    {
      unset += count;
    }
    else
    {
      gtk_list_store_append(GTK_LIST_STORE(name_model), &iter);
      gtk_list_store_set(GTK_LIST_STORE(name_model), &iter,
                         TREE_COL_TEXT, value,
                         TREE_COL_TOOLTIP, value,
                         TREE_COL_PATH, value_path,
                         TREE_COL_COUNT, count, -1);
    }
    g_free(value_path);
  }
  sqlite3_finalize(stmt);

  // we add the unset entry if any
  if(unset > 0)
  {
    gtk_list_store_append(GTK_LIST_STORE(name_model), &iter);
    gtk_list_store_set(GTK_LIST_STORE(name_model), &iter,
                       TREE_COL_TEXT, _("unnamed"),
                       TREE_COL_TOOLTIP, _("no camera defined."),
                       TREE_COL_PATH, _("unnamed"),
                       TREE_COL_COUNT, unset, -1);
  }
  camera->tree_ok = TRUE;
}

void _camera_tree_update_visibility(GtkWidget *w,
                                    _widgets_camera_t *camera)
{
  if(!camera->tree_ok) _camera_tree_update(camera);
}

static gboolean _camera_select_func(GtkTreeModel *model,
                                    GtkTreePath *path,
                                    GtkTreeIter *iter,
                                    gpointer data)
{
  GtkTreeSelection *sel = (GtkTreeSelection *)data;
  gchar **elems = (gchar **)g_object_get_data(G_OBJECT(sel), "elems");
  gchar *str = NULL;
  gtk_tree_model_get(model, iter, TREE_COL_PATH, &str, -1);

  for(int i = 0; i < g_strv_length(elems); i++)
  {
    if(!g_strcmp0(str, elems[i]))
    {
      gtk_tree_selection_select_path(sel, path);
      break;
    }
  }

  return FALSE;
}

static void _camera_update_selection(_widgets_camera_t *camera)
{
  // get the current text
  GtkWidget *entry = gtk_popover_get_default_widget(GTK_POPOVER(camera->pop));
  const gchar *txt = gtk_entry_get_text(GTK_ENTRY(entry));

  // get the current treeview
  GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(camera->name_tree));
  camera->internal_change++;
  gtk_tree_selection_unselect_all(sel);

  if(g_strcmp0(txt, ""))
  {
    gchar **elems = g_strsplit(txt, ",", -1);
    g_object_set_data(G_OBJECT(sel), "elems", elems);
    gtk_tree_model_foreach(gtk_tree_view_get_model(GTK_TREE_VIEW(camera->name_tree)), _camera_select_func, sel);
    g_strfreev(elems);
  }
  camera->internal_change--;
}

static gboolean _camera_press(GtkWidget *w,
                              GdkEventButton *e,
                              _widgets_camera_t *camera)
{
  if(e->button == 3)
  {
    _camera_tree_update_visibility(w, camera);
    gtk_popover_set_default_widget(GTK_POPOVER(camera->pop), w);
    gtk_popover_set_relative_to(GTK_POPOVER(camera->pop), w);

    // update the selection
    _camera_update_selection(camera);

    gtk_widget_show_all(camera->pop);
    return TRUE;
  }
  else if(e->button == 1 && e->type == GDK_2BUTTON_PRESS)
  {
    gtk_entry_set_text(GTK_ENTRY(camera->name), "");
    _camera_changed(w, camera);
  }
  return FALSE;
}

static gboolean _camera_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;

  rule->manual_widget_set++;
  _widgets_camera_t *camera = (_widgets_camera_t *)rule->w_specific;
  camera->tree_ok = FALSE;
  gtk_entry_set_text(GTK_ENTRY(camera->name), rule->raw_text);
  if(rule->topbar && rule->w_specific_top)
  {
    camera = (_widgets_camera_t *)rule->w_specific_top;
    camera->tree_ok = FALSE;
    gtk_entry_set_text(GTK_ENTRY(camera->name), rule->raw_text);
  }
  _camera_synchronise(camera);
  rule->manual_widget_set--;

  return TRUE;
}

static void _camera_popup_closed(GtkWidget *w,
                                 _widgets_camera_t *camera)
{
  // we validate the corresponding entry
  gtk_widget_activate(gtk_popover_get_default_widget(GTK_POPOVER(w)));
}

static void _camera_tree_row_activated(GtkTreeView *self,
                                       GtkTreePath *path,
                                       GtkTreeViewColumn *column,
                                       _widgets_camera_t *camera)
{
  // as the selection as already been updated on first click, we just close the popup
  gtk_widget_hide(camera->pop);
}

static void _camera_tree_selection_change(GtkTreeSelection *sel,
                                          _widgets_camera_t *camera)
{
  if(!camera || camera->internal_change) return;
  GtkTreeModel *model = gtk_tree_view_get_model(gtk_tree_selection_get_tree_view(sel));
  GList *list = gtk_tree_selection_get_selected_rows(sel, NULL);

  gchar *txt = NULL;
  for(const GList *l = list; l; l = g_list_next(l))
  {
    GtkTreePath *path = (GtkTreePath *)l->data;
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, path))
    {
      gchar *val = NULL;
      gtk_tree_model_get(model, &iter, TREE_COL_PATH, &val, -1);
      if(val) txt = dt_util_dstrcat(txt, "%s%s", (txt == NULL) ? "" : ",", val);
    }
  }
  g_list_free_full(list, (GDestroyNotify)gtk_tree_path_free);

  // we set the entry with this value
  GtkWidget *entry = gtk_popover_get_default_widget(GTK_POPOVER(camera->pop));
  gtk_entry_set_text(GTK_ENTRY(entry), (txt == NULL) ? "" : txt);
  g_free(txt);
}

static void _camera_ok_clicked(GtkWidget *w,
                               _widgets_camera_t *camera)
{
  gtk_widget_hide(camera->pop);
}

void _camera_tree_count_func(GtkTreeViewColumn *col,
                             GtkCellRenderer *renderer,
                             GtkTreeModel *model,
                             GtkTreeIter *iter,
                             gpointer data)
{
  gchar *name;
  guint count;

  gtk_tree_model_get(model, iter, TREE_COL_TEXT, &name, TREE_COL_COUNT, &count, -1);
  if(!g_strcmp0(name, "") && count == 0)
  {
    g_object_set(renderer, "text", name, NULL);
    g_object_set(renderer, "sensitive", FALSE, NULL);
  }
  else
  {
    gchar *coltext = g_strdup_printf("%s (%u)", name, count);
    g_object_set(renderer, "text", coltext, NULL);
    g_free(coltext);
    g_object_set(renderer, "sensitive", TRUE, NULL);
  }

  g_free(name);
}

static void _camera_widget_init(dt_lib_filtering_rule_t *rule,
                                const dt_collection_properties_t prop,
                                const gchar *text,
                                dt_lib_module_t *self,
                                const gboolean top)
{
  _widgets_camera_t *camera = (_widgets_camera_t *)g_malloc0(sizeof(_widgets_camera_t));
  camera->rule = rule;

  GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  if(top)
    gtk_box_pack_start(GTK_BOX(rule->w_special_box_top), hb, TRUE, TRUE, 0);
  else
    gtk_box_pack_start(GTK_BOX(rule->w_special_box), hb, TRUE, TRUE, 0);
  camera->name = gtk_entry_new();
  gtk_entry_set_width_chars(GTK_ENTRY(camera->name), (top) ? 10 : 0);
  gtk_widget_set_can_default(camera->name, TRUE);
  gtk_entry_set_placeholder_text(GTK_ENTRY(camera->name), _("camera"));
  gtk_widget_set_tooltip_text(camera->name, _("enter camera to search.\n"
                                              "multiple values can be separated by ','\n"
                                              "\nright-click to get existing camera"));
  gtk_box_pack_start(GTK_BOX(hb), camera->name, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(camera->name), "activate", G_CALLBACK(_camera_changed), camera);
  g_signal_connect(G_OBJECT(camera->name), "focus-out-event", G_CALLBACK(_camera_focus_out), camera);
  g_signal_connect(G_OBJECT(camera->name), "button-press-event", G_CALLBACK(_camera_press), camera);

  if(top)
  {
    dt_gui_add_class(hb, "dt_quick_filter");
  }

  // the popup
  camera->pop = gtk_popover_new(camera->name);
  gtk_widget_set_size_request(camera->pop, 250, 400);
  g_signal_connect(G_OBJECT(camera->pop), "closed", G_CALLBACK(_camera_popup_closed), camera);
  hb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(camera->pop), hb);

  // the name tree
  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  gtk_box_pack_start(GTK_BOX(hb), sw, TRUE, TRUE, 0);
  GtkTreeModel *model
      = GTK_TREE_MODEL(gtk_list_store_new(TREE_NUM_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT));
  camera->name_tree = gtk_tree_view_new_with_model(model);
  gtk_widget_show(camera->name_tree);
  gtk_widget_set_tooltip_text(camera->name_tree, _("click to select camera\n"
                                                   "ctrl+click to select multiple values"));
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(camera->name_tree), FALSE);
  GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(camera->name_tree));
  gtk_tree_selection_set_mode(sel, GTK_SELECTION_MULTIPLE);
  g_signal_connect(G_OBJECT(camera->name_tree), "row-activated", G_CALLBACK(_camera_tree_row_activated), camera);
  g_signal_connect(G_OBJECT(sel), "changed", G_CALLBACK(_camera_tree_selection_change), camera);

  GtkTreeViewColumn *col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(GTK_TREE_VIEW(camera->name_tree), col);
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_set_cell_data_func(col, renderer, _camera_tree_count_func, NULL, NULL);

  gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(camera->name_tree), TREE_COL_TOOLTIP);

  gtk_container_add(GTK_CONTAINER(sw), camera->name_tree);

  // the button to close the popup
  GtkWidget *btn = gtk_button_new_with_label(_("ok"));
  gtk_box_pack_start(GTK_BOX(hb), btn, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(btn), "clicked", G_CALLBACK(_camera_ok_clicked), camera);

  if(top)
    rule->w_specific_top = camera;
  else
    rule->w_specific = camera;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
