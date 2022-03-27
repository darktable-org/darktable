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

#define CPF_USER_DATA_INCLUDE CPF_USER_DATA
#define CPF_USER_DATA_EXCLUDE CPF_USER_DATA << 1

typedef struct _widgets_colors_t
{
  dt_lib_filtering_rule_t *rule;

  GtkWidget *colors[6];
  GtkWidget *operator;
} _widgets_colors_t;

#define CPF_USER_DATA_INCLUDE CPF_USER_DATA
#define CPF_USER_DATA_EXCLUDE CPF_USER_DATA << 1

static void _colors_synchronise(_widgets_colors_t *source)
{
  _widgets_colors_t *dest = NULL;
  if(source == source->rule->w_specific_top)
    dest = source->rule->w_specific;
  else
    dest = source->rule->w_specific_top;

  if(dest)
  {
    source->rule->manual_widget_set++;
    for(int k = 0; k < DT_COLORLABELS_LAST; k++)
    {
      const int sel_value = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(source->colors[k]), "sel_value"));
      g_object_set_data(G_OBJECT(dest->colors[k]), "sel_value", GINT_TO_POINTER(sel_value));
      GtkDarktableButton *bt = DTGTK_BUTTON(source->colors[k]);
      dtgtk_button_set_paint(DTGTK_BUTTON(dest->colors[k]), dtgtk_cairo_paint_label_sel, bt->icon_flags, NULL);
      gtk_widget_queue_draw(dest->colors[k]);
    }

    const gboolean and_op = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(source->operator), "sel_value"));
    g_object_set_data(G_OBJECT(dest->operator), "sel_value", GINT_TO_POINTER(and_op));
    dtgtk_button_set_paint(DTGTK_BUTTON(dest->operator), and_op ? dtgtk_cairo_paint_and : dtgtk_cairo_paint_or,
                           CPF_STYLE_FLAT, NULL);
    gtk_widget_set_sensitive(dest->operator, gtk_widget_get_sensitive(source->operator));
    gtk_widget_queue_draw(dest->operator);
    source->rule->manual_widget_set--;
  }
}

static void _colors_changed(GtkWidget *widget, gpointer user_data)
{
  _widgets_colors_t *colors = (_widgets_colors_t *)user_data;
  if(colors->rule->manual_widget_set) return;

  int nb = 0;
  int mask = 0;

  for(int k = 0; k < DT_COLORLABELS_LAST; k++)
  {
    const int sel_value = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(colors->colors[k]), "sel_value"));
    if(sel_value)
    {
      nb++;
      mask = sel_value == 1 ? (mask | 1 << k) : (mask | 1 << (8 + k));
    }
  }

  const gboolean and_op = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(colors->operator), "sel_value"));
  dtgtk_button_set_paint(DTGTK_BUTTON(colors->operator), and_op ? dtgtk_cairo_paint_and : dtgtk_cairo_paint_or,
                         CPF_STYLE_FLAT, NULL);
  gtk_widget_set_sensitive(colors->operator, nb> 1);

  if(and_op || nb == 1) mask |= 0x80000000;

  gchar *txt = g_strdup_printf("%d", mask);
  _rule_set_raw_text(colors->rule, txt, TRUE);
  g_free(txt);
}

