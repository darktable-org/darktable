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

// Public functions
GtkWidget *dtgtk_range_select_new()
{
  GtkDarktableRangeSelect *range = g_object_new(dtgtk_range_select_get_type(), NULL);
  GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(range));
  gtk_style_context_add_class(context, "dt_range_select");
  range->min = 0.0;
  range->max = 0.0;
  range->step = 1.0;
  snprintf(range->formater, sizeof(range->formater), "%%.2lf");
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE, TRUE, 0);
  range->entry_min = gtk_entry_new();
  gtk_entry_set_width_chars(GTK_ENTRY(range->entry_min), 5);
  gtk_box_pack_start(GTK_BOX(hbox), range->entry_min, FALSE, TRUE, 0);
  range->entry_max = gtk_entry_new();
  gtk_entry_set_width_chars(GTK_ENTRY(range->entry_max), 5);
  gtk_box_pack_end(GTK_BOX(hbox), range->entry_max, FALSE, TRUE, 0);
  range->band = gtk_drawing_area_new();
  gtk_widget_set_size_request(range->band, -1, 20); // TODO : make the height changeable with css
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

void dtgtk_range_select_set_range(GtkDarktableRangeSelect *range, const dt_range_bounds_t bounds, const double min,
                                  const double max, gboolean signal)
{
  // set the values
  range->min = min;
  range->max = max;
  range->bounds = bounds;

  // update the entries
  char txt[16] = { 0 };
  snprintf(txt, sizeof(txt), range->formater, min);
  gtk_entry_set_text(GTK_ENTRY(range->entry_min), txt);
  snprintf(txt, sizeof(txt), range->formater, max);
  gtk_entry_set_text(GTK_ENTRY(range->entry_max), txt);

  // update the band selection

  // emit the signal if needed
  if(signal) g_signal_emit_by_name(G_OBJECT(range), "value-changed");
}

dt_range_bounds_t dtgtk_range_select_get_range(GtkDarktableRangeSelect *range, double *min, double *max)
{
  *min = range->min;
  *max = range->max;
  return range->bounds;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;