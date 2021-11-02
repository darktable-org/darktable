/*
    This file is part of darktable,
    Copyright (C) 2012-2021 darktable developers.

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
#include "common/calculator.h"
#include "common/darktable.h"
#include "control/conf.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <math.h>
#include <strings.h>

#include <pango/pangocairo.h>

G_DEFINE_TYPE(DtBauhausWidget, dt_bh, GTK_TYPE_DRAWING_AREA)

enum
{
  // Sliders
  DT_ACTION_ELEMENT_VALUE = 0,
  DT_ACTION_ELEMENT_BUTTON = 1,
  DT_ACTION_ELEMENT_FORCE = 2,
  DT_ACTION_ELEMENT_ZOOM = 3,

  // Combos
  DT_ACTION_ELEMENT_SELECTION = 0,
//DT_ACTION_ELEMENT_BUTTON = 1,
};

// INNER_PADDING is the horizontal space between slider and quad
// and vertical space between labels and slider baseline
static const double INNER_PADDING = 4.0;

// fwd declare
static gboolean dt_bauhaus_popup_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data);
static gboolean dt_bauhaus_popup_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data);
static void dt_bauhaus_widget_accept(dt_bauhaus_widget_t *w);
static void dt_bauhaus_widget_reject(dt_bauhaus_widget_t *w);
static void _bauhaus_combobox_set(GtkWidget *widget, const int pos, const gboolean mute);

static void bauhaus_request_focus(dt_bauhaus_widget_t *w)
{
  if(w->module && w->module->type == DT_ACTION_TYPE_IOP_INSTANCE)
      dt_iop_request_focus((dt_iop_module_t *)w->module);
  gtk_widget_set_state_flags(GTK_WIDGET(w), GTK_STATE_FLAG_FOCUSED, TRUE);
}

static gboolean _combobox_next_entry(GList *entries, int *new_pos, int delta_y)
{
  dt_bauhaus_combobox_entry_t *entry = (dt_bauhaus_combobox_entry_t *)g_list_nth_data(entries, *new_pos);
  while(entry && !entry->sensitive)
  {
    *new_pos += delta_y;
    entry = (dt_bauhaus_combobox_entry_t *)g_list_nth_data(entries, *new_pos);
  }
  return entry != NULL;
}

static inline int get_line_height()
{
  return darktable.bauhaus->scale * darktable.bauhaus->line_height;
}

static dt_bauhaus_combobox_entry_t *new_combobox_entry(const char *label, dt_bauhaus_combobox_alignment_t alignment,
                                                       gboolean sensitive, void *data, void (*free_func)(void *))
{
  dt_bauhaus_combobox_entry_t *entry = (dt_bauhaus_combobox_entry_t *)calloc(1, sizeof(dt_bauhaus_combobox_entry_t));
  entry->label = g_strdup(label);
  entry->alignment = alignment;
  entry->sensitive = sensitive;
  entry->data = data;
  entry->free_func = free_func;
  return entry;
}

static void free_combobox_entry(gpointer data)
{
  dt_bauhaus_combobox_entry_t *entry = (dt_bauhaus_combobox_entry_t *)data;
  g_free(entry->label);
  if(entry->free_func)
    entry->free_func(entry->data);
  free(entry);
}

static inline float inner_height(GtkAllocation allocation)
{
  // retrieve the inner height of the widget (inside the top/bottom margin)
  return allocation.height - 2.0f * darktable.bauhaus->widget_space;
}

static GdkRGBA * default_color_assign()
{
  // helper to initialize a color pointer with red color as a default
  GdkRGBA color;
  color.red = 1.0f;
  color.green = 0.0f;
  color.blue = 0.0f;
  color.alpha = 1.0f;
  return gdk_rgba_copy(&color);
}

void dt_bauhaus_widget_set_section(GtkWidget *widget, const gboolean is_section)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  w->is_section = is_section;
}

static int show_pango_text(dt_bauhaus_widget_t *w, GtkStyleContext *context, cairo_t *cr,
                           char *text, float x_pos, float y_pos, float max_width,
                           gboolean right_aligned, gboolean calc_only,
                           PangoEllipsizeMode ellipsize, gboolean is_markup, gboolean is_label)
{
  PangoLayout *layout = pango_cairo_create_layout(cr);

  if(max_width > 0)
  {
    pango_layout_set_ellipsize(layout, ellipsize);
    pango_layout_set_width(layout, (int)(PANGO_SCALE * max_width + 0.5f));
  }

  if(text)
  {
    if(is_markup)
      pango_layout_set_markup(layout, text, -1);
    else
      pango_layout_set_text(layout, text, -1);
  }
  else
  {
    // length of -1 is not allowed with NULL string (wtf)
    pango_layout_set_text(layout, NULL, 0);
  }

  PangoFontDescription *font_desc =
    w->is_section && is_label
    ? pango_font_description_copy_static(darktable.bauhaus->pango_sec_font_desc)
    : pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);

  // This should be able to update the font style for current text depending on :hover, :focused, etc.
  // CSS pseudo-classes, yet it defaults to system font.
  // FIXME: get that working so we can put :active text in bold, for example.
  //gtk_style_context_get(context, gtk_widget_get_state_flags(GTK_WIDGET(w)), "font", font_desc, NULL);

  pango_layout_set_font_description(layout, font_desc);

  PangoAttrList *attrlist = pango_attr_list_new();
  PangoAttribute *attr = pango_attr_font_features_new("tnum");
  pango_attr_list_insert(attrlist, attr);
  pango_layout_set_attributes(layout, attrlist);
  pango_attr_list_unref(attrlist);

  pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);

  int pango_width, pango_height;
  pango_layout_get_size(layout, &pango_width, &pango_height);
  const float text_width = ((double)pango_width/PANGO_SCALE);

  if(right_aligned) x_pos -= text_width;

  if(!calc_only)
  {
    cairo_move_to(cr, x_pos, y_pos);
    pango_cairo_show_layout(cr, layout);
  }
  pango_font_description_free(font_desc);
  g_object_unref(layout);

  return text_width;
}

// -------------------------------
static gboolean _cursor_timeout_callback(gpointer user_data)
{
  if(darktable.bauhaus->cursor_blink_counter > 0) darktable.bauhaus->cursor_blink_counter--;

  darktable.bauhaus->cursor_visible = !darktable.bauhaus->cursor_visible;
  gtk_widget_queue_draw(darktable.bauhaus->popup_area);

 // this can be >0 when we haven't reached the desired number or -1 when blinking forever
  if(darktable.bauhaus->cursor_blink_counter != 0)
    return TRUE;

  darktable.bauhaus->cursor_timeout = 0; // otherwise the cursor won't come up when starting to type
  return FALSE;
}

static void _start_cursor(int max_blinks)
{
  darktable.bauhaus->cursor_blink_counter = max_blinks;
  darktable.bauhaus->cursor_visible = FALSE;
  if(darktable.bauhaus->cursor_timeout == 0)
    darktable.bauhaus->cursor_timeout = g_timeout_add(500, _cursor_timeout_callback, NULL);
}

static void _stop_cursor()
{
  if(darktable.bauhaus->cursor_timeout > 0)
  {
    g_source_remove(darktable.bauhaus->cursor_timeout);
    darktable.bauhaus->cursor_timeout = 0;
    darktable.bauhaus->cursor_visible = FALSE;
  }
}
// -------------------------------


static void dt_bauhaus_slider_set_normalized(dt_bauhaus_widget_t *w, float pos);

static float slider_right_pos(float width)
{
  // relative position (in widget) of the right bound of the slider corrected with the inner padding
  return 1.0f - (darktable.bauhaus->quad_width + INNER_PADDING) / width;
}

static float slider_coordinate(const float abs_position, const float width)
{
  // Translates an horizontal position relative to the slider
  // in an horizontal position relative to the widget
  const float left_bound = 0.0f;
  const float right_bound = slider_right_pos(width); // exclude the quad area on the right
  return (left_bound + abs_position * (right_bound - left_bound)) * width;
}


static float get_slider_line_offset(float pos, float scale, float x, float y, float ht, const int width)
{
  // ht is in [0,1] scale here
  const float l = 0.0f;
  const float r = slider_right_pos(width);

  float offset = 0.0f;
  // handle linear startup and rescale y to fit the whole range again
  if(y < ht)
  {
    offset = (x - l) / (r - l) - pos;
  }
  else
  {
    y -= ht;
    y /= (1.0f - ht);

    offset = (x - y * y * .5f - (1.0f - y * y) * (l + pos * (r - l)))
             / (.5f * y * y / scale + (1.0f - y * y) * (r - l));
  }
  // clamp to result in a [0,1] range:
  if(pos + offset > 1.0f) offset = 1.0f - pos;
  if(pos + offset < 0.0f) offset = -pos;
  return offset;
}

// draw a loupe guideline for the quadratic zoom in in the slider interface:
static void draw_slider_line(cairo_t *cr, float pos, float off, float scale, const int width,
                             const int height, const int ht)
{
  // pos is normalized position [0,1], offset is on that scale.
  // ht is in pixels here
  const float l = 0.0f;
  const float r = slider_right_pos(width);

  const int steps = 64;
  cairo_move_to(cr, width * (l + (pos + off) * (r - l)), ht * .7f);
  cairo_line_to(cr, width * (l + (pos + off) * (r - l)), ht);
  for(int j = 1; j < steps; j++)
  {
    const float y = j / (steps - 1.0f);
    const float x = y * y * .5f * (1.f + off / scale) + (1.0f - y * y) * (l + (pos + off) * (r - l));
    cairo_line_to(cr, x * width, ht + y * (height - ht));
  }
}
// -------------------------------

// handlers on the popup window, to close popup:
static gboolean dt_bauhaus_window_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  const float tol = 50;
  gint wx, wy;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  gdk_window_get_origin(gtk_widget_get_window(widget), &wx, &wy);

  if(event->x_root > wx + allocation.width + tol || event->y_root > wy + inner_height(allocation) + tol
     || event->x_root < (int)wx - tol || event->y_root < (int)wy - tol)
  {
    dt_bauhaus_widget_reject(darktable.bauhaus->current);
    gtk_widget_set_state_flags(GTK_WIDGET(darktable.bauhaus->current), GTK_STATE_FLAG_NORMAL, TRUE);
    dt_bauhaus_hide_popup();
    return TRUE;
  }
  // make sure to propagate the event further
  return FALSE;
}

static gboolean dt_bauhaus_window_button_press(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  const float tol = 0;
  gint wx, wy;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  gdk_window_get_origin(gtk_widget_get_window(widget), &wx, &wy);

  if((event->x_root > wx + allocation.width + tol || event->y_root > wy + inner_height(allocation) + tol
      || event->x_root < (int)wx - tol || event->y_root < (int)wy - tol))
  {
    dt_bauhaus_widget_reject(darktable.bauhaus->current);
    gtk_widget_set_state_flags(GTK_WIDGET(darktable.bauhaus->current), GTK_STATE_FLAG_NORMAL, FALSE);
    dt_bauhaus_hide_popup();
    return TRUE;
  }
  // make sure to propagate the event further
  return FALSE;
}

static void combobox_popup_scroll(int amt)
{
  const dt_bauhaus_combobox_data_t *d = &darktable.bauhaus->current->data.combobox;
  int new_value = CLAMP(d->active + amt, 0, d->num_labels - 1);

  // skip insensitive ones
  if(!_combobox_next_entry(d->entries, &new_value, amt))
    return;

  gint wx = 0, wy = 0;
  const int skip = darktable.bauhaus->line_height;
  GdkWindow *w = gtk_widget_get_window(darktable.bauhaus->popup_window);
  gdk_window_get_origin(w, &wx, &wy);
  // gdk_window_get_position(w, &wx, &wy);
  gdk_window_move(w, wx, wy - skip * (new_value - d->active));

  // make sure highlighted entry is updated:
  darktable.bauhaus->mouse_x = 0;
  darktable.bauhaus->mouse_y = new_value * skip + skip / 2;
  gtk_widget_queue_draw(darktable.bauhaus->popup_area);

  // and we change the value
  _bauhaus_combobox_set(GTK_WIDGET(darktable.bauhaus->current), new_value, d->mute_scrolling);
}


static gboolean dt_bauhaus_popup_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  switch(darktable.bauhaus->current->type)
  {
    case DT_BAUHAUS_COMBOBOX:
    {
      int delta_y = 0;
      if(dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y))
         combobox_popup_scroll(delta_y);
      break;
    }
    case DT_BAUHAUS_SLIDER:
      break;
    default:
      break;
  }
  return TRUE;
}

static gboolean dt_bauhaus_popup_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  GtkAllocation allocation_popup_window;
  gtk_widget_get_allocation(darktable.bauhaus->popup_window, &allocation_popup_window);
  gtk_widget_queue_draw(darktable.bauhaus->popup_area);
  dt_bauhaus_widget_t *w = darktable.bauhaus->current;
  GtkAllocation allocation_w;
  gtk_widget_get_allocation(GTK_WIDGET(w), &allocation_w);
  const int width = allocation_popup_window.width, height = inner_height(allocation_popup_window);
  // coordinate transform is in vain because we're only ever called after a button release.
  // that means the system is always the one of the popup.
  // that also means that we can't have hovering combobox entries while still holding the button. :(
  const float ex = event->x;
  const float ey = event->y;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);

  gtk_widget_set_state_flags(GTK_WIDGET(w), GTK_STATE_FLAG_PRELIGHT, TRUE);

  if(darktable.bauhaus->keys_cnt == 0) _stop_cursor();

  switch(w->type)
  {
    case DT_BAUHAUS_COMBOBOX:
      darktable.bauhaus->mouse_x = ex;
      darktable.bauhaus->mouse_y = ey;
      break;
    case DT_BAUHAUS_SLIDER:
    {
      const dt_bauhaus_slider_data_t *d = &w->data.slider;
      const float mouse_off = get_slider_line_offset(d->oldpos, d->scale, ex / width, ey / height,
                                                     allocation_w.height / (float)height, allocation.width);
      if(!darktable.bauhaus->change_active)
      {
        if((darktable.bauhaus->mouse_line_distance < 0 && mouse_off >= 0)
           || (darktable.bauhaus->mouse_line_distance > 0 && mouse_off <= 0))
          darktable.bauhaus->change_active = 1;
        darktable.bauhaus->mouse_line_distance = mouse_off;
      }
      if(darktable.bauhaus->change_active)
      {
        // remember mouse position for motion effects in draw
        darktable.bauhaus->mouse_x = ex;
        darktable.bauhaus->mouse_y = ey;
        dt_bauhaus_slider_set_normalized(w, d->oldpos + mouse_off);
      }
    }
    break;
    default:
      break;
  }
  return TRUE;
}

static gboolean dt_bauhaus_popup_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  gtk_widget_set_state_flags(widget, GTK_STATE_FLAG_NORMAL, TRUE);
  return TRUE;
}

static gboolean dt_bauhaus_popup_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(darktable.bauhaus->current && (darktable.bauhaus->current->type == DT_BAUHAUS_COMBOBOX)
     && (event->button == 1) &&                                // only accept left mouse click
     (dt_get_wtime() - darktable.bauhaus->opentime >= 0.250f)) // default gtk timeout for double-clicks
  {
    gtk_widget_set_state_flags(widget, GTK_STATE_FLAG_ACTIVE, TRUE);

    // event might be in wrong system, transform ourselves:
    gint wx, wy, x, y;
    gdk_window_get_origin(gtk_widget_get_window(darktable.bauhaus->popup_window), &wx, &wy);

    gdk_device_get_position(
        gdk_seat_get_pointer(gdk_display_get_default_seat(gtk_widget_get_display(widget))), 0, &x, &y);
    darktable.bauhaus->end_mouse_x = x - wx;
    darktable.bauhaus->end_mouse_y = y - wy;
    const dt_bauhaus_combobox_data_t *d = &darktable.bauhaus->current->data.combobox;
    if(!d->mute_scrolling)
      dt_bauhaus_widget_accept(darktable.bauhaus->current);
    dt_bauhaus_hide_popup();
  }
  else if(darktable.bauhaus->hiding)
  {
    dt_bauhaus_hide_popup();
  }
  return TRUE;
}

static gboolean dt_bauhaus_popup_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(event->button == 1)
  {
    if(darktable.bauhaus->current->type == DT_BAUHAUS_COMBOBOX
       && dt_get_wtime() - darktable.bauhaus->opentime < 0.250f) // default gtk timeout for double-clicks
    {
      // counts as double click, reset:
      const dt_bauhaus_combobox_data_t *d = &darktable.bauhaus->current->data.combobox;
      dt_bauhaus_combobox_set(GTK_WIDGET(darktable.bauhaus->current), d->defpos);
      dt_bauhaus_widget_reject(darktable.bauhaus->current);
      gtk_widget_set_state_flags(GTK_WIDGET(darktable.bauhaus->current),
                                 GTK_STATE_FLAG_FOCUSED, FALSE);
    }
    else
    {
      // only accept left mouse click
      darktable.bauhaus->end_mouse_x = event->x;
      darktable.bauhaus->end_mouse_y = event->y;
      dt_bauhaus_widget_accept(darktable.bauhaus->current);
      gtk_widget_set_state_flags(GTK_WIDGET(darktable.bauhaus->current),
                                 GTK_STATE_FLAG_FOCUSED, FALSE);
    }
  }
  else
  {
    dt_bauhaus_widget_reject(darktable.bauhaus->current);
  }
  darktable.bauhaus->hiding = TRUE;
  return TRUE;
}

static void dt_bauhaus_window_show(GtkWidget *w, gpointer user_data)
{
  // Could grab the popup_area rather than popup_window, but if so
  // then popup_area would get all motion events including those
  // outside of the popup. This way the popup_area gets motion events
  // related to updating the popup, and popup_window gets all others
  // which would be the ones telling it to close the popup.
  gtk_grab_add(w);
}

static void dt_bh_init(DtBauhausWidget *class)
{
  // not sure if we want to use this instead of our code in *_new()
  // TODO: the common code from bauhaus_widget_init() could go here.
}

static void dt_bh_class_init(DtBauhausWidgetClass *class)
{
  darktable.bauhaus->signals[DT_BAUHAUS_VALUE_CHANGED_SIGNAL]
      = g_signal_new("value-changed", G_TYPE_FROM_CLASS(class), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                     g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);
  darktable.bauhaus->signals[DT_BAUHAUS_QUAD_PRESSED_SIGNAL]
      = g_signal_new("quad-pressed", G_TYPE_FROM_CLASS(class), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                     g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);

  // TODO: could init callbacks once per class for more efficiency:
  // GtkWidgetClass *widget_class;
  // widget_class = GTK_WIDGET_CLASS (class);
  // widget_class->draw = dt_bauhaus_draw;
}

void dt_bauhaus_load_theme()
{
  darktable.bauhaus->line_space = 1.5;
  darktable.bauhaus->line_height = 9;
  darktable.bauhaus->marker_size = 0.25f;

  GtkWidget *root_window = dt_ui_main_window(darktable.gui->ui);
  GtkStyleContext *ctx = gtk_style_context_new();
  GtkWidgetPath *path = gtk_widget_path_new();
  const int pos = gtk_widget_path_append_type(path, GTK_TYPE_WIDGET);
  gtk_widget_path_iter_set_name(path, pos, "iop-plugin-ui");
  gtk_style_context_set_path(ctx, path);
  gtk_style_context_set_screen (ctx, gtk_widget_get_screen(root_window));

  gtk_style_context_lookup_color(ctx, "bauhaus_fg", &darktable.bauhaus->color_fg);
  gtk_style_context_lookup_color(ctx, "bauhaus_fg_insensitive", &darktable.bauhaus->color_fg_insensitive);
  gtk_style_context_lookup_color(ctx, "bauhaus_bg", &darktable.bauhaus->color_bg);
  gtk_style_context_lookup_color(ctx, "bauhaus_border", &darktable.bauhaus->color_border);
  gtk_style_context_lookup_color(ctx, "bauhaus_fill", &darktable.bauhaus->color_fill);
  gtk_style_context_lookup_color(ctx, "bauhaus_indicator_border", &darktable.bauhaus->indicator_border);

  gtk_style_context_lookup_color(ctx, "graph_bg", &darktable.bauhaus->graph_bg);
  gtk_style_context_lookup_color(ctx, "graph_exterior", &darktable.bauhaus->graph_exterior);
  gtk_style_context_lookup_color(ctx, "graph_border", &darktable.bauhaus->graph_border);
  gtk_style_context_lookup_color(ctx, "graph_grid", &darktable.bauhaus->graph_grid);
  gtk_style_context_lookup_color(ctx, "graph_fg", &darktable.bauhaus->graph_fg);
  gtk_style_context_lookup_color(ctx, "graph_fg_active", &darktable.bauhaus->graph_fg_active);
  gtk_style_context_lookup_color(ctx, "graph_overlay", &darktable.bauhaus->graph_overlay);
  gtk_style_context_lookup_color(ctx, "inset_histogram", &darktable.bauhaus->inset_histogram);
  gtk_style_context_lookup_color(ctx, "graph_red", &darktable.bauhaus->graph_colors[0]);
  gtk_style_context_lookup_color(ctx, "graph_green", &darktable.bauhaus->graph_colors[1]);
  gtk_style_context_lookup_color(ctx, "graph_blue", &darktable.bauhaus->graph_colors[2]);
  gtk_style_context_lookup_color(ctx, "colorlabel_red",
                                 &darktable.bauhaus->colorlabels[DT_COLORLABELS_RED]);
  gtk_style_context_lookup_color(ctx, "colorlabel_yellow",
                                 &darktable.bauhaus->colorlabels[DT_COLORLABELS_YELLOW]);
  gtk_style_context_lookup_color(ctx, "colorlabel_green",
                                 &darktable.bauhaus->colorlabels[DT_COLORLABELS_GREEN]);
  gtk_style_context_lookup_color(ctx, "colorlabel_blue",
                                 &darktable.bauhaus->colorlabels[DT_COLORLABELS_BLUE]);
  gtk_style_context_lookup_color(ctx, "colorlabel_purple",
                                 &darktable.bauhaus->colorlabels[DT_COLORLABELS_PURPLE]);

  PangoFontDescription *pfont = 0;
  gtk_style_context_get(ctx, GTK_STATE_FLAG_NORMAL, "font", &pfont, NULL);

  // make sure we release previously loaded font
  if(darktable.bauhaus->pango_font_desc)
    pango_font_description_free(darktable.bauhaus->pango_font_desc);

  darktable.bauhaus->pango_font_desc = pfont;

  if(darktable.bauhaus->pango_sec_font_desc)
    pango_font_description_free(darktable.bauhaus->pango_sec_font_desc);

  // now get the font for the section labels
  gtk_widget_path_iter_set_name(path, pos, "section_label");
  gtk_style_context_set_path(ctx, path);
  gtk_style_context_get(ctx, GTK_STATE_FLAG_NORMAL, "font", &pfont, NULL);
  darktable.bauhaus->pango_sec_font_desc = pfont;

  gtk_widget_path_free(path);

  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 128, 128);
  cairo_t *cr = cairo_create(cst);
  PangoLayout *layout = pango_cairo_create_layout(cr);
  pango_layout_set_text(layout, "m", -1);
  pango_layout_set_font_description(layout, darktable.bauhaus->pango_font_desc);
  pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);
  int pango_width, pango_height;
  pango_layout_get_size(layout, &pango_width, &pango_height);
  g_object_unref(layout);
  cairo_destroy(cr);
  cairo_surface_destroy(cst);

  darktable.bauhaus->scale = 1.33f;
  darktable.bauhaus->line_height = pango_height / PANGO_SCALE;
  darktable.bauhaus->widget_space = INNER_PADDING / 4.0f; // used as a top/bottom margin for widgets
  darktable.bauhaus->quad_width = darktable.bauhaus->line_height;

  darktable.bauhaus->baseline_size = darktable.bauhaus->line_height / 2.5f; // absolute size in Cairo unit
  darktable.bauhaus->border_width = 2.0f; // absolute size in Cairo unit
  darktable.bauhaus->marker_size = (darktable.bauhaus->baseline_size + darktable.bauhaus->border_width) * 0.9f;
}

void dt_bauhaus_init()
{
  darktable.bauhaus = (dt_bauhaus_t *)calloc(1, sizeof(dt_bauhaus_t));
  darktable.bauhaus->keys_cnt = 0;
  darktable.bauhaus->current = NULL;
  darktable.bauhaus->popup_area = gtk_drawing_area_new();
  gtk_widget_set_name(darktable.bauhaus->popup_area, "bauhaus-popup");
  darktable.bauhaus->pango_font_desc = NULL;

  dt_bauhaus_load_theme();

  darktable.bauhaus->skip_accel = 1;

  // this easily gets keyboard input:
  // darktable.bauhaus->popup_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  // but this doesn't flicker, and the above hack with key input seems to work well.
  darktable.bauhaus->popup_window = gtk_window_new(GTK_WINDOW_POPUP);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(darktable.bauhaus->popup_window);
#endif
  // this is needed for popup, not for toplevel.
  // since popup_area gets the focus if we show the window, this is all
  // we need.

  gtk_widget_set_size_request(darktable.bauhaus->popup_area, DT_PIXEL_APPLY_DPI(300), DT_PIXEL_APPLY_DPI(300));
  gtk_window_set_resizable(GTK_WINDOW(darktable.bauhaus->popup_window), FALSE);
  gtk_window_set_default_size(GTK_WINDOW(darktable.bauhaus->popup_window), 260, 260);
  // gtk_window_set_modal(GTK_WINDOW(c->popup_window), TRUE);
  // gtk_window_set_decorated(GTK_WINDOW(c->popup_window), FALSE);

  // for pie menu:
  // gtk_window_set_position(GTK_WINDOW(c->popup_window), GTK_WIN_POS_MOUSE);// | GTK_WIN_POS_CENTER);

  // needed on macOS to avoid fullscreening the popup with newer GTK
  gtk_window_set_type_hint(GTK_WINDOW(darktable.bauhaus->popup_window), GDK_WINDOW_TYPE_HINT_POPUP_MENU);

  gtk_container_add(GTK_CONTAINER(darktable.bauhaus->popup_window), darktable.bauhaus->popup_area);
  // gtk_window_set_title(GTK_WINDOW(c->popup_window), _("dtgtk control popup"));
  gtk_window_set_keep_above(GTK_WINDOW(darktable.bauhaus->popup_window), TRUE);
  gtk_window_set_gravity(GTK_WINDOW(darktable.bauhaus->popup_window), GDK_GRAVITY_STATIC);

  gtk_widget_set_can_focus(darktable.bauhaus->popup_area, TRUE);
  gtk_widget_add_events(darktable.bauhaus->popup_area, GDK_POINTER_MOTION_MASK
                                                       | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                                       | GDK_KEY_PRESS_MASK | GDK_LEAVE_NOTIFY_MASK
                                                       | darktable.gui->scroll_mask);

  g_signal_connect(G_OBJECT(darktable.bauhaus->popup_window), "show", G_CALLBACK(dt_bauhaus_window_show), (gpointer)NULL);
  g_signal_connect(G_OBJECT(darktable.bauhaus->popup_area), "draw", G_CALLBACK(dt_bauhaus_popup_draw),
                   (gpointer)NULL);
  g_signal_connect(G_OBJECT(darktable.bauhaus->popup_window), "motion-notify-event",
                   G_CALLBACK(dt_bauhaus_window_motion_notify), (gpointer)NULL);
  g_signal_connect(G_OBJECT(darktable.bauhaus->popup_window), "button-press-event", G_CALLBACK(dt_bauhaus_window_button_press),
                   (gpointer)NULL);
  g_signal_connect(G_OBJECT(darktable.bauhaus->popup_area), "motion-notify-event",
                   G_CALLBACK(dt_bauhaus_popup_motion_notify), (gpointer)NULL);
  g_signal_connect(G_OBJECT(darktable.bauhaus->popup_area), "leave-notify-event",
                   G_CALLBACK(dt_bauhaus_popup_leave_notify), (gpointer)NULL);
  g_signal_connect(G_OBJECT(darktable.bauhaus->popup_area), "button-press-event",
                   G_CALLBACK(dt_bauhaus_popup_button_press), (gpointer)NULL);
  // this is connected to the widget itself, not the popup. we're only interested
  // in mouse release events that are initiated by a press on the original widget.
  g_signal_connect (G_OBJECT (darktable.bauhaus->popup_area), "button-release-event",
                    G_CALLBACK (dt_bauhaus_popup_button_release), (gpointer)NULL);
  g_signal_connect(G_OBJECT(darktable.bauhaus->popup_area), "key-press-event",
                   G_CALLBACK(dt_bauhaus_popup_key_press), (gpointer)NULL);
  g_signal_connect(G_OBJECT(darktable.bauhaus->popup_area), "scroll-event",
                   G_CALLBACK(dt_bauhaus_popup_scroll), (gpointer)NULL);
}

void dt_bauhaus_cleanup()
{
}

// fwd declare a few callbacks
static gboolean dt_bauhaus_slider_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean dt_bauhaus_slider_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean dt_bauhaus_slider_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data);
static gboolean dt_bauhaus_slider_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data);
static gboolean dt_bauhaus_slider_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);


static gboolean dt_bauhaus_combobox_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean dt_bauhaus_combobox_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data);
static gboolean dt_bauhaus_combobox_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data);
static gboolean dt_bauhaus_combobox_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);

// static gboolean
// dt_bauhaus_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean dt_bauhaus_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data);


// end static init/cleanup
// =================================================



// common initialization
static void dt_bauhaus_widget_init(dt_bauhaus_widget_t *w, dt_iop_module_t *self)
{
  w->module = DT_ACTION(self);

  w->section = NULL;

  // no quad icon and no toggle button:
  w->quad_paint = 0;
  w->quad_paint_data = NULL;
  w->quad_toggle = 0;
  w->combo_populate = NULL;

  switch(w->type)
  {
    case DT_BAUHAUS_SLIDER:
    {
      gtk_widget_set_name(GTK_WIDGET(w), "bauhaus-slider");
      gtk_widget_set_size_request(GTK_WIDGET(w), -1, 2 * darktable.bauhaus->widget_space + INNER_PADDING + darktable.bauhaus->baseline_size + get_line_height() - darktable.bauhaus->border_width / 2.0f);
      break;
    }
    case DT_BAUHAUS_COMBOBOX:
    {
      gtk_widget_set_name(GTK_WIDGET(w), "bauhaus-combobox");
      gtk_widget_set_size_request(GTK_WIDGET(w), -1, 2 * darktable.bauhaus->widget_space + get_line_height());
      break;
    }
  }

  gtk_widget_add_events(GTK_WIDGET(w), GDK_POINTER_MOTION_MASK
                                       | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                       | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK
                                       | GDK_FOCUS_CHANGE_MASK
                                       | darktable.gui->scroll_mask);

  g_signal_connect(G_OBJECT(w), "draw", G_CALLBACK(dt_bauhaus_draw), NULL);

  // for combobox, where mouse-release triggers a selection, we need to catch this
  // event where the mouse-press occurred, which will be this widget. we just pass
  // it on though:
  // g_signal_connect (G_OBJECT (w), "button-release-event",
  //                   G_CALLBACK (dt_bauhaus_popup_button_release), (gpointer)NULL);
}

void dt_bauhaus_combobox_set_default(GtkWidget *widget, int def)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->defpos = def;
}

int dt_bauhaus_combobox_get_default(GtkWidget *widget)
{
  const dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  const dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  return d->defpos;
}

void dt_bauhaus_slider_set_hard_min(GtkWidget* widget, float val)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  float pos = dt_bauhaus_slider_get(widget);
  d->hard_min = val;
  d->min = MAX(d->min, d->hard_min);
  d->soft_min = MAX(d->soft_min, d->hard_min);

  if(val > d->hard_max) dt_bauhaus_slider_set_hard_max(widget,val);
  if(pos < val)
  {
    dt_bauhaus_slider_set_soft(widget,val);
  }
  else
  {
    dt_bauhaus_slider_set_soft(widget,pos);
  }
}

float dt_bauhaus_slider_get_hard_min(GtkWidget* widget)
{
  const dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  const dt_bauhaus_slider_data_t *d = &w->data.slider;
  return d->hard_min;
}

void dt_bauhaus_slider_set_hard_max(GtkWidget* widget, float val)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  float pos = dt_bauhaus_slider_get(widget);
  d->hard_max = val;
  d->max = MIN(d->max, d->hard_max);
  d->soft_max = MIN(d->soft_max, d->hard_max);

  if(val < d->hard_min) dt_bauhaus_slider_set_hard_min(widget,val);
  if(pos > val)
  {
    dt_bauhaus_slider_set_soft(widget,val);
  }
  else
  {
    dt_bauhaus_slider_set_soft(widget,pos);
  }
}

float dt_bauhaus_slider_get_hard_max(GtkWidget* widget)
{
  const dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  const dt_bauhaus_slider_data_t *d = &w->data.slider;
  return d->hard_max;
}

void dt_bauhaus_slider_set_soft_min(GtkWidget* widget, float val)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  float oldval = dt_bauhaus_slider_get(widget);
  d->min = d->soft_min = CLAMP(val,d->hard_min,d->hard_max);
  dt_bauhaus_slider_set_soft(widget,oldval);
}

float dt_bauhaus_slider_get_soft_min(GtkWidget* widget)
{
  const dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  const dt_bauhaus_slider_data_t *d = &w->data.slider;
  return d->soft_min;
}

void dt_bauhaus_slider_set_soft_max(GtkWidget* widget, float val)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  float oldval = dt_bauhaus_slider_get(widget);
  d->max = d->soft_max = CLAMP(val,d->hard_min,d->hard_max);
  dt_bauhaus_slider_set_soft(widget,oldval);
}

float dt_bauhaus_slider_get_soft_max(GtkWidget* widget)
{
  const dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  const dt_bauhaus_slider_data_t *d = &w->data.slider;
  return d->soft_max;
}

void dt_bauhaus_slider_set_default(GtkWidget *widget, float def)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->defpos = def;
}

void dt_bauhaus_slider_set_soft_range(GtkWidget *widget, float soft_min, float soft_max)
{
  dt_bauhaus_slider_set_soft_min(widget,soft_min);
  dt_bauhaus_slider_set_soft_max(widget,soft_max);
}

float dt_bauhaus_slider_get_default(GtkWidget *widget)
{
  const dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  const dt_bauhaus_slider_data_t *d = &w->data.slider;
  return d->defpos;
}

void dt_bauhaus_slider_enable_soft_boundaries(GtkWidget *widget, float hard_min, float hard_max)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->hard_min = hard_min;
  d->hard_max = hard_max;
}

extern const dt_action_def_t dt_action_def_slider;
extern const dt_action_def_t dt_action_def_combo;

void dt_bauhaus_widget_set_label(GtkWidget *widget, const char *section, const char *label)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  memset(w->label, 0, sizeof(w->label)); // keep valgrind happy
  if(label) g_strlcpy(w->label, _(label), sizeof(w->label));
  if(section) w->section = g_strdup(_(section));

  if(w->module)
  {
    if(!darktable.bauhaus->skip_accel || w->module->type != DT_ACTION_TYPE_IOP_INSTANCE)
    {
      w->module = dt_action_define(w->module, section, label, widget,
                                   w->type == DT_BAUHAUS_SLIDER ? &dt_action_def_slider : &dt_action_def_combo);
    }

    gtk_widget_queue_draw(GTK_WIDGET(w));
  }
}

const char* dt_bauhaus_widget_get_label(GtkWidget *widget)
{
  const dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  return w->label;
}

void dt_bauhaus_widget_set_quad_paint(GtkWidget *widget, dt_bauhaus_quad_paint_f f, int paint_flags, void *paint_data)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  w->quad_paint = f;
  w->quad_paint_flags = paint_flags;
  w->quad_paint_data = paint_data;
}

// make this quad a toggle button:
void dt_bauhaus_widget_set_quad_toggle(GtkWidget *widget, int toggle)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  w->quad_toggle = toggle;
}

void dt_bauhaus_widget_set_quad_active(GtkWidget *widget, int active)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if (active)
    w->quad_paint_flags |= CPF_ACTIVE;
  else
    w->quad_paint_flags &= ~CPF_ACTIVE;
  gtk_widget_queue_draw(GTK_WIDGET(w));
}

int dt_bauhaus_widget_get_quad_active(GtkWidget *widget)
{
  const dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  return (w->quad_paint_flags & CPF_ACTIVE) == CPF_ACTIVE;
}

void dt_bauhaus_widget_press_quad(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if (w->quad_toggle)
  {
    if (w->quad_paint_flags & CPF_ACTIVE)
      w->quad_paint_flags &= ~CPF_ACTIVE;
    else
      w->quad_paint_flags |= CPF_ACTIVE;
  }
  else
    w->quad_paint_flags |= CPF_ACTIVE;

  g_signal_emit_by_name(G_OBJECT(w), "quad-pressed");
}

void dt_bauhaus_widget_release_quad(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if (!w->quad_toggle)
  {
    if (w->quad_paint_flags & CPF_ACTIVE)
      w->quad_paint_flags &= ~CPF_ACTIVE;
    gtk_widget_queue_draw(GTK_WIDGET(w));
  }
}

static float _default_linear_curve(GtkWidget *self, float value, dt_bauhaus_curve_t dir)
{
  // regardless of dir: input <-> output
  return value;
}

static float _reverse_linear_curve(GtkWidget *self, float value, dt_bauhaus_curve_t dir)
{
  // regardless of dir: input <-> output
  return 1.0 - value;
}

static void dt_bauhaus_slider_destroy(dt_bauhaus_widget_t *widget, gpointer user_data)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  if(w->section) g_free(w->section);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  if(d->timeout_handle) g_source_remove(d->timeout_handle);
  d->timeout_handle = 0;
}

GtkWidget *dt_bauhaus_slider_new(dt_iop_module_t *self)
{
  return dt_bauhaus_slider_new_with_range(self, 0.0, 1.0, 0.1, 0.5, 3);
}

GtkWidget *dt_bauhaus_slider_new_with_range(dt_iop_module_t *self, float min, float max, float step,
                                            float defval, int digits)
{
  return dt_bauhaus_slider_new_with_range_and_feedback(self, min, max, step, defval, digits, 1);
}

GtkWidget *dt_bauhaus_slider_new_action(dt_action_t *self, float min, float max, float step,
                                        float defval, int digits)
{
  return dt_bauhaus_slider_new_with_range((dt_iop_module_t *)self, min, max, step, defval, digits);
}

GtkWidget *dt_bauhaus_slider_new_with_range_and_feedback(dt_iop_module_t *self, float min, float max,
                                                         float step, float defval, int digits, int feedback)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(g_object_new(DT_BAUHAUS_WIDGET_TYPE, NULL));
  return dt_bauhaus_slider_from_widget(w,self, min, max, step, defval, digits, feedback);
}

GtkWidget *dt_bauhaus_slider_from_widget(dt_bauhaus_widget_t* w,dt_iop_module_t *self, float min, float max,
                                                         float step, float defval, int digits, int feedback)
{
  w->type = DT_BAUHAUS_SLIDER;
  dt_bauhaus_widget_init(w, self);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->min = d->soft_min = d->hard_min = min;
  d->max = d->soft_max = d->hard_max = max;
  d->step = step;
  // normalize default:
  d->defpos = defval;
  d->pos = (defval - min) / (max - min);
  d->oldpos = d->pos;
  d->scale = 5.0f * step / (max - min);
  d->digits = digits;
  snprintf(d->format, sizeof(d->format), "%%.0%df", digits);
  d->factor = 1.0f;
  d->offset = 0.0f;

  d->grad_cnt = 0;

  d->fill_feedback = feedback;

  d->is_dragging = 0;
  d->is_changed = 0;
  d->timeout_handle = 0;
  d->curve = _default_linear_curve;

  gtk_widget_add_events(GTK_WIDGET(w), GDK_KEY_PRESS_MASK);
  gtk_widget_set_can_focus(GTK_WIDGET(w), TRUE);

  g_signal_connect(G_OBJECT(w), "button-press-event", G_CALLBACK(dt_bauhaus_slider_button_press),
                   (gpointer)NULL);
  g_signal_connect(G_OBJECT(w), "button-release-event", G_CALLBACK(dt_bauhaus_slider_button_release),
                   (gpointer)NULL);
  g_signal_connect(G_OBJECT(w), "scroll-event", G_CALLBACK(dt_bauhaus_slider_scroll), (gpointer)NULL);
  g_signal_connect(G_OBJECT(w), "key-press-event", G_CALLBACK(dt_bauhaus_slider_key_press), (gpointer)NULL);
  g_signal_connect(G_OBJECT(w), "motion-notify-event", G_CALLBACK(dt_bauhaus_slider_motion_notify),
                   (gpointer)NULL);
  g_signal_connect(G_OBJECT(w), "destroy", G_CALLBACK(dt_bauhaus_slider_destroy), (gpointer)NULL);
  return GTK_WIDGET(w);
}

static void dt_bauhaus_combobox_destroy(dt_bauhaus_widget_t *widget, gpointer user_data)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  if(w->section) g_free(w->section);
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  g_list_free_full(d->entries, free_combobox_entry);
  d->entries = NULL;
  d->num_labels = 0;
  d->active = -1;
}

GtkWidget *dt_bauhaus_combobox_new(dt_iop_module_t *self)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(g_object_new(DT_BAUHAUS_WIDGET_TYPE, NULL));
  dt_bauhaus_combobox_from_widget(w,self);
  return GTK_WIDGET(w);
}

GtkWidget *dt_bauhaus_combobox_new_action(dt_action_t *self)
{
  return dt_bauhaus_combobox_new((dt_iop_module_t *)self);
}

GtkWidget *dt_bauhaus_combobox_new_full(dt_action_t *action, const char *section, const char *label, const char *tip,
                                        int pos, GtkCallback callback, gpointer data, const char **texts)
{
  GtkWidget *combo = dt_bauhaus_combobox_new_action(action);
  dt_bauhaus_widget_set_label(combo, section, label);
  dt_bauhaus_combobox_add_list(combo, (dt_action_t *)(DT_BAUHAUS_WIDGET(combo)->module), texts);
  dt_bauhaus_combobox_set(combo, pos);
  gtk_widget_set_tooltip_text(combo, tip ? tip : _(label));
  if(callback) g_signal_connect(G_OBJECT(combo), "value-changed", G_CALLBACK(callback), data);

  return combo;
}

void dt_bauhaus_combobox_from_widget(dt_bauhaus_widget_t* w,dt_iop_module_t *self)
{
  w->type = DT_BAUHAUS_COMBOBOX;
  dt_bauhaus_widget_init(w, self);
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->entries = NULL;
  d->num_labels = 0;
  d->defpos = 0;
  d->active = -1;
  d->editable = 0;
  d->scale = 1;
  d->text_align = DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT;
  d->entries_ellipsis = PANGO_ELLIPSIZE_END;
  d->mute_scrolling = FALSE;
  memset(d->text, 0, sizeof(d->text));

  gtk_widget_add_events(GTK_WIDGET(w), GDK_KEY_PRESS_MASK);
  gtk_widget_set_can_focus(GTK_WIDGET(w), TRUE);

  g_signal_connect(G_OBJECT(w), "button-press-event", G_CALLBACK(dt_bauhaus_combobox_button_press),
                   (gpointer)NULL);
  g_signal_connect(G_OBJECT(w), "button-release-event", G_CALLBACK(dt_bauhaus_popup_button_release),
                   (gpointer)NULL);
  g_signal_connect(G_OBJECT(w), "scroll-event", G_CALLBACK(dt_bauhaus_combobox_scroll), (gpointer)NULL);
  g_signal_connect(G_OBJECT(w), "key-press-event", G_CALLBACK(dt_bauhaus_combobox_key_press), (gpointer)NULL);
  g_signal_connect(G_OBJECT(w), "motion-notify-event", G_CALLBACK(dt_bauhaus_combobox_motion_notify),
                   (gpointer)NULL);
  g_signal_connect(G_OBJECT(w), "destroy", G_CALLBACK(dt_bauhaus_combobox_destroy), (gpointer)NULL);
}

void dt_bauhaus_combobox_add_populate_fct(GtkWidget *widget, void (*fct)(GtkWidget *w, struct dt_iop_module_t **module))
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  w->combo_populate = fct;
}

void dt_bauhaus_combobox_add_list(GtkWidget *widget, dt_action_t *action, const char **texts)
{
  if(action)
    g_hash_table_insert(darktable.control->combo_list, action, texts);

  while(texts && *texts)
    dt_bauhaus_combobox_add_full(widget, _(*(texts++)), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, NULL, NULL, TRUE);
}

void dt_bauhaus_combobox_add(GtkWidget *widget, const char *text)
{
  dt_bauhaus_combobox_add_full(widget, text, DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, NULL, NULL, TRUE);
}

void dt_bauhaus_combobox_add_section(GtkWidget *widget, const char *text)
{
  dt_bauhaus_combobox_add_full(widget, text, DT_BAUHAUS_COMBOBOX_ALIGN_LEFT, NULL, NULL, FALSE);
}

void dt_bauhaus_combobox_add_aligned(GtkWidget *widget, const char *text, dt_bauhaus_combobox_alignment_t align)
{
  dt_bauhaus_combobox_add_full(widget, text, align, NULL, NULL, TRUE);
}

void dt_bauhaus_combobox_add_full(GtkWidget *widget, const char *text, dt_bauhaus_combobox_alignment_t align,
                                  gpointer data, void (free_func)(void *data), gboolean sensitive)
{
  if(darktable.control->accel_initialising) return;

  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->num_labels++;
  dt_bauhaus_combobox_entry_t *entry = new_combobox_entry(text, align, sensitive, data, free_func);
  d->entries = g_list_append(d->entries, entry);
  if(d->active < 0) d->active = 0;
}

void dt_bauhaus_combobox_set_entries_ellipsis(GtkWidget *widget, PangoEllipsizeMode ellipis)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->entries_ellipsis = ellipis;
}

PangoEllipsizeMode dt_bauhaus_combobox_get_entries_ellipsis(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return PANGO_ELLIPSIZE_END;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  return d->entries_ellipsis;
}

void dt_bauhaus_combobox_set_editable(GtkWidget *widget, int editable)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->editable = editable ? 1 : 0;
}

int dt_bauhaus_combobox_get_editable(GtkWidget *widget)
{
  const dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return 0;
  const dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  return d->editable;
}

void dt_bauhaus_combobox_set_popup_scale(GtkWidget *widget, const int scale)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->scale = scale;
}

void dt_bauhaus_combobox_set_selected_text_align(GtkWidget *widget, const dt_bauhaus_combobox_alignment_t text_align)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->text_align = text_align;
}

void dt_bauhaus_combobox_remove_at(GtkWidget *widget, int pos)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;

  if(pos < 0 || pos >= d->num_labels) return;

  // move active position up if removing anything before it
  // or when removing last position that is currently active.
  // this also sets active to -1 when removing the last remaining entry in a combobox.
  if(d->active > pos)
    d->active--;
  else if((d->active == pos) && (d->active >= d->num_labels-1))
    d->active = d->num_labels-2;

  GList *rm = g_list_nth(d->entries, pos);
  free_combobox_entry(rm->data);
  d->entries = g_list_delete_link(d->entries, rm);

  d->num_labels--;
}

void dt_bauhaus_combobox_insert(GtkWidget *widget, const char *text,int pos)
{
  dt_bauhaus_combobox_insert_full(widget, text, DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, NULL, NULL, pos);
}

void dt_bauhaus_combobox_insert_full(GtkWidget *widget, const char *text, dt_bauhaus_combobox_alignment_t align,
                                     gpointer data, void (*free_func)(void *), int pos)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->num_labels++;
  d->entries = g_list_insert(d->entries, new_combobox_entry(text, align, TRUE, data, free_func), pos);
  if(d->active < 0) d->active = 0;
}

int dt_bauhaus_combobox_length(GtkWidget *widget)
{
  const dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return 0;
  const dt_bauhaus_combobox_data_t *d = &w->data.combobox;

  return d->num_labels;
}

const char *dt_bauhaus_combobox_get_text(GtkWidget *widget)
{
  const dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return NULL;
  const dt_bauhaus_combobox_data_t *d = &w->data.combobox;

  if(d->editable && d->active < 0)
  {
    return d->text;
  }
  else
  {
    if(d->active < 0 || d->active >= d->num_labels) return NULL;
    const dt_bauhaus_combobox_entry_t *entry = (dt_bauhaus_combobox_entry_t *)g_list_nth_data(d->entries, d->active);
    return entry->label;
  }
  return NULL;
}

gpointer dt_bauhaus_combobox_get_data(GtkWidget *widget)
{
  const dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return NULL;
  const dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  const dt_bauhaus_combobox_entry_t *entry = (dt_bauhaus_combobox_entry_t *)g_list_nth_data(d->entries, d->active);
  return entry ? entry->data : NULL;
}

void dt_bauhaus_combobox_clear(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->active = -1;
  g_list_free_full(d->entries, free_combobox_entry);
  d->entries = NULL;
  d->num_labels = 0;
}

const GList *dt_bauhaus_combobox_get_entries(GtkWidget *widget)
{
  const dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return 0;
  const dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  return d->entries;
}

void dt_bauhaus_combobox_set_text(GtkWidget *widget, const char *text)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  if(!d->editable) return;
  g_strlcpy(d->text, text, sizeof(d->text));
}

static void _bauhaus_combobox_set(GtkWidget *widget, const int pos, const gboolean mute)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->active = CLAMP(pos, -1, d->num_labels - 1);
  gtk_widget_queue_draw(GTK_WIDGET(w));
  if(!darktable.gui->reset && !mute) g_signal_emit_by_name(G_OBJECT(w), "value-changed");
}

void dt_bauhaus_combobox_set(GtkWidget *widget, const int pos)
{
  _bauhaus_combobox_set(widget, pos, FALSE);
}

gboolean dt_bauhaus_combobox_set_from_text(GtkWidget *widget, const char *text)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return FALSE;
  if(!text) return FALSE;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  int i = 0;
  for(GList *iter = d->entries; iter; iter = g_list_next(iter), i++)
  {
    const dt_bauhaus_combobox_entry_t *entry = (dt_bauhaus_combobox_entry_t *)iter->data;
    if(!g_strcmp0(entry->label, text))
    {
      dt_bauhaus_combobox_set(widget, i);
      return TRUE;
    }
  }
  return FALSE;
}

gboolean dt_bauhaus_combobox_set_from_value(GtkWidget *widget, int value)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return FALSE;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  int i = 0;
  for(GList *iter = d->entries; iter; iter = g_list_next(iter), i++)
  {
    const dt_bauhaus_combobox_entry_t *entry = (dt_bauhaus_combobox_entry_t *)iter->data;
    if(GPOINTER_TO_INT(entry->data) == value)
    {
      dt_bauhaus_combobox_set(widget, i);
      return TRUE;
    }
  }
  return FALSE;
}

int dt_bauhaus_combobox_get(GtkWidget *widget)
{
  const dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return -1;
  const dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  return d->active;
}

void dt_bauhaus_combobox_entry_set_sensitive(GtkWidget *widget, int pos, gboolean sensitive)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  dt_bauhaus_combobox_entry_t *entry = (dt_bauhaus_combobox_entry_t *)g_list_nth_data(d->entries, pos);
  if(entry)
    entry->sensitive = sensitive;
}

void dt_bauhaus_slider_clear_stops(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->grad_cnt = 0;
}

void dt_bauhaus_slider_set_stop(GtkWidget *widget, float stop, float r, float g, float b)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  // need to replace stop?
  for(int k = 0; k < d->grad_cnt; k++)
  {
    if(d->grad_pos[k] == stop)
    {
      d->grad_col[k][0] = r;
      d->grad_col[k][1] = g;
      d->grad_col[k][2] = b;
      return;
    }
  }
  // new stop:
  if(d->grad_cnt < DT_BAUHAUS_SLIDER_MAX_STOPS)
  {
    int k = d->grad_cnt++;
    d->grad_pos[k] = stop;
    d->grad_col[k][0] = r;
    d->grad_col[k][1] = g;
    d->grad_col[k][2] = b;
  }
  else
  {
    fprintf(stderr, "[bauhaus_slider_set_stop] only %d stops allowed.\n", DT_BAUHAUS_SLIDER_MAX_STOPS);
  }
}


static void draw_equilateral_triangle(cairo_t *cr, float radius)
{
  const float sin = 0.866025404 * radius;
  const float cos = 0.5f * radius;
  cairo_move_to(cr, 0.0, radius);
  cairo_line_to(cr, -sin, -cos);
  cairo_line_to(cr, sin, -cos);
  cairo_line_to(cr, 0.0, radius);
}


static void dt_bauhaus_draw_indicator(dt_bauhaus_widget_t *w, float pos, cairo_t *cr, float wd, const GdkRGBA fg_color, const GdkRGBA border_color)
{
  // draw scale indicator (the tiny triangle)
  if(w->type != DT_BAUHAUS_SLIDER) return;

  const float border_width = darktable.bauhaus->border_width;
  const float size = darktable.bauhaus->marker_size;

  cairo_save(cr);
  cairo_translate(cr, slider_coordinate(pos, wd), get_line_height() + INNER_PADDING - border_width);
  cairo_scale(cr, 1.0f, -1.0f);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

  // draw the outer triangle
  draw_equilateral_triangle(cr, size);
  cairo_set_line_width(cr, border_width);
  set_color(cr, border_color);
  cairo_stroke(cr);

  draw_equilateral_triangle(cr, size - border_width);
  cairo_clip(cr);

  // draw the inner triangle
  draw_equilateral_triangle(cr, size - border_width);
  set_color(cr, fg_color);
  cairo_set_line_width(cr, border_width);

  const dt_bauhaus_slider_data_t *d = &w->data.slider;

  if(d->fill_feedback)
    cairo_fill(cr); // Plain indicator (regular sliders)
  else
    cairo_stroke(cr);  // Hollow indicator to see a color through it (gradient sliders)

  cairo_restore(cr);
}

static void dt_bauhaus_draw_quad(dt_bauhaus_widget_t *w, cairo_t *cr)
{
  GtkWidget *widget = GTK_WIDGET(w);
  const gboolean sensitive = gtk_widget_is_sensitive(GTK_WIDGET(w));
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int width = allocation.width;
  const int height = inner_height(allocation);

  if(w->quad_paint)
  {
    cairo_save(cr);

    if(sensitive && (w->quad_paint_flags & CPF_ACTIVE))
      set_color(cr, darktable.bauhaus->color_fg);
    else
      set_color(cr, darktable.bauhaus->color_fg_insensitive);

    w->quad_paint(cr, width - darktable.bauhaus->quad_width,  // x
                      0.0,                                    // y
                      darktable.bauhaus->quad_width,          // width
                      darktable.bauhaus->quad_width,          // height
                      w->quad_paint_flags, w->quad_paint_data);

    cairo_restore(cr);
  }
  else
  {
    // draw active area square:
    cairo_save(cr);
    if(sensitive)
      set_color(cr, darktable.bauhaus->color_fg);
    else
      set_color(cr, darktable.bauhaus->color_fg_insensitive);
    switch(w->type)
    {
      case DT_BAUHAUS_COMBOBOX:
        cairo_translate(cr, width - darktable.bauhaus->quad_width * .5f, height * .33f);
        draw_equilateral_triangle(cr, darktable.bauhaus->quad_width * .25f);
        cairo_fill_preserve(cr);
        cairo_set_line_width(cr, 0.5);
        set_color(cr, darktable.bauhaus->color_border);
        cairo_stroke(cr);
        break;
      case DT_BAUHAUS_SLIDER:
        break;
      default:
        cairo_set_source_rgb(cr, 1.0, 0.0, 0.0);
        cairo_rectangle(cr, width - darktable.bauhaus->quad_width, 0.0, darktable.bauhaus->quad_width, darktable.bauhaus->quad_width);
        cairo_fill(cr);
        break;
    }
    cairo_restore(cr);
  }
}

static void dt_bauhaus_draw_baseline(dt_bauhaus_widget_t *w, cairo_t *cr, float width)
{
  // draw line for orientation in slider
  if(w->type != DT_BAUHAUS_SLIDER) return;

  const float slider_width = width - darktable.bauhaus->quad_width - INNER_PADDING;
  cairo_save(cr);
  dt_bauhaus_slider_data_t *d = &w->data.slider;

  // pos of baseline
  const float htm = darktable.bauhaus->line_height + INNER_PADDING;

  // thickness of baseline
  const float htM = darktable.bauhaus->baseline_size - darktable.bauhaus->border_width;

  // the background of the line
  cairo_pattern_t *gradient = NULL;
  cairo_rectangle(cr, 0, htm, slider_width, htM);

  if(d->grad_cnt > 0)
  {
    // gradient line as used in some modules
    gradient = cairo_pattern_create_linear(0, 0, slider_width, htM);
    for(int k = 0; k < d->grad_cnt; k++)
      cairo_pattern_add_color_stop_rgba(gradient, d->grad_pos[k], d->grad_col[k][0], d->grad_col[k][1],
                                        d->grad_col[k][2], 0.4f);
    cairo_set_source(cr, gradient);
  }
  else
  {
    // regular baseline
    set_color(cr, darktable.bauhaus->color_bg);
  }

  cairo_fill(cr);

  // get the reference of the slider aka the position of the 0 value
  const float origin = fmaxf(fminf((d->factor > 0 ? -d->min - d->offset/d->factor
                                                  :  d->max + d->offset/d->factor)
                                                  / (d->max - d->min), 1.0f) * slider_width, 0.0f);
  const float position = d->pos * slider_width;
  const float delta = position - origin;

  // have a `fill ratio feel' from zero to current position
  // - but only if set
  if(d->fill_feedback)
  {
    // only brighten, useful for colored sliders to not get too faint:
    cairo_set_operator(cr, CAIRO_OPERATOR_SCREEN);
    set_color(cr, darktable.bauhaus->color_fill);
    cairo_rectangle(cr, origin, htm, delta, htM);
    cairo_fill(cr);

    // change back to default cairo operator:
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  }

  // draw the 0 reference graduation if it's different than the bounds of the slider
  const float graduation_top = htm + htM + 2.0f * darktable.bauhaus->border_width;
  const float graduation_height = darktable.bauhaus->border_width / 2.0f;
  set_color(cr, darktable.bauhaus->color_fg);

  // If the max of the slider is 180 or 360, it is likely a hue slider in degrees
  // a zero in periodic stuff has not much meaning so we skip it
  if(d->hard_max != 180.0f && d->hard_max != 360.0f)
  {
    // translate the dot if it overflows the widget frame
    if(origin < graduation_height)
      cairo_arc(cr, graduation_height, graduation_top, graduation_height, 0, 2 * M_PI);
    else if(origin > slider_width - graduation_height)
      cairo_arc(cr, slider_width - graduation_height, graduation_top, graduation_height, 0, 2 * M_PI);
    else
      cairo_arc(cr, origin, graduation_top, graduation_height, 0, 2 * M_PI);
}

  cairo_fill(cr);
  cairo_restore(cr);

  if(d->grad_cnt > 0) cairo_pattern_destroy(gradient);
}

static void dt_bauhaus_widget_reject(dt_bauhaus_widget_t *w)
{
  switch(w->type)
  {
    case DT_BAUHAUS_COMBOBOX:
      break;
    case DT_BAUHAUS_SLIDER:
    {
      dt_bauhaus_slider_data_t *d = &w->data.slider;
      dt_bauhaus_slider_set_normalized(w, d->oldpos);
    }
    break;
    default:
      break;
  }
}

static void dt_bauhaus_widget_accept(dt_bauhaus_widget_t *w)
{
  GtkWidget *widget = GTK_WIDGET(w);

  GtkAllocation allocation_popup_window;
  gtk_widget_get_allocation(darktable.bauhaus->popup_window, &allocation_popup_window);

  const int width = allocation_popup_window.width, height = inner_height(allocation_popup_window);
  const int base_width = width - darktable.bauhaus->widget_space;
  const int base_height = darktable.bauhaus->line_height + darktable.bauhaus->widget_space * 2.0f + INNER_PADDING * 2.0f;

  switch(w->type)
  {
    case DT_BAUHAUS_COMBOBOX:
    {
      // only set to what's in the filtered list.
      dt_bauhaus_combobox_data_t *d = &w->data.combobox;
      const int active = darktable.bauhaus->end_mouse_y >= 0
                 ? ((darktable.bauhaus->end_mouse_y - darktable.bauhaus->widget_space) / (darktable.bauhaus->line_height))
                 : d->active;

      int k = 0, i = 0, kk = 0, match = 1;

      gchar *keys = g_utf8_casefold(darktable.bauhaus->keys, -1);
      for(GList *it = d->entries; it; it = g_list_next(it))
      {
        const dt_bauhaus_combobox_entry_t *entry = (dt_bauhaus_combobox_entry_t *)it->data;
        gchar *text_cmp = g_utf8_casefold(entry->label, -1);
        if(!strncmp(text_cmp, keys, darktable.bauhaus->keys_cnt))
        {
          if(active == k)
          {
            if(entry->sensitive)
              dt_bauhaus_combobox_set(widget, i);
            g_free(keys);
            g_free(text_cmp);
            return;
          }
          kk = i; // remember for down there
          // editable should only snap to perfect matches, not prefixes:
          if(d->editable && strcmp(entry->label, darktable.bauhaus->keys)) match = 0;
          k++;
        }
        i++;
        g_free(text_cmp);
      }
      // didn't find it, but had only one matching choice?
      if(k == 1 && match)
        dt_bauhaus_combobox_set(widget, kk);
      else if(d->editable)
      {
        // otherwise, if combobox is editable, assume it is a custom input
        memset(d->text, 0, sizeof(d->text));
        g_strlcpy(d->text, darktable.bauhaus->keys, sizeof(d->text));
        // select custom entry
        dt_bauhaus_combobox_set(widget, -1);
      }
      g_free(keys);
      break;
    }
    case DT_BAUHAUS_SLIDER:
    {
      dt_bauhaus_slider_data_t *d = &w->data.slider;
      const float mouse_off = get_slider_line_offset(
          d->oldpos, d->scale, darktable.bauhaus->end_mouse_x / width,
          darktable.bauhaus->end_mouse_y / height, base_height / (float)height, base_width);
      dt_bauhaus_slider_set_normalized(w, d->oldpos + mouse_off);
      d->oldpos = d->pos;
      break;
    }
    default:
      break;
  }
}

static gchar *_build_label(const dt_bauhaus_widget_t *w)
{
  if(w->show_extended_label && w->section)
    return g_strdup_printf("%s - %s", w->section, w->label);
  else
    return g_strdup(w->label);
}

static gboolean dt_bauhaus_popup_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_bauhaus_widget_t *w = darktable.bauhaus->current;

  // dimensions of the popup
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int width = allocation.width - INNER_PADDING;
  const int height = inner_height(allocation);

  // dimensions of the original line
  int wd = width - darktable.bauhaus->widget_space;
  int ht = darktable.bauhaus->line_height + darktable.bauhaus->widget_space * 2.0f + INNER_PADDING * 2.0f;

  const int popwin_wd = allocation.width + darktable.bauhaus->widget_space * 2.0f;
  const int popwin_ht = allocation.height + darktable.bauhaus->widget_space * 2.0f;

  // get area properties
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                       popwin_wd, popwin_ht);

  cairo_t *cr = cairo_create(cst);
  GtkStyleContext *context = gtk_widget_get_style_context(widget);

  // look up some colors once
  GdkRGBA text_color, text_color_selected, text_color_hover, text_color_insensitive;
  gtk_style_context_get_color(context, GTK_STATE_FLAG_NORMAL, &text_color);
  gtk_style_context_get_color(context, GTK_STATE_FLAG_SELECTED, &text_color_selected);
  gtk_style_context_get_color(context, GTK_STATE_FLAG_PRELIGHT, &text_color_hover);
  gtk_style_context_get_color(context, GTK_STATE_FLAG_INSENSITIVE, &text_color_insensitive);

  GdkRGBA *fg_color = default_color_assign();
  GdkRGBA *bg_color;
  GtkStateFlags state = gtk_widget_get_state_flags(widget);

  gtk_style_context_get(context, state, "background-color", &bg_color, NULL);
  gtk_style_context_get_color(context, state, fg_color);

  // draw background
  gtk_render_background(context, cr, 0, 0, popwin_wd, popwin_ht);

  // draw border
  cairo_save(cr);
  set_color(cr, *fg_color);
  cairo_set_line_width(cr, darktable.bauhaus->widget_space);
  cairo_rectangle(cr, 0, 0, popwin_wd - 2, popwin_ht - 2);
  cairo_stroke(cr);
  cairo_restore(cr);

  // translate to account for the widget spacing
  cairo_translate(cr, darktable.bauhaus->widget_space, darktable.bauhaus->widget_space);

  // switch on bauhaus widget type (so we only need one static window)
  switch(w->type)
  {
    case DT_BAUHAUS_SLIDER:
    {
      const dt_bauhaus_slider_data_t *d = &w->data.slider;

      cairo_translate(cr, INNER_PADDING, 0);

      dt_bauhaus_draw_baseline(w, cr, wd);

      cairo_save(cr);
      cairo_set_line_width(cr, 0.5);
      const int num_scales = 1.f / d->scale;

      cairo_rectangle(cr, - INNER_PADDING, ht, width + INNER_PADDING, height);
      cairo_clip(cr);

      for(int k = 0; k < num_scales; k++)
      {
        const float off = k * d->scale - d->oldpos;
        GdkRGBA fg_copy = *fg_color;
        fg_copy.alpha = d->scale / fabsf(off);
        set_color(cr, fg_copy);
        draw_slider_line(cr, d->oldpos, off, d->scale, width, height, ht);
        cairo_stroke(cr);
      }
      cairo_restore(cr);
      set_color(cr, *fg_color);

      // draw mouse over indicator line
      cairo_save(cr);
      cairo_set_line_width(cr, 2.);
      const float mouse_off
          = darktable.bauhaus->change_active
                ? get_slider_line_offset(d->oldpos, d->scale, (darktable.bauhaus->mouse_x - INNER_PADDING)/ width,
                                         darktable.bauhaus->mouse_y / height, ht / (float)height, width)
                : 0.0f;
      draw_slider_line(cr, d->oldpos, mouse_off, d->scale, width, height, ht);
      cairo_stroke(cr);
      cairo_restore(cr);

      // draw indicator
      dt_bauhaus_draw_indicator(w, d->oldpos + mouse_off, cr, wd, *fg_color, *bg_color);

      // draw numerical value:
      cairo_save(cr);

      char *text = dt_bauhaus_slider_get_text(GTK_WIDGET(w));
      set_color(cr, *fg_color);
      const float value_width =
        show_pango_text(w, context, cr, text, wd - darktable.bauhaus->quad_width - INNER_PADDING,
                        0, 0, TRUE, FALSE, PANGO_ELLIPSIZE_END, FALSE, FALSE);
      g_free(text);

      const float label_width = width - darktable.bauhaus->quad_width - INNER_PADDING * 2.0 - value_width;
      if(label_width > 0)
      {
        gchar *lb = _build_label(w);
        show_pango_text(w, context, cr, lb, 0, 0, label_width, FALSE, FALSE, PANGO_ELLIPSIZE_END, FALSE, FALSE);
        g_free(lb);
      }
      cairo_restore(cr);
    }
    break;
    case DT_BAUHAUS_COMBOBOX:
    {
      const dt_bauhaus_combobox_data_t *d = &w->data.combobox;
      cairo_save(cr);
      float first_label_width = 0.0;
      gboolean first_label = TRUE;
      gboolean show_box_label = TRUE;
      int k = 0, i = 0;
      const int hovered = (darktable.bauhaus->mouse_y - darktable.bauhaus->widget_space) / darktable.bauhaus->line_height;
      gchar *keys = g_utf8_casefold(darktable.bauhaus->keys, -1);
      const PangoEllipsizeMode ellipsis = d->entries_ellipsis;
      ht = darktable.bauhaus->line_height;

      for(GList *it = d->entries; it; it = g_list_next(it))
      {
        const dt_bauhaus_combobox_entry_t *entry = (dt_bauhaus_combobox_entry_t *)it->data;
        gchar *text_cmp = g_utf8_casefold(entry->label, -1);
        if(!strncmp(text_cmp, keys, darktable.bauhaus->keys_cnt))
        {
          float max_width = wd - INNER_PADDING - darktable.bauhaus->quad_width;
          if(first_label) max_width *= 0.8; // give the label at least some room
          float label_width = 0.0f;
          if(!entry->sensitive)
            set_color(cr, text_color_insensitive);
          else if(i == hovered)
            set_color(cr, text_color_hover);
          else if(i == d->active)
            set_color(cr, text_color_selected);
          else
            set_color(cr, text_color);

          if(entry->alignment == DT_BAUHAUS_COMBOBOX_ALIGN_LEFT)
          {
            gchar *esc_label = g_markup_escape_text(entry->label, -1);
            gchar *label = g_strdup_printf("<b>%s</b>", esc_label);
            label_width = show_pango_text(w, context, cr, label, INNER_PADDING, ht * k + darktable.bauhaus->widget_space,
                                          max_width, FALSE, FALSE, ellipsis, TRUE, FALSE);
            g_free(label);
            g_free(esc_label);
          }
          else
            label_width
                = show_pango_text(w, context, cr, entry->label, wd - darktable.bauhaus->quad_width,
                                  ht * k + darktable.bauhaus->widget_space, max_width, TRUE, FALSE, ellipsis, FALSE, FALSE);

          // prefer the entry over the label wrt. ellipsization when expanded
          if(first_label)
          {
            show_box_label = entry->alignment == DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT;
            first_label_width = label_width;
            first_label = FALSE;
          }

          k++;
        }
        i++;
        g_free(text_cmp);
      }
      cairo_restore(cr);

      // left aligned box label. add it to the gui after the entries so we can ellipsize it if needed
      if(show_box_label)
      {
        set_color(cr, text_color);
        gchar *lb = _build_label(w);
        show_pango_text(w, context, cr, lb, INNER_PADDING, darktable.bauhaus->widget_space,
                        wd - INNER_PADDING - darktable.bauhaus->quad_width - first_label_width, FALSE, FALSE,
                        PANGO_ELLIPSIZE_END, FALSE, TRUE);
        g_free(lb);
      }
      g_free(keys);
    }
    break;
    default:
      // yell
      break;
  }

  // draw currently typed text. if a type doesn't want this, it should not
  // allow stuff to be written here in the key callback.
  const int line_height = get_line_height();
  const int size = MIN(3 * line_height, .2 * height);
  if(darktable.bauhaus->keys_cnt)
  {
    cairo_save(cr);
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoRectangle ink;
    pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);
    set_color(cr, text_color);

    // make extra large, but without dependency on popup window height
    // (that might differ for comboboxes for example). only fall back
    // to height dependency if the popup is really small.
    PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    pango_font_description_set_absolute_size(desc, size * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);

    pango_layout_set_text(layout, darktable.bauhaus->keys, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cr, wd - INNER_PADDING - darktable.bauhaus->quad_width - ink.width, height * 0.5 - size);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);
    pango_font_description_free(desc);
    g_object_unref(layout);
  }
  if(darktable.bauhaus->cursor_visible)
  {
    // show the blinking cursor
    cairo_save(cr);
    set_color(cr, text_color);
    cairo_move_to(cr, wd - darktable.bauhaus->quad_width + 3, height * 0.5 + size/3);
    cairo_line_to(cr, wd - darktable.bauhaus->quad_width + 3, height * 0.5 - size);
    cairo_set_line_width(cr, 2.);
    cairo_stroke(cr);
    cairo_restore(cr);
  }

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);

  gdk_rgba_free(bg_color);
  gdk_rgba_free(fg_color);

  return TRUE;
}

static gboolean dt_bauhaus_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  const int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  GtkStyleContext *context = gtk_widget_get_style_context(widget);

  // translate to account for the widget spacing
  cairo_translate(cr, 0, darktable.bauhaus->widget_space);

  GdkRGBA *fg_color = default_color_assign();
  GdkRGBA *bg_color;
  GdkRGBA *text_color = default_color_assign();
  const GtkStateFlags state = gtk_widget_get_state_flags(widget);
  gtk_style_context_get_color(context, state, text_color);
  gtk_render_background(context, cr, 0, 0, width, height + INNER_PADDING);
  gtk_style_context_get_color(context, state, fg_color);
  gtk_style_context_get(context, state, "background-color", &bg_color, NULL);

  // draw type specific content:
  cairo_save(cr);
  cairo_set_line_width(cr, 1.0);
  switch(w->type)
  {
    case DT_BAUHAUS_COMBOBOX:
    {
      // draw label and quad area at right end
      set_color(cr, *text_color);
      dt_bauhaus_draw_quad(w, cr);

      dt_bauhaus_combobox_data_t *d = &w->data.combobox;
      const PangoEllipsizeMode combo_ellipsis = d->entries_ellipsis;
      gchar *text = d->text;
      if(d->active >= 0)
      {
        const dt_bauhaus_combobox_entry_t *entry = g_list_nth_data(d->entries, d->active);
        text = entry->label;
      }
      set_color(cr, *text_color);

      const float available_width = width - darktable.bauhaus->quad_width - INNER_PADDING;

      //calculate total widths of label and combobox
      gchar *label_text = _build_label(w);
      const float label_width
          = show_pango_text(w, context, cr, label_text, 0, 0, 0, FALSE, TRUE, PANGO_ELLIPSIZE_END, FALSE, TRUE);
      const float combo_width
        = show_pango_text(w, context, cr, text, width - darktable.bauhaus->quad_width - INNER_PADDING, 0, 0,
                          TRUE, TRUE, combo_ellipsis, FALSE, FALSE);

      //check if they fit
      if((label_width + combo_width) > available_width)
      {
        //they don't fit: evenly divide the available width between the two in proportion
        const float ratio = label_width / (label_width + combo_width);
        show_pango_text(w, context, cr, label_text, 0, darktable.bauhaus->widget_space,
                        available_width * ratio - INNER_PADDING * 2, FALSE, FALSE, PANGO_ELLIPSIZE_END, FALSE,
                        TRUE);
        if(d->text_align == DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT)
          show_pango_text(w, context, cr, text, width - darktable.bauhaus->quad_width - INNER_PADDING, darktable.bauhaus->widget_space,
                          available_width * (1.0f - ratio),
                          TRUE, FALSE, combo_ellipsis, FALSE, FALSE);
        else
          show_pango_text(w, context, cr, text, INNER_PADDING, darktable.bauhaus->widget_space,
                          available_width * (1.0f - ratio),
                          FALSE, FALSE, combo_ellipsis, FALSE, FALSE);
      }
      else
      {
        show_pango_text(w, context, cr, label_text, 0, darktable.bauhaus->widget_space, 0, FALSE, FALSE,
                        PANGO_ELLIPSIZE_END, FALSE, TRUE);
        if(d->text_align == DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT)
          show_pango_text(w, context, cr, text, width - darktable.bauhaus->quad_width - INNER_PADDING, darktable.bauhaus->widget_space, 0,
                          TRUE, FALSE, combo_ellipsis, FALSE, FALSE);
        else
          show_pango_text(w, context, cr, text, INNER_PADDING, darktable.bauhaus->widget_space, 0,
                          FALSE, FALSE, combo_ellipsis, FALSE, FALSE);
      }
      g_free(label_text);
      break;
    }
    case DT_BAUHAUS_SLIDER:
    {
      const dt_bauhaus_slider_data_t *d = &w->data.slider;

      // line for orientation
      dt_bauhaus_draw_baseline(w, cr, width);
      dt_bauhaus_draw_quad(w, cr);

      float value_width = 0;
      if(gtk_widget_is_sensitive(widget))
      {
        cairo_save(cr);
        cairo_rectangle(cr, 0, 0, width - darktable.bauhaus->quad_width - INNER_PADDING, height + INNER_PADDING);
        cairo_clip(cr);
        dt_bauhaus_draw_indicator(w, d->pos, cr, width, *fg_color, *bg_color);
        cairo_restore(cr);

        // TODO: merge that text with combo

        char *text = dt_bauhaus_slider_get_text(widget);
        set_color(cr, *text_color);
        value_width = show_pango_text(w, context, cr, text, width - darktable.bauhaus->quad_width - INNER_PADDING,
                                      0, 0, TRUE, FALSE, PANGO_ELLIPSIZE_END, FALSE, FALSE);
        g_free(text);
      }
      // label on top of marker:
      gchar *label_text = _build_label(w);
      set_color(cr, *text_color);
      const float label_width = width - darktable.bauhaus->quad_width - INNER_PADDING - value_width;
      if(label_width > 0)
        show_pango_text(w, context, cr, label_text, 0, 0, label_width, FALSE, FALSE, PANGO_ELLIPSIZE_END, FALSE,
                        TRUE);
      g_free(label_text);
    }
    break;
    default:
      break;
  }
  cairo_restore(cr);
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);

  gdk_rgba_free(text_color);
  gdk_rgba_free(fg_color);
  gdk_rgba_free(bg_color);

  return TRUE;
}

void dt_bauhaus_hide_popup()
{
  if(darktable.bauhaus->current)
  {
    gtk_grab_remove(darktable.bauhaus->popup_window);
    gtk_widget_hide(darktable.bauhaus->popup_window);
    gtk_window_set_attached_to(GTK_WINDOW(darktable.bauhaus->popup_window), NULL);
    darktable.bauhaus->current = NULL;
    // TODO: give focus to center view? do in accept() as well?
  }
  _stop_cursor();
}

void dt_bauhaus_show_popup(dt_bauhaus_widget_t *w)
{
  if(darktable.bauhaus->current) dt_bauhaus_hide_popup();
  darktable.bauhaus->current = w;
  darktable.bauhaus->keys_cnt = 0;
  memset(darktable.bauhaus->keys, 0, sizeof(darktable.bauhaus->keys));
  darktable.bauhaus->change_active = 0;
  darktable.bauhaus->mouse_line_distance = 0.0f;
  darktable.bauhaus->hiding = FALSE;
  _stop_cursor();

  bauhaus_request_focus(w);

  gtk_widget_realize(darktable.bauhaus->popup_window);

  GtkAllocation tmp;
  gtk_widget_get_allocation(GTK_WIDGET(w), &tmp);
  if(tmp.width == 1)
  {
    if(dt_ui_panel_ancestor(darktable.gui->ui, DT_UI_PANEL_RIGHT, GTK_WIDGET(w)))
      tmp.width = dt_ui_panel_get_size(darktable.gui->ui, DT_UI_PANEL_RIGHT);
    else if(dt_ui_panel_ancestor(darktable.gui->ui, DT_UI_PANEL_LEFT, GTK_WIDGET(w)))
      tmp.width = dt_ui_panel_get_size(darktable.gui->ui, DT_UI_PANEL_LEFT);
    else
      tmp.width = 300;
    tmp.width -= INNER_PADDING * 2;
  }

  GdkWindow *widget_window = gtk_widget_get_window(GTK_WIDGET(w));

  gint wx, wy;
  GdkDevice *pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));
  if(widget_window == gdk_device_get_window_at_position(pointer, NULL, NULL))
    gdk_window_get_origin(widget_window, &wx, &wy);
  else
  {
    gdk_device_get_position(pointer, NULL, &wx, &wy);
    wx -= (tmp.width - darktable.bauhaus->quad_width) / 2;
    wy -= darktable.bauhaus->line_height / 2;
  }

  switch(darktable.bauhaus->current->type)
  {
    case DT_BAUHAUS_SLIDER:
    {
      dt_bauhaus_slider_data_t *d = &w->data.slider;
      d->oldpos = d->pos;
      tmp.height = tmp.width;
      _start_cursor(6);
      break;
    }
    case DT_BAUHAUS_COMBOBOX:
    {
      // we launch the dynamic populate fct if any
      dt_iop_module_t *module = (dt_iop_module_t *)(w->module);
      if(w->combo_populate) w->combo_populate(GTK_WIDGET(w), &module);
      // comboboxes change immediately
      darktable.bauhaus->change_active = 1;
      const dt_bauhaus_combobox_data_t *d = &w->data.combobox;
      if(!d->num_labels) return;
      tmp.height = darktable.bauhaus->line_height * d->num_labels + 5 * darktable.bauhaus->widget_space;
      tmp.width *= d->scale;

      GtkAllocation allocation_w;
      gtk_widget_get_allocation(GTK_WIDGET(w), &allocation_w);
      const int ht = allocation_w.height;
      const int skip = darktable.bauhaus->line_height;
      wy -= d->active * darktable.bauhaus->line_height;
      darktable.bauhaus->mouse_x = 0;
      darktable.bauhaus->mouse_y = d->active * skip + ht / 2;
      break;
    }
    default:
      break;
  }

  wx -= darktable.bauhaus->widget_space + INNER_PADDING;
  tmp.width += darktable.bauhaus->widget_space + INNER_PADDING;

  // gtk_widget_get_window will return null if not shown yet.
  // it is needed for gdk_window_move, and gtk_window move will
  // sometimes be ignored. this is why we always call both...
  // we also don't want to show before move, as this results in noticeable flickering.
  GdkWindow *window = gtk_widget_get_window(darktable.bauhaus->popup_window);
  if(window) gdk_window_move(window, wx, wy);
  gtk_window_move(GTK_WINDOW(darktable.bauhaus->popup_window), wx, wy);
  gtk_widget_set_size_request(darktable.bauhaus->popup_area, tmp.width, tmp.height);
  gtk_widget_set_size_request(darktable.bauhaus->popup_window, tmp.width, tmp.height);
  // gtk_window_set_keep_above isn't enough on macOS
  gtk_window_set_attached_to(GTK_WINDOW(darktable.bauhaus->popup_window), GTK_WIDGET(darktable.bauhaus->current));
  gtk_widget_show_all(darktable.bauhaus->popup_window);
  gtk_widget_grab_focus(darktable.bauhaus->popup_area);
}

static gboolean dt_bauhaus_slider_add_delta_internal(GtkWidget *widget, float delta, guint state)
{
  if (delta == 0) return TRUE;

  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  const dt_bauhaus_slider_data_t *d = &w->data.slider;

  float multiplier = 0.0f;

  if(dt_modifier_is(state, GDK_SHIFT_MASK))
  {
    multiplier = dt_conf_get_float("darkroom/ui/scale_rough_step_multiplier");
  }
  else if(dt_modifier_is(state, GDK_CONTROL_MASK))
  {
    multiplier = dt_conf_get_float("darkroom/ui/scale_precise_step_multiplier");
  }
  else
  {
    multiplier = dt_conf_get_float("darkroom/ui/scale_step_multiplier");
  }

  const float min_visible = powf(10.0f, -d->digits) / (d->max - d->min);
  if(fabsf(delta*multiplier) < min_visible)
    multiplier = min_visible / fabsf(delta);

  delta *= multiplier;

  bauhaus_request_focus(w);

  dt_bauhaus_slider_set_normalized(w, d->pos + delta);

  return TRUE;
}

static gboolean dt_bauhaus_slider_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  const dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  if(w->type != DT_BAUHAUS_SLIDER) return FALSE;

  if(dt_gui_ignore_scroll(event)) return FALSE;

  gtk_widget_grab_focus(widget);

  int delta_y;
  if(dt_gui_get_scroll_unit_delta(event, &delta_y))
  {
    if(delta_y == 0) return TRUE;
    const gdouble delta = delta_y * -w->data.slider.scale / 5.0;
    gtk_widget_set_state_flags(GTK_WIDGET(w), GTK_STATE_FLAG_FOCUSED, TRUE);
    return dt_bauhaus_slider_add_delta_internal(widget, delta, event->state);
  }

  return TRUE; // Ensure that scrolling the slider cannot move side panel
}

static gboolean dt_bauhaus_slider_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  const dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  if(w->type != DT_BAUHAUS_SLIDER) return FALSE;
  const dt_bauhaus_slider_data_t *d = &w->data.slider;

  int handled = 0;
  float delta = 0.0f;
  if(event->keyval == GDK_KEY_Up || event->keyval == GDK_KEY_KP_Up || event->keyval == GDK_KEY_Right
     || event->keyval == GDK_KEY_KP_Right)
  {
    handled = 1;
    delta = d->scale / 5.0f;
  }
  else if(event->keyval == GDK_KEY_Down || event->keyval == GDK_KEY_KP_Down || event->keyval == GDK_KEY_Left
          || event->keyval == GDK_KEY_KP_Left)
  {
    handled = 1;
    delta = -d->scale / 5.0f;
  }

  if(handled) return dt_bauhaus_slider_add_delta_internal(widget, delta, event->state);

  return FALSE;
}


static gboolean dt_bauhaus_combobox_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  if(w->type != DT_BAUHAUS_COMBOBOX) return FALSE;

  if(dt_gui_ignore_scroll(event)) return FALSE;

  const dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  gtk_widget_grab_focus(widget);

  int delta_y = 0;
  if(dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y))
  {
    bauhaus_request_focus(w);

    // go to next sensitive one
    int new_pos = CLAMP(d->active + delta_y, 0, d->num_labels - 1);
    if(_combobox_next_entry(d->entries, &new_pos, delta_y))
      dt_bauhaus_combobox_set(widget, new_pos);
    return TRUE;
  }
  return TRUE; // Ensure that scrolling the combobox cannot move side panel
}

static gboolean dt_bauhaus_combobox_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  if(w->type != DT_BAUHAUS_COMBOBOX) return FALSE;
  const dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  if(event->keyval == GDK_KEY_Up || event->keyval == GDK_KEY_KP_Up || event->keyval == GDK_KEY_Left
     || event->keyval == GDK_KEY_KP_Left)
  {
    bauhaus_request_focus(w);

    // skip insensitive ones
    int new_pos = CLAMP(d->active - 1, 0, d->num_labels - 1);
    if(_combobox_next_entry(d->entries, &new_pos, -1))
      dt_bauhaus_combobox_set(widget, new_pos);
    return TRUE;
  }
  else if(event->keyval == GDK_KEY_Down || event->keyval == GDK_KEY_KP_Down || event->keyval == GDK_KEY_Right
          || event->keyval == GDK_KEY_KP_Right)
  {
    bauhaus_request_focus(w);

    // skip insensitive ones
    int new_pos = CLAMP(d->active + 1, 0, d->num_labels - 1);
    if(_combobox_next_entry(d->entries, &new_pos, 1))
      dt_bauhaus_combobox_set(widget, new_pos);
    return TRUE;
  }
  return FALSE;
}

static gboolean dt_bauhaus_combobox_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;

  if(w->type != DT_BAUHAUS_COMBOBOX) return FALSE;
  bauhaus_request_focus(w);
  gtk_widget_grab_focus(GTK_WIDGET(w));

  GtkAllocation tmp;
  gtk_widget_get_allocation(GTK_WIDGET(w), &tmp);
  const dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  if(w->quad_paint && (event->x > allocation.width - darktable.bauhaus->quad_width - INNER_PADDING))
  {
    dt_bauhaus_widget_press_quad(widget);
    return TRUE;
  }
  else if(event->button == 3)
  {
    darktable.bauhaus->mouse_x = event->x;
    darktable.bauhaus->mouse_y = event->y;
    dt_bauhaus_show_popup(w);
    return TRUE;
  }
  else if(event->button == 1)
  {
    // reset to default.
    if(event->type == GDK_2BUTTON_PRESS)
    {
      // never called, as we popup the other window under your cursor before.
      // (except in weird corner cases where the popup is under the -1st entry
      dt_bauhaus_combobox_set(widget, d->defpos);
      dt_bauhaus_hide_popup();
    }
    else
    {
      // single click, show options
      darktable.bauhaus->opentime = dt_get_wtime();
      darktable.bauhaus->mouse_x = event->x;
      darktable.bauhaus->mouse_y = event->y;
      dt_bauhaus_show_popup(w);
    }
    return TRUE;
  }
  return FALSE;
}

float dt_bauhaus_slider_get(GtkWidget *widget)
{
  // first cast to bh widget, to check that type:
  const dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return -1.0f;
  const dt_bauhaus_slider_data_t *d = &w->data.slider;
  if(d->max == d->min)
  {
    return d->max;
  }
  const float rawval = d->curve(widget, d->pos, DT_BAUHAUS_GET);
  return d->min + rawval * (d->max - d->min);
}

float dt_bauhaus_slider_get_val(GtkWidget *widget)
{
  const dt_bauhaus_slider_data_t *d = &DT_BAUHAUS_WIDGET(widget)->data.slider;
  return dt_bauhaus_slider_get(widget) * d->factor + d->offset;
}

char *dt_bauhaus_slider_get_text(GtkWidget *w)
{
  const dt_bauhaus_slider_data_t *d = &DT_BAUHAUS_WIDGET(w)->data.slider;
  return g_strdup_printf(d->format, dt_bauhaus_slider_get_val(w));
}

void dt_bauhaus_slider_set(GtkWidget *widget, float pos)
{
  // this is the public interface function, translate by bounds and call set_normalized
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  const dt_bauhaus_slider_data_t *d = &w->data.slider;
  const float rawval = (pos - d->min) / (d->max - d->min);
  dt_bauhaus_slider_set_normalized(w, d->curve(widget, rawval, DT_BAUHAUS_SET));
}

void dt_bauhaus_slider_set_val(GtkWidget *widget, float val)
{
  const dt_bauhaus_slider_data_t *d = &DT_BAUHAUS_WIDGET(widget)->data.slider;
  dt_bauhaus_slider_set_soft(widget, (val - d->offset) / d->factor);
}

void dt_bauhaus_slider_set_digits(GtkWidget *widget, int val)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);

  if(w->type != DT_BAUHAUS_SLIDER) return;

  dt_bauhaus_slider_data_t *d = &w->data.slider;

  d->digits = val;
  snprintf(d->format, sizeof(d->format), "%%.0%df", val);
}

int dt_bauhaus_slider_get_digits(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);

  if(w->type != DT_BAUHAUS_SLIDER) return 0;

  const dt_bauhaus_slider_data_t *d = &w->data.slider;

  return d->digits;
}

void dt_bauhaus_slider_set_step(GtkWidget *widget, float val)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);

  if(w->type != DT_BAUHAUS_SLIDER) return;

  dt_bauhaus_slider_data_t *d = &w->data.slider;

  d->step = val;
  d->scale = 5.0f * d->step / (d->max - d->min);
}

float dt_bauhaus_slider_get_step(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);

  if(w->type != DT_BAUHAUS_SLIDER) return 0;

  const dt_bauhaus_slider_data_t *d = &w->data.slider;

  return d->factor < 0 ? - d->step : d->step;
}

void dt_bauhaus_slider_set_feedback(GtkWidget *widget, int feedback)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);

  if(w->type != DT_BAUHAUS_SLIDER) return;

  dt_bauhaus_slider_data_t *d = &w->data.slider;

  d->fill_feedback = feedback;

  gtk_widget_queue_draw(widget);
}

int dt_bauhaus_slider_get_feedback(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);

  if(w->type != DT_BAUHAUS_SLIDER) return 0;

  dt_bauhaus_slider_data_t *d = &w->data.slider;

  return d->fill_feedback;
}

void dt_bauhaus_slider_reset(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);

  if(w->type != DT_BAUHAUS_SLIDER) return;
  dt_bauhaus_slider_data_t *d = &w->data.slider;

  d->min = d->soft_min;
  d->max = d->soft_max;
  d->scale = 5.0f * d->step / (d->max - d->min);

  dt_bauhaus_slider_set_soft(widget, d->defpos);

  return;
}

void dt_bauhaus_slider_set_format(GtkWidget *widget, const char *format)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  g_strlcpy(d->format, format, sizeof(d->format));
}

void dt_bauhaus_slider_set_factor(GtkWidget *widget, float factor)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->factor = factor;
  if(factor < 0) d->curve = _reverse_linear_curve;
}

void dt_bauhaus_slider_set_offset(GtkWidget *widget, float offset)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->offset = offset;
}

void dt_bauhaus_slider_set_curve(GtkWidget *widget, float (*curve)(GtkWidget *self, float value, dt_bauhaus_curve_t dir))
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  if(curve == NULL) curve = _default_linear_curve;

  d->pos = curve(widget, d->curve(widget, d->pos  , DT_BAUHAUS_GET), DT_BAUHAUS_SET);

  d->curve = curve;
}

void dt_bauhaus_slider_set_soft(GtkWidget *widget, float pos)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  const float rpos = CLAMP(pos, d->hard_min, d->hard_max);
  d->min = MIN(d->min, rpos);
  d->max = MAX(d->max, rpos);
  d->scale = 5.0f * d->step / (d->max - d->min);
  dt_bauhaus_slider_set(widget, rpos);
}

static gboolean dt_bauhaus_slider_postponed_value_change(gpointer data)
{
  if(!GTK_IS_WIDGET(data)) return 0;

  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)data;
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  if(d->is_changed)
  {
    g_signal_emit_by_name(G_OBJECT(w), "value-changed");
    d->is_changed = 0;
    return TRUE;
  }
  else
  {
    d->timeout_handle = 0;
    return FALSE;
  }
}

static void dt_bauhaus_slider_set_normalized(dt_bauhaus_widget_t *w, float pos)
{
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  float rpos = CLAMP(pos, 0.0f, 1.0f);
  rpos = d->curve(GTK_WIDGET(w), rpos, DT_BAUHAUS_GET);
  rpos = d->min + (d->max - d->min) * rpos;
  const float base = powf(10.0f, d->digits);
  rpos = roundf(base * rpos) / base;
  rpos = (rpos - d->min) / (d->max - d->min);
  d->pos = d->curve(GTK_WIDGET(w), rpos, DT_BAUHAUS_SET);
  gtk_widget_queue_draw(GTK_WIDGET(w));
  d->is_changed = 1;
  if(!darktable.gui->reset)
  {
    if(!d->is_dragging)
    {
      g_signal_emit_by_name(G_OBJECT(w), "value-changed");
      d->is_changed = 0;
    }
    else
    {
      if(!d->timeout_handle)
      {
        const int delay = CLAMP(darktable.develop->average_delay * 3 / 2, DT_BAUHAUS_SLIDER_VALUE_CHANGED_DELAY_MIN,
                                DT_BAUHAUS_SLIDER_VALUE_CHANGED_DELAY_MAX);
        d->timeout_handle = g_timeout_add(delay, dt_bauhaus_slider_postponed_value_change, w);
      }
    }
  }
}

static gboolean dt_bauhaus_popup_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  switch(darktable.bauhaus->current->type)
  {
    case DT_BAUHAUS_SLIDER:
    {
      // hack to do screenshots from popup:
      // if(event->string[0] == 'p') return system("scrot");
      // else
      if(darktable.bauhaus->keys_cnt + 2 < 64
         && (event->keyval == GDK_KEY_space || event->keyval == GDK_KEY_KP_Space ||              // SPACE
             event->keyval == GDK_KEY_percent ||                                                 // %
             (event->string[0] >= 40 && event->string[0] <= 57) ||                               // ()+-*/.,0-9
             event->keyval == GDK_KEY_asciicircum || event->keyval == GDK_KEY_dead_circumflex || // ^
             event->keyval == GDK_KEY_X || event->keyval == GDK_KEY_x))                          // Xx
      {
        if(event->keyval == GDK_KEY_dead_circumflex)
          darktable.bauhaus->keys[darktable.bauhaus->keys_cnt++] = '^';
        else
          darktable.bauhaus->keys[darktable.bauhaus->keys_cnt++] = event->string[0];
        gtk_widget_queue_draw(darktable.bauhaus->popup_area);
      }
      else if(darktable.bauhaus->keys_cnt > 0
              && (event->keyval == GDK_KEY_BackSpace || event->keyval == GDK_KEY_Delete))
      {
        darktable.bauhaus->keys[--darktable.bauhaus->keys_cnt] = 0;
        gtk_widget_queue_draw(darktable.bauhaus->popup_area);
      }
      else if(darktable.bauhaus->keys_cnt > 0 && darktable.bauhaus->keys_cnt + 1 < 64
              && (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter))
      {
        // accept input
        darktable.bauhaus->keys[darktable.bauhaus->keys_cnt] = 0;
        // unnormalized input, user was typing this:
        const float old_value = dt_bauhaus_slider_get_val(GTK_WIDGET(darktable.bauhaus->current));
        const float new_value = dt_calculator_solve(old_value, darktable.bauhaus->keys);
        if(isfinite(new_value)) dt_bauhaus_slider_set_val(GTK_WIDGET(darktable.bauhaus->current), new_value);
        darktable.bauhaus->keys_cnt = 0;
        memset(darktable.bauhaus->keys, 0, sizeof(darktable.bauhaus->keys));
        dt_bauhaus_hide_popup();
      }
      else if(event->keyval == GDK_KEY_Escape)
      {
        // discard input and close popup
        darktable.bauhaus->keys_cnt = 0;
        memset(darktable.bauhaus->keys, 0, sizeof(darktable.bauhaus->keys));
        dt_bauhaus_hide_popup();
      }
      else
        return FALSE;
      if(darktable.bauhaus->keys_cnt > 0) _start_cursor(-1);
      return TRUE;
    }
    case DT_BAUHAUS_COMBOBOX:
    {
      if(!g_utf8_validate(event->string, -1, NULL)) return FALSE;
      const gunichar c = g_utf8_get_char(event->string);
      const long int char_width = g_utf8_next_char(event->string) - event->string;
      // if(event->string[0] == 'p') return system("scrot");
      // else
      if(darktable.bauhaus->keys_cnt + 1 + char_width < 64 && g_unichar_isprint(c))
      {
        // only accept key input if still valid or editable?
        g_utf8_strncpy(darktable.bauhaus->keys + darktable.bauhaus->keys_cnt, event->string, 1);
        darktable.bauhaus->keys_cnt += char_width;
        gtk_widget_queue_draw(darktable.bauhaus->popup_area);
      }
      else if(darktable.bauhaus->keys_cnt > 0
              && (event->keyval == GDK_KEY_BackSpace || event->keyval == GDK_KEY_Delete))
      {
        darktable.bauhaus->keys_cnt
            -= (darktable.bauhaus->keys + darktable.bauhaus->keys_cnt)
               - g_utf8_prev_char(darktable.bauhaus->keys + darktable.bauhaus->keys_cnt);
        darktable.bauhaus->keys[darktable.bauhaus->keys_cnt] = 0;
        gtk_widget_queue_draw(darktable.bauhaus->popup_area);
      }
      else if(darktable.bauhaus->keys_cnt > 0 && darktable.bauhaus->keys_cnt + 1 < 64
              && (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter))
      {
        // accept unique matches only for editable:
        if(darktable.bauhaus->current->data.combobox.editable)
          darktable.bauhaus->end_mouse_y = FLT_MAX;
        else
          darktable.bauhaus->end_mouse_y = 0;
        darktable.bauhaus->keys[darktable.bauhaus->keys_cnt] = 0;
        dt_bauhaus_widget_accept(darktable.bauhaus->current);
        darktable.bauhaus->keys_cnt = 0;
        memset(darktable.bauhaus->keys, 0, sizeof(darktable.bauhaus->keys));
        dt_bauhaus_hide_popup();
      }
      else if(event->keyval == GDK_KEY_Escape)
      {
        // discard input and close popup
        darktable.bauhaus->keys_cnt = 0;
        memset(darktable.bauhaus->keys, 0, sizeof(darktable.bauhaus->keys));
        dt_bauhaus_hide_popup();
      }
      else if(event->keyval == GDK_KEY_Up)
      {
        combobox_popup_scroll(-1);
      }
      else if(event->keyval == GDK_KEY_Down)
      {
        combobox_popup_scroll(1);
      }
      else if(event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter)
      {
        // return pressed, but didn't type anything
        darktable.bauhaus->end_mouse_y = -1; // negative will use currently highlighted instead.
        darktable.bauhaus->keys[darktable.bauhaus->keys_cnt] = 0;
        darktable.bauhaus->keys_cnt = 0;
        memset(darktable.bauhaus->keys, 0, sizeof(darktable.bauhaus->keys));
        dt_bauhaus_widget_accept(darktable.bauhaus->current);
        dt_bauhaus_hide_popup();
      }
      else
        return FALSE;
      return TRUE;
    }
    default:
      return FALSE;
  }
}

