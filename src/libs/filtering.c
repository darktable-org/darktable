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

#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/film.h"
#include "common/history.h"
#include "common/map_locations.h"
#include "common/metadata.h"
#include "common/utility.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "dtgtk/button.h"
#include "dtgtk/range.h"
#include "gui/gtk.h"
#include "gui/preferences_dialogs.h"
#include "libs/collect.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "views/view.h"
#ifndef _WIN32
#include <gio/gunixmounts.h>
#endif
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <locale.h>

DT_MODULE(1)

#define MAX_RULES 10

#define PARAM_STRING_SIZE 256 // FIXME: is this enough !?

typedef struct _widgets_sort_t
{
  GtkWidget *box;
  GtkWidget *sort;
  GtkWidget *direction;
} _widgets_sort_t;

typedef struct _widgets_rating_t
{
  GtkWidget *range_select;
} _widgets_rating_t;

typedef struct _widgets_aspect_ratio_t
{
  GtkWidget *range_select;
} _widgets_aspect_ratio_t;

typedef struct _widgets_folders_t
{
  GtkWidget *folder;
  GtkWidget *subfolders;
  GtkWidget *explore;
} _widgets_folders_t;

typedef struct _widgets_fallback_t
{
  GtkWidget *entry;
} _widgets_fallback_t;

typedef struct dt_lib_filtering_rule_t
{
  int num;

  dt_collection_properties_t prop;

  GtkWidget *w_main;
  GtkWidget *w_operator;
  GtkWidget *w_prop;
  GtkWidget *w_close;
  GtkWidget *w_off;

  GtkWidget *w_widget_box;
  char raw_text[PARAM_STRING_SIZE];
  GtkWidget *w_special_box;
  void *w_specific;      // structure which contains all the widgets specific to the rule type
  int manual_widget_set; // when we update manually the widget, we don't want events to be handled

  gchar *searchstring;
} dt_lib_filtering_rule_t;

typedef struct dt_lib_filtering_t
{
  dt_lib_filtering_rule_t rule[MAX_RULES];
  int nb_rules;

  GtkWidget *rules_box;
  GtkWidget *rules_sw;
  _widgets_sort_t *sort;
  gboolean manual_sort_set;

  gboolean singleclick;
  struct dt_lib_filtering_params_t *params;

} dt_lib_filtering_t;

typedef struct dt_lib_filtering_params_rule_t
{
  uint32_t item : 16;
  uint32_t mode : 16;
  uint32_t off : 16;
  char string[PARAM_STRING_SIZE];
} dt_lib_filtering_params_rule_t;

typedef struct dt_lib_filtering_params_t
{
  uint32_t rules;
  dt_lib_filtering_params_rule_t rule[MAX_RULES];
} dt_lib_filtering_params_t;

static void _filters_gui_update(dt_lib_module_t *self);
static void collection_updated(gpointer instance, dt_collection_change_t query_change,
                               dt_collection_properties_t changed_property, gpointer imgs, int next,
                               gpointer self);

int last_state = 0;

const char *name(dt_lib_module_t *self)
{
  return _("filters for collections");
}

void init_presets(dt_lib_module_t *self)
{
  dt_lib_filtering_params_t params;

#define CLEAR_PARAMS(r)                                                                                           \
  {                                                                                                               \
    memset(&params, 0, sizeof(params));                                                                           \
    params.rules = 1;                                                                                             \
    params.rule[0].mode = 0;                                                                                      \
    params.rule[0].item = r;                                                                                      \
  }

  CLEAR_PARAMS(DT_COLLECTION_PROP_RATING);
  g_strlcpy(params.rule[0].string, ">=0", PARAM_STRING_SIZE);
  dt_lib_presets_add(_("rating : all except rejected"), self->plugin_name, self->version(), &params,
                     sizeof(params), TRUE);
  CLEAR_PARAMS(DT_COLLECTION_PROP_RATING);
  g_strlcpy(params.rule[0].string, ">=2", PARAM_STRING_SIZE);
  dt_lib_presets_add(_("rating : ★ ★"), self->plugin_name, self->version(), &params, sizeof(params), TRUE);

  CLEAR_PARAMS(DT_COLLECTION_PROP_COLORLABEL);
  g_strlcpy(params.rule[0].string, "red", PARAM_STRING_SIZE);
  dt_lib_presets_add(_("color labels : red"), self->plugin_name, self->version(), &params, sizeof(params), TRUE);

#undef CLEAR_PARAMS
}

/* Update the params struct with active ruleset */
static void _filters_update_params(dt_lib_filtering_t *d)
{
  /* reset params */
  dt_lib_filtering_params_t *p = d->params;
  memset(p, 0, sizeof(dt_lib_filtering_params_t));

  /* for each active rule set update params */
  const int _a = dt_conf_get_int("plugins/lighttable/filtering/num_rules") - 1;
  const int active = CLAMP(_a, 0, (MAX_RULES - 1));
  char confname[200] = { 0 };
  for(int i = 0; i <= active; i++)
  {
    /* get item */
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", i);
    p->rule[i].item = dt_conf_get_int(confname);

    /* get mode */
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", i);
    p->rule[i].mode = dt_conf_get_int(confname);

    /* get on-off */
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1d", i);
    p->rule[i].off = dt_conf_get_int(confname);

    /* get string */
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", i);
    const char *string = dt_conf_get_string_const(confname);
    if(string != NULL)
    {
      g_strlcpy(p->rule[i].string, string, PARAM_STRING_SIZE);
    }
  }

  p->rules = active + 1;
}

void *get_params(dt_lib_module_t *self, int *size)
{
  _filters_update_params(self->data);

  /* allocate a copy of params to return, freed by caller */
  *size = sizeof(dt_lib_filtering_params_t);
  void *p = malloc(*size);
  memcpy(p, ((dt_lib_filtering_t *)self->data)->params, *size);
  return p;
}

int set_params(dt_lib_module_t *self, const void *params, int size)
{
  /* update conf settings from params */
  dt_lib_filtering_params_t *p = (dt_lib_filtering_params_t *)params;
  char confname[200] = { 0 };

  gboolean reset_view_filter = FALSE;
  for(uint32_t i = 0; i < p->rules; i++)
  {
    /* set item */
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1u", i);
    dt_conf_set_int(confname, p->rule[i].item);

    /* set mode */
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1u", i);
    dt_conf_set_int(confname, p->rule[i].mode);

    /* set on-off */
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1u", i);
    dt_conf_set_int(confname, p->rule[i].off);

    /* set string */
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1u", i);
    dt_conf_set_string(confname, p->rule[i].string);

    /* if one of the rules is a rating filter, the view rating filter will be reset to all */
    if(p->rule[i].item == DT_COLLECTION_PROP_RATING)
    {
      reset_view_filter = TRUE;
    }
  }

  if(reset_view_filter)
  {
    dt_view_filter_reset(darktable.view_manager, FALSE);
  }

  /* set number of rules */
  g_strlcpy(confname, "plugins/lighttable/filtering/num_rules", sizeof(confname));
  dt_conf_set_int(confname, p->rules);

  /* update internal params */
  _filters_update_params(self->data);

  /* update ui */
  _filters_gui_update(self);

  /* update view */
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
  return 0;
}


const char **views(dt_lib_module_t *self)
{
  static const char *v[] = { "lighttable", "map", "print", NULL };
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

static dt_lib_filtering_t *get_collect(dt_lib_filtering_rule_t *r)
{
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)(((char *)r) - r->num * sizeof(dt_lib_filtering_rule_t));
  return d;
}

static void _history_save(dt_lib_filtering_t *d)
{
  // get the string of the rules
  char buf[4096] = { 0 };
  dt_collection_serialize(buf, sizeof(buf), TRUE);

  // compare to last saved history
  char confname[200] = { 0 };
  gchar *str = dt_conf_get_string("plugins/lighttable/filtering/history0");
  if(!g_strcmp0(str, buf))
  {
    g_free(str);
    return;
  }
  g_free(str);

  // remove all subseqeunt history that have the same values
  const int nbmax = dt_conf_get_int("plugins/lighttable/filtering/history_max");
  int move = 0;
  for(int i = 1; i < nbmax; i++)
  {
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/history%1d", i);
    gchar *string = dt_conf_get_string(confname);

    if(!g_strcmp0(string, buf))
    {
      move++;
      dt_conf_set_string(confname, "");
    }
    else if(move > 0)
    {
      dt_conf_set_string(confname, "");
      snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/history%1d", i - move);
      dt_conf_set_string(confname, string);
    }
    g_free(string);
  }

  // move all history entries +1 (and delete the last one)
  for(int i = nbmax - 2; i >= 0; i--)
  {
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/history%1d", i);
    gchar *string = dt_conf_get_string(confname);

    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/history%1d", i + 1);
    dt_conf_set_string(confname, string);
    g_free(string);
  }

  // save current history
  dt_conf_set_string("plugins/lighttable/filtering/history0", buf);
}

static void _conf_update_rule(dt_lib_filtering_rule_t *rule)
{
  const dt_lib_collect_mode_t mode = MAX(0, gtk_combo_box_get_active(GTK_COMBO_BOX(rule->w_operator)));
  const gboolean off = !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(rule->w_off));

  char confname[200] = { 0 };
  snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", rule->num);
  dt_conf_set_string(confname, rule->raw_text);
  snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", rule->num);
  dt_conf_set_int(confname, rule->prop);
  snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", rule->num);
  dt_conf_set_int(confname, mode);
  snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1d", rule->num);
  dt_conf_set_int(confname, off);

  _history_save(get_collect(rule));
}

