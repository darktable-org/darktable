/*
    This file is part of darktable,
    Copyright (C) 2013-2022 darktable developers.

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
#include "dtgtk/thumbtable.h"
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
} dt_slideshow_event_t;

typedef enum dt_slideshow_slot_t
{
  S_LEFT      = 0,
  S_LEFT_M    = 1,
  S_CURRENT   = 2,
  S_RIGHT_M   = 3,
  S_RIGHT     = 4,
  S_SLOT_LAST = 5
} dt_slideshow_slot_t;

typedef struct _slideshow_buf_t
{
  uint8_t *buf;
  size_t width;
  size_t height;
  int rank;
  int imgid;
  gboolean invalidated;
} dt_slideshow_buf_t;

typedef struct dt_slideshow_t
{
  int32_t col_count;
  size_t width, height;

  // buffers
  dt_slideshow_buf_t buf[S_SLOT_LAST];
  int id_preview_displayed;
  int id_displayed;

  // state machine stuff for image transitions:
  dt_pthread_mutex_t lock;

  gboolean auto_advance;
  int exporting;
  int delay;

  // some magic to hide the mouse pointer
  guint mouse_timeout;
} dt_slideshow_t;

typedef struct dt_slideshow_format_t
{
  dt_imageio_module_data_t head;
  dt_slideshow_buf_t buf;
  int32_t rank;
} dt_slideshow_format_t;

// fwd declare state machine mechanics:
static void _step_state(dt_slideshow_t *d, dt_slideshow_event_t event);
static dt_job_t *_process_job_create(dt_slideshow_t *d);

static int _get_image_at_rank(const int rank)
{
  // get random image id from sql
  int id = -1;

  if(rank >= 0)
  {
    const gchar *query = dt_collection_get_query(darktable.collection);

    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, rank);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, 1);
    if(sqlite3_step(stmt) == SQLITE_ROW)
      id = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }

  return id;
}

static dt_slideshow_slot_t _get_slot_for_image(const dt_slideshow_t *d, const int imgid)
{
  dt_slideshow_slot_t slt = -1;

  for(dt_slideshow_slot_t slot=S_LEFT; slot<S_SLOT_LAST; slot++)
  {
    if(d->buf[slot].imgid == imgid)
    {
      slt = slot;
      break;
    }
  }
  return slt;
}

static void _init_slot(dt_slideshow_buf_t *s)
{
  s->buf = NULL;
  s->width = 0;
  s->height = 0;
  s->rank = -1;
  s->imgid = -1;
  s->invalidated = TRUE;
}

static void _shift_left(dt_slideshow_t *d)
{
  uint8_t *tmp_buf = d->buf[S_LEFT].buf;

  for(dt_slideshow_slot_t slot=S_LEFT; slot<S_RIGHT; slot++)
  {
    memcpy(&d->buf[slot], &d->buf[slot+1], sizeof(dt_slideshow_buf_t));
  }

  _init_slot(&d->buf[S_RIGHT]);
  d->buf[S_RIGHT].rank  = d->buf[S_CURRENT].rank + 2;
  d->buf[S_RIGHT].imgid = d->buf[S_RIGHT].rank <= d->col_count
    ? _get_image_at_rank(d->buf[S_RIGHT].rank)
    : -1;
  d->id_displayed = -1;
  d->id_preview_displayed = -1;
  dt_free_align(tmp_buf);
}

static void _shift_right(dt_slideshow_t *d)
{
  uint8_t *tmp_buf = d->buf[S_RIGHT].buf;

  for(dt_slideshow_slot_t slot=S_RIGHT; slot>S_LEFT; slot--)
  {
    memcpy(&d->buf[slot], &d->buf[slot-1], sizeof(dt_slideshow_buf_t));
  }

  _init_slot(&d->buf[S_LEFT]);
  d->buf[S_LEFT].rank = d->buf[S_CURRENT].rank - 2;
  d->buf[S_LEFT].imgid = d->buf[S_LEFT].rank >= 0
    ? _get_image_at_rank(d->buf[S_LEFT].rank)
    : -1;
  d->id_displayed = -1;
  d->id_preview_displayed = -1;
  dt_free_align(tmp_buf);
}

static void _requeue_job(dt_slideshow_t *d)
{
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_BG, _process_job_create(d));
}

static void _set_delay(dt_slideshow_t *d, int value)
{
  d->delay = CLAMP(d->delay + value, 1, 60);
  dt_conf_set_int("slideshow_delay", d->delay);
}

static int _process_image(dt_slideshow_t *d, dt_slideshow_slot_t slot)
{
  dt_pthread_mutex_lock(&d->lock);
  d->exporting++;
  const size_t s_width = d->width;
  const size_t s_height = d->height;
  const int imgid = d->buf[slot].imgid;
  size_t width = d->buf[slot].width;
  size_t height = d->buf[slot].height;
  uint8_t *buf;

  dt_pthread_mutex_unlock(&d->lock);

  dt_dev_image_ext
    (imgid, d->width, d->height, -1, &buf, &width, &height, 0, FALSE, -1);

  dt_pthread_mutex_lock(&d->lock);

  // original slot for this image

  dt_slideshow_slot_t slt = slot;

  // check if we have not moved the slideshow forward or backward, if the
  // slot is not the same, check for a possible new slot for this image.

  if(d->buf[slt].imgid != imgid)
    slt = _get_slot_for_image(d, imgid);

  // also ensure that the screen has not been resized, otherwise we discard
  // the image. it will get regenerated in another later job.

  if(slt != -1
     && d->width == s_width
     && d->height == s_height)
  {
    d->buf[slt].width = width;
    d->buf[slt].height = height;
    d->buf[slt].buf = buf;
    d->buf[slt].invalidated = FALSE;
  }
  else
  {
    // otherwise, just free the buffer which is now not needed
    dt_free_align(buf);
  }

  d->exporting--;
  dt_pthread_mutex_unlock(&d->lock);

  return 0;
}

static gboolean _is_slot_waiting(dt_slideshow_t *d, dt_slideshow_slot_t slot)
{
  return d->buf[slot].invalidated
         && d->buf[slot].buf == NULL
         && d->buf[slot].imgid >= 0
         && d->buf[slot].rank >= 0;
}

static gboolean _is_idle(dt_slideshow_t *d)
{
  gboolean idle = TRUE;
  for(dt_slideshow_slot_t slot=S_LEFT; slot<S_SLOT_LAST; slot++)
    idle &= !_is_slot_waiting(d, slot);
  return idle;
}

static gboolean _auto_advance(gpointer user_data)
{
  dt_slideshow_t *d = (dt_slideshow_t *)user_data;
  if(!d->auto_advance) return FALSE;
  if(!_is_idle(d)) return TRUE; // never try to advance if still exporting, but call me back again
  _step_state(d, S_REQUEST_STEP);
  return FALSE;
}

static int32_t _process_job_run(dt_job_t *job)
{
  dt_slideshow_t *d = dt_control_job_get_params(job);

  dt_pthread_mutex_lock(&d->lock);

  if(_is_slot_waiting(d,S_CURRENT))
  {
    dt_pthread_mutex_unlock(&d->lock);
    _process_image(d, S_CURRENT);
    dt_control_queue_redraw_center();
  }
  else if(_is_slot_waiting(d, S_RIGHT_M))
  {
    dt_pthread_mutex_unlock(&d->lock);
    _process_image(d, S_RIGHT_M);
  }
  else if(_is_slot_waiting(d, S_RIGHT))
  {
    dt_pthread_mutex_unlock(&d->lock);
    _process_image(d, S_RIGHT);
  }
  else if(_is_slot_waiting(d, S_LEFT_M))
  {
    dt_pthread_mutex_unlock(&d->lock);
    _process_image(d, S_LEFT_M);
  }
  else if(_is_slot_waiting(d, S_LEFT))
  {
    dt_pthread_mutex_unlock(&d->lock);
    _process_image(d, S_LEFT);
  }
  else
  {
    dt_pthread_mutex_unlock(&d->lock);
  }

  // any other slot to fill?
  if(!_is_idle(d))
    _requeue_job(d);

  return 0;
}

static dt_job_t *_process_job_create(dt_slideshow_t *d)
{
  dt_job_t *job = dt_control_job_create(&_process_job_run, "process slideshow image");
  if(!job) return NULL;
  dt_control_job_set_params(job, d, NULL);
  return job;
}

// state machine stepping
static void _step_state(dt_slideshow_t *d, dt_slideshow_event_t event)
{
  dt_pthread_mutex_lock(&d->lock);

  gboolean refresh_display = FALSE;

  if(event == S_REQUEST_STEP)
  {
    if(d->buf[S_CURRENT].rank < d->col_count - 1)
    {
      _shift_left(d);
      d->buf[S_RIGHT].rank = d->buf[S_CURRENT].rank + 2;
      d->buf[S_RIGHT].imgid = d->buf[S_RIGHT].rank < d->col_count
        ? _get_image_at_rank(d->buf[S_RIGHT].rank)
        : -1;
      d->buf[S_RIGHT].invalidated = TRUE;
      dt_free_align(d->buf[S_RIGHT].buf);
      d->buf[S_RIGHT].buf = NULL;
      refresh_display = TRUE;
      _requeue_job(d);
    }
    else
    {
      dt_control_log(_("end of images"));
      d->auto_advance = FALSE;
    }
  }
  else if(event == S_REQUEST_STEP_BACK)
  {
    if(d->buf[S_CURRENT].rank > 0)
    {
      _shift_right(d);
      d->buf[S_LEFT].rank = d->buf[S_CURRENT].rank - 2;
      d->buf[S_LEFT].imgid = d->buf[S_LEFT].rank >= 0
        ? _get_image_at_rank(d->buf[S_LEFT].rank)
        : -1;
      d->buf[S_LEFT].invalidated = TRUE;
      dt_free_align(d->buf[S_LEFT].buf);
      d->buf[S_LEFT].buf = NULL;
      refresh_display = TRUE;
      _requeue_job(d);
    }
    else
    {
      dt_control_log(_("end of images. press any key to return to lighttable mode"));
      d->auto_advance = FALSE;
    }
  }

  dt_pthread_mutex_unlock(&d->lock);

  if(refresh_display) dt_control_queue_redraw_center();
  if(d->auto_advance) g_timeout_add_seconds(d->delay, _auto_advance, d);
}

// callbacks for a view module:

const char *name(const dt_view_t *self)
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
  d->exporting = 0;
  d->id_displayed = -1;
  d->id_preview_displayed = -1;

  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_LEFT, FALSE, TRUE);
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_RIGHT, FALSE, TRUE);
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_TOP, FALSE, TRUE);
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_BOTTOM, FALSE, TRUE);
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_CENTER_TOP, FALSE, TRUE);
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_CENTER_BOTTOM, FALSE, TRUE);

  // alloc screen-size double buffer
  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
  GdkRectangle rect;

  GdkDisplay *display = gtk_widget_get_display(window);
  GdkMonitor *mon = gdk_display_get_monitor_at_window(display, gtk_widget_get_window(window));
  gdk_monitor_get_geometry(mon, &rect);

  dt_pthread_mutex_lock(&d->lock);

  d->width = rect.width * darktable.gui->ppd;
  d->height = rect.height * darktable.gui->ppd;

  for(dt_slideshow_slot_t slot=S_LEFT; slot<S_SLOT_LAST; slot++)
  {
    _init_slot(&d->buf[slot]);
  }

  // if one selected start with it, otherwise start at the current lighttable offset
  const int imgid = dt_act_on_get_main_image();
  gint selrank = -1;

  if(imgid > 0)
  {
    sqlite3_stmt *stmt;
    gchar *query = g_strdup_printf("SELECT rowid FROM memory.collected_images WHERE imgid=%d", imgid);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      selrank = sqlite3_column_int(stmt, 0) - 1;
    }
    g_free(query);
    sqlite3_finalize(stmt);
  }

  const int rank =
    selrank == -1
    ? dt_thumbtable_get_offset(dt_ui_thumbtable(darktable.gui->ui))
    : selrank;

  d->buf[S_LEFT].rank    = rank - 2;
  d->buf[S_LEFT_M].rank  = rank - 1;
  d->buf[S_CURRENT].rank = rank;
  d->buf[S_RIGHT_M].rank = rank + 1;
  d->buf[S_RIGHT].rank   = rank + 2;

  for(dt_slideshow_slot_t slot=S_LEFT; slot<S_SLOT_LAST; slot++)
    d->buf[slot].imgid = _get_image_at_rank(d->buf[slot].rank);

  d->col_count = dt_collection_get_count(darktable.collection);

  d->auto_advance = FALSE;
  d->delay = dt_conf_get_int("slideshow_delay");
  // restart from beginning, will first increment counter by step and then prefetch
  dt_pthread_mutex_unlock(&d->lock);

  gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));

  // start first job
  dt_control_queue_redraw_center();
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_BG, _process_job_create(d));
  dt_control_log(_("waiting to start slideshow"));
}

void leave(dt_view_t *self)
{
  dt_slideshow_t *d = (dt_slideshow_t *)self->data;

  if(d->mouse_timeout > 0) g_source_remove(d->mouse_timeout);
  d->mouse_timeout = 0;
  dt_control_change_cursor(GDK_LEFT_PTR);
  d->auto_advance = FALSE;

  // exporting could be in action, just wait for the last to finish
  // otherwise we will crash releasing lock and memory.
  while(d->exporting > 0) sleep(1);

  dt_thumbtable_set_offset(dt_ui_thumbtable(darktable.gui->ui), d->buf[S_CURRENT].rank, FALSE);

  dt_pthread_mutex_lock(&d->lock);

  for(dt_slideshow_slot_t slot=S_LEFT; slot<S_SLOT_LAST; slot++)
  {
    dt_free_align(d->buf[slot].buf);
    d->buf[slot].buf = NULL;
  }
  dt_pthread_mutex_unlock(&d->lock);
}

void expose(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  // draw front buffer.
  dt_slideshow_t *d = (dt_slideshow_t *)self->data;

  dt_pthread_mutex_lock(&d->lock);
  dt_slideshow_buf_t *slot = &(d->buf[S_CURRENT]);
  const int32_t imgid = slot->imgid;

  if(d->width < slot->width
     || d->height < slot->height)
  {
    slot->invalidated = TRUE;
    _requeue_job(d);
  }

  // redraw even if the current displayed image is imgid as we want the
  // "working..." label to be cleared.

  if(slot->buf && imgid >= 0 && !slot->invalidated)
  {
    cairo_paint(cr);

    cairo_save(cr);

    dt_view_paint_buffer
      (cr, width, height,
       slot->buf, slot->width, slot->height, DT_WINDOW_SLIDESHOW);

    d->id_displayed = imgid;
    d->id_preview_displayed = imgid;
    cairo_restore(cr);
  }
  else if(imgid >= 0 && imgid != d->id_preview_displayed)
  {
    // get a small preview
    dt_mipmap_buffer_t buf;
    dt_mipmap_size_t mip =
      dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, width / 8, height /8);
    dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, mip, DT_MIPMAP_BLOCKING, 'r');
    if(buf.buf)
    {
      cairo_paint(cr);
      dt_view_paint_pixbuf(cr, width, height,
                           (uint8_t *)buf.buf, buf.width, buf.height, DT_WINDOW_SLIDESHOW);
    }

    d->id_preview_displayed = imgid;
    dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
  }

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

static void _start_stop_callback(dt_action_t *action)
{
  dt_slideshow_t *d = dt_action_view(action)->data;
  if(!d->auto_advance)
  {
    d->auto_advance = TRUE;
    _step_state(d, S_REQUEST_STEP);
  }
  else
  {
    d->auto_advance = FALSE;
    dt_control_log(_("slideshow paused"));
  }
}

static void _slow_down_callback(dt_action_t *action)
{
  dt_slideshow_t *d = dt_action_view(action)->data;
  _set_delay(d, 1);
  dt_control_log(ngettext("slideshow delay set to %d second", "slideshow delay set to %d seconds", d->delay), d->delay);
}

static void _speed_up_callback(dt_action_t *action)
{
  dt_slideshow_t *d = dt_action_view(action)->data;
  _set_delay(d, -1);
  dt_control_log(ngettext("slideshow delay set to %d second", "slideshow delay set to %d seconds", d->delay), d->delay);
}

static void _step_back_callback(dt_action_t *action)
{
  dt_slideshow_t *d = dt_action_view(action)->data;
  if(d->auto_advance) dt_control_log(_("slideshow paused"));
  d->auto_advance = FALSE;
  _step_state(d, S_REQUEST_STEP_BACK);
}

static void _step_forward_callback(dt_action_t *action)
{
  dt_slideshow_t *d = dt_action_view(action)->data;
  if(d->auto_advance) dt_control_log(_("slideshow paused"));
  d->auto_advance = FALSE;
  _step_state(d, S_REQUEST_STEP);
}

static void _exit_callback(dt_action_t *action)
{
  dt_slideshow_t *d = dt_action_view(action)->data;
  // go back to lt mode
  d->auto_advance = FALSE;
  dt_ctl_switch_mode_to("lighttable");
}

void gui_init(dt_view_t *self)
{
  dt_action_register(DT_ACTION(self), N_("start and stop"), _start_stop_callback, GDK_KEY_space, 0);
  dt_action_register(DT_ACTION(self), N_("exit slideshow"), _exit_callback, GDK_KEY_Escape, 0);

  dt_action_t *ac;
  ac = dt_action_register(DT_ACTION(self), N_("slow down"), _slow_down_callback, GDK_KEY_Up, 0);
  dt_shortcut_register(ac, 0, 0, GDK_KEY_KP_Add, 0);
  dt_shortcut_register(ac, 0, 0, GDK_KEY_plus, 0);
  ac = dt_action_register(DT_ACTION(self), N_("speed up"), _speed_up_callback, GDK_KEY_Down, 0);
  dt_shortcut_register(ac, 0, 0, GDK_KEY_KP_Subtract, 0);
  dt_shortcut_register(ac, 0, 0, GDK_KEY_minus, 0);

  dt_action_register(DT_ACTION(self), N_("step forward"), _step_forward_callback, GDK_KEY_Right, 0);
  dt_action_register(DT_ACTION(self), N_("step back"), _step_back_callback, GDK_KEY_Left, 0);
}

GSList *mouse_actions(const dt_view_t *self)
{
  GSList *lm = NULL;
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_LEFT, 0, _("go to next image"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_RIGHT, 0, _("go to previous image"));
  return lm;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
