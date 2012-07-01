/*
    This file is part of darktable,
    copyright (c) 2011 johannes hanika.

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

#include "common/darktable.h"
#include "common/debug.h"
#include "common/collection.h"
#include "common/points.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
#include "control/conf.h"

#include <sys/time.h>
#include <unistd.h>
int usleep(useconds_t usec);
#include <inttypes.h>

static int
bpp (dt_imageio_module_data_t *data)
{
  return 32;
}

static int
write_image (dt_imageio_module_data_t *data, const char *filename, const void *in, void *exif, int exif_len, int imgid)
{
#if 0
  const int offx = 0;//(width  - data->width )/2;
  const int offy = 0;//(height - data->height)/2;
  float *out = pixels + (offy * width  + offx )* 4;
  const float *rd = in;
  memset(pixels, 0, 4*sizeof(float)*width*height);
  const float alpha = 0.2f;
  for(int i=3;i<4*width*height;i+=4) pixels[i] = 0.2f;
  for(int j=0; j<MIN(data->height, height); j++)
  {
    for(int i=0; i<MIN(data->width, width); i++)
    {
      for(int c=0; c<3; c++) out[4*i+c] = rd[4*i+c];
      out[4*i+3] = alpha;
    }
    out += 4*width;
    rd  += 4*data->width;
  }
#endif
}

static int
process_next_image()
{
  // static int counter = 0;
  dt_imageio_module_format_t buf;
  dt_imageio_module_data_t dat;
  buf.bpp = bpp;
  buf.write_image = write_image;
  dat.max_width  = 1920;//width;
  dat.max_height = 1080;//height;
#if 0

  // get random image id from sql
  int32_t id = 0;
  const uint32_t cnt = dt_collection_get_count (darktable.collection);
  // enumerated all images?
  if(++counter >= cnt) return 1;
  uint32_t ran = counter - 1;
  if(use_random)
  {
    // get random number up to next power of two greater than cnt:
    const uint32_t zeros = __builtin_clz(cnt);
    // pull radical inverses only in our desired range:
    do ran = next_random() >> zeros;
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
    // get image from cache
    dt_imageio_export(id, "unused", &buf, &dat);
  }
#endif
  return 0;
}

int main(int argc, char *arg[])
{
  gtk_init (&argc, &arg);
  char *m_arg[] = {"darktable-cli", "--library", ":memory:", NULL};
  // init dt without gui:
  if(dt_init(3, m_arg, 0)) exit(1);

  process_next_image();

  dt_cleanup();
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