static void _event_rule_changed(GtkWidget *entry, dt_lib_filtering_rule_t *rule)
{
  if(rule->manual_widget_set) return;

  // update the config files
  _conf_update_rule(rule);

  // update the query without throwing signal everywhere
  dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(collection_updated),
                                  darktable.view_manager->proxy.module_collect.module);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
  dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(collection_updated),
                                    darktable.view_manager->proxy.module_collect.module);
}

static void _rule_set_raw_text(dt_lib_filtering_rule_t *rule, const gchar *text, const gboolean signal)
{
  snprintf(rule->raw_text, sizeof(rule->raw_text), "%s", text);
  if(signal) _event_rule_changed(NULL, rule);
}

static gboolean _event_rule_close(GtkWidget *widget, GdkEventButton *event, dt_lib_filtering_rule_t *rule)
{
  if(rule->manual_widget_set) return TRUE;

  // decrease the nb of active rules
  dt_lib_filtering_t *d = get_collect(rule);
  if(d->nb_rules <= 0) return FALSE;
  d->nb_rules--;
  dt_conf_set_int("plugins/lighttable/filtering/num_rules", d->nb_rules);

  // move up all still active rules by one.
  for(int i = rule->num; i < MAX_RULES - 1; i++)
  {
    char confname[200] = { 0 };
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", i + 1);
    const int mode = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", i + 1);
    const int item = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1d", i + 1);
    const int off = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", i + 1);
    gchar *string = dt_conf_get_string(confname);
    if(string)
    {
      snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", i);
      dt_conf_set_int(confname, mode);
      snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", i);
      dt_conf_set_int(confname, item);
      snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1d", i);
      dt_conf_set_int(confname, off);
      snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", i);
      dt_conf_set_string(confname, string);
      g_free(string);
    }
  }

  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
  return TRUE;
}

static void _rating_decode(const gchar *txt, int *min, int *max, dt_range_bounds_t *bounds)
{
  gchar *n1 = NULL;
  gchar *n2 = NULL;
  *bounds = DT_RANGE_BOUND_RANGE;
  // easy case : select all
  if(!strcmp(txt, "") || !strcmp(txt, "%"))
  {
    *bounds = DT_RANGE_BOUND_MAX | DT_RANGE_BOUND_MIN;
    return;
  }
  else if(g_str_has_prefix(txt, "<="))
  {
    *bounds = DT_RANGE_BOUND_MIN;
    n1 = g_strdup(txt + 2);
    n2 = g_strdup(txt + 2);
  }
  else if(g_str_has_prefix(txt, "="))
  {
    *bounds = DT_RANGE_BOUND_FIXED;
    n1 = g_strdup(txt + 1);
    n2 = g_strdup(txt + 1);
  }
  else if(g_str_has_prefix(txt, ">="))
  {
    *bounds = DT_RANGE_BOUND_MAX;
    n1 = g_strdup(txt + 2);
    n2 = g_strdup(txt + 2);
  }
  else
  {
    GRegex *regex;
    GMatchInfo *match_info;

    // we test the range expression first
    regex = g_regex_new("^\\s*\\[\\s*([-+]?[0-9]+\\.?[0-9]*)\\s*;\\s*([-+]?[0-9]+\\.?[0-9]*)\\s*\\]\\s*$", 0, 0,
                        NULL);
    g_regex_match_full(regex, txt, -1, 0, 0, &match_info, NULL);
    int match_count = g_match_info_get_match_count(match_info);

    if(match_count == 3)
    {
      n1 = g_match_info_fetch(match_info, 1);
      n2 = g_match_info_fetch(match_info, 2);
    }
    g_match_info_free(match_info);
    g_regex_unref(regex);
  }

  // if we still don't have values, let's try simple value
  if(!n1 || !n2)
  {
    *bounds = DT_RANGE_BOUND_FIXED;
    n1 = g_strdup(txt);
    n2 = g_strdup(txt);
  }

  // now we transform the text values into double
  const int v1 = atoi(n1);
  const int v2 = atoi(n2);
  *min = MIN(v1, v2);
  *max = MAX(v1, v2);
  g_free(n1);
  g_free(n2);
}

static void _rating_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lib_filtering_rule_t *rule = (dt_lib_filtering_rule_t *)user_data;
  if(rule->manual_widget_set) return;
  if(!rule->w_specific) return;
  _widgets_aspect_ratio_t *ratio = (_widgets_aspect_ratio_t *)rule->w_specific;

  // we recreate the right raw text and put it in the raw entry
  double min, max;
  dt_range_bounds_t bounds = dtgtk_range_select_get_selection(DTGTK_RANGE_SELECT(ratio->range_select), &min, &max);
  const int mini = min;
  const int maxi = max;

  char txt[128] = { 0 };
  if((bounds & DT_RANGE_BOUND_MAX) && (bounds & DT_RANGE_BOUND_MIN))
    snprintf(txt, sizeof(txt), "%%");
  else if(bounds & DT_RANGE_BOUND_MAX)
    snprintf(txt, sizeof(txt), ">=%d", mini);
  else if(bounds & DT_RANGE_BOUND_MIN)
    snprintf(txt, sizeof(txt), "<=%d", maxi);
  else if(bounds & DT_RANGE_BOUND_FIXED)
    snprintf(txt, sizeof(txt), "=%d", mini);
  else
    snprintf(txt, sizeof(txt), "[%d;%d]", mini, maxi);

  _rule_set_raw_text(rule, txt, TRUE);
}

static gboolean _rating_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;
  int min, max;
  dt_range_bounds_t bounds;
  _rating_decode(rule->raw_text, &min, &max, &bounds);

  rule->manual_widget_set++;
  _widgets_aspect_ratio_t *ratio = (_widgets_aspect_ratio_t *)rule->w_specific;
  dtgtk_range_select_set_selection(DTGTK_RANGE_SELECT(ratio->range_select), bounds, min, max, FALSE);
  rule->manual_widget_set--;
  return TRUE;
}

static gchar *_rating_print_func(const double value, gboolean detailled)
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
  GdkRGBA bc = darktable.gui->colors[DT_GUI_COLOR_RANGE_ICONS];
  GdkRGBA *shade_color = gdk_rgba_copy(&bc);
  shade_color->alpha *= 0.6;

  if(flags & CPF_PRELIGHT)
  {
    // we want less visible borders and filled icon
    dt_gui_gtk_set_source_rgba(cr, DT_GUI_COLOR_RANGE_ICONS, 0.6);
    my_data = shade_color;
  }
  else if(flags & CPF_ACTIVE)
  {
    // we want filled icon
    my_data = shade_color;
  }
  // then we draw the regular icon
  dtgtk_cairo_paint_star(cr, x, y, w, h, flags, my_data);
}
static void _rating_widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                                const gchar *text, dt_lib_module_t *self)
{
  _widgets_rating_t *rate = (_widgets_rating_t *)g_malloc0(sizeof(_widgets_rating_t));

  int smin, smax;
  dt_range_bounds_t sbounds;
  _rating_decode(text, &smin, &smax, &sbounds);

  rate->range_select = dtgtk_range_select_new(dt_collection_name_untranslated(prop), FALSE);
  GtkDarktableRangeSelect *range = DTGTK_RANGE_SELECT(rate->range_select);
  range->step = 1.0;
  dtgtk_range_select_add_icon(range, 7, -1, dtgtk_cairo_paint_reject, 0, NULL);
  dtgtk_range_select_add_icon(range, 36, 1, _rating_paint_icon, 0, NULL);
  dtgtk_range_select_add_icon(range, 50, 2, _rating_paint_icon, 0, NULL);
  dtgtk_range_select_add_icon(range, 64, 3, _rating_paint_icon, 0, NULL);
  dtgtk_range_select_add_icon(range, 78, 4, _rating_paint_icon, 0, NULL);
  dtgtk_range_select_add_icon(range, 93, 5, _rating_paint_icon, 0, NULL);
  range->print = _rating_print_func;

  dtgtk_range_select_set_selection(range, sbounds, smin, smax, FALSE);

  /*char query[1024] = { 0 };
  g_snprintf(query, sizeof(query),
             "SELECT CASE WHEN (flags & 8) == 8 THEN -1 ELSE (flags & 7) END AS rating,"
             " COUNT(*) AS count"
             " FROM main.images AS mi"
             " GROUP BY rating"
             " ORDER BY rating");
  if(strlen(query) > 0)
  {
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const double val = sqlite3_column_double(stmt, 0);
      const int count = sqlite3_column_int(stmt, 1);

      dtgtk_range_select_add_block(range, val, count);
    }
    sqlite3_finalize(stmt);
    range->min = -1;
    range->max = 6;
  }*/
  range->min = -1;
  range->max = 6;
  gtk_box_pack_start(GTK_BOX(rule->w_special_box), rate->range_select, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(rate->range_select), "value-changed", G_CALLBACK(_rating_changed), rule);

  rule->w_specific = rate;
}

