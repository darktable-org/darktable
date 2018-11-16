/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.
    copyright (c) 2011--2012 henrik andersson.

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
#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
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
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "libs/colorpicker.h"
#include "views/view.h"
#include "views/view_api.h"

#include <gdk/gdkkeysyms.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

DT_MODULE(1)

static gboolean film_strip_key_accel(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                     GdkModifierType modifier, gpointer data);

static gboolean zoom_key_accel(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                               GdkModifierType modifier, gpointer data);

static gboolean export_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                          GdkModifierType modifier, gpointer data);

static gboolean skip_f_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                          GdkModifierType modifier, gpointer data);
static gboolean skip_b_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                          GdkModifierType modifier, gpointer data);
static gboolean _toolbox_toggle_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                         GdkModifierType modifier, gpointer data);
static gboolean _brush_size_up_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                        GdkModifierType modifier, gpointer data);
static gboolean _brush_size_down_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                          GdkModifierType modifier, gpointer data);
static gboolean _brush_hardness_up_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                            GdkModifierType modifier, gpointer data);
static gboolean _brush_hardness_down_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                              GdkModifierType modifier, gpointer data);
static gboolean _brush_opacity_up_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                           GdkModifierType modifier, gpointer data);
static gboolean _brush_opacity_down_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                             GdkModifierType modifier, gpointer data);

static void _update_softproof_gamut_checking(dt_develop_t *d);

/* signal handler for filmstrip image switching */
static void _view_darkroom_filmstrip_activate_callback(gpointer instance, gpointer user_data);


const char *name(dt_view_t *self)
{
  return _("darkroom");
}


void init(dt_view_t *self)
{
  self->data = malloc(sizeof(dt_develop_t));
  dt_dev_init((dt_develop_t *)self->data, 1);
}

uint32_t view(const dt_view_t *self)
{
  return DT_VIEW_DARKROOM;
}

void cleanup(dt_view_t *self)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  dt_dev_cleanup(dev);
  free(dev);
}

static cairo_status_t write_snapshot_data(void *closure, const unsigned char *data, unsigned int length)
{
  int fd = GPOINTER_TO_INT(closure);
  ssize_t res = write(fd, data, length);
  if(res != length)
    return CAIRO_STATUS_WRITE_ERROR;
  return CAIRO_STATUS_SUCCESS;
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

  const int32_t tb = DT_PIXEL_APPLY_DPI(dt_conf_get_int("plugins/darkroom/ui/border_size"));
  // account for border, make it transparent for other modules called below:
  pointerx -= tb;
  pointery -= tb;

  dt_develop_t *dev = (dt_develop_t *)self->data;

  if(dev->gui_synch && !dev->image_loading)
  {
    // synch module guis from gtk thread:
    darktable.gui->reset = 1;
    GList *modules = dev->iop;
    while(modules)
    {
      dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
      dt_iop_gui_update(module);
      modules = g_list_next(modules);
    }
    darktable.gui->reset = 0;
    dev->gui_synch = 0;
  }

  if(dev->image_status == DT_DEV_PIXELPIPE_DIRTY || dev->image_status == DT_DEV_PIXELPIPE_INVALID
     || dev->pipe->input_timestamp < dev->preview_pipe->input_timestamp)
    dt_dev_process_image(dev);
  if(dev->preview_status == DT_DEV_PIXELPIPE_DIRTY || dev->preview_status == DT_DEV_PIXELPIPE_INVALID
     || dev->pipe->input_timestamp > dev->preview_pipe->input_timestamp)
    dt_dev_process_preview(dev);

  dt_pthread_mutex_t *mutex = NULL;
  int stride;
  const float zoom_y = dt_control_get_dev_zoom_y();
  const float zoom_x = dt_control_get_dev_zoom_x();
  const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  const int closeup = dt_control_get_dev_closeup();
  static cairo_surface_t *image_surface = NULL;
  static int image_surface_width = 0, image_surface_height = 0, image_surface_imgid = -1;
  static float roi_hash_old = -1.0f;
  // compute patented dreggn hash so we don't need to check all values:
  const float roi_hash = width + 7.0f * height + 23.0f * zoom + 42.0f * zoom_x + 91.0f * zoom_y
                         + 666.0f * zoom;

  if(image_surface_width != width || image_surface_height != height || image_surface == NULL)
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
    dt_view_set_scrollbar(self, zx, -0.5 + boxw/2, 0.5, boxw/2, zy, -0.5+ boxh/2, 0.5, boxh/2);
  }

  if((dev->image_status == DT_DEV_PIXELPIPE_VALID)
     && dev->pipe->input_timestamp >= dev->preview_pipe->input_timestamp)
  {
    // draw image
    roi_hash_old = roi_hash;
    mutex = &dev->pipe->backbuf_mutex;
    dt_pthread_mutex_lock(mutex);
    float wd = dev->pipe->backbuf_width;
    float ht = dev->pipe->backbuf_height;
    stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, wd);
    surface = dt_cairo_image_surface_create_for_data(dev->pipe->backbuf, CAIRO_FORMAT_RGB24, wd, ht, stride);
    wd /= darktable.gui->ppd;
    ht /= darktable.gui->ppd;
    if(dev->full_preview)
      dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_DARKROOM_PREVIEW_BG);
    else
      dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_DARKROOM_BG);
    cairo_paint(cr);
    cairo_translate(cr, .5f * (width - wd), .5f * (height - ht));
    if(closeup)
    {
      const double scale = 1<<closeup;
      cairo_scale(cr, scale, scale);
      cairo_translate(cr, -(.5 - 0.5/scale) * wd, -(.5 - 0.5/scale) * ht);
    }
    cairo_rectangle(cr, 0, 0, wd, ht);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgb(cr, .3, .3, .3);
    cairo_stroke(cr);
    cairo_surface_destroy(surface);
    dt_pthread_mutex_unlock(mutex);
    image_surface_imgid = dev->image_storage.id;
  }
  else if((dev->preview_status == DT_DEV_PIXELPIPE_VALID) && (roi_hash != roi_hash_old))
  {
    // draw preview
    roi_hash_old = roi_hash;
    mutex = &dev->preview_pipe->backbuf_mutex;
    dt_pthread_mutex_lock(mutex);

    const float wd = dev->preview_pipe->backbuf_width;
    const float ht = dev->preview_pipe->backbuf_height;
    const float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 1);
    dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_DARKROOM_BG);
    cairo_paint(cr);
    cairo_rectangle(cr, tb, tb, width-2*tb, height-2*tb);
    cairo_clip(cr);
    stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, wd);
    surface
        = cairo_image_surface_create_for_data(dev->preview_pipe->backbuf, CAIRO_FORMAT_RGB24, wd, ht, stride);
    cairo_translate(cr, width / 2.0, height / 2.0f);
    cairo_scale(cr, zoom_scale, zoom_scale);
    cairo_translate(cr, -.5f * wd - zoom_x * wd, -.5f * ht - zoom_y * ht);
    // avoid to draw the 1px garbage that sometimes shows up in the preview :(
    cairo_rectangle(cr, 0, 0, wd - 1, ht - 1);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
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

  /* if we are in full preview mode, we don"t want anything else than the image */
  if(dev->full_preview) return;

  /* check if we should create a snapshot of view */
  if(darktable.develop->proxy.snapshot.request && !darktable.develop->image_loading)
  {
    /* reset the request */
    darktable.develop->proxy.snapshot.request = FALSE;

    /* validation of snapshot filename */
    g_assert(darktable.develop->proxy.snapshot.filename != NULL);

    /* Store current image surface to snapshot file.
       FIXME: add checks so that we dont make snapshots of preview pipe image surface.
    */
    int fd = g_open(darktable.develop->proxy.snapshot.filename, O_CREAT | O_WRONLY, 0600);
    cairo_surface_write_to_png_stream(image_surface, write_snapshot_data, GINT_TO_POINTER(fd));
    close(fd);
  }

  // Displaying sample areas if enabled
  if(darktable.lib->proxy.colorpicker.live_samples && darktable.lib->proxy.colorpicker.display_samples)
  {
    GSList *samples = darktable.lib->proxy.colorpicker.live_samples;
    dt_colorpicker_sample_t *sample = NULL;

    cairo_save(cri);

    const float wd = dev->preview_pipe->backbuf_width;
    const float ht = dev->preview_pipe->backbuf_height;
    const float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 1);

    cairo_translate(cri, width / 2.0, height / 2.0f);
    cairo_scale(cri, zoom_scale, zoom_scale);
    cairo_translate(cri, -.5f * wd - zoom_x * wd, -.5f * ht - zoom_y * ht);

    while(samples)
    {
      sample = samples->data;

      cairo_set_line_width(cri, 1.0 / zoom_scale);
      if(sample == darktable.lib->proxy.colorpicker.selected_sample)
        cairo_set_source_rgb(cri, .2, 0, 0);
      else
        cairo_set_source_rgb(cri, 0, 0, .2);

      const float *box = sample->box;
      const float *point = sample->point;
      if(sample->size == DT_COLORPICKER_SIZE_BOX)
      {
        cairo_rectangle(cri, box[0] * wd, box[1] * ht, (box[2] - box[0]) * wd, (box[3] - box[1]) * ht);
        cairo_stroke(cri);
        cairo_translate(cri, 1.0 / zoom_scale, 1.0 / zoom_scale);
        if(sample == darktable.lib->proxy.colorpicker.selected_sample)
          cairo_set_source_rgb(cri, .8, 0, 0);
        else
          cairo_set_source_rgb(cri, 0, 0, .8);
        cairo_rectangle(cri, box[0] * wd + 1.0 / zoom_scale, box[1] * ht,
                        (box[2] - box[0]) * wd - 3. / zoom_scale, (box[3] - box[1]) * ht - 2. / zoom_scale);
        cairo_stroke(cri);
      }
      else
      {
        cairo_rectangle(cri, point[0] * wd - .01 * wd, point[1] * ht - .01 * wd, .02 * wd, .02 * wd);
        cairo_stroke(cri);

        if(sample == darktable.lib->proxy.colorpicker.selected_sample)
          cairo_set_source_rgb(cri, .8, 0, 0);
        else
          cairo_set_source_rgb(cri, 0, 0, .8);
        cairo_rectangle(cri, (point[0] - 0.01) * wd + 1.0 / zoom_scale,
                        point[1] * ht - 0.01 * wd + 1.0 / zoom_scale, .02 * wd - 2. / zoom_scale,
                        .02 * wd - 2. / zoom_scale);
        cairo_move_to(cri, point[0] * wd, point[1] * ht - .01 * wd + 1. / zoom_scale);
        cairo_line_to(cri, point[0] * wd, point[1] * ht + .01 * wd - 1. / zoom_scale);
        cairo_move_to(cri, point[0] * wd - .01 * wd + 1. / zoom_scale, point[1] * ht);
        cairo_line_to(cri, point[0] * wd + .01 * wd - 1. / zoom_scale, point[1] * ht);
        cairo_stroke(cri);
      }

      samples = g_slist_next(samples);
    }

    cairo_restore(cri);
  }

  // execute module callback hook.
  if(dev->gui_module && dev->gui_module->request_color_pick != DT_REQUEST_COLORPICK_OFF)
  {
    const float wd = dev->preview_pipe->backbuf_width;
    const float ht = dev->preview_pipe->backbuf_height;
    const float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 1);

    cairo_translate(cri, width / 2.0, height / 2.0f);
    cairo_scale(cri, zoom_scale, zoom_scale);
    cairo_translate(cri, -.5f * wd - zoom_x * wd, -.5f * ht - zoom_y * ht);

    // cairo_set_operator(cri, CAIRO_OPERATOR_XOR);
    cairo_set_line_width(cri, 1.0 / zoom_scale);
    cairo_set_source_rgb(cri, .2, .2, .2);

    const float *box = dev->gui_module->color_picker_box;
    const float *point = dev->gui_module->color_picker_point;
    if(darktable.lib->proxy.colorpicker.size)
    {
      cairo_rectangle(cri, box[0] * wd, box[1] * ht, (box[2] - box[0]) * wd, (box[3] - box[1]) * ht);
      cairo_stroke(cri);
      cairo_translate(cri, 1.0 / zoom_scale, 1.0 / zoom_scale);
      cairo_set_source_rgb(cri, .8, .8, .8);
      cairo_rectangle(cri, box[0] * wd + 1.0 / zoom_scale, box[1] * ht,
                      (box[2] - box[0]) * wd - 3. / zoom_scale, (box[3] - box[1]) * ht - 2. / zoom_scale);
      cairo_stroke(cri);
    }
    else if(point[0] >= 0.0f && point[0] <= 1.0f && point[1] >= 0.0f && point[1] <= 1.0f)
    {
      cairo_rectangle(cri, point[0] * wd - .01 * wd, point[1] * ht - .01 * wd, .02 * wd, .02 * wd);
      cairo_stroke(cri);

      cairo_set_source_rgb(cri, .8, .8, .8);
      cairo_rectangle(cri, (point[0] - 0.01) * wd + 1.0 / zoom_scale,
                      point[1] * ht - 0.01 * wd + 1.0 / zoom_scale, .02 * wd - 2. / zoom_scale,
                      .02 * wd - 2. / zoom_scale);
      cairo_move_to(cri, point[0] * wd, point[1] * ht - .01 * wd + 1. / zoom_scale);
      cairo_line_to(cri, point[0] * wd, point[1] * ht + .01 * wd - 1. / zoom_scale);
      cairo_move_to(cri, point[0] * wd - .01 * wd + 1. / zoom_scale, point[1] * ht);
      cairo_line_to(cri, point[0] * wd + .01 * wd - 1. / zoom_scale, point[1] * ht);
      cairo_stroke(cri);
    }
  }
  else
  {
    // masks
    if(dev->form_visible)
      dt_masks_events_post_expose(dev->gui_module, cri, width, height, pointerx, pointery);
    // module
    if(dev->gui_module && dev->gui_module->gui_post_expose)
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
  int selected = dt_control_get_mouse_over_id();
  if(selected < 0)
  {
    // try last selected
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images", -1, &stmt,
                                NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW) selected = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    // Leave as selected only the image being edited
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM main.selected_images", NULL, NULL, NULL);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "INSERT OR IGNORE INTO main.selected_images VALUES (?1)", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, selected);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  if(selected < 0)
  {
    // fail :(
    dt_control_log(_("no image selected!"));
    return 1;
  }

  // this loads the image from db if needed:
  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, selected, 'r');
  // get image and check if it has been deleted from disk first!

  char imgfilename[PATH_MAX] = { 0 };
  gboolean from_cache = TRUE;
  dt_image_full_path(img->id, imgfilename, sizeof(imgfilename), &from_cache);
  if(!g_file_test(imgfilename, G_FILE_TEST_IS_REGULAR))
  {
    dt_control_log(_("image `%s' is currently unavailable"), img->filename);
    // dt_image_remove(selected);
    dt_image_cache_read_release(darktable.image_cache, img);
    return 1;
  }
  // and drop the lock again.
  dt_image_cache_read_release(darktable.image_cache, img);
  darktable.develop->image_storage.id = selected;
  return 0;
}



