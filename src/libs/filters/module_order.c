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

typedef struct _widgets_module_order_t
{
  dt_lib_filtering_rule_t *rule;

  GtkWidget *combo;
} _widgets_module_order_t;

typedef enum _module_order_type_t
{
  _MORDER_ALL = 0,
  _MORDER_CUSTOM,
  _MORDER_LEGACY,
  _MORDER_V30,
  _MORDER_V30_JPG
} _module_order_type_t;

static char **_module_order_names = NULL;

static void _module_order_synchronise(_widgets_module_order_t *source)
{
  _widgets_module_order_t *dest = NULL;
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

static void _module_order_decode(const gchar *txt, int *val)
{
  if(!txt || strlen(txt) == 0) return;

  if(!g_strcmp0(txt, "$0"))
    *val = _MORDER_CUSTOM;
  else if(!g_strcmp0(txt, "$1"))
    *val = _MORDER_LEGACY;
  else if(!g_strcmp0(txt, "$2"))
    *val = _MORDER_V30;
  else if(!g_strcmp0(txt, "$3"))
    *val = _MORDER_V30_JPG;
  else
    *val = _MORDER_ALL;
}

static void _module_order_changed(GtkWidget *widget, gpointer user_data)
{
  _widgets_module_order_t *module_order = (_widgets_module_order_t *)user_data;
  if(module_order->rule->manual_widget_set) return;

  const _module_order_type_t tp = dt_bauhaus_combobox_get(module_order->combo);
  switch(tp)
  {
    case _MORDER_CUSTOM:
      _rule_set_raw_text(module_order->rule, "$0", TRUE);
      break;
    case _MORDER_LEGACY:
      _rule_set_raw_text(module_order->rule, "$1", TRUE);
      break;
    case _MORDER_V30:
      _rule_set_raw_text(module_order->rule, "$2", TRUE);
      break;
    case _MORDER_V30_JPG:
      _rule_set_raw_text(module_order->rule, "$3", TRUE);
      break;
    case _MORDER_ALL:
      _rule_set_raw_text(module_order->rule, "", TRUE);
      break;
  }
  _module_order_synchronise(module_order);
}

static gboolean _module_order_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;
  int val = _MORDER_ALL;
  _module_order_decode(rule->raw_text, &val);

  rule->manual_widget_set++;
  _widgets_module_order_t *module_order = (_widgets_module_order_t *)rule->w_specific;
  char query[1024] = { 0 };
  // clang-format off
  g_snprintf(query, sizeof(query),
                   "SELECT mo.version, COUNT(*) "
                   " FROM main.images as mi"
                   " LEFT JOIN (SELECT imgid, version FROM main.module_order) AS mo"
                   " ON mo.imgid = mi.id"
                   " WHERE %s"
                   " GROUP BY mo.version",
                   rule->lib->last_where_ext);
  // clang-format on
  int counts[DT_IOP_ORDER_LAST + 1] = { 0 };
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int count = sqlite3_column_int(stmt, 1);
    const int v = (sqlite3_column_bytes(stmt, 0) == 0) ? DT_IOP_ORDER_LAST : sqlite3_column_int(stmt, 0);
    counts[v] = count;
  }
  sqlite3_finalize(stmt);

  for(int i = 0; i < DT_IOP_ORDER_LAST + 1; i++)
  {
    gchar *item = g_strdup_printf("%s (%d)", _(_module_order_names[i + 1]), counts[i]);
    dt_bauhaus_combobox_set_entry_label(module_order->combo, i + 1, item);
    g_free(item);
  }

  dt_bauhaus_combobox_set(module_order->combo, val);
  _module_order_synchronise(module_order);
  rule->manual_widget_set--;

  return TRUE;
}

static void _module_order_widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                                      const gchar *text, dt_lib_module_t *self, const gboolean top)
{
  _widgets_module_order_t *module_order = (_widgets_module_order_t *)g_malloc0(sizeof(_widgets_module_order_t));
  module_order->rule = rule;

  // is the table with all the order names is NULL, fill it
  if(!_module_order_names)
  {
    _module_order_names = g_malloc0_n(DT_IOP_ORDER_LAST + 3, sizeof(char *));
    _module_order_names[0] = g_strdup(N_("all images"));
    for(int i = 0; i < DT_IOP_ORDER_LAST; i++) _module_order_names[i + 1] = g_strdup(N_(dt_iop_order_string(i)));

    _module_order_names[DT_IOP_ORDER_LAST + 1] = g_strdup(N_("none"));
  }
  module_order->combo = dt_bauhaus_combobox_new_full(
      DT_ACTION(self), N_("rules"), N_("module order"), _("filter images based on their module order"), 0,
      (GtkCallback)_module_order_changed, module_order, (const char **)_module_order_names);
  DT_BAUHAUS_WIDGET(module_order->combo)->show_label = FALSE;

  if(top)
    gtk_box_pack_start(GTK_BOX(rule->w_special_box_top), module_order->combo, TRUE, TRUE, 0);
  else
    gtk_box_pack_start(GTK_BOX(rule->w_special_box), module_order->combo, TRUE, TRUE, 0);

  if(top)
  {
    dt_gui_add_class(module_order->combo, "dt_quick_filter");
  }

  if(top)
    rule->w_specific_top = module_order;
  else
    rule->w_specific = module_order;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