static void _ratio_decode(const gchar *txt, double *min, double *max, dt_range_bounds_t *bounds)
{
  gchar *n1 = NULL;
  gchar *n2 = NULL;
  *bounds = DT_RANGE_BOUND_RANGE;
  // easy case : select all
  if(!strcmp(txt, "") || !strcmp(txt, "%"))
  {
    *bounds = DT_RANGE_BOUND_MAX | DT_RANGE_BOUND_MIN;
    return;
  }
  else if(g_str_has_prefix(txt, "<="))
  {
    *bounds = DT_RANGE_BOUND_MIN;
    n1 = g_strdup(txt + 2);
    n2 = g_strdup(txt + 2);
  }
  else if(g_str_has_prefix(txt, "="))
  {
    *bounds = DT_RANGE_BOUND_FIXED;
    n1 = g_strdup(txt + 1);
    n2 = g_strdup(txt + 1);
  }
  else if(g_str_has_prefix(txt, ">="))
  {
    *bounds = DT_RANGE_BOUND_MAX;
    n1 = g_strdup(txt + 2);
    n2 = g_strdup(txt + 2);
  }
  else
  {
    GRegex *regex;
    GMatchInfo *match_info;

    // we test the range expression first
    regex = g_regex_new("^\\s*\\[\\s*([-+]?[0-9]+\\.?[0-9]*)\\s*;\\s*([-+]?[0-9]+\\.?[0-9]*)\\s*\\]\\s*$", 0, 0,
                        NULL);
    g_regex_match_full(regex, txt, -1, 0, 0, &match_info, NULL);
    int match_count = g_match_info_get_match_count(match_info);

    if(match_count == 3)
    {
      n1 = g_match_info_fetch(match_info, 1);
      n2 = g_match_info_fetch(match_info, 2);
    }
    g_match_info_free(match_info);
    g_regex_unref(regex);
  }

  // if we still don't have values, let's try simple value
  if(!n1 || !n2)
  {
    *bounds = DT_RANGE_BOUND_FIXED;
    n1 = g_strdup(txt);
    n2 = g_strdup(txt);
  }

  // now we transform the text values into double
  const double v1 = atof(n1);
  const double v2 = atof(n2);
  *min = fmin(v1, v2);
  *max = fmax(v1, v2);
  g_free(n1);
  g_free(n2);
}

static void _ratio_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lib_filtering_rule_t *rule = (dt_lib_filtering_rule_t *)user_data;
  if(rule->manual_widget_set) return;
  if(!rule->w_specific) return;
  _widgets_aspect_ratio_t *ratio = (_widgets_aspect_ratio_t *)rule->w_specific;

  // we recreate the right raw text and put it in the raw entry
  double min, max;
  dt_range_bounds_t bounds = dtgtk_range_select_get_selection(DTGTK_RANGE_SELECT(ratio->range_select), &min, &max);

  char txt[128] = { 0 };
  gchar *locale = strdup(setlocale(LC_ALL, NULL));
  setlocale(LC_NUMERIC, "C");
  if((bounds & DT_RANGE_BOUND_MAX) && (bounds & DT_RANGE_BOUND_MIN))
    snprintf(txt, sizeof(txt), "%%");
  else if(bounds & DT_RANGE_BOUND_MAX)
    snprintf(txt, sizeof(txt), ">=%lf", min);
  else if(bounds & DT_RANGE_BOUND_MIN)
    snprintf(txt, sizeof(txt), "<=%lf", max);
  else if(bounds & DT_RANGE_BOUND_FIXED)
    snprintf(txt, sizeof(txt), "=%lf", min);
  else
    snprintf(txt, sizeof(txt), "[%lf;%lf]", min, max);

  setlocale(LC_NUMERIC, locale);
  g_free(locale);

  _rule_set_raw_text(rule, txt, TRUE);
}

static gboolean _ratio_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;
  double min, max;
  dt_range_bounds_t bounds;
  _ratio_decode(rule->raw_text, &min, &max, &bounds);

  rule->manual_widget_set++;
  _widgets_aspect_ratio_t *ratio = (_widgets_aspect_ratio_t *)rule->w_specific;
  dtgtk_range_select_set_selection(DTGTK_RANGE_SELECT(ratio->range_select), bounds, min, max, FALSE);
  rule->manual_widget_set--;
  return TRUE;
}

static double _ratio_value_band_func(const double value)
{
  if(value >= 1.0) return value;
  // for value < 1 (portrait), we want the inverse of the value
  return 2.0 - 1.0 / value;
}

static double _ratio_band_value_func(const double value)
{
  if(value >= 1.0) return value;
  // for value < 1 (portrait), we want the inverse of the value
  return 1.0 / (2.0 - value);
}

static gchar *_ratio_print_func(const double value, gboolean detailled)
{
  gchar *locale = strdup(setlocale(LC_ALL, NULL));
  setlocale(LC_NUMERIC, "C");
  gchar *txt = g_strdup_printf("%.2lf", value);
  setlocale(LC_NUMERIC, locale);
  g_free(locale);

  if(detailled)
  {
    if(value < 1.0)
      return dt_util_dstrcat(txt, " %s", _("portrait"));
    else if(value > 1.0)
      return dt_util_dstrcat(txt, " %s", _("landscape"));
    else if(value == 1.0)
      return dt_util_dstrcat(txt, " %s", _("square"));
  }
  return txt;
}

static void _ratio_widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                               const gchar *text, dt_lib_module_t *self)
{
  _widgets_aspect_ratio_t *ratio = (_widgets_aspect_ratio_t *)g_malloc0(sizeof(_widgets_aspect_ratio_t));

  double smin, smax;
  dt_range_bounds_t sbounds;
  _ratio_decode(text, &smin, &smax, &sbounds);

  ratio->range_select = dtgtk_range_select_new(dt_collection_name_untranslated(prop), TRUE);
  GtkDarktableRangeSelect *range = DTGTK_RANGE_SELECT(ratio->range_select);

  dtgtk_range_select_set_selection(range, sbounds, smin, smax, FALSE);
  dtgtk_range_select_set_band_func(range, _ratio_band_value_func, _ratio_value_band_func);
  dtgtk_range_select_add_marker(range, 1.0, TRUE);
  range->print = _ratio_print_func;

  char query[1024] = { 0 };
  g_snprintf(query, sizeof(query),
             "SELECT ROUND(aspect_ratio,3), COUNT(*) AS count"
             " FROM main.images AS mi"
             " GROUP BY ROUND(aspect_ratio,3)");
  if(strlen(query) > 0)
  {
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    double min = 9999999.0;
    double max = 0.0;
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const double val = sqlite3_column_double(stmt, 0);
      const int count = sqlite3_column_int(stmt, 1);
      min = fmin(min, val);
      max = fmax(max, val);

      dtgtk_range_select_add_block(range, val, count);
    }
    sqlite3_finalize(stmt);
    range->min = min;
    range->max = max;
  }
  gtk_box_pack_start(GTK_BOX(rule->w_special_box), ratio->range_select, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(ratio->range_select), "value-changed", G_CALLBACK(_ratio_changed), rule);

  rule->w_specific = ratio;
}

static void _folders_decode(const gchar *txt, gchar *path, gchar *dir, gboolean *sub)
{
  if(!txt || strlen(txt) == 0) return;

  // do we include subfolders
  *sub = g_str_has_suffix(txt, "*");

  // set the path
  path = g_strdup(txt);
  if(*sub) path[strlen(path) - 1] = '\0';

  // split the path to find dir name
  gchar **elems = g_strsplit(path, G_DIR_SEPARATOR_S, -1);
  const unsigned int size = g_strv_length(elems);
  if(size > 0) dir = g_strdup(elems[size - 1]);
  g_strfreev(elems);
}

static void _folders_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lib_filtering_rule_t *rule = (dt_lib_filtering_rule_t *)user_data;
  if(rule->manual_widget_set) return;
  if(!rule->w_specific) return;
  _widgets_folders_t *folders = (_widgets_folders_t *)rule->w_specific;

  // we recreate the right raw text and put it in the raw entry
  gchar *value = g_strdup(gtk_widget_get_tooltip_text(folders->folder));
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(folders->subfolders))) value = dt_util_dstrcat(value, "*");

  _rule_set_raw_text(rule, value, TRUE);
  g_free(value);
}

static gboolean _folders_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;
  gchar *path = NULL;
  gchar *dir = NULL;
  gboolean sub = TRUE;
  _folders_decode(rule->raw_text, path, dir, &sub);

  // if we don't manage to decode, we don't refresh and return false
  if(!path || !dir)
  {
    if(path) g_free(path);
    if(dir) g_free(dir);
    return FALSE;
  }

  rule->manual_widget_set++;
  _widgets_folders_t *folders = (_widgets_folders_t *)rule->w_specific;
  gtk_entry_set_text(GTK_ENTRY(folders->folder), dir);
  gtk_widget_set_tooltip_text(folders->folder, path);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(folders->subfolders), sub);
  rule->manual_widget_set--;

  g_free(path);
  g_free(dir);
  return TRUE;
}