static void select_this_image(const int imgid)
{
  // select this image, if no multiple selection:
  if(dt_collection_get_selected_count(NULL) < 2)
  {
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM main.selected_images", NULL, NULL, NULL);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "INSERT OR IGNORE INTO main.selected_images VALUES (?1)", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
}

static void dt_dev_cleanup_module_accels(dt_iop_module_t *module)
{
  dt_accel_disconnect_list(module->accel_closures);
  dt_accel_cleanup_locals_iop(module);
}

static void dt_dev_change_image(dt_develop_t *dev, const uint32_t imgid)
{
  // stop crazy users from sleeping on key-repeat spacebar:
  if(dev->image_loading) return;

  // make sure we can destroy and re-setup the pixel pipes.
  // we acquire the pipe locks, which will block the processing threads
  // in darkroom mode before they touch the pipes (init buffers etc).
  // we don't block here, since we hold the gdk lock, which will
  // result in circular locking when background threads emit signals
  // which in turn try to acquire the gdk lock.
  //
  // worst case, it'll drop some change image events. sorry.
  if(dt_pthread_mutex_BAD_trylock(&dev->preview_pipe_mutex)) return;
  if(dt_pthread_mutex_BAD_trylock(&dev->pipe_mutex))
  {
    dt_pthread_mutex_BAD_unlock(&dev->preview_pipe_mutex);
    return;
  }

  // get last active plugin, make sure focus out is called:
  gchar *active_plugin = dt_conf_get_string("plugins/darkroom/active");
  dt_iop_request_focus(NULL);
  // store last active group
  dt_conf_set_int("plugins/darkroom/groups", dt_dev_modulegroups_get(dev));

  // store last active plugin:
  if(darktable.develop->gui_module)
    dt_conf_set_string("plugins/darkroom/active", darktable.develop->gui_module->op);
  else
    dt_conf_set_string("plugins/darkroom/active", "");
  g_assert(dev->gui_attached);

  // commit image ops to db
  dt_dev_write_history(dev);

  // be sure light table will update the thumbnail
  // TODO: only if image changed!
  // if()
  {
    dt_mipmap_cache_remove(darktable.mipmap_cache, dev->image_storage.id);
    dt_image_synch_xmp(dev->image_storage.id);
  }

  // cleanup visible masks
  if(!dev->form_gui)
  {
    dev->form_gui = (dt_masks_form_gui_t *)calloc(1, sizeof(dt_masks_form_gui_t));
    dt_masks_init_form_gui(dev->form_gui);
  }
  dt_masks_change_form_gui(NULL);

  select_this_image(imgid);

  while(dev->history)
  {
    // clear history of old image
    free(((dt_dev_history_item_t *)dev->history->data)->params);
    free(((dt_dev_history_item_t *)dev->history->data)->blend_params);
    free((dt_dev_history_item_t *)dev->history->data);
    dev->history = g_list_delete_link(dev->history, dev->history);
  }

  // get new image:
  dt_dev_reload_image(dev, imgid);

  // make sure no signals propagate here:
  darktable.gui->reset = 1;

  const guint nb_iop = g_list_length(dev->iop);
  dt_dev_pixelpipe_cleanup_nodes(dev->pipe);
  dt_dev_pixelpipe_cleanup_nodes(dev->preview_pipe);
  for(int i = nb_iop - 1; i >= 0; i--)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(g_list_nth_data(dev->iop, i));

    // the base module is the one with the highest multi_priority
    const guint clen = g_list_length(dev->iop);
    int base_multi_priority = 0;
    for(int k = 0; k < clen; k++)
    {
      dt_iop_module_t *mod = (dt_iop_module_t *)(g_list_nth_data(dev->iop, k));
      if(strcmp(module->op, mod->op) == 0) base_multi_priority = MAX(base_multi_priority, mod->multi_priority);
    }

    if(module->multi_priority == base_multi_priority) // if the module is the "base" instance, we keep it
    {
      module->multi_priority = 0;
      module->multi_name[0] = '\0';
      dt_iop_reload_defaults(module);
      dt_iop_gui_update(module);
    }
    else // else we delete it and remove it from the panel
    {
      if(!dt_iop_is_hidden(module))
      {
        gtk_widget_destroy(module->expander);
        dt_iop_gui_cleanup_module(module);
      }

      // we remove the module from the list
      dev->iop = g_list_remove_link(dev->iop, g_list_nth(dev->iop, i));

      // we cleanup the module
      dt_accel_disconnect_list(module->accel_closures);
      dt_accel_cleanup_locals_iop(module);
      module->accel_closures = NULL;
      dt_iop_cleanup_module(module);
      free(module);
    }
  }
  
  // we also clear the saved modules
  while(dev->alliop)
  {
    dt_iop_cleanup_module((dt_iop_module_t *)dev->alliop->data);
    free(dev->alliop->data);
    dev->alliop = g_list_delete_link(dev->alliop, dev->alliop);
  }
  
  dt_dev_pixelpipe_create_nodes(dev->pipe, dev);
  dt_dev_pixelpipe_create_nodes(dev->preview_pipe, dev);
  dt_masks_read_forms(dev);
  dt_dev_read_history(dev);

  // we have to init all module instances other than "base" instance
  GList *modules = g_list_last(dev->iop);
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    if(module->multi_priority > 0)
    {
      if(!dt_iop_is_hidden(module))
      {
        module->gui_init(module);
        dt_iop_reload_defaults(module);
        // we search the base iop corresponding
        GList *mods = g_list_first(dev->iop);
        dt_iop_module_t *base = NULL;
        int pos_module = 0;
        int pos_base = 0;
        int pos = 0;
        while(mods)
        {
          dt_iop_module_t *mod = (dt_iop_module_t *)(mods->data);
          if(mod->multi_priority == 0 && mod->instance == module->instance)
          {
            base = mod;
            pos_base = pos;
          }
          else if(mod == module)
            pos_module = pos;
          mods = g_list_next(mods);
          pos++;
        }
        if(!base) continue;

        /* add module to right panel */
        GtkWidget *expander = dt_iop_gui_get_expander(module);
        dt_ui_container_add_widget(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER, expander);
        GValue gv = { 0, { { 0 } } };
        g_value_init(&gv, G_TYPE_INT);
        gtk_container_child_get_property(
            GTK_CONTAINER(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER)),
            base->expander, "position", &gv);
        gtk_box_reorder_child(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER),
                              expander, g_value_get_int(&gv) + pos_base - pos_module);
        dt_iop_gui_set_expanded(module, FALSE, FALSE);
        dt_iop_gui_update_blending(module);
      }

      /* setup key accelerators */
      module->accel_closures = NULL;
      if(module->connect_key_accels) module->connect_key_accels(module);
      dt_iop_connect_common_accels(module);

      // we update show params for multi-instances for each other instances
      dt_dev_modules_update_multishow(module->dev);
    }
    else
    {
      //  update the module header to ensure proper multi-name display
      if(!dt_iop_is_hidden(module)) dt_iop_gui_update_header(module);
    }

    modules = g_list_previous(modules);
  }

  dt_dev_pop_history_items(dev, dev->history_end);

  if(active_plugin)
  {
    modules = dev->iop;
    while(modules)
    {
      dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
      if(!strcmp(module->op, active_plugin)) dt_iop_request_focus(module);
      modules = g_list_next(modules);
    }
    g_free(active_plugin);
  }

  dt_dev_masks_list_change(dev);

  /* last set the group to update visibility of iop modules for new pipe */
  dt_dev_modulegroups_set(dev, dt_conf_get_int("plugins/darkroom/groups"));

  /* cleanup histograms */
  g_list_foreach(dev->iop, (GFunc)dt_iop_cleanup_histogram, (gpointer)NULL);

  // make signals work again, but only after focus event,
  // to avoid crop/rotate for example to add another history item.
  darktable.gui->reset = 0;

  // Signal develop initialize
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_DEVELOP_IMAGE_CHANGED);

  // prefetch next few from first selected image on.
  dt_view_filmstrip_prefetch();

  // release pixel pipe mutices
  dt_pthread_mutex_BAD_unlock(&dev->preview_pipe_mutex);
  dt_pthread_mutex_BAD_unlock(&dev->pipe_mutex);

  // update hint message
  dt_collection_hint_message(darktable.collection);
}

