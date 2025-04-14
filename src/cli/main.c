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
#include <limits.h>
#include <stdlib.h>
#include <string.h>
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
fprintf(stderr, "darktable %s\n"
                "Copyright (C) 2012-%s Johannes Hanika and other contributors.\n"
                "\n"
                "<https://www.darktable.org>\n"
                "darktable is an open source photography workflow application and\n"
                "non-destructive raw developer for photographers.\n"
                "GNU GPL version 3 or later <https://gnu.org/licenses/gpl.html>\n"
                "This is free software: you are free to change and redistribute it.\n"
                "There is NO WARRANTY, to the extent permitted by law.\n\n"
                "Usage:\n"
                "\n"
                "  darktable-cli [IMAGE_FILE | IMAGE_FOLDER]\n"
                "                [XMP_FILE] DIR [OPTIONS]\n"
                "                [--core DARKTABLE_OPTIONS]\n"
                "\n"
                "Options:\n"
                "   --apply-custom-presets <0|1|false|true>, default: true\n"
                "                          disable for multiple instances\n"
                "   --bpp <bpp>, unsupported\n"
                "   --export_masks <0|1|false|true>, default: false\n"

                "   --height <max height> default: 0 = full resolution\n"
                "   --width <max width> default: 0 = full resolution\n"

                "   --hq <0|1|false|true> default: true\n"
                "   --upscale <0|1|false|true>, default: false\n"
                "   --style <style name>\n"
                "   --style-overwrite\n"
                "   --out-ext <extension>, default from output destination or '.jpg'\n"
                "                          if specified, takes preference over output\n"
                "   --import <file or dir> specify input file or dir, can be used'\n"
                "                          multiple times instead of input file\n"
                "   --icc-type <type> specify icc type, default to NONE\n"
                "                     use --help icc-type for list of supported types\n"
                "   --icc-file <file> specify icc filename, default to NONE\n"
                "   --icc-intent <intent> specify icc intent, default to LAST\n"
                "                     use --help icc-intent for list of supported intents\n"
                "   --verbose\n"
                "   -h, --help [option]\n"
                "   -v, --version\n",
                darktable_package_version,
                darktable_last_commit_year);

  // FS TODO: to add instructions for DARKTABLE_OPTIONS
}

static void icc_types(void)
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
  fprintf(stderr, " DISPLAY_P3\n");
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
  ICC_FROM_STR(DISPLAY_P3);
  return DT_COLORSPACE_LAST;
}
#undef ICC_FROM_STR

static void icc_intents(void)
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


typedef struct _options_t
{
  int last_arg_pos;
  char *input_filename;
  char *xmp_filename;
  gchar *output_filename;
  gchar *output_ext;
  char *style;
  gboolean style_overwrite;
  int width;
  int height;
  int bpp;
  gboolean verbose;
  gboolean high_quality;
  gboolean upscale;
  gboolean custom_presets;
  gboolean export_masks;
  GList *imports;
  dt_colorspaces_color_profile_type_t icc_type;
  gchar *icc_filename;
  dt_iop_color_intent_t icc_intent;
} options_t;


