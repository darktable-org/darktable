/*
    This file is part of darktable,
    Copyright (C) 2009-2023 darktable developers.

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
/** this is the view for the darkroom module.  */

#include "common/extra_optimizations.h"

#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/file_location.h"
#include "common/focus_peaking.h"
#include "common/history.h"
#include "common/image_cache.h"
#include "common/selection.h"
#include "common/styles.h"
#include "common/tags.h"
#include "common/undo.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "dtgtk/button.h"
#include "dtgtk/thumbtable.h"
#include "gui/accelerators.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "gui/guides.h"
#include "gui/presets.h"
#include "gui/styles.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_module.h"
#include "libs/colorpicker.h"
#include "libs/modulegroups.h"
#include "views/view.h"
#include "views/view_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#ifdef USE_LUA
#include "lua/image.h"
#endif

#include <gdk/gdkkeysyms.h>
#include <glib.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifndef G_SOURCE_FUNC // Defined for glib >= 2.58
#define G_SOURCE_FUNC(f) ((GSourceFunc) (void (*)(void)) (f))
#endif

DT_MODULE(1)

static void _update_softproof_gamut_checking(dt_develop_t *d);

/* signal handler for filmstrip image switching */
static void _view_darkroom_filmstrip_activate_callback(gpointer instance, int imgid, gpointer user_data);

static void _dev_change_image(dt_develop_t *dev, const int32_t imgid);

static void _darkroom_display_second_window(dt_develop_t *dev);
static void _darkroom_ui_second_window_write_config(GtkWidget *widget);

const char *name(const dt_view_t *self)
{
  return _("darkroom");
}

#ifdef USE_LUA

static int display_image_cb(lua_State *L)
{
  dt_develop_t *dev = darktable.develop;
  dt_lua_image_t imgid = -1;
  if(luaL_testudata(L, 1, "dt_lua_image_t"))
  {
    luaA_to(L, dt_lua_image_t, &imgid, 1);
    _dev_change_image(dev, imgid);
  }
  else
  {
    // ensure the image infos in db is up to date
    dt_dev_write_history(dev);
  }
  luaA_push(L, dt_lua_image_t, &dev->image_storage.id);
  return 1;
}

#endif


void init(dt_view_t *self)
{
  self->data = malloc(sizeof(dt_develop_t));
  dt_dev_init((dt_develop_t *)self->data, TRUE);

  darktable.view_manager->proxy.darkroom.view = self;

#ifdef USE_LUA
  lua_State *L = darktable.lua_state.state;
  const int my_type = dt_lua_module_entry_get_type(L, "view", self->module_name);
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, display_image_cb, 1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "display_image");

  lua_pushcfunction(L, dt_lua_event_multiinstance_register);
  lua_pushcfunction(L, dt_lua_event_multiinstance_destroy);
  lua_pushcfunction(L, dt_lua_event_multiinstance_trigger);
  dt_lua_event_add(L, "darkroom-image-loaded");
#endif
}

uint32_t view(const dt_view_t *self)
{
  return DT_VIEW_DARKROOM;
}

void cleanup(dt_view_t *self)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;

  // unref the grid lines popover if needed
  if(darktable.view_manager->guides_popover) g_object_unref(darktable.view_manager->guides_popover);

  if(dev->second_window.second_wnd)
  {
    if(gtk_widget_is_visible(dev->second_window.second_wnd))
    {
      dt_conf_set_bool("second_window/last_visible", TRUE);
      _darkroom_ui_second_window_write_config(dev->second_window.second_wnd);
    }
    else
      dt_conf_set_bool("second_window/last_visible", FALSE);

    gtk_widget_destroy(dev->second_window.second_wnd);
    dev->second_window.second_wnd = NULL;
    dev->second_window.widget = NULL;
  }
  else
  {
    dt_conf_set_bool("second_window/last_visible", FALSE);
  }

  dt_dev_cleanup(dev);
  free(dev);
}

static dt_darkroom_layout_t _lib_darkroom_get_layout(dt_view_t *self)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  if(dev->iso_12646.enabled)
    return DT_DARKROOM_LAYOUT_EDITING;
  else
    return DT_DARKROOM_LAYOUT_EDITING;
}

static cairo_filter_t _get_filtering_level(dt_develop_t *dev, dt_dev_zoom_t zoom, int closeup)
{
  const float scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 0);

  // for pixel representation above 1:1, that is when a single pixel on the image
  // is represented on screen by multiple pixels we want to disable any cairo filter
  // which could only blur or smooth the output.

  if(scale >= 0.9999f)
    return CAIRO_FILTER_FAST;
  else
    return darktable.gui->dr_filter_image;
}

void _display_module_trouble_message_callback(gpointer instance,
                                              dt_iop_module_t *module,
                                              const char *const trouble_msg,
                                              const char *const trouble_tooltip)
{
  GtkWidget *label_widget = NULL;

  if(module && module->has_trouble && module->widget)
  {
    label_widget = dt_gui_container_first_child(GTK_CONTAINER(gtk_widget_get_parent(module->widget)));
    if(g_strcmp0(gtk_widget_get_name(label_widget), "iop-plugin-warning"))
      label_widget = NULL;
  }

  if(trouble_msg && *trouble_msg)
  {
    if(module && module->widget)
    {
      if(label_widget)
      {
        // set the warning message in the module's message area just below the header
        gtk_label_set_text(GTK_LABEL(label_widget), trouble_msg);
      }
      else
      {
        label_widget = gtk_label_new(trouble_msg);;
        gtk_label_set_line_wrap(GTK_LABEL(label_widget), TRUE);
        gtk_label_set_xalign(GTK_LABEL(label_widget), 0.0);
        gtk_widget_set_name(label_widget, "iop-plugin-warning");
        dt_gui_add_class(label_widget, "dt_warning");

        GtkWidget *iopw = gtk_widget_get_parent(module->widget);
        gtk_box_pack_start(GTK_BOX(iopw), label_widget, TRUE, TRUE, 0);
        gtk_box_reorder_child(GTK_BOX(iopw), label_widget, 0);
        gtk_widget_show(label_widget);
      }

      gtk_widget_set_tooltip_text(GTK_WIDGET(label_widget), trouble_tooltip);

      // set the module's trouble flag
      module->has_trouble = TRUE;

      dt_iop_gui_update_header(module);
    }
  }
  else if(module && module->has_trouble)
  {
    // no more trouble, so clear the trouble flag and remove the message area
    module->has_trouble = FALSE;

    dt_iop_gui_update_header(module);

    if(label_widget) gtk_widget_destroy(label_widget);
  }
}


static void _darkroom_pickers_draw(dt_view_t *self, cairo_t *cri,
                                   int32_t width, int32_t height,
                                   dt_dev_zoom_t zoom, int closeup, float zoom_x, float zoom_y,
                                   GSList *samples, gboolean is_primary_sample)
{
  if(!samples) return;

  dt_develop_t *dev = (dt_develop_t *)self->data;

  cairo_save(cri);
  // The colorpicker samples bounding rectangle should only be displayed inside the visible image
  const int pwidth = (dev->pipe->output_backbuf_width<<closeup) / darktable.gui->ppd;
  const int pheight = (dev->pipe->output_backbuf_height<<closeup) / darktable.gui->ppd;

  const double hbar = (self->width - pwidth) * .5;
  const double tbar = (self->height - pheight) * .5;
  cairo_rectangle(cri, hbar, tbar, pwidth, pheight);
  cairo_clip(cri);

  // FIXME: use dt_dev_get_processed_size() for this?
  const double wd = dev->preview_pipe->backbuf_width;
  const double ht = dev->preview_pipe->backbuf_height;
  const double zoom_scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 1);
  const double lw = 1.0 / zoom_scale;
  const double dashes[1] = { lw * 4.0 };

  cairo_translate(cri, 0.5 * width, 0.5 * height);
  cairo_scale(cri, zoom_scale, zoom_scale);
  cairo_translate(cri, -0.5 * wd - zoom_x * wd, -0.5 * ht - zoom_y * ht);
  // makes point sample crosshair gap look nicer
  cairo_set_line_cap(cri, CAIRO_LINE_CAP_SQUARE);

  dt_colorpicker_sample_t *selected_sample = darktable.lib->proxy.colorpicker.selected_sample;
  const gboolean only_selected_sample = !is_primary_sample && selected_sample
    && !darktable.lib->proxy.colorpicker.display_samples;

  for( ; samples; samples = g_slist_next(samples))
  {
    dt_colorpicker_sample_t *sample = samples->data;
    if(only_selected_sample && (sample != selected_sample))
      continue;

    // The picker is at the resolution of the preview pixelpipe. This
    // is width/2 of a preview-pipe pixel in (scaled) user space
    // coordinates. Use half pixel width so rounding to nearest device
    // pixel doesn't make uneven centering.
    double half_px = 0.5;
    const double min_half_px_device = 4.0;
    // FIXME: instead of going to all this effort to show how error-prone a preview pipe sample can be, just produce a better point sample
    gboolean show_preview_pixel_scale = TRUE;

    // overlays are aligned with pixels for a clean look
    if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
    {
      double x = sample->box[0] * wd, y = sample->box[1] * ht,
        w = sample->box[2] * wd, h = sample->box[3] * ht;
      cairo_user_to_device(cri, &x, &y);
      cairo_user_to_device(cri, &w, &h);
      x=round(x+0.5)-0.5;
      y=round(y+0.5)-0.5;
      w=round(w+0.5)-0.5;
      h=round(h+0.5)-0.5;
      cairo_device_to_user(cri, &x, &y);
      cairo_device_to_user(cri, &w, &h);
      cairo_rectangle(cri, x, y, w - x, h - y);
      if(is_primary_sample)
      {
        // handles
        const double hw = 5. / zoom_scale;
        cairo_rectangle(cri, x - hw, y - hw, 2. * hw, 2. * hw);
        cairo_rectangle(cri, x - hw, h - hw, 2. * hw, 2. * hw);
        cairo_rectangle(cri, w - hw, y - hw, 2. * hw, 2. * hw);
        cairo_rectangle(cri, w - hw, h - hw, 2. * hw, 2. * hw);
      }
    }
    else if(sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
    {
      // FIXME: to be really accurate, the colorpicker should render precisely over the nearest pixelpipe pixel, but this gets particularly tricky to do with iop pickers with transformations after them in the pipeline
      double x = sample->point[0] * wd, y = sample->point[1] * ht;
      cairo_user_to_device(cri, &x, &y);
      x=round(x+0.5)-0.5;
      y=round(y+0.5)-0.5;
      // render picker center a reasonable size in device pixels
      half_px = round(half_px * zoom_scale);
      if(half_px < min_half_px_device)
      {
        half_px = min_half_px_device;
        show_preview_pixel_scale = FALSE;
      }
      // crosshair radius
      double cr = (is_primary_sample ? 4. : 5.) * half_px;
      if(sample == selected_sample) cr *= 2;
      cairo_device_to_user(cri, &x, &y);
      cairo_device_to_user_distance(cri, &cr, &half_px);

      // "handles"
      if(is_primary_sample)
        cairo_arc(cri, x, y, cr, 0., 2. * M_PI);
      // crosshair
      cairo_move_to(cri, x - cr, y);
      cairo_line_to(cri, x + cr, y);
      cairo_move_to(cri, x, y - cr);
      cairo_line_to(cri, x, y + cr);
    }

    // default is to draw 1 (logical) pixel light lines with 1
    // (logical) pixel dark outline for legibility
    const double line_scale = (sample == selected_sample ? 2.0 : 1.0);
    cairo_set_line_width(cri, lw * 3.0 * line_scale);
    cairo_set_source_rgba(cri, 0.0, 0.0, 0.0, 0.4);
    cairo_stroke_preserve(cri);

    cairo_set_line_width(cri, lw * line_scale);
    cairo_set_dash(cri, dashes,
                   !is_primary_sample
                   && sample != selected_sample
                   && sample->size == DT_LIB_COLORPICKER_SIZE_BOX,
                   0.0);
    cairo_set_source_rgba(cri, 1.0, 1.0, 1.0, 0.8);
    cairo_stroke(cri);

    // draw the actual color sampled
    // FIXME: if an area sample is selected, when selected should fill it with colorpicker color?
    // NOTE: The sample may be based on outdated data, but still
    // display as it will update eventually. If we only drew on valid
    // data, swatches on point live samples would flicker when the
    // primary sample was drawn, and the primary sample swatch would
    // flicker when an iop is adjusted.
    if(sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
    {
      if(sample == selected_sample)
        cairo_arc(cri, sample->point[0] * wd, sample->point[1] * ht, half_px * 2., 0., 2. * M_PI);
      else if(show_preview_pixel_scale)
        cairo_rectangle(cri, sample->point[0] * wd - half_px, sample->point[1] * ht - half_px, half_px * 2., half_px * 2.);
      else
        cairo_arc(cri, sample->point[0] * wd, sample->point[1] * ht, half_px, 0., 2. * M_PI);

      set_color(cri, sample->swatch);
      cairo_fill(cri);
    }
  }

  cairo_restore(cri);
}

void expose(
    dt_view_t *self,
    cairo_t *cri,
    int32_t width,
    int32_t height,
    int32_t pointerx,
    int32_t pointery)
{
  cairo_set_source_rgb(cri, .2, .2, .2);
  cairo_save(cri);

  dt_develop_t *dev = (dt_develop_t *)self->data;
  const int32_t tb = dev->border_size;
  // account for border, make it transparent for other modules called below:
  pointerx -= tb;
  pointery -= tb;

  if(dev->gui_synch && !dev->image_loading)
  {
    // synch module guis from gtk thread:
    ++darktable.gui->reset;
    for(const GList *modules = dev->iop; modules; modules = g_list_next(modules))
    {
      dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
      dt_iop_gui_update(module);
    }
    --darktable.gui->reset;
    dev->gui_synch = 0;
  }

  if(dev->image_status == DT_DEV_PIXELPIPE_DIRTY
     || dev->image_status == DT_DEV_PIXELPIPE_INVALID
     || dev->pipe->input_timestamp < dev->preview_pipe->input_timestamp)
  {
    dt_dev_process_image(dev);
  }

  if(dev->preview_status == DT_DEV_PIXELPIPE_DIRTY
     || dev->preview_status == DT_DEV_PIXELPIPE_INVALID
     || dev->pipe->input_timestamp > dev->preview_pipe->input_timestamp)
  {
    dt_dev_process_preview(dev);
  }

  if(dev->preview2_status == DT_DEV_PIXELPIPE_DIRTY
     || dev->preview2_status == DT_DEV_PIXELPIPE_INVALID
     || dev->pipe->input_timestamp > dev->preview2_pipe->input_timestamp)
  {
    dt_dev_process_preview2(dev);
  }

  dt_pthread_mutex_t *mutex = NULL;
  // FIXME: these four dt_control_get_dev_*() calls each lock/unlock global_mutex -- consolidate this work
  const float zoom_y = dt_control_get_dev_zoom_y();
  const float zoom_x = dt_control_get_dev_zoom_x();
  const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  const int closeup = dt_control_get_dev_closeup();
  const float backbuf_scale = dt_dev_get_zoom_scale(dev, zoom, 1.0f, 0) * darktable.gui->ppd;

  static cairo_surface_t *image_surface = NULL;
  static int image_surface_width = 0, image_surface_height = 0, image_surface_imgid = -1;

  if(image_surface_width != width
     || image_surface_height != height
     || image_surface == NULL)
  {
    // create double-buffered image to draw on, to make modules draw more fluently.
    image_surface_width = width;
    image_surface_height = height;
    if(image_surface) cairo_surface_destroy(image_surface);
    image_surface = dt_cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
    image_surface_imgid = -1; // invalidate old stuff
  }
  cairo_surface_t *surface;
  cairo_t *cr = cairo_create(image_surface);

  // adjust scroll bars
  {
    float zx = zoom_x, zy = zoom_y, boxw = 1., boxh = 1.;
    dt_dev_check_zoom_bounds(dev, &zx, &zy, zoom, closeup, &boxw, &boxh);

    /* If boxw and boxh very closely match the zoomed size in the darktable window we might have resizing with
       every expose because adding a slider will change the image area and might force a resizing in next expose.
       So we disable in cases close to full.
    */
    if(boxw > 0.95f)
    {
      zx = .0f;
      boxw = 1.01f;
    }
    if(boxh > 0.95f)
    {
      zy = .0f;
      boxh = 1.01f;
    }

    dt_view_set_scrollbar(self, zx, -0.5 + boxw/2, 0.5, boxw/2, zy, -0.5+ boxh/2, 0.5, boxh/2);
  }

  if(dev->pipe->output_backbuf                            // do we have an image?
     && dev->pipe->output_imgid == dev->image_storage.id  // is the right image?
     && dev->pipe->backbuf_scale == backbuf_scale    // is this the zoom scale we want to display?
     && dev->pipe->backbuf_zoom_x == zoom_x && dev->pipe->backbuf_zoom_y == zoom_y)
  {
    // draw image
    mutex = &dev->pipe->backbuf_mutex;
    dt_pthread_mutex_lock(mutex);
    const float wd = dev->pipe->output_backbuf_width;
    const float ht = dev->pipe->output_backbuf_height;

    surface = dt_view_create_surface(dev->pipe->output_backbuf, wd, ht);

    if(dev->iso_12646.enabled)
    {
      // force middle grey in background
      cairo_set_source_rgb(cr, 0.4663, 0.4663, 0.4663);
    }
    else
    {
      if(dev->full_preview)
        dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_DARKROOM_PREVIEW_BG);
      else
        dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_DARKROOM_BG);
    }
    cairo_paint(cr);

    dt_view_paint_surface(cr, width, height, surface, wd, ht, DT_WINDOW_MAIN);

    cairo_surface_destroy(surface);
    dt_pthread_mutex_unlock(mutex);
    image_surface_imgid = dev->image_storage.id;
  }
  else if(dev->preview_pipe->output_backbuf
          && dev->preview_pipe->output_imgid == dev->image_storage.id)
  {
    // draw preview
    mutex = &dev->preview_pipe->backbuf_mutex;
    dt_pthread_mutex_lock(mutex);

    const float wd = dev->preview_pipe->output_backbuf_width;
    const float ht = dev->preview_pipe->output_backbuf_height;
    const float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 1);

    if(dev->iso_12646.enabled)
    {
      // force middle grey in background
      cairo_set_source_rgb(cr, 0.4663, 0.4663, 0.4663);
    }
    else
    {
      dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_DARKROOM_BG);
    }

    cairo_paint(cr);

    if(dev->iso_12646.enabled)
    {
      // draw the white frame around picture
      const double tbw = (float)(tb >> closeup) / 3.0;
      cairo_rectangle(cr, tbw, tbw, width - 2.0 * tbw, height - 2.0 * tbw);
      cairo_set_source_rgb(cr, 1., 1., 1.);
      cairo_fill(cr);
    }

    cairo_rectangle(cr, tb, tb, width-2*tb, height-2*tb);
    cairo_clip(cr);

    surface = dt_view_create_surface(dev->preview_pipe->output_backbuf, wd, ht);

    cairo_translate(cr, width / 2.0, height / 2.0f);
    cairo_scale(cr, zoom_scale, zoom_scale);
    cairo_translate(cr, -.5f * wd - zoom_x * wd, -.5f * ht - zoom_y * ht);

    cairo_rectangle(cr, 0, 0, wd, ht);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), _get_filtering_level(dev, zoom, closeup));
    cairo_fill(cr);
    cairo_surface_destroy(surface);
    dt_pthread_mutex_unlock(mutex);
    image_surface_imgid = dev->image_storage.id;
  }
  else if(dev->preview_pipe->output_imgid != dev->image_storage.id)
  {
    gchar *load_txt;
    float fontsize;

    if(dev->image_invalid_cnt)
    {
      fontsize = DT_PIXEL_APPLY_DPI(16);
      load_txt = g_strdup_printf(
          _("darktable could not load `%s', switching to lighttable now.\n\n"
            "please check that the camera model that produced the image is supported in darktable\n"
            "(list of supported cameras is at https://www.darktable.org/resources/camera-support/).\n"
            "if you are sure that the camera model is supported, please consider opening an issue\n"
            "at https://github.com/darktable-org/darktable"),
          dev->image_storage.filename);
      if(dev->image_invalid_cnt > 400)
      {
        dev->image_invalid_cnt = 0;
        dt_view_manager_switch(darktable.view_manager, "lighttable");
        return;
      }
    }
    else
    {
      fontsize = DT_PIXEL_APPLY_DPI(14);
      if(dt_conf_get_bool("darkroom/ui/loading_screen"))
        load_txt = g_strdup_printf(C_("darkroom", "loading `%s' ..."), dev->image_storage.filename);
      else
        load_txt = g_strdup(dev->image_storage.filename);
    }

    if(dt_conf_get_bool("darkroom/ui/loading_screen"))
    {
      dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_DARKROOM_BG);
      cairo_paint(cr);

      // waiting message
      PangoRectangle ink;
      PangoLayout *layout;
      PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
      pango_font_description_set_absolute_size(desc, fontsize * PANGO_SCALE);
      pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
      layout = pango_cairo_create_layout(cr);
      pango_layout_set_font_description(layout, desc);
      pango_layout_set_text(layout, load_txt, -1);
      pango_layout_get_pixel_extents(layout, &ink, NULL);
      const float xc = width / 2.0, yc = height * 0.85 - DT_PIXEL_APPLY_DPI(10), wd = ink.width * .5f;
      cairo_move_to(cr, xc - wd, yc + 1. / 3. * fontsize - fontsize);
      pango_cairo_layout_path(cr, layout);
      cairo_set_line_width(cr, 2.0);
      dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_LOG_BG);
      cairo_stroke_preserve(cr);
      dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_LOG_FG);
      cairo_fill(cr);
      pango_font_description_free(desc);
      g_object_unref(layout);
      image_surface_imgid = dev->image_storage.id;
    }
    else
    {
      dt_toast_log("%s", load_txt);
    }
    g_free(load_txt);
  }
  cairo_restore(cri);

  if(image_surface_imgid == dev->image_storage.id)
  {
    cairo_destroy(cr);
    cairo_set_source_surface(cri, image_surface, 0, 0);
    cairo_paint(cri);
  }

  /* if we are in full preview mode, we don"t want anything else than the image */
  if(dev->full_preview) return;

  // Displaying sample areas if enabled
  if(darktable.lib->proxy.colorpicker.live_samples
     && (darktable.lib->proxy.colorpicker.display_samples
         || (darktable.lib->proxy.colorpicker.selected_sample &&
             darktable.lib->proxy.colorpicker.selected_sample != darktable.lib->proxy.colorpicker.primary_sample)))
  {
    _darkroom_pickers_draw(
      self, cri, width, height, zoom, closeup, zoom_x, zoom_y,
      darktable.lib->proxy.colorpicker.live_samples, FALSE);
  }

  // draw guide lines if needed
  if(!dev->gui_module || !(dev->gui_module->flags() & IOP_FLAGS_GUIDES_SPECIAL_DRAW))
  {
    // we restrict the drawing to the image only
    // the drawing is done on the preview pipe reference
    const float wd = dev->preview_pipe->backbuf_width;
    const float ht = dev->preview_pipe->backbuf_height;
    const float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, 1 << closeup, 1);

    cairo_save(cri);
    // don't draw guides on image margins
    cairo_rectangle(cri, tb, tb, width - 2 * tb, height - 2 * tb);
    cairo_clip(cri);
    // switch to the preview reference
    cairo_translate(cri, width / 2.0, height / 2.0);
    cairo_scale(cri, zoom_scale, zoom_scale);
    cairo_translate(cri, -.5f * wd - zoom_x * wd, -.5f * ht - zoom_y * ht);
    dt_guides_draw(cri, 0.0f, 0.0f, wd, ht, zoom_scale);
    cairo_restore(cri);
  }

  // display mask if we have a current module activated or if the masks manager module is expanded

  const gboolean display_masks = (dev->gui_module && dev->gui_module->enabled
                                  && dt_dev_modulegroups_get_activated(darktable.develop) != DT_MODULEGROUP_BASICS)
                                 || dt_lib_gui_get_expanded(dt_lib_get_module("masks"));

  // draw colorpicker for in focus module or execute module callback hook
  // FIXME: draw picker in gui_post_expose() hook in libs/colorpicker.c -- catch would be that live samples would appear over guides, softproof/gamut text overlay would be hidden by picker
  if(dt_iop_color_picker_is_visible(dev))
  {
    GSList samples = { .data = darktable.lib->proxy.colorpicker.primary_sample, .next = NULL };
    _darkroom_pickers_draw(self, cri, width, height, zoom, closeup, zoom_x, zoom_y,
                           &samples, TRUE);
  }
  else
  {
    if(dev->form_visible && display_masks)
      dt_masks_events_post_expose(dev->gui_module, cri, width, height, pointerx, pointery);
    // module
    if(dev->gui_module && dev->gui_module->gui_post_expose
       && dt_dev_modulegroups_get_activated(darktable.develop) != DT_MODULEGROUP_BASICS)
      dev->gui_module->gui_post_expose(dev->gui_module, cri, width, height, pointerx, pointery);
  }

  // indicate if we are in gamut check or softproof mode
  if(darktable.color_profiles->mode != DT_PROFILE_NORMAL)
  {
    gchar *label = darktable.color_profiles->mode == DT_PROFILE_GAMUTCHECK ? _("gamut check") : _("soft proof");
    cairo_set_source_rgba(cri, 0.5, 0.5, 0.5, 0.5);
    PangoLayout *layout;
    PangoRectangle ink;
    PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    layout = pango_cairo_create_layout(cri);
    pango_font_description_set_absolute_size(desc, DT_PIXEL_APPLY_DPI(20) * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, label, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cri, ink.height * 2, height - (ink.height * 3));
    pango_cairo_layout_path(cri, layout);
    cairo_set_source_rgb(cri, 0.7, 0.7, 0.7);
    cairo_fill_preserve(cri);
    cairo_set_line_width(cri, 0.7);
    cairo_set_source_rgb(cri, 0.3, 0.3, 0.3);
    cairo_stroke(cri);
    pango_font_description_free(desc);
    g_object_unref(layout);
  }
}