static void film_strip_activated(const int imgid, void *data)
{
  // switch images in darkroom mode:
  const dt_view_t *self = (dt_view_t *)data;
  dt_develop_t *dev = (dt_develop_t *)self->data;

  // disable color picker when changing image
  if(dev->gui_module)
  {
    dev->gui_module->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  }

  // first compute/update possibly new aspect ratio of current picture
  dt_image_set_aspect_ratio(dev->image_storage.id);

  // clean the undo list
  dt_undo_clear(darktable.undo, DT_UNDO_DEVELOP);
  dt_dev_change_image(dev, imgid);
  dt_view_filmstrip_scroll_to_image(darktable.view_manager, imgid, FALSE);
  // record the imgid to display when going back to lighttable
  dt_view_lighttable_set_position(darktable.view_manager, dt_collection_image_offset(imgid));
  // force redraw
  dt_control_queue_redraw();
}

static void _view_darkroom_filmstrip_activate_callback(gpointer instance, gpointer user_data)
{
  int32_t imgid = 0;
  if((imgid = dt_view_filmstrip_get_activated_imgid(darktable.view_manager)) > 0)
    film_strip_activated(imgid, user_data);
}

static void dt_dev_jump_image(dt_develop_t *dev, int diff)
{
  const gchar *qin = dt_collection_get_query(darktable.collection);
  int offset = 0;
  if(qin)
  {
    int orig_imgid = -1, imgid = -1;
    sqlite3_stmt *stmt;

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images", -1, &stmt,
                                NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW) orig_imgid = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    offset = dt_collection_image_offset(orig_imgid);

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), qin, -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, offset + diff);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, 1);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      imgid = sqlite3_column_int(stmt, 0);

      if(orig_imgid == imgid)
      {
        // nothing to do
        sqlite3_finalize(stmt);
        return;
      }

      if(!dev->image_loading)
      {
        dt_view_filmstrip_scroll_to_image(darktable.view_manager, imgid, FALSE);
        // record the imgid to display when going back to lighttable
        dt_view_lighttable_set_position(darktable.view_manager, dt_collection_image_offset(imgid));
        dt_dev_change_image(dev, imgid);
      }
    }
    sqlite3_finalize(stmt);
  }
}

static gboolean zoom_key_accel(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                               GdkModifierType modifier, gpointer data)
{
  dt_develop_t *dev = darktable.develop;
  int zoom, closeup;
  float zoom_x, zoom_y;
  switch(GPOINTER_TO_INT(data))
  {
    case 1:
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
      dt_dev_invalidate(dev);
      break;
    case 2:
      zoom_x = zoom_y = 0.0f;
      dt_control_set_dev_zoom(DT_ZOOM_FILL);
      dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, DT_ZOOM_FILL, 0, NULL, NULL);
      dt_control_set_dev_zoom_x(zoom_x);
      dt_control_set_dev_zoom_y(zoom_y);
      dt_control_set_dev_closeup(0);
      dt_dev_invalidate(dev);
      break;
    case 3:
      dt_control_set_dev_zoom(DT_ZOOM_FIT);
      dt_control_set_dev_zoom_x(0);
      dt_control_set_dev_zoom_y(0);
      dt_control_set_dev_closeup(0);
      dt_dev_invalidate(dev);
      break;
    default:
      break;
  }
  dt_control_queue_redraw_center();
  return TRUE;
}

static gboolean film_strip_key_accel(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                     GdkModifierType modifier, gpointer data)
{
  dt_lib_module_t *m = darktable.view_manager->proxy.filmstrip.module;
  gboolean vs = dt_lib_is_visible(m);
  dt_lib_set_visible(m, !vs);
  return TRUE;
}


static gboolean export_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                          GdkModifierType modifier, gpointer data)
{
  dt_develop_t *dev = (dt_develop_t *)data;

  /* write history before exporting */
  dt_dev_write_history(dev);

  /* export current image */
  const int max_width = dt_conf_get_int("plugins/lighttable/export/width");
  const int max_height = dt_conf_get_int("plugins/lighttable/export/height");
  char *format_name = dt_conf_get_string("plugins/lighttable/export/format_name");
  char *storage_name = dt_conf_get_string("plugins/lighttable/export/storage_name");
  const int format_index = dt_imageio_get_index_of_format(dt_imageio_get_format_by_name(format_name));
  const int storage_index = dt_imageio_get_index_of_storage(dt_imageio_get_storage_by_name(storage_name));
  const gboolean high_quality = dt_conf_get_bool("plugins/lighttable/export/high_quality_processing");
  const gboolean upscale = dt_conf_get_bool("plugins/lighttable/export/upscale");
  char *style = dt_conf_get_string("plugins/lighttable/export/style");
  const gboolean style_append = dt_conf_get_bool("plugins/lighttable/export/style_append");
  dt_colorspaces_color_profile_type_t icc_type = dt_conf_get_int("plugins/lighttable/export/icctype");
  gchar *icc_filename = dt_conf_get_string("plugins/lighttable/export/iccprofile");
  dt_iop_color_intent_t icc_intent = dt_conf_get_int("plugins/lighttable/export/iccintent");
  // darkroom is for single images, so only export the one the user is working on
  GList *l = g_list_append(NULL, GINT_TO_POINTER(dev->image_storage.id));
  dt_control_export(l, max_width, max_height, format_index, storage_index, high_quality, upscale, style, style_append,
                    icc_type, icc_filename, icc_intent);
  g_free(format_name);
  g_free(storage_name);
  g_free(style);
  g_free(icc_filename);
  return TRUE;
}

static gboolean skip_f_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                          GdkModifierType modifier, gpointer data)
{
  dt_dev_jump_image((dt_develop_t *)data, 1);
  return TRUE;
}

static gboolean skip_b_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                          GdkModifierType modifier, gpointer data)
{
  dt_dev_jump_image((dt_develop_t *)data, -1);
  return TRUE;
}

static void _darkroom_ui_pipe_finish_signal_callback(gpointer instance, gpointer data)
{
  dt_control_queue_redraw();
}

static void _darkroom_ui_favorite_presets_popupmenu(GtkWidget *w, gpointer user_data)
{
  /* create favorites menu and popup */
  dt_gui_favorite_presets_menu_show();

  /* if we got any styles, lets popup menu for selection */
  if(darktable.gui->presets_popup_menu)
  {
    gtk_widget_show_all(GTK_WIDGET(darktable.gui->presets_popup_menu));

#if GTK_CHECK_VERSION(3, 22, 0)
    gtk_menu_popup_at_pointer(darktable.gui->presets_popup_menu, NULL);
#else
    gtk_menu_popup(darktable.gui->presets_popup_menu, NULL, NULL, NULL, NULL, 0, 0);
#endif
  }
  else
    dt_control_log(_("no userdefined presets for favorite modules were found"));
}

static void _darkroom_ui_apply_style_activate_callback(gchar *name)
{
  dt_control_log(_("applied style `%s' on current image"), name);

  /* write current history changes so nothing gets lost */
  dt_dev_write_history(darktable.develop);

  /* apply style on image and reload*/
  dt_styles_apply_to_image(name, FALSE, darktable.develop->image_storage.id);
  dt_dev_reload_image(darktable.develop, darktable.develop->image_storage.id);
}

static void _darkroom_ui_apply_style_popupmenu(GtkWidget *w, gpointer user_data)
{
  /* show styles popup menu */
  GList *styles = dt_styles_get_list("");
  GtkMenuShell *menu = NULL;
  if(styles)
  {
    menu = GTK_MENU_SHELL(gtk_menu_new());
    do
    {
      dt_style_t *style = (dt_style_t *)styles->data;
      GtkWidget *mi = gtk_menu_item_new_with_label(style->name);

      char *items_string = dt_styles_get_item_list_as_string(style->name);
      gchar *tooltip = NULL;

      if(style->description && *style->description)
      {
        tooltip = g_strconcat("<b>", style->description, "</b>\n", items_string, NULL);
      }
      else
      {
        tooltip = g_strdup(items_string);
      }

      gtk_widget_set_tooltip_markup(mi, tooltip);

      gtk_menu_shell_append(menu, mi);
      g_signal_connect_swapped(G_OBJECT(mi), "activate",
                               G_CALLBACK(_darkroom_ui_apply_style_activate_callback),
                               (gpointer)g_strdup(style->name));
      gtk_widget_show(mi);

      g_free(items_string);
      g_free(tooltip);
    } while((styles = g_list_next(styles)) != NULL);
    g_list_free_full(styles, dt_style_free);
  }

  /* if we got any styles, lets popup menu for selection */
  if(menu)
  {
#if GTK_CHECK_VERSION(3, 22, 0)
    gtk_menu_popup_at_pointer(GTK_MENU(menu), NULL);
#else
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, 0, 0);
#endif
  }
  else
    dt_control_log(_("no styles have been created yet"));
}

