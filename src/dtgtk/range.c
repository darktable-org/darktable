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
#include "gui/gtk.h"
#include <string.h>

#define SNAP_SIZE 5
#define BAR_WIDTH 4

static void _range_select_class_init(GtkDarktableRangeSelectClass *klass);
static void _range_select_init(GtkDarktableRangeSelect *button);

typedef struct _range_block
{
  double value;      // this is the "real" value
  double band_value; // this the translated value for the band
  int nb; // nb of item with this value

  // this items are only used in case of a predetermined selection
  gchar *txt;
  double value2;
  dt_range_bounds_t bounds;
} _range_block;

typedef struct _range_icon
{
  int posx; // position of the icon in percent of the band width
  double value; // associated value for hover and selected flags (used for drawing icons)
  DTGTKCairoPaintIconFunc paint;
  gint flags;
  void *data;
} _range_icon;

typedef struct _range_marker
{
  double value;
  double band_value;

  gboolean magnetic;
} _range_marker;

enum
{
  VALUE_CHANGED,
  VALUE_RESET,
  LAST_SIGNAL
};
static guint _signals[LAST_SIGNAL] = { 0 };

static void _range_select_class_init(GtkDarktableRangeSelectClass *klass)
{
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
static double _default_decode_func(const gchar *text)
{
  return atof(text);
}

static void _popup_item_activate(GtkWidget *w, gpointer user_data)
{
  GtkDarktableRangeSelect *range = (GtkDarktableRangeSelect *)user_data;
  // retrive block and source values
  GtkWidget *source = GTK_WIDGET(g_object_get_data(G_OBJECT(w), "source_widget"));
  _range_block *blo = (_range_block *)g_object_get_data(G_OBJECT(w), "range_block");

  // if source is the band, apply the value directly
  if(source == range->band)
  {
    dtgtk_range_select_set_selection(range, blo->bounds, blo->value, blo->value2, TRUE);
  }
  else if(source == range->entry_min)
  {
    if(range->bounds & DT_RANGE_BOUND_MIN) range->bounds &= ~DT_RANGE_BOUND_MIN;
    dtgtk_range_select_set_selection(range, range->bounds, blo->value, range->select_max, TRUE);
  }
  else if(source == range->entry_max)
  {
    if(range->bounds & DT_RANGE_BOUND_MAX) range->bounds &= ~DT_RANGE_BOUND_MAX;
    dtgtk_range_select_set_selection(range, range->bounds, range->select_min, blo->value, TRUE);
  }
}

static void _popup_show(GtkDarktableRangeSelect *range, GtkWidget *w)
{
  GtkMenuShell *pop = GTK_MENU_SHELL(gtk_menu_new());
  gtk_widget_set_name(GTK_WIDGET(pop), "range-popup");
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
    if(w == range->entry_min && !(range->bounds & DT_RANGE_BOUND_MAX) && blo->value > range->select_max) continue;
    if(w == range->entry_max && !(range->bounds & DT_RANGE_BOUND_MIN) && blo->value < range->select_min) continue;

    gchar *txt = (blo->txt) ? g_strdup(blo->txt) : range->print(blo->value, TRUE);
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
    if(w == range->entry_min && !(range->bounds & DT_RANGE_BOUND_MAX) && blo->value > range->select_max) continue;
    if(w == range->entry_max && !(range->bounds & DT_RANGE_BOUND_MIN) && blo->value < range->select_min) continue;

    gchar *txt = (blo->txt) ? g_strdup(blo->txt) : range->print(blo->value, TRUE);
    if(blo->nb > 0) txt = dt_util_dstrcat(txt, " (%d)", blo->nb);
    GtkWidget *smt = gtk_menu_item_new_with_label(txt);
    g_free(txt);
    g_object_set_data(G_OBJECT(smt), "range_block", blo);
    g_object_set_data(G_OBJECT(smt), "source_widget", w);
    g_signal_connect(G_OBJECT(smt), "activate", G_CALLBACK(_popup_item_activate), range);

    gtk_menu_shell_append(pop, smt);
  }

  dt_gui_menu_popup(GTK_MENU(pop), NULL, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH);
}

