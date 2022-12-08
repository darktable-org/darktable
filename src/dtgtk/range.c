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
#include "range.h"
#include "bauhaus/bauhaus.h"
#include "control/control.h"
#include "gui/gtk.h"

#include <string.h>
#include <locale.h>

#define SNAP_SIZE 5
#define BAR_WIDTH 4

// define GTypes
G_DEFINE_TYPE(GtkDarktableRangeSelect, _range_select, GTK_TYPE_EVENT_BOX);

static void _range_select_class_init(GtkDarktableRangeSelectClass *klass);
static void _range_select_init(GtkDarktableRangeSelect *button);

typedef struct _range_date_popup
{
  GtkWidget *popup;

  GtkWidget *type;

  GtkWidget *relative_label;

  GtkWidget *calendar;
  GtkWidget *relative_date_box;
  GtkWidget *years;
  GtkWidget *months;
  GtkWidget *days;
  GtkWidget *hours;
  GtkWidget *minutes;
  GtkWidget *seconds;

  GtkWidget *treeview;

  GtkWidget *selection;
  GtkWidget *ok_btn;
  GtkWidget *now_btn;

  int internal_change;
} _range_date_popup;

typedef struct _range_block
{
  double value_r; // this is the "real" value
  int nb; // nb of item with this value

  // this items are only used in case of a predetermined selection
  gchar *txt;
  double value2_r;
  dt_range_bounds_t bounds;
} _range_block;

typedef struct _range_icon
{
  int posx; // position of the icon in percent of the band width
  double value_r; // associated value for hover and selected flags (used for drawing icons)
  DTGTKCairoPaintIconFunc paint;
  gint flags;
  void *data;
} _range_icon;

typedef struct _range_marker
{
  double value_r;
  gboolean magnetic;
} _range_marker;

typedef enum _range_hover
{
  HOVER_OUTSIDE,
  HOVER_INSIDE,
  HOVER_MIN,
  HOVER_MAX
} _range_hover;

typedef enum _range_bound
{
  BOUND_MIN,
  BOUND_MAX,
  BOUND_MIDDLE
} _range_bound;

typedef enum _range_datetime_cols_t
{
  DATETIME_COL_TEXT = 0,
  DATETIME_COL_ID,
  DATETIME_COL_TOOLTIP,
  DATETIME_COL_PATH,
  DATETIME_COL_COUNT,
  DATETIME_COL_INDEX,
  DATETIME_NUM_COLS
} _range_datetime_cols_t;

typedef enum _range_signal
{
  VALUE_CHANGED,
  VALUE_RESET,
  LAST_SIGNAL
} _range_signal;
static guint _signals[LAST_SIGNAL] = { 0 };

static void _dt_pref_changed(gpointer instance, gpointer user_data)
{
  if(!user_data) return;
  GtkDarktableRangeSelect *range = (GtkDarktableRangeSelect *)user_data;

  GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(range->band));
  GtkStateFlags state = gtk_widget_get_state_flags(range->band);
  int mh = -1;
  int mw = -1;
  gtk_style_context_get(context, state, "min-height", &mh, NULL);
  gtk_style_context_get(context, state, "min-width", &mw, NULL);
  GtkBorder margin, padding;
  gtk_style_context_get_margin(context, state, &margin);
  gtk_style_context_get_padding(context, state, &padding);
  if(mw > 0)
    mw += margin.left + margin.right + padding.right + padding.left;
  else
    mw = -1;
  if(mh > 0)
    mh += margin.top + margin.bottom + padding.top + padding.bottom;
  else
    mh = -1;
  gtk_widget_set_size_request(range->band, mw, mh);

  dtgtk_range_select_redraw(range);
}

// cleanup everything when the widget is destroyed
static void _range_select_destroy(GtkWidget *widget)
{
  g_return_if_fail(DTGTK_IS_RANGE_SELECT(widget));

  GtkDarktableRangeSelect *range = DTGTK_RANGE_SELECT(widget);

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_dt_pref_changed), range);

  if(range->markers) g_list_free_full(range->markers, g_free);
  range->markers = NULL;
  if(range->blocks) g_list_free_full(range->blocks, g_free);
  range->blocks = NULL;
  if(range->icons) g_list_free_full(range->icons, g_free);
  range->icons = NULL;

  if(range->surface) cairo_surface_destroy(range->surface);
  range->surface = NULL;

  if(range->cur_help) g_free(range->cur_help);
  range->cur_help = NULL;

  GTK_WIDGET_CLASS(_range_select_parent_class)->destroy(widget);
}

static void _range_select_class_init(GtkDarktableRangeSelectClass *klass)
{
  GtkWidgetClass *widget_class = (GtkWidgetClass *)klass;
  widget_class->destroy = _range_select_destroy;

  _signals[VALUE_CHANGED] = g_signal_new("value-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                         NULL, g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);
  _signals[VALUE_RESET] = g_signal_new("value-reset", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                       g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);
}

static void _range_select_init(GtkDarktableRangeSelect *button)
{
}

static double _default_value_translator(const double value)
{
  return value;
}
static gchar *_default_print_func(const double value, const gboolean detailled)
{
  return g_strdup_printf("%.0lf", floor(value));
}
static gboolean _default_decode_func(const gchar *text, double *value)
{
  // TODO : verify the value is numeric
  gchar *locale = strdup(setlocale(LC_ALL, NULL));
  setlocale(LC_NUMERIC, "C");
  *value = atof(text);
  setlocale(LC_NUMERIC, locale);
  g_free(locale);
  return TRUE;
}
static gchar *_default_print_date_func(const double value, const gboolean detailled)
{
  if(!detailled)
  {
    char txt[DT_DATETIME_EXIF_LENGTH] = { 0 };
    if(dt_datetime_gtimespan_to_exif(txt, sizeof(txt), value))
      return g_strdup(txt);
    else
      return g_strdup(_("invalid"));
  }
  else
  {
    GDateTime *dt = dt_datetime_gtimespan_to_gdatetime(value);
    if(!dt) return g_strdup(_("invalid"));

    gchar *txt = g_date_time_format(dt, "%x %X");
    g_date_time_unref(dt);
    return txt;
  }
}
static gboolean _default_decode_date_func(const gchar *text, double *value)
{
  long val = dt_datetime_exif_to_gtimespan(text);
  if(val > 0)
  {
    *value = val;
    return TRUE;
  }
  return FALSE;
}

static void _date_tree_count_func(GtkTreeViewColumn *col, GtkCellRenderer *renderer, GtkTreeModel *model,
                                  GtkTreeIter *iter, gpointer data)
{
  gchar *name;
  guint count;

  gtk_tree_model_get(model, iter, DATETIME_COL_TEXT, &name, DATETIME_COL_COUNT, &count, -1);
  if(!count)
  {
    g_object_set(renderer, "text", name, NULL);
  }
  else
  {
    gchar *coltext = g_strdup_printf("%s (%d)", name, count);
    g_object_set(renderer, "text", coltext, NULL);
    g_free(coltext);
  }

  g_free(name);
}

static void _popup_date_recreate_model(GtkDarktableRangeSelect *range)
{
  _range_date_popup *pop = range->date_popup;
  gchar *name = NULL;
  gchar *tooltip = NULL;
  gchar *path = NULL;
  GtkTreeIter iter;

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(pop->treeview));
  gtk_tree_view_set_model(GTK_TREE_VIEW(pop->treeview), NULL);
  gtk_tree_store_clear(GTK_TREE_STORE(model));

  GtkTreeIter last_parent = { 0 };
  GDateTime *last_dt = NULL;
  int index = 0;
  int nb_predefined = 0;

  for(const GList *bl = range->blocks; bl; bl = g_list_next(bl))
  {
    _range_block *blo = bl->data;
    GDateTime *dt = dt_datetime_gtimespan_to_gdatetime(blo->value_r);
    if(!dt) continue;

    // find the number of common parts at the beginning of tokens and last_tokens
    GtkTreeIter parent = last_parent;
    int common_length = 0;
    if(last_dt && !blo->txt)
    {
      if(g_date_time_get_year(dt) == g_date_time_get_year(last_dt))
      {
        common_length++;
        if(g_date_time_get_month(dt) == g_date_time_get_month(last_dt))
        {
          common_length++;
          if(g_date_time_get_day_of_month(dt) == g_date_time_get_day_of_month(last_dt))
          {
            common_length++;
            // we stop here as we show time as last nodes
          }
        }
      }

      // point parent iter to where the entries should be added
      for(int i = common_length; i < 4; i++)
      {
        gtk_tree_model_iter_parent(model, &parent, &last_parent);
        last_parent = parent;
      }
    }

    if(blo->txt)
    {
      // this is a predefined entry, to be shown as root node on top
      tooltip = g_date_time_format(dt, "%x %X");
      path = g_date_time_format(dt, "%Y:%m:%d %H:%M:%S");
      gtk_tree_store_insert_with_values(GTK_TREE_STORE(model), &iter, NULL, nb_predefined, DATETIME_COL_TEXT,
                                        blo->txt, DATETIME_COL_TOOLTIP, tooltip, DATETIME_COL_PATH, path,
                                        DATETIME_COL_COUNT, 0, DATETIME_COL_INDEX, index, -1);
      index++;
      nb_predefined++;
      g_free(tooltip);
      g_free(path);
    }
    else
    {
      // insert year entry as root if needed
      if(common_length == 0)
      {
        name = g_date_time_format(dt, "%Y");
        tooltip = g_strdup_printf(_("year %s"), name);
        gtk_tree_store_insert_with_values(GTK_TREE_STORE(model), &iter, NULL, nb_predefined, DATETIME_COL_TEXT,
                                          name, DATETIME_COL_TOOLTIP, tooltip, DATETIME_COL_PATH, name,
                                          DATETIME_COL_COUNT, 0, DATETIME_COL_INDEX, index, -1);
        index++;
        common_length++;
        parent = iter;
        g_free(name);
        g_free(tooltip);
      }
      // insert month entry if needed
      if(common_length == 1)
      {
        name = g_date_time_format(dt, "%m");
        tooltip = g_date_time_format(dt, "%B %Y");
        path = g_date_time_format(dt, "%Y:%m");
        gtk_tree_store_insert_with_values(GTK_TREE_STORE(model), &iter, &parent, nb_predefined, DATETIME_COL_TEXT,
                                          name, DATETIME_COL_TOOLTIP, tooltip, DATETIME_COL_PATH, path,
                                          DATETIME_COL_COUNT, 0, DATETIME_COL_INDEX, index, -1);
        index++;
        common_length++;
        parent = iter;
        g_free(name);
        g_free(tooltip);
        g_free(path);
      }
      // insert day entry if needed
      if(common_length == 2)
      {
        name = g_date_time_format(dt, "%d");
        tooltip = g_date_time_format(dt, "%x");
        path = g_date_time_format(dt, "%Y:%m:%d");
        gtk_tree_store_insert_with_values(GTK_TREE_STORE(model), &iter, &parent, nb_predefined, DATETIME_COL_TEXT,
                                          name, DATETIME_COL_TOOLTIP, tooltip, DATETIME_COL_PATH, path,
                                          DATETIME_COL_COUNT, 0, DATETIME_COL_INDEX, index, -1);
        index++;
        common_length++;
        parent = iter;
        g_free(name);
        g_free(tooltip);
        g_free(path);
      }
      // in all cases, we need to add the time entry as last node
      name = g_date_time_format(dt, "%H:%M:%S");
      tooltip = g_date_time_format(dt, "%x %X");
      path = g_date_time_format(dt, "%Y:%m:%d %H:%M:%S");
      gtk_tree_store_insert_with_values(GTK_TREE_STORE(model), &iter, &parent, nb_predefined, DATETIME_COL_TEXT,
                                        name, DATETIME_COL_TOOLTIP, tooltip, DATETIME_COL_PATH, path,
                                        DATETIME_COL_COUNT, 0, DATETIME_COL_INDEX, index, -1);
      index++;
      last_parent = iter;
      g_free(name);
      g_free(tooltip);
      g_free(path);

      // all we return all the way back to increment counting
      while(gtk_tree_model_iter_parent(model, &parent, &iter))
      {
        int parentcount = 0;
        gtk_tree_model_get(model, &parent, DATETIME_COL_COUNT, &parentcount, -1);
        gtk_tree_store_set(GTK_TREE_STORE(model), &parent, DATETIME_COL_COUNT, blo->nb + parentcount, -1);
        iter = parent;
      }

      if(last_dt) g_date_time_unref(last_dt);
      last_dt = dt;
    }
  }
  if(last_dt) g_date_time_unref(last_dt);

  // now that the treemodel is OK, we update the treeview, based on this
  gtk_tree_view_set_model(GTK_TREE_VIEW(pop->treeview), model);
}

