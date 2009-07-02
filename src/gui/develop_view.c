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

void dt_dev_enter()
{
  int selected;
  DT_CTL_GET_GLOBAL(selected, lib_image_mouse_over_id);

  DT_CTL_SET_GLOBAL(dev_zoom_x, 0);
  DT_CTL_SET_GLOBAL(dev_zoom_y, 0);

  darktable.develop->gui_leaving = 0;
  dt_dev_load_image(darktable.develop, dt_image_cache_use(selected, 'r'));
  // get top level vbox containing all expanders, iop_vbox:
  GtkBox *box = GTK_BOX(glade_xml_get_widget (darktable.gui->main_window, "iop_vbox"));
  GList *modules = g_list_last(darktable.develop->iop);
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    GtkExpander *expander = GTK_EXPANDER(gtk_expander_new((const gchar *)(module->op)));
    gtk_expander_set_expanded(expander, FALSE);
    gtk_expander_set_spacing(expander, 10);
    gtk_box_pack_start(box, GTK_WIDGET(expander), FALSE, FALSE, 0);
    module->gui_init(module);
    // add the widget created by gui_init to the expander.
    gtk_container_add(GTK_CONTAINER(expander), module->widget);
    modules = g_list_previous(modules);
  }
  gtk_widget_show_all(GTK_WIDGET(box));
  // synch gui and flag gegl pipe as dirty
  // FIXME: this assumes static pipeline as well
  // this is done here and not in dt_read_history, as it would else be triggered before module->gui_init.
  dt_dev_pop_history_items(darktable.develop, darktable.develop->history_end);
}

void dt_dev_remove_child(GtkWidget *widget, gpointer data)
{
  gtk_container_remove(GTK_CONTAINER(data), widget);
}

void dt_dev_leave()
{
  // commit image ops to db
  dt_dev_write_history(darktable.develop);

  // commit updated mipmaps to db
  if(darktable.develop->mipf)
  {
    int wd, ht;
    dt_image_get_mip_size(darktable.develop->image, DT_IMAGE_MIPF, &wd, &ht);
    dt_dev_process_preview_job(darktable.develop);
    if(dt_image_alloc(darktable.develop->image, DT_IMAGE_MIP4))
    {
      fprintf(stderr, "[dev_leave] could not alloc mip4 to write mipmaps!\n");
      return;
    }
    dt_image_check_buffer(darktable.develop->image, DT_IMAGE_MIP4, sizeof(uint8_t)*4*wd*ht);
    pthread_mutex_lock(&(darktable.develop->preview_pipe->backbuf_mutex));
    memcpy(darktable.develop->image->mip[DT_IMAGE_MIP4], darktable.develop->preview_pipe->backbuf, sizeof(uint8_t)*4*wd*ht);
    dt_image_release(darktable.develop->image, DT_IMAGE_MIP4, 'w');
    pthread_mutex_unlock(&(darktable.develop->preview_pipe->backbuf_mutex));
    if(dt_imageio_preview_write(darktable.develop->image, DT_IMAGE_MIP4))
      fprintf(stderr, "[dev_leave] could not write mip level %d of image %s to database!\n", DT_IMAGE_MIP4, darktable.develop->image->filename);
    dt_image_update_mipmaps(darktable.develop->image);

    dt_image_release(darktable.develop->image, DT_IMAGE_MIP4, 'r');
    dt_image_release(darktable.develop->image, DT_IMAGE_MIPF, 'r');
  }

  // clear gui.
  dt_develop_t *dev = darktable.develop;
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
  if(darktable.develop->image->pixels)
    dt_image_release(darktable.develop->image, DT_IMAGE_FULL, 'r');

  DT_CTL_SET_GLOBAL_STR(dev_op, "original", 20);

  // release image struct with metadata as well.
  dt_image_cache_release(darktable.develop->image, 'r');
}