static gboolean _event_entry_press(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  GtkDarktableRangeSelect *range = (GtkDarktableRangeSelect *)user_data;
  if(e->button == 3)
  {
    _popup_show(range, w);
  }
  return TRUE;
}

static void _event_entry_activated(GtkWidget *entry, gpointer user_data)
{
  GtkDarktableRangeSelect *range = (GtkDarktableRangeSelect *)user_data;
  gchar *txt = g_strstrip(g_utf8_strdown(gtk_entry_get_text(GTK_ENTRY(entry)), -1));
  if(range->entry_min == entry && !g_strcmp0(txt, _("min")))
  {
    if(range->bounds & DT_RANGE_BOUND_FIXED)
      range->bounds = DT_RANGE_BOUND_MIN;
    else
      range->bounds |= DT_RANGE_BOUND_MIN;
  }
  else if(range->entry_min == entry && !g_strcmp0(txt, _("max")))
  {
    if(range->bounds & DT_RANGE_BOUND_FIXED)
      range->bounds = DT_RANGE_BOUND_MAX;
    else
      range->bounds |= DT_RANGE_BOUND_MAX;
  }
  else if(range->entry_min == entry)
  {
    if(range->bounds & DT_RANGE_BOUND_MAX) range->bounds = DT_RANGE_BOUND_MAX;
    range->select_min = range->decode(txt);
  }
  else if(range->entry_max == entry)
  {
    if(range->bounds & DT_RANGE_BOUND_MIN) range->bounds = DT_RANGE_BOUND_MIN;
    range->select_max = range->decode(txt);
  }
  g_free(txt);

  dtgtk_range_select_set_selection(range, range->bounds, range->select_min, range->select_max, TRUE);
}

static double _graph_snap_position(GtkDarktableRangeSelect *range, const double posx)
{
  double ret = posx;
  for(const GList *bl = range->markers; bl; bl = g_list_next(bl))
  {
    _range_marker *mark = bl->data;
    if(!mark->magnetic) continue;
    const int mpos = mark->band_value / range->band_factor - range->band_start;
    if(fabs(mpos - posx) < SNAP_SIZE) return mpos;
  }
  return ret;
}
static double _graph_snap_value(GtkDarktableRangeSelect *range, const double posx)
{
  double ret = posx * range->band_factor + range->band_start;
  ret = range->band_value(ret);
  for(const GList *bl = range->markers; bl; bl = g_list_next(bl))
  {
    _range_marker *mark = bl->data;
    if(!mark->magnetic) continue;
    const int mpos = mark->band_value / range->band_factor - range->band_start;
    if(fabs(mpos - posx) < SNAP_SIZE) return mark->value;
  }
  return ret;
}

static int _graph_get_height(const int val, const int max, const int height)
{
  return sqrt(val / (double)max) * (height * 0.8) + height * 0.1;
}