static void _entry_set_tooltip(GtkWidget *entry, const _range_bound bound, const dt_range_type_t range_type)
{
  if(range_type == DT_RANGE_TYPE_NUMERIC && bound == BOUND_MIN)
  {
    gtk_widget_set_tooltip_text(entry, _("enter the minimal value\n"
                                         "use 'min' if no bound\n"
                                         "right-click to select from existing values"));
  }
  else if(range_type == DT_RANGE_TYPE_NUMERIC && bound == BOUND_MAX)
  {
    gtk_widget_set_tooltip_text(entry, _("enter the maximal value\n"
                                         "use 'max' if no bound\n"
                                         "right-click to select from existing values"));
  }
  else if(range_type == DT_RANGE_TYPE_NUMERIC && bound == BOUND_MIDDLE)
  {
    gtk_widget_set_tooltip_text(entry, _("enter the value\n"
                                         "right-click to select from existing values"));
  }
  else if(range_type == DT_RANGE_TYPE_DATETIME && bound == BOUND_MIN)
  {
    gtk_widget_set_tooltip_text(entry, _("enter the minimal date\n"
                                         "in the form YYYY:MM:DD hh:mm:ss.sss (only the year is mandatory)\n"
                                         "use 'min' if no bound\n"
                                         "use '-' prefix for relative date\n"
                                         "right-click to select from calendar or existing values"));
  }
  else if(range_type == DT_RANGE_TYPE_DATETIME && bound == BOUND_MAX)
  {
    gtk_widget_set_tooltip_text(entry, _("enter the maximal date\n"
                                         "in the form YYYY:MM:DD hh:mm:ss.sss (only the year is mandatory)\n"
                                         "use 'max' if no bound\n"
                                         "'now' keyword is handled\n"
                                         "use '-' prefix for relative date\n"
                                         "right-click to select from calendar or existing values"));
  }
  else if(range_type == DT_RANGE_TYPE_DATETIME && bound == BOUND_MIDDLE)
  {
    gtk_widget_set_tooltip_text(entry, _("enter the date\n"
                                         "in the form YYYY:MM:DD hh:mm:ss.sss (only the year is mandatory)\n"
                                         "right-click to select from calendar or existing values"));
  }
}

static void _popup_date_update_widget_visibility(GtkDarktableRangeSelect *range)
{
  _range_date_popup *pop = range->date_popup;
  const int type = dt_bauhaus_combobox_get(pop->type);
  // first, we only allow fixed date for band right click
  if(gtk_popover_get_default_widget(GTK_POPOVER(pop->popup)) == range->band && type != 0)
  {
    dt_bauhaus_combobox_set(pop->type, 0);
    return;
  }

  if(type == 1)
  {
    // set the label
    if(gtk_popover_get_default_widget(GTK_POPOVER(pop->popup)) == range->entry_min)
    {
      gtk_label_set_text(GTK_LABEL(pop->relative_label), _("date-time interval to subtract from the max value"));
    }
    else
    {
      gtk_label_set_text(GTK_LABEL(pop->relative_label), _("date-time interval to add to the min value"));
    }
  }

  // set the visibility
  gtk_widget_set_visible(pop->calendar, type == 0);
  gtk_widget_set_visible(pop->relative_label, type == 1);
  gtk_widget_set_visible(pop->relative_date_box, type == 1);
  gtk_widget_set_visible(pop->now_btn, gtk_popover_get_default_widget(GTK_POPOVER(pop->popup)) == range->entry_max);
}

static void _popup_date_update(GtkDarktableRangeSelect *range, GtkWidget *w)
{
  _range_date_popup *pop = range->date_popup;
  gchar *txt;

  gtk_popover_set_default_widget(GTK_POPOVER(pop->popup), w);

  pop->internal_change++;

  dt_bauhaus_combobox_clear(pop->type);
  dt_bauhaus_combobox_add(pop->type, _("fixed"));
  if(w == range->entry_min || w == range->entry_max) dt_bauhaus_combobox_add(pop->type, _("relative"));
  gtk_widget_set_sensitive(pop->type, (w == range->entry_min || w == range->entry_max));

  int datetype = 0;
  if((w == range->entry_max && range->bounds & DT_RANGE_BOUND_MAX_RELATIVE)
     || (w == range->entry_min && range->bounds & DT_RANGE_BOUND_MIN_RELATIVE))
    datetype = 1;

  dt_bauhaus_combobox_set(pop->type, datetype);
  _popup_date_update_widget_visibility(range);

  // we also update the calendar part
  double val = 0.0;
  if(w == range->entry_max)
    val = range->select_max_r;
  else
    val = range->select_min_r;
  GDateTime *dt = dt_datetime_gtimespan_to_gdatetime(val);
  if(!dt) dt = g_date_time_new_now_utc();

  // update the calendar
  gtk_calendar_select_month(GTK_CALENDAR(pop->calendar), g_date_time_get_month(dt) - 1, g_date_time_get_year(dt));
  gtk_calendar_select_day(GTK_CALENDAR(pop->calendar), g_date_time_get_day_of_month(dt));
  gtk_calendar_clear_marks(GTK_CALENDAR(pop->calendar));
  gtk_calendar_mark_day(GTK_CALENDAR(pop->calendar), g_date_time_get_day_of_month(dt));

  // update the relative date fields
  char tx[32];
  snprintf(tx, sizeof(tx), "%d", range->select_relative_date_r.year);
  gtk_entry_set_text(GTK_ENTRY(pop->years), tx);
  snprintf(tx, sizeof(tx), "%d", range->select_relative_date_r.month);
  gtk_entry_set_text(GTK_ENTRY(pop->months), tx);
  snprintf(tx, sizeof(tx), "%d", range->select_relative_date_r.day);
  gtk_entry_set_text(GTK_ENTRY(pop->days), tx);

  // and the time fields
  if(datetype == 0)
  {
    txt = g_date_time_format(dt, "%H");
    gtk_entry_set_text(GTK_ENTRY(pop->hours), txt);
    g_free(txt);
    txt = g_date_time_format(dt, "%M");
    gtk_entry_set_text(GTK_ENTRY(pop->minutes), txt);
    g_free(txt);
    txt = g_date_time_format(dt, "%S");
    gtk_entry_set_text(GTK_ENTRY(pop->seconds), txt);
    g_free(txt);
  }
  else
  {
    snprintf(tx, sizeof(tx), "%d", range->select_relative_date_r.hour);
    gtk_entry_set_text(GTK_ENTRY(pop->hours), tx);
    snprintf(tx, sizeof(tx), "%d", range->select_relative_date_r.minute);
    gtk_entry_set_text(GTK_ENTRY(pop->minutes), tx);
    snprintf(tx, sizeof(tx), "%d", range->select_relative_date_r.second);
    gtk_entry_set_text(GTK_ENTRY(pop->seconds), tx);
  }

  // and we finally populate the selection fields
  if(datetype == 0)
  {
    txt = g_date_time_format(dt, "%Y:%m:%d %H:%M:%S");
    gtk_entry_set_text(GTK_ENTRY(pop->selection), txt);
    g_free(txt);
  }
  else
  {
    snprintf(tx, sizeof(tx), "%s%04d:%02d:%02d %02d:%02d:%02d", (w == range->entry_max) ? "+" : "-",
             range->select_relative_date_r.year, range->select_relative_date_r.month,
             range->select_relative_date_r.day, range->select_relative_date_r.hour,
             range->select_relative_date_r.minute, range->select_relative_date_r.second);
    gtk_entry_set_text(GTK_ENTRY(pop->selection), tx);
  }

  // and we set its tooltip
  _range_bound bound = BOUND_MIN;
  if(w == range->band)
    bound = BOUND_MIDDLE;
  else if(w == range->entry_max)
    bound = BOUND_MAX;
  _entry_set_tooltip(pop->selection, bound, DT_RANGE_TYPE_DATETIME);

  pop->internal_change--;
}

static void _current_set_text(GtkDarktableRangeSelect *range, const double current_value_r)
{
  if(!range->cur_label) return;
  gchar *val = range->print(current_value_r, TRUE);
  gchar *sel = range->current_bounds(range);
  gchar *txt = g_strdup_printf("<b>%s</b> | %s %s", val, _("selected"), sel);

  gtk_label_set_markup(GTK_LABEL(range->cur_label), txt);
  g_free(txt);
  g_free(sel);
  g_free(val);
}

static void _current_hide_popup(GtkDarktableRangeSelect *range)
{
  if(!range->cur_window) return;
  gtk_widget_destroy(range->cur_window);
  range->cur_window = NULL;
}

