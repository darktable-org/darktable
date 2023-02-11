/*
    This file is part of darktable,
    Copyright (C) 2014-2023 darktable developers.

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
#include "common/image_cache.h"
#include "common/import_session.h"
#include "common/iop_profile.h"
#include "common/selection.h"
#include "common/utility.h"
#include "common/variables.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "control/settings.h"
#include "develop/imageop_math.h"
#include "develop/noise_generator.h"
#include "dtgtk/thumbtable.h"
#include "gui/accelerators.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "imageio/imageio_common.h"
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
static int32_t _capture_view_get_selected_imgid(const dt_view_t *view);

const char *name(const dt_view_t *self)
{
  return _("Tethering");
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
  darktable.view_manager->proxy.tethering.view = self;
  darktable.view_manager->proxy.tethering.get_job_code = _capture_view_get_jobcode;
  darktable.view_manager->proxy.tethering.set_job_code = _capture_view_set_jobcode;
  darktable.view_manager->proxy.tethering.get_selected_imgid = _capture_view_get_selected_imgid;
}

void cleanup(dt_view_t *self)
{
  free(self->data);
}


static int32_t _capture_view_get_selected_imgid(const dt_view_t *view)
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
  dt_control_log(_("New session initiated '%s'"), name);
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

typedef struct _tethering_format_t
{
  dt_imageio_module_data_t head;
  float *buf;
} _tethering_format_t;

static const char *_tethering_mime(dt_imageio_module_data_t *data)
{
  return "memory";
}

static int _tethering_levels(dt_imageio_module_data_t *data)
{
  return IMAGEIO_RGB | IMAGEIO_FLOAT;
}

static int _tethering_bpp(dt_imageio_module_data_t *data)
{
  return 32;
}

static int _tethering_write_image(dt_imageio_module_data_t *data, const char *filename, const void *in,
                                  dt_colorspaces_color_profile_type_t over_type, const char *over_filename,
                                  void *exif, int exif_len, int imgid, int num, int total,
                                  dt_dev_pixelpipe_t *pipe, const gboolean export_masks)
{
  _tethering_format_t *d = (_tethering_format_t *)data;
  d->buf = (float *)malloc(sizeof(float) * 4 * d->head.width * d->head.height);
  memcpy(d->buf, in, sizeof(float) * 4 * d->head.width * d->head.height);
  return 0;
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
  if(l) lib->image_id = GPOINTER_TO_INT(l->data);

  lib->image_over = lib->image_id;

  if(cam->is_live_viewing == TRUE) // display the preview
  {
    dt_pthread_mutex_lock(&cam->live_view_buffer_mutex);
    if(cam->live_view_buffer)
    {
      const gint pw = cam->live_view_width;
      const gint ph = cam->live_view_height;
      const uint8_t *const p_buf = cam->live_view_buffer;

      // draw live view image
      uint8_t *const tmp_i = dt_alloc_align(64, sizeof(uint8_t) * pw * ph * 4);
      if(tmp_i)
      {
        const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, pw);
        pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);
        // FIXME: if liveview image is tagged and we can read its colorspace, use that
        cmsDoTransformLineStride(darktable.color_profiles->transform_srgb_to_display, p_buf, tmp_i, pw, ph, pw * 4,
                                 stride, 0, 0);
        pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);

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

          // ensure some sanity on the scale factor
          scale = fminf(10.0, scale);

          // FIXME: use cairo_pattern_set_filter()?
          cairo_translate(cr, width * 0.5, (height + BAR_HEIGHT) * 0.5); // origin to middle of canvas
          if(cam->live_view_flip == TRUE)
            cairo_scale(cr, -1.0, 1.0);    // mirror image
          if(cam->live_view_rotation)
            cairo_rotate(cr, -M_PI_2 * cam->live_view_rotation); // rotate around middle
          if(cam->live_view_zoom == FALSE)
            cairo_scale(cr, scale, scale);                  // scale to fit canvas
          cairo_translate(cr, -0.5 * pw, -0.5 * ph);        // origin back to corner
          cairo_scale(cr, darktable.gui->ppd, darktable.gui->ppd);
          cairo_set_source_surface(cr, source, 0.0, 0.0);
          cairo_paint(cr);
        }
        cairo_surface_destroy(source);
        dt_free_align(tmp_i);
      }

      // process live view histogram
      float *const tmp_f = dt_alloc_align_float((size_t)4 * pw * ph);
      if(tmp_f)
      {
        dt_develop_t *dev = darktable.develop;
        // FIXME: add OpenMP
        for(size_t p = 0; p < (size_t)4 * pw * ph; p += 4)
        {
          uint32_t DT_ALIGNED_ARRAY state[4]
              = { splitmix32(p + 1), splitmix32((p + 1) * (p + 3)), splitmix32(1337), splitmix32(666) };
          xoshiro128plus(state);
          xoshiro128plus(state);
          xoshiro128plus(state);
          xoshiro128plus(state);

          for(int k = 0; k < 3; k++)
            tmp_f[p + k] = dt_noise_generator(DT_NOISE_UNIFORM, p_buf[p + k], 0.5f, 0, state) / 255.0f;
        }

        // We need to do special cases for work/export colorspace
        // which dt_ioppr_get_histogram_profile_type() can't handle
        // when not in darkroom view.
        const dt_iop_order_iccprofile_info_t *profile_to = NULL;
        const dt_iop_order_iccprofile_info_t *const srgb_profile =
          dt_ioppr_add_profile_info_to_list(dev, DT_COLORSPACE_SRGB, "", DT_INTENT_RELATIVE_COLORIMETRIC);
        if(darktable.color_profiles->histogram_type == DT_COLORSPACE_WORK)
        {
          // The work profile of a SOC JPEG is nonsensical. So that
          // the histogram will have some relationship to a captured
          // image's profile, go with the standard work profile.
          // FIXME: can figure out the current default work colorspace via checking presets?
          profile_to = dt_ioppr_add_profile_info_to_list(dev, DT_COLORSPACE_LIN_REC2020, "", DT_INTENT_RELATIVE_COLORIMETRIC);
        }
        else if(darktable.color_profiles->histogram_type == DT_COLORSPACE_EXPORT)
        {
          // don't touch the image
          profile_to = srgb_profile;
        }
        else
        {
          profile_to = dt_ioppr_get_histogram_profile_info(dev);
          if(!profile_to)
            profile_to = srgb_profile;
        }

        // FIXME: if liveview image is tagged and we can read its colorspace, use that
        darktable.lib->proxy.histogram.process(darktable.lib->proxy.histogram.module, tmp_f, pw, ph,
                                               srgb_profile, profile_to);
        dt_control_queue_redraw_widget(darktable.lib->proxy.histogram.module->widget);
        dt_free_align(tmp_f);
      }
    }
    dt_pthread_mutex_unlock(&cam->live_view_buffer_mutex);
  }
  else if(lib->image_id >= 0) // First of all draw image if available
  {
    // FIXME: every time the mouse moves over the center view this redraws, which isn't necessary
    cairo_surface_t *surf = NULL;
    const dt_view_surface_value_t res = dt_view_image_get_surface(lib->image_id, width - (MARGIN * 2.0f),
                                                                  height - (MARGIN * 2.0f), &surf, FALSE);
    if(res != DT_VIEW_SURFACE_OK)
    {
      // if the image is missing, we reload it again
      g_timeout_add(250, _expose_again, NULL);
      if(!lib->busy) dt_control_log_busy_enter();
      lib->busy = TRUE;
    }
    else
    {
      const float scaler = 1.0f / darktable.gui->ppd_thb;
      cairo_translate(cr, (width - cairo_image_surface_get_width(surf) * scaler) / 2,
                      (height - cairo_image_surface_get_height(surf) * scaler) / 2);
      cairo_scale(cr, scaler, scaler);
      cairo_set_source_surface(cr, surf, 0.0, 0.0);
      cairo_paint(cr);
      cairo_surface_destroy(surf);
      if(lib->busy) dt_control_log_busy_leave();
      lib->busy = FALSE;
    }

    // update the histogram
    dt_imageio_module_format_t format;
    _tethering_format_t dat;
    format.bpp = _tethering_bpp;
    format.write_image = _tethering_write_image;
    format.levels = _tethering_levels;
    format.mime = _tethering_mime;
    // FIXME: is this reasonable resolution? does it match what pixelpipe preview pipe does?
    dat.head.max_width = darktable.mipmap_cache->max_width[DT_MIPMAP_F];
    dat.head.max_height = darktable.mipmap_cache->max_height[DT_MIPMAP_F];
    dat.head.style[0] = '\0';

    dt_colorspaces_color_profile_type_t histogram_type = DT_COLORSPACE_NONE;
    const char *histogram_filename = NULL;
    if(darktable.color_profiles->histogram_type == DT_COLORSPACE_WORK)
    {
      const dt_colorspaces_color_profile_t *work_profile =
        dt_colorspaces_get_work_profile(lib->image_id);
      histogram_type = work_profile->type;
      histogram_filename = work_profile->filename;
    }
    else if(darktable.color_profiles->histogram_type == DT_COLORSPACE_EXPORT)
    {
      const dt_colorspaces_color_profile_t *export_profile =
        dt_colorspaces_get_output_profile(lib->image_id, DT_COLORSPACE_NONE, NULL);
      histogram_type = export_profile->type;
      histogram_filename = export_profile->filename;
    }
    else
    {
      // special cases above as this can't handle work/export profile
      // when not in darkroom view
      dt_ioppr_get_histogram_profile_type(&histogram_type, &histogram_filename);
    }

    // this uses the export rather than thumbnail pipe -- slower, but
    // as we're not competing with the full pixelpipe, it's a
    // reasonable trade-off for a histogram which matches that in
    // darkroom view
    // FIXME: instead export image in work profile, then pass that to histogram process as well as converting to display profile for output, eliminating dt_view_image_get_surface() above
    if(!dt_imageio_export_with_flags(lib->image_id, "unused", &format, (dt_imageio_module_data_t *)&dat, TRUE,
                                     FALSE, FALSE, FALSE, FALSE, FALSE, NULL, FALSE, FALSE, histogram_type, histogram_filename,
                                     DT_INTENT_PERCEPTUAL, NULL, NULL, 1, 1, NULL, -1))
    {
      const dt_iop_order_iccprofile_info_t *const histogram_profile =
        dt_ioppr_add_profile_info_to_list(darktable.develop, histogram_type, histogram_filename,
                                          DT_INTENT_RELATIVE_COLORIMETRIC);
      darktable.lib->proxy.histogram.process(darktable.lib->proxy.histogram.module,
                                             dat.buf, dat.head.width, dat.head.height,
                                             histogram_profile, histogram_profile);
      dt_control_queue_redraw_widget(darktable.lib->proxy.histogram.module->widget);
      free(dat.buf);
    }
  }
  else // not in live view, no image selected
  {
    // if we just left live view, blank out its histogram
    darktable.lib->proxy.histogram.process(darktable.lib->proxy.histogram.module, NULL, 0, 0, NULL, NULL);
    dt_control_queue_redraw_widget(darktable.lib->proxy.histogram.module->widget);
  }
}


void expose(dt_view_t *self, cairo_t *cri, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  cairo_set_source_rgb(cri, .2, .2, .2);
  cairo_rectangle(cri, 0, 0, width, height);
  cairo_fill(cri);

  // Expose tethering center view
  cairo_save(cri);

  _expose_tethered_mode(self, cri, width, height, pointerx, pointery);

  cairo_restore(cri);

  // post expose to modules
  for(const GList *modules = darktable.lib->plugins; modules; modules = g_list_next(modules))
  {
    dt_lib_module_t *module = (dt_lib_module_t *)(modules->data);
    if(module->gui_post_expose && dt_lib_is_visible_in_view(module, self))
      module->gui_post_expose(module, cri, width, height, pointerx, pointery);
  }
}

int try_enter(dt_view_t *self)
{
  /* verify that camera supports tethering and is available */
  if(dt_camctl_can_enter_tether_mode(darktable.camctl, NULL)) return 0;

  dt_control_log(_("No camera with tethering support available for use..."));
  return 1;
}