static gboolean _event_band_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  GtkDarktableRangeSelect *range = (GtkDarktableRangeSelect *)user_data;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);

  // retrieve the margins
  GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(range->band));
  GtkStateFlags state = gtk_widget_get_state_flags(range->band);
  GtkBorder bord;
  gtk_style_context_get_border(context, state, &bord);
  const int margin_top = bord.top * allocation.height / 1000;
  const int margin_bottom = bord.bottom * allocation.height / 1000;
  const int bandh = allocation.height - margin_bottom - margin_top;

  // draw the graph (and create it if needed)
  if(!range->surface || range->surf_width != allocation.width)
  {
    int mw = -1;
    gtk_style_context_get(context, state, "min-width", &mw, NULL);
    range->surf_width = allocation.width;
    if(mw > 0)
      range->band_real_width = MIN(allocation.width, mw);
    else
      range->band_real_width = allocation.width;
    range->band_margin_side = (allocation.width - range->band_real_width) / 2;

    // if the surface already exist, destroy it
    if(range->surface) cairo_surface_destroy(range->surface);

    // determine the steps of blocks and extrema values
    range->band_start = range->value_band(range->min);
    const double wv = range->value_band(range->max) - range->band_start;
    range->band_factor = wv / range->band_real_width;
    const double step
        = fmax(range->step, range->band_factor * BAR_WIDTH); // we want at least blocks with width of 2 pixels
    const int bl_width = step / range->band_factor;

    // get the maximum height of blocks
    // we have to do some clever things in order to packed together blocks that wiil be shown at the same place
    // (see step above)
    double bl_min = range->band_start;
    int bl_count = 0;
    int count_max = 0;
    for(const GList *bl = range->blocks; bl; bl = g_list_next(bl))
    {
      _range_block *blo = bl->data;
      if(blo->txt) continue;
      // are we inside the current block ?
      if(blo->band_value - bl_min < step)
        bl_count += blo->nb;
      else
      {
        // we store the previous block count
        count_max = MAX(count_max, bl_count);
        bl_count = blo->nb;
        bl_min = (int)((blo->band_value - range->band_start) / step) * step + range->band_start;
      }
    }
    count_max = MAX(count_max, bl_count);

    // create the surface
    range->surface = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, allocation.width, allocation.height);
    cairo_t *scr = cairo_create(range->surface);

    // draw the markers
    dt_gui_gtk_set_source_rgba(scr, DT_GUI_COLOR_RANGE_ICONS, 1.0);
    for(const GList *bl = range->markers; bl; bl = g_list_next(bl))
    {
      _range_marker *mark = bl->data;
      const int posx = mark->band_value / range->band_factor - range->band_start;
      cairo_rectangle(scr, posx + range->band_margin_side, 0, 2, allocation.height);
      cairo_fill(scr);
    }

    // draw background
    dt_gui_gtk_set_source_rgba(scr, DT_GUI_COLOR_RANGE_BG, 1.0);
    cairo_rectangle(scr, range->band_margin_side, margin_top, range->band_real_width, bandh);
    cairo_fill(scr);

    // draw the rectangles on the surface
    // we have to do some clever things in order to packed together blocks that wiil be shown at the same place
    // (see above)
    dt_gui_gtk_set_source_rgba(scr, DT_GUI_COLOR_RANGE_GRAPH, 1.0);
    bl_min = range->band_start;
    bl_count = 0;
    for(const GList *bl = range->blocks; bl; bl = g_list_next(bl))
    {
      _range_block *blo = bl->data;
      if(blo->txt) continue;
      // are we inside the current block ?
      if(blo->band_value - bl_min < step)
        bl_count += blo->nb;
      else
      {
        // we draw the previous block
        if(bl_count > 0)
        {
          const int posx = (int)((bl_min - range->band_start) / step) * bl_width;
          const int bh = _graph_get_height(bl_count, count_max, bandh);
          cairo_rectangle(scr, posx + range->band_margin_side, margin_top + bandh - bh, bl_width, bh);
          cairo_fill(scr);
        }
        bl_count = blo->nb;
        bl_min = (int)((blo->band_value - range->band_start) / step) * step + range->band_start;
      }
    }
    // and we draw the last rectangle
    if(bl_count > 0)
    {
      const int posx = (int)((bl_min - range->band_start) / step) * bl_width;
      const int bh = _graph_get_height(bl_count, count_max, bandh);
      cairo_rectangle(scr, posx + range->band_margin_side, margin_top + bandh - bh, bl_width, bh);
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
  int sel_start = 0;
  if(!(range->bounds & DT_RANGE_BOUND_MIN))
    sel_start = (range->value_band(range->select_min) - range->band_start) / range->band_factor;
  int sel_end = range->band_real_width;
  if(range->set_selection)
    sel_end = _graph_snap_position(range, range->current_x);
  else if(!(range->bounds & DT_RANGE_BOUND_MAX))
    sel_end = (range->value_band(range->select_max) - range->band_start) / range->band_factor;
  int x1 = (sel_start < sel_end) ? sel_start : sel_end;
  int x2 = (sel_start < sel_end) ? sel_end : sel_start;
  // we need to add the step in order to show that the value is included in the selection
  x2 += range->step / range->band_factor;
  // if we are currently setting the selection, we need to round the value toward steps
  double x1_value = 0.0;
  double x2_value = 0.0;
  if(range->set_selection)
  {
    x1_value = _graph_snap_value(range, x1);
    x2_value = _graph_snap_value(range, x2);
    if(range->step > 0.0)
    {
      x1_value = floor(x1_value / range->step) * range->step;
      x2_value = floor(x2_value / range->step) * range->step;
    }
    x1 = (range->value_band(x1_value) - range->band_start) / range->band_factor;
    x2 = (range->value_band(x2_value) - range->band_start) / range->band_factor;
  }
  x1 = MAX(x1, 0);
  x2 = MIN(x2, range->band_real_width);
  const int sel_width = MAX(2, x2 - x1);
  dt_gui_gtk_set_source_rgba(cr, DT_GUI_COLOR_RANGE_SELECTION, 1.0);
  cairo_rectangle(cr, x1 + range->band_margin_side, margin_top, sel_width, bandh);
  cairo_fill(cr);

  double current_value = _graph_snap_value(range, range->current_x);
  if((range->step > 0.0)) current_value = floor(current_value / range->step) * range->step;

  // draw the icons
  if(g_list_length(range->icons) > 0)
  {
    // determine icon size
    const int size = bandh * 0.6;
    const int posy = margin_top + bandh * 0.2;
    dt_gui_gtk_set_source_rgba(cr, DT_GUI_COLOR_RANGE_ICONS, 1.0);

    for(const GList *bl = range->icons; bl; bl = g_list_next(bl))
    {
      _range_icon *icon = bl->data;
      const int posx = range->band_real_width * icon->posx / 100 - size / 2;
      // we set prelight flag if the mouse value correspond
      gint f = icon->flags;
      if(range->mouse_inside && range->current_x > 0 && icon->value == current_value)
        f |= CPF_PRELIGHT;
      else
        f &= ~CPF_PRELIGHT;

      // we set the active flag if the icon value is inside the selection
      if(!range->set_selection && (icon->value >= range->select_min || (range->bounds & DT_RANGE_BOUND_MIN))
         && (icon->value <= range->select_max || (range->bounds & DT_RANGE_BOUND_MAX)))
        f |= CPF_ACTIVE;
      else if(range->set_selection && (icon->value >= x1_value || (range->bounds & DT_RANGE_BOUND_MIN))
              && (icon->value < x2_value || (range->bounds & DT_RANGE_BOUND_MAX)))
        f |= CPF_ACTIVE;
      else
        f &= ~CPF_ACTIVE;
      // and we draw the icon
      icon->paint(cr, posx + range->band_margin_side, posy, size, size, f, icon->data);
    }
  }

  // draw the current position line
  if(range->mouse_inside && range->current_x > 0)
  {
    dt_gui_gtk_set_source_rgba(cr, DT_GUI_COLOR_RANGE_CURSOR, 1.0);
    const int posx = _graph_snap_position(range, range->current_x) + range->band_margin_side;
    cairo_move_to(cr, posx, margin_top);
    cairo_line_to(cr, posx, bandh);
    cairo_stroke(cr);
    if(range->show_entries)
    {
      gchar *txt = range->print(current_value, TRUE);
      gtk_label_set_text(GTK_LABEL(range->current), txt);
      g_free(txt);
      gtk_widget_set_visible(range->current, TRUE);
    }
  }
  else if(range->show_entries)
  {
    gtk_widget_set_visible(range->current, FALSE);
  }

  return TRUE;
}