void reset(dt_view_t *self)
{
  dt_control_set_dev_zoom(DT_ZOOM_FIT);
  dt_control_set_dev_zoom_x(0);
  dt_control_set_dev_zoom_y(0);
  dt_control_set_dev_closeup(0);
}

int try_enter(dt_view_t *self)
{
  int32_t imgid = dt_act_on_get_main_image();

  if(imgid < 0)
  {
    // fail :(
    dt_control_log(_("no image to open!"));
    return 1;
  }

  // this loads the image from db if needed:
  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  // get image and check if it has been deleted from disk first!

  char imgfilename[PATH_MAX] = { 0 };
  gboolean from_cache = TRUE;
  dt_image_full_path(img->id, imgfilename, sizeof(imgfilename), &from_cache);
  if(!g_file_test(imgfilename, G_FILE_TEST_IS_REGULAR))
  {
    dt_control_log(_("image `%s' is currently unavailable"), img->filename);
    dt_image_cache_read_release(darktable.image_cache, img);
    return 1;
  }
  // and drop the lock again.
  dt_image_cache_read_release(darktable.image_cache, img);
  darktable.develop->image_storage.id = imgid;
  darktable.develop->proxy.wb_coeffs[0] = 0.f;
  return 0;
}

#ifdef USE_LUA

static void _fire_darkroom_image_loaded_event(const bool clean, const int32_t imgid)
{
  dt_lua_async_call_alien(dt_lua_event_trigger_wrapper,
      0, NULL, NULL,
      LUA_ASYNC_TYPENAME, "const char*", "darkroom-image-loaded",
      LUA_ASYNC_TYPENAME, "bool", clean,
      LUA_ASYNC_TYPENAME, "dt_lua_image_t", GINT_TO_POINTER(imgid),
      LUA_ASYNC_DONE);
}

#endif

static void _dev_change_image(dt_develop_t *dev, const int32_t imgid)
{
  // stop crazy users from sleeping on key-repeat spacebar:
  if(dev->image_loading) return;

  // deactivate module label timer if set
  if(darktable.develop->gui_module
     && darktable.develop->gui_module->label_recompute_handle)
  {
    g_source_remove(darktable.develop->gui_module->label_recompute_handle);
    darktable.develop->gui_module->label_recompute_handle = 0;
  }

  // Pipe reset needed when changing image
  // FIXME: synch with dev_init() and dev_cleanup() instead of redoing it
  dev->proxy.chroma_adaptation = NULL;
  dev->proxy.wb_is_D65 = TRUE;
  dev->proxy.wb_coeffs[0] = 0.f;

  // change active image
  g_slist_free(darktable.view_manager->active_images);
  darktable.view_manager->active_images = g_slist_prepend(NULL, GINT_TO_POINTER(imgid));
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_ACTIVE_IMAGES_CHANGE);

  // if the previous shown image is selected and the selection is unique
  // then we change the selected image to the new one
  if(dev->image_storage.id > 0)
  {
    sqlite3_stmt *stmt;
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2
      (dt_database_get(darktable.db),
       "SELECT m.imgid"
       " FROM memory.collected_images as m, main.selected_images as s"
       " WHERE m.imgid=s.imgid",
       -1, &stmt, NULL);
    // clang-format on
    gboolean follow = FALSE;
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      if(sqlite3_column_int(stmt, 0) == dev->image_storage.id
         && sqlite3_step(stmt) != SQLITE_ROW)
      {
        follow = TRUE;
      }
    }
    sqlite3_finalize(stmt);
    if(follow)
    {
      dt_selection_select_single(darktable.selection, imgid);
    }
  }

  // disable color picker when changing image
  if(darktable.lib->proxy.colorpicker.picker_proxy)
    dt_iop_color_picker_reset(darktable.lib->proxy.colorpicker.picker_proxy->module, FALSE);

  // update aspect ratio
  if(dev->preview_pipe->backbuf
     && dev->preview_status == DT_DEV_PIXELPIPE_VALID)
  {
    const double aspect_ratio =
      (double)dev->preview_pipe->backbuf_width / (double)dev->preview_pipe->backbuf_height;
    dt_image_set_aspect_ratio_to(dev->preview_pipe->image.id, aspect_ratio, TRUE);
  }
  else
  {
    dt_image_set_aspect_ratio(dev->image_storage.id, TRUE);
  }

  // clean the undo list
  dt_undo_clear(darktable.undo, DT_UNDO_DEVELOP);

  // prevent accels_window to refresh
  darktable.view_manager->accels_window.prevent_refresh = TRUE;

  // make sure we can destroy and re-setup the pixel pipes.
  // we acquire the pipe locks, which will block the processing threads
  // in darkroom mode before they touch the pipes (init buffers etc).
  // we don't block here, since we hold the gdk lock, which will
  // result in circular locking when background threads emit signals
  // which in turn try to acquire the gdk lock.
  //
  // worst case, it'll drop some change image events. sorry.
  if(dt_pthread_mutex_BAD_trylock(&dev->preview_pipe_mutex))
  {

  #ifdef USE_LUA

  _fire_darkroom_image_loaded_event(FALSE, imgid);

#endif

  return;
  }
  if(dt_pthread_mutex_BAD_trylock(&dev->pipe_mutex))
  {
    dt_pthread_mutex_BAD_unlock(&dev->preview_pipe_mutex);

 #ifdef USE_LUA

  _fire_darkroom_image_loaded_event(FALSE, imgid);

#endif

   return;
  }
  if(dt_pthread_mutex_BAD_trylock(&dev->preview2_pipe_mutex))
  {
    dt_pthread_mutex_BAD_unlock(&dev->pipe_mutex);
    dt_pthread_mutex_BAD_unlock(&dev->preview_pipe_mutex);

 #ifdef USE_LUA

  _fire_darkroom_image_loaded_event(FALSE, imgid);

#endif

   return;
  }

  // get current plugin in focus before defocus
  gchar *active_plugin = NULL;
  if(darktable.develop->gui_module)
  {
    active_plugin = g_strdup(darktable.develop->gui_module->op);
  }

  // store last active group
  dt_conf_set_int("plugins/darkroom/groups", dt_dev_modulegroups_get(dev));

  dt_iop_request_focus(NULL);

  g_assert(dev->gui_attached);

  // commit image ops to db
  dt_dev_write_history(dev);

  // be sure light table will update the thumbnail
  if(!dt_history_hash_is_mipmap_synced(dev->image_storage.id))
  {
    const dt_history_hash_t hash_status = dt_history_hash_get_status(dev->image_storage.id);

    dt_mipmap_cache_remove(darktable.mipmap_cache, dev->image_storage.id);
    dt_image_update_final_size(dev->image_storage.id);
    const gboolean fresh = (hash_status == DT_HISTORY_HASH_BASIC) || (hash_status == DT_HISTORY_HASH_AUTO);
    const dt_imageio_write_xmp_t xmp_mode = dt_image_get_xmp_mode();
    if((xmp_mode == DT_WRITE_XMP_ALWAYS) || ((xmp_mode == DT_WRITE_XMP_LAZY) && !fresh))
      dt_image_synch_xmp(dev->image_storage.id);
    dt_history_hash_set_mipmap(dev->image_storage.id);
#ifdef USE_LUA
    dt_lua_async_call_alien(dt_lua_event_trigger_wrapper,
        0, NULL, NULL,
        LUA_ASYNC_TYPENAME, "const char*", "darkroom-image-history-changed",
        LUA_ASYNC_TYPENAME, "dt_lua_image_t", GINT_TO_POINTER(dev->image_storage.id),
        LUA_ASYNC_DONE);
#endif
  }

  // cleanup visible masks
  if(!dev->form_gui)
  {
    dev->form_gui = (dt_masks_form_gui_t *)calloc(1, sizeof(dt_masks_form_gui_t));
    dt_masks_init_form_gui(dev->form_gui);
  }
  dt_masks_change_form_gui(NULL);

  while(dev->history)
  {
    // clear history of old image
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(dev->history->data);
    dt_dev_free_history_item(hist);
    dev->history = g_list_delete_link(dev->history, dev->history);
  }

  // get new image:
  dt_dev_reload_image(dev, imgid);

  // make sure no signals propagate here:
  ++darktable.gui->reset;

  const guint nb_iop = g_list_length(dev->iop);
  dt_dev_pixelpipe_cleanup_nodes(dev->pipe);
  dt_dev_pixelpipe_cleanup_nodes(dev->preview_pipe);
  dt_dev_pixelpipe_cleanup_nodes(dev->preview2_pipe);
  for(int i = nb_iop - 1; i >= 0; i--)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(g_list_nth_data(dev->iop, i));

    // the base module is the one with the lowest multi_priority
    int base_multi_priority = 0;
    for(const GList *l = dev->iop; l; l = g_list_next(l))
    {
      dt_iop_module_t *mod = (dt_iop_module_t *)l->data;
      if(strcmp(module->op, mod->op) == 0) base_multi_priority = MIN(base_multi_priority, mod->multi_priority);
    }

    if(module->multi_priority == base_multi_priority) // if the module is the "base" instance, we keep it
    {
      module->iop_order = dt_ioppr_get_iop_order(dev->iop_order_list, module->op, module->multi_priority);
      module->multi_priority = 0;
      module->multi_name[0] = '\0';
      dt_iop_reload_defaults(module);
      dt_iop_gui_update(module);
    }
    else // else we delete it and remove it from the panel
    {
      if(!dt_iop_is_hidden(module))
      {
        dt_iop_gui_cleanup_module(module);
        gtk_widget_destroy(module->expander);
      }

      // we remove the module from the list
      dev->iop = g_list_remove_link(dev->iop, g_list_nth(dev->iop, i));

      // we cleanup the module
      dt_action_cleanup_instance_iop(module);

      free(module);
    }
  }
  dev->iop = g_list_sort(dev->iop, dt_sort_iop_by_order);

  // we also clear the saved modules
  while(dev->alliop)
  {
    dt_iop_cleanup_module((dt_iop_module_t *)dev->alliop->data);
    free(dev->alliop->data);
    dev->alliop = g_list_delete_link(dev->alliop, dev->alliop);
  }
  // and masks
  g_list_free_full(dev->forms, (void (*)(void *))dt_masks_free_form);
  dev->forms = NULL;
  g_list_free_full(dev->allforms, (void (*)(void *))dt_masks_free_form);
  dev->allforms = NULL;

  dt_dev_pixelpipe_create_nodes(dev->pipe, dev);
  dt_dev_pixelpipe_create_nodes(dev->preview_pipe, dev);
  if(dev->second_window.widget && GTK_IS_WIDGET(dev->second_window.widget))
    dt_dev_pixelpipe_create_nodes(dev->preview2_pipe, dev);
  dt_dev_read_history(dev);

  // we have to init all module instances other than "base" instance
  char option[1024];
  for(const GList *modules = g_list_last(dev->iop); modules; modules = g_list_previous(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    if(module->multi_priority > 0)
    {
      if(!dt_iop_is_hidden(module))
      {
        dt_iop_gui_init(module);

        /* add module to right panel */
        dt_iop_gui_set_expander(module);
        dt_iop_gui_update_blending(module);
      }
    }
    else
    {
      //  update the module header to ensure proper multi-name display
      if(!dt_iop_is_hidden(module))
      {
        snprintf(option, sizeof(option), "plugins/darkroom/%s/expanded", module->op);
        module->expanded = dt_conf_get_bool(option);
        dt_iop_gui_update_expanded(module);
        if(module->change_image) module->change_image(module);
        dt_iop_gui_update_header(module);
      }
    }
  }

  dt_dev_pop_history_items(dev, dev->history_end);

  // set the module list order
  dt_dev_reorder_gui_module_list(dev);

  /* cleanup histograms */
  g_list_foreach(dev->iop, (GFunc)dt_iop_cleanup_histogram, (gpointer)NULL);

  /* make signals work again, we can't restore the active_plugin while signals
     are blocked due to implementation of dt_iop_request_focus so we do it now
     A double history entry is not generated.
  */
  --darktable.gui->reset;

  dt_dev_masks_list_change(dev);

  /* Now we can request focus again and write a safe plugins/darkroom/active */
  if(active_plugin)
  {
    gboolean valid = FALSE;
    for(const GList *modules = dev->iop; modules; modules = g_list_next(modules))
    {
      dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
      if(!strcmp(module->op, active_plugin))
      {
        valid = TRUE;
        dt_conf_set_string("plugins/darkroom/active", active_plugin);
        dt_iop_request_focus(module);
      }
    }
    if(!valid)
    {
      dt_conf_set_string("plugins/darkroom/active", "");
    }
    g_free(active_plugin);
  }

  // Signal develop initialize
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_IMAGE_CHANGED);

  // release pixel pipe mutices
  dt_pthread_mutex_BAD_unlock(&dev->preview2_pipe_mutex);
  dt_pthread_mutex_BAD_unlock(&dev->preview_pipe_mutex);
  dt_pthread_mutex_BAD_unlock(&dev->pipe_mutex);

  // update hint message
  dt_collection_hint_message(darktable.collection);

  // update accels_window
  darktable.view_manager->accels_window.prevent_refresh = FALSE;
  if(darktable.view_manager->accels_window.window && darktable.view_manager->accels_window.sticky)
    dt_view_accels_refresh(darktable.view_manager);

  // just make sure at this stage we have only history info into the undo, all automatic
  // tagging should be ignored.
  dt_undo_clear(darktable.undo, DT_UNDO_TAGS);

  //connect iop accelerators
  dt_iop_connect_accels_all();

  /* last set the group to update visibility of iop modules for new pipe */
  dt_dev_modulegroups_set(dev, dt_conf_get_int("plugins/darkroom/groups"));

  dt_image_check_camera_missing_sample(&dev->image_storage);

#ifdef USE_LUA

  _fire_darkroom_image_loaded_event(TRUE, imgid);

#endif

}

static void _view_darkroom_filmstrip_activate_callback(gpointer instance, int32_t imgid, gpointer user_data)
{
  if(imgid > 0)
  {
    // switch images in darkroom mode:
    const dt_view_t *self = (dt_view_t *)user_data;
    dt_develop_t *dev = (dt_develop_t *)self->data;

    _dev_change_image(dev, imgid);
    // move filmstrip
    dt_thumbtable_set_offset_image(dt_ui_thumbtable(darktable.gui->ui), imgid, TRUE);
    // force redraw
    dt_control_queue_redraw();
  }
}

static void dt_dev_jump_image(dt_develop_t *dev, int diff, gboolean by_key)
{
  if(dev->image_loading) return;

  const int32_t imgid = dev->image_storage.id;
  int new_offset = 1;
  int new_id = -1;

  // we new offset and imgid after the jump
  sqlite3_stmt *stmt;
  // clang-format off
  gchar *query = g_strdup_printf("SELECT rowid, imgid "
                                 "FROM memory.collected_images "
                                 "WHERE rowid=(SELECT rowid FROM memory.collected_images WHERE imgid=%d)+%d",
                                 imgid, diff);
  // clang-format on
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    new_offset = sqlite3_column_int(stmt, 0);
    new_id = sqlite3_column_int(stmt, 1);
  }
  else if(diff > 0)
  {
    // if we are here, that means that the current is not anymore in the list
    // in this case, let's use the current offset image
    new_id = dt_ui_thumbtable(darktable.gui->ui)->offset_imgid;
    new_offset = dt_ui_thumbtable(darktable.gui->ui)->offset;
  }
  else
  {
    // if we are here, that means that the current is not anymore in the list
    // in this case, let's use the image before current offset
    new_offset = MAX(1, dt_ui_thumbtable(darktable.gui->ui)->offset - 1);
    sqlite3_stmt *stmt2;
    gchar *query2 = g_strdup_printf("SELECT imgid FROM memory.collected_images WHERE rowid=%d", new_offset);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query2, -1, &stmt2, NULL);
    if(sqlite3_step(stmt2) == SQLITE_ROW)
    {
      new_id = sqlite3_column_int(stmt2, 0);
    }
    else
    {
      new_id = dt_ui_thumbtable(darktable.gui->ui)->offset_imgid;
      new_offset = dt_ui_thumbtable(darktable.gui->ui)->offset;
    }
    g_free(query2);
    sqlite3_finalize(stmt2);
  }
  g_free(query);
  sqlite3_finalize(stmt);

  if(new_id < 0 || new_id == imgid) return;

  // if id seems valid, we change the image and move filmstrip
  _dev_change_image(dev, new_id);
  dt_thumbtable_set_offset(dt_ui_thumbtable(darktable.gui->ui), new_offset, TRUE);

  // if it's a change by key_press, we set mouse_over to the active image
  if(by_key) dt_control_set_mouse_over_id(new_id);
}

static void zoom_key_accel(dt_action_t *action)
{
  dt_develop_t *dev = darktable.develop;
  int zoom, closeup;
  float zoom_x, zoom_y;
  if(!strcmp(action->id, "zoom close-up"))
  {
      zoom = dt_control_get_dev_zoom();
      zoom_x = dt_control_get_dev_zoom_x();
      zoom_y = dt_control_get_dev_zoom_y();
      closeup = dt_control_get_dev_closeup();
      if(zoom == DT_ZOOM_1) closeup = (closeup > 0) ^ 1; // flip closeup/no closeup, no difference whether it was 1 or larger
      dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, DT_ZOOM_1, closeup, NULL, NULL);
      dt_control_set_dev_zoom(DT_ZOOM_1);
      dt_control_set_dev_zoom_x(zoom_x);
      dt_control_set_dev_zoom_y(zoom_y);
      dt_control_set_dev_closeup(closeup);
  }
  else if(!strcmp(action->id, "zoom fill"))
  {
      zoom_x = zoom_y = 0.0f;
      dt_control_set_dev_zoom(DT_ZOOM_FILL);
      dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, DT_ZOOM_FILL, 0, NULL, NULL);
      dt_control_set_dev_zoom_x(zoom_x);
      dt_control_set_dev_zoom_y(zoom_y);
      dt_control_set_dev_closeup(0);
  }
  else if(!strcmp(action->id, "zoom fit"))
  {
      dt_control_set_dev_zoom(DT_ZOOM_FIT);
      dt_control_set_dev_zoom_x(0);
      dt_control_set_dev_zoom_y(0);
      dt_control_set_dev_closeup(0);
  }

  dt_dev_invalidate(dev);
  dt_control_queue_redraw_center();
  dt_control_navigation_redraw();
}

static void zoom_in_callback(dt_action_t *action)
{
  dt_view_t *self = dt_action_view(action);
  dt_develop_t *dev = self->data;

  scrolled(self, dev->width / 2, dev->height / 2, 1, GDK_CONTROL_MASK);
}

static void zoom_out_callback(dt_action_t *action)
{
  dt_view_t *self = dt_action_view(action);
  dt_develop_t *dev = self->data;

  scrolled(self, dev->width / 2, dev->height / 2, 0, GDK_CONTROL_MASK);
}

