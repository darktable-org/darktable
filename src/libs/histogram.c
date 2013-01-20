/*
    This file is part of darktable,
    copyright (c) 2011 Henrik Andersson.

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

#include "common/darktable.h"
#include "common/debug.h"
#include "control/control.h"
#include "control/conf.h"
#include "common/image_cache.h"
#include "develop/develop.h"
#include "libs/lib.h"
#include "gui/gtk.h"
#include "gui/draw.h"

DT_MODULE(1)

typedef struct dt_lib_histogram_t
{
  float white, black;
  int32_t dragging;
  int32_t button_down_x, button_down_y;
  int32_t highlight;
  gboolean expand;
  gboolean red, green, blue;
  float expand_x, expand_w, mode_x, mode_w, red_x, green_x, blue_x;
  float color_w, button_h, button_y, button_spacing;
}
dt_lib_histogram_t;

static gboolean _lib_histogram_expose_callback(GtkWidget *widget, GdkEventExpose *event, gpointer user_data);
static gboolean _lib_histogram_motion_notify_callback(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);
static gboolean _lib_histogram_button_press_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean _lib_histogram_button_release_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean _lib_histogram_scroll_callback(GtkWidget *widget, GdkEventScroll *event, gpointer user_data);
static gboolean _lib_histogram_enter_notify_callback(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);
static gboolean _lib_histogram_leave_notify_callback(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);

const char* name()
{
  return _("histogram");
}

uint32_t views()
{
  return DT_VIEW_DARKROOM | DT_VIEW_TETHERING;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_RIGHT_TOP;
}

int expandable()
{
  return 0;
}

int position()
{
  return 1001;
}


static void _lib_histogram_change_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_control_queue_redraw_widget(self->widget);
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)g_malloc(sizeof(dt_lib_histogram_t));
  memset(d,0,sizeof(dt_lib_histogram_t));
  self->data = (void *)d;

  d->expand = dt_conf_get_bool("plugins/darkroom/histogram/expand");

  d->red = dt_conf_get_bool("plugins/darkroom/histogram/show_red");
  d->green = dt_conf_get_bool("plugins/darkroom/histogram/show_green");
  d->blue = dt_conf_get_bool("plugins/darkroom/histogram/show_blue");

  /* create drawingarea */
  self->widget = gtk_drawing_area_new();

  gtk_widget_add_events(self->widget,
                        GDK_LEAVE_NOTIFY_MASK |
                        GDK_ENTER_NOTIFY_MASK |
                        GDK_POINTER_MOTION_MASK |
                        GDK_BUTTON_PRESS_MASK |
                        GDK_BUTTON_RELEASE_MASK |
                        GDK_SCROLL);

  /* connect callbacks */
  g_object_set(G_OBJECT(self->widget), "tooltip-text",
               _("drag to change exposure,\ndoubleclick resets"), (char *)NULL);
  g_signal_connect (G_OBJECT (self->widget), "expose-event",
                    G_CALLBACK (_lib_histogram_expose_callback), self);
  g_signal_connect (G_OBJECT (self->widget), "button-press-event",
                    G_CALLBACK (_lib_histogram_button_press_callback), self);
  g_signal_connect (G_OBJECT (self->widget), "button-release-event",
                    G_CALLBACK (_lib_histogram_button_release_callback), self);
  g_signal_connect (G_OBJECT (self->widget), "motion-notify-event",
                    G_CALLBACK (_lib_histogram_motion_notify_callback), self);
  g_signal_connect (G_OBJECT (self->widget), "leave-notify-event",
                    G_CALLBACK (_lib_histogram_leave_notify_callback), self);
  g_signal_connect (G_OBJECT (self->widget), "enter-notify-event",
                    G_CALLBACK (_lib_histogram_enter_notify_callback), self);
  g_signal_connect (G_OBJECT (self->widget), "scroll-event",
                    G_CALLBACK (_lib_histogram_scroll_callback), self);

  /* set size of navigation draw area */
  int panel_width = dt_conf_get_int("panel_width");
  gtk_widget_set_size_request(self->widget, -1, d->expand?4*panel_width*.5:panel_width*.5);

  /* connect to preview pipe finished  signal */
  dt_control_signal_connect(darktable.signals,DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED, G_CALLBACK(_lib_histogram_change_callback), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  /* disconnect callback from  signal */
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_lib_histogram_change_callback), self);

  g_free(self->data);
  self->data = NULL;
}

