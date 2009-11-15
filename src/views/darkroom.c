/** this is the view for the darkroom module.  */
#include "views/view.h"
#include "develop/develop.h"
#include "control/jobs.h"
#include "develop/imageop.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "control/control.h"
#include "gui/gtk.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <glade/glade.h>

const char *name(dt_view_t *self)
{
  return _("darkroom");
}

void init(dt_view_t *self)
{
  self->data = malloc(sizeof(dt_develop_t));
  dt_dev_init((dt_develop_t *)self->data, 1);
}


void cleanup(dt_view_t *self)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  dt_dev_cleanup(dev);
  free(dev);
}


void expose(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  cairo_save(cr);

  dt_develop_t *dev = (dt_develop_t *)self->data;
  
  if(dev->image_dirty || dev->pipe->input_timestamp < dev->preview_pipe->input_timestamp) dt_dev_process_image(dev);
  if(dev->preview_dirty) dt_dev_process_preview(dev);

  pthread_mutex_t *mutex = NULL;
  int wd, ht, stride, closeup;
  int32_t zoom;
  float zoom_x, zoom_y;
  DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);
  DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
  DT_CTL_GET_GLOBAL(zoom, dev_zoom);
  DT_CTL_GET_GLOBAL(closeup, dev_closeup);
  cairo_surface_t *surface = NULL;
   
  if(!dev->image_dirty && dev->pipe->input_timestamp >= dev->preview_pipe->input_timestamp)
  { // draw image
    mutex = &dev->pipe->backbuf_mutex;
    pthread_mutex_lock(mutex);
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
    pthread_mutex_unlock(mutex);
  }
  // else // if(!dev->preview_dirty)
  else if(!dev->preview_loading)
  { // draw preview
    mutex = &dev->preview_pipe->backbuf_mutex;
    pthread_mutex_lock(mutex);

    wd = dev->preview_pipe->backbuf_width;
    ht = dev->preview_pipe->backbuf_height;
    float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2 : 1, 1);
    cairo_set_source_rgb (cr, .2, .2, .2);
    cairo_paint(cr);
    stride = cairo_format_stride_for_width (CAIRO_FORMAT_RGB24, wd);
    cairo_surface_t *surface = cairo_image_surface_create_for_data (dev->preview_pipe->backbuf, CAIRO_FORMAT_RGB24, wd, ht, stride); 
    cairo_translate(cr, width/2.0, height/2.0f);
    cairo_scale(cr, zoom_scale, zoom_scale);
    cairo_translate(cr, -.5f*wd-zoom_x*wd, -.5f*ht-zoom_y*ht);
    cairo_rectangle(cr, 0, 0, wd, ht);
    cairo_set_source_surface (cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
    cairo_fill(cr);
    cairo_surface_destroy (surface);

    pthread_mutex_unlock(mutex);
  }
  cairo_restore(cr);

  // execute module callback hook.
  if(dev->gui_module && dev->gui_module->gui_post_expose)
    dev->gui_module->gui_post_expose(dev->gui_module, cr, width, height, pointerx, pointery);
}


void reset(dt_view_t *self)
{
  DT_CTL_SET_GLOBAL(dev_zoom, DT_ZOOM_FIT);
  DT_CTL_SET_GLOBAL(dev_zoom_x, 0);
  DT_CTL_SET_GLOBAL(dev_zoom_y, 0);
  DT_CTL_SET_GLOBAL(dev_closeup, 0);
}


void enter(dt_view_t *self)
{
  GtkWidget *widget;
  widget = glade_xml_get_widget (darktable.gui->main_window, "navigation_expander");
  gtk_widget_set_visible(widget, TRUE);
  widget = glade_xml_get_widget (darktable.gui->main_window, "histogram_expander");
  gtk_widget_set_visible(widget, TRUE);
  widget = glade_xml_get_widget (darktable.gui->main_window, "history_expander");
  gtk_widget_set_visible(widget, TRUE);

  dt_develop_t *dev = (dt_develop_t *)self->data;
  int selected;
  DT_CTL_GET_GLOBAL(selected, lib_image_mouse_over_id);
  if(selected >= 0)
  {
    DT_CTL_SET_GLOBAL(dev_zoom, DT_ZOOM_FIT);
    DT_CTL_SET_GLOBAL(dev_zoom_x, 0);
    DT_CTL_SET_GLOBAL(dev_zoom_y, 0);
    DT_CTL_SET_GLOBAL(dev_closeup, 0);
  }

  dev->gui_leaving = 0;
  dev->gui_module = NULL;
  dt_dev_load_image(dev, dt_image_cache_use(selected, 'r'));
  // get top level vbox containing all expanders, iop_vbox:
  GtkBox *box = GTK_BOX(glade_xml_get_widget (darktable.gui->main_window, "iop_vbox"));
  GList *modules = g_list_last(dev->iop);
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    module->gui_init(module);
    // add the widget created by gui_init to an expander and both to list.
    GtkWidget *expander = dt_iop_gui_get_expander(module);
    gtk_box_pack_start(box, expander, FALSE, FALSE, 0);
    modules = g_list_previous(modules);
  }
  // end marker widget:
  GtkWidget *endmarker = gtk_drawing_area_new();
  gtk_widget_set_size_request(GTK_WIDGET(endmarker), 250, 50);
  gtk_box_pack_start(box, endmarker, FALSE, FALSE, 0);
  g_signal_connect (G_OBJECT (endmarker), "expose-event",
                    G_CALLBACK (dt_control_expose_endmarker), 0);
  gtk_widget_show_all(GTK_WIDGET(box));
  // hack: now hide all custom expander widgets again.
  modules = dev->iop;
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    if(!gtk_expander_get_expanded (module->expander)) gtk_widget_hide(module->widget);
    modules = g_list_next(modules);
  }
  // synch gui and flag gegl pipe as dirty
  // FIXME: this assumes static pipeline as well
  // this is done here and not in dt_read_history, as it would else be triggered before module->gui_init.
  dt_dev_pop_history_items(dev, dev->history_end);

  // image should be there now.
  float zoom_x, zoom_y;
  dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, DT_ZOOM_FIT, 0, NULL, NULL);
  DT_CTL_SET_GLOBAL(dev_zoom_x, zoom_x);
  DT_CTL_SET_GLOBAL(dev_zoom_y, zoom_y);
}


