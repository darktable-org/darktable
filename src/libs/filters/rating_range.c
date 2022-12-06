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

static gboolean _rating_range_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;
  _widgets_range_t *special = (_widgets_range_t *)rule->w_specific;
  _widgets_range_t *specialtop = (_widgets_range_t *)rule->w_specific_top;
  GtkDarktableRangeSelect *range = DTGTK_RANGE_SELECT(special->range_select);
  GtkDarktableRangeSelect *rangetop = (specialtop) ? DTGTK_RANGE_SELECT(specialtop->range_select) : NULL;

  rule->manual_widget_set++;
  char query[1024] = { 0 };
  // clang-format off
  g_snprintf(query, sizeof(query),
             "SELECT CASE WHEN (flags & 8) == 8 THEN -1 ELSE (flags & 7) END AS rating,"
             " COUNT(*) AS count"
             " FROM main.images AS mi"
             " WHERE %s"
             " GROUP BY rating"
             " ORDER BY rating",
             rule->lib->last_where_ext);
  // clang-format on
  int nb[7] = { 0 };
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int val = sqlite3_column_int(stmt, 0);
    const int count = sqlite3_column_int(stmt, 1);

    if(val < 6 && val >= -1) nb[val + 1] += count;
  }
  sqlite3_finalize(stmt);

  dtgtk_range_select_reset_blocks(range);
  dtgtk_range_select_add_range_block(range, 1.0, 1.0, DT_RANGE_BOUND_MIN | DT_RANGE_BOUND_MAX, _("all images"),
                                     nb[0] + nb[1] + nb[2] + nb[3] + nb[4] + nb[5] + nb[6]);
  dtgtk_range_select_add_range_block(range, 0.0, 1.0, DT_RANGE_BOUND_MAX, _("all except rejected"),
                                     nb[1] + nb[2] + nb[3] + nb[4] + nb[5] + nb[6]);
  dtgtk_range_select_add_range_block(range, -1.0, -1.0, DT_RANGE_BOUND_FIXED, _("rejected only"), nb[0]);
  dtgtk_range_select_add_range_block(range, 0.0, 0.0, DT_RANGE_BOUND_FIXED, _("not rated only"), nb[1]);
  dtgtk_range_select_add_range_block(range, 1.0, 5.0, DT_RANGE_BOUND_MAX, "★", nb[2]);
  dtgtk_range_select_add_range_block(range, 2.0, 5.0, DT_RANGE_BOUND_MAX, "★ ★", nb[3]);
  dtgtk_range_select_add_range_block(range, 3.0, 5.0, DT_RANGE_BOUND_MAX, "★ ★ ★", nb[4]);
  dtgtk_range_select_add_range_block(range, 4.0, 5.0, DT_RANGE_BOUND_MAX, "★ ★ ★ ★", nb[5]);
  dtgtk_range_select_add_range_block(range, 5.0, 5.0, DT_RANGE_BOUND_MAX, "★ ★ ★ ★ ★", nb[6]);

  if(rangetop)
  {
    dtgtk_range_select_reset_blocks(rangetop);
    dtgtk_range_select_add_range_block(rangetop, 1.0, 1.0, DT_RANGE_BOUND_MIN | DT_RANGE_BOUND_MAX,
                                       _("all images"), nb[0] + nb[1] + nb[2] + nb[3] + nb[4] + nb[5] + nb[6]);
    dtgtk_range_select_add_range_block(rangetop, 0.0, 1.0, DT_RANGE_BOUND_MAX, _("all except rejected"),
                                       nb[1] + nb[2] + nb[3] + nb[4] + nb[5] + nb[6]);
    dtgtk_range_select_add_range_block(rangetop, -1.0, -1.0, DT_RANGE_BOUND_FIXED, _("rejected only"), nb[0]);
    dtgtk_range_select_add_range_block(rangetop, 0.0, 0.0, DT_RANGE_BOUND_FIXED, _("not rated only"), nb[1]);
    dtgtk_range_select_add_range_block(rangetop, 1.0, 5.0, DT_RANGE_BOUND_MAX, "★", nb[2]);
    dtgtk_range_select_add_range_block(rangetop, 2.0, 5.0, DT_RANGE_BOUND_MAX, "★ ★", nb[3]);
    dtgtk_range_select_add_range_block(rangetop, 3.0, 5.0, DT_RANGE_BOUND_MAX, "★ ★ ★", nb[4]);
    dtgtk_range_select_add_range_block(rangetop, 4.0, 5.0, DT_RANGE_BOUND_MAX, "★ ★ ★ ★", nb[5]);
    dtgtk_range_select_add_range_block(rangetop, 5.0, 5.0, DT_RANGE_BOUND_MAX, "★ ★ ★ ★ ★", nb[6]);
  }

  dtgtk_range_select_set_selection_from_raw_text(range, rule->raw_text, FALSE);
  if(rangetop) dtgtk_range_select_set_selection_from_raw_text(rangetop, rule->raw_text, FALSE);
  rule->manual_widget_set--;
  return TRUE;
}

