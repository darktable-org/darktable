/*
    This file is part of darktable,
    Copyright (C) 2022 darktable developers.

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

typedef struct _widgets_filename_t
{
  dt_lib_filters_rule_t *rule;

  GtkWidget *name;
  GtkWidget *ext;
  GtkWidget *pop;
  GtkWidget *name_tree;
  GtkWidget *ext_tree;
  gboolean tree_ok;
  int internal_change;
  gchar *last_where_ext;
} _widgets_filename_t;

static void _filename_decode(const gchar *txt, gchar **name, gchar **ext)
{
  if(!txt || strlen(txt) == 0) return;

  // split the path to find filename and extension parts
  gchar **elems = g_strsplit(txt, G_DIR_SEPARATOR_S, -1);
  const unsigned int size = g_strv_length(elems);
  if(size == 2)
  {
    *name = g_strdup(elems[0]);
    *ext = g_strdup(elems[1]);
  }
  g_strfreev(elems);
}

static void _filename_changed(GtkWidget *widget, gpointer user_data)
{
  _widgets_filename_t *filename = (_widgets_filename_t *)user_data;
  if(filename->rule->manual_widget_set) return;

  // we recreate the right raw text and put it in the raw entry
  gchar *value = g_strdup_printf("%s/%s", gtk_entry_get_text(GTK_ENTRY(filename->name)),
                                 gtk_entry_get_text(GTK_ENTRY(filename->ext)));

  _rule_set_raw_text(filename->rule, value, TRUE);
  g_free(value);
}

static gboolean _filename_focus_out(GtkWidget *entry, GdkEventFocus *event, gpointer user_data)
{
  _widgets_filename_t *filename = (_widgets_filename_t *)user_data;
  if(filename->rule->cleaning) return FALSE;
  _filename_changed(entry, user_data);
  return FALSE;
}

void _filename_tree_update(_widgets_filename_t *filename)
{
  char query[1024] = { 0 };
  int nb_raw = 0;
  int nb_not_raw = 0;
  int nb_ldr = 0;
  int nb_hdr = 0;

  GtkTreeIter iter;
  GtkTreeModel *name_model = gtk_tree_view_get_model(GTK_TREE_VIEW(filename->name_tree));
  gtk_list_store_clear(GTK_LIST_STORE(name_model));
  GtkTreeModel *ext_model = gtk_tree_view_get_model(GTK_TREE_VIEW(filename->ext_tree));
  gtk_list_store_clear(GTK_LIST_STORE(ext_model));

  // how do we separate filename and extension directly in sqlite :
  // starting example : 'nice.bird.cr2'
  // replace(filename, '.', '') => nicebirdcr2 (remove all the point)
  // rtrim(filename, replace(filename, '.', '')) => nice.bird. (remove ending chars presents in 'nice.bird.cr2' and
  // 'nicebirdcr2') rtrim(rtrim(filename, replace(filename, '.', '')), '.') => nice.bird (remove ending '.')
  // replace(filename, rtrim(filename, replace(filename, '.', '')), '.') => .cr2 (replace the filename part by a
  // '.')

  // clang-format off
  g_snprintf(query, sizeof(query),
             "SELECT rtrim(rtrim(filename, replace(filename, '.', '')), '.') AS fn, COUNT(*) AS count"
             " FROM main.images AS mi"
             " WHERE %s"
             " GROUP BY fn"
             " ORDER BY filename",
             filename->last_where_ext);
  // clang-format on
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *name = (const char *)sqlite3_column_text(stmt, 0);
    if(name == NULL) continue; // safeguard against degenerated db entries
    const int count = sqlite3_column_int(stmt, 1);

    gtk_list_store_append(GTK_LIST_STORE(name_model), &iter);
    gtk_list_store_set(GTK_LIST_STORE(name_model), &iter, TREE_COL_TEXT, name, TREE_COL_TOOLTIP, name,
                       TREE_COL_PATH, name, TREE_COL_COUNT, count, -1);
  }
  sqlite3_finalize(stmt);


  // clang-format off
  g_snprintf(query, sizeof(query),
             "SELECT upper(replace(filename, rtrim(filename, replace(filename, '.', '')), '.')) AS ext, COUNT(*) AS count, flags"
             " FROM main.images AS mi"
             " WHERE %s"
             " GROUP BY ext"
             " ORDER BY ext",
             filename->last_where_ext);
  // clang-format on
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *name = (const char *)sqlite3_column_text(stmt, 0);
    if(name == NULL) continue; // safeguard against degenerated db entries
    const int count = sqlite3_column_int(stmt, 1);
    const int flags = sqlite3_column_int(stmt, 2);

    gtk_list_store_append(GTK_LIST_STORE(ext_model), &iter);
    gtk_list_store_set(GTK_LIST_STORE(ext_model), &iter, TREE_COL_TEXT, name, TREE_COL_TOOLTIP, name,
                       TREE_COL_PATH, name, TREE_COL_COUNT, count, -1);

    if(flags & DT_IMAGE_RAW)
      nb_raw += count;
    else
      nb_not_raw += count;
    if(flags & DT_IMAGE_LDR) nb_ldr += count;
    if(flags & DT_IMAGE_HDR) nb_hdr += count;
  }
  sqlite3_finalize(stmt);

  // and we insert the predefined extensions
  gtk_list_store_insert(GTK_LIST_STORE(ext_model), &iter, 0);
  gtk_list_store_set(GTK_LIST_STORE(ext_model), &iter, TREE_COL_TEXT, "", TREE_COL_TOOLTIP, "", TREE_COL_PATH, "",
                     TREE_COL_COUNT, 0, -1);
  gtk_list_store_insert(GTK_LIST_STORE(ext_model), &iter, 0);
  gtk_list_store_set(GTK_LIST_STORE(ext_model), &iter, TREE_COL_TEXT, "HDR", TREE_COL_TOOLTIP,
                     "high dynamic range files", TREE_COL_PATH, "HDR", TREE_COL_COUNT, nb_hdr, -1);
  gtk_list_store_insert(GTK_LIST_STORE(ext_model), &iter, 0);
  gtk_list_store_set(GTK_LIST_STORE(ext_model), &iter, TREE_COL_TEXT, "LDR", TREE_COL_TOOLTIP,
                     "low dynamic range files", TREE_COL_PATH, "LDR", TREE_COL_COUNT, nb_ldr, -1);
  gtk_list_store_insert(GTK_LIST_STORE(ext_model), &iter, 0);
  gtk_list_store_set(GTK_LIST_STORE(ext_model), &iter, TREE_COL_TEXT, "NOT RAW", TREE_COL_TOOLTIP,
                     "all except RAW files", TREE_COL_PATH, "NOT RAW", TREE_COL_COUNT, nb_not_raw, -1);
  gtk_list_store_insert(GTK_LIST_STORE(ext_model), &iter, 0);
  gtk_list_store_set(GTK_LIST_STORE(ext_model), &iter, TREE_COL_TEXT, "RAW", TREE_COL_TOOLTIP, "RAW files",
                     TREE_COL_PATH, "RAW", TREE_COL_COUNT, nb_raw, -1);

  filename->tree_ok = TRUE;
}

void _filename_tree_update_visibility(GtkWidget *w, _widgets_filename_t *filename)
{
  if(!filename->tree_ok) _filename_tree_update(filename);
  gtk_widget_set_visible(gtk_widget_get_parent(filename->name_tree), w == filename->name);
  gtk_widget_set_visible(gtk_widget_get_parent(filename->ext_tree), w == filename->ext);
}

static gboolean _filename_select_func(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
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

static void _filename_update_selection(_widgets_filename_t *filename)
{
  // get the current text
  GtkWidget *entry = gtk_popover_get_default_widget(GTK_POPOVER(filename->pop));
  const gchar *txt = gtk_entry_get_text(GTK_ENTRY(entry));

  // get the current treeview
  GtkWidget *tree = (entry == filename->name) ? filename->name_tree : filename->ext_tree;

  GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
  filename->internal_change++;
  gtk_tree_selection_unselect_all(sel);

  if(g_strcmp0(txt, ""))
  {
    gchar **elems = g_strsplit(txt, ",", -1);
    g_object_set_data(G_OBJECT(sel), "elems", elems);
    gtk_tree_model_foreach(gtk_tree_view_get_model(GTK_TREE_VIEW(tree)), _filename_select_func, sel);
    g_strfreev(elems);
  }
  filename->internal_change--;
}

static gboolean _filename_press(GtkWidget *w, GdkEventButton *e, _widgets_filename_t *filename)
{
  if(e->button == 3)
  {
    _filename_tree_update_visibility(w, filename);
    gtk_popover_set_default_widget(GTK_POPOVER(filename->pop), w);
    gtk_popover_set_relative_to(GTK_POPOVER(filename->pop), w);

    // update the selection
    _filename_update_selection(filename);

    gtk_widget_show_all(filename->pop);
    return TRUE;
  }
  return FALSE;
}

static gboolean _filename_update(dt_lib_filters_rule_t *rule, gchar *last_where_ext)
{
  if(!rule->w_specific) return FALSE;
  gchar *name = NULL;
  gchar *ext = NULL;
  _filename_decode(rule->raw_text, &name, &ext);

  _widgets_filename_t *filename = (_widgets_filename_t *)rule->w_specific;
  if(last_where_ext)
  {
    g_free(filename->last_where_ext);
    filename->last_where_ext = g_strdup(last_where_ext);
  }
  filename->tree_ok = FALSE;
  rule->manual_widget_set++;
  if(name) gtk_entry_set_text(GTK_ENTRY(filename->name), name);
  if(ext) gtk_entry_set_text(GTK_ENTRY(filename->ext), ext);
  rule->manual_widget_set--;

  g_free(name);
  g_free(ext);
  return TRUE;
}

static void _filename_popup_closed(GtkWidget *w, _widgets_filename_t *filename)
{
  // we validate the corresponding entry
  gtk_widget_activate(gtk_popover_get_default_widget(GTK_POPOVER(w)));
}

static void _filename_tree_row_activated(GtkTreeView *self, GtkTreePath *path, GtkTreeViewColumn *column,
                                         _widgets_filename_t *filename)
{
  // as the selection as already been updated on first click, we just close the popup
  gtk_widget_hide(filename->pop);
}

static void _filename_tree_selection_change(GtkTreeSelection *sel, _widgets_filename_t *filename)
{
  if(!filename || filename->internal_change) return;
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
  GtkWidget *entry = gtk_popover_get_default_widget(GTK_POPOVER(filename->pop));
  gtk_entry_set_text(GTK_ENTRY(entry), (txt == NULL) ? "" : txt);
  g_free(txt);
}

static void _filename_ok_clicked(GtkWidget *w, _widgets_filename_t *filename)
{
  gtk_widget_hide(filename->pop);
}

void _filename_tree_count_func(GtkTreeViewColumn *col, GtkCellRenderer *renderer, GtkTreeModel *model,
                               GtkTreeIter *iter, gpointer data)
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
    gchar *coltext = g_strdup_printf("%s (%d)", name, count);
    g_object_set(renderer, "text", coltext, NULL);
    g_free(coltext);
    g_object_set(renderer, "sensitive", TRUE, NULL);
  }

  g_free(name);
}

static void _filename_widget_init(dt_lib_filters_rule_t *rule, const dt_collection_properties_t prop,
                                  const gchar *text, dt_lib_module_t *self, const gboolean top)
{
  _widgets_filename_t *filename = (_widgets_filename_t *)g_malloc0(sizeof(_widgets_filename_t));
  filename->rule = rule;
  filename->last_where_ext = g_strdup("1=1"); // initialize the query to search the full db

  GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(rule->w_special_box), hb, TRUE, TRUE, 0);
  filename->name = gtk_entry_new();
  gtk_entry_set_width_chars(GTK_ENTRY(filename->name), (top) ? 10 : 0);
  gtk_widget_set_can_default(filename->name, TRUE);
  gtk_entry_set_placeholder_text(GTK_ENTRY(filename->name), _("filename"));
  gtk_widget_set_tooltip_text(filename->name, _("enter filename to search.\n"
                                                "multiple values can be separated by ','\n"
                                                "\nright-click to get existing filenames"));
  gtk_box_pack_start(GTK_BOX(hb), filename->name, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(filename->name), "activate", G_CALLBACK(_filename_changed), filename);
  g_signal_connect(G_OBJECT(filename->name), "focus-out-event", G_CALLBACK(_filename_focus_out), filename);
  g_signal_connect(G_OBJECT(filename->name), "button-press-event", G_CALLBACK(_filename_press), filename);

  filename->ext = gtk_entry_new();
  gtk_entry_set_width_chars(GTK_ENTRY(filename->ext), (top) ? 5 : 0);
  gtk_widget_set_can_default(filename->ext, TRUE);
  gtk_entry_set_placeholder_text(GTK_ENTRY(filename->ext), _("extension"));
  gtk_widget_set_tooltip_text(filename->ext, _("enter extension to search with starting dot\n"
                                               "multiple values can be separated by ','\n"
                                               "handled keywords: 'RAW', 'NOT RAW', 'LDR', 'HDR'\n"
                                               "\nright-click to get existing extensions"));
  gtk_box_pack_start(GTK_BOX(hb), filename->ext, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(filename->ext), "activate", G_CALLBACK(_filename_changed), filename);
  g_signal_connect(G_OBJECT(filename->ext), "focus-out-event", G_CALLBACK(_filename_focus_out), filename);
  g_signal_connect(G_OBJECT(filename->ext), "button-press-event", G_CALLBACK(_filename_press), filename);
  if(top)
  {
    dt_gui_add_class(hb, "dt_quick_filter");
  }

  // the popup
  filename->pop = gtk_popover_new(filename->name);
  gtk_widget_set_size_request(filename->pop, 250, 400);
  g_signal_connect(G_OBJECT(filename->pop), "closed", G_CALLBACK(_filename_popup_closed), filename);
  hb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(filename->pop), hb);

  // the name tree
  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  gtk_widget_set_no_show_all(sw, TRUE);
  gtk_box_pack_start(GTK_BOX(hb), sw, TRUE, TRUE, 0);
  GtkTreeModel *model
      = GTK_TREE_MODEL(gtk_list_store_new(TREE_NUM_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT));
  filename->name_tree = gtk_tree_view_new_with_model(model);
  gtk_widget_show(filename->name_tree);
  gtk_widget_set_tooltip_text(filename->name_tree, _("simple click to select filename\n"
                                                     "ctrl-click to select multiple values"));
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(filename->name_tree), FALSE);
  GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(filename->name_tree));
  gtk_tree_selection_set_mode(sel, GTK_SELECTION_MULTIPLE);
  g_signal_connect(G_OBJECT(filename->name_tree), "row-activated", G_CALLBACK(_filename_tree_row_activated),
                   filename);
  g_signal_connect(G_OBJECT(sel), "changed", G_CALLBACK(_filename_tree_selection_change), filename);

  GtkTreeViewColumn *col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(GTK_TREE_VIEW(filename->name_tree), col);
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_set_cell_data_func(col, renderer, _filename_tree_count_func, NULL, NULL);

  gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(filename->name_tree), TREE_COL_TOOLTIP);

  gtk_container_add(GTK_CONTAINER(sw), filename->name_tree);

  // the extension tree
  sw = gtk_scrolled_window_new(NULL, NULL);
  gtk_widget_set_no_show_all(sw, TRUE);
  gtk_box_pack_start(GTK_BOX(hb), sw, TRUE, TRUE, 0);
  model
      = GTK_TREE_MODEL(gtk_list_store_new(TREE_NUM_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT));
  filename->ext_tree = gtk_tree_view_new_with_model(model);
  gtk_widget_show(filename->ext_tree);
  gtk_widget_set_tooltip_text(filename->ext_tree, _("simple click to select extension\n"
                                                    "ctrl-click to select multiple values"));
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(filename->ext_tree), FALSE);
  sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(filename->ext_tree));
  gtk_tree_selection_set_mode(sel, GTK_SELECTION_MULTIPLE);
  g_signal_connect(G_OBJECT(filename->name_tree), "row-activated", G_CALLBACK(_filename_tree_row_activated),
                   filename);
  g_signal_connect(G_OBJECT(sel), "changed", G_CALLBACK(_filename_tree_selection_change), filename);

  col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(GTK_TREE_VIEW(filename->ext_tree), col);
  renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_set_cell_data_func(col, renderer, _filename_tree_count_func, NULL, NULL);

  gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(filename->ext_tree), TREE_COL_TOOLTIP);

  gtk_container_add(GTK_CONTAINER(sw), filename->ext_tree);

  // the button to close the popup
  GtkWidget *btn = gtk_button_new_with_label(_("ok"));
  gtk_box_pack_start(GTK_BOX(hb), btn, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(btn), "clicked", G_CALLBACK(_filename_ok_clicked), filename);

  rule->w_specific = filename;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