static gboolean dt_bauhaus_slider_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  bauhaus_request_focus(w);
  gtk_widget_grab_focus(GTK_WIDGET(w));

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  if(event->x > allocation.width - darktable.bauhaus->quad_width - INNER_PADDING)
  {
    dt_bauhaus_widget_press_quad(widget);
    return TRUE;
  }
  else if(event->button == 3)
  {
    dt_bauhaus_show_popup(w);
    return TRUE;
  }
  else if(event->button == 1)
  {
    // reset to default.
    if(event->type == GDK_2BUTTON_PRESS)
    {
      dt_bauhaus_slider_data_t *d = &w->data.slider;
      d->is_dragging = 0;
      dt_bauhaus_slider_reset(GTK_WIDGET(w));
    }
    else
    {
      const float l = 0.0f;
      const float r = slider_right_pos((float)allocation.width);
      dt_bauhaus_slider_set_normalized(w, (event->x / allocation.width - l) / (r - l));
      dt_bauhaus_slider_data_t *d = &w->data.slider;
      d->is_dragging = 1;
    }
    return TRUE;
  }
  return FALSE;
}

static gboolean dt_bauhaus_slider_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  dt_bauhaus_slider_data_t *d = &w->data.slider;

  dt_bauhaus_widget_release_quad(widget);
  if((event->button == 1) && (d->is_dragging))
  {
    bauhaus_request_focus(w);

    GtkAllocation tmp;
    gtk_widget_get_allocation(GTK_WIDGET(w), &tmp);
    d->is_dragging = 0;
    if(d->timeout_handle) g_source_remove(d->timeout_handle);
    d->timeout_handle = 0;
    const float l = 0.0f;
    const float r = slider_right_pos((float)tmp.width);
    dt_bauhaus_slider_set_normalized(w, (event->x / tmp.width - l) / (r - l));

    return TRUE;
  }
  return FALSE;
}

