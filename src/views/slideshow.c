/*
    This file is part of darktable,
    copyright (c) 2013 johannes hanika.
    copyright (c) 2019 pascal obry.

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

#include "common/collection.h"
#include "common/colorspaces.h"
#include "common/debug.h"
#include "common/dtpthread.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
#include "control/conf.h"
#include "control/control.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "views/view.h"
#include "views/view_api.h"

#include <gdk/gdkkeysyms.h>
#include <stdint.h>

DT_MODULE(1)

typedef enum dt_slideshow_event_t
{
  S_REQUEST_STEP,
  S_REQUEST_STEP_BACK,
  S_IMAGE_LOADED,
  S_BLENDED
} dt_slideshow_event_t;

typedef enum dt_slideshow_state_t
{
  S_PREFETCHING,
  S_WAITING_FOR_USER,
  S_BLENDING
} dt_slideshow_state_t;

typedef struct _slideshow_buf_t
{
  uint32_t *buf;
  uint32_t width;
  uint32_t height;
  int32_t num;
} dt_slideshow_buf_t;

typedef struct dt_slideshow_t
{
  uint32_t random_state;
  uint32_t scramble;
  uint32_t use_random;
  int32_t step;
  uint32_t width, height;

  // double buffer
  dt_slideshow_buf_t front, back;

  // state machine stuff for image transitions:
  dt_pthread_mutex_t lock;
  dt_slideshow_state_t state;      // global state cycle
  uint32_t state_waiting_for_user; // user input (needed to step the cycle at one point)

  uint32_t auto_advance;
  int delay;

  // some magic to hide the mosue pointer
  guint mouse_timeout;
} dt_slideshow_t;

typedef struct dt_slideshow_format_t
{
  int max_width, max_height;
  int width, height;
  char style[128];
  gboolean style_append;
  dt_slideshow_t *d;
} dt_slideshow_format_t;

// fwd declare state machine mechanics:
static void _step_state(dt_slideshow_t *d, dt_slideshow_event_t event);

// callbacks for in-memory export
static int bpp(dt_imageio_module_data_t *data)
{
  return 8;
}

static int levels(dt_imageio_module_data_t *data)
{
  return IMAGEIO_RGB | IMAGEIO_INT8;
}

static const char *mime(dt_imageio_module_data_t *data)
{
  return "memory";
}

static int write_image(dt_imageio_module_data_t *datai, const char *filename, const void *in,
                       dt_colorspaces_color_profile_type_t over_type, const char *over_filename,
                       void *exif, int exif_len, int imgid, int num, int total, dt_dev_pixelpipe_t *pipe)
{
  dt_slideshow_format_t *data = (dt_slideshow_format_t *)datai;
  dt_pthread_mutex_lock(&data->d->lock);
  if(data->d->back.buf)
  {
    // might have been cleaned up when leaving slide show
    memcpy(data->d->back.buf, in, sizeof(uint32_t) * datai->width * datai->height);
    data->d->back.width = datai->width;
    data->d->back.height = datai->height;
  }
  dt_pthread_mutex_unlock(&data->d->lock);
  _step_state(data->d, S_IMAGE_LOADED);
  // trigger expose
  dt_control_queue_redraw_center();
  return 0;
}

static uint32_t next_random(dt_slideshow_t *d)
{
  uint32_t i = d->random_state++;
  // van der corput for 32 bits. this guarantees every number will appear exactly once
  i = ((i & 0x0000ffff) << 16) | (i >> 16);
  i = ((i & 0x00ff00ff) << 8) | ((i & 0xff00ff00) >> 8);
  i = ((i & 0x0f0f0f0f) << 4) | ((i & 0xf0f0f0f0) >> 4);
  i = ((i & 0x33333333) << 2) | ((i & 0xcccccccc) >> 2);
  i = ((i & 0x55555555) << 1) | ((i & 0xaaaaaaaa) >> 1);
  return i ^ d->scramble;
}

// process image
static int process_next_image(dt_slideshow_t *d)
{
  dt_imageio_module_format_t buf;
  buf.mime = mime;
  buf.levels = levels;
  buf.bpp = bpp;
  buf.write_image = write_image;

  dt_slideshow_format_t dat;
  dat.width = dat.max_width = d->width;
  dat.height = dat.max_height = d->height;
  dat.style[0] = '\0';
  dat.d = d;

  // get random image id from sql
  int32_t id = 0;
  const int32_t cnt = dt_collection_get_count(darktable.collection);
  if(!cnt) return 1;

  dt_pthread_mutex_lock(&d->lock);
  d->back.num = d->front.num + d->step;
  int32_t ran = d->back.num;
  dt_pthread_mutex_unlock(&d->lock);
  // enumerated all images? i.e. prefetching the one two after the limit, when viewing the one past the end.
  if(ran == -2 || ran == cnt + 1)
  {
    dt_control_log(_("end of images. press any key to return to lighttable mode"));
  }
  if(d->use_random)
  {
    // get random number up to next power of two greater than cnt:
    const uint32_t zeros = __builtin_clz(cnt);
    // pull radical inverses only in our desired range:
    do
      ran = next_random(d) >> zeros;
    while(ran >= cnt);
  }
  int32_t rand = ran % cnt;
  while(rand < 0) rand += cnt;
  const gchar *query = dt_collection_get_query(darktable.collection);
  if(!query) return 1;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, rand);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, rand + 1);
  if(sqlite3_step(stmt) == SQLITE_ROW) id = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  // this is a little slow, might be worth to do an option:
  const gboolean high_quality = dt_conf_get_bool("plugins/slideshow/high_quality");
  if(id)
    // the flags are: ignore exif, display byteorder, high quality, upscale, thumbnail
    dt_imageio_export_with_flags(id, "unused", &buf, (dt_imageio_module_data_t *)&dat, TRUE, TRUE,
                                 high_quality, TRUE, FALSE, NULL, FALSE, DT_COLORSPACE_DISPLAY,
                                 NULL, DT_INTENT_LAST, NULL, NULL, 1, 1);
  return 0;
}

static int32_t process_job_run(dt_job_t *job)
{
  dt_slideshow_t *d = dt_control_job_get_params(job);
  process_next_image(d);
  return 0;
}

static dt_job_t *process_job_create(dt_slideshow_t *d)
{
  dt_job_t *job = dt_control_job_create(&process_job_run, "process slideshow image");
  if(!job) return NULL;
  dt_control_job_set_params(job, d, NULL);
  return job;
}

static gboolean auto_advance(gpointer user_data)
{
  dt_slideshow_t *d = (dt_slideshow_t *)user_data;
  if(!d->auto_advance) return FALSE;
  _step_state(d, S_REQUEST_STEP);
  return FALSE;
}

static void exchange_buffer(dt_slideshow_t *d)
{
  uint32_t *tmp = d->front.buf;
  d->front.buf = d->back.buf;
  d->back.buf = tmp;

  const int32_t num = d->front.num;
  d->front.num = d->back.num;
  d->back.num = num;

  const int32_t w = d->front.width;
  d->front.width = d->back.width;
  d->back.width = w;

  const int32_t h = d->front.height;
  d->front.height = d->back.height;
  d->back.height = h;
}

// state machine stepping
static void _step_state(dt_slideshow_t *d, dt_slideshow_event_t event)
{
  dt_pthread_mutex_lock(&d->lock);

  if(event == S_REQUEST_STEP || event == S_REQUEST_STEP_BACK)
  {
    if(event == S_REQUEST_STEP) d->step = 1;
    if(event == S_REQUEST_STEP_BACK) d->step = -1;
    // make sure we only enter busy if really flipping the bit
    if(d->state_waiting_for_user) dt_control_log_busy_enter();
    d->state_waiting_for_user = 0;
  }

  switch(d->state)
  {
    case S_PREFETCHING:
      if(event == S_IMAGE_LOADED)
      {
        d->state = S_WAITING_FOR_USER;
        // and go to next case
      }
      else
        break;

    case S_WAITING_FOR_USER:
      if(d->state_waiting_for_user == 0)
      {
        d->state = S_BLENDING;

        // swap buffers, start blending cycle
        if(d->front.num + d->step == d->back.num)
        {
          // going back
          exchange_buffer(d);

          // start over
          dt_control_log_busy_leave();
          d->state_waiting_for_user = 1;
        }

        // start new one-off timer from when flipping buffers.
        // this will show images before processing-heavy shots a little
        // longer, but at least not result in shorter viewing times just after these
        if(d->auto_advance) g_timeout_add_seconds(d->delay, auto_advance, d);
        // and execute the next case, too
      }
      else
        break;

    case S_BLENDING:
      // draw new front buf
      dt_control_queue_redraw_center();

      // start bgjob
      dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_BG, process_job_create(d));
      d->state = S_PREFETCHING;
      break;

    default:
      // uh. should never happen. sanitize:
      d->state_waiting_for_user = 1;
      d->state = S_PREFETCHING;
      break;
  }
  dt_pthread_mutex_unlock(&d->lock);
}

// callbacks for a view module:

const char *name(dt_view_t *self)
{
  return _("slideshow");
}

uint32_t view(const dt_view_t *self)
{
  return DT_VIEW_SLIDESHOW;
}

void init(dt_view_t *self)
{
  self->data = calloc(1, sizeof(dt_slideshow_t));
  dt_slideshow_t *lib = (dt_slideshow_t *)self->data;
  dt_pthread_mutex_init(&lib->lock, 0);
}


void cleanup(dt_view_t *self)
{
  dt_slideshow_t *lib = (dt_slideshow_t *)self->data;
  dt_pthread_mutex_destroy(&lib->lock);
  free(self->data);
}

int try_enter(dt_view_t *self)
{
  /* verify that there are images to display */
  if(dt_collection_get_count(darktable.collection) != 0)
  {
    return 0;
  }
  else
  {
    dt_control_log(_("there are no images in this collection"));
    return 1;
  }
}

