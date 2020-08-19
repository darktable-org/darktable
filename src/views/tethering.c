/*
    This file is part of darktable,
    Copyright (C) 2014-2020 darktable developers.

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

/** this is the view for the capture module.
    The capture module purpose is to allow a workflow for capturing images
    which is module extendable but main purpos is to support tethered capture
    using gphoto library.

    When entered a session is constructed = one empty filmroll might be same filmroll
    as earlier created dependent on capture filesystem structure...

    TODO: How to pass initialized data such as dt_camera_t ?

*/


#include "common/camera_control.h"
#include "common/collection.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/iop_profile.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/import_session.h"
#include "common/selection.h"
#include "common/utility.h"
#include "common/variables.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "control/settings.h"
#include "dtgtk/thumbtable.h"
#include "gui/accelerators.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "views/view_api.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <gdk/gdkkeysyms.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

DT_MODULE(1)

/** module data for the capture view */
typedef struct dt_capture_t
{
  /** The current image activated in capture view, either latest tethered shoot
    or manually picked from filmstrip view...
  */
  int32_t image_id;

  dt_view_image_over_t image_over;

  struct dt_import_session_t *session;

  /** default listener taking care of downloading & importing images */
  dt_camctl_listener_t *listener;

  /** Cursor position for dragging the zoomed live view */
  double live_view_zoom_cursor_x, live_view_zoom_cursor_y;

  gboolean busy;
} dt_capture_t;

/* signal handler for filmstrip image switching */
static void _view_capture_filmstrip_activate_callback(gpointer instance, int imgid, gpointer user_data);

static void _capture_view_set_jobcode(const dt_view_t *view, const char *name);
static const char *_capture_view_get_jobcode(const dt_view_t *view);
static uint32_t _capture_view_get_selected_imgid(const dt_view_t *view);

const char *name(const dt_view_t *self)
{
  return _("tethering");
}

uint32_t view(const dt_view_t *self)
{
  return DT_VIEW_TETHERING;
}

static void _view_capture_filmstrip_activate_callback(gpointer instance, int imgid, gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_capture_t *lib = (dt_capture_t *)self->data;

  lib->image_id = imgid;
  dt_view_active_images_reset(FALSE);
  dt_view_active_images_add(lib->image_id, TRUE);
  if(imgid >= 0)
  {
    dt_collection_memory_update();
    dt_selection_select_single(darktable.selection, imgid);
    dt_thumbtable_set_offset_image(dt_ui_thumbtable(darktable.gui->ui), imgid, TRUE);
    dt_control_queue_redraw_center();
  }
}

void init(dt_view_t *self)
{
  self->data = calloc(1, sizeof(dt_capture_t));

  /* setup the tethering view proxy */
  darktable.view_manager->proxy.tethering.view               = self;
  darktable.view_manager->proxy.tethering.get_job_code       = _capture_view_get_jobcode;
  darktable.view_manager->proxy.tethering.set_job_code       = _capture_view_set_jobcode;
  darktable.view_manager->proxy.tethering.get_selected_imgid = _capture_view_get_selected_imgid;
}

void cleanup(dt_view_t *self)
{
  free(self->data);
}


static uint32_t _capture_view_get_selected_imgid(const dt_view_t *view)
{
  g_assert(view != NULL);
  dt_capture_t *cv = (dt_capture_t *)view->data;
  return cv->image_id;
}

static void _capture_view_set_jobcode(const dt_view_t *view, const char *name)
{
  g_assert(view != NULL);
  dt_capture_t *cv = (dt_capture_t *)view->data;
  dt_import_session_set_name(cv->session, name);
  dt_film_open(dt_import_session_film_id(cv->session));
  dt_control_log(_("new session initiated '%s'"), name);
}

static const char *_capture_view_get_jobcode(const dt_view_t *view)
{
  g_assert(view != NULL);
  dt_capture_t *cv = (dt_capture_t *)view->data;
  return dt_import_session_name(cv->session);
}