static void _capture_mipmaps_updated_signal_callback(gpointer instance, int imgid, gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  struct dt_capture_t *lib = (dt_capture_t *)self->data;

  lib->image_id = imgid;
  dt_view_active_images_reset(FALSE);
  dt_view_active_images_add(lib->image_id, TRUE);
  dt_thumbtable_full_redraw(dt_ui_thumbtable(darktable.gui->ui), TRUE);

  dt_control_queue_redraw_center();
}


/** callbacks to deal with images taken in tethering mode */
static const char *_camera_request_image_filename(const dt_camera_t *camera, const char *filename,
                                                  const dt_image_basic_exif_t *basic_exif, void *data)
{
  struct dt_capture_t *lib = (dt_capture_t *)data;

  /* update import session with original filename so that $(FILE_EXTENSION)
   *     and alike can be expanded. */
  dt_import_session_set_filename(lib->session, filename);
  const gchar *file = dt_import_session_filename(lib->session, FALSE);

  if(file == NULL) return NULL;

  return g_strdup(file);
}

static const char *_camera_request_image_path(const dt_camera_t *camera, const dt_image_basic_exif_t *basic_exif, void *data)
{
  struct dt_capture_t *lib = (dt_capture_t *)data;
  return dt_import_session_path(lib->session, FALSE);
}

