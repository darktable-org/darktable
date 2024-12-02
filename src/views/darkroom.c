/*
    This file is part of darktable,
    Copyright (C) 2009-2024 darktable developers.

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
#include "common/overlay.h"
#include "common/selection.h"
#include "common/styles.h"
#include "common/tags.h"
#include "common/undo.h"
#include "common/utility.h"
#include "common/color_picker.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "dtgtk/button.h"
#include "dtgtk/stylemenu.h"
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

DT_MODULE(1)

static void _update_softproof_gamut_checking(dt_develop_t *d);

/* signal handler for filmstrip image switching */
static void _view_darkroom_filmstrip_activate_callback(gpointer instance,
                                                       const dt_imgid_t imgid,
                                                       gpointer user_data);

static void _dev_change_image(dt_develop_t *dev, const dt_imgid_t imgid);

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
  dt_lua_image_t imgid = NO_IMGID;
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
  dt_develop_t *dev = malloc(sizeof(dt_develop_t));
  self->data = darktable.develop = dev;

  dt_dev_init(dev, TRUE);

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
  dt_develop_t *dev = self->data;

  // unref the grid lines popover if needed
  if(darktable.view_manager->guides_popover) g_object_unref(darktable.view_manager->guides_popover);

  if(dev->second_wnd)
  {
    if(gtk_widget_is_visible(dev->second_wnd))
    {
      dt_conf_set_bool("second_window/last_visible", TRUE);
      _darkroom_ui_second_window_write_config(dev->second_wnd);
    }
    else
      dt_conf_set_bool("second_window/last_visible", FALSE);

    gtk_widget_destroy(dev->second_wnd);
    dev->second_wnd = NULL;
    dev->preview2.widget = NULL;
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
  return DT_DARKROOM_LAYOUT_EDITING;
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


static void _darkroom_pickers_draw(dt_view_t *self,
                                   cairo_t *cri,
                                   const float wd,
                                   const float ht,
                                   const float zoom_scale,
                                   GSList *samples,
                                   const gboolean is_primary_sample)
{
  if(!samples) return;

  dt_develop_t *dev = self->data;

  cairo_save(cri);
  const double lw = 1.0 / zoom_scale;
  const double dashes[1] = { lw * 4.0 };

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

    double x = 0.0;
    double y = 0.0;
    // overlays are aligned with pixels for a clean look
    if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
    {
      dt_boundingbox_t fbox;
      dt_color_picker_transform_box(dev, 2, sample->box, fbox, FALSE);
      x = fbox[0];
      y = fbox[1];
      double w = fbox[2];
      double h = fbox[3];
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
      /* FIXME: to be really accurate, the colorpicker should render precisely over the nearest pixelpipe pixel
         but this gets particularly tricky to do with iop pickers with transformations after them in the pipeline
      */
      dt_boundingbox_t fbox;
      dt_color_picker_transform_box(dev, 1, sample->point, fbox, FALSE);
      x = fbox[0];
      y = fbox[1];
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
        cairo_arc(cri, x, y, half_px * 2., 0., 2. * M_PI);
      else if(show_preview_pixel_scale)
        cairo_rectangle(cri, x - half_px, y, half_px * 2., half_px * 2.);
      else
        cairo_arc(cri, x, y, half_px, 0., 2. * M_PI);

      set_color(cri, sample->swatch);
      cairo_fill(cri);
    }
  }

  cairo_restore(cri);
}

static inline gboolean _full_request(dt_develop_t *dev)
{
  return
        dev->full.pipe->status == DT_DEV_PIXELPIPE_DIRTY
     || dev->full.pipe->status == DT_DEV_PIXELPIPE_INVALID
     || dev->full.pipe->input_timestamp < dev->preview_pipe->input_timestamp;
}

static inline gboolean _preview_request(dt_develop_t *dev)
{
  return
        dev->preview_pipe->status == DT_DEV_PIXELPIPE_DIRTY
     || dev->preview_pipe->status == DT_DEV_PIXELPIPE_INVALID
     || dev->full.pipe->input_timestamp > dev->preview_pipe->input_timestamp;
}

static inline gboolean _preview2_request(dt_develop_t *dev)
{
  return
     (dev->preview2.pipe->status == DT_DEV_PIXELPIPE_DIRTY
       || dev->preview2.pipe->status == DT_DEV_PIXELPIPE_INVALID
       || dev->full.pipe->input_timestamp > dev->preview2.pipe->input_timestamp)
     && dev->gui_attached
     && dev->preview2.widget
     && GTK_IS_WIDGET(dev->preview2.widget);
}

static void _module_gui_post_expose(dt_iop_module_t *module,
                                    cairo_t *cri,
                                    float width, float height,
                                    float x, float y, float zoom_scale)
{
  if(!module || !module->gui_post_expose || width < 1.0f || height < 1.0f) return;

  cairo_save(cri);
  module->gui_post_expose(module, cri, width, height, x, y, zoom_scale);
  cairo_restore(cri);
}

static void _view_paint_surface(cairo_t *cr,
                                const size_t width,
                                const size_t height,
                                dt_dev_viewport_t *port,
                                const dt_window_t window)
{
  dt_dev_pixelpipe_t *p = port->pipe;

  dt_pthread_mutex_lock(&p->backbuf_mutex);

  dt_view_paint_surface(cr, width, height,
                        port, window,
                        p->backbuf, p->backbuf_scale,
                        p->backbuf_width, p->backbuf_height,
                        p->backbuf_zoom_x, p->backbuf_zoom_y);

  dt_pthread_mutex_unlock(&p->backbuf_mutex);
}