/** toolbar buttons */

static gboolean _toolbar_show_popup(gpointer user_data)
{
  gtk_widget_show_all(GTK_WIDGET(user_data));

  // cancel glib timeout if invoked by long button press
  return FALSE;
}

/* overexposed */
static void _overexposed_quickbutton_clicked(GtkWidget *w, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->overexposed.enabled = !d->overexposed.enabled;
  //   dt_dev_reprocess_center(d);
  dt_dev_reprocess_all(d);
}

static gboolean _overexposed_quickbutton_pressed(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  const GdkEventButton *e = (GdkEventButton *)event;
  if(e->button == 3)
  {
    _toolbar_show_popup(d->overexposed.floating_window);
    return TRUE;
  }
  else
  {
    d->overexposed.timeout = g_timeout_add_seconds(1, _toolbar_show_popup, d->overexposed.floating_window);
    return FALSE;
  }
}

static gboolean _overexposed_quickbutton_released(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  if(d->overexposed.timeout > 0) g_source_remove(d->overexposed.timeout);
  d->overexposed.timeout = 0;
  return FALSE;
}

static void colorscheme_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->overexposed.colorscheme = dt_bauhaus_combobox_get(combo);
  if(d->overexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->overexposed.button));
  else
    //     dt_dev_reprocess_center(d);
    dt_dev_reprocess_all(d);
}

static void lower_callback(GtkWidget *slider, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->overexposed.lower = dt_bauhaus_slider_get(slider);
  if(d->overexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->overexposed.button));
  else
    //     dt_dev_reprocess_center(d);
    dt_dev_reprocess_all(d);
}

static void upper_callback(GtkWidget *slider, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->overexposed.upper = dt_bauhaus_slider_get(slider);
  if(d->overexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->overexposed.button));
  else
    //     dt_dev_reprocess_center(d);
    dt_dev_reprocess_all(d);
}

/* rawoverexposed */
static void _rawoverexposed_quickbutton_clicked(GtkWidget *w, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->rawoverexposed.enabled = !d->rawoverexposed.enabled;
  //   dt_dev_reprocess_center(d);
  dt_dev_reprocess_all(d);
}

static gboolean _rawoverexposed_quickbutton_pressed(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  const GdkEventButton *e = (GdkEventButton *)event;
  if(e->button == 3)
  {
    _toolbar_show_popup(d->rawoverexposed.floating_window);
    return TRUE;
  }
  else
  {
    d->rawoverexposed.timeout = g_timeout_add_seconds(1, _toolbar_show_popup, d->rawoverexposed.floating_window);
    return FALSE;
  }
}

static gboolean _rawoverexposed_quickbutton_released(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  if(d->rawoverexposed.timeout > 0) g_source_remove(d->rawoverexposed.timeout);
  d->rawoverexposed.timeout = 0;
  return FALSE;
}

static void rawoverexposed_mode_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->rawoverexposed.mode = dt_bauhaus_combobox_get(combo);
  if(d->rawoverexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->rawoverexposed.button));
  else
    //     dt_dev_reprocess_center(d);
    dt_dev_reprocess_all(d);
}

static void rawoverexposed_colorscheme_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->rawoverexposed.colorscheme = dt_bauhaus_combobox_get(combo);
  if(d->rawoverexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->rawoverexposed.button));
  else
    //     dt_dev_reprocess_center(d);
    dt_dev_reprocess_all(d);
}

static void rawoverexposed_threshold_callback(GtkWidget *slider, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->rawoverexposed.threshold = dt_bauhaus_slider_get(slider);
  if(d->rawoverexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->rawoverexposed.button));
  else
    //     dt_dev_reprocess_center(d);
    dt_dev_reprocess_all(d);
}

static gboolean _toolbox_toggle_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                             GdkModifierType modifier, gpointer data)
{
  gtk_button_clicked(GTK_BUTTON(data));
  return TRUE;
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

  dt_dev_reprocess_all(d);
}

static gboolean _softproof_quickbutton_pressed(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  GdkEventButton *e = (GdkEventButton *)event;

  gtk_popover_set_relative_to(GTK_POPOVER(d->profile.floating_window), d->profile.softproof_button);

  if(e->button == 3)
  {
    _toolbar_show_popup(d->profile.floating_window);
    return TRUE;
  }
  else
  {
    d->profile.timeout = g_timeout_add_seconds(1, _toolbar_show_popup, d->profile.floating_window);
    return FALSE;
  }
}

static gboolean _profile_quickbutton_released(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  if(d->profile.timeout > 0) g_source_remove(d->profile.timeout);
  d->profile.timeout = 0;
  return FALSE;
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

  dt_dev_reprocess_all(d);
}

static gboolean _gamut_quickbutton_pressed(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  GdkEventButton *e = (GdkEventButton *)event;

  gtk_popover_set_relative_to(GTK_POPOVER(d->profile.floating_window), d->profile.gamut_button);

  if(e->button == 3)
  {
    _toolbar_show_popup(d->profile.floating_window);
    return TRUE;
  }
  else
  {
    d->profile.timeout = g_timeout_add_seconds(1, _toolbar_show_popup, d->profile.floating_window);
    return FALSE;
  }
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
  if(profile_changed) dt_dev_reprocess_all(d);
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
}

/** end of toolbox */

static gboolean _brush_size_up_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                        GdkModifierType modifier, gpointer data)
{
  dt_develop_t *dev = (dt_develop_t *)data;

  if(dev->form_visible) dt_masks_events_mouse_scrolled(dev->gui_module, 0, 0, 0, 0);
  return TRUE;
}
static gboolean _brush_size_down_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                          GdkModifierType modifier, gpointer data)
{
  dt_develop_t *dev = (dt_develop_t *)data;

  if(dev->form_visible) dt_masks_events_mouse_scrolled(dev->gui_module, 0, 0, 1, 0);
  return TRUE;
}

static gboolean _brush_hardness_up_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                            GdkModifierType modifier, gpointer data)
{
  dt_develop_t *dev = (dt_develop_t *)data;

  if(dev->form_visible) dt_masks_events_mouse_scrolled(dev->gui_module, 0, 0, 0, GDK_SHIFT_MASK);
  return TRUE;
}
static gboolean _brush_hardness_down_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                              GdkModifierType modifier, gpointer data)
{
  dt_develop_t *dev = (dt_develop_t *)data;

  if(dev->form_visible) dt_masks_events_mouse_scrolled(dev->gui_module, 0, 0, 1, GDK_SHIFT_MASK);
  return TRUE;
}

static gboolean _brush_opacity_up_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                           GdkModifierType modifier, gpointer data)
{
  dt_develop_t *dev = (dt_develop_t *)data;

  if(dev->form_visible) dt_masks_events_mouse_scrolled(dev->gui_module, 0, 0, 0, GDK_CONTROL_MASK);
  return TRUE;
}
static gboolean _brush_opacity_down_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                             GdkModifierType modifier, gpointer data)
{
  dt_develop_t *dev = (dt_develop_t *)data;

  if(dev->form_visible) dt_masks_events_mouse_scrolled(dev->gui_module, 0, 0, 1, GDK_CONTROL_MASK);
  return TRUE;
}