static gboolean dt_bauhaus_slider_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  dt_bauhaus_slider_data_t *d = &w->data.slider;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  if(d->is_dragging || event->x <= allocation.width - darktable.bauhaus->quad_width)
  {
    // remember mouse position for motion effects in draw
    if(event->state & GDK_BUTTON1_MASK && event->type != GDK_2BUTTON_PRESS)
    {
      bauhaus_request_focus(w);
      const float l = 0.0f;
      const float r = slider_right_pos((float)allocation.width);
      dt_bauhaus_slider_set_normalized(w, (event->x / allocation.width - l) / (r - l));
    }
    darktable.control->element = event->x > (0.1 * (allocation.width - darktable.bauhaus->quad_width)) &&
                                 event->x < (0.9 * (allocation.width - darktable.bauhaus->quad_width))
                               ? DT_ACTION_ELEMENT_VALUE
                               : DT_ACTION_ELEMENT_FORCE;
  }
  else
    darktable.control->element = DT_ACTION_ELEMENT_BUTTON;

  return TRUE;
}

static gboolean dt_bauhaus_combobox_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);

  darktable.control->element = event->x <= allocation.width - darktable.bauhaus->quad_width
                             ? DT_ACTION_ELEMENT_SELECTION
                             : DT_ACTION_ELEMENT_BUTTON;

  return TRUE;
}