static void _dt_pref_changed(gpointer instance, gpointer user_data)
{
  if(!user_data) return;
  GtkDarktableRangeSelect *range = (GtkDarktableRangeSelect *)user_data;
  // invalidate the surface
  range->surf_width = 0;
  // redraw the band
  gtk_widget_queue_draw(range->band);
}

static gboolean _event_band_motion(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  GtkDarktableRangeSelect *range = (GtkDarktableRangeSelect *)user_data;
  range->current_x = event->x - range->band_margin_side;
  range->mouse_inside = (range->current_x >= 0 && range->current_x <= range->band_real_width);

  gtk_widget_queue_draw(range->band);
  return TRUE;
}

static gboolean _event_band_leave(GtkWidget *w, GdkEventCrossing *e, gpointer user_data)
{
  GtkDarktableRangeSelect *range = (GtkDarktableRangeSelect *)user_data;
  range->mouse_inside = FALSE;

  gtk_widget_queue_draw(range->band);
  return TRUE;
}

static gboolean _event_band_press(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  GtkDarktableRangeSelect *range = (GtkDarktableRangeSelect *)user_data;
  if(e->button == 1 && e->type == GDK_2BUTTON_PRESS)
  {
    dtgtk_range_select_set_selection(range, DT_RANGE_BOUND_MIN | DT_RANGE_BOUND_MAX, range->min, range->max, TRUE);
  }
  else if(e->button == 1)
  {
    if(!range->mouse_inside) return TRUE;
    range->select_min = _graph_snap_value(range, e->x - range->band_margin_side);
    range->select_max = range->select_min;
    range->bounds = DT_RANGE_BOUND_RANGE;
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
  range->select_max = _graph_snap_value(range, e->x - range->band_margin_side);
  // we verify that the values are in the right order
  if(range->select_max < range->select_min)
  {
    const double tmp = range->select_min;
    range->select_min = range->select_max;
    range->select_max = tmp;
  }
  // we also set the bounds
  if(range->select_max - range->select_min < 0.001)
    range->bounds = DT_RANGE_BOUND_FIXED;
  else
  {
    if(range->select_min <= range->min) range->bounds |= DT_RANGE_BOUND_MIN;
    if(range->select_max >= range->max) range->bounds |= DT_RANGE_BOUND_MAX;
  }

  range->set_selection = FALSE;

  dtgtk_range_select_set_selection(range, range->bounds, range->select_min, range->select_max, TRUE);

  return TRUE;
}

// Public functions
GtkWidget *dtgtk_range_select_new(const gchar *property, gboolean show_entries)
{
  GtkDarktableRangeSelect *range = g_object_new(dtgtk_range_select_get_type(), NULL);
  GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(range));
  gtk_style_context_add_class(context, "dt_range_select");
  gchar *cl = g_strdup_printf("dt_range_%s", property);
  gtk_style_context_add_class(context, cl);
  g_free(cl);

  // initialize values
  range->min = 0.0;
  range->max = 1.0;
  range->step = 0.0;
  range->select_min = 0.1;
  range->select_max = 0.9;
  range->bounds = DT_RANGE_BOUND_RANGE;
  range->mouse_inside = FALSE;
  range->current_x = 0.0;
  range->surface = NULL;
  range->surf_width = 0;
  range->band_value = _default_value_translator;
  range->value_band = _default_value_translator;
  range->print = _default_print_func;
  range->decode = _default_decode_func;
  range->show_entries = show_entries;

  // the boxes widgets
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  // the graph band
  range->band = gtk_drawing_area_new();
  gtk_widget_set_events(range->band, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_STRUCTURE_MASK
                                         | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK
                                         | GDK_POINTER_MOTION_MASK);
  g_signal_connect(G_OBJECT(range->band), "draw", G_CALLBACK(_event_band_draw), range);
  g_signal_connect(G_OBJECT(range->band), "button-press-event", G_CALLBACK(_event_band_press), range);
  g_signal_connect(G_OBJECT(range->band), "button-release-event", G_CALLBACK(_event_band_release), range);
  g_signal_connect(G_OBJECT(range->band), "motion-notify-event", G_CALLBACK(_event_band_motion), range);
  g_signal_connect(G_OBJECT(range->band), "leave-notify-event", G_CALLBACK(_event_band_leave), range);
  g_signal_connect(G_OBJECT(range->band), "style-updated", G_CALLBACK(_dt_pref_changed), range);
  gtk_widget_set_name(GTK_WIDGET(range->band), "dt-range-band");
  context = gtk_widget_get_style_context(GTK_WIDGET(range->band));
  GtkStateFlags state = gtk_widget_get_state_flags(range->band);
  int mh = -1;
  gtk_style_context_get(context, state, "min-height", &mh, NULL);
  gtk_widget_set_size_request(range->band, -1, mh);
  gtk_box_pack_start(GTK_BOX(vbox), range->band, TRUE, TRUE, 0);

  if(range->show_entries)
  {
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE, TRUE, 0);

    // the entries
    range->entry_min = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(range->entry_min), 5);
    g_signal_connect(G_OBJECT(range->entry_min), "activate", G_CALLBACK(_event_entry_activated), range);
    g_signal_connect(G_OBJECT(range->entry_min), "button-press-event", G_CALLBACK(_event_entry_press), range);
    gtk_box_pack_start(GTK_BOX(hbox), range->entry_min, FALSE, TRUE, 0);

    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    range->current = gtk_label_new("test");
    gtk_widget_set_name(range->current, "dt-range-current");
    gtk_widget_set_halign(range->current, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(range->current, GTK_ALIGN_CENTER);
    gtk_widget_set_no_show_all(range->current, TRUE);
    gtk_box_pack_start(GTK_BOX(hb), range->current, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), hb, TRUE, TRUE, 0);

    range->entry_max = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(range->entry_max), 5);
    gtk_entry_set_alignment(GTK_ENTRY(range->entry_max), 1.0);
    g_signal_connect(G_OBJECT(range->entry_max), "activate", G_CALLBACK(_event_entry_activated), range);
    g_signal_connect(G_OBJECT(range->entry_max), "button-press-event", G_CALLBACK(_event_entry_press), range);
    gtk_box_pack_end(GTK_BOX(hbox), range->entry_max, FALSE, TRUE, 0);
  }

  gtk_container_add(GTK_CONTAINER(range), vbox);
  gtk_widget_set_name(GTK_WIDGET(range), "range_select");

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_PREFERENCES_CHANGE, G_CALLBACK(_dt_pref_changed),
                                  range);

  return (GtkWidget *)range;
}

