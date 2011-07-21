/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#include "common/collection.h"
#include "views/view.h"
#include "develop/develop.h"
#include "control/jobs.h"
#include "control/control.h"
#include "control/conf.h"
#include "dtgtk/tristatebutton.h"
#include "develop/imageop.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/debug.h"
#include "common/tags.h"
#include "gui/gtk.h"
#include "gui/metadata.h"
#include "gui/iop_modulegroups.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

static void film_strip_key_accel(GtkAccelGroup *accel_group,
                                 GObject *acceleratable,
                                 guint keyval, GdkModifierType modifier,
                                 gpointer data);

static void zoom_key_accel(GtkAccelGroup *accel_group, GObject *acceleratable,
                           guint keyval, GdkModifierType modifier,
                           gpointer data);

static void export_key_accel_callback(GtkAccelGroup *accel_group,
                                      GObject *acceleratable, guint keyval,
                                      GdkModifierType modifier);

static void skip_f_key_accel_callback(GtkAccelGroup *accel_group,
                                      GObject *acceleratable,
                                      guint keyval, GdkModifierType modifier,
                                      gpointer data);
static void skip_b_key_accel_callback(GtkAccelGroup *accel_group,
                                      GObject *acceleratable,
                                      guint keyval, GdkModifierType modifier,
                                      gpointer data);

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

  // Film strip shortcuts
  gtk_accel_map_add_entry("<Darktable>/darkroom/toggle film strip",
                          GDK_f, GDK_CONTROL_MASK);
  dt_accel_group_connect_by_path(
      darktable.control->accels_darkroom,
      "<Darktable>/darkroom/toggle film strip",
      NULL);

  // Zoom shortcuts
  gtk_accel_map_add_entry("<Darktable>/darkroom/zoom/close",
                          GDK_1, GDK_MOD1_MASK);
  gtk_accel_map_add_entry("<Darktable>/darkroom/zoom/fill",
                          GDK_2, GDK_MOD1_MASK);
  gtk_accel_map_add_entry("<Darktable>/darkroom/zoom/fit",
                          GDK_3, GDK_MOD1_MASK);

  dt_accel_group_connect_by_path(
      darktable.control->accels_darkroom,
      "<Darktable>/darkroom/zoom/close",
      NULL);
  dt_accel_group_connect_by_path(
      darktable.control->accels_darkroom,
      "<Darktable>/darkroom/zoom/fill",
      NULL);
  dt_accel_group_connect_by_path(
      darktable.control->accels_darkroom,
      "<Darktable>/darkroom/zoom/fit",
      NULL);

  // enable shortcut to export with current export settings:
  gtk_accel_map_add_entry("<Darktable>/darkroom/export",
                          GDK_e, GDK_CONTROL_MASK);

  dt_accel_group_connect_by_path(
      darktable.control->accels_darkroom,
      "<Darktable>/darkroom/export",
      NULL);


  // Shortcut to skip images
  gtk_accel_map_add_entry("<Darktable>/darkroom/image forward",
                          GDK_space, 0);
  gtk_accel_map_add_entry("<Darktable>/darkroom/image back",
                          GDK_BackSpace, 0);

  dt_accel_group_connect_by_path(
      darktable.control->accels_darkroom,
      "<Darktable>/darkroom/image forward",
      NULL);
  dt_accel_group_connect_by_path(
      darktable.control->accels_darkroom,
      "<Darktable>/darkroom/image back",
      NULL);
}


void cleanup(dt_view_t *self)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  dt_dev_cleanup(dev);
  free(dev);
}