static void _current_show_popup(GtkDarktableRangeSelect *range)
{
  if(range->cur_window) return;
  range->cur_window = gtk_popover_new(range->band);
  gtk_widget_set_name(range->cur_window, "range-current");
  gtk_popover_set_modal(GTK_POPOVER(range->cur_window), FALSE);
  gtk_popover_set_position(GTK_POPOVER(range->cur_window), GTK_POS_BOTTOM);

  GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  // the label for the current value / selection
  range->cur_label = gtk_label_new("");
  dt_gui_add_class(range->cur_label, "dt_transparent_background");
  PangoAttrList *attrlist = pango_attr_list_new();
  PangoAttribute *attr = pango_attr_font_features_new("tnum");
  pango_attr_list_insert(attrlist, attr);
  gtk_label_set_attributes(GTK_LABEL(range->cur_label), attrlist);
  pango_attr_list_unref(attrlist);
  _current_set_text(range, 0);
  gtk_box_pack_start(GTK_BOX(vb), range->cur_label, FALSE, TRUE, 0);

  // the label for the static infos
  GtkWidget *lb = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(lb), 0.0);
  if(range->cur_help) gtk_label_set_markup(GTK_LABEL(lb), range->cur_help);
  gtk_box_pack_start(GTK_BOX(vb), lb, FALSE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(range->cur_window), vb);
  gtk_widget_show_all(range->cur_window);
}

static void _bound_change(GtkDarktableRangeSelect *range, const gchar *val, const _range_bound bound)
{
  gchar *txt = g_strstrip(g_utf8_strdown(val, -1));
  if(bound == BOUND_MIN && !g_strcmp0(txt, _("min")))
  {
    range->bounds |= DT_RANGE_BOUND_MIN;
    range->bounds &= ~DT_RANGE_BOUND_MIN_RELATIVE;
    range->bounds &= ~DT_RANGE_BOUND_FIXED;
  }
  else if(bound == BOUND_MAX && !g_strcmp0(txt, _("max")))
  {
    range->bounds |= DT_RANGE_BOUND_MAX;
    range->bounds &= ~DT_RANGE_BOUND_MAX_RELATIVE;
    range->bounds &= ~DT_RANGE_BOUND_FIXED;
    range->bounds &= ~DT_RANGE_BOUND_MAX_NOW;
  }
  else if(range->type == DT_RANGE_TYPE_DATETIME && bound == BOUND_MIDDLE && !g_strcmp0(txt, "now"))
  {
    range->bounds = DT_RANGE_BOUND_FIXED;
    range->select_min_r = range->select_max_r = dt_datetime_now_to_gtimespan();
  }
  else if(range->type == DT_RANGE_TYPE_DATETIME && bound == BOUND_MAX && !g_strcmp0(txt, "now"))
  {
    range->bounds &= ~DT_RANGE_BOUND_MAX;
    range->bounds &= ~DT_RANGE_BOUND_MAX_RELATIVE;
    range->bounds &= ~DT_RANGE_BOUND_FIXED;
    range->bounds |= DT_RANGE_BOUND_MAX_NOW;
    range->select_max_r = dt_datetime_now_to_gtimespan();
  }
  else if(range->type == DT_RANGE_TYPE_DATETIME && bound == BOUND_MAX && g_str_has_prefix(txt, "+")
          && !(range->bounds & DT_RANGE_BOUND_MIN_RELATIVE))
  {
    if(dt_datetime_exif_to_numbers_raw(&range->select_relative_date_r, txt + 1))
    {
      range->bounds &= ~DT_RANGE_BOUND_MAX;
      range->bounds |= DT_RANGE_BOUND_MAX_RELATIVE;
      range->bounds &= ~DT_RANGE_BOUND_FIXED;
      range->bounds &= ~DT_RANGE_BOUND_MAX_NOW;
      range->select_max_r
          = dt_datetime_gtimespan_add_numbers(range->select_min_r, range->select_relative_date_r, TRUE);
    }
  }
  else if(range->type == DT_RANGE_TYPE_DATETIME && bound == BOUND_MIN && g_str_has_prefix(txt, "-")
          && !(range->bounds & DT_RANGE_BOUND_MAX_RELATIVE))
  {
    if(dt_datetime_exif_to_numbers_raw(&range->select_relative_date_r, txt + 1))
    {
      range->bounds &= ~DT_RANGE_BOUND_MIN;
      range->bounds |= DT_RANGE_BOUND_MIN_RELATIVE;
      range->bounds &= ~DT_RANGE_BOUND_FIXED;
      range->select_min_r
          = dt_datetime_gtimespan_add_numbers(range->select_max_r, range->select_relative_date_r, FALSE);
    }
  }
  else
  {
    double v = 0.0;
    if(range->decode(txt, &v))
    {
      if(bound == BOUND_MIN)
      {
        range->bounds &= ~DT_RANGE_BOUND_MIN;
        range->bounds &= ~DT_RANGE_BOUND_MIN_RELATIVE;
        range->bounds &= ~DT_RANGE_BOUND_FIXED;
        range->select_min_r = v;
      }
      else if(bound == BOUND_MAX)
      {
        range->bounds &= ~DT_RANGE_BOUND_MAX;
        range->bounds &= ~DT_RANGE_BOUND_MAX_RELATIVE;
        range->bounds &= ~DT_RANGE_BOUND_MAX_NOW;
        range->bounds &= ~DT_RANGE_BOUND_FIXED;
        range->select_max_r = v;
      }
      else if(bound == BOUND_MIDDLE)
      {
        range->bounds = DT_RANGE_BOUND_FIXED;
        range->select_min_r = range->select_max_r = v;
      }
    }
  }
  g_free(txt);

  dtgtk_range_select_set_selection(range, range->bounds, range->select_min_r, range->select_max_r, TRUE, FALSE);
}

static void _popup_date_ok_clicked(GtkWidget *w, GtkDarktableRangeSelect *range)
{
  if(!range->date_popup || range->date_popup->internal_change) return;
  _range_date_popup *pop = range->date_popup;

  _range_bound bound = BOUND_MIN;
  if(gtk_popover_get_default_widget(GTK_POPOVER(pop->popup)) == range->band)
    bound = BOUND_MIDDLE;
  else if(gtk_popover_get_default_widget(GTK_POPOVER(pop->popup)) == range->entry_max)
    bound = BOUND_MAX;

  _bound_change(range, gtk_entry_get_text(GTK_ENTRY(pop->selection)), bound);

  // and hide the popup
  gtk_widget_hide(pop->popup);
}

static void _popup_date_now_clicked(GtkWidget *w, GtkDarktableRangeSelect *range)
{
  if(!range->date_popup || range->date_popup->internal_change) return;
  _range_date_popup *pop = range->date_popup;

  if(gtk_popover_get_default_widget(GTK_POPOVER(pop->popup)) != range->entry_max) return;

  range->bounds &= ~DT_RANGE_BOUND_MAX;
  range->bounds &= ~DT_RANGE_BOUND_MAX_RELATIVE;
  range->bounds &= ~DT_RANGE_BOUND_FIXED;
  range->bounds |= DT_RANGE_BOUND_MAX_NOW;

  dtgtk_range_select_set_selection(range, range->bounds, range->select_min_r, range->select_max_r, TRUE, FALSE);

  // and hide the popup
  gtk_widget_hide(pop->popup);
}

static void _popup_date_tree_row_activated(GtkTreeView *self, GtkTreePath *path, GtkTreeViewColumn *column,
                                           GtkDarktableRangeSelect *range)
{
  if(!range->date_popup || range->date_popup->internal_change) return;
  _range_date_popup *pop = range->date_popup;

  // we validate the ok button
  gtk_widget_activate(pop->ok_btn);
}

static void _popup_date_tree_selection_change(GtkTreeView *self, GtkDarktableRangeSelect *range)
{
  if(!range->date_popup || range->date_popup->internal_change) return;
  _range_date_popup *pop = range->date_popup;

  // we retrieve the row path
  GtkTreeIter iter;
  gchar *text = NULL;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(pop->treeview));
  if(!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(pop->treeview)), NULL, &iter))
    return;

  gtk_tree_model_get(model, &iter, DATETIME_COL_PATH, &text, -1);

  // we decode the path
  int y, m, d, h, min, s;
  y = h = min = s = 0;
  m = d = 1;
  if(g_str_has_prefix(text, "b"))
  {
    // that means this is a predefined block, so we just need to read its value
  }
  else
  {
    // initialize value depending of the source widget
    if(gtk_popover_get_default_widget(GTK_POPOVER(pop->popup)) == range->entry_max)
    {
      m = 12;
      d = 31;
      h = 23;
      min = 59;
      s = 59;
    }

    GMatchInfo *match_info;
    // we capture each date componenent
    GRegex *regex = g_regex_new(
        "^\\s*(\\d{4})?(?::(\\d{2}))?(?::(\\d{2}))?(?: (\\d{2}))?(?::(\\d{2}))?(?::(\\d{2}))?\\s*$", 0, 0, NULL);
    g_regex_match_full(regex, text, -1, 0, 0, &match_info, NULL);
    int match_count = g_match_info_get_match_count(match_info);

    if(match_count <= 1)
    {
      // invalid path
      g_match_info_free(match_info);
      g_regex_unref(regex);
      return;
    }
    if(match_count > 1)
    {
      gchar *nb = g_match_info_fetch(match_info, 1);
      y = MAX(0, atoi(nb));
      g_free(nb);
    }
    if(match_count > 2)
    {
      gchar *nb = g_match_info_fetch(match_info, 2);
      m = CLAMP(atoi(nb), 1, 12);
      g_free(nb);
    }
    // we now need to determine the number of days of this month
    int max_day = g_date_get_days_in_month(m, y);
    d = MIN(d, max_day);
    if(match_count > 3)
    {
      gchar *nb = g_match_info_fetch(match_info, 3);
      d = CLAMP(atoi(nb), 0, 31);
      g_free(nb);
    }
    if(match_count > 4)
    {
      gchar *nb = g_match_info_fetch(match_info, 4);
      h = CLAMP(atoi(nb), 0, 23);
      g_free(nb);
    }
    if(match_count > 5)
    {
      gchar *nb = g_match_info_fetch(match_info, 5);
      min = CLAMP(atoi(nb), 0, 59);
      g_free(nb);
    }
    if(match_count > 6)
    {
      gchar *nb = g_match_info_fetch(match_info, 6);
      s = CLAMP(atoi(nb), 0, 59);
      g_free(nb);
    }

    g_match_info_free(match_info);
    g_regex_unref(regex);
  }

  // we set the final entry
  gchar *txt = g_strdup_printf("%04d:%02d:%02d %02d:%02d:%02d", y, m, d, h, min, s);
  gtk_entry_set_text(GTK_ENTRY(pop->selection), txt);
  g_free(txt);
}