void dt_dev_remove_child(GtkWidget *widget, gpointer data)
{
  gtk_container_remove(GTK_CONTAINER(data), widget);
}

void leave(dt_view_t *self)
{
  GtkWidget *widget;
  widget = glade_xml_get_widget (darktable.gui->main_window, "navigation_expander");
  gtk_widget_set_visible(widget, FALSE);
  widget = glade_xml_get_widget (darktable.gui->main_window, "histogram_expander");
  gtk_widget_set_visible(widget, FALSE);
  widget = glade_xml_get_widget (darktable.gui->main_window, "history_expander");
  gtk_widget_set_visible(widget, FALSE);

  dt_develop_t *dev = (dt_develop_t *)self->data;
  // commit image ops to db
  dt_dev_write_history(dev);
  // write .dt file
  if(gconf_client_get_bool(darktable.control->gconf, DT_GCONF_DIR"/write_dt_files", NULL))
  {
    char filename[512];
    dt_image_full_path(dev->image, filename, 512);
    char *c = filename + strlen(filename);
    for(;c>filename && *c != '.';c--);
    sprintf(c, ".dt");
    dt_imageio_dt_write(dev->image->id, filename);
  }

  // commit updated mipmaps to db
  if(dev->mipf)
  {
    int wd, ht;
    float fwd, fht;
    // set processed width to something useful while image is not there yet:
    dt_dev_get_processed_size(dev, &dev->image->output_width, &dev->image->output_height);

    dt_image_get_mip_size(dev->image, DT_IMAGE_MIP4, &wd, &ht);
    dt_image_get_exact_mip_size(dev->image, DT_IMAGE_MIP4, &fwd, &fht);
    dev->preview_downsampling = 1.0;
    dt_dev_process_preview_job(dev);
    if(dt_image_alloc(dev->image, DT_IMAGE_MIP4))
    {
      fprintf(stderr, "[dev_leave] could not alloc mip4 to write mipmaps!\n");
      return;
    }
    dt_image_check_buffer(dev->image, DT_IMAGE_MIP4, sizeof(uint8_t)*4*wd*ht);
    pthread_mutex_lock(&(dev->preview_pipe->backbuf_mutex));

    dt_iop_clip_and_zoom_8(dev->preview_pipe->backbuf, 0, 0, dev->preview_pipe->backbuf_width, dev->preview_pipe->backbuf_height, 
                                                             dev->preview_pipe->backbuf_width, dev->preview_pipe->backbuf_height, 
                       dev->image->mip[DT_IMAGE_MIP4], 0, 0, fwd, fht, wd, ht);

    dt_image_release(dev->image, DT_IMAGE_MIP4, 'w');
    pthread_mutex_unlock(&(dev->preview_pipe->backbuf_mutex));
    if(dt_imageio_preview_write(dev->image, DT_IMAGE_MIP4))
      fprintf(stderr, "[dev_leave] could not write mip level %d of image %s to database!\n", DT_IMAGE_MIP4, dev->image->filename);
    dt_image_update_mipmaps(dev->image);

    dt_image_release(dev->image, DT_IMAGE_MIP4, 'r');
    dt_image_release(dev->image, DT_IMAGE_MIPF, 'r');
  }

  // clear gui.
  dev->gui_leaving = 1;
  pthread_mutex_lock(&dev->history_mutex);
  GtkBox *box = GTK_BOX(glade_xml_get_widget (darktable.gui->main_window, "iop_vbox"));
  while(dev->history)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(dev->history->data);
    // printf("removing history item %d - %s, data %f %f\n", hist->module->instance, hist->module->op, *(float *)hist->params, *((float *)hist->params+1));
    free(hist->params); hist->params = NULL;
    dev->history = g_list_delete_link(dev->history, dev->history);
  }
  while(dev->iop)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(dev->iop->data);
    // printf("removing module %d - %s\n", module->instance, module->op);
    module->gui_cleanup(module);
    module->cleanup(module);
    dev->iop = g_list_delete_link(dev->iop, dev->iop);
  }
  gtk_container_foreach(GTK_CONTAINER(box), (GtkCallback)dt_dev_remove_child, (gpointer)box);
  pthread_mutex_unlock(&dev->history_mutex);
  
  // release full buffer
  if(dev->image->pixels)
    dt_image_release(dev->image, DT_IMAGE_FULL, 'r');

  DT_CTL_SET_GLOBAL_STR(dev_op, "original", 20);

  // release image struct with metadata as well.
  dt_image_cache_flush(dev->image);
  dt_image_cache_release(dev->image, 'r');
}


