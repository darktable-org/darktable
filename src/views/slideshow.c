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
#include "control/control.h"

#include <stdint.h>

DT_MODULE(1)


typedef struct dt_slideshow_t
{
  uint32_t random_state;
  uint32_t scramble;
  uint32_t use_random;
  uint32_t width, height;

  // double buffer
  uint32_t *buf1, *buf2;
  uint32_t *front, *back;
}
dt_slideshow_t;

// callbacks for in-memory export
static int
bpp (dt_imageio_module_data_t *data)
{
  return 8;
}

static int
levels(dt_imageio_module_data_t *data)
{
  return IMAGEIO_RGB | IMAGEIO_FLOAT;
}

static const char*
mime(dt_imageio_module_data_t *data)
{
  return "memory";
}

static int
write_image (dt_imageio_module_data_t *data, const char *filename, const void *in, void *exif, int exif_len, int imgid)
{
  // TODO: data->width data->height and in is the rgba buffer.
  // TODO: copy it to something that goes into cairo in expose()
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
  static int counter = 0;
  dt_imageio_module_format_t buf;
  dt_imageio_module_data_t dat;
  buf.mime = mime;
  buf.levels = levels;
  buf.bpp = bpp;
  buf.write_image = write_image;
  dat.max_width  = d->width;
  dat.max_height = d->height;
  dat.style[0] = '\0';

  // get random image id from sql
  int32_t id = 0;
  const uint32_t cnt = dt_collection_get_count (darktable.collection);
  // enumerated all images?
  if(++counter >= cnt) return 1;
  uint32_t ran = counter - 1;
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

  if(id)
  {
    dt_imageio_export(id, "unused", &buf, &dat, TRUE);
  }
  return 0;
}

static int32_t process_job_run(dt_job_t *job)
{
  dt_slideshow_t *d = *(dt_slideshow_t **)job->param;
  process_next_image(d);
  // TODO: swap front/back buffers
  // TODO: set some flag on d-> to signal change in buffers
  // TODO: trigger re-expose
  return 0;
}

static void process_job_init(dt_job_t *job, dt_slideshow_t *d)
{
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
}


void cleanup(dt_view_t *self)
{
  free(self->data);
}

void enter(dt_view_t *self)
{
  // TODO: alloc screen-size double buffer
  dt_slideshow_t *d = (dt_slideshow_t*)self->data;
  GdkScreen *screen = gtk_widget_get_screen(self->widget);
  if(!screen)
    screen = gdk_screen_get_default();
  d->width = gdk_screen_get_width(screen);
  d->height = gdk_screen_get_height(screen);
  fprintf(stderr, "[slideshow] enter %dx%d\n", d->width, d->height);
  d->buf1 = dt_alloc_align(64, sizeof(uint32_t)*d->width*d->height);
  d->buf2 = dt_alloc_align(64, sizeof(uint32_t)*d->width*d->height);
  d->front = d->buf1;
  d->back = d->buf2;
}

void leave(dt_view_t *self)
{
  fprintf(stderr, "[slideshow] leave\n");
  dt_free(d->buf1);
  dt_free(d->buf2);
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
  cairo_paint(cr);
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
  dt_job_t job;
  process_job_init(&job, d);
  dt_control_add_job(darktable.control, &job);
  return 0;
}

int key_released(dt_view_t *self, guint key, guint state)
{
  return 0;
}

int key_pressed(dt_view_t *self, guint key, guint state)
{
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