void expose(dt_view_t *self, cairo_t *cri, int32_t width_i, int32_t height_i, int32_t pointerx, int32_t pointery)
{
  // if width or height > max pipeline pixels: center the view and clamp.
  int32_t width  = MIN(width_i,  DT_IMAGE_WINDOW_SIZE);
  int32_t height = MIN(height_i, DT_IMAGE_WINDOW_SIZE);

  cairo_set_source_rgb (cri, .2, .2, .2);
  cairo_rectangle(cri, 0, 0, fmaxf(0, width_i-DT_IMAGE_WINDOW_SIZE) *.5f, height);
  cairo_fill (cri);
  cairo_rectangle(cri, fmaxf(0.0, width_i-DT_IMAGE_WINDOW_SIZE) *.5f + width, 0, width_i, height);
  cairo_fill (cri);

  if(width_i  > DT_IMAGE_WINDOW_SIZE) cairo_translate(cri, -(DT_IMAGE_WINDOW_SIZE-width_i) *.5f, 0.0f);
  if(height_i > DT_IMAGE_WINDOW_SIZE) cairo_translate(cri, 0.0f, -(DT_IMAGE_WINDOW_SIZE-height_i)*.5f);
  cairo_save(cri);

  dt_develop_t *dev = (dt_develop_t *)self->data;

  if(dev->gui_synch)
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
  if(dev->preview_dirty) dt_dev_process_preview(dev);

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
    image_surface_imgid = dev->image->id;
  }
  else if(!dev->preview_dirty)
    // else if(!dev->preview_loading)
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
    image_surface_imgid = dev->image->id;
  }
  cairo_restore(cri);

  if(image_surface_imgid == dev->image->id)
  {
    cairo_destroy(cr);
    cairo_set_source_surface(cri, image_surface, 0, 0);
    cairo_paint(cri);
  }

  if(darktable.gui->request_snapshot)
  {
    cairo_surface_write_to_png(image_surface, darktable.gui->snapshot[0].filename);
    darktable.gui->request_snapshot = 0;
  }
  // and if a snapshot is currently selected, draw it on top!
  if(darktable.gui->snapshot_image)
  {
    cairo_set_source_surface(cri, darktable.gui->snapshot_image, 0, 0);
    cairo_rectangle(cri, 0, 0, width*.5f, height);
    cairo_fill(cri);
    cairo_set_source_rgb(cri, .7, .7, .7);
    cairo_set_line_width(cri, 1.0);
    cairo_move_to(cri, width*.5f, 0.0f);
    cairo_line_to(cri, width*.5f, height);
    cairo_stroke(cri);
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
    cairo_rectangle(cri, box[0]*wd, box[1]*ht, (box[2] - box[0])*wd, (box[3] - box[1])*ht);
    cairo_stroke(cri);
    cairo_translate(cri, 1.0/zoom_scale, 1.0/zoom_scale);
    cairo_set_source_rgb(cri, .8, .8, .8);
    cairo_rectangle(cri, box[0]*wd, box[1]*ht, (box[2] - box[0])*wd, (box[3] - box[1])*ht);
    cairo_stroke(cri);
  }
  else if(dev->gui_module && dev->gui_module->gui_post_expose)
  {
    if(width_i  > DT_IMAGE_WINDOW_SIZE) pointerx += (DT_IMAGE_WINDOW_SIZE-width_i) *.5f;
    if(height_i > DT_IMAGE_WINDOW_SIZE) pointery += (DT_IMAGE_WINDOW_SIZE-height_i)*.5f;
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


static void module_tristate_changed_callback(GtkWidget *button,gint state, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  char option[512]= {0};

  if(state==0)
  {
    /* module is hidden lets set gconf values */
    gtk_widget_hide(GTK_WIDGET(module->topwidget));
    snprintf(option, 512, "plugins/darkroom/%s/visible", module->op);
    dt_conf_set_bool (option, FALSE);
    snprintf(option, 512, "plugins/darkroom/%s/favorite", module->op);
    dt_conf_set_bool (option, FALSE);
    gtk_expander_set_expanded(module->expander, FALSE);

    /* construct tooltip text into option */
    snprintf(option, 512, _("show %s"), module->name());
  }
  else if(state==1)
  {
    /* module is shown lets set gconf values */
    dt_gui_iop_modulegroups_switch(module->groups());
    gtk_widget_show(GTK_WIDGET(module->topwidget));
    snprintf(option, 512, "plugins/darkroom/%s/visible", module->op);
    dt_conf_set_bool (option, TRUE);
    snprintf(option, 512, "plugins/darkroom/%s/favorite", module->op);
    dt_conf_set_bool (option, FALSE);

    gtk_expander_set_expanded(module->expander, TRUE);

    /* construct tooltip text into option */
    snprintf(option, 512, _("%s as favorite"), module->name());
  }
  else if(state==2)
  {
    /* module is shown and favorite lets set gconf values */
    dt_gui_iop_modulegroups_switch(module->groups());
    gtk_widget_show(GTK_WIDGET(module->topwidget));
    snprintf(option, 512, "plugins/darkroom/%s/visible", module->op);
    dt_conf_set_bool (option, TRUE);
    snprintf(option, 512, "plugins/darkroom/%s/favorite", module->op);
    dt_conf_set_bool (option, TRUE);

    gtk_expander_set_expanded(module->expander, TRUE);

    /* construct tooltip text into option */
    snprintf(option, 512, _("hide %s"), module->name());
  }

  g_object_set(G_OBJECT(button), "tooltip-text", option, (char *)NULL);
}


int try_enter(dt_view_t *self)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  int selected;
  DT_CTL_GET_GLOBAL(selected, lib_image_mouse_over_id);
  if(selected < 0)
  {
    // try last selected
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "select * from selected_images", -1, &stmt, NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW)
      selected = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }

  if(selected < 0)
  {
    // fail :(
    dt_control_log(_("no image selected!"));
    return 1;
  }

  // this loads the image from db if needed:
  dev->image = dt_image_cache_get(selected, 'r');
  // get image and check if it has been deleted from disk first!
  char imgfilename[1024];
  dt_image_full_path(dev->image->id, imgfilename, 1024);
  if(!g_file_test(imgfilename, G_FILE_TEST_IS_REGULAR))
  {
    dt_control_log(_("image `%s' is currently unavailable"), dev->image->filename);
    // dt_image_remove(selected);
    dt_image_cache_release(dev->image, 'r');
    dev->image = NULL;
    return 1;
  }
  return 0;
}