void expose(dt_view_t *self,
            cairo_t *cri,
            const int32_t width,
            const int32_t height,
            int32_t pointerx,
            int32_t pointery)
{
  cairo_set_source_rgb(cri, .2, .2, .2);

  dt_develop_t *dev = self->data;
  dt_dev_viewport_t *port = &dev->full;

  if(dev->gui_synch && !port->pipe->loading)
  {
    // synch module guis from gtk thread:
    ++darktable.gui->reset;
    for(const GList *modules = dev->iop; modules; modules = g_list_next(modules))
    {
      dt_iop_module_t *module = modules->data;
      dt_iop_gui_update(module);
    }
    --darktable.gui->reset;
    dev->gui_synch = FALSE;
  }

  float pzx = 0.0f, pzy = 0.0f, zoom_scale = 0.0f;
  dt_dev_get_pointer_zoom_pos(port, pointerx, pointery, &pzx, &pzy, &zoom_scale);

  // adjust scroll bars
  float zoom_x, zoom_y, boxw, boxh;
  if(!dt_dev_get_zoom_bounds(port, &zoom_x, &zoom_y, &boxw, &boxh))
    boxw = boxh = 1.0f;

  /* If boxw and boxh very closely match the zoomed size in the darktable window we might have resizing with
      every expose because adding a slider will change the image area and might force a resizing in next expose.
      So we disable in cases close to full.
  */
  if(boxw > 0.95f)
  {
    zoom_x = .0f;
    boxw = 1.01f;
  }
  if(boxh > 0.95f)
  {
    zoom_y = .0f;
    boxh = 1.01f;
  }

  dt_view_set_scrollbar(self, zoom_x, -0.5 + boxw/2, 0.5, boxw/2, zoom_y, -0.5+ boxh/2, 0.5, boxh/2);

  const gboolean expose_full =
        port->pipe->backbuf                                // do we have an image?
     && port->pipe->output_imgid == dev->image_storage.id; // same image?

  if(expose_full)
  {
    // draw image
    _view_paint_surface(cri, width, height, port, DT_WINDOW_MAIN);
  }
  else if(dev->preview_pipe->output_imgid != dev->image_storage.id)
  {
    gchar *load_txt;
    float fontsize;
    dt_image_t *img = dt_image_cache_get(darktable.image_cache, dev->image_storage.id, 'r');;
    dt_imageio_retval_t status = img->load_status;
    dt_image_cache_read_release(darktable.image_cache, img);

    if(dev->image_invalid_cnt)
    {
      fontsize = DT_PIXEL_APPLY_DPI(16);
      switch(status)
      {
      case DT_IMAGEIO_FILE_NOT_FOUND:
        load_txt = g_strdup_printf(
          _("file `%s' is not available, switching to lighttable now.\n\n"
            "if stored on an external drive, ensure that the drive is connected and files\n"
            "can be accessed in the same locations as when you imported this image."),
          dev->image_storage.filename);
        break;
      case DT_IMAGEIO_FILE_CORRUPTED:
        load_txt = g_strdup_printf(
          _("file `%s' appears corrupt, switching to lighttable now.\n\n"
            "please check that it was correctly and completely copied from the camera."),
          dev->image_storage.filename);
        break;
      case DT_IMAGEIO_UNSUPPORTED_FORMAT:
        load_txt = g_strdup_printf(
          _("file `%s' is not in any recognized format, switching to lighttable now."),
          dev->image_storage.filename);
        break;
      case DT_IMAGEIO_UNSUPPORTED_CAMERA:
        load_txt = g_strdup_printf(
          _("file `%s' is from an unsupported camera model, switching to lighttable now."),
          dev->image_storage.filename);
        break;
      case DT_IMAGEIO_UNSUPPORTED_FEATURE:
        load_txt = g_strdup_printf(
          _("file `%s' uses an unsupported feature, switching to lighttable now.\n\n"
            "please check that the image format and compression mode you selected in your\n"
            "camera's menus is supported (see https://www.darktable.org/resources/camera-support/\n"
            "and the release notes for this version of darktable)"),
          dev->image_storage.filename);
        break;
      case DT_IMAGEIO_IOERROR:
        load_txt = g_strdup_printf(
          _("error while reading file `%s', switching to lighttable now.\n\n"
            "please check that the file has not been truncated."),
          dev->image_storage.filename);
        break;
      default:
        load_txt = g_strdup_printf(
          _("darktable could not load `%s', switching to lighttable now.\n\n"
            "please check that the camera model that produced the image is supported in darktable\n"
            "(list of supported cameras is at https://www.darktable.org/resources/camera-support/).\n"
            "if you are sure that the camera model is supported, please consider opening an issue\n"
            "at https://github.com/darktable-org/darktable"),
          dev->image_storage.filename);
        break;
      }
      // if we already saw an error, retry a FEW more times with a bit of delay in between
      // it would be better if we could just put the delay after the first occurrence, but that
      // resulted in the error message not showing
      if(dev->image_invalid_cnt > 1)
      {
        g_usleep(1000000); // one second
        if(dev->image_invalid_cnt > 8)
        {
          dev->image_invalid_cnt = 0;
          dt_view_manager_switch(darktable.view_manager, "lighttable");
          g_free(load_txt);
          return;
        }
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
      dt_gui_gtk_set_source_rgb(cri, DT_GUI_COLOR_DARKROOM_BG);
      cairo_paint(cri);

      // waiting message
      PangoRectangle ink;
      PangoLayout *layout;
      PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
      pango_font_description_set_absolute_size(desc, fontsize * PANGO_SCALE);
      pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
      layout = pango_cairo_create_layout(cri);
      pango_layout_set_font_description(layout, desc);
      pango_layout_set_text(layout, load_txt, -1);
      pango_layout_get_pixel_extents(layout, &ink, NULL);
      const double xc = width / 2.0, yc = height * 0.88 - DT_PIXEL_APPLY_DPI(10), wd = ink.width * 0.5;
      cairo_move_to(cri, xc - wd, yc + 1.0 / 3.0 * fontsize - fontsize);
      pango_cairo_layout_path(cri, layout);
      cairo_set_line_width(cri, 2.0);
      dt_gui_gtk_set_source_rgb(cri, DT_GUI_COLOR_LOG_BG);
      cairo_stroke_preserve(cri);
      dt_gui_gtk_set_source_rgb(cri, DT_GUI_COLOR_LOG_FG);
      cairo_fill(cri);
      pango_font_description_free(desc);
      g_object_unref(layout);
    }
    else
    {
      dt_toast_log("%s", load_txt);
    }
    g_free(load_txt);
  }

  if(_full_request(dev)) dt_dev_process_image(dev);
  if(_preview_request(dev)) dt_dev_process_preview(dev);
  if(_preview2_request(dev)) dt_dev_process_preview2(dev);

  /* if we are in full preview mode, we don"t want anything else than the image */
  if(dev->full_preview)
    return;

  float wd, ht;
  if(!dt_dev_get_preview_size(dev, &wd, &ht)) return;

  const double tb = port->border_size;
  // account for border, make it transparent for other modules called below:

  cairo_save(cri);

  // don't draw guides and color pickers on image margins
  cairo_rectangle(cri, tb, tb, width - 2.0 * tb, height - 2.0 * tb);
  cairo_clip(cri);

  cairo_translate(cri, 0.5 * width, 0.5 * height);
  cairo_scale(cri, zoom_scale, zoom_scale);
  cairo_translate(cri, -.5f * wd - zoom_x * wd, -.5f * ht - zoom_y * ht);

  // Displaying sample areas if enabled
  if(darktable.lib->proxy.colorpicker.live_samples
     && (darktable.lib->proxy.colorpicker.display_samples
         || (darktable.lib->proxy.colorpicker.selected_sample &&
             darktable.lib->proxy.colorpicker.selected_sample != darktable.lib->proxy.colorpicker.primary_sample)))
  {
    dt_print_pipe(DT_DEBUG_EXPOSE,
        "expose livesamples FALSE",
         port->pipe, NULL, DT_DEVICE_NONE, NULL, NULL, "%dx%d, px=%d py=%d",
         width, height, pointerx, pointery);
    _darkroom_pickers_draw(self, cri, wd, ht, zoom_scale,
                           darktable.lib->proxy.colorpicker.live_samples, FALSE);
  }

  // draw colorpicker for in focus module or execute module callback hook
  // FIXME: draw picker in gui_post_expose() hook in
  // libs/colorpicker.c -- catch would be that live samples would
  // appear over guides, softproof/gamut text overlay would be hidden
  // by picker
  if(dt_iop_color_picker_is_visible(dev))
  {
    dt_print_pipe(DT_DEBUG_EXPOSE,
        "expose livesample TRUE",
         port->pipe, NULL, DT_DEVICE_NONE, NULL, NULL, "%dx%d, px=%d py=%d",
         width, height, pointerx, pointery);
    GSList samples = { .data = darktable.lib->proxy.colorpicker.primary_sample,
                       .next = NULL };
    _darkroom_pickers_draw(self, cri, wd, ht, zoom_scale, &samples, TRUE);
  }

  cairo_reset_clip(cri);

  dt_iop_module_t *dmod = dev->gui_module;

  // display mask if we have a current module activated or if the
  // masks manager module is expanded
  const gboolean display_masks = (dmod && dmod->enabled && dt_dev_modulegroups_test_activated(darktable.develop))
    || dt_lib_gui_get_expanded(dt_lib_get_module("masks"));

  if(dev->form_visible && display_masks)
  {
    dt_print_pipe(DT_DEBUG_EXPOSE,
        "expose masks",
         port->pipe, dev->gui_module, DT_DEVICE_NONE, NULL, NULL, "%dx%d, px=%d py=%d",
         width, height, pointerx, pointery);
    dt_masks_events_post_expose(dmod, cri, width, height, pzx, pzy, zoom_scale);
  }

  // if dragging the rotation line, do it and nothing else
  if(dev->proxy.rotate
     && (darktable.control->button_down_which == 3
         || dmod == dev->proxy.rotate))
  {
    // reminder, we want this to be exposed always for guidings
    _module_gui_post_expose(dev->proxy.rotate, cri, wd, ht, pzx, pzy, zoom_scale);
  }
  else
  {
    gboolean guides = TRUE;
    // true if anything could be exposed
    if(dmod && dmod != dev->proxy.rotate)
    {
      // the cropping.exposer->gui_post_expose needs special care
      if(expose_full
        && (dmod->operation_tags_filter() & IOP_TAG_CROPPING)
        && dev->cropping.exposer
        && (dmod->iop_order < dev->cropping.exposer->iop_order))
      {
        dt_print_pipe(DT_DEBUG_EXPOSE,
                      "expose cropper",
                      port->pipe, dev->cropping.exposer,
                      DT_DEVICE_NONE, NULL, NULL, "%dx%d, px=%d py=%d",
                      width, height, pointerx, pointery);
        _module_gui_post_expose(dev->cropping.exposer, cri, wd, ht, pzx, pzy, zoom_scale);
        guides = FALSE;
      }

      // gui active module
      if(dt_dev_modulegroups_test_activated(darktable.develop))
      {
        dt_print_pipe(DT_DEBUG_EXPOSE,
                      "expose module",
                      port->pipe, dmod,
                      DT_DEVICE_NONE, NULL, NULL,
                      "%dx%d, px=%d py=%d",
                      width, height, pointerx, pointery);
        _module_gui_post_expose(dmod, cri, wd, ht, pzx, pzy, zoom_scale);

        // avoid drawing later if we just did via post_expose
        if(dmod->flags() & IOP_FLAGS_GUIDES_SPECIAL_DRAW)
          guides = FALSE;
      }
    }
    if(guides)
      dt_guides_draw(cri, 0.0f, 0.0f, wd, ht, zoom_scale);
  }

  cairo_restore(cri);

  // indicate if we are in gamut check or softproof mode
  if(darktable.color_profiles->mode != DT_PROFILE_NORMAL)
  {
    gchar *label = darktable.color_profiles->mode == DT_PROFILE_GAMUTCHECK ? _("gamut check") : _("soft proof");
    dt_print_pipe(DT_DEBUG_EXPOSE,
        "expose profile",
         port->pipe, NULL, port->pipe->devid, NULL, NULL, "%dx%d, px=%d py=%d. proof: %s",
         width, height, pointerx, pointery, label);

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
  dt_dev_zoom_move(&darktable.develop->full, DT_ZOOM_FIT, 0.0f, 0, -1.0f, -1.0f, TRUE);
}

gboolean try_enter(dt_view_t *self)
{
  const dt_imgid_t imgid = dt_act_on_get_main_image();

  if(!dt_is_valid_imgid(imgid))
  {
    // fail :(
    dt_control_log(_("no image to open!"));
    return TRUE;
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
    return TRUE;
  }
  else if(img->load_status != DT_IMAGEIO_OK)
  {
    const char *reason;
    switch(img->load_status)
    {
    case DT_IMAGEIO_FILE_NOT_FOUND:
      reason = _("file not found");
      break;
    case DT_IMAGEIO_LOAD_FAILED:
    default:
      reason = _("unspecified failure");
      break;
    case DT_IMAGEIO_UNSUPPORTED_FORMAT:
      reason = _("unsupported file format");
      break;
    case DT_IMAGEIO_UNSUPPORTED_CAMERA:
      reason = _("unsupported camera model");
      break;
    case DT_IMAGEIO_UNSUPPORTED_FEATURE:
      reason = _("unsupported feature in file");
      break;
    case DT_IMAGEIO_FILE_CORRUPTED:
      reason = _("file appears corrupt");
      break;
    case DT_IMAGEIO_IOERROR:
      reason = _("I/O error");
      break;
    case DT_IMAGEIO_CACHE_FULL:
      reason = _("cache full");
      break;
    }
    dt_control_log(_("image `%s' could not be loaded\n%s"), img->filename, reason);
    dt_image_cache_read_release(darktable.image_cache, img);
    return TRUE;
  }
  // and drop the lock again.
  dt_image_cache_read_release(darktable.image_cache, img);
  darktable.develop->image_storage.id = imgid;

  dt_dev_reset_chroma(darktable.develop);

  // possible enable autosaving due to conf setting but wait for some seconds for first save
  darktable.develop->autosaving = (double)dt_conf_get_int("autosave_interval") > 1.0;
  darktable.develop->autosave_time = dt_get_wtime() + 10.0;
  return FALSE;
}

#ifdef USE_LUA

static void _fire_darkroom_image_loaded_event(const bool clean, const dt_imgid_t imgid)
{
  dt_lua_async_call_alien(dt_lua_event_trigger_wrapper,
      0, NULL, NULL,
      LUA_ASYNC_TYPENAME, "const char*", "darkroom-image-loaded",
      LUA_ASYNC_TYPENAME, "bool", clean,
      LUA_ASYNC_TYPENAME, "dt_lua_image_t", GINT_TO_POINTER(imgid),
      LUA_ASYNC_DONE);
}

#endif

static gboolean _dev_load_requested_image(gpointer user_data);

static void _dev_change_image(dt_develop_t *dev, const dt_imgid_t imgid)
{
  // Pipe reset needed when changing image
  // FIXME: synch with dev_init() and dev_cleanup() instead of redoing it

  // change active image
  g_slist_free(darktable.view_manager->active_images);
  darktable.view_manager->active_images = g_slist_prepend(NULL, GINT_TO_POINTER(imgid));
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_ACTIVE_IMAGES_CHANGE);

  // if the previous shown image is selected and the selection is unique
  // then we change the selected image to the new one
  if(dt_is_valid_imgid(dev->requested_id))
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
      if(sqlite3_column_int(stmt, 0) == dev->requested_id
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
     && dev->preview_pipe->status == DT_DEV_PIXELPIPE_VALID)
  {
    const double aspect_ratio =
      (double)dev->preview_pipe->backbuf_width / (double)dev->preview_pipe->backbuf_height;
    dt_image_set_aspect_ratio_to(dev->preview_pipe->image.id, aspect_ratio, TRUE);
  }
  else
  {
    dt_image_set_aspect_ratio(dev->image_storage.id, TRUE);
  }

  // prevent accels_window to refresh
  darktable.view_manager->accels_window.prevent_refresh = TRUE;

  // get current plugin in focus before defocus
  const dt_iop_module_t *gui_module = dt_dev_gui_module();
  if(gui_module)
  {
    dt_conf_set_string("plugins/darkroom/active",
                       gui_module->op);
  }

  // store last active group
  dt_conf_set_int("plugins/darkroom/groups", dt_dev_modulegroups_get(dev));

  // commit any pending changes in focused module
  dt_iop_request_focus(NULL);

  g_assert(dev->gui_attached);

  // commit image ops to db
  dt_dev_write_history(dev);

  dev->requested_id = imgid;
  dt_dev_clear_chroma_troubles(dev);

  // possible enable autosaving due to conf setting but wait for some seconds for first save
  darktable.develop->autosaving = (double)dt_conf_get_int("autosave_interval") > 1.0;
  darktable.develop->autosave_time = dt_get_wtime() + 10.0;

  g_idle_add(_dev_load_requested_image, dev);
}

static gboolean _dev_load_requested_image(gpointer user_data)
{
  dt_develop_t *dev = user_data;
  const dt_imgid_t imgid = dev->requested_id;

  if(dev->image_storage.id == NO_IMGID
     && dev->image_storage.id == imgid) return G_SOURCE_REMOVE;

  // make sure we can destroy and re-setup the pixel pipes.
  // we acquire the pipe locks, which will block the processing threads
  // in darkroom mode before they touch the pipes (init buffers etc).
  // we don't block here, since we hold the gdk lock, which will
  // result in circular locking when background threads emit signals
  // which in turn try to acquire the gdk lock.
  //
  // worst case, it'll drop some change image events. sorry.
  if(dt_pthread_mutex_BAD_trylock(&dev->preview_pipe->mutex))
  {

#ifdef USE_LUA

  _fire_darkroom_image_loaded_event(FALSE, imgid);

#endif
  return G_SOURCE_CONTINUE;
  }
  if(dt_pthread_mutex_BAD_trylock(&dev->full.pipe->mutex))
  {
    dt_pthread_mutex_BAD_unlock(&dev->preview_pipe->mutex);

 #ifdef USE_LUA

  _fire_darkroom_image_loaded_event(FALSE, imgid);

#endif

   return G_SOURCE_CONTINUE;
  }
  if(dt_pthread_mutex_BAD_trylock(&dev->preview2.pipe->mutex))
  {
    dt_pthread_mutex_BAD_unlock(&dev->full.pipe->mutex);
    dt_pthread_mutex_BAD_unlock(&dev->preview_pipe->mutex);

 #ifdef USE_LUA

  _fire_darkroom_image_loaded_event(FALSE, imgid);

#endif

   return G_SOURCE_CONTINUE;
  }

  const dt_imgid_t old_imgid = dev->image_storage.id;

  dt_overlay_add_from_history(old_imgid);

  // be sure light table will update the thumbnail
  if(!dt_history_hash_is_mipmap_synced(old_imgid))
  {
    dt_mipmap_cache_remove(darktable.mipmap_cache, old_imgid);
    dt_image_update_final_size(old_imgid);
    dt_image_synch_xmp(old_imgid);
    dt_history_hash_set_mipmap(old_imgid);
#ifdef USE_LUA
    dt_lua_async_call_alien(dt_lua_event_trigger_wrapper,
        0, NULL, NULL,
        LUA_ASYNC_TYPENAME, "const char*", "darkroom-image-history-changed",
        LUA_ASYNC_TYPENAME, "dt_lua_image_t", GINT_TO_POINTER(old_imgid),
        LUA_ASYNC_DONE);
#endif
  }

  // clean the undo list
  dt_undo_clear(darktable.undo, DT_UNDO_DEVELOP);

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
    dt_dev_history_item_t *hist = dev->history->data;
    dt_dev_free_history_item(hist);
    dev->history = g_list_delete_link(dev->history, dev->history);
  }

  // get new image:
  dt_dev_reload_image(dev, imgid);

  // make sure no signals propagate here:
  ++darktable.gui->reset;

  dt_pthread_mutex_lock(&dev->history_mutex);
  dt_dev_pixelpipe_cleanup_nodes(dev->full.pipe);
  dt_dev_pixelpipe_cleanup_nodes(dev->preview_pipe);
  dt_dev_pixelpipe_cleanup_nodes(dev->preview2.pipe);

  // chroma data will be fixed by reading whitebalance data from history
  dt_dev_reset_chroma(dev);

  const guint nb_iop = g_list_length(dev->iop);
  for(int i = nb_iop - 1; i >= 0; i--)
  {
    dt_iop_module_t *module = (g_list_nth_data(dev->iop, i));

    // the base module is the one with the lowest multi_priority
    int base_multi_priority = 0;
    for(const GList *l = dev->iop; l; l = g_list_next(l))
    {
      dt_iop_module_t *mod = l->data;
      if(dt_iop_module_is(module->so, mod->op))
        base_multi_priority = MIN(base_multi_priority, mod->multi_priority);
    }

    if(module->multi_priority == base_multi_priority) // if the module is the "base" instance, we keep it
    {
      module->iop_order = dt_ioppr_get_iop_order(dev->iop_order_list, module->op, module->multi_priority);
      module->multi_priority = 0;
      module->multi_name[0] = '\0';
      dt_iop_reload_defaults(module);
    }
    else // else we delete it and remove it from the panel
    {
      if(!dt_iop_is_hidden(module))
      {
        dt_iop_gui_cleanup_module(module);
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

  dt_dev_pixelpipe_create_nodes(dev->full.pipe, dev);
  dt_dev_pixelpipe_create_nodes(dev->preview_pipe, dev);
  if(dev->preview2.widget && GTK_IS_WIDGET(dev->preview2.widget))
    dt_dev_pixelpipe_create_nodes(dev->preview2.pipe, dev);
  dt_dev_read_history(dev);

  // we have to init all module instances other than "base" instance
  char option[1024];
  for(const GList *modules = g_list_last(dev->iop); modules; modules = g_list_previous(modules))
  {
    dt_iop_module_t *module = modules->data;
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
  dt_pthread_mutex_unlock(&dev->history_mutex);

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
  const char *active_plugin = dt_conf_get_string_const("plugins/darkroom/active");
  if(active_plugin)
  {
    gboolean valid = FALSE;
    for(const GList *modules = dev->iop; modules; modules = g_list_next(modules))
    {
      dt_iop_module_t *module = modules->data;
      if(dt_iop_module_is(module->so, active_plugin))
      {
        valid = TRUE;
        dt_iop_request_focus(module);
      }
    }
    if(!valid)
    {
      dt_conf_set_string("plugins/darkroom/active", "");
    }
  }

  // Signal develop initialize
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_IMAGE_CHANGED);

  // release pixel pipe mutices
  dt_pthread_mutex_BAD_unlock(&dev->preview2.pipe->mutex);
  dt_pthread_mutex_BAD_unlock(&dev->preview_pipe->mutex);
  dt_pthread_mutex_BAD_unlock(&dev->full.pipe->mutex);

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

  return G_SOURCE_REMOVE;
}

static void _view_darkroom_filmstrip_activate_callback(gpointer instance,
                                                       const dt_imgid_t imgid,
                                                       gpointer user_data)
{
  if(dt_is_valid_imgid(imgid))
  {
    // switch images in darkroom mode:
    const dt_view_t *self = (dt_view_t *)user_data;
    dt_develop_t *dev = self->data;

    _dev_change_image(dev, imgid);
    // move filmstrip
    dt_thumbtable_set_offset_image(dt_ui_thumbtable(darktable.gui->ui), imgid, TRUE);
    // force redraw
    dt_control_queue_redraw();
  }
}

static void dt_dev_jump_image(dt_develop_t *dev, int diff, gboolean by_key)
{

  const dt_imgid_t imgid = dev->requested_id;
  int new_offset = 1;
  dt_imgid_t new_id = NO_IMGID;

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

  if(!dt_is_valid_imgid(new_id) || new_id == imgid) return;

  // if id seems valid, we change the image and move filmstrip
  _dev_change_image(dev, new_id);
  dt_thumbtable_set_offset(dt_ui_thumbtable(darktable.gui->ui), new_offset, TRUE);

  // if it's a change by key_press, we set mouse_over to the active image
  if(by_key) dt_control_set_mouse_over_id(new_id);
}

static void zoom_key_accel(dt_action_t *action)
{
  // flip closeup/no closeup, no difference whether it was 1 or larger
  dt_dev_zoom_move(&darktable.develop->full, DT_ZOOM_1, 0.0f, -1, -1.0f, -1.0f, TRUE);
}

static void zoom_in_callback(dt_action_t *action)
{
  dt_view_t *self = dt_action_view(action);
  dt_develop_t *dev = self->data;

  scrolled(self, dev->full.width / 2, dev->full.height / 2, 1, GDK_CONTROL_MASK);
}

static void zoom_out_callback(dt_action_t *action)
{
  dt_view_t *self = dt_action_view(action);
  dt_develop_t *dev = self->data;

  scrolled(self, dev->full.width / 2, dev->full.height / 2, 0, GDK_CONTROL_MASK);
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

static void _darkroom_ui_preview2_pipe_finish_signal_callback(gpointer instance,
                                                              gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_develop_t *dev = self->data;
  if(dev->preview2.widget)
    gtk_widget_queue_draw(dev->preview2.widget);
}

static void _darkroom_ui_favorite_presets_popupmenu(GtkWidget *w, gpointer user_data)
{
  /* create favorites menu and popup */
  dt_gui_favorite_presets_menu_show(w);
}

static void _darkroom_ui_apply_style_activate_callback(GtkMenuItem *menuitem,
                                                       const dt_stylemenu_data_t *menu_data)
{
  GdkEvent *event = gtk_get_current_event();
  if(event->type == GDK_KEY_PRESS)
    dt_styles_apply_to_dev(menu_data->name, darktable.develop->image_storage.id);
  gdk_event_free(event);
}

static gboolean _darkroom_ui_apply_style_button_callback(GtkMenuItem *menuitem,
                                                         GdkEventButton *event,
                                                         const dt_stylemenu_data_t *menu_data)
{
  if(event->button == 1)
    dt_styles_apply_to_dev(menu_data->name, darktable.develop->image_storage.id);
  else
    dt_shortcut_copy_lua(NULL, menu_data->name);

  return FALSE;
}

static void _darkroom_ui_apply_style_popupmenu(GtkWidget *w, gpointer user_data)
{
  /* if we got any styles, lets popup menu for selection */
  GtkMenuShell *menu = dtgtk_build_style_menu_hierarchy(FALSE,
                                                        _darkroom_ui_apply_style_activate_callback,
                                                        _darkroom_ui_apply_style_button_callback,
                                                        user_data);
  if(menu)
  {
    dt_gui_menu_popup(GTK_MENU(menu), w, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST);
  }
  else
    dt_control_log(_("no styles have been created yet"));
}

static void _second_window_quickbutton_clicked(GtkWidget *w, dt_develop_t *dev)
{
  if(dev->second_wnd && !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)))
  {
    _darkroom_ui_second_window_write_config(dev->second_wnd);

    gtk_widget_destroy(dev->second_wnd);
    dev->second_wnd = NULL;
    dev->preview2.widget = NULL;
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

static void _full_iso12646_callback(GtkToggleButton *checkbutton, dt_develop_t *dev)
{
  dev->full.iso_12646 = gtk_toggle_button_get_active(checkbutton);
  dt_conf_set_bool("full_window/iso_12646", dev->full.iso_12646);
  dt_dev_configure(&dev->full);
}

static void _latescaling_quickbutton_clicked(GtkWidget *w, gpointer user_data)
{
  dt_develop_t *dev = (dt_develop_t *)user_data;
  if(!dev->gui_attached) return;

  dev->late_scaling.enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));

  // we just toggled off and had one of HQ pipelines running
  if(!dev->late_scaling.enabled
      && (dev->full.pipe->processing || (dev->second_wnd && dev->preview2.pipe->processing)))
  {
    if(dev->full.pipe->processing)
      dt_atomic_set_int(&dev->full.pipe->shutdown, TRUE);
    if(dev->second_wnd && dev->preview2.pipe->processing)
      dt_atomic_set_int(&dev->preview2.pipe->shutdown, TRUE);

    // do it the hard way for safety
    dt_dev_pixelpipe_rebuild(dev);
  }
  else
  {
    if(dev->second_wnd)
      dt_dev_reprocess_all(dev);
    else
      dt_dev_reprocess_center(dev);
  }
}

/* overlay color */
static void _guides_quickbutton_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_guides_button_toggled(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
  dt_control_queue_redraw_center();
}

static void _guides_view_changed(gpointer instance,
                                 dt_view_t *old_view,
                                 dt_view_t *new_view,
                                 dt_lib_module_t *self)
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

static void _colorscheme_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->overexposed.colorscheme = dt_bauhaus_combobox_get(combo);
  if(d->overexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->overexposed.button));
  else
    dt_dev_reprocess_center(d);
}

