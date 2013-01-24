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
#include "dtgtk/tristatebutton.h"
#include "develop/imageop.h"
#include "develop/blend.h"
#include "develop/lightroom.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/debug.h"
#include "common/tags.h"
#include "common/styles.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "libs/colorpicker.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

static gboolean film_strip_key_accel(GtkAccelGroup *accel_group,
                                     GObject *acceleratable,
                                     guint keyval, GdkModifierType modifier,
                                     gpointer data);

static gboolean zoom_key_accel(GtkAccelGroup *accel_group, GObject *acceleratable,
                               guint keyval, GdkModifierType modifier,
                               gpointer data);

static gboolean export_key_accel_callback(GtkAccelGroup *accel_group,
    GObject *acceleratable, guint keyval,
    GdkModifierType modifier,gpointer data);

static gboolean skip_f_key_accel_callback(GtkAccelGroup *accel_group,
    GObject *acceleratable,
    guint keyval, GdkModifierType modifier,
    gpointer data);
static gboolean skip_b_key_accel_callback(GtkAccelGroup *accel_group,
    GObject *acceleratable,
    guint keyval, GdkModifierType modifier,
    gpointer data);

/* signal handler for filmstrip image switching */
static void _view_darkroom_filmstrip_activate_callback(gpointer instance,gpointer user_data);

const char
*name(dt_view_t *self)
{
  return _("darkroom");
}


void
init(dt_view_t *self)
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


