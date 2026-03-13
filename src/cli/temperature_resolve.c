/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/film.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"

#include <lcms2.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "external/cie_colorimetric_tables.c"

#define INITIALBLACKBODYTEMPERATURE 4000
#define DT_IOP_LOWEST_TEMPERATURE 1901.0
#define DT_IOP_HIGHEST_TEMPERATURE 25000.0
#define DT_IOP_LOWEST_TINT 0.135
#define DT_IOP_HIGHEST_TINT 2.326
#define DT_IOP_TEMP_USER 2

static void usage(const char *progname)
{
  fprintf(stderr,
          "Usage:\n"
          "  %s IMAGE_FILE TEMPERATURE_K TINT [--core DARKTABLE_OPTIONS]\n"
          "\n"
          "Outputs JSON with the image-specific white-balance multipliers that\n"
          "darktable's temperature module would store for the requested\n"
          "temperature+tint.\n",
          progname);
}

static double clamp_double(const double value, const double min, const double max)
{
  return value < min ? min : (value > max ? max : value);
}

typedef double((*spd_fn)(unsigned long int wavelength, double TempK));

static double _spd_blackbody(unsigned long int wavelength, double TempK)
{
  const long double lambda = (double)wavelength * 1e-9;

#define c1 3.7417715246641281639549488324352159753e-16L
#define c2 0.014387769599838156481252937624049081933L

  return (double)(c1 / (powl(lambda, 5) * (expl(c2 / (lambda * TempK)) - 1.0L)));

#undef c2
#undef c1
}

static double _spd_daylight(unsigned long int wavelength, double TempK)
{
  cmsCIExyY white_point = { D65xyY.x, D65xyY.y, 1.0 };
  cmsWhitePointFromTemp(&white_point, TempK);

  const double M = (0.0241 + 0.2562 * white_point.x - 0.7341 * white_point.y);
  const double m1 = (-1.3515 - 1.7703 * white_point.x + 5.9114 * white_point.y) / M;
  const double m2 = (0.0300 - 31.4424 * white_point.x + 30.0717 * white_point.y) / M;

  const unsigned long int j = (wavelength - cie_daylight_components[0].wavelength)
                              / (cie_daylight_components[1].wavelength
                                 - cie_daylight_components[0].wavelength);

  return cie_daylight_components[j].S[0]
         + m1 * cie_daylight_components[j].S[1]
         + m2 * cie_daylight_components[j].S[2];
}

static cmsCIEXYZ _spectrum_to_XYZ(double TempK, spd_fn spd)
{
  cmsCIEXYZ source = { 0.0, 0.0, 0.0 };

  for(size_t i = 0; i < cie_1931_std_colorimetric_observer_count; i++)
  {
    const unsigned long int lambda = cie_1931_std_colorimetric_observer[0].wavelength
                                     + (cie_1931_std_colorimetric_observer[1].wavelength
                                        - cie_1931_std_colorimetric_observer[0].wavelength)
                                         * i;
    const double power = spd(lambda, TempK);
    source.X += power * cie_1931_std_colorimetric_observer[i].xyz.X;
    source.Y += power * cie_1931_std_colorimetric_observer[i].xyz.Y;
    source.Z += power * cie_1931_std_colorimetric_observer[i].xyz.Z;
  }

  const double max_component = fmax(fmax(source.X, source.Y), source.Z);
  source.X /= max_component;
  source.Y /= max_component;
  source.Z /= max_component;

  return source;
}

static cmsCIEXYZ _temperature_to_XYZ(double temperature)
{
  temperature = clamp_double(temperature, DT_IOP_LOWEST_TEMPERATURE, DT_IOP_HIGHEST_TEMPERATURE);
  if(temperature < INITIALBLACKBODYTEMPERATURE)
    return _spectrum_to_XYZ(temperature, _spd_blackbody);
  return _spectrum_to_XYZ(temperature, _spd_daylight);
}

