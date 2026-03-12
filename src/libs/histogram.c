/*
    This file is part of darktable,
    Copyright (C) 2011-2026 darktable developers.

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
#include "common/color_picker.h"
#include "gui/accelerators.h"
#include "scopes.h"

// FIXME: is this used?
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

DT_MODULE(1)

static const gchar *rgb_names[DT_SCOPES_RGB_N] =
  { N_("red"),
    N_("green"),
    N_("blue")
  };


const char *name(dt_lib_module_t *self)
{
  return _("scopes");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_DARKROOM | DT_VIEW_TETHERING;
}

uint32_t container(dt_lib_module_t *self)
{
  return g_strcmp0
    (dt_conf_get_string_const("plugins/darkroom/histogram/panel_position"), "right")
    ? DT_UI_CONTAINER_PANEL_LEFT_TOP
    : DT_UI_CONTAINER_PANEL_RIGHT_TOP;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position(const dt_lib_module_t *self)
{
  return 1000;
}



static void _scope_process
  (struct dt_lib_module_t *self,
   const float *const input,
   int width,
   int height,
   const dt_iop_order_iccprofile_info_t *const profile_info_from,
   const dt_iop_order_iccprofile_info_t *const profile_info_to)
{
  dt_times_t start;
  dt_get_perf_times(&start);

  dt_scopes_t *const s = self->data;

  // special case, clear the scopes
  if(!input)
  {
    dt_pthread_mutex_lock(&s->lock);
    // FIXME: is better to do this or just advance update_counter by one?
    for(dt_scopes_mode_type_t i = 0; i < DT_SCOPES_MODE_N; i++)
      dt_scopes_call_if_exists(&s->modes[i], clear);
    dt_pthread_mutex_unlock(&s->lock);
    return;
  }

  // FIXME: scope goes black when click histogram lib colorpicker on
  // -- is this meant to happen?
  //
  // FIXME: scope doesn't redraw when click histogram lib colorpicker
  // off -- is this meant to happen?
  dt_histogram_roi_t roi = { .width = width,
                             .height = height,
                             .crop_x = 0,
                             .crop_y = 0,
                             .crop_right = 0,
                             .crop_bottom = 0 };

  // Constraining the area if the colorpicker is active in area mode
  //
  // FIXME: only need to do colorspace conversion below on roi
  //
  // FIXME: if the only time we use roi in histogram to limit area is
  // here, and whenever we use tether there is no colorpicker (true?),
  // and if we're always doing a colorspace transform in darkroom and
  // clip to roi during conversion, then can get rid of all roi code
  // for common/histogram?  when darkroom colorpicker is active,
  // gui_module is set to colorout
  if(dt_view_get_current() == DT_VIEW_DARKROOM
     && darktable.lib->proxy.colorpicker.restrict_histogram)
  {
    const dt_colorpicker_sample_t *const sample =
      darktable.lib->proxy.colorpicker.primary_sample;
    const dt_iop_color_picker_t *proxy = darktable.lib->proxy.colorpicker.picker_proxy;
    if(proxy && !proxy->module)
    {
      // FIXME: for histogram process whole image, then pull point
      // sample #'s from primary_picker->scope_mean (point) or _mean,
      // _min, _max (as in rgb curve) and draw them as an overlay
      //
      // FIXME: for waveform point sample, could process whole image,
      // then do an overlay of the point sample from
      // primary_picker->scope_mean as red/green/blue dots (or short
      // lines) at appropriate position at the horizontal/vertical
      // position of sample
      dt_boundingbox_t pos;
      const gboolean isbox = sample->size == DT_LIB_COLORPICKER_SIZE_BOX;
      const gboolean ispoint = sample->size == DT_LIB_COLORPICKER_SIZE_POINT;
      if(ispoint || isbox)
      {
        dt_color_picker_transform_box(darktable.develop,
                                     isbox ? 2 : 1,
                                     isbox ? sample->box : sample->point,
                                     pos, TRUE);
        roi.crop_x = MIN(width, MAX(0, pos[0] * width));
        roi.crop_y = MIN(height, MAX(0, pos[1] * height));
        roi.crop_right = width -    MIN(width,  MAX(0, (isbox ? pos[2] : pos[0]) * width));
        roi.crop_bottom = height -  MIN(height, MAX(0, (isbox ? pos[3] : pos[1]) * height));
      }
    }
  }

  // Convert pixelpipe output in display RGB to histogram profile. If
  // in tether view, then the image is already converted by the
  // caller.

  float *img_display = dt_alloc_align_float((size_t)4 * width * height);
  if(!img_display) return;

  // FIXME: we might get called with profile_info_to == NULL due to caller errors
  if(!profile_info_to)
  {
    dt_print(DT_DEBUG_ALWAYS,
       "[histogram] no histogram profile, replaced with linear Rec2020");
    dt_control_log(_("unsupported profile selected for histogram,"
                     " it will be replaced with linear Rec2020"));
  }

  const dt_iop_order_iccprofile_info_t *fallback =
    dt_ioppr_add_profile_info_to_list(darktable.develop,
      DT_COLORSPACE_LIN_REC2020, "", DT_INTENT_RELATIVE_COLORIMETRIC);

  const dt_iop_order_iccprofile_info_t *profile_info_out = !profile_info_to ? fallback : profile_info_to;

  dt_ioppr_transform_image_colorspace_rgb(input, img_display, width, height,
                                            profile_info_from, profile_info_out, "final histogram");
  dt_pthread_mutex_lock(&s->lock);

  s->update_counter++;
  // if using a non-rgb profile_info_out as in cmyk softproofing we pass DT_COLORSPACE_LIN_REC2020
  //   for calculating the vertex_rgb data.
  dt_scopes_call(s->cur_mode, process, img_display, &roi,
                 profile_info_out->type ? profile_info_out : fallback);

  dt_pthread_mutex_unlock(&s->lock);
  dt_free_align(img_display);

  dt_show_times_f(&start, "[histogram]", "final %s",
                  dt_scopes_call(s->cur_mode, name));
}


// FIXME: make this default in _drawable_draw_callback() and only if there is a draw_bkgd method do somethign else?
void lib_histogram_draw_bkgd(const dt_scopes_mode_t *const self,
                             cairo_t *cr,
                             const int width,
                             const int height)
{
  cairo_save(cr);
  cairo_rectangle(cr, 0, 0, width, height);
  set_color(cr, darktable.bauhaus->graph_bg);
  cairo_fill(cr);
  cairo_restore(cr);
}

// FIXME: have different drawable for each scope in a stack --
// simplifies this function from being a swath of conditionals -- then
// essentially draw callbacks _lib_histogram_draw_waveform,
// _lib_histogram_draw_rgb_parade, etc.
//
// FIXME: if exposure change regions are separate widgets, then we
// could have a menu to swap in different overlay widgets (sort of
// like basic adjustments) to adjust other things about the image,
// e.g. tone equalizer, color balance, etc.
static gboolean _drawable_draw_callback(GtkWidget *widget,
                                        cairo_t *crf,
                                        dt_scopes_t *s)
{
  dt_times_t start;
  dt_get_perf_times(&start);

  dt_scopes_mode_t *const cur_mode = s->cur_mode;
  const dt_develop_t *const dev = darktable.develop;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int width = allocation.width, height = allocation.height;

  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  gtk_render_background(gtk_widget_get_style_context(widget), cr, 0, 0, width, height);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(.5)); // borders width

  dt_scopes_call_if_exists(cur_mode, draw_bkgd, cr, width, height);

  // exposure change regions
  // FIXME: should draw these if there is no data currently for this scope?
  set_color(cr, darktable.bauhaus->graph_overlay);
  dt_scopes_call_if_exists(cur_mode, draw_highlight,
                           cr, s->highlight, width, height);

  // draw grid
  // FIXME: set this in individual draw code
  set_color(cr, darktable.bauhaus->graph_grid);
  dt_scopes_call_if_exists(cur_mode, draw_grid, cr, width, height);

  // FIXME: should set histogram buffer to black if have just entered
  // tether view and nothing is displayed
  dt_pthread_mutex_lock(&s->lock);
  // darkroom view: draw scope so long as preview pipe is finished
  // tether view: draw whatever has come in from tether
  if((dt_view_get_current() == DT_VIEW_TETHERING
      || dev->image_storage.id == dev->preview_pipe->output_imgid)
     && (cur_mode->update_counter == s->update_counter))
  {
    if(dt_scopes_func_exists(cur_mode, draw_scope_channels))
      dt_scopes_call(cur_mode, draw_scope_channels,
                     cr, width, height, s->channels);
    else
      dt_scopes_call(cur_mode, draw_scope, cr, width, height);
  }
  dt_pthread_mutex_unlock(&s->lock);

  // finally a thin border
  cairo_rectangle(cr, 0, 0, width, height);
  set_color(cr, darktable.bauhaus->graph_border);
  cairo_stroke(cr);

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);

  dt_show_times_f(&start, "[histogram]", "scope draw");
  return FALSE;
}

void lib_histogram_update_tooltip(const dt_scopes_t *const scopes)
{
  const char *const scope_name = dt_scopes_call(scopes->cur_mode, name);
  gchar *tip = g_strdup_printf("%s\n(%s)\n%s\n%s",
                               _(scope_name),
                               _("use buttons at top of graph to change type"),
                               _("click on ❓ and then graph for documentation"),
                               _("use color picker module to restrict area"));
  if(scopes->highlight == DT_SCOPES_HIGHLIGHT_BLACK_POINT)
    dt_util_str_cat(&tip, "\n%s\n%s",
                          _("drag to change black point"),
                          _("double-click resets"));
  if(scopes->highlight == DT_SCOPES_HIGHLIGHT_EXPOSURE)
    dt_util_str_cat(&tip, "\n%s\n%s",
                          _("drag to change exposure"),
                    _("double-click resets"));
  dt_scopes_call_if_exists(scopes->cur_mode, append_to_tooltip, &tip);
  gtk_widget_set_tooltip_text(scopes->scope_draw, tip);
  g_free(tip);
}

static void _drawable_button_press(GtkGestureSingle *gesture,
                                   int n_press,
                                   double x,
                                   double y,
                                   dt_scopes_t *s)
{
  if(s->highlight != DT_SCOPES_HIGHLIGHT_NONE
     && dt_scopes_func_exists(s->cur_mode, get_exposure_pos))
  {
    const double pos = dt_scopes_call(s->cur_mode, get_exposure_pos, x, y);
    dt_dev_exposure_handle_event(GTK_EVENT_CONTROLLER(gesture), n_press, pos, 0.0,
                                 s->highlight == DT_SCOPES_HIGHLIGHT_BLACK_POINT);
  }
}

static void _drawable_button_release(GtkGestureSingle *gesture,
                                     int n_press,
                                     double x,
                                     double y,
                                     dt_scopes_t *s)
{
  if(s->highlight != DT_SCOPES_HIGHLIGHT_NONE)
    dt_dev_exposure_handle_event(GTK_EVENT_CONTROLLER(gesture), -n_press, x, 0.0, FALSE);
}

static void _drawable_motion(GtkEventControllerMotion *controller,
                             double x,
                             double y,
                             dt_scopes_t *s)
{
  dt_scopes_mode_t *const cur_mode = s->cur_mode;
  if(dt_key_modifier_state() & GDK_BUTTON1_MASK
     && s->highlight != DT_SCOPES_HIGHLIGHT_NONE
     && dt_scopes_func_exists(cur_mode, get_exposure_pos))
  {
    const double pos = dt_scopes_call(cur_mode, get_exposure_pos, x, y);
    dt_dev_exposure_handle_event(GTK_EVENT_CONTROLLER(controller), 1, pos, 0.0, FALSE);
  }
  else
  {
    GtkAllocation allocation;
    gtk_widget_get_allocation(s->scope_draw, &allocation);
    const double posx = x / (double)(allocation.width);
    const double posy = y / (double)(allocation.height);
    const dt_scopes_highlight_t prior_highlight = s->highlight;

    if(dt_scopes_func_exists(cur_mode, get_highlight))
      s->highlight = dt_scopes_call(cur_mode, get_highlight, posx, posy);
    else
      s->highlight = DT_SCOPES_HIGHLIGHT_NONE;

    if(prior_highlight != s->highlight)
    {
      lib_histogram_update_tooltip(s);
      dt_scopes_refresh(s);
      if(s->highlight != DT_SCOPES_HIGHLIGHT_NONE)
      {
        // FIXME: should really use named cursors, and differentiate
        // between "grab" and "grabbing"
        dt_control_change_cursor("pointer");
      }
    }
  }
}

static void _drawable_leave(GtkEventControllerMotion *controller,
                            dt_scopes_t *s)
{
  // if dragging, gtk keeps up motion notifications until mouse button
  // is released, at which point we'll get another leave event for
  // drawable if pointer is still outside of the widget
  if(!(dt_key_modifier_state() & GDK_BUTTON1_MASK)
     && s->highlight != DT_SCOPES_HIGHLIGHT_NONE)
  {
    s->highlight = DT_SCOPES_HIGHLIGHT_NONE;
    dt_control_change_cursor("default");
    dt_scopes_refresh(s);
  }
}

static void _mode_toggle(GtkWidget *button, dt_scopes_t *s)
{
  // create radio-button-like behavior for choosing scope modes with a
  // GtkDarktableToggleButton group, using signal blocking to avoid
  // re-entering this signal handler
  dt_scopes_mode_t *prior_mode = s->cur_mode;
  for(int i = 0; i < DT_SCOPES_MODE_N; i++)
  {
    if(s->modes[i].button_activate == button)
    {
      // clicking on current mode button leaves it on
      if(s->cur_mode == &s->modes[i]
         && !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button)))
      {
        g_signal_handler_block(button, s->modes[i].toggle_signal_handler);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->modes[i].button_activate), TRUE);
        g_signal_handler_unblock(button, s->modes[i].toggle_signal_handler);
        return;
      }
      s->cur_mode = &s->modes[i];
    }
    else if(prior_mode == &s->modes[i])
    {
      // user clicked on a different mode button, set the prior mode
      // toggle button to inactive
      g_signal_handler_block(s->modes[i].button_activate, s->modes[i].toggle_signal_handler);
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->modes[i].button_activate), FALSE);
      g_signal_handler_unblock(s->modes[i].button_activate, s->modes[i].toggle_signal_handler);
    }
  }

  dt_conf_set_string("plugins/darkroom/histogram/mode",
                     dt_scopes_call(s->cur_mode, name));
  lib_histogram_update_tooltip(s);

  dt_scopes_call(prior_mode, mode_leave);
  gtk_widget_set_visible(s->button_box_rgb,
                         dt_scopes_func_exists(s->cur_mode, draw_scope_channels));
  dt_scopes_call(s->cur_mode, update_buttons);
  dt_scopes_call(s->cur_mode, mode_enter);

  // even if no current data, GUI should still respond to update
  dt_scopes_refresh(s);
  // FIXME: does this comparison of update_counter need to be protected within a mutex?
  if(s->update_counter != s->cur_mode->update_counter)
    dt_scopes_reprocess();
}

static void _channel_toggle(GtkWidget *button, dt_scopes_t *s)
{
  for(int i = 0; i < DT_SCOPES_RGB_N; i++)
    if(s->channel_buttons[i] == button)
    {
      char conf[48];
      g_snprintf(conf, sizeof(conf),
                 "plugins/darkroom/histogram/show_%s", rgb_names[i]);
      s->channels[i]
        = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
      dt_conf_set_bool(conf, s->channels[i]);
      dt_scopes_refresh(s);
    }
}

static void _eventbox_scroll_callback(GtkEventControllerScroll* self,
                                      gdouble dx,
                                      gdouble dy,
                                      dt_scopes_t *s)
{
  GdkEvent *event = gtk_get_current_event();
  if(!event) return;
  if(gdk_event_get_event_type(event) == GDK_SCROLL)
    {
    // FIXME: so long as we have event, test its flags -- and for GTK 4 we can use gtk_get_current_event() and get flags -- make a helper function to do this
    if(dt_modifier_is(event->scroll.state,
                      GDK_SHIFT_MASK | GDK_MOD1_MASK))
    {
      // bubble to adjusting the overall widget size
      // FIXME: use gtk_event_controller_handle_event()
      gtk_widget_event(s->scope_draw, event);
    }
    else if(s->highlight != DT_SCOPES_HIGHLIGHT_NONE)
    {
      // FIXME: should scroll for exposure change be handled by each scope, rather than here?
      // FIXME: should handle horizontal scrolling as well?
      // FIXME: should scrolling of scope be handled in the drawable rather than the eventbox
      const gboolean black = s->highlight == DT_SCOPES_HIGHLIGHT_BLACK_POINT;
      dt_dev_exposure_handle_event(GTK_EVENT_CONTROLLER(self), 0, 0, black ? -dy : dy, black);
    }
    else
    {
      // FIXME: should scrolling of scope be handled in the drawable rather than the eventbox? right now can scroll on buttons and it will change the vectorscope!
      dt_scopes_call_if_exists(s->cur_mode, eventbox_scroll,
                               event->scroll.x, event->scroll.y,
                               dx, dy, event->scroll.state);
    }
  }
  gdk_event_free(event);
}

static void _eventbox_motion_notify_callback(GtkEventControllerMotion *controller,
                                             double x,
                                             double y,
                                             dt_scopes_t *s)
{
  // This is required in order to correctly display the button tooltips
  // FIXME: it would seem possible that it is necessary to update button tooltips only when the main widget tooltip has changed, if the tooltip bubbled down, but calling this at the end of lib_histogram_update_tooltip() doesn't seem to help
  dt_scopes_call_if_exists(s->cur_mode, update_buttons);
  dt_scopes_call_if_exists(s->cur_mode, eventbox_motion, controller, x, y);
}

static void _eventbox_enter_notify_callback(GtkEventControllerMotion *controller,
                                            double x,
                                            double y,
                                            dt_scopes_t *s)
{
  // FIXME: do need to do this, or should this already be updated?
  lib_histogram_update_tooltip(s);
  // right after startup, vectorscope can display color harmony box,
  // so be sure to hide non-current mode buttons
  for(dt_scopes_mode_type_t i = 0; i < DT_SCOPES_MODE_N; i++)
    if(s->cur_mode != &s->modes[i])
      dt_scopes_call(&s->modes[i], mode_leave);
  dt_scopes_call(s->cur_mode, mode_enter);
  gtk_widget_set_visible(s->button_box_rgb,
                         dt_scopes_func_exists(s->cur_mode, draw_scope_channels));
  gtk_widget_show(s->button_box_main);
  gtk_widget_show(s->button_box_opt);
}

static void _eventbox_leave_notify_callback(GtkEventControllerMotion *controller,
                                            dt_scopes_t *s)
{
  // when click between buttons on the buttonbox a leave event is
  // generated -- ignore it (for GTK 4, replace this with the simpler
  // gtk_event_controller_get_current_event())
  GdkEvent *event = gtk_get_current_event();
  if(event)
  {
    if(gdk_event_get_event_type(event) == GDK_LEAVE_NOTIFY)
    {
      const GdkEventCrossing *xc = &event->crossing;
      if(xc->mode == GDK_CROSSING_UNGRAB && xc->detail == GDK_NOTIFY_INFERIOR)
      {
        gdk_event_free(event);
        return;
      }
    }
    gdk_event_free(event);
  }
  gtk_widget_hide(s->button_box_main);
  gtk_widget_hide(s->button_box_opt);
}

static void _lib_histogram_collapse_callback(dt_action_t *action)
{
  dt_lib_module_t *self = darktable.lib->proxy.histogram.module;

  // Get the state
  const gint visible = dt_lib_is_visible(self);

  // Inverse the visibility
  dt_lib_set_visible(self, !visible);
}

// this is only called in darkroom view
static void _lib_histogram_preview_updated_callback(gpointer instance,
                                                    const dt_lib_module_t *self)
{
  // preview pipe has already given process() the high quality
  // pre-gamma image. Now that preview pipe is complete, draw it
  //
  // FIXME: it would be nice if process() just queued a redraw if not
  // in live view, but then our draw code would have to have some
  // other way to assure that the histogram image is current besides
  // checking the pixelpipe to see if it has processed the current
  // image
  // FIXME: now that have update_counter can we do this? and get rid of catching DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED?
  const dt_scopes_t *s = self->data;
  dt_scopes_refresh(s);
}

void view_enter(struct dt_lib_module_t *self,
                struct dt_view_t *old_view,
                struct dt_view_t *new_view)
{
  const dt_scopes_t *s = self->data;
  if(new_view->view(new_view) == DT_VIEW_DARKROOM)
  {
    DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED, _lib_histogram_preview_updated_callback);
  }
  // button box should be hidden when enter view, unless mouse is over
  // histogram, in which case gtk kindly generates enter events
  gtk_widget_hide(s->button_box_main);
  gtk_widget_hide(s->button_box_opt);

  // FIXME: set histogram data to blank if enter tether with no active image
}

void view_leave(struct dt_lib_module_t *self,
                struct dt_view_t *old_view,
                struct dt_view_t *new_view)
{
  DT_CONTROL_SIGNAL_DISCONNECT(_lib_histogram_preview_updated_callback, self);
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_scopes_t *const s = dt_calloc1_align_type(dt_scopes_t);
  self->data = (void *)s;

  // must match order of dt_scopes_mode_type_t
  const dt_scopes_functions_t *const dt_scopes_mode_func_tables[DT_SCOPES_MODE_N] =
    { &dt_scopes_functions_vectorscope,
      &dt_scopes_functions_waveform,
      &dt_scopes_functions_split,
      &dt_scopes_functions_parade,
      &dt_scopes_functions_histogram,};
  const char *str = dt_conf_get_string_const("plugins/darkroom/histogram/mode");
  s->update_counter = 1;
  s->cur_mode = &s->modes[DT_SCOPES_MODE_WAVEFORM];  // failsafe

  // FIXME: is there a better way to init this?
  for(dt_scopes_mode_type_t i = 0; i < DT_SCOPES_MODE_N; i++)
  {
    s->modes[i].functions = dt_scopes_mode_func_tables[i];
    s->modes[i].update_counter = 0;
    s->modes[i].scopes = s;
    dt_scopes_call(&s->modes[i], gui_init, s);
    if(g_strcmp0(str, dt_scopes_call(&s->modes[i], name)) == 0)
      s->cur_mode = &s->modes[i];
  }

  dt_pthread_mutex_init(&s->lock, NULL);

  s->channels[DT_SCOPES_RGB_RED]
    = dt_conf_get_bool("plugins/darkroom/histogram/show_red");
  s->channels[DT_SCOPES_RGB_GREEN]
    = dt_conf_get_bool("plugins/darkroom/histogram/show_green");
  s->channels[DT_SCOPES_RGB_BLUE]
    = dt_conf_get_bool("plugins/darkroom/histogram/show_blue");

  // proxy functions and data so that pixelpipe or tether can
  // provide data for a histogram
  // FIXME: do need to pass self, or can wrap a callback as a lambda
  darktable.lib->proxy.histogram.module = self;
  darktable.lib->proxy.histogram.process = _scope_process;

  // create widgets
  GtkWidget *overlay = gtk_overlay_new();
  dt_action_t *dark =
    dt_action_section(&darktable.view_manager->proxy.darkroom.view->actions,
                      N_("histogram"));

  // shows the scope, scale, and has draggable areas
  s->scope_draw = dt_ui_resize_wrap(NULL,
                                    0,
                                    "plugins/darkroom/histogram/graphheight");
  dt_action_t *ac = dt_action_define(dark, NULL, N_("hide histogram"), s->scope_draw, NULL);
  dt_action_register(ac, NULL, _lib_histogram_collapse_callback,
                     GDK_KEY_H, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  gtk_widget_set_events(s->scope_draw, GDK_ENTER_NOTIFY_MASK);

  // a row of control buttons, split in two button boxes, on left and right side
  s->button_box_main = dt_gui_vbox();
  dt_gui_add_class(s->button_box_main, "button_box");
  gtk_widget_set_valign(s->button_box_main, GTK_ALIGN_START);
  gtk_widget_set_halign(s->button_box_main, GTK_ALIGN_START);

  GtkWidget *box_left = dt_gui_hbox();
  gtk_widget_set_valign(box_left, GTK_ALIGN_START);
  gtk_widget_set_halign(box_left, GTK_ALIGN_START);
  dt_gui_box_add(s->button_box_main, box_left);

  for(dt_scopes_mode_type_t i = 0; i < DT_SCOPES_MODE_N; i++)
    dt_scopes_call_if_exists(&s->modes[i],
                             add_to_main_box, dark, s->button_box_main);

  s->button_box_opt = dt_gui_hbox();
  dt_gui_add_class(s->button_box_opt, "button_box");
  gtk_widget_set_valign(s->button_box_opt, GTK_ALIGN_START);
  gtk_widget_set_halign(s->button_box_opt, GTK_ALIGN_END);

  // this intermediate box is needed to make the actions on buttons work
  GtkWidget *box_right = dt_gui_hbox();
  gtk_widget_set_valign(box_right, GTK_ALIGN_START);
  gtk_widget_set_halign(box_right, GTK_ALIGN_START);
  dt_gui_box_add(s->button_box_opt, box_right);

  // FIXME: the button transitions when they appear on mouseover
  // (mouse enters scope widget) or change (mouse click) cause redraws
  // of the entire scope -- is there a way to avoid this?

  // must match order of dt_scopes_mode_type_t
  // FIXME: should these be defined in each scope mode, or in a register function call?
  const void *dt_lib_histogram_scope_type_icons[DT_SCOPES_MODE_N] =
    { dtgtk_cairo_paint_vectorscope,
      dtgtk_cairo_paint_waveform_scope,
      dtgtk_cairo_paint_split_waveform_vectorscope,
      dtgtk_cairo_paint_rgb_parade,
      dtgtk_cairo_paint_histogram_scope };
  for(int i=0; i<DT_SCOPES_MODE_N; i++)
  {
    // FIXME: can use use GtkNotebook with gtk_notebook_set_show_tabs() to FALSE to handle mode-switching behavior?
    s->modes[i].button_activate =
      dtgtk_togglebutton_new(dt_lib_histogram_scope_type_icons[i], CPF_NONE, NULL);
    const char *const name = dt_scopes_call(&s->modes[i], name);
    gtk_widget_set_tooltip_text(s->modes[i].button_activate, _(name));
    dt_action_define(dark, N_("modes"), name,
                     s->modes[i].button_activate, &dt_action_def_toggle);
    dt_gui_box_add(box_left, s->modes[i].button_activate);
    // GTK4: use gtk_toggle_button_set_group(), GTK3: handle in callback
    s->modes[i].toggle_signal_handler =
      g_signal_connect_data(G_OBJECT(s->modes[i].button_activate), "toggled",
                            G_CALLBACK(_mode_toggle), s, NULL, 0);
  }

  dt_action_t *teth = &darktable.view_manager->proxy.tethering.view->actions;
  if(teth)
    dt_action_register(teth, N_("hide histogram"),
                       _lib_histogram_collapse_callback,
                       GDK_KEY_H, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

  s->button_box_rgb = dt_gui_hbox();
  gtk_widget_set_valign(s->button_box_rgb, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(s->button_box_rgb, GTK_ALIGN_END);

  // red/green/blue channel on/off
  for(int i=DT_SCOPES_RGB_RED; i < DT_SCOPES_RGB_N; i++)
  {
    g_autofree char *name = g_strdup_printf("%s-channel-button", rgb_names[i]);
    g_autofree char *tip = g_strdup_printf(_("toggle %s channel"), _(rgb_names[i]));
    GtkWidget *btn = dtgtk_togglebutton_new(dtgtk_cairo_paint_color,
                                            CPF_NONE, NULL);
    dt_gui_add_class(btn, "rgb_toggle");
    gtk_widget_set_name(btn, name);
    gtk_widget_set_tooltip_text(btn, tip);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn),
                                 s->channels[i]);
    dt_action_define(dark, N_("toggle colors"), rgb_names[i], btn, &dt_action_def_toggle);
    dt_gui_box_add(s->button_box_rgb, btn);
    g_signal_connect(G_OBJECT(btn), "toggled", G_CALLBACK(_channel_toggle), s);
    s->channel_buttons[i] = btn;
  }

  for(dt_scopes_mode_type_t i = 0; i < DT_SCOPES_MODE_N; i++)
  {
    dt_scopes_call_if_exists(&s->modes[i], add_to_options_box, dark, box_right);
    dt_scopes_call_if_exists(&s->modes[i], update_buttons);
    if(s->cur_mode == &s->modes[i])
      gtk_toggle_button_set_active
        (GTK_TOGGLE_BUTTON(s->modes[i].button_activate), TRUE);
  }
  dt_gui_box_add(box_right, s->button_box_rgb);

  // FIXME: add a brightness control (via GtkScaleButton?). Different per each mode?

  // assemble the widgets

  // The main widget is an overlay which has no window, and hence
  // can't catch events. We need something on top to catch events to
  // show/hide the buttons. The drawable is below the buttons, and
  // hence won't catch motion events for the buttons, and gets a leave
  // event when the cursor moves over the buttons.
  //
  // |----- EventBox -----|
  // |                    |
  // |  |-- Overlay  --|  |
  // |  |              |  |
  // |  |  ButtonBox   |  |
  // |  |              |  |
  // |  |--------------|  |
  // |  |              |  |
  // |  |  DrawingArea |  |
  // |  |              |  |
  // |  |--------------|  |
  // |                    |
  // |--------------------|

  GtkWidget *eventbox = gtk_event_box_new();
  gtk_container_add(GTK_CONTAINER(overlay), s->scope_draw);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), s->button_box_main);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), s->button_box_opt);
  gtk_container_add(GTK_CONTAINER(eventbox), overlay);
  self->widget = eventbox;

  gtk_widget_set_name(self->widget, "main-histogram");

  /* connect callbacks */
  // FIXME: why does cursor motion over buttons trigger multiple draw callbacks?
  g_signal_connect(G_OBJECT(s->scope_draw),
                   "draw", G_CALLBACK(_drawable_draw_callback), s);
  dt_gui_connect_click_all(s->scope_draw, _drawable_button_press,
                           _drawable_button_release, s);
  dt_gui_connect_motion(s->scope_draw, _drawable_motion, NULL,
                        _drawable_leave, s);

  // FIXME: scope implementation didn't setprop phase, maybe defaulted to bubble -- do we need to set this here?
  dt_gui_connect_scroll(eventbox, GTK_EVENT_CONTROLLER_SCROLL_VERTICAL
                                  | GTK_EVENT_CONTROLLER_SCROLL_DISCRETE,
                        _eventbox_scroll_callback, s);
  // FIXME: add (optional) propagation phase argument to dt_gui_connect_motion()
  GtkEventController *motion_controller =
    dt_gui_connect_motion(eventbox, _eventbox_motion_notify_callback,
                          _eventbox_enter_notify_callback,
                          _eventbox_leave_notify_callback, s);
  // necessary for catching motion events
  gtk_event_controller_set_propagation_phase(motion_controller, GTK_PHASE_BUBBLE);

  gtk_widget_show_all(self->widget);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_scopes_t *s = self->data;

  for(dt_scopes_mode_type_t i = 0; i < DT_SCOPES_MODE_N; i++)
    dt_scopes_call(&s->modes[i], gui_cleanup);
  dt_pthread_mutex_destroy(&s->lock);

  dt_free_align(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
