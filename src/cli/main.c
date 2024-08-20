/*
    This file is part of darktable,
    Copyright (C) 2012-2020 darktable developers.

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

#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/exif.h"
#include "common/film.h"
#include "common/history.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/imageio_jpeg.h"
#include "common/imageio_module.h"
#include "common/points.h"
#include "control/conf.h"
#include "develop/imageop.h"

#include <inttypes.h>
#include <libintl.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef __APPLE__
#include "osx/osx.h"
#endif

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

#define DT_MAX_STYLE_NAME_LENGTH 128

typedef struct options {
  const char* style;
  int width;
  int height;
  int bpp;
  gboolean  verbose;
  gboolean high_quality;
  gboolean upscale;
  gboolean style_overwrite;
  gboolean custom_presets;
  gboolean export_masks;
} options;

int process_file(const char* input_filename, const char* xmp_filename, char* output_filename, const options* program_options);

static void usage(const char *progname)
{
  fprintf(stderr, "usage: %s [--batch batch_file | <input file> [<xmp file>] <output file>] [options] [--core <darktable options>]\n", progname);
  fprintf(stderr, "\n");
  fprintf(stderr, "options:\n");
  fprintf(stderr, "   --width <max width> default: 0 = full resolution\n");
  fprintf(stderr, "   --height <max height> default: 0 = full resolution\n");
  fprintf(stderr, "   --bpp <bpp>, unsupported\n");
  fprintf(stderr, "   --hq <0|1|false|true> default: true\n");
  fprintf(stderr, "   --upscale <0|1|false|true>, default: false\n");
  fprintf(stderr, "   --export_masks <0|1|false|true>, default: false\n");
  fprintf(stderr, "   --style <style name>\n");
  fprintf(stderr, "   --style-overwrite\n");
  fprintf(stderr, "   --apply-custom-presets <0|1|false|true>, default: true\n");
  fprintf(stderr, "                          disable for multiple instances\n");
  fprintf(stderr, "   --verbose\n");
  fprintf(stderr, "   --help,-h\n");
  fprintf(stderr, "   --version\n");
}

int main(int argc, char *arg[])
{
#ifdef __APPLE__
  dt_osx_prepare_environment();
#endif
  bindtextdomain(GETTEXT_PACKAGE, DARKTABLE_LOCALEDIR);
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
  textdomain(GETTEXT_PACKAGE);

  if(!gtk_parse_args(&argc, &arg)) exit(1);

  // parse command line arguments
  char *input_filename = NULL;
  char *xmp_filename = NULL;
  char *output_filename = NULL;
  char *batch_filename = NULL;
  int file_counter = 0;
  options program_options = {
    .style = NULL,
    .width = 0,
    .height = 0,
    .bpp = 0,
    .verbose = FALSE,
    .high_quality = TRUE,
    .upscale = FALSE,
    .style_overwrite = FALSE,
    .custom_presets = TRUE,
    .export_masks = FALSE
  };

  int k;
  for(k = 1; k < argc; k++)
  {
    if(arg[k][0] == '-')
    {
      if(!strcmp(arg[k], "--help") || !strcmp(arg[k], "-h"))
      {
        usage(arg[0]);
        exit(1);
      }
      else if(!strcmp(arg[k], "--version"))
      {
        printf("this is darktable-cli %s\ncopyright (c) 2012-%s johannes hanika, tobias ellinghaus\n",
               darktable_package_version, darktable_last_commit_year);
        exit(0);
      }
      else if(!strcmp(arg[k], "--width") && argc > k + 1)
      {
        k++;
        program_options.width = MAX(atoi(arg[k]), 0);
      }
      else if(!strcmp(arg[k], "--height") && argc > k + 1)
      {
        k++;
        program_options.height = MAX(atoi(arg[k]), 0);
      }
      else if(!strcmp(arg[k], "--bpp") && argc > k + 1)
      {
        k++;
        program_options.bpp = MAX(atoi(arg[k]), 0);
        fprintf(stderr, "%s %d\n", _("TODO: sorry, due to API restrictions we currently cannot set the BPP to"),
                program_options.bpp);
      }
      else if(!strcmp(arg[k], "--hq") && argc > k + 1)
      {
        k++;
        gchar *str = g_ascii_strup(arg[k], -1);
        if(!g_strcmp0(str, "0") || !g_strcmp0(str, "FALSE"))
          program_options.high_quality = FALSE;
        else if(!g_strcmp0(str, "1") || !g_strcmp0(str, "TRUE"))
          program_options.high_quality = TRUE;
        else
        {
          fprintf(stderr, "%s: %s\n", _("unknown option for --hq"), arg[k]);
          usage(arg[0]);
          exit(1);
        }
        g_free(str);
      }
      else if(!strcmp(arg[k], "--export_masks") && argc > k + 1)
      {
        k++;
        gchar *str = g_ascii_strup(arg[k], -1);
        if(!g_strcmp0(str, "0") || !g_strcmp0(str, "FALSE"))
          program_options.export_masks = FALSE;
        else if(!g_strcmp0(str, "1") || !g_strcmp0(str, "TRUE"))
          program_options.export_masks = TRUE;
        else
        {
          fprintf(stderr, "%s: %s\n", _("unknown option for --export_masks"), arg[k]);
          usage(arg[0]);
          exit(1);
        }
        g_free(str);
      }
      else if(!strcmp(arg[k], "--upscale") && argc > k + 1)
      {
        k++;
        gchar *str = g_ascii_strup(arg[k], -1);
        if(!g_strcmp0(str, "0") || !g_strcmp0(str, "FALSE"))
          program_options.upscale = FALSE;
        else if(!g_strcmp0(str, "1") || !g_strcmp0(str, "TRUE"))
          program_options.upscale = TRUE;
        else
        {
          fprintf(stderr, "%s: %s\n", _("unknown option for --upscale"), arg[k]);
          usage(arg[0]);
          exit(1);
        }
        g_free(str);
      }
      else if(!strcmp(arg[k], "--style") && argc > k + 1)
      {
        k++;
        program_options.style = arg[k];
      }
      else if(!strcmp(arg[k], "--style-overwrite"))
      {
        program_options.style_overwrite = TRUE;
      }
      else if(!strcmp(arg[k], "--apply-custom-presets") && argc > k + 1)
      {
        k++;
        gchar *str = g_ascii_strup(arg[k], -1);
        if(!g_strcmp0(str, "0") || !g_strcmp0(str, "FALSE"))
          program_options.custom_presets = FALSE;
        else if(!g_strcmp0(str, "1") || !g_strcmp0(str, "TRUE"))
          program_options.custom_presets = TRUE;
        else
        {
          fprintf(stderr, "%s: %s\n", _("unknown option for --apply-custom-presets"), arg[k]);
          usage(arg[0]);
          exit(1);
        }
        g_free(str);
      }
      else if(!strcmp(arg[k], "--batch") && argc > k + 1)
      {
        k++;
        batch_filename = arg[k];
      }
      else if(!strcmp(arg[k], "-v") || !strcmp(arg[k], "--verbose"))
      {
        program_options.verbose = TRUE;
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
        input_filename = arg[k];
      else if(file_counter == 1)
        xmp_filename = arg[k];
      else if(file_counter == 2)
        output_filename = arg[k];
      file_counter++;
    }
  }

  int m_argc = 0;
  char **m_arg = malloc((5 + argc - k + 1) * sizeof(char *));
  m_arg[m_argc++] = "darktable-cli";
  m_arg[m_argc++] = "--library";
  m_arg[m_argc++] = ":memory:";
  m_arg[m_argc++] = "--conf";
  m_arg[m_argc++] = "write_sidecar_files=FALSE";
  for(; k < argc; k++) m_arg[m_argc++] = arg[k];
  m_arg[m_argc] = NULL;

  if(batch_filename != NULL && file_counter > 0)
  {
    usage(arg[0]);
    free(m_arg);
    exit(1);
  }
  else
  {
    if(file_counter < 2 || file_counter > 3)
    {
      usage(arg[0]);
      free(m_arg);
      exit(1);
    }
    else if(file_counter == 2)
    {
      // no xmp file given
      output_filename = xmp_filename;
      xmp_filename = NULL;
    }
  }

  // init dt without gui and without data.db:
  if(dt_init(m_argc, m_arg, FALSE, program_options.custom_presets, NULL))
  {
    return 1;
  }

  int status = process_file(input_filename, xmp_filename, output_filename, &program_options);

  free(m_arg);
  exit(status);
}

/* Side effect: output_filename is modified. */
int process_file(const char* input_filename, const char* xmp_filename, char* output_filename, const options* program_options)
{
  if(g_file_test(output_filename, G_FILE_TEST_IS_DIR))
  {
    fprintf(stderr, _("error: output file is a directory. please specify file name"));
    fprintf(stderr, "\n");
    return 1;
  }

  // the output file already exists, so there will be a sequence number added
  if(g_file_test(output_filename, G_FILE_TEST_EXISTS))
  {
    fprintf(stderr, "%s\n", _("output file already exists, it will get renamed"));
  }

  GList *id_list = NULL;

  if(g_file_test(input_filename, G_FILE_TEST_IS_DIR))
  {
    const int filmid = dt_film_import(input_filename);
    if(!filmid)
    {
      fprintf(stderr, _("error: can't open folder %s"), input_filename);
      fprintf(stderr, "\n");
      return 1;
    }
    id_list = dt_film_get_image_ids(filmid);
  }
  else
  {
    dt_film_t film;
    int id = 0;
    int filmid = 0;

    gchar *directory = g_path_get_dirname(input_filename);
    filmid = dt_film_new(&film, directory);
    id = dt_image_import(filmid, input_filename, TRUE);
    if(!id)
    {
      fprintf(stderr, _("error: can't open file %s"), input_filename);
      fprintf(stderr, "\n");
      return 1;
    }
    g_free(directory);

    id_list = g_list_append(id_list, GINT_TO_POINTER(id));
  }

  const int total = g_list_length(id_list);

  if(total == 0)
  {
    fprintf(stderr, _("no images to export, aborting\n"));
    return 1;
  }

  // attach xmp, if requested:
  if(xmp_filename)
  {
    for(GList *iter = id_list; iter; iter = g_list_next(iter))
    {
      int id = GPOINTER_TO_INT(iter->data);
      dt_image_t *image = dt_image_cache_get(darktable.image_cache, id, 'w');
      if(dt_exif_xmp_read(image, xmp_filename, 1) != 0)
      {
        fprintf(stderr, _("error: can't open xmp file %s"), xmp_filename);
        fprintf(stderr, "\n");
        return 1;
      }
      // don't write new xmp:
      dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_RELAXED);
    }
  }

  // print the history stack. only look at the first image and assume all got the same processing applied
  if(program_options->verbose)
  {
    int id = GPOINTER_TO_INT(id_list->data);
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
    return 1;
  }

  sdata = storage->get_params(storage);
  if(sdata == NULL)
  {
    fprintf(stderr, "%s\n", _("failed to get parameters from storage module, aborting export ..."));
    return 1;
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
    return 1;
  }

  fdata = format->get_params(format);
  if(fdata == NULL)
  {
    fprintf(stderr, "%s\n", _("failed to get parameters from format module, aborting export ..."));
    return 1;
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

  fdata->max_width = program_options->width;
  fdata->max_height = program_options->height;
  fdata->max_width = (w != 0 && fdata->max_width > w) ? w : fdata->max_width;
  fdata->max_height = (h != 0 && fdata->max_height > h) ? h : fdata->max_height;
  fdata->style[0] = '\0';
  fdata->style_append = 1; // make append the default and override with --style-overwrite

  if(program_options->style)
  {
    g_strlcpy((char *)fdata->style, program_options->style, DT_MAX_STYLE_NAME_LENGTH);
    fdata->style[127] = '\0';
    if(program_options->style_overwrite)
      fdata->style_append = 0;
  }

  if(storage->initialize_store)
  {
    storage->initialize_store(storage, sdata, &format, &fdata, &id_list, program_options->high_quality, program_options->upscale);

    format->set_params(format, fdata, format->params_size(format));
    storage->set_params(storage, sdata, storage->params_size(storage));
  }

  // TODO: do we want to use the settings from conf?
  // TODO: expose these via command line arguments
  dt_colorspaces_color_profile_type_t icc_type = DT_COLORSPACE_NONE;
  const gchar *icc_filename = NULL;
  dt_iop_color_intent_t icc_intent = DT_INTENT_LAST;

  // TODO: add a callback to set the bpp without going through the config

  int num = 1;
  for(GList *iter = id_list; iter; iter = g_list_next(iter), num++)
  {
    const int id = GPOINTER_TO_INT(iter->data);
    // TODO: have a parameter in command line to get the export presets
    dt_export_metadata_t metadata;
    metadata.flags = dt_lib_export_metadata_default_flags();
    metadata.list = NULL;
    storage->store(storage, sdata, id, format, fdata, num, total,
                   program_options->high_quality, program_options->upscale, program_options->export_masks,
                   icc_type, icc_filename, icc_intent, &metadata);
  }

  // cleanup time
  if(storage->finalize_store) storage->finalize_store(storage, sdata);
  storage->free_params(storage, sdata);
  format->free_params(format, fdata);
  g_list_free(id_list);

  dt_cleanup();

  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