void dt_bauhaus_vimkey_exec(const char *input)
{
  dt_action_t *ac = darktable.control->actions_iops.target;
  input += 5; // skip ":set "

  while(ac)
  {
    const int prefix = strcspn(input, ".=");

    if(ac->type >= DT_ACTION_TYPE_WIDGET ||
       ac->type <= DT_ACTION_TYPE_SECTION)
    {
      if(!strncasecmp(ac->label, input, prefix))
      {
        if(!ac->label[prefix])
        {
          input += prefix;
          if(*input) input++; // skip . or =

          if(ac->type <= DT_ACTION_TYPE_SECTION)
          {
            ac = ac->target;
            continue;
          }
          else
            break;
        }
      }
    }

    ac = ac->next;
  }

  if(!ac || ac->type != DT_ACTION_TYPE_WIDGET || !ac->target || !DT_IS_BAUHAUS_WIDGET(ac->target))
    return;

  float old_value = .0f, new_value = .0f;

  GtkWidget *w = ac->target;

  switch(DT_BAUHAUS_WIDGET(w)->type)
  {
    case DT_BAUHAUS_SLIDER:
      old_value = dt_bauhaus_slider_get(w);
      new_value = dt_calculator_solve(old_value, input);
      fprintf(stderr, " = %f\n", new_value);
      if(isfinite(new_value)) dt_bauhaus_slider_set_soft(w, new_value);
      break;
    case DT_BAUHAUS_COMBOBOX:
      // TODO: what about text as entry?
      old_value = dt_bauhaus_combobox_get(w);
      new_value = dt_calculator_solve(old_value, input);
      fprintf(stderr, " = %f\n", new_value);
      if(isfinite(new_value)) dt_bauhaus_combobox_set(w, new_value);
      break;
    default:
      break;
  }
}

