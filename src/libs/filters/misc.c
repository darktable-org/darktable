/*
    This file is part of darktable,
    Copyright (C) 2024 darktable developers.

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
  This file implements the necessary routines for all text baseed filters for the filtering module
*/

typedef struct _widgets_misc_t
{
  dt_lib_filtering_rule_t *rule;

  GtkWidget *name;
  GtkWidget *pop;
  GtkWidget *name_tree;
  gboolean tree_ok;
  int internal_change;
  dt_collection_properties_t prop;
} _widgets_misc_t;

static void _misc_synchronise(_widgets_misc_t *source)
{
  _widgets_misc_t *dest = NULL;
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

static void _misc_changed(GtkWidget *widget,
                          gpointer user_data)
{
  _widgets_misc_t *misc = (_widgets_misc_t *)user_data;
  if(misc->rule->manual_widget_set) return;

  _rule_set_raw_text(misc->rule, gtk_entry_get_text(GTK_ENTRY(misc->name)), TRUE);
  _misc_synchronise(misc);
}

static gboolean _misc_focus_out(GtkWidget *entry,
                                GdkEventFocus *event,
                                gpointer user_data)
{
  _widgets_misc_t *misc = (_widgets_misc_t *)user_data;
  if(misc->rule->cleaning) return FALSE;
  _misc_changed(entry, user_data);
  return FALSE;
}

void _misc_tree_update(_widgets_misc_t *misc)
{
  dt_lib_filtering_t *d = misc->rule->lib;

  char query[1024] = { 0 };

  GtkTreeIter iter;
  GtkTreeModel *name_model = gtk_tree_view_get_model(GTK_TREE_VIEW(misc->name_tree));
  gtk_list_store_clear(GTK_LIST_STORE(name_model));

  gchar *table = NULL;
  gchar *tooltip = NULL;

  if(misc->prop == DT_COLLECTION_PROP_CAMERA)
  {
    tooltip = g_strdup(_("no camera defined"));
  }
  else if(misc->prop == DT_COLLECTION_PROP_LENS)
  {
    tooltip = g_strdup(_("no lens defined"));
  }
  else if(misc->prop == DT_COLLECTION_PROP_GROUP_ID)
  {
    tooltip = g_strdup(_("no group id defined"));
  }
  else if(misc->prop == DT_COLLECTION_PROP_WHITEBALANCE)
  {
    table = g_strdup("whitebalance");
    tooltip = g_strdup(_("no white balance defined"));
  }
  else if(misc->prop == DT_COLLECTION_PROP_FLASH)
  {
    table = g_strdup("flash");
    tooltip = g_strdup(_("no flash defined"));
  }
  else if(misc->prop == DT_COLLECTION_PROP_EXPOSURE_PROGRAM)
  {
    table = g_strdup("exposure_program");
    tooltip = g_strdup(_("no exposure program defined"));
  }
  else if(misc->prop == DT_COLLECTION_PROP_METERING_MODE)
  {
    table = g_strdup("metering_mode");
    tooltip = g_strdup(_("no metering mode defined"));
  }

  // SQL
  if(misc->prop == DT_COLLECTION_PROP_CAMERA)
  {
    // clang-format off
    g_snprintf(query, sizeof(query),
               "SELECT TRIM(cm.maker || ' ' || cm.model) AS camera, COUNT(*) AS count"
               " FROM main.images AS mi, main.cameras AS cm"
               " WHERE mi.camera_id = cm.id AND %s"
               " GROUP BY camera"
               " ORDER BY camera",
               d->last_where_ext);
    // clang-format on
  }
  else if(misc->prop == DT_COLLECTION_PROP_LENS)
  {
    // clang-format off
    g_snprintf(query, sizeof(query),
               "SELECT CASE LOWER(TRIM(ln.name))"
               "         WHEN 'n/a' THEN ''"
               "         ELSE ln.name"
               "       END AS lens, COUNT(*) AS count"
               " FROM main.images AS mi, main.lens AS ln"
               " WHERE mi.lens_id = ln.id AND %s"
               " GROUP BY lens"
               " ORDER BY lens",
               d->last_where_ext);
    // clang-format on
  }
  else if(misc->prop == DT_COLLECTION_PROP_GROUP_ID)
  {
    // clang-format off
    g_snprintf(query, sizeof(query),
               "SELECT mi.group_id, COUNT(*) AS count"
               " FROM main.images AS mi"
               " WHERE %s"
               " GROUP BY group_id"
               " HAVING COUNT(*) > 1"
               " ORDER BY group_id",
               d->last_where_ext);
    // clang-format on
  }
  else // white balance, flash, exposure program, metering mode
  {
    // clang-format off
    g_snprintf(query, sizeof(query),
               "SELECT t.name"
               "     , COUNT(*) AS count"
               " FROM main.images AS mi"
               " JOIN main.%s AS t"
               " WHERE mi.%s_id = t.id AND %s"
               " GROUP BY name"
               " ORDER BY name",
               table, table,
               d->last_where_ext);
    // clang-format on
  }

  g_free(table);

  // clang-format on
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  int unset = 0;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *name = (char *)sqlite3_column_text(stmt, 0);
    const int count = sqlite3_column_int(stmt, 1);

    if(!name || !g_strcmp0(name, ""))
    {
      unset += count;
    }
    else
    {
      gchar *value_path = g_strdup_printf("\"%s\"", name);
      gtk_list_store_append(GTK_LIST_STORE(name_model), &iter);
      gtk_list_store_set(GTK_LIST_STORE(name_model), &iter,
                         TREE_COL_TEXT, name,
                         TREE_COL_TOOLTIP, name,
                         TREE_COL_PATH, value_path,
                         TREE_COL_COUNT, count, -1);
      g_free(value_path);
    }
  }
  sqlite3_finalize(stmt);

  // we add the unset entry if any
  if(unset > 0)
  {
    gtk_list_store_append(GTK_LIST_STORE(name_model), &iter);
    gtk_list_store_set(GTK_LIST_STORE(name_model), &iter,
                       TREE_COL_TEXT, _("unnamed"),
                       TREE_COL_TOOLTIP, tooltip,
                       TREE_COL_PATH, _("unnamed"),
                       TREE_COL_COUNT, unset, -1);
  }
  g_free(tooltip);
  misc->tree_ok = TRUE;
}