static void skip_f_key_accel_callback(dt_action_t *action)
{
  dt_dev_jump_image(dt_action_view(action)->data, 1, TRUE);
}

static void skip_b_key_accel_callback(dt_action_t *action)
{
  dt_dev_jump_image(dt_action_view(action)->data, -1, TRUE);
}

static void _darkroom_ui_pipe_finish_signal_callback(gpointer instance, gpointer data)
{
  dt_control_queue_redraw_center();
}

static void _darkroom_ui_preview2_pipe_finish_signal_callback(gpointer instance, gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_develop_t *dev = (dt_develop_t *)self->data;
  if(dev->second_window.widget)
    gtk_widget_queue_draw(dev->second_window.widget);
}

static void _darkroom_ui_favorite_presets_popupmenu(GtkWidget *w, gpointer user_data)
{
  /* create favorites menu and popup */
  dt_gui_favorite_presets_menu_show();

  /* if we got any styles, lets popup menu for selection */
  if(darktable.gui->presets_popup_menu)
  {
    dt_gui_menu_popup(darktable.gui->presets_popup_menu, w, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST);
  }
  else
    dt_control_log(_("no userdefined presets for favorite modules were found"));
}

static void _darkroom_ui_apply_style_activate_callback(const gchar *name)
{
  dt_styles_apply_to_dev(name, darktable.develop->image_storage.id);
}

gboolean _styles_tooltip_callback(GtkWidget* self, gint x, gint y, gboolean keyboard_mode,
                                  GtkTooltip* tooltip, gpointer user_data)
{
  gchar *name = (char *)user_data;
  dt_develop_t *dev = darktable.develop;

  const uint32_t imgid = dev->image_storage.id;

  // write history to ensure the preview will be done with latest
  // development history.
  dt_dev_write_history(dev);

  GtkWidget *ht = dt_gui_style_content_dialog(name, imgid);
  gtk_widget_show_all(ht);

  gtk_tooltip_set_custom(tooltip, ht);

  return TRUE;
}

static void _darkroom_ui_apply_style_popupmenu(GtkWidget *w, gpointer user_data)
{
  /* show styles popup menu */
  GList *styles = dt_styles_get_list("");
  GtkMenuShell *menu = NULL;
  if(styles)
  {
    menu = GTK_MENU_SHELL(gtk_menu_new());
    for(const GList *st_iter = styles; st_iter; st_iter = g_list_next(st_iter))
    {
      dt_style_t *style = (dt_style_t *)st_iter->data;

      gchar **split = g_strsplit(style->name, "|", 0);

      // if sub-menu, do not put leading group in final name

      gchar *mi_name = NULL;

      if(split[1])
      {
        gsize mi_len = 1 + strlen(split[1]);
        for(int i=2; split[i]; i++)
          mi_len += strlen(split[i]) + strlen(" | ");

        mi_name = g_new0(gchar, mi_len);
        gchar* tmp_ptr = g_stpcpy(mi_name, split[1]);
        for(int i=2; split[i]; i++)
        {
          tmp_ptr = g_stpcpy(tmp_ptr, " | ");
          tmp_ptr = g_stpcpy(tmp_ptr, split[i]);
        }
      }
      else
        mi_name = g_strdup(split[0]);

      GtkWidget *mi = gtk_menu_item_new_with_label(mi_name);
      // need a tooltip for the signal below to be raised
      gtk_widget_set_has_tooltip(mi, TRUE);
      g_signal_connect_data(mi, "query-tooltip",
                            G_CALLBACK(_styles_tooltip_callback), g_strdup(style->name), (GClosureNotify)g_free, 0);

      g_free(mi_name);

      // check if we already have a sub-menu with this name
      GtkMenu *sm = NULL;

      GList *children = gtk_container_get_children(GTK_CONTAINER(menu));
      for(const GList *child = children; child; child = g_list_next(child))
      {
        GtkMenuItem *smi = (GtkMenuItem *)child->data;
        if(!g_strcmp0(split[0],gtk_menu_item_get_label(smi)))
        {
          sm = (GtkMenu *)gtk_menu_item_get_submenu(smi);
          break;
        }
      }
      g_list_free(children);

      GtkMenuItem *smi = NULL;

      // no sub-menu, but we need one
      if(!sm && split[1])
      {
        smi = (GtkMenuItem *)gtk_menu_item_new_with_label(split[0]);
        sm = (GtkMenu *)gtk_menu_new();
        gtk_menu_item_set_submenu(smi, GTK_WIDGET(sm));
      }

      if(sm)
        gtk_menu_shell_append(GTK_MENU_SHELL(sm), mi);
      else
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

      if(smi)
      {
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), GTK_WIDGET(smi));
        gtk_widget_show(GTK_WIDGET(smi));
      }

      g_signal_connect_data(G_OBJECT(mi), "activate",
                            G_CALLBACK(_darkroom_ui_apply_style_activate_callback),
                            g_strdup(style->name), (GClosureNotify)g_free, G_CONNECT_SWAPPED);
      gtk_widget_show(mi);

      g_strfreev(split);
    }
    g_list_free_full(styles, dt_style_free);
  }

  /* if we got any styles, lets popup menu for selection */
  if(menu)
  {
    dt_gui_menu_popup(GTK_MENU(menu), w, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST);
  }
  else
    dt_control_log(_("no styles have been created yet"));
}

static void _second_window_quickbutton_clicked(GtkWidget *w, dt_develop_t *dev)
{
  if(dev->second_window.second_wnd && !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)))
  {
    _darkroom_ui_second_window_write_config(dev->second_window.second_wnd);

    gtk_widget_destroy(dev->second_window.second_wnd);
    dev->second_window.second_wnd = NULL;
    dev->second_window.widget = NULL;
  }
  else if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)))
    _darkroom_display_second_window(dev);
}

/** toolbar buttons */

static gboolean _toolbar_show_popup(gpointer user_data)
{
  GtkPopover *popover = GTK_POPOVER(user_data);

  GtkWidget *button = gtk_popover_get_relative_to(popover);
  GdkDevice *pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));

  int x, y;
  GdkWindow *pointer_window = gdk_device_get_window_at_position(pointer, &x, &y);
  gpointer   pointer_widget = NULL;
  if(pointer_window)
    gdk_window_get_user_data(pointer_window, &pointer_widget);

  GdkRectangle rect = { gtk_widget_get_allocated_width(button) / 2, 0, 1, 1 };

  if(pointer_widget && button != pointer_widget)
    gtk_widget_translate_coordinates(pointer_widget, button, x, y, &rect.x, &rect.y);

  gtk_popover_set_pointing_to(popover, &rect);

  // for the guides popover, it need to be updated before we show it
  if(darktable.view_manager && GTK_WIDGET(popover) == darktable.view_manager->guides_popover)
    dt_guides_update_popover_values();

  gtk_widget_show_all(GTK_WIDGET(popover));

  // cancel glib timeout if invoked by long button press
  return FALSE;
}

/* colour assessment */
static int _iso_12646_get_border(dt_develop_t *d)
{
  if(d->iso_12646.enabled)
  {
    return MIN(1.75 * darktable.gui->dpi, 0.3 * MIN(d->width, d->height));
  }
  else
  {
    // Reset border size from config
    return DT_PIXEL_APPLY_DPI(dt_conf_get_int("plugins/darkroom/ui/border_size"));
  }
}

static void _iso_12646_quickbutton_clicked(GtkWidget *w, gpointer user_data)
{
  dt_develop_t *dev = (dt_develop_t *)user_data;
  if(!dev->gui_attached) return;

  dev->iso_12646.enabled = !dev->iso_12646.enabled;
  dev->width = dev->orig_width;
  dev->height = dev->orig_height;
  dev->border_size = _iso_12646_get_border(dev);
  dt_dev_configure(dev, dev->width, dev->height);

  dt_dev_second_window_configure(dev, dev->second_window.orig_width, dev->second_window.orig_width);
  dt_dev_reprocess_center(dev);
  dt_control_queue_redraw_center();
}

/* overlay color */
static void _guides_quickbutton_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_guides_button_toggled(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
  dt_control_queue_redraw_center();
}

static void _guides_view_changed(gpointer instance, dt_view_t *old_view, dt_view_t *new_view, dt_lib_module_t *self)
{
  dt_guides_update_button_state();
}

/* overexposed */
static void _overexposed_quickbutton_clicked(GtkWidget *w, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->overexposed.enabled = !d->overexposed.enabled;
  dt_dev_reprocess_center(d);
}

static void colorscheme_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->overexposed.colorscheme = dt_bauhaus_combobox_get(combo);
  if(d->overexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->overexposed.button));
  else
    dt_dev_reprocess_center(d);
}

static void lower_callback(GtkWidget *slider, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->overexposed.lower = dt_bauhaus_slider_get(slider);
  if(d->overexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->overexposed.button));
  else
    dt_dev_reprocess_center(d);
}

static void upper_callback(GtkWidget *slider, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->overexposed.upper = dt_bauhaus_slider_get(slider);
  if(d->overexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->overexposed.button));
  else
    dt_dev_reprocess_center(d);
}

static void mode_callback(GtkWidget *slider, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->overexposed.mode = dt_bauhaus_combobox_get(slider);
  if(d->overexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->overexposed.button));
  else
    dt_dev_reprocess_center(d);
}

/* rawoverexposed */
static void _rawoverexposed_quickbutton_clicked(GtkWidget *w, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->rawoverexposed.enabled = !d->rawoverexposed.enabled;
  dt_dev_reprocess_center(d);
}

static void rawoverexposed_mode_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->rawoverexposed.mode = dt_bauhaus_combobox_get(combo);
  if(d->rawoverexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->rawoverexposed.button));
  else
    dt_dev_reprocess_center(d);
}

static void rawoverexposed_colorscheme_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->rawoverexposed.colorscheme = dt_bauhaus_combobox_get(combo);
  if(d->rawoverexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->rawoverexposed.button));
  else
    dt_dev_reprocess_center(d);
}

static void rawoverexposed_threshold_callback(GtkWidget *slider, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->rawoverexposed.threshold = dt_bauhaus_slider_get(slider);
  if(d->rawoverexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->rawoverexposed.button));
  else
    dt_dev_reprocess_center(d);
}

/* softproof */
static void _softproof_quickbutton_clicked(GtkWidget *w, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  if(darktable.color_profiles->mode == DT_PROFILE_SOFTPROOF)
    darktable.color_profiles->mode = DT_PROFILE_NORMAL;
  else
    darktable.color_profiles->mode = DT_PROFILE_SOFTPROOF;

  _update_softproof_gamut_checking(d);

  dt_dev_reprocess_center(d);
}

/* gamut */
static void _gamut_quickbutton_clicked(GtkWidget *w, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  if(darktable.color_profiles->mode == DT_PROFILE_GAMUTCHECK)
    darktable.color_profiles->mode = DT_PROFILE_NORMAL;
  else
    darktable.color_profiles->mode = DT_PROFILE_GAMUTCHECK;

  _update_softproof_gamut_checking(d);

  dt_dev_reprocess_center(d);
}

/* set the gui state for both softproof and gamut checking */
static void _update_softproof_gamut_checking(dt_develop_t *d)
{
  g_signal_handlers_block_by_func(d->profile.softproof_button, _softproof_quickbutton_clicked, d);
  g_signal_handlers_block_by_func(d->profile.gamut_button, _gamut_quickbutton_clicked, d);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->profile.softproof_button), darktable.color_profiles->mode == DT_PROFILE_SOFTPROOF);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->profile.gamut_button), darktable.color_profiles->mode == DT_PROFILE_GAMUTCHECK);

  g_signal_handlers_unblock_by_func(d->profile.softproof_button, _softproof_quickbutton_clicked, d);
  g_signal_handlers_unblock_by_func(d->profile.gamut_button, _gamut_quickbutton_clicked, d);
}

static void display_intent_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  const int pos = dt_bauhaus_combobox_get(combo);

  dt_iop_color_intent_t new_intent = darktable.color_profiles->display_intent;

  // we are not using the int value directly so it's robust against changes on lcms' side
  switch(pos)
  {
    case 0:
      new_intent = DT_INTENT_PERCEPTUAL;
      break;
    case 1:
      new_intent = DT_INTENT_RELATIVE_COLORIMETRIC;
      break;
    case 2:
      new_intent = DT_INTENT_SATURATION;
      break;
    case 3:
      new_intent = DT_INTENT_ABSOLUTE_COLORIMETRIC;
      break;
  }

  if(new_intent != darktable.color_profiles->display_intent)
  {
    darktable.color_profiles->display_intent = new_intent;
    dt_dev_reprocess_all(d);
  }
}

static void display2_intent_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  const int pos = dt_bauhaus_combobox_get(combo);

  dt_iop_color_intent_t new_intent = darktable.color_profiles->display2_intent;

  // we are not using the int value directly so it's robust against changes on lcms' side
  switch(pos)
  {
    case 0:
      new_intent = DT_INTENT_PERCEPTUAL;
      break;
    case 1:
      new_intent = DT_INTENT_RELATIVE_COLORIMETRIC;
      break;
    case 2:
      new_intent = DT_INTENT_SATURATION;
      break;
    case 3:
      new_intent = DT_INTENT_ABSOLUTE_COLORIMETRIC;
      break;
  }

  if(new_intent != darktable.color_profiles->display2_intent)
  {
    darktable.color_profiles->display2_intent = new_intent;
    dt_dev_reprocess_all(d);
  }
}

static void softproof_profile_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  gboolean profile_changed = FALSE;
  const int pos = dt_bauhaus_combobox_get(combo);
  for(GList *profiles = darktable.color_profiles->profiles; profiles; profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)profiles->data;
    if(pp->out_pos == pos)
    {
      if(darktable.color_profiles->softproof_type != pp->type
        || (darktable.color_profiles->softproof_type == DT_COLORSPACE_FILE
            && strcmp(darktable.color_profiles->softproof_filename, pp->filename)))

      {
        darktable.color_profiles->softproof_type = pp->type;
        g_strlcpy(darktable.color_profiles->softproof_filename, pp->filename,
                  sizeof(darktable.color_profiles->softproof_filename));
        profile_changed = TRUE;
      }
      goto end;
    }
  }

  // profile not found, fall back to sRGB. shouldn't happen
  fprintf(stderr, "can't find softproof profile `%s', using sRGB instead\n", dt_bauhaus_combobox_get_text(combo));
  profile_changed = darktable.color_profiles->softproof_type != DT_COLORSPACE_SRGB;
  darktable.color_profiles->softproof_type = DT_COLORSPACE_SRGB;
  darktable.color_profiles->softproof_filename[0] = '\0';

end:
  if(profile_changed)
  {
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED, DT_COLORSPACES_PROFILE_TYPE_SOFTPROOF);
    dt_dev_reprocess_all(d);
  }
}

static void display_profile_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  gboolean profile_changed = FALSE;
  const int pos = dt_bauhaus_combobox_get(combo);
  for(GList *profiles = darktable.color_profiles->profiles; profiles; profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)profiles->data;
    if(pp->display_pos == pos)
    {
      if(darktable.color_profiles->display_type != pp->type
        || (darktable.color_profiles->display_type == DT_COLORSPACE_FILE
            && strcmp(darktable.color_profiles->display_filename, pp->filename)))
      {
        darktable.color_profiles->display_type = pp->type;
        g_strlcpy(darktable.color_profiles->display_filename, pp->filename,
                  sizeof(darktable.color_profiles->display_filename));
        profile_changed = TRUE;
      }
      goto end;
    }
  }

  // profile not found, fall back to system display profile. shouldn't happen
  fprintf(stderr, "can't find display profile `%s', using system display profile instead\n", dt_bauhaus_combobox_get_text(combo));
  profile_changed = darktable.color_profiles->display_type != DT_COLORSPACE_DISPLAY;
  darktable.color_profiles->display_type = DT_COLORSPACE_DISPLAY;
  darktable.color_profiles->display_filename[0] = '\0';

end:
  if(profile_changed)
  {
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);
    dt_colorspaces_update_display_transforms();
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED, DT_COLORSPACES_PROFILE_TYPE_DISPLAY);
    dt_dev_reprocess_all(d);
  }
}

static void display2_profile_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  gboolean profile_changed = FALSE;
  const int pos = dt_bauhaus_combobox_get(combo);
  for(GList *profiles = darktable.color_profiles->profiles; profiles; profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)profiles->data;
    if(pp->display2_pos == pos)
    {
      if(darktable.color_profiles->display2_type != pp->type
         || (darktable.color_profiles->display2_type == DT_COLORSPACE_FILE
             && strcmp(darktable.color_profiles->display2_filename, pp->filename)))
      {
        darktable.color_profiles->display2_type = pp->type;
        g_strlcpy(darktable.color_profiles->display2_filename, pp->filename,
                  sizeof(darktable.color_profiles->display2_filename));
        profile_changed = TRUE;
      }
      goto end;
    }
  }

  // profile not found, fall back to system display2 profile. shouldn't happen
  fprintf(stderr, "can't find preview display profile `%s', using system display profile instead\n",
          dt_bauhaus_combobox_get_text(combo));
  profile_changed = darktable.color_profiles->display2_type != DT_COLORSPACE_DISPLAY2;
  darktable.color_profiles->display2_type = DT_COLORSPACE_DISPLAY2;
  darktable.color_profiles->display2_filename[0] = '\0';

end:
  if(profile_changed)
  {
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);
    dt_colorspaces_update_display2_transforms();
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                            DT_COLORSPACES_PROFILE_TYPE_DISPLAY2);
    dt_dev_reprocess_all(d);
  }
}

static void histogram_profile_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  gboolean profile_changed = FALSE;
  const int pos = dt_bauhaus_combobox_get(combo);
  for(GList *profiles = darktable.color_profiles->profiles; profiles; profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)profiles->data;
    if(pp->category_pos == pos)
    {
      if(darktable.color_profiles->histogram_type != pp->type
        || (darktable.color_profiles->histogram_type == DT_COLORSPACE_FILE
            && strcmp(darktable.color_profiles->histogram_filename, pp->filename)))
      {
        darktable.color_profiles->histogram_type = pp->type;
        g_strlcpy(darktable.color_profiles->histogram_filename, pp->filename,
                  sizeof(darktable.color_profiles->histogram_filename));
        profile_changed = TRUE;
      }
      goto end;
    }
  }

  // profile not found, fall back to export profile. shouldn't happen
  fprintf(stderr, "can't find histogram profile `%s', using export profile instead\n", dt_bauhaus_combobox_get_text(combo));
  profile_changed = darktable.color_profiles->histogram_type != DT_COLORSPACE_WORK;
  darktable.color_profiles->histogram_type = DT_COLORSPACE_WORK;
  darktable.color_profiles->histogram_filename[0] = '\0';

end:
  if(profile_changed)
  {
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED, DT_COLORSPACES_PROFILE_TYPE_HISTOGRAM);
    dt_dev_reprocess_all(d);
  }
}

// FIXME: turning off lcms2 in prefs hides the widget but leaves the window sized like before -> ugly-ish
static void _preference_changed(gpointer instance, gpointer user_data)
{
  GtkWidget *display_intent = GTK_WIDGET(user_data);

  const int force_lcms2 = dt_conf_get_bool("plugins/lighttable/export/force_lcms2");
  if(force_lcms2)
  {
    gtk_widget_set_no_show_all(display_intent, FALSE);
    gtk_widget_set_visible(display_intent, TRUE);
  }
  else
  {
    gtk_widget_set_no_show_all(display_intent, TRUE);
    gtk_widget_set_visible(display_intent, FALSE);
  }
  dt_get_sysresource_level();
  dt_opencl_update_settings();
  dt_configure_ppd_dpi(darktable.gui);
}

static void _preference_prev_downsample_change(gpointer instance, gpointer user_data)
{
  if(user_data != NULL)
  {
    float *ds_value = user_data;
    *ds_value = dt_dev_get_preview_downsampling();
  }
}

static void _preference_changed_button_hide(gpointer instance, dt_develop_t *dev)
{
  for(const GList *modules = dev->iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);

    if(module->header)
      add_remove_mask_indicator(module, (module->blend_params->mask_mode != DEVELOP_MASK_DISABLED) &&
                                (module->blend_params->mask_mode != DEVELOP_MASK_ENABLED));
  }
}

static void _update_display_profile_cmb(GtkWidget *cmb_display_profile)
{
  for(const GList *l = darktable.color_profiles->profiles; l; l = g_list_next(l))
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)l->data;
    if(prof->display_pos > -1)
    {
      if(prof->type == darktable.color_profiles->display_type
         && (prof->type != DT_COLORSPACE_FILE
             || !strcmp(prof->filename, darktable.color_profiles->display_filename)))
      {
        if(dt_bauhaus_combobox_get(cmb_display_profile) != prof->display_pos)
        {
          dt_bauhaus_combobox_set(cmb_display_profile, prof->display_pos);
          break;
        }
      }
    }
  }
}

static void _update_display2_profile_cmb(GtkWidget *cmb_display_profile)
{
  for(const GList *l = darktable.color_profiles->profiles; l; l = g_list_next(l))
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)l->data;
    if(prof->display2_pos > -1)
    {
      if(prof->type == darktable.color_profiles->display2_type
         && (prof->type != DT_COLORSPACE_FILE
             || !strcmp(prof->filename, darktable.color_profiles->display2_filename)))
      {
        if(dt_bauhaus_combobox_get(cmb_display_profile) != prof->display2_pos)
        {
          dt_bauhaus_combobox_set(cmb_display_profile, prof->display2_pos);
          break;
        }
      }
    }
  }
}

static void _display_profile_changed(gpointer instance, uint8_t profile_type, gpointer user_data)
{
  GtkWidget *cmb_display_profile = GTK_WIDGET(user_data);

  _update_display_profile_cmb(cmb_display_profile);
}

static void _display2_profile_changed(gpointer instance, uint8_t profile_type, gpointer user_data)
{
  GtkWidget *cmb_display_profile = GTK_WIDGET(user_data);

  _update_display2_profile_cmb(cmb_display_profile);
}

/** end of toolbox */

static void _brush_size_up_callback(dt_action_t *action)
{
  dt_develop_t *dev = dt_action_view(action)->data;

  if(dev->form_visible) dt_masks_events_mouse_scrolled(dev->gui_module, 0, 0, 1, 0);
}
static void _brush_size_down_callback(dt_action_t *action)
{
  dt_develop_t *dev = dt_action_view(action)->data;

  if(dev->form_visible) dt_masks_events_mouse_scrolled(dev->gui_module, 0, 0, 0, 0);
}