static void _draw_color_toggle(cairo_t *cr, float x, float y, float width, float height, gboolean state)
{
  float border = MIN(width*.1, height*.1);
  cairo_rectangle(cr, x+border, y+border, width-2.0*border, height-2.0*border);
  cairo_fill_preserve(cr);
  if(state)
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.5);
  else
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.5);
  cairo_set_line_width(cr, border);
  cairo_stroke(cr);
}

static void _draw_mode_toggle(cairo_t *cr, float x, float y, float width, float height, gboolean linear)
{
  float border = MIN(width*.1, height*.1);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.4);
  cairo_rectangle(cr, x+border, y+border, width-2.0*border, height-2.0*border);
  cairo_fill_preserve(cr);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.5);
  cairo_set_line_width(cr, border);
  cairo_stroke(cr);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.5);
  cairo_move_to(cr, x+2.0*border, y+height-2.0*border);
  if(linear)
    cairo_line_to(cr, x+width-2.0*border, y+2.0*border);
  else
    cairo_curve_to(cr, x+2.0*border, y+.33*height, x+0.66*width, y+2.0*border, x+width-2.0*border, y+2.0*border);
  cairo_stroke(cr);
}

static void _draw_expand_toggle(cairo_t *cr, float x, float y, float width, float height, gboolean state)
{
  float border = MIN(width*.1, height*.1);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.4);
  cairo_rectangle(cr, x+border, y+border, width-2.0*border, height-2.0*border);
  cairo_fill_preserve(cr);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.5);
  cairo_set_line_width(cr, border);
  cairo_stroke(cr);

  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.5);

  cairo_move_to(cr, x + (width / 2.0), y + height - 2.0 * border);
  cairo_line_to(cr, x + (width / 2.0), y + 2.0 * border);

  if(state)
  {
    cairo_move_to(cr, x + 2 * border, y + (height / 2.0));
    cairo_line_to(cr, x + (width / 2.0), y + 2.0 * border);

    cairo_move_to(cr, x + width - 2 * border, y + (height / 2.0));
    cairo_line_to(cr, x + (width / 2.0), y + 2.0 * border);
  }
  else
  {
    cairo_move_to(cr, x + 2 * border, y + (height / 2.0));
    cairo_line_to(cr, x + (width / 2.0), y + height - 2.0 * border);

    cairo_move_to(cr, x + width - 2 * border, y + (height / 2.0));
    cairo_line_to(cr, x + (width / 2.0), y + + height - 2.0 * border);
  }

  cairo_stroke(cr);
}

static void _draw_histogram_box(cairo_t *cr, int x, int y, int width, int height)
{
  cairo_save(cr);
  cairo_set_line_width(cr, 1.0);

  cairo_rectangle(cr, x, y, width, height);
  cairo_clip(cr);

  cairo_set_source_rgb (cr, .2, .2, .2);
  cairo_rectangle(cr, x, y, width, height);
  cairo_fill(cr);
  cairo_restore(cr);
}

static void _draw_histogram_grid(cairo_t *cr, int left, int top, int right, int bottom)
{
  cairo_save(cr);
  cairo_set_line_width(cr, .4);
  cairo_set_source_rgb (cr, .1, .1, .1);
  dt_draw_grid(cr, 4, left, top, right, bottom);
  cairo_restore(cr);
}