static void
select_this_image(const int imgid)
{
  // select this image, if no multiple selection:
  int count = 0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "select count(imgid) from selected_images", -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
    count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  if(count < 2)
  {
    DT_DEBUG_SQLITE3_EXEC(darktable.db, "delete from selected_images", NULL, NULL, NULL);
    DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "insert into selected_images values (?1)", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
}

static void
dt_dev_change_image(dt_develop_t *dev, dt_image_t *image)
{
  // store last active group
  dt_conf_set_int("plugins/darkroom/groups", dt_gui_iop_modulegroups_get());
  // store last active plugin:
  if(darktable.develop->gui_module)
    dt_conf_set_string("plugins/darkroom/active", darktable.develop->gui_module->op);
  else
    dt_conf_set_string("plugins/darkroom/active", "");
  g_assert(dev->gui_attached);
  // tag image as changed
  // TODO: only tag the image when there was a real change.
  // TODO: this applies especially for the expensive bits:
  //       write xmp (disk) / re-create mip map
  guint tagid = 0;
  dt_tag_new("darktable|changed",&tagid);
  dt_tag_attach(tagid, dev->image->id);
  // commit image ops to db
  dt_dev_write_history(dev);

  // be sure light table will update the thumbnail
  if(dev->image) dev->image->force_reimport = 1;
  // release full buffer
  if(dev->image && dev->image->pixels)
    dt_image_release(dev->image, DT_IMAGE_FULL, 'r');

  // writes the .xmp and the database:
  dt_image_cache_flush(dev->image);

  dev->image = image;
  while(dev->history)
  {
    // clear history of old image
    free(((dt_dev_history_item_t *)dev->history->data)->params);
    free( (dt_dev_history_item_t *)dev->history->data);
    dev->history = g_list_delete_link(dev->history, dev->history);
  }
  GList *modules = g_list_last(dev->iop);
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    if(strcmp(module->op, "gamma"))
    {
      char var[1024];
      snprintf(var, 1024, "plugins/darkroom/%s/expanded", module->op);
      dt_conf_set_bool(var, gtk_expander_get_expanded (module->expander));
      // remove widget:
      GtkWidget *top = GTK_WIDGET(module->topwidget);
      GtkWidget *exp = GTK_WIDGET(module->expander);
      GtkWidget *shh = GTK_WIDGET(module->showhide);
      GtkWidget *parent = NULL;
      g_object_get(G_OBJECT(module->widget), "parent", &parent, (char *)NULL);
      // re-init and re-gui_init
      module->gui_cleanup(module);
      gtk_widget_destroy(GTK_WIDGET(module->widget));
      dt_iop_reload_defaults(module);
      module->gui_init(module);
      // copy over already inited stuff:
      module->topwidget = top;
      module->expander = GTK_EXPANDER(exp);
      module->showhide = shh;
      // reparent
      gtk_container_add(GTK_CONTAINER(parent), module->widget);
      gtk_widget_show_all(module->topwidget);
      // all the signal handlers get passed module*, which is still valid.
    }
    modules = g_list_previous(modules);
  }
  // hack: now hide all custom expander widgets again.
  modules = dev->iop;
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    if(strcmp(module->op, "gamma"))
    {
      char option[1024];
      snprintf(option, 1024, "plugins/darkroom/%s/visible", module->op);
      gboolean active = dt_conf_get_bool (option);
      snprintf(option, 1024, "plugins/darkroom/%s/favorite", module->op);
      gboolean favorite = dt_conf_get_bool (option);
      gint state=0;
      if(active)
      {
        state++;
        if(favorite) state++;
      }

      if(module->showhide)
        dtgtk_tristatebutton_set_state(DTGTK_TRISTATEBUTTON(module->showhide),state);

      snprintf(option, 1024, "plugins/darkroom/%s/expanded", module->op);
      active = dt_conf_get_bool (option);
      gtk_expander_set_expanded (module->expander, active);
    }
    else
    {
      gtk_widget_hide_all(GTK_WIDGET(module->topwidget));
    }
    modules = g_list_next(modules);
  }
  dt_gui_iop_modulegroups_switch(dt_conf_get_int("plugins/darkroom/groups"));
  dt_dev_read_history(dev);
  dt_dev_pop_history_items(dev, dev->history_end);
  dt_dev_raw_reload(dev);

  // get last active plugin:
  gchar *active_plugin = dt_conf_get_string("plugins/darkroom/active");
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
}