static void _brush_hardness_up_callback(dt_action_t *action)
{
  dt_develop_t *dev = dt_action_view(action)->data;

  if(dev->form_visible) dt_masks_events_mouse_scrolled(dev->gui_module, 0, 0, 1, GDK_SHIFT_MASK);
}
static void _brush_hardness_down_callback(dt_action_t *action)
{
  dt_develop_t *dev = dt_action_view(action)->data;

  if(dev->form_visible) dt_masks_events_mouse_scrolled(dev->gui_module, 0, 0, 0, GDK_SHIFT_MASK);
}

static void _brush_opacity_up_callback(dt_action_t *action)
{
  dt_develop_t *dev = dt_action_view(action)->data;

  if(dev->form_visible) dt_masks_events_mouse_scrolled(dev->gui_module, 0, 0, 1, GDK_CONTROL_MASK);
}
static void _brush_opacity_down_callback(dt_action_t *action)
{
  dt_develop_t *dev = dt_action_view(action)->data;

  if(dev->form_visible) dt_masks_events_mouse_scrolled(dev->gui_module, 0, 0, 0, GDK_CONTROL_MASK);
}

static void _overlay_cycle_callback(dt_action_t *action)
{
  const int currentval = dt_conf_get_int("darkroom/ui/overlay_color");
  const int nextval = (currentval + 1) % 6; // colors can go from 0 to 5
  dt_conf_set_int("darkroom/ui/overlay_color", nextval);
  dt_guides_set_overlay_colors();
  dt_control_queue_redraw_center();
}

static void _toggle_mask_visibility_callback(dt_action_t *action)
{
  if(darktable.gui->reset) return;

  dt_develop_t *dev = dt_action_view(action)->data;
  dt_iop_module_t *mod = dev->gui_module;

  //retouch and spot removal module use masks differently and have different buttons associated
  //keep the shortcuts independent
  if(mod && strcmp(mod->so->op, "spots") != 0 && strcmp(mod->so->op, "retouch") != 0)
  {
    dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)mod->blend_data;

    ++darktable.gui->reset;

    dt_iop_color_picker_reset(mod, TRUE);

    dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, mod->blend_params->mask_id);
    if(grp && (grp->type & DT_MASKS_GROUP) && grp->points)
    {
      if(bd->masks_shown == DT_MASKS_EDIT_OFF)
        bd->masks_shown = DT_MASKS_EDIT_FULL;
      else
        bd->masks_shown = DT_MASKS_EDIT_OFF;

      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), bd->masks_shown != DT_MASKS_EDIT_OFF);
      dt_masks_set_edit_mode(mod, bd->masks_shown);

      // set all add shape buttons to inactive
      for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[n]), FALSE);
    }

    --darktable.gui->reset;
  }
}

static void _darkroom_undo_callback(dt_action_t *action)
{
  dt_undo_do_undo(darktable.undo, DT_UNDO_DEVELOP);
}

static void _darkroom_redo_callback(dt_action_t *action)
{
  dt_undo_do_redo(darktable.undo, DT_UNDO_DEVELOP);
}

static void change_slider_accel_precision(dt_action_t *action);

static float _action_process_skip_mouse(gpointer target, dt_action_element_t element, dt_action_effect_t effect, float move_size)
{
  if(!isnan(move_size))
  {
    switch(effect)
    {
    case DT_ACTION_EFFECT_ON:
      darktable.develop->darkroom_skip_mouse_events = TRUE;
      break;
    case DT_ACTION_EFFECT_OFF:
      darktable.develop->darkroom_skip_mouse_events = FALSE;
      break;
    default:
      darktable.develop->darkroom_skip_mouse_events ^= TRUE;
    }
  }

  return darktable.develop->darkroom_skip_mouse_events;
}

const dt_action_def_t dt_action_def_skip_mouse
  = { N_("hold"),
      _action_process_skip_mouse,
      dt_action_elements_hold,
      NULL, TRUE };

static float _action_process_preview(gpointer target, dt_action_element_t element, dt_action_effect_t effect, float move_size)
{
  dt_develop_t *lib = darktable.view_manager->proxy.darkroom.view->data;

  if(!isnan(move_size))
  {
    if(lib->full_preview)
    {
      if(effect != DT_ACTION_EFFECT_ON)
      {
        dt_ui_restore_panels(darktable.gui->ui);
        dt_control_set_dev_zoom(lib->full_preview_last_zoom);
        dt_control_set_dev_zoom_x(lib->full_preview_last_zoom_x);
        dt_control_set_dev_zoom_y(lib->full_preview_last_zoom_y);
        dt_control_set_dev_closeup(lib->full_preview_last_closeup);
        lib->full_preview = FALSE;
        dt_iop_request_focus(lib->full_preview_last_module);
        dt_masks_set_edit_mode(darktable.develop->gui_module, lib->full_preview_masks_state);
        dt_dev_invalidate(darktable.develop);
        dt_control_queue_redraw_center();
        dt_control_navigation_redraw();
      }
    }
    else
    {
      if(effect != DT_ACTION_EFFECT_OFF &&
         lib->preview_status != DT_DEV_PIXELPIPE_DIRTY &&
         lib->preview_status != DT_DEV_PIXELPIPE_INVALID)
      {
        lib->full_preview = TRUE;
        // we hide all panels
        for(int k = 0; k < DT_UI_PANEL_SIZE; k++)
          dt_ui_panel_show(darktable.gui->ui, k, FALSE, FALSE);
        // we remember the masks edit state
        if(darktable.develop->gui_module)
        {
          dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)darktable.develop->gui_module->blend_data;
          if(bd) lib->full_preview_masks_state = bd->masks_shown;
        }
        // we set the zoom values to "fit"
        lib->full_preview_last_zoom = dt_control_get_dev_zoom();
        lib->full_preview_last_zoom_x = dt_control_get_dev_zoom_x();
        lib->full_preview_last_zoom_y = dt_control_get_dev_zoom_y();
        lib->full_preview_last_closeup = dt_control_get_dev_closeup();
        dt_control_set_dev_zoom(DT_ZOOM_FIT);
        dt_control_set_dev_zoom_x(0);
        dt_control_set_dev_zoom_y(0);
        dt_control_set_dev_closeup(0);
        // we quit the active iop if any
        lib->full_preview_last_module = darktable.develop->gui_module;
        dt_iop_request_focus(NULL);
        gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));
        dt_dev_invalidate(darktable.develop);
        dt_control_queue_redraw_center();
      }
    }
  }

  return lib->full_preview;
}

const dt_action_def_t dt_action_def_preview
  = { N_("preview"),
      _action_process_preview,
      dt_action_elements_hold,
      NULL, TRUE };

static float _action_process_move(gpointer target, dt_action_element_t element, dt_action_effect_t effect, float move_size)
{
  dt_develop_t *dev = darktable.view_manager->proxy.darkroom.view->data;

  if(!isnan(move_size))
  {
    dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
    const int closeup = dt_control_get_dev_closeup();
    float scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 0);
    int procw, proch;
    dt_dev_get_processed_size(dev, &procw, &proch);

    // For each cursor press, move one screen by default
    float step_changex = dev->width / (procw * scale);
    float step_changey = dev->height / (proch * scale);
    float factor = 0.2f * move_size;
    if(effect == DT_ACTION_EFFECT_DOWN) factor *= -1;

    float zx = dt_control_get_dev_zoom_x();
    float zy = dt_control_get_dev_zoom_y();

    if(target)
      zx += step_changex * factor;
    else
      zy -= step_changey * factor;

    dt_dev_check_zoom_bounds(dev, &zx, &zy, zoom, closeup, NULL, NULL);
    dt_control_set_dev_zoom_x(zx);
    dt_control_set_dev_zoom_y(zy);

    dt_dev_invalidate(dev);
    dt_control_queue_redraw_center();
    dt_control_navigation_redraw();
  }

  return 0; // FIXME return position (%)
}

const dt_action_element_def_t _action_elements_move[]
  = { { NULL, dt_action_effect_value } };

const dt_action_def_t _action_def_move
  = { N_("move"),
      _action_process_move,
      _action_elements_move,
      NULL, TRUE };

static gboolean _quickbutton_press_release(GtkWidget *button, GdkEventButton *event, GtkWidget *popover)
{
  static guint start_time = 0;

  int delay = 0;
  g_object_get(gtk_settings_get_default(), "gtk-long-press-time", &delay, NULL);

  if((event->type == GDK_BUTTON_PRESS && event->button == 3) ||
     (event->type == GDK_BUTTON_RELEASE && event->time - start_time > delay))
  {
    gtk_popover_set_relative_to(GTK_POPOVER(popover), button);

    g_object_set(G_OBJECT(popover), "transitions-enabled", FALSE, NULL);

    _toolbar_show_popup(popover);
    return TRUE;
  }
  else
  {
    start_time = event->time;
    return FALSE;
  }
}

void connect_button_press_release(GtkWidget *w, GtkWidget *p)
{
  g_signal_connect(w, "button-press-event", G_CALLBACK(_quickbutton_press_release), p);
  g_signal_connect(w, "button-release-event", G_CALLBACK(_quickbutton_press_release), p);
}

void gui_init(dt_view_t *self)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;

  dt_action_t *sa = &self->actions, *ac = NULL;

  /*
   * Add view specific tool buttons
   */

  /* create favorite plugin preset popup tool */
  GtkWidget *favorite_presets = dtgtk_button_new(dtgtk_cairo_paint_presets, 0, NULL);
  dt_action_define(sa, NULL, N_("quick access to presets"), favorite_presets, &dt_action_def_button);
  gtk_widget_set_tooltip_text(favorite_presets, _("quick access to presets"));
  g_signal_connect(G_OBJECT(favorite_presets), "clicked", G_CALLBACK(_darkroom_ui_favorite_presets_popupmenu),
                   NULL);
  dt_gui_add_help_link(favorite_presets, dt_get_help_url("favorite_presets"));
  dt_view_manager_view_toolbox_add(darktable.view_manager, favorite_presets, DT_VIEW_DARKROOM);

  /* create quick styles popup menu tool */
  GtkWidget *styles = dtgtk_button_new(dtgtk_cairo_paint_styles, 0, NULL);
  dt_action_define(sa, NULL, N_("quick access to styles"), styles, &dt_action_def_button);
  g_signal_connect(G_OBJECT(styles), "clicked", G_CALLBACK(_darkroom_ui_apply_style_popupmenu), NULL);
  gtk_widget_set_tooltip_text(styles, _("quick access for applying any of your styles"));
  dt_gui_add_help_link(styles, dt_get_help_url("bottom_panel_styles"));
  dt_view_manager_view_toolbox_add(darktable.view_manager, styles, DT_VIEW_DARKROOM);

  /* create second window display button */
  dev->second_window.button = dtgtk_togglebutton_new(dtgtk_cairo_paint_display2, 0, NULL);
  dt_action_define(sa, NULL, N_("second window"), dev->second_window.button, &dt_action_def_toggle);
  g_signal_connect(G_OBJECT(dev->second_window.button), "clicked", G_CALLBACK(_second_window_quickbutton_clicked),
                   dev);
  gtk_widget_set_tooltip_text(dev->second_window.button, _("display a second darkroom image window"));
  dt_view_manager_view_toolbox_add(darktable.view_manager, dev->second_window.button, DT_VIEW_DARKROOM);

  /* Enable ISO 12646-compliant colour assessment conditions */
  dev->iso_12646.button = dtgtk_togglebutton_new(dtgtk_cairo_paint_bulb, 0, NULL);
  ac = dt_action_define(sa, NULL, N_("color assessment"), dev->iso_12646.button, &dt_action_def_toggle);
  dt_shortcut_register(ac, 0, 0, GDK_KEY_b, GDK_CONTROL_MASK);
  gtk_widget_set_tooltip_text(dev->iso_12646.button,
                              _("toggle ISO 12646 color assessment conditions"));
  g_signal_connect(G_OBJECT(dev->iso_12646.button), "clicked", G_CALLBACK(_iso_12646_quickbutton_clicked), dev);
  dt_view_manager_module_toolbox_add(darktable.view_manager, dev->iso_12646.button, DT_VIEW_DARKROOM);

  GtkWidget *colorscheme, *mode;

  /* create rawoverexposed popup tool */
  {
    // the button
    dev->rawoverexposed.button = dtgtk_togglebutton_new(dtgtk_cairo_paint_rawoverexposed, 0, NULL);
    ac = dt_action_define(sa, N_("raw overexposed"), N_("toggle"), dev->rawoverexposed.button, &dt_action_def_toggle);
    dt_shortcut_register(ac, 0, 0, GDK_KEY_o, GDK_SHIFT_MASK);
    gtk_widget_set_tooltip_text(dev->rawoverexposed.button,
                                _("toggle raw over exposed indication\nright click for options"));
    g_signal_connect(G_OBJECT(dev->rawoverexposed.button), "clicked",
                     G_CALLBACK(_rawoverexposed_quickbutton_clicked), dev);
    dt_view_manager_module_toolbox_add(darktable.view_manager, dev->rawoverexposed.button, DT_VIEW_DARKROOM);
    dt_gui_add_help_link(dev->rawoverexposed.button, dt_get_help_url("rawoverexposed"));

    // and the popup window
    dev->rawoverexposed.floating_window = gtk_popover_new(dev->rawoverexposed.button);
    connect_button_press_release(dev->rawoverexposed.button, dev->rawoverexposed.floating_window);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(dev->rawoverexposed.floating_window), vbox);

    /** let's fill the encapsulating widgets */
    /* mode of operation */
    DT_BAUHAUS_COMBOBOX_NEW_FULL(mode, self, N_("raw overexposed"), N_("mode"),
                                 _("select how to mark the clipped pixels"),
                                 dev->rawoverexposed.mode, rawoverexposed_mode_callback, dev,
                                 N_("mark with CFA color"), N_("mark with solid color"), N_("false color"));
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(mode), TRUE, TRUE, 0);

    DT_BAUHAUS_COMBOBOX_NEW_FULL(colorscheme, self, N_("raw overexposed"), N_("color scheme"),
                                _("select the solid color to indicate over exposure.\nwill only be used if mode = mark with solid color"),
                                dev->rawoverexposed.colorscheme,
                                rawoverexposed_colorscheme_callback, dev,
                                NC_("solidcolor", "red"),
                                NC_("solidcolor", "green"),
                                NC_("solidcolor", "blue"),
                                NC_("solidcolor", "black"));
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(colorscheme), TRUE, TRUE, 0);

    /* threshold */
    GtkWidget *threshold = dt_bauhaus_slider_new_action(DT_ACTION(self), 0.0, 2.0, 0.01, 1.0, 3);
    dt_bauhaus_slider_set(threshold, dev->rawoverexposed.threshold);
    dt_bauhaus_widget_set_label(threshold, N_("raw overexposed"), N_("clipping threshold"));
    gtk_widget_set_tooltip_text(
        threshold, _("threshold of what shall be considered overexposed\n1.0 - white level\n0.0 - black level"));
    g_signal_connect(G_OBJECT(threshold), "value-changed", G_CALLBACK(rawoverexposed_threshold_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(threshold), TRUE, TRUE, 0);

    gtk_widget_show_all(vbox);
  }

  /* create overexposed popup tool */
  {
    // the button
    dev->overexposed.button = dtgtk_togglebutton_new(dtgtk_cairo_paint_overexposed, 0, NULL);
    ac = dt_action_define(DT_ACTION(self), N_("overexposed"), N_("toggle"), dev->overexposed.button, &dt_action_def_toggle);
    dt_shortcut_register(ac, 0, 0, GDK_KEY_o, 0);
    gtk_widget_set_tooltip_text(dev->overexposed.button,
                                _("toggle clipping indication\nright click for options"));
    g_signal_connect(G_OBJECT(dev->overexposed.button), "clicked",
                     G_CALLBACK(_overexposed_quickbutton_clicked), dev);
    dt_view_manager_module_toolbox_add(darktable.view_manager, dev->overexposed.button, DT_VIEW_DARKROOM);
    dt_gui_add_help_link(dev->overexposed.button, dt_get_help_url("overexposed"));

    // and the popup window
    dev->overexposed.floating_window = gtk_popover_new(dev->overexposed.button);
    connect_button_press_release(dev->overexposed.button, dev->overexposed.floating_window);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(dev->overexposed.floating_window), vbox);

    /** let's fill the encapsulating widgets */
    /* preview mode */
    DT_BAUHAUS_COMBOBOX_NEW_FULL(mode, self, N_("overexposed"), N_("clipping preview mode"),
                                 _("select the metric you want to preview\nfull gamut is the combination of all other modes"),
                                 dev->overexposed.mode, mode_callback, dev,
                                 N_("full gamut"), N_("any RGB channel"), N_("luminance only"), N_("saturation only"));
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(mode), TRUE, TRUE, 0);

    /* color scheme */
    DT_BAUHAUS_COMBOBOX_NEW_FULL(colorscheme, self, N_("overexposed"), N_("color scheme"),
                                 _("select colors to indicate clipping"),
                                 dev->overexposed.colorscheme, colorscheme_callback, dev,
                                 N_("black & white"), N_("red & blue"), N_("purple & green"));
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(colorscheme), TRUE, TRUE, 0);

    /* lower */
    GtkWidget *lower = dt_bauhaus_slider_new_action(DT_ACTION(self), -32., -4., 1., -12.69, 2);
    dt_bauhaus_slider_set(lower, dev->overexposed.lower);
    dt_bauhaus_slider_set_format(lower, _(" EV"));
    dt_bauhaus_widget_set_label(lower, N_("overexposed"), N_("lower threshold"));
    gtk_widget_set_tooltip_text(lower, _("clipping threshold for the black point,\n"
                                         "in EV, relatively to white (0 EV).\n"
                                         "8 bits sRGB clips blacks at -12.69 EV,\n"
                                         "8 bits Adobe RGB clips blacks at -19.79 EV,\n"
                                         "16 bits sRGB clips blacks at -20.69 EV,\n"
                                         "typical fine-art mat prints produce black at -5.30 EV,\n"
                                         "typical color glossy prints produce black at -8.00 EV,\n"
                                         "typical B&W glossy prints produce black at -9.00 EV."
                                         ));
    g_signal_connect(G_OBJECT(lower), "value-changed", G_CALLBACK(lower_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(lower), TRUE, TRUE, 0);

    /* upper */
    GtkWidget *upper = dt_bauhaus_slider_new_action(DT_ACTION(self), 0.0, 100.0, 0.1, 99.99, 2);
    dt_bauhaus_slider_set(upper, dev->overexposed.upper);
    dt_bauhaus_slider_set_format(upper, "%");
    dt_bauhaus_widget_set_label(upper, N_("overexposed"), N_("upper threshold"));
    /* xgettext:no-c-format */
    gtk_widget_set_tooltip_text(upper, _("clipping threshold for the white point.\n"
                                         "100% is peak medium luminance."));
    g_signal_connect(G_OBJECT(upper), "value-changed", G_CALLBACK(upper_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(upper), TRUE, TRUE, 0);

    gtk_widget_show_all(vbox);
  }

  /* create profile popup tool & buttons (softproof + gamut) */
  {
    // the softproof button
    dev->profile.softproof_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_softproof, 0, NULL);
    ac = dt_action_define(sa, NULL, N_("softproof"), dev->profile.softproof_button, &dt_action_def_toggle);
    dt_shortcut_register(ac, 0, 0, GDK_KEY_s, GDK_CONTROL_MASK);
    gtk_widget_set_tooltip_text(dev->profile.softproof_button,
                                _("toggle softproofing\nright click for profile options"));
    g_signal_connect(G_OBJECT(dev->profile.softproof_button), "clicked",
                     G_CALLBACK(_softproof_quickbutton_clicked), dev);
    dt_view_manager_module_toolbox_add(darktable.view_manager, dev->profile.softproof_button, DT_VIEW_DARKROOM);
    dt_gui_add_help_link(dev->profile.softproof_button, dt_get_help_url("softproof"));

    // the gamut check button
    dev->profile.gamut_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_gamut_check, 0, NULL);
    ac = dt_action_define(sa, NULL, N_("gamut check"), dev->profile.gamut_button, &dt_action_def_toggle);
    dt_shortcut_register(ac, 0, 0, GDK_KEY_g, GDK_CONTROL_MASK);
    gtk_widget_set_tooltip_text(dev->profile.gamut_button,
                 _("toggle gamut checking\nright click for profile options"));
    g_signal_connect(G_OBJECT(dev->profile.gamut_button), "clicked",
                     G_CALLBACK(_gamut_quickbutton_clicked), dev);
    dt_view_manager_module_toolbox_add(darktable.view_manager, dev->profile.gamut_button, DT_VIEW_DARKROOM);
    dt_gui_add_help_link(dev->profile.gamut_button, dt_get_help_url("gamut"));

    // and the popup window, which is shared between the two profile buttons
    dev->profile.floating_window = gtk_popover_new(NULL);
    connect_button_press_release(dev->second_window.button, dev->profile.floating_window);
    connect_button_press_release(dev->profile.softproof_button, dev->profile.floating_window);
    connect_button_press_release(dev->profile.gamut_button, dev->profile.floating_window);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(dev->profile.floating_window), vbox);

    /** let's fill the encapsulating widgets */
    char datadir[PATH_MAX] = { 0 };
    char confdir[PATH_MAX] = { 0 };
    dt_loc_get_user_config_dir(confdir, sizeof(confdir));
    dt_loc_get_datadir(datadir, sizeof(datadir));
    const int force_lcms2 = dt_conf_get_bool("plugins/lighttable/export/force_lcms2");

    static const gchar *intents_list[]
      = { N_("perceptual"),
          N_("relative colorimetric"),
          NC_("rendering intent", "saturation"),
          N_("absolute colorimetric"),
          NULL };

    GtkWidget *display_intent = dt_bauhaus_combobox_new_full(DT_ACTION(self), N_("profiles"), N_("intent"),
                                                             "", 0, display_intent_callback, dev, intents_list);
    GtkWidget *display2_intent = dt_bauhaus_combobox_new_full(DT_ACTION(self), N_("profiles"), N_("preview intent"),
                                                              "", 0, display2_intent_callback, dev, intents_list);

    if(!force_lcms2)
    {
      gtk_widget_set_no_show_all(display_intent, TRUE);
      gtk_widget_set_no_show_all(display2_intent, TRUE);
    }

    GtkWidget *display_profile = dt_bauhaus_combobox_new_action(DT_ACTION(self));
    GtkWidget *display2_profile = dt_bauhaus_combobox_new_action(DT_ACTION(self));
    GtkWidget *softproof_profile = dt_bauhaus_combobox_new_action(DT_ACTION(self));
    GtkWidget *histogram_profile = dt_bauhaus_combobox_new_action(DT_ACTION(self));

    dt_bauhaus_widget_set_label(display_profile, N_("profiles"), N_("display profile"));
    dt_bauhaus_widget_set_label(display2_profile, N_("profiles"), N_("preview display profile"));
    dt_bauhaus_widget_set_label(softproof_profile, N_("profiles"), N_("softproof profile"));
    dt_bauhaus_widget_set_label(histogram_profile, N_("profiles"), N_("histogram profile"));

    dt_bauhaus_combobox_set_entries_ellipsis(display_profile, PANGO_ELLIPSIZE_MIDDLE);
    dt_bauhaus_combobox_set_entries_ellipsis(display2_profile, PANGO_ELLIPSIZE_MIDDLE);
    dt_bauhaus_combobox_set_entries_ellipsis(softproof_profile, PANGO_ELLIPSIZE_MIDDLE);
    dt_bauhaus_combobox_set_entries_ellipsis(histogram_profile, PANGO_ELLIPSIZE_MIDDLE);

    gtk_box_pack_start(GTK_BOX(vbox), display_profile, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), display_intent, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), display2_profile, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), display2_intent, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), softproof_profile, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), histogram_profile, TRUE, TRUE, 0);

    for(const GList *l = darktable.color_profiles->profiles; l; l = g_list_next(l))
    {
      dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)l->data;
      if(prof->display_pos > -1)
      {
        dt_bauhaus_combobox_add(display_profile, prof->name);
        if(prof->type == darktable.color_profiles->display_type
          && (prof->type != DT_COLORSPACE_FILE
              || !strcmp(prof->filename, darktable.color_profiles->display_filename)))
        {
          dt_bauhaus_combobox_set(display_profile, prof->display_pos);
        }
      }
      if(prof->display2_pos > -1)
      {
        dt_bauhaus_combobox_add(display2_profile, prof->name);
        if(prof->type == darktable.color_profiles->display2_type
           && (prof->type != DT_COLORSPACE_FILE
               || !strcmp(prof->filename, darktable.color_profiles->display2_filename)))
        {
          dt_bauhaus_combobox_set(display2_profile, prof->display2_pos);
        }
      }
      // the system display profile is only suitable for display purposes
      if(prof->out_pos > -1)
      {
        dt_bauhaus_combobox_add(softproof_profile, prof->name);
        if(prof->type == darktable.color_profiles->softproof_type
          && (prof->type != DT_COLORSPACE_FILE
              || !strcmp(prof->filename, darktable.color_profiles->softproof_filename)))
          dt_bauhaus_combobox_set(softproof_profile, prof->out_pos);
      }
      if(prof->category_pos > -1)
      {
        dt_bauhaus_combobox_add(histogram_profile, prof->name);
        if(prof->type == darktable.color_profiles->histogram_type
          && (prof->type != DT_COLORSPACE_FILE
              || !strcmp(prof->filename, darktable.color_profiles->histogram_filename)))
        {
          dt_bauhaus_combobox_set(histogram_profile, prof->category_pos);
        }
      }
    }

    char *system_profile_dir = g_build_filename(datadir, "color", "out", NULL);
    char *user_profile_dir = g_build_filename(confdir, "color", "out", NULL);
    char *tooltip = g_strdup_printf(_("display ICC profiles in %s or %s"), user_profile_dir, system_profile_dir);
    gtk_widget_set_tooltip_text(display_profile, tooltip);
    g_free(tooltip);
    tooltip = g_strdup_printf(_("preview display ICC profiles in %s or %s"), user_profile_dir, system_profile_dir);
    gtk_widget_set_tooltip_text(display2_profile, tooltip);
    g_free(tooltip);
    tooltip = g_strdup_printf(_("softproof ICC profiles in %s or %s"), user_profile_dir, system_profile_dir);
    gtk_widget_set_tooltip_text(softproof_profile, tooltip);
    g_free(tooltip);
    tooltip = g_strdup_printf(_("histogram and color picker ICC profiles in %s or %s"), user_profile_dir, system_profile_dir);
    gtk_widget_set_tooltip_text(histogram_profile, tooltip);
    g_free(tooltip);
    g_free(system_profile_dir);
    g_free(user_profile_dir);

    g_signal_connect(G_OBJECT(display_profile), "value-changed", G_CALLBACK(display_profile_callback), dev);
    g_signal_connect(G_OBJECT(display2_profile), "value-changed", G_CALLBACK(display2_profile_callback), dev);
    g_signal_connect(G_OBJECT(softproof_profile), "value-changed", G_CALLBACK(softproof_profile_callback), dev);
    g_signal_connect(G_OBJECT(histogram_profile), "value-changed", G_CALLBACK(histogram_profile_callback), dev);

    _update_softproof_gamut_checking(dev);

    DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_PREFERENCES_CHANGE,
                              G_CALLBACK(_preference_prev_downsample_change), &(dev->preview_downsampling));
    // update the gui when the preferences changed (i.e. show intent when using lcms2)
    DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_PREFERENCES_CHANGE,
                              G_CALLBACK(_preference_changed), (gpointer)display_intent);
    DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_PREFERENCES_CHANGE, G_CALLBACK(_preference_changed),
                              (gpointer)display2_intent);
    // and when profiles change
    DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                              G_CALLBACK(_display_profile_changed), (gpointer)display_profile);
    DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                              G_CALLBACK(_display2_profile_changed), (gpointer)display2_profile);

    gtk_widget_show_all(vbox);
  }

  /* create grid changer popup tool */
  {
    // the button
    darktable.view_manager->guides_toggle = dtgtk_togglebutton_new(dtgtk_cairo_paint_grid, 0, NULL);
    ac = dt_action_define(sa, N_("guide lines"), N_("toggle"), darktable.view_manager->guides_toggle, &dt_action_def_toggle);
    dt_shortcut_register(ac, 0, 0, GDK_KEY_g, 0);
    gtk_widget_set_tooltip_text(darktable.view_manager->guides_toggle,
                                _("toggle guide lines\nright click for guides options"));
    darktable.view_manager->guides_popover = dt_guides_popover(self, darktable.view_manager->guides_toggle);
    g_object_ref(darktable.view_manager->guides_popover);
    g_signal_connect(G_OBJECT(darktable.view_manager->guides_toggle), "clicked",
                     G_CALLBACK(_guides_quickbutton_clicked), dev);
    connect_button_press_release(darktable.view_manager->guides_toggle, darktable.view_manager->guides_popover);
    dt_view_manager_module_toolbox_add(darktable.view_manager, darktable.view_manager->guides_toggle,
                                       DT_VIEW_DARKROOM | DT_VIEW_TETHERING);
    // we want to update button state each time the view change
    DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED,
                                    G_CALLBACK(_guides_view_changed), dev);
  }

  darktable.view_manager->proxy.darkroom.get_layout = _lib_darkroom_get_layout;
  dev->border_size = DT_PIXEL_APPLY_DPI(dt_conf_get_int("plugins/darkroom/ui/border_size"));

  // Fullscreen preview key
  ac = dt_action_define(sa, NULL, N_("full preview"), NULL, &dt_action_def_preview);
  dt_shortcut_register(ac, 0, DT_ACTION_EFFECT_HOLD, GDK_KEY_w, 0);

  // add an option to allow skip mouse events while other overlays are consuming mouse actions
  ac = dt_action_define(sa, NULL, N_("force pan & zoom with mouse"), NULL, &dt_action_def_skip_mouse);
  dt_shortcut_register(ac, 0, DT_ACTION_EFFECT_HOLD, GDK_KEY_a, 0);

  // move left/right/up/down
  ac = dt_action_define(sa, N_("move"), N_("horizontal"), GINT_TO_POINTER(1), &_action_def_move);
  dt_shortcut_register(ac, 0, DT_ACTION_EFFECT_DOWN, GDK_KEY_Left , 0);
  dt_shortcut_register(ac, 0, DT_ACTION_EFFECT_UP  , GDK_KEY_Right, 0);
  ac = dt_action_define(sa, N_("move"), N_("vertical"), GINT_TO_POINTER(0), &_action_def_move);
  dt_shortcut_register(ac, 0, DT_ACTION_EFFECT_DOWN, GDK_KEY_Down , 0);
  dt_shortcut_register(ac, 0, DT_ACTION_EFFECT_UP  , GDK_KEY_Up   , 0);

  // Zoom shortcuts
  dt_action_register(DT_ACTION(self), N_("zoom close-up"), zoom_key_accel, GDK_KEY_1, GDK_MOD1_MASK);
  dt_action_register(DT_ACTION(self), N_("zoom fill"), zoom_key_accel, GDK_KEY_2, GDK_MOD1_MASK);
  dt_action_register(DT_ACTION(self), N_("zoom fit"), zoom_key_accel, GDK_KEY_3, GDK_MOD1_MASK);

  // zoom in/out
  dt_action_register(DT_ACTION(self), N_("zoom in"), zoom_in_callback, GDK_KEY_plus, GDK_CONTROL_MASK);
  dt_action_register(DT_ACTION(self), N_("zoom out"), zoom_out_callback, GDK_KEY_minus, GDK_CONTROL_MASK);

  // Shortcut to skip images
  dt_action_register(DT_ACTION(self), N_("image forward"), skip_f_key_accel_callback, GDK_KEY_space, 0);
  dt_action_register(DT_ACTION(self), N_("image back"), skip_b_key_accel_callback, GDK_KEY_BackSpace, 0);

  // cycle overlay colors
  dt_action_register(DT_ACTION(self), N_("cycle overlay colors"), _overlay_cycle_callback, GDK_KEY_o, GDK_CONTROL_MASK);

  // toggle visibility of drawn masks for current gui module
  dt_action_register(DT_ACTION(self), N_("show drawn masks"), _toggle_mask_visibility_callback, 0, 0);

  // brush size +/-
  dt_action_register(DT_ACTION(self), N_("increase brush size"), _brush_size_up_callback, 0, 0);
  dt_action_register(DT_ACTION(self), N_("decrease brush size"), _brush_size_down_callback, 0, 0);

  // brush hardness +/-
  dt_action_register(DT_ACTION(self), N_("increase brush hardness"), _brush_hardness_up_callback, GDK_KEY_braceright, 0);
  dt_action_register(DT_ACTION(self), N_("decrease brush hardness"), _brush_hardness_down_callback, GDK_KEY_braceleft, 0);

  // brush opacity +/-
  dt_action_register(DT_ACTION(self), N_("increase brush opacity"), _brush_opacity_up_callback, GDK_KEY_greater, 0);
  dt_action_register(DT_ACTION(self), N_("decrease brush opacity"), _brush_opacity_down_callback, GDK_KEY_less, 0);

  // undo/redo
  dt_action_register(DT_ACTION(self), N_("undo"), _darkroom_undo_callback, GDK_KEY_z, GDK_CONTROL_MASK);
  dt_action_register(DT_ACTION(self), N_("redo"), _darkroom_redo_callback, GDK_KEY_y, GDK_CONTROL_MASK);

  // change the precision for adjusting sliders with keyboard shortcuts
  dt_action_register(DT_ACTION(self), N_("change keyboard shortcut slider precision"), change_slider_accel_precision, 0, 0);
}