void gui_init(dt_view_t *self)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  /*
   * Add view specific tool buttons
   */

  /* create favorite plugin preset popup tool */
  GtkWidget *favorite_presets
      = dtgtk_button_new(dtgtk_cairo_paint_presets, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  gtk_widget_set_tooltip_text(favorite_presets, _("quick access to presets of your favorites"));
  g_signal_connect(G_OBJECT(favorite_presets), "clicked", G_CALLBACK(_darkroom_ui_favorite_presets_popupmenu),
                   NULL);
  dt_gui_add_help_link(favorite_presets, dt_get_help_url("favorite_presets"));
  dt_view_manager_view_toolbox_add(darktable.view_manager, favorite_presets, DT_VIEW_DARKROOM);

  /* create quick styles popup menu tool */
  GtkWidget *styles = dtgtk_button_new(dtgtk_cairo_paint_styles, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  g_signal_connect(G_OBJECT(styles), "clicked", G_CALLBACK(_darkroom_ui_apply_style_popupmenu), NULL);
  gtk_widget_set_tooltip_text(styles, _("quick access for applying any of your styles"));
  dt_gui_add_help_link(styles, dt_get_help_url("bottom_panel_styles"));
  dt_view_manager_view_toolbox_add(darktable.view_manager, styles, DT_VIEW_DARKROOM);

  const int panel_width = dt_conf_get_int("panel_width");

  /* create rawoverexposed popup tool */
  {
    // the button
    dev->rawoverexposed.button
        = dtgtk_togglebutton_new(dtgtk_cairo_paint_rawoverexposed, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
    gtk_widget_set_tooltip_text(dev->rawoverexposed.button,
                                _("toggle raw over exposed indication\nright click for options"));
    g_signal_connect(G_OBJECT(dev->rawoverexposed.button), "clicked",
                     G_CALLBACK(_rawoverexposed_quickbutton_clicked), dev);
    g_signal_connect(G_OBJECT(dev->rawoverexposed.button), "button-press-event",
                     G_CALLBACK(_rawoverexposed_quickbutton_pressed), dev);
    g_signal_connect(G_OBJECT(dev->rawoverexposed.button), "button-release-event",
                     G_CALLBACK(_rawoverexposed_quickbutton_released), dev);
    dt_view_manager_module_toolbox_add(darktable.view_manager, dev->rawoverexposed.button, DT_VIEW_DARKROOM);
    dt_gui_add_help_link(dev->rawoverexposed.button, dt_get_help_url("rawoverexposed"));

    // and the popup window
    dev->rawoverexposed.floating_window = gtk_popover_new(dev->rawoverexposed.button);
    gtk_widget_set_size_request(GTK_WIDGET(dev->rawoverexposed.floating_window), panel_width, -1);
#if GTK_CHECK_VERSION(3, 16, 0)
    g_object_set(G_OBJECT(dev->rawoverexposed.floating_window), "transitions-enabled", FALSE, NULL);
#endif

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_margin_start(vbox, DT_PIXEL_APPLY_DPI(8));
    gtk_widget_set_margin_end(vbox, DT_PIXEL_APPLY_DPI(8));
    gtk_widget_set_margin_top(vbox, DT_PIXEL_APPLY_DPI(8));
    gtk_widget_set_margin_bottom(vbox, DT_PIXEL_APPLY_DPI(8));

    gtk_container_add(GTK_CONTAINER(dev->rawoverexposed.floating_window), vbox);

    /** let's fill the encapsulating widgets */
    /* mode of operation */
    GtkWidget *mode = dt_bauhaus_combobox_new(NULL);
    dt_bauhaus_widget_set_label(mode, NULL, _("mode"));
    dt_bauhaus_combobox_add(mode, _("mark with CFA color"));
    dt_bauhaus_combobox_add(mode, _("mark with solid color"));
    dt_bauhaus_combobox_add(mode, _("false color"));
    dt_bauhaus_combobox_set(mode, dev->rawoverexposed.mode);
    gtk_widget_set_tooltip_text(mode, _("select how to mark the clipped pixels"));
    g_signal_connect(G_OBJECT(mode), "value-changed", G_CALLBACK(rawoverexposed_mode_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(mode), TRUE, TRUE, 0);
    gtk_widget_set_state_flags(mode, GTK_STATE_FLAG_SELECTED, TRUE);

    /* color scheme */
    GtkWidget *colorscheme = dt_bauhaus_combobox_new(NULL);
    dt_bauhaus_widget_set_label(colorscheme, NULL, _("color scheme"));
    dt_bauhaus_combobox_add(colorscheme, C_("solidcolor", "red"));
    dt_bauhaus_combobox_add(colorscheme, C_("solidcolor", "green"));
    dt_bauhaus_combobox_add(colorscheme, C_("solidcolor", "blue"));
    dt_bauhaus_combobox_add(colorscheme, C_("solidcolor", "black"));
    dt_bauhaus_combobox_set(colorscheme, dev->rawoverexposed.colorscheme);
    gtk_widget_set_tooltip_text(
        colorscheme,
        _("select the solid color to indicate over exposure.\nwill only be used if mode = mark with solid color"));
    g_signal_connect(G_OBJECT(colorscheme), "value-changed", G_CALLBACK(rawoverexposed_colorscheme_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(colorscheme), TRUE, TRUE, 0);
    gtk_widget_set_state_flags(colorscheme, GTK_STATE_FLAG_SELECTED, TRUE);

    /* threshold */
    GtkWidget *threshold = dt_bauhaus_slider_new_with_range(NULL, 0.0, 2.0, 0.01, 1.0, 3);
    dt_bauhaus_slider_set(threshold, dev->rawoverexposed.threshold);
    dt_bauhaus_widget_set_label(threshold, NULL, _("clipping threshold"));
    gtk_widget_set_tooltip_text(
        threshold, _("threshold of what shall be considered overexposed\n1.0 - white level\n0.0 - black level"));
    g_signal_connect(G_OBJECT(threshold), "value-changed", G_CALLBACK(rawoverexposed_threshold_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(threshold), TRUE, TRUE, 0);
  }

  /* create overexposed popup tool */
  {
    // the button
    dev->overexposed.button
        = dtgtk_togglebutton_new(dtgtk_cairo_paint_overexposed, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
    gtk_widget_set_tooltip_text(dev->overexposed.button,
                                _("toggle over/under exposed indication\nright click for options"));
    g_signal_connect(G_OBJECT(dev->overexposed.button), "clicked",
                     G_CALLBACK(_overexposed_quickbutton_clicked), dev);
    g_signal_connect(G_OBJECT(dev->overexposed.button), "button-press-event",
                     G_CALLBACK(_overexposed_quickbutton_pressed), dev);
    g_signal_connect(G_OBJECT(dev->overexposed.button), "button-release-event",
                     G_CALLBACK(_overexposed_quickbutton_released), dev);
    dt_view_manager_module_toolbox_add(darktable.view_manager, dev->overexposed.button, DT_VIEW_DARKROOM);
    dt_gui_add_help_link(dev->overexposed.button, dt_get_help_url("overexposed"));

    // and the popup window
    dev->overexposed.floating_window = gtk_popover_new(dev->overexposed.button);
    gtk_widget_set_size_request(GTK_WIDGET(dev->overexposed.floating_window), panel_width, -1);
#if GTK_CHECK_VERSION(3, 16, 0)
    g_object_set(G_OBJECT(dev->overexposed.floating_window), "transitions-enabled", FALSE, NULL);
#endif

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_margin_start(vbox, DT_PIXEL_APPLY_DPI(8));
    gtk_widget_set_margin_end(vbox, DT_PIXEL_APPLY_DPI(8));
    gtk_widget_set_margin_top(vbox, DT_PIXEL_APPLY_DPI(8));
    gtk_widget_set_margin_bottom(vbox, DT_PIXEL_APPLY_DPI(8));

    gtk_container_add(GTK_CONTAINER(dev->overexposed.floating_window), vbox);

    /** let's fill the encapsulating widgets */
    /* color scheme */
    GtkWidget *colorscheme = dt_bauhaus_combobox_new(NULL);
    dt_bauhaus_widget_set_label(colorscheme, NULL, _("color scheme"));
    dt_bauhaus_combobox_add(colorscheme, _("black & white"));
    dt_bauhaus_combobox_add(colorscheme, _("red & blue"));
    dt_bauhaus_combobox_add(colorscheme, _("purple & green"));
    dt_bauhaus_combobox_set(colorscheme, dev->overexposed.colorscheme);
    gtk_widget_set_tooltip_text(colorscheme, _("select colors to indicate over/under exposure"));
    g_signal_connect(G_OBJECT(colorscheme), "value-changed", G_CALLBACK(colorscheme_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(colorscheme), TRUE, TRUE, 0);
    gtk_widget_set_state_flags(colorscheme, GTK_STATE_FLAG_SELECTED, TRUE);

    /* lower */
    GtkWidget *lower = dt_bauhaus_slider_new_with_range(NULL, 0.0, 100.0, 0.1, 2.0, 2);
    dt_bauhaus_slider_set(lower, dev->overexposed.lower);
    dt_bauhaus_slider_set_format(lower, "%.0f%%");
    dt_bauhaus_widget_set_label(lower, NULL, _("lower threshold"));
    gtk_widget_set_tooltip_text(lower, _("threshold of what shall be considered underexposed"));
    g_signal_connect(G_OBJECT(lower), "value-changed", G_CALLBACK(lower_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(lower), TRUE, TRUE, 0);

    /* upper */
    GtkWidget *upper = dt_bauhaus_slider_new_with_range(NULL, 0.0, 100.0, 0.1, 98.0, 2);
    dt_bauhaus_slider_set(upper, dev->overexposed.upper);
    dt_bauhaus_slider_set_format(upper, "%.0f%%");
    dt_bauhaus_widget_set_label(upper, NULL, _("upper threshold"));
    gtk_widget_set_tooltip_text(upper, _("threshold of what shall be considered overexposed"));
    g_signal_connect(G_OBJECT(upper), "value-changed", G_CALLBACK(upper_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(upper), TRUE, TRUE, 0);
  }

  /* create profile popup tool & buttons (softproof + gamut) */
  {
    // the softproof button
    dev->profile.softproof_button
    = dtgtk_togglebutton_new(dtgtk_cairo_paint_softproof, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
    gtk_widget_set_tooltip_text(dev->profile.softproof_button,
                                _("toggle softproofing\nright click for profile options"));
    g_signal_connect(G_OBJECT(dev->profile.softproof_button), "clicked",
                     G_CALLBACK(_softproof_quickbutton_clicked), dev);
    g_signal_connect(G_OBJECT(dev->profile.softproof_button), "button-press-event",
                     G_CALLBACK(_softproof_quickbutton_pressed), dev);
    g_signal_connect(G_OBJECT(dev->profile.softproof_button), "button-release-event",
                     G_CALLBACK(_profile_quickbutton_released), dev);
    dt_view_manager_module_toolbox_add(darktable.view_manager, dev->profile.softproof_button, DT_VIEW_DARKROOM);
    dt_gui_add_help_link(dev->profile.softproof_button, dt_get_help_url("softproof"));

    // the gamut check button
    dev->profile.gamut_button
    = dtgtk_togglebutton_new(dtgtk_cairo_paint_gamut_check, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
    gtk_widget_set_tooltip_text(dev->profile.gamut_button,
                 _("toggle gamut checking\nright click for profile options"));
    g_signal_connect(G_OBJECT(dev->profile.gamut_button), "clicked",
                     G_CALLBACK(_gamut_quickbutton_clicked), dev);
    g_signal_connect(G_OBJECT(dev->profile.gamut_button), "button-press-event",
                     G_CALLBACK(_gamut_quickbutton_pressed), dev);
    g_signal_connect(G_OBJECT(dev->profile.gamut_button), "button-release-event",
                     G_CALLBACK(_profile_quickbutton_released), dev);
    dt_view_manager_module_toolbox_add(darktable.view_manager, dev->profile.gamut_button, DT_VIEW_DARKROOM);
    dt_gui_add_help_link(dev->profile.gamut_button, dt_get_help_url("gamut"));

    // and the popup window, which is shared between the two profile buttons
    dev->profile.floating_window = gtk_popover_new(NULL);
    gtk_widget_set_size_request(GTK_WIDGET(dev->profile.floating_window), panel_width, -1);
#if GTK_CHECK_VERSION(3, 16, 0)
    g_object_set(G_OBJECT(dev->profile.floating_window), "transitions-enabled", FALSE, NULL);
#endif

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_margin_start(vbox, DT_PIXEL_APPLY_DPI(8));
    gtk_widget_set_margin_end(vbox, DT_PIXEL_APPLY_DPI(8));
    gtk_widget_set_margin_top(vbox, DT_PIXEL_APPLY_DPI(8));
    gtk_widget_set_margin_bottom(vbox, DT_PIXEL_APPLY_DPI(8));

    gtk_container_add(GTK_CONTAINER(dev->profile.floating_window), vbox);

    /** let's fill the encapsulating widgets */
    char datadir[PATH_MAX] = { 0 };
    char confdir[PATH_MAX] = { 0 };
    dt_loc_get_user_config_dir(confdir, sizeof(confdir));
    dt_loc_get_datadir(datadir, sizeof(datadir));
    const int force_lcms2 = dt_conf_get_bool("plugins/lighttable/export/force_lcms2");

    GtkWidget *display_intent = dt_bauhaus_combobox_new(NULL);
    dt_bauhaus_widget_set_label(display_intent, NULL, _("display intent"));
    gtk_box_pack_start(GTK_BOX(vbox), display_intent, TRUE, TRUE, 0);
    dt_bauhaus_combobox_add(display_intent, _("perceptual"));
    dt_bauhaus_combobox_add(display_intent, _("relative colorimetric"));
    dt_bauhaus_combobox_add(display_intent, C_("rendering intent", "saturation"));
    dt_bauhaus_combobox_add(display_intent, _("absolute colorimetric"));

    if(!force_lcms2)
    {
      gtk_widget_set_no_show_all(display_intent, TRUE);
      gtk_widget_set_visible(display_intent, FALSE);
    }

    GtkWidget *display_profile = dt_bauhaus_combobox_new(NULL);
    GtkWidget *softproof_profile = dt_bauhaus_combobox_new(NULL);
    dt_bauhaus_widget_set_label(softproof_profile, NULL, _("softproof profile"));
    dt_bauhaus_widget_set_label(display_profile, NULL, _("display profile"));
    gtk_box_pack_start(GTK_BOX(vbox), softproof_profile, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), display_profile, TRUE, TRUE, 0);

    GList *l = darktable.color_profiles->profiles;
    while(l)
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
      // the system display profile is only suitable for display purposes
      if(prof->out_pos > -1)
      {
        dt_bauhaus_combobox_add(softproof_profile, prof->name);
        if(prof->type == darktable.color_profiles->softproof_type
          && (prof->type != DT_COLORSPACE_FILE
              || !strcmp(prof->filename, darktable.color_profiles->softproof_filename)))
          dt_bauhaus_combobox_set(softproof_profile, prof->out_pos);
      }
      l = g_list_next(l);
    }

    char *system_profile_dir = g_build_filename(datadir, "color", "out", NULL);
    char *user_profile_dir = g_build_filename(confdir, "color", "out", NULL);
    char *tooltip = g_strdup_printf(_("display ICC profiles in %s or %s"), user_profile_dir, system_profile_dir);
    gtk_widget_set_tooltip_text(display_profile, tooltip);
    g_free(tooltip);
    tooltip = g_strdup_printf(_("softproof ICC profiles in %s or %s"), user_profile_dir, system_profile_dir);
    gtk_widget_set_tooltip_text(softproof_profile, tooltip);
    g_free(tooltip);
    g_free(system_profile_dir);
    g_free(user_profile_dir);

    g_signal_connect(G_OBJECT(display_intent), "value-changed", G_CALLBACK(display_intent_callback), dev);
    g_signal_connect(G_OBJECT(display_profile), "value-changed", G_CALLBACK(display_profile_callback), dev);
    g_signal_connect(G_OBJECT(softproof_profile), "value-changed", G_CALLBACK(softproof_profile_callback), dev);

    _update_softproof_gamut_checking(dev);

    // update the gui when the preferences changed (i.e. show intent when using lcms2)
    dt_control_signal_connect(darktable.signals, DT_SIGNAL_PREFERENCES_CHANGE,
                              G_CALLBACK(_preference_changed), (gpointer)display_intent);
  }
}

void enter(dt_view_t *self)
{
  // clean the undo list
  dt_undo_clear(darktable.undo, DT_UNDO_DEVELOP);

  /* connect to ui pipe finished signal for redraw */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED,
                            G_CALLBACK(_darkroom_ui_pipe_finish_signal_callback), (gpointer)self);

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

  select_this_image(dev->image_storage.id);

  dt_control_set_dev_zoom(DT_ZOOM_FIT);
  dt_control_set_dev_zoom_x(0);
  dt_control_set_dev_zoom_y(0);
  dt_control_set_dev_closeup(0);

  // take a copy of the image struct for convenience.

  dt_dev_load_image(darktable.develop, dev->image_storage.id);


  /*
   * add IOP modules to plugin list
   */
  // avoid triggering of events before plugin is ready:
  darktable.gui->reset = 1;
  char option[1024];
  GList *modules = g_list_last(dev->iop);
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);

    /* initialize gui if iop have one defined */
    if(!dt_iop_is_hidden(module))
    {
      module->gui_init(module);
      dt_iop_reload_defaults(module);

      /* add module to right panel */
      GtkWidget *expander = dt_iop_gui_get_expander(module);
      dt_ui_container_add_widget(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER, expander);

      snprintf(option, sizeof(option), "plugins/darkroom/%s/expanded", module->op);
      dt_iop_gui_set_expanded(module, dt_conf_get_bool(option), FALSE);
    }

    /* setup key accelerators */
    module->accel_closures = NULL;
    if(module->connect_key_accels) module->connect_key_accels(module);
    dt_iop_connect_common_accels(module);

    modules = g_list_previous(modules);
  }
  darktable.gui->reset = 0;

  /* signal that darktable.develop is initialized and ready to be used */
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_DEVELOP_INITIALIZE);

  // synch gui and flag pipe as dirty
  // this is done here and not in dt_read_history, as it would else be triggered before module->gui_init.
  dt_dev_pop_history_items(dev, dev->history_end);

  /* ensure that filmstrip shows current image */
  dt_view_filmstrip_scroll_to_image(darktable.view_manager, dev->image_storage.id, FALSE);

  // switch on groups as they were last time:
  dt_dev_modulegroups_set(dev, dt_conf_get_int("plugins/darkroom/groups"));

  // make signals work again:
  darktable.gui->reset = 0;

  // get last active plugin:
  gchar *active_plugin = dt_conf_get_string("plugins/darkroom/active");
  if(active_plugin)
  {
    modules = dev->iop;
    while(modules)
    {
      dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
      if(!strcmp(module->op, active_plugin)) dt_iop_request_focus(module);
      modules = g_list_next(modules);
    }
    g_free(active_plugin);
  }
  dt_dev_masks_list_change(dev);

  // image should be there now.
  float zoom_x, zoom_y;
  dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, DT_ZOOM_FIT, 0, NULL, NULL);
  dt_control_set_dev_zoom_x(zoom_x);
  dt_control_set_dev_zoom_y(zoom_y);

  /* connect signal for filmstrip image activate */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_VIEWMANAGER_FILMSTRIP_ACTIVATE,
                            G_CALLBACK(_view_darkroom_filmstrip_activate_callback), self);

  // prefetch next few from first selected image on.
  dt_view_filmstrip_prefetch();

  dt_collection_hint_message(darktable.collection);

  char *scrollbars_conf = dt_conf_get_string("scrollbars");

  gboolean scrollbars_visible = FALSE;
  if(scrollbars_conf)
  {
    if(!strcmp(scrollbars_conf, "lighttable + darkroom"))
      scrollbars_visible = TRUE;
    g_free(scrollbars_conf);
  }

  dt_ui_scrollbars_show(darktable.gui->ui, scrollbars_visible);
}

void leave(dt_view_t *self)
{
  /* disconnect from filmstrip image activate */
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_view_darkroom_filmstrip_activate_callback),
                               (gpointer)self);

  /* disconnect from pipe finish signal */
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_darkroom_ui_pipe_finish_signal_callback),
                               (gpointer)self);

  // store groups for next time:
  dt_conf_set_int("plugins/darkroom/groups", dt_dev_modulegroups_get(darktable.develop));

  // store last active plugin:
  if(darktable.develop->gui_module)
    dt_conf_set_string("plugins/darkroom/active", darktable.develop->gui_module->op);
  else
    dt_conf_set_string("plugins/darkroom/active", "");

  dt_develop_t *dev = (dt_develop_t *)self->data;

  // tag image as changed
  // TODO: only tag the image when there was a real change.
  guint tagid = 0;
  dt_tag_new_from_gui("darktable|changed", &tagid);
  dt_tag_attach(tagid, dev->image_storage.id);
  // commit image ops to db
  dt_dev_write_history(dev);

  // be sure light table will regenerate the thumbnail:
  // TODO: only if changed!
  // if()
  {
    dt_mipmap_cache_remove(darktable.mipmap_cache, dev->image_storage.id);
    // dump new xmp data
    dt_image_synch_xmp(dev->image_storage.id);
  }

  // update possibly changed aspect ratio
  dt_image_set_aspect_ratio(dev->image_storage.id);

  // clear gui.

  dt_pthread_mutex_lock(&dev->preview_pipe_mutex);
  dt_pthread_mutex_lock(&dev->pipe_mutex);

  dev->gui_leaving = 1;

  dt_dev_pixelpipe_cleanup_nodes(dev->pipe);
  dt_dev_pixelpipe_cleanup_nodes(dev->preview_pipe);

  dt_pthread_mutex_lock(&dev->history_mutex);
  while(dev->history)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(dev->history->data);
    // printf("removing history item %d - %s, data %f %f\n", hist->module->instance, hist->module->op, *(float
    // *)hist->params, *((float *)hist->params+1));
    free(hist->params);
    hist->params = NULL;
    free(hist->blend_params);
    hist->blend_params = NULL;
    free(hist);
    dev->history = g_list_delete_link(dev->history, dev->history);
  }

  while(dev->iop)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(dev->iop->data);
    if(!dt_iop_is_hidden(module)) dt_iop_gui_cleanup_module(module);

    dt_dev_cleanup_module_accels(module);
    module->accel_closures = NULL;
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

  dt_pthread_mutex_unlock(&dev->history_mutex);

  dt_pthread_mutex_unlock(&dev->pipe_mutex);
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

  // take care of the overexposed window
  if(dev->overexposed.timeout > 0) g_source_remove(dev->overexposed.timeout);
  gtk_widget_hide(dev->overexposed.floating_window);
  gtk_widget_hide(dev->profile.floating_window);

  dt_ui_scrollbars_show(darktable.gui->ui, FALSE);

  dt_print(DT_DEBUG_CONTROL, "[run_job-] 11 %f in darkroom mode\n", dt_get_wtime());
}