static void _folders_widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                                 const gchar *text, dt_lib_module_t *self)
{
  _widgets_folders_t *folders = (_widgets_folders_t *)g_malloc0(sizeof(_widgets_folders_t));

  gchar *path = NULL;
  gchar *dir = NULL;
  gboolean sub = TRUE;
  _folders_decode(text, path, dir, &sub);

  folders->folder = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(folders->folder), dir);
  gtk_widget_set_tooltip_text(folders->folder, path);
  gtk_box_pack_start(GTK_BOX(rule->w_special_box), folders->folder, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(folders->folder), "activate", G_CALLBACK(_folders_changed), rule);

  folders->subfolders = dtgtk_togglebutton_new(dtgtk_cairo_paint_treelist, CPF_STYLE_FLAT, NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(folders->subfolders), sub);
  gtk_box_pack_start(GTK_BOX(rule->w_special_box), folders->subfolders, FALSE, TRUE, 0);

  folders->explore = dtgtk_button_new(dtgtk_cairo_paint_directory, CPF_STYLE_FLAT, NULL);
  gtk_box_pack_start(GTK_BOX(rule->w_special_box), folders->explore, FALSE, TRUE, 0);

  if(path) g_free(path);
  if(dir) g_free(dir);
  rule->w_specific = folders;
}

static void _fallback_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lib_filtering_rule_t *rule = (dt_lib_filtering_rule_t *)user_data;
  if(rule->manual_widget_set) return;
  if(!rule->w_specific) return;
  _widgets_fallback_t *fallback = (_widgets_fallback_t *)rule->w_specific;

  _rule_set_raw_text(rule, gtk_entry_get_text(GTK_ENTRY(fallback->entry)), TRUE);
}

static gboolean _fallback_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;

  rule->manual_widget_set++;
  _widgets_fallback_t *fallback = (_widgets_fallback_t *)rule->w_specific;
  gtk_entry_set_text(GTK_ENTRY(fallback->entry), rule->raw_text);
  rule->manual_widget_set--;

  return TRUE;
}

static void _fallback_widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                                  const gchar *text, dt_lib_module_t *self)
{
  _widgets_fallback_t *fallback = (_widgets_fallback_t *)g_malloc0(sizeof(_widgets_fallback_t));

  fallback->entry = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(fallback->entry), text);
  gtk_box_pack_start(GTK_BOX(rule->w_special_box), fallback->entry, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(fallback->entry), "activate", G_CALLBACK(_fallback_changed), rule);

  rule->w_specific = fallback;
}

static gboolean _widget_update(dt_lib_filtering_rule_t *rule)
{
  switch(rule->prop)
  {
    case DT_COLLECTION_PROP_RATING:
      return _rating_update(rule);
    case DT_COLLECTION_PROP_ASPECT_RATIO:
      return _ratio_update(rule);
    case DT_COLLECTION_PROP_FOLDERS:
      return _folders_update(rule);
    default:
      return _fallback_update(rule);
  }
}

static gboolean _widget_init_special(dt_lib_filtering_rule_t *rule, const gchar *text, dt_lib_module_t *self)
{
  // if the widgets already exits, destroy them
  if(rule->w_special_box)
  {
    gtk_widget_destroy(rule->w_special_box);
    rule->w_special_box = NULL;
    g_free(rule->w_specific);
    rule->w_specific = NULL;
  }

  // recreate the box
  rule->w_special_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(rule->w_special_box, "collect-rule-special");
  gtk_box_pack_start(GTK_BOX(rule->w_widget_box), rule->w_special_box, TRUE, TRUE, 0);

  // initialize the specific entries if any
  gboolean widgets_ok = FALSE;
  switch(rule->prop)
  {
    case DT_COLLECTION_PROP_RATING:
      _rating_widget_init(rule, rule->prop, text, self);
      break;
    case DT_COLLECTION_PROP_ASPECT_RATIO:
      _ratio_widget_init(rule, rule->prop, text, self);
      break;
    case DT_COLLECTION_PROP_FOLDERS:
      _folders_widget_init(rule, rule->prop, text, self);
      break;
    default:
      _fallback_widget_init(rule, rule->prop, text, self);
      break;
  }

  widgets_ok = _widget_update(rule);

  // set the visibility for the eventual special widgets
  if(rule->w_specific)
  {
    gtk_widget_show_all(rule->w_special_box); // we ensure all the childs widgets are shown by default
    gtk_widget_set_no_show_all(rule->w_special_box, TRUE);

    // special/raw state is stored per rule property
    char confname[200] = { 0 };
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/raw_%d", rule->prop);
    const gboolean special = (widgets_ok && !dt_conf_get_bool(confname));

    gtk_widget_set_visible(rule->w_special_box, special);
  }
  else
  {
    gtk_widget_set_no_show_all(rule->w_special_box, TRUE);
    gtk_widget_set_visible(rule->w_special_box, FALSE);
  }


  return (rule->w_specific != NULL);
}

static void _event_rule_change_type(GtkWidget *widget, dt_lib_module_t *self)
{
  const int mode = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "collect_id"));
  dt_lib_filtering_rule_t *rule = (dt_lib_filtering_rule_t *)g_object_get_data(G_OBJECT(widget), "collect_data");
  if(mode == rule->prop) return;

  const dt_collection_properties_t oldprop = rule->prop;
  rule->prop = mode;
  gtk_button_set_label(GTK_BUTTON(rule->w_prop), dt_collection_name(mode));

  // increase the nb of use of this property
  char confname[200] = { 0 };
  snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/nb_use_%d", mode);
  dt_conf_set_int(confname, dt_conf_get_int(confname) + 1);

  // re-init the special widgets
  _widget_init_special(rule, "", self);

  // reset the raw entry
  _rule_set_raw_text(rule, "", FALSE);

  // update the config files
  _conf_update_rule(rule);

  // when tag was/become the first rule, we need to handle the order
  if(rule->num == 0)
  {
    gboolean order_request = FALSE;
    uint32_t order = 0;
    if(oldprop != DT_COLLECTION_PROP_TAG && rule->prop == DT_COLLECTION_PROP_TAG)
    {
      // save global order
      const uint32_t sort = dt_collection_get_sort_field(darktable.collection);
      const gboolean descending = dt_collection_get_sort_descending(darktable.collection);
      dt_conf_set_int("plugins/lighttable/filtering/order", sort | (descending ? DT_COLLECTION_ORDER_FLAG : 0));
    }
    else if(oldprop == DT_COLLECTION_PROP_TAG && rule->prop != DT_COLLECTION_PROP_TAG)
    {
      // restore global order
      order = dt_conf_get_int("plugins/lighttable/filtering/order");
      order_request = TRUE;
      dt_collection_set_tag_id((dt_collection_t *)darktable.collection, 0);
    }
    if(order_request) DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_IMAGES_ORDER_CHANGE, order);
  }

  // update the query without throwing signal everywhere
  dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(collection_updated),
                                  darktable.view_manager->proxy.module_collect.module);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
  dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(collection_updated),
                                    darktable.view_manager->proxy.module_collect.module);
}

static void _event_append_rule(GtkWidget *widget, dt_lib_module_t *self)
{
  // add new rule
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)self->data;
  const int mode = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "collect_id"));
  char confname[200] = { 0 };

  if(mode >= 0)
  {
    // add an empty rule
    if(d->nb_rules >= MAX_RULES)
    {
      dt_control_log("You can't have more than %d rules", MAX_RULES);
      return;
    }
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", d->nb_rules);
    dt_conf_set_int(confname, mode);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", d->nb_rules);
    dt_conf_set_int(confname, DT_LIB_COLLECT_MODE_AND);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1d", d->nb_rules);
    dt_conf_set_int(confname, 0);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", d->nb_rules);
    dt_conf_set_string(confname, "");
    d->nb_rules++;
    dt_conf_set_int("plugins/lighttable/filtering/num_rules", d->nb_rules);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/nb_use_%d", mode);
    dt_conf_set_int(confname, dt_conf_get_int(confname) + 1);

    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF,
                               NULL);
  }
}

static void _preset_load(GtkWidget *widget, dt_lib_module_t *self)
{
  // add new rule
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)self->data;
  const gboolean append = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "collect_data"));
  char confname[200] = { 0 };

  GtkWidget *child = gtk_bin_get_child(GTK_BIN(widget));
  const gchar *presetname = gtk_label_get_label(GTK_LABEL(child));
  sqlite3_stmt *stmt;
  gchar *query = g_strdup_printf("SELECT op_params"
                                 " FROM data.presets"
                                 " WHERE operation=?1 AND op_version=?2 AND name=?3");
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, self->plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, self->version());
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, presetname, -1, SQLITE_TRANSIENT);
  g_free(query);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    void *op_params = (void *)sqlite3_column_blob(stmt, 0);
    if(append)
    {
      // we append the presets rules to the existing ones
      dt_lib_filtering_params_t *p = (dt_lib_filtering_params_t *)op_params;
      if(d->nb_rules + p->rules > MAX_RULES)
      {
        dt_control_log("You can't have more than %d rules", MAX_RULES);
        sqlite3_finalize(stmt);
        return;
      }
      for(uint32_t i = 0; i < p->rules; i++)
      {
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", d->nb_rules);
        dt_conf_set_int(confname, p->rule[i].item);
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", d->nb_rules);
        dt_conf_set_int(confname, p->rule[i].mode);
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1d", d->nb_rules);
        dt_conf_set_int(confname, p->rule[i].off);
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", d->nb_rules);
        dt_conf_set_string(confname, p->rule[i].string);
        d->nb_rules++;
      }
      dt_conf_set_int("plugins/lighttable/filtering/num_rules", d->nb_rules);

      dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF,
                                 NULL);
    }
    else
    {
      // we replace the existing rules by the preset
      set_params(self, op_params, sqlite3_column_bytes(stmt, 0));
    }
  }
  sqlite3_finalize(stmt);
}

