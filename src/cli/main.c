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
 *  - add options for HQ export, interpolator
 *  - make these settings work
 *  - get rid of setting the storage using the config
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
#include "control/conf.h"

#include <sys/time.h>
#include <unistd.h>
int usleep(useconds_t usec);
#include <inttypes.h>

static void
usage(const char* progname)
{
  fprintf(stderr, "usage: %s <input file> <xmp file> <output file> [--width <max width>,--height <max height>,--bpp <bpp>] [darktable options]\n", progname);
}

int main(int argc, char *arg[])
{
  gtk_init (&argc, &arg);

  // parse command line arguments

  char *image_filename = NULL;
  char *xmp_filename = NULL;
  char *output_filename = NULL;
  int file_counter = 0;
  int width = 0, height = 0, bpp = 0;

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
        printf("this is darktable-cli\ncopyright (c) 2012 johannes hanika, tobias ellinghaus\n");
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
        fprintf(stderr, "WARNING: sorry, due to api restrictions we currently cannot set the bpp\n");
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

  if(file_counter < 3){
    usage(arg[0]);
    exit(0);
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
    fprintf(stderr, "error: can't open file %s\n", image_filename);
    exit(1);
  }
  g_free(directory);

  // try to find out the export format from the output_filename
  const char *ext = output_filename + strlen(output_filename);
  while(ext > output_filename && *ext != '.') ext--;
  ext++;

  if(!strcmp(ext, "jpg"))
    ext = "jpeg";

  // init the export data structures
  dt_imageio_module_format_t *format;
  dt_imageio_module_storage_t *storage;

  //exporting to disk is the only thing that makes sense here
  int k=0;
  int old_k = dt_conf_get_int("plugins/lighttable/export/storage"); //FIXME
  GList *it = g_list_first(darktable.imageio->plugins_storage);
  if( it != NULL )
    do
    {
      k++;
      if(!strcmp(((dt_imageio_module_storage_t *)it->data)->plugin_name, "disk"))
      {
        storage = it->data;
        break;
      }
    }
    while( ( it = g_list_next(it) ) );
  if(it) // ah, we found the disk storage facility
  {
    format = dt_imageio_get_format_by_name(ext);
    int dat_size = 0;
    dt_imageio_module_data_t *dat = format->get_params(format, &dat_size);
    dat->max_width  = width;
    dat->max_height = height;
    //TODO: add a callback to set the bpp without going through the config


    dt_conf_set_int ("plugins/lighttable/export/storage", k); //FIXME this has to change!
    dt_imageio_export(id, output_filename, format, dat);
    dt_conf_set_int ("plugins/lighttable/export/storage", old_k); //FIXME
  }

  dt_cleanup();
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