enum
{
  DND_TARGET_IOP,
};

/** drag and drop module list */
static const GtkTargetEntry _iop_target_list_internal[] = { { "iop", GTK_TARGET_SAME_WIDGET, DND_TARGET_IOP } };
static const guint _iop_n_targets_internal = G_N_ELEMENTS(_iop_target_list_internal);

static dt_iop_module_t *_get_dnd_dest_module(GtkBox *container, gint x, gint y, dt_iop_module_t *module_src)
{
  dt_iop_module_t *module_dest = NULL;

  GtkAllocation allocation_w = {0};
  gtk_widget_get_allocation(module_src->header, &allocation_w);
  const int y_slop = allocation_w.height / 2;
  // after source in pixelpipe, which is before it in the widgets list
  gboolean after_src = TRUE;

  GtkWidget *widget_dest = NULL;
  GList *children = gtk_container_get_children(GTK_CONTAINER(container));
  for(GList *l = children; l != NULL; l = g_list_next(l))
  {
    GtkWidget *w = GTK_WIDGET(l->data);
    if(w)
    {
      if(w == module_src->expander) after_src = FALSE;
      if(gtk_widget_is_visible(w))
      {
        gtk_widget_get_allocation(w, &allocation_w);
        // If dragging to later in the pixelpipe, we will insert after
        // the destination module. If dragging to earlier in the
        // pixelpipe, will insert before the destination module. This
        // results in two code paths here and in our caller, but can
        // handle all cases from inserting at the very start to the
        // very end.
        if((after_src && y <= allocation_w.y + y_slop) ||
           (!after_src && y <= allocation_w.y + allocation_w.height + y_slop))
        {
          widget_dest = w;
          break;
        }
      }
    }
  }
  g_list_free(children);

  if(widget_dest)
  {
    for(const GList *modules = darktable.develop->iop; modules; modules = g_list_next(modules))
    {
      dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
      if(mod->expander == widget_dest)
      {
        module_dest = mod;
        break;
      }
    }
  }

  return module_dest;
}

static dt_iop_module_t *_get_dnd_source_module(GtkBox *container)
{
  dt_iop_module_t *module_source = NULL;
  gpointer *source_data = g_object_get_data(G_OBJECT(container), "source_data");
  if(source_data) module_source = (dt_iop_module_t *)source_data;

  return module_source;
}

// this will be used for a custom highlight, if ever implemented
static void _on_drag_end(GtkWidget *widget, GdkDragContext *context, gpointer user_data)
{
}

// FIXME: default highlight for the dnd is barely visible
// it should be possible to configure it
static void _on_drag_begin(GtkWidget *widget, GdkDragContext *context, gpointer user_data)
{
  GtkBox *container = dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER);
  dt_iop_module_t *module_src = _get_dnd_source_module(container);
  if(module_src && module_src->expander)
  {
    GdkWindow *window = gtk_widget_get_parent_window(module_src->header);
    if(window)
    {
      GtkAllocation allocation_w = {0};
      gtk_widget_get_allocation(module_src->header, &allocation_w);
      // method from https://blog.gtk.org/2017/04/23/drag-and-drop-in-lists/
      cairo_surface_t *surface = dt_cairo_image_surface_create(CAIRO_FORMAT_RGB24, allocation_w.width, allocation_w.height);
      cairo_t *cr = cairo_create(surface);

      // hack to render not transparent
      dt_gui_add_class(module_src->header, "iop_drag_icon");
      gtk_widget_draw(module_src->header, cr);
      dt_gui_remove_class(module_src->header, "iop_drag_icon");

      // FIXME: this centers the icon on the mouse -- instead translate such that the label doesn't jump when mouse down?
      cairo_surface_set_device_offset(surface, -allocation_w.width * darktable.gui->ppd / 2, -allocation_w.height * darktable.gui->ppd / 2);
      gtk_drag_set_icon_surface(context, surface);

      cairo_destroy(cr);
      cairo_surface_destroy(surface);
    }
  }
}

static void _on_drag_data_get(GtkWidget *widget, GdkDragContext *context,
                              GtkSelectionData *selection_data, guint info, guint time,
                              gpointer user_data)
{
  gpointer *target_data = g_object_get_data(G_OBJECT(widget), "target_data");
  guint number_data = 0;
  if(target_data) number_data = GPOINTER_TO_UINT(target_data[DND_TARGET_IOP]);
  gtk_selection_data_set(selection_data, gdk_atom_intern("iop", TRUE), // type
                                        32,                            // format
                                        (guchar*)&number_data,         // data
                                        1);                            // length
}

static gboolean _on_drag_drop(GtkWidget *widget, GdkDragContext *dc, gint x, gint y, guint time, gpointer user_data)
{
  GdkAtom target_atom = GDK_NONE;

  target_atom = gdk_atom_intern("iop", TRUE);

  gtk_drag_get_data(widget, dc, target_atom, time);

  return TRUE;
}

static gboolean _on_drag_motion(GtkWidget *widget, GdkDragContext *dc, gint x, gint y, guint time, gpointer user_data)
{
  gboolean can_moved = FALSE;
  GtkBox *container = dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER);
  dt_iop_module_t *module_src = _get_dnd_source_module(container);
  if(!module_src) return FALSE;

  dt_iop_module_t *module_dest = _get_dnd_dest_module(container, x, y, module_src);

  if(module_src && module_dest && module_src != module_dest)
  {
    if(module_src->iop_order < module_dest->iop_order)
      can_moved = dt_ioppr_check_can_move_after_iop(darktable.develop->iop, module_src, module_dest);
    else
      can_moved = dt_ioppr_check_can_move_before_iop(darktable.develop->iop, module_src, module_dest);
  }

  for(const GList *modules = g_list_last(darktable.develop->iop); modules; modules = g_list_previous(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);

    if(module->expander)
    {
      dt_gui_remove_class(module->expander, "iop_drop_after");
      dt_gui_remove_class(module->expander, "iop_drop_before");
    }
  }

  if(can_moved)
  {
    if(module_src->iop_order < module_dest->iop_order)
      dt_gui_add_class(module_dest->expander, "iop_drop_after");
    else
      dt_gui_add_class(module_dest->expander, "iop_drop_before");

    gdk_drag_status(dc, GDK_ACTION_COPY, time);
    GtkWidget *w = g_object_get_data(G_OBJECT(widget), "highlighted");
    if(w) gtk_drag_unhighlight(w);
    g_object_set_data(G_OBJECT(widget), "highlighted", (gpointer)module_dest->expander);
    gtk_drag_highlight(module_dest->expander);
  }
  else
  {
    gdk_drag_status(dc, 0, time);
    GtkWidget *w = g_object_get_data(G_OBJECT(widget), "highlighted");
    if(w)
    {
      gtk_drag_unhighlight(w);
      g_object_set_data(G_OBJECT(widget), "highlighted", (gpointer)FALSE);
    }
  }

  return can_moved;
}

static void _on_drag_data_received(GtkWidget *widget, GdkDragContext *dc, gint x, gint y,
                                   GtkSelectionData *selection_data,
                                   guint info, guint time, gpointer user_data)
{
  int moved = 0;
  GtkBox *container = dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER);
  dt_iop_module_t *module_src = _get_dnd_source_module(container);
  dt_iop_module_t *module_dest = _get_dnd_dest_module(container, x, y, module_src);

  if(module_src && module_dest && module_src != module_dest)
  {
    if(module_src->iop_order < module_dest->iop_order)
    {
      /* printf("[_on_drag_data_received] moving %s %s(%f) after %s %s(%f)\n",
          module_src->op, module_src->multi_name, module_src->iop_order,
          module_dest->op, module_dest->multi_name, module_dest->iop_order); */
      moved = dt_ioppr_move_iop_after(darktable.develop, module_src, module_dest);
    }
    else
    {
      /* printf("[_on_drag_data_received] moving %s %s(%f) before %s %s(%f)\n",
          module_src->op, module_src->multi_name, module_src->iop_order,
          module_dest->op, module_dest->multi_name, module_dest->iop_order); */
      moved = dt_ioppr_move_iop_before(darktable.develop, module_src, module_dest);
    }
  }
  else
  {
    if(module_src == NULL)
      fprintf(stderr, "[_on_drag_data_received] can't find source module\n");
    if(module_dest == NULL)
      fprintf(stderr, "[_on_drag_data_received] can't find destination module\n");
  }

  for(const GList *modules = g_list_last(darktable.develop->iop); modules; modules = g_list_previous(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);

    if(module->expander)
    {
      dt_gui_remove_class(module->expander, "iop_drop_after");
      dt_gui_remove_class(module->expander, "iop_drop_before");
    }
  }

  gtk_drag_finish(dc, TRUE, FALSE, time);

  if(moved)
  {
    // we move the headers
    GValue gv = { 0, { { 0 } } };
    g_value_init(&gv, G_TYPE_INT);
    gtk_container_child_get_property(
        GTK_CONTAINER(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER)), module_dest->expander,
        "position", &gv);
    gtk_box_reorder_child(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER),
        module_src->expander, g_value_get_int(&gv));

    // we update the headers
    dt_dev_modules_update_multishow(module_src->dev);

    dt_dev_add_history_item(module_src->dev, module_src, TRUE);

    dt_ioppr_check_iop_order(module_src->dev, 0, "_on_drag_data_received end");

    // rebuild the accelerators
    dt_iop_connect_accels_multi(module_src->so);

    dt_dev_pixelpipe_rebuild(module_src->dev);

    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_MODULE_MOVED);
  }
}

static void _on_drag_leave(GtkWidget *widget, GdkDragContext *dc, guint time, gpointer user_data)
{
  for(const GList *modules = g_list_last(darktable.develop->iop); modules; modules = g_list_previous(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);

    if(module->expander)
    {
      dt_gui_remove_class(module->expander, "iop_drop_after");
      dt_gui_remove_class(module->expander, "iop_drop_before");
    }
  }

  GtkWidget *w = g_object_get_data(G_OBJECT(widget), "highlighted");
  if(w)
  {
    gtk_drag_unhighlight(w);
    g_object_set_data(G_OBJECT(widget), "highlighted", (gpointer)FALSE);
  }
}

static void _register_modules_drag_n_drop(dt_view_t *self)
{
  if(darktable.gui)
  {
    GtkWidget *container = GTK_WIDGET(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER));

    gtk_drag_source_set(container, GDK_BUTTON1_MASK | GDK_SHIFT_MASK, _iop_target_list_internal, _iop_n_targets_internal, GDK_ACTION_COPY);

    g_object_set_data(G_OBJECT(container), "targetlist", (gpointer)_iop_target_list_internal);
    g_object_set_data(G_OBJECT(container), "ntarget", GUINT_TO_POINTER(_iop_n_targets_internal));

    g_signal_connect(container, "drag-begin", G_CALLBACK(_on_drag_begin), NULL);
    g_signal_connect(container, "drag-data-get", G_CALLBACK(_on_drag_data_get), NULL);
    g_signal_connect(container, "drag-end", G_CALLBACK(_on_drag_end), NULL);

    gtk_drag_dest_set(container, 0, _iop_target_list_internal, _iop_n_targets_internal, GDK_ACTION_COPY);

    g_signal_connect(container, "drag-data-received", G_CALLBACK(_on_drag_data_received), NULL);
    g_signal_connect(container, "drag-drop", G_CALLBACK(_on_drag_drop), NULL);
    g_signal_connect(container, "drag-motion", G_CALLBACK(_on_drag_motion), NULL);
    g_signal_connect(container, "drag-leave", G_CALLBACK(_on_drag_leave), NULL);
  }
}