void expose(dt_view_t *self, cairo_t *cri, int32_t width_i, int32_t height_i, int32_t pointerx, int32_t pointery)
{
  // startup-time conf parameter:
  const int32_t capwd = darktable.thumbnail_width;
  const int32_t capht = darktable.thumbnail_height;
  // if width or height > max pipeline pixels: center the view and clamp.
  int32_t width  = MIN(width_i,  capwd);
  int32_t height = MIN(height_i, capht);

  cairo_set_source_rgb (cri, .2, .2, .2);
  cairo_save(cri);
  cairo_set_fill_rule(cri, CAIRO_FILL_RULE_EVEN_ODD);
  cairo_rectangle(cri, 0, 0, width_i, height_i);
  cairo_rectangle(cri,
                  MAX(1.0, width_i -capwd) *.5f,
                  MAX(1.0, height_i-capht) *.5f,
                  MIN(width, width_i-1), MIN(height, height_i-1));
  cairo_fill (cri);
  cairo_restore(cri);

  if(width_i  > capwd) cairo_translate(cri, -(capwd-width_i) *.5f, 0.0f);
  if(height_i > capht) cairo_translate(cri, 0.0f, -(capht-height_i)*.5f);
  cairo_save(cri);

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

  if(dev->image_dirty || dev->pipe->input_timestamp < dev->preview_pipe->input_timestamp) dt_dev_process_image(dev);
  if(dev->preview_dirty || dev->pipe->input_timestamp > dev->preview_pipe->input_timestamp) dt_dev_process_preview(dev);

  dt_pthread_mutex_t *mutex = NULL;
  int wd, ht, stride, closeup;
  int32_t zoom;
  float zoom_x, zoom_y;
  DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);
  DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
  DT_CTL_GET_GLOBAL(zoom, dev_zoom);
  DT_CTL_GET_GLOBAL(closeup, dev_closeup);
  static cairo_surface_t *image_surface = NULL;
  static int image_surface_width = 0, image_surface_height = 0, image_surface_imgid = -1;

  if(image_surface_width != width || image_surface_height != height || image_surface == NULL)
  {
    // create double-buffered image to draw on, to make modules draw more fluently.
    image_surface_width = width;
    image_surface_height = height;
    if(image_surface) cairo_surface_destroy(image_surface);
    image_surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
    image_surface_imgid = -1; // invalidate old stuff
  }
  cairo_surface_t *surface;
  cairo_t *cr = cairo_create(image_surface);

  // adjust scroll bars
  {
    float zx = zoom_x, zy = zoom_y, boxw = 1., boxh = 1.;
    dt_dev_check_zoom_bounds(dev, &zx, &zy, zoom, closeup, &boxw, &boxh);
    dt_view_set_scrollbar(self, zx+.5-boxw*.5, 1.0, boxw, zy+.5-boxh*.5, 1.0, boxh);
  }

  if(!dev->image_dirty && dev->pipe->input_timestamp >= dev->preview_pipe->input_timestamp)
  {
    // draw image
    mutex = &dev->pipe->backbuf_mutex;
    dt_pthread_mutex_lock(mutex);
    wd = dev->pipe->backbuf_width;
    ht = dev->pipe->backbuf_height;
    stride = cairo_format_stride_for_width (CAIRO_FORMAT_RGB24, wd);
    surface = cairo_image_surface_create_for_data (dev->pipe->backbuf, CAIRO_FORMAT_RGB24, wd, ht, stride);
    cairo_set_source_rgb (cr, .2, .2, .2);
    cairo_paint(cr);
    cairo_translate(cr, .5f*(width-wd), .5f*(height-ht));
    if(closeup)
    {
      const float closeup_scale = 2.0;
      cairo_scale(cr, closeup_scale, closeup_scale);
      float boxw = 1, boxh = 1, zx0 = zoom_x, zy0 = zoom_y, zx1 = zoom_x, zy1 = zoom_y, zxm = -1.0, zym = -1.0;
      dt_dev_check_zoom_bounds(dev, &zx0, &zy0, zoom, 0, &boxw, &boxh);
      dt_dev_check_zoom_bounds(dev, &zx1, &zy1, zoom, 1, &boxw, &boxh);
      dt_dev_check_zoom_bounds(dev, &zxm, &zym, zoom, 1, &boxw, &boxh);
      const float fx = 1.0 - fmaxf(0.0, (zx0 - zx1)/(zx0 - zxm)), fy = 1.0 - fmaxf(0.0, (zy0 - zy1)/(zy0 - zym));
      cairo_translate(cr, -wd/(2.0*closeup_scale) * fx, -ht/(2.0*closeup_scale) * fy);
    }
    cairo_rectangle(cr, 0, 0, wd, ht);
    cairo_set_source_surface (cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgb (cr, .3, .3, .3);
    cairo_stroke(cr);
    cairo_surface_destroy (surface);
    dt_pthread_mutex_unlock(mutex);
    image_surface_imgid = dev->image_storage.id;
  }
  else if(!dev->preview_dirty)
  {
    // draw preview
    mutex = &dev->preview_pipe->backbuf_mutex;
    dt_pthread_mutex_lock(mutex);

    wd = dev->preview_pipe->backbuf_width;
    ht = dev->preview_pipe->backbuf_height;
    float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2 : 1, 1);
    cairo_set_source_rgb (cr, .2, .2, .2);
    cairo_paint(cr);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_clip(cr);
    stride = cairo_format_stride_for_width (CAIRO_FORMAT_RGB24, wd);
    surface = cairo_image_surface_create_for_data (dev->preview_pipe->backbuf, CAIRO_FORMAT_RGB24, wd, ht, stride);
    cairo_translate(cr, width/2.0, height/2.0f);
    cairo_scale(cr, zoom_scale, zoom_scale);
    cairo_translate(cr, -.5f*wd-zoom_x*wd, -.5f*ht-zoom_y*ht);
    // avoid to draw the 1px garbage that sometimes shows up in the preview :(
    cairo_rectangle(cr, 0, 0, wd-1, ht-1);
    cairo_set_source_surface (cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
    cairo_fill(cr);
    cairo_surface_destroy (surface);
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

  /* check if we should create a snapshot of view */
  if(darktable.develop->proxy.snapshot.request)
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
  if(darktable.lib->proxy.colorpicker.live_samples &&
      darktable.lib->proxy.colorpicker.display_samples)
  {
    GSList *samples = darktable.lib->proxy.colorpicker.live_samples;
    dt_colorpicker_sample_t *sample = NULL;

    cairo_save(cri);

    int32_t zoom, closeup;
    float zoom_x, zoom_y;
    float wd = dev->preview_pipe->backbuf_width;
    float ht = dev->preview_pipe->backbuf_height;
    DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);
    DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
    DT_CTL_GET_GLOBAL(zoom, dev_zoom);
    DT_CTL_GET_GLOBAL(closeup, dev_closeup);
    float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2 : 1, 1);

    cairo_translate(cri, width/2.0, height/2.0f);
    cairo_scale(cri, zoom_scale, zoom_scale);
    cairo_translate(cri, -.5f*wd-zoom_x*wd, -.5f*ht-zoom_y*ht);

    while(samples)
    {
      sample = samples->data;

      cairo_set_line_width(cri, 1.0/zoom_scale);
      if(sample == darktable.lib->proxy.colorpicker.selected_sample)
        cairo_set_source_rgb(cri, .2, 0, 0);
      else
        cairo_set_source_rgb(cri, 0, 0, .2);

      float *box = sample->box;
      float *point = sample->point;
      if(sample->size == DT_COLORPICKER_SIZE_BOX)
      {
        cairo_rectangle(cri, box[0]*wd, box[1]*ht,
                        (box[2] - box[0])*wd, (box[3] - box[1])*ht);
        cairo_stroke(cri);
        cairo_translate(cri, 1.0/zoom_scale, 1.0/zoom_scale);
        if(sample == darktable.lib->proxy.colorpicker.selected_sample)
          cairo_set_source_rgb(cri, .8, 0, 0);
        else
          cairo_set_source_rgb(cri, 0, 0, .8);
        cairo_rectangle(cri, box[0]*wd + 1.0/zoom_scale, box[1]*ht,
                        (box[2] - box[0])*wd - 3./zoom_scale,
                        (box[3] - box[1])*ht - 2./zoom_scale);
        cairo_stroke(cri);
      }
      else
      {
        cairo_rectangle(cri, point[0] * wd - .01 * wd, point[1] * ht - .01 * wd,
                        .02 * wd, .02 * wd);
        cairo_stroke(cri);

        if(sample == darktable.lib->proxy.colorpicker.selected_sample)
          cairo_set_source_rgb(cri, .8, 0, 0);
        else
          cairo_set_source_rgb(cri, 0, 0, .8);
        cairo_rectangle(cri, (point[0] - 0.01) * wd + 1.0/zoom_scale,
                        point[1] * ht - 0.01 * wd + 1.0/zoom_scale,
                        .02 * wd - 2./zoom_scale, .02 * wd - 2./zoom_scale);
        cairo_move_to(cri, point[0] * wd,
                      point[1] * ht - .01 * wd + 1./zoom_scale);
        cairo_line_to(cri, point[0] * wd,
                      point[1] * ht + .01 * wd - 1./zoom_scale);
        cairo_move_to(cri, point[0] * wd - .01 * wd + 1./zoom_scale,
                      point[1] * ht);
        cairo_line_to(cri, point[0] * wd + .01 * wd - 1./zoom_scale,
                      point[1] * ht);
        cairo_stroke(cri);
      }

      samples = g_slist_next(samples);
    }

    cairo_restore(cri);
  }

  // execute module callback hook.
  if(dev->gui_module && dev->gui_module->request_color_pick)
  {
    int32_t zoom, closeup;
    float zoom_x, zoom_y;
    float wd = dev->preview_pipe->backbuf_width;
    float ht = dev->preview_pipe->backbuf_height;
    DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);
    DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
    DT_CTL_GET_GLOBAL(zoom, dev_zoom);
    DT_CTL_GET_GLOBAL(closeup, dev_closeup);
    float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2 : 1, 1);

    cairo_translate(cri, width/2.0, height/2.0f);
    cairo_scale(cri, zoom_scale, zoom_scale);
    cairo_translate(cri, -.5f*wd-zoom_x*wd, -.5f*ht-zoom_y*ht);

    // cairo_set_operator(cri, CAIRO_OPERATOR_XOR);
    cairo_set_line_width(cri, 1.0/zoom_scale);
    cairo_set_source_rgb(cri, .2, .2, .2);

    float *box = dev->gui_module->color_picker_box;
    float *point = dev->gui_module->color_picker_point;
    if(darktable.lib->proxy.colorpicker.size)
    {
      cairo_rectangle(cri, box[0]*wd, box[1]*ht,
                      (box[2] - box[0])*wd, (box[3] - box[1])*ht);
      cairo_stroke(cri);
      cairo_translate(cri, 1.0/zoom_scale, 1.0/zoom_scale);
      cairo_set_source_rgb(cri, .8, .8, .8);
      cairo_rectangle(cri, box[0]*wd + 1.0/zoom_scale, box[1]*ht,
                      (box[2] - box[0])*wd - 3./zoom_scale,
                      (box[3] - box[1])*ht - 2./zoom_scale);
      cairo_stroke(cri);
    }
    else
    {
      cairo_rectangle(cri, point[0] * wd - .01 * wd, point[1] * ht - .01 * wd,
                      .02 * wd, .02 * wd);
      cairo_stroke(cri);

      cairo_set_source_rgb(cri, .8, .8, .8);
      cairo_rectangle(cri, (point[0] - 0.01) * wd + 1.0/zoom_scale,
                      point[1] * ht - 0.01 * wd + 1.0/zoom_scale,
                      .02 * wd - 2./zoom_scale, .02 * wd - 2./zoom_scale);
      cairo_move_to(cri, point[0] * wd,
                    point[1] * ht - .01 * wd + 1./zoom_scale);
      cairo_line_to(cri, point[0] * wd,
                    point[1] * ht + .01 * wd - 1./zoom_scale);
      cairo_move_to(cri, point[0] * wd - .01 * wd + 1./zoom_scale,
                    point[1] * ht);
      cairo_line_to(cri, point[0] * wd + .01 * wd - 1./zoom_scale,
                    point[1] * ht);
      cairo_stroke(cri);
    }
  }
  else if(dev->gui_module && dev->gui_module->gui_post_expose)
  {
    if(width_i  > capwd) pointerx += (capwd-width_i) *.5f;
    if(height_i > capht) pointery += (capht-height_i)*.5f;
    dev->gui_module->gui_post_expose(dev->gui_module, cri, width, height, pointerx, pointery);
  }
}


