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


typedef struct _widgets_search_t
{
  dt_lib_filtering_rule_t *rule;

  GtkWidget *text;
  double last_key_time;
  int time_out;
} _widgets_search_t;

static void _search_synchronize(_widgets_search_t *source)
{
  _widgets_search_t *dest = NULL;
  if(source == source->rule->w_specific_top)
    dest = source->rule->w_specific;
  else
    dest = source->rule->w_specific_top;

  if(dest)
  {
    source->rule->manual_widget_set++;
    const gchar *txt = gtk_entry_get_text(GTK_ENTRY(source->text));
    gtk_entry_set_text(GTK_ENTRY(dest->text), txt);
    source->rule->manual_widget_set--;
  }
}

static gboolean _search_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;

  char txt[1024] = { 0 };
  if(g_str_has_prefix(rule->raw_text, "%") && g_str_has_suffix(rule->raw_text, "%"))
  {
    snprintf(txt, MIN(sizeof(txt), strlen(rule->raw_text) - 1), "%s", rule->raw_text + 1);
  }
  else if(g_strcmp0(rule->raw_text, ""))
  {
    snprintf(txt, sizeof(txt), "\"%s\"", rule->raw_text);
  }

  rule->manual_widget_set++;
  _widgets_search_t *search = (_widgets_search_t *)rule->w_specific;
  gtk_entry_set_text(GTK_ENTRY(search->text), txt);
  if(rule->w_specific_top)
  {
    search = (_widgets_search_t *)rule->w_specific_top;
    gtk_entry_set_text(GTK_ENTRY(search->text), txt);
  }
  _search_synchronize(search);
  rule->manual_widget_set--;

  return TRUE;
}

static void _search_set_widget_dimmed(GtkWidget *widget, const gboolean dimmed)
{
  if(dimmed)
    dt_gui_add_class(widget, "dt_dimmed");
  else
    dt_gui_remove_class(widget, "dt_dimmed");
  gtk_widget_queue_draw(GTK_WIDGET(widget));
}

static gboolean _search_changed_wait(gpointer user_data)
{
  _widgets_search_t *search = (_widgets_search_t *)user_data;
  if(search->time_out)
  {
    search->time_out--;
    double clock = dt_get_wtime();
    if(clock - search->last_key_time >= 0.4)
    {
      search->time_out = 1; // force the query execution
      search->last_key_time = clock;
    }

    if(search->time_out == 1)
    { // tell we are busy
      _search_set_widget_dimmed(search->text, TRUE);
    }
    else if(!search->time_out)
    {
      // by default adds start and end wildcard
      // ' or " removes the corresponding wildcard
      char start[2] = { 0 };
      char *text = NULL;
      const char *entry = gtk_entry_get_text(GTK_ENTRY(search->text));
      char *p = (char *)entry;
      if(strlen(entry) > 1 && !(entry[0] == '"' && entry[1] == '"'))
      {
        if(entry[0] == '"')
          p++;
        else if(entry[0])
          start[0] = '%';
        if(entry[strlen(entry) - 1] == '"')
        {
          text = g_strconcat(start, (char *)p, NULL);
          text[strlen(text) - 1] = '\0';
        }
        else if(entry[0])
          text = g_strconcat(start, (char *)p, "%", NULL);
      }

      // avoids activating twice the same query
      if(g_strcmp0(search->rule->raw_text, text))
      {
        _rule_set_raw_text(search->rule, text, TRUE);
        _search_synchronize(search);
      }

      g_free(text);
      _search_set_widget_dimmed(search->text, FALSE);
      return FALSE;
    }
  }
  return TRUE;
}

static void _search_changed(GtkWidget *widget, gpointer user_data)
{
  _widgets_search_t *search = (_widgets_search_t *)user_data;
  if(search->rule->manual_widget_set) return;

  search->last_key_time = dt_get_wtime();
  if(!search->time_out)
  {
    search->time_out = 15;
    g_timeout_add(100, _search_changed_wait, search);
  }
}

static void _search_reset_text_entry(GtkButton *button, dt_lib_filtering_rule_t *rule)
{
  _rule_set_raw_text(rule, "", TRUE);
}

static void _search_widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                                const gchar *text, dt_lib_module_t *self, const gboolean top)
{
  _widgets_search_t *search = (_widgets_search_t *)g_malloc0(sizeof(_widgets_search_t));
  search->rule = rule;

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  if(top)
    gtk_box_pack_start(GTK_BOX(rule->w_special_box_top), hbox, TRUE, TRUE, 0);
  else
    gtk_box_pack_start(GTK_BOX(rule->w_special_box), hbox, TRUE, TRUE, 0);
  search->text = gtk_search_entry_new();
  g_signal_connect(G_OBJECT(search->text), "search-changed", G_CALLBACK(_search_changed), search);
  g_signal_connect(G_OBJECT(search->text), "stop-search", G_CALLBACK(_search_reset_text_entry), rule);
  if(top)
    gtk_entry_set_width_chars(GTK_ENTRY(search->text), 14);
  else
    gtk_entry_set_width_chars(GTK_ENTRY(search->text), 0);
  gtk_widget_set_tooltip_text(search->text,
                              /* xgettext:no-c-format */
                              _("filter by text from images metadata, tags, file path and name"
                                /* xgettext:no-c-format */
                                "\n`%' is the wildcard character"
                                /* xgettext:no-c-format */
                                "\nby default start and end wildcards are auto-applied"
                                /* xgettext:no-c-format */
                                "\nstarting or ending with a double quote disables the corresponding wildcard"
                                /* xgettext:no-c-format */
                                "\nis dimmed during the search execution"));
  dt_gui_add_class(search->text, "dt_transparent_background");
  gtk_box_pack_start(GTK_BOX(hbox), search->text, TRUE, TRUE, 0);
  if(top)
  {
    dt_gui_add_class(hbox, "dt_quick_filter");
  }

  if(top)
    rule->w_specific_top = search;
  else
    rule->w_specific = search;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
