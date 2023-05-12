/*
    This file is part of darktable,
    Copyright (C) 2012-2023 darktable developers.

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
#include "common/file_location.h"
#include "common/history.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/points.h"
#include "control/conf.h"
#include "develop/imageop.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_jpeg.h"
#include "imageio/imageio_module.h"

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

// Make sure it's OK to limit output extension length
#define DT_MAX_OUTPUT_EXT_LENGTH 5

static void usage(const char *progname)
{
  fprintf(stderr, "usage: %s [<input file or dir>] [<XMP file>] <output destination> [options] [--core <darktable options>]\n", progname);
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
  fprintf(stderr, "   --out-ext <extension>, default from output destination or '.jpg'\n");
  fprintf(stderr, "                          if specified, takes preference over output\n");
  fprintf(stderr, "   --import <file or dir> specify input file or dir, can be used'\n");
  fprintf(stderr, "                          multiple times instead of input file\n");
  fprintf(stderr, "   --icc-type <type> specify icc type, default to NONE\n");
  fprintf(stderr, "                     use --help icc-type for list of supported types\n");
  fprintf(stderr, "   --icc-file <file> specify icc filename, default to NONE\n");
  fprintf(stderr, "   --icc-intent <intent> specify icc intent, default to LAST\n");
  fprintf(stderr, "                     use --help icc-intent for list of supported intents\n");
  fprintf(stderr, "   --verbose\n");
  fprintf(stderr, "   --help,-h [option]\n");
  fprintf(stderr, "   --version\n");
}

static void icc_types()
{
  // TODO: Can this be automated to keep in sync with colorspaces.h?
  fprintf(stderr, "available ICC types:\n");
  fprintf(stderr, " NONE\n");
  fprintf(stderr, " FILE\n");
  fprintf(stderr, " SRGB\n");
  fprintf(stderr, " ADOBERGB\n");
  fprintf(stderr, " LIN_REC709\n");
  fprintf(stderr, " LIN_REC2020\n");
  fprintf(stderr, " XYZ\n");
  fprintf(stderr, " LAB\n");
  fprintf(stderr, " INFRARED\n");
  fprintf(stderr, " DISPLAY\n");
  fprintf(stderr, " EMBEDDED_ICC\n");
  fprintf(stderr, " EMBEDDED_MATRIX\n");
  fprintf(stderr, " STANDARD_MATRIX\n");
  fprintf(stderr, " ENHANCED_MATRIX\n");
  fprintf(stderr, " VENDOR_MATRIX\n");
  fprintf(stderr, " ALTERNATE_MATRIX\n");
  fprintf(stderr, " BRG\n");
  fprintf(stderr, " EXPORT\n"); // export and softproof are categories and will return NULL with dt_colorspaces_get_profile()
  fprintf(stderr, " SOFTPROOF\n");
  fprintf(stderr, " WORK\n");
  fprintf(stderr, " DISPLAY2\n");
  fprintf(stderr, " REC709\n");
  fprintf(stderr, " PROPHOTO_RGB\n");
  fprintf(stderr, " PQ_REC2020\n");
  fprintf(stderr, " HLG_REC2020\n");
  fprintf(stderr, " PQ_P3\n");
  fprintf(stderr, " HLG_P3\n");
}

#define ICC_FROM_STR(name) if(!strcmp(option, #name)) return DT_COLORSPACE_ ## name;
static dt_colorspaces_color_profile_type_t get_icc_type(const char* option)
{
  ICC_FROM_STR(NONE);
  ICC_FROM_STR(FILE);
  ICC_FROM_STR(SRGB);
  ICC_FROM_STR(ADOBERGB);
  ICC_FROM_STR(LIN_REC709);
  ICC_FROM_STR(LIN_REC2020);
  ICC_FROM_STR(XYZ);
  ICC_FROM_STR(LAB);
  ICC_FROM_STR(INFRARED);
  ICC_FROM_STR(DISPLAY);
  ICC_FROM_STR(EMBEDDED_ICC);
  ICC_FROM_STR(EMBEDDED_MATRIX);
  ICC_FROM_STR(STANDARD_MATRIX);
  ICC_FROM_STR(ENHANCED_MATRIX);
  ICC_FROM_STR(VENDOR_MATRIX);
  ICC_FROM_STR(ALTERNATE_MATRIX);
  ICC_FROM_STR(BRG);
  ICC_FROM_STR(EXPORT); // export and softproof are categories and will return NULL with dt_colorspaces_get_profile()
  ICC_FROM_STR(SOFTPROOF);
  ICC_FROM_STR(WORK);
  ICC_FROM_STR(DISPLAY2);
  ICC_FROM_STR(REC709);
  ICC_FROM_STR(PROPHOTO_RGB);
  ICC_FROM_STR(PQ_REC2020);
  ICC_FROM_STR(HLG_REC2020);
  ICC_FROM_STR(PQ_P3);
  ICC_FROM_STR(HLG_P3);
  return DT_COLORSPACE_LAST;
}
#undef ICC_FROM_STR

static void icc_intents()
{
  // TODO: Can this be automated to keep in sync with colorspaces.h?
  fprintf(stderr, "available ICC intents:\n");
  fprintf(stderr, " PERCEPTUAL\n");
  fprintf(stderr, " RELATIVE_COLORIMETRIC\n");
  fprintf(stderr, " SATURATION\n");
  fprintf(stderr, " ABSOLUTE_COLORIMETRIC\n");
}
#define ICC_INTENT_FROM_STR(name) if(!strcmp(option, #name)) return DT_INTENT_ ## name;
static dt_iop_color_intent_t get_icc_intent(const char* option)
{
  ICC_INTENT_FROM_STR(PERCEPTUAL);
  ICC_INTENT_FROM_STR(RELATIVE_COLORIMETRIC);
  ICC_INTENT_FROM_STR(SATURATION);
  ICC_INTENT_FROM_STR(ABSOLUTE_COLORIMETRIC);
  return DT_INTENT_LAST;
}
#undef ICC_INTENT_FROM_STR

int main(int argc, char *arg[])
{
#ifdef __APPLE__
  dt_osx_prepare_environment();
#endif

  // get valid locale dir
  dt_loc_init(NULL, NULL, NULL, NULL, NULL, NULL);
  char localedir[PATH_MAX] = { 0 };
  dt_loc_get_localedir(localedir, sizeof(localedir));
  bindtextdomain(GETTEXT_PACKAGE, localedir);

  if(!gtk_parse_args(&argc, &arg)) exit(1);

  // parse command line arguments
  char *input_filename = NULL;
  char *xmp_filename = NULL;
  gchar *output_filename = NULL;
  gchar *output_ext = NULL;
  char *style = NULL;
  int file_counter = 0;
  int width = 0, height = 0, bpp = 0;
  gboolean verbose = FALSE, high_quality = TRUE, upscale = FALSE,
           style_overwrite = FALSE, custom_presets = TRUE, export_masks = FALSE,
           output_to_dir = FALSE;

  GList* inputs = NULL;

  dt_colorspaces_color_profile_type_t icc_type = DT_COLORSPACE_NONE;
  gchar *icc_filename = NULL;
  dt_iop_color_intent_t icc_intent = DT_INTENT_LAST;

  int k;
  for(k = 1; k < argc; k++)
  {
    if(arg[k][0] == '-')
    {
      if(!strcmp(arg[k], "--help") || !strcmp(arg[k], "-h"))
      {
        usage(arg[0]);
        if(k+1 < argc) {
          if(!strcmp(arg[k+1], "icc-type"))
            icc_types();
          if(!strcmp(arg[k+1], "icc-intent"))
            icc_intents();
        }
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
        width = MAX(atoi(arg[k]), 0);
      }
      else if(!strcmp(arg[k], "--height") && argc > k + 1)
      {
        k++;
        height = MAX(atoi(arg[k]), 0);
      }
      else if(!strcmp(arg[k], "--bpp") && argc > k + 1)
      {
        k++;
        bpp = MAX(atoi(arg[k]), 0);
        fprintf(stderr, "%s %d\n",
                _("TODO: sorry, due to API restrictions we currently cannot set the BPP to"), bpp);
      }
      else if(!strcmp(arg[k], "--hq") && argc > k + 1)
      {
        k++;
        gchar *str = g_ascii_strup(arg[k], -1);
        if(!g_strcmp0(str, "0") || !g_strcmp0(str, "FALSE"))
          high_quality = FALSE;
        else if(!g_strcmp0(str, "1") || !g_strcmp0(str, "TRUE"))
          high_quality = TRUE;
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
          export_masks = FALSE;
        else if(!g_strcmp0(str, "1") || !g_strcmp0(str, "TRUE"))
          export_masks = TRUE;
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
          upscale = FALSE;
        else if(!g_strcmp0(str, "1") || !g_strcmp0(str, "TRUE"))
          upscale= TRUE;
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
        style = arg[k];
      }
      else if(!strcmp(arg[k], "--style-overwrite"))
      {
        style_overwrite = TRUE;
      }
      else if(!strcmp(arg[k], "--apply-custom-presets") && argc > k + 1)
      {
        k++;
        gchar *str = g_ascii_strup(arg[k], -1);
        if(!g_strcmp0(str, "0") || !g_strcmp0(str, "FALSE"))
          custom_presets = FALSE;
        else if(!g_strcmp0(str, "1") || !g_strcmp0(str, "TRUE"))
          custom_presets = TRUE;
        else
        {
          fprintf(stderr, "%s: %s\n", _("unknown option for --apply-custom-presets"), arg[k]);
          usage(arg[0]);
          exit(1);
        }
        g_free(str);
      }
      else if(!strcmp(arg[k], "--out-ext") && argc > k + 1)
      {
        k++;
        if(strlen(arg[k])> DT_MAX_OUTPUT_EXT_LENGTH)
        {
          fprintf(stderr, "%s: %s\n", _("too long ext for --out-ext"), arg[k]);
          usage(arg[0]);
          exit(1);
        }
        if(*arg[k] == '.')
        {
          //remove dot ;)
          arg[k]++;
        }
        output_ext = g_strdup(arg[k]);
      }
      else if(!strcmp(arg[k], "--import") && argc > k + 1)
      {
        k++;
        if(g_file_test(arg[k], G_FILE_TEST_EXISTS))
          inputs = g_list_prepend(inputs, g_strdup(arg[k]));
        else
          fprintf(stderr, _("notice: input file or dir '%s' doesn't exist, skipping\n"), arg[k]);
      }
      else if(!strcmp(arg[k], "--icc-type") && argc > k + 1)
      {
        k++;
        gchar *str = g_ascii_strup(arg[k], -1);
        icc_type = get_icc_type(str);
        g_free(str);
        if(icc_type >= DT_COLORSPACE_LAST){
          fprintf(stderr, _("incorrect ICC type for --icc-type: '%s'\n"), arg[k]);
          icc_types();
          usage(arg[0]);
          exit(1);
        }
      }
      else if(!strcmp(arg[k], "--icc-file") && argc > k + 1)
      {
        k++;
        if(g_file_test(arg[k], G_FILE_TEST_EXISTS) && ! g_file_test(arg[k], G_FILE_TEST_IS_DIR))
        {
          if(icc_filename)
            g_free(icc_filename);
          icc_filename = g_strdup(arg[k]);
        }
        else
          fprintf(stderr, _("notice: ICC file '%s' doesn't exist, skipping\n"), arg[k]);
      }
      else if(!strcmp(arg[k], "--icc-intent") && argc > k + 1)
      {
        k++;
        gchar *str = g_ascii_strup(arg[k], -1);
        icc_intent = get_icc_intent(str);
        g_free(str);
        if(icc_intent >= DT_INTENT_LAST){
          fprintf(stderr, _("incorrect ICC intent for --icc-intent: '%s'\n"), arg[k]);
          icc_intents();
          usage(arg[0]);
          exit(1);
        }
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
      else
      {
        fprintf(stderr, _("warning: unknown option '%s'\n"), arg[k]);
      }
    }
    else
    {
      if(file_counter == 0)
        input_filename = arg[k];
      else if(file_counter == 1)
        xmp_filename = arg[k];
      else if(file_counter == 2)
      {
        output_filename = g_strdup(arg[k]);
      }
      file_counter++;
    }
  }

  int m_argc = 0;
  char **m_arg = malloc(sizeof(char *) * (5 + argc - k + 1));
  m_arg[m_argc++] = "darktable-cli";
  m_arg[m_argc++] = "--library";
  m_arg[m_argc++] = ":memory:";
  m_arg[m_argc++] = "--conf";
  m_arg[m_argc++] = "write_sidecar_files=never";
  for(; k < argc; k++) m_arg[m_argc++] = arg[k];
  m_arg[m_argc] = NULL;

  if( (inputs && file_counter < 1) || (!inputs && file_counter < 2) || file_counter > 3)
  {
    usage(arg[0]);
    free(m_arg);
    if(output_filename)
      g_free(output_filename);
    if(output_ext)
      g_free(output_ext);
    if(inputs)
      g_list_free_full(inputs, g_free);
    exit(1);
  }
  else if(inputs && file_counter == 1)
  {
    //user specified inputs as options, and only dest is present
    if(output_filename)
      g_free(output_filename);
    output_filename = g_strdup(input_filename);
    input_filename = xmp_filename = NULL;
  }
  else if(inputs && file_counter == 2)
  {
    // inputs as options, xmp & output specified
    if(output_filename)
      g_free(output_filename);
    output_filename = g_strdup(xmp_filename);
    xmp_filename = input_filename;
    input_filename = NULL;
  }
  else if(inputs && file_counter == 3)
  {
    fprintf(stderr, _("error: input file and import opts specified! that's not supported!\n"));
    usage(arg[0]);
    free(m_arg);
    if(output_filename)
      g_free(output_filename);
    if(output_ext)
      g_free(output_ext);
    g_list_free_full(inputs, g_free);
    exit(1);
  }
  else if(file_counter == 2)
  {
    // assume no xmp file given
    if(output_filename)
      g_free(output_filename);
    output_filename = g_strdup(xmp_filename);
    xmp_filename = NULL;
  }

  if(!inputs && input_filename)
  {
    // input is present as param
    inputs = g_list_prepend(inputs, g_strdup(input_filename));
    input_filename = NULL;
  }

  if(g_file_test(output_filename, G_FILE_TEST_IS_DIR))
  {
    output_to_dir = TRUE;
    if(!output_ext)
    {
      output_ext = g_strdup("jpg");
    }
    fprintf(stderr, _("notice: output location is a directory. assuming '%s/$(FILE_NAME).%s' output pattern"), output_filename, output_ext);
    fprintf(stderr, "\n");
    gchar* temp_of = g_strdup(output_filename);
    g_free(output_filename);
    if(g_str_has_suffix(temp_of, "/"))
      temp_of[strlen(temp_of) - 1] = '\0';
    output_filename = g_strconcat(temp_of, "/$(FILE_NAME)", NULL);
    g_free(temp_of);
  }

  // the output file already exists, so there will be a sequence number added
  if(g_file_test(output_filename, G_FILE_TEST_EXISTS) && !output_to_dir)
  {
    if(!output_ext || (output_ext && g_str_has_suffix(output_filename, output_ext) && !g_strcmp0(output_ext,strrchr(output_filename, '.')+1))){
      //output file exists or there's output ext specified and it's same as file...
      fprintf(stderr, "%s\n", _("output file already exists, it will get renamed"));
    }
    //TODO: test if file with replaced ext exists
    // or not if we decide we don't replace file ext with output ext specified
  }

  // init dt without gui and without data.db:
  if(dt_init(m_argc, m_arg, FALSE, custom_presets, NULL))
  {
    free(m_arg);
    g_free(output_filename);
    if(output_ext)
      g_free(output_ext);
    if(inputs)
      g_list_free_full(inputs, g_free);
    exit(1);
  }

  GList *id_list = NULL;

  for(GList *l = inputs; l != NULL; l=g_list_next(l))
  {
    gchar* input = l->data;

    if(g_file_test(input, G_FILE_TEST_IS_DIR))
    {
      const int filmid = dt_film_import(input);
      if(!filmid)
      {
        // one of inputs was a failure, no prob
        fprintf(stderr, _("error: can't open folder %s"), input);
        fprintf(stderr, "\n");
        continue;
      }
      id_list = g_list_concat(id_list, dt_film_get_image_ids(filmid));
    }
    else
    {
      dt_film_t film;
      int filmid = 0;

      gchar *directory = g_path_get_dirname(input);
      filmid = dt_film_new(&film, directory);
      const int32_t id = dt_image_import(filmid, input, TRUE, TRUE);
      g_free(directory);
      if(!id)
      {
        fprintf(stderr, _("error: can't open file %s"), input);
        fprintf(stderr, "\n");
        continue;
      }
      id_list = g_list_append(id_list, GINT_TO_POINTER(id));
    }
  }

  //we no longer need inputs
  if(inputs)
      g_list_free_full(inputs, g_free);
  inputs = NULL;

  const int total = g_list_length(id_list);

  if(total == 0)
  {
    fprintf(stderr, _("no images to export, aborting\n"));
    free(m_arg);
    g_free(output_filename);
    if(output_ext)
      g_free(output_ext);
    exit(1);
  }

  // attach xmp, if requested:
  if(xmp_filename)
  {
    for(GList *iter = id_list; iter; iter = g_list_next(iter))
    {
      int id = GPOINTER_TO_INT(iter->data);
      dt_image_t *image = dt_image_cache_get(darktable.image_cache, id, 'w');
      if(dt_exif_xmp_read(image, xmp_filename, 1))
      {
        fprintf(stderr, _("error: can't open XMP file %s"), xmp_filename);
        fprintf(stderr, "\n");
        free(m_arg);
        g_free(output_filename);
        if(output_ext)
          g_free(output_ext);
        exit(1);
      }
      // don't write new xmp:
      dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_RELAXED);
    }
  }

  // print the history stack. only look at the first image and assume all got the same processing applied
  if(verbose)
  {
    int id = GPOINTER_TO_INT(id_list->data);
    gchar *history = dt_history_get_items_as_string(id);
    if(history)
      printf("%s\n", history);
    else
      printf("[%s]\n", _("empty history stack"));
  }

  if(!output_ext)
  {
    // by this point we're sure output is not dir, there's no output ext specified
    // so only place to look for it is in filename
    // try to find out the export format from the output_filename
    char *ext = strrchr(output_filename, '.');
    if(ext && strlen(ext) > DT_MAX_OUTPUT_EXT_LENGTH)
    {
      // too long ext, no point in wasting time
      fprintf(stderr, _("too long output file extension: %s\n"), ext);
      usage(arg[0]);
      g_free(output_filename);
      exit(1);
    }
    else if(!ext || strlen(ext) <= 1)
    {
      // no ext or empty ext, no point in wasting time
      fprintf(stderr, _("no output file extension given\n"));
      usage(arg[0]);
      g_free(output_filename);
      exit(1);
    }
    *ext = '\0';
    ext++;
    output_ext = g_strdup(ext);
  } else {
    // check and remove redundant file ext
    char *ext = strrchr(output_filename, '.');
    if(ext && !strcmp(output_ext, ext+1))
    {
      *ext = '\0';
    }
  }

  if(!strcmp(output_ext, "jpg"))
  {
    g_free(output_ext);
    output_ext = g_strdup("jpeg");
  }

  if(!strcmp(output_ext, "tif"))
  {
    g_free(output_ext);
    output_ext = g_strdup("tiff");
  }

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
    free(m_arg);
    g_free(output_filename);
    g_free(output_ext);
    exit(1);
  }

  sdata = storage->get_params(storage);
  if(sdata == NULL)
  {
    fprintf(stderr, "%s\n", _("failed to get parameters from storage module, aborting export ..."));
    free(m_arg);
    g_free(output_filename);
    g_free(output_ext);
    exit(1);
  }

  // and now for the really ugly hacks. don't tell your children about this one or they won't sleep at night
  // any longer ...
  g_strlcpy((char *)sdata, output_filename, DT_MAX_PATH_FOR_PARAMS);
  // all is good now, the last line didn't happen.
  g_free(output_filename);

  format = dt_imageio_get_format_by_name(output_ext);
  if(format == NULL)
  {
    fprintf(stderr, _("unknown extension '.%s'"), output_ext);
    fprintf(stderr, "\n");
    free(m_arg);
    g_free(output_ext);
    exit(1);
  }

  fdata = format->get_params(format);
  if(fdata == NULL)
  {
    fprintf(stderr, "%s\n", _("failed to get parameters from format module, aborting export ..."));
    free(m_arg);
    g_free(output_ext);
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
  fdata->style_append = 1; // make append the default and override with --style-overwrite

  if(style)
  {
    g_strlcpy((char *)fdata->style, style, DT_MAX_STYLE_NAME_LENGTH);
    fdata->style[127] = '\0';
    if(style_overwrite)
      fdata->style_append = 0;
  }

  if(storage->initialize_store)
  {
    storage->initialize_store(storage, sdata, &format, &fdata, &id_list, high_quality, upscale);

    format->set_params(format, fdata, format->params_size(format));
    storage->set_params(storage, sdata, storage->params_size(storage));
  }

  // TODO: add a callback to set the bpp without going through the config

  int num = 1, res = 0;
  for(GList *iter = id_list; iter; iter = g_list_next(iter), num++)
  {
    const int id = GPOINTER_TO_INT(iter->data);
    // TODO: have a parameter in command line to get the export presets
    dt_export_metadata_t metadata;
    metadata.flags = dt_lib_export_metadata_default_flags();
    metadata.list = NULL;
    if(storage->store(storage, sdata, id, format, fdata, num, total, high_quality, upscale, export_masks,
                      icc_type, icc_filename, icc_intent, &metadata) != 0)
      res = 1;
  }

  // cleanup time
  if(storage->finalize_store) storage->finalize_store(storage, sdata);
  storage->free_params(storage, sdata);
  format->free_params(format, fdata);
  g_list_free(id_list);

  if(icc_filename)
    g_free(icc_filename);

  dt_cleanup();

  free(m_arg);
  exit(res);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

