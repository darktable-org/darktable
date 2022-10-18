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

typedef struct _widgets_rating_legacy_t
{
  dt_lib_filters_rule_t *rule;

  GtkWidget *overlay;
  GtkWidget *comparator;
  GtkWidget *stars;
} _widgets_rating_legacy_t;

static void _rating_legacy_decode(const gchar *txt, int *comp, int *stars)
{
  if(!txt) return;

  // we handle the "text" forms first
  if(strlen(txt) == 0)
  {
    // all images
    *comp = 3;
    *stars = 0;
    return;
  }
  if(!g_strcmp0(txt, "=0"))
  {
    // unstarred only
    *comp = 3;
    *stars = 1;
    return;
  }
  if(!g_strcmp0(txt, "=-1"))
  {
    // rejected only
    *comp = 3;
    *stars = 7;
    return;
  }
  if(!g_strcmp0(txt, ">=0"))
  {
    // all except rejected
    *comp = 3;
    *stars = 8;
    return;
  }

  // we read the comparator first
  int comp_length = 2;
  if(g_str_has_prefix(txt, "<="))
    *comp = 1;
  else if(g_str_has_prefix(txt, ">="))
    *comp = 3;
  else if(g_str_has_prefix(txt, "<>"))
    *comp = 5;
  else
  {
    comp_length = 1;
    if(g_str_has_prefix(txt, "<"))
      *comp = 0;
    else if(g_str_has_prefix(txt, ">"))
      *comp = 4;
    else if(g_str_has_prefix(txt, "="))
      *comp = 2;
    else
    {
      // no comparator is considers as =
      *comp = 2;
      comp_length = 0;
    }
  }


  // and now we read the stars values
  if(strlen(txt) <= comp_length)
  {
    *stars = 0;
    return;
  }

  const int val = atoi(txt + comp_length);
  if(val >= 1 && val <= 5)
  {
    *stars = val + 1;
  }
}

static void _rating_legacy_changed(GtkWidget *widget, gpointer user_data)
{
  _widgets_rating_legacy_t *rating_legacy = (_widgets_rating_legacy_t *)user_data;
  if(rating_legacy->rule->manual_widget_set) return;

  const int comp = dt_bauhaus_combobox_get(rating_legacy->comparator);
  const int stars = dt_bauhaus_combobox_get(rating_legacy->stars);
  if(stars == 0)
    _rule_set_raw_text(rating_legacy->rule, "", TRUE);
  else if(stars == 1)
    _rule_set_raw_text(rating_legacy->rule, "=0", TRUE);
  else if(stars == 7)
    _rule_set_raw_text(rating_legacy->rule, "=-1", TRUE);
  else if(stars == 8)
    _rule_set_raw_text(rating_legacy->rule, ">=0", TRUE);
  else
  {
    gchar *txt;
    switch(comp)
    {
      case 0:
        txt = g_strdup_printf("<%d", stars - 1);
        break;
      case 1:
        txt = g_strdup_printf("<=%d", stars - 1);
        break;
      case 2:
        txt = g_strdup_printf("=%d", stars - 1);
        break;
      case 4:
        txt = g_strdup_printf(">%d", stars - 1);
        break;
      case 5:
        txt = g_strdup_printf("<>%d", stars - 1);
        break;
      default:
        txt = g_strdup_printf(">=%d", stars - 1);
        break;
    }
    _rule_set_raw_text(rating_legacy->rule, txt, TRUE);
    g_free(txt);
  }

  gtk_widget_set_visible(rating_legacy->comparator, stars > 1 && stars < 7);
}

static gboolean _rating_update(dt_lib_filters_rule_t *rule, gchar *last_where_ext)
{
  if(!rule->w_specific) return FALSE;
  int comp = 3, stars = 0;
  _rating_legacy_decode(rule->raw_text, &comp, &stars);

  rule->manual_widget_set++;
  _widgets_rating_legacy_t *rating_legacy = (_widgets_rating_legacy_t *)rule->w_specific;
  dt_bauhaus_combobox_set(rating_legacy->comparator, comp);
  dt_bauhaus_combobox_set(rating_legacy->stars, stars);
  gtk_widget_set_visible(rating_legacy->comparator, stars > 1 && stars < 7);
  rule->manual_widget_set--;

  return TRUE;
}

static void _rating_widget_init(dt_lib_filters_rule_t *rule, const dt_collection_properties_t prop,
                                const gchar *text, dt_lib_module_t *self, const gboolean top)
{
  _widgets_rating_legacy_t *rating_legacy
      = (_widgets_rating_legacy_t *)g_malloc0(sizeof(_widgets_rating_legacy_t));
  rating_legacy->rule = rule;

  rating_legacy->overlay = gtk_overlay_new();

  DT_BAUHAUS_COMBOBOX_NEW_FULL(rating_legacy->comparator, self, NULL, N_("comparator"),
                               _("filter by images rating"), 3, _rating_legacy_changed, rating_legacy, "<", "≤",
                               "=", "≥", ">", "≠");
  DT_BAUHAUS_WIDGET(rating_legacy->comparator)->show_label = FALSE;
  gtk_widget_set_halign(rating_legacy->comparator, GTK_ALIGN_START);
  gtk_widget_set_no_show_all(rating_legacy->comparator, TRUE);
  dt_gui_add_class(rating_legacy->comparator, "dt_transparent_background");
  gtk_overlay_add_overlay(GTK_OVERLAY(rating_legacy->overlay), rating_legacy->comparator);
  gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(rating_legacy->overlay), rating_legacy->comparator, TRUE);

  /* create the filter combobox */
  DT_BAUHAUS_COMBOBOX_NEW_FULL(rating_legacy->stars, self, NULL, N_("ratings"), _("filter by images rating"), 0,
                               _rating_legacy_changed, rating_legacy, N_("all"), N_("unstarred only"), "★", "★ ★",
                               "★ ★ ★", "★ ★ ★ ★", "★ ★ ★ ★ ★", N_("rejected only"), N_("all except rejected"));
  DT_BAUHAUS_WIDGET(rating_legacy->stars)->show_label = FALSE;
  // we increase the left padding of the 5 star entry to be sure it's visible with the comparator on top
  // we do that here to not cause trouble with shortcuts
  dt_bauhaus_combobox_set_entry_label(rating_legacy->stars, 6, "           ★ ★ ★ ★ ★");
  gtk_container_add(GTK_CONTAINER(rating_legacy->overlay), rating_legacy->stars);

  gtk_box_pack_start(GTK_BOX(rule->w_special_box), rating_legacy->overlay, TRUE, TRUE, 0);
  gtk_widget_set_halign(rating_legacy->overlay, GTK_ALIGN_CENTER);


  if(top)
  {
    dt_gui_add_class(rating_legacy->overlay, "dt_quick_filter");
  }

  rule->w_specific = rating_legacy;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
