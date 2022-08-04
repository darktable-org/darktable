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

typedef struct _widgets_history_t
{
  dt_lib_filtering_rule_t *rule;

  GtkWidget *combo;
} _widgets_history_t;

typedef enum _history_type_t
{
  _HISTORY_ALL = 0,
  _HISTORY_BASIC,
  _HISTORY_AUTO,
  _HISTORY_ALTERED
} _history_type_t;

static const char *_history_names[] = { N_("All images"), N_("Basic"), N_("Auto applied"), N_("Altered"), NULL };

static void _history_synchronise(_widgets_history_t *source)
{
  _widgets_history_t *dest = NULL;
  if(source == source->rule->w_specific_top)
    dest = source->rule->w_specific;
  else
    dest = source->rule->w_specific_top;

  if(dest)
  {
    source->rule->manual_widget_set++;
    const int val = dt_bauhaus_combobox_get(source->combo);
    dt_bauhaus_combobox_set(dest->combo, val);
    source->rule->manual_widget_set--;
  }
}

static void _history_decode(const gchar *txt, int *val)
{
  if(!txt || strlen(txt) == 0) return;

  if(!g_strcmp0(txt, "$BASIC"))
    *val = _HISTORY_BASIC;
  else if(!g_strcmp0(txt, "$AUTO_APPLIED"))
    *val = _HISTORY_AUTO;
  else if(!g_strcmp0(txt, "$ALTERED"))
    *val = _HISTORY_ALTERED;
  else
    *val = _HISTORY_ALL;
}

static void _history_changed(GtkWidget *widget, gpointer user_data)
{
  _widgets_history_t *history = (_widgets_history_t *)user_data;
  if(history->rule->manual_widget_set) return;

  const _history_type_t tp = dt_bauhaus_combobox_get(history->combo);
  switch(tp)
  {
    case _HISTORY_ALL:
      _rule_set_raw_text(history->rule, "", TRUE);
      break;
    case _HISTORY_BASIC:
      _rule_set_raw_text(history->rule, "$BASIC", TRUE);
      break;
    case _HISTORY_AUTO:
      _rule_set_raw_text(history->rule, "$AUTO_APPLIED", TRUE);
      break;
    case _HISTORY_ALTERED:
      _rule_set_raw_text(history->rule, "$ALTERED", TRUE);
      break;
  }
  _history_synchronise(history);
}

static gboolean _history_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;
  int val = _HISTORY_ALL;
  _history_decode(rule->raw_text, &val);

  rule->manual_widget_set++;
  _widgets_history_t *history = (_widgets_history_t *)rule->w_specific;
  char query[1024] = { 0 };
  // clang-format off
  g_snprintf(query, sizeof(query),
                   "SELECT CASE"
                   "       WHEN basic_hash == current_hash THEN 0"
                   "       WHEN auto_hash == current_hash THEN 1"
                   "       WHEN current_hash IS NOT NULL THEN 2"
                   "       ELSE 0"
                   "     END as altered, COUNT(*) AS count"
                   " FROM main.images AS mi"
                   " LEFT JOIN (SELECT DISTINCT imgid, basic_hash, auto_hash, current_hash"
                   "            FROM main.history_hash) ON id = imgid"
                   " WHERE %s"
                   " GROUP BY altered"
                   " ORDER BY altered ASC",
                   rule->lib->last_where_ext);
  // clang-format on
  int counts[3] = { 0 };
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int i = sqlite3_column_int(stmt, 0);
    const int count = sqlite3_column_int(stmt, 1);
    counts[i] = count;
  }
  sqlite3_finalize(stmt);

  for(int i = 0; i < 3; i++)
  {
    gchar *item = g_strdup_printf("%s (%d)", _(_history_names[i + 1]), counts[i]);

    dt_bauhaus_combobox_set_entry_label(history->combo, i + 1, item);
    g_free(item);
  }

  dt_bauhaus_combobox_set(history->combo, val);
  _history_synchronise(history);
  rule->manual_widget_set--;

  return TRUE;
}

static void _history_widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                                 const gchar *text, dt_lib_module_t *self, const gboolean top)
{
  _widgets_history_t *history = (_widgets_history_t *)g_malloc0(sizeof(_widgets_history_t));
  history->rule = rule;

  history->combo
      = dt_bauhaus_combobox_new_full(DT_ACTION(self), NULL, N_("History filter"), _("Filter on history state"), 0,
                                     (GtkCallback)_history_changed, history, _history_names);
  DT_BAUHAUS_WIDGET(history->combo)->show_label = FALSE;

  if(top)
    gtk_box_pack_start(GTK_BOX(rule->w_special_box_top), history->combo, TRUE, TRUE, 0);
  else
    gtk_box_pack_start(GTK_BOX(rule->w_special_box), history->combo, TRUE, TRUE, 0);

  if(top)
  {
    dt_gui_add_class(history->combo, "dt_quick_filter");
  }

  if(top)
    rule->w_specific_top = history;
  else
    rule->w_specific = history;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