GType dtgtk_range_select_get_type()
{
  static GType dtgtk_range_select_type = 0;
  if(!dtgtk_range_select_type)
  {
    static const GTypeInfo dtgtk_range_select_info = {
      sizeof(GtkDarktableRangeSelectClass),
      (GBaseInitFunc)NULL,
      (GBaseFinalizeFunc)NULL,
      (GClassInitFunc)_range_select_class_init,
      NULL, /* class_finalize */
      NULL, /* class_data */
      sizeof(GtkDarktableRangeSelect),
      0, /* n_preallocs */
      (GInstanceInitFunc)_range_select_init,
    };
    dtgtk_range_select_type
        = g_type_register_static(GTK_TYPE_BIN, "GtkDarktableRangeSelect", &dtgtk_range_select_info, 0);
  }
  return dtgtk_range_select_type;
}

void dtgtk_range_select_set_selection(GtkDarktableRangeSelect *range, const dt_range_bounds_t bounds,
                                      const double min, const double max, gboolean signal)
{
  // round the value to respect step if set
  if(range->step > 0.0)
  {
    range->select_min = floor(min / range->step) * range->step;
    range->select_max = floor(max / range->step) * range->step;
  }
  else
  {
    // set the values
    range->select_min = min;
    range->select_max = max;
  }
  range->bounds = bounds;

  // update the entries
  if(range->show_entries)
  {
    gchar *txt = NULL;
    if(range->bounds & DT_RANGE_BOUND_MIN)
      txt = g_strdup(_("min"));
    else
      txt = range->print(range->select_min, FALSE);
    gtk_entry_set_text(GTK_ENTRY(range->entry_min), txt);
    g_free(txt);

    if(range->bounds & DT_RANGE_BOUND_MAX)
      txt = g_strdup(_("max"));
    else
      txt = range->print(range->select_max, FALSE);
    gtk_entry_set_text(GTK_ENTRY(range->entry_max), txt);
    g_free(txt);
  }

  // update the band selection
  gtk_widget_queue_draw(range->band);

  // emit the signal if needed
  if(signal) g_signal_emit_by_name(G_OBJECT(range), "value-changed");
}

