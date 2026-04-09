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

static gchar *_date_get_db_colname(dt_lib_filtering_rule_t *rule)
{
  switch(rule->prop)
  {
    case DT_COLLECTION_PROP_IMPORT_TIMESTAMP:
      return g_strdup("import_timestamp");
    case DT_COLLECTION_PROP_CHANGE_TIMESTAMP:
      return g_strdup("change_timestamp");
    case DT_COLLECTION_PROP_EXPORT_TIMESTAMP:
      return g_strdup("export_timestamp");
    case DT_COLLECTION_PROP_PRINT_TIMESTAMP:
      return g_strdup("print_timestamp");
    default:
      return g_strdup("datetime_taken");
  }
}

static gboolean _date_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;

  dt_lib_filtering_t *d = rule->lib;
  _widgets_range_t *special = (_widgets_range_t *)rule->w_specific;
  _widgets_range_t *specialtop = (_widgets_range_t *)rule->w_specific_top;
  GtkDarktableRangeSelect *range = DTGTK_RANGE_SELECT(special->range_select);
  GtkDarktableRangeSelect *rangetop = (specialtop) ? DTGTK_RANGE_SELECT(specialtop->range_select) : NULL;

  rule->manual_widget_set++;
  // first, we update the graph
  gchar *colname = _date_get_db_colname(rule);
  char query[1024] = { 0 };
  // clang-format off
  g_snprintf(query, sizeof(query),
             "SELECT %s AS date, COUNT(*) AS count"
             " FROM main.images AS mi"
             " WHERE %s IS NOT NULL AND %s"
             " GROUP BY date",
             colname, colname, d->last_where_ext);
  // clang-format on
  g_free(colname);
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  dtgtk_range_select_reset_blocks(range);
  if(rangetop) dtgtk_range_select_reset_blocks(rangetop);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int count = sqlite3_column_int(stmt, 1);
    const int64_t dt = sqlite3_column_int64(stmt, 0);
    dtgtk_range_select_add_block(range, dt, count);
    if(rangetop) dtgtk_range_select_add_block(rangetop, dt, count);
  }
  sqlite3_finalize(stmt);

  // and setup the selection
  dtgtk_range_select_set_selection_from_raw_text(range, rule->raw_text, FALSE);
  if(rangetop) dtgtk_range_select_set_selection_from_raw_text(rangetop, rule->raw_text, FALSE);
  rule->manual_widget_set--;

  dtgtk_range_select_redraw(range);
  if(rangetop) dtgtk_range_select_redraw(rangetop);
  return TRUE;
}

typedef struct _widgets_month_t
{
  dt_lib_filtering_rule_t *rule;
  GtkWidget *toggles[12];
} _widgets_month_t;

static gboolean _month_update(dt_lib_filtering_rule_t *rule);

static int _month_get_mask(const char *text)
{
  if(g_str_has_prefix(text, "0x"))
    return (int)strtoll(&text[2], NULL, 16);
  else
    return 0;
}

static void _month_set_mask(dt_lib_filtering_rule_t *rule, const int mask, const gboolean signal)
{
  gchar *txt = g_strdup_printf("0x%x", mask);
  _rule_set_raw_text(rule, txt, signal);
  g_free(txt);
}

static gchar *_month_pretty_print(const gchar *raw_txt)
{
  const int mask = _month_get_mask(raw_txt);
  if(mask == 0 || mask == 0xFFF) return g_strdup(_("all"));

  gchar *txt = NULL;
  for(int i = 0; i < 12; i++)
  {
    if(mask & (1 << i))
    {
      if(txt)
        dt_util_str_cat(&txt, ", %s", Q_(dt_month_short_names[i]));
      else
        txt = g_strdup(Q_(dt_month_short_names[i]));
    }
  }
  return txt ? txt : g_strdup(_("all"));
}

static void _month_widget_changed(GtkToggleButton *button, gpointer user_data)
{
  _widgets_month_t *month = (_widgets_month_t *)user_data;
  if(month->rule->manual_widget_set) return;
  if(month->rule->lib->leaving) return;

  int mask = 0;
  for(int i = 0; i < 12; i++)
  {
    if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(month->toggles[i])))
      mask |= (1 << i);
  }

  _month_set_mask(month->rule, mask, TRUE);

  // synchronize the other widget if any
  _month_update(month->rule);
}

static gboolean _month_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;

  rule->manual_widget_set++;
  _widgets_month_t *month = (_widgets_month_t *)rule->w_specific;
  _widgets_month_t *monthtop = (_widgets_month_t *)rule->w_specific_top;

  const int mask = _month_get_mask(rule->raw_text);

  for(int i = 0; i < 12; i++)
  {
    const gboolean active = (mask & (1 << i)) != 0;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(month->toggles[i]), active);
    if(monthtop)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(monthtop->toggles[i]), active);
  }

  rule->manual_widget_set--;
  return TRUE;
}

static void _month_widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                               const gchar *text, dt_lib_module_t *self, const gboolean top)
{
  _widgets_month_t *month = g_malloc0(sizeof(_widgets_month_t));
  month->rule = rule;
  if(top)
    rule->w_specific_top = month;
  else
    rule->w_specific = month;

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(hbox, "filter-month-box");

  for(int i = 0; i < 12; i++)
  {
    month->toggles[i] = gtk_toggle_button_new_with_label(Q_(dt_month_short_names[i]));
    gtk_widget_set_tooltip_text(month->toggles[i],
                                _("filter by capture month\n"
                                  "click to toggle month selection"));
    g_signal_connect(G_OBJECT(month->toggles[i]), "toggled",
                     G_CALLBACK(_month_widget_changed), month);
    gtk_box_pack_start(GTK_BOX(hbox), month->toggles[i], TRUE, TRUE, 0);
  }

  // apply initial state from text
  if(text && text[0])
  {
    const int mask = _month_get_mask(text);
    rule->manual_widget_set++;
    for(int i = 0; i < 12; i++)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(month->toggles[i]),
                                   (mask & (1 << i)) != 0);
    rule->manual_widget_set--;
  }

  if(top)
    gtk_box_pack_start(GTK_BOX(rule->w_special_box_top), hbox, TRUE, TRUE, 0);
  else
    gtk_box_pack_start(GTK_BOX(rule->w_special_box), hbox, TRUE, TRUE, 0);
}

static void _date_widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                              const gchar *text, dt_lib_module_t *self, const gboolean top)
{
  _widgets_range_t *special = g_malloc0(sizeof(_widgets_range_t));

  special->range_select
      = dtgtk_range_select_new(dt_collection_name_untranslated(prop), !top, DT_RANGE_TYPE_DATETIME);
  if(top) gtk_widget_set_size_request(special->range_select, 160, -1);
  GtkDarktableRangeSelect *range = DTGTK_RANGE_SELECT(special->range_select);

  range->type = DT_RANGE_TYPE_DATETIME;
  range->step_bd = 86400; // step of 1 day (in seconds)
  dtgtk_range_select_set_selection_from_raw_text(range, text, FALSE);

  gchar *colname = _date_get_db_colname(rule);
  char query[1024] = { 0 };
  // clang-format off
  g_snprintf(query, sizeof(query),
             "SELECT MIN(%s), MAX(%s)"
             " FROM main.images"
             " WHERE %s IS NOT NULL",
             colname, colname, colname);
  // clang-format on
  g_free(colname);
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    range->min_r = sqlite3_column_int64(stmt, 0);
    range->max_r = sqlite3_column_int64(stmt, 1);
  }
  sqlite3_finalize(stmt);

  _range_widget_add_to_rule(rule, special, top);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