static void _popup_date_changed(GtkWidget *w, GtkDarktableRangeSelect *range)
{
  if(!range->date_popup || range->date_popup->internal_change) return;
  // we just update the final entry text
  _range_date_popup *pop = range->date_popup;

  guint y, m, d;
  if(dt_bauhaus_combobox_get(pop->type) == 1)
  {
    y = MAX(atoi(gtk_entry_get_text(GTK_ENTRY(pop->years))), 0);
    m = MAX(atoi(gtk_entry_get_text(GTK_ENTRY(pop->months))), 0);
    d = MAX(atoi(gtk_entry_get_text(GTK_ENTRY(pop->days))), 0);
  }
  else
  {
    gtk_calendar_get_date(GTK_CALENDAR(pop->calendar), &y, &m, &d);
    m++;
  }
  int h = CLAMP(atoi(gtk_entry_get_text(GTK_ENTRY(pop->hours))), 0, 23);
  int min = CLAMP(atoi(gtk_entry_get_text(GTK_ENTRY(pop->minutes))), 0, 59);
  int s = CLAMP(atoi(gtk_entry_get_text(GTK_ENTRY(pop->seconds))), 0, 59);

  // if we select via calendar, we try to set time to what user expect
  if(w == pop->calendar)
  {
    // if we set the max value, and we have null time, we want to set time to the end of the day
    if(gtk_popover_get_default_widget(GTK_POPOVER(pop->popup)) == range->entry_max && h == 0 && min == 0 && s == 0)
    {
      h = 23;
      min = 59;
      s = 59;
      pop->internal_change++;
      gtk_entry_set_text(GTK_ENTRY(pop->hours), "23");
      gtk_entry_set_text(GTK_ENTRY(pop->minutes), "59");
      gtk_entry_set_text(GTK_ENTRY(pop->seconds), "59");
      pop->internal_change--;
    }
    // same for min value (but less common)
    else if(gtk_popover_get_default_widget(GTK_POPOVER(pop->popup)) == range->entry_min && h == 23 && min == 59
            && s == 59)
    {
      h = min = s = 0;
      pop->internal_change++;
      gtk_entry_set_text(GTK_ENTRY(pop->hours), "00");
      gtk_entry_set_text(GTK_ENTRY(pop->minutes), "00");
      gtk_entry_set_text(GTK_ENTRY(pop->seconds), "00");
      pop->internal_change--;
    }
  }

  gchar *txt = NULL;
  if(dt_bauhaus_combobox_get(pop->type) == 1
     && gtk_popover_get_default_widget(GTK_POPOVER(pop->popup)) == range->entry_min)
    txt = g_strdup_printf("-%04d:%02d:%02d %02d:%02d:%02d", y, m, d, h, min, s);
  else if(dt_bauhaus_combobox_get(pop->type) == 1
          && gtk_popover_get_default_widget(GTK_POPOVER(pop->popup)) == range->entry_max)
    txt = g_strdup_printf("+%04d:%02d:%02d %02d:%02d:%02d", y, m, d, h, min, s);
  else
    txt = g_strdup_printf("%04d:%02d:%02d %02d:%02d:%02d", y, m, d, h, min, s);

  gtk_entry_set_text(GTK_ENTRY(pop->selection), txt);
  g_free(txt);
}

static void _popup_date_day_selected_2click(GtkWidget *w, GtkDarktableRangeSelect *range)
{
  if(!range->date_popup || range->date_popup->internal_change) return;
  // we validate the ok button
  gtk_widget_activate(range->date_popup->ok_btn);
}

static void _popup_date_type_changed(GtkWidget *w, GtkDarktableRangeSelect *range)
{
  if(!range->date_popup || range->date_popup->internal_change) return;

  _popup_date_update_widget_visibility(range);
}

static void _popup_date_init(GtkDarktableRangeSelect *range)
{
  _range_date_popup *pop = (_range_date_popup *)g_malloc0(sizeof(_range_date_popup));
  range->date_popup = pop;
  pop->popup = gtk_popover_new(range->band);
  GtkWidget *vbox0 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(vbox0, "dt-range-date-popup");
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_set_homogeneous(GTK_BOX(hbox), TRUE);
  gtk_box_pack_start(GTK_BOX(vbox0), hbox, FALSE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(pop->popup), vbox0);
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(hbox), vbox, FALSE, TRUE, 0);

  // the type of date selection
  pop->type = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(pop->type, NULL, _("date type"));
  g_signal_connect(G_OBJECT(pop->type), "value-changed", G_CALLBACK(_popup_date_type_changed), range);
  gtk_box_pack_start(GTK_BOX(vbox), pop->type, FALSE, TRUE, 0);

  // the label to explain the reference date for relative values
  pop->relative_label = gtk_label_new("");
  gtk_label_set_line_wrap(GTK_LABEL(pop->relative_label), TRUE);
  gtk_widget_set_no_show_all(pop->relative_label, TRUE);
  gtk_box_pack_start(GTK_BOX(vbox), pop->relative_label, FALSE, TRUE, 0);

  // the date section
  GtkWidget *lb = gtk_label_new(_("date"));
  dt_gui_add_class(lb, "dt_section_label");
  gtk_box_pack_start(GTK_BOX(vbox), lb, FALSE, TRUE, 0);

  // the calendar
  pop->calendar = gtk_calendar_new();
  gtk_widget_set_no_show_all(pop->calendar, TRUE);
  gtk_widget_set_tooltip_text(pop->calendar, _("click to select date\n"
                                               "double-click to use the date directly"));
  g_signal_connect(G_OBJECT(pop->calendar), "day_selected", G_CALLBACK(_popup_date_changed), range);
  g_signal_connect(G_OBJECT(pop->calendar), "day_selected-double-click",
                   G_CALLBACK(_popup_date_day_selected_2click), range);
  gtk_box_pack_start(GTK_BOX(vbox), pop->calendar, FALSE, TRUE, 0);

  // the relative date box
  pop->relative_date_box = gtk_grid_new();
  gtk_grid_set_column_homogeneous(GTK_GRID(pop->relative_date_box), TRUE);
  gtk_box_pack_start(GTK_BOX(vbox), pop->relative_date_box, FALSE, TRUE, 0);
  lb = gtk_label_new(_("years: "));
  gtk_label_set_xalign(GTK_LABEL(lb), 1.0);
  gtk_grid_attach(GTK_GRID(pop->relative_date_box), lb, 0, 0, 1, 1);
  pop->years = gtk_entry_new();
  gtk_entry_set_width_chars(GTK_ENTRY(pop->years), 3);
  gtk_widget_set_halign(pop->years, GTK_ALIGN_START);
  g_signal_connect(G_OBJECT(pop->years), "changed", G_CALLBACK(_popup_date_changed), range);
  gtk_grid_attach(GTK_GRID(pop->relative_date_box), pop->years, 1, 0, 1, 1);
  lb = gtk_label_new(_("months: "));
  gtk_label_set_xalign(GTK_LABEL(lb), 1.0);
  gtk_grid_attach(GTK_GRID(pop->relative_date_box), lb, 0, 1, 1, 1);
  pop->months = gtk_entry_new();
  gtk_entry_set_width_chars(GTK_ENTRY(pop->months), 3);
  gtk_widget_set_halign(pop->months, GTK_ALIGN_START);
  g_signal_connect(G_OBJECT(pop->months), "changed", G_CALLBACK(_popup_date_changed), range);
  gtk_grid_attach(GTK_GRID(pop->relative_date_box), pop->months, 1, 1, 1, 1);
  lb = gtk_label_new(_("days: "));
  gtk_label_set_xalign(GTK_LABEL(lb), 1.0);
  gtk_grid_attach(GTK_GRID(pop->relative_date_box), lb, 0, 2, 1, 1);
  pop->days = gtk_entry_new();
  gtk_entry_set_width_chars(GTK_ENTRY(pop->days), 3);
  gtk_widget_set_halign(pop->days, GTK_ALIGN_START);
  g_signal_connect(G_OBJECT(pop->days), "changed", G_CALLBACK(_popup_date_changed), range);
  gtk_grid_attach(GTK_GRID(pop->relative_date_box), pop->days, 1, 2, 1, 1);
  gtk_widget_show_all(pop->relative_date_box);
  gtk_widget_set_no_show_all(pop->relative_date_box, TRUE);

  // the time section
  lb = gtk_label_new(_("time"));
  dt_gui_add_class(lb, "dt_section_label");
  gtk_box_pack_start(GTK_BOX(vbox), lb, FALSE, TRUE, 0);

  GtkWidget *hbox2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_halign(hbox2, GTK_ALIGN_CENTER);
  gtk_box_pack_start(GTK_BOX(vbox), hbox2, FALSE, FALSE, 0);
  pop->hours = gtk_entry_new();
  gtk_entry_set_width_chars(GTK_ENTRY(pop->hours), 2);
  g_signal_connect(G_OBJECT(pop->hours), "changed", G_CALLBACK(_popup_date_changed), range);
  gtk_box_pack_start(GTK_BOX(hbox2), pop->hours, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox2), gtk_label_new(" : "), FALSE, TRUE, 0);
  pop->minutes = gtk_entry_new();
  gtk_entry_set_width_chars(GTK_ENTRY(pop->minutes), 2);
  g_signal_connect(G_OBJECT(pop->minutes), "changed", G_CALLBACK(_popup_date_changed), range);
  gtk_box_pack_start(GTK_BOX(hbox2), pop->minutes, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox2), gtk_label_new(" : "), FALSE, TRUE, 0);
  pop->seconds = gtk_entry_new();
  gtk_entry_set_width_chars(GTK_ENTRY(pop->seconds), 2);
  g_signal_connect(G_OBJECT(pop->seconds), "changed", G_CALLBACK(_popup_date_changed), range);
  gtk_box_pack_start(GTK_BOX(hbox2), pop->seconds, FALSE, TRUE, 0);

  // the treeview
  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  GtkTreeModel *model = GTK_TREE_MODEL(gtk_tree_store_new(DATETIME_NUM_COLS, G_TYPE_STRING, G_TYPE_UINT,
                                                          G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT, G_TYPE_UINT));
  pop->treeview = gtk_tree_view_new_with_model(model);
  gtk_widget_set_tooltip_text(pop->calendar, _("click to select date\n"
                                               "double-click to use the date directly"));
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(pop->treeview), FALSE);
  g_signal_connect(G_OBJECT(pop->treeview), "row-activated", G_CALLBACK(_popup_date_tree_row_activated), range);
  g_signal_connect(G_OBJECT(gtk_tree_view_get_selection(GTK_TREE_VIEW(pop->treeview))), "changed",
                   G_CALLBACK(_popup_date_tree_selection_change), range);

  GtkTreeViewColumn *col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(GTK_TREE_VIEW(pop->treeview), col);
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_set_cell_data_func(col, renderer, _date_tree_count_func, NULL, NULL);

  gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(pop->treeview), DATETIME_COL_TOOLTIP);

  gtk_container_add(GTK_CONTAINER(sw), pop->treeview);
  gtk_box_pack_start(GTK_BOX(hbox), sw, FALSE, TRUE, 0);

  // the select line
  hbox2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(vbox0), hbox2, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox2), gtk_label_new(_("current date: ")), FALSE, TRUE, 0);
  pop->selection = gtk_entry_new();
  gtk_entry_set_alignment(GTK_ENTRY(pop->selection), 0.5);
  gtk_box_pack_start(GTK_BOX(hbox2), pop->selection, TRUE, TRUE, 0);
  pop->now_btn = gtk_button_new_with_label(_("now"));
  gtk_widget_set_no_show_all(pop->now_btn, TRUE);
  gtk_widget_set_tooltip_text(pop->now_btn, _("set the value to always match current datetime"));
  g_signal_connect(G_OBJECT(pop->now_btn), "clicked", G_CALLBACK(_popup_date_now_clicked), range);
  gtk_box_pack_start(GTK_BOX(hbox2), pop->now_btn, FALSE, TRUE, 0);
  pop->ok_btn = gtk_button_new_with_label(_("apply"));
  gtk_widget_set_tooltip_text(pop->ok_btn, _("set the range bound with this value"));
  g_signal_connect(G_OBJECT(pop->ok_btn), "clicked", G_CALLBACK(_popup_date_ok_clicked), range);
  gtk_box_pack_start(GTK_BOX(hbox2), pop->ok_btn, FALSE, TRUE, 0);
}