static void _lower_callback(GtkWidget *slider, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->overexposed.lower = dt_bauhaus_slider_get(slider);
  if(d->overexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->overexposed.button));
  else
    dt_dev_reprocess_center(d);
}

static void _upper_callback(GtkWidget *slider, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->overexposed.upper = dt_bauhaus_slider_get(slider);
  if(d->overexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->overexposed.button));
  else
    dt_dev_reprocess_center(d);
}

static void _mode_callback(GtkWidget *slider, gpointer user_data)
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

static void _rawoverexposed_mode_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->rawoverexposed.mode = dt_bauhaus_combobox_get(combo);
  if(d->rawoverexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->rawoverexposed.button));
  else
    dt_dev_reprocess_center(d);
}

static void _rawoverexposed_colorscheme_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->rawoverexposed.colorscheme = dt_bauhaus_combobox_get(combo);
  if(d->rawoverexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->rawoverexposed.button));
  else
    dt_dev_reprocess_center(d);
}

static void _rawoverexposed_threshold_callback(GtkWidget *slider, gpointer user_data)
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

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->profile.softproof_button),
                               darktable.color_profiles->mode == DT_PROFILE_SOFTPROOF);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->profile.gamut_button),
                               darktable.color_profiles->mode == DT_PROFILE_GAMUTCHECK);

  g_signal_handlers_unblock_by_func(d->profile.softproof_button, _softproof_quickbutton_clicked, d);
  g_signal_handlers_unblock_by_func(d->profile.gamut_button, _gamut_quickbutton_clicked, d);
}

