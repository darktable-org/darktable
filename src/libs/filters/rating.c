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

static gboolean _rating_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;
  _widgets_range_t *special = (_widgets_range_t *)rule->w_specific;
  _widgets_range_t *specialtop = (_widgets_range_t *)rule->w_specific_top;

  rule->manual_widget_set++;
  dtgtk_range_select_set_selection_from_raw_text(DTGTK_RANGE_SELECT(special->range_select), rule->raw_text, FALSE);
  if(specialtop)
    dtgtk_range_select_set_selection_from_raw_text(DTGTK_RANGE_SELECT(specialtop->range_select), rule->raw_text,
                                                   FALSE);
  rule->manual_widget_set--;
  return TRUE;
}

static gchar *_rating_print_func(const double value, const gboolean detailled)
{
  if(detailled)
  {
    switch((int)floor(value))
    {
      case -1:
        return g_strdup(_("rejected"));
      case 0:
        return g_strdup(_("not rated"));
      case 1:
        return g_strdup("★");
      case 2:
        return g_strdup("★ ★");
      case 3:
        return g_strdup("★ ★ ★");
      case 4:
        return g_strdup("★ ★ ★ ★");
      case 5:
        return g_strdup("★ ★ ★ ★ ★");
    }
  }
  return g_strdup_printf("%.0lf", floor(value));
}

static void _rating_paint_icon(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  // first, we set the color depending on the flags
  void *my_data = data;

  if((flags & CPF_PRELIGHT) || (flags & CPF_ACTIVE))
  {
    // we want a filled icon
    GdkRGBA bc = darktable.gui->colors[DT_GUI_COLOR_RANGE_ICONS];
    GdkRGBA *shade_color = gdk_rgba_copy(&bc);
    shade_color->alpha *= 0.6;
    my_data = shade_color;
  }

  // then we draw the regular icon
  dtgtk_cairo_paint_star(cr, x, y, w, h, flags, my_data);
}

static void _rating_widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                                const gchar *text, dt_lib_module_t *self, const gboolean top)
{
  _widgets_range_t *special = (_widgets_range_t *)g_malloc0(sizeof(_widgets_range_t));

  special->range_select
      = dtgtk_range_select_new(dt_collection_name_untranslated(prop), FALSE, DT_RANGE_TYPE_NUMERIC);
  GtkDarktableRangeSelect *range = DTGTK_RANGE_SELECT(special->range_select);
  // for the topbar widget, we need to set the width to 8 * fontheight (7 icons + little space between them)
  if(top) gtk_widget_set_size_request(GTK_WIDGET(range), darktable.bauhaus->line_height * 8, -1);
  range->step_bd = 1.0;
  dtgtk_range_select_add_icon(range, 7, -1, dtgtk_cairo_paint_reject, 0, NULL);
  dtgtk_range_select_add_icon(range, 22, 0, dtgtk_cairo_paint_unratestar, 0, NULL);
  dtgtk_range_select_add_icon(range, 36, 1, _rating_paint_icon, 0, NULL);
  dtgtk_range_select_add_icon(range, 50, 2, _rating_paint_icon, 0, NULL);
  dtgtk_range_select_add_icon(range, 64, 3, _rating_paint_icon, 0, NULL);
  dtgtk_range_select_add_icon(range, 78, 4, _rating_paint_icon, 0, NULL);
  dtgtk_range_select_add_icon(range, 93, 5, _rating_paint_icon, 0, NULL);
  range->print = _rating_print_func;

  dtgtk_range_select_set_selection_from_raw_text(range, text, FALSE);

  char query[1024] = { 0 };
  g_snprintf(query, sizeof(query),
             "SELECT CASE WHEN (flags & 8) == 8 THEN -1 ELSE (flags & 7) END AS rating,"
             " COUNT(*) AS count"
             " FROM main.images AS mi"
             " GROUP BY rating"
             " ORDER BY rating");
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

  dtgtk_range_select_add_range_block(range, 1.0, 1.0, DT_RANGE_BOUND_MIN | DT_RANGE_BOUND_MAX, _("all images"),
                                     nb[0] + nb[1] + nb[2] + nb[3] + nb[4] + nb[5] + nb[6]);
  dtgtk_range_select_add_range_block(range, 0.0, 1.0, DT_RANGE_BOUND_MAX, _("all except rejected"),
                                     nb[1] + nb[2] + nb[3] + nb[4] + nb[5] + nb[6]);
  dtgtk_range_select_add_range_block(range, -1.0, -1.0, DT_RANGE_BOUND_FIXED, _("rejected only"), nb[0]);
  dtgtk_range_select_add_range_block(range, 0.0, 0.0, DT_RANGE_BOUND_FIXED, _("unstared only"), nb[1]);
  dtgtk_range_select_add_range_block(range, 1.0, 5.0, DT_RANGE_BOUND_MAX, "★", nb[2]);
  dtgtk_range_select_add_range_block(range, 2.0, 5.0, DT_RANGE_BOUND_MAX, "★ ★", nb[3]);
  dtgtk_range_select_add_range_block(range, 3.0, 5.0, DT_RANGE_BOUND_MAX, "★ ★ ★", nb[4]);
  dtgtk_range_select_add_range_block(range, 4.0, 5.0, DT_RANGE_BOUND_MAX, "★ ★ ★ ★", nb[5]);
  dtgtk_range_select_add_range_block(range, 5.0, 5.0, DT_RANGE_BOUND_MAX, "★ ★ ★ ★ ★", nb[6]);

  range->min_r = -1;
  range->max_r = 5.999;

  _range_widget_add_to_rule(rule, special, top);
}