void mouse_leave(dt_view_t *self)
{
  // if we are not hovering over a thumbnail in the filmstrip -> show metadata of opened image.
  dt_develop_t *dev = (dt_develop_t *)self->data;
  dt_control_set_mouse_over_id(dev->image_storage.id);

  // masks
  int handled = dt_masks_events_mouse_leave(dev->gui_module);
  if(handled) return;
  // module
  if(dev->gui_module && dev->gui_module->mouse_leave)
    handled = dev->gui_module->mouse_leave(dev->gui_module);

  // reset any changes the selected plugin might have made.
  dt_control_change_cursor(GDK_LEFT_PTR);
}

void mouse_moved(dt_view_t *self, double x, double y, double pressure, int which)
{
  const int32_t tb = DT_PIXEL_APPLY_DPI(dt_conf_get_int("plugins/darkroom/ui/border_size"));
  const int32_t capwd = self->width  - 2*tb;
  const int32_t capht = self->height - 2*tb;
  dt_develop_t *dev = (dt_develop_t *)self->data;

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
  int32_t offx = 0.0f, offy = 0.0f;
  if(width_i > capwd) offx = (capwd - width_i) * .5f;
  if(height_i > capht) offy = (capht - height_i) * .5f;
  int handled = 0;
  x += offx;
  y += offy;

  if(dev->gui_module && dev->gui_module->request_color_pick != DT_REQUEST_COLORPICK_OFF && ctl->button_down
     && ctl->button_down_which == 1)
  {
    // module requested a color box
    float zoom_x, zoom_y, bzoom_x, bzoom_y;
    dt_dev_get_pointer_zoom_pos(dev, x, y, &zoom_x, &zoom_y);
    dt_dev_get_pointer_zoom_pos(dev, ctl->button_x + offx, ctl->button_y + offy, &bzoom_x, &bzoom_y);
    if(darktable.lib->proxy.colorpicker.size)
    {
      dev->gui_module->color_picker_box[0] = fmaxf(0.0, fminf(.5f + bzoom_x, .5f + zoom_x));
      dev->gui_module->color_picker_box[1] = fmaxf(0.0, fminf(.5f + bzoom_y, .5f + zoom_y));
      dev->gui_module->color_picker_box[2] = fminf(1.0, fmaxf(.5f + bzoom_x, .5f + zoom_x));
      dev->gui_module->color_picker_box[3] = fminf(1.0, fmaxf(.5f + bzoom_y, .5f + zoom_y));
    }
    else
    {
      dev->gui_module->color_picker_point[0] = .5f + zoom_x;
      dev->gui_module->color_picker_point[1] = .5f + zoom_y;
    }

    dev->preview_pipe->changed |= DT_DEV_PIPE_SYNCH;
    dt_dev_invalidate_all(dev);
    dt_control_queue_redraw();
    return;
  }
  // masks
  handled = dt_masks_events_mouse_moved(dev->gui_module, x, y, pressure, which);
  if(handled) return;
  // module
  if(dev->gui_module && dev->gui_module->mouse_moved)
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
    dt_control_queue_redraw();
  }
}