static gchar *_rating_print_func(const double value, const gboolean detailled)
{
  if(detailled)
  {
    darktable.control->element = value + 1;

    switch((int)floor(value))
    {
      case -1:
        return g_strdup(_("rejected"));
      case 0:
        return g_strdup(_("not rated"));
    }
  }
  return g_strdup_printf("%.0lf", floor(value));
}

static gchar *_rating_get_bounds_pretty(GtkDarktableRangeSelect *range)
{
  if((range->bounds & DT_RANGE_BOUND_MIN) && (range->bounds & DT_RANGE_BOUND_MAX)) return g_strdup(_("all images"));

  if((range->bounds & DT_RANGE_BOUND_MIN)) range->select_min_r = range->min_r;
  if((range->bounds & DT_RANGE_BOUND_MAX)) range->select_max_r = range->max_r;

  if(range->select_min_r == range->select_max_r)
  {
    gchar *printed_min = range->print(range->select_min_r, TRUE);
    gchar *min_only = g_strdup_printf("%s %s", printed_min, _("only"));
    g_free(printed_min);
    return min_only;
  }

  const int rating_min = (int)floor(range->select_min_r);
  const int rating_max = (int)floor(range->select_max_r);

  if(rating_min == -1 && rating_max == 0)
    return g_strdup_printf("%s + %s", _("rejected"), _("not rated"));

  if(range->bounds & DT_RANGE_BOUND_MIN)
  {
    gchar *printed_max = range->print(range->select_max_r, TRUE);
    gchar *lt_max_rejected = g_strdup_printf("≤%s + %s", printed_max, _("rejected"));
    g_free(printed_max);
    return lt_max_rejected;
  }
  else if(range->bounds & DT_RANGE_BOUND_MAX)
  {
    if(rating_min == 0)
      return g_strdup(_("all except rejected"));
    else
    {
      gchar *printed_min = range->print(range->select_min_r, TRUE);
      gchar *gt_min = g_strdup_printf("≥%s", printed_min);
      g_free(printed_min);
      return gt_min;
    }
  }
  else if(rating_min == 0)
  {
    gchar *printed_max = range->print(range->select_max_r, TRUE);
    gchar *lt_max = g_strdup_printf("≤%s", printed_max);
    g_free(printed_max);
    return lt_max;
  }

  return dtgtk_range_select_get_bounds_pretty(range);
}

static void _rating_paint_icon(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  // first, we set the color depending on the flags
  void *my_data = NULL;
  GdkRGBA shade_color;

  if((flags & CPF_PRELIGHT) || (flags & CPF_ACTIVE))
  {
    // we want a filled icon
    cairo_get_source(cr);
    cairo_pattern_get_rgba(cairo_get_source(cr), &shade_color.red, &shade_color.green, &shade_color.blue,
                           &shade_color.alpha);
    shade_color.alpha *= 0.6;
    my_data = &shade_color;
  }

  // then we draw the regular icon
  dtgtk_cairo_paint_star(cr, x, y, w, h, flags, my_data);
}

enum
{
  // DT_ACTION_EFFECT_TOGGLE = DT_ACTION_EFFECT_DEFAULT_KEY,
  _ACTION_EFFECT_BETTER = DT_ACTION_EFFECT_DEFAULT_UP,
  _ACTION_EFFECT_WORSE = DT_ACTION_EFFECT_DEFAULT_DOWN,
  _ACTION_EFFECT_CAP,
};

enum
{
  _ACTION_ELEMENT_MAX = 5 + 1 + 1,
};

