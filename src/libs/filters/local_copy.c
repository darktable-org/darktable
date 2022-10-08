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

typedef struct _widgets_local_copy_t
{
  dt_lib_filters_rule_t *rule;

  GtkWidget *combo;
} _widgets_local_copy_t;

typedef enum _local_copy_type_t
{
  _LCP_ALL = 0,
  _LCP_YES,
  _LCP_NO
} _local_copy_type_t;

static const char *_local_copy_names[]
    = { N_("all images"), N_("copied locally"), N_("not copied locally"), NULL };

static void _local_copy_decode(const gchar *txt, int *val)
{
  if(!txt || strlen(txt) == 0) return;

  if(!g_strcmp0(txt, "$LOCAL_COPY"))
    *val = _LCP_YES;
  else if(!g_strcmp0(txt, "$NO_LOCAL_COPY"))
    *val = _LCP_NO;
  else
    *val = _LCP_ALL;
}

static void _local_copy_changed(GtkWidget *widget, gpointer user_data)
{
  _widgets_local_copy_t *local_copy = (_widgets_local_copy_t *)user_data;
  if(local_copy->rule->manual_widget_set) return;

  const _local_copy_type_t tp = dt_bauhaus_combobox_get(local_copy->combo);
  switch(tp)
  {
    case _LCP_ALL:
      _rule_set_raw_text(local_copy->rule, "", TRUE);
      break;
    case _LCP_NO:
      _rule_set_raw_text(local_copy->rule, "$NO_LOCAL_COPY", TRUE);
      break;
    case _LCP_YES:
      _rule_set_raw_text(local_copy->rule, "$LOCAL_COPY", TRUE);
      break;
  }
}

static gboolean _local_copy_update(dt_lib_filters_rule_t *rule, gchar *last_where_ext)
{
  if(!rule->w_specific) return FALSE;
  int val = _LCP_ALL;
  _local_copy_decode(rule->raw_text, &val);

  rule->manual_widget_set++;
  _widgets_local_copy_t *local_copy = (_widgets_local_copy_t *)rule->w_specific;
  char query[1024] = { 0 };
  // clang-format off
  g_snprintf(query, sizeof(query),
                   "SELECT CASE "
                   "         WHEN (flags & %d) THEN 0"
                   "         ELSE 1"
                   "       END as lcp, COUNT(*) AS count"
                   " FROM main.images AS mi "
                   " WHERE %s"
                   " GROUP BY lcp ORDER BY lcp ASC",
                   DT_IMAGE_LOCAL_COPY, last_where_ext);
  // clang-format on
  int counts[2] = { 0 };
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int i = sqlite3_column_int(stmt, 0);
    const int count = sqlite3_column_int(stmt, 1);
    counts[i] = count;
  }
  sqlite3_finalize(stmt);

  for(int i = 0; i < 2; i++)
  {
    gchar *item = g_strdup_printf("%s (%d)", _(_local_copy_names[i + 1]), counts[i]);
    dt_bauhaus_combobox_set_entry_label(local_copy->combo, i + 1, item);
    g_free(item);
  }

  dt_bauhaus_combobox_set(local_copy->combo, val);
  rule->manual_widget_set--;

  return TRUE;
}

static void _local_copy_widget_init(dt_lib_filters_rule_t *rule, const dt_collection_properties_t prop,
                                    const gchar *text, dt_lib_module_t *self, const gboolean top)
{
  _widgets_local_copy_t *local_copy = (_widgets_local_copy_t *)g_malloc0(sizeof(_widgets_local_copy_t));
  local_copy->rule = rule;

  local_copy->combo = dt_bauhaus_combobox_new_full(
      DT_ACTION(self), NULL, N_("local_copy filter"), _("local copied state filter"), 0,
      (GtkCallback)_local_copy_changed, local_copy, _local_copy_names);
  DT_BAUHAUS_WIDGET(local_copy->combo)->show_label = FALSE;

  gtk_box_pack_start(GTK_BOX(rule->w_special_box), local_copy->combo, TRUE, TRUE, 0);

  if(top)
  {
    dt_gui_add_class(local_copy->combo, "dt_quick_filter");
  }

  rule->w_specific = local_copy;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
