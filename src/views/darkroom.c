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
#include "common/darktable.h"
#include "common/collection.h"
#include "views/view.h"
#include "develop/develop.h"
#include "control/jobs.h"
#include "control/control.h"
#include "control/conf.h"
#include "dtgtk/button.h"
#include "develop/imageop.h"
#include "develop/blend.h"
#include "develop/masks.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
#include "common/debug.h"
#include "common/tags.h"
#include "common/styles.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "libs/colorpicker.h"
#include "bauhaus/bauhaus.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <gdk/gdkkeysyms.h>

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
static gboolean _overexposed_toggle_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                             GdkModifierType modifier, gpointer data);
static gboolean _brush_size_up_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                        GdkModifierType modifier, gpointer data);
static gboolean _brush_size_down_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                          GdkModifierType modifier, gpointer data);

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

uint32_t view(dt_view_t *self)
{
  return DT_VIEW_DARKROOM;
}

void cleanup(dt_view_t *self)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  dt_dev_cleanup(dev);
  free(dev);
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
  int wd, ht, stride, closeup;
  int32_t zoom;
  float zoom_x, zoom_y;
  zoom_y = dt_control_get_dev_zoom_y();
  zoom_x = dt_control_get_dev_zoom_x();
  zoom = dt_control_get_dev_zoom();
  closeup = dt_control_get_dev_closeup();
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
    dt_view_set_scrollbar(self, zx + .5 - boxw * .5, 1.0, boxw, zy + .5 - boxh * .5, 1.0, boxh);
  }

  if((dev->image_status == DT_DEV_PIXELPIPE_VALID)
     && dev->pipe->input_timestamp >= dev->preview_pipe->input_timestamp)
  {
    // draw image
    roi_hash_old = roi_hash;
    mutex = &dev->pipe->backbuf_mutex;
    dt_pthread_mutex_lock(mutex);
    wd = dev->pipe->backbuf_width;
    ht = dev->pipe->backbuf_height;
    stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, wd);
    surface = dt_cairo_image_surface_create_for_data(dev->pipe->backbuf, CAIRO_FORMAT_RGB24, wd, ht, stride);
    wd /= darktable.gui->ppd;
    ht /= darktable.gui->ppd;
    if(dev->full_preview)
      cairo_set_source_rgb(cr, .1, .1, .1);
    else
      cairo_set_source_rgb(cr, .2, .2, .2);
    cairo_paint(cr);
    cairo_translate(cr, .5f * (width - wd), .5f * (height - ht));
    if(closeup)
    {
      cairo_scale(cr, 2.0, 2.0);
      cairo_translate(cr, -.25f * wd, -.25f * ht);
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

    wd = dev->preview_pipe->backbuf_width;
    ht = dev->preview_pipe->backbuf_height;
    float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2 : 1, 1);
    cairo_set_source_rgb(cr, .2, .2, .2);
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
    cairo_surface_write_to_png(image_surface, darktable.develop->proxy.snapshot.filename);
  }

  // Displaying sample areas if enabled
  if(darktable.lib->proxy.colorpicker.live_samples && darktable.lib->proxy.colorpicker.display_samples)
  {
    GSList *samples = darktable.lib->proxy.colorpicker.live_samples;
    dt_colorpicker_sample_t *sample = NULL;

    cairo_save(cri);

    float wd = dev->preview_pipe->backbuf_width;
    float ht = dev->preview_pipe->backbuf_height;
    float zoom_y = dt_control_get_dev_zoom_y();
    float zoom_x = dt_control_get_dev_zoom_x();
    dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
    int closeup = dt_control_get_dev_closeup();
    float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2 : 1, 1);

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

      float *box = sample->box;
      float *point = sample->point;
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
    float wd = dev->preview_pipe->backbuf_width;
    float ht = dev->preview_pipe->backbuf_height;
    float zoom_y = dt_control_get_dev_zoom_y();
    float zoom_x = dt_control_get_dev_zoom_x();
    dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
    int closeup = dt_control_get_dev_closeup();
    float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2 : 1, 1);

    cairo_translate(cri, width / 2.0, height / 2.0f);
    cairo_scale(cri, zoom_scale, zoom_scale);
    cairo_translate(cri, -.5f * wd - zoom_x * wd, -.5f * ht - zoom_y * ht);

    // cairo_set_operator(cri, CAIRO_OPERATOR_XOR);
    cairo_set_line_width(cri, 1.0 / zoom_scale);
    cairo_set_source_rgb(cri, .2, .2, .2);

    float *box = dev->gui_module->color_picker_box;
    float *point = dev->gui_module->color_picker_point;
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
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select * from selected_images", -1, &stmt,
                                NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW) selected = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    // Leave as selected only the image being edited
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "delete from selected_images", NULL, NULL, NULL);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "insert or ignore into selected_images values (?1)", -1, &stmt, NULL);
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
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "delete from selected_images", NULL, NULL, NULL);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "insert or ignore into selected_images values (?1)", -1, &stmt, NULL);
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
  if(dt_pthread_mutex_trylock(&dev->preview_pipe_mutex)) return;
  if(dt_pthread_mutex_trylock(&dev->pipe_mutex))
  {
    dt_pthread_mutex_unlock(&dev->preview_pipe_mutex);
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
  }
  dt_masks_init_form_gui(dev);
  dev->form_visible = NULL;
  dev->form_gui->pipe_hash = 0;
  dev->form_gui->formid = 0;

  select_this_image(imgid);

  while(dev->history)
  {
    // clear history of old image
    free(((dt_dev_history_item_t *)dev->history->data)->params);
    free((dt_dev_history_item_t *)dev->history->data);
    dev->history = g_list_delete_link(dev->history, dev->history);
  }

  // get new image:
  dt_dev_reload_image(dev, imgid);

  // make sure no signals propagate here:
  darktable.gui->reset = 1;

  guint nb_iop = g_list_length(dev->iop);
  dt_dev_pixelpipe_cleanup_nodes(dev->pipe);
  dt_dev_pixelpipe_cleanup_nodes(dev->preview_pipe);
  for(int i = nb_iop - 1; i > 0; i--)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(g_list_nth_data(dev->iop, i));

    // the base module is the one with the highest multi_priority
    const guint clen = g_list_length(dev->iop);
    int mp_base = 0;
    for(int k = 0; k < clen; k++)
    {
      dt_iop_module_t *mod = (dt_iop_module_t *)(g_list_nth_data(dev->iop, k));
      if(strcmp(module->op, mod->op) == 0) mp_base = MAX(mp_base, mod->multi_priority);
    }

    if(module->multi_priority == mp_base) // if the module is the "base" instance, we keep it
    {
      module->multi_priority = 0;
      dt_iop_reload_defaults(module);
      dt_iop_gui_update(module);
    }
    else // else we delete it and remove it from the panel
    {
      if(!dt_iop_is_hidden(module))
      {
        gtk_container_remove(
            GTK_CONTAINER(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER)),
            module->expander);
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
  dt_dev_pixelpipe_create_nodes(dev->pipe, dev);
  dt_dev_pixelpipe_create_nodes(dev->preview_pipe, dev);
  dt_masks_read_forms(dev);
  dt_dev_read_history(dev);

  // we have to init all module instances other than "base" instance
  GList *modules = dev->iop;
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
        dt_iop_gui_set_expanded(module, TRUE, FALSE);
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

    modules = g_list_next(modules);
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
  dt_pthread_mutex_unlock(&dev->preview_pipe_mutex);
  dt_pthread_mutex_unlock(&dev->pipe_mutex);
}

