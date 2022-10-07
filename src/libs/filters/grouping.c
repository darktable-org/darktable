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

typedef struct _widgets_grouping_t
{
  dt_lib_filtering_rule_t *rule;

  GtkWidget *combo;
} _widgets_grouping_t;

typedef enum _grouping_type_t
{
  _GROUPING_ALL = 0,
  _GROUPING_ORPHAN,
  _GROUPING_GROUP,
  _GROUPING_LEADER,
  _GROUPING_FOLLOWER
} _grouping_type_t;

static void _grouping_synchronise(_widgets_grouping_t *source)
{
  _widgets_grouping_t *dest = NULL;
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

static void _grouping_decode(const gchar *txt, int *val)
{
  if(!txt || strlen(txt) == 0) return;

  if(!g_strcmp0(txt, "$NO_GROUP"))
    *val = _GROUPING_ORPHAN;
  else if(!g_strcmp0(txt, "$GROUP"))
    *val = _GROUPING_GROUP;
  else if(!g_strcmp0(txt, "$LEADER"))
    *val = _GROUPING_LEADER;
  else if(!g_strcmp0(txt, "$FOLLOWER"))
    *val = _GROUPING_FOLLOWER;
  else
    *val = _GROUPING_ALL;
}

static void _grouping_changed(GtkWidget *widget, gpointer user_data)
{
  _widgets_grouping_t *grouping = (_widgets_grouping_t *)user_data;
  if(grouping->rule->manual_widget_set) return;

  const _grouping_type_t tp = dt_bauhaus_combobox_get(grouping->combo);
  switch(tp)
  {
    case _GROUPING_ALL:
      _rule_set_raw_text(grouping->rule, "", TRUE);
      break;
    case _GROUPING_ORPHAN:
      _rule_set_raw_text(grouping->rule, "$NO_GROUP", TRUE);
      break;
    case _GROUPING_GROUP:
      _rule_set_raw_text(grouping->rule, "$GROUP", TRUE);
      break;
    case _GROUPING_LEADER:
      _rule_set_raw_text(grouping->rule, "$LEADER", TRUE);
      break;
    case _GROUPING_FOLLOWER:
      _rule_set_raw_text(grouping->rule, "$FOLLOWER", TRUE);
      break;
  }
  _grouping_synchronise(grouping);
}

static gboolean _grouping_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;
  int val = _GROUPING_ALL;
  _grouping_decode(rule->raw_text, &val);

  rule->manual_widget_set++;
  _widgets_grouping_t *grouping = (_widgets_grouping_t *)rule->w_specific;
  char query[1024] = { 0 };
  // clang-format off
  g_snprintf(query, sizeof(query),
                   "SELECT gr_count, COUNT(gr_count) "
                   " FROM (SELECT COUNT(*) AS gr_count "
                   "        FROM main.images "
                   "        WHERE %s "
                   "        GROUP BY group_id)"
                   " GROUP BY gr_count "
                   " ORDER BY gr_count",
                   rule->lib->last_where_ext);
  // clang-format on
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  int nb_no_group = 0;
  int nb_group = 0;
  int nb_leader = 0;
  int nb_follower = 0;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int items = sqlite3_column_int(stmt, 0);
    const int count = sqlite3_column_int(stmt, 1);

    if(items == 1)
      nb_no_group += count;
    else if(items > 1)
    {
      nb_group += count * items;
      nb_leader += count;
      nb_follower += count * (items - 1);
    }
  }
  sqlite3_finalize(stmt);

  gchar *item = g_strdup_printf("%s (%d)", _("ungrouped images"), nb_no_group);
  dt_bauhaus_combobox_set_entry_label(grouping->combo, 1, item);
  g_free(item);
  item = g_strdup_printf("%s (%d)", _("grouped images"), nb_group);
  dt_bauhaus_combobox_set_entry_label(grouping->combo, 2, item);
  g_free(item);
  item = g_strdup_printf("%s (%d)", _("group leaders"), nb_leader);
  dt_bauhaus_combobox_set_entry_label(grouping->combo, 3, item);
  g_free(item);
  item = g_strdup_printf("%s (%d)", _("group followers"), nb_follower);
  dt_bauhaus_combobox_set_entry_label(grouping->combo, 4, item);
  g_free(item);

  dt_bauhaus_combobox_set(grouping->combo, val);
  _grouping_synchronise(grouping);
  rule->manual_widget_set--;

  return TRUE;
}

static void _grouping_widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                                  const gchar *text, dt_lib_module_t *self, const gboolean top)
{
  _widgets_grouping_t *grouping = (_widgets_grouping_t *)g_malloc0(sizeof(_widgets_grouping_t));
  grouping->rule = rule;

  DT_BAUHAUS_COMBOBOX_NEW_FULL(grouping->combo, self, NULL, N_("grouping filter"),
                               _("select the type of grouped image to filter"), 0, _grouping_changed,
                               grouping, N_("all images"), N_("ungrouped images"), N_("grouped images"),
                               N_("group leaders"), N_("group followers"));
  DT_BAUHAUS_WIDGET(grouping->combo)->show_label = FALSE;

  if(top)
    gtk_box_pack_start(GTK_BOX(rule->w_special_box_top), grouping->combo, TRUE, TRUE, 0);
  else
    gtk_box_pack_start(GTK_BOX(rule->w_special_box), grouping->combo, TRUE, TRUE, 0);

  if(top)
  {
    dt_gui_add_class(grouping->combo, "dt_quick_filter");
  }

  if(top)
    rule->w_specific_top = grouping;
  else
    rule->w_specific = grouping;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
