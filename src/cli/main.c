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
#include "common/imageio_module.h"
#include "common/exif.h"
#include "common/history.h"

#include <sys/time.h>
#include <unistd.h>
int usleep(useconds_t usec);
#include <inttypes.h>
#include <libintl.h>

static void
usage(const char* progname)
{
  fprintf(stderr, "usage: %s <input file> [<xmp file>] <output file> [--width <max width>,--height <max height>,--bpp <bpp>,--hq <0|1|true|false> --verbose]\n", progname);
}

int main(int argc, char *arg[])
{
  bindtextdomain (GETTEXT_PACKAGE, DARKTABLE_LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  gtk_init (&argc, &arg);

  // parse command line arguments

  char *image_filename = NULL;
  char *xmp_filename = NULL;
  char *output_filename = NULL;
  int file_counter = 0;
  int width = 0, height = 0, bpp = 0;
  gboolean verbose = FALSE, high_quality = TRUE;

  for(int k=1; k<argc; k++)
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
        printf("this is darktable-cli\ncopyright (c) 2012-2013 johannes hanika, tobias ellinghaus\n");
        exit(1);
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
        fprintf(stderr, "%s %d\n", _("TODO: sorry, due to api restrictions we currently cannot set the bpp to"), bpp);
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

  char *m_arg[] = {"darktable-cli", "--library", ":memory:", NULL};
  // init dt without gui:
  if(dt_init(3, m_arg, 0)) exit(1);

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
    const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, id);
    dt_image_t *image = dt_image_cache_write_get(darktable.image_cache, cimg);
    dt_exif_xmp_read(image, xmp_filename, 1);
    // don't write new xmp:
    dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_RELAXED);
    dt_image_cache_read_release(darktable.image_cache, image);
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

  if(!strcmp(ext, "jpg"))
    ext = "jpeg";

  // init the export data structures
  int size = 0, dat_size = 0;
  dt_imageio_module_format_t *format;
  dt_imageio_module_storage_t *storage;
  dt_imageio_module_data_t *sdata, *fdata;

  storage = dt_imageio_get_storage_by_name("disk"); // only exporting to disk makes sense
  if(storage == NULL)
  {
    fprintf(stderr, "%s\n", _("cannot find disk storage module. please check your installation, something seems to be broken."));
    exit(1);
  }

  sdata = storage->get_params(storage, &size);
  if(sdata == NULL)
  {
    fprintf(stderr, "%s\n", _("failed to get parameters from storage module, aborting export ..."));
    exit(1);
  }

  // and now for the really ugly hacks. don't tell your children about this one or they won't sleep at night any longer ...
  g_strlcpy((char*)sdata, output_filename, DT_MAX_PATH_LEN);
  // all is good now, the last line didn't happen.

  format = dt_imageio_get_format_by_name(ext);
  if(format == NULL)
  {
    fprintf(stderr, _("unknown extension '.%s'"), ext);
    fprintf(stderr, "\n");
    exit(1);
  }

  fdata = format->get_params(format, &dat_size);
  if(fdata == NULL)
  {
    fprintf(stderr, "%s\n", _("failed to get parameters from format module, aborting export ..."));
    exit(1);
  }

  uint32_t w,h,fw,fh,sw,sh;
  fw=fh=sw=sh=0;
  storage->dimension(storage, &sw, &sh);
  format->dimension(format, &fw, &fh);

  if( sw==0 || fw==0) w=sw>fw?sw:fw;
  else w=sw<fw?sw:fw;

  if( sh==0 || fh==0) h=sh>fh?sh:fh;
  else h=sh<fh?sh:fh;

  fdata->max_width  = width;
  fdata->max_height = height;
  fdata->max_width = (w!=0 && fdata->max_width >w)?w:fdata->max_width;
  fdata->max_height = (h!=0 && fdata->max_height >h)?h:fdata->max_height;
  fdata->style[0] = '\0';

  //TODO: add a callback to set the bpp without going through the config

  storage->store(sdata, id, format, fdata, 1, 1, high_quality);

  // cleanup time
  if(storage->finalize_store) storage->finalize_store(storage, sdata);
  storage->free_params(storage, sdata);
  format->free_params(format, fdata);

  dt_cleanup();
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