static int prepare_matrices(const dt_image_t *img, double XYZ_to_CAM[4][3])
{
  const double XYZ_to_RGB[4][3] = {
    { 3.2404542, -1.5371385, -0.4985314 },
    { -0.9692660, 1.8760108, 0.0415560 },
    { 0.0556434, -0.2040259, 1.0572252 },
    { 0.0, 0.0, 0.0 }
  };
  double CAM_to_XYZ[3][4];

  if(!dt_image_is_raw(img))
  {
    memcpy(XYZ_to_CAM, XYZ_to_RGB, sizeof(XYZ_to_RGB));
    return 1;
  }

  return dt_colorspaces_conversion_matrices_xyz(img->adobe_XYZ_to_CAM,
                                                (float *)img->d65_color_matrix,
                                                XYZ_to_CAM,
                                                CAM_to_XYZ);
}

static int resolve_temp_tint(const dt_image_t *img,
                             const double temperature,
                             const double tint,
                             double mul[4])
{
  double XYZ_to_CAM[4][3];
  if(!prepare_matrices(img, XYZ_to_CAM)) return 0;

  cmsCIEXYZ xyz = _temperature_to_XYZ(temperature);
  xyz.Y /= clamp_double(tint, DT_IOP_LOWEST_TINT, DT_IOP_HIGHEST_TINT);

  const double XYZ[3] = { xyz.X, xyz.Y, xyz.Z };
  const int channels = (img->flags & DT_IMAGE_4BAYER) ? 4 : 3;
  for(int k = 0; k < channels; k++)
  {
    double cam = 0.0;
    for(int i = 0; i < 3; i++) cam += XYZ_to_CAM[k][i] * XYZ[i];
    if(cam == 0.0) return 0;
    mul[k] = 1.0 / cam;
  }

  if(channels == 3) mul[3] = 1.0;

  if(mul[1] == 0.0) return 0;
  mul[0] /= mul[1];
  mul[2] /= mul[1];
  mul[3] /= mul[1];
  mul[1] = 1.0;
  return 1;
}