static void _camera_capture_image_downloaded(const dt_camera_t *camera, const char *in_folder,
                                             const char *in_filename, const char *filename, void *data)
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
  lib->image_id = l ? GPOINTER_TO_INT(l->data) : -1;

  dt_view_active_images_reset(FALSE);
  dt_view_active_images_add(lib->image_id, TRUE);
  dt_thumbtable_set_offset_image(dt_ui_thumbtable(darktable.gui->ui), lib->image_id, TRUE);

  /* initialize a session */
  lib->session = dt_import_session_new();

  const char *tmp = dt_conf_get_string_const("plugins/session/jobcode");
  if(tmp != NULL)
  {
    _capture_view_set_jobcode(self, tmp);
  }

  /* connect signal for mipmap update for a redraw */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_MIPMAP_UPDATED,
                                  G_CALLBACK(_capture_mipmaps_updated_signal_callback), (gpointer)self);


  /* connect signal for fimlstrip image activate */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE,
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
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_capture_mipmaps_updated_signal_callback),
                                     (gpointer)self);

  /* disconnect from filmstrip image activate */
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_view_capture_filmstrip_activate_callback),
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
    snprintf(str, sizeof(str), "%d,%d", cam->live_view_zoom_x, cam->live_view_zoom_y);
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
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