static void _popup_add_item(GtkMenuShell *pop, const gchar *name, const int id, const gboolean title,
                            GCallback callback, gpointer data, dt_lib_module_t *self)
{
  GtkWidget *smt = gtk_menu_item_new_with_label(name);
  if(title)
  {
    gtk_widget_set_name(smt, "collect-popup-title");
    gtk_widget_set_sensitive(smt, FALSE);
  }
  else
  {
    gtk_widget_set_name(smt, "collect-popup-item");
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(smt));
    gtk_label_set_xalign(GTK_LABEL(child), 1.0);
    g_object_set_data(G_OBJECT(smt), "collect_id", GINT_TO_POINTER(id));
    if(data) g_object_set_data(G_OBJECT(smt), "collect_data", data);
    g_signal_connect(G_OBJECT(smt), "activate", callback, self);
  }
  gtk_menu_shell_append(pop, smt);
}

static gboolean _preset_show_popup(GtkWidget *widget, gboolean append, dt_lib_module_t *self)
{
  // we show a popup with all the possible rules
  GtkMenuShell *pop = GTK_MENU_SHELL(gtk_menu_new());
  gtk_widget_set_name(GTK_WIDGET(pop), "collect-popup");
  gtk_widget_set_size_request(GTK_WIDGET(pop), 200, -1);

  if(append)
    _popup_add_item(pop, _("append preset"), 0, TRUE, NULL, NULL, self);
  else
    _popup_add_item(pop, _("load preset"), 0, TRUE, NULL, NULL, self);

  const gboolean hide_default = dt_conf_get_bool("plugins/lighttable/hide_default_presets");
  const gboolean default_first = dt_conf_get_bool("modules/default_presets_first");

  sqlite3_stmt *stmt;
  // order like the pref value
  gchar *query = g_strdup_printf("SELECT name, writeprotect, description"
                                 " FROM data.presets"
                                 " WHERE operation=?1 AND op_version=?2"
                                 " ORDER BY writeprotect %s, LOWER(name), rowid",
                                 default_first ? "DESC" : "ASC");
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, self->plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, self->version());
  g_free(query);

  // collect all presets for op from db
  int last_wp = -1;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    // default vs built-in stuff
    const gboolean writeprotect = sqlite3_column_int(stmt, 1);
    if(hide_default && writeprotect)
    {
      // skip default module if set to hide them.
      continue;
    }
    if(last_wp == -1)
    {
      last_wp = writeprotect;
    }
    else if(last_wp != writeprotect)
    {
      last_wp = writeprotect;
      _popup_add_item(pop, " ", 0, TRUE, NULL, NULL, self);
    }

    const char *name = (char *)sqlite3_column_text(stmt, 0);
    _popup_add_item(pop, name, -1, FALSE, G_CALLBACK(_preset_load), GINT_TO_POINTER(append), self);
  }
  sqlite3_finalize(stmt);

  dt_gui_menu_popup(GTK_MENU(pop), widget, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH);
  return TRUE;
}

static gboolean _rule_show_popup(GtkWidget *widget, dt_lib_filtering_rule_t *rule, dt_lib_module_t *self)
{
  if(rule && rule->manual_widget_set) return TRUE;

  GCallback callback;
  if(rule)
    callback = G_CALLBACK(_event_rule_change_type);
  else
    callback = G_CALLBACK(_event_append_rule);

#define ADD_COLLECT_ENTRY(menu, value)                                                                            \
  _popup_add_item(menu, dt_collection_name(value), value, FALSE, callback, rule, self);

  // we show a popup with all the possible rules
  GtkMenuShell *spop = GTK_MENU_SHELL(gtk_menu_new());
  gtk_widget_set_name(GTK_WIDGET(spop), "collect-popup");
  gtk_widget_set_size_request(GTK_WIDGET(spop), 200, -1);

  // the differents categories
  _popup_add_item(spop, _("files"), 0, TRUE, NULL, NULL, self);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_FILMROLL);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_FOLDERS);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_FILENAME);

  _popup_add_item(spop, _("metadata"), 0, TRUE, NULL, NULL, self);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_TAG);
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    const uint32_t keyid = dt_metadata_get_keyid_by_display_order(i);
    const gchar *name = dt_metadata_get_name(keyid);
    gchar *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);
    const gboolean hidden = dt_conf_get_int(setting) & DT_METADATA_FLAG_HIDDEN;
    g_free(setting);
    const int meta_type = dt_metadata_get_type(keyid);
    if(meta_type != DT_METADATA_TYPE_INTERNAL && !hidden)
    {
      ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_METADATA + i);
    }
  }
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_RATING);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_COLORLABEL);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_GEOTAGGING);

  _popup_add_item(spop, _("times"), 0, TRUE, NULL, NULL, self);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_DAY);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_TIME);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_IMPORT_TIMESTAMP);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_CHANGE_TIMESTAMP);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_EXPORT_TIMESTAMP);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_PRINT_TIMESTAMP);

  _popup_add_item(spop, _("capture details"), 0, TRUE, NULL, NULL, self);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_CAMERA);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_LENS);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_APERTURE);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_EXPOSURE);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_FOCAL_LENGTH);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_ISO);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_ASPECT_RATIO);

  _popup_add_item(spop, _("darktable"), 0, TRUE, NULL, NULL, self);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_GROUPING);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_LOCAL_COPY);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_HISTORY);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_MODULE);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_ORDER);

  dt_gui_menu_popup(GTK_MENU(spop), widget, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH);
  return TRUE;
}

static gboolean _event_rule_append(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  _rule_show_popup(widget, NULL, self);
  return TRUE;
}

static gboolean _event_rule_change_popup(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_filtering_rule_t *rule = (dt_lib_filtering_rule_t *)g_object_get_data(G_OBJECT(widget), "rule");
  _rule_show_popup(rule->w_prop, rule, self);
  return TRUE;
}

static gboolean _event_preset_append(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  _preset_show_popup(widget, TRUE, self);
  return TRUE;
}

static gboolean _event_preset_load(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  _preset_show_popup(widget, FALSE, self);
  return TRUE;
}

// initialise or update a rule widget. Return if the a new widget has been created
static gboolean _widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                             const gchar *text, const dt_lib_collect_mode_t mode, gboolean off, const int pos,
                             dt_lib_module_t *self)
{
  rule->manual_widget_set++;

  const gboolean newmain = (rule->w_main == NULL);
  const gboolean newprop = (prop != rule->prop);
  GtkWidget *hbox = NULL;

  rule->prop = prop;

  if(newmain)
  {
    // the main box
    rule->w_main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(rule->w_main, "collect-rule-widget");

    // the first line
    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(rule->w_main), hbox, TRUE, TRUE, 0);
    gtk_widget_set_name(hbox, "collect-header-box");

    // operator type
    rule->w_operator = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(rule->w_operator), _("and"));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(rule->w_operator), _("or"));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(rule->w_operator), _("and not"));
    gtk_widget_set_name(rule->w_operator, "collect-operator");
    gtk_widget_set_tooltip_text(rule->w_operator, _("define how this rule should interact with the previous one"));
    gtk_box_pack_start(GTK_BOX(hbox), rule->w_operator, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(rule->w_operator), "changed", G_CALLBACK(_event_rule_changed), rule);
  }

  gtk_combo_box_set_active(GTK_COMBO_BOX(rule->w_operator), mode);

  // property
  if(newmain)
  {
    rule->w_prop = gtk_button_new_with_label(dt_collection_name(prop));
    gtk_widget_set_name(rule->w_prop, "collect-property");
    gtk_widget_set_tooltip_text(rule->w_prop, _("rule property"));
    g_object_set_data(G_OBJECT(rule->w_prop), "rule", rule);
    GtkWidget *lb = gtk_bin_get_child(GTK_BIN(rule->w_prop));
    gtk_label_set_ellipsize(GTK_LABEL(lb), PANGO_ELLIPSIZE_END);
    g_signal_connect(G_OBJECT(rule->w_prop), "button-press-event", G_CALLBACK(_event_rule_change_popup), self);
    gtk_box_pack_start(GTK_BOX(hbox), rule->w_prop, TRUE, TRUE, 0);
  }
  else if(newprop)
  {
    gtk_button_set_label(GTK_BUTTON(rule->w_prop), dt_collection_name(prop));
  }

  if(newmain)
  {
    // in order to ensure the property is correctly centered, we add an invisible widget at the right
    GtkWidget *overlay = gtk_overlay_new();
    GtkWidget *false_cb = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(false_cb), _("and"));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(false_cb), _("or"));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(false_cb), _("and not"));
    gtk_widget_set_sensitive(false_cb, FALSE);
    gtk_widget_set_name(false_cb, "collect-operator");
    gtk_container_add(GTK_CONTAINER(overlay), false_cb);
    gtk_box_pack_start(GTK_BOX(hbox), overlay, FALSE, FALSE, 0);


    GtkWidget *hbox2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(hbox2, GTK_ALIGN_FILL);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), hbox2);

    // on-off button
    rule->w_off = dtgtk_togglebutton_new(dtgtk_cairo_paint_switch, CPF_STYLE_FLAT, NULL);
    gtk_widget_set_tooltip_text(rule->w_off, _("disable this collect rule"));
    g_signal_connect(G_OBJECT(rule->w_off), "toggled", G_CALLBACK(_event_rule_changed), rule);
    gtk_box_pack_end(GTK_BOX(hbox2), rule->w_off, FALSE, FALSE, 0);

    // remove button
    rule->w_close = dtgtk_button_new(dtgtk_cairo_paint_cancel, CPF_STYLE_FLAT, NULL);
    gtk_widget_set_name(GTK_WIDGET(rule->w_close), "basics-link");
    gtk_widget_set_tooltip_text(rule->w_close, _("remove this collect rule"));
    g_signal_connect(G_OBJECT(rule->w_close), "button-press-event", G_CALLBACK(_event_rule_close), rule);
    gtk_box_pack_end(GTK_BOX(hbox2), rule->w_close, FALSE, FALSE, 0);
  }

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rule->w_off), !off);

  if(newmain)
  {
    // the second line
    rule->w_widget_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(rule->w_main), rule->w_widget_box, TRUE, TRUE, 0);
    gtk_widget_set_name(rule->w_widget_box, "collect-module-hbox");
  }

  const gboolean newraw = g_strcmp0(text, rule->raw_text);

  _rule_set_raw_text(rule, text, FALSE);

  // initialize the specific entries if any
  if(newmain || newprop || newraw) _widget_init_special(rule, text, self);

  rule->manual_widget_set--;
  return newmain;
}