static void _popup_item_activate(GtkWidget *w, gpointer user_data)
{
  GtkDarktableRangeSelect *range = (GtkDarktableRangeSelect *)user_data;
  // retrieve block and source values
  GtkWidget *source = GTK_WIDGET(g_object_get_data(G_OBJECT(w), "source_widget"));
  _range_block *blo = (_range_block *)g_object_get_data(G_OBJECT(w), "range_block");

  // if source is the band, apply the value directly
  if(source == range->band)
  {
    dtgtk_range_select_set_selection(range, blo->bounds, blo->value_r, blo->value2_r, TRUE, FALSE);
  }
  else if(source == range->entry_min)
  {
    if(range->bounds & DT_RANGE_BOUND_MIN) range->bounds &= ~DT_RANGE_BOUND_MIN;
    dtgtk_range_select_set_selection(range, range->bounds, blo->value_r, range->select_max_r, TRUE, FALSE);
  }
  else if(source == range->entry_max)
  {
    if(range->bounds & DT_RANGE_BOUND_MAX) range->bounds &= ~DT_RANGE_BOUND_MAX;
    dtgtk_range_select_set_selection(range, range->bounds, range->select_min_r, blo->value_r, TRUE, FALSE);
  }
}

static GtkWidget *_popup_get_numeric_menu(GtkDarktableRangeSelect *range, GtkWidget *w)
{
  GtkMenuShell *pop = GTK_MENU_SHELL(gtk_menu_new());
  gtk_widget_set_size_request(GTK_WIDGET(pop), 200, -1);

  // we first show all the predefined items
  int nb = 0;
  for(const GList *bl = range->blocks; bl; bl = g_list_next(bl))
  {
    _range_block *blo = bl->data;
    // we don't allow range block on entries
    if(!blo->txt) continue;
    // we don't allow range block on entries
    if(w != range->band && blo->bounds != DT_RANGE_BOUND_FIXED) continue;
    // for entries, we only values that won't change the min-max order
    if(w == range->entry_min && !(range->bounds & DT_RANGE_BOUND_MAX) && blo->value_r > range->select_max_r)
      continue;
    if(w == range->entry_max && !(range->bounds & DT_RANGE_BOUND_MIN) && blo->value_r < range->select_min_r)
      continue;

    gchar *txt = (blo->txt) ? g_strdup(blo->txt) : range->print(blo->value_r, TRUE);
    if(blo->nb > 0) txt = dt_util_dstrcat(txt, " (%d)", blo->nb);
    GtkWidget *smt = gtk_menu_item_new_with_label(txt);
    g_free(txt);
    g_object_set_data(G_OBJECT(smt), "range_block", blo);
    g_object_set_data(G_OBJECT(smt), "source_widget", w);
    g_signal_connect(G_OBJECT(smt), "activate", G_CALLBACK(_popup_item_activate), range);

    gtk_menu_shell_append(pop, smt);
    nb++;
  }

  if(nb > 0 && g_list_length(range->blocks) - nb > 0) gtk_menu_shell_append(pop, gtk_separator_menu_item_new());

  // and the classic ones
  for(const GList *bl = range->blocks; bl; bl = g_list_next(bl))
  {
    _range_block *blo = bl->data;
    if(blo->txt) continue;
    // we don't allow range block on entries
    if(w != range->band && blo->bounds != DT_RANGE_BOUND_FIXED) continue;
    // for entries, we only values that won't change the min-max order
    if(w == range->entry_min && !(range->bounds & DT_RANGE_BOUND_MAX) && blo->value_r > range->select_max_r)
      continue;
    if(w == range->entry_max && !(range->bounds & DT_RANGE_BOUND_MIN) && blo->value_r < range->select_min_r)
      continue;

    gchar *txt = (blo->txt) ? g_strdup(blo->txt) : range->print(blo->value_r, TRUE);
    if(blo->nb > 0) txt = dt_util_dstrcat(txt, " (%d)", blo->nb);
    GtkWidget *smt = gtk_menu_item_new_with_label(txt);
    g_free(txt);
    g_object_set_data(G_OBJECT(smt), "range_block", blo);
    g_object_set_data(G_OBJECT(smt), "source_widget", w);
    g_signal_connect(G_OBJECT(smt), "activate", G_CALLBACK(_popup_item_activate), range);

    gtk_menu_shell_append(pop, smt);
  }

  return GTK_WIDGET(pop);
}

static void _popup_show(GtkDarktableRangeSelect *range, GtkWidget *w)
{
  if(range->type == DT_RANGE_TYPE_NUMERIC)
  {
    GtkWidget *pop = _popup_get_numeric_menu(range, w);
    dt_gui_menu_popup(GTK_MENU(pop), NULL, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH);
  }
  else if(range->type == DT_RANGE_TYPE_DATETIME)
  {
    _popup_date_update(range, w);

    // show the popup
    GdkDevice *pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));

    int x, y;
    GdkWindow *pointer_window = gdk_device_get_window_at_position(pointer, &x, &y);
    gpointer pointer_widget = NULL;
    if(pointer_window) gdk_window_get_user_data(pointer_window, &pointer_widget);

    GdkRectangle rect = { gtk_widget_get_allocated_width(w) / 2, gtk_widget_get_allocated_height(w), 1, 1 };

    if(pointer_widget && w != pointer_widget)
      gtk_widget_translate_coordinates(pointer_widget, w, x, y, &rect.x, &rect.y);

    gtk_popover_set_pointing_to(GTK_POPOVER(range->date_popup->popup), &rect);

    gtk_widget_show_all(range->date_popup->popup);
  }
}

static gboolean _event_entry_press(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  GtkDarktableRangeSelect *range = (GtkDarktableRangeSelect *)user_data;
  if(e->button == 3)
  {
    _popup_show(range, w);
    return TRUE;
  }
  return FALSE;
}

static void _event_entry_activated(GtkWidget *entry, gpointer user_data)
{
  GtkDarktableRangeSelect *range = (GtkDarktableRangeSelect *)user_data;
  _range_bound bound = BOUND_MIN;
  if(entry == range->entry_max) bound = BOUND_MAX;

  _bound_change(range, gtk_entry_get_text(GTK_ENTRY(entry)), bound);
}

static gboolean _event_entry_focus_out(GtkWidget *entry, GdkEventFocus *event, gpointer user_data)
{
  _event_entry_activated(entry, user_data);
  return FALSE;
}

static double _graph_value_to_pos(GtkDarktableRangeSelect *range, const double value)
{
  return (range->value_to_band(value) - range->band_start_bd) / range->band_factor;
}
static double _graph_value_from_pos(GtkDarktableRangeSelect *range, const double posx, gboolean snap)
{
  double ret = posx * range->band_factor + range->band_start_bd;
  // if needed we round the value toward step
  if(range->step_bd > 0.0)
  {
    ret = floor(ret / range->step_bd) * range->step_bd;
  }
  ret = range->value_from_band(ret);
  if(snap)
  {
    for(const GList *bl = range->markers; bl; bl = g_list_next(bl))
    {
      _range_marker *mark = bl->data;
      if(!mark->magnetic) continue;
      const int mpos = _graph_value_to_pos(range, mark->value_r);
      if(fabs(mpos - posx) < SNAP_SIZE) return mark->value_r;
    }
  }
  return ret;
}

static double _graph_snap_position(GtkDarktableRangeSelect *range, const double posx)
{
  double ret = posx;
  for(const GList *bl = range->markers; bl; bl = g_list_next(bl))
  {
    _range_marker *mark = bl->data;
    if(!mark->magnetic) continue;
    const int mpos = _graph_value_to_pos(range, mark->value_r);
    if(fabs(mpos - posx) < SNAP_SIZE) return mpos;
  }
  return ret;
}

static int _graph_get_height(const int val, const int max, const int height)
{
  return sqrt(val / (double)max) * (height * 0.8) + height * 0.1;
}

static void _range_set_source_rgba(cairo_t *cr, GtkWidget *w, double alpha, const GtkStateFlags state)
{
  // retrieve the css color of a widget and apply it to cairo surface
  GtkStyleContext *context = gtk_widget_get_style_context(w);
  GdkRGBA coul;
  gtk_style_context_get_color(context, state, &coul);
  cairo_set_source_rgba(cr, coul.red, coul.green, coul.blue, coul.alpha * alpha);
}