static options_t *parse_options(int argc, char *arg[])
{
  options_t *opts = g_malloc0(sizeof(options_t));

  // Set non-zero defaults
  opts->high_quality = TRUE;
  opts->custom_presets = TRUE;
  opts->icc_type = DT_COLORSPACE_NONE;
  opts->icc_intent = DT_INTENT_LAST;

  int file_counter = 0;
  // Use GList for --import values. Caller will free these.
  opts->imports = NULL;

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
      else if(!strcmp(arg[k], "--version") || !strcmp(arg[k], "-v"))
      {
          printf("darktable %s\nCopyright (C) 2012-%s Johannes Hanika and other contributors.\n\n",darktable_package_version, darktable_last_commit_year);
          printf("See %s for detailed documentation.\n", PACKAGE_DOCS);
          printf("See %s to report bugs.\n",PACKAGE_BUGREPORT);
          exit(0);
      }
      else if(!strcmp(arg[k], "--width") && argc > k + 1)
      {
        k++;
        opts->width = MAX(atoi(arg[k]), 0);
      }
      else if(!strcmp(arg[k], "--height") && argc > k + 1)
      {
        k++;
        opts->height = MAX(atoi(arg[k]), 0);
      }
      else if(!strcmp(arg[k], "--bpp") && argc > k + 1)
      {
        k++;
        opts->bpp = MAX(atoi(arg[k]), 0);
        fprintf(stderr, "%s %d\n", _("TODO: sorry, due to API restrictions we currently cannot set the BPP to"),
                opts->bpp);
      }
      else if(!strcmp(arg[k], "--hq") && argc > k + 1)
      {
        k++;
        gchar *str = g_ascii_strup(arg[k], -1);
        if(!g_strcmp0(str, "0") || !g_strcmp0(str, "FALSE"))
          opts->high_quality = FALSE;
        else if(!g_strcmp0(str, "1") || !g_strcmp0(str, "TRUE"))
          opts->high_quality = TRUE;
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
          opts->export_masks = FALSE;
        else if(!g_strcmp0(str, "1") || !g_strcmp0(str, "TRUE"))
          opts->export_masks = TRUE;
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
          opts->upscale = FALSE;
        else if(!g_strcmp0(str, "1") || !g_strcmp0(str, "TRUE"))
          opts->upscale = TRUE;
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
        opts->style = arg[k];
      }
      else if(!strcmp(arg[k], "--style-overwrite"))
        opts->style_overwrite = TRUE;
      else if(!strcmp(arg[k], "--apply-custom-presets") && argc > k + 1)
      {
        k++;
        gchar *str = g_ascii_strup(arg[k], -1);
        if(!g_strcmp0(str, "0") || !g_strcmp0(str, "FALSE"))
          opts->custom_presets = FALSE;
        else if(!g_strcmp0(str, "1") || !g_strcmp0(str, "TRUE"))
          opts->custom_presets = TRUE;
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
        if(strlen(arg[k]) > DT_MAX_OUTPUT_EXT_LENGTH)
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
        opts->output_ext = g_strdup(arg[k]);
      }
      else if(!strcmp(arg[k], "--import") && argc > k + 1)
      {
        k++;
        if(g_file_test(arg[k], G_FILE_TEST_EXISTS))
          opts->imports = g_list_prepend(opts->imports, g_strdup(arg[k]));
        else
        {
          fprintf(stderr, _("notice: input file or dir '%s' doesn't exist, skipping\n"), arg[k]);
        }
      }
      else if(!strcmp(arg[k], "--icc-type") && argc > k + 1)
      {
        k++;
        gchar *str = g_ascii_strup(arg[k], -1);
        opts->icc_type = get_icc_type(str);
        g_free(str);
        if(opts->icc_type >= DT_COLORSPACE_LAST)
        {
          fprintf(stderr, _("incorrect ICC type for --icc-type: '%s'\n"), arg[k]);
          icc_types();
          usage(arg[0]);
          exit(1);
        }
      }
      else if(!strcmp(arg[k], "--icc-file") && argc > k + 1)
      {
        k++;
        if(g_file_test(arg[k], G_FILE_TEST_EXISTS) && !g_file_test(arg[k], G_FILE_TEST_IS_DIR))
        {
          if(opts->icc_filename)
            g_free(opts->icc_filename);
          opts->icc_filename = g_strdup(arg[k]);
        }
        else
        {
          fprintf(stderr, _("notice: ICC file '%s' doesn't exist, skipping\n"), arg[k]);
        }
      }
      else if(!strcmp(arg[k], "--icc-intent") && argc > k + 1)
      {
        k++;
        gchar *str = g_ascii_strup(arg[k], -1);
        opts->icc_intent = get_icc_intent(str);
        g_free(str);
        if(opts->icc_intent >= DT_INTENT_LAST)
        {
          fprintf(stderr, _("incorrect ICC intent for --icc-intent: '%s'\n"), arg[k]);
          icc_intents();
          usage(arg[0]);
          exit(1);
        }
      }
      else if(!strcmp(arg[k], "-v") || !strcmp(arg[k], "--verbose"))
        opts->verbose = TRUE;
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
      // Positional arguments. They are processed in order.
      switch(file_counter)
      {
        case 0:
          opts->input_filename = arg[k];
          break;
        case 1:
          opts->xmp_filename = arg[k];
          break;
        case 2:
          opts->output_filename = g_strdup(arg[k]);
          break;
        default:
          break;
      }
      file_counter++;
    }
  }

  opts->last_arg_pos = k;

  if((opts->imports && file_counter < 1) || (!opts->imports && file_counter < 2) || file_counter > 3)
  {
    usage(arg[0]);
    exit(1);
  }
  else if(opts->imports && file_counter == 1)
  {
    if(opts->output_filename) g_free(opts->output_filename);
    opts->output_filename = g_strdup(opts->input_filename);
    opts->input_filename = opts->xmp_filename = NULL;
  }
  else if(opts->imports && file_counter == 2)
  {
    if(opts->output_filename) g_free(opts->output_filename);
    opts->output_filename = g_strdup(opts->xmp_filename);
    opts->xmp_filename = opts->input_filename;
    opts->input_filename = NULL;
  }
  else if(opts->imports && file_counter == 3)
  {
    fprintf(stderr, _("error: input file and import opts specified! that's not supported!\n"));
    usage(arg[0]);
    exit(1);
  }
  else if(file_counter == 2)
  {
    /* Assume no XMP file provided */
    if(opts->output_filename) g_free(opts->output_filename);
    opts->output_filename = g_strdup(opts->xmp_filename);
    opts->xmp_filename = NULL;
  }

  if(!opts->imports && opts->input_filename)
  {
    opts->imports = g_list_prepend(opts->imports, g_strdup(opts->input_filename));
    opts->input_filename = NULL;
  }

  /* Handle output filename if it is a directory */
  if(g_file_test(opts->output_filename, G_FILE_TEST_IS_DIR))
  {
    if(!opts->output_ext)
    {
      opts->output_ext = g_strdup("jpg");
    }
    fprintf(stderr, _("notice: output location is a directory. assuming '%s/$(FILE_NAME).%s' output pattern\n"),
            opts->output_filename, opts->output_ext);
    gchar *temp_of = g_strdup(opts->output_filename);
    g_free(opts->output_filename);
    if(g_str_has_suffix(temp_of, "/")) temp_of[strlen(temp_of) - 1] = '\0';
    opts->output_filename = g_strconcat(temp_of, "/$(FILE_NAME)", NULL);
    g_free(temp_of);
  }
  return opts;
}