void reset(dt_view_t *self)
{
  DT_CTL_SET_GLOBAL(dev_zoom, DT_ZOOM_FIT);
  DT_CTL_SET_GLOBAL(dev_zoom_x, 0);
  DT_CTL_SET_GLOBAL(dev_zoom_y, 0);
  DT_CTL_SET_GLOBAL(dev_closeup, 0);
}

int try_enter(dt_view_t *self)
{
  int selected;
  DT_CTL_GET_GLOBAL(selected, lib_image_mouse_over_id);
  if(selected < 0)
  {
    // try last selected
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select * from selected_images", -1, &stmt, NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW)
      selected = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    // Leave as selected only the image being edited
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "delete from selected_images", NULL, NULL, NULL);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "insert or ignore into selected_images values (?1)", -1, &stmt, NULL);
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
  const dt_image_t *img = dt_image_cache_read_get(darktable.image_cache, selected);
  // get image and check if it has been deleted from disk first!
  char imgfilename[DT_MAX_PATH_LEN];
  dt_image_full_path(img->id, imgfilename, DT_MAX_PATH_LEN);
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



static void
select_this_image(const int imgid)
{
  // select this image, if no multiple selection:
  if(dt_collection_get_selected_count(NULL) < 2)
  {
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "delete from selected_images", NULL, NULL, NULL);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "insert or ignore into selected_images values (?1)", -1, &stmt, NULL);
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

