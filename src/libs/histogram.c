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

#include <stdint.h>

// FIXME: move this to scopes and/or cull as needed/possible
#include "common/color_harmony.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/imagebuf.h"
#include "common/image_cache.h"
#include "common/math.h"
#include "common/color_picker.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "dtgtk/button.h"
#include "dtgtk/drawingarea.h"
#include "dtgtk/paint.h"
#include "dtgtk/togglebutton.h"
#include "gui/accelerators.h"
#include "gui/color_picker_proxy.h"
#include "gui/draw.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "libs/colorpicker.h"
#include "common/splines.h"
#include "scopes.h"

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

DT_MODULE(1)

static const gchar *rgb_names[DT_SCOPES_RGB_N] =
  { N_("red"), N_("green"), N_("blue") };

typedef struct dt_lib_histogram_t
{
  // FIXME: this will eventually become the data of this dt_lib_module_t
  dt_scopes_t *scopes;

  dt_pthread_mutex_t lock;
  GtkWidget *button_box_opt;           // GtkBox -- contains options buttons
  GtkWidget *button_box_rgb;           // GtkBox -- contains RGB channels buttons
  GtkWidget *scope_type_button
    [DT_SCOPES_MODE_N];                // Array of GtkToggleButton -- mode buttons
  // FIXME: these are not currently used outside of gui_init(), so store?
  GtkWidget *channel_buttons
    [DT_SCOPES_RGB_N];                 // Array of GtkToggleButton -- RGB channel display
} dt_lib_histogram_t;

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

  dt_lib_histogram_t *const d = self->data;
  dt_scopes_t *const s = d->scopes;

  // special case, clear the scopes
  if(!input)
  {
    dt_pthread_mutex_lock(&d->lock);
    // FIXME: is better to do this or just advance update_counter by one?
    for(dt_scopes_mode_type_t i = 0; i < DT_SCOPES_MODE_N; i++)
      dt_scopes_call_if_exists(&s->modes[i], clear);
    dt_pthread_mutex_unlock(&d->lock);
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
  dt_pthread_mutex_lock(&d->lock);

  s->update_counter++;
  // if using a non-rgb profile_info_out as in cmyk softproofing we pass DT_COLORSPACE_LIN_REC2020
  //   for calculating the vertex_rgb data.
  dt_scopes_call(s->cur_mode, process, img_display, &roi,
                 profile_info_out->type ? profile_info_out : fallback);
  // FIXME: counter work is effectively atomic as is within a mutex, so just append update_counter_changed() work to end of the process() code that needs it
  s->cur_mode->update_counter = s->update_counter;
  dt_scopes_call_if_exists(s->cur_mode, update_counter_changed);

  dt_pthread_mutex_unlock(&d->lock);
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
                                        const gpointer user_data)
{
  dt_times_t start;
  dt_get_perf_times(&start);

  dt_lib_histogram_t *const d = (dt_lib_histogram_t *)user_data;
  dt_scopes_mode_t *const cur_mode = d->scopes->cur_mode;
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
                           cr, d->scopes->highlight, width, height);

  // draw grid
  // FIXME: set this in individual draw code
  set_color(cr, darktable.bauhaus->graph_grid);
  dt_scopes_call_if_exists(cur_mode, draw_grid, cr, width, height);

  // FIXME: should set histogram buffer to black if have just entered
  // tether view and nothing is displayed
  dt_pthread_mutex_lock(&d->lock);
  // darkroom view: draw scope so long as preview pipe is finished
  // tether view: draw whatever has come in from tether
  if((dt_view_get_current() == DT_VIEW_TETHERING
      || dev->image_storage.id == dev->preview_pipe->output_imgid)
     && (cur_mode->update_counter == d->scopes->update_counter))
  {
    if(dt_scopes_func_exists(cur_mode, draw_scope_channels))
      dt_scopes_call(cur_mode, draw_scope_channels,
                     cr, width, height, d->scopes->channels);
    else
      dt_scopes_call(cur_mode, draw_scope, cr, width, height);
  }
  dt_pthread_mutex_unlock(&d->lock);

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