static gboolean _colors_clicked(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  _widgets_colors_t *colors = g_object_get_data(G_OBJECT(w), "colors_self");
  const int k = GPOINTER_TO_INT(user_data);

  int sel_value = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "sel_value"));
  if(sel_value)
    sel_value = 0;
  else if(dt_modifier_is(e->state, GDK_CONTROL_MASK))
    sel_value = 2;
  else if(dt_modifier_is(e->state, 0))
    sel_value = 1;
  const int mask = sel_value == 0 ? 0 : sel_value == 1 ? CPF_USER_DATA_INCLUDE : CPF_USER_DATA_EXCLUDE;

  if(k == DT_COLORLABELS_LAST)
  {
    g_object_set_data(G_OBJECT(colors->colors[DT_COLORLABELS_LAST]), "sel_value", GINT_TO_POINTER(sel_value));
    for(int i = 0; i < DT_COLORLABELS_LAST; i++)
    {
      g_object_set_data(G_OBJECT(colors->colors[i]), "sel_value", GINT_TO_POINTER(sel_value));
      dtgtk_button_set_paint(DTGTK_BUTTON(colors->colors[i]), dtgtk_cairo_paint_label_sel,
                             (i | mask | CPF_BG_TRANSPARENT), NULL);
      gtk_widget_queue_draw(colors->colors[i]);
    }
  }
  else
  {
    g_object_set_data(G_OBJECT(colors->colors[DT_COLORLABELS_LAST]), "sel_value", GINT_TO_POINTER(0));
    g_object_set_data(G_OBJECT(w), "sel_value", GINT_TO_POINTER(sel_value));
    dtgtk_button_set_paint(DTGTK_BUTTON(w), dtgtk_cairo_paint_label_sel, (k | mask | CPF_BG_TRANSPARENT), NULL);
    gtk_widget_queue_draw(w);
  }

  _colors_changed(w, colors);
  _colors_synchronise(colors);
  return FALSE;
}

static void _colors_operator_clicked(GtkWidget *w, _widgets_colors_t *colors)
{
  const gboolean and_op = !GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "sel_value"));
  g_object_set_data(G_OBJECT(w), "sel_value", GINT_TO_POINTER(and_op));
  dtgtk_button_set_paint(DTGTK_BUTTON(w), and_op ? dtgtk_cairo_paint_and : dtgtk_cairo_paint_or, CPF_STYLE_FLAT,
                         NULL);
  _colors_changed(w, colors);
  _colors_synchronise(colors);
}

static void _colors_reset(_widgets_colors_t *colors)
{
  for(int i = 0; i < DT_COLORLABELS_LAST; i++)
  {
    g_object_set_data(G_OBJECT(colors->colors[i]), "sel_value", GINT_TO_POINTER(0));
    dtgtk_button_set_paint(DTGTK_BUTTON(colors->colors[i]), dtgtk_cairo_paint_label_sel, (i | CPF_BG_TRANSPARENT),
                           NULL);
    gtk_widget_queue_draw(colors->colors[i]);
  }
  g_object_set_data(G_OBJECT(colors->operator), "sel_value", GINT_TO_POINTER(1));
  dtgtk_button_set_paint(DTGTK_BUTTON(colors->operator), dtgtk_cairo_paint_and, CPF_STYLE_FLAT, NULL);
  gtk_widget_set_sensitive(colors->operator, FALSE);
}

