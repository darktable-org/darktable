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


static gboolean _iso_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;

  dt_lib_filtering_t *d = rule->lib;
  _widgets_range_t *special = (_widgets_range_t *)rule->w_specific;
  _widgets_range_t *specialtop = (_widgets_range_t *)rule->w_specific_top;
  GtkDarktableRangeSelect *range = DTGTK_RANGE_SELECT(special->range_select);
  GtkDarktableRangeSelect *rangetop = (specialtop) ? DTGTK_RANGE_SELECT(specialtop->range_select) : NULL;

  rule->manual_widget_set++;
  // first, we update the graph
  char query[1024] = { 0 };
  // clang-format off
  g_snprintf(query, sizeof(query),
             "SELECT ROUND(iso,0), COUNT(*) AS count"
             " FROM main.images AS mi"
             " WHERE %s"
             " GROUP BY ROUND(iso, 0)",
             d->last_where_ext);
  // clang-format on
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  dtgtk_range_select_reset_blocks(range);
  if(rangetop) dtgtk_range_select_reset_blocks(rangetop);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const double val = sqlite3_column_double(stmt, 0);
    const int count = sqlite3_column_int(stmt, 1);

    dtgtk_range_select_add_block(range, val, count);
    if(rangetop) dtgtk_range_select_add_block(rangetop, val, count);
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

static double _iso_value_to_band_func(const double value)
{
  if(value <= 1) return 0; // this shouldn't happen
  return log2(value / 100.0);
}

static double _iso_value_from_band_func(const double value)
{
  return 100 * pow(2.0, value);
}

static gchar *_iso_print_func(const double value, const gboolean detailled)
{
  if(detailled)
  {
    // we round the value
    double v = value;
    if(value < 200)
      v = round(v / 25) * 25;
    else
      v = round(v / 50) * 50;
    return g_strdup_printf("%.0lf ISO", v);
  }
  else
  {
    gchar *locale = g_strdup(setlocale(LC_ALL, NULL));
    setlocale(LC_NUMERIC, "C");
    gchar *txt = g_strdup_printf("%.0lf", value);
    setlocale(LC_NUMERIC, locale);
    g_free(locale);
    return txt;
  }
}

static void _iso_widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                             const gchar *text, dt_lib_module_t *self, const gboolean top)
{
  _widgets_range_t *special = (_widgets_range_t *)g_malloc0(sizeof(_widgets_range_t));

  special->range_select
      = dtgtk_range_select_new(dt_collection_name_untranslated(prop), !top, DT_RANGE_TYPE_NUMERIC);
  if(top) gtk_widget_set_size_request(special->range_select, 160, -1);
  GtkDarktableRangeSelect *range = DTGTK_RANGE_SELECT(special->range_select);

  dtgtk_range_select_set_selection_from_raw_text(range, text, FALSE);
  dtgtk_range_select_set_band_func(range, _iso_value_from_band_func, _iso_value_to_band_func);
  range->print = _iso_print_func;

  char query[1024] = { 0 };
  // clang-format off
  g_snprintf(query, sizeof(query),
             "SELECT MIN(iso), MAX(iso)"
             " FROM main.images");
  // clang-format on
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  double min = 50;
  double max = 12800;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    min = sqlite3_column_double(stmt, 0);
    max = sqlite3_column_double(stmt, 1);
  }
  sqlite3_finalize(stmt);
  range->min_r = floor(min);
  range->max_r = floor(max) + 1;

  _range_widget_add_to_rule(rule, special, top);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