void configure(dt_view_t *self, int wd, int ht)
{
  // dt_capture_t *lib=(dt_capture_t*)self->data;
}

#define MARGIN DT_PIXEL_APPLY_DPI(20)
#define BAR_HEIGHT DT_PIXEL_APPLY_DPI(18) /* see libs/camera.c */

static gboolean _expose_again(gpointer user_data)
{
  dt_control_queue_redraw_center();
  return FALSE;
}

static void _expose_tethered_mode(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx,
                                  int32_t pointery)
{
  dt_capture_t *lib = (dt_capture_t *)self->data;
  dt_camera_t *cam = (dt_camera_t *)darktable.camctl->active_camera;
  if(!cam) return;

  lib->image_over = DT_VIEW_DESERT;
  GSList *l = dt_view_active_images_get();
  if(g_slist_length(l) > 0)
    lib->image_id = GPOINTER_TO_INT(g_slist_nth_data(l, 0));

  lib->image_over = lib->image_id;

  if(cam->is_live_viewing == TRUE) // display the preview
  {
    dt_pthread_mutex_lock(&cam->live_view_pixbuf_mutex);
    if(GDK_IS_PIXBUF(cam->live_view_pixbuf))
    {
      const gint pw = gdk_pixbuf_get_width(cam->live_view_pixbuf);
      const gint ph = gdk_pixbuf_get_height(cam->live_view_pixbuf);
      const gint p_stride = gdk_pixbuf_get_rowstride(cam->live_view_pixbuf);
      const gint p_channels = gdk_pixbuf_get_n_channels(cam->live_view_pixbuf);
      const guint8 *const p_buf = gdk_pixbuf_read_pixels(cam->live_view_pixbuf);
      float *const tmp_f = dt_alloc_align(64, sizeof(float) * pw * ph * 4);
      if(tmp_f)
      {
        // Convert to float as:
        //  - the data needs to be converted to float anyhow for histogram to work on it
        //  - don't lose precision when the data is converted to display then histogram colorspace
        //  - this lets us use the nice iop_profile conversion code
        dt_imageio_flip_buffers_ui8_to_float(tmp_f, p_buf, 0.0f, 255.0f, p_channels,
                                             pw, ph, pw, ph, p_stride, ORIENTATION_NONE);
        // In theory we could convert to display colorspace in
        // camera_control, as conversion here could happen multiple
        // times if multiple expose events happen. But it seems good
        // to keep camera_control simple, and keep view-related code
        // here. And there is an unlikely case where the display
        // profile would change after the live view image is received
        // but before it is display. And in most cases the new live
        // view image will arrive before the next expose event.
        // FIXME: if liveview image is tagged and we can read its colorspace, use that
        const dt_iop_order_iccprofile_info_t *const profile_info_from
            = dt_ioppr_add_profile_info_to_list(darktable.develop, DT_COLORSPACE_SRGB, "", INTENT_PERCEPTUAL);
        const dt_iop_order_iccprofile_info_t *const profile_info_to
            = dt_ioppr_add_profile_info_to_list(darktable.develop, darktable.color_profiles->display_type,
                                                darktable.color_profiles->display_filename, INTENT_PERCEPTUAL);
        // QUESTION: if converted tmp_f straight to histogram colorspace would the histogram quality be any better?
        // QUESTION: can we dither or add noise in this conversion to make it look better?
        dt_ioppr_transform_image_colorspace_rgb(tmp_f, tmp_f, pw, ph,
                                                profile_info_from, profile_info_to, "live view");

        int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, pw);
        uint8_t *const tmp_i = dt_alloc_align(64, sizeof(uint8_t) * pw * ph * 4);
        if(tmp_i)
        {
          // FIXME: use 10-bit as long as we have the extra preicsion?
          // FIXME: is there already a fast routine to do this?
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(pw, ph, stride) \
    shared(tmp_f, tmp_i) \
    schedule(simd:static) aligned(tmp_f, tmp_i:64)
#endif
          for(int y=0; y<ph; y++)
            for(int x=0; x<pw; x++)
              for(int k=0; k<3; k++)
                tmp_i[y*stride+x*4+k]=CLAMP(tmp_f[4*(y*pw+x)+(2-k)]*255.0f,0,255);
          cairo_surface_t *source
            = dt_cairo_image_surface_create_for_data(tmp_i, CAIRO_FORMAT_RGB24, pw, ph, stride);
          if(cairo_surface_status(source) == CAIRO_STATUS_SUCCESS)
          {
            const float w = width - (MARGIN * 2.0f);
            const float h = height - (MARGIN * 2.0f) - BAR_HEIGHT;
            float scale;
            if(cam->live_view_rotation % 2 == 0)
              scale = fminf(w / pw, h / ph);
            else
              scale = fminf(w / ph, h / pw);
            scale = fminf(1.0, scale);

            // FIXME: use cairo_pattern_set_filter()?
            cairo_translate(cr, width * 0.5, (height + BAR_HEIGHT) * 0.5);                    // origin to middle of canvas
            // FIXME: should do rotate/flip in dt_imageio_flip_buffers_ui8_to_float() so that histogram corresponds?
            if(cam->live_view_flip == TRUE) cairo_scale(cr, -1.0, 1.0);                       // mirror image
            if(cam->live_view_rotation) cairo_rotate(cr, -M_PI_2 * cam->live_view_rotation);  // rotate around middle
            if(cam->live_view_zoom == FALSE) cairo_scale(cr, scale, scale);                   // scale to fit canvas
            cairo_translate(cr, -0.5 * pw, -0.5 * ph);                                        // origin back to corner
            cairo_scale(cr, darktable.gui->ppd, darktable.gui->ppd);
            cairo_set_source_surface(cr, source, 0.0, 0.0);
            cairo_paint(cr);
          }
          cairo_surface_destroy(source);
          dt_free_align(tmp_i);
        }
        // FIXME: this histogram isn't a precise match for when the equivalent image is captured -- though the live view histogram is a good match -- is something off?
        darktable.lib->proxy.histogram.process(darktable.lib->proxy.histogram.module, tmp_f, pw, ph);
        dt_control_queue_redraw_widget(darktable.lib->proxy.histogram.module->widget);
        dt_free_align(tmp_f);
      }
    }
    dt_pthread_mutex_unlock(&cam->live_view_pixbuf_mutex);
  }
  // FIXME: set histogram data to blank and draw blank if there is no active image -- or make a test in histogram draw which will know to draw it blank
  else if(lib->image_id >= 0) // First of all draw image if available
  {
    // FIXME: every time the mouse moves over the center view this redraws, which isn't necessary
    cairo_surface_t *surf = NULL;
    // Note that this will also update the histogram. As the histogram
    // is calculated from the 8-bit JPEG, there may be banding, and
    // overexposed pixels are clipped. In the case of an image not
    // heavily processed by presets, this should be an OK trade-off
    // for generating a histogram without too much extra code. An
    // alternative would be to spin up a non-gui pixelpipe (as with
    // export) and use its preview path to process the image and
    // generate a histogram. Or to use dt_imageio_export_with_flags()
    // with thumbnail_export set to TRUE.
    const int res
        = dt_view_image_get_surface(lib->image_id, width - (MARGIN * 2.0f), height - (MARGIN * 2.0f), &surf, FALSE);
    if(res)
    {
      // if the image is missing, we reload it again
      g_timeout_add(250, _expose_again, NULL);
      if(!lib->busy) dt_control_log_busy_enter();
      lib->busy = TRUE;
    }
    else
    {
      cairo_translate(cr, (width - cairo_image_surface_get_width(surf)) / 2,
                      (height - cairo_image_surface_get_height(surf)) / 2);
      cairo_set_source_surface(cr, surf, 0.0, 0.0);
      cairo_paint(cr);
      cairo_surface_destroy(surf);
      if(lib->busy) dt_control_log_busy_leave();
      lib->busy = FALSE;
    }
  }
}