static gboolean _event_band_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  GtkDarktableRangeSelect *range = (GtkDarktableRangeSelect *)user_data;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  GtkStateFlags state = gtk_widget_get_state_flags(range->band);

  // draw the graph (and create it if needed)
  if(!range->surface || range->alloc_main.width != allocation.width
     || range->alloc_main.height != allocation.height)
  {
    range->alloc_main = allocation;
    // get all the margins, paddings defined in css
    GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(range->band));
    GtkBorder margin, padding;
    gtk_style_context_get_margin(context, state, &margin);
    gtk_style_context_get_padding(context, state, &padding);

    // set the area inside margins
    range->alloc_margin.width = range->alloc_main.width - margin.left - margin.right;
    range->alloc_margin.x = margin.left;
    range->alloc_margin.height = range->alloc_main.height - margin.top - margin.bottom;
    range->alloc_margin.y = margin.top;

    if(range->max_width_px > 0 && range->alloc_margin.width > range->max_width_px)
    {
      const int dx = range->alloc_margin.width - range->max_width_px;
      range->alloc_margin.width -= dx;
      range->alloc_margin.x += dx / 2;
    }

    // set the area inside padding
    range->alloc_padding.width = range->alloc_margin.width - padding.left - padding.right;
    range->alloc_padding.x = range->alloc_margin.x + padding.left;
    range->alloc_padding.height = range->alloc_margin.height - padding.top - padding.bottom;
    range->alloc_padding.y = range->alloc_margin.y + padding.top;

    // if the surface already exist, destroy it
    if(range->surface) cairo_surface_destroy(range->surface);

    // determine the steps of blocks and extrema values
    range->band_start_bd = range->value_to_band(range->min_r);
    const double width_bd = range->value_to_band(range->max_r) - range->band_start_bd;
    range->band_factor = width_bd / range->alloc_padding.width;
    // we want at least blocks with width of BAR_WIDTH pixels
    const double step_bd = fmax(range->step_bd, range->band_factor * BAR_WIDTH);
    const int bl_width_px = step_bd / range->band_factor;

    // get the maximum height of blocks
    // we have to do some clever things in order to packed together blocks that will be shown at the same place
    // (see step above)
    double bl_min_px = 0;
    int bl_count = 0;
    int count_max = 0;
    for(const GList *bl = range->blocks; bl; bl = g_list_next(bl))
    {
      _range_block *blo = bl->data;
      if(blo->txt) continue;
      // are we inside the current block ?
      const int blo_pos_px = _graph_value_to_pos(range, blo->value_r);
      if(blo_pos_px - bl_min_px < bl_width_px)
        bl_count += blo->nb;
      else
      {
        // we store the previous block count
        count_max = MAX(count_max, bl_count);
        bl_count = blo->nb;
        bl_min_px = (int)(blo_pos_px / bl_width_px) * bl_width_px;
      }
    }
    count_max = MAX(count_max, bl_count);

    // create the surface
    range->surface = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, allocation.width, allocation.height);
    cairo_t *scr = cairo_create(range->surface);

    // draw background (defined in css)
    gtk_render_background(context, scr, range->alloc_margin.x, range->alloc_margin.y, range->alloc_margin.width,
                          range->alloc_margin.height);
    // draw border (defined in css)
    gtk_render_frame(context, scr, range->alloc_margin.x, range->alloc_margin.y, range->alloc_margin.width,
                     range->alloc_margin.height);

    // draw the rectangles on the surface
    // we have to do some clever things in order to packed together blocks that will be shown at the same place
    // (see above)
    _range_set_source_rgba(scr, range->band_graph, 1.0, state);
    bl_min_px = 0;
    bl_count = 0;
    for(const GList *bl = range->blocks; bl; bl = g_list_next(bl))
    {
      _range_block *blo = bl->data;
      if(blo->txt) continue;
      // are we inside the current block ?
      const int blo_pos_px = _graph_value_to_pos(range, blo->value_r);
      if(blo_pos_px - bl_min_px < bl_width_px)
        bl_count += blo->nb;
      else
      {
        // we draw the previous block
        if(bl_count > 0)
        {
          const int posx_px = (int)(bl_min_px / bl_width_px) * bl_width_px;
          const int bh = _graph_get_height(bl_count, count_max, range->alloc_padding.height);
          cairo_rectangle(scr, posx_px + range->alloc_padding.x,
                          range->alloc_padding.y + range->alloc_padding.height - bh, bl_width_px, bh);
          cairo_fill(scr);
        }
        bl_count = blo->nb;
        bl_min_px = (int)(blo_pos_px / bl_width_px) * bl_width_px;
      }
    }
    // and we draw the last rectangle
    if(bl_count > 0)
    {
      const int posx_px = (int)(bl_min_px / bl_width_px) * bl_width_px;
      const int bh = _graph_get_height(bl_count, count_max, range->alloc_padding.height);
      cairo_rectangle(scr, posx_px + range->alloc_padding.x,
                      range->alloc_padding.y + range->alloc_padding.height - bh, bl_width_px, bh);
      cairo_fill(scr);
    }

    cairo_destroy(scr);
  }
  if(range->surface)
  {
    cairo_set_source_surface(cr, range->surface, 0, 0);
    cairo_paint(cr);
  }

  // draw the selection rectangle
  double sel_min_r = range->select_min_r;
  double sel_max_r
      = (range->set_selection) ? _graph_value_from_pos(range, range->current_x_px, TRUE) : range->select_max_r;
  if(sel_min_r > sel_max_r)
  {
    const double tmp = sel_min_r;
    sel_min_r = sel_max_r;
    sel_max_r = tmp;
  }
  int sel_min_px = (range->bounds & DT_RANGE_BOUND_MIN) ? 0 : _graph_value_to_pos(range, sel_min_r);
  int sel_max_px
      = (range->bounds & DT_RANGE_BOUND_MAX) ? range->alloc_padding.width : _graph_value_to_pos(range, sel_max_r);
  // we need to add the step in order to show that the value is included in the selection
  sel_max_px += range->step_bd / range->band_factor;
  sel_min_px = MAX(sel_min_px, 0);
  sel_max_px = MIN(sel_max_px, range->alloc_padding.width);
  const int sel_width_px = MAX(2, sel_max_px - sel_min_px);
  _range_set_source_rgba(cr, range->band_selection, 1.0, state);
  cairo_rectangle(cr, sel_min_px + range->alloc_padding.x, range->alloc_padding.y, sel_width_px,
                  range->alloc_padding.height);
  cairo_fill(cr);

  double current_value_r = _graph_value_from_pos(range, range->current_x_px, TRUE);

  // draw the markers
  _range_set_source_rgba(cr, range->band_icons, 1.0, state);
  for(const GList *bl = range->markers; bl; bl = g_list_next(bl))
  {
    _range_marker *mark = bl->data;
    const int posx_px = _graph_value_to_pos(range, mark->value_r);
    cairo_rectangle(cr, posx_px + range->alloc_padding.x - 1, range->alloc_padding.y, 2,
                    range->alloc_padding.height * 0.1);
    cairo_fill(cr);
  }

  // draw the icons
  if(g_list_length(range->icons) > 0)
  {
    // we do a first pass to determine the max icon width
    int last = 0;
    int min_percent = 100;
    for(const GList *bl = range->icons; bl; bl = g_list_next(bl))
    {
      _range_icon *icon = bl->data;
      if(last == 0)
        min_percent = MIN(min_percent, icon->posx * 2);
      else
        min_percent = MIN(min_percent, icon->posx - last);
      last = icon->posx;
    }
    min_percent = MIN(min_percent, (100 - last) * 2);
    // we want to let some margin between icons
    min_percent *= 0.9;
    // and we don't want to exceed 60% of the height
    const int size = MIN(range->alloc_padding.height * 0.6, range->alloc_padding.width * min_percent / 100);
    const int posy = range->alloc_padding.y + (range->alloc_padding.height - size) / 2.0;

    for(const GList *bl = range->icons; bl; bl = g_list_next(bl))
    {
      _range_icon *icon = bl->data;
      const int posx_px = range->alloc_padding.width * icon->posx / 100 - size / 2;
      // we set prelight flag if the mouse value correspond
      gint f = icon->flags;
      GtkStateFlags ic_state = GTK_STATE_FLAG_NORMAL;
      if(range->mouse_inside && range->current_x_px > 0 && icon->value_r == current_value_r)
      {
        f |= CPF_PRELIGHT;
        ic_state |= GTK_STATE_FLAG_PRELIGHT;
      }
      else
      {
        f &= ~CPF_PRELIGHT;
      }

      // we set the active flag if the icon value is inside the selection
      if((icon->value_r >= sel_min_r || (range->bounds & DT_RANGE_BOUND_MIN))
         && (icon->value_r <= sel_max_r || (range->bounds & DT_RANGE_BOUND_MAX)))
      {
        f |= CPF_ACTIVE;
        ic_state |= GTK_STATE_FLAG_ACTIVE;
      }
      else
        f &= ~CPF_ACTIVE;

      _range_set_source_rgba(cr, range->band_icons, 1.0, ic_state);
      // and we draw the icon
      icon->paint(cr, posx_px + range->alloc_padding.x, posy, size, size, f, icon->data);
    }
  }

  // draw the current position line
  if(range->mouse_inside && range->current_x_px > 0)
  {
    _range_set_source_rgba(cr, range->band_cursor, 1.0, state);
    const int posx_px = _graph_snap_position(range, range->current_x_px) + range->alloc_padding.x;
    cairo_move_to(cr, posx_px, range->alloc_padding.y);
    cairo_line_to(cr, posx_px, range->alloc_padding.height + range->alloc_padding.y);
    cairo_stroke(cr);
    _current_set_text(range, current_value_r);
  }

  return TRUE;
}

void dtgtk_range_select_redraw(GtkDarktableRangeSelect *range)
{
  // recreate the model for treeview (datetime)
  if(range->type == DT_RANGE_TYPE_DATETIME)
  {
    _popup_date_recreate_model(range);
  }
  // invalidate the surface
  range->alloc_main.width = 0;
  // redraw the band
  gtk_widget_queue_draw(range->band);
}

static gboolean _event_band_motion(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  GtkDarktableRangeSelect *range = (GtkDarktableRangeSelect *)user_data;
  range->current_x_px = event->x - range->alloc_padding.x;

  // if we are outside the graph, don't go further
  const gboolean inside = (range->current_x_px >= 0 && range->current_x_px <= range->alloc_padding.width);
  if(!inside)
  {
    range->mouse_inside = HOVER_OUTSIDE;
    dt_control_change_cursor(GDK_LEFT_PTR);
    _current_hide_popup(range);
    return TRUE;
  }
  _current_show_popup(range);
  // point the popup to the current position
  gint wx, wy;
  gtk_widget_translate_coordinates(range->band, gtk_widget_get_toplevel(range->band), 0, 0, &wx, &wy);
  GdkRectangle rect = { event->x, 0, 1, gtk_widget_get_allocated_height(range->band) };
  gtk_popover_set_pointing_to(GTK_POPOVER(range->cur_window), &rect);

  const double smin_r = (range->bounds & DT_RANGE_BOUND_MIN) ? range->min_r : range->select_min_r;
  const double smax_r = (range->bounds & DT_RANGE_BOUND_MAX) ? range->max_r : range->select_max_r;
  const int smin_px = _graph_value_to_pos(range, smin_r);
  const int smax_px = _graph_value_to_pos(range, smax_r) + range->step_bd / range->band_factor;

  // change the cursor if we are close to an extrema
  if(range->allow_resize
     && !range->set_selection
     && fabs(range->current_x_px - smin_px) <= SNAP_SIZE)
  {
    range->mouse_inside = HOVER_MIN;
    dt_control_change_cursor(GDK_LEFT_SIDE);
  }
  else if(range->allow_resize
          && !range->set_selection
          && fabs(range->current_x_px - smax_px) <= SNAP_SIZE)
  {
    range->mouse_inside = HOVER_MAX;
    dt_control_change_cursor(GDK_RIGHT_SIDE);
  }
  else
  {
    range->mouse_inside = HOVER_INSIDE;
    dt_control_change_cursor(GDK_LEFT_PTR);
  }
  gtk_widget_queue_draw(range->band);
  return TRUE;
}

static gboolean _event_band_leave(GtkWidget *w, GdkEventCrossing *e, gpointer user_data)
{
  GtkDarktableRangeSelect *range = (GtkDarktableRangeSelect *)user_data;
  range->mouse_inside = HOVER_OUTSIDE;
  dt_control_change_cursor(GDK_LEFT_PTR);
  _current_hide_popup(range);

  gtk_widget_queue_draw(range->band);
  return TRUE;
}

