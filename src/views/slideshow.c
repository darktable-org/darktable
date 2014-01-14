/*
    This file is part of darktable,
    copyright (c) 2013 johannes hanika.

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

#include "views/view.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
#include "common/collection.h"
#include "common/debug.h"
#include "common/dtpthread.h"
#include "control/control.h"
#include "control/conf.h"
#include "gui/gtk.h"

#include <stdint.h>

DT_MODULE(1)


typedef struct dt_slideshow_t
{
  uint32_t random_state;
  uint32_t scramble;
  uint32_t use_random;
  int32_t counter;
  int32_t step;
  uint32_t width, height;


  // double buffer
  uint32_t *buf1, *buf2;
  uint32_t *front, *back;
  // processed sizes might differ from screen size
  uint32_t front_width, front_height;
  uint32_t back_width, back_height;
  dt_pthread_mutex_t lock;
  uint32_t working;

  // output profile before we overwrote it:
  gchar *oldprofile;
}
dt_slideshow_t;

typedef struct dt_slideshow_format_t
{
  int max_width, max_height;
  int width, height;
  char style[128];
  dt_slideshow_t *d;
}
dt_slideshow_format_t;

// callbacks for in-memory export
static int
bpp (dt_imageio_module_data_t *data)
{
  return 8;
}

static int
levels(dt_imageio_module_data_t *data)
{
  return IMAGEIO_RGB | IMAGEIO_INT8;
}

static const char*
mime(dt_imageio_module_data_t *data)
{
  return "memory";
}

static int
write_image (dt_imageio_module_data_t *datai, const char *filename, const void *in, void *exif, int exif_len, int imgid)
{
  dt_slideshow_format_t *data = (dt_slideshow_format_t *)datai;
  dt_pthread_mutex_lock(&data->d->lock);
  if(data->d->back)
  { // might have been cleaned up when leaving slide show
    memcpy(data->d->back, in, sizeof(uint32_t)*data->d->width*data->d->height);
    data->d->back_width = datai->width;
    data->d->back_height = datai->height;
  }
  data->d->working = 0;
  dt_pthread_mutex_unlock(&data->d->lock);
  // trigger expose
  dt_control_queue_redraw_center();
  return 0;
}

static uint32_t
next_random(dt_slideshow_t *d)
{
  uint32_t i = d->random_state ++;
  // van der corput for 32 bits. this guarantees every number will appear exactly once
  i = ((i & 0x0000ffff) << 16) | ( i >> 16);
  i = ((i & 0x00ff00ff) <<  8) | ((i & 0xff00ff00) >> 8);
  i = ((i & 0x0f0f0f0f) <<  4) | ((i & 0xf0f0f0f0) >> 4);
  i = ((i & 0x33333333) <<  2) | ((i & 0xcccccccc) >> 2);
  i = ((i & 0x55555555) <<  1) | ((i & 0xaaaaaaaa) >> 1);
  return i ^ d->scramble;
}

// process image
static int
process_next_image(dt_slideshow_t *d)
{
  dt_imageio_module_format_t buf;
  dt_slideshow_format_t dat;
  buf.mime = mime;
  buf.levels = levels;
  buf.bpp = bpp;
  buf.write_image = write_image;
  dat.max_width  = d->width;
  dat.max_height = d->height;
  dat.style[0] = '\0';
  dat.d = d;

  // get random image id from sql
  int32_t id = 0;
  const int32_t cnt = dt_collection_get_count (darktable.collection);
  dt_pthread_mutex_lock(&d->lock);
  d->counter += d->step;
  int32_t ran = d->counter;
  dt_pthread_mutex_unlock(&d->lock);
  // enumerated all images?
  if(ran < 0 || ran >= cnt)
  {
    dt_control_log(_("end of images. press any key to return to lighttable mode"), d->counter, d->step);
  }
  if(d->use_random)
  {
    // get random number up to next power of two greater than cnt:
    const uint32_t zeros = __builtin_clz(cnt);
    // pull radical inverses only in our desired range:
    do ran = next_random(d) >> zeros;
    while(ran >= cnt);
  }
  const int32_t rand = ran % cnt;
  const gchar *query = dt_collection_get_query (darktable.collection);
  if(!query) return 1;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, rand);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, rand+1);
  if(sqlite3_step(stmt) == SQLITE_ROW)
    id = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  // this is a little slow, might be worth to do an option:
  const int high_quality = 1;
  if(id)
    // the flags are: ignore exif, display byteorder, high quality, thumbnail
    dt_imageio_export_with_flags(id, "unused", &buf, (dt_imageio_module_data_t *)&dat, 1, 1, high_quality, 0, 0, 0, 0, 0);
  return 0;
}

static int32_t process_job_run(dt_job_t *job)
{
  dt_slideshow_t *d = *(dt_slideshow_t **)job->param;
  process_next_image(d);
  return 0;
}

static void process_job_init(dt_job_t *job, dt_slideshow_t *d)
{
  d->working = 1;
  dt_control_job_init(job, "process slideshow image");
  job->execute = process_job_run;
  *((dt_slideshow_t **)job->param) = d;
}


// callbacks for a view module:

const char *name(dt_view_t *self)
{
  return _("slideshow");
}

uint32_t view(dt_view_t *self)
{
  return DT_VIEW_SLIDESHOW;
}

void init(dt_view_t *self)
{
  self->data = malloc(sizeof(dt_slideshow_t));
  dt_slideshow_t *lib = (dt_slideshow_t*)self->data;
  memset(lib, 0, sizeof(dt_slideshow_t));
  dt_pthread_mutex_init(&lib->lock, 0);
}


void cleanup(dt_view_t *self)
{
  dt_slideshow_t *lib = (dt_slideshow_t*)self->data;
  dt_pthread_mutex_destroy(&lib->lock);
  free(self->data);
}

void enter(dt_view_t *self)
{
  dt_slideshow_t *d = (dt_slideshow_t*)self->data;

  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_LEFT, FALSE);
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_RIGHT, FALSE);
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_TOP, FALSE);
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_BOTTOM, FALSE);
  // also hide arrows
  dt_ui_border_show(darktable.gui->ui, FALSE);

  // use display profile:
  d->oldprofile = dt_conf_get_string("plugins/lighttable/export/iccprofile");
  const gchar *overprofile = "X profile";
  dt_conf_set_string("plugins/lighttable/export/iccprofile", overprofile);

  // alloc screen-size double buffer
  GdkScreen *screen = gtk_widget_get_screen(dt_ui_main_window(darktable.gui->ui));
  if(!screen)
    screen = gdk_screen_get_default();
  dt_pthread_mutex_lock(&d->lock);
  d->width = gdk_screen_get_width(screen);
  d->height = gdk_screen_get_height(screen);
  d->buf1 = dt_alloc_align(64, sizeof(uint32_t)*d->width*d->height);
  d->buf2 = dt_alloc_align(64, sizeof(uint32_t)*d->width*d->height);
  d->front = d->buf1;
  d->back = d->buf2;

  // restart from beginning
  d->counter = 0;
  d->step = 0;
  dt_pthread_mutex_unlock(&d->lock);

  // start first job
  dt_job_t job;
  process_job_init(&job, d);
  dt_control_add_job(darktable.control, &job);
}

void leave(dt_view_t *self)
{
  dt_ui_border_show(darktable.gui->ui, TRUE);
  dt_slideshow_t *d = (dt_slideshow_t*)self->data;
  dt_conf_set_string("plugins/lighttable/export/iccprofile", d->oldprofile);
  g_free(d->oldprofile);
  d->oldprofile = 0;
  dt_pthread_mutex_lock(&d->lock);
  dt_free_align(d->buf1);
  dt_free_align(d->buf2);
  d->buf1 = d->buf2 = d->front = d->back = 0;
  dt_pthread_mutex_unlock(&d->lock);
}

void reset(dt_view_t *self)
{
  // dt_slideshow_t *lib = (dt_slideshow_t *)self->data;
}


void mouse_enter(dt_view_t *self)
{
}

void mouse_leave(dt_view_t *self)
{
}

void expose(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  // TODO: pick up state changes and wait for frontbuffer lock
  // TODO: draw image from bg thread
  dt_slideshow_t *d = (dt_slideshow_t*)self->data;

  dt_pthread_mutex_lock(&d->lock);
  cairo_paint(cr);
  if(d->front)
  {
    // undo clip region/border around the image:
    cairo_restore(cr); // pop view manager
    cairo_restore(cr); // pop control
    cairo_reset_clip(cr);
    cairo_save(cr);
    cairo_translate(cr, (d->width-d->front_width)*.5f, (d->height-d->front_height)*.5f);
    cairo_surface_t *surface = NULL;
    const int32_t stride = cairo_format_stride_for_width (CAIRO_FORMAT_RGB24, d->front_width);
    surface = cairo_image_surface_create_for_data ((uint8_t *)d->front, CAIRO_FORMAT_RGB24, d->front_width, d->front_height, stride);
    cairo_set_source_surface (cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_rectangle(cr, 0, 0, d->front_width, d->front_height);
    cairo_fill(cr);
    cairo_surface_destroy (surface);
    cairo_restore(cr);
    cairo_save(cr); // pretend we didn't already pop the stack
    cairo_save(cr);
  }
  dt_pthread_mutex_unlock(&d->lock);
}

int scrolled(dt_view_t *self, double x, double y, int up, int state)
{
  return 0;
}


void mouse_moved(dt_view_t *self, double x, double y, int which)
{
}


int button_released(dt_view_t *self, double x, double y, int which, uint32_t state)
{
  return 0;
}


int button_pressed(dt_view_t *self, double x, double y, int which, int type, uint32_t state)
{
  dt_slideshow_t *d = (dt_slideshow_t *)self->data;
  if(which == 1)
    d->step = 1;
  else if(which == 3)
    d->step = -1;
  else return 1;

  // swap buffers and kick off new job.
  dt_pthread_mutex_lock(&d->lock);
  if(d->working)
  {
    // TODO: this is stupid, defer the else branch and shut up!
    dt_control_log(_("busy"));
  }
  else
  {
    // TODO: if step changed, don't just swap but kick off new job to
    // TODO: be shown as soon as it finishes
    uint32_t *tmp = d->front;
    d->front = d->back;
    d->back = tmp;
    d->front_width = d->back_width;
    d->front_height = d->back_height;

    // draw new front buf
    dt_control_queue_redraw_center();

    dt_job_t job;
    process_job_init(&job, d);
    dt_control_add_job(darktable.control, &job);
  }
  dt_pthread_mutex_unlock(&d->lock);
  return 0;
}

int key_released(dt_view_t *self, guint key, guint state)
{
  return 0;
}

int key_pressed(dt_view_t *self, guint key, guint state)
{
  // go back to lt mode
  dt_ctl_switch_mode_to(DT_LIBRARY);
  return 0;
}

void border_scrolled(dt_view_t *view, double x, double y, int which, int up)
{
}

void init_key_accels(dt_view_t *self)
{
}

void connect_key_accels(dt_view_t *self)
{
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