static void
dt_dev_change_image(dt_develop_t *dev, const uint32_t imgid)
{
  // stop crazy users from sleeping on key-repeat spacebar:
  if(dev->image_loading) return;

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

  select_this_image(imgid);

  while(dev->history)
  {
    // clear history of old image
    free(((dt_dev_history_item_t *)dev->history->data)->params);
    free( (dt_dev_history_item_t *)dev->history->data);
    dev->history = g_list_delete_link(dev->history, dev->history);
  }

  // get new image:
  dt_dev_reload_image(dev, imgid);

  // make sure no signals propagate here:
  darktable.gui->reset = 1;

  GList *modules = g_list_last(dev->iop);
  int nb_iop = g_list_length(dev->iop);
  dt_dev_pixelpipe_cleanup_nodes(dev->pipe);
  dt_dev_pixelpipe_cleanup_nodes(dev->preview_pipe);
  for (int i=nb_iop-1; i>0; i--)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(g_list_nth_data(dev->iop,i));
    if (module->multi_priority == 0) //if the module is the "base" instance, we keep it
    {
      dt_iop_reload_defaults(module);
      dt_iop_gui_update(module);
    }
    else  //else we delete it and remove it from the panel
    {
      if (!dt_iop_is_hidden(module))
      {
        gtk_container_remove (GTK_CONTAINER(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER)),module->expander);
        dt_iop_gui_cleanup_module(module);
      }

      //we remove the module from the list
      dev->iop = g_list_remove_link(dev->iop,g_list_nth(dev->iop,i));

      //we cleanup the module
      dt_accel_disconnect_list(module->accel_closures);
      dt_accel_cleanup_locals_iop(module);
      module->accel_closures = NULL;
      dt_iop_cleanup_module(module);
      free(module);
    }
  }
  dt_dev_pixelpipe_create_nodes(dev->pipe, dev);
  dt_dev_pixelpipe_create_nodes(dev->preview_pipe, dev);
  dt_dev_read_history(dev);

  //we have to init all module instances other than "base" instance
  modules = dev->iop;
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    if(module->multi_priority > 0)
    {
      if (!dt_iop_is_hidden(module))
      {
        module->gui_init(module);
        dt_iop_reload_defaults(module);
        //we search the base iop corresponding
        GList *mods = g_list_first(dev->iop);
        dt_iop_module_t *base = NULL;
        int pos_module = 0;
        int pos_base = 0;
        int pos = 0;
        while (mods)
        {
          dt_iop_module_t *mod = (dt_iop_module_t *)(mods->data);
          if (mod->multi_priority == 0 && mod->instance == module->instance)
          {
            base = mod;
            pos_base = pos;
          }
          else if (mod == module) pos_module = pos;
          mods = g_list_next(mods);
          pos++;
        }
        if (!base) continue;

        /* add module to right panel */
        GtkWidget *expander = dt_iop_gui_get_expander(module);
        dt_ui_container_add_widget(darktable.gui->ui,
                                   DT_UI_CONTAINER_PANEL_RIGHT_CENTER, expander);
        GValue gv = { 0, { { 0 } } };
        g_value_init(&gv,G_TYPE_INT);
        gtk_container_child_get_property(GTK_CONTAINER(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER)),base->expander,"position",&gv);
        gtk_box_reorder_child (dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER),expander,g_value_get_int(&gv)+pos_base-pos_module);
        dt_iop_gui_set_expanded(module, TRUE);
        dt_iop_gui_update_blending(module);
      }

      /* setup key accelerators */
      module->accel_closures = NULL;
      if(module->connect_key_accels)
        module->connect_key_accels(module);
      dt_iop_connect_common_accels(module);

      //we update show params for multi-instances for each other instances
      dt_dev_modules_update_multishow(module->dev);
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
      if(!strcmp(module->op, active_plugin))
        dt_iop_request_focus(module);
      modules = g_list_next(modules);
    }
    g_free(active_plugin);
  }

  /* last set the group to update visibility of iop modules for new pipe */
  dt_dev_modulegroups_set(dev,dt_conf_get_int("plugins/darkroom/groups"));

  // make signals work again, but only after focus event,
  // to avoid crop/rotate for example to add another history item.
  darktable.gui->reset = 0;

  // Signal develop initialize
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_DEVELOP_IMAGE_CHANGED);

  // prefetch next few from first selected image on.
  dt_view_filmstrip_prefetch();
}

static void
film_strip_activated(const int imgid, void *data)
{
  // switch images in darkroom mode:
  dt_view_t *self = (dt_view_t *)data;
  dt_develop_t *dev = (dt_develop_t *)self->data;
  dt_dev_change_image(dev, imgid);
  dt_view_filmstrip_scroll_to_image(darktable.view_manager, imgid, FALSE);
  // force redraw
  dt_control_queue_redraw();
}

static void _view_darkroom_filmstrip_activate_callback(gpointer instance,gpointer user_data)
{
  int32_t imgid = 0;
  if ((imgid=dt_view_filmstrip_get_activated_imgid(darktable.view_manager))>0)
    film_strip_activated(imgid,user_data);
}

static void
dt_dev_jump_image(dt_develop_t *dev, int diff)
{
  const gchar *qin = dt_collection_get_query (darktable.collection);
  int offset = 0;
  if(qin)
  {
    int orig_imgid = -1, imgid = -1;
    sqlite3_stmt *stmt;

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select imgid from selected_images", -1, &stmt, NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW)
      orig_imgid = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    offset = dt_collection_image_offset (orig_imgid);

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), qin, -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, offset + diff);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, 1);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      imgid = sqlite3_column_int(stmt, 0);

      if (orig_imgid == imgid)
      {
        //nothing to do
        sqlite3_finalize(stmt);
        return;
      }

      if (!dev->image_loading)
      {
        dt_view_filmstrip_scroll_to_image(darktable.view_manager, imgid, FALSE);
      }
      dt_dev_change_image(dev, imgid);

    }
    sqlite3_finalize(stmt);
  }
}

static gboolean
zoom_key_accel(GtkAccelGroup *accel_group,
               GObject *acceleratable, guint keyval,
               GdkModifierType modifier, gpointer data)
{
  dt_develop_t *dev = darktable.develop;
  int zoom, closeup;
  float zoom_x, zoom_y;
  switch ((long int)data)
  {
    case 1:
      DT_CTL_GET_GLOBAL(zoom, dev_zoom);
      DT_CTL_GET_GLOBAL(closeup, dev_closeup);
      if(zoom == DT_ZOOM_1) closeup ^= 1;
      DT_CTL_SET_GLOBAL(dev_closeup, closeup);
      DT_CTL_SET_GLOBAL(dev_zoom, DT_ZOOM_1);
      dt_dev_invalidate(dev);
      break;
    case 2:
      DT_CTL_SET_GLOBAL(dev_zoom, DT_ZOOM_FILL);
      dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, DT_ZOOM_FILL, 0, NULL, NULL);
      DT_CTL_SET_GLOBAL(dev_zoom_x, zoom_x);
      DT_CTL_SET_GLOBAL(dev_zoom_y, zoom_y);
      DT_CTL_SET_GLOBAL(dev_closeup, 0);
      dt_dev_invalidate(dev);
      break;
    case 3:
      DT_CTL_SET_GLOBAL(dev_zoom, DT_ZOOM_FIT);
      DT_CTL_SET_GLOBAL(dev_zoom_x, 0);
      DT_CTL_SET_GLOBAL(dev_zoom_y, 0);
      DT_CTL_SET_GLOBAL(dev_closeup, 0);
      dt_dev_invalidate(dev);
      break;
    default:
      break;
  }
  dt_control_queue_redraw_center();
  return TRUE;
}

static gboolean
film_strip_key_accel(GtkAccelGroup *accel_group,
                     GObject *acceleratable, guint keyval,
                     GdkModifierType modifier, gpointer data)
{
  dt_lib_module_t *m = darktable.view_manager->proxy.filmstrip.module;
  gboolean vs = dt_lib_is_visible(m);
  dt_lib_set_visible(m,!vs);
  return TRUE;
}


