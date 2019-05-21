/*
    This file is part of darktable,
    copyright (c) 2019 edgardo hoszowski.

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

#ifndef DT_DEVELOP_CIMG_H
#define DT_DEVELOP_CIMG_H

#include <glib.h>

typedef enum dt_gmic_colorspaces_t
{
  DT_GMIC_RGB_3C = 0,
  DT_GMIC_RGB_1C = 1,
  DT_GMIC_sRGB_3C = 2,
  DT_GMIC_sRGB_1C = 3,
  DT_GMIC_LAB_3C = 4,
  DT_GMIC_LAB_1C = 5
} dt_gmic_colorspaces_t;

typedef enum dt_gmic_params_type_t
{
  DT_GMIC_PARAM_NONE = 0,
  DT_GMIC_PARAM_FLOAT = 1,
  DT_GMIC_PARAM_INT = 2,
  DT_GMIC_PARAM_BOOL = 3,
  DT_GMIC_PARAM_CHOICE = 4,
  DT_GMIC_PARAM_COLOR = 5,
  DT_GMIC_PARAM_POINT = 6,
  DT_GMIC_PARAM_SEPARATOR = 7,
  DT_GMIC_PARAM_NOTE = 8
} dt_gmic_params_type_t;

typedef struct dt_gmic_parameter_point_t
{
  float x, y, r, g, b, a, radius;
  int removable, burst;
} dt_gmic_parameter_point_t;

typedef struct dt_gmic_parameter_choice_t
{
  int default_value;
  GList *list_values;
} dt_gmic_parameter_choice_t;

typedef struct dt_gmic_parameter_float_t
{
  float default_value, min_value, max_value, increment;
  int num_decimals;
} dt_gmic_parameter_float_t;

typedef struct dt_gmic_parameter_int_t
{
  int default_value, min_value, max_value, increment;
} dt_gmic_parameter_int_t;

typedef struct dt_gmic_parameter_bool_t
{
  gboolean default_value;
} dt_gmic_parameter_bool_t;

typedef struct dt_gmic_parameter_color_t
{
  float r, g, b, a;
} dt_gmic_parameter_color_t;

typedef struct dt_gmic_parameter_t
{
  int id;
  char description[31];
  dt_gmic_params_type_t type;
  gboolean percent;
  union
  {
    dt_gmic_parameter_float_t _float;
    dt_gmic_parameter_int_t _int;
    dt_gmic_parameter_bool_t _bool;
    dt_gmic_parameter_choice_t _choice;
    dt_gmic_parameter_color_t _color;
    dt_gmic_parameter_point_t _point;
    char *_separator;
    char *_note;
  } value;
} dt_gmic_parameter_t;

typedef struct dt_gmic_command_t
{
  char name[31];
  char description[101];
  dt_gmic_colorspaces_t colorspace;
  gboolean scale_image;
  GList *parameters;
  char *command;
} dt_gmic_command_t;

GList *dt_load_gmic_commands_from_dir(const char *subdir);
void dt_gmic_commands_cleanup();

void dt_gmic_run_3c(const float *const in, float *out, const int width, const int height, const char *str,
                    const gboolean scale_image);
void dt_gmic_run_1c(const float *const in, float *out, const int width, const int height, const char *str,
                    const gboolean scale_image);

#endif