int button_released(dt_view_t *self, double x, double y, int which, uint32_t state)
{
  const int32_t tb = DT_PIXEL_APPLY_DPI(dt_conf_get_int("plugins/darkroom/ui/border_size"));
  const int32_t capwd = self->width  - 2*tb;
  const int32_t capht = self->height - 2*tb;
  dt_develop_t *dev = darktable.develop;
  const int32_t width_i = self->width;
  const int32_t height_i = self->height;
  if(width_i > capwd) x += (capwd - width_i) * .5f;
  if(height_i > capht) y += (capht - height_i) * .5f;

  int handled = 0;
  // masks
  if(dev->form_visible) handled = dt_masks_events_button_released(dev->gui_module, x, y, which, state);
  if(handled) return handled;
  // module
  if(dev->gui_module && dev->gui_module->button_released)
    handled = dev->gui_module->button_released(dev->gui_module, x, y, which, state);
  if(handled) return handled;
  if(which == 1) dt_control_change_cursor(GDK_LEFT_PTR);
  return 1;
}


int button_pressed(dt_view_t *self, double x, double y, double pressure, int which, int type, uint32_t state)
{
  const int32_t tb = DT_PIXEL_APPLY_DPI(dt_conf_get_int("plugins/darkroom/ui/border_size"));
  const int32_t capwd = self->width  - 2*tb;
  const int32_t capht = self->height - 2*tb;
  dt_develop_t *dev = (dt_develop_t *)self->data;
  const int32_t width_i = self->width;
  const int32_t height_i = self->height;
  if(width_i > capwd) x += (capwd - width_i) * .5f;
  if(height_i > capht) y += (capht - height_i) * .5f;

  int handled = 0;
  if(dev->gui_module && dev->gui_module->request_color_pick != DT_REQUEST_COLORPICK_OFF && which == 1)
  {
    float zoom_x, zoom_y;
    dt_dev_get_pointer_zoom_pos(dev, x, y, &zoom_x, &zoom_y);
    if(darktable.lib->proxy.colorpicker.size)
    {
      dev->gui_module->color_picker_box[0] = .5f + zoom_x;
      dev->gui_module->color_picker_box[1] = .5f + zoom_y;
      dev->gui_module->color_picker_box[2] = .5f + zoom_x;
      dev->gui_module->color_picker_box[3] = .5f + zoom_y;
    }
    else
    {
      dev->gui_module->color_picker_point[0] = .5f + zoom_x;
      dev->gui_module->color_picker_point[1] = .5f + zoom_y;
      dev->preview_pipe->changed |= DT_DEV_PIPE_SYNCH;
      dt_dev_invalidate_all(dev);
    }
    dt_control_queue_redraw();
    return 1;
  }
  // masks
  if(dev->form_visible)
    handled = dt_masks_events_button_pressed(dev->gui_module, x, y, pressure, which, type, state);
  if(handled) return handled;
  // module
  if(dev->gui_module && dev->gui_module->button_pressed)
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
    dt_dev_zoom_t zoom;
    int closeup, procw, proch;
    float zoom_x, zoom_y;
    zoom = dt_control_get_dev_zoom();
    closeup = dt_control_get_dev_closeup();
    zoom_x = dt_control_get_dev_zoom_x();
    zoom_y = dt_control_get_dev_zoom_y();
    dt_dev_get_processed_size(dev, &procw, &proch);
    const float scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 0);
    zoom_x += (1.0 / scale) * (x - .5f * dev->width) / procw;
    zoom_y += (1.0 / scale) * (y - .5f * dev->height) / proch;
    if(zoom == DT_ZOOM_1)
    {
      if(!closeup)
        closeup = 1;
      else
      {
        zoom = DT_ZOOM_FIT;
        zoom_x = zoom_y = 0.0f;
        closeup = 0;
      }
    }
    else
      zoom = DT_ZOOM_1;
    dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, zoom, closeup, NULL, NULL);
    dt_control_set_dev_zoom(zoom);
    dt_control_set_dev_closeup(closeup);
    dt_control_set_dev_zoom_x(zoom_x);
    dt_control_set_dev_zoom_y(zoom_y);
    dt_dev_invalidate(dev);
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
  dt_control_queue_redraw();
}

void scrolled(dt_view_t *self, double x, double y, int up, int state)
{
  const int32_t tb = DT_PIXEL_APPLY_DPI(dt_conf_get_int("plugins/darkroom/ui/border_size"));
  const int32_t capwd = self->width  - 2*tb;
  const int32_t capht = self->height - 2*tb;
  dt_develop_t *dev = (dt_develop_t *)self->data;
  const int32_t width_i = self->width;
  const int32_t height_i = self->height;
  if(width_i > capwd) x += (capwd - width_i) * .5f;
  if(height_i > capht) y += (capht - height_i) * .5f;

  int handled = 0;
  // masks
  if(dev->form_visible) handled = dt_masks_events_mouse_scrolled(dev->gui_module, x, y, up, state);
  if(handled) return;
  // module
  if(dev->gui_module && dev->gui_module->scrolled)
    handled = dev->gui_module->scrolled(dev->gui_module, x, y, up, state);
  if(handled) return;
  // free zoom
  dt_dev_zoom_t zoom;
  int closeup, procw, proch;
  float zoom_x, zoom_y;
  zoom = dt_control_get_dev_zoom();
  closeup = dt_control_get_dev_closeup();
  zoom_x = dt_control_get_dev_zoom_x();
  zoom_y = dt_control_get_dev_zoom_y();
  dt_dev_get_processed_size(dev, &procw, &proch);
  float scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 0);
  const float fitscale = dt_dev_get_zoom_scale(dev, DT_ZOOM_FIT, 1.0, 0);
  float oldscale = scale;

  // offset from center now (current zoom_{x,y} points there)
  float mouse_off_x = x - .5 * dev->width, mouse_off_y = y - .5 * dev->height;
  zoom_x += mouse_off_x / (procw * scale);
  zoom_y += mouse_off_y / (proch * scale);
  zoom = DT_ZOOM_FREE;
  closeup = 0;
  if(up)
  {
    if(scale == 1.0f && !((state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK)) return;
    if(scale >= 2.0f)
      return;
    else if(scale < fitscale)
      scale += .05f * (1.0f - fitscale);
    else
      scale += .1f * (1.0f - fitscale);
  }
  else
  {
    if(scale == fitscale && !((state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK))
      return;
    else if(scale < 0.5 * fitscale)
      return;
    else if(scale <= fitscale)
      scale -= .05f * (1.0f - fitscale);
    else
      scale -= .1f * (1.0f - fitscale);
  }
  // we want to be sure to stop at 1:1 and FIT levels
  if((scale - 1.0) * (oldscale - 1.0) < 0) scale = 1.0f;
  if((scale - fitscale) * (oldscale - fitscale) < 0) scale = fitscale;
  scale = fmaxf(fminf(scale, 2.0f), 0.5 * fitscale);

  // for 200% zoom we want pixel doubling instead of interpolation
  if(scale > 1.9999f)
  {
    scale = 1.0f; // don't interpolate
    closeup = 1;  // enable closeup mode (pixel doubling)
  }

  dt_control_set_dev_zoom_scale(scale);
  if(fabsf(scale - 1.0f) < 0.001f) zoom = DT_ZOOM_1;
  if(fabsf(scale - fitscale) < 0.001f) zoom = DT_ZOOM_FIT;
  zoom_x -= mouse_off_x / (procw * scale);
  zoom_y -= mouse_off_y / (proch * scale);
  dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, zoom, closeup, NULL, NULL);
  dt_control_set_dev_zoom(zoom);
  dt_control_set_dev_closeup(closeup);
  dt_control_set_dev_zoom_x(zoom_x);
  dt_control_set_dev_zoom_y(zoom_y);

  dt_dev_invalidate(dev);

  dt_control_queue_redraw();
}

int key_released(dt_view_t *self, guint key, guint state)
{
  const dt_control_accels_t *accels = &darktable.control->accels;
  dt_develop_t *lib = (dt_develop_t *)self->data;

  if(!darktable.control->key_accelerators_on)
    return 0;

  if(key == accels->darkroom_preview.accel_key && state == accels->darkroom_preview.accel_mods && lib->full_preview)
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
  }
  // add an option to allow skip mouse events while editing masks
  if(key == accels->darkroom_skip_mouse_events.accel_key && state == accels->darkroom_skip_mouse_events.accel_mods)
  {
    darktable.develop->darkroom_skip_mouse_events = FALSE;
  }

  return 1;
}