void enter(dt_view_t *self)
{
  dt_slideshow_t *d = (dt_slideshow_t *)self->data;

  dt_control_change_cursor(GDK_BLANK_CURSOR);
  d->mouse_timeout = 0;

  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_LEFT, FALSE, TRUE);
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_RIGHT, FALSE, TRUE);
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_TOP, FALSE, TRUE);
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_BOTTOM, FALSE, TRUE);
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_CENTER_TOP, FALSE, TRUE);
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_CENTER_BOTTOM, FALSE, TRUE);

  // also hide arrows
  dt_ui_border_show(darktable.gui->ui, FALSE);
  dt_control_queue_redraw();

  // alloc screen-size double buffer
  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
  GdkRectangle rect;

#if GTK_CHECK_VERSION(3, 22, 0)
  GdkDisplay *display = gtk_widget_get_display(window);
  GdkMonitor *mon = gdk_display_get_monitor_at_window(display, gtk_widget_get_window(window));
  gdk_monitor_get_geometry(mon, &rect);
#else
  GdkScreen *screen = gtk_widget_get_screen(window);
  if(!screen) screen = gdk_screen_get_default();
  int monitor = gdk_screen_get_monitor_at_window(screen, gtk_widget_get_window(window));
  gdk_screen_get_monitor_geometry(screen, monitor, &rect);