static int process_images(options_t *opts)
{
  GList *id_list = NULL;
  // Process each input (dir or file)
  for(GList *l = opts->imports; l != NULL; l = g_list_next(l))
  {
    gchar *input = l->data;
    if(g_file_test(input, G_FILE_TEST_IS_DIR))
    {
      const dt_filmid_t filmid = dt_film_import(input);
      if(!filmid)
      {
        fprintf(stderr, _("error: can't open folder %s\n"), input);
        continue;
      }
      id_list = g_list_concat(id_list, dt_film_get_image_ids(filmid));
    }
    else
    {
      dt_film_t film;
      dt_filmid_t filmid = NO_FILMID;
      gchar *directory = g_path_get_dirname(input);
      filmid = dt_film_new(&film, directory);
      const dt_imgid_t id = dt_image_import(filmid, input, TRUE, TRUE);
      g_free(directory);
      if(!dt_is_valid_imgid(id))
      {
        fprintf(stderr, _("error: can't open file %s\n"), input);
        continue;
      }
      id_list = g_list_append(id_list, GINT_TO_POINTER(id));
    }
  }
  g_list_free_full(opts->imports, g_free);
  opts->imports = NULL;

  int total = g_list_length(id_list);
  if(total == 0)
  {
    fprintf(stderr, _("no images to export, aborting\n"));
    return 1;
  }

  // Attach XMP if requested
  if(opts->xmp_filename)
  {
    for(GList *iter = id_list; iter; iter = g_list_next(iter))
    {
      int id = GPOINTER_TO_INT(iter->data);
      dt_image_t *image = dt_image_cache_get(id, 'w');
      if(dt_exif_xmp_read(image, opts->xmp_filename, 1))
      {
        fprintf(stderr, _("error: can't open XMP file %s\n"), opts->xmp_filename);
        return 1;
      }
      dt_image_cache_write_release(image, DT_IMAGE_CACHE_RELAXED);
    }
  }

  // Display history if verbose is set (using first image as reference)
  if(opts->verbose)
  {
    int first_id = GPOINTER_TO_INT(id_list->data);
    gchar *history = dt_history_get_items_as_string(first_id);
    if(history)
      printf("%s\n", history);
    else
      printf("[%s]\n", _("empty history stack"));
  }

  /* Determine output extension from opts->output_filename if not already set */
  if(!opts->output_ext)
  {
    char *ext = strrchr(opts->output_filename, '.');
    if(ext && strlen(ext) > DT_MAX_OUTPUT_EXT_LENGTH)
    {
      fprintf(stderr, _("too long output file extension: %s\n"), ext);
      return 1;
    }
    else if(!ext || strlen(ext) <= 1)
    {
      fprintf(stderr, _("no output file extension given\n"));
      return 1;
    }
    *ext = '\0';
    ext++;
    opts->output_ext = g_strdup(ext);
  }
  else
  {
    /* Remove redundant file ext if needed */
    char *ext = strrchr(opts->output_filename, '.');
    if(ext && !strcmp(opts->output_ext, ext + 1)) *ext = '\0';
  }

  /* Normalize common file extension differences */
  if(!strcmp(opts->output_ext, "jpg"))
  {
    g_free(opts->output_ext);
    opts->output_ext = g_strdup("jpeg");
  }
  if(!strcmp(opts->output_ext, "tif"))
  {
    g_free(opts->output_ext);
    opts->output_ext = g_strdup("tiff");
  }
  if(!strcmp(opts->output_ext, "jxl"))
  {
    g_free(opts->output_ext);
    opts->output_ext = g_strdup("jpegxl");
  }

  /* Initialize storage and format modules */
  dt_imageio_module_storage_t *storage = dt_imageio_get_storage_by_name("disk");
  if(storage == NULL)
  {
    fprintf(stderr, "%s\n",
            _("cannot find disk storage module. please check your installation, something seems to be broken."));
    return 1;
  }
  dt_imageio_module_data_t *sdata = storage->get_params(storage);
  if(sdata == NULL)
  {
    fprintf(stderr, "%s\n", _("failed to get parameters from storage module, aborting export ..."));
    return 1;
  }
  g_strlcpy((char *)sdata, opts->output_filename, DT_MAX_PATH_FOR_PARAMS);
  g_free(opts->output_filename);

  dt_imageio_module_format_t *format = dt_imageio_get_format_by_name(opts->output_ext);
  if(format == NULL)
  {
    fprintf(stderr, _("unknown extension '.%s'\n"), opts->output_ext);
    return 1;
  }
  dt_imageio_module_data_t *fdata = format->get_params(format);
  if(fdata == NULL)
  {
    fprintf(stderr, "%s\n", _("failed to get parameters from format module, aborting export ..."));
    return 1;
  }

  /* Adjust dimensions */
  uint32_t sw = 0, sh = 0, fw = 0, fh = 0, w = 0, h = 0;
  storage->dimension(storage, sdata, &sw, &sh);
  format->dimension(format, fdata, &fw, &fh);
  w = (sw == 0 || fw == 0) ? (sw > fw ? sw : fw) : (sw < fw ? sw : fw);
  h = (sh == 0 || fh == 0) ? (sh > fh ? sh : fh) : (sh < fh ? sh : fh);
  fdata->max_width = (w != 0 && opts->width > w) ? w : opts->width;
  fdata->max_height = (h != 0 && opts->height > h) ? h : opts->height;
  fdata->style[0] = '\0';
  fdata->style_append = 1; // default is to append

  if(opts->style)
  {
    g_strlcpy((char *)fdata->style, opts->style, DT_MAX_STYLE_NAME_LENGTH);
    fdata->style[DT_MAX_STYLE_NAME_LENGTH - 1] = '\0';
    if(opts->style_overwrite) fdata->style_append = 0;
  }

  if(storage->initialize_store)
  {
    storage->initialize_store(storage, sdata, &format, &fdata, &id_list, opts->high_quality, opts->upscale);
    format->set_params(format, fdata, format->params_size(format));
    storage->set_params(storage, sdata, storage->params_size(storage));
  }

  /* Loop through each image and export */
  int num = 1, res = 0;
  for(GList *iter = id_list; iter; iter = g_list_next(iter), num++)
  {
    const int id = GPOINTER_TO_INT(iter->data);
    dt_export_metadata_t metadata;
    metadata.flags = dt_lib_export_metadata_default_flags();
    metadata.list = NULL;
    if(storage->store(storage, sdata, id, format, fdata, num, total, opts->high_quality, opts->upscale,
                      opts->export_masks, opts->icc_type, opts->icc_filename, opts->icc_intent, &metadata)
       != 0)
      res = 1;
  }

  if(storage->finalize_store) storage->finalize_store(storage, sdata);
  storage->free_params(storage, sdata);
  format->free_params(format, fdata);
  g_list_free(id_list);

  if(opts->icc_filename)
    g_free(opts->icc_filename);

  return res;
}