void _misc_tree_update_visibility(GtkWidget *w,
                                  _widgets_misc_t *misc)
{
  if(!misc->tree_ok) _misc_tree_update(misc);
}

static gboolean _misc_select_func(GtkTreeModel *model,
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

static void _misc_update_selection(_widgets_misc_t *misc)
{
  // get the current text
  GtkWidget *entry = gtk_popover_get_default_widget(GTK_POPOVER(misc->pop));
  const gchar *txt = gtk_entry_get_text(GTK_ENTRY(entry));

  // get the current treeview
  GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(misc->name_tree));
  misc->internal_change++;
  gtk_tree_selection_unselect_all(sel);

  if(g_strcmp0(txt, ""))
  {
    gchar **elems = g_strsplit(txt, ",", -1);
    g_object_set_data(G_OBJECT(sel), "elems", elems);
    gtk_tree_model_foreach(gtk_tree_view_get_model(GTK_TREE_VIEW(misc->name_tree)), _misc_select_func, sel);
    g_strfreev(elems);
  }
  misc->internal_change--;
}

static gboolean _misc_press(GtkWidget *w,
                            GdkEventButton *e,
                            _widgets_misc_t *misc)
{
  if(e->button == 3)
  {
    _misc_tree_update_visibility(w, misc);
    gtk_popover_set_default_widget(GTK_POPOVER(misc->pop), w);
    gtk_popover_set_relative_to(GTK_POPOVER(misc->pop), w);

    // update the selection
    _misc_update_selection(misc);

    gtk_widget_show_all(misc->pop);
    return TRUE;
  }
  else if(e->button == 1 && e->type == GDK_2BUTTON_PRESS)
  {
    gtk_entry_set_text(GTK_ENTRY(misc->name), "");
    _misc_changed(w, misc);
  }
  return FALSE;
}