static gboolean _event_band_press(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  GtkDarktableRangeSelect *range = (GtkDarktableRangeSelect *)user_data;
  if(e->button == 1 && e->type == GDK_2BUTTON_PRESS)
  {
    dtgtk_range_select_set_selection(range, DT_RANGE_BOUND_MIN | DT_RANGE_BOUND_MAX, range->min_r, range->max_r,
                                     TRUE, TRUE);
  }
  else if(e->button == 1)
  {
    if(!range->mouse_inside) return TRUE;
    const double pos_r = _graph_value_from_pos(range, e->x - range->alloc_padding.x, TRUE);
    if(range->mouse_inside == HOVER_MAX)
    {
      range->bounds &= ~DT_RANGE_BOUND_MAX;
      range->select_max_r = pos_r;
    }
    else if(range->mouse_inside == HOVER_MIN)
    {
      range->bounds &= ~DT_RANGE_BOUND_MIN;
      range->select_min_r = range->select_max_r;
      range->select_max_r = pos_r;
    }
    else if(dt_modifier_is(e->state, GDK_SHIFT_MASK))
    {
      // if we have shift pressed, we only want to set the second bound which is done in release
      range->bounds &= ~DT_RANGE_BOUND_FIXED;
      range->bounds &= ~DT_RANGE_BOUND_MAX;
      range->bounds |= DT_RANGE_BOUND_RANGE;
    }
    else
    {
      range->select_min_r = pos_r;
      range->select_max_r = range->select_min_r;
      range->bounds = DT_RANGE_BOUND_RANGE;
    }
    range->set_selection = TRUE;

    gtk_widget_queue_draw(range->band);
  }
  else if(e->button == 3)
  {
    _popup_show(range, range->band);
  }
  return TRUE;
}
static gboolean _event_band_release(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  GtkDarktableRangeSelect *range = (GtkDarktableRangeSelect *)user_data;
  if(!range->set_selection) return TRUE;
  range->select_max_r = _graph_value_from_pos(range, e->x - range->alloc_padding.x, TRUE);
  const double min_pos_px = _graph_value_to_pos(range, range->select_min_r);

  // we verify that the values are in the right order
  if(range->select_max_r < range->select_min_r)
  {
    const double tmp = range->select_min_r;
    range->select_min_r = range->select_max_r;
    range->select_max_r = tmp;
  }

  // we also set the bounds
  if(fabs(e->x - range->alloc_padding.x - min_pos_px) < 2)
    range->bounds = DT_RANGE_BOUND_FIXED;
  else
  {
    double min_r = range->min_r;
    double max_r = range->max_r;
    if(range->step_bd > 0.0)
    {
      min_r = _graph_value_to_pos(range, min_r);
      min_r = _graph_value_from_pos(range, min_r, FALSE);
      max_r = _graph_value_to_pos(range, max_r);
      max_r = _graph_value_from_pos(range, max_r, FALSE);
    }
    if(range->select_min_r <= min_r) range->bounds |= DT_RANGE_BOUND_MIN;
    if(range->select_max_r >= max_r) range->bounds |= DT_RANGE_BOUND_MAX;
  }

  range->set_selection = FALSE;

  dtgtk_range_select_set_selection(range, range->bounds, range->select_min_r, range->select_max_r, TRUE, FALSE);

  return TRUE;
}

// Public functions
GtkWidget *dtgtk_range_select_new(const gchar *property, const gboolean show_entries, const dt_range_type_t type)
{
  GtkDarktableRangeSelect *range = g_object_new(dtgtk_range_select_get_type(), NULL);

  // initialize values
  range->min_r = 0.0;
  range->max_r = 1.0;
  range->step_bd = 0.0;
  range->select_min_r = 0.1;
  range->select_max_r = 0.9;
  range->bounds = DT_RANGE_BOUND_RANGE;
  range->band_factor = 1.0;
  range->mouse_inside = HOVER_OUTSIDE;
  range->current_x_px = 0.0;
  range->surface = NULL;
  range->value_from_band = _default_value_translator;
  range->value_to_band = _default_value_translator;
  range->print = (type == DT_RANGE_TYPE_NUMERIC) ? _default_print_func : _default_print_date_func;
  range->decode = (type == DT_RANGE_TYPE_NUMERIC) ? _default_decode_func : _default_decode_date_func;
  range->show_entries = show_entries;
  range->type = type;
  range->alloc_main.width = 0;
  range->max_width_px = -1;
  range->cur_help = NULL;
  range->current_bounds = dtgtk_range_select_get_bounds_pretty;
  range->allow_resize = TRUE;

  // the boxes widgets
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  // the graph band
  range->band = gtk_drawing_area_new();
  gtk_widget_set_events(range->band, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_STRUCTURE_MASK
                                         | GDK_LEAVE_NOTIFY_MASK | GDK_POINTER_MOTION_MASK);
  g_signal_connect(G_OBJECT(range->band), "draw", G_CALLBACK(_event_band_draw), range);
  g_signal_connect(G_OBJECT(range->band), "button-press-event", G_CALLBACK(_event_band_press), range);
  g_signal_connect(G_OBJECT(range->band), "button-release-event", G_CALLBACK(_event_band_release), range);
  g_signal_connect(G_OBJECT(range->band), "motion-notify-event", G_CALLBACK(_event_band_motion), range);
  g_signal_connect(G_OBJECT(range->band), "leave-notify-event", G_CALLBACK(_event_band_leave), range);
  g_signal_connect(G_OBJECT(range->band), "style-updated", G_CALLBACK(_dt_pref_changed), range);
  gtk_widget_set_name(GTK_WIDGET(range->band), "dt-range-band");
  gtk_widget_set_can_default(range->band, TRUE);
  gtk_box_pack_start(GTK_BOX(vbox), range->band, TRUE, TRUE, 0);

  // always hidden widgets used to retrieve drawing colors
  range->band_graph = gtk_drawing_area_new();
  gtk_widget_set_name(GTK_WIDGET(range->band_graph), "dt-range-band-graph");
  gtk_widget_set_no_show_all(range->band_graph, TRUE);
  gtk_box_pack_start(GTK_BOX(vbox), range->band_graph, FALSE, FALSE, 0);
  range->band_selection = gtk_drawing_area_new();
  gtk_widget_set_name(GTK_WIDGET(range->band_selection), "dt-range-band-selection");
  gtk_widget_set_no_show_all(range->band_selection, TRUE);
  gtk_box_pack_start(GTK_BOX(vbox), range->band_selection, FALSE, FALSE, 0);
  range->band_icons = gtk_drawing_area_new();
  gtk_widget_set_name(GTK_WIDGET(range->band_icons), "dt-range-band-icons");
  gtk_widget_set_no_show_all(range->band_icons, TRUE);
  gtk_box_pack_start(GTK_BOX(vbox), range->band_icons, FALSE, FALSE, 0);
  range->band_cursor = gtk_drawing_area_new();
  gtk_widget_set_name(GTK_WIDGET(range->band_cursor), "dt-range-band-cursor");
  gtk_widget_set_no_show_all(range->band_cursor, TRUE);
  gtk_box_pack_start(GTK_BOX(vbox), range->band_cursor, FALSE, FALSE, 0);

  if(range->show_entries)
  {
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE, TRUE, 0);

    // the entries
    range->entry_min = gtk_entry_new();
    gtk_widget_set_can_default(range->entry_min, TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(range->entry_min), 0);
    _entry_set_tooltip(range->entry_min, BOUND_MIN, range->type);
    g_signal_connect(G_OBJECT(range->entry_min), "activate", G_CALLBACK(_event_entry_activated), range);
    g_signal_connect(G_OBJECT(range->entry_min), "focus-out-event", G_CALLBACK(_event_entry_focus_out), range);
    g_signal_connect(G_OBJECT(range->entry_min), "button-press-event", G_CALLBACK(_event_entry_press), range);
    gtk_box_pack_start(GTK_BOX(hbox), range->entry_min, TRUE, TRUE, 0);

    range->entry_max = gtk_entry_new();
    gtk_widget_set_can_default(range->entry_max, TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(range->entry_max), 0);
    gtk_entry_set_alignment(GTK_ENTRY(range->entry_max), 1.0);
    _entry_set_tooltip(range->entry_min, BOUND_MAX, range->type);
    g_signal_connect(G_OBJECT(range->entry_max), "activate", G_CALLBACK(_event_entry_activated), range);
    g_signal_connect(G_OBJECT(range->entry_max), "focus-out-event", G_CALLBACK(_event_entry_focus_out), range);
    g_signal_connect(G_OBJECT(range->entry_max), "button-press-event", G_CALLBACK(_event_entry_press), range);
    gtk_box_pack_end(GTK_BOX(hbox), range->entry_max, TRUE, TRUE, 0);
  }

  gtk_container_add(GTK_CONTAINER(range), vbox);
  gtk_widget_set_name(vbox, "range-select");

  if(type == DT_RANGE_TYPE_DATETIME) _popup_date_init(range);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_PREFERENCES_CHANGE, G_CALLBACK(_dt_pref_changed),
                                  range);
  gtk_widget_set_name((GtkWidget *)range, "dt-range");
  
  return (GtkWidget *)range;
}

GType dtgtk_range_select_get_type()
{
  return _range_select_get_type();
}

gchar *dtgtk_range_select_get_bounds_pretty(GtkDarktableRangeSelect *range)
{
  // get nice text for bounds
  if((range->bounds & DT_RANGE_BOUND_MIN) && (range->bounds & DT_RANGE_BOUND_MAX)) return g_strdup(_("all"));

  gchar *txt = NULL;

  if(range->bounds & DT_RANGE_BOUND_MIN)
    txt = g_strdup(_("min"));
  else if(range->bounds & DT_RANGE_BOUND_MIN_RELATIVE)
    txt = g_strdup_printf("-%04d:%02d:%02d %02d:%02d:%02d", range->select_relative_date_r.year,
                          range->select_relative_date_r.month, range->select_relative_date_r.day,
                          range->select_relative_date_r.hour, range->select_relative_date_r.minute,
                          range->select_relative_date_r.second);
  else
    txt = range->print(range->select_min_r, TRUE);

  txt = dt_util_dstrcat(txt, " → ");

  if(range->bounds & DT_RANGE_BOUND_MAX)
    txt = dt_util_dstrcat(txt, _("max"));
  else if(range->bounds & DT_RANGE_BOUND_MAX_RELATIVE)
    txt = dt_util_dstrcat(txt, "+%04d:%02d:%02d %02d:%02d:%02d", range->select_relative_date_r.year,
                          range->select_relative_date_r.month, range->select_relative_date_r.day,
                          range->select_relative_date_r.hour, range->select_relative_date_r.minute,
                          range->select_relative_date_r.second);
  else if(range->bounds & DT_RANGE_BOUND_MAX_NOW)
    txt = dt_util_dstrcat(txt, _("now"));
  else
    txt = dt_util_dstrcat(txt, "%s", range->print(range->select_max_r, TRUE));

  return txt;
}