dt_range_bounds_t dtgtk_range_select_get_selection(GtkDarktableRangeSelect *range, double *min, double *max)
{
  *min = range->select_min;
  *max = range->select_max;
  return range->bounds;
}

void dtgtk_range_select_add_block(GtkDarktableRangeSelect *range, const double value, const int count)
{
  _range_block *block = (_range_block *)g_malloc0(sizeof(_range_block));
  block->value = value;
  block->value2 = value;
  block->bounds = DT_RANGE_BOUND_FIXED;
  block->nb = count;
  block->band_value = range->value_band(value);

  range->blocks = g_list_append(range->blocks, block);
}
void dtgtk_range_select_add_range_block(GtkDarktableRangeSelect *range, const double min, const double max,
                                        const dt_range_bounds_t bounds, gchar *txt, const int count)
{
  _range_block *block = (_range_block *)g_malloc0(sizeof(_range_block));
  block->value = min;
  block->value2 = max;
  block->bounds = bounds;
  if(txt) block->txt = g_strdup(txt);
  block->nb = count;
  block->band_value = range->value_band(min);

  range->blocks = g_list_append(range->blocks, block);
}

void dtgtk_range_select_reset_blocks(GtkDarktableRangeSelect *range)
{
  if(!range->blocks) return;
  g_list_free_full(range->blocks, g_free);
  range->blocks = NULL;
}

