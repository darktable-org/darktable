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
#define CL_AND_MASK 0x80000000
#define CL_ALL_EXCLUDED 0x1F000       // all excluded but grey
#define CL_GREY_EXCLUDED 0x20000      // grey excluded
#define CL_ALL_INCLUDED 0x1F          // all included but grey
#define CL_GREY_INCLUDED 0x20         // grey included

typedef struct _widgets_colors_t
{
  dt_lib_filtering_rule_t *rule;

  GtkWidget *colors[6];
  GtkWidget *operator;
} _widgets_colors_t;

static gboolean _colors_update(dt_lib_filtering_rule_t *rule);
static int _get_mask(const char *text)
{
  if(g_str_has_prefix(text, "0x"))
    return strtoll(&text[2], NULL, 16);
  else
    return 0;
}

static void _set_mask(dt_lib_filtering_rule_t *rule , const int mask, const gboolean signal)
{
  gchar *txt = g_strdup_printf("0x%x", mask);
  _rule_set_raw_text(rule, txt, signal);
  g_free(txt);
}

static gboolean _colors_clicked(GtkWidget *w, GdkEventButton *e, _widgets_colors_t *colors)
{
  // double click reset the widget
  if(e->button == 1 && e->type == GDK_2BUTTON_PRESS)
  {
    _set_mask(colors->rule, CL_AND_MASK, TRUE);
    _colors_update(colors->rule);
    return TRUE;
  }

  dt_lib_filtering_rule_t *rule = colors->rule;
  const int mask = _get_mask(rule->raw_text);
  const int k = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "colors_index"));
  const int mask_k = (1 << k) | (1 << (k + 12));
  int new_mask = mask_k;
  if(k == DT_COLORLABELS_LAST)
  {
    if(mask & mask_k)
      new_mask = 0;
    else if(dt_modifier_is(e->state, GDK_CONTROL_MASK))
      new_mask = CL_ALL_EXCLUDED | CL_GREY_EXCLUDED;
    else if(dt_modifier_is(e->state, 0))
      new_mask = CL_ALL_INCLUDED | CL_GREY_INCLUDED;
    new_mask |= (mask & CL_AND_MASK);
  }
  else
  {
    if(mask & mask_k)
      new_mask = 0;
    else if(dt_modifier_is(e->state, GDK_CONTROL_MASK))
      new_mask = 1 << (k + 12);
    else if(dt_modifier_is(e->state, 0))
      new_mask = 1 << k;
    new_mask |= (mask & ~mask_k);
  }

  if((new_mask & CL_ALL_EXCLUDED) == CL_ALL_EXCLUDED)
    new_mask |= CL_GREY_EXCLUDED;
  else
    new_mask &= ~CL_GREY_EXCLUDED;
  if((new_mask & CL_ALL_INCLUDED) == CL_ALL_INCLUDED)
    new_mask |= CL_GREY_INCLUDED;
  else
    new_mask &= ~CL_GREY_INCLUDED;

  _set_mask(colors->rule, new_mask, TRUE);
  _colors_update(rule);
  return FALSE;
}

static void _colors_operator_clicked(GtkWidget *w, _widgets_colors_t *colors)
{
  dt_lib_filtering_rule_t *rule = colors->rule;
  const int mask = _get_mask(rule->raw_text);
  _set_mask(colors->rule, mask ^ CL_AND_MASK, TRUE);
  _colors_update(rule);
}

static gchar *_colors_pretty_print(const gchar *raw_txt)
{
  gchar *txt = NULL;
  const int val = _get_mask(raw_txt);
  const int colors_set = val & (CL_ALL_INCLUDED | CL_GREY_INCLUDED);
  const int colors_unset = (val & (CL_ALL_EXCLUDED | CL_GREY_EXCLUDED)) >> 12;
  // we update the colors icons
  int nb = 0;
  for(int i = 0; i < DT_COLORLABELS_LAST; i++)
  {
    const int id = 1 << i;
    gboolean incl = TRUE;
    if(colors_set & id)
      incl = TRUE;
    else if(colors_unset & id)
      incl = FALSE;
    else
      continue;

    nb++;
    gchar *col = NULL;
    switch(i)
    {
      case DT_COLORLABELS_RED:
        col = g_strdup(_("R"));
        break;
      case DT_COLORLABELS_YELLOW:
        col = g_strdup(_("Y"));
        break;
      case DT_COLORLABELS_GREEN:
        col = g_strdup(_("G"));
        break;
      case DT_COLORLABELS_BLUE:
        col = g_strdup(_("B"));
        break;
      default:
        col = g_strdup(_("P"));
        break;
    }
    txt = dt_util_dstrcat(txt, "%s%s%s%s", (i == 0) ? "" : " ", (incl) ? "" : "<s>", col, (incl) ? "" : "</s>");
    g_free(col);
  }
  if(nb == 0)
  {
    txt = g_strdup(_("all"));
  }
  else if(nb > 1)
  {
    const gboolean op = val & 0x80000000;
    gchar *txt2 = g_strdup_printf("%s(%s)", (op) ? "∩" : "∪", txt);
    g_free(txt);
    txt = txt2;
  }
  return txt;
}

