/*
    This file is part of darktable,
    Copyright (C) 2012-2025 darktable developers.

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

// Based on the original darktable-cli implementation by Johannes Hanika and contributors.
// This refactor aims to replicate existing functionality with improved maintainability.

#include "common/collection.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/exif.h"
#include "common/file_location.h"
#include "common/film.h"
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
#include <sys/time.h>
#include <unistd.h>

#ifdef __APPLE__
#include "osx/osx.h"
#endif

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

// Core initialization args. malloc/free lifecycle.
// TODO: Find a better home for these.
static int core_args_argc = 0;
static char **core_args_argv = NULL;

// Parse a directory.
static int scan_directory_for_images(const char *const restrict dir_path, GList **const restrict imports)
{
  GDir *const cdir = g_dir_open(dir_path, 0, NULL);
  if(!cdir)
  {
    fprintf(stderr, "Error: cannot read directory '%s'\n", dir_path);
    exit(1);
  }
  int file_count = 0;
  const gchar *fname;
  while((fname = g_dir_read_name(cdir)) != NULL)
  {
    if(fname[0] == '.') continue;
    gchar *const fullname = g_build_filename(dir_path, fname, NULL);
    if(!g_file_test(fullname, G_FILE_TEST_IS_DIR))
    {
      if(dt_supported_image(fullname))
      {
        *imports = g_list_append(*imports, g_strdup(fullname));
        file_count++;
      }
    }
    g_free(fullname);
  }
  g_dir_close(cdir);
  return file_count;
}

// Dynamically generate extension map from modules.  This is slower but hopefully robust.
static GHashTable *fetch_module_names(void)
{
  GHashTable *ext_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  dt_imageio_t *iio = darktable.imageio;

  for(GList *it = iio->plugins_format; it; it = g_list_next(it))
  {
    dt_imageio_module_format_t *module = it->data;
    dt_imageio_module_data_t *data = module->get_params(module);

    if(data)
    {
      const char *ext = module->extension(data);
      const char *name = module->name();
      // Check for valid extensions.
      if(ext && name && strlen(ext) > 0)
      {
        g_hash_table_insert(ext_map, g_strdup(ext), g_strdup(module->plugin_name));
      }

      module->free_params(module, data);
    }
  }

  return ext_map;
}

// From original version plus two.  Free form is the hard part.
static void show_help(const char *const restrict program_name)
{
  fprintf(stdout, "usage: %s <input> [<xmp>] <output> [options]\n", program_name);
  fprintf(stdout, "       %s --import <file/dir> [--import <file/dir>] ... <output> [options]\n", program_name);
  fprintf(stdout, "\nOptions:\n");
  fprintf(stdout, "  --help                       Show this help\n");
  fprintf(stdout, "  --version                    Show version\n");
  fprintf(stdout, "  --verbose                    Verbose output\n");
  fprintf(stdout, "  --width <pixels>             Output width\n");
  fprintf(stdout, "  --height <pixels>            Output height\n");
  fprintf(stdout, "  --bpp <8|16>                 Bits per pixel\n"); // Not implemented.
  fprintf(stdout, "  --hq <0|1 false|true>        High quality processing\n");
  fprintf(stdout, "  --upscale <0|1>              Allow upscaling\n");
  fprintf(stdout, "  --export-masks <0|1>         Export masks\n");
  fprintf(stdout, "  --out-ext <ext>              Output extension\n");
  fprintf(stdout, "  --style <name>               Apply style\n");
  fprintf(stdout, "  --style-overwrite <0|1>      Override builtin style\n"); // Does NOT overWRITE...
  fprintf(stdout, "  --apply-custom-presets <0|1> Apply custom presets\n");
  fprintf(stdout, "  --icc-type <type>            ICC profile type in Darktable database\n");
  fprintf(stdout, "  --icc-intent <intent>        ICC rendering intent\n");
  fprintf(stdout, "  --list-icc-types             List available ICC profile types\n");
  fprintf(stdout, "  --list-icc-intents           List available ICC rendering intents\n");
  fprintf(stdout, "  --icc-file <file>            Use custom ICC profile file instead of type\n");
}

// List ICC types for user.  Updating colorspaces.h's definition might really help this
//   and other uses in the codebase.  On the other hand, this IS faster.
static void list_icc_types(void)
{
  fprintf(stdout, "Available ICC profile types for export:\n\n");
  fprintf(stdout, "  %-15s  %s\n", "Name", "Description");
  fprintf(stdout, "  %-15s  %s\n", "----", "-----------");
  fprintf(stdout, "  %-15s  %s\n", "SRGB", "Standard RGB (IEC 61966-2-1) <-Default");
  fprintf(stdout, "  %-15s  %s\n", "ADOBERGB", "Adobe RGB (1998)");
  fprintf(stdout, "  %-15s  %s\n", "LIN_REC709", "Linear Rec. 709 RGB");
  fprintf(stdout, "  %-15s  %s\n", "LIN_REC2020", "Linear Rec. 2020 RGB");
  fprintf(stdout, "  %-15s  %s\n", "XYZ", "CIE XYZ color space");
  fprintf(stdout, "  %-15s  %s\n", "LAB", "CIE L*a*b* color space");
  fprintf(stdout, "  %-15s  %s\n", "REC709", "Rec. 709 RGB (gamma corrected)");
  fprintf(stdout, "  %-15s  %s\n", "PROPHOTO_RGB", "ProPhoto RGB / ROMM RGB");
  fprintf(stdout, "  %-15s  %s\n", "PQ_REC2020", "Rec. 2020 with PQ transfer");
  fprintf(stdout, "  %-15s  %s\n", "HLG_REC2020", "Rec. 2020 with HLG transfer");
  fprintf(stdout, "  %-15s  %s\n", "PQ_P3", "Display P3 with PQ transfer");
  fprintf(stdout, "  %-15s  %s\n", "HLG_P3", "Display P3 with HLG transfer");
  fprintf(stdout, "  %-15s  %s\n", "DISPLAY_P3", "Apple Display P3");
  fprintf(stdout, "\nFor custom ICC profiles, use --icc-file <path> instead of --icc-type\n");
}

// Output-capable ICC profiles (user-specifiable via --icc-type).
// Note: DT_COLORSPACE_FILE is set in parse_args for easy enforcement of mutual exclusivity.
static const struct
{
  const char *name;
  dt_colorspaces_color_profile_type_t type;
} output_profiles[] = { { "SRGB", DT_COLORSPACE_SRGB },
                        { "ADOBERGB", DT_COLORSPACE_ADOBERGB },
                        { "LIN_REC709", DT_COLORSPACE_LIN_REC709 },
                        { "LIN_REC2020", DT_COLORSPACE_LIN_REC2020 },
                        { "XYZ", DT_COLORSPACE_XYZ },
                        { "LAB", DT_COLORSPACE_LAB },
                        { "REC709", DT_COLORSPACE_REC709 },
                        { "PROPHOTO_RGB", DT_COLORSPACE_PROPHOTO_RGB },
                        { "PQ_REC2020", DT_COLORSPACE_PQ_REC2020 },
                        { "HLG_REC2020", DT_COLORSPACE_HLG_REC2020 },
                        { "PQ_P3", DT_COLORSPACE_PQ_P3 },
                        { "HLG_P3", DT_COLORSPACE_HLG_P3 },
                        { "DISPLAY_P3", DT_COLORSPACE_DISPLAY_P3 },
                        { NULL, 0 } };

// ICC string -> enum.
static dt_colorspaces_color_profile_type_t parse_icc_type(const char *input)
{
  for(int i = 0; output_profiles[i].name; i++)
  {
    if(!strcmp(input, output_profiles[i].name)) return output_profiles[i].type;
  }

  fprintf(stderr, "Error: Unknown ICC profile type '%s'.\n", input);
  fprintf(stderr, "Use --list-icc-types to see available profiles.\n");
  exit(1);
}

// TODO: Explain previous bug:
//  Rendering intent carryover in the following scenario:
//   File 1 has XMP-defined rendering intent, and File 2 has no XMP sidecar.
//   File 2's rendering intent ends up defined by File 1 rather than iop/colorout.c's default (DT_PERCEPTUAL).

// List available ICC rendering intents.
static void list_icc_intents(void)
{
  fprintf(stdout, "Available ICC rendering intents:\n\n");
  fprintf(stdout, "  PERCEPTUAL\n");
  fprintf(stdout, "  RELATIVE_COLORIMETRIC\n");
  fprintf(stdout, "  SATURATION\n");
  fprintf(stdout, "  ABSOLUTE_COLORIMETRIC\n");
}

// ICC intent string -> enum.
static dt_iop_color_intent_t parse_icc_intent(const char *input)
{
  gchar *upper = g_ascii_strup(input, -1);
  dt_iop_color_intent_t result;

  if(!strcmp(upper, "PERCEPTUAL"))
    result = DT_INTENT_PERCEPTUAL;
  else if(!strcmp(upper, "RELATIVE_COLORIMETRIC"))
    result = DT_INTENT_RELATIVE_COLORIMETRIC;
  else if(!strcmp(upper, "SATURATION"))
    result = DT_INTENT_SATURATION;
  else if(!strcmp(upper, "ABSOLUTE_COLORIMETRIC"))
    result = DT_INTENT_ABSOLUTE_COLORIMETRIC;
  else
  {
    fprintf(stderr, "Error: invalid ICC intent '%s'\n", input);
    fprintf(stderr, "Valid: PERCEPTUAL, RELATIVE_COLORIMETRIC, SATURATION, ABSOLUTE_COLORIMETRIC\n");
    g_free(upper);
    exit(1);
  }

  g_free(upper);
  return result;
}

// Configuration values parsed from CLI arguments.
typedef struct cli_config_t
{
  int width;
  int height;
  int bpp; // TODO: Implement as a convenience feature?  Users can pass --core --config...
  gboolean apply_custom_presets;
  gboolean hq;
  gboolean upscale;
  gboolean export_masks;
  gboolean style_overwrite;
  // String parameters point to argv.
  char *xmp_filename;
  char *style;
  char *icc_file;
  char *output_ext;

  // Enum parameters
  dt_colorspaces_color_profile_type_t icc_type;
  dt_iop_color_intent_t icc_intent;
} cli_config_t;

// Loose argument requirements require iterative evaluation of filename inputs.
static void parse_args(const int argc, char *const restrict argv[], char **output_filename, GList **inputs,
                       cli_config_t *config)
{
  int split_point = -1;
  GList *positional_args = NULL;

  // Parse-internal state
  gboolean has_import_args = FALSE;
  char *input_filename = NULL;
  int core_args_start = -1;

  // Split in one shot.
  for(int i = 1; i < argc; i++)
  {
    if(argv[i][0] == '-' && argv[i][1] == '-' && argv[i][2] != '\0')
    {
      split_point = i;
      break;
    }
  }

  // Positional arguments can appear in three different locations in argv:
  //   1. Before any named options: input.jpg output.jpg --width 100
  //   2. After named options: --width 100 input.jpg output.jpg
  //   3. Between named options: input.jpg --width 100 output.jpg
  //   All we know is the rightmost positional is always output.  This requires 2 stages.
  // Collect early positionals, stage 1.
  for(int i = 1; i < split_point && split_point != -1; i++)
  {
    const char *const arg = argv[i];
    if(arg[0] == '-')
    {
      fprintf(stderr, "Error: named argument '%s' in positional section\n", arg);
      g_list_free(positional_args);
      exit(1);
    }
    positional_args = g_list_append(positional_args, (gpointer)arg);
  }
  // No named options found => all positional.
  if(split_point == -1)
  {
    for(int i = 1; i < argc; i++)
    {
      positional_args = g_list_append(positional_args, (gpointer)argv[i]);
    }
  }
  // Named options are straightforward at this point. Stage 1b.
  for(int i = split_point; i < argc && split_point != -1; i++)
  {
    const char *const arg = argv[i];

    if(arg[0] == '-' && arg[1] != '\0')
    {
      if(!strcmp(arg, "--width") && i + 1 < argc)
      {
        config->width = atoi(argv[++i]);
      }
      else if(!strcmp(arg, "--height") && i + 1 < argc)
      {
        config->height = atoi(argv[++i]);
      }
      else if(!strcmp(arg, "--bpp") && i + 1 < argc)
      {
        config->bpp = atoi(argv[++i]);
      }
      else if(!strcmp(arg, "--hq") && i + 1 < argc)
      {
        i++;
        config->hq = (!strcmp(argv[i], "1") || !g_ascii_strcasecmp(argv[i], "true"));
      }
      else if(!strcmp(arg, "--upscale") && i + 1 < argc)
      {
        i++;
        config->upscale = (!strcmp(argv[i], "1") || !g_ascii_strcasecmp(argv[i], "true"));
      }
      else if(!strcmp(arg, "--export-masks") && i + 1 < argc)
      {
        i++;
        config->export_masks = (!strcmp(argv[i], "1") || !g_ascii_strcasecmp(argv[i], "true"));
      }
      else if(!strcmp(arg, "--out-ext") && i + 1 < argc)
      {
        config->output_ext = argv[++i];
      }
      else if(!strcmp(arg, "--style") && i + 1 < argc)
      {
        config->style = argv[++i];
      }
      else if(!strcmp(arg, "--style-overwrite"))
      {
        config->style_overwrite = TRUE;
      }
      else if(!strcmp(arg, "--icc-type") && i + 1 < argc)
      {
        config->icc_type = parse_icc_type(argv[++i]);
      }
      else if(!strcmp(arg, "--icc-file") && i + 1 < argc)
      {
        config->icc_file = argv[++i];
        if(!g_file_test(config->icc_file, G_FILE_TEST_EXISTS))
        {
          fprintf(stderr, "Error: ICC profile file '%s' does not exist\n", config->icc_file);
          exit(1);
        }
      }
      else if(!strcmp(arg, "--icc-intent") && i + 1 < argc)
      {
        config->icc_intent = parse_icc_intent(argv[++i]);
      }
      else if(!strcmp(arg, "--import") && i + 1 < argc)
      {
        *inputs = g_list_append(*inputs, g_strdup(argv[++i]));
        has_import_args = TRUE;
      }
      else if(!strcmp(arg, "--apply-custom-presets") && i + 1 < argc)
      {
        const char *const preset_arg = argv[++i];
        config->apply_custom_presets = (!strcmp(preset_arg, "1") || !g_ascii_strcasecmp(preset_arg, "true"));
      }
      else if(!strcmp(arg, "--core")) // Forward all remaining arguments to darktable core.
      {
        core_args_start = i + 1;
        break;
      }
      else
      {
        // This behavior is stricter than the original.
        fprintf(stderr, "Error: unknown option '%s'\n", arg);
        exit(1);
      }
    }
    else
    {
      // NOTE: Positional args can now appear after or between named options.
      // TODO: Test --core --config variants to be sure.
      positional_args = g_list_append(positional_args, (gpointer)arg);
    }
  }

  // Check --icc-type --icc-file exclusivity.
  // -1/NONE: User didn't specify --icc-type, so only non-NONE values will conflict.
  if(config->icc_file != NULL && config->icc_type != DT_COLORSPACE_NONE)
  {
    fprintf(stderr, "Error: Can not specify both --icc-file and --icc-type.\n");
    exit(1);
  }

  // Stage 2: Semantic interpretation.
  const int pos_count = g_list_length(positional_args);
  if(pos_count > 0)
  {
    // Rightmost positional argument is always output path.
    GList *const last = g_list_last(positional_args);
    *output_filename = g_strdup((char *)last->data);
    // Remaining is input and/or XMP.
    for(GList *iter = positional_args; iter != last; iter = iter->next)
    {
      const char *const arg = (char *)iter->data;
      const char *ext = strrchr(arg, '.');
      if(ext && g_ascii_strcasecmp(ext, ".xmp") == 0)
      {
        config->xmp_filename = (char *)arg;
      }
      else
      {
        if(!input_filename)
        {
          input_filename = (char *)arg;
        }
        else
        {
          // Multiple input case simply appends.
          *inputs = g_list_append(*inputs, g_strdup(arg));
        }
      }
    }
  }
  g_list_free(positional_args);

  // If --icc-file, set type to DT_COLORSPACE_FILE.  Otherwise, ICC default = SRGB.
  if(config->icc_file != NULL)
  {
    config->icc_type = DT_COLORSPACE_FILE;
  }
  else if(config->icc_type == DT_COLORSPACE_NONE)
  {
    config->icc_type = DT_COLORSPACE_SRGB;
  }

  // Validation.
  if(!*output_filename)
  {
    fprintf(stderr, "Error: output file required\n");
    exit(1);
  }
  // Input count.
  int input_count = (input_filename ? 1 : 0) + g_list_length(*inputs);
  if(input_count == 0)
  {
    fprintf(stderr, "Error: no input files\n");
    exit(1);
  }
  // Enforce exclusive use of positional inputs or --import.
  gboolean has_positional = (input_filename != NULL);
  if(has_positional && has_import_args)
  {
    fprintf(stderr, "Error: cannot mix positional arguments with --import\n");
    exit(1);
  }
  // Consolidate input_filename into inputs since we no longer need the flag.
  if(input_filename)
  {
    *inputs = g_list_prepend(*inputs, g_strdup(input_filename));
    input_filename = NULL;
  }
  // Actual directories get $(FILE_NAME) template injection.
  gboolean output_is_dir = g_file_test(*output_filename, G_FILE_TEST_IS_DIR);
  gboolean output_looks_like_dir = g_str_has_suffix(*output_filename, G_DIR_SEPARATOR_S);

  if(output_is_dir || output_looks_like_dir)
  {
    fprintf(stderr,
            "notice: output location is a directory. assuming '%s" G_DIR_SEPARATOR_S
            "$(FILE_NAME)' output pattern\n",
            *output_filename);
    // Inject $(FILE_NAME) template.
    gchar *temp = g_strdup(*output_filename);
    if(g_str_has_suffix(temp, G_DIR_SEPARATOR_S))
    {
      temp[strlen(temp) - 1] = '\0';
    }
    g_free(*output_filename);
    *output_filename = g_strconcat(temp, G_DIR_SEPARATOR_S "$(FILE_NAME)", NULL);
    g_free(temp);
  }

  else if(input_count > 1 && !strstr(*output_filename, "$("))
  {
    fprintf(stderr, "Error: multiple inputs require directory output or template pattern\n");
    exit(1);
  }

  // Ensure all inputs exist.
  // Note: Consider an override flag for some use cases.
  for(GList *iter = *inputs; iter; iter = iter->next)
  {
    const char *input_file = (char *)iter->data;
    if(!g_file_test(input_file, G_FILE_TEST_EXISTS))
    {
      fprintf(stderr, "Error: input file '%s' does not exist\n", input_file);
      exit(1);
    }
    // Reject empty directories and provide path rather than wait for "no images to export".
    if(g_file_test(input_file, G_FILE_TEST_IS_DIR))
    {
      GDir *dir = g_dir_open(input_file, 0, NULL);
      if(dir)
      {
        const gchar *first_file = g_dir_read_name(dir);
        g_dir_close(dir);
        if(!first_file)
        {
          fprintf(stderr, "Error: directory '%s' is empty\n", input_file);
          exit(1);
        }
      }
    }
  }
  // core_args_argv CLI defaults for dt_init.  Copied from original.
  // TODO: Verify these are still needed and perform their intended function.
  {
    int core_arg_count = (core_args_start != -1) ? argc - core_args_start : 0;
    core_args_argc = 5 + core_arg_count;
    core_args_argv = malloc(sizeof(char *) * (core_args_argc + 1));
    core_args_argv[0] = "darktable-cli";
    core_args_argv[1] = "--library";
    core_args_argv[2] = ":memory:";
    core_args_argv[3] = "--conf";
    core_args_argv[4] = "write_sidecar_files=never";
    // If --core is used, append user arguments. Otherwise, stick with these first five.
    if(core_args_start != -1)
    {
      for(int i = 0; i < core_arg_count; i++)
      {
        core_args_argv[5 + i] = argv[core_args_start + i];
      }
    }
    core_args_argv[core_args_argc] = NULL;
  }
}

// Load the dt database.
// TODO: * This approach creates considerable redundant SQL traffic.  Can we avoid?
static GList *cli_import_images(GList *file_list, const char *xmp_path, int *total_imported)
{
  GList *id_list = NULL;
  int count = 0;

  for(GList *iter = file_list; iter; iter = g_list_next(iter))
  {
    char *input_file = iter->data;
    count++;

    fprintf(stderr, "[%d] processing %s\n", count, input_file);

    // Create film for the directory containing the input file.
    // TODO: Update test for multiple input directory scenario.
    gchar *directory = g_path_get_dirname(input_file);
    // TODO: * Does an open film persist?  Just add to existing session?
    dt_film_t film;
    dt_filmid_t filmid = dt_film_new(&film, directory);

    if(!dt_is_valid_filmid(filmid))
    {
      fprintf(stderr, "Error: cannot open directory %s\n", directory);
      g_free(directory);
      continue;
    }
    g_free(directory);

    // Import and track ID.
    const dt_imgid_t imgid = dt_image_import(filmid, input_file, TRUE, TRUE);
    // TODO: * Likewise, can we dump the film's contents at the end of loading?
    if(!dt_is_valid_imgid(imgid))
    {
      fprintf(stderr, "Error: cannot import image %s\n", input_file);
      continue;
    }

    // Apply XMP if provided.
    if(xmp_path)
    {
      fprintf(stderr, "[%d] applying XMP %s\n", count, xmp_path);
      dt_image_t *image = dt_image_cache_get(imgid, 'w');
      if(image)
      {
        if(dt_exif_xmp_read(image, xmp_path, 1))
        {
          fprintf(stderr, "Warning: failed to read XMP file %s\n", xmp_path);
        }
        dt_image_cache_write_release(image, DT_IMAGE_CACHE_RELAXED);
      }
    }

    id_list = g_list_append(id_list, GINT_TO_POINTER(imgid));
  }

  if(total_imported) *total_imported = g_list_length(id_list);
  return id_list;
}

// Export from id_list.
// NOTE: To test rendering intents, use extreme ICC profiles or enable force_lcms2.
static int cli_export_images(GList *id_list, dt_imageio_module_storage_t *storage, dt_imageio_module_data_t *sdata,
                             dt_imageio_module_format_t *format, dt_imageio_module_data_t *fdata,
                             gboolean high_quality, gboolean allow_upscale, gboolean masks,
                             dt_colorspaces_color_profile_type_t icc_type, const char *icc_file,
                             dt_iop_color_intent_t icc_intent, int total_count)
{
  int export_errors = 0;
  int num = 1;

  for(GList *iter = id_list; iter; iter = g_list_next(iter), num++)
  {
    const int imgid = GPOINTER_TO_INT(iter->data);

    dt_export_metadata_t metadata;
    metadata.flags = dt_lib_export_metadata_default_flags();
    metadata.list = NULL;

    // storage->store is the only way to avoid the GUI and is more direct.
    // TODO: Since we're already hijacking, is there room for further streamlining?
    int export_result
        = storage->store(storage, sdata, imgid, format, fdata, num, total_count, high_quality, allow_upscale,
                         FALSE, 1.0, masks, icc_type, icc_file, icc_intent, &metadata);

    // Note: Module handles stdout message for success.
    if(export_result != 0)
    {
      fprintf(stderr, "Error: export failed for image ID %d\n", imgid);
      export_errors++;
    }
  }

  return export_errors;
}

// Shamelessly copied from src/main.c
#ifdef __APPLE__
int apple_main(int argc, char *argv[])
#else
int main(const int argc, char *const restrict argv[])
#endif
{
#ifdef __APPLE__
  dt_osx_prepare_environment();
#endif

  // Locale defaults.
  dt_loc_init(NULL, NULL, NULL, NULL, NULL, NULL);
  char localedir[PATH_MAX] = { 0 };
  dt_loc_get_localedir(localedir, sizeof(localedir));
  bindtextdomain(GETTEXT_PACKAGE, localedir);

  // Easy cases first: empty/help/version/lists.
  if(argc == 1)
  {
    show_help(argv[0]);
    exit(1);
  }

  for(int i = 1; i < argc; i++)
  {
    if(!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
    {
      show_help(argv[0]);
      exit(1); // For some reason, the original code returned 1 with --help.
    }
    else if(!strcmp(argv[i], "--version"))
    {
      fprintf(stdout, "darktable %s\n", darktable_package_version);
      exit(0);
    }
    else if(!strcmp(argv[i], "--list-icc-intents"))
    {
      list_icc_intents();
      exit(0);
    }
    else if(!strcmp(argv[i], "--list-icc-types"))
    {
      list_icc_types();
      exit(0);
    }
  }
  // Parse and cross fingers.
  char *output_filename = NULL;
  GList *inputs = NULL;
  // Config initialization: icc_type defaults to NONE (-1) to distinguish "not set" from explicit values.
  // The actual default (SRGB) is applied after parse_args() to avoid masking user input.
  cli_config_t config = {
    .hq = TRUE,
    .apply_custom_presets = TRUE,
    .icc_type = DT_COLORSPACE_NONE, // Must be explicit since NONE=-1, not 0
    .icc_intent = DT_INTENT_PERCEPTUAL,
    // Rest zero-initialized (FALSE/0/NULL)
  };
  parse_args(argc, argv, &output_filename, &inputs, &config);

  if(!inputs)
  {
    fprintf(stderr, "Error: no input files\n");
    exit(1);
  }

  // Darktable init.  Needed here since we use DT for input validation.
  // TODO: update test battery to verify --apply-custom-presets functionality.
  if(dt_init(core_args_argc, core_args_argv, FALSE, config.apply_custom_presets, NULL))
  {
    fprintf(stderr, "Error: failed to initialize darktable\n");
    exit(1);
  }

  // Extract image paths from directories.
  GList *image_files = NULL;
  for(GList *iter = inputs; iter; iter = g_list_next(iter))
  {
    char *const input = iter->data;

    if(g_file_test(input, G_FILE_TEST_IS_DIR))
    {
      const int file_count = scan_directory_for_images(input, &image_files);
      if(file_count < 0)
      {
        exit(1);
      }
      if(file_count == 0)
      {
        fprintf(stderr, "Warning: no supported image files found in directory '%s'\n", input);
      }
    }
    else
    {
      // Simple file list case.
      image_files = g_list_append(image_files, g_strdup(input));
    }
  }

  // Free original inputs, no longer needed.
  g_list_free_full(inputs, g_free);
  inputs = NULL;

  int total_files = g_list_length(image_files); // storage.store needs this for some reason.
  if(total_files == 0)
  {
    fprintf(stderr, "no images to export, aborting\n");
    exit(1);
  }

  // Decipher output extension: --out-ext -> extract from filename -> jpg default.
  if(config.output_ext == NULL)
  {
    // User didn't specify --out-ext.
    const char *ext_from_filename = strrchr(output_filename, '.');
    if(ext_from_filename && strlen(ext_from_filename) > 1)
    {
      // Extract LAST extension from filename.
      config.output_ext = (char *)(ext_from_filename + 1);
    }
    else
    {
      // Default to jpg.
      config.output_ext = "jpg";
    }
  }

  // Map file extension to darktable modules.  This is slow but hopefully robust.
  GHashTable *ext_map = fetch_module_names();
  gchar *ext_lower = g_ascii_strdown(config.output_ext, -1);
  const char *mapped = g_hash_table_lookup(ext_map, ext_lower);
  const char *format_name = mapped ? mapped : config.output_ext;
  g_free(ext_lower);

  dt_imageio_module_format_t *format = dt_imageio_get_format_by_name(format_name);
  if(!format)
  {
    fprintf(stderr, "Error: unknown extension '.%s'\n", config.output_ext);
    exit(1);
  }

  dt_imageio_module_storage_t *storage = dt_imageio_get_storage_by_name("disk");
  if(!storage)
  {
    fprintf(stderr, "Error: failed to get disk storage module\n");
    exit(1);
  }

  // Pull module names.  Obviously sensitive to upstream struct change.
  dt_imageio_module_data_t *sdata = storage->get_params(storage);
  dt_imageio_module_data_t *fdata = format->get_params(format);

  if(!sdata || !fdata)
  {
    fprintf(stderr, "Error: failed to initialize export parameters for format '%s'\n", format_name);
    if(!sdata) fprintf(stderr, "  - storage module ('disk') failed to provide parameters\n");
    if(!fdata) fprintf(stderr, "  - format module ('%s') failed to provide parameters\n", format_name);
    if(sdata) storage->free_params(storage, sdata);
    if(fdata) format->free_params(format, fdata);
    exit(1);
  }

  // TODO: get_params() is gui agnostic. set_params() has some gui dependencies,
  // dt_bauhaus_combobox being the most obvious. This would be nice to avoid.
  // Load output path into storage module.
  // Strip extension ONLY if we extracted it from filename (storage module will add it back)
  // If user specified --out-ext, keep full filename (allows double extensions like output.jpg.png)
  const char *ext_from_filename = strrchr(output_filename, '.');
  if(ext_from_filename && strlen(ext_from_filename) > 1 && strcmp(config.output_ext, (ext_from_filename + 1)) == 0)
  {
    // Strip extension since storage adds it back.
    gchar *output_without_ext = g_strdup(output_filename);
    gchar *last_dot = strrchr(output_without_ext, '.');
    if(last_dot) *last_dot = '\0';
    g_strlcpy((char *)sdata, output_without_ext, DT_MAX_PATH_FOR_PARAMS);
    g_free(output_without_ext);
  }
  else
  {
    // No extension in filename or --out-ext passed.  Use exact filename.
    g_strlcpy((char *)sdata, output_filename, DT_MAX_PATH_FOR_PARAMS); // output_filename rolls into sdata
  }
  fdata->max_width = config.width;
  fdata->max_height = config.height;
  // TODO: Update test for real-world styles and overrides.
  if(config.style)
  {
    g_strlcpy((char *)fdata->style, config.style, 128);
    fdata->style_append = config.style_overwrite ? 0 : 1;
  }

  // Import images and track ID's.
  int total_imported = 0;
  GList *id_list = cli_import_images(image_files, config.xmp_filename, &total_imported);
  // Dump id_list to stderr if multi-directory tests fail.
  if(!id_list)
  {
    fprintf(stderr, "Error: no images imported successfully\n");
    storage->free_params(storage, sdata);
    format->free_params(format, fdata);
    exit(1);
  }

  // Export. Finally.
  int export_errors
      = cli_export_images(id_list, storage, sdata, format, fdata, config.hq, config.upscale, config.export_masks,
                          config.icc_type, config.icc_file, config.icc_intent, total_files);

  if(export_errors > 0)
  {
    fprintf(stderr, "Warning: %d images failed to export\n", export_errors);
  }

  // Cleanup.
  if(storage->finalize_store) storage->finalize_store(storage, sdata);
  storage->free_params(storage, sdata);
  format->free_params(format, fdata);
  g_list_free(id_list);

  if(core_args_argv) free(core_args_argv);
  if(ext_map) g_hash_table_destroy(ext_map);
  dt_cleanup();
  g_list_free_full(image_files, g_free);
  g_free(output_filename);
  return 0;
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