static gboolean _misc_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;

  rule->manual_widget_set++;
  _widgets_misc_t *misc = (_widgets_misc_t *)rule->w_specific;
  misc->tree_ok = FALSE;
  gtk_entry_set_text(GTK_ENTRY(misc->name), rule->raw_text);
  if(rule->topbar && rule->w_specific_top)
  {
    misc = (_widgets_misc_t *)rule->w_specific_top;
    misc->tree_ok = FALSE;
    gtk_entry_set_text(GTK_ENTRY(misc->name), rule->raw_text);
  }
  _misc_synchronise(misc);
  rule->manual_widget_set--;

  return TRUE;
}

static void _misc_popup_closed(GtkWidget *w,
                               _widgets_misc_t *misc)
{
  // we validate the corresponding entry
  gtk_widget_activate(gtk_popover_get_default_widget(GTK_POPOVER(w)));
}

static void _misc_tree_row_activated(GtkTreeView *self,
                                     GtkTreePath *path,
                                     GtkTreeViewColumn *column,
                                     _widgets_misc_t *misc)
{
  // as the selection as already been updated on first click, we just close the popup
  gtk_widget_hide(misc->pop);
}

static void _misc_tree_selection_changed(GtkTreeSelection *sel,
                                        _widgets_misc_t *misc)
{
  if(!misc || misc->internal_change) return;
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
      if(val) dt_util_str_cat(&txt, "%s%s", (txt == NULL) ? "" : ",", val);
    }
  }
  g_list_free_full(list, (GDestroyNotify)gtk_tree_path_free);

  // we set the entry with this value
  GtkWidget *entry = gtk_popover_get_default_widget(GTK_POPOVER(misc->pop));
  gtk_entry_set_text(GTK_ENTRY(entry), (txt == NULL) ? "" : txt);
  g_free(txt);
}

static void _misc_ok_clicked(GtkWidget *w,
                             _widgets_misc_t *misc)
{
  gtk_widget_hide(misc->pop);
}