// give autocomplete suggestions
GList *dt_bauhaus_vimkey_complete(const char *input)
{
  GList *res = NULL;

  dt_action_t *ac = darktable.control->actions_iops.target;

  while(ac)
  {
    const int prefix = strcspn(input, ".");

    if(ac->type >= DT_ACTION_TYPE_WIDGET ||
       ac->type <= DT_ACTION_TYPE_SECTION)
    {
      if(!prefix || !strncasecmp(ac->label, input, prefix))
      {
        if(!ac->label[prefix] && input[prefix] == '.')
        {
            input += prefix + 1;
          if(ac->type <= DT_ACTION_TYPE_SECTION) ac = ac->target;
          continue;
        }
        else
          res = g_list_append(res, (gchar *)ac->label + prefix);
      }
    }

    ac = ac->next;
  }
  return res;
}

void dt_bauhaus_combobox_mute_scrolling(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->mute_scrolling = TRUE;
}

static float _action_process_slider(gpointer target, dt_action_element_t element, dt_action_effect_t effect, float move_size)
{
  GtkWidget *widget = GTK_WIDGET(target);
  dt_bauhaus_widget_t *bhw = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &bhw->data.slider;
  const float value = dt_bauhaus_slider_get(widget);
  const float min_visible = powf(10.0f, -dt_bauhaus_slider_get_digits(widget));

  if(!isnan(move_size))
  {
    switch(element)
    {
    case DT_ACTION_ELEMENT_VALUE:
    case DT_ACTION_ELEMENT_FORCE:
      switch(effect)
      {
      case DT_ACTION_EFFECT_POPUP:
        dt_bauhaus_show_popup(DT_BAUHAUS_WIDGET(widget));
        break;
      case DT_ACTION_EFFECT_DOWN:
        move_size *= -1;
      case DT_ACTION_EFFECT_UP:
        d->is_dragging = 1;
        const float step = dt_bauhaus_slider_get_step(widget);
        float multiplier = dt_accel_get_slider_scale_multiplier();

        if(move_size && fabsf(move_size * step * multiplier) < min_visible)
          multiplier = min_visible / fabsf(move_size * step);

        if(element == DT_ACTION_ELEMENT_FORCE)
        {
          if(d->pos < 0.0001) d->min = d->soft_min;
          if(d->pos > 0.9999) d->max = d->soft_max;
          dt_bauhaus_slider_set_soft(widget, value + move_size * step * multiplier);
        }
        else
          dt_bauhaus_slider_set(widget, value + move_size * step * multiplier);
        d->is_dragging = 0;
        break;
      case DT_ACTION_EFFECT_RESET:
        dt_bauhaus_slider_reset(widget);
        break;
      case DT_ACTION_EFFECT_TOP:
        dt_bauhaus_slider_set_soft(widget, element == DT_ACTION_ELEMENT_FORCE ? d->hard_max: d->max);
        break;
      case DT_ACTION_EFFECT_BOTTOM:
        dt_bauhaus_slider_set_soft(widget, element == DT_ACTION_ELEMENT_FORCE ? d->hard_min: d->min);
        break;
      case DT_ACTION_EFFECT_SET:
        dt_bauhaus_slider_set_soft(widget, move_size);
        break;
      default:
        fprintf(stderr, "[_action_process_slider] unknown shortcut effect (%d) for slider\n", effect);
        break;
      }

      gchar *text = dt_bauhaus_slider_get_text(widget);
      dt_action_widget_toast(bhw->module, widget, text);
      g_free(text);

      break;
    case DT_ACTION_ELEMENT_BUTTON:
      dt_bauhaus_widget_press_quad(widget);
      break;
    case DT_ACTION_ELEMENT_ZOOM:
      switch(effect)
      {
      case DT_ACTION_EFFECT_POPUP:
        dt_bauhaus_show_popup(DT_BAUHAUS_WIDGET(widget));
        break;
      case DT_ACTION_EFFECT_DOWN:
        move_size *= -1;
      case DT_ACTION_EFFECT_UP:
        if(d->soft_min != d->hard_min || d->soft_max != d->hard_max)
        {
          const float multiplier = powf(2.0f, move_size/2);
          const float new_min = value - multiplier * (value - d->min);
          const float new_max = value + multiplier * (d->max - value);
          if(new_min >= d->hard_min
             && new_max <= d->hard_max
             && new_max - new_min >= min_visible * 10)
          {
            d->min = new_min;
            d->max = new_max;
          }
        }
        break;
      case DT_ACTION_EFFECT_RESET:
        d->min = d->soft_min;
        d->max = d->soft_max;
        break;
      case DT_ACTION_EFFECT_TOP:
        d->max = d->hard_max;
        break;
      case DT_ACTION_EFFECT_BOTTOM:
        d->min = d->hard_min;
        break;
      default:
        fprintf(stderr, "[_action_process_slider] unknown shortcut effect (%d) for slider\n", effect);
        break;
      }
      dt_bauhaus_slider_set_soft(widget, value); // restore value (and move min/max again if needed)

      gtk_widget_queue_draw(GTK_WIDGET(widget));
      dt_toast_log(("[%f , %f]"), d->min, d->max);

      break;
    default:
      fprintf(stderr, "[_action_process_slider] unknown shortcut element (%d) for slider\n", element);
      break;
    }
  }

  if(effect == DT_ACTION_EFFECT_SET)
    return dt_bauhaus_slider_get(widget);

  return d->pos +
         ( d->min == -d->max                             ? DT_VALUE_PATTERN_PLUS_MINUS :
         ( d->min == 0 && (d->max == 1 || d->max == 100) ? DT_VALUE_PATTERN_PERCENTAGE : 0 ));
}

