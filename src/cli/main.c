/*
    This file is part of darktable,
    copyright (c) 2012 johannes hanika, tobias ellinghaus.

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

/**
 * TODO:
 *  - make --bpp work
 *  - add options for interpolator
 *  - make these settings work
 *  - ???
 *  - profit
 */

#include "common/darktable.h"
#include "common/debug.h"
#include "common/collection.h"
#include "common/points.h"
#include "common/film.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/imageio_jpeg.h"
#include "common/imageio_module.h"
#include "common/exif.h"
#include "common/history.h"
#include "control/conf.h"
#include "develop/imageop.h"

#include <sys/time.h>
#include <unistd.h>
#include <inttypes.h>
#include <libintl.h>

static void generate_thumbnail_cache()
{
  const int max_mip = DT_MIPMAP_2;
  fprintf(stderr, _("creating cache directories\n"));
  char filename[PATH_MAX] = {0};
  for(int k=DT_MIPMAP_0;k<=max_mip;k++)
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
  uint64_t image_count = 0, counter = 0;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select count(id) from images", -1, &stmt, 0);
  if(sqlite3_step(stmt) == SQLITE_ROW)
    image_count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  // go through all images:
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from images", -1, &stmt, 0);
  // could only alloc max_mip-1, but would need to detect the special case that max==0.
  const size_t bufsize = (size_t)4 * darktable.mipmap_cache->max_width[max_mip]
                         * darktable.mipmap_cache->max_height[max_mip];
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
    for(int k=max_mip;k>=DT_MIPMAP_0;k--)
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
    for(int k=max_mip-1;k>=DT_MIPMAP_0;k--)
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
        const int32_t length
          = dt_imageio_jpeg_compress(tmp, blob, width, height, cache_quality);
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
    counter ++;
    fprintf(stderr, "\rimage %lu/%lu (%.02f%%)            ", counter, image_count, 100.0*counter/(float)image_count);
  }
  dt_free_align(tmp);
  sqlite3_finalize(stmt);
  fprintf(stderr, "done                     \n");
}

static void usage(const char *progname)
{
  fprintf(stderr, "usage: %s <input file> [<xmp file>] <output file> [--width <max width>,--height <max "
                  "height>,--bpp <bpp>,--hq <0|1|true|false>,--verbose] [--core <darktable options>] [--generate-cache]\n",
          progname);
}

