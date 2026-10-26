/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

#include "common/filter.h"

typedef struct _widgets_duplicates_t
{
  dt_lib_filtering_rule_t *rule;

  GtkWidget *combo;
} _widgets_duplicates_t;

typedef enum _duplicates_type_t
{
  _DUP_ALL = 0,
  _DUP_WITH_DUPS,
  _DUP_DUPS_ONLY
} _duplicates_type_t;

static const char *_duplicates_names[]
    = { N_("all images"), N_("images with duplicates"), N_("duplicates only"), NULL };

static void _duplicates_synchronise(_widgets_duplicates_t *source)
{
  _widgets_duplicates_t *dest = NULL;
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

static void _duplicates_changed(GtkWidget *widget, gpointer user_data)
{
  _widgets_duplicates_t *duplicates = (_widgets_duplicates_t *)user_data;
  if(duplicates->rule->manual_widget_set) return;

  const _duplicates_type_t tp = dt_bauhaus_combobox_get(duplicates->combo);
  switch(tp)
  {
    case _DUP_ALL:
      _rule_set_raw_text(duplicates->rule, "", TRUE);
      break;
    case _DUP_WITH_DUPS:
      _rule_set_raw_text(duplicates->rule, "$IMGS_WITH_DUPLICATES", TRUE);
      break;
    case _DUP_DUPS_ONLY:
      _rule_set_raw_text(duplicates->rule, "$DUPLICATES_ONLY", TRUE);
      break;
  }
  _duplicates_synchronise(duplicates);
}

static void _duplicates_decode(const gchar *txt, int *val)
{
  if(!txt || strlen(txt) == 0) return;

  if(!g_strcmp0(txt, "$IMGS_WITH_DUPLICATES"))
    *val = _DUP_WITH_DUPS;
  else if(!g_strcmp0(txt, "$DUPLICATES_ONLY"))
    *val = _DUP_DUPS_ONLY;
  else
    *val = _DUP_ALL;
}

static gboolean _duplicates_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;
  int val = _DUP_ALL;
  _duplicates_decode(rule->raw_text, &val);

  rule->manual_widget_set++;
  _widgets_duplicates_t *duplicates = (_widgets_duplicates_t *)rule->w_specific;

  int dup_counts[2] = { 0 };
  GList *counts = dt_filter_get_duplicate_counts(rule->lib->last_where_ext);
  for(GList *iter = counts; iter; iter = g_list_next(iter))
  {
    dt_filter_range_count_t *count = iter->data;
    dup_counts[(int)count->value_r] = count->count;
  }
  g_list_free_full(counts, dt_filter_range_count_free);

  // 0 = originals only
  // 1 = duplicates only
  dup_counts[0] += dup_counts[1]; // counter for originals + all duplicates

  for(int i = 0; i < 2; i++)
  {
    gchar *item = g_strdup_printf("%s (%d)", _(_duplicates_names[i + 1]), dup_counts[i]);
    dt_bauhaus_combobox_set_entry_label(duplicates->combo, i + 1, item);
    g_free(item);
  }

  dt_bauhaus_combobox_set(duplicates->combo, val);
  _duplicates_synchronise(duplicates);
  rule->manual_widget_set--;

  return TRUE;
}

static void _duplicates_widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                                    const gchar *text, dt_lib_module_t *self, const gboolean top)
{
  _widgets_duplicates_t *duplicates = g_malloc0(sizeof(_widgets_duplicates_t));
  duplicates->rule = rule;

  duplicates->combo = dt_bauhaus_combobox_new_full(
      DT_ACTION(self), N_("rules"), N_("duplicates"), _("duplicates state filter"), 0,
      (GtkCallback)_duplicates_changed, duplicates, _duplicates_names);
  dt_bauhaus_widget_hide_label(duplicates->combo);

  if(top)
    gtk_box_pack_start(GTK_BOX(rule->w_special_box_top), duplicates->combo, TRUE, TRUE, 0);
  else
    gtk_box_pack_start(GTK_BOX(rule->w_special_box), duplicates->combo, TRUE, TRUE, 0);

  if(top)
  {
    dt_gui_add_class(duplicates->combo, "dt_quick_filter");
  }

  if(top)
    rule->w_specific_top = duplicates;
  else
    rule->w_specific = duplicates;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