gboolean combobox_idle_value_changed(gpointer widget)
{
  g_signal_emit_by_name(G_OBJECT(widget), "value-changed");

  while(g_idle_remove_by_data(widget));

  return FALSE;
}

static float _action_process_combo(gpointer target, dt_action_element_t element, dt_action_effect_t effect, float move_size)
{
  GtkWidget *widget = GTK_WIDGET(target);
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  int value = dt_bauhaus_combobox_get(widget);

  if(!isnan(move_size))
  {
    if(element == DT_ACTION_ELEMENT_BUTTON)
      dt_bauhaus_widget_press_quad(widget);
    else switch(effect)
    {
    case DT_ACTION_EFFECT_POPUP:
      dt_bauhaus_show_popup(DT_BAUHAUS_WIDGET(widget));
      break;
    case DT_ACTION_EFFECT_LAST:
      move_size *= - 1; // reversed in effect_previous
    case DT_ACTION_EFFECT_FIRST:
      move_size *= 1e3; // reversed in effect_previous
    case DT_ACTION_EFFECT_PREVIOUS:
      move_size *= - 1;
    case DT_ACTION_EFFECT_NEXT:
      value = CLAMP(value + move_size, 0, dt_bauhaus_combobox_length(widget) - 1);

      if(_combobox_next_entry(w->data.combobox.entries, &value, move_size > 0 ? 1 : -1))
      {
        ++darktable.gui->reset;
        dt_bauhaus_combobox_set(widget, value);
        --darktable.gui->reset;
      }

      g_idle_add(combobox_idle_value_changed, widget);
      break;
    case DT_ACTION_EFFECT_RESET:
      value = dt_bauhaus_combobox_get_default(widget);
      dt_bauhaus_combobox_set(widget, value);
      break;
    default:
      value = effect - DT_ACTION_EFFECT_COMBO_SEPARATOR - 1;
      dt_bauhaus_combobox_set(widget, value);
      break;
    }

    gchar *text = g_strdup_printf("\n%s", dt_bauhaus_combobox_get_text(widget));
    dt_action_widget_toast(w->module, widget, text);
    g_free(text);
  }

  GList *e = w->data.combobox.entries;
  for(int above = value; above && e; above--, e = e->next)
  {
    dt_bauhaus_combobox_entry_t *entry = e->data;
    if(entry && ! entry->sensitive) value--; // don't count unselectable combo items in value
  }
  return - 1 - value + (value == effect - DT_ACTION_EFFECT_COMBO_SEPARATOR - 1 ? DT_VALUE_PATTERN_ACTIVE : 0);
}