static gboolean
export_key_accel_callback(GtkAccelGroup *accel_group,
                          GObject *acceleratable, guint keyval,
                          GdkModifierType modifier, gpointer data)
{
  /* write history before exporting */
  dt_dev_write_history((dt_develop_t *)data);

  /* export current image */
  int max_width  = dt_conf_get_int ("plugins/lighttable/export/width");
  int max_height = dt_conf_get_int ("plugins/lighttable/export/height");
  int format_index = dt_conf_get_int ("plugins/lighttable/export/format");
  int storage_index = dt_conf_get_int ("plugins/lighttable/export/storage");
  gboolean high_quality = dt_conf_get_bool("plugins/lighttable/export/high_quality_processing");
  char *style = dt_conf_get_string("plugins/lighttable/export/style");
  dt_control_export(max_width, max_height, format_index, storage_index, high_quality, style);
  return TRUE;
}

static gboolean skip_f_key_accel_callback(GtkAccelGroup *accel_group,
    GObject *acceleratable, guint keyval,
    GdkModifierType modifier, gpointer data)
{
  dt_dev_jump_image((dt_develop_t*)data, 1);
  return TRUE;
}

static gboolean skip_b_key_accel_callback(GtkAccelGroup *accel_group,
    GObject *acceleratable,
    guint keyval, GdkModifierType modifier,
    gpointer data)
{
  dt_dev_jump_image((dt_develop_t*)data, -1);
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
  if (darktable.gui->presets_popup_menu)
  {
    gtk_menu_popup(darktable.gui->presets_popup_menu, NULL, NULL, NULL, NULL, 0, 0);
    gtk_widget_show_all(GTK_WIDGET(darktable.gui->presets_popup_menu));
  }
  else dt_control_log(_("no userdefined presets for favorite modules were found"));
}

static void _darkroom_ui_apply_style_activate_callback(gchar *name)
{
  dt_control_log(_("applied style `%s' on current image"),name);

  /* write current history changes so nothing gets lost */
  dt_dev_write_history(darktable.develop);

  /* apply style on image and reload*/
  dt_styles_apply_to_image (name, FALSE, darktable.develop->image_storage.id);
  dt_dev_reload_image(darktable.develop, darktable.develop->image_storage.id);
}

static void _darkroom_ui_apply_style_popupmenu(GtkWidget *w, gpointer user_data)
{
  /* show styles popup menu */
  GList *styles = dt_styles_get_list("");
  GtkWidget *menu = NULL;
  if(styles)
  {
    menu= gtk_menu_new();
    do
    {
      dt_style_t *style=(dt_style_t *)styles->data;
      GtkWidget *mi=gtk_menu_item_new_with_label(style->name);

      char* items_string = dt_styles_get_item_list_as_string(style->name);
      gchar* tooltip = NULL;

      if((style->description) && strlen(style->description))
      {
        tooltip = g_strconcat("<b><i>", style->description, "</i></b>\n", items_string, NULL);
      }
      else
      {
        tooltip = g_strdup(items_string);
      }

      gtk_widget_set_tooltip_markup(mi, tooltip);

      gtk_menu_append (GTK_MENU (menu), mi);
      gtk_signal_connect_object (GTK_OBJECT (mi), "activate",
                                 GTK_SIGNAL_FUNC (_darkroom_ui_apply_style_activate_callback),
                                 (gpointer) g_strdup (style->name));
      gtk_widget_show (mi);

      g_free(style->name);
      g_free(style->description);
      g_free(style);
      g_free(items_string);
      g_free(tooltip);
    }
    while ((styles=g_list_next(styles))!=NULL);
  }

  /* if we got any styles, lets popup menu for selection */
  if (menu)
  {
    gtk_menu_popup (GTK_MENU(menu), NULL, NULL, NULL, NULL,
                    0, 0);
  }
  else dt_control_log(_("no styles have been created yet"));
}

static void _darkroom_ui_apply_LR_style(GtkWidget *w, gpointer user_data)
{
  dt_develop_t *dev = (dt_develop_t *)user_data;
  dt_lightroom_import (dev->image_storage.id, dev, FALSE);
}