static void _filters_gui_update(dt_lib_module_t *self)
{
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)self->data;

  ++darktable.gui->reset;
  d->nb_rules = CLAMP(dt_conf_get_int("plugins/lighttable/filtering/num_rules"), 0, MAX_RULES);
  char confname[200] = { 0 };

  // create or update defined rules
  for(int i = 0; i < d->nb_rules; i++)
  {
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", i);
    const dt_collection_properties_t prop = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", i);
    const gchar *txt = dt_conf_get_string_const(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", i);
    const dt_lib_collect_mode_t rmode = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1d", i);
    const int off = dt_conf_get_int(confname);
    if(_widget_init(&d->rule[i], prop, txt, rmode, off, i, self))
      gtk_box_pack_start(GTK_BOX(d->rules_box), d->rule[i].w_main, FALSE, TRUE, 0);
    gtk_widget_show_all(d->rule[i].w_main);
  }

  // remove all remaining rules
  for(int i = d->nb_rules; i < MAX_RULES; i++)
  {
    d->rule[i].prop = 0;
    if(d->rule[i].w_main)
    {
      gtk_widget_destroy(d->rule[i].w_main);
      d->rule[i].w_main = NULL;
      d->rule[i].w_special_box = NULL;
    }
  }

  --darktable.gui->reset;
}

void gui_reset(dt_lib_module_t *self)
{
  dt_conf_set_int("plugins/lighttable/filtering/num_rules", 0);

  dt_collection_set_query_flags(darktable.collection, COLLECTION_QUERY_FULL);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
}

int position()
{
  return 380;
}

static void collection_updated(gpointer instance, dt_collection_change_t query_change,
                               dt_collection_properties_t changed_property, gpointer imgs, int next, gpointer self)
{
  dt_lib_module_t *dm = (dt_lib_module_t *)self;
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)dm->data;

  // update tree
  // d->view_rule = -1;
  // d->rule[d->active_rule].typing = FALSE;

  // determine if we want to refresh the tree or not
  gboolean refresh = TRUE;
  if(query_change == DT_COLLECTION_CHANGE_RELOAD && changed_property != DT_COLLECTION_PROP_UNDEF)
  {
    // if we only reload the collection, that means that we don't change the query itself
    // so we only rebuild the treeview if a used property has changed
    refresh = FALSE;
    for(int i = 0; i <= d->nb_rules; i++)
    {
      if(d->rule[i].prop == changed_property)
      {
        refresh = TRUE;
        break;
      }
    }
  }

  if(refresh) _filters_gui_update(self);
}


static void filmrolls_updated(gpointer instance, gpointer self)
{
  // TODO: We should update the count of images here
  _filters_gui_update(self);
}

static void filmrolls_imported(gpointer instance, int film_id, gpointer self)
{
  _filters_gui_update(self);
}

static void preferences_changed(gpointer instance, gpointer self)
{
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF, NULL);
}

static void filmrolls_removed(gpointer instance, gpointer self)
{
  /*dt_lib_module_t *dm = (dt_lib_module_t *)self;
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)dm->data;

  // update tree
  if (d->view_rule != DT_COLLECTION_PROP_FOLDERS)
  {
    d->view_rule = -1;
  }
  d->rule[d->active_rule].typing = FALSE;*/
  _filters_gui_update(self);
}

static void tag_changed(gpointer instance, gpointer self)
{
  dt_lib_module_t *dm = (dt_lib_module_t *)self;
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)dm->data;
  // we check if one of the rules is TAG
  gboolean needs_update = FALSE;
  for(int i = 0; i < d->nb_rules && !needs_update; i++)
  {
    needs_update = needs_update || d->rule[i].prop == DT_COLLECTION_PROP_TAG;
  }
  if(needs_update)
  {
    // we have tags as one of rules, needs reload.
    dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(collection_updated),
                                    darktable.view_manager->proxy.module_collect.module);
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_TAG, NULL);
    dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(collection_updated),
                                      darktable.view_manager->proxy.module_collect.module);
  }
}

static void _geotag_changed(gpointer instance, GList *imgs, const int locid, gpointer self)
{
  // if locid <> NULL this event doesn't concern collect module
  if(!locid)
  {
    dt_lib_module_t *dm = (dt_lib_module_t *)self;
    dt_lib_filtering_t *d = (dt_lib_filtering_t *)dm->data;
    // update tree
    gboolean needs_update = FALSE;
    for(int i = 0; i < d->nb_rules && !needs_update; i++)
    {
      needs_update = needs_update || d->rule[i].prop == DT_COLLECTION_PROP_GEOTAGGING;
    }
    if(needs_update)
    {
      _filters_gui_update(self);

      // need to reload collection since we have geotags as active collection filter
      dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(collection_updated),
                                      darktable.view_manager->proxy.module_collect.module);
      dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_GEOTAGGING,
                                 NULL);
      dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(collection_updated),
                                        darktable.view_manager->proxy.module_collect.module);
    }
  }
}

static void metadata_changed(gpointer instance, int type, gpointer self)
{
  /* TODO
  dt_lib_module_t *dm = (dt_lib_module_t *)self;
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)dm->data;
  if(type == DT_METADATA_SIGNAL_HIDDEN
     || type == DT_METADATA_SIGNAL_SHOWN)
  {
    // hidden/shown metadata have changed - update the collection list
    for(int i = 0; i < MAX_RULES; i++)
    {
      g_signal_handlers_block_matched(d->rule[i].combo, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, combo_changed, NULL);
      dt_bauhaus_combobox_clear(d->rule[i].combo);
      _populate_collect_combo(d->rule[i].combo);
      if(d->rule[i].prop != -1) // && !_combo_set_active_collection(d->rule[i].combo, property))
      {
        // this one has been hidden - remove entry
        // g_signal_handlers_block_matched(d->rule[i].text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, entry_changed, NULL);
        gtk_entry_set_text(GTK_ENTRY(d->rule[i].w_raw_text), "");
        // g_signal_handlers_unblock_matched(d->rule[i].text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, entry_changed,
        // NULL); d->rule[i].typing = FALSE;
        _conf_update_rule(&d->rule[i]);
      }
      // g_signal_handlers_unblock_matched(d->rule[i].combo, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, combo_changed, NULL);
    }
  }

  // update collection if metadata have been hidden or a metadata collection is active
  const dt_collection_properties_t prop = d->rule[d->active_rule].prop;
  if(type == DT_METADATA_SIGNAL_HIDDEN
     || (prop >= DT_COLLECTION_PROP_METADATA
         && prop < DT_COLLECTION_PROP_METADATA + DT_METADATA_NUMBER))
  {
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_METADATA,
                               NULL);
  }*/
}


static void view_set_click(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)self->data;
  d->singleclick = dt_conf_get_bool("plugins/lighttable/filtering/single-click");
}

void _menuitem_preferences(GtkMenuItem *menuitem, dt_lib_module_t *self)
{
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog
      = gtk_dialog_new_with_buttons(_("collections settings"), GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT,
                                    _("cancel"), GTK_RESPONSE_NONE, _("save"), GTK_RESPONSE_YES, NULL);
  dt_prefs_init_dialog_collect(dialog);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_widget_show_all(dialog);
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
}

void set_preferences(void *menu, dt_lib_module_t *self)
{
  GtkWidget *mi = gtk_menu_item_new_with_label(_("preferences..."));
  g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_menuitem_preferences), self);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
}