void expose(dt_view_t *self, cairo_t *cri, int32_t width, int32_t height, int32_t pointerx,
            int32_t pointery)
{
  cairo_set_source_rgb(cri, .2, .2, .2);
  cairo_rectangle(cri, 0, 0, width, height);
  cairo_fill(cri);

  // Expose tethering center view
  cairo_save(cri);

  _expose_tethered_mode(self, cri, width, height, pointerx, pointery);

  cairo_restore(cri);

  // post expose to modules
  GList *modules = darktable.lib->plugins;

  while(modules)
  {
    dt_lib_module_t *module = (dt_lib_module_t *)(modules->data);
    if(module->gui_post_expose && dt_lib_is_visible_in_view(module, self))
      module->gui_post_expose(module, cri, width, height, pointerx, pointery);
    modules = g_list_next(modules);
  }
}

int try_enter(dt_view_t *self)
{
  /* verify that camera supports tethering and is available */
  if(dt_camctl_can_enter_tether_mode(darktable.camctl, NULL)) return 0;

  dt_control_log(_("no camera with tethering support available for use..."));
  return 1;
}

static void _capture_mipmaps_updated_signal_callback(gpointer instance, int imgid, gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  struct dt_capture_t *lib = (dt_capture_t *)self->data;

  lib->image_id = imgid;
  dt_view_active_images_reset(FALSE);
  dt_view_active_images_add(lib->image_id, TRUE);

  dt_control_queue_redraw_center();
}