void dtgtk_range_select_set_band_func(GtkDarktableRangeSelect *range, DTGTKTranslateValueFunc band_value,
                                      DTGTKTranslateValueFunc value_band)
{
  if(band_value)
    range->band_value = band_value;
  else
    range->band_value = _default_value_translator;

  if(value_band)
    range->value_band = value_band;
  else
    range->value_band = _default_value_translator;
}

void dtgtk_range_select_add_icon(GtkDarktableRangeSelect *range, const int posx, const double value,
                                 DTGTKCairoPaintIconFunc paint, gint flags, void *data)
{
  _range_icon *icon = (_range_icon *)g_malloc0(sizeof(_range_icon));
  icon->posx = posx;
  icon->value = value;
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

void dtgtk_range_select_add_marker(GtkDarktableRangeSelect *range, const double value, const gboolean magnetic)
{
  _range_marker *mark = (_range_marker *)g_malloc0(sizeof(_range_marker));
  mark->value = value;
  mark->magnetic = magnetic;
  mark->band_value = range->value_band(value);

  range->markers = g_list_append(range->markers, mark);
}

void dtgtk_range_select_reset_markers(GtkDarktableRangeSelect *range)
{
  if(!range->markers) return;
  g_list_free_full(range->markers, g_free);
  range->markers = NULL;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;