static void _unregister_modules_drag_n_drop(dt_view_t *self)
{
  if(darktable.gui)
  {
    gtk_drag_source_unset(dt_ui_center(darktable.gui->ui));

    GtkBox *container = dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER);

    g_signal_handlers_disconnect_matched(container, G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA, 0, 0, NULL, G_CALLBACK(_on_drag_begin), NULL);
    g_signal_handlers_disconnect_matched(container, G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA, 0, 0, NULL, G_CALLBACK(_on_drag_data_get), NULL);
    g_signal_handlers_disconnect_matched(container, G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA, 0, 0, NULL, G_CALLBACK(_on_drag_end), NULL);
    g_signal_handlers_disconnect_matched(container, G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA, 0, 0, NULL, G_CALLBACK(_on_drag_data_received), NULL);
    g_signal_handlers_disconnect_matched(container, G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA, 0, 0, NULL, G_CALLBACK(_on_drag_drop), NULL);
    g_signal_handlers_disconnect_matched(container, G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA, 0, 0, NULL, G_CALLBACK(_on_drag_motion), NULL);
    g_signal_handlers_disconnect_matched(container, G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA, 0, 0, NULL, G_CALLBACK(_on_drag_leave), NULL);
  }
}

void enter(dt_view_t *self)
{
  // prevent accels_window to refresh
  darktable.view_manager->accels_window.prevent_refresh = TRUE;

  // clean the undo list
  dt_undo_clear(darktable.undo, DT_UNDO_DEVELOP);

  /* connect to ui pipe finished signal for redraw */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED,
                            G_CALLBACK(_darkroom_ui_pipe_finish_signal_callback), (gpointer)self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW2_PIPE_FINISHED,
                            G_CALLBACK(_darkroom_ui_preview2_pipe_finish_signal_callback), (gpointer)self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_TROUBLE_MESSAGE,
                                  G_CALLBACK(_display_module_trouble_message_callback),
                                  (gpointer)self);

  dt_print(DT_DEBUG_CONTROL, "[run_job+] 11 %f in darkroom mode\n", dt_get_wtime());
  dt_develop_t *dev = (dt_develop_t *)self->data;
  if(!dev->form_gui)
  {
    dev->form_gui = (dt_masks_form_gui_t *)calloc(1, sizeof(dt_masks_form_gui_t));
    dt_masks_init_form_gui(dev->form_gui);
  }
  dt_masks_change_form_gui(NULL);
  dev->form_gui->pipe_hash = 0;
  dev->form_gui->formid = 0;
  dev->gui_leaving = 0;
  dev->gui_module = NULL;

  // change active image
  dt_view_active_images_reset(FALSE);
  dt_view_active_images_add(dev->image_storage.id, TRUE);
  dt_ui_thumbtable(darktable.gui->ui)->mouse_inside = FALSE; // consider mouse outside filmstrip by default

  dt_control_set_dev_zoom(DT_ZOOM_FIT);
  dt_control_set_dev_zoom_x(0);
  dt_control_set_dev_zoom_y(0);
  dt_control_set_dev_closeup(0);

  // take a copy of the image struct for convenience.

  dt_dev_load_image(darktable.develop, dev->image_storage.id);


  /*
   * add IOP modules to plugin list
   */
  GtkWidget *box = GTK_WIDGET(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER));
  GtkScrolledWindow *sw = GTK_SCROLLED_WINDOW(gtk_widget_get_ancestor(box, GTK_TYPE_SCROLLED_WINDOW));
  if(sw) gtk_scrolled_window_set_propagate_natural_width(sw, FALSE);

  char option[1024];
  for(const GList *modules = g_list_last(dev->iop); modules; modules = g_list_previous(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);

    /* initialize gui if iop have one defined */
    if(!dt_iop_is_hidden(module))
    {
      dt_iop_gui_init(module);

      /* add module to right panel */
      dt_iop_gui_set_expander(module);

      if(module->multi_priority == 0)
      {
        snprintf(option, sizeof(option), "plugins/darkroom/%s/expanded", module->op);
        module->expanded = dt_conf_get_bool(option);
        dt_iop_gui_update_expanded(module);
      }

      dt_iop_reload_defaults(module);
    }
  }

  /* signal that darktable.develop is initialized and ready to be used */
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_INITIALIZE);

  // synch gui and flag pipe as dirty
  // this is done here and not in dt_read_history, as it would else be triggered before module->gui_init.
  dt_dev_pop_history_items(dev, dev->history_end);

  /* ensure that filmstrip shows current image */
  dt_thumbtable_set_offset_image(dt_ui_thumbtable(darktable.gui->ui), dev->image_storage.id, TRUE);

  // get last active plugin:
  const char *active_plugin = dt_conf_get_string_const("plugins/darkroom/active");
  if(active_plugin)
  {
    for(const GList *modules = dev->iop; modules; modules = g_list_next(modules))
    {
      dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
      if(!strcmp(module->op, active_plugin)) dt_iop_request_focus(module);
    }
  }

  // update module multishow state now modules are loaded
  dt_dev_modules_update_multishow(dev);

  // image should be there now.
  float zoom_x, zoom_y;
  dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, DT_ZOOM_FIT, 0, NULL, NULL);
  dt_control_set_dev_zoom_x(zoom_x);
  dt_control_set_dev_zoom_y(zoom_y);

  /* connect signal for filmstrip image activate */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE,
                            G_CALLBACK(_view_darkroom_filmstrip_activate_callback), self);

  dt_collection_hint_message(darktable.collection);

  dt_ui_scrollbars_show(darktable.gui->ui, dt_conf_get_bool("darkroom/ui/scrollbars"));

  _register_modules_drag_n_drop(self);

  if(dt_conf_get_bool("second_window/last_visible"))
  {
    _darkroom_display_second_window(dev);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dev->second_window.button), TRUE);
  }

  // just make sure at this stage we have only history info into the undo, all automatic
  // tagging should be ignored.
  dt_undo_clear(darktable.undo, DT_UNDO_TAGS);

  // update accels_window
  darktable.view_manager->accels_window.prevent_refresh = FALSE;

  //connect iop accelerators
  dt_iop_connect_accels_all();

  // switch on groups as they were last time:
  dt_dev_modulegroups_set(dev, dt_conf_get_int("plugins/darkroom/groups"));

  // connect to preference change for module header button hiding
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_PREFERENCES_CHANGE,
                                  G_CALLBACK(_preference_changed_button_hide), dev);

  dt_iop_color_picker_init();

  dt_image_check_camera_missing_sample(&dev->image_storage);

#ifdef USE_LUA

  _fire_darkroom_image_loaded_event(TRUE, dev->image_storage.id);

#endif

}

void leave(dt_view_t *self)
{
  dt_iop_color_picker_cleanup();
  if(darktable.lib->proxy.colorpicker.picker_proxy)
    dt_iop_color_picker_reset(darktable.lib->proxy.colorpicker.picker_proxy->module, FALSE);

  _unregister_modules_drag_n_drop(self);

  /* disconnect from filmstrip image activate */
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_view_darkroom_filmstrip_activate_callback),
                               (gpointer)self);

  /* disconnect from pipe finish signal */
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_darkroom_ui_pipe_finish_signal_callback),
                               (gpointer)self);

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_darkroom_ui_preview2_pipe_finish_signal_callback),
                               (gpointer)self);

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals,
                                     G_CALLBACK(_display_module_trouble_message_callback),
                                     (gpointer)self);

  // store groups for next time:
  dt_conf_set_int("plugins/darkroom/groups", dt_dev_modulegroups_get(darktable.develop));

  // store last active plugin:
  if(darktable.develop->gui_module)
    dt_conf_set_string("plugins/darkroom/active", darktable.develop->gui_module->op);
  else
    dt_conf_set_string("plugins/darkroom/active", "");

  dt_develop_t *dev = (dt_develop_t *)self->data;

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals,
                                     G_CALLBACK(_preference_changed_button_hide), dev);

  // reset color assessment mode
  if(dev->iso_12646.enabled)
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dev->iso_12646.button), FALSE);
    dev->iso_12646.enabled = FALSE;
    dev->width = dev->orig_width;
    dev->height = dev->orig_height;
    dev->second_window.width = dev->second_window.orig_width;
    dev->second_window.height = dev->second_window.orig_height;
    dev->border_size = DT_PIXEL_APPLY_DPI(dt_conf_get_int("plugins/darkroom/ui/border_size"));
  }

  // commit image ops to db
  dt_dev_write_history(dev);

  // update aspect ratio
  if(dev->preview_pipe->backbuf && dev->preview_status == DT_DEV_PIXELPIPE_VALID)
  {
    double aspect_ratio = (double)dev->preview_pipe->backbuf_width / (double)dev->preview_pipe->backbuf_height;
    dt_image_set_aspect_ratio_to(dev->preview_pipe->image.id, aspect_ratio, FALSE);
  }
  else
  {
    dt_image_set_aspect_ratio(dev->image_storage.id, FALSE);
  }

  // be sure light table will regenerate the thumbnail:
  if(!dt_history_hash_is_mipmap_synced(dev->image_storage.id))
  {
    dt_mipmap_cache_remove(darktable.mipmap_cache, dev->image_storage.id);
    dt_image_update_final_size(dev->image_storage.id);
    // possibly dump new xmp data
    const dt_history_hash_t hash_status = dt_history_hash_get_status(dev->image_storage.id);

    const gboolean fresh = (hash_status == DT_HISTORY_HASH_BASIC) || (hash_status == DT_HISTORY_HASH_AUTO);
    const dt_imageio_write_xmp_t xmp_mode = dt_image_get_xmp_mode();
    if((xmp_mode == DT_WRITE_XMP_ALWAYS) || ((xmp_mode == DT_WRITE_XMP_LAZY) && !fresh))
      dt_image_synch_xmp(dev->image_storage.id);
    dt_history_hash_set_mipmap(dev->image_storage.id);
#ifdef USE_LUA
    dt_lua_async_call_alien(dt_lua_event_trigger_wrapper,
        0, NULL, NULL,
        LUA_ASYNC_TYPENAME, "const char*", "darkroom-image-history-changed",
        LUA_ASYNC_TYPENAME, "dt_lua_image_t", GINT_TO_POINTER(dev->image_storage.id),
        LUA_ASYNC_DONE);
#endif
  }

  // clear gui.

  dt_pthread_mutex_lock(&dev->preview_pipe_mutex);
  dt_pthread_mutex_lock(&dev->preview2_pipe_mutex);
  dt_pthread_mutex_lock(&dev->pipe_mutex);

  dev->gui_leaving = 1;

  dt_dev_pixelpipe_cleanup_nodes(dev->pipe);
  dt_dev_pixelpipe_cleanup_nodes(dev->preview2_pipe);
  dt_dev_pixelpipe_cleanup_nodes(dev->preview_pipe);

  dt_pthread_mutex_lock(&dev->history_mutex);
  while(dev->history)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(dev->history->data);
    // printf("removing history item %d - %s, data %f %f\n", hist->module->instance, hist->module->op, *(float
    // *)hist->params, *((float *)hist->params+1));
    dt_dev_free_history_item(hist);
    dev->history = g_list_delete_link(dev->history, dev->history);
  }

  while(dev->iop)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(dev->iop->data);
    if(!dt_iop_is_hidden(module)) dt_iop_gui_cleanup_module(module);

    // force refresh if module has mask visualized
    if(module->request_mask_display || module->suppress_mask) dt_iop_refresh_center(module);

    dt_action_cleanup_instance_iop(module);
    dt_iop_cleanup_module(module);
    free(module);
    dev->iop = g_list_delete_link(dev->iop, dev->iop);
  }
  while(dev->alliop)
  {
    dt_iop_cleanup_module((dt_iop_module_t *)dev->alliop->data);
    free(dev->alliop->data);
    dev->alliop = g_list_delete_link(dev->alliop, dev->alliop);
  }

  GtkWidget *box = GTK_WIDGET(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER));
  GtkScrolledWindow *sw = GTK_SCROLLED_WINDOW(gtk_widget_get_ancestor(box, GTK_TYPE_SCROLLED_WINDOW));
  if(sw) gtk_scrolled_window_set_propagate_natural_width(sw, TRUE);

  dt_pthread_mutex_unlock(&dev->history_mutex);

  dt_pthread_mutex_unlock(&dev->pipe_mutex);
  dt_pthread_mutex_unlock(&dev->preview2_pipe_mutex);
  dt_pthread_mutex_unlock(&dev->preview_pipe_mutex);

  // cleanup visible masks
  if(dev->form_gui)
  {
    dev->gui_module = NULL; // modules have already been free()
    dt_masks_clear_form_gui(dev);
    free(dev->form_gui);
    dev->form_gui = NULL;
    dt_masks_change_form_gui(NULL);
  }
  // clear masks
  g_list_free_full(dev->forms, (void (*)(void *))dt_masks_free_form);
  dev->forms = NULL;
  g_list_free_full(dev->allforms, (void (*)(void *))dt_masks_free_form);
  dev->allforms = NULL;

  gtk_widget_hide(dev->overexposed.floating_window);
  gtk_widget_hide(dev->rawoverexposed.floating_window);
  gtk_widget_hide(dev->profile.floating_window);

  dt_ui_scrollbars_show(darktable.gui->ui, FALSE);

  // darkroom development could have changed a collection, so update that before being back in lighttable
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF,
                             g_list_prepend(NULL, GINT_TO_POINTER(darktable.develop->image_storage.id)));

  darktable.develop->image_storage.id = -1;

  dt_print(DT_DEBUG_CONTROL, "[run_job-] 11 %f in darkroom mode\n", dt_get_wtime());
}

void mouse_leave(dt_view_t *self)
{
  // if we are not hovering over a thumbnail in the filmstrip -> show metadata of opened image.
  dt_develop_t *dev = (dt_develop_t *)self->data;
  dt_control_set_mouse_over_id(dev->image_storage.id);

  dev->darkroom_mouse_in_center_area = FALSE;
  // masks
  int handled = dt_masks_events_mouse_leave(dev->gui_module);
  if(handled) return;
  // module
  if(dev->gui_module && dev->gui_module->mouse_leave)
    handled = dev->gui_module->mouse_leave(dev->gui_module);

  // reset any changes the selected plugin might have made.
  dt_control_change_cursor(GDK_LEFT_PTR);
}

/* This helper function tests for a position to be within the displayed area
   of an image. To avoid "border cases" we accept values to be slightly out of area too.
*/
static int mouse_in_imagearea(dt_view_t *self, double *x, double *y)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;

  const int closeup = dt_control_get_dev_closeup();
  const int pwidth = (dev->pipe->output_backbuf_width<<closeup) / darktable.gui->ppd;
  const int pheight = (dev->pipe->output_backbuf_height<<closeup) / darktable.gui->ppd;

  *x = CLAMP(*x, (self->width - pwidth) / 2, (self->width + pwidth) / 2);
  *y = CLAMP(*y, (self->height - pheight) / 2, (self->height + pheight) / 2);
  return TRUE;
}

void mouse_enter(dt_view_t *self)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  // masks
  dev->darkroom_mouse_in_center_area = TRUE;
  dt_masks_events_mouse_enter(dev->gui_module);
}

void mouse_moved(dt_view_t *self, double x, double y, double pressure, int which)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  const int32_t tb = dev->border_size;
  const int32_t capwd = self->width  - 2*tb;
  const int32_t capht = self->height - 2*tb;

  // if we are not hovering over a thumbnail in the filmstrip -> show metadata of opened image.
  int32_t mouse_over_id = dt_control_get_mouse_over_id();
  if(mouse_over_id == -1)
  {
    mouse_over_id = dev->image_storage.id;
    dt_control_set_mouse_over_id(mouse_over_id);
  }

  dt_control_t *ctl = darktable.control;
  const int32_t width_i = self->width;
  const int32_t height_i = self->height;
  float offx = 0.0f, offy = 0.0f;
  if(width_i > capwd) offx = (capwd - width_i) * .5f;
  if(height_i > capht) offy = (capht - height_i) * .5f;
  int handled = 0;

  if(dt_iop_color_picker_is_visible(dev) && ctl->button_down && ctl->button_down_which == 1)
  {
    // module requested a color box
    if(mouse_in_imagearea(self, &x, &y))
    {
      dt_colorpicker_sample_t *const sample = darktable.lib->proxy.colorpicker.primary_sample;
      // Make sure a minimal width/height
      float delta_x = 1 / (float) dev->pipe->processed_width;
      float delta_y = 1 / (float) dev->pipe->processed_height;

      float zoom_x, zoom_y;
      dt_dev_get_pointer_zoom_pos(dev, x + offx, y + offy, &zoom_x, &zoom_y);

      if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
      {
        sample->box[0] = fmaxf(0.0, MIN(sample->point[0], .5f + zoom_x) - delta_x);
        sample->box[1] = fmaxf(0.0, MIN(sample->point[1], .5f + zoom_y) - delta_y);
        sample->box[2] = fminf(1.0, MAX(sample->point[0], .5f + zoom_x) + delta_x);
        sample->box[3] = fminf(1.0, MAX(sample->point[1], .5f + zoom_y) + delta_y);
      }
      else if(sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
      {
        sample->point[0] = .5f + zoom_x;
        sample->point[1] = .5f + zoom_y;
        dev->preview_status = DT_DEV_PIXELPIPE_DIRTY;
      }
    }
    dt_control_queue_redraw_center();
    return;
  }
  x += offx;
  y += offy;
  // masks
  handled = dt_masks_events_mouse_moved(dev->gui_module, x, y, pressure, which);
  if(handled) return;
  // module
  if(dev->gui_module && dev->gui_module->mouse_moved
     && dt_dev_modulegroups_get_activated(darktable.develop) != DT_MODULEGROUP_BASICS)
    handled = dev->gui_module->mouse_moved(dev->gui_module, x, y, pressure, which);
  if(handled) return;

  if(darktable.control->button_down && darktable.control->button_down_which == 1)
  {
    // depending on dev_zoom, adjust dev_zoom_x/y.
    const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
    const int closeup = dt_control_get_dev_closeup();
    int procw, proch;
    dt_dev_get_processed_size(dev, &procw, &proch);
    const float scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 0);
    float old_zoom_x, old_zoom_y;
    old_zoom_x = dt_control_get_dev_zoom_x();
    old_zoom_y = dt_control_get_dev_zoom_y();
    float zx = old_zoom_x - (1.0 / scale) * (x - ctl->button_x - offx) / procw;
    float zy = old_zoom_y - (1.0 / scale) * (y - ctl->button_y - offy) / proch;
    dt_dev_check_zoom_bounds(dev, &zx, &zy, zoom, closeup, NULL, NULL);
    dt_control_set_dev_zoom_x(zx);
    dt_control_set_dev_zoom_y(zy);
    ctl->button_x = x - offx;
    ctl->button_y = y - offy;
    dt_dev_invalidate(dev);
    dt_control_queue_redraw_center();
    dt_control_navigation_redraw();
  }
}