static float _action_process_ratings(gpointer target, dt_action_element_t element, dt_action_effect_t effect, float move_size)
{
  if(!target) return NAN;

  double new_value = element - 1.0;
  GtkDarktableRangeSelect *range = target;
  double min = range->select_min_r;
  double max = range->select_max_r;
  dt_range_bounds_t bounds = range->bounds;

  if(!isnan(move_size))
  {
    switch(effect)
    {
    case DT_ACTION_EFFECT_TOGGLE:
      if(element != _ACTION_ELEMENT_MAX && (min != new_value || bounds & DT_RANGE_BOUND_MIN))
      {
        if(max == min) max = new_value;
        min = new_value;
        if(min > max) max = min;
        bounds &= ~DT_RANGE_BOUND_MIN;
      }
      else
        bounds ^= DT_RANGE_BOUND_MAX;
      break;
    case _ACTION_EFFECT_BETTER:
      if(element != _ACTION_ELEMENT_MAX)
      {
        if(min < 5) min++;
        if(min > max) max = min;
        bounds &= ~DT_RANGE_BOUND_MIN;
      }
      else
      {
        if(max < 5) max++;
        bounds &= ~DT_RANGE_BOUND_MAX;
      }
      break;
    case _ACTION_EFFECT_WORSE:
      if(element != _ACTION_ELEMENT_MAX)
      {
        if(min > -1)
        {
          if(max == min) max = min - 1;
          min--;
        }
        bounds &= ~DT_RANGE_BOUND_MIN;
      }
      else
      {
        if(max > -1) max--;
        if(min > max) min = max;
        bounds &= ~DT_RANGE_BOUND_MAX;
      }
      break;
    case _ACTION_EFFECT_CAP:
      if(element != _ACTION_ELEMENT_MAX && (max != new_value || bounds & DT_RANGE_BOUND_MAX))
      {
        max = new_value;
        if(min > max) min = max;
        bounds &= ~DT_RANGE_BOUND_MAX;
      }
      else
        bounds ^= DT_RANGE_BOUND_MIN;
      break;
    }
    dtgtk_range_select_set_selection(range, bounds, min, max, TRUE, FALSE);
    gchar *txt = dtgtk_range_select_get_bounds_pretty(range);
    dt_action_widget_toast(NULL, target, txt);
    g_free(txt);
  }

  const gboolean is_active = (new_value >= min || bounds & DT_RANGE_BOUND_MIN) && (new_value <= max || bounds & DT_RANGE_BOUND_MAX);
  return - min - 2 + (is_active ? DT_VALUE_PATTERN_ACTIVE : 0);
}

const gchar *dt_action_effect_rating[]
  = { [DT_ACTION_EFFECT_TOGGLE] = N_("toggle"),
      [_ACTION_EFFECT_BETTER  ] = N_("better"),
      [_ACTION_EFFECT_WORSE   ] = N_("worse"),
      [_ACTION_EFFECT_CAP     ] = N_("cap"),
      NULL };

const dt_action_element_def_t _action_elements_ratings[]
  = { { N_("rejected"  ), dt_action_effect_rating },
      { N_("not rated" ), dt_action_effect_rating },
      { N_("one"       ), dt_action_effect_rating },
      { N_("two"       ), dt_action_effect_rating },
      { N_("three"     ), dt_action_effect_rating },
      { N_("four"      ), dt_action_effect_rating },
      { N_("five"      ), dt_action_effect_rating },
      { N_("max"       ), dt_action_effect_rating },
      { NULL } };

const dt_action_def_t dt_action_def_ratings_rule
  = { N_("rating filter"),
      _action_process_ratings,
      _action_elements_ratings };

static void _rating_range_widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                                const gchar *text, dt_lib_module_t *self, const gboolean top)
{
  _widgets_range_t *special = (_widgets_range_t *)g_malloc0(sizeof(_widgets_range_t));

  special->range_select
      = dtgtk_range_select_new(dt_collection_name_untranslated(prop), FALSE, DT_RANGE_TYPE_NUMERIC);
  GtkDarktableRangeSelect *range = DTGTK_RANGE_SELECT(special->range_select);
  gtk_widget_set_name(special->range_select, "dt-range-rating");
  // to keep a nice ratio, we don't want the widget to exceed a certain value
  GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(range->band));
  GtkStateFlags state = gtk_widget_get_state_flags(range->band);
  range->allow_resize = FALSE;
  int mh = -1;
  gtk_style_context_get(context, state, "min-height", &mh, NULL);
  if(mh > 0) range->max_width_px = 8 * mh * 0.8;
  range->step_bd = 1.0;
  dtgtk_range_select_add_icon(range, 7, -1, dtgtk_cairo_paint_reject, 0, NULL);
  dtgtk_range_select_add_icon(range, 22, 0, dtgtk_cairo_paint_unratestar, 0, NULL);

  dtgtk_range_select_add_icon(range, 36, 1, _rating_paint_icon, 0, NULL);
  dtgtk_range_select_add_icon(range, 50, 2, _rating_paint_icon, 0, NULL);
  dtgtk_range_select_add_icon(range, 64, 3, _rating_paint_icon, 0, NULL);
  dtgtk_range_select_add_icon(range, 78, 4, _rating_paint_icon, 0, NULL);
  dtgtk_range_select_add_icon(range, 93, 5, _rating_paint_icon, 0, NULL);
  range->print = _rating_print_func;
  range->current_bounds = _rating_get_bounds_pretty;

  dtgtk_range_select_set_selection_from_raw_text(range, text, FALSE);

  range->min_r = -1;
  range->max_r = 5.999;

  _range_widget_add_to_rule(rule, special, top);

  dt_action_define(DT_ACTION(self), N_("rules"), dt_collection_name_untranslated(prop),
                   special->range_select, &dt_action_def_ratings_rule);

  // avoid the action tooltips to interfere with the current value popup
  gtk_widget_set_has_tooltip(special->range_select, FALSE);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