const dt_action_element_def_t _action_elements_slider[]
  = { { N_("value"), dt_action_effect_value },
      { N_("button"), dt_action_effect_toggle },
      { N_("force"), dt_action_effect_value },
      { N_("zoom"), dt_action_effect_value },
      { NULL } };
const dt_action_element_def_t _action_elements_combo[]
  = { { N_("selection"), dt_action_effect_selection },
      { N_("button"), dt_action_effect_toggle },
      { NULL } };

static const dt_shortcut_fallback_t _action_fallbacks_slider[]
  = { { .element = DT_ACTION_ELEMENT_BUTTON, .button = DT_SHORTCUT_LEFT },
      { .element = DT_ACTION_ELEMENT_BUTTON, .effect = DT_ACTION_EFFECT_TOGGLE_CTRL, .button = DT_SHORTCUT_LEFT, .mods = GDK_CONTROL_MASK },
      { .element = DT_ACTION_ELEMENT_FORCE,  .mods   = GDK_CONTROL_MASK | GDK_SHIFT_MASK, .speed = 10.0 },
      { .element = DT_ACTION_ELEMENT_ZOOM,   .effect = DT_ACTION_EFFECT_DEFAULT_MOVE, .button = DT_SHORTCUT_RIGHT, .move = DT_SHORTCUT_MOVE_VERTICAL },
      { } };