/** callbacks to deal with images taken in tethering mode */
static const char *_camera_request_image_filename(const dt_camera_t *camera, const char *filename,
                                                  time_t *exif_time, void *data)
{
  struct dt_capture_t *lib = (dt_capture_t *)data;

  /* update import session with original filename so that $(FILE_EXTENSION)
   *     and alikes can be expanded. */
  dt_import_session_set_filename(lib->session, filename);
  const gchar *file = dt_import_session_filename(lib->session, FALSE);

  if(file == NULL) return NULL;

  return g_strdup(file);
}

static const char *_camera_request_image_path(const dt_camera_t *camera, time_t *exif_time, void *data)
{
  struct dt_capture_t *lib = (dt_capture_t *)data;
  return dt_import_session_path(lib->session, FALSE);
}

static void _camera_capture_image_downloaded(const dt_camera_t *camera, const char *filename, void *data)
{
  dt_capture_t *lib = (dt_capture_t *)data;

  /* create an import job of downloaded image */
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_BG,
                     dt_image_import_job_create(dt_import_session_film_id(lib->session), filename));
}


void enter(dt_view_t *self)
{
  dt_capture_t *lib = (dt_capture_t *)self->data;

  // no active image when entering the tethering view
  lib->image_over = DT_VIEW_DESERT;
  GSList *l = dt_view_active_images_get();
  if(g_slist_length(l) > 0)
    lib->image_id = GPOINTER_TO_INT(g_slist_nth_data(l, 0));
  else
    lib->image_id = -1;

  dt_view_active_images_reset(FALSE);
  dt_view_active_images_add(lib->image_id, TRUE);
  dt_thumbtable_set_offset_image(dt_ui_thumbtable(darktable.gui->ui), lib->image_id, TRUE);

  /* initialize a session */
  lib->session = dt_import_session_new();

  char *tmp = dt_conf_get_string("plugins/capture/jobcode");
  if(tmp != NULL)
  {
    _capture_view_set_jobcode(self, tmp);
    g_free(tmp);
  }

  /* connect signal for mipmap update for a redraw */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_MIPMAP_UPDATED,
                            G_CALLBACK(_capture_mipmaps_updated_signal_callback), (gpointer)self);


  /* connect signal for fimlstrip image activate */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE,
                            G_CALLBACK(_view_capture_filmstrip_activate_callback), self);

  // register listener
  lib->listener = g_malloc0(sizeof(dt_camctl_listener_t));
  lib->listener->data = lib;
  lib->listener->image_downloaded = _camera_capture_image_downloaded;
  lib->listener->request_image_path = _camera_request_image_path;
  lib->listener->request_image_filename = _camera_request_image_filename;
  dt_camctl_register_listener(darktable.camctl, lib->listener);
}