static void _drawable_motion(GtkEventControllerMotion *controller,
                             double x,
                             double y,
                             dt_lib_histogram_t *d)
{
  dt_scopes_mode_t *const cur_mode = d->scopes->cur_mode;
  if(dt_key_modifier_state() & GDK_BUTTON1_MASK
     && d->scopes->highlight != DT_SCOPES_HIGHLIGHT_NONE
     && dt_scopes_func_exists(cur_mode, get_exposure_pos))
  {
    const double pos = dt_scopes_call(cur_mode, get_exposure_pos, x, y);
    dt_dev_exposure_handle_event(controller, 1, pos, FALSE);
  }
  else
  {
    GtkAllocation allocation;
    gtk_widget_get_allocation(d->scopes->scope_draw, &allocation);
    const double posx = x / (double)(allocation.width);
    const double posy = y / (double)(allocation.height);
    const dt_scopes_highlight_t prior_highlight = d->scopes->highlight;

    if(dt_scopes_func_exists(cur_mode, get_highlight))
      d->scopes->highlight = dt_scopes_call(cur_mode, get_highlight, posx, posy);
    else
      d->scopes->highlight = DT_SCOPES_HIGHLIGHT_NONE;

    if(prior_highlight != d->scopes->highlight)
    {
      lib_histogram_update_tooltip(d->scopes);
      dt_scopes_refresh(d->scopes);
      if(d->scopes->highlight != DT_SCOPES_HIGHLIGHT_NONE)
      {
        // FIXME: should really use named cursors, and differentiate
        // between "grab" and "grabbing"
        dt_control_change_cursor(GDK_HAND1);
      }
    }
  }
}

static void _drawable_button_press(GtkGestureSingle *gesture,
                                   int n_press,
                                   double x,
                                   double y,
                                   dt_lib_histogram_t *d)
{
  if(d->scopes->highlight != DT_SCOPES_HIGHLIGHT_NONE
     && dt_scopes_func_exists(d->scopes->cur_mode, get_exposure_pos))
  {
    const double pos = dt_scopes_call(d->scopes->cur_mode, get_exposure_pos, x, y);
    dt_dev_exposure_handle_event(gesture, n_press, pos,
                                 d->scopes->highlight == DT_SCOPES_HIGHLIGHT_BLACK_POINT);
  }
}

static void _drawable_button_release(GtkGestureSingle *gesture,
                                     int n_press,
                                     double x,
                                     double y,
                                     dt_lib_histogram_t *d)
{
  if(d->scopes->highlight != DT_SCOPES_HIGHLIGHT_NONE)
    dt_dev_exposure_handle_event(gesture, -n_press, x, FALSE);
}

static void _drawable_leave(GtkEventControllerMotion *controller,
                            dt_lib_histogram_t *d)
{
  // if dragging, gtk keeps up motion notifications until mouse button
  // is released, at which point we'll get another leave event for
  // drawable if pointer is still outside of the widget
  if(!(dt_key_modifier_state() & GDK_BUTTON1_MASK)
     && d->scopes->highlight != DT_SCOPES_HIGHLIGHT_NONE)
  {
    d->scopes->highlight = DT_SCOPES_HIGHLIGHT_NONE;
    dt_control_change_cursor(GDK_LEFT_PTR);
    dt_scopes_refresh(d->scopes);
  }
}

static void _scope_type_update(const dt_lib_histogram_t *const d)
{
  dt_scopes_mode_t *cur_mode = d->scopes->cur_mode;
  // hide any other modes
  // FIXME: hacky -- should just hide prior mode -- but as this is called at gui init and all buttons are shown by show_all(), this handles that case
  for(dt_scopes_mode_type_t i = 0; i < DT_SCOPES_MODE_N; i++)
  {
    dt_scopes_mode_t *mode = &d->scopes->modes[i];
    if(cur_mode != mode)
      dt_scopes_call(mode, mode_leave);
  }

  gtk_widget_set_visible(d->button_box_rgb,
                         dt_scopes_func_exists(cur_mode, draw_scope_channels));
  dt_scopes_call(cur_mode, mode_enter);
  dt_scopes_call(cur_mode, update_buttons);
}