int main(int argc, char *argv[])
{
#ifdef __APPLE__
  dt_osx_prepare_environment();
#endif

  dt_loc_init(NULL, NULL, NULL, NULL, NULL, NULL);
  char localedir[PATH_MAX] = { 0 };
  dt_loc_get_localedir(localedir, sizeof(localedir));
  bindtextdomain(GETTEXT_PACKAGE, localedir);

  if(!gtk_parse_args(&argc, &argv)) exit(1);

  options_t *opts = parse_options(argc, argv);

  int m_argc = 0;
  char **m_arg = malloc(sizeof(char *) * (5 + argc - opts->last_arg_pos + 1));
  m_arg[m_argc++] = "darktable-cli";
  m_arg[m_argc++] = "--library";
  m_arg[m_argc++] = ":memory:";
  m_arg[m_argc++] = "--conf";
  m_arg[m_argc++] = "write_sidecar_files=never";
  for(; opts->last_arg_pos < argc; opts->last_arg_pos++) m_arg[m_argc++] = argv[opts->last_arg_pos];
  m_arg[m_argc] = NULL;

  if(dt_init(m_argc, m_arg, FALSE, opts->custom_presets, NULL))
  {
    free(m_arg);
    exit(1);
  }

  int res = process_images(opts);

  dt_cleanup();
  free(m_arg);
  g_free(opts->output_ext);
  free(opts);

  exit(res);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