int main(int argc, char *arg[])
{
  bindtextdomain(GETTEXT_PACKAGE, DARKTABLE_LOCALEDIR);
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
  textdomain(GETTEXT_PACKAGE);

  gtk_init_check(&argc, &arg);

  // parse command line arguments
  char *image_filename = NULL;
  char *xmp_filename = NULL;
  char *output_filename = NULL;
  int file_counter = 0;
  int width = 0, height = 0, bpp = 0;
  gboolean verbose = FALSE, high_quality = TRUE, generate_cache = FALSE;

  int k;
  for(k = 1; k < argc; k++)
  {
    if(arg[k][0] == '-')
    {
      if(!strcmp(arg[k], "--help"))
      {
        usage(arg[0]);
        exit(1);
      }
      else if(!strcmp(arg[k], "--version"))
      {
        printf("this is darktable-cli\ncopyright (c) 2012-2014 johannes hanika, tobias ellinghaus\n");
        exit(1);
      }
      else if(!strcmp(arg[k], "--generate-cache"))
      {
        generate_cache = TRUE;
      }
      else if(!strcmp(arg[k], "--width"))
      {
        k++;
        width = MAX(atoi(arg[k]), 0);
      }
      else if(!strcmp(arg[k], "--height"))
      {
        k++;
        height = MAX(atoi(arg[k]), 0);
      }
      else if(!strcmp(arg[k], "--bpp"))
      {
        k++;
        bpp = MAX(atoi(arg[k]), 0);
        fprintf(stderr, "%s %d\n",
                _("TODO: sorry, due to API restrictions we currently cannot set the BPP to"), bpp);
      }
      else if(!strcmp(arg[k], "--hq"))
      {
        k++;
        gchar *str = g_ascii_strup(arg[k], -1);
        if(!g_strcmp0(str, "0") || !g_strcmp0(str, "FALSE"))
          high_quality = FALSE;
        else if(!g_strcmp0(str, "1") || !g_strcmp0(str, "TRUE"))
          high_quality = TRUE;
        else
        {
          fprintf(stderr, "%s: %s\n", _("Unknown option for --hq"), arg[k]);
          usage(arg[0]);
          exit(1);
        }
        g_free(str);
      }
      else if(!strcmp(arg[k], "-v") || !strcmp(arg[k], "--verbose"))
      {
        verbose = TRUE;
      }
      else if(!strcmp(arg[k], "--core"))
      {
        // everything from here on should be passed to the core
        k++;
        break;
      }
    }
    else
    {
      if(file_counter == 0)
        image_filename = arg[k];
      else if(file_counter == 1)
        xmp_filename = arg[k];
      else if(file_counter == 2)
        output_filename = arg[k];
      file_counter++;
    }
  }

  int m_argc = 0;
  char *m_arg[5 + argc - k];
  m_arg[m_argc++] = "darktable-cli";
  m_arg[m_argc++] = "--library";
  m_arg[m_argc++] = ":memory:";
  m_arg[m_argc++] = "--conf";
  m_arg[m_argc++] = "write_sidecar_files=FALSE";
  for(; k < argc; k++) m_arg[m_argc++] = arg[k];
  m_arg[m_argc] = NULL;

  if(!generate_cache)
  {
    if(file_counter < 2 || file_counter > 3)
    {
      usage(arg[0]);
      exit(1);
    }
    else if(file_counter == 2)
    {
      // no xmp file given
      output_filename = xmp_filename;
      xmp_filename = NULL;
    }

    // the output file already exists, so there will be a sequence number added
    if(g_file_test(output_filename, G_FILE_TEST_EXISTS))
    {
      fprintf(stderr, "%s\n", _("output file already exists, it will get renamed"));
    }
  }

  // init dt without gui:
  if(dt_init(m_argc, m_arg, 0, NULL)) exit(1);

  if(generate_cache)
  {
    fprintf(stderr, _("creating complete lighttable thumbnail cache\n"));
    generate_thumbnail_cache();
    dt_cleanup();
    exit(0);
  }

  dt_film_t film;
  int id = 0;
  int filmid = 0;

  gchar *directory = g_path_get_dirname(image_filename);
  filmid = dt_film_new(&film, directory);
  id = dt_image_import(filmid, image_filename, TRUE);
  if(!id)
  {
    fprintf(stderr, _("error: can't open file %s"), image_filename);
    fprintf(stderr, "\n");
    exit(1);
  }
  g_free(directory);

  // attach xmp, if requested:
  if(xmp_filename)
  {
    dt_image_t *image = dt_image_cache_get(darktable.image_cache, id, 'w');
    dt_exif_xmp_read(image, xmp_filename, 1);
    // don't write new xmp:
    dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_RELAXED);
  }

  // print the history stack
  if(verbose)
  {
    gchar *history = dt_history_get_items_as_string(id);
    if(history)
      printf("%s\n", history);
    else
      printf("[%s]\n", _("empty history stack"));
  }

  // try to find out the export format from the output_filename
  char *ext = output_filename + strlen(output_filename);
  while(ext > output_filename && *ext != '.') ext--;
  *ext = '\0';
  ext++;

  if(!strcmp(ext, "jpg")) ext = "jpeg";

  if(!strcmp(ext, "tif")) ext = "tiff";

  // init the export data structures
  dt_imageio_module_format_t *format;
  dt_imageio_module_storage_t *storage;
  dt_imageio_module_data_t *sdata, *fdata;

  storage = dt_imageio_get_storage_by_name("disk"); // only exporting to disk makes sense
  if(storage == NULL)
  {
    fprintf(
        stderr, "%s\n",
        _("cannot find disk storage module. please check your installation, something seems to be broken."));
    exit(1);
  }

  sdata = storage->get_params(storage);
  if(sdata == NULL)
  {
    fprintf(stderr, "%s\n", _("failed to get parameters from storage module, aborting export ..."));
    exit(1);
  }

  // and now for the really ugly hacks. don't tell your children about this one or they won't sleep at night
  // any longer ...
  g_strlcpy((char *)sdata, output_filename, DT_MAX_PATH_FOR_PARAMS);
  // all is good now, the last line didn't happen.

  format = dt_imageio_get_format_by_name(ext);
  if(format == NULL)
  {
    fprintf(stderr, _("unknown extension '.%s'"), ext);
    fprintf(stderr, "\n");
    exit(1);
  }

  fdata = format->get_params(format);
  if(fdata == NULL)
  {
    fprintf(stderr, "%s\n", _("failed to get parameters from format module, aborting export ..."));
    exit(1);
  }

  uint32_t w, h, fw, fh, sw, sh;
  fw = fh = sw = sh = 0;
  storage->dimension(storage, sdata, &sw, &sh);
  format->dimension(format, fdata, &fw, &fh);

  if(sw == 0 || fw == 0)
    w = sw > fw ? sw : fw;
  else
    w = sw < fw ? sw : fw;

  if(sh == 0 || fh == 0)
    h = sh > fh ? sh : fh;
  else
    h = sh < fh ? sh : fh;

  fdata->max_width = width;
  fdata->max_height = height;
  fdata->max_width = (w != 0 && fdata->max_width > w) ? w : fdata->max_width;
  fdata->max_height = (h != 0 && fdata->max_height > h) ? h : fdata->max_height;
  fdata->style[0] = '\0';
  fdata->style_append = 0;

  if(storage->initialize_store)
  {
    GList *single_image = g_list_append(NULL, GINT_TO_POINTER(id));
    storage->initialize_store(storage, sdata, &format, &fdata, &single_image, high_quality);
    g_list_free(single_image);
  }
  // TODO: add a callback to set the bpp without going through the config

  storage->store(storage, sdata, id, format, fdata, 1, 1, high_quality);

  // cleanup time
  if(storage->finalize_store) storage->finalize_store(storage, sdata);
  storage->free_params(storage, sdata);
  format->free_params(format, fdata);

  dt_cleanup();
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
