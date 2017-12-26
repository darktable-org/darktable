/*
    This file is part of darktable,
    copyright (c) 2010 -- 2014 henrik andersson.

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
#include "common/darktable.h"
#include "common/image_cache.h"
#include "common/import_session.h"
#include "common/utility.h"
#include "common/variables.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "control/settings.h"
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

} dt_capture_t;

/* signal handler for filmstrip image switching */
static void _view_capture_filmstrip_activate_callback(gpointer instance, gpointer user_data);

static void _capture_view_set_jobcode(const dt_view_t *view, const char *name);
static const char *_capture_view_get_jobcode(const dt_view_t *view);
static uint32_t _capture_view_get_selected_imgid(const dt_view_t *view);

const char *name(dt_view_t *self)
{
  return _("tethering");
}

uint32_t view(const dt_view_t *self)
{
  return DT_VIEW_TETHERING;
}

static void _view_capture_filmstrip_activate_callback(gpointer instance, gpointer user_data)
{
  if(dt_view_filmstrip_get_activated_imgid(darktable.view_manager) >= 0) dt_control_queue_redraw_center();
}

static gboolean film_strip_key_accel(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                              GdkModifierType modifier, gpointer data)
{
  dt_lib_module_t *m = darktable.view_manager->proxy.filmstrip.module;
  gboolean vs = dt_lib_is_visible(m);
  dt_lib_set_visible(m, !vs);
  return TRUE;
}


void init(dt_view_t *self)
{
  self->data = calloc(1, sizeof(dt_capture_t));

  /* prefetch next few from first selected image on. */
  dt_view_filmstrip_prefetch();

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
static void _expose_tethered_mode(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx,
                           int32_t pointery)
{
  dt_capture_t *lib = (dt_capture_t *)self->data;
  dt_camera_t *cam = (dt_camera_t *)darktable.camctl->active_camera;
  lib->image_over = DT_VIEW_DESERT;
  lib->image_id = dt_view_filmstrip_get_activated_imgid(darktable.view_manager);

  if(cam->is_live_viewing == TRUE) // display the preview
  {
    dt_pthread_mutex_lock(&cam->live_view_pixbuf_mutex);
    if(GDK_IS_PIXBUF(cam->live_view_pixbuf))
    {
      gint pw = gdk_pixbuf_get_width(cam->live_view_pixbuf);
      gint ph = gdk_pixbuf_get_height(cam->live_view_pixbuf);

      float w = width - (MARGIN * 2.0f);
      float h = height - (MARGIN * 2.0f) - BAR_HEIGHT;

      float scale;
      if(cam->live_view_rotation % 2 == 0)
        scale = fminf(w / pw, h / ph);
      else
        scale = fminf(w / ph, h / pw);
      scale = fminf(1.0, scale);

      cairo_translate(cr, width * 0.5, (height + BAR_HEIGHT) * 0.5);  // origin to middle of canvas
      if(cam->live_view_flip == TRUE) cairo_scale(cr, -1.0, 1.0);     // mirror image
      cairo_rotate(cr, -M_PI_2 * cam->live_view_rotation);            // rotate around middle
      if(cam->live_view_zoom == FALSE) cairo_scale(cr, scale, scale); // scale to fit canvas
      cairo_translate(cr, -0.5 * pw, -0.5 * ph);                      // origin back to corner

      gdk_cairo_set_source_pixbuf(cr, cam->live_view_pixbuf, 0, 0);
      cairo_paint(cr);
    }
    dt_pthread_mutex_unlock(&cam->live_view_pixbuf_mutex);
  }
  else if(lib->image_id >= 0) // First of all draw image if available
  {
    cairo_translate(cr, MARGIN, MARGIN);
    dt_view_image_expose(&(lib->image_over), lib->image_id, cr, width - (MARGIN * 2.0f),
                         height - (MARGIN * 2.0f), 1, pointerx, pointery, FALSE, FALSE);
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

static void _capture_mipmaps_updated_signal_callback(gpointer instance, gpointer user_data)
{
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

  /* connect signal for mipmap update for a redraw */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_MIPMAP_UPDATED,
                            G_CALLBACK(_capture_mipmaps_updated_signal_callback), (gpointer)self);


  /* connect signal for fimlstrip image activate */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_VIEWMANAGER_FILMSTRIP_ACTIVATE,
                            G_CALLBACK(_view_capture_filmstrip_activate_callback), self);

  dt_view_filmstrip_scroll_to_image(darktable.view_manager, lib->image_id, TRUE);


  /* initialize a session */
  lib->session = dt_import_session_new();

  char *tmp = dt_conf_get_string("plugins/capture/jobcode");
  if(tmp != NULL)
  {
    _capture_view_set_jobcode(self, tmp);
    g_free(tmp);
  }

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

void init_key_accels(dt_view_t *self)
{
  // Setup key accelerators in capture view...
  dt_accel_register_view(self, NC_("accel", "toggle film strip"), GDK_KEY_f, GDK_CONTROL_MASK);
}

void connect_key_accels(dt_view_t *self)
{
  GClosure *closure = g_cclosure_new(G_CALLBACK(film_strip_key_accel), (gpointer)self, NULL);
  dt_accel_connect_view(self, "toggle film strip", closure);
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