int key_pressed(dt_view_t *self, guint key, guint state)
{
  const dt_control_accels_t *accels = &darktable.control->accels;
  dt_develop_t *lib = (dt_develop_t *)self->data;

  if(!darktable.control->key_accelerators_on)
    return 0;

  if(key == accels->darkroom_preview.accel_key && state == accels->darkroom_preview.accel_mods)
  {
    if(!lib->full_preview)
    {
      lib->full_preview = TRUE;
      // we hide all panels
      for(int k = 0; k < DT_UI_PANEL_SIZE; k++)
        dt_ui_panel_show(darktable.gui->ui, k, FALSE, FALSE);
      // we remember the masks edit state
      if(darktable.develop->gui_module)
      {
        dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)darktable.develop->gui_module->blend_data;
        if (bd) lib->full_preview_masks_state = bd->masks_shown;
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
      // and we redraw all
      dt_dev_invalidate(darktable.develop);
      dt_control_queue_redraw_center();
    }
    else
      return 0;
  }

  if(key == accels->global_zoom_in.accel_key && state == accels->global_zoom_in.accel_mods)
  {
    dt_develop_t *dev = (dt_develop_t *)self->data;

    scrolled(self, dev->width / 2, dev->height / 2, 1, state);
    return 1;
  }

  if(key == accels->global_zoom_out.accel_key && state == accels->global_zoom_out.accel_mods)
  {
    dt_develop_t *dev = (dt_develop_t *)self->data;

    scrolled(self, dev->width / 2, dev->height / 2, 0, state);
    return 1;
  }

  if(key == GDK_KEY_Left || key == GDK_KEY_Right || key == GDK_KEY_Up || key == GDK_KEY_Down)
  {
    dt_develop_t *dev = (dt_develop_t *)self->data;
    dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
    const int closeup = dt_control_get_dev_closeup();
    float scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 0);
    int procw, proch;
    dt_dev_get_processed_size(dev, &procw, &proch);

    GdkModifierType modifiers;
    modifiers = gtk_accelerator_get_default_mod_mask();

    // For each cursor press, move one screen by default
    float step_changex = dev->width / (procw * scale);
    float step_changey = dev->height / (proch * scale);
    float factor = 0.2f;

    if((state & modifiers) == GDK_MOD1_MASK) factor = 0.02f;
    if((state & modifiers) == GDK_CONTROL_MASK) factor = 1.0f;

    float old_zoom_x, old_zoom_y;

    old_zoom_x = dt_control_get_dev_zoom_x();
    old_zoom_y = dt_control_get_dev_zoom_y();

    float zx = old_zoom_x;
    float zy = old_zoom_y;

    if(key == GDK_KEY_Left) zx = zx - step_changex * factor;
    if(key == GDK_KEY_Right) zx = zx + step_changex * factor;
    if(key == GDK_KEY_Up) zy = zy - step_changey * factor;
    if(key == GDK_KEY_Down) zy = zy + step_changey * factor;

    dt_dev_check_zoom_bounds(dev, &zx, &zy, zoom, closeup, NULL, NULL);
    dt_control_set_dev_zoom_x(zx);
    dt_control_set_dev_zoom_y(zy);

    dt_dev_invalidate(dev);
    dt_control_queue_redraw();

    return 1;
  }

  // add an option to allow skip mouse events while editing masks
  if(key == accels->darkroom_skip_mouse_events.accel_key && state == accels->darkroom_skip_mouse_events.accel_mods)
  {
    darktable.develop->darkroom_skip_mouse_events = TRUE;
    return 1;
  }

  return 1;
}


void configure(dt_view_t *self, int wd, int ht)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  dt_dev_configure(dev, wd, ht);
}

void init_key_accels(dt_view_t *self)
{
  // Film strip shortcuts
  dt_accel_register_view(self, NC_("accel", "toggle film strip"), GDK_KEY_f, GDK_CONTROL_MASK);

  // Zoom shortcuts
  dt_accel_register_view(self, NC_("accel", "zoom close-up"), GDK_KEY_1, GDK_MOD1_MASK);
  dt_accel_register_view(self, NC_("accel", "zoom fill"), GDK_KEY_2, GDK_MOD1_MASK);
  dt_accel_register_view(self, NC_("accel", "zoom fit"), GDK_KEY_3, GDK_MOD1_MASK);

  // enable shortcut to export with current export settings:
  dt_accel_register_view(self, NC_("accel", "export"), GDK_KEY_e, GDK_CONTROL_MASK);

  // Shortcut to skip images
  dt_accel_register_view(self, NC_("accel", "image forward"), GDK_KEY_space, 0);
  dt_accel_register_view(self, NC_("accel", "image back"), GDK_KEY_BackSpace, 0);

  // toggle raw overexposure indication
  dt_accel_register_view(self, NC_("accel", "raw overexposed"), GDK_KEY_o, GDK_SHIFT_MASK);

  // toggle overexposure indication
  dt_accel_register_view(self, NC_("accel", "overexposed"), GDK_KEY_o, 0);

  // toggle softproofing
  dt_accel_register_view(self, NC_("accel", "softproof"), GDK_KEY_s, GDK_CONTROL_MASK);

  // toggle gamut check
  dt_accel_register_view(self, NC_("accel", "gamut check"), GDK_KEY_g, GDK_CONTROL_MASK);

  // brush size +/-
  dt_accel_register_view(self, NC_("accel", "increase brush size"), GDK_KEY_bracketright, 0);
  dt_accel_register_view(self, NC_("accel", "decrease brush size"), GDK_KEY_bracketleft, 0);

  // brush hardness +/-
  dt_accel_register_view(self, NC_("accel", "increase brush hardness"), GDK_KEY_braceright, 0);
  dt_accel_register_view(self, NC_("accel", "decrease brush hardness"), GDK_KEY_braceleft, 0);

  // brush opacity +/-
  dt_accel_register_view(self, NC_("accel", "increase brush opacity"), GDK_KEY_greater, 0);
  dt_accel_register_view(self, NC_("accel", "decrease brush opacity"), GDK_KEY_less, 0);

  // fullscreen view
  dt_accel_register_view(self, NC_("accel", "full preview"), GDK_KEY_z, 0);

  // undo/redo
  dt_accel_register_view(self, NC_("accel", "undo"), GDK_KEY_z, GDK_CONTROL_MASK);
  dt_accel_register_view(self, NC_("accel", "redo"), GDK_KEY_y, GDK_CONTROL_MASK);

  // add an option to allow skip mouse events while editing masks
  dt_accel_register_view(self, NC_("accel", "allow to pan & zoom while editing masks"), GDK_KEY_a, 0);
}

static gboolean _darkroom_undo_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                        GdkModifierType modifier, gpointer data)
{
  dt_undo_do_undo(darktable.undo, DT_UNDO_DEVELOP);
  return TRUE;
}

static gboolean _darkroom_redo_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                        GdkModifierType modifier, gpointer data)
{
  dt_undo_do_redo(darktable.undo, DT_UNDO_DEVELOP);
  return TRUE;
}

void connect_key_accels(dt_view_t *self)
{
  GClosure *closure;
  dt_develop_t *data = (dt_develop_t *)self->data;

  // Film strip shortcuts
  closure = g_cclosure_new(G_CALLBACK(film_strip_key_accel), (gpointer)self, NULL);
  dt_accel_connect_view(self, "toggle film strip", closure);

  // Zoom shortcuts
  closure = g_cclosure_new(G_CALLBACK(zoom_key_accel), GINT_TO_POINTER(1), NULL);
  dt_accel_connect_view(self, "zoom close-up", closure);

  closure = g_cclosure_new(G_CALLBACK(zoom_key_accel), GINT_TO_POINTER(2), NULL);
  dt_accel_connect_view(self, "zoom fill", closure);

  closure = g_cclosure_new(G_CALLBACK(zoom_key_accel), GINT_TO_POINTER(3), NULL);
  dt_accel_connect_view(self, "zoom fit", closure);

  // enable shortcut to export with current export settings:
  closure = g_cclosure_new(G_CALLBACK(export_key_accel_callback), (gpointer)self->data, NULL);
  dt_accel_connect_view(self, "export", closure);

  // Shortcut to skip images
  closure = g_cclosure_new(G_CALLBACK(skip_f_key_accel_callback), (gpointer)self->data, NULL);
  dt_accel_connect_view(self, "image forward", closure);

  closure = g_cclosure_new(G_CALLBACK(skip_b_key_accel_callback), (gpointer)self->data, NULL);
  dt_accel_connect_view(self, "image back", closure);

  // toggle raw overexposure indication
  closure = g_cclosure_new(G_CALLBACK(_toolbox_toggle_callback), data->rawoverexposed.button, NULL);
  dt_accel_connect_view(self, "raw overexposed", closure);

  // toggle overexposure indication
  closure = g_cclosure_new(G_CALLBACK(_toolbox_toggle_callback), data->overexposed.button, NULL);
  dt_accel_connect_view(self, "overexposed", closure);

  // toggle softproof indication
  closure = g_cclosure_new(G_CALLBACK(_toolbox_toggle_callback), data->profile.softproof_button, NULL);
  dt_accel_connect_view(self, "softproof", closure);

  // toggle gamut indication
  closure = g_cclosure_new(G_CALLBACK(_toolbox_toggle_callback), data->profile.gamut_button, NULL);
  dt_accel_connect_view(self, "gamut check", closure);

  // brush size +/-
  closure = g_cclosure_new(G_CALLBACK(_brush_size_up_callback), (gpointer)self->data, NULL);
  dt_accel_connect_view(self, "increase brush size", closure);
  closure = g_cclosure_new(G_CALLBACK(_brush_size_down_callback), (gpointer)self->data, NULL);
  dt_accel_connect_view(self, "decrease brush size", closure);

  // brush hardness +/-
  closure = g_cclosure_new(G_CALLBACK(_brush_hardness_up_callback), (gpointer)self->data, NULL);
  dt_accel_connect_view(self, "increase brush hardness", closure);
  closure = g_cclosure_new(G_CALLBACK(_brush_hardness_down_callback), (gpointer)self->data, NULL);
  dt_accel_connect_view(self, "decrease brush hardness", closure);

  // brush opacity +/-
  closure = g_cclosure_new(G_CALLBACK(_brush_opacity_up_callback), (gpointer)self->data, NULL);
  dt_accel_connect_view(self, "increase brush opacity", closure);
  closure = g_cclosure_new(G_CALLBACK(_brush_opacity_down_callback), (gpointer)self->data, NULL);
  dt_accel_connect_view(self, "decrease brush opacity", closure);

  // undo/redo
  closure = g_cclosure_new(G_CALLBACK(_darkroom_undo_callback), (gpointer)self, NULL);
  dt_accel_connect_view(self, "undo", closure);
  closure = g_cclosure_new(G_CALLBACK(_darkroom_redo_callback), (gpointer)self, NULL);
  dt_accel_connect_view(self, "redo", closure);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