static void _display_intent_callback(GtkWidget *combo, gpointer user_data)
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

static void _display2_intent_callback(GtkWidget *combo, gpointer user_data)
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

static void _softproof_profile_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  gboolean profile_changed = FALSE;
  const int pos = dt_bauhaus_combobox_get(combo);
  for(GList *profiles = darktable.color_profiles->profiles; profiles; profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *pp = profiles->data;
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
  dt_print(DT_DEBUG_ALWAYS,
           "can't find softproof profile `%s', using sRGB instead",
           dt_bauhaus_combobox_get_text(combo));
  profile_changed = darktable.color_profiles->softproof_type != DT_COLORSPACE_SRGB;
  darktable.color_profiles->softproof_type = DT_COLORSPACE_SRGB;
  darktable.color_profiles->softproof_filename[0] = '\0';

end:
  if(profile_changed)
  {
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED, DT_COLORSPACES_PROFILE_TYPE_SOFTPROOF);
    dt_dev_reprocess_all(d);
  }
}

static void _display_profile_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  gboolean profile_changed = FALSE;
  const int pos = dt_bauhaus_combobox_get(combo);
  for(GList *profiles = darktable.color_profiles->profiles; profiles; profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *pp = profiles->data;
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
  dt_print(DT_DEBUG_ALWAYS,
           "can't find display profile `%s', using system display profile instead",
           dt_bauhaus_combobox_get_text(combo));
  profile_changed = darktable.color_profiles->display_type != DT_COLORSPACE_DISPLAY;
  darktable.color_profiles->display_type = DT_COLORSPACE_DISPLAY;
  darktable.color_profiles->display_filename[0] = '\0';

end:
  if(profile_changed)
  {
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);
    dt_colorspaces_update_display_transforms();
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED, DT_COLORSPACES_PROFILE_TYPE_DISPLAY);
    dt_dev_reprocess_all(d);
  }
}

static void _display2_profile_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  gboolean profile_changed = FALSE;
  const int pos = dt_bauhaus_combobox_get(combo);
  for(GList *profiles = darktable.color_profiles->profiles; profiles; profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *pp = profiles->data;
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
  dt_print(DT_DEBUG_ALWAYS,
           "can't find preview display profile `%s', using system display profile instead",
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
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                            DT_COLORSPACES_PROFILE_TYPE_DISPLAY2);
    dt_dev_reprocess_all(d);
  }
}

static void _display2_iso12646_callback(GtkToggleButton *checkbutton, dt_develop_t *dev)
{
  dev->preview2.iso_12646 = gtk_toggle_button_get_active(checkbutton);
  dt_conf_set_bool("second_window/iso_12646", dev->preview2.iso_12646);
  dt_dev_configure(&dev->preview2);
}

static void _histogram_profile_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  gboolean profile_changed = FALSE;
  const int pos = dt_bauhaus_combobox_get(combo);
  for(GList *profiles = darktable.color_profiles->profiles; profiles; profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *pp = profiles->data;
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
  dt_print(DT_DEBUG_ALWAYS,
           "can't find histogram profile `%s', using export profile instead",
           dt_bauhaus_combobox_get_text(combo));
  profile_changed = darktable.color_profiles->histogram_type != DT_COLORSPACE_WORK;
  darktable.color_profiles->histogram_type = DT_COLORSPACE_WORK;
  darktable.color_profiles->histogram_filename[0] = '\0';

end:
  if(profile_changed)
  {
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED, DT_COLORSPACES_PROFILE_TYPE_HISTOGRAM);
    dt_dev_reprocess_all(d);
  }
}

static void _preference_changed(gpointer instance, gpointer user_data)
{
  GtkWidget *display_intent = GTK_WIDGET(user_data);

  const int force_lcms2 = dt_conf_get_bool("plugins/lighttable/export/force_lcms2");

  gtk_widget_set_no_show_all(display_intent, !force_lcms2);
  gtk_widget_set_visible(display_intent, force_lcms2);

  dt_get_sysresource_level();
  dt_opencl_update_settings();
  dt_configure_ppd_dpi(darktable.gui);
}

static void _preference_changed_button_hide(gpointer instance, dt_develop_t *dev)
{
  for(const GList *modules = dev->iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *module = modules->data;

    if(module->header)
      dt_iop_add_remove_mask_indicator(module, (module->blend_params->mask_mode != DEVELOP_MASK_DISABLED) &&
                                               (module->blend_params->mask_mode != DEVELOP_MASK_ENABLED));
  }
}