static void
film_strip_activated(const int imgid, void *data)
{
  // switch images in darkroom mode:
  dt_view_t *self = (dt_view_t *)data;
  dt_develop_t *dev = (dt_develop_t *)self->data;
  dt_image_t *image = dt_image_cache_get(imgid, 'r');
  dt_dev_change_image(dev, image);
  // release image struct with metadata.
  dt_image_cache_release(dev->image, 'r');
  // select newly loaded image
  select_this_image(dev->image->id);
  // force redraw
  dt_control_queue_draw_all();
  // prefetch next few from first selected image on.
  dt_view_film_strip_prefetch();
}

static void
dt_dev_jump_image(dt_develop_t *dev, int diff)
{
  char query[1024];
  const gchar *qin = dt_collection_get_query (darktable.collection);
  int offset = 0;
  if(qin)
  {
    int orig_imgid = -1, imgid = -1;
    sqlite3_stmt *stmt;
    dt_image_t *image;

    DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "select imgid from selected_images", -1, &stmt, NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW)
      orig_imgid = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    snprintf(query, 1024, "select rowid from (%s) where id=?3", qin);
    DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, query, -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1,  0);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, -1);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, orig_imgid);
    if(sqlite3_step(stmt) == SQLITE_ROW)
      offset = sqlite3_column_int(stmt, 0) - 1;
    sqlite3_finalize(stmt);

    DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, qin, -1, &stmt, NULL);
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

      image = dt_image_cache_get(imgid, 'r');
      dt_dev_change_image(dev, image);
      dt_image_cache_release(dev->image, 'r');
      select_this_image(dev->image->id);
      dt_view_film_strip_scroll_to(darktable.view_manager, dev->image->id);

      if(dt_conf_get_bool("plugins/filmstrip/on"))
      {
        dt_view_film_strip_prefetch();
      }
      dt_control_queue_draw_all();
    }
    sqlite3_finalize(stmt);
  }
}

static void
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
}

static void
film_strip_key_accel(GtkAccelGroup *accel_group,
                     GObject *acceleratable, guint keyval,
                     GdkModifierType modifier, gpointer data)
{
  dt_view_film_strip_toggle(darktable.view_manager, film_strip_activated,
                            (void*)data);
  dt_control_queue_draw_all();
}

static void
export_key_accel_callback(GtkAccelGroup *accel_group,
                          GObject *acceleratable, guint keyval,
                          GdkModifierType modifier)
{
  dt_control_export();
}

static void skip_f_key_accel_callback(GtkAccelGroup *accel_group,
                                      GObject *acceleratable, guint keyval,
                                      GdkModifierType modifier, gpointer data)
{
  dt_dev_jump_image((dt_develop_t*)data, 1);
}

static void skip_b_key_accel_callback(GtkAccelGroup *accel_group,
                                      GObject *acceleratable,
                                      guint keyval, GdkModifierType modifier,
                                      gpointer data)
{
  dt_dev_jump_image((dt_develop_t*)data, -1);
}


static void show_module_callback(GtkAccelGroup *accel_group,
                                 GObject *acceleratable,
                                 guint keyval, GdkModifierType modifier,
                                 gpointer data)

{
  dt_iop_module_t *module = (dt_iop_module_t*)data;

  // Showing the module, if it isn't already visible
  if(!dtgtk_tristatebutton_get_state(DTGTK_TRISTATEBUTTON(module->showhide)))
  {
    dtgtk_tristatebutton_set_state(DTGTK_TRISTATEBUTTON(module->showhide), 1);
    gtk_widget_queue_draw(module->showhide);
  }

  dt_gui_iop_modulegroups_switch(module->groups());
  gtk_expander_set_expanded(GTK_EXPANDER(module->expander), TRUE);
  dt_iop_request_focus(module);
}