#endif

  dt_pthread_mutex_lock(&d->lock);

  d->width = rect.width * darktable.gui->ppd;
  d->height = rect.height * darktable.gui->ppd;

  d->front.buf = dt_alloc_align(64, sizeof(uint32_t) * d->width * d->height);
  d->back.buf = dt_alloc_align(64, sizeof(uint32_t) * d->width * d->height);
  d->front.width = d->back.width = d->width;
  d->front.height = d->back.height = d->height;

  d->back.num = dt_view_lighttable_get_position(darktable.view_manager) - 1;
  d->front.num = -1;

  // start in prefetching phase, do that by initing one state before
  // and stepping through that at the very end of this function
  d->state = S_PREFETCHING;
  d->state_waiting_for_user = 0;

  d->auto_advance = 0;
  d->delay = dt_conf_get_int("slideshow_delay");
  // restart from beginning, will first increment counter by step and then prefetch
  d->step = 1;
  dt_pthread_mutex_unlock(&d->lock);

  gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));

  // start first job
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_BG, process_job_create(d));
  dt_control_log(_("waiting to start slideshow"));
}

void leave(dt_view_t *self)
{
  dt_slideshow_t *d = (dt_slideshow_t *)self->data;

  if(d->mouse_timeout > 0) g_source_remove(d->mouse_timeout);
  d->mouse_timeout = 0;
  dt_control_change_cursor(GDK_LEFT_PTR);
  dt_ui_border_show(darktable.gui->ui, TRUE);
  d->auto_advance = 0;
  dt_view_lighttable_set_position(darktable.view_manager, d->front.num);
  dt_pthread_mutex_lock(&d->lock);
  dt_free_align(d->front.buf);
  dt_free_align(d->back.buf);
  d->front.buf = d->back.buf = NULL;
  dt_pthread_mutex_unlock(&d->lock);
}