static const dt_shortcut_fallback_t _action_fallbacks_combo[]
  = { { .element = DT_ACTION_ELEMENT_SELECTION, .effect = DT_ACTION_EFFECT_RESET, .button = DT_SHORTCUT_LEFT, .click = DT_SHORTCUT_DOUBLE },
      { .element = DT_ACTION_ELEMENT_BUTTON,    .button = DT_SHORTCUT_LEFT },
      { .element = DT_ACTION_ELEMENT_BUTTON,    .effect = DT_ACTION_EFFECT_TOGGLE_CTRL, .button = DT_SHORTCUT_LEFT, .mods = GDK_CONTROL_MASK },
      { .move    = DT_SHORTCUT_MOVE_SCROLL,     .effect = DT_ACTION_EFFECT_DEFAULT_MOVE, .speed = -1 },
      { .move    = DT_SHORTCUT_MOVE_VERTICAL,   .effect = DT_ACTION_EFFECT_DEFAULT_MOVE, .speed = -1 },
      { } };

const dt_action_def_t dt_action_def_slider
  = { N_("slider"),
      _action_process_slider,
      _action_elements_slider,
      _action_fallbacks_slider };
const dt_action_def_t dt_action_def_combo
  = { N_("dropdown"),
      _action_process_combo,
      _action_elements_combo,
      _action_fallbacks_combo };

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