void dtgtk_range_select_set_selection(GtkDarktableRangeSelect *range, const dt_range_bounds_t bounds,
                                      const double min_r, const double max_r, gboolean signal,
                                      gboolean round_values)
{
  // round the value to respect step if set
  if(round_values && range->step_bd > 0.0)
  {
    range->select_min_r = _graph_value_to_pos(range, min_r);
    range->select_min_r = _graph_value_from_pos(range, range->select_min_r, FALSE);
    range->select_max_r = _graph_value_to_pos(range, max_r);
    range->select_max_r = _graph_value_from_pos(range, range->select_max_r, FALSE);
  }
  else
  {
    // set the values
    range->select_min_r = min_r;
    range->select_max_r = max_r;
  }
  range->bounds = bounds;

  // update the entries
  if(range->show_entries)
  {
    gchar *txt = NULL;
    if(range->bounds & DT_RANGE_BOUND_MIN)
      txt = g_strdup(_("min"));
    else if(range->bounds & DT_RANGE_BOUND_MIN_RELATIVE)
      txt = g_strdup_printf("-%04d:%02d:%02d %02d:%02d:%02d", range->select_relative_date_r.year,
                            range->select_relative_date_r.month, range->select_relative_date_r.day,
                            range->select_relative_date_r.hour, range->select_relative_date_r.minute,
                            range->select_relative_date_r.second);
    else
      txt = range->print(range->select_min_r, FALSE);
    gtk_entry_set_text(GTK_ENTRY(range->entry_min), txt);
    g_free(txt);

    if(range->bounds & DT_RANGE_BOUND_MAX)
      txt = g_strdup(_("max"));
    else if(range->bounds & DT_RANGE_BOUND_MAX_RELATIVE)
      txt = g_strdup_printf("+%04d:%02d:%02d %02d:%02d:%02d", range->select_relative_date_r.year,
                            range->select_relative_date_r.month, range->select_relative_date_r.day,
                            range->select_relative_date_r.hour, range->select_relative_date_r.minute,
                            range->select_relative_date_r.second);
    else if(range->bounds & DT_RANGE_BOUND_MAX_NOW)
      txt = g_strdup(_("now"));
    else
      txt = range->print(range->select_max_r, FALSE);
    gtk_entry_set_text(GTK_ENTRY(range->entry_max), txt);
    g_free(txt);
  }

  // update the band selection
  gtk_widget_queue_draw(range->band);

  // emit the signal if needed
  if(signal) g_signal_emit_by_name(G_OBJECT(range), "value-changed");
}

dt_range_bounds_t dtgtk_range_select_get_selection(GtkDarktableRangeSelect *range, double *min_r, double *max_r)
{
  *min_r = range->select_min_r;
  *max_r = range->select_max_r;
  return range->bounds;
}

void dtgtk_range_select_add_block(GtkDarktableRangeSelect *range, const double value_r, const int count)
{
  _range_block *block = (_range_block *)g_malloc0(sizeof(_range_block));
  block->value_r = value_r;
  block->value2_r = value_r;
  block->bounds = DT_RANGE_BOUND_FIXED;
  block->nb = count;

  range->blocks = g_list_append(range->blocks, block);
}
void dtgtk_range_select_add_range_block(GtkDarktableRangeSelect *range, const double min_r, const double max_r,
                                        const dt_range_bounds_t bounds, gchar *txt, const int count)
{
  _range_block *block = (_range_block *)g_malloc0(sizeof(_range_block));
  block->value_r = min_r;
  block->value2_r = max_r;
  block->bounds = bounds;
  if(txt) block->txt = g_strdup(txt);
  block->nb = count;

  range->blocks = g_list_append(range->blocks, block);
}

void dtgtk_range_select_reset_blocks(GtkDarktableRangeSelect *range)
{
  if(!range->blocks) return;
  g_list_free_full(range->blocks, g_free);
  range->blocks = NULL;
}

void dtgtk_range_select_set_band_func(GtkDarktableRangeSelect *range, DTGTKTranslateValueFunc value_from_band,
                                      DTGTKTranslateValueFunc value_to_band)
{
  if(value_from_band)
    range->value_from_band = value_from_band;
  else
    range->value_from_band = _default_value_translator;

  if(value_to_band)
    range->value_to_band = value_to_band;
  else
    range->value_to_band = _default_value_translator;
}

void dtgtk_range_select_add_icon(GtkDarktableRangeSelect *range, const int posx, const double value_r,
                                 DTGTKCairoPaintIconFunc paint, gint flags, void *data)
{
  _range_icon *icon = (_range_icon *)g_malloc0(sizeof(_range_icon));
  icon->posx = posx;
  icon->value_r = value_r;
  icon->paint = paint;
  icon->flags = flags;
  icon->data = data;

  range->icons = g_list_append(range->icons, icon);
}

void dtgtk_range_select_reset_icons(GtkDarktableRangeSelect *range)
{
  if(!range->icons) return;
  g_list_free_full(range->icons, g_free);
  range->icons = NULL;
}

void dtgtk_range_select_add_marker(GtkDarktableRangeSelect *range, const double value_r, const gboolean magnetic)
{
  _range_marker *mark = (_range_marker *)g_malloc0(sizeof(_range_marker));
  mark->value_r = value_r;
  mark->magnetic = magnetic;

  range->markers = g_list_append(range->markers, mark);
}

void dtgtk_range_select_reset_markers(GtkDarktableRangeSelect *range)
{
  if(!range->markers) return;
  g_list_free_full(range->markers, g_free);
  range->markers = NULL;
}

gchar *dtgtk_range_select_get_raw_text(GtkDarktableRangeSelect *range)
{
  double min, max;
  dt_range_bounds_t bounds = dtgtk_range_select_get_selection(range, &min, &max);

  if((bounds & DT_RANGE_BOUND_MAX) && (bounds & DT_RANGE_BOUND_MIN)) return g_strdup("%");

  gchar *txt_min = range->print(min, FALSE);
  gchar *txt_max = range->print(max, FALSE);

  // special cases for date-time
  if(range->type == DT_RANGE_TYPE_DATETIME)
  {
    if(bounds & DT_RANGE_BOUND_MIN_RELATIVE)
    {
      txt_min = g_strdup_printf("-%04d:%02d:%02d %02d:%02d:%02d", range->select_relative_date_r.year,
                                range->select_relative_date_r.month, range->select_relative_date_r.day,
                                range->select_relative_date_r.hour, range->select_relative_date_r.minute,
                                range->select_relative_date_r.second);
    }
    else if(bounds & DT_RANGE_BOUND_MAX_RELATIVE)
    {
      txt_max = g_strdup_printf("+%04d:%02d:%02d %02d:%02d:%02d", range->select_relative_date_r.year,
                                range->select_relative_date_r.month, range->select_relative_date_r.day,
                                range->select_relative_date_r.hour, range->select_relative_date_r.minute,
                                range->select_relative_date_r.second);
    }
    if(bounds & DT_RANGE_BOUND_MAX_NOW)
    {
      txt_max = g_strdup("now");
    }
  }

  gchar *ret = NULL;

  if(bounds & DT_RANGE_BOUND_MAX)
    ret = g_strdup_printf(">=%s", txt_min);
  else if(bounds & DT_RANGE_BOUND_MIN)
    ret = g_strdup_printf("<=%s", txt_max);
  else if(bounds & DT_RANGE_BOUND_FIXED)
    ret = g_strdup_printf("%s", txt_min);
  else
    ret = g_strdup_printf("[%s;%s]", txt_min, txt_max);

  g_free(txt_min);
  g_free(txt_max);
  return ret;
}

void dtgtk_range_select_set_selection_from_raw_text(GtkDarktableRangeSelect *range, const gchar *txt,
                                                    gboolean signal)
{
  double smin, smax;
  dt_range_bounds_t sbounds;

  gchar *n1 = NULL;
  gchar *n2 = NULL;
  smin = smax = 0;
  sbounds = DT_RANGE_BOUND_RANGE;
  // easy case : select all
  if(!strcmp(txt, "") || !strcmp(txt, "%"))
  {
    sbounds = DT_RANGE_BOUND_MAX | DT_RANGE_BOUND_MIN;
    dtgtk_range_select_set_selection(range, sbounds, smin, smax, signal, FALSE);
    return;
  }
  else if(g_str_has_prefix(txt, "<="))
  {
    sbounds = DT_RANGE_BOUND_MIN;
    n1 = g_strdup(txt + 2);
    n2 = g_strdup(txt + 2);
  }
  else if(g_str_has_prefix(txt, "="))
  {
    sbounds = DT_RANGE_BOUND_FIXED;
    n1 = g_strdup(txt + 1);
    n2 = g_strdup(txt + 1);
  }
  else if(g_str_has_prefix(txt, ">="))
  {
    sbounds = DT_RANGE_BOUND_MAX;
    n1 = g_strdup(txt + 2);
    n2 = g_strdup(txt + 2);
  }
  else
  {
    GRegex *regex;
    GMatchInfo *match_info;

    // we test the range expression first
    // we use a relaxed regex to include float and datetime
    regex = g_regex_new("^\\s*\\[\\s*([-+]?[0-9\\.\\s:]*[0-9]+)\\s*;\\s*((?:now)?[-+]?[0-9\\.\\s:]*)\\s*\\]\\s*$",
                        0, 0, NULL);
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
    sbounds = DT_RANGE_BOUND_FIXED;
    n1 = g_strdup(txt);
    n2 = g_strdup(txt);
  }

  // now we transform the text values into double
  double v1 = 0;
  double v2 = 0;
  if(range->type == DT_RANGE_TYPE_DATETIME)
  {
    // initialize to a more rational value than 01/01/1970
    v1 = v2 = dt_datetime_now_to_gtimespan();
    // if we have relative values at both ends, it's invalid
    if(!g_str_has_prefix(n1, "-") || !g_str_has_prefix(n2, "+"))
    {
      // relative min value
      if(g_str_has_prefix(n1, "-"))
      {
        if(dt_datetime_exif_to_numbers_raw(&range->select_relative_date_r, n1 + 1))
          sbounds = DT_RANGE_BOUND_MIN_RELATIVE;
      }
      else
        range->decode(n1, &v1);

      // special max values
      if(g_str_has_prefix(n2, "+"))
      {
        if(dt_datetime_exif_to_numbers_raw(&range->select_relative_date_r, n2 + 1))
        {
          sbounds = DT_RANGE_BOUND_MAX_RELATIVE;
          v2 = dt_datetime_gtimespan_add_numbers(v1, range->select_relative_date_r, TRUE);
        }
      }
      else if(!g_strcmp0(n2, "now"))
      {
        sbounds |= DT_RANGE_BOUND_MAX_NOW;
        v2 = dt_datetime_now_to_gtimespan();
      }
      else
        range->decode(n2, &v2);

      // last round if min was relative
      if(sbounds & DT_RANGE_BOUND_MIN_RELATIVE)
      {
        v1 = dt_datetime_gtimespan_add_numbers(v2, range->select_relative_date_r, FALSE);
      }
    }
    smin = v1;
    smax = v2;
  }
  else if(range->decode(n1, &v1) && range->decode(n2, &v2))
  {
    smin = fmin(v1, v2);
    smax = fmax(v1, v2);
  }
  g_free(n1);
  g_free(n2);

  dtgtk_range_select_set_selection(range, sbounds, smin, smax, signal, FALSE);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