void expose(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  // draw front buffer.
  dt_slideshow_t *d = (dt_slideshow_t *)self->data;

  dt_pthread_mutex_lock(&d->lock);
  cairo_paint(cr);
  if(d->front.buf && d->front.num >= 0)
  {
    // cope with possible resize of the window
    const float tr_width = d->width < d->front.width ? 0.f : (d->width - d->front.width) * .5f / darktable.gui->ppd;
    const float tr_height = d->height < d->front.height ? 0.f : (d->height - d->front.height) * .5f / darktable.gui->ppd;

    cairo_save(cr);
    cairo_translate(cr, tr_width, tr_height);
    cairo_surface_t *surface = NULL;
    const int32_t stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, d->front.width);
    surface = dt_cairo_image_surface_create_for_data((uint8_t *)d->front.buf, CAIRO_FORMAT_RGB24, d->front.width,
                                                     d->front.height, stride);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_rectangle(cr, 0, 0, d->front.width/darktable.gui->ppd, d->front.height/darktable.gui->ppd);
    cairo_fill(cr);
    cairo_surface_destroy(surface);
    cairo_restore(cr);
  }

  // adjust image size to window size
  d->width = width * darktable.gui->ppd;
  d->height = height * darktable.gui->ppd;
  dt_pthread_mutex_unlock(&d->lock);
}

static gboolean _hide_mouse(gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_slideshow_t *d = (dt_slideshow_t *)self->data;
  d->mouse_timeout = 0;
  dt_control_change_cursor(GDK_BLANK_CURSOR);
  return FALSE;
}


void mouse_moved(dt_view_t *self, double x, double y, double pressure, int which)
{
  dt_slideshow_t *d = (dt_slideshow_t *)self->data;

  if(d->mouse_timeout > 0) g_source_remove(d->mouse_timeout);
  else dt_control_change_cursor(GDK_LEFT_PTR);
  d->mouse_timeout = g_timeout_add_seconds(1, _hide_mouse, self);
}


int button_released(dt_view_t *self, double x, double y, int which, uint32_t state)
{
  return 0;
}


int button_pressed(dt_view_t *self, double x, double y, double pressure, int which, int type, uint32_t state)
{
  dt_slideshow_t *d = (dt_slideshow_t *)self->data;

  if(which == 1)
    _step_state(d, S_REQUEST_STEP);
  else if(which == 3)
    _step_state(d, S_REQUEST_STEP_BACK);
  else
    return 1;

  return 0;
}

int key_released(dt_view_t *self, guint key, guint state)
{
  return 0;
}

int key_pressed(dt_view_t *self, guint key, guint state)
{
  dt_slideshow_t *d = (dt_slideshow_t *)self->data;
  dt_control_accels_t *accels = &darktable.control->accels;

  if(key == accels->slideshow_start.accel_key && state == accels->slideshow_start.accel_mods)
  {
    if(!d->auto_advance)
    {
      d->auto_advance = 1;
      _step_state(d, S_REQUEST_STEP);
    }
    else
    {
      d->auto_advance = 0;
      dt_control_log(_("slideshow paused"));
    }
    return 0;
  }
  else if(key == GDK_KEY_Up || key == GDK_KEY_KP_Add)
  {
    d->delay = CLAMP(d->delay + 1, 1, 60);
    dt_control_log(ngettext("slideshow delay set to %d second", "slideshow delay set to %d seconds", d->delay), d->delay);
    dt_conf_set_int("slideshow_delay", d->delay);
    return 0;
  }
  else if(key == GDK_KEY_Down || key == GDK_KEY_KP_Subtract)
  {
    d->delay = CLAMP(d->delay - 1, 1, 60);
    dt_control_log(ngettext("slideshow delay set to %d second", "slideshow delay set to %d seconds", d->delay), d->delay);
    dt_conf_set_int("slideshow_delay", d->delay);
    return 0;
  }
  else if(key == GDK_KEY_Left || key == GDK_KEY_Shift_L)
  {
    _step_state(d, S_REQUEST_STEP_BACK);
    return 0;
  }
  else if(key == GDK_KEY_Right || key == GDK_KEY_Shift_R)
  {
    _step_state(d, S_REQUEST_STEP);
    return 0;
  }

  // go back to lt mode
  dt_ctl_switch_mode_to("lighttable");
  return 0;
}

void init_key_accels(dt_view_t *self)
{
  dt_accel_register_view(self, NC_("accel", "start and stop"), GDK_KEY_space, 0);
}

void connect_key_accels(dt_view_t *self)
{
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
