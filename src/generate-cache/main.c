/*
    This file is part of darktable,
    copyright (c) 2014 johannes hanika.
    copyright (c) 2015 LebedevRI.

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

#include <assert.h>    // for assert
#include <glib.h>      // for g_mkdir_with_parents, etc
#include <gtk/gtk.h>   // for gtk_init_check
#include <libintl.h>   // for bind_textdomain_codeset, etc
#include <limits.h>    // for PATH_MAX
#include <sqlite3.h>   // for sqlite3_finalize, etc
#include <stddef.h>    // for size_t
#include <stdint.h>    // for uint32_t, uint8_t
#include <stdio.h>     // for fprintf, stderr, snprintf, NULL, etc
#include <stdlib.h>    // for exit, free, malloc
#include <string.h>    // for strcmp
#include <sys/types.h> // for int32_t
#include <unistd.h>    // for access, unlink, R_OK

#include "common/darktable.h"    // for darktable, darktable_t, etc
#include "common/database.h"     // for dt_database_get
#include "common/debug.h"        // for DT_DEBUG_SQLITE3_PREPARE_V2
#include "common/imageio_jpeg.h" // for dt_imageio_jpeg_compress
#include "common/mipmap_cache.h" // for dt_mipmap_cache_t, etc
#include "config.h"              // for GETTEXT_PACKAGE, etc
#include "control/conf.h"        // for dt_conf_get_int
#include "develop/imageop.h"     // for dt_iop_flip_and_zoom_8

static void generate_thumbnail_cache()
{
  const int max_mip = DT_MIPMAP_2;
  fprintf(stderr, _("creating cache directories\n"));
  char filename[PATH_MAX] = { 0 };
  for(int k = DT_MIPMAP_0; k <= max_mip; k++)
  {
    snprintf(filename, sizeof(filename), "%s.d/%d", darktable.mipmap_cache->cachedir, k);
    fprintf(stderr, _("creating cache directory '%s'\n"), filename);
    int mkd = g_mkdir_with_parents(filename, 0750);
    if(mkd)
    {
      fprintf(stderr, _("could not create directory '%s'!\n"), filename);
      return;
    }
  }
  // some progress counter
  sqlite3_stmt *stmt;
  size_t image_count = 0, counter = 0;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select count(id) from images", -1, &stmt, 0);
  if(sqlite3_step(stmt) == SQLITE_ROW) image_count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  // go through all images:
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from images", -1, &stmt, 0);
  // could only alloc max_mip-1, but would need to detect the special case that max==0.
  const size_t bufsize
      = (size_t)4 * darktable.mipmap_cache->max_width[max_mip] * darktable.mipmap_cache->max_height[max_mip];
  uint8_t *tmp = (uint8_t *)dt_alloc_align(16, bufsize);
  if(!tmp)
  {
    fprintf(stderr, "couldn't allocate temporary memory!\n");
    sqlite3_finalize(stmt);
    return;
  }
  const int cache_quality = MIN(100, MAX(10, dt_conf_get_int("database_cache_quality")));
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int32_t imgid = sqlite3_column_int(stmt, 0);
    // check whether all of these files are already there
    int all_exist = 1;
    for(int k = max_mip; k >= DT_MIPMAP_0; k--)
    {
      snprintf(filename, sizeof(filename), "%s.d/%d/%d.jpg", darktable.mipmap_cache->cachedir, k, imgid);
      all_exist &= !access(filename, R_OK);
    }
    if(all_exist) goto next;
    dt_mipmap_buffer_t buf;
    // get largest thumbnail for this image
    // this one will take care of itself, we'll just write out the lower thumbs manually:
    dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, max_mip, DT_MIPMAP_BLOCKING, 'r');
    if(buf.width > 8 && buf.height > 8) // don't create for skulls
      for(int k = max_mip - 1; k >= DT_MIPMAP_0; k--)
      {
        uint32_t width, height;
        const int wd = darktable.mipmap_cache->max_width[k];
        const int ht = darktable.mipmap_cache->max_height[k];
        // use exactly the same mechanism as the cache internally to rescale the thumbnail:
        dt_iop_flip_and_zoom_8(buf.buf, buf.width, buf.height, tmp, wd, ht, 0, &width, &height);

        snprintf(filename, sizeof(filename), "%s.d/%d/%d.jpg", darktable.mipmap_cache->cachedir, k, imgid);
        FILE *f = fopen(filename, "wb");
        if(f)
        {
          // allocate temp memory:
          uint8_t *blob = (uint8_t *)malloc(bufsize);
          if(!blob) goto write_error;
          const int32_t length = dt_imageio_jpeg_compress(tmp, blob, width, height, cache_quality);
          assert(length <= bufsize);
          int written = fwrite(blob, sizeof(uint8_t), length, f);
          if(written != length)
          {
          write_error:
            unlink(filename);
          }
          free(blob);
          fclose(f);
        }
      }
    dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
  next:
    counter++;
    fprintf(stderr, "\rimage %zu/%zu (%.02f%%)            ", counter, image_count,
            100.0 * counter / (float)image_count);
  }
  dt_free_align(tmp);
  sqlite3_finalize(stmt);
  fprintf(stderr, "done                     \n");
}

static void usage(const char *progname)
{
  fprintf(stderr, "usage: %s [-h, --help; --version] [--core <darktable options>]\n", progname);
}

int main(int argc, char *arg[])
{
  bindtextdomain(GETTEXT_PACKAGE, DARKTABLE_LOCALEDIR);
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
  textdomain(GETTEXT_PACKAGE);

  gtk_init_check(&argc, &arg);

  // parse command line arguments

  int k;
  for(k = 1; k < argc; k++)
  {
    if(!strcmp(arg[k], "-h") || !strcmp(arg[k], "--help"))
    {
      usage(arg[0]);
      exit(1);
    }
    else if(!strcmp(arg[k], "--version"))
    {
      printf("this is darktable-generate-cache\ncopyright (c) 2014 johannes hanika; 2015 LebedevRI\n");
      exit(1);
    }
    else if(!strcmp(arg[k], "--core"))
    {
      // everything from here on should be passed to the core
      k++;
      break;
    }
  }

  int m_argc = 0;
  char *m_arg[5 + argc - k];
  m_arg[m_argc++] = "darktable-generate-cache";
  m_arg[m_argc++] = "--library";
  m_arg[m_argc++] = ":memory:";
  m_arg[m_argc++] = "--conf";
  m_arg[m_argc++] = "write_sidecar_files=FALSE";
  for(; k < argc; k++) m_arg[m_argc++] = arg[k];
  m_arg[m_argc] = NULL;

  // init dt without gui:
  if(dt_init(m_argc, m_arg, 0, NULL)) exit(1);

  fprintf(stderr, _("creating complete lighttable thumbnail cache\n"));
  generate_thumbnail_cache();
  dt_cleanup();
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