static void film_strip_activated(const int imgid, void *data)
{
  // switch images in darkroom mode:
  dt_view_t *self = (dt_view_t *)data;
  dt_develop_t *dev = (dt_develop_t *)self->data;
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

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select imgid from selected_images", -1, &stmt,
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
      if(zoom == DT_ZOOM_1) closeup ^= 1;
      dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, DT_ZOOM_1, closeup, NULL, NULL);
      dt_control_set_dev_zoom(DT_ZOOM_1);
      dt_control_set_dev_zoom_x(zoom_x);
      dt_control_set_dev_zoom_y(zoom_y);
      dt_control_set_dev_closeup(closeup);
      dt_dev_invalidate(dev);
      break;
    case 2:
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
  int max_width = dt_conf_get_int("plugins/lighttable/export/width");
  int max_height = dt_conf_get_int("plugins/lighttable/export/height");
  char *format_name = dt_conf_get_string("plugins/lighttable/export/format_name");
  char *storage_name = dt_conf_get_string("plugins/lighttable/export/storage_name");
  int format_index = dt_imageio_get_index_of_format(dt_imageio_get_format_by_name(format_name));
  int storage_index = dt_imageio_get_index_of_storage(dt_imageio_get_storage_by_name(storage_name));
  gboolean high_quality = dt_conf_get_bool("plugins/lighttable/export/high_quality_processing");
  char *style = dt_conf_get_string("plugins/lighttable/export/style");
  gboolean style_append = dt_conf_get_bool("plugins/lighttable/export/style_append");
  // darkroom is for single images, so only export the one the user is working on
  GList *l = g_list_append(NULL, GINT_TO_POINTER(dev->image_storage.id));
  dt_control_export(l, max_width, max_height, format_index, storage_index, high_quality, style, style_append);
  g_free(format_name);
  g_free(storage_name);
  g_free(style);
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
    gtk_menu_popup(darktable.gui->presets_popup_menu, NULL, NULL, NULL, NULL, 0, 0);
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

      g_free(style->name);
      g_free(style->description);
      g_free(style);
      g_free(items_string);
      g_free(tooltip);
    } while((styles = g_list_next(styles)) != NULL);
  }

  /* if we got any styles, lets popup menu for selection */
  if(menu)
  {
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, 0, 0);
  }
  else
    dt_control_log(_("no styles have been created yet"));
}