static gboolean _colors_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;

  rule->manual_widget_set++;
  _widgets_colors_t *colors = (_widgets_colors_t *)rule->w_specific;
  _widgets_colors_t *colorstop = (_widgets_colors_t *)rule->w_specific_top;
  const int val = atoi(rule->raw_text);
  const int colors_set = val & 0xFF;
  const int colors_unset = (val & 0xFF00) >> 8;
  const gboolean op = val & 0x80000000;
  // we update the colors icons
  int nb = 0;
  for(int i = 0; i < DT_COLORLABELS_LAST + 1; i++)
  {
    const int id = 1 << i;
    int mask = 0;
    int sel_value = 0;
    if(colors_set & id)
    {
      mask = CPF_USER_DATA_INCLUDE;
      sel_value = 1;
      nb++;
    }
    else if(colors_unset & id)
    {
      mask = CPF_USER_DATA_EXCLUDE;
      sel_value = 2;
      nb++;
    }

    g_object_set_data(G_OBJECT(colors->colors[i]), "sel_value", GINT_TO_POINTER(sel_value));
    dtgtk_button_set_paint(DTGTK_BUTTON(colors->colors[i]), dtgtk_cairo_paint_label_sel,
                           (i | mask | CPF_BG_TRANSPARENT), NULL);
    gtk_widget_queue_draw(colors->colors[i]);
    if(colorstop)
    {
      g_object_set_data(G_OBJECT(colorstop->colors[i]), "sel_value", GINT_TO_POINTER(sel_value));
      dtgtk_button_set_paint(DTGTK_BUTTON(colorstop->colors[i]), dtgtk_cairo_paint_label_sel,
                             (i | mask | CPF_BG_TRANSPARENT), NULL);
      gtk_widget_queue_draw(colorstop->colors[i]);
    }
  }
  // we update the operator
  g_object_set_data(G_OBJECT(colors->operator), "sel_value", GINT_TO_POINTER(op));
  dtgtk_button_set_paint(DTGTK_BUTTON(colors->operator), op ? dtgtk_cairo_paint_and : dtgtk_cairo_paint_or,
                         CPF_STYLE_FLAT, NULL);
  gtk_widget_queue_draw(colors->operator);
  gtk_widget_set_sensitive(colors->operator, nb> 1);
  if(colorstop)
  {
    g_object_set_data(G_OBJECT(colorstop->operator), "sel_value", GINT_TO_POINTER(op));
    dtgtk_button_set_paint(DTGTK_BUTTON(colorstop->operator), op ? dtgtk_cairo_paint_and : dtgtk_cairo_paint_or,
                           CPF_STYLE_FLAT, NULL);
    gtk_widget_queue_draw(colorstop->operator);
    gtk_widget_set_sensitive(colorstop->operator, nb> 1);
  }

  rule->manual_widget_set--;

  return TRUE;
}

static void _colors_widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                                const gchar *text, dt_lib_module_t *self, const gboolean top)
{
  _widgets_colors_t *colors = (_widgets_colors_t *)g_malloc0(sizeof(_widgets_colors_t));
  colors->rule = rule;
  if(top)
    rule->w_specific_top = colors;
  else
    rule->w_specific = colors;

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  if(!top) gtk_widget_set_name(hbox, "filter_colors_box");
  gtk_widget_set_halign(hbox, GTK_ALIGN_CENTER);
  for(int k = 0; k < DT_COLORLABELS_LAST + 1; k++)
  {
    colors->colors[k] = dtgtk_button_new(dtgtk_cairo_paint_label_sel, (k | CPF_BG_TRANSPARENT), NULL);
    g_object_set_data(G_OBJECT(colors->colors[k]), "colors_self", colors);
    gtk_box_pack_start(GTK_BOX(hbox), colors->colors[k], FALSE, FALSE, 0);
    gtk_widget_set_tooltip_text(colors->colors[k], _("filter by images color label"
                                                     "\nclick to toggle the color label selection"
                                                     "\nctrl+click to exclude the color label"
                                                     "\nthe grey button affects all color labels"));
    g_signal_connect(G_OBJECT(colors->colors[k]), "button-press-event", G_CALLBACK(_colors_clicked),
                     GINT_TO_POINTER(k));
  }
  colors->operator= dtgtk_button_new(dtgtk_cairo_paint_and, CPF_STYLE_FLAT, NULL);
  _colors_reset(colors);
  gtk_box_pack_start(GTK_BOX(hbox), colors->operator, FALSE, FALSE, 2);
  gtk_widget_set_tooltip_text(colors->operator,
                              _("filter by images color label"
                                "\nand (∩): images having all selected color labels"
                                "\nor (∪): images with at least one of the selected color labels"));
  g_signal_connect(G_OBJECT(colors->operator), "clicked", G_CALLBACK(_colors_operator_clicked), colors);

  if(top)
  {
    GtkStyleContext *context = gtk_widget_get_style_context(hbox);
    gtk_style_context_add_class(context, "quick_filter_box");
    gtk_style_context_add_class(context, "dt_font_resize_07");
  }

  if(top)
    gtk_box_pack_start(GTK_BOX(rule->w_special_box_top), hbox, TRUE, TRUE, 0);
  else
    gtk_box_pack_start(GTK_BOX(rule->w_special_box), hbox, TRUE, TRUE, 0);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on