int button_released(dt_view_t *self, double x, double y, int which, uint32_t state)
{
  dt_develop_t *dev = darktable.develop;
  const int32_t tb = dev->border_size;
  const int32_t capwd = self->width  - 2*tb;
  const int32_t capht = self->height - 2*tb;
  const int32_t width_i = self->width;
  const int32_t height_i = self->height;
  if(width_i > capwd) x += (capwd - width_i) * .5f;
  if(height_i > capht) y += (capht - height_i) * .5f;

  int handled = 0;
  if(dt_iop_color_picker_is_visible(dev) && which == 1)
  {
    // only sample box picker at end, for speed
    if(darktable.lib->proxy.colorpicker.primary_sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
    {
      dev->preview_status = DT_DEV_PIXELPIPE_DIRTY;
      dt_control_queue_redraw_center();
      dt_control_change_cursor(GDK_LEFT_PTR);
    }
    return 1;
  }
  // masks
  if(dev->form_visible) handled = dt_masks_events_button_released(dev->gui_module, x, y, which, state);
  if(handled) return handled;
  // module
  if(dev->gui_module && dev->gui_module->button_released
     && dt_dev_modulegroups_get_activated(darktable.develop) != DT_MODULEGROUP_BASICS)
    handled = dev->gui_module->button_released(dev->gui_module, x, y, which, state);
  if(handled) return handled;
  if(which == 1) dt_control_change_cursor(GDK_LEFT_PTR);
  return 1;
}


int button_pressed(dt_view_t *self, double x, double y, double pressure, int which, int type, uint32_t state)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  dt_colorpicker_sample_t *const sample = darktable.lib->proxy.colorpicker.primary_sample;
  const int32_t tb = dev->border_size;
  const int32_t capwd = self->width  - 2*tb;
  const int32_t capht = self->height - 2*tb;
  const int32_t width_i = self->width;
  const int32_t height_i = self->height;
  float offx = 0.0f, offy = 0.0f;
  if(width_i > capwd) offx = (capwd - width_i) * .5f;
  if(height_i > capht) offy = (capht - height_i) * .5f;

  int handled = 0;
  if(dt_iop_color_picker_is_visible(dev))
  {
    float zoom_x, zoom_y;
    dt_dev_get_pointer_zoom_pos(dev, x + offx, y + offy, &zoom_x, &zoom_y);
    zoom_x += 0.5f;
    zoom_y += 0.5f;

    // FIXME: this overlaps with work in dt_dev_get_pointer_zoom_pos() above
    // FIXME: this work is only necessary for left-click in box mode or right-click of point live sample
    const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
    const int closeup = dt_control_get_dev_closeup();
    const float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 1);
    const int procw = dev->preview_pipe->backbuf_width;
    const int proch = dev->preview_pipe->backbuf_height;

    if(which == 1)
    {
      if(mouse_in_imagearea(self, &x, &y))
      {
        // The default box will be a square with 1% of the image width
        const float delta_x = 0.01f;
        const float delta_y = delta_x * (float)dev->pipe->processed_width / (float)dev->pipe->processed_height;

        // FIXME: here and in mouse move use to dt_lib_colorpicker_set_{box_area,point} interface? -- would require a different hack for figuring out base of the drag
        // hack: for box pickers, these represent the "base" point being dragged
        sample->point[0] = zoom_x;
        sample->point[1] = zoom_y;

        if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
        {
          // this is slightly more than as drawn, to give room for slop
          const float handle_px = 6.0f;
          float hx = handle_px / (procw * zoom_scale);
          float hy = handle_px / (proch * zoom_scale);
          gboolean on_corner_prev_box = TRUE;
          // initialized to calm gcc-11
          float opposite_x = 0.f, opposite_y = 0.f;

          if(fabsf(zoom_x - sample->box[0]) <= hx)
            opposite_x = sample->box[2];
          else if(fabsf(zoom_x - sample->box[2]) <= hx)
            opposite_x = sample->box[0];
          else
            on_corner_prev_box = FALSE;

          if(fabsf(zoom_y - sample->box[1]) <= hy)
            opposite_y = sample->box[3];
          else if(fabsf(zoom_y - sample->box[3]) <= hy)
            opposite_y = sample->box[1];
          else
            on_corner_prev_box = FALSE;

          if(on_corner_prev_box)
          {
            sample->point[0] = opposite_x;
            sample->point[1] = opposite_y;
          }
          else
          {
            sample->box[0] = fmaxf(0.0, zoom_x - delta_x);
            sample->box[1] = fmaxf(0.0, zoom_y - delta_y);
            sample->box[2] = fminf(1.0, zoom_x + delta_x);
            sample->box[3] = fminf(1.0, zoom_y + delta_y);
          }
          dt_control_change_cursor(GDK_FLEUR);
        }
        else if(sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
        {
          dev->preview_status = DT_DEV_PIXELPIPE_DIRTY;
        }
      }
      dt_control_queue_redraw_center();
      return 1;
    }

    if(which == 3)
    {
      // apply a live sample's area to the active picker?
      // FIXME: this is a naive implementation, nicer would be to cycle through overlapping samples then reset
      dt_iop_color_picker_t *picker = darktable.lib->proxy.colorpicker.picker_proxy;
      if(darktable.lib->proxy.colorpicker.display_samples && mouse_in_imagearea(self, &x, &y))
        for(GSList *samples = darktable.lib->proxy.colorpicker.live_samples; samples; samples = g_slist_next(samples))
        {
          dt_colorpicker_sample_t *live_sample = samples->data;
          if(live_sample->size == DT_LIB_COLORPICKER_SIZE_BOX
             && (picker->flags & DT_COLOR_PICKER_AREA))
          {
            if(zoom_x < live_sample->box[0] || zoom_x > live_sample->box[2]
               || zoom_y < live_sample->box[1] || zoom_y > live_sample->box[3])
              continue;
            dt_lib_colorpicker_set_box_area(darktable.lib, live_sample->box);
          }
          else if(live_sample->size == DT_LIB_COLORPICKER_SIZE_POINT
                  && (picker->flags & DT_COLOR_PICKER_POINT))
          {
            // magic values derived from _darkroom_pickers_draw
            float slop_px = MAX(26.0f, roundf(3.0f * zoom_scale));
            const float slop_x = slop_px / (procw * zoom_scale);
            const float slop_y = slop_px / (proch * zoom_scale);
            if(fabsf(zoom_x - live_sample->point[0]) > slop_x || fabsf(zoom_y - live_sample->point[1]) > slop_y)
              continue;
            dt_lib_colorpicker_set_point(darktable.lib, live_sample->point);
          }
          else
            continue;
          dev->preview_status = DT_DEV_PIXELPIPE_DIRTY;
          dt_control_queue_redraw_center();
          return 1;
        }

      if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
      {
        // default is hardcoded this way
        // FIXME: color_pixer_proxy should have an dt_iop_color_picker_clear_area() function for this
        dt_boundingbox_t reset = { 0.01f, 0.01f, 0.99f, 0.99f };
        dt_lib_colorpicker_set_box_area(darktable.lib, reset);
        dev->preview_status = DT_DEV_PIXELPIPE_DIRTY;
        dt_control_queue_redraw_center();
      }

      return 1;
    }
  }

  x += offx;
  y += offy;
  // masks
  if(dev->form_visible)
    handled = dt_masks_events_button_pressed(dev->gui_module, x, y, pressure, which, type, state);
  if(handled) return handled;
  // module
  if(dev->gui_module && dev->gui_module->button_pressed
     && dt_dev_modulegroups_get_activated(darktable.develop) != DT_MODULEGROUP_BASICS)
    handled = dev->gui_module->button_pressed(dev->gui_module, x, y, pressure, which, type, state);
  if(handled) return handled;

  if(which == 1 && type == GDK_2BUTTON_PRESS) return 0;
  if(which == 1)
  {
    dt_control_change_cursor(GDK_HAND1);
    return 1;
  }
  if(which == 2)
  {
    // zoom to 1:1 2:1 and back
    int procw, proch;
    dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
    int closeup = dt_control_get_dev_closeup();
    float zoom_x = dt_control_get_dev_zoom_x();
    float zoom_y = dt_control_get_dev_zoom_y();
    dt_dev_get_processed_size(dev, &procw, &proch);
    float scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 0);
    const float ppd = darktable.gui->ppd;
    const gboolean low_ppd = (darktable.gui->ppd == 1);
    const float mouse_off_x = x - 0.5f * dev->width;
    const float mouse_off_y = y - 0.5f * dev->height;
    zoom_x += mouse_off_x / (procw * scale);
    zoom_y += mouse_off_y / (proch * scale);
    const float tscale = scale * ppd;
    closeup = 0;
    if((tscale > 0.95f) && (tscale < 1.05f)) // we are at 100% and switch to 200%
    {
      zoom = DT_ZOOM_1;
      scale = dt_dev_get_zoom_scale(dev, DT_ZOOM_1, 1.0, 0);
      if(low_ppd) closeup = 1;
    }
    else if((tscale > 1.95f) && (tscale < 2.05f)) // at 200% so switch to zoomfit
    {
      zoom = DT_ZOOM_FIT;
      scale = dt_dev_get_zoom_scale(dev, DT_ZOOM_FIT, 1.0, 0);
    }
    else // other than 100 or 200% so zoom to 100 %
    {
      if(low_ppd)
      {
        zoom = DT_ZOOM_1;
        scale = dt_dev_get_zoom_scale(dev, DT_ZOOM_1, 1.0, 0);
      }
      else
      {
        zoom = DT_ZOOM_FREE;
        scale = 1.0f / ppd;
      }
    }
    dt_control_set_dev_zoom_scale(scale);
    dt_control_set_dev_closeup(closeup);
    scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 0);
    zoom_x -= mouse_off_x / (procw * scale);
    zoom_y -= mouse_off_y / (proch * scale);
    dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, zoom, closeup, NULL, NULL);
    dt_control_set_dev_zoom(zoom);
    dt_control_set_dev_zoom_x(zoom_x);
    dt_control_set_dev_zoom_y(zoom_y);
    dt_dev_invalidate(dev);
    dt_control_queue_redraw_center();
    dt_control_navigation_redraw();
    return 1;
  }
  return 0;
}

void scrollbar_changed(dt_view_t *self, double x, double y)
{
  dt_control_set_dev_zoom_x(x);
  dt_control_set_dev_zoom_y(y);

  /* redraw pipe */
  dt_dev_invalidate(darktable.develop);
  dt_control_queue_redraw_center();
  dt_control_navigation_redraw();
}

void scrolled(dt_view_t *self, double x, double y, int up, int state)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  const int32_t tb = dev->border_size;
  const int32_t capwd = self->width  - 2*tb;
  const int32_t capht = self->height - 2*tb;
  const int32_t width_i = self->width;
  const int32_t height_i = self->height;
  if(width_i > capwd) x += (capwd - width_i) * .5f;
  if(height_i > capht) y += (capht - height_i) * .5f;

  int handled = 0;
  // masks
  if(dev->form_visible) handled = dt_masks_events_mouse_scrolled(dev->gui_module, x, y, up, state);
  if(handled) return;
  // module
  if(dev->gui_module && dev->gui_module->scrolled
     && dt_dev_modulegroups_get_activated(darktable.develop) != DT_MODULEGROUP_BASICS)
    handled = dev->gui_module->scrolled(dev->gui_module, x, y, up, state);
  if(handled) return;
  // free zoom
  int procw, proch;
  dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  int closeup = dt_control_get_dev_closeup();
  float zoom_x = dt_control_get_dev_zoom_x();
  float zoom_y = dt_control_get_dev_zoom_y();
  dt_dev_get_processed_size(dev, &procw, &proch);
  float scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 0);
  const float ppd = darktable.gui->ppd;
  const float fitscale = dt_dev_get_zoom_scale(dev, DT_ZOOM_FIT, 1.0, 0);
  const float oldscale = scale;

  // offset from center now (current zoom_{x,y} points there)
  const float mouse_off_x = x - 0.5f * dev->width;
  const float mouse_off_y = y - 0.5f * dev->height;
  zoom_x += mouse_off_x / (procw * scale);
  zoom_y += mouse_off_y / (proch * scale);
  zoom = DT_ZOOM_FREE;
  closeup = 0;

  const gboolean constrained = !dt_modifier_is(state, GDK_CONTROL_MASK);
  const gboolean low_ppd = (darktable.gui->ppd == 1);
  const float stepup = 0.1f * fabsf(1.0f - fitscale) / ppd;

  if(up)
  {
    if(fitscale <= 1.0f && (scale == (1.0f / ppd) || scale == (2.0f / ppd)) && constrained) return; // for large image size
    else if(fitscale > 1.0f && fitscale <= 2.0f && scale == (2.0f / ppd) && constrained) return; // for medium image size

    if((oldscale <= 1.0f / ppd) && constrained && (scale + stepup >= 1.0f / ppd))
      scale = 1.0f / ppd;
    else if((oldscale <= 2.0f / ppd) && constrained && (scale + stepup >= 2.0f / ppd))
      scale = 2.0f / ppd;
    // calculate new scale
    else if(scale >= 16.0f / ppd)
      return;
    else if(scale >= 8.0f / ppd)
      scale = 16.0f / ppd;
    else if(scale >= 4.0f / ppd)
      scale = 8.0f / ppd;
    else if(scale >= 2.0f / ppd)
      scale = 4.0f / ppd;
    else if(scale >= fitscale)
      scale += stepup;
    else
      scale += 0.5f * stepup;
  }
  else
  {
    if(fitscale <= 2.0f && ((scale == fitscale && constrained) || scale < 0.5 * fitscale)) return; // for large and medium image size
    else if(fitscale > 2.0f && scale < 1.0f / ppd) return; // for small image size

    // calculate new scale
    if(scale <= fitscale)
      scale -= 0.5f * stepup;
    else if(scale <= 2.0f / ppd)
      scale -= stepup;
    else if(scale <= 4.0f / ppd)
      scale = 2.0f / ppd;
    else if(scale <= 8.0f / ppd)
      scale = 4.0f / ppd;
    else
      scale = 8.0f / ppd;
  }

  if(fitscale <= 1.0f) // for large image size, stop at 1:1 and FIT levels, minimum at 0.5 * FIT
  {
    if((scale - 1.0) * (oldscale - 1.0) < 0) scale = 1.0f / ppd;
    if((scale - fitscale) * (oldscale - fitscale) < 0) scale = fitscale;
    scale = fmaxf(scale, 0.5 * fitscale);
  }
  else if(fitscale > 1.0f && fitscale <= 2.0f) // for medium image size, stop at 2:1 and FIT levels, minimum at 0.5 * FIT
  {
    if((scale - 2.0) * (oldscale - 2.0) < 0) scale = 2.0f / ppd;
    if((scale - fitscale) * (oldscale - fitscale) < 0) scale = fitscale;
    scale = fmaxf(scale, 0.5 * fitscale);
  }
  else scale = fmaxf(scale, 1.0f / ppd); // for small image size, minimum at 1:1
  scale = fminf(scale, 16.0f / ppd);

  // pixel doubling instead of interpolation at >= 200% lodpi, >= 400% hidpi
  if(scale > 15.9999f / ppd)
  {
    scale = dt_dev_get_zoom_scale(dev, DT_ZOOM_1, 1.0, 0);
    zoom = DT_ZOOM_1;
    closeup = low_ppd ? 4 : 3;
  }
  else if(scale > 7.9999f / ppd)
  {
    scale = dt_dev_get_zoom_scale(dev, DT_ZOOM_1, 1.0, 0);
    zoom = DT_ZOOM_1;
    closeup = low_ppd ? 3 : 2;
  }
  else if(scale > 3.9999f / ppd)
  {
    scale = dt_dev_get_zoom_scale(dev, DT_ZOOM_1, 1.0, 0);
    zoom = DT_ZOOM_1;
    closeup = low_ppd ? 2 : 1;
  }
  else if(scale > 1.9999f / ppd)
  {
    scale = dt_dev_get_zoom_scale(dev, DT_ZOOM_1, 1.0, 0);
    zoom = DT_ZOOM_1;
    if(low_ppd) closeup = 1;
  }

  if(fabsf(scale - 1.0f) < 0.001f) zoom = DT_ZOOM_1;
  if(fabsf(scale - fitscale) < 0.001f) zoom = DT_ZOOM_FIT;
  dt_control_set_dev_zoom_scale(scale);
  dt_control_set_dev_closeup(closeup);
  scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 0);

  zoom_x -= mouse_off_x / (procw * scale);
  zoom_y -= mouse_off_y / (proch * scale);
  dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, zoom, closeup, NULL, NULL);
  dt_control_set_dev_zoom(zoom);
  dt_control_set_dev_zoom_x(zoom_x);
  dt_control_set_dev_zoom_y(zoom_y);
  dt_dev_invalidate(dev);
  dt_control_queue_redraw_center();
  dt_control_navigation_redraw();
}

static void change_slider_accel_precision(dt_action_t *action)
{
  const int curr_precision = dt_conf_get_int("accel/slider_precision");
  const int new_precision = curr_precision + 1 == 3 ? 0 : curr_precision + 1;
  dt_conf_set_int("accel/slider_precision", new_precision);

  if(new_precision == DT_IOP_PRECISION_FINE)
    dt_toast_log(_("keyboard shortcut slider precision: fine"));
  else if(new_precision == DT_IOP_PRECISION_NORMAL)
    dt_toast_log(_("keyboard shortcut slider precision: normal"));
  else
    dt_toast_log(_("keyboard shortcut slider precision: coarse"));
}

void configure(dt_view_t *self, int wd, int ht)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  dev->orig_width = wd;
  dev->orig_height = ht;
  dev->border_size = _iso_12646_get_border(dev);
  dt_dev_configure(dev, wd, ht);
}