static void _overexposed_quickbutton_clicked(GtkWidget *w, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->overexposed.enabled = !d->overexposed.enabled;
  //   dt_dev_reprocess_center(d);
  dt_dev_reprocess_all(d);
}

static gboolean _overexposed_close_popup(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  g_signal_handler_disconnect(widget, d->overexposed.destroy_signal_handler);
  d->overexposed.destroy_signal_handler = 0;
  gtk_widget_hide(d->overexposed.floating_window);
  return FALSE;
}

static gboolean _overexposed_show_popup(gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  /** finally move the window next to the button */
  gint x, y, wx, wy;
  gint px, py, window_w, window_h;
  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
  gtk_widget_show_all(d->overexposed.floating_window);
  gdk_window_get_origin(gtk_widget_get_window(d->overexposed.button), &px, &py);

  window_w = gdk_window_get_width(gtk_widget_get_window(d->overexposed.floating_window));
  window_h = gdk_window_get_height(gtk_widget_get_window(d->overexposed.floating_window));

  gtk_widget_translate_coordinates(d->overexposed.button, window, 0, 0, &wx, &wy);
  x = px + wx - window_w + 5;
  y = py + wy - window_h - 5;
  gtk_window_move(GTK_WINDOW(d->overexposed.floating_window), x, y);

  gtk_window_present(GTK_WINDOW(d->overexposed.floating_window));

  // when the mouse moves back over the main window we close the popup.
  d->overexposed.destroy_signal_handler
      = g_signal_connect(window, "focus-in-event", G_CALLBACK(_overexposed_close_popup), user_data);

  return FALSE;
}

static gboolean _overexposed_quickbutton_pressed(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  GdkEventButton *e = (GdkEventButton *)event;
  if(e->button == 3)
  {
    _overexposed_show_popup(user_data);
    return TRUE;
  }
  else
  {
    d->overexposed.timeout = g_timeout_add_seconds(1, _overexposed_show_popup, user_data);
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

static gboolean _overexposed_toggle_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                             GdkModifierType modifier, gpointer data)
{
  gtk_button_clicked(GTK_BUTTON(((dt_develop_t *)data)->overexposed.button));
  return TRUE;
}

static gboolean _brush_size_up_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                        GdkModifierType modifier, gpointer data)
{
  dt_develop_t *dev = (dt_develop_t *)data;

  dt_masks_events_mouse_scrolled(dev->gui_module, 0, 0, 0, 0);
  return TRUE;
}
static gboolean _brush_size_down_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                          GdkModifierType modifier, gpointer data)
{
  dt_develop_t *dev = (dt_develop_t *)data;

  dt_masks_events_mouse_scrolled(dev->gui_module, 0, 0, 1, 0);
  return TRUE;
}

