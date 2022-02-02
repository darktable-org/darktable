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

static void _range_select_class_init(GtkDarktableRangeSelectClass *klass);
static void _range_select_init(GtkDarktableRangeSelect *button);

typedef struct _range_block
{
  double value;      // this is the "real" value
  double band_value; // this the translated value for the band

  int nb; // nb of item with this value
} _range_block;

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

static double _value_translater_default(const double value)
{
  return value;
}

static gboolean _event_band_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  GtkDarktableRangeSelect *range = (GtkDarktableRangeSelect *)user_data;

  // get context and fg color
  GtkStateFlags state = gtk_widget_get_state_flags(widget);
  GdkRGBA fg_color;
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  gtk_style_context_get_color(context, state, &fg_color);

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);

  // draw background
  gtk_render_background(context, cr, 0, 0, allocation.width, allocation.height);

  // draw the graph (and create it if needed)
  if(!range->surface)
  {
    // determine the steps of blocks and extrema values
    range->band_start = range->value_band(range->min);
    const double wv = range->value_band(range->max) - range->band_start;
    range->band_factor = wv / (allocation.width - 8);
    const double step = range->band_factor * 2.0; // we want at least blocks with width of 2 pixels
    range->band_start -= step;

    // get the maximum height of blocks
    // we have to do some clever things in order to packed together blocks that wiil be shown at the same place
    // (see step above)
    double bl_min = range->band_start;
    int bl_count = 0;
    int count_max = 0;
    for(const GList *bl = range->blocks; bl; bl = g_list_next(bl))
    {
      _range_block *blo = bl->data;
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
    cairo_set_source_rgba(scr, fg_color.red, fg_color.green, fg_color.blue, fg_color.alpha);

    // draw the rectangles on the surface
    // we have to do some clever things in order to packed together blocks that wiil be shown at the same place
    // (see above)
    bl_min = range->band_start;
    bl_count = 0;
    for(const GList *bl = range->blocks; bl; bl = g_list_next(bl))
    {
      _range_block *blo = bl->data;
      // are we inside the current block ?
      if(blo->band_value - bl_min < step)
        bl_count += blo->nb;
      else
      {
        // we draw the previous block
        if(bl_count > 0)
        {
          const int posx = (int)((bl_min - range->band_start) / step) * 2 + 2;
          const int bh = sqrt(bl_count / (double)count_max) * (allocation.height - 2) + 2;
          cairo_rectangle(scr, posx, allocation.height - bh, 2, bh);
          cairo_fill(scr);
        }
        bl_count = blo->nb;
        bl_min = (int)((blo->band_value - range->band_start) / step) * step + range->band_start;
      }
    }
    // and we draw the last rectangle
    if(bl_count > 0)
    {
      const int posx = (int)((bl_min - range->band_start) / step) * 2 + 2;
      const int bh = sqrt(bl_count / (double)count_max) * (allocation.height - 2) + 2;
      cairo_rectangle(scr, posx, allocation.height - bh, 2, bh);
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
  int sel_end = allocation.width;
  if(range->set_selection)
    sel_end = range->current_x;
  else if(!(range->bounds & DT_RANGE_BOUND_MAX))
    sel_end = (range->value_band(range->select_max) - range->band_start) / range->band_factor;
  const int x1 = (sel_start < sel_end) ? sel_start : sel_end;
  const int x2 = (sel_start < sel_end) ? sel_end : sel_start;
  const int sel_width = MAX(2, x2 - x1);
  cairo_set_source_rgba(cr, fg_color.red, fg_color.green, fg_color.blue, fg_color.alpha * 0.7);
  cairo_rectangle(cr, x1, 0, sel_width, allocation.height);
  cairo_fill(cr);

  // draw the current position line
  if(range->mouse_inside && range->current_x > 0)
  {
    dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_TIMELINE_TEXT_BG);
    cairo_move_to(cr, range->current_x, 0);
    cairo_line_to(cr, range->current_x, allocation.height);
    cairo_stroke(cr);
    char txt[16] = { 0 };
    const double val = range->current_x * range->band_factor + range->band_start;
    snprintf(txt, sizeof(txt), range->formater, range->band_value(val));
    gtk_widget_set_tooltip_text(range->band, txt);
  }

  return TRUE;
}

static gboolean _event_band_motion(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  GtkDarktableRangeSelect *range = (GtkDarktableRangeSelect *)user_data;
  range->mouse_inside = TRUE;
  range->current_x = event->x;

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
  const double val = e->x * range->band_factor + range->band_start;
  range->select_min = range->band_value(val);
  range->select_max = range->select_min;
  range->bounds = DT_RANGE_BOUND_RANGE;
  range->set_selection = TRUE;

  gtk_widget_queue_draw(range->band);
  return TRUE;
}
static gboolean _event_band_release(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  GtkDarktableRangeSelect *range = (GtkDarktableRangeSelect *)user_data;
  const double val = e->x * range->band_factor + range->band_start;
  range->select_max = range->band_value(val);
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
GtkWidget *dtgtk_range_select_new()
{
  GtkDarktableRangeSelect *range = g_object_new(dtgtk_range_select_get_type(), NULL);
  GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(range));
  gtk_style_context_add_class(context, "dt_range_select");

  // initialize values
  range->min = 0.0;
  range->max = 1.0;
  range->step = 0.1;
  range->select_min = 0.1;
  range->select_max = 0.9;
  range->bounds = DT_RANGE_BOUND_RANGE;
  range->mouse_inside = FALSE;
  range->current_x = 0.0;
  snprintf(range->formater, sizeof(range->formater), "%%.2lf");
  range->surface = NULL;
  range->band_value = _value_translater_default;
  range->value_band = _value_translater_default;

  // the boxes widgets
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE, TRUE, 0);

  // the entries
  range->entry_min = gtk_entry_new();
  gtk_entry_set_width_chars(GTK_ENTRY(range->entry_min), 5);
  gtk_box_pack_start(GTK_BOX(hbox), range->entry_min, FALSE, TRUE, 0);
  range->entry_max = gtk_entry_new();
  gtk_entry_set_width_chars(GTK_ENTRY(range->entry_max), 5);
  gtk_box_pack_end(GTK_BOX(hbox), range->entry_max, FALSE, TRUE, 0);

  // the bottom band
  range->band = gtk_drawing_area_new();
  gtk_widget_set_size_request(range->band, -1, 30); // TODO : make the height changeable with css
  gtk_widget_set_events(range->band, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_STRUCTURE_MASK
                                         | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK
                                         | GDK_POINTER_MOTION_MASK);
  g_signal_connect(G_OBJECT(range->band), "draw", G_CALLBACK(_event_band_draw), range);
  g_signal_connect(G_OBJECT(range->band), "button-press-event", G_CALLBACK(_event_band_press), range);
  g_signal_connect(G_OBJECT(range->band), "button-release-event", G_CALLBACK(_event_band_release), range);
  g_signal_connect(G_OBJECT(range->band), "motion-notify-event", G_CALLBACK(_event_band_motion), range);
  g_signal_connect(G_OBJECT(range->band), "leave-notify-event", G_CALLBACK(_event_band_leave), range);
  gtk_widget_set_name(GTK_WIDGET(range->band), "range_select_band");
  gtk_box_pack_start(GTK_BOX(vbox), range->band, TRUE, TRUE, 0);

  gtk_container_add(GTK_CONTAINER(range), vbox);
  gtk_widget_set_name(GTK_WIDGET(range), "range_select");
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
  // set the values
  range->select_min = min;
  range->select_max = max;
  range->bounds = bounds;

  // update the entries
  char txt[16] = { 0 };
  if(range->bounds & DT_RANGE_BOUND_MIN)
    snprintf(txt, sizeof(txt), "%s", _("min"));
  else
    snprintf(txt, sizeof(txt), range->formater, min);
  gtk_entry_set_text(GTK_ENTRY(range->entry_min), txt);

  if(range->bounds & DT_RANGE_BOUND_MAX)
    snprintf(txt, sizeof(txt), "%s", _("max"));
  else
    snprintf(txt, sizeof(txt), range->formater, max);
  gtk_entry_set_text(GTK_ENTRY(range->entry_max), txt);

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
  block->nb = count;
  block->band_value = range->value_band(value);

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
    range->band_value = _value_translater_default;

  if(value_band)
    range->value_band = value_band;
  else
    range->value_band = _value_translater_default;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;