void _misc_tree_count_func(GtkTreeViewColumn *col,
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

static void _misc_widget_init(dt_lib_filtering_rule_t *rule,
                              const dt_collection_properties_t prop,
                              const gchar *text,
                              dt_lib_module_t *self,
                              const gboolean top)
{
  _widgets_misc_t *misc = g_malloc0(sizeof(_widgets_misc_t));
  misc->rule = rule;
  misc->prop = prop;

  GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  if(top)
    gtk_box_pack_start(GTK_BOX(rule->w_special_box_top), hb, TRUE, TRUE, 0);
  else
    gtk_box_pack_start(GTK_BOX(rule->w_special_box), hb, TRUE, TRUE, 0);
  misc->name = dt_ui_entry_new(top ? 10 : 0);
  gtk_widget_set_can_default(misc->name, TRUE);

  gchar *name = NULL;
  gchar *tooltip = NULL;
  if(prop == DT_COLLECTION_PROP_CAMERA)
  {
    name = g_strdup(_("camera"));
    tooltip = g_strdup(_("enter camera to search.\n"
                         "multiple values can be separated by ','\n"
                         "\nright-click to get existing cameras"));
  }
  else if(prop == DT_COLLECTION_PROP_LENS)
  {
    name = g_strdup(_("lens"));
    tooltip = g_strdup(_("enter lens to search.\n"
                         "multiple values can be separated by ','\n"
                         "\nright-click to get existing lenses"));
  }
  else if(prop == DT_COLLECTION_PROP_WHITEBALANCE)
  {
    name = g_strdup(_("white balance"));
    tooltip = g_strdup(_("enter white balance to search.\n"
                         "multiple values can be separated by ','\n"
                         "\nright-click to get existing white balances"));
  }
  else if(prop == DT_COLLECTION_PROP_FLASH)
  {
    name = g_strdup(_("flash"));
    tooltip = g_strdup(_("enter flash to search.\n"
                         "multiple values can be separated by ','\n"
                         "\nright-click to get existing flashes"));
  }
  else if(prop == DT_COLLECTION_PROP_EXPOSURE_PROGRAM)
  {
    name = g_strdup(_("exposure program"));
    tooltip = g_strdup(_("enter exposure program to search.\n"
                         "multiple values can be separated by ','\n"
                         "\nright-click to get existing exposure programs"));
  }
  else if(prop == DT_COLLECTION_PROP_METERING_MODE)
  {
    name = g_strdup(_("metering mode"));
    tooltip = g_strdup(_("enter metering mode to search.\n"
                         "multiple values can be separated by ','\n"
                         "\nright-click to get existing metering modes"));
  }
  else if(prop == DT_COLLECTION_PROP_GROUP_ID)
  {
    name = g_strdup(_("group id"));
    tooltip = g_strdup(_("enter group id to search.\n"
                         "multiple values can be separated by ','\n"
                         "\nright-click to get existing group ids"));
  }

  gtk_entry_set_placeholder_text(GTK_ENTRY(misc->name), name);
  gtk_widget_set_tooltip_text(misc->name, tooltip);

  g_free(tooltip);
  g_free(name);

  gtk_box_pack_start(GTK_BOX(hb), misc->name, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(misc->name), "activate", G_CALLBACK(_misc_changed), misc);
  g_signal_connect(G_OBJECT(misc->name), "focus-out-event", G_CALLBACK(_misc_focus_out), misc);
  g_signal_connect(G_OBJECT(misc->name), "button-press-event", G_CALLBACK(_misc_press), misc);

  if(top)
  {
    dt_gui_add_class(hb, "dt_quick_filter");
  }

  // the popup
  misc->pop = gtk_popover_new(misc->name);
  gtk_widget_set_size_request(misc->pop, 250, 400);
  g_signal_connect(G_OBJECT(misc->pop), "closed", G_CALLBACK(_misc_popup_closed), misc);
  hb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(misc->pop), hb);

  // the name tree
  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  gtk_box_pack_start(GTK_BOX(hb), sw, TRUE, TRUE, 0);
  GtkTreeModel *model
      = GTK_TREE_MODEL(gtk_list_store_new(TREE_NUM_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT));
  misc->name_tree = gtk_tree_view_new_with_model(model);
  gtk_widget_show(misc->name_tree);
  gtk_widget_set_tooltip_text(misc->name_tree, _("click to select\n"
                                                 "ctrl+click to select multiple values"));
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(misc->name_tree), FALSE);
  GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(misc->name_tree));
  gtk_tree_selection_set_mode(sel, GTK_SELECTION_MULTIPLE);
  g_signal_connect(G_OBJECT(misc->name_tree), "row-activated", G_CALLBACK(_misc_tree_row_activated), misc);
  g_signal_connect(G_OBJECT(sel), "changed", G_CALLBACK(_misc_tree_selection_changed), misc);

  GtkTreeViewColumn *col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(GTK_TREE_VIEW(misc->name_tree), col);
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_set_cell_data_func(col, renderer, _misc_tree_count_func, NULL, NULL);

  gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(misc->name_tree), TREE_COL_TOOLTIP);

  gtk_container_add(GTK_CONTAINER(sw), misc->name_tree);

  // the button to close the popup
  GtkWidget *btn = gtk_button_new_with_label(_("ok"));
  gtk_box_pack_start(GTK_BOX(hb), btn, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(btn), "clicked", G_CALLBACK(_misc_ok_clicked), misc);

  if(top)
    rule->w_specific_top = misc;
  else
    rule->w_specific = misc;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