void enter(dt_view_t *self)
{

  /* connect to ui pipe finished signal for redraw */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED,
                            G_CALLBACK(_darkroom_ui_pipe_finish_signal_callback), (gpointer)self);

  dt_print(DT_DEBUG_CONTROL, "[run_job+] 11 %f in darkroom mode\n", dt_get_wtime());
  dt_develop_t *dev = (dt_develop_t *)self->data;
  if(!dev->form_gui)
  {
    dev->form_gui = (dt_masks_form_gui_t *)calloc(1, sizeof(dt_masks_form_gui_t));
  }
  dt_masks_init_form_gui(dev);
  dev->form_visible = NULL;
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
   * Add view specific tool buttons
   */

  /* create favorite plugin preset popup tool */
  GtkWidget *favorite_presets
      = dtgtk_button_new(dtgtk_cairo_paint_presets, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER);
  g_object_set(G_OBJECT(favorite_presets), "tooltip-text", _("quick access to presets of your favorites"),
               (char *)NULL);
  g_signal_connect(G_OBJECT(favorite_presets), "clicked", G_CALLBACK(_darkroom_ui_favorite_presets_popupmenu),
                   NULL);
  dt_view_manager_view_toolbox_add(darktable.view_manager, favorite_presets);

  /* create quick styles popup menu tool */
  GtkWidget *styles = dtgtk_button_new(dtgtk_cairo_paint_styles, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER);
  g_signal_connect(G_OBJECT(styles), "clicked", G_CALLBACK(_darkroom_ui_apply_style_popupmenu), NULL);
  g_object_set(G_OBJECT(styles), "tooltip-text", _("quick access for applying any of your styles"),
               (char *)NULL);
  dt_view_manager_view_toolbox_add(darktable.view_manager, styles);

  /* create overexposed popup tool */
  {
    // the button
    dev->overexposed.button
        = dtgtk_togglebutton_new(dtgtk_cairo_paint_overexposed, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER);
    g_object_set(G_OBJECT(dev->overexposed.button), "tooltip-text",
                 _("toggle over/under exposed indication\nright click for options"), (char *)NULL);
    g_signal_connect(G_OBJECT(dev->overexposed.button), "clicked",
                     G_CALLBACK(_overexposed_quickbutton_clicked), dev);
    g_signal_connect(G_OBJECT(dev->overexposed.button), "button-press-event",
                     G_CALLBACK(_overexposed_quickbutton_pressed), dev);
    g_signal_connect(G_OBJECT(dev->overexposed.button), "button-release-event",
                     G_CALLBACK(_overexposed_quickbutton_released), dev);
    dt_view_manager_module_toolbox_add(darktable.view_manager, dev->overexposed.button);

    // and the popup window
    const int panel_width = dt_conf_get_int("panel_width");

    GtkWidget *window = dt_ui_main_window(darktable.gui->ui);

    dev->overexposed.floating_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(dev->overexposed.floating_window), panel_width, -1);
    GtkWidget *frame = gtk_frame_new(NULL);
    GtkWidget *event_box = gtk_event_box_new();
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_margin_start(vbox, DT_PIXEL_APPLY_DPI(8));
    gtk_widget_set_margin_end(vbox, DT_PIXEL_APPLY_DPI(8));
    gtk_widget_set_margin_top(vbox, DT_PIXEL_APPLY_DPI(8));
    gtk_widget_set_margin_bottom(vbox, DT_PIXEL_APPLY_DPI(8));

    gtk_widget_set_can_focus(dev->overexposed.floating_window, TRUE);
    gtk_window_set_decorated(GTK_WINDOW(dev->overexposed.floating_window), FALSE);
    gtk_window_set_type_hint(GTK_WINDOW(dev->overexposed.floating_window), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
    gtk_window_set_transient_for(GTK_WINDOW(dev->overexposed.floating_window), GTK_WINDOW(window));
    gtk_widget_set_opacity(dev->overexposed.floating_window, 0.9);

    gtk_widget_set_state_flags(frame, GTK_STATE_FLAG_SELECTED, TRUE);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_OUT);


    gtk_container_add(GTK_CONTAINER(dev->overexposed.floating_window), frame);
    gtk_container_add(GTK_CONTAINER(frame), event_box);
    gtk_container_add(GTK_CONTAINER(event_box), vbox);

    /** let's fill the encapsulating widgets */
    /* color scheme */
    GtkWidget *colorscheme = dt_bauhaus_combobox_new(NULL);
    dt_bauhaus_widget_set_label(colorscheme, NULL, _("color scheme"));
    dt_bauhaus_combobox_add(colorscheme, _("black & white"));
    dt_bauhaus_combobox_add(colorscheme, _("red & blue"));
    dt_bauhaus_combobox_add(colorscheme, _("purple & green"));
    dt_bauhaus_combobox_set(colorscheme, dev->overexposed.colorscheme);
    g_object_set(G_OBJECT(colorscheme), "tooltip-text", _("select colors to indicate over/under exposure"),
                 (char *)NULL);
    g_signal_connect(G_OBJECT(colorscheme), "value-changed", G_CALLBACK(colorscheme_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(colorscheme), TRUE, TRUE, 0);
    gtk_widget_set_state_flags(colorscheme, GTK_STATE_FLAG_SELECTED, TRUE);

    /* lower */
    GtkWidget *lower = dt_bauhaus_slider_new_with_range(NULL, 0.0, 100.0, 0.1, 2.0, 2);
    dt_bauhaus_slider_set(lower, dev->overexposed.lower);
    dt_bauhaus_slider_set_format(lower, "%.0f%%");
    dt_bauhaus_widget_set_label(lower, NULL, _("lower threshold"));
    g_object_set(G_OBJECT(lower), "tooltip-text", _("threshold of what shall be considered underexposed"),
                 (char *)NULL);
    g_signal_connect(G_OBJECT(lower), "value-changed", G_CALLBACK(lower_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(lower), TRUE, TRUE, 0);

    /* upper */
    GtkWidget *upper = dt_bauhaus_slider_new_with_range(NULL, 0.0, 100.0, 0.1, 98.0, 2);
    dt_bauhaus_slider_set(upper, dev->overexposed.upper);
    dt_bauhaus_slider_set_format(upper, "%.0f%%");
    dt_bauhaus_widget_set_label(upper, NULL, _("upper threshold"));
    g_object_set(G_OBJECT(upper), "tooltip-text", _("threshold of what shall be considered overexposed"),
                 (char *)NULL);
    g_signal_connect(G_OBJECT(upper), "value-changed", G_CALLBACK(upper_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(upper), TRUE, TRUE, 0);
  }

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

  // synch gui and flag gegl pipe as dirty
  // this is done here and not in dt_read_history, as it would else be triggered before module->gui_init.
  dt_dev_pop_history_items(dev, dev->history_end);

  /* ensure that filmstrip shows current image */
  dt_view_filmstrip_scroll_to_image(darktable.view_manager, dev->image_storage.id, FALSE);

  // switch on groups as they where last time:
  dt_dev_modulegroups_set(dev, dt_conf_get_int("plugins/darkroom/groups"));

  // make signals work again:
  darktable.gui->reset = 0;

  // get last active plugin:
  gchar *active_plugin = dt_conf_get_string("plugins/darkroom/active");
  if(active_plugin)
  {
    GList *modules = dev->iop;
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

  // clear gui.
  dev->gui_leaving = 1;
  dt_pthread_mutex_lock(&dev->history_mutex);
  dt_dev_pixelpipe_cleanup_nodes(dev->pipe);
  dt_dev_pixelpipe_cleanup_nodes(dev->preview_pipe);

  while(dev->history)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(dev->history->data);
    // printf("removing history item %d - %s, data %f %f\n", hist->module->instance, hist->module->op, *(float
    // *)hist->params, *((float *)hist->params+1));
    free(hist->params);
    hist->params = NULL;
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

  dt_pthread_mutex_unlock(&dev->history_mutex);

  // cleanup visible masks
  if(dev->form_gui)
  {
    dt_masks_clear_form_gui(dev);
    free(dev->form_gui);
    dev->form_gui = NULL;
    dev->form_visible = NULL;
  }

  // take care of the overexposed window
  if(dev->overexposed.timeout > 0) g_source_remove(dev->overexposed.timeout);
  if(dev->overexposed.destroy_signal_handler > 0)
    g_signal_handler_disconnect(dt_ui_main_window(darktable.gui->ui), dev->overexposed.destroy_signal_handler);
  gtk_widget_destroy(dev->overexposed.floating_window);

  dt_print(DT_DEBUG_CONTROL, "[run_job-] 11 %f in darkroom mode\n", dt_get_wtime());
}

void mouse_leave(dt_view_t *self)
{
  // if we are not hovering over a thumbnail in the filmstrip -> show metadata of opened image.
  dt_develop_t *dev = (dt_develop_t *)self->data;
  dt_control_set_mouse_over_id(dev->image_storage.id);

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
  if(dev->form_visible) handled = dt_masks_events_mouse_moved(dev->gui_module, x, y, pressure, which);
  if(handled) return;
  // module
  if(dev->gui_module && dev->gui_module->mouse_moved)
    handled = dev->gui_module->mouse_moved(dev->gui_module, x, y, pressure, which);
  if(handled) return;

  if(darktable.control->button_down && darktable.control->button_down_which == 1)
  {
    // depending on dev_zoom, adjust dev_zoom_x/y.
    dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
    int closeup = dt_control_get_dev_closeup();
    int procw, proch;
    dt_dev_get_processed_size(dev, &procw, &proch);
    const float scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2 : 1, 0);
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
    const float scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2 : 1, 0);
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
  float scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2.0 : 1.0, 0);
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


void border_scrolled(dt_view_t *view, double x, double y, int which, int up)
{
  dt_develop_t *dev = (dt_develop_t *)view->data;
  dt_dev_zoom_t zoom;
  int closeup;
  float zoom_x, zoom_y;
  zoom = dt_control_get_dev_zoom();
  closeup = dt_control_get_dev_closeup();
  zoom_x = dt_control_get_dev_zoom_x();
  zoom_y = dt_control_get_dev_zoom_y();
  if(which > 1)
  {
    if(up)
      zoom_x -= 0.02;
    else
      zoom_x += 0.02;
  }
  else
  {
    if(up)
      zoom_y -= 0.02;
    else
      zoom_y += 0.02;
  }
  dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, zoom, closeup, NULL, NULL);
  dt_control_set_dev_zoom_x(zoom_x);
  dt_control_set_dev_zoom_y(zoom_y);
  dt_dev_invalidate(dev);
  dt_control_queue_redraw();
}

