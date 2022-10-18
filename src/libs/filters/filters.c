/*
    This file is part of darktable,
    Copyright (C) 2010-2022 darktable developers.

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

#include "filters.h"
#include "bauhaus/bauhaus.h"
#include "common/debug.h"
#include "common/image.h"
#include "dtgtk/button.h"
#include "dtgtk/range.h"
#include "gui/accelerators.h"

typedef void (*_widget_init_func)(dt_lib_filters_rule_t *rule, const dt_collection_properties_t prop,
                                  const gchar *text, dt_lib_module_t *self, gboolean top);

typedef gboolean (*_widget_update_func)(dt_lib_filters_rule_t *rule, gchar *last_where_ext);
typedef struct _filter_t
{
  dt_collection_properties_t prop;
  _widget_init_func widget_init;
  _widget_update_func update;
} _filter_t;

typedef struct _widgets_range_t
{
  dt_lib_filters_rule_t *rule;

  GtkWidget *range_select;
} _widgets_range_t;

typedef enum _tree_cols_t
{
  TREE_COL_TEXT = 0,
  TREE_COL_TOOLTIP,
  TREE_COL_PATH,
  TREE_COL_COUNT,
  TREE_NUM_COLS
} _tree_cols_t;

static void _rule_set_raw_text(dt_lib_filters_rule_t *rule, const gchar *text, const gboolean signal);
static void _range_widget_add_to_rule(dt_lib_filters_rule_t *rule, _widgets_range_t *special, const gboolean top);

// filters definitions
#include "libs/filters/aperture.c"
#include "libs/filters/colors.c"
#include "libs/filters/date.c"
#include "libs/filters/exposure.c"
#include "libs/filters/filename.c"
#include "libs/filters/focal.c"
#include "libs/filters/grouping.c"
#include "libs/filters/history.c"
#include "libs/filters/iso.c"
#include "libs/filters/local_copy.c"
#include "libs/filters/module_order.c"
#include "libs/filters/rating.c"
#include "libs/filters/rating_range.c"
#include "libs/filters/ratio.c"
#include "libs/filters/search.c"

static _filter_t filters[]
    = { { DT_COLLECTION_PROP_COLORLABEL, _colors_widget_init, _colors_update },
        { DT_COLLECTION_PROP_FILENAME, _filename_widget_init, _filename_update },
        { DT_COLLECTION_PROP_TEXTSEARCH, _search_widget_init, _search_update },
        { DT_COLLECTION_PROP_DAY, _date_widget_init, _date_update },
        { DT_COLLECTION_PROP_CHANGE_TIMESTAMP, _date_widget_init, _date_update },
        { DT_COLLECTION_PROP_EXPORT_TIMESTAMP, _date_widget_init, _date_update },
        { DT_COLLECTION_PROP_IMPORT_TIMESTAMP, _date_widget_init, _date_update },
        { DT_COLLECTION_PROP_PRINT_TIMESTAMP, _date_widget_init, _date_update },
        { DT_COLLECTION_PROP_ASPECT_RATIO, _ratio_widget_init, _ratio_update },
        { DT_COLLECTION_PROP_RATING_RANGE, _rating_range_widget_init, _rating_range_update },
        { DT_COLLECTION_PROP_APERTURE, _aperture_widget_init, _aperture_update },
        { DT_COLLECTION_PROP_FOCAL_LENGTH, _focal_widget_init, _focal_update },
        { DT_COLLECTION_PROP_ISO, _iso_widget_init, _iso_update },
        { DT_COLLECTION_PROP_EXPOSURE, _exposure_widget_init, _exposure_update },
        { DT_COLLECTION_PROP_GROUPING, _grouping_widget_init, _grouping_update },
        { DT_COLLECTION_PROP_LOCAL_COPY, _local_copy_widget_init, _local_copy_update },
        { DT_COLLECTION_PROP_HISTORY, _history_widget_init, _history_update },
        { DT_COLLECTION_PROP_ORDER, _module_order_widget_init, _module_order_update },
        { DT_COLLECTION_PROP_RATING, _rating_widget_init, _rating_update } };

static _filter_t *_filters_get(const dt_collection_properties_t prop)
{
  const int nb = sizeof(filters) / sizeof(_filter_t);
  for(int i = 0; i < nb; i++)
  {
    if(filters[i].prop == prop) return &filters[i];
  }
  return NULL;
}

static void _rule_set_raw_text(dt_lib_filters_rule_t *rule, const gchar *text, const gboolean signal)
{
  snprintf(rule->raw_text, sizeof(rule->raw_text), "%s", (text == NULL) ? "" : text);
  if(signal && !rule->manual_widget_set) rule->rule_changed(rule);
}

static void _range_set_tooltip(_widgets_range_t *special)
{
  // we recreate the tooltip
  gchar *val = dtgtk_range_select_get_bounds_pretty(DTGTK_RANGE_SELECT(special->range_select));
  gchar *txt = g_strdup_printf("<b>%s</b>\n%s\n%s", dt_collection_name(special->rule->prop),
                               _("click or click&#38;drag to select one or multiple values"),
                               _("right-click opens a menu to select the available values"));

  if(special->rule->prop != DT_COLLECTION_PROP_RATING_RANGE)
    txt = g_strdup_printf("%s\n<b><i>%s:</i></b> %s", txt, _("actual selection"), val);
  gtk_widget_set_tooltip_markup(special->range_select, txt);
  g_free(txt);
  g_free(val);
}

static void _range_changed(GtkWidget *widget, gpointer user_data)
{
  _widgets_range_t *special = (_widgets_range_t *)user_data;
  if(special->rule->manual_widget_set) return;
  // TODO if(special->rule->lib->leaving) return;

  // we recreate the right raw text and put it in the raw entry
  gchar *txt = dtgtk_range_select_get_raw_text(DTGTK_RANGE_SELECT(special->range_select));
  _rule_set_raw_text(special->rule, txt, TRUE);
  g_free(txt);

  _range_set_tooltip(special);
}

static void _range_widget_add_to_rule(dt_lib_filters_rule_t *rule, _widgets_range_t *special, const gboolean top)
{
  special->rule = rule;

  _range_set_tooltip(special);

  gtk_box_pack_start(GTK_BOX(rule->w_special_box), special->range_select, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(special->range_select), "value-changed", G_CALLBACK(_range_changed), special);
  if(top)
  {
    dt_gui_add_class(gtk_bin_get_child(GTK_BIN(special->range_select)), "dt_quick_filter");
  }

  rule->w_specific = special;
}

gboolean dt_filters_exists(const dt_collection_properties_t prop)
{
  return (_filters_get(prop) != NULL);
}

gboolean dt_filters_update(dt_lib_filters_rule_t *rule, gchar *last_where_ext)
{
  _filter_t *f = _filters_get(rule->prop);
  if(f)
    return f->update(rule, last_where_ext);
  else
    return FALSE;
}

void dt_filters_init(dt_lib_filters_rule_t *rule, const dt_collection_properties_t prop, const gchar *text,
                     dt_lib_module_t *self, gboolean top)
{
  _filter_t *f = _filters_get(prop);
  if(f)
  {
    rule->prop = prop;
    f->widget_init(rule, prop, text, self, top);
  }
}

void dt_filters_reset(dt_lib_filters_rule_t *rule, const gboolean signal)
{
  _rule_set_raw_text(rule, "", signal);
}

void dt_filters_free(dt_lib_filters_rule_t *rule)
{
  if(rule->w_special_box) gtk_widget_destroy(rule->w_special_box);
  rule->w_special_box = NULL;
  g_free(rule->w_specific);
  rule->w_specific = NULL;
  g_free(rule);
}

int dt_filters_get_count()
{
  return sizeof(filters) / sizeof(_filter_t);
}

dt_collection_properties_t dt_filters_get_prop_by_pos(const int pos)
{
  return filters[pos].prop;
}