static void connect_closures(dt_view_t *self)
{
  GClosure *closure;
  dt_develop_t *dev = (dt_develop_t*)self->data;

  // Film strip shortcuts
  closure = g_cclosure_new(G_CALLBACK(film_strip_key_accel),
                           (gpointer)self, NULL);
  dev->closures = g_slist_prepend(dev->closures, closure);
  dt_accel_group_connect_by_path(darktable.control->accels_darkroom,
                                 "<Darktable>/darkroom/toggle film strip",
                                 closure);

  // Zoom shortcuts
  closure = g_cclosure_new(G_CALLBACK(zoom_key_accel), (gpointer)1, NULL);
  dev->closures = g_slist_prepend(dev->closures, closure);
  dt_accel_group_connect_by_path(darktable.control->accels_darkroom,
                                 "<Darktable>/darkroom/zoom/close",
                                 closure);
  closure = g_cclosure_new(G_CALLBACK(zoom_key_accel), (gpointer)2, NULL);
  dev->closures = g_slist_prepend(dev->closures, closure);
  dt_accel_group_connect_by_path(darktable.control->accels_darkroom,
                                 "<Darktable>/darkroom/zoom/fill",
                                 closure);
  closure = g_cclosure_new(G_CALLBACK(zoom_key_accel), (gpointer)3, NULL);
  dev->closures = g_slist_prepend(dev->closures, closure);
  dt_accel_group_connect_by_path(darktable.control->accels_darkroom,
                                 "<Darktable>/darkroom/zoom/fit",
                                 closure);

  // enable shortcut to export with current export settings:
  closure = g_cclosure_new(G_CALLBACK(export_key_accel_callback), NULL, NULL);
  dev->closures = g_slist_prepend(dev->closures, closure);
  dt_accel_group_connect_by_path(darktable.control->accels_darkroom,
                                 "<Darktable>/darkroom/export",
                                 closure);


  // Shortcut to skip images
  closure = g_cclosure_new(G_CALLBACK(skip_f_key_accel_callback),
                           (gpointer)self->data, NULL);
  dev->closures = g_slist_prepend(dev->closures, closure);
  dt_accel_group_connect_by_path(darktable.control->accels_darkroom,
                                 "<Darktable>/darkroom/image forward",
                                 closure);
  closure = g_cclosure_new(G_CALLBACK(skip_b_key_accel_callback),
                           (gpointer)self->data, NULL);
  dev->closures = g_slist_prepend(dev->closures, closure);
  dt_accel_group_connect_by_path(darktable.control->accels_darkroom,
                                 "<Darktable>/darkroom/image back",
                                 closure);

}