void dt_dev_expose(dt_develop_t *dev, cairo_t *cr, int32_t width, int32_t height)
{
  if(dev->image_dirty)   dt_dev_process_image(dev);
  if(dev->preview_dirty) dt_dev_process_preview(dev);

  pthread_mutex_t *mutex = NULL;
  int wd, ht, stride;
  cairo_surface_t *surface = NULL;

  if(dev->image_dirty && !dev->preview_dirty)
  { // draw preview
    mutex = &dev->preview_pipe->backbuf_mutex;
    pthread_mutex_lock(mutex);
    // wd = dev->capwidth_preview;
    // ht = dev->capheight_preview;
    // stride = cairo_format_stride_for_width (CAIRO_FORMAT_RGB24, wd);
    // surface = cairo_image_surface_create_for_data (dev->preview_pipe->backbuf, CAIRO_FORMAT_RGB24, wd, ht, stride); 

    // TODO: replace by continuous zoom 
    int32_t zoom, closeup;
    float zoom_x, zoom_y;
    DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);
    DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
    DT_CTL_GET_GLOBAL(zoom, dev_zoom);
    DT_CTL_GET_GLOBAL(closeup, dev_closeup);
    float zoom_scale;
    switch(zoom)
    {
      case DT_ZOOM_FIT:
        zoom_scale = fminf(width/dev->mipf_exact_width, height/dev->mipf_exact_height);
        break;
      case DT_ZOOM_FILL:
        zoom_scale = fmaxf(width/dev->mipf_exact_width, height/dev->mipf_exact_height);
        break;
      default: // 1:1 or higher
        zoom_scale = dev->image->width/dev->mipf_exact_width;
        if(closeup) zoom_scale *= 2.0;
        break;
    }
    cairo_set_source_rgb (cr, .2, .2, .2);
    cairo_paint(cr);
    stride = cairo_format_stride_for_width (CAIRO_FORMAT_RGB24, dev->mipf_width);
    cairo_surface_t *surface = cairo_image_surface_create_for_data (dev->preview_pipe->backbuf, CAIRO_FORMAT_RGB24, dev->mipf_width, dev->mipf_height, stride); 
    cairo_translate(cr, width/2.0, height/2.0f);
    // cairo_translate(cr, (dev->small_raw_width-fwd), (dev->small_raw_height-fht));
    // cairo_scale(cr, zoom_scale*(dev->small_raw_width/fwd), zoom_scale*(dev->small_raw_height/fht));
    cairo_scale(cr, zoom_scale, zoom_scale);
    cairo_translate(cr, -.5f*dev->mipf_exact_width-zoom_x*dev->mipf_exact_width, -.5f*dev->mipf_exact_height-zoom_y*dev->mipf_exact_height);
    // cairo_translate(cr, -1, -1); // compensate jumping draw below pointer
    cairo_rectangle(cr, 0, 0, dev->mipf_exact_width, dev->mipf_exact_height);
    cairo_set_source_surface (cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
    cairo_fill(cr);
    cairo_surface_destroy (surface);

    /*
    cairo_set_source_rgb (cr, .2, .2, .2);
    cairo_paint(cr);
    cairo_translate(cr, .5f*(width-wd), .5f*(height-ht));
    cairo_rectangle(cr, 0, 0, wd, ht);
    cairo_set_source_surface (cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgb (cr, .3, .3, .3);
    cairo_stroke(cr);
    cairo_surface_destroy (surface);
    */
    pthread_mutex_unlock(mutex);
  }
  else if(!dev->image_dirty)
  { // draw image
    mutex = &dev->pipe->backbuf_mutex;
    pthread_mutex_lock(mutex);
    wd = dev->capwidth;
    ht = dev->capheight;
    stride = cairo_format_stride_for_width (CAIRO_FORMAT_RGB24, wd);
    surface = cairo_image_surface_create_for_data (dev->pipe->backbuf, CAIRO_FORMAT_RGB24, wd, ht, stride); 
    cairo_set_source_rgb (cr, .2, .2, .2);
    cairo_paint(cr);
    cairo_translate(cr, .5f*(width-wd), .5f*(height-ht));
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
  // TODO: execute module callback hook!
}