void enter(dt_view_t *self)
{
  /* connect to ui pipe finished signal for redraw */
  dt_control_signal_connect(darktable.signals,
                            DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED,G_CALLBACK(_darkroom_ui_pipe_finish_signal_callback),
                            (gpointer)self);

  dt_print(DT_DEBUG_CONTROL, "[run_job+] 11 %f in darkroom mode\n", dt_get_wtime());
  dt_develop_t *dev = (dt_develop_t *)self->data;

  dev->gui_leaving = 0;
  dev->gui_module = NULL;

  select_this_image(dev->image_storage.id);

  DT_CTL_SET_GLOBAL(dev_zoom, DT_ZOOM_FIT);
  DT_CTL_SET_GLOBAL(dev_zoom_x, 0);
  DT_CTL_SET_GLOBAL(dev_zoom_y, 0);
  DT_CTL_SET_GLOBAL(dev_closeup, 0);

  // take a copy of the image struct for convenience.

  dt_dev_load_image(darktable.develop, dev->image_storage.id);

  /*
   * Add view specific tool buttons
   */

  /* create favorite plugin preset popup tool */
  GtkWidget *favorite_presets = dtgtk_button_new(dtgtk_cairo_paint_presets, CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER);
  g_object_set(G_OBJECT(favorite_presets), "tooltip-text", _("quick access to presets of your favorites"),
               (char *)NULL);
  g_signal_connect (G_OBJECT (favorite_presets), "clicked",
                    G_CALLBACK (_darkroom_ui_favorite_presets_popupmenu),
                    NULL);
  dt_view_manager_view_toolbox_add(darktable.view_manager, favorite_presets);

  /* add IOP modules to plugin list */

  /* create quick styles popup menu tool */
  GtkWidget *styles = dtgtk_button_new (dtgtk_cairo_paint_styles,CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER);
  g_signal_connect (G_OBJECT (styles), "clicked",
                    G_CALLBACK (_darkroom_ui_apply_style_popupmenu),
                    NULL);
  g_object_set (G_OBJECT (styles), "tooltip-text", _("quick access for applying any of your styles"),
                (char *)NULL);
  dt_view_manager_view_toolbox_add(darktable.view_manager, styles);

  /* create LR import button (only if LR .xmp found) */
  char *lr_xmp_pathname = dt_get_lightroom_xmp(dev->image_storage.id);
  if(lr_xmp_pathname)
  {
    g_free(lr_xmp_pathname);
    GtkWidget *LRimp = dtgtk_button_new (dtgtk_cairo_paint_LR,CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER);
    g_signal_connect (G_OBJECT (LRimp), "clicked",
                      G_CALLBACK (_darkroom_ui_apply_LR_style),
                      dev);
    g_object_set (G_OBJECT (LRimp), "tooltip-text", _("import from Lightroom"),
                  (char *)NULL);
    dt_view_manager_view_toolbox_add(darktable.view_manager, LRimp);
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
    if (!dt_iop_is_hidden(module))
    {
      module->gui_init(module);
      dt_iop_reload_defaults(module);

      /* add module to right panel */
      GtkWidget *expander = dt_iop_gui_get_expander(module);
      dt_ui_container_add_widget(darktable.gui->ui,
                                 DT_UI_CONTAINER_PANEL_RIGHT_CENTER, expander);

      snprintf(option, 1024, "plugins/darkroom/%s/expanded", module->op);
      dt_iop_gui_set_expanded(module, dt_conf_get_bool(option));
    }

    /* setup key accelerators */
    module->accel_closures = NULL;
    if(module->connect_key_accels)
      module->connect_key_accels(module);
    dt_iop_connect_common_accels(module);

    modules = g_list_previous(modules);
  }
  darktable.gui->reset = 0;

  /* signal that darktable.develop is initialized and ready to be used */
  dt_control_signal_raise(darktable.signals,DT_SIGNAL_DEVELOP_INITIALIZE);

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
      if(!strcmp(module->op, active_plugin))
        dt_iop_request_focus(module);
      modules = g_list_next(modules);
    }
    g_free(active_plugin);
  }

  // image should be there now.
  float zoom_x, zoom_y;
  dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, DT_ZOOM_FIT, 0, NULL, NULL);
  DT_CTL_SET_GLOBAL(dev_zoom_x, zoom_x);
  DT_CTL_SET_GLOBAL(dev_zoom_y, zoom_y);

  /* connect signal for filmstrip image activate */
  dt_control_signal_connect(darktable.signals,
                            DT_SIGNAL_VIEWMANAGER_FILMSTRIP_ACTIVATE,
                            G_CALLBACK(_view_darkroom_filmstrip_activate_callback),
                            self);

  // prefetch next few from first selected image on.
  dt_view_filmstrip_prefetch();
}

void leave(dt_view_t *self)
{
  /* disconnect from filmstrip image activate */
  dt_control_signal_disconnect(darktable.signals,
                               G_CALLBACK(_view_darkroom_filmstrip_activate_callback),
                               (gpointer)self);

  /* disconnect from pipe finish signal */
  dt_control_signal_disconnect(darktable.signals,
                               G_CALLBACK(_darkroom_ui_pipe_finish_signal_callback),
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
  dt_tag_new("darktable|changed",&tagid);
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
    // printf("removing history item %d - %s, data %f %f\n", hist->module->instance, hist->module->op, *(float *)hist->params, *((float *)hist->params+1));
    free(hist->params);
    hist->params = NULL;
    free(hist);
    dev->history = g_list_delete_link(dev->history, dev->history);
  }

  while(dev->iop)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(dev->iop->data);
    if (!dt_iop_is_hidden(module))
      dt_iop_gui_cleanup_module(module);

    dt_dev_cleanup_module_accels(module);
    module->accel_closures = NULL;
    dt_iop_cleanup_module(module) ;
    free(module);
    dev->iop = g_list_delete_link(dev->iop, dev->iop);
  }

  dt_pthread_mutex_unlock(&dev->history_mutex);

  dt_print(DT_DEBUG_CONTROL, "[run_job-] 11 %f in darkroom mode\n", dt_get_wtime());
}

void mouse_leave(dt_view_t *self)
{
  // if we are not hovering over a thumbnail in the filmstrip -> show metadata of opened image.
  dt_develop_t *dev = (dt_develop_t *)self->data;
  int32_t mouse_over_id = dev->image_storage.id;
  DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, mouse_over_id);

  // reset any changes the selected plugin might have made.
  dt_control_change_cursor(GDK_LEFT_PTR);
}