void enter(dt_view_t *self)
{
  char accelpath[256];

  // Attaching accelerator group
  gtk_window_add_accel_group(GTK_WINDOW(darktable.gui->widgets.main_window),
                             darktable.control->accels_darkroom);

  // Connecting the closures
  connect_closures(self);

  dt_print(DT_DEBUG_CONTROL, "[run_job+] 11 %f in darkroom mode\n", dt_get_wtime());
  dt_develop_t *dev = (dt_develop_t *)self->data;

  select_this_image(dev->image->id);

  DT_CTL_SET_GLOBAL(dev_zoom, DT_ZOOM_FIT);
  DT_CTL_SET_GLOBAL(dev_zoom_x, 0);
  DT_CTL_SET_GLOBAL(dev_zoom_y, 0);
  DT_CTL_SET_GLOBAL(dev_closeup, 0);

  dev->gui_leaving = 0;
  dev->gui_module = NULL;
  dt_dev_load_image(dev, dev->image);

  // adjust gui:
  GtkWidget *widget;
  gtk_widget_set_visible (darktable.gui->
                          widgets.modulegroups_eventbox, TRUE);

  widget = darktable.gui->widgets.navigation_expander;
  gtk_widget_set_visible(widget, TRUE);
  widget = darktable.gui->widgets.histogram_expander;
  gtk_widget_set_visible(widget, TRUE);
  widget = darktable.gui->widgets.snapshots_eventbox;
  gtk_widget_set_visible(widget, TRUE);
  widget = darktable.gui->widgets.history_eventbox;
  gtk_widget_set_visible(widget, TRUE);
  widget = darktable.gui->widgets.bottom_darkroom_box;
  gtk_widget_set_visible(widget, TRUE);
  widget = darktable.gui->widgets.bottom_lighttable_box;
  gtk_widget_set_visible(widget, FALSE);
  widget = darktable.gui->widgets.plugins_vbox_left;
  gtk_widget_set_visible(widget, FALSE);
  widget = darktable.gui->widgets.import_eventbox;
  gtk_widget_set_visible(widget, FALSE);
  widget = darktable.gui->widgets.module_list_eventbox;
  gtk_widget_set_visible(widget, TRUE);

  // get top level vbox containing all expanders, plugins_vbox:
  GtkBox *box = GTK_BOX(darktable.gui->widgets.plugins_vbox);
  GtkTable *module_list = GTK_TABLE(darktable.gui->widgets.module_list);
  gtk_table_set_row_spacings(module_list,2);
  gtk_table_set_col_spacings(module_list,2);
  GList *modules = g_list_last(dev->iop);
  int ti = 0, tj = 0;
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    module->gui_init(module);
    // add the widget created by gui_init to an expander and both to list.
    GtkWidget *expander = dt_iop_gui_get_expander(module);
    module->topwidget = GTK_WIDGET(expander);
    gtk_box_pack_start(box, expander, FALSE, FALSE, 0);
    module->show_closure = NULL;
    if(strcmp(module->op, "gamma") && !(module->flags() & IOP_FLAGS_DEPRECATED))
    {
      // Connecting the (optional) module show accelerator
      snprintf(accelpath, 256, "<Darktable>/darkroom/plugins/%s/show plugin", module->op);
      module->show_closure = g_cclosure_new(G_CALLBACK(show_module_callback),
                                                       module, NULL);
      dt_accel_group_connect_by_path(darktable.control->accels_darkroom,
                                     accelpath, module->show_closure);

      module->showhide = dtgtk_tristatebutton_new(NULL,0);
      char filename[1024], datadir[1024];
      dt_get_datadir(datadir, 1024);
      snprintf(filename, 1024, "%s/pixmaps/plugins/darkroom/%s.png", datadir, module->op);
      if(!g_file_test(filename, G_FILE_TEST_IS_REGULAR))
        snprintf(filename, 1024, "%s/pixmaps/plugins/darkroom/template.png", datadir);
      GtkWidget *image = gtk_image_new_from_file(filename);
      gtk_button_set_image(GTK_BUTTON(module->showhide), image);
      g_signal_connect(G_OBJECT(module->showhide), "tristate-changed",
                       G_CALLBACK(module_tristate_changed_callback), module);
      gtk_table_attach(module_list, module->showhide, ti, ti+1, tj, tj+1,
                       GTK_FILL | GTK_EXPAND | GTK_SHRINK,
                       GTK_SHRINK,
                       0, 0);
      if(ti < 5) ti++;
      else
      {
        ti = 0;
        tj ++;
      }
    }
    modules = g_list_previous(modules);
  }
  // end marker widget:
  GtkWidget *endmarker = gtk_drawing_area_new();

  gtk_box_pack_start(box, endmarker, FALSE, FALSE, 0);
  g_signal_connect (G_OBJECT (endmarker), "expose-event",
                    G_CALLBACK (dt_control_expose_endmarker), 0);
  gtk_widget_set_size_request(endmarker, -1, 50);

  gtk_widget_show_all(GTK_WIDGET(box));
  gtk_widget_show_all(GTK_WIDGET(module_list));


  /* set list of modules to modulegroups */
  dt_gui_iop_modulegroups_set_list (dev->iop);

  // hack: now hide all custom expander widgets again.
  modules = dev->iop;
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    if(strcmp(module->op, "gamma"))
    {
      char option[1024];
      snprintf(option, 1024, "plugins/darkroom/%s/visible", module->op);
      gboolean active = dt_conf_get_bool (option);
      snprintf(option, 1024, "plugins/darkroom/%s/favorite", module->op);
      gboolean favorite = dt_conf_get_bool (option);
      gint state=0;
      if(active)
      {
        state++;
        if(favorite) state++;
      }

      if(module->showhide)
        dtgtk_tristatebutton_set_state(DTGTK_TRISTATEBUTTON(module->showhide),state);

      snprintf(option, 1024, "plugins/darkroom/%s/expanded", module->op);
      active = dt_conf_get_bool (option);
      gtk_expander_set_expanded (module->expander, active);
    }
    else
    {
      gtk_widget_hide_all(GTK_WIDGET(module->topwidget));
    }
    modules = g_list_next(modules);
  }


  // synch gui and flag gegl pipe as dirty
  // FIXME: this assumes static pipeline as well
  // this is done here and not in dt_read_history, as it would else be triggered before module->gui_init.
  dt_dev_pop_history_items(dev, dev->history_end);

  if(dt_conf_get_bool("plugins/filmstrip/on"))
  {
    // double click callback:
    dt_view_film_strip_scroll_to(darktable.view_manager, dev->image->id);
    dt_view_film_strip_open(darktable.view_manager, film_strip_activated, self);
    dt_view_film_strip_prefetch();
  }

  // switch on groups as they where last time:
  dt_gui_iop_modulegroups_switch(dt_conf_get_int("plugins/darkroom/groups"));

  // get last active plugin:
  gchar *active_plugin = dt_conf_get_string("plugins/darkroom/active");
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

  // image should be there now.
  float zoom_x, zoom_y;
  dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, DT_ZOOM_FIT, 0, NULL, NULL);
  DT_CTL_SET_GLOBAL(dev_zoom_x, zoom_x);
  DT_CTL_SET_GLOBAL(dev_zoom_y, zoom_y);
}