int main(int argc, char *argv[])
{
  if(argc < 2 || !strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))
  {
    usage(argv[0]);
    return argc < 2 ? EXIT_FAILURE : EXIT_SUCCESS;
  }

  if(argc < 4)
  {
    usage(argv[0]);
    return EXIT_FAILURE;
  }

  const char *image_path = argv[1];
  char *endptr = NULL;
  const double requested_temperature = g_ascii_strtod(argv[2], &endptr);
  if(endptr == argv[2] || *endptr != '\0')
  {
    fprintf(stderr, "invalid temperature: %s\n", argv[2]);
    return EXIT_FAILURE;
  }

  endptr = NULL;
  const double requested_tint = g_ascii_strtod(argv[3], &endptr);
  if(endptr == argv[3] || *endptr != '\0')
  {
    fprintf(stderr, "invalid tint: %s\n", argv[3]);
    return EXIT_FAILURE;
  }

  int core_index = 4;
  if(argc > 4)
  {
    if(strcmp(argv[4], "--core"))
    {
      usage(argv[0]);
      return EXIT_FAILURE;
    }
    core_index = 5;
  }

  int dt_argc = 0;
  gchar *sandbox_dir = g_dir_make_tmp("darktable-wb-resolve-XXXXXX", NULL);
  if(sandbox_dir == NULL)
  {
    fprintf(stderr, "failed to create temporary config directory\n");
    return EXIT_FAILURE;
  }

  gchar *config_dir = g_build_filename(sandbox_dir, "config", NULL);
  gchar *cache_dir = g_build_filename(sandbox_dir, "cache", NULL);
  gchar *tmp_dir = g_build_filename(sandbox_dir, "tmp", NULL);
  if(g_mkdir_with_parents(config_dir, 0700)
     || g_mkdir_with_parents(cache_dir, 0700)
     || g_mkdir_with_parents(tmp_dir, 0700))
  {
    fprintf(stderr, "failed to create temporary darktable directories\n");
    g_free(tmp_dir);
    g_free(cache_dir);
    g_free(config_dir);
    g_free(sandbox_dir);
    return EXIT_FAILURE;
  }

  char **dt_argv = malloc(sizeof(char *) * (11 + argc - core_index + 1));
  if(dt_argv == NULL)
  {
    fprintf(stderr, "out of memory\n");
    g_free(tmp_dir);
    g_free(cache_dir);
    g_free(config_dir);
    g_free(sandbox_dir);
    return EXIT_FAILURE;
  }

  dt_argv[dt_argc++] = "darktable-wb-resolve";
  dt_argv[dt_argc++] = "--library";
  dt_argv[dt_argc++] = ":memory:";
  dt_argv[dt_argc++] = "--configdir";
  dt_argv[dt_argc++] = config_dir;
  dt_argv[dt_argc++] = "--cachedir";
  dt_argv[dt_argc++] = cache_dir;
  dt_argv[dt_argc++] = "--tmpdir";
  dt_argv[dt_argc++] = tmp_dir;
  dt_argv[dt_argc++] = "--conf";
  dt_argv[dt_argc++] = "write_sidecar_files=never";
  for(int i = core_index; i < argc; i++) dt_argv[dt_argc++] = argv[i];
  dt_argv[dt_argc] = NULL;

  if(dt_init(dt_argc, dt_argv, FALSE, TRUE, NULL))
  {
    free(dt_argv);
    g_free(tmp_dir);
    g_free(cache_dir);
    g_free(config_dir);
    g_free(sandbox_dir);
    return EXIT_FAILURE;
  }
  free(dt_argv);

  gchar *directory = g_path_get_dirname(image_path);
  dt_film_t film;
  dt_film_init(&film);
  const dt_filmid_t filmid = dt_film_new(&film, directory);
  g_free(directory);

  const dt_imgid_t imgid = dt_image_import(filmid, image_path, TRUE, TRUE);
  if(!dt_is_valid_imgid(imgid))
  {
    fprintf(stderr, "failed to load image: %s\n", image_path);
    dt_cleanup();
    g_free(tmp_dir);
    g_free(cache_dir);
    g_free(config_dir);
    g_free(sandbox_dir);
    return EXIT_FAILURE;
  }

  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(&buf, imgid, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');
  const gboolean loaded = buf.buf != NULL;
  dt_mipmap_cache_release(&buf);
  if(!loaded)
  {
    fprintf(stderr, "failed to decode image data: %s\n", image_path);
    dt_cleanup();
    g_free(tmp_dir);
    g_free(cache_dir);
    g_free(config_dir);
    g_free(sandbox_dir);
    return EXIT_FAILURE;
  }

  const dt_image_t *img = dt_image_cache_get(imgid, 'r');
  if(img == NULL)
  {
    fprintf(stderr, "failed to access image cache for: %s\n", image_path);
    dt_cleanup();
    g_free(tmp_dir);
    g_free(cache_dir);
    g_free(config_dir);
    g_free(sandbox_dir);
    return EXIT_FAILURE;
  }

  const double resolved_temperature = clamp_double(requested_temperature,
                                                   DT_IOP_LOWEST_TEMPERATURE,
                                                   DT_IOP_HIGHEST_TEMPERATURE);
  const double resolved_tint = clamp_double(requested_tint,
                                            DT_IOP_LOWEST_TINT,
                                            DT_IOP_HIGHEST_TINT);
  double mul[4] = { 0.0, 0.0, 0.0, 0.0 };
  const int ok = resolve_temp_tint(img, resolved_temperature, resolved_tint, mul);

  if(ok)
  {
    printf("{\n");
    printf("  \"image\": \"%s\",\n", image_path);
    printf("  \"requested\": { \"temperature\": %.9g, \"tint\": %.9g },\n",
           requested_temperature,
           requested_tint);
    printf("  \"resolved\": { \"temperature\": %.9g, \"tint\": %.9g },\n",
           resolved_temperature,
           resolved_tint);
    printf("  \"module\": \"temperature\",\n");
    printf("  \"preset\": { \"id\": %d, \"name\": \"user modified\" },\n", DT_IOP_TEMP_USER);
    printf("  \"params\": { \"red\": %.9g, \"green\": %.9g, \"blue\": %.9g, \"various\": %.9g, \"preset\": %d },\n",
           mul[0],
           mul[1],
           mul[2],
           mul[3],
           DT_IOP_TEMP_USER);
    printf("  \"multipliers\": [%.9g, %.9g, %.9g, %.9g]\n",
           mul[0],
           mul[1],
           mul[2],
           mul[3]);
    printf("}\n");
  }
  else
  {
    fprintf(stderr, "failed to resolve white balance multipliers for: %s\n", image_path);
  }

  dt_image_cache_read_release(img);
  dt_cleanup();
  g_free(tmp_dir);
  g_free(cache_dir);
  g_free(config_dir);
  g_free(sandbox_dir);
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