void mouse_moved(dt_view_t *self, double x, double y, int which)
{
  const int32_t capwd = darktable.thumbnail_width;
  const int32_t capht = darktable.thumbnail_height;
  dt_develop_t *dev = (dt_develop_t *)self->data;

  // if we are not hovering over a thumbnail in the filmstrip -> show metadata of opened image.
  int32_t mouse_over_id = -1;
  DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
  if(mouse_over_id == -1)
  {
    mouse_over_id = dev->image_storage.id;
    DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, mouse_over_id);
  }

  dt_control_t *ctl = darktable.control;
  const int32_t width_i  = self->width;
  const int32_t height_i = self->height;
  int32_t offx = 0.0f, offy = 0.0f;
  if(width_i  > capwd) offx = (capwd-width_i) *.5f;
  if(height_i > capht) offy = (capht-height_i)*.5f;
  int handled = 0;
  x += offx;
  y += offy;
  if(dev->gui_module && dev->gui_module->request_color_pick &&
      ctl->button_down &&
      ctl->button_down_which == 1)
  {
    // module requested a color box
    float zoom_x, zoom_y, bzoom_x, bzoom_y;
    dt_dev_get_pointer_zoom_pos(dev, x, y, &zoom_x, &zoom_y);
    dt_dev_get_pointer_zoom_pos(dev, ctl->button_x + offx, ctl->button_y + offy, &bzoom_x, &bzoom_y);
    if(darktable.lib->proxy.colorpicker.size)
    {
      dev->gui_module->color_picker_box[0] = fmaxf(0.0, fminf(.5f+bzoom_x, .5f+zoom_x));
      dev->gui_module->color_picker_box[1] = fmaxf(0.0, fminf(.5f+bzoom_y, .5f+zoom_y));
      dev->gui_module->color_picker_box[2] = fminf(1.0, fmaxf(.5f+bzoom_x, .5f+zoom_x));
      dev->gui_module->color_picker_box[3] = fminf(1.0, fmaxf(.5f+bzoom_y, .5f+zoom_y));
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
  if(dev->gui_module && dev->gui_module->mouse_moved) handled = dev->gui_module->mouse_moved(dev->gui_module, x, y, which);
  if(handled) return;

  if(darktable.control->button_down && darktable.control->button_down_which == 1)
  {
    // depending on dev_zoom, adjust dev_zoom_x/y.
    dt_dev_zoom_t zoom;
    int closeup;
    DT_CTL_GET_GLOBAL(zoom, dev_zoom);
    DT_CTL_GET_GLOBAL(closeup, dev_closeup);
    int procw, proch;
    dt_dev_get_processed_size(dev, &procw, &proch);
    const float scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2 : 1, 0);
    float old_zoom_x, old_zoom_y;
    DT_CTL_GET_GLOBAL(old_zoom_x, dev_zoom_x);
    DT_CTL_GET_GLOBAL(old_zoom_y, dev_zoom_y);
    float zx = old_zoom_x - (1.0/scale)*(x - ctl->button_x - offx)/procw;
    float zy = old_zoom_y - (1.0/scale)*(y - ctl->button_y - offy)/proch;
    dt_dev_check_zoom_bounds(dev, &zx, &zy, zoom, closeup, NULL, NULL);
    DT_CTL_SET_GLOBAL(dev_zoom_x, zx);
    DT_CTL_SET_GLOBAL(dev_zoom_y, zy);
    ctl->button_x = x - offx;
    ctl->button_y = y - offy;
    dt_dev_invalidate(dev);
    dt_control_queue_redraw();
  }
}


int button_released(dt_view_t *self, double x, double y, int which, uint32_t state)
{
  const int32_t capwd = darktable.thumbnail_width;
  const int32_t capht = darktable.thumbnail_height;
  dt_develop_t *dev = darktable.develop;
  const int32_t width_i  = self->width;
  const int32_t height_i = self->height;
  if(width_i  > capwd) x += (capwd-width_i) *.5f;
  if(height_i > capht) y += (capht-height_i)*.5f;

  int handled = 0;
  if(dev->gui_module && dev->gui_module->button_released) handled = dev->gui_module->button_released(dev->gui_module, x, y, which, state);
  if(handled) return handled;
  if(which == 1) dt_control_change_cursor(GDK_LEFT_PTR);
  return 1;
}


int button_pressed(dt_view_t *self, double x, double y, int which, int type, uint32_t state)
{
  const int32_t capwd = darktable.thumbnail_width;
  const int32_t capht = darktable.thumbnail_height;
  dt_develop_t *dev = (dt_develop_t *)self->data;
  const int32_t width_i  = self->width;
  const int32_t height_i = self->height;
  if(width_i  > capwd) x += (capwd-width_i) *.5f;
  if(height_i > capht) y += (capht-height_i)*.5f;

  int handled = 0;
  if(dev->gui_module && dev->gui_module->request_color_pick && which == 1)
  {
    float zoom_x, zoom_y;
    dt_dev_get_pointer_zoom_pos(dev, x, y, &zoom_x, &zoom_y);
    if(darktable.lib->proxy.colorpicker.size)
    {
      dev->gui_module->color_picker_box[0] = .5f+zoom_x;
      dev->gui_module->color_picker_box[1] = .5f+zoom_y;
      dev->gui_module->color_picker_box[2] = .5f+zoom_x;
      dev->gui_module->color_picker_box[3] = .5f+zoom_y;
    }
    else
    {
      dev->gui_module->color_picker_point[0] = .5f+zoom_x;
      dev->gui_module->color_picker_point[1] = .5f+zoom_y;
      dev->preview_pipe->changed |= DT_DEV_PIPE_SYNCH;
      dt_dev_invalidate_all(dev);
    }
    dt_control_queue_redraw();
    return 1;
  }
  if(dev->gui_module && dev->gui_module->button_pressed) handled = dev->gui_module->button_pressed(dev->gui_module, x, y, which, type, state);
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
    DT_CTL_GET_GLOBAL(zoom, dev_zoom);
    DT_CTL_GET_GLOBAL(closeup, dev_closeup);
    DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
    DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);
    dt_dev_get_processed_size(dev, &procw, &proch);
    const float scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2 : 1, 0);
    zoom_x += (1.0/scale)*(x - .5f*dev->width )/procw;
    zoom_y += (1.0/scale)*(y - .5f*dev->height)/proch;
    if(zoom == DT_ZOOM_1)
    {
      if(!closeup) closeup = 1;
      else
      {
        zoom = DT_ZOOM_FIT;
        zoom_x = zoom_y = 0.0f;
        closeup = 0;
      }
    }
    else zoom = DT_ZOOM_1;
    dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, zoom, closeup, NULL, NULL);
    DT_CTL_SET_GLOBAL(dev_zoom, zoom);
    DT_CTL_SET_GLOBAL(dev_closeup, closeup);
    DT_CTL_SET_GLOBAL(dev_zoom_x, zoom_x);
    DT_CTL_SET_GLOBAL(dev_zoom_y, zoom_y);
    dt_dev_invalidate(dev);
    return 1;
  }
  return 0;
}