static gboolean _colors_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;

  rule->manual_widget_set++;
  _widgets_colors_t *colors = (_widgets_colors_t *)rule->w_specific;
  _widgets_colors_t *colorstop = (_widgets_colors_t *)rule->w_specific_top;

  const int mask = _get_mask(rule->raw_text);
  int mask_excluded = 0x1000;
  int mask_included = 1;
  int nb = 0;
  for(int i = 0; i <= DT_COLORLABELS_LAST; i++)
  {
    const int i_mask = mask & mask_excluded ? CPF_USER_DATA_EXCLUDE : mask & mask_included ? CPF_USER_DATA_INCLUDE : 0;
    dtgtk_button_set_paint(DTGTK_BUTTON(colors->colors[i]), dtgtk_cairo_paint_label_sel, (i | i_mask), NULL);
    gtk_widget_queue_draw(colors->colors[i]);
    if(colorstop)
    {
      dtgtk_button_set_paint(DTGTK_BUTTON(colorstop->colors[i]), dtgtk_cairo_paint_label_sel, (i | i_mask), NULL);
      gtk_widget_queue_draw(colorstop->colors[i]);
    }
    if((mask & mask_excluded) || (mask & mask_included))
      nb++;
    mask_excluded <<= 1;
    mask_included <<= 1;
  }
  if(nb <= 1)
    _set_mask(colors->rule, mask | CL_AND_MASK, FALSE);
  dtgtk_button_set_paint(DTGTK_BUTTON(colors->operator),
                         (mask & CL_AND_MASK) ? dtgtk_cairo_paint_and : dtgtk_cairo_paint_or, 0, NULL);
  gtk_widget_set_sensitive(colors->operator, nb > 1);
  gtk_widget_queue_draw(colors->operator);
  if(colorstop)
  {
    dtgtk_button_set_paint(DTGTK_BUTTON(colorstop->operator),
                           (mask & CL_AND_MASK) ? dtgtk_cairo_paint_and : dtgtk_cairo_paint_or, 0, NULL);
    gtk_widget_set_sensitive(colorstop->operator, nb > 1);
    gtk_widget_queue_draw(colorstop->operator);
  }

  rule->manual_widget_set--;

  return TRUE;
}

static gboolean _colors_enter_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  darktable.control->element = GPOINTER_TO_INT(user_data) + 1;
  return FALSE;
}

static float _action_process_colors(gpointer target, dt_action_element_t element, dt_action_effect_t effect, float move_size)
{
  if(!target) return DT_ACTION_NOT_VALID;

  _widgets_colors_t *colors = g_object_get_data(G_OBJECT(target), "colors_self");
  GtkWidget *w = element ? colors->colors[element - 1] : colors->operator;
  dt_lib_filtering_rule_t *rule = colors->rule;
  const int mask_k = (1 << (element - 1)) | (1 << (element - 1 + 12));
  int mask = _get_mask(rule->raw_text) & (element ? mask_k : CL_AND_MASK);

  if(DT_PERFORM_ACTION(move_size))
  {
    GdkEventButton e = { .state = effect == DT_ACTION_EFFECT_TOGGLE_CTRL ? GDK_CONTROL_MASK : 0 };

    if((!mask || (effect != DT_ACTION_EFFECT_ON && effect != DT_ACTION_EFFECT_ON_CTRL))
       && (mask || effect != DT_ACTION_EFFECT_OFF))
    {
      if(element)
        _colors_clicked(w, &e, colors);
      else
        _colors_operator_clicked(w, colors);
    }

    mask = _get_mask(rule->raw_text) & (element ? mask_k : CL_AND_MASK);
  }

  return mask != 0;
}

