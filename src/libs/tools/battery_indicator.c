/*
    This file is part of darktable,
    Copyright (C) 2018-2020 darktable developers.

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

#include "bauhaus/bauhaus.h"
#include "dtgtk/icon.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

typedef struct dt_lib_tool_battery_t
{
  GtkWidget *icon;
  float fill;
} dt_lib_tool_battery_t;

static float _get_fill();
static gboolean _check_fill(gpointer user_data);
static void _paint_battery(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);

const char *name(dt_lib_module_t *self)
{
  return _("battery indicator");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"*", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_CENTER_TOP_RIGHT;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position(const dt_lib_module_t *self)
{
  return 1000;
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_tool_battery_t *d = (dt_lib_tool_battery_t *)g_malloc0(sizeof(dt_lib_tool_battery_t));
  self->data = (void *)d;

  d->fill = _get_fill();

  self->widget = d->icon = dtgtk_icon_new(_paint_battery, 0, d);
  gtk_widget_set_size_request(d->icon, DT_PIXEL_APPLY_DPI(23), -1);
  gtk_widget_set_tooltip_text(d->icon, _("battery indicator"));

  g_timeout_add_seconds(60, _check_fill, d); // TODO: is checking the battery status once per minute fine?
}

void gui_cleanup(dt_lib_module_t *self)
{
  g_free(self->data);
  self->data = NULL;
}

static gboolean _check_fill(gpointer user_data)
{
  dt_lib_tool_battery_t *d = (dt_lib_tool_battery_t *)user_data;

  float fill = _get_fill();

  if(fill != d->fill)
  {
    d->fill = fill;
    gtk_widget_queue_draw(d->icon);
  }

  return TRUE;
}

static float _get_fill()
{
  FILE *fd;
  int energy_now = 1, energy_full = 1, voltage_now = 1;

  fd = g_fopen("/sys/class/power_supply/BAT0/energy_now", "r");
  if(fd)
  {
    fscanf(fd, "%d", &energy_now);
    fclose(fd);
  }

  fd = g_fopen("/sys/class/power_supply/BAT0/energy_full", "r");
  if(fd)
  {
    fscanf(fd, "%d", &energy_full);
    fclose(fd);
  }

  fd = g_fopen("/sys/class/power_supply/BAT0/voltage_now", "r");
  if(fd)
  {
    fscanf(fd, "%d", &voltage_now);
    fclose(fd);
  }

  return ((float)energy_now * 1000 / (float)voltage_now) * 100 / ((float)energy_full * 1000 / (float)voltage_now);
}

static void _paint_battery(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  dt_lib_tool_battery_t *d = (dt_lib_tool_battery_t *)data;

  cairo_translate(cr, x, y);
  cairo_scale(cr, w, h);

  float fill = d->fill;

  if(fill < 20)
    cairo_set_source_rgb(cr, 1, 0, 0);

  cairo_rectangle(cr, 0.05, 0.15, 0.9f*fill/100.0f, 0.7);
  cairo_fill(cr);

  cairo_set_line_width(cr, 0.04);
  cairo_rectangle(cr, 0.01, 0.10, 0.88, 0.8);
  cairo_stroke(cr);
  cairo_rectangle(cr, 0.86, 0.3, 0.14, 0.4);
  cairo_fill(cr);

  PangoLayout *layout;
  PangoRectangle ink;
  // grow is needed because ink.* are int and everything gets rounded to 1 or so otherwise,
  // leading to imprecise positioning
  static const float grow = 10.0;
  PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
  pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
  pango_font_description_set_absolute_size(desc, .48 * grow * PANGO_SCALE);
  layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, desc);
  cairo_scale(cr, 1.0 / grow, 1.0 / grow);

  char text[100];
  snprintf(text, sizeof(text), "%d", (int)roundf(fill));
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, 0.5*grow - ink.x - ink.width / 2.0, 0.5*grow - ink.y - ink.height / 2.0);
  cairo_set_source_rgb(cr, 0, 0, 0);
  pango_cairo_show_layout(cr, layout);
  pango_font_description_free(desc);
  g_object_unref(layout);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