static void _history_pretty_print(const char *buf, char *out, size_t outsize)
{
  memset(out, 0, outsize);

  if(!buf || buf[0] == '\0') return;

  int num_rules = 0;
  char str[400] = { 0 };
  int mode, item, off;
  int c;
  sscanf(buf, "%d", &num_rules);
  while(buf[0] != '\0' && buf[0] != ':') buf++;
  if(buf[0] == ':') buf++;

  for(int k = 0; k < num_rules; k++)
  {
    const int n = sscanf(buf, "%d:%d:%d:%399[^$]", &mode, &item, &off, str);

    if(n == 4)
    {
      if(k > 0)
      {
        switch(mode)
        {
          case DT_LIB_COLLECT_MODE_AND:
            c = g_strlcpy(out, _(" and "), outsize);
            out += c;
            outsize -= c;
            break;
          case DT_LIB_COLLECT_MODE_OR:
            c = g_strlcpy(out, _(" or "), outsize);
            out += c;
            outsize -= c;
            break;
          default: // case DT_LIB_COLLECT_MODE_AND_NOT:
            c = g_strlcpy(out, _(" but not "), outsize);
            out += c;
            outsize -= c;
            break;
        }
      }
      int i = 0;
      while(str[i] != '\0' && str[i] != '$') i++;
      if(str[i] == '$') str[i] = '\0';

      if(off)
      {
        c = snprintf(out, outsize, "%s%s %s", item < DT_COLLECTION_PROP_LAST ? dt_collection_name(item) : "???",
                     _("(off)"), item == 0 ? dt_image_film_roll_name(str) : str);
      }
      else
      {
        c = snprintf(out, outsize, "%s %s", item < DT_COLLECTION_PROP_LAST ? dt_collection_name(item) : "???",
                     item == 0 ? dt_image_film_roll_name(str) : str);
      }
      out += c;
      outsize -= c;
    }
    while(buf[0] != '$' && buf[0] != '\0') buf++;
    if(buf[0] == '$') buf++;
  }
}

static void _event_history_apply(GtkWidget *widget, dt_lib_module_t *self)
{
  const int hid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "history"));
  if(hid < 0 || hid >= dt_conf_get_int("plugins/lighttable/filtering/history_max")) return;

  char confname[200];
  snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/history%1d", hid);
  const char *line = dt_conf_get_string_const(confname);
  if(line && line[0] != '\0') dt_collection_deserialize(line, TRUE);
}

static gboolean _event_history_show(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  // we show a popup with all the history entries
  GtkMenuShell *pop = GTK_MENU_SHELL(gtk_menu_new());
  gtk_widget_set_name(GTK_WIDGET(pop), "collect-popup");
  gtk_widget_set_size_request(GTK_WIDGET(pop), 200, -1);

  const int maxitems = dt_conf_get_int("plugins/lighttable/filtering/history_max");

  for(int i = 0; i < maxitems; i++)
  {
    char confname[200];
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/history%1d", i);
    const char *line = dt_conf_get_string_const(confname);
    if(line && line[0] != '\0')
    {
      char str[2048] = { 0 };
      _history_pretty_print(line, str, sizeof(str));
      GtkWidget *smt = gtk_menu_item_new_with_label(str);
      gtk_widget_set_name(smt, "collect-popup-item");
      gtk_widget_set_tooltip_text(smt, str);
      // GtkWidget *child = gtk_bin_get_child(GTK_BIN(smt));
      g_object_set_data(G_OBJECT(smt), "history", GINT_TO_POINTER(i));
      g_signal_connect(G_OBJECT(smt), "activate", G_CALLBACK(_event_history_apply), self);
      gtk_menu_shell_append(pop, smt);
    }
    else
      break;
  }

  dt_gui_menu_popup(GTK_MENU(pop), widget, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH);
  return TRUE;
}

/* save the images order if the first collect filter is on tag*/
static void _sort_set_tag_order(dt_lib_module_t *self)
{
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)self->data;
  if(darktable.collection->tagid)
  {
    const uint32_t sort = GPOINTER_TO_UINT(dt_bauhaus_combobox_get_data(d->sort->sort));
    const gboolean descending = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->sort->direction));
    dt_tag_set_tag_order_by_id(darktable.collection->tagid, sort, descending);
  }
}

// set the sort order to the collection and update the query
static void _sort_update_query(dt_lib_module_t *self, gboolean update_filter)
{
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)self->data;
  const dt_collection_sort_t sort = GPOINTER_TO_UINT(dt_bauhaus_combobox_get_data(d->sort->sort));
  const gboolean reverse = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->sort->direction));

  // if needed, we sync the filter bar
  if(update_filter) dt_view_filter_update_sort(darktable.view_manager, sort, reverse);

  // we update the collection
  dt_collection_set_sort(darktable.collection, sort, reverse);

  /* save the images order */
  _sort_set_tag_order(self);

  /* sometimes changes */
  dt_collection_set_query_flags(darktable.collection, COLLECTION_QUERY_FULL);

  /* updates query */
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_SORT, NULL);
}

// update the sort asc/desc arrow
static void _sort_update_arrow(GtkWidget *widget)
{
  const gboolean reverse = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  if(reverse)
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(widget), dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_DOWN,
                                 NULL);
  else
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(widget), dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_UP, NULL);
  gtk_widget_queue_draw(widget);
}

static void _sort_reverse_changed(GtkDarktableToggleButton *widget, dt_lib_module_t *self)
{
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)self->data;
  if(d->manual_sort_set) return;

  _sort_update_arrow(GTK_WIDGET(widget));
  _sort_update_query(self, TRUE);
}

static void _sort_combobox_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)self->data;
  if(d->manual_sort_set) return;

  _sort_update_query(self, TRUE);
}

// this proxy function is primary called when the sort part of the filter bar is changed
static void _proxy_set_sort(dt_lib_module_t *self, dt_collection_sort_t sort, gboolean asc)
{
  // we update the widgets
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)self->data;
  d->manual_sort_set = TRUE;
  dt_bauhaus_combobox_set(d->sort->sort, sort);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->sort->direction), asc);
  _sort_update_arrow(d->sort->direction);
  d->manual_sort_set = FALSE;

  // we update the collection
  _sort_update_query(self, FALSE);
}

static _widgets_sort_t *_sort_get_widgets(dt_lib_module_t *self)
{
  _widgets_sort_t *wsort = (_widgets_sort_t *)g_malloc0(sizeof(_widgets_sort_t));
  wsort->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(wsort->box, "collect-sort-widget");
  const dt_collection_sort_t sort = dt_collection_get_sort_field(darktable.collection);
  wsort->sort = dt_bauhaus_combobox_new_full(DT_ACTION(self), NULL, N_("sort by"),
                                             _("determine the sort order of shown images"), sort,
                                             _sort_combobox_changed, self, NULL);

#define ADD_SORT_ENTRY(value)                                                                                     \
  dt_bauhaus_combobox_add_full(wsort->sort, dt_collection_sort_name(value), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,      \
                               GUINT_TO_POINTER(value), NULL, TRUE)

  ADD_SORT_ENTRY(DT_COLLECTION_SORT_FILENAME);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_DATETIME);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_IMPORT_TIMESTAMP);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_CHANGE_TIMESTAMP);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_EXPORT_TIMESTAMP);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_PRINT_TIMESTAMP);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_RATING);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_ID);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_COLOR);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_GROUP);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_PATH);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_CUSTOM_ORDER);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_TITLE);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_DESCRIPTION);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_ASPECT_RATIO);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_SHUFFLE);