static gboolean _lib_histogram_expose_callback(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;

  dt_develop_t *dev = darktable.develop;
  float *hist = dev->histogram;
  float hist_max = dev->histogram_linear?dev->histogram_max:logf(1.0 + dev->histogram_max);

  /* set size of navigation draw area */
  int panel_width = dt_conf_get_int("panel_width");
  gtk_widget_set_size_request(widget, -1, d->expand?4*panel_width*.5:panel_width*.5);

  int width = widget->allocation.width, height = d->expand?widget->allocation.height/4:widget->allocation.height;

  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, widget->allocation.height);
  cairo_t *cr = cairo_create(cst);
  GtkStyle *style=gtk_rc_get_style_by_paths(gtk_settings_get_default(), NULL,"GtkWidget", GTK_TYPE_WIDGET);
  if(!style) style = gtk_rc_get_style(widget);
  cairo_set_source_rgb(cr, style->bg[0].red/65535.0, style->bg[0].green/65535.0, style->bg[0].blue/65535.0);
  cairo_paint(cr);

  if(d->mode_x == 0)
  {
    d->color_w = 0.06*width;
    d->button_spacing = 0.01*width;
    d->button_h = 0.06*width;
    d->button_y = d->button_spacing;
    d->expand_w = d->color_w;
    d->expand_x = width - 4*(d->color_w+d->button_spacing) - (d->expand_w+d->button_spacing);
    d->mode_w = d->color_w;
    d->mode_x = width - 3*(d->color_w+d->button_spacing) - (d->mode_w+d->button_spacing);
    d->red_x = width - 3*(d->color_w+d->button_spacing);
    d->green_x = width - 2*(d->color_w+d->button_spacing);
    d->blue_x = width - (d->color_w+d->button_spacing);
  }

  _draw_histogram_box(cr, 0, 0, width, height);

  if(d->expand)
  {
    _draw_histogram_box(cr, 0, height, width, height);
    _draw_histogram_box(cr, 0, 2*height, width, height);
    _draw_histogram_box(cr, 0, 3*height, width, height);
  }

  if(d->highlight == 1)
  {
    cairo_set_source_rgb (cr, .5, .5, .5);
    cairo_rectangle(cr, 0, 0, .2*width, height);
    cairo_fill(cr);
  }
  else if(d->highlight == 2)
  {
    cairo_set_source_rgb (cr, .5, .5, .5);
    cairo_rectangle(cr, 0.2*width, 0, width, height);
    cairo_fill(cr);
  }

  // draw grid
  _draw_histogram_grid(cr, 0, 0, width, height);

  if(d->expand)
  {
    _draw_histogram_grid(cr, 0, height, width, 2*height);
    _draw_histogram_grid(cr, 0, 2*height, width, 3*height);
    _draw_histogram_grid(cr, 0, 3*height, width, 4*height);
  }

  if(hist_max > 0)
  {
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
    cairo_set_line_width(cr, 1.);
    if(d->red)
    {
      cairo_set_source_rgba(cr, 1., 0., 0., 0.4);
      dt_draw_histogram_8(cr, 0, width, height, hist_max, hist, 0, dev->histogram_linear);
    }
    if(d->green)
    {
      cairo_set_source_rgba(cr, 0., 1., 0., 0.4);
      dt_draw_histogram_8(cr, 0, width, height, hist_max, hist, 1, dev->histogram_linear);
    }
    if(d->blue)
    {
      cairo_set_source_rgba(cr, 0., 0., 1., 0.4);
      dt_draw_histogram_8(cr, 0, width, height, hist_max, hist, 2, dev->histogram_linear);
    }
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_restore(cr);

    if(d->expand)
    {
      if(d->red)
      {
        cairo_save(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
        cairo_set_line_width(cr, 1.);

        cairo_set_source_rgba(cr, 1., 0., 0., 0.4);
        dt_draw_histogram_8(cr, height, width, height, hist_max, hist, 0, dev->histogram_linear);

        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_restore(cr);
      }
      if(d->green)
      {
        cairo_save(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
        cairo_set_line_width(cr, 1.);

        cairo_set_source_rgba(cr, 0., 1., 0., 0.4);
        dt_draw_histogram_8(cr, 2*height, width, height, hist_max, hist, 1, dev->histogram_linear);

        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_restore(cr);
      }
      if(d->blue)
      {
        cairo_save(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
        cairo_set_line_width(cr, 1.);

        cairo_set_source_rgba(cr, 0., 0., 1., 0.4);
        dt_draw_histogram_8(cr, 3*height, width, height, hist_max, hist, 2, dev->histogram_linear);

        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_restore(cr);
      }
    }
  }

  cairo_set_source_rgb(cr, .25, .25, .25);
  cairo_select_font_face (cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size (cr, .1*height);

  char exifline[50];
  cairo_move_to (cr, .02*width, .98*height);
  dt_image_print_exif(&dev->image_storage, exifline, 50);
  cairo_show_text(cr, exifline);

  // buttons to control the display of the histogram: expand, linear/log, r, g, b
  if(d->highlight != 0)
  {
    _draw_expand_toggle(cr, d->expand_x, d->button_y, d->expand_w, d->button_h, d->expand);
    _draw_mode_toggle(cr, d->mode_x, d->button_y, d->mode_w, d->button_h, darktable.develop->histogram_linear);
    cairo_set_source_rgba(cr, 1.0, 0.0, 0.0, 0.4);
    _draw_color_toggle(cr, d->red_x, d->button_y, d->color_w, d->button_h, d->red);
    cairo_set_source_rgba(cr, 0.0, 1.0, 0.0, 0.4);
    _draw_color_toggle(cr, d->green_x, d->button_y, d->color_w, d->button_h, d->green);
    cairo_set_source_rgba(cr, 0.0, 0.0, 1.0, 0.4);
    _draw_color_toggle(cr, d->blue_x, d->button_y, d->color_w, d->button_h, d->blue);
  }

  cairo_destroy(cr);
  cairo_t *cr_pixmap = gdk_cairo_create(gtk_widget_get_window(widget));
  cairo_set_source_surface (cr_pixmap, cst, 0, 0);
  cairo_paint(cr_pixmap);
  cairo_destroy(cr_pixmap);
  cairo_surface_destroy(cst);
  return TRUE;
}

static gboolean _lib_histogram_motion_notify_callback(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;

  /* check if exposure hooks are available */
  gboolean hooks_available = dt_dev_exposure_hooks_available(darktable.develop);

  if(!hooks_available)
    return TRUE;

  if (d->dragging && d->highlight == 2)
  {
    float white = d->white - (event->x - d->button_down_x)*
                  1.0f/(float)widget->allocation.width;
    dt_dev_exposure_set_white(darktable.develop, white);
  }
  else if(d->dragging && d->highlight == 1)
  {
    float black = d->black - (event->x - d->button_down_x)*
                  .1f/(float)widget->allocation.width;
    dt_dev_exposure_set_black(darktable.develop, black);
  }
  else
  {
    const float x = event->x;
    const float y = event->y - 5;
    const float pos = x / (float)(widget->allocation.width);


    if(pos < 0 || pos > 1.0);
    else if(x > d->expand_x && x < d->expand_x+d->expand_w && y > d->button_y && y < d->button_y + d->button_h)
    {
      d->highlight = 7;
      g_object_set(G_OBJECT(widget), "tooltip-text", d->expand?_("collapse channels"):_("expand channels"), (char *)NULL);
    }
    else if(x > d->mode_x && x < d->mode_x+d->mode_w && y > d->button_y && y < d->button_y + d->button_h)
    {
      d->highlight = 3;
      g_object_set(G_OBJECT(widget), "tooltip-text", darktable.develop->histogram_linear?_("set histogram mode to logarithmic"):_("set histogram mode to linear"), (char *)NULL);
    }
    else if(x > d->red_x && x < d->red_x+d->color_w && y > d->button_y && y < d->button_y + d->button_h)
    {
      d->highlight = 4;
      g_object_set(G_OBJECT(widget), "tooltip-text", d->red?_("click to hide red channel"):_("click to show red channel"), (char *)NULL);
    }
    else if(x > d->green_x && x < d->green_x+d->color_w && y > d->button_y && y < d->button_y + d->button_h)
    {
      d->highlight = 5;
      g_object_set(G_OBJECT(widget), "tooltip-text", d->red?_("click to hide green channel"):_("click to show green channel"), (char *)NULL);
    }
    else if(x > d->blue_x && x < d->blue_x+d->color_w && y > d->button_y && y < d->button_y + d->button_h)
    {
      d->highlight = 6;
      g_object_set(G_OBJECT(widget), "tooltip-text", d->red?_("click to hide blue channel"):_("click to show blue channel"), (char *)NULL);
    }
    else if(pos < 0.2)
    {
      d->highlight = 1;
      g_object_set(G_OBJECT(widget), "tooltip-text", _("drag to change black point,\ndoubleclick resets"), (char *)NULL);
    }
    else
    {
      d->highlight = 2;
      g_object_set(G_OBJECT(widget), "tooltip-text", _("drag to change exposure,\ndoubleclick resets"), (char *)NULL);
    }
    gtk_widget_queue_draw(widget);
  }
  gint x, y; // notify gtk for motion_hint.
  gdk_window_get_pointer(event->window, &x, &y, NULL);
  return TRUE;
}

static gboolean _lib_histogram_button_press_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;

  /* check if exposure hooks are available */
  gboolean hooks_available = dt_dev_exposure_hooks_available(darktable.develop);

  if(!hooks_available)
    return TRUE;

  if(event->type == GDK_2BUTTON_PRESS)
  {
    dt_dev_exposure_reset_defaults(darktable.develop);
  }
  else
  {
    if(d->highlight == 7) // expand button
    {
      d->expand = !d->expand;
      dt_conf_set_bool("plugins/darkroom/histogram/expand", d->expand);
    }
    else if(d->highlight == 3) // mode button
    {
      darktable.develop->histogram_linear = !darktable.develop->histogram_linear;
      dt_conf_set_string("plugins/darkroom/histogram/mode", darktable.develop->histogram_linear?"linear":"logarithmic");
    }
    else if(d->highlight == 4) // red button
    {
      d->red = !d->red;
      dt_conf_set_bool("plugins/darkroom/histogram/show_red", d->red);
    }
    else if(d->highlight == 5) // green button
    {
      d->green = !d->green;
      dt_conf_set_bool("plugins/darkroom/histogram/show_green", d->green);
    }
    else if(d->highlight == 6) // blue button
    {
      d->blue = !d->blue;
      dt_conf_set_bool("plugins/darkroom/histogram/show_blue", d->blue);
    }
    else
    {
      d->dragging = 1;

      if(d->highlight == 2)
        d->white = dt_dev_exposure_get_white(darktable.develop);

      if(d->highlight == 1)
        d->black = dt_dev_exposure_get_black(darktable.develop);

      d->button_down_x = event->x;
      d->button_down_y = event->y;
    }
  }
  // update for good measure
  dt_control_queue_redraw_widget(self->widget);

  return TRUE;
}

static gboolean _lib_histogram_scroll_callback(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;

  float cw = dt_dev_exposure_get_white(darktable.develop);
  float cb = dt_dev_exposure_get_black(darktable.develop);

  if(event->direction == GDK_SCROLL_UP && d->highlight == 2)
    dt_dev_exposure_set_white(darktable.develop, cw-0.1);

  if(event->direction == GDK_SCROLL_DOWN && d->highlight == 2)
    dt_dev_exposure_set_white(darktable.develop, cw+0.1);

  if(event->direction == GDK_SCROLL_UP && d->highlight == 1)
    dt_dev_exposure_set_black(darktable.develop, cb-0.005);

  if(event->direction == GDK_SCROLL_DOWN && d->highlight == 1)
    dt_dev_exposure_set_black(darktable.develop, cb+0.005);

  return TRUE;
}

static gboolean _lib_histogram_button_release_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  d->dragging = 0;
  return TRUE;
}

static gboolean _lib_histogram_enter_notify_callback(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_control_change_cursor(GDK_HAND1);
  return TRUE;
}

static gboolean _lib_histogram_leave_notify_callback(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  d->dragging = 0;
  d->highlight = 0;
  dt_control_change_cursor(GDK_LEFT_PTR);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