static void
dt_dev_remove_child(GtkWidget *widget, gpointer data)
{
  gtk_container_remove(GTK_CONTAINER(data), widget);
}

void leave(dt_view_t *self)
{
  GSList *c = ((dt_develop_t*)self->data)->closures;

  // Detaching the accelerator group
  gtk_window_remove_accel_group(GTK_WINDOW(darktable.gui->widgets.main_window),
                                darktable.control->accels_darkroom);

  // Disconnecting and deleting the closures
  while(c)
  {
    dt_accel_group_disconnect(darktable.control->accels_darkroom, c->data);
    c = g_slist_next(c);
  }
  g_slist_free(((dt_develop_t*)self->data)->closures);
  ((dt_develop_t*)self->data)->closures = NULL;

  // store groups for next time:
  dt_conf_set_int("plugins/darkroom/groups", dt_gui_iop_modulegroups_get());

  // store last active plugin:
  if(darktable.develop->gui_module)
    dt_conf_set_string("plugins/darkroom/active", darktable.develop->gui_module->op);
  else
    dt_conf_set_string("plugins/darkroom/active", "");

  if(dt_conf_get_bool("plugins/filmstrip/on"))
    dt_view_film_strip_close(darktable.view_manager);

  GList *childs = gtk_container_get_children (
                    GTK_CONTAINER (darktable.gui->widgets.bottom_left_toolbox));
  while(childs)
  {
    gtk_widget_destroy ( GTK_WIDGET (childs->data));
    childs=g_list_next(childs);
  }

  GtkWidget *widget;
  widget = darktable.gui->widgets.navigation_expander;
  gtk_widget_set_visible(widget, FALSE);
  widget = darktable.gui->widgets.histogram_expander;
  gtk_widget_set_visible(widget, FALSE);
  widget = darktable.gui->widgets.snapshots_eventbox;
  gtk_widget_set_visible(widget, FALSE);
  widget = darktable.gui->widgets.history_eventbox;
  gtk_widget_set_visible(widget, FALSE);
  widget = darktable.gui->widgets.bottom_darkroom_box;
  gtk_widget_set_visible(widget, FALSE);
  widget = darktable.gui->widgets.bottom_lighttable_box;
  gtk_widget_set_visible(widget, TRUE);
  widget = darktable.gui->widgets.plugins_vbox_left;
  gtk_widget_set_visible(widget, TRUE);
  widget = darktable.gui->widgets.module_list_eventbox;
  gtk_widget_set_visible(widget, FALSE);

  dt_develop_t *dev = (dt_develop_t *)self->data;
  // tag image as changed
  // TODO: only tag the image when there was a real change.
  guint tagid = 0;
  dt_tag_new("darktable|changed",&tagid);
  dt_tag_attach(tagid, dev->image->id);
  // commit image ops to db
  dt_dev_write_history(dev);
  // write .xmp file
  dt_image_write_sidecar_file(dev->image->id);

  // be sure light table will regenerate the thumbnail:
  if(dev->image) dev->image->force_reimport = 1;

  // clear gui.
  dev->gui_leaving = 1;
  dt_pthread_mutex_lock(&dev->history_mutex);
  dt_dev_pixelpipe_cleanup_nodes(dev->pipe);
  dt_dev_pixelpipe_cleanup_nodes(dev->preview_pipe);
  GtkBox *box = GTK_BOX(darktable.gui->widgets.plugins_vbox);
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
    // printf("removing module %d - %s\n", module->instance, module->op);
    char var[1024];
    snprintf(var, 1024, "plugins/darkroom/%s/expanded", module->op);
    dt_conf_set_bool(var, gtk_expander_get_expanded (module->expander));

    // disconnect the show accelerator
    if(module->show_closure)
      dt_accel_group_disconnect(darktable.control->accels_darkroom,
                                module->show_closure);

    module->gui_cleanup(module);
    dt_iop_cleanup_module(module) ;
    free(module);
    dev->iop = g_list_delete_link(dev->iop, dev->iop);
  }
  gtk_container_foreach(GTK_CONTAINER(box), (GtkCallback)dt_dev_remove_child, (gpointer)box);
  dt_pthread_mutex_unlock(&dev->history_mutex);

  // release full buffer
  if(dev->image->pixels)
    dt_image_release(dev->image, DT_IMAGE_FULL, 'r');

  // release image struct with metadata as well.
  dt_image_cache_flush(dev->image);
  dt_image_cache_release(dev->image, 'r');
  dt_print(DT_DEBUG_CONTROL, "[run_job-] 11 %f in darkroom mode\n", dt_get_wtime());
}

