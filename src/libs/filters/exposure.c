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

#include "common/utility.h"

static gboolean _exposure_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;

  dt_lib_filtering_t *d = rule->lib;
  _widgets_range_t *special = (_widgets_range_t *)rule->w_specific;
  _widgets_range_t *specialtop = (_widgets_range_t *)rule->w_specific_top;
  GtkDarktableRangeSelect *range = DTGTK_RANGE_SELECT(special->range_select);
  GtkDarktableRangeSelect *rangetop = (specialtop) ? DTGTK_RANGE_SELECT(specialtop->range_select) : NULL;

  rule->manual_widget_set++;
  const gchar *colname = "exposure";

  // first, we update the graph

  // get min and max
  dt_filter_range_t *exposurerange = dt_filter_get_range(colname, d->last_where_ext);
  range->min_r = exposurerange->min_r;
  range->max_r = exposurerange->max_r;
  dt_filter_range_free(exposurerange);

  GList *counts = dt_filter_get_range_counts(colname, d->last_where_ext);
  for(GList *iter = counts; iter; iter = g_list_next(iter))
  {
    dt_filter_range_count_t *count = iter->data;
    dtgtk_range_select_add_block(range, count->value_r, count->count);
    if(rangetop) dtgtk_range_select_add_block(rangetop, count->value_r, count->count);
  }
  g_list_free_full(counts, dt_filter_range_count_free);

  // and setup the selection
  dtgtk_range_select_set_selection_from_raw_text(range, rule->raw_text, FALSE);
  if(rangetop) dtgtk_range_select_set_selection_from_raw_text(rangetop, rule->raw_text, FALSE);
  rule->manual_widget_set--;

  dtgtk_range_select_redraw(range);
  if(rangetop) dtgtk_range_select_redraw(rangetop);
  return TRUE;
}

static double _exposure_value_to_band_func(const double value)
{
  return pow(value, 0.25);
}

static double _exposure_value_from_band_func(const double value)
{
  return pow(value, 4);
}

static gchar *_exposure_print_func(const double value, const gboolean detailled)
{
  if(detailled)
  {
    return dt_util_format_exposure(value);
  }
  else
  {
    return dt_util_float_to_str("%.6lf", value);
  }
}

static void _exposure_widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                                  const gchar *text, dt_lib_module_t *self, const gboolean top)
{
  _widgets_range_t *special = g_malloc0(sizeof(_widgets_range_t));

  special->range_select
      = dtgtk_range_select_new(dt_collection_name_untranslated(prop), !top, DT_RANGE_TYPE_NUMERIC);
  if(top) gtk_widget_set_size_request(special->range_select, 160, -1);
  GtkDarktableRangeSelect *range = DTGTK_RANGE_SELECT(special->range_select);

  if(!top)
  {
    gtk_entry_set_width_chars(GTK_ENTRY(range->entry_min), 10);
    gtk_entry_set_width_chars(GTK_ENTRY(range->entry_max), 10);
  }
  dtgtk_range_select_set_selection_from_raw_text(range, text, FALSE);
  dtgtk_range_select_set_band_func(range, _exposure_value_from_band_func, _exposure_value_to_band_func);
  dtgtk_range_select_add_marker(range, 1.0, TRUE);
  range->print = _exposure_print_func;

  dt_filter_range_t *exposurerange = dt_filter_get_range("exposure", NULL);
  range->min_r = floor(exposurerange->min_r);
  range->max_r = floor(exposurerange->max_r) + 1;
  dt_filter_range_free(exposurerange);

  _range_widget_add_to_rule(rule, special, top);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