// void mouse_leave(dt_view_t *self) {}

void mouse_moved(dt_view_t *self, double x, double y, int which)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  int handled = 0;
  if(dev->gui_module && dev->gui_module->mouse_moved) handled = dev->gui_module->mouse_moved(dev->gui_module, x, y, which);
  if(handled) return;

  if(darktable.control->button_down && darktable.control->button_down_which == 1)
  { // depending on dev_zoom, adjust dev_zoom_x/y.
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
    float zx = old_zoom_x - (1.0/scale)*(x - darktable.control->button_x)/procw;
    float zy = old_zoom_y - (1.0/scale)*(y - darktable.control->button_y)/proch;
    dt_dev_check_zoom_bounds(dev, &zx, &zy, zoom, closeup, NULL, NULL);
    DT_CTL_SET_GLOBAL(dev_zoom_x, zx);
    DT_CTL_SET_GLOBAL(dev_zoom_y, zy);
    darktable.control->button_x = x;
    darktable.control->button_y = y;
    dt_dev_invalidate(dev);
    dt_control_queue_draw_all();
  }
}

void button_released(dt_view_t *self, double x, double y, int which, uint32_t state)
{
  if(which == 1) dt_control_change_cursor(GDK_ARROW);
}

void button_pressed(dt_view_t *self, double x, double y, int which, int type, uint32_t state)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  int handled = 0;
  if(dev->gui_module && dev->gui_module->button_pressed) handled = dev->gui_module->button_pressed(dev->gui_module, x, y, which, type, state);
  if(handled) return;

  if(which == 1) dt_control_change_cursor(GDK_HAND1);
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
  }
}

void scrolled(dt_view_t *self, double x, double y, int up)
{ // free zoom
  dt_develop_t *dev = (dt_develop_t *)self->data;
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
  if(up) scale += .1f*(1.0f - minscale);
  else   scale -= .1f*(1.0f - minscale);
  DT_CTL_SET_GLOBAL(dev_zoom_scale, scale);
  if(scale > 0.99)            zoom = DT_ZOOM_1;
  if(scale < minscale + 0.01) zoom = DT_ZOOM_FIT;
  zoom_x -= mouse_off_x/(procw*scale);
  zoom_y -= mouse_off_y/(proch*scale);
  dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, zoom, closeup, NULL, NULL);
  DT_CTL_SET_GLOBAL(dev_zoom, zoom);
  DT_CTL_SET_GLOBAL(dev_closeup, closeup);
  DT_CTL_SET_GLOBAL(dev_zoom_x, zoom_x);
  DT_CTL_SET_GLOBAL(dev_zoom_y, zoom_y);
  dt_dev_invalidate(dev);
}

void key_pressed(dt_view_t *self, uint16_t which)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  int zoom, closeup;
  float zoom_x, zoom_y;
  switch (which)
  {
    case KEYCODE_1:
      DT_CTL_GET_GLOBAL(zoom, dev_zoom);
      DT_CTL_GET_GLOBAL(closeup, dev_closeup);
      if(zoom == DT_ZOOM_1) closeup ^= 1;
      DT_CTL_SET_GLOBAL(dev_closeup, closeup);
      DT_CTL_SET_GLOBAL(dev_zoom, DT_ZOOM_1);
      dt_dev_invalidate(dev);
      break;
    case KEYCODE_2:
      DT_CTL_SET_GLOBAL(dev_zoom, DT_ZOOM_FILL);
      dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, DT_ZOOM_FILL, 0, NULL, NULL);
      DT_CTL_SET_GLOBAL(dev_zoom_x, zoom_x);
      DT_CTL_SET_GLOBAL(dev_zoom_y, zoom_y);
      DT_CTL_SET_GLOBAL(dev_closeup, 0);
      dt_dev_invalidate(dev);
      break;
    case KEYCODE_3:
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

void configure(dt_view_t *self, int wd, int ht)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  dt_dev_configure(dev, wd, ht);
}