void mouse_leave(dt_view_t *self)
{
  // if we are not hovering over a thumbnail in the filmstrip -> show metadata of opened image.
  dt_develop_t *dev = (dt_develop_t *)self->data;
  int32_t mouse_over_id = dev->image->id;
  DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, mouse_over_id);
  dt_gui_metadata_update();

  // reset any changes the selected plugin might have made.
  dt_control_change_cursor(GDK_LEFT_PTR);
}

void mouse_moved(dt_view_t *self, double x, double y, int which)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;

  // if we are not hovering over a thumbnail in the filmstrip -> show metadata of opened image.
  int32_t mouse_over_id = -1;
  DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
  if(mouse_over_id == -1)
  {
    mouse_over_id = dev->image->id;
    DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, mouse_over_id);
    dt_gui_metadata_update();
  }

  dt_control_t *ctl = darktable.control;
  const int32_t width_i  = self->width;
  const int32_t height_i = self->height;
  int32_t offx = 0.0f, offy = 0.0f;
  if(width_i  > DT_IMAGE_WINDOW_SIZE) offx =   (DT_IMAGE_WINDOW_SIZE-width_i) *.5f;
  if(height_i > DT_IMAGE_WINDOW_SIZE) offy =   (DT_IMAGE_WINDOW_SIZE-height_i)*.5f;
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
    dev->gui_module->color_picker_box[0] = fmaxf(0.0, fminf(.5f+bzoom_x, .5f+zoom_x));
    dev->gui_module->color_picker_box[1] = fmaxf(0.0, fminf(.5f+bzoom_y, .5f+zoom_y));
    dev->gui_module->color_picker_box[2] = fminf(1.0, fmaxf(.5f+bzoom_x, .5f+zoom_x));
    dev->gui_module->color_picker_box[3] = fminf(1.0, fmaxf(.5f+bzoom_y, .5f+zoom_y));

    dev->preview_pipe->changed |= DT_DEV_PIPE_SYNCH;
    dt_dev_invalidate_all(dev);
    dt_control_queue_draw_all();
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
    dt_control_queue_draw_all();
  }
}


int button_released(dt_view_t *self, double x, double y, int which, uint32_t state)
{
  dt_develop_t *dev = darktable.develop;
  const int32_t width_i  = self->width;
  const int32_t height_i = self->height;
  if(width_i  > DT_IMAGE_WINDOW_SIZE) x += (DT_IMAGE_WINDOW_SIZE-width_i) *.5f;
  if(height_i > DT_IMAGE_WINDOW_SIZE) y += (DT_IMAGE_WINDOW_SIZE-height_i)*.5f;

  int handled = 0;
  if(dev->gui_module && dev->gui_module->button_released) handled = dev->gui_module->button_released(dev->gui_module, x, y, which, state);
  if(handled) return handled;
  if(which == 1) dt_control_change_cursor(GDK_LEFT_PTR);
  return 1;
}


int button_pressed(dt_view_t *self, double x, double y, int which, int type, uint32_t state)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  const int32_t width_i  = self->width;
  const int32_t height_i = self->height;
  if(width_i  > DT_IMAGE_WINDOW_SIZE) x += (DT_IMAGE_WINDOW_SIZE-width_i) *.5f;
  if(height_i > DT_IMAGE_WINDOW_SIZE) y += (DT_IMAGE_WINDOW_SIZE-height_i)*.5f;

  int handled = 0;
  if(dev->gui_module && dev->gui_module->request_color_pick && which == 1)
  {
    float zoom_x, zoom_y;
    dt_dev_get_pointer_zoom_pos(dev, x, y, &zoom_x, &zoom_y);
    dev->gui_module->color_picker_box[0] = .5f+zoom_x;
    dev->gui_module->color_picker_box[1] = .5f+zoom_y;
    dev->gui_module->color_picker_box[2] = .5f+zoom_x;
    dev->gui_module->color_picker_box[3] = .5f+zoom_y;
    dt_control_queue_draw_all();
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
  dt_develop_t *dev = (dt_develop_t *)self->data;
  const int32_t width_i  = self->width;
  const int32_t height_i = self->height;
  if(width_i  > DT_IMAGE_WINDOW_SIZE) x += (DT_IMAGE_WINDOW_SIZE-width_i) *.5f;
  if(height_i > DT_IMAGE_WINDOW_SIZE) y += (DT_IMAGE_WINDOW_SIZE-height_i)*.5f;

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
  dt_control_queue_draw_all();
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

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