void scrolled(dt_view_t *self, double x, double y, int up, int state)
{
  const int32_t capwd = darktable.thumbnail_width;
  const int32_t capht = darktable.thumbnail_height;
  dt_develop_t *dev = (dt_develop_t *)self->data;
  const int32_t width_i  = self->width;
  const int32_t height_i = self->height;
  if(width_i  > capwd) x += (capwd-width_i) *.5f;
  if(height_i > capht) y += (capht-height_i)*.5f;

  int handled = 0;
  if(dev->gui_module && dev->gui_module->scrolled) handled = dev->gui_module->scrolled(dev->gui_module, x, y, up, state);
  if(handled) return;
  // free zoom
  dt_dev_zoom_t zoom;
  int closeup, procw, proch;
  float zoom_x, zoom_y;
  DT_CTL_GET_GLOBAL(zoom, dev_zoom);
  DT_CTL_GET_GLOBAL(closeup, dev_closeup);
  DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
  DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);
  dt_dev_get_processed_size(dev, &procw, &proch);
  float scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2.0 : 1.0, 0);
  const float minscale = dt_dev_get_zoom_scale(dev, DT_ZOOM_FIT, 1.0, 0);
  // offset from center now (current zoom_{x,y} points there)
  float mouse_off_x = x - .5*dev->width, mouse_off_y = y - .5*dev->height;
  zoom_x += mouse_off_x/(procw*scale);
  zoom_y += mouse_off_y/(proch*scale);
  zoom = DT_ZOOM_FREE;
  closeup = 0;
  if(up)
  {
    if (scale == 1.0f) return;
    else scale += .1f*(1.0f - minscale);
  }
  else
  {
    if (scale == minscale) return;
    else scale -= .1f*(1.0f - minscale);
  }
  DT_CTL_SET_GLOBAL(dev_zoom_scale, scale);
  if(scale > 0.99)            zoom = DT_ZOOM_1;
  if(scale < minscale + 0.01) zoom = DT_ZOOM_FIT;
  if(zoom != DT_ZOOM_1)
  {
    zoom_x -= mouse_off_x/(procw*scale);
    zoom_y -= mouse_off_y/(proch*scale);
  }
  dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, zoom, closeup, NULL, NULL);
  DT_CTL_SET_GLOBAL(dev_zoom, zoom);
  DT_CTL_SET_GLOBAL(dev_closeup, closeup);
  if(zoom != DT_ZOOM_1)
  {
    DT_CTL_SET_GLOBAL(dev_zoom_x, zoom_x);
    DT_CTL_SET_GLOBAL(dev_zoom_y, zoom_y);
  }
  dt_dev_invalidate(dev);

  dt_control_queue_redraw();
}


void border_scrolled(dt_view_t *view, double x, double y, int which, int up)
{
  dt_develop_t *dev = (dt_develop_t *)view->data;
  dt_dev_zoom_t zoom;
  int closeup;
  float zoom_x, zoom_y;
  DT_CTL_GET_GLOBAL(zoom, dev_zoom);
  DT_CTL_GET_GLOBAL(closeup, dev_closeup);
  DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
  DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);
  if(which > 1)
  {
    if(up) zoom_x -= 0.02;
    else   zoom_x += 0.02;
  }
  else
  {
    if(up) zoom_y -= 0.02;
    else   zoom_y += 0.02;
  }
  dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, zoom, closeup, NULL, NULL);
  DT_CTL_SET_GLOBAL(dev_zoom_x, zoom_x);
  DT_CTL_SET_GLOBAL(dev_zoom_y, zoom_y);
  dt_dev_invalidate(dev);
  dt_control_queue_redraw();
}


int key_pressed(dt_view_t *self, guint key, guint state)
{
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
  dt_accel_register_view(self, NC_("accel", "toggle film strip"),
                         GDK_f, GDK_CONTROL_MASK);

  // Zoom shortcuts
  dt_accel_register_view(self, NC_("accel", "zoom close-up"),
                         GDK_1, GDK_MOD1_MASK);
  dt_accel_register_view(self, NC_("accel", "zoom fill"),
                         GDK_2, GDK_MOD1_MASK);
  dt_accel_register_view(self, NC_("accel", "zoom fit"),
                         GDK_3, GDK_MOD1_MASK);

  // enable shortcut to export with current export settings:
  dt_accel_register_view(self, NC_("accel", "export"),
                         GDK_e, GDK_CONTROL_MASK);

  // Shortcut to skip images
  dt_accel_register_view(self, NC_("accel", "image forward"),
                         GDK_space, 0);
  dt_accel_register_view(self, NC_("accel", "image back"),
                         GDK_BackSpace, 0);

}

void connect_key_accels(dt_view_t *self)
{
  GClosure *closure;

  // Film strip shortcuts
  closure = g_cclosure_new(G_CALLBACK(film_strip_key_accel),
                           (gpointer)self, NULL);
  dt_accel_connect_view(self, "toggle film strip", closure);

  // Zoom shortcuts
  closure = g_cclosure_new(G_CALLBACK(zoom_key_accel), (gpointer)1, NULL);
  dt_accel_connect_view(self, "zoom close-up", closure);

  closure = g_cclosure_new(G_CALLBACK(zoom_key_accel), (gpointer)2, NULL);
  dt_accel_connect_view(self, "zoom fill", closure);

  closure = g_cclosure_new(G_CALLBACK(zoom_key_accel), (gpointer)3, NULL);
  dt_accel_connect_view(self, "zoom fit", closure);

  // enable shortcut to export with current export settings:
  closure = g_cclosure_new(G_CALLBACK(export_key_accel_callback),
                           (gpointer)self->data, NULL);
  dt_accel_connect_view(self, "export", closure);

  // Shortcut to skip images
  closure = g_cclosure_new(G_CALLBACK(skip_f_key_accel_callback),
                           (gpointer)self->data, NULL);
  dt_accel_connect_view(self, "image forward", closure);

  closure = g_cclosure_new(G_CALLBACK(skip_b_key_accel_callback),
                           (gpointer)self->data, NULL);
  dt_accel_connect_view(self, "image back", closure);

}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