void leave(dt_view_t *self)
{
  dt_capture_t *cv = (dt_capture_t *)self->data;

  dt_camctl_unregister_listener(darktable.camctl, cv->listener);
  g_free(cv->listener);
  cv->listener = NULL;

  /* destroy session, will cleanup empty film roll */
  dt_import_session_destroy(cv->session);

  /* disconnect from mipmap updated signal */
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_capture_mipmaps_updated_signal_callback),
                               (gpointer)self);

  /* disconnect from filmstrip image activate */
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_view_capture_filmstrip_activate_callback),
                               (gpointer)self);
}

void reset(dt_view_t *self)
{
  // dt_control_set_mouse_over_id(-1);
}

void mouse_moved(dt_view_t *self, double x, double y, double pressure, int which)
{
  dt_capture_t *lib = (dt_capture_t *)self->data;
  dt_camera_t *cam = (dt_camera_t *)darktable.camctl->active_camera;
  // pan the zoomed live view
  if(cam->live_view_pan && cam->live_view_zoom && cam->is_live_viewing)
  {
    gint delta_x, delta_y;
    switch(cam->live_view_rotation)
    {
      case 0:
        delta_x = lib->live_view_zoom_cursor_x - x;
        delta_y = lib->live_view_zoom_cursor_y - y;
        break;
      case 1:
        delta_x = y - lib->live_view_zoom_cursor_y;
        delta_y = lib->live_view_zoom_cursor_x - x;
        break;
      case 2:
        delta_x = x - lib->live_view_zoom_cursor_x;
        delta_y = y - lib->live_view_zoom_cursor_y;
        break;
      case 3:
        delta_x = lib->live_view_zoom_cursor_y - y;
        delta_y = x - lib->live_view_zoom_cursor_x;
        break;
      default: // can't happen
        delta_x = delta_y = 0;
    }
    cam->live_view_zoom_x = MAX(0, cam->live_view_zoom_x + delta_x);
    cam->live_view_zoom_y = MAX(0, cam->live_view_zoom_y + delta_y);
    lib->live_view_zoom_cursor_x = x;
    lib->live_view_zoom_cursor_y = y;
    gchar str[20];
    snprintf(str, sizeof(str), "%u,%u", cam->live_view_zoom_x, cam->live_view_zoom_y);
    dt_camctl_camera_set_property_string(darktable.camctl, NULL, "eoszoomposition", str);
  }
  dt_control_queue_redraw_center();
}

int button_pressed(dt_view_t *self, double x, double y, double pressure, int which, int type, uint32_t state)
{
  dt_camera_t *cam = (dt_camera_t *)darktable.camctl->active_camera;
  dt_capture_t *lib = (dt_capture_t *)self->data;

  if(which == 1 && cam->is_live_viewing && cam->live_view_zoom)
  {
    cam->live_view_pan = TRUE;
    lib->live_view_zoom_cursor_x = x;
    lib->live_view_zoom_cursor_y = y;
    dt_control_change_cursor(GDK_HAND1);
    return 1;
  }
  else if((which == 2 || which == 3) && cam->is_live_viewing) // zoom the live view
  {
    cam->live_view_zoom = !cam->live_view_zoom;
    if(cam->live_view_zoom == TRUE)
      dt_camctl_camera_set_property_string(darktable.camctl, NULL, "eoszoom", "5");
    else
      dt_camctl_camera_set_property_string(darktable.camctl, NULL, "eoszoom", "1");
    return 1;
  }
  return 0;
}

int button_released(dt_view_t *self, double x, double y, int which, uint32_t state)
{
  dt_camera_t *cam = (dt_camera_t *)darktable.camctl->active_camera;
  if(which == 1)
  {
    cam->live_view_pan = FALSE;
    dt_control_change_cursor(GDK_LEFT_PTR);
    return 1;
  }
  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