static void _update_display_profile_cmb(GtkWidget *cmb_display_profile)
{
  for(const GList *l = darktable.color_profiles->profiles; l; l = g_list_next(l))
  {
    dt_colorspaces_color_profile_t *prof = l->data;
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
    dt_colorspaces_color_profile_t *prof = l->data;
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

static void _display_profile_changed(gpointer instance,
                                     const uint8_t profile_type,
                                     gpointer user_data)
{
  GtkWidget *cmb_display_profile = GTK_WIDGET(user_data);

  _update_display_profile_cmb(cmb_display_profile);
}

static void _display2_profile_changed(gpointer instance,
                                      const uint8_t profile_type,
                                      gpointer user_data)
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

  //retouch and spot removal module use masks differently and have
  //different buttons associated keep the shortcuts independent
  if(mod
     && !dt_iop_module_is(mod->so, "spots")
     && !dt_iop_module_is(mod->so, "retouch"))
  {
    dt_iop_gui_blend_data_t *bd = mod->blend_data;

    ++darktable.gui->reset;

    dt_iop_color_picker_reset(mod, TRUE);

    dt_masks_form_t *grp =
      dt_masks_get_from_id(darktable.develop, mod->blend_params->mask_id);
    if(grp && (grp->type & DT_MASKS_GROUP) && grp->points)
    {
      if(bd->masks_shown == DT_MASKS_EDIT_OFF)
        bd->masks_shown = DT_MASKS_EDIT_FULL;
      else
        bd->masks_shown = DT_MASKS_EDIT_OFF;

      gtk_toggle_button_set_active
        (GTK_TOGGLE_BUTTON(bd->masks_edit), bd->masks_shown != DT_MASKS_EDIT_OFF);
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

static void _darkroom_do_synchronize_selection_callback(dt_action_t *action)
{
  dt_gui_cursor_set_busy();

  GList *sel = dt_selection_get_list(darktable.selection, FALSE, FALSE);

  // write histroy for edited picture
  dt_dev_write_history(darktable.develop);

  const dt_imgid_t imgid = darktable.develop->image_storage.id;

  // get first item in list, last edited iop
  GList *hist = dt_history_get_items(imgid, FALSE, FALSE, FALSE);
  dt_history_item_t *first_item = (dt_history_item_t *)g_list_first(hist)->data;

  // the iop num in the history
  GList *op = g_list_append(NULL, GINT_TO_POINTER(first_item->num));

  g_list_free_full(hist, g_free);

  // group all changes for atomic undo/redo
  dt_undo_start_group(darktable.undo, DT_UNDO_HISTORY);

  // copy history item into the all selected items
  for(GList *l = sel;
      l;
      l = g_list_next(l))
  {
    // target picture
    const dt_imgid_t dest_imgid = GPOINTER_TO_INT(l->data);
    if(dest_imgid != imgid)
    {
      dt_history_copy_and_paste_on_image(imgid,
                                         dest_imgid,
                                         TRUE,
                                         op,
                                         TRUE,
                                         FALSE,
                                         TRUE);
    }
  }

  dt_undo_end_group(darktable.undo);

  g_list_free(op);
  g_list_free(sel);

  dt_gui_cursor_clear_busy();
}

static void _change_slider_accel_precision(dt_action_t *action);

static float _action_process_skip_mouse(gpointer target,
                                        const dt_action_element_t element,
                                        const dt_action_effect_t effect,
                                        const float move_size)
{
  if(DT_PERFORM_ACTION(move_size))
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

    // don't turn on if drag underway; would not receive button_released
    if(darktable.control->button_down)
      darktable.develop->darkroom_skip_mouse_events = FALSE;
  }

  return darktable.develop->darkroom_skip_mouse_events;
}

const dt_action_def_t dt_action_def_skip_mouse
  = { N_("hold"),
      _action_process_skip_mouse,
      dt_action_elements_hold,
      NULL, TRUE };

static float _action_process_preview(gpointer target,
                                     const dt_action_element_t element,
                                     const dt_action_effect_t effect,
                                     const float move_size)
{
  dt_develop_t *lib = darktable.view_manager->proxy.darkroom.view->data;

  if(DT_PERFORM_ACTION(move_size))
  {
    if(lib->full_preview)
    {
      if(effect != DT_ACTION_EFFECT_ON)
      {
        dt_ui_restore_panels(darktable.gui->ui);
        // restore previously stored zoom settings
        dt_dev_zoom_move(&darktable.develop->full, DT_ZOOM_RESTORE, 0.0f, 0, -1.0f, -1.0f, TRUE);
        lib->full_preview = FALSE;
        dt_iop_request_focus(lib->full_preview_last_module);
        dt_masks_set_edit_mode(dt_dev_gui_module(), lib->full_preview_masks_state);
        dt_dev_invalidate(darktable.develop);
        dt_control_queue_redraw_center();
        dt_control_navigation_redraw();
      }
    }
    else
    {
      if(effect != DT_ACTION_EFFECT_OFF &&
         lib->preview_pipe->status != DT_DEV_PIXELPIPE_DIRTY &&
         lib->preview_pipe->status != DT_DEV_PIXELPIPE_INVALID)
      {
        lib->full_preview = TRUE;
        // we hide all panels
        for(int k = 0; k < DT_UI_PANEL_SIZE; k++)
          dt_ui_panel_show(darktable.gui->ui, k, FALSE, FALSE);
        // we remember the masks edit state
        dt_iop_module_t *gui_module = dt_dev_gui_module();
        if(gui_module)
        {
          dt_iop_gui_blend_data_t *bd = gui_module->blend_data;
          if(bd) lib->full_preview_masks_state = bd->masks_shown;
        }
        // we set the zoom values to "fit" after storing previous settings
        dt_dev_zoom_move(&darktable.develop->full, DT_ZOOM_FULL_PREVIEW, 0.0f, 0, -1.0f, -1.0f, TRUE);
        // we quit the active iop if any
        lib->full_preview_last_module = gui_module;
        dt_iop_request_focus(NULL);
        gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));
        dt_dev_invalidate(darktable.develop);
        dt_control_queue_redraw_center();
      }
    }
  }

  return (float)lib->full_preview;
}

const dt_action_def_t dt_action_def_preview
  = { N_("preview"),
      _action_process_preview,
      dt_action_elements_hold,
      NULL, TRUE };

static float _action_process_move(gpointer target,
                                  const dt_action_element_t element,
                                  const dt_action_effect_t effect,
                                  float move_size)
{
  dt_develop_t *dev = darktable.view_manager->proxy.darkroom.view->data;

  if(DT_PERFORM_ACTION(move_size))
  {
    // For each cursor press, move fifth of screen by default
    float factor = 0.2f * move_size;
    if(effect == DT_ACTION_EFFECT_DOWN) factor *= -1;

    dt_dev_zoom_move(&dev->full, DT_ZOOM_MOVE, factor, 0,
                     target ? dev->full.width : 0,
                     target ? 0 : - dev->full.height, TRUE);
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

static gboolean _quickbutton_press_release(GtkWidget *button,
                                           GdkEventButton *event,
                                           GtkWidget *popover)
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
  dt_develop_t *dev = self->data;

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
  dt_gui_add_help_link(favorite_presets, "favorite_presets");
  dt_view_manager_view_toolbox_add(darktable.view_manager, favorite_presets, DT_VIEW_DARKROOM);

  /* create quick styles popup menu tool */
  GtkWidget *styles = dtgtk_button_new(dtgtk_cairo_paint_styles, 0, NULL);
  dt_action_define(sa, NULL, N_("quick access to styles"), styles, &dt_action_def_button);
  g_signal_connect(G_OBJECT(styles), "clicked", G_CALLBACK(_darkroom_ui_apply_style_popupmenu), NULL);
  gtk_widget_set_tooltip_text(styles, _("quick access for applying any of your styles"));
  dt_gui_add_help_link(styles, "bottom_panel_styles");
  dt_view_manager_view_toolbox_add(darktable.view_manager, styles, DT_VIEW_DARKROOM);
  /* ensure that we get strings from the style files shipped with darktable localized */
  (void)_("darktable camera styles");
  (void)_("generic");

  /* create second window display button */
  dev->second_wnd_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_display2, 0, NULL);
  dt_action_define(sa, NULL, N_("second window"), dev->second_wnd_button, &dt_action_def_toggle);
  g_signal_connect(G_OBJECT(dev->second_wnd_button), "clicked", G_CALLBACK(_second_window_quickbutton_clicked),dev);
  gtk_widget_set_tooltip_text(dev->second_wnd_button, _("display a second darkroom image window"));
  dt_view_manager_view_toolbox_add(darktable.view_manager, dev->second_wnd_button, DT_VIEW_DARKROOM);

  /* Enable ISO 12646-compliant colour assessment conditions */
  GtkWidget *full_iso12646 = dtgtk_togglebutton_new(dtgtk_cairo_paint_bulb, 0, NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(full_iso12646), dev->full.iso_12646);
  ac = dt_action_define(DT_ACTION(self), NULL, N_("color assessment"), full_iso12646, &dt_action_def_toggle);
  gtk_widget_set_tooltip_text(full_iso12646,
                              _("toggle ISO 12646 color assessment conditions"));
  dt_shortcut_register(ac, 0, 0, GDK_KEY_b, GDK_CONTROL_MASK);
  g_signal_connect(G_OBJECT(full_iso12646), "toggled", G_CALLBACK(_full_iso12646_callback), dev);

  dt_view_manager_module_toolbox_add(darktable.view_manager, full_iso12646, DT_VIEW_DARKROOM);

  /* Enable late-scaling button */
  dev->late_scaling.button =
    dtgtk_togglebutton_new(dtgtk_cairo_paint_lt_mode_fullpreview, 0, NULL);
  ac = dt_action_define(sa, NULL, N_("high quality processing"),
                        dev->late_scaling.button, &dt_action_def_toggle);
  gtk_widget_set_tooltip_text
    (dev->late_scaling.button,
     _("toggle high quality processing."
       " if activated darktable processes image data as it does while exporting"));
  g_signal_connect(G_OBJECT(dev->late_scaling.button), "clicked",
                   G_CALLBACK(_latescaling_quickbutton_clicked), dev);
  dt_view_manager_module_toolbox_add(darktable.view_manager,
                                     dev->late_scaling.button, DT_VIEW_DARKROOM);

  GtkWidget *colorscheme, *mode;

  /* create rawoverexposed popup tool */
  {
    // the button
    dev->rawoverexposed.button = dtgtk_togglebutton_new(dtgtk_cairo_paint_rawoverexposed, 0, NULL);
    ac = dt_action_define(sa, N_("raw overexposed"), N_("toggle"), dev->rawoverexposed.button, &dt_action_def_toggle);
    dt_shortcut_register(ac, 0, 0, GDK_KEY_o, GDK_SHIFT_MASK);
    gtk_widget_set_tooltip_text(dev->rawoverexposed.button,
                                _("toggle indication of raw overexposure\nright-click for options"));
    g_signal_connect(G_OBJECT(dev->rawoverexposed.button), "clicked",
                     G_CALLBACK(_rawoverexposed_quickbutton_clicked), dev);
    dt_view_manager_module_toolbox_add(darktable.view_manager, dev->rawoverexposed.button, DT_VIEW_DARKROOM);
    dt_gui_add_help_link(dev->rawoverexposed.button, "rawoverexposed");

    // and the popup window
    dev->rawoverexposed.floating_window = gtk_popover_new(dev->rawoverexposed.button);
    connect_button_press_release(dev->rawoverexposed.button, dev->rawoverexposed.floating_window);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(dev->rawoverexposed.floating_window), vbox);

    /** let's fill the encapsulating widgets */
    /* mode of operation */
    DT_BAUHAUS_COMBOBOX_NEW_FULL(mode, self, N_("raw overexposed"), N_("mode"),
                                 _("select how to mark the clipped pixels"),
                                 dev->rawoverexposed.mode, _rawoverexposed_mode_callback, dev,
                                 N_("mark with CFA color"), N_("mark with solid color"), N_("false color"));
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(mode), TRUE, TRUE, 0);

    DT_BAUHAUS_COMBOBOX_NEW_FULL(colorscheme, self, N_("raw overexposed"), N_("color scheme"),
                                _("select the solid color to indicate overexposure.\nwill only be used if mode = mark with solid color"),
                                dev->rawoverexposed.colorscheme,
                                _rawoverexposed_colorscheme_callback, dev,
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
    g_signal_connect(G_OBJECT(threshold), "value-changed", G_CALLBACK(_rawoverexposed_threshold_callback), dev);
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
                                _("toggle clipping indication\nright-click for options"));
    g_signal_connect(G_OBJECT(dev->overexposed.button), "clicked",
                     G_CALLBACK(_overexposed_quickbutton_clicked), dev);
    dt_view_manager_module_toolbox_add(darktable.view_manager, dev->overexposed.button, DT_VIEW_DARKROOM);
    dt_gui_add_help_link(dev->overexposed.button, "overexposed");

    // and the popup window
    dev->overexposed.floating_window = gtk_popover_new(dev->overexposed.button);
    connect_button_press_release(dev->overexposed.button, dev->overexposed.floating_window);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(dev->overexposed.floating_window), vbox);

    /** let's fill the encapsulating widgets */
    /* preview mode */
    DT_BAUHAUS_COMBOBOX_NEW_FULL(mode, self, N_("overexposed"), N_("clipping preview mode"),
                                 _("select the metric you want to preview\nfull gamut is the combination of all other modes"),
                                 dev->overexposed.mode, _mode_callback, dev,
                                 N_("full gamut"), N_("any RGB channel"), N_("luminance only"), N_("saturation only"));
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(mode), TRUE, TRUE, 0);

    /* color scheme */
    DT_BAUHAUS_COMBOBOX_NEW_FULL(colorscheme, self, N_("overexposed"), N_("color scheme"),
                                 _("select colors to indicate clipping"),
                                 dev->overexposed.colorscheme, _colorscheme_callback, dev,
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
    g_signal_connect(G_OBJECT(lower), "value-changed", G_CALLBACK(_lower_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(lower), TRUE, TRUE, 0);

    /* upper */
    GtkWidget *upper = dt_bauhaus_slider_new_action(DT_ACTION(self), 0.0, 100.0, 0.1, 99.99, 2);
    dt_bauhaus_slider_set(upper, dev->overexposed.upper);
    dt_bauhaus_slider_set_format(upper, "%");
    dt_bauhaus_widget_set_label(upper, N_("overexposed"), N_("upper threshold"));
    /* xgettext:no-c-format */
    gtk_widget_set_tooltip_text(upper, _("clipping threshold for the white point.\n"
                                         "100% is peak medium luminance."));
    g_signal_connect(G_OBJECT(upper), "value-changed", G_CALLBACK(_upper_callback), dev);
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
                                _("toggle softproofing\nright-click for profile options"));
    g_signal_connect(G_OBJECT(dev->profile.softproof_button), "clicked",
                     G_CALLBACK(_softproof_quickbutton_clicked), dev);
    dt_view_manager_module_toolbox_add(darktable.view_manager, dev->profile.softproof_button, DT_VIEW_DARKROOM);
    dt_gui_add_help_link(dev->profile.softproof_button, "softproof");

    // the gamut check button
    dev->profile.gamut_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_gamut_check, 0, NULL);
    ac = dt_action_define(sa, NULL, N_("gamut check"), dev->profile.gamut_button, &dt_action_def_toggle);
    dt_shortcut_register(ac, 0, 0, GDK_KEY_g, GDK_CONTROL_MASK);
    gtk_widget_set_tooltip_text(dev->profile.gamut_button,
                 _("toggle gamut checking\nright-click for profile options"));
    g_signal_connect(G_OBJECT(dev->profile.gamut_button), "clicked",
                     G_CALLBACK(_gamut_quickbutton_clicked), dev);
    dt_view_manager_module_toolbox_add(darktable.view_manager, dev->profile.gamut_button, DT_VIEW_DARKROOM);
    dt_gui_add_help_link(dev->profile.gamut_button, "gamut");

    // and the popup window, which is shared between the two profile buttons
    dev->profile.floating_window = gtk_popover_new(NULL);
    connect_button_press_release(dev->second_wnd_button, dev->profile.floating_window);
    connect_button_press_release(dev->profile.softproof_button, dev->profile.floating_window);
    connect_button_press_release(dev->profile.gamut_button, dev->profile.floating_window);
    // randomly connect to one of the buttons, so widgets can be realized
    gtk_popover_set_relative_to(GTK_POPOVER(dev->profile.floating_window), dev->second_wnd_button);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(dev->profile.floating_window), vbox);

    /** let's fill the encapsulating widgets */
    const int force_lcms2 = dt_conf_get_bool("plugins/lighttable/export/force_lcms2");

    static const gchar *intents_list[]
      = { N_("perceptual"),
          N_("relative colorimetric"),
          NC_("rendering intent", "saturation"),
          N_("absolute colorimetric"),
          NULL };

    GtkWidget *display_intent = dt_bauhaus_combobox_new_full(DT_ACTION(self), N_("profiles"), N_("intent"),
                                                             "", 0, _display_intent_callback, dev, intents_list);
    GtkWidget *display2_intent = dt_bauhaus_combobox_new_full(DT_ACTION(self), N_("profiles"), N_("preview intent"),
                                                              "", 0, _display2_intent_callback, dev, intents_list);

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

    GtkWidget *display2_iso12646 = gtk_check_button_new_with_label(_("second preview window ISO 12646 color assessment"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(display2_iso12646), dev->preview2.iso_12646);
    ac = dt_action_define(DT_ACTION(self), NULL, N_("color assessment second preview"), display2_iso12646, &dt_action_def_toggle);
    dt_shortcut_register(ac, 0, 0, GDK_KEY_b, GDK_MOD1_MASK);

    gtk_box_pack_start(GTK_BOX(vbox), display_profile, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), display_intent, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), display2_profile, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), display2_intent, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), display2_iso12646, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), softproof_profile, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), histogram_profile, TRUE, TRUE, 0);

    for(const GList *l = darktable.color_profiles->profiles; l; l = g_list_next(l))
    {
      dt_colorspaces_color_profile_t *prof = l->data;
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

    char *tooltip = dt_ioppr_get_location_tooltip("out", _("display ICC profiles"));
    gtk_widget_set_tooltip_markup(display_profile, tooltip);
    g_free(tooltip);

    tooltip = dt_ioppr_get_location_tooltip("out", _("preview display ICC profiles"));
    gtk_widget_set_tooltip_markup(display2_profile, tooltip);
    g_free(tooltip);

    tooltip = dt_ioppr_get_location_tooltip("out", _("softproof ICC profiles"));
    gtk_widget_set_tooltip_markup(softproof_profile, tooltip);
    g_free(tooltip);

    tooltip = dt_ioppr_get_location_tooltip("out", _("histogram and color picker ICC profiles"));
    gtk_widget_set_tooltip_markup(histogram_profile, tooltip);
    g_free(tooltip);

    g_signal_connect(G_OBJECT(display_profile), "value-changed", G_CALLBACK(_display_profile_callback), dev);
    g_signal_connect(G_OBJECT(display2_profile), "value-changed", G_CALLBACK(_display2_profile_callback), dev);
    g_signal_connect(G_OBJECT(display2_iso12646), "toggled", G_CALLBACK(_display2_iso12646_callback), dev);
    g_signal_connect(G_OBJECT(softproof_profile), "value-changed", G_CALLBACK(_softproof_profile_callback), dev);
    g_signal_connect(G_OBJECT(histogram_profile), "value-changed", G_CALLBACK(_histogram_profile_callback), dev);

    _update_softproof_gamut_checking(dev);

    // update the gui when the preferences changed (i.e. show intent when using lcms2)
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_PREFERENCES_CHANGE, _preference_changed, display_intent);
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_PREFERENCES_CHANGE, _preference_changed, display2_intent);
    // and when profiles change
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED, _display_profile_changed, display_profile);
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED, _display2_profile_changed, display2_profile);

    gtk_widget_show_all(vbox);
  }

  /* create grid changer popup tool */
  {
    // the button
    darktable.view_manager->guides_toggle = dtgtk_togglebutton_new(dtgtk_cairo_paint_grid, 0, NULL);
    ac = dt_action_define(sa, N_("guide lines"), N_("toggle"), darktable.view_manager->guides_toggle, &dt_action_def_toggle);
    dt_shortcut_register(ac, 0, 0, GDK_KEY_g, 0);
    gtk_widget_set_tooltip_text(darktable.view_manager->guides_toggle,
                                _("toggle guide lines\nright-click for guides options"));
    darktable.view_manager->guides_popover = dt_guides_popover(self, darktable.view_manager->guides_toggle);
    g_object_ref(darktable.view_manager->guides_popover);
    g_signal_connect(G_OBJECT(darktable.view_manager->guides_toggle), "clicked",
                     G_CALLBACK(_guides_quickbutton_clicked), dev);
    connect_button_press_release(darktable.view_manager->guides_toggle, darktable.view_manager->guides_popover);
    dt_view_manager_module_toolbox_add(darktable.view_manager, darktable.view_manager->guides_toggle,
                                       DT_VIEW_DARKROOM | DT_VIEW_TETHERING);
    // we want to update button state each time the view change
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED, _guides_view_changed, dev);
  }

  darktable.view_manager->proxy.darkroom.get_layout = _lib_darkroom_get_layout;
  dev->full.border_size = DT_PIXEL_APPLY_DPI(dt_conf_get_int("plugins/darkroom/ui/border_size"));

  // Fullscreen preview key
  ac = dt_action_define(sa, NULL, N_("full preview"), NULL, &dt_action_def_preview);
  dt_shortcut_register(ac, 0, DT_ACTION_EFFECT_HOLD, GDK_KEY_w, 0);

  // add an option to allow skip mouse events while other overlays are consuming mouse actions
  ac = dt_action_define(sa, NULL, N_("force pan/zoom/rotate with mouse"), NULL, &dt_action_def_skip_mouse);
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
  dt_action_register(DT_ACTION(self), N_("change keyboard shortcut slider precision"), _change_slider_accel_precision, 0, 0);

  dt_action_register(DT_ACTION(self), N_("synchronize selection"),
                     _darkroom_do_synchronize_selection_callback,
                     GDK_KEY_x, GDK_CONTROL_MASK);
}