static gboolean _scope_mode_clicked(GtkWidget *button,
                                    GdkEventButton *event,
                                    dt_lib_histogram_t *d)
{
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button)))
    return TRUE;

  dt_scopes_mode_t *prior_mode = d->scopes->cur_mode;
  for(int i = 0; i < DT_SCOPES_MODE_N; i++) // find the position of the button
  {
    if(d->scope_type_button[i] == button)
      d->scopes->cur_mode = &d->scopes->modes[i];
    // FIXME: keep track of index of cur_mode so don't have to run full loop?
    if(prior_mode == &d->scopes->modes[i])
      gtk_toggle_button_set_active
        (GTK_TOGGLE_BUTTON(d->scope_type_button[i]), FALSE);
  }
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), TRUE);

  // FIXME: before change mode, should call leave function from cur_mode, not later in _scope_type_changed()
  dt_conf_set_string("plugins/darkroom/histogram/mode",
                     dt_scopes_call(d->scopes->cur_mode, name));
  lib_histogram_update_tooltip(d->scopes);
  _scope_type_update(d);

  // even if no current data, GUI should still respond to update
  dt_scopes_refresh(d->scopes);
  // FIXME: does this comparison of update_counter need to be protected within a mutex?
  if(d->scopes->update_counter != d->scopes->cur_mode->update_counter)
    dt_scopes_reprocess();

  return TRUE;
}

static void _channel_toggle(GtkWidget *button, dt_lib_histogram_t *d)
{
  for(int i = 0; i < DT_SCOPES_RGB_N; i++)
    if(d->channel_buttons[i] == button)
    {
      char conf[48];
      g_snprintf(conf, sizeof(conf),
                 "plugins/darkroom/histogram/show_%s", rgb_names[i]);
      d->scopes->channels[i]
        = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
      dt_conf_set_bool(conf, d->scopes->channels[i]);
      dt_scopes_refresh(d->scopes);
    }
}

static gboolean _eventbox_scroll_callback(GtkWidget *widget,
                                          // FIXME: is this GTK4 compatible?
                                          GdkEventScroll *event,
                                          dt_lib_histogram_t *d)
{
  if(dt_modifier_is(event->state, GDK_SHIFT_MASK | GDK_MOD1_MASK))
    // bubble to adjusting the overall widget size
    gtk_widget_event(d->scopes->scope_draw, (GdkEvent*)event);
  else if(d->scopes->highlight != DT_SCOPES_HIGHLIGHT_NONE)
  {
    const gboolean black = d->scopes->highlight == DT_SCOPES_HIGHLIGHT_BLACK_POINT;
    if(black) { event->delta_x *= -1; event->delta_y *= -1; }
    dt_dev_exposure_handle_event(event, 0, 0, black);
  }
  else
    dt_scopes_call_if_exists(d->scopes->cur_mode, eventbox_scroll, event);
  return TRUE;
}

static gboolean _eventbox_enter_notify_callback(GtkWidget *widget,
                                                GdkEventCrossing *event,
                                                const gpointer user_data)
{
  const dt_lib_histogram_t *const d = (dt_lib_histogram_t *)user_data;

  // FIXME: do need to do this, or should this already be updated?
  lib_histogram_update_tooltip(d->scopes);
  // FIXME: do need to call this, or should the right buttons already be displayed?
  _scope_type_update(d);
  gtk_widget_show(d->scopes->button_box_main);
  gtk_widget_show(d->button_box_opt);
  return FALSE;
}

static gboolean _eventbox_motion_notify_callback(GtkWidget *widget,
                                                 const GdkEventMotion *event,
                                                 const dt_lib_histogram_t *d)
{
  // This is required in order to correctly display the button tooltips
  // FIXME: it would seem possible that it is necessary to update button tooltips only when the main widget tooltip has changed, if the tooltip bubbled down, but calling this at the end of lib_histogram_update_tooltip() doesn't seem to help
  dt_scopes_call_if_exists(d->scopes->cur_mode, update_buttons);
  dt_scopes_call_if_exists(d->scopes->cur_mode, eventbox_motion, widget, event);

  return FALSE;
}

static gboolean _eventbox_leave_notify_callback(GtkWidget *widget,
                                                const GdkEventCrossing *event,
                                                const gpointer user_data)
{
  // when click between buttons on the buttonbox a leave event is generated -- ignore it
  if(!(event->mode == GDK_CROSSING_UNGRAB && event->detail == GDK_NOTIFY_INFERIOR))
  {
    const dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
    gtk_widget_hide(d->scopes->button_box_main);
    gtk_widget_hide(d->button_box_opt);
  }
  return FALSE;
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
  const dt_lib_histogram_t *d = self->data;
  dt_scopes_refresh(d->scopes);
}