int key_released(dt_view_t *self, guint key, guint state)
{
  dt_control_accels_t *accels = &darktable.control->accels;
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

  return 1;
}

int key_pressed(dt_view_t *self, guint key, guint state)
{
  dt_control_accels_t *accels = &darktable.control->accels;
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

  // toggle overexposure indication
  dt_accel_register_view(self, NC_("accel", "overexposed"), GDK_KEY_o, 0);

  // brush size +/-
  dt_accel_register_view(self, NC_("accel", "brush larger"), GDK_KEY_bracketright, 0);
  dt_accel_register_view(self, NC_("accel", "brush smaller"), GDK_KEY_bracketleft, 0);

  // fullscreen view
  dt_accel_register_view(self, NC_("accel", "full preview"), GDK_KEY_z, 0);
}

void connect_key_accels(dt_view_t *self)
{
  GClosure *closure;

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

  // toggle overexposure indication
  closure = g_cclosure_new(G_CALLBACK(_overexposed_toggle_callback), (gpointer)self->data, NULL);
  dt_accel_connect_view(self, "overexposed", closure);

  // brush size +/-
  closure = g_cclosure_new(G_CALLBACK(_brush_size_up_callback), (gpointer)self->data, NULL);
  dt_accel_connect_view(self, "brush larger", closure);
  closure = g_cclosure_new(G_CALLBACK(_brush_size_down_callback), (gpointer)self->data, NULL);
  dt_accel_connect_view(self, "brush smaller", closure);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