void enter(dt_view_t *self)
{
  // prevent accels_window to refresh
  darktable.view_manager->accels_window.prevent_refresh = TRUE;

  // clean the undo list
  dt_undo_clear(darktable.undo, DT_UNDO_DEVELOP);

  /* connect to ui pipe finished signal for redraw */
  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED, _darkroom_ui_pipe_finish_signal_callback, self);
  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_DEVELOP_PREVIEW2_PIPE_FINISHED, _darkroom_ui_preview2_pipe_finish_signal_callback, self);
  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_TROUBLE_MESSAGE, _display_module_trouble_message_callback, self);

  dt_print(DT_DEBUG_CONTROL, "[run_job+] 11 %f in darkroom mode", dt_get_wtime());
  dt_develop_t *dev = self->data;
  if(!dev->form_gui)
  {
    dev->form_gui = (dt_masks_form_gui_t *)calloc(1, sizeof(dt_masks_form_gui_t));
    dt_masks_init_form_gui(dev->form_gui);
  }
  dt_masks_change_form_gui(NULL);
  dev->form_gui->pipe_hash = 0;
  dev->form_gui->formid = NO_MASKID;
  dev->gui_leaving = FALSE;
  dev->gui_module = NULL;

  // change active image
  dt_view_active_images_reset(FALSE);
  dt_view_active_images_add(dev->image_storage.id, TRUE);
  dt_ui_thumbtable(darktable.gui->ui)->mouse_inside = FALSE; // consider mouse outside filmstrip by default

  dt_dev_zoom_move(&dev->full, DT_ZOOM_FIT, 0.0f, 0, -1.0f, -1.0f, TRUE);

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
    dt_iop_module_t *module = modules->data;

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
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_INITIALIZE);
  /* signal that there is a new image to be developed */
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_IMAGE_CHANGED);

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
      dt_iop_module_t *module = modules->data;
      if(dt_iop_module_is(module->so, active_plugin))
        dt_iop_request_focus(module);
    }
  }

  // image should be there now.
  dt_dev_zoom_move(&dev->full, DT_ZOOM_MOVE, -1.f, TRUE, 0.0f, 0.0f, TRUE);

  /* connect signal for filmstrip image activate */
  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE,
                            _view_darkroom_filmstrip_activate_callback, self);
  dt_collection_hint_message(darktable.collection);

  dt_ui_scrollbars_show(darktable.gui->ui, dt_conf_get_bool("darkroom/ui/scrollbars"));

  if(dt_conf_get_bool("second_window/last_visible"))
  {
    _darkroom_display_second_window(dev);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dev->second_wnd_button), TRUE);
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
  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_PREFERENCES_CHANGE, _preference_changed_button_hide, dev);
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

  /* disconnect from filmstrip image activate */
  DT_CONTROL_SIGNAL_DISCONNECT(_view_darkroom_filmstrip_activate_callback, self);
  /* disconnect from pipe finish signal */
  DT_CONTROL_SIGNAL_DISCONNECT(_darkroom_ui_pipe_finish_signal_callback, self);
  DT_CONTROL_SIGNAL_DISCONNECT(_darkroom_ui_preview2_pipe_finish_signal_callback, self);
  DT_CONTROL_SIGNAL_DISCONNECT(_display_module_trouble_message_callback, self);

  // store groups for next time:
  dt_conf_set_int("plugins/darkroom/groups", dt_dev_modulegroups_get(darktable.develop));

  // store last active plugin:
  if(darktable.develop->gui_module)
    dt_conf_set_string("plugins/darkroom/active", darktable.develop->gui_module->op);
  else
    dt_conf_set_string("plugins/darkroom/active", "");

  dt_develop_t *dev = self->data;

  DT_CONTROL_SIGNAL_DISCONNECT(_preference_changed_button_hide, dev);

  // reset color assessment mode
  if(dev->full.iso_12646)
  {
    dev->full.width = dev->full.orig_width;
    dev->full.height = dev->full.orig_height;
    dev->preview2.width = dev->preview2.orig_width;
    dev->preview2.height = dev->preview2.orig_height;
    dev->full.border_size = DT_PIXEL_APPLY_DPI(dt_conf_get_int("plugins/darkroom/ui/border_size"));
  }

  // commit image ops to db
  dt_dev_write_history(dev);

  const dt_imgid_t imgid = dev->image_storage.id;

  dt_overlay_add_from_history(imgid);

  // update aspect ratio
  if(dev->preview_pipe->backbuf && dev->preview_pipe->status == DT_DEV_PIXELPIPE_VALID)
  {
    double aspect_ratio = (double)dev->preview_pipe->backbuf_width / (double)dev->preview_pipe->backbuf_height;
    dt_image_set_aspect_ratio_to(dev->preview_pipe->image.id, aspect_ratio, FALSE);
  }
  else
  {
    dt_image_set_aspect_ratio(imgid, FALSE);
  }

  // be sure light table will regenerate the thumbnail:
  if(!dt_history_hash_is_mipmap_synced(imgid))
  {
    dt_mipmap_cache_remove(darktable.mipmap_cache, imgid);
    dt_image_update_final_size(imgid);
    dt_image_synch_xmp(imgid);
    dt_history_hash_set_mipmap(imgid);
#ifdef USE_LUA
    dt_lua_async_call_alien(dt_lua_event_trigger_wrapper,
        0, NULL, NULL,
        LUA_ASYNC_TYPENAME, "const char*", "darkroom-image-history-changed",
        LUA_ASYNC_TYPENAME, "dt_lua_image_t", GINT_TO_POINTER(imgid),
        LUA_ASYNC_DONE);
#endif
  }
  else
    dt_image_synch_xmp(imgid);


  // clear gui.

  dt_pthread_mutex_lock(&dev->preview_pipe->mutex);
  dt_pthread_mutex_lock(&dev->preview2.pipe->mutex);
  dt_pthread_mutex_lock(&dev->full.pipe->mutex);

  dev->gui_leaving = TRUE;

  dt_pthread_mutex_lock(&dev->history_mutex);

  dt_dev_pixelpipe_cleanup_nodes(dev->full.pipe);
  dt_dev_pixelpipe_cleanup_nodes(dev->preview2.pipe);
  dt_dev_pixelpipe_cleanup_nodes(dev->preview_pipe);

  while(dev->history)
  {
    dt_dev_history_item_t *hist = dev->history->data;
    // printf("removing history item %d - %s, data %f %f\n", hist->module->instance, hist->module->op, *(float
    // *)hist->params, *((float *)hist->params+1));
    dt_dev_free_history_item(hist);
    dev->history = g_list_delete_link(dev->history, dev->history);
  }

  while(dev->iop)
  {
    dt_iop_module_t *module = dev->iop->data;
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

  dt_pthread_mutex_unlock(&dev->full.pipe->mutex);
  dt_pthread_mutex_unlock(&dev->preview2.pipe->mutex);
  dt_pthread_mutex_unlock(&dev->preview_pipe->mutex);

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

  darktable.develop->image_storage.id = NO_IMGID;

  dt_print(DT_DEBUG_CONTROL, "[run_job-] 11 %f in darkroom mode", dt_get_wtime());
}