void view_enter(struct dt_lib_module_t *self,
                struct dt_view_t *old_view,
                struct dt_view_t *new_view)
{
  const dt_lib_histogram_t *d = self->data;
  if(new_view->view(new_view) == DT_VIEW_DARKROOM)
  {
    DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED, _lib_histogram_preview_updated_callback);
  }
  // button box should be hidden when enter view, unless mouse is over
  // histogram, in which case gtk kindly generates enter events
  gtk_widget_hide(d->scopes->button_box_main);
  gtk_widget_hide(d->button_box_opt);

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
  dt_lib_histogram_t *d = dt_calloc1_align_type(dt_lib_histogram_t);
  self->data = (void *)d;

  dt_scopes_t *const s = dt_calloc1_align_type(dt_scopes_t);
  d->scopes = s;

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
    dt_scopes_call(&s->modes[i], gui_init, s);
    if(g_strcmp0(str, dt_scopes_call(&s->modes[i], name)) == 0)
      s->cur_mode = &s->modes[i];
  }

  dt_pthread_mutex_init(&d->lock, NULL);

  d->scopes->channels[DT_SCOPES_RGB_RED]
    = dt_conf_get_bool("plugins/darkroom/histogram/show_red");
  d->scopes->channels[DT_SCOPES_RGB_GREEN]
    = dt_conf_get_bool("plugins/darkroom/histogram/show_green");
  d->scopes->channels[DT_SCOPES_RGB_BLUE]
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
  d->scopes->scope_draw = dt_ui_resize_wrap(NULL,
                                            0,
                                            "plugins/darkroom/histogram/graphheight");
  dt_action_t *ac = dt_action_define(dark, NULL, N_("hide histogram"), d->scopes->scope_draw, NULL);
  dt_action_register(ac, NULL, _lib_histogram_collapse_callback,
                     GDK_KEY_H, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  gtk_widget_set_events(d->scopes->scope_draw, GDK_ENTER_NOTIFY_MASK);

  // a row of control buttons, split in two button boxes, on left and right side
  d->scopes->button_box_main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  dt_gui_add_class(d->scopes->button_box_main, "button_box");
  gtk_widget_set_valign(d->scopes->button_box_main, GTK_ALIGN_START);
  gtk_widget_set_halign(d->scopes->button_box_main, GTK_ALIGN_START);

  GtkWidget *box_left = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_valign(box_left, GTK_ALIGN_START);
  gtk_widget_set_halign(box_left, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(d->scopes->button_box_main), box_left, FALSE, FALSE, 0);

  for(dt_scopes_mode_type_t i = 0; i < DT_SCOPES_MODE_N; i++)
    dt_scopes_call_if_exists(&s->modes[i],
                             gui_add_to_main, dark, d->scopes->button_box_main);

  d->button_box_opt = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  dt_gui_add_class(d->button_box_opt, "button_box");
  gtk_widget_set_valign(d->button_box_opt, GTK_ALIGN_START);
  gtk_widget_set_halign(d->button_box_opt, GTK_ALIGN_END);

  // this intermediate box is needed to make the actions on buttons work
  GtkWidget *box_right = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_valign(box_right, GTK_ALIGN_START);
  gtk_widget_set_halign(box_right, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(d->button_box_opt), box_right, FALSE, FALSE, 0);

  d->button_box_rgb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_valign(d->button_box_rgb, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(d->button_box_rgb, GTK_ALIGN_END);
  gtk_box_pack_end(GTK_BOX(box_right), d->button_box_rgb, FALSE, FALSE, 0);

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
    // FIXME: make these GtkRadioButton?
    d->scope_type_button[i] =
      dtgtk_togglebutton_new(dt_lib_histogram_scope_type_icons[i], CPF_NONE, NULL);
    const char *const name = dt_scopes_call(&s->modes[i], name);
    gtk_widget_set_tooltip_text(d->scope_type_button[i],
                                _(name));
    dt_action_define(dark, N_("modes"), name,
                     d->scope_type_button[i], &dt_action_def_toggle);
    gtk_box_pack_start(GTK_BOX(box_left), d->scope_type_button[i], FALSE, FALSE, 0);
    // FIXME: use "clicked" event instead of "button-press-event"? do GtkDarktableToggleButton support clicked events?
    g_signal_connect(G_OBJECT(d->scope_type_button[i]), "button-press-event",
                     G_CALLBACK(_scope_mode_clicked), d);
    if(s->cur_mode == &s->modes[i])
      gtk_toggle_button_set_active
        (GTK_TOGGLE_BUTTON(d->scope_type_button[i]), TRUE);
  }

  dt_action_t *teth = &darktable.view_manager->proxy.tethering.view->actions;
  if(teth)
  {
    dt_action_register(teth, N_("hide histogram"),
                       _lib_histogram_collapse_callback,
                       GDK_KEY_H, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  }

  // red/green/blue channel on/off
  for(int i=DT_SCOPES_RGB_BLUE; i >= DT_SCOPES_RGB_RED; i--)
  {
    g_autofree char *name = g_strdup_printf("%s-channel-button", rgb_names[i]);
    g_autofree char *tip = g_strdup_printf(_("toggle %s channel"), _(rgb_names[i]));
    GtkWidget *btn = dtgtk_togglebutton_new(dtgtk_cairo_paint_color,
                                            CPF_NONE, NULL);
    dt_gui_add_class(btn, "rgb_toggle");
    gtk_widget_set_name(btn, name);
    gtk_widget_set_tooltip_text(btn, tip);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn),
                                 d->scopes->channels[i]);
    dt_action_define(dark, N_("toggle colors"), rgb_names[i], btn, &dt_action_def_toggle);
    gtk_box_pack_end(GTK_BOX(d->button_box_rgb), btn, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(btn), "toggled", G_CALLBACK(_channel_toggle), d);
    d->channel_buttons[i] = btn;
  }

  for(dt_scopes_mode_type_t i = 0; i < DT_SCOPES_MODE_N; i++)
  {
    dt_scopes_call_if_exists(&s->modes[i], gui_init_options, dark, box_right);
    dt_scopes_call_if_exists(&s->modes[i], update_buttons);
  }

  // will change visibility of buttons, hence must run after all buttons are declared
  // FIXMME: can instead do something like: dt_scopes_call(d->scopes->cur_mode, update_buttons)
  _scope_type_update(d);

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
  gtk_container_add(GTK_CONTAINER(overlay), d->scopes->scope_draw);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), d->scopes->button_box_main);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), d->button_box_opt);
  gtk_container_add(GTK_CONTAINER(eventbox), overlay);
  self->widget = eventbox;

  gtk_widget_set_name(self->widget, "main-histogram");

  /* connect callbacks */
  gtk_widget_add_events(d->scopes->scope_draw, GDK_LEAVE_NOTIFY_MASK | GDK_POINTER_MOTION_MASK
                                               | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
  // FIXME: why does cursor motion over buttons trigger multiple draw callbacks?
  g_signal_connect(G_OBJECT(d->scopes->scope_draw),
                   "draw", G_CALLBACK(_drawable_draw_callback), d);
  dt_gui_connect_click_all(d->scopes->scope_draw, _drawable_button_press,
                           _drawable_button_release, d);
  dt_gui_connect_motion(d->scopes->scope_draw, _drawable_motion, NULL,
                        _drawable_leave, d);

  gtk_widget_add_events
    (eventbox,
     GDK_LEAVE_NOTIFY_MASK | GDK_ENTER_NOTIFY_MASK
     | GDK_POINTER_MOTION_MASK | darktable.gui->scroll_mask);
  g_signal_connect(G_OBJECT(eventbox), "scroll-event",
                   G_CALLBACK(_eventbox_scroll_callback), d);
  g_signal_connect(G_OBJECT(eventbox), "enter-notify-event",
                   G_CALLBACK(_eventbox_enter_notify_callback), d);
  g_signal_connect(G_OBJECT(eventbox), "leave-notify-event",
                   G_CALLBACK(_eventbox_leave_notify_callback), d);
  g_signal_connect(G_OBJECT(eventbox), "motion-notify-event",
                   G_CALLBACK(_eventbox_motion_notify_callback), d);

  gtk_widget_show_all(self->widget);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_histogram_t *d = self->data;

  for(dt_scopes_mode_type_t i = 0; i < DT_SCOPES_MODE_N; i++)
    dt_scopes_call(&d->scopes->modes[i], gui_cleanup);
  dt_free_align(d->scopes);
  d->scopes = NULL;

  dt_pthread_mutex_destroy(&d->lock);
  dt_free_align(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