#undef ADD_SORT_ENTRY

  gtk_box_pack_start(GTK_BOX(wsort->box), wsort->sort, TRUE, TRUE, 0);

  /* reverse order checkbutton */
  wsort->direction = dtgtk_togglebutton_new(dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_UP, NULL);
  gtk_widget_set_name(GTK_WIDGET(wsort->direction), "control-button");
  if(darktable.collection->params.descending)
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(wsort->direction), dtgtk_cairo_paint_solid_arrow,
                                 CPF_DIRECTION_DOWN, NULL);
  gtk_widget_set_halign(wsort->direction, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(wsort->box), wsort->direction, FALSE, TRUE, 0);
  /* select the last value and connect callback */
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(wsort->direction),
                               dt_collection_get_sort_descending(darktable.collection));
  g_signal_connect(G_OBJECT(wsort->direction), "toggled", G_CALLBACK(_sort_reverse_changed), self);

  gtk_widget_show_all(wsort->box);

  return wsort;
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)calloc(1, sizeof(dt_lib_filtering_t));

  self->data = (void *)d;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->plugin_name));

  d->nb_rules = 0;
  d->params = (dt_lib_filtering_params_t *)g_malloc0(sizeof(dt_lib_filtering_params_t));
  view_set_click(NULL, self);

  for(int i = 0; i < MAX_RULES; i++)
  {
    d->rule[i].num = i;
  }

  // the box to insert the collect rules
  d->rules_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), d->rules_box, FALSE, TRUE, 0);

  // the botton buttons
  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(spacer, "collect-spacer");
  gtk_box_pack_start(GTK_BOX(self->widget), spacer, TRUE, TRUE, 0);
  GtkWidget *bhbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(bhbox, "collect-actions-widget");
  gtk_box_set_homogeneous(GTK_BOX(bhbox), TRUE);
  gtk_box_pack_start(GTK_BOX(self->widget), bhbox, TRUE, TRUE, 0);
  GtkWidget *btn = dt_ui_button_new(_("+ rule"), _("append new rule to collect images"), NULL);
  g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_event_rule_append), self);
  gtk_box_pack_start(GTK_BOX(bhbox), btn, TRUE, TRUE, 0);
  btn = dt_ui_button_new(_("+ preset"), _("append preset to collect images"), NULL);
  g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_event_preset_append), self);
  gtk_box_pack_start(GTK_BOX(bhbox), btn, TRUE, TRUE, 0);
  btn = dt_ui_button_new(_("load"), _("load a collect preset"), NULL);
  g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_event_preset_load), self);
  gtk_box_pack_start(GTK_BOX(bhbox), btn, TRUE, TRUE, 0);
  btn = dt_ui_button_new(_("history"), _("revert to a previous set of rules"), NULL);
  g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_event_history_show), self);
  gtk_box_pack_start(GTK_BOX(bhbox), btn, TRUE, TRUE, 0);
  gtk_widget_show_all(bhbox);

  // the sorting part
  spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(spacer, "collect-spacer2");
  bhbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), spacer, TRUE, TRUE, 0);
  d->sort = _sort_get_widgets(self);
  gtk_box_pack_start(GTK_BOX(bhbox), d->sort->box, TRUE, TRUE, 0);
  btn = dt_ui_button_new(_("+"), _("add sort order"), NULL);
  // g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_event_history_show), self);
  gtk_box_pack_start(GTK_BOX(bhbox), btn, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), bhbox, FALSE, TRUE, 0);

  /* setup proxy */
  darktable.view_manager->proxy.module_filtering.module = self;
  darktable.view_manager->proxy.module_filtering.update = _filters_gui_update;
  darktable.view_manager->proxy.module_filtering.set_sort = _proxy_set_sort;

  _filters_gui_update(self);

  if(d->rule[0].prop == DT_COLLECTION_PROP_TAG)
  {
    const char *tag = dt_conf_get_string_const("plugins/lighttable/filtering/string0");
    dt_collection_set_tag_id((dt_collection_t *)darktable.collection, dt_tag_get_tag_id_by_name(tag));
  }

  // force redraw collection images because of late update of the table memory.darktable_iop_names
  d->nb_rules = CLAMP(dt_conf_get_int("plugins/lighttable/filtering/num_rules"), 0, MAX_RULES);
  for(int i = 0; i <= d->nb_rules; i++)
  {
    if(d->rule[i].prop == DT_COLLECTION_PROP_MODULE)
    {
      dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_MODULE,
                                 NULL);
      break;
    }
  }

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED, G_CALLBACK(collection_updated),
                                  self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED, G_CALLBACK(filmrolls_updated),
                                  self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_PREFERENCES_CHANGE, G_CALLBACK(preferences_changed),
                                  self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_FILMROLLS_IMPORTED, G_CALLBACK(filmrolls_imported),
                                  self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_FILMROLLS_REMOVED, G_CALLBACK(filmrolls_removed),
                                  self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_TAG_CHANGED, G_CALLBACK(tag_changed), self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_GEOTAG_CHANGED, G_CALLBACK(_geotag_changed), self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_METADATA_CHANGED, G_CALLBACK(metadata_changed),
                                  self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_PREFERENCES_CHANGE, G_CALLBACK(view_set_click),
                                  self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)self->data;

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(collection_updated), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(filmrolls_updated), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(filmrolls_imported), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(preferences_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(filmrolls_removed), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(tag_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_geotag_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(view_set_click), self);
  darktable.view_manager->proxy.module_collect.module = NULL;
  free(d->params);

  /* TODO: Make sure we are cleaning up all allocations */

  free(self->data);
  self->data = NULL;
}

#ifdef USE_LUA
static int new_rule_cb(lua_State *L)
{
  dt_lib_filtering_params_rule_t rule;
  memset(&rule, 0, sizeof(dt_lib_filtering_params_rule_t));
  luaA_push(L, dt_lib_filtering_params_rule_t, &rule);
  return 1;
}

static int filter_cb(lua_State *L)
{
  dt_lib_module_t *self = lua_touserdata(L, lua_upvalueindex(1));

  int size;
  dt_lib_filtering_params_t *p = get_params(self, &size);
  // put it in stack so memory is not lost if a lua exception is raised

  if(lua_gettop(L) > 0)
  {
    luaL_checktype(L, 1, LUA_TTABLE);
    dt_lib_filtering_params_t *new_p = get_params(self, &size);
    new_p->rules = 0;

    do
    {
      lua_pushinteger(L, new_p->rules + 1);
      lua_gettable(L, 1);
      if(lua_isnil(L, -1)) break;
      luaA_to(L, dt_lib_filtering_params_rule_t, &new_p->rule[new_p->rules], -1);
      new_p->rules++;
    } while(new_p->rules < MAX_RULES);

    if(new_p->rules == MAX_RULES)
    {
      lua_pushinteger(L, new_p->rules + 1);
      lua_gettable(L, 1);
      if(!lua_isnil(L, -1))
      {
        luaL_error(L, "Number of rules given exceeds max allowed (%d)", MAX_RULES);
      }
    }
    set_params(self, new_p, size);
    free(new_p);
  }

  lua_newtable(L);
  for(int i = 0; i < p->rules; i++)
  {
    luaA_push(L, dt_lib_filtering_params_rule_t, &p->rule[i]);
    lua_seti(L, -2, i + 1); // lua tables are 1 based
  }
  free(p);
  return 1;
}

static int mode_member(lua_State *L)
{
  dt_lib_filtering_params_rule_t *rule = luaL_checkudata(L, 1, "dt_lib_filtering_params_rule_t");

  if(lua_gettop(L) > 2)
  {
    dt_lib_collect_mode_t value;
    luaA_to(L, dt_lib_collect_mode_t, &value, 3);
    rule->mode = value;
    return 0;
  }

  const dt_lib_collect_mode_t tmp = rule->mode; // temp buffer because of bitfield in the original struct
  luaA_push(L, dt_lib_collect_mode_t, &tmp);
  return 1;
}

static int item_member(lua_State *L)
{
  dt_lib_filtering_params_rule_t *rule = luaL_checkudata(L, 1, "dt_lib_filtering_params_rule_t");

  if(lua_gettop(L) > 2)
  {
    dt_collection_properties_t value;
    luaA_to(L, dt_collection_properties_t, &value, 3);
    rule->item = value;
    return 0;
  }

  const dt_collection_properties_t tmp = rule->item; // temp buffer because of bitfield in the original struct
  luaA_push(L, dt_collection_properties_t, &tmp);
  return 1;
}

static int data_member(lua_State *L)
{
  dt_lib_filtering_params_rule_t *rule = luaL_checkudata(L, 1, "dt_lib_filtering_params_rule_t");

  if(lua_gettop(L) > 2)
  {
    size_t tgt_size;
    const char *data = luaL_checklstring(L, 3, &tgt_size);
    if(tgt_size > PARAM_STRING_SIZE)
    {
      return luaL_error(L, "string '%s' too long (max is %d)", data, PARAM_STRING_SIZE);
    }
    g_strlcpy(rule->string, data, sizeof(rule->string));
    return 0;
  }

  lua_pushstring(L, rule->string);
  return 1;
}

void init(struct dt_lib_module_t *self)
{
  lua_State *L = darktable.lua_state.state;
  int my_type = dt_lua_module_entry_get_type(L, "lib", self->plugin_name);
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, filter_cb, 1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "filter");
  lua_pushcfunction(L, new_rule_cb);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "new_rule");

  dt_lua_init_type(L, dt_lib_filtering_params_rule_t);
  lua_pushcfunction(L, mode_member);
  dt_lua_type_register(L, dt_lib_filtering_params_rule_t, "mode");
  lua_pushcfunction(L, item_member);
  dt_lua_type_register(L, dt_lib_filtering_params_rule_t, "item");
  lua_pushcfunction(L, data_member);
  dt_lua_type_register(L, dt_lib_filtering_params_rule_t, "data");


  luaA_enum(L, dt_lib_collect_mode_t);
  luaA_enum_value(L, dt_lib_collect_mode_t, DT_LIB_COLLECT_MODE_AND);
  luaA_enum_value(L, dt_lib_collect_mode_t, DT_LIB_COLLECT_MODE_OR);
  luaA_enum_value(L, dt_lib_collect_mode_t, DT_LIB_COLLECT_MODE_AND_NOT);

  luaA_enum(L, dt_collection_properties_t);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_FILMROLL);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_FOLDERS);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_CAMERA);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_TAG);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_DAY);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_TIME);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_IMPORT_TIMESTAMP);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_CHANGE_TIMESTAMP);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_EXPORT_TIMESTAMP);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_PRINT_TIMESTAMP);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_HISTORY);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_RATING);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_COLORLABEL);

  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    if(dt_metadata_get_type(i) != DT_METADATA_TYPE_INTERNAL)
    {
      const char *name = dt_metadata_get_name(i);
      gchar *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);
      const gboolean hidden = dt_conf_get_int(setting) & DT_METADATA_FLAG_HIDDEN;
      g_free(setting);

      if(!hidden) luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_METADATA + i);
    }
  }

  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_LENS);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_FOCAL_LENGTH);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_ISO);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_APERTURE);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_ASPECT_RATIO);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_EXPOSURE);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_FILENAME);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_GEOTAGGING);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_LOCAL_COPY);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_GROUPING);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_MODULE);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_ORDER);
}
#endif
#undef MAX_RULES
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