GSList *mouse_actions(const dt_view_t *self)
{
  GSList *lm = NULL;
  GSList *lm2 = NULL;
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_DOUBLE_LEFT, 0, _("switch to lighttable"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_SCROLL, 0, _("zoom in the image"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_SCROLL, GDK_CONTROL_MASK, _("unbounded zoom in the image"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_MIDDLE, 0, _("zoom to 100% 200% and back"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_LEFT_DRAG, 0, _("pan a zoomed image"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_LEFT, GDK_SHIFT_MASK, dt_conf_get_bool("darkroom/ui/single_module")
                                     ? _("[modules] expand module without closing others")
                                     : _("[modules] expand module and close others"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_LEFT, GDK_CONTROL_MASK,
                                     _("[modules] rename module"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_DRAG_DROP, GDK_SHIFT_MASK | GDK_CONTROL_MASK,
                                     _("[modules] change module position in pipe"));

  const dt_develop_t *dev = (dt_develop_t *)self->data;
  if(dev->form_visible)
  {
    // masks
    lm2 = dt_masks_mouse_actions(dev->form_visible);
  }
  else if(dev->gui_module && dev->gui_module->mouse_actions)
  {
    // modules with on canvas actions
    lm2 = dev->gui_module->mouse_actions(dev->gui_module);
  }

  return g_slist_concat(lm, lm2);
}

//-----------------------------------------------------------
// second darkroom window
//-----------------------------------------------------------

/* helper macro that applies the DPI transformation to fixed pixel values. input should be defaulting to 96
 * DPI */
#define DT_PIXEL_APPLY_DPI_2ND_WND(dev, value) ((value) * dev->second_window.dpi_factor)

static cairo_filter_t _get_second_window_filtering_level(dt_develop_t *dev, dt_dev_zoom_t zoom, int closeup)
{
  const float scale = dt_second_window_get_zoom_scale(dev, zoom, 1<<closeup, 0);

  // for pixel representation above 1:1, that is when a single pixel on the image
  // is represented on screen by multiple pixels we want to disable any cairo filter
  // which could only blur or smooth the output.

  if(scale >= 0.9999f)
    return CAIRO_FILTER_FAST;
  else
    return darktable.gui->dr_filter_image;
}

static void dt_second_window_change_cursor(dt_develop_t *dev, dt_cursor_t curs)
{
  GtkWidget *widget = dev->second_window.second_wnd;
  GdkCursor *cursor = gdk_cursor_new_for_display(gdk_display_get_default(), curs);
  gdk_window_set_cursor(gtk_widget_get_window(widget), cursor);
  g_object_unref(cursor);
}

static void second_window_expose(GtkWidget *widget, dt_develop_t *dev, cairo_t *cri, int32_t width, int32_t height,
                                 int32_t pointerx, int32_t pointery)
{
  cairo_set_source_rgb(cri, .2, .2, .2);
  cairo_save(cri);

  const int32_t tb = dev->second_window.border_size;
  // account for border, make it transparent for other modules called below:
  pointerx -= tb;
  pointery -= tb;

  if(dev->preview2_status == DT_DEV_PIXELPIPE_DIRTY
     || dev->preview2_status == DT_DEV_PIXELPIPE_INVALID
     || dev->pipe->input_timestamp > dev->preview2_pipe->input_timestamp)
    dt_dev_process_preview2(dev);

  dt_pthread_mutex_t *mutex = NULL;
  const float zoom_y = dt_second_window_get_dev_zoom_y(dev);
  const float zoom_x = dt_second_window_get_dev_zoom_x(dev);
  const dt_dev_zoom_t zoom = dt_second_window_get_dev_zoom(dev);
  const int closeup = dt_second_window_get_dev_closeup(dev);
  const float backbuf_scale = dt_second_window_get_zoom_scale(dev, zoom, 1.0f, 0) * dev->second_window.ppd;

  static cairo_surface_t *image_surface = NULL;
  static int image_surface_width = 0, image_surface_height = 0, image_surface_imgid = -1;

  if(image_surface_width != width
     || image_surface_height != height
     || image_surface == NULL)
  {
    // create double-buffered image to draw on, to make modules draw more fluently.
    image_surface_width = width;
    image_surface_height = height;
    if(image_surface) cairo_surface_destroy(image_surface);
    image_surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width * dev->second_window.ppd, height * dev->second_window.ppd);
    cairo_surface_set_device_scale(image_surface, dev->second_window.ppd, dev->second_window.ppd);

    image_surface_imgid = -1; // invalidate old stuff
  }
  cairo_surface_t *surface;
  cairo_t *cr = cairo_create(image_surface);

  if(dev->preview2_pipe->output_backbuf  // do we have an image?
     && dev->preview2_pipe->backbuf_scale == backbuf_scale // is this the zoom scale we want to display?
     && dev->preview2_pipe->backbuf_zoom_x == zoom_x
     && dev->preview2_pipe->backbuf_zoom_y == zoom_y)
  {
    // draw image
    mutex = &dev->preview2_pipe->backbuf_mutex;
    dt_pthread_mutex_lock(mutex);
    const float wd = dev->preview2_pipe->output_backbuf_width;
    const float ht = dev->preview2_pipe->output_backbuf_height;

    surface = dt_view_create_surface(dev->preview2_pipe->output_backbuf, wd, ht);

    dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_DARKROOM_BG);
    cairo_paint(cr);

    dt_view_paint_surface(cr, width, height, surface, wd, ht, DT_WINDOW_SECOND);

    cairo_surface_destroy(surface);
    dt_pthread_mutex_unlock(mutex);
    image_surface_imgid = dev->image_storage.id;
  }
  else if(dev->preview_pipe->output_backbuf)
  {
    // draw preview
    mutex = &dev->preview_pipe->backbuf_mutex;
    dt_pthread_mutex_lock(mutex);

    const float wd = dev->preview_pipe->output_backbuf_width;
    const float ht = dev->preview_pipe->output_backbuf_height;
    const float zoom_scale = dt_second_window_get_zoom_scale(dev, zoom, 1 << closeup, 1);
    dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_DARKROOM_BG);
    cairo_paint(cr);
    cairo_rectangle(cr, tb, tb, width - 2 * tb, height - 2 * tb);
    cairo_clip(cr);

    surface = dt_view_create_surface(dev->preview_pipe->output_backbuf, wd, ht);

    cairo_translate(cr, width / 2.0, height / 2.0f);
    cairo_scale(cr, zoom_scale, zoom_scale);
    cairo_translate(cr, -.5f * wd - zoom_x * wd, -.5f * ht - zoom_y * ht);
    // avoid to draw the 1px garbage that sometimes shows up in the preview :(
    cairo_rectangle(cr, 0, 0, wd - 1, ht - 1);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), _get_second_window_filtering_level(dev, zoom, closeup));
    cairo_fill(cr);
    cairo_surface_destroy(surface);
    dt_pthread_mutex_unlock(mutex);
    image_surface_imgid = dev->image_storage.id;
  }

  cairo_restore(cri);

  if(image_surface_imgid == dev->image_storage.id)
  {
    cairo_destroy(cr);
    cairo_set_source_surface(cri, image_surface, 0, 0);
    cairo_paint(cri);
  }
}

static void second_window_scrolled(GtkWidget *widget, dt_develop_t *dev, double x, double y, const int up,
                                   const int state)
{
  const int32_t tb = dev->second_window.border_size;
  const int32_t capwd = dev->second_window.width - 2 * tb;
  const int32_t capht = dev->second_window.height - 2 * tb;
  const int32_t width_i = dev->second_window.width;
  const int32_t height_i = dev->second_window.height;
  if(width_i > capwd) x += (capwd - width_i) * .5f;
  if(height_i > capht) y += (capht - height_i) * .5f;

  // free zoom
  dt_dev_zoom_t zoom = dt_second_window_get_dev_zoom(dev);
  int procw, proch;
  int closeup = dt_second_window_get_dev_closeup(dev);
  float zoom_x = dt_second_window_get_dev_zoom_x(dev);
  float zoom_y = dt_second_window_get_dev_zoom_y(dev);
  dt_second_window_get_processed_size(dev, &procw, &proch);
  float scale = dt_second_window_get_zoom_scale(dev, zoom, 1 << closeup, 0);
  const float ppd = dev->second_window.ppd;
  const float fitscale = dt_second_window_get_zoom_scale(dev, DT_ZOOM_FIT, 1.0, 0);
  const float oldscale = scale;

  // offset from center now (current zoom_{x,y} points there)
  const float mouse_off_x = x - 0.5f * dev->second_window.width;
  const float mouse_off_y = y - 0.5f * dev->second_window.height;
  zoom_x += mouse_off_x / (procw * scale);
  zoom_y += mouse_off_y / (proch * scale);
  zoom = DT_ZOOM_FREE;
  closeup = 0;

  const gboolean constrained = !dt_modifier_is(state, GDK_CONTROL_MASK);
  const gboolean low_ppd = (dev->second_window.ppd == 1);
  const float stepup = 0.1f * fabsf(1.0f - fitscale) / ppd;
  if(up)
  {
    if(fitscale <= 1.0f && (scale == (1.0f / ppd) || scale == (2.0f / ppd)) && constrained) return; // for large image size
    else if(fitscale > 1.0f && fitscale <= 2.0f && scale == (2.0f / ppd) && constrained) return; // for medium image size

    if((oldscale <= 1.0f / ppd) && constrained && (scale + stepup >= 1.0f / ppd))
      scale = 1.0f / ppd;
    else if((oldscale <= 2.0f / ppd) && constrained && (scale + stepup >= 2.0f / ppd))
      scale = 2.0f / ppd;
    // calculate new scale
    else if(scale >= 16.0f / ppd)
      return;
    else if(scale >= 8.0f / ppd)
      scale = 16.0f / ppd;
    else if(scale >= 4.0f / ppd)
      scale = 8.0f / ppd;
    else if(scale >= 2.0f / ppd)
      scale = 4.0f / ppd;
    else if(scale >= fitscale)
      scale += stepup;
    else
      scale += 0.5f * stepup;
  }
  else
  {
    if(fitscale <= 2.0f && ((scale == fitscale && constrained) || scale < 0.5 * fitscale)) return; // for large and medium image size
    else if(fitscale > 2.0f && scale < 1.0f / ppd) return; // for small image size

    // calculate new scale
    if(scale <= fitscale)
      scale -= 0.5f * stepup;
    else if(scale <= 2.0f / ppd)
      scale -= stepup;
    else if(scale <= 4.0f / ppd)
      scale = 2.0f / ppd;
    else if(scale <= 8.0f / ppd)
      scale = 4.0f / ppd;
    else
      scale = 8.0f / ppd;
  }
  if(fitscale <= 1.0f) // for large image size, stop at 1:1 and FIT levels, minimum at 0.5 * FIT
  {
    if((scale - 1.0) * (oldscale - 1.0) < 0) scale = 1.0f / ppd;
    if((scale - fitscale) * (oldscale - fitscale) < 0) scale = fitscale;
    scale = fmaxf(scale, 0.5 * fitscale);
  }
  else if(fitscale > 1.0f && fitscale <= 2.0f) // for medium image size, stop at 2:1 and FIT levels, minimum at 0.5 * FIT
  {
    if((scale - 2.0) * (oldscale - 2.0) < 0) scale = 2.0f / ppd;
    if((scale - fitscale) * (oldscale - fitscale) < 0) scale = fitscale;
    scale = fmaxf(scale, 0.5 * fitscale);
  }
  else scale = fmaxf(scale, 1.0f / ppd); // for small image size, minimum at 1:1
  scale = fminf(scale, 16.0f / ppd);

  // pixel doubling instead of interpolation at >= 200% lodpi, >= 400% hidpi
  if(scale > 15.9999f / ppd)
  {
    scale = dt_second_window_get_zoom_scale(dev, DT_ZOOM_1, 1.0, 0);
    zoom = DT_ZOOM_1;
    closeup = low_ppd ? 4 : 3;
  }
  else if(scale > 7.9999f / ppd)
  {
    scale = dt_second_window_get_zoom_scale(dev, DT_ZOOM_1, 1.0, 0);
    zoom = DT_ZOOM_1;
    closeup = low_ppd ? 3 : 2;
  }
  else if(scale > 3.9999f / ppd)
  {
    scale = dt_second_window_get_zoom_scale(dev, DT_ZOOM_1, 1.0, 0);
    zoom = DT_ZOOM_1;
    closeup = low_ppd ? 2 : 1;
  }
  else if(scale > 1.9999f / ppd)
  {
   scale = dt_second_window_get_zoom_scale(dev, DT_ZOOM_1, 1.0, 0);
   zoom = DT_ZOOM_1;
   if(low_ppd) closeup = 1;
  }

  if(fabsf(scale - 1.0f) < 0.001f) zoom = DT_ZOOM_1;
  if(fabsf(scale - fitscale) < 0.001f) zoom = DT_ZOOM_FIT;
  dt_second_window_set_zoom_scale(dev, scale);
  dt_second_window_set_dev_closeup(dev, closeup);
  scale = dt_second_window_get_zoom_scale(dev, zoom, 1 << closeup, 0);

  zoom_x -= mouse_off_x / (procw * scale);
  zoom_y -= mouse_off_y / (proch * scale);
  dt_second_window_check_zoom_bounds(dev, &zoom_x, &zoom_y, zoom, closeup, NULL, NULL);
  dt_second_window_set_dev_zoom(dev, zoom);
  dt_second_window_set_dev_zoom_x(dev, zoom_x);
  dt_second_window_set_dev_zoom_y(dev, zoom_y);

  // pipe needs to be reconstructed
  dev->preview2_status = DT_DEV_PIXELPIPE_DIRTY;

  gtk_widget_queue_draw(widget);
}

static void second_window_leave(dt_develop_t *dev)
{
  // reset any changes the selected plugin might have made.
  dt_second_window_change_cursor(dev, GDK_LEFT_PTR);
}

static int second_window_button_pressed(GtkWidget *widget, dt_develop_t *dev, double x, double y, const double pressure,
                                        const int which, const int type, const uint32_t state)
{
  const int32_t tb = dev->second_window.border_size;
  const int32_t capwd = dev->second_window.width - 2 * tb;
  const int32_t capht = dev->second_window.height - 2 * tb;
  const int32_t width_i = dev->second_window.width;
  const int32_t height_i = dev->second_window.height;
  if(width_i > capwd) x += (capwd - width_i) * .5f;
  if(height_i > capht) y += (capht - height_i) * .5f;

  dev->second_window.button_x = x - tb;
  dev->second_window.button_y = y - tb;

  if(which == 1 && type == GDK_2BUTTON_PRESS) return 0;
  if(which == 1)
  {
    dt_second_window_change_cursor(dev, GDK_HAND1);
    return 1;
  }
  if(which == 2)
  {
    // zoom to 1:1 2:1 and back
    int procw, proch;
    dt_dev_zoom_t zoom = dt_second_window_get_dev_zoom(dev);
    int closeup = dt_second_window_get_dev_closeup(dev);
    float zoom_x = dt_second_window_get_dev_zoom_x(dev);
    float zoom_y = dt_second_window_get_dev_zoom_y(dev);
    dt_second_window_get_processed_size(dev, &procw, &proch);
    float scale = dt_second_window_get_zoom_scale(dev, zoom, 1 << closeup, 0);
    const float ppd = dev->second_window.ppd;
    const gboolean low_ppd = dev->second_window.ppd == 1;

    const float mouse_off_x = x - 0.5f * dev->second_window.width;
    const float mouse_off_y = y - 0.5f * dev->second_window.height;
    zoom_x += mouse_off_x / (procw * scale);
    zoom_y += mouse_off_y / (proch * scale);
    const float tscale = scale * ppd;
    closeup = 0;

    if((tscale > 0.95f) && (tscale < 1.05f)) // we are at 100% and switch to 200%
    {
      zoom = DT_ZOOM_1;
      scale = dt_dev_get_zoom_scale(dev, DT_ZOOM_1, 1.0, 0);
      if(low_ppd) closeup = 1;
    }
    else if((tscale > 1.95f) && (tscale < 2.05f)) // at 200% so switch to zoomfit
    {
      zoom = DT_ZOOM_FIT;
      scale = dt_dev_get_zoom_scale(dev, DT_ZOOM_FIT, 1.0, 0);
    }
    else // other than 100 or 200% so zoom to 100 %
    {
      if(low_ppd)
      {
        zoom = DT_ZOOM_1;
        scale = dt_dev_get_zoom_scale(dev, DT_ZOOM_1, 1.0, 0);
      }
      else
      {
        zoom = DT_ZOOM_FREE;
        scale = 1.0f / ppd;
      }
    }
    dt_second_window_set_zoom_scale(dev, scale);
    dt_second_window_set_dev_closeup(dev, closeup);
    scale = dt_second_window_get_zoom_scale(dev, zoom, 1 << closeup, 0);
    zoom_x -= mouse_off_x / (procw * scale);
    zoom_y -= mouse_off_y / (proch * scale);
    dt_second_window_check_zoom_bounds(dev, &zoom_x, &zoom_y, zoom, closeup, NULL, NULL);
    dt_second_window_set_dev_zoom(dev, zoom);
    dt_second_window_set_dev_zoom_x(dev, zoom_x);
    dt_second_window_set_dev_zoom_y(dev, zoom_y);

    // pipe needs to be reconstructed
    dev->preview2_status = DT_DEV_PIXELPIPE_DIRTY;

    gtk_widget_queue_draw(widget);

    return 1;
  }
  return 0;
}

static int second_window_button_released(dt_develop_t *dev, const double x, const double y, const int which,
                                         const uint32_t state)
{
  if(which == 1) dt_second_window_change_cursor(dev, GDK_LEFT_PTR);
  return 1;
}

static void second_window_mouse_moved(GtkWidget *widget, dt_develop_t *dev, double x, double y,
                                      const double pressure, const int which)
{
  const int32_t tb = dev->second_window.border_size;
  const int32_t capwd = dev->second_window.width - 2 * tb;
  const int32_t capht = dev->second_window.height - 2 * tb;

  const int32_t width_i = dev->second_window.width;
  const int32_t height_i = dev->second_window.height;
  int32_t offx = 0.0f, offy = 0.0f;
  if(width_i > capwd) offx = (capwd - width_i) * .5f;
  if(height_i > capht) offy = (capht - height_i) * .5f;

  x += offx;
  y += offy;

  if(which & GDK_BUTTON1_MASK)
  {
    // depending on dev_zoom, adjust dev_zoom_x/y.
    const dt_dev_zoom_t zoom = dt_second_window_get_dev_zoom(dev);
    const int closeup = dt_second_window_get_dev_closeup(dev);
    int procw, proch;
    dt_second_window_get_processed_size(dev, &procw, &proch);
    const float scale = dt_second_window_get_zoom_scale(dev, zoom, 1 << closeup, 0);
    float old_zoom_x, old_zoom_y;
    old_zoom_x = dt_second_window_get_dev_zoom_x(dev);
    old_zoom_y = dt_second_window_get_dev_zoom_y(dev);
    float zx = old_zoom_x - (1.0 / scale) * (x - dev->second_window.button_x - offx) / procw;
    float zy = old_zoom_y - (1.0 / scale) * (y - dev->second_window.button_y - offy) / proch;
    dt_second_window_check_zoom_bounds(dev, &zx, &zy, zoom, closeup, NULL, NULL);
    dt_second_window_set_dev_zoom_x(dev, zx);
    dt_second_window_set_dev_zoom_y(dev, zy);
    dev->second_window.button_x = x - offx;
    dev->second_window.button_y = y - offy;

    // pipe needs to be reconstructed
    dev->preview2_status = DT_DEV_PIXELPIPE_DIRTY;

    gtk_widget_queue_draw(widget);
  }
}

static void _second_window_configure_ppd_dpi(dt_develop_t *dev)
{
  GtkWidget *widget = dev->second_window.second_wnd;

  dev->second_window.ppd = dev->second_window.ppd_thb = dt_get_system_gui_ppd(widget);
  if(dt_conf_get_bool("ui/performance"))
    dev->second_window.ppd_thb *= DT_GUI_THUMBSIZE_REDUCE;

  // get the screen resolution
  float screen_dpi_overwrite = dt_conf_get_float("screen_dpi_overwrite");
  if(screen_dpi_overwrite > 0.0)
  {
    dev->second_window.dpi = screen_dpi_overwrite;
    gdk_screen_set_resolution(gtk_widget_get_screen(widget), screen_dpi_overwrite);
    dt_print(DT_DEBUG_CONTROL, "[screen resolution] setting the screen resolution to %f dpi as specified in "
                               "the configuration file\n", screen_dpi_overwrite);
  }
  else
  {
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_autoset_dpi(widget);
#endif
    dev->second_window.dpi = gdk_screen_get_resolution(gtk_widget_get_screen(widget));
    if(dev->second_window.dpi < 0.0)
    {
      dev->second_window.dpi = 96.0;
      gdk_screen_set_resolution(gtk_widget_get_screen(widget), 96.0);
      dt_print(DT_DEBUG_CONTROL, "[screen resolution] setting the screen resolution to the default 96 dpi\n");
    }
    else
      dt_print(DT_DEBUG_CONTROL, "[screen resolution] setting the screen resolution to %f dpi\n", dev->second_window.dpi);
  }
  dev->second_window.dpi_factor
      = dev->second_window.dpi / 96; // according to man xrandr and the docs of gdk_screen_set_resolution 96 is the default
}

static gboolean _second_window_draw_callback(GtkWidget *widget, cairo_t *crf, dt_develop_t *dev)
{
  int pointerx, pointery;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int32_t width = allocation.width;
  const int32_t height = allocation.height;

  dt_dev_second_window_configure(dev, width, height);
  dt_control_queue_redraw_center();

  gdk_window_get_device_position(gtk_widget_get_window(widget),
                                 gdk_seat_get_pointer(gdk_display_get_default_seat(gtk_widget_get_display(widget))),
                                 &pointerx, &pointery, NULL);
  second_window_expose(widget, dev, crf, width, height, pointerx, pointery);

  return TRUE;
}

static gboolean _second_window_scrolled_callback(GtkWidget *widget, GdkEventScroll *event, dt_develop_t *dev)
{
  int delta_y;
  if(dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y))
  {
    second_window_scrolled(widget, dev, event->x, event->y, delta_y < 0, event->state & 0xf);
    gtk_widget_queue_draw(widget);
  }

  return TRUE;
}

static gboolean _second_window_button_pressed_callback(GtkWidget *w, GdkEventButton *event, dt_develop_t *dev)
{
  double pressure = 1.0;
  GdkDevice *device = gdk_event_get_source_device((GdkEvent *)event);

  if(device && gdk_device_get_source(device) == GDK_SOURCE_PEN)
  {
    gdk_event_get_axis((GdkEvent *)event, GDK_AXIS_PRESSURE, &pressure);
  }
  second_window_button_pressed(w, dev, event->x, event->y, pressure, event->button, event->type, event->state & 0xf);
  gtk_widget_grab_focus(w);
  gtk_widget_queue_draw(w);
  return FALSE;
}

static gboolean _second_window_button_released_callback(GtkWidget *w, GdkEventButton *event, dt_develop_t *dev)
{
  second_window_button_released(dev, event->x, event->y, event->button, event->state & 0xf);
  gtk_widget_queue_draw(w);
  return TRUE;
}

static gboolean _second_window_mouse_moved_callback(GtkWidget *w, GdkEventMotion *event, dt_develop_t *dev)
{
  double pressure = 1.0;
  GdkDevice *device = gdk_event_get_source_device((GdkEvent *)event);

  if(device && gdk_device_get_source(device) == GDK_SOURCE_PEN)
  {
    gdk_event_get_axis((GdkEvent *)event, GDK_AXIS_PRESSURE, &pressure);
  }
  second_window_mouse_moved(w, dev, event->x, event->y, pressure, event->state);
  return FALSE;
}

static gboolean _second_window_leave_callback(GtkWidget *widget, GdkEventCrossing *event, dt_develop_t *dev)
{
  second_window_leave(dev);
  return TRUE;
}

static gboolean _second_window_configure_callback(GtkWidget *da, GdkEventConfigure *event, dt_develop_t *dev)
{
  static int oldw = 0;
  static int oldh = 0;

  if(oldw != event->width || oldh != event->height)
  {
    dev->second_window.width = event->width;
    dev->second_window.height = event->height;
    dev->second_window.orig_width = event->width;
    dev->second_window.orig_height = event->height;

    // pipe needs to be reconstructed
    dev->preview2_status = DT_DEV_PIXELPIPE_DIRTY;
    dev->preview2_pipe->changed |= DT_DEV_PIPE_REMOVE;
    dev->preview2_pipe->cache_obsolete = 1;
  }
  oldw = event->width;
  oldh = event->height;

  dt_colorspaces_set_display_profile(DT_COLORSPACE_DISPLAY2);

#ifndef GDK_WINDOWING_QUARTZ
  _second_window_configure_ppd_dpi(dev);
#endif

  return TRUE;
}

static void _darkroom_ui_second_window_init(GtkWidget *widget, dt_develop_t *dev)
{
  const int width = MAX(10, dt_conf_get_int("second_window/window_w"));
  const int height = MAX(10, dt_conf_get_int("second_window/window_h"));

  dev->second_window.width = width;
  dev->second_window.height = height;
  dev->second_window.orig_width = width;
  dev->second_window.orig_height = height;
  dev->second_window.border_size = 0;

  const gint x = MAX(0, dt_conf_get_int("second_window/window_x"));
  const gint y = MAX(0, dt_conf_get_int("second_window/window_y"));
  gtk_window_set_default_size(GTK_WINDOW(widget), width, height);
  gtk_widget_show_all(widget);
  gtk_window_move(GTK_WINDOW(widget), x, y);
  gtk_window_resize(GTK_WINDOW(widget), width, height);
  const int fullscreen = dt_conf_get_bool("second_window/fullscreen");
  if(fullscreen)
    gtk_window_fullscreen(GTK_WINDOW(widget));
  else
  {
    gtk_window_unfullscreen(GTK_WINDOW(widget));
    const int maximized = dt_conf_get_bool("second_window/maximized");
    if(maximized)
      gtk_window_maximize(GTK_WINDOW(widget));
    else
      gtk_window_unmaximize(GTK_WINDOW(widget));
  }
}

static void _darkroom_ui_second_window_write_config(GtkWidget *widget)
{
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  gint x, y;
  gtk_window_get_position(GTK_WINDOW(widget), &x, &y);
  dt_conf_set_int("second_window/window_x", x);
  dt_conf_set_int("second_window/window_y", y);
  dt_conf_set_int("second_window/window_w", allocation.width);
  dt_conf_set_int("second_window/window_h", allocation.height);
  dt_conf_set_bool("second_window/maximized",
                   (gdk_window_get_state(gtk_widget_get_window(widget)) & GDK_WINDOW_STATE_MAXIMIZED));
  dt_conf_set_bool("second_window/fullscreen",
                   (gdk_window_get_state(gtk_widget_get_window(widget)) & GDK_WINDOW_STATE_FULLSCREEN));
}

static gboolean _second_window_delete_callback(GtkWidget *widget, GdkEvent *event, dt_develop_t *dev)
{
  _darkroom_ui_second_window_write_config(dev->second_window.second_wnd);

  dev->second_window.second_wnd = NULL;
  dev->second_window.widget = NULL;

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dev->second_window.button), FALSE);

  return FALSE;
}

static void _darkroom_display_second_window(dt_develop_t *dev)
{
  if(dev->second_window.second_wnd == NULL)
  {
    dev->second_window.width = -1;
    dev->second_window.height = -1;

    dev->second_window.second_wnd = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_name(dev->second_window.second_wnd, "second_window");

    _second_window_configure_ppd_dpi(dev);

    gtk_window_set_icon_name(GTK_WINDOW(dev->second_window.second_wnd), "darktable");
    gtk_window_set_title(GTK_WINDOW(dev->second_window.second_wnd), _("darktable - darkroom preview"));

    GtkWidget *container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(dev->second_window.second_wnd), container);

    GtkWidget *widget = gtk_grid_new();
    gtk_box_pack_start(GTK_BOX(container), widget, TRUE, TRUE, 0);

    dev->second_window.widget = gtk_drawing_area_new();
    gtk_widget_set_size_request(dev->second_window.widget, DT_PIXEL_APPLY_DPI_2ND_WND(dev, 50), DT_PIXEL_APPLY_DPI_2ND_WND(dev, 200));
    gtk_widget_set_hexpand(dev->second_window.widget, TRUE);
    gtk_widget_set_vexpand(dev->second_window.widget, TRUE);
    gtk_widget_set_app_paintable(dev->second_window.widget, TRUE);

    gtk_grid_attach(GTK_GRID(widget), dev->second_window.widget, 0, 0, 1, 1);

    gtk_widget_set_events(dev->second_window.widget, GDK_POINTER_MOTION_MASK
                                                         | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                                         | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK
                                                         | darktable.gui->scroll_mask);

    /* connect callbacks */
    g_signal_connect(G_OBJECT(dev->second_window.widget), "draw", G_CALLBACK(_second_window_draw_callback), dev);
    g_signal_connect(G_OBJECT(dev->second_window.widget), "scroll-event",
                     G_CALLBACK(_second_window_scrolled_callback), dev);
    g_signal_connect(G_OBJECT(dev->second_window.widget), "button-press-event",
                     G_CALLBACK(_second_window_button_pressed_callback), dev);
    g_signal_connect(G_OBJECT(dev->second_window.widget), "button-release-event",
                     G_CALLBACK(_second_window_button_released_callback), dev);
    g_signal_connect(G_OBJECT(dev->second_window.widget), "motion-notify-event",
                     G_CALLBACK(_second_window_mouse_moved_callback), dev);
    g_signal_connect(G_OBJECT(dev->second_window.widget), "leave-notify-event",
                     G_CALLBACK(_second_window_leave_callback), dev);
    g_signal_connect(G_OBJECT(dev->second_window.widget), "configure-event",
                     G_CALLBACK(_second_window_configure_callback), dev);

    g_signal_connect(G_OBJECT(dev->second_window.second_wnd), "delete-event",
                     G_CALLBACK(_second_window_delete_callback), dev);
    g_signal_connect(G_OBJECT(dev->second_window.second_wnd), "event",
                     G_CALLBACK(dt_shortcut_dispatcher), NULL);

    _darkroom_ui_second_window_init(dev->second_window.second_wnd, dev);
  }

  gtk_widget_show_all(dev->second_window.second_wnd);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