void mouse_leave(dt_view_t *self)
{
  // if we are not hovering over a thumbnail in the filmstrip -> show metadata of opened image.
  dt_develop_t *dev = self->data;
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

void mouse_enter(dt_view_t *self)
{
  dt_develop_t *dev = self->data;
  // masks
  dev->darkroom_mouse_in_center_area = TRUE;
  dt_masks_events_mouse_enter(dev->gui_module);
}

void mouse_moved(dt_view_t *self, double x, double y, double pressure, int which)
{
  dt_develop_t *dev = self->data;

  // if we are not hovering over a thumbnail in the filmstrip -> show metadata of opened image.
  dt_imgid_t mouse_over_id = dt_control_get_mouse_over_id();
  if(!dt_is_valid_imgid(mouse_over_id))
  {
    mouse_over_id = dev->image_storage.id;
    dt_control_set_mouse_over_id(mouse_over_id);
  }

  dt_control_t *ctl = darktable.control;
  int handled = 0;

  float zoom_x, zoom_y, zoom_scale;
  dt_dev_get_pointer_zoom_pos(&dev->full, x, y, &zoom_x, &zoom_y, &zoom_scale);

  if(!darktable.develop->darkroom_skip_mouse_events
     && dt_iop_color_picker_is_visible(dev)
     && ctl->button_down && ctl->button_down_which == 1)
  {
    // module requested a color box
    dt_colorpicker_sample_t *const sample = darktable.lib->proxy.colorpicker.primary_sample;
    // Make sure a minimal width/height
    float delta_x = 1.0f / (float) dev->full.pipe->processed_width;
    float delta_y = 1.0f / (float) dev->full.pipe->processed_height;

    dt_boundingbox_t pbox = { zoom_x, zoom_y };

    if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
    {
      float corner[2];
      dt_color_picker_transform_box(dev, 1, sample->point, corner, TRUE);

      pbox[0] = MAX(0.0, MIN(corner[0], zoom_x) - delta_x);
      pbox[1] = MAX(0.0, MIN(corner[1], zoom_y) - delta_y);
      pbox[2] = MIN(1.0, MAX(corner[0], zoom_x) + delta_x);
      pbox[3] = MIN(1.0, MAX(corner[1], zoom_y) + delta_y);
      dt_color_picker_backtransform_box(dev, 2, pbox, sample->box);
    }
    else if(sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
    {
      dt_color_picker_backtransform_box(dev, 1, pbox, sample->point);
    }
    dev->preview_pipe->status = DT_DEV_PIXELPIPE_DIRTY;
    dt_control_queue_redraw_center();
    handled = TRUE;
  }

  // masks
  if(dev->form_visible
     && !handled
     && !darktable.develop->darkroom_skip_mouse_events
     && !dt_iop_color_picker_is_visible(dev))
    handled = dt_masks_events_mouse_moved(dev->gui_module, zoom_x, zoom_y, pressure, which, zoom_scale);

  // module
  if(dev->gui_module && dev->gui_module->mouse_moved
     && !handled
     && !darktable.develop->darkroom_skip_mouse_events
     && !dt_iop_color_picker_is_visible(dev)
     && dt_dev_modulegroups_test_activated(darktable.develop))
    handled = dev->gui_module->mouse_moved(dev->gui_module, zoom_x, zoom_y, pressure, which, zoom_scale);

  if(ctl->button_down && ctl->button_down_which == 1)
  {
    if(!handled)
      dt_dev_zoom_move(&dev->full, DT_ZOOM_MOVE, -1.f, 0, x - ctl->button_x, y - ctl->button_y, TRUE);
    else
    {
      const int32_t bs = dev->full.border_size;
      float dx = MIN(0, x - bs) + MAX(0, x - dev->full.width  - bs);
      float dy = MIN(0, y - bs) + MAX(0, y - dev->full.height - bs);
      if(fabsf(dx) + fabsf(dy) > 0.5f)
        dt_dev_zoom_move(&dev->full, DT_ZOOM_MOVE, 1.f, 0, dx, dy, TRUE);
    }
    ctl->button_x = x;
    ctl->button_y = y;
  }
  else if(darktable.control->button_down
          && !handled
          && darktable.control->button_down_which == 3
          && dev->proxy.rotate)
  {
    dev->proxy.rotate->mouse_moved(dev->proxy.rotate, zoom_x, zoom_y, pressure, which, zoom_scale);
  }
}


int button_released(dt_view_t *self, double x, double y, int which, uint32_t state)
{
  dt_develop_t *dev = darktable.develop;

  float zoom_x, zoom_y, zoom_scale;
  dt_dev_get_pointer_zoom_pos(&dev->full, x, y, &zoom_x, &zoom_y, &zoom_scale);

  if(darktable.develop->darkroom_skip_mouse_events && which == 1)
  {
    dt_control_change_cursor(GDK_LEFT_PTR);
    return 1;
  }

  int handled = 0;
  if(dt_iop_color_picker_is_visible(dev) && which == 1)
  {
    // only sample box picker at end, for speed
    if(darktable.lib->proxy.colorpicker.primary_sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
    {
      dev->preview_pipe->status = DT_DEV_PIXELPIPE_DIRTY;
      dt_control_queue_redraw_center();
      dt_control_change_cursor(GDK_LEFT_PTR);
    }
    return 1;
  }

  // rotate
  if(which == 3 && dev->proxy.rotate)
    handled = dev->proxy.rotate->button_released(dev->proxy.rotate, zoom_x, zoom_y, which, state, zoom_scale);
  if(handled) return handled;
  // masks
  if(dev->form_visible)
    handled = dt_masks_events_button_released(dev->gui_module, zoom_x, zoom_y, which, state, zoom_scale);
  if(handled) return handled;
  // module
  if(dev->gui_module && dev->gui_module->button_released
     && dt_dev_modulegroups_test_activated(darktable.develop))
    handled = dev->gui_module->button_released(dev->gui_module, zoom_x, zoom_y, which, state, zoom_scale);
  if(handled) return handled;
  if(which == 1) dt_control_change_cursor(GDK_LEFT_PTR);

  return 1;
}


int button_pressed(dt_view_t *self,
                   double x,
                   double y,
                   double pressure,
                   const int which,
                   const int type,
                   const uint32_t state)
{
  dt_develop_t *dev = self->data;
  dt_colorpicker_sample_t *const sample = darktable.lib->proxy.colorpicker.primary_sample;

  float zoom_x, zoom_y, zoom_scale;
  dt_dev_get_pointer_zoom_pos(&dev->full, x, y, &zoom_x, &zoom_y, &zoom_scale);

  if(darktable.develop->darkroom_skip_mouse_events)
  {
    if(which == 1)
    {
      if(type == GDK_2BUTTON_PRESS) return 0;
      dt_control_change_cursor(GDK_HAND1);
      return 1;
    }
    else if(which == 3 && dev->proxy.rotate)
      return dev->proxy.rotate->button_pressed(dev->proxy.rotate, zoom_x, zoom_y, pressure, which, type, state, zoom_scale);
  }

  int handled = 0;
  if(dt_iop_color_picker_is_visible(dev))
  {
    const int procw = dev->preview_pipe->backbuf_width;
    const int proch = dev->preview_pipe->backbuf_height;

    if(which == 1)
    {
      sample->point[0] = zoom_x;
      sample->point[1] = zoom_y;

      if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
      {
        dt_boundingbox_t sbox;
        dt_color_picker_transform_box(dev, 2, sample->box, sbox, TRUE);

        const float handle_px = 6.0f;
        float hx = handle_px / (procw * zoom_scale);
        float hy = handle_px / (proch * zoom_scale);

        const float dx0 = fabsf(zoom_x - sbox[0]);
        const float dx1 = fabsf(zoom_x - sbox[2]);
        const float dy0 = fabsf(zoom_y - sbox[1]);
        const float dy1 = fabsf(zoom_y - sbox[3]);

        if(MIN(dx0, dx1) < hx && MIN(dy0, dy1) < hy)
        {
          sample->point[0] = sbox[dx0 < dx1 ? 2 : 0];
          sample->point[1] = sbox[dy0 < dy1 ? 3 : 1];
        }
        else
        {
          const float dx = 0.02f;
          const float dy = dx * (float)dev->full.pipe->processed_width / (float)dev->full.pipe->processed_height;
          const dt_boundingbox_t fbox = { zoom_x - dx, zoom_y - dy, zoom_x + dx, zoom_y + dy };
          dt_color_picker_backtransform_box(dev, 2, fbox, sample->box);
        }
        dt_control_change_cursor(GDK_FLEUR);
      }

      dt_color_picker_backtransform_box(dev, 1, sample->point, sample->point);
      dev->preview_pipe->status = DT_DEV_PIXELPIPE_DIRTY;
      dt_control_queue_redraw_center();
      return 1;
    }

    if(which == 3)
    {
      // apply a live sample's area to the active picker?
      // FIXME: this is a naive implementation, nicer would be to cycle through overlapping samples then reset
      dt_iop_color_picker_t *picker = darktable.lib->proxy.colorpicker.picker_proxy;
      if(darktable.lib->proxy.colorpicker.display_samples)
      {
        for(GSList *samples = darktable.lib->proxy.colorpicker.live_samples; samples; samples = g_slist_next(samples))
        {
          dt_colorpicker_sample_t *live_sample = samples->data;
          dt_boundingbox_t sbox;
          if(live_sample->size == DT_LIB_COLORPICKER_SIZE_BOX
             && (picker->flags & DT_COLOR_PICKER_AREA))
          {
            dt_color_picker_transform_box(dev, 2, live_sample->box, sbox, TRUE);
            if(zoom_x < sbox[0] || zoom_x > sbox[2] ||
               zoom_y < sbox[1] || zoom_y > sbox[3])
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
            dt_color_picker_transform_box(dev, 1, live_sample->point, sbox, TRUE);
            if(!feqf(zoom_x, sbox[0], slop_x) || !feqf(zoom_y, sbox[1], slop_y))
              continue;
            dt_lib_colorpicker_set_point(darktable.lib, live_sample->point);
          }
          else
            continue;
          dev->preview_pipe->status = DT_DEV_PIXELPIPE_DIRTY;
          dt_control_queue_redraw_center();
          return 1;
        }
      }
      if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
      {
        // default is hardcoded this way
        // FIXME: color_pixer_proxy should have an dt_iop_color_picker_clear_area() function for this
        dt_boundingbox_t reset = { 0.02f, 0.02f, 0.98f, 0.98f };
        dt_pickerbox_t box;
        dt_color_picker_backtransform_box(dev, 2, reset, box);
        dt_lib_colorpicker_set_box_area(darktable.lib, box);
        dev->preview_pipe->status = DT_DEV_PIXELPIPE_DIRTY;
        dt_control_queue_redraw_center();
      }

      return 1;
    }
  }

  // masks
  if(dev->form_visible)
    handled = dt_masks_events_button_pressed(dev->gui_module, zoom_x, zoom_y, pressure, which, type, state);
  if(handled) return handled;
  // module
  if(dev->gui_module && dev->gui_module->button_pressed
     && dt_dev_modulegroups_test_activated(darktable.develop))
    handled = dev->gui_module->button_pressed(dev->gui_module, zoom_x, zoom_y, pressure, which, type, state, zoom_scale);
  if(handled) return handled;

  if(which == 1 && type == GDK_2BUTTON_PRESS) return 0;
  if(which == 1)
  {
    dt_control_change_cursor(GDK_HAND1);
    return 1;
  }

  if(which == 2  && type == GDK_BUTTON_PRESS) // Middle mouse button
    dt_dev_zoom_move(&dev->full, DT_ZOOM_1, 0.0f, -2, x, y, !dt_modifier_is(state, GDK_CONTROL_MASK));
  if(which == 3 && dev->proxy.rotate)
    return dev->proxy.rotate->button_pressed(dev->proxy.rotate, zoom_x, zoom_y, pressure, which, type, state, zoom_scale);

  return 0;
}

void scrollbar_changed(dt_view_t *self, double x, double y)
{
  dt_dev_zoom_move(&darktable.develop->full, DT_ZOOM_POSITION, 0.0f, 0, x, y, TRUE);
}

void scrolled(dt_view_t *self, double x, double y, int up, int state)
{
  dt_develop_t *dev = self->data;

  float zoom_x, zoom_y, zoom_scale;
  dt_dev_get_pointer_zoom_pos(&dev->full, x, y, &zoom_x, &zoom_y, &zoom_scale);

  int handled = 0;

  // masks
  if(dev->form_visible
     && !darktable.develop->darkroom_skip_mouse_events)
    handled = dt_masks_events_mouse_scrolled(dev->gui_module, zoom_x, zoom_y, up, state);
  if(handled) return;

  // module
  if(dev->gui_module && dev->gui_module->scrolled
     && !darktable.develop->darkroom_skip_mouse_events
     && !dt_iop_color_picker_is_visible(dev)
     && dt_dev_modulegroups_test_activated(darktable.develop))
    handled = dev->gui_module->scrolled(dev->gui_module, zoom_x, zoom_y, up, state);
  if(handled) return;

  // free zoom
  const gboolean constrained = !dt_modifier_is(state, GDK_CONTROL_MASK);
  dt_dev_zoom_move(&dev->full, DT_ZOOM_SCROLL, 0.0f, up, x, y, constrained);
}

static void _change_slider_accel_precision(dt_action_t *action)
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
  dt_develop_t *dev = self->data;
  dev->full.orig_width = wd;
  dev->full.orig_height = ht;
  dt_dev_configure(&dev->full);
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

  const dt_develop_t *dev = self->data;
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
#define DT_PIXEL_APPLY_DPI_2ND_WND(dev, value) ((value) * dev->preview2.dpi_factor)

static void _dt_second_window_change_cursor(dt_develop_t *dev, const gchar *curs)
{
  GtkWidget *widget = dev->second_wnd;
  GdkCursor *cursor = gdk_cursor_new_from_name(gdk_display_get_default(), curs);
  gdk_window_set_cursor(gtk_widget_get_window(widget), cursor);
  g_object_unref(cursor);
}

static void _second_window_leave(dt_develop_t *dev)
{
  // reset any changes the selected plugin might have made.
  _dt_second_window_change_cursor(dev, "default");
}

static void _second_window_configure_ppd_dpi(dt_develop_t *dev)
{
  GtkWidget *widget = dev->second_wnd;

  dev->preview2.ppd = dt_get_system_gui_ppd(widget);
  dev->preview2.dpi = dt_get_screen_resolution(widget);

#ifdef GDK_WINDOWING_QUARTZ
  dev->preview2.dpi_factor
      = dev->preview2.dpi / 72; // macOS has a fixed DPI of 72
#else
  dev->preview2.dpi_factor
      = dev->preview2.dpi / 96; // according to man xrandr and the docs of gdk_screen_set_resolution 96 is the default
#endif
}

static gboolean _second_window_draw_callback(GtkWidget *widget,
                                             cairo_t *cri,
                                             dt_develop_t *dev)
{
  cairo_set_source_rgb(cri, 0.2, 0.2, 0.2);

  if(dev->preview2.pipe->backbuf)  // do we have an image?
  {
    // draw image
    dt_gui_gtk_set_source_rgb(cri, DT_GUI_COLOR_DARKROOM_BG);
    cairo_paint(cri);

    _view_paint_surface(cri, dev->preview2.orig_width, dev->preview2.orig_height,
                        &dev->preview2, DT_WINDOW_SECOND);
  }

  if(_preview2_request(dev)) dt_dev_process_preview2(dev);

  return TRUE;
}

static gboolean _second_window_scrolled_callback(GtkWidget *widget,
                                                 GdkEventScroll *event,
                                                 dt_develop_t *dev)
{
  int delta_y;
  if(dt_gui_get_scroll_unit_delta(event, &delta_y))
  {
    const gboolean constrained = !dt_modifier_is(event->state, GDK_CONTROL_MASK);
    dt_dev_zoom_move(&dev->preview2, DT_ZOOM_SCROLL, 0.0f, delta_y < 0,
                     event->x, event->y, constrained);
  }

  return TRUE;
}

static gboolean _second_window_button_pressed_callback(GtkWidget *w,
                                                       GdkEventButton *event,
                                                       dt_develop_t *dev)
{
  if(event->type == GDK_2BUTTON_PRESS) return 0;
  if(event->button == 1)
  {
    darktable.control->button_x = event->x;
    darktable.control->button_y = event->y;
    _dt_second_window_change_cursor(dev, "grabbing");
    return TRUE;
  }
  if(event->button == 2)
  {
    dt_dev_zoom_move(&dev->preview2, DT_ZOOM_1, 0.0f, -2, event->x, event->y, !dt_modifier_is(event->state, GDK_CONTROL_MASK));
    return TRUE;
  }
  return FALSE;
}

static gboolean _second_window_button_released_callback(GtkWidget *w,
                                                        GdkEventButton *event,
                                                        dt_develop_t *dev)
{
  if(event->button == 1) _dt_second_window_change_cursor(dev, "default");

  gtk_widget_queue_draw(w);
  return TRUE;
}

static gboolean _second_window_mouse_moved_callback(GtkWidget *w,
                                                    GdkEventMotion *event,
                                                    dt_develop_t *dev)
{
  if(event->state & GDK_BUTTON1_MASK)
  {
    dt_control_t *ctl = darktable.control;
    dt_dev_zoom_move(&dev->preview2, DT_ZOOM_MOVE, -1.f, 0, event->x - ctl->button_x, event->y - ctl->button_y, TRUE);
    ctl->button_x = event->x;
    ctl->button_y = event->y;
    return TRUE;
  }
  return FALSE;
}

static gboolean _second_window_leave_callback(GtkWidget *widget,
                                              GdkEventCrossing *event,
                                              dt_develop_t *dev)
{
  _second_window_leave(dev);
  return TRUE;
}

static gboolean _second_window_configure_callback(GtkWidget *da,
                                                  GdkEventConfigure *event,
                                                  dt_develop_t *dev)
{
  if(dev->preview2.orig_width != event->width
     || dev->preview2.orig_height != event->height)
  {
    dev->preview2.width = event->width;
    dev->preview2.height = event->height;
    dev->preview2.orig_width = event->width;
    dev->preview2.orig_height = event->height;

    // pipe needs to be reconstructed
    dev->preview2.pipe->status = DT_DEV_PIXELPIPE_DIRTY;
    dev->preview2.pipe->changed |= DT_DEV_PIPE_REMOVE;
    dev->preview2.pipe->cache_obsolete = TRUE;
  }

  dt_colorspaces_set_display_profile(DT_COLORSPACE_DISPLAY2);

#ifndef GDK_WINDOWING_QUARTZ
  _second_window_configure_ppd_dpi(dev);
#endif

  dt_dev_configure(&dev->preview2);

  return TRUE;
}

static void _darkroom_ui_second_window_init(GtkWidget *widget, dt_develop_t *dev)
{
  const int width = MAX(10, dt_conf_get_int("second_window/window_w"));
  const int height = MAX(10, dt_conf_get_int("second_window/window_h"));

  dev->preview2.border_size = 0;

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

static gboolean _second_window_delete_callback(GtkWidget *widget,
                                               GdkEvent *event,
                                               dt_develop_t *dev)
{
  _darkroom_ui_second_window_write_config(dev->second_wnd);

  dev->second_wnd = NULL;
  dev->preview2.widget = NULL;

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dev->second_wnd_button), FALSE);

  return FALSE;
}

static void _darkroom_display_second_window(dt_develop_t *dev)
{
  if(dev->second_wnd == NULL)
  {
    dev->preview2.width = -1;
    dev->preview2.height = -1;

    dev->second_wnd = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_name(dev->second_wnd, "second_window");

    _second_window_configure_ppd_dpi(dev);

    gtk_window_set_icon_name(GTK_WINDOW(dev->second_wnd), "darktable");
    gtk_window_set_title(GTK_WINDOW(dev->second_wnd), _("darktable - darkroom preview"));

    dev->preview2.widget = gtk_drawing_area_new();
    gtk_container_add(GTK_CONTAINER(dev->second_wnd), dev->preview2.widget);
    gtk_widget_set_size_request(dev->preview2.widget, DT_PIXEL_APPLY_DPI_2ND_WND(dev, 50), DT_PIXEL_APPLY_DPI_2ND_WND(dev, 200));
    gtk_widget_set_hexpand(dev->preview2.widget, TRUE);
    gtk_widget_set_vexpand(dev->preview2.widget, TRUE);
    gtk_widget_set_app_paintable(dev->preview2.widget, TRUE);

    gtk_widget_set_events(dev->preview2.widget, GDK_POINTER_MOTION_MASK
                                                         | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                                         | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK
                                                         | darktable.gui->scroll_mask);

    /* connect callbacks */
    g_signal_connect(G_OBJECT(dev->preview2.widget), "draw",
                     G_CALLBACK(_second_window_draw_callback), dev);
    g_signal_connect(G_OBJECT(dev->preview2.widget), "scroll-event",
                     G_CALLBACK(_second_window_scrolled_callback), dev);
    g_signal_connect(G_OBJECT(dev->preview2.widget), "button-press-event",
                     G_CALLBACK(_second_window_button_pressed_callback), dev);
    g_signal_connect(G_OBJECT(dev->preview2.widget), "button-release-event",
                     G_CALLBACK(_second_window_button_released_callback), dev);
    g_signal_connect(G_OBJECT(dev->preview2.widget), "motion-notify-event",
                     G_CALLBACK(_second_window_mouse_moved_callback), dev);
    g_signal_connect(G_OBJECT(dev->preview2.widget), "leave-notify-event",
                     G_CALLBACK(_second_window_leave_callback), dev);
    g_signal_connect(G_OBJECT(dev->preview2.widget), "configure-event",
                     G_CALLBACK(_second_window_configure_callback), dev);

    g_signal_connect(G_OBJECT(dev->second_wnd), "delete-event",
                     G_CALLBACK(_second_window_delete_callback), dev);
    g_signal_connect(G_OBJECT(dev->second_wnd), "event",
                     G_CALLBACK(dt_shortcut_dispatcher), NULL);

    _darkroom_ui_second_window_init(dev->second_wnd, dev);
  }

  gtk_widget_show_all(dev->second_wnd);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