#undef CPF_USER_DATA_INCLUDE
#undef CPF_USER_DATA_EXCLUDE
#undef CL_AND_MASK
#undef CL_ALL_EXCLUDED
#undef CL_GREY_EXCLUDED
#undef CL_ALL_INCLUDED
#undef CL_GREY_INCLUDED

const dt_action_element_def_t _action_elements_colors[]
  = { { N_("operator"), dt_action_effect_toggle },
      { N_("red"     ), dt_action_effect_toggle },
      { N_("yellow"  ), dt_action_effect_toggle },
      { N_("green"   ), dt_action_effect_toggle },
      { N_("blue"    ), dt_action_effect_toggle },
      { N_("purple"  ), dt_action_effect_toggle },
      { N_("all"     ), dt_action_effect_toggle },
      { NULL } };

const dt_action_def_t dt_action_def_colors_rule
  = { N_("color filter"),
      _action_process_colors,
      _action_elements_colors };

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
  gtk_widget_set_name(hbox, "filter-colors-box");
  gtk_widget_set_halign(hbox, GTK_ALIGN_CENTER);
  for(int k = 0; k < DT_COLORLABELS_LAST + 1; k++)
  {
    colors->colors[k] = dtgtk_button_new(dtgtk_cairo_paint_label_sel, k, NULL);
    g_object_set_data(G_OBJECT(colors->colors[k]), "colors_index", GINT_TO_POINTER(k));
    dt_gui_add_class(colors->colors[k], "dt_no_hover");
    dt_gui_add_class(colors->colors[k], "dt_dimmed");
    g_object_set_data(G_OBJECT(colors->colors[k]), "colors_self", colors);
    gtk_box_pack_start(GTK_BOX(hbox), colors->colors[k], FALSE, FALSE, 0);
    gtk_widget_set_tooltip_text(colors->colors[k], _("filter by images color label"
                                                     "\nclick to toggle the color label selection"
                                                     "\nctrl+click to exclude the color label"
                                                     "\nthe gray button affects all color labels"));
    g_signal_connect(G_OBJECT(colors->colors[k]), "button-press-event", G_CALLBACK(_colors_clicked), colors);
    g_signal_connect(G_OBJECT(colors->colors[k]), "enter-notify-event", G_CALLBACK(_colors_enter_notify),
                     GINT_TO_POINTER(k));
    dt_action_define(DT_ACTION(self), N_("rules"), N_("color label"), colors->colors[k], &dt_action_def_colors_rule);
  }
  colors->operator= dtgtk_button_new(dtgtk_cairo_paint_and, 0, NULL);
  gtk_box_pack_start(GTK_BOX(hbox), colors->operator, FALSE, FALSE, 2);
  gtk_widget_set_tooltip_text(colors->operator,
                              _("filter by images color label"
                                "\nand (∩): images having all selected color labels"
                                "\nor (∪): images with at least one of the selected color labels"));
  g_signal_connect(G_OBJECT(colors->operator), "clicked", G_CALLBACK(_colors_operator_clicked), colors);
  g_signal_connect(G_OBJECT(colors->operator), "enter-notify-event", G_CALLBACK(_colors_enter_notify),
                   GINT_TO_POINTER(-1));
  dt_action_t *ac = dt_action_define(DT_ACTION(self), N_("rules"), N_("color label"), colors->operator, &dt_action_def_colors_rule);

  if(darktable.control->accel_initialising)
  {
    dt_shortcut_register(ac, DT_COLORLABELS_RED    + 1, DT_ACTION_EFFECT_TOGGLE, GDK_KEY_F1, GDK_SHIFT_MASK);
    dt_shortcut_register(ac, DT_COLORLABELS_YELLOW + 1, DT_ACTION_EFFECT_TOGGLE, GDK_KEY_F2, GDK_SHIFT_MASK);
    dt_shortcut_register(ac, DT_COLORLABELS_GREEN  + 1, DT_ACTION_EFFECT_TOGGLE, GDK_KEY_F3, GDK_SHIFT_MASK);
    dt_shortcut_register(ac, DT_COLORLABELS_BLUE   + 1, DT_ACTION_EFFECT_TOGGLE, GDK_KEY_F4, GDK_SHIFT_MASK);
    dt_shortcut_register(ac, DT_COLORLABELS_PURPLE + 1, DT_ACTION_EFFECT_TOGGLE, GDK_KEY_F5, GDK_SHIFT_MASK);
  }

  if(top)
  {
    dt_gui_add_class(hbox, "dt_quick_filter");
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
