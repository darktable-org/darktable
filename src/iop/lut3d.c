/*
    This file is part of darktable,
    copyright (c) 2016 Chih-Mao Chen

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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bauhaus/bauhaus.h"
#include "common/imageio_png.h"
#include "common/colorspaces_inline_conversions.h"
#include "develop/imageop.h"
#include "dtgtk/button.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <libgen.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#if defined (_WIN32)
#include "win/getdelim.h"
#endif // defined (_WIN32)

DT_MODULE_INTROSPECTION(1, dt_iop_lut3d_params_t)

typedef enum dt_iop_lut3d_colorspace_t
{
  DT_IOP_LOG_RGB = 0,
  DT_IOP_SRGB = 1,
  DT_IOP_REC709 = 2,
  DT_IOP_LIN_REC709 = 3,
  DT_IOP_LIN_PROPHOTORGB = 4,
  DT_IOP_LOG_FUJI = 5,
} dt_iop_lut3d_colorspace_t;

typedef enum dt_iop_lut3d_interpolation_t
{
  DT_IOP_TETRAHEDRAL = 0,
  DT_IOP_TRILINEAR = 1,
  DT_IOP_PYRAMID = 2,
} dt_iop_lut3d_interpolation_t;

typedef void((*dt_interpolation_worker)(float *input, float *output, const float *const clut, const uint8_t level));

typedef struct dt_iop_lut3d_params_t
{
  char filepath[512];
  uint8_t colorspace;
  uint8_t interpolation;
  float middle_grey;
  float min_exposure;
  float dynamic_range;
  int log2tolin;
} dt_iop_lut3d_params_t;

typedef struct dt_iop_lut3d_gui_data_t
{
//  GtkWidget *lutfolder;
  GtkWidget *hbox;
  GtkWidget *filepath;
  GtkWidget *colorspace;
  GtkWidget *interpolation;
  GtkWidget *middle_grey;
  GtkWidget *min_exposure;
  GtkWidget *dynamic_range;
  GtkWidget *log2tolin;
  GtkWidget *log_options;
} dt_iop_lut3d_gui_data_t;

typedef struct dt_iop_lut3d_global_data_t
{
  float *clut;  // cube lut pointer
  uint8_t level; // cube_size
} dt_iop_lut3d_global_data_t;

const char *name()
{
  return _("lut 3D");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int groups()
{
  return IOP_GROUP_COLOR;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_Lab;
}

// From `HaldCLUT_correct.c' by Eskil Steenberg (http://www.quelsolaar.com) (BSD licensed)
void correct_pixel_trilinear(float *input, float *output, const float *const clut, const uint8_t level)
{
  int red, green, blue, i, j;
  float tmp[6];
  const int level2 = level * level;

  for(int c = 0; c < 3; ++c) input[c] = input[c] < 0.0f ? 0.0f : (input[c] > 1.0f ? 1.0f : input[c]);
  red = input[0] * (float)(level - 1);
  if(red > level - 2) red = (float)level - 2;
  if(red < 0) red = 0;
  green = input[1] * (float)(level - 1);
  if(green > level - 2) green = (float)level - 2;
  if(green < 0) green = 0;
  blue = input[2] * (float)(level - 1);
  if(blue > level - 2) blue = (float)level - 2;
  if(blue < 0) blue = 0;

  const float r = input[0] * (float)(level - 1) - red; // delta red
  const float g = input[1] * (float)(level - 1) - green; // delta green
  const float b = input[2] * (float)(level - 1) - blue; // delta blue

// indexes of P000 to P111 in clut
  const int color = red + green * level + blue * level * level;
  i = color * 3;  // P000
  j = (color + 1) * 3;  // P100

  tmp[0] = clut[i++] * (1 - r) + clut[j++] * r;
  tmp[1] = clut[i++] * (1 - r) + clut[j++] * r;
  tmp[2] = clut[i] * (1 - r) + clut[j] * r;

  i = (color + level) * 3;  // P010
  j = (color + level + 1) * 3;  //P110

  tmp[3] = clut[i++] * (1 - r) + clut[j++] * r;
  tmp[4] = clut[i++] * (1 - r) + clut[j++] * r;
  tmp[5] = clut[i] * (1 - r) + clut[j] * r;

  output[0] = tmp[0] * (1 - g) + tmp[3] * g;
  output[1] = tmp[1] * (1 - g) + tmp[4] * g;
  output[2] = tmp[2] * (1 - g) + tmp[5] * g;

  i = (color + level2) * 3;  // P001
  j = (color + level2 + 1) * 3;  // P101

  tmp[0] = clut[i++] * (1 - r) + clut[j++] * r;
  tmp[1] = clut[i++] * (1 - r) + clut[j++] * r;
  tmp[2] = clut[i] * (1 - r) + clut[j] * r;

  i = (color + level + level2) * 3;  // P011
  j = (color + level + level2 + 1) * 3;  // P111

  tmp[3] = clut[i++] * (1 - r) + clut[j++] * r;
  tmp[4] = clut[i++] * (1 - r) + clut[j++] * r;
  tmp[5] = clut[i] * (1 - r) + clut[j] * r;

  tmp[0] = tmp[0] * (1 - g) + tmp[3] * g;
  tmp[1] = tmp[1] * (1 - g) + tmp[4] * g;
  tmp[2] = tmp[2] * (1 - g) + tmp[5] * g;

  output[0] = output[0] * (1 - b) + tmp[0] * b;
  output[1] = output[1] * (1 - b) + tmp[1] * b;
  output[2] = output[2] * (1 - b) + tmp[2] * b;
}

void correct_pixel_tetrahedral(float *input, float *output, const float *const clut, const uint8_t level)
{
  int red, green, blue;
  const int level2 = level * level;

  for(int c = 0; c < 3; ++c) input[c] = input[c] < 0.0f ? 0.0f : (input[c] > 1.0f ? 1.0f : input[c]);
  red = input[0] * (float)(level - 1);
  if(red > level - 2) red = (float)level - 2;
  if(red < 0) red = 0;
  green = input[1] * (float)(level - 1);
  if(green > level - 2) green = (float)level - 2;
  if(green < 0) green = 0;
  blue = input[2] * (float)(level - 1);
  if(blue > level - 2) blue = (float)level - 2;
  if(blue < 0) blue = 0;

  const float r = input[0] * (float)(level - 1) - red; // delta red
  const float g = input[1] * (float)(level - 1) - green; // delta green
  const float b = input[2] * (float)(level - 1) - blue; // delta blue

// indexes of P000 to P111 in clut
  const int color = red + green * level + blue * level2;
  const int i000 = color * 3;  // P000
  const int i100 = (color + 1) * 3;  // P100
  const int i010 = (color + level) * 3;  // P010
  const int i110 = (color + level + 1) * 3;  //P110
  const int i001 = (color + level2) * 3;  // P001
  const int i101 = (color + level2 + 1) * 3;  // P101
  const int i011 = (color + level + level2) * 3;  // P011
  const int i111 = (color + level + level2 + 1) * 3;  // P111

  if (r > g)
  {
    if (g > b)
    {
      output[0] = (1-r)*clut[i000] + (r-g)*clut[i100] + (g-b)*clut[i110] + b*clut[i111];
      output[1] = (1-r)*clut[i000+1] + (r-g)*clut[i100+1] + (g-b)*clut[i110+1] + b*clut[i111+1];
      output[2] = (1-r)*clut[i000+2] + (r-g)*clut[i100+2] + (g-b)*clut[i110+2] + b*clut[i111+2];
    }
    else if (r > b)
    {
      output[0] = (1-r)*clut[i000] + (r-b)*clut[i100] + (b-g)*clut[i101] + g*clut[i111];
      output[1] = (1-r)*clut[i000+1] + (r-b)*clut[i100+1] + (b-g)*clut[i101+1] + g*clut[i111+1];
      output[2] = (1-r)*clut[i000+2] + (r-b)*clut[i100+2] + (b-g)*clut[i101+2] + g*clut[i111+2];
    }
    else
    {
      output[0] = (1-b)*clut[i000] + (b-r)*clut[i001] + (r-g)*clut[i101] + g*clut[i111];
      output[1] = (1-b)*clut[i000+1] + (b-r)*clut[i001+1] + (r-g)*clut[i101+1] + g*clut[i111+1];
      output[2] = (1-b)*clut[i000+2] + (b-r)*clut[i001+2] + (r-g)*clut[i101+2] + g*clut[i111+2];
    }
  }
  else
  {
    if (b > g)
    {
      output[0] = (1-b)*clut[i000] + (b-g)*clut[i001] + (g-r)*clut[i011] + r*clut[i111];
      output[1] = (1-b)*clut[i000+1] + (b-g)*clut[i001+1] + (g-r)*clut[i011+1] + r*clut[i111+1];
      output[2] = (1-b)*clut[i000+2] + (b-g)*clut[i001+2] + (g-r)*clut[i011+2] + r*clut[i111+2];
    }
    else if (b > r)
    {
      output[0] = (1-g)*clut[i000] + (g-b)*clut[i010] + (b-r)*clut[i011] + r*clut[i111];
      output[1] = (1-g)*clut[i000+1] + (g-b)*clut[i010+1] + (b-r)*clut[i011+1] + r*clut[i111+1];
      output[2] = (1-g)*clut[i000+2] + (g-b)*clut[i010+2] + (b-r)*clut[i011+2] + r*clut[i111+2];
    }
    else
    {
      output[0] = (1-g)*clut[i000] + (g-r)*clut[i010] + (r-b)*clut[i110] + b*clut[i111];
      output[1] = (1-g)*clut[i000+1] + (g-r)*clut[i010+1] + (r-b)*clut[i110+1] + b*clut[i111+1];
      output[2] = (1-g)*clut[i000+2] + (g-r)*clut[i010+2] + (r-b)*clut[i110+2] + b*clut[i111+2];
    }
  }
}

void correct_pixel_pyramid(float *input, float *output, const float *const clut, const uint8_t level)
{
  int red, green, blue;
  const int level2 = level * level;

  for(int c = 0; c < 3; ++c) input[c] = input[c] < 0.0f ? 0.0f : (input[c] > 1.0f ? 1.0f : input[c]);
  red = input[0] * (float)(level - 1);
  if(red > level - 2) red = (float)level - 2;
  if(red < 0) red = 0;
  green = input[1] * (float)(level - 1);
  if(green > level - 2) green = (float)level - 2;
  if(green < 0) green = 0;
  blue = input[2] * (float)(level - 1);
  if(blue > level - 2) blue = (float)level - 2;
  if(blue < 0) blue = 0;

  const float r = input[0] * (float)(level - 1) - red; // delta red
  const float g = input[1] * (float)(level - 1) - green; // delta green
  const float b = input[2] * (float)(level - 1) - blue; // delta blue

// indexes of P000 to P111 in clut
  const int color = red + green * level + blue * level2;
  const int i000 = color * 3;  // P000
  const int i100 = (color + 1) * 3;  // P100
  const int i010 = (color + level) * 3;  // P010
  const int i110 = (color + level + 1) * 3;  //P110
  const int i001 = (color + level2) * 3;  // P001
  const int i101 = (color + level2 + 1) * 3;  // P101
  const int i011 = (color + level + level2) * 3;  // P011
  const int i111 = (color + level + level2 + 1) * 3;  // P111

  if (g > r && b > r)
  {
    output[0] = clut[i000] + (clut[i111]-clut[i011])*r + (clut[i010]-clut[i000])*g + (clut[i001]-clut[i000])*b
      + (clut[i011]-clut[i001]-clut[i010]+clut[i000])*g*b;
    output[1] = clut[i000+1] + (clut[i111+1]-clut[i011+1])*r + (clut[i010+1]-clut[i000+1])*g + (clut[i001+1]-clut[i000+1])*b
      + (clut[i011+1]-clut[i001+1]-clut[i010+1]+clut[i000+1])*g*b;
    output[2] = clut[i000+2] + (clut[i111+2]-clut[i011+2])*r + (clut[i010+2]-clut[i000+2])*g + (clut[i001+2]-clut[i000+2])*b
      + (clut[i011+2]-clut[i001+2]-clut[i010+2]+clut[i000+2])*g*b;
  }
  else if (r > g && b > g)
  {
    output[0] = clut[i000] + (clut[i100]-clut[i000])*r + (clut[i111]-clut[i101])*g + (clut[i001]-clut[i000])*b
      + (clut[i101]-clut[i001]-clut[i100]+clut[i000])*r*b;
    output[1] = clut[i000+1] + (clut[i100+1]-clut[i000+1])*r + (clut[i111+1]-clut[i101+1])*g + (clut[i001+1]-clut[i000+1])*b
      + (clut[i101+1]-clut[i001+1]-clut[i100+1]+clut[i000+1])*r*b;
    output[2] = clut[i000+2] + (clut[i100+2]-clut[i000+2])*r + (clut[i111]-clut[i101+2])*g + (clut[i001+2]-clut[i000+2])*b
      + (clut[i101+2]-clut[i001+2]-clut[i100+2]+clut[i000+2])*r*b;
  }
  else
  {
    output[0] = clut[i000] + (clut[i100]-clut[i000])*r + (clut[i010]-clut[i000])*g + (clut[i111]-clut[i110])*b
      + (clut[i110]-clut[i100]-clut[i010]+clut[i000])*r*g;
    output[1] = clut[i000+1] + (clut[i100+1]-clut[i000+1])*r + (clut[i010+1]-clut[i000+1])*g + (clut[i111+1]-clut[i110+1])*b
      + (clut[i110+1]-clut[i100+1]-clut[i010+1]+clut[i000+1])*r*g;
    output[2] = clut[i000+2] + (clut[i100+2]-clut[i000+2])*r + (clut[i010+2]-clut[i000+2])*g + (clut[i111+2]-clut[i110+2])*b
      + (clut[i110+2]-clut[i100+2]-clut[i010+2]+clut[i000+2])*r*g;
  }
}

uint8_t calculate_clut_haldclut(char *filepath, float **clut)
{
  dt_imageio_png_t png;
  if(read_header(filepath, &png))
  {
    fprintf(stderr, "[lut3d] invalid png header from %s\n", filepath);
    dt_control_log(_("invalid png header from %s"), filepath);
    return 0;
  }
  dt_print(DT_DEBUG_DEV, "[lut3d] png: width=%d, height=%d, color_type=%d, bit_depth=%d\n", png.width,
           png.height, png.color_type, png.bit_depth);
//  printf("[lut3d] png: width=%d, height=%d, color_type=%d, bit_depth=%d\n", png.width,
//           png.height, png.color_type, png.bit_depth);
  if (png.bit_depth !=8 && png.bit_depth != 16)
  {
    fprintf(stderr, "[lut3d] png bit-depth %d not supported\n", png.bit_depth);
    dt_control_log(_("png bit-depth %d not supported"), png.bit_depth);
    fclose(png.f);
    png_destroy_read_struct(&png.png_ptr, &png.info_ptr, NULL);
    return 0;
  }
  uint8_t level = 2;
  while(level * level * level < png.width) ++level;
  if(level * level * level != png.width)
  {
    fprintf(stderr, "[lut3d] invalid level in png file %d %d\n", level, png.width);
    dt_control_log(_("invalid level in png file %d %d"), level, png.width);
    fclose(png.f);
    png_destroy_read_struct(&png.png_ptr, &png.info_ptr, NULL);
    return 0;
  }
//printf("haldclut file opened\n");
  const size_t buf_size = (size_t)png.height * png_get_rowbytes(png.png_ptr, png.info_ptr);
  dt_print(DT_DEBUG_DEV, "[lut3d] allocating %zu bytes for png lut\n", buf_size);
  void *buf = NULL;
  buf = dt_alloc_align(16, buf_size);
  if(!buf)
  {
    fclose(png.f);
    png_destroy_read_struct(&png.png_ptr, &png.info_ptr, NULL);
    fprintf(stderr, "[lut3d] error - allocating buffer for png lut\n");
    dt_control_log(_("error - allocating buffer for png lut"));
    return 0;
  }
  if (read_image(&png, buf))
  {
    dt_free_align(buf);
    fprintf(stderr, "[lut3d] error - could not read png image `%s'\n", filepath);
    dt_control_log(_("error - could not read png image %s"), filepath);
    return 0;
  }
  const size_t buf_size_lut = (size_t)png.height * png.height * 3;
  float *lclut=dt_alloc_align(16, buf_size_lut * sizeof(float));
  if(!lclut)
  {
    dt_free_align(buf);
    fprintf(stderr, "[lut3d] error - allocating buffer for png lut\n");
    dt_control_log(_("error - allocating buffer for png lut"));
    return 0;
  }
  const float norm = powf(2.f, png.bit_depth) - 1.0f;
  if (png.bit_depth == 8)
  {
    const uint8_t *buf_8 = buf;
    for (size_t i = 0; i < buf_size_lut; ++i)
      lclut[i] = (float)buf_8[i] / norm;
  }
  else
  {
    const uint16_t *buf_16 = buf;
    for (size_t i = 0; i < buf_size_lut; ++i)
      lclut[i] = (float)buf_16[i] / norm;
  }
  dt_free_align(buf);
  *clut = lclut;
//printf("haldclut ok - level %d, size %d; clut %p\n", level, buf_size, lclut);
  return level;
}

// provided by @rabauke, atof replaces strtod & sccanf which are locale dependent
double dt_atof(const char *str)
{
  if (strncmp(str, "nan", 3) == 0 || strncmp(str, "NAN", 3) == 0)
    return NAN;
  double integral_result = 0;
  double fractional_result = 0;
  double sign = 1;
  if (*str == '+') {
    str++;
    sign = +1;
  } else if (*str == '-') {
    str++;
    sign = -1;
  }
  if (strncmp(str, "inf", 3) == 0 || strncmp(str, "INF", 3) == 0)
    return sign * INFINITY;
  // search for end of integral part and parse from
  // right to left for numerical stability
//  {
    const char * istr_back = str;
    while (*str >= '0' && *str <= '9')
      str++;
    const char * istr_2 = str;
    double imultiplier = 1;
    while (istr_2 != istr_back)
    {
      --istr_2;
      integral_result += (*istr_2 - '0') * imultiplier;
      imultiplier *= 10;
    }
    if (*str == '.')
    {
      str++;
//  }
  // search for end of fractional part and parse from
  // right to left for numerical stability
//  {
    const char * fstr_back = str;
    while (*str >= '0' && *str <= '9')
      str++;
    const char * fstr_2 = str;
    double fmultiplier = 1;
    while (fstr_2 != fstr_back)
    {
      --fstr_2;
      fractional_result += (*fstr_2 - '0') * fmultiplier;
      fmultiplier *= 10;
    }
    fractional_result /= fmultiplier;
    }
//  }
  double result = sign * (integral_result + fractional_result);
  if (*str == 'e' || *str == 'E')
  {
    str++;
    double power_sign = 1;
    if (*str == '+') {
      str++;
      power_sign = +1;
    }
    else if (*str == '-') {
      str++;
      power_sign = -1;
    }
    double power = 0;
    while (*str >= '0' && *str <= '9') {
      power *= 10;
      power += *str - '0';
      str++;
    }
    if (power_sign > 0)
      result *= pow(10, power);
    else
      result /= pow(10, power);
  }
  return result;
}

// retun max 3 tokens from the line (separator = ' ' and token length = 50)
uint8_t parse_cube_line(char *line, char *token)
{
  uint8_t i, c;
  i = c = 0;
  char *t = token;
  char *l = line;

//printf("input line %s size %d last %02hhx\n", line, strlen(line), line[strlen(line)-1]);
  while (*l != 0 && c < 3 && i < 50)
  {
    if (*l == '#' || *l == '\n' || *l == '\r')
    { // end of useful part of the line
      if (i > 0)
      {
        *t = 0;
        c++;
        return c;
      }
      else
      {
        *t = 0;
        return c;
      }
    }
    if (*l == ' ' || *l == '\t')
    { // separator
      if (i > 0)
      {
        *t = 0;
        c++;
        i = 0;
        t = token + (50 * c);
      }
    }
    else
    { // capture info
      *t = *l;
      t++;
      i++;
    }
    l++;
  }
  return c;
}
uint8_t calculate_clut_cube(char *filepath, float **clut)
{
  FILE *cube_file;
  char *line = NULL;
  size_t len = 0;
  ssize_t read;
  char token[3][50];
  uint8_t level = 0;
  float *lclut = NULL;
  uint32_t i = 0;
  uint32_t buf_size = 0;

  if(!(cube_file = g_fopen(filepath, "r")))
  {
    fprintf(stderr, "[lut3d] invalid cube file: %s\n", filepath);
    dt_control_log(_("error - invalid cube file: %s"), filepath);
    return 0;
  }
//printf("cube file opened\n");
  while ((read = getline(&line, &len, cube_file)) != -1)
  {
    uint8_t nb_token = parse_cube_line(line, &token[0][0]);
    if (nb_token)
    {
//printf("line parsed: %d tokens: %s, %s, %s\n", nb_token, (nb_token > 0) ? token[0] : "", (nb_token > 1) ? token[1] : "", (nb_token > 2) ? token[2] : "" );
      if (token[0][0] == 'T') continue;
      else if (strcmp("DOMAIN_MIN", token[0]) == 0)
      {
        if (strtod(token[1], NULL) != 0.0f)
        {
          fprintf(stderr, "[lut3d] DOMAIN MIN <> 0.0 is not supported\n");
          dt_control_log(_("DOMAIN MIN <> 0.0 is not supported"));
          if (lclut) dt_free_align(lclut);
          free(line);
          fclose(cube_file);
        }
//printf("->Domain min %d\n", atoll(token[1]));
      }
      else if (strcmp("DOMAIN_MAX", token[0]) == 0)
      {
        if (strtod(token[1], NULL) != 1.0f)
        {
          fprintf(stderr, "[lut3d] DOMAIN MAX <> 1.0 is not supported\n");
          dt_control_log(_("DOMAIN MAX <> 1.0 is not supported"));
          if (lclut) dt_free_align(lclut);
          free(line);
          fclose(cube_file);
        }
//printf("->Domain max %d\n", atoll(token[1]));
      }
      else if (strcmp("LUT_1D_SIZE", token[0]) == 0)
      {
        fprintf(stderr, "[lut3d] 1D cube lut is not supported\n");
        dt_control_log(_("[1D cube lut is not supported"));
        free(line);
        fclose(cube_file);
        return 0;
//printf("->Lut 1D %d\n", atoll(token[1]));
      }
      else if (strcmp("LUT_3D_SIZE", token[0]) == 0)
      {
        level = atoll(token[1]);
        buf_size = level * level * level * 3;
        lclut = dt_alloc_align(16, buf_size * sizeof(float));
//printf("->Lut 3D %d buf_size %d\n", level, buf_size);
        if(!lclut)
        {
          fprintf(stderr, "[lut3d] error - allocating buffer for cube lut\n");
          dt_control_log(_("error - allocating buffer for cube lut"));
          free(line);
          fclose(cube_file);
          return 0;
        }
      }
      else
      {
        if (!level)
        {
          fprintf(stderr, "[lut3d] error - cube lut size is not defined\n");
          dt_control_log(_("error - cube lut size is not defined"));
          free(line);
          fclose(cube_file);
          return 0;
        }
        for (int j=0; j < 3; j++)
        {
          lclut[i+j] = dt_atof(token[j]);
// if (fabsf(lclut[i+j] - strtod(token[j], NULL)) > 0.0002f) printf("atof %f; strtod %f\n",lclut[i+j],strtod(token[j], NULL));
        }
        i += 3;
// printf("->Values %f %f %f\n", strtod(token[0], NULL), strtod(token[1], NULL), strtod(token[2], NULL));
      }
    }
  }
  if (i != buf_size || i == 0)
  {
    fprintf(stderr, "[lut3d] error - cube lut lines number is not correct\n");
    dt_control_log(_("error - cube lut lines number is not correct"));
    dt_free_align(lclut);
    free(line);
    fclose(cube_file);
    return 0;
  }
  *clut = lclut;
//printf("cube ok - level %d, buf_size %d nb colors %d clut %p\n", level, buf_size, i, lclut);
  free(line);
  fclose(cube_file);
  return level;
}

static inline void lin_to_logFuji(float *lin)
{
  for (int ch=0; ch<3; ch++)
  {
    const float a = 0.555556f * 7.28132488f;
    const float b = 0.009468f;
    const float c = 0.344676f;
    const float d = 0.790453f;
    const float e = 8.735631f * 7.28132488f;
    const float f = 0.092864f;
    const float cut = 0.00089f / 7.28132488f;
    float logNorm;

    if (lin[ch] >= cut)
      logNorm = c * log10f(a * lin[ch] +b) + d;
    else
      logNorm = e * lin[ch] + f;
    if( logNorm < 0.0f || lin[ch] <= 0.0f)
      lin[ch] = 0.0f;
    else
      lin[ch] = logNorm;
  }
}

static inline void logFuji_to_lin(float *lout)
{
  for (int ch=0; ch<3; ch++)
  {
    const float a = 0.555556f * 7.28132488f;
    const float b = 0.009468f;
    const float c = 0.344676f;
    const float d = 0.790453f;
    const float e = 8.735631f * 7.28132488f;
    const float f = 0.092864f;
    const float cut = 0.100537775223865;
    float Norm;

    if (lout[ch] >= cut)
      Norm = (powf(10, (lout[ch] - d) / c) - b) / a;
    else Norm = (lout[ch] - f) / e;
    lout[ch] = Norm < 0.0f ? 0.0f : Norm > 1.0f ? 1.0f : Norm;
  }
}

// From data/kernels/extended.cl
static inline float fastlog2(float x)
{
  union { float f; unsigned int i; } vx = { x };
  union { unsigned int i; float f; } mx = { (vx.i & 0x007FFFFF) | 0x3f000000 };

  float y = vx.i;

  y *= 1.1920928955078125e-7f;

  return y - 124.22551499f
    - 1.498030302f * mx.f
    - 1.72587999f / (0.3520887068f + mx.f);
}

static inline void lin_to_log2(float *lin, const float middle_grey,
                                    const float min_exposure, const float dynamic_range)
{
  for (int ch=0; ch<3; ch++)
  {
    const float logNorm = (fastlog2(lin[ch] / middle_grey) - min_exposure) / dynamic_range;
    if( logNorm < 0.0f || lin[ch] <= 0.0f)
    {
      lin[ch] = 0.0f;
    }
    else
    {
      lin[ch] = logNorm;
    }
  }
}

static inline void log2_to_lin(float *lout, const float middle_grey,
                                    const float min_exposure, const float dynamic_range)
{
  for (int ch=0; ch<3; ch++)
  {
    const float Norm = middle_grey * powf(2, lout[ch] * dynamic_range + min_exposure);
    lout[ch] = Norm < 0.0f ? 0.0f : Norm > 1.0f ? 1.0f : Norm;
  }
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ibuf, void *const obuf,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
printf("process\n");
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;
  dt_iop_lut3d_global_data_t *gp = (dt_iop_lut3d_global_data_t *)self->data;
  const int ch = piece->colors;
  const int colorspace = p->colorspace;
  const float middle_grey = p->middle_grey * 0.01f;
  const float min_exposure = p->min_exposure;
  const float dynamic_range = p->dynamic_range;
  const int log2tolin = p->log2tolin;
  const float *const clut = (float *)gp->clut;
  const uint8_t level = gp->level;
  const dt_interpolation_worker interpolation_worker = (p->interpolation == DT_IOP_TETRAHEDRAL) ? correct_pixel_tetrahedral
                                                          : (p->interpolation == DT_IOP_TRILINEAR) ? correct_pixel_trilinear
                                                          : correct_pixel_pyramid;
  if (clut)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
    for(int j = 0; j < roi_out->height; j++)
    {
      float *in = ((float *)ibuf) + (size_t)ch * roi_in->width * j;
      float *out = ((float *)obuf) + (size_t)ch * roi_out->width * j;
      for(int i = 0; i < roi_out->width; i++)
      {
        float lin[3] = {0, 0, 0};
        float lout[3] = {0, 0, 0};
        if (colorspace == DT_IOP_LOG_RGB) // log RGB
        {
          dt_Lab_to_prophotorgb(in, lin);
          lin_to_log2(lin, middle_grey, min_exposure, dynamic_range);
          interpolation_worker(lin, lout, clut, level);
          if (log2tolin) log2_to_lin(lout, middle_grey, min_exposure, dynamic_range);
          dt_prophotorgb_to_Lab(lout, out);
        }
        else if (colorspace == DT_IOP_SRGB) // gamma sRGB
        {
          dt_Lab_to_sRGB(in, lin);
          interpolation_worker(lin, lout, clut, level);
          dt_sRGB_to_Lab(lout, out);
        }
        else if (colorspace == DT_IOP_REC709) // gamma REC.709
        {
          dt_Lab_to_REC709(in, lin);
          interpolation_worker(lin, lout, clut, level);
          dt_REC709_to_Lab(lout, out);
        }
        else if (colorspace == DT_IOP_LIN_REC709) // linear REC.709
        {
          dt_Lab_to_linear_sRGB(in, lin);
          interpolation_worker(lin, lout, clut, level);
          dt_linear_sRGB_to_Lab(lout, out);
        }
        else if (colorspace == DT_IOP_LIN_PROPHOTORGB) // gamma REC.709
        {
          dt_Lab_to_prophotorgb(in, lin);
          interpolation_worker(lin, lout, clut, level);
          dt_prophotorgb_to_Lab(lout, out);
        }
        else if (colorspace == DT_IOP_LOG_FUJI) // log RGB Fuji
        {
          dt_Lab_to_prophotorgb(in, lin);
          lin_to_logFuji(lin);
          interpolation_worker(lin, lout, clut, level);
          logFuji_to_lin(lout);
          dt_prophotorgb_to_Lab(lout, out);
        }
/*        else if (colorspace == DT_IOP_XYZ) // XYZ
        {
          dt_Lab_to_XYZ(in, lin);
          interpolation_worker(lin, lout, clut, level);
          dt_XYZ_to_Lab(lout, out);
        }
        else if (colorspace == 4) // lab
        {
          lin[0] = in[0] / 100.0f;
          lin[1] = in[1] / 255.0f;
          lin[2] = in[2] / 255.0f;
          interpolation_worker(lin, lout, clut, level);
          out[0] = lout[0] * 100.0f;
          out[1] = lout[1] * 255.0f;
          out[2] = lout[2] * 255.0f;
        } */
        else for(int c = 0; c < 3; ++c) out[c] = in[c];
        in += ch;
        out += ch;
      }
    }
    return;
  }
  // no clut
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    float *in = ((float *)ibuf) + (size_t)ch * roi_in->width * j;
    float *out = ((float *)obuf) + (size_t)ch * roi_out->width * j;
    for(int i = 0; i < roi_out->width; i++)
    {
      for(int c = 0; c < 3; ++c) out[c] = in[c];
      in += ch;
      out += ch;
    }
  }
}
void reload_defaults(dt_iop_module_t *self)
{
}

void init(dt_iop_module_t *self)
{
printf("init\n");
  self->data = NULL;
  self->params = calloc(1, sizeof(dt_iop_lut3d_params_t));
  self->default_params = calloc(1, sizeof(dt_iop_lut3d_params_t));
  self->default_enabled = 0;
//  self->priority = 710; // self order created by iop_dependencies.py, do not edit!
  self->params_size = sizeof(dt_iop_lut3d_params_t);
  self->gui_data = NULL;
  dt_iop_lut3d_params_t tmp = (dt_iop_lut3d_params_t)
    { { "" },
    DT_IOP_SRGB,
    DT_IOP_TETRAHEDRAL,
    50.0f,  // grey point
    -6.0f,  // shadows range
    10.0f,   // dynamic range
    1  // log2 to lin
  };

  memcpy(self->params, &tmp, sizeof(dt_iop_lut3d_params_t));
  memcpy(self->default_params, &tmp, sizeof(dt_iop_lut3d_params_t));
}

void init_global(dt_iop_module_so_t *self)
{
printf("init_global\n");
  dt_iop_lut3d_global_data_t *gd
      = (dt_iop_lut3d_global_data_t *)malloc(sizeof(dt_iop_lut3d_global_data_t));
  self->data = gd;
  gd->clut = NULL;
  gd->level = 0;
}

void cleanup(dt_iop_module_t *self)
{
printf("cleanup\n");
  free(self->params);
  self->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *self)
{
printf("cleanup_global\n");
  dt_iop_lut3d_global_data_t *gp = (dt_iop_lut3d_global_data_t *)self->data;
  if (gp->clut)
  {
    dt_free_align(gp->clut);
    gp->clut = NULL;
  }
  free(self->data);
  self->data = NULL;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  printf("commit\n");
    dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;
    dt_iop_lut3d_global_data_t *gp = (dt_iop_lut3d_global_data_t *)self->data;
    gchar *lutfolder = dt_conf_get_string("plugins/darkroom/lut3d/def_path");
    if (p->filepath[0] && lutfolder[0] && !gp->clut)
    {
      char *fullpath = g_build_filename(lutfolder, p->filepath, NULL);
      if (g_str_has_suffix (p->filepath, ".png") || g_str_has_suffix (p->filepath, ".PNG"))
      {
        gp->level = calculate_clut_haldclut(fullpath, &gp->clut);
        gp->level *= gp->level;
      }
      else if (g_str_has_suffix (p->filepath, ".cube") || g_str_has_suffix (p->filepath, ".CUBE"))
      {
        gp->level = calculate_clut_cube(fullpath, &gp->clut);
      }
      g_free(fullpath);
    }
  }

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
printf("init_pipe\n");
  // create part of the pixelpipe
}

static void filepath_callback(GtkWidget *w, dt_iop_module_t *self)
{
//printf("filepath_callback\n");
  if(darktable.gui->reset) return;
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;
  dt_iop_lut3d_global_data_t *gp = (dt_iop_lut3d_global_data_t *)self->data;
  snprintf(p->filepath, sizeof(p->filepath), "%s", gtk_entry_get_text(GTK_ENTRY(w)));

  if (gp->clut)
  { // the clut is obsolete
    dt_free_align(gp->clut);
  }
  gp->clut = NULL;
  gp->level = 0;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void colorspace_callback(GtkWidget *widget, dt_iop_module_t *self)
{
//printf("colorspace_callback\n");
  if(darktable.gui->reset) return;
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;
  dt_iop_lut3d_gui_data_t *g = (dt_iop_lut3d_gui_data_t *)self->gui_data;
  p->colorspace = dt_bauhaus_combobox_get(widget);
  if (p->colorspace == DT_IOP_LOG_RGB)
    gtk_widget_set_visible(g->log_options, TRUE);
  else
    gtk_widget_set_visible(g->log_options, FALSE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void interpolation_callback(GtkWidget *widget, dt_iop_module_t *self)
{
//printf("interpolation_callback\n");
  if(darktable.gui->reset) return;
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;
  p->interpolation = dt_bauhaus_combobox_get(widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void middle_grey_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;
  p->middle_grey = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void dynamic_range_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;
  p->dynamic_range = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void min_exposure_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;
  p->min_exposure = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void log2tolin_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;
  p->log2tolin = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void button_clicked(GtkWidget *widget, dt_iop_module_t *self)
{
  int filetype;
//printf("button_clicked\n");
  dt_iop_lut3d_gui_data_t *g = (dt_iop_lut3d_gui_data_t *)self->gui_data;
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;
  gchar* lutfolder = dt_conf_get_string("plugins/darkroom/lut3d/def_path");
  if (strlen(lutfolder) == 0)
  {
    fprintf(stderr, "[lut3d] Lut root folder not defined\n");
    dt_control_log(_("Lut root folder not defined"));
    g_free(lutfolder);
    return;
  }
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new(
      _("select lut file"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_OPEN, _("_cancel"), GTK_RESPONSE_CANCEL,
      _("_select"), GTK_RESPONSE_ACCEPT, (char *)NULL);
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);

  if (strlen(p->filepath) == 0 || access(p->filepath, F_OK) == -1)
  {
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), lutfolder);
  }
  else
  {
    char *composed = g_build_filename(lutfolder, p->filepath, NULL);
//printf("button_clicked lutfolder/filepath %s\n", composed);
    gtk_file_chooser_select_filename(GTK_FILE_CHOOSER(filechooser), composed);
    g_free(composed);
  }

  filetype = 1;
  if (p->filepath[0] && (g_str_has_suffix (p->filepath, ".cube") || g_str_has_suffix (p->filepath, ".CUBE")))
    filetype = 2;

  GtkFileFilter* filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*.png");
  gtk_file_filter_set_name(filter, _("hald cluts (png)"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);
  if (filetype == 1) gtk_file_chooser_set_filter (GTK_FILE_CHOOSER(filechooser), filter);

  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*.cube");
  gtk_file_filter_set_name(filter, _("3D lut (cube)"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);
  if (filetype == 2) gtk_file_chooser_set_filter (GTK_FILE_CHOOSER(filechooser), filter);

  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*");
  gtk_file_filter_set_name(filter, _("all files"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *filepath = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
//    printf("button_clicked entered filepath %s\n", filepath);

    if (strcmp(lutfolder, filepath) < 0)
    { // remove root lut folder from file path
      gchar *c = filepath + strlen(lutfolder) + 1;
//      printf("button_clicked should be filepath %s\n", (gchar *)c);
      strcpy(filepath, (gchar *)c);
    }
    else // file chosen outside of root folder
    {
      fprintf(stderr, "[lut3d] Select file outside Lut root folder is not allowed\n");
      dt_control_log(_("Select file outside Lut root folder is not allowed"));
      g_free(lutfolder);
      gtk_widget_destroy(filechooser);
      return;
    }
//    printf("button_clicked filepath retained %s\n", filepath);
    gtk_entry_set_text(GTK_ENTRY(g->filepath), filepath);
//    snprintf(p->filepath, sizeof(p->filepath), "%s", filepath);
    g_free(filepath);
  }
  g_free(lutfolder);
  gtk_widget_destroy(filechooser);
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_lut3d_gui_data_t *g = (dt_iop_lut3d_gui_data_t *)self->gui_data;
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;
  gchar *lutfolder = dt_conf_get_string("plugins/darkroom/lut3d/def_path");
//printf("gui_update, lut folder %s\n", lutfolder);
  gtk_entry_set_text(GTK_ENTRY(g->filepath), p->filepath);
  if (lutfolder[0] == 0)
  {
    dt_control_log(_("Lut folder not set in preferences (core options)"));
    gtk_widget_set_sensitive(g->hbox, FALSE);
  }
  else
  {
    gtk_widget_set_sensitive(g->hbox, TRUE);
  }
  g_free(lutfolder);
  dt_bauhaus_combobox_set(g->colorspace, p->colorspace);
  dt_bauhaus_slider_set_soft(g->middle_grey, p->middle_grey);
  dt_bauhaus_slider_set_soft(g->min_exposure, p->min_exposure);
  dt_bauhaus_slider_set_soft(g->dynamic_range, p->dynamic_range);
  if (p->colorspace == DT_IOP_LOG_RGB)
    gtk_widget_set_visible(g->log_options, TRUE);
  else
    gtk_widget_set_visible(g->log_options, FALSE);
}

void gui_init(dt_iop_module_t *self)
{
setvbuf(stdout, NULL, _IONBF, 0);
//printf("gui_init\n");
  self->gui_data = malloc(sizeof(dt_iop_lut3d_gui_data_t));
  dt_iop_lut3d_gui_data_t *g = (dt_iop_lut3d_gui_data_t *)self->gui_data;
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));

  g->hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(8));
  g->filepath = gtk_entry_new();
  gtk_box_pack_start(GTK_BOX(g->hbox), g->filepath, TRUE, TRUE, 0);
  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(g->filepath));
  gtk_widget_set_tooltip_text(g->filepath, _("the filepath (relative to lut folder) is saved with image (and not the lut data themselves)"));
  g_signal_connect(G_OBJECT(g->filepath), "changed", G_CALLBACK(filepath_callback), self);

  GtkWidget *button = dtgtk_button_new(dtgtk_cairo_paint_directory, CPF_DO_NOT_USE_BORDER, NULL);
  gtk_widget_set_size_request(button, DT_PIXEL_APPLY_DPI(18), DT_PIXEL_APPLY_DPI(18));
  gtk_widget_set_tooltip_text(button, _("select a png (haldclut) or a cube file"));
  gtk_box_pack_start(GTK_BOX(g->hbox), button, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), self);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->hbox), TRUE, TRUE, 0);

  g->colorspace = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->colorspace, NULL, _("application color space"));
  dt_bauhaus_combobox_add(g->colorspace, _("log RGB"));
  dt_bauhaus_combobox_add(g->colorspace, _("sRGB"));
  dt_bauhaus_combobox_add(g->colorspace, _("REC.709"));
  dt_bauhaus_combobox_add(g->colorspace, _("lin sRGB"));
  dt_bauhaus_combobox_add(g->colorspace, _("lin prophoto RGB"));
  dt_bauhaus_combobox_add(g->colorspace, _("log Fuji"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->colorspace) , TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->colorspace, _("select the color space in which the LUT has to be applied\n"
                                                 "if log RGB is desired, use unbreak module first then select linear RGB here\n"));
  g_signal_connect(G_OBJECT(g->colorspace), "value-changed", G_CALLBACK(colorspace_callback), self);

  g->interpolation = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->interpolation, NULL, _("interpolation"));
  dt_bauhaus_combobox_add(g->interpolation, _("tetrahedral"));
  dt_bauhaus_combobox_add(g->interpolation, _("trilinear"));
  dt_bauhaus_combobox_add(g->interpolation, _("pyramid"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->interpolation) , TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->interpolation, _("select the interpolation method\n"));
  g_signal_connect(G_OBJECT(g->interpolation), "value-changed", G_CALLBACK(interpolation_callback), self);

  // add collapsable section for those extra options that are generally not to be used

  g->log_options = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(self->widget), g->log_options, TRUE, TRUE, 0);
  if (p->colorspace == DT_IOP_LOG_RGB)
    gtk_widget_set_visible(g->log_options, TRUE);
  else
    gtk_widget_set_visible(g->log_options, FALSE);

  // middle_grey slider
  g->middle_grey = dt_bauhaus_slider_new_with_range(self, 0.1, 100., 0.5, p->middle_grey, 2);
  dt_bauhaus_widget_set_label(g->middle_grey, NULL, _("middle grey luma"));
  gtk_box_pack_start(GTK_BOX(g->log_options), g->middle_grey, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->middle_grey, "%.2f %%");
  gtk_widget_set_tooltip_text(g->middle_grey, _("adjust to match the average luma of the subject"));
  g_signal_connect(G_OBJECT(g->middle_grey), "value-changed", G_CALLBACK(middle_grey_callback), self);

  // Shadows range slider
  g->min_exposure = dt_bauhaus_slider_new_with_range(self, -16.0, -0.0, 0.1, p->min_exposure, 2);
  dt_bauhaus_slider_enable_soft_boundaries(g->min_exposure, -16., 16.0);
  dt_bauhaus_widget_set_label(g->min_exposure, NULL, _("black relative exposure"));
  gtk_box_pack_start(GTK_BOX(g->log_options), g->min_exposure, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->min_exposure, "%.2f EV");
  gtk_widget_set_tooltip_text(g->min_exposure, _("number of stops between middle grey and pure black\nthis is a reading a posemeter would give you on the scene"));
  g_signal_connect(G_OBJECT(g->min_exposure), "value-changed", G_CALLBACK(min_exposure_callback), self);

  // Dynamic range slider
  g->dynamic_range = dt_bauhaus_slider_new_with_range(self, 0.5, 16.0, 0.1, p->dynamic_range, 2);
  dt_bauhaus_slider_enable_soft_boundaries(g->dynamic_range, 0.01, 32.0);
  dt_bauhaus_widget_set_label(g->dynamic_range, NULL, _("dynamic range"));
  gtk_box_pack_start(GTK_BOX(g->log_options), g->dynamic_range, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->dynamic_range, "%.2f EV");
  gtk_widget_set_tooltip_text(g->dynamic_range, _("number of stops between pure black and pure white\nthis is a reading a posemeter would give you on the scene"));
  g_signal_connect(G_OBJECT(g->dynamic_range), "value-changed", G_CALLBACK(dynamic_range_callback), self);

  g->log2tolin = gtk_check_button_new_with_label(_("log2 to lin"));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(g->log2tolin), p->log2tolin);
  gtk_widget_set_tooltip_text(g->log2tolin, _("convert back to lin"));
  gtk_box_pack_start(GTK_BOX(g->log_options), g->log2tolin , TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->log2tolin), "toggled", G_CALLBACK(log2tolin_callback), self);
}

void gui_cleanup(dt_iop_module_t *self)
{
//printf("gui_cleanup\n");
  dt_iop_lut3d_gui_data_t *g = (dt_iop_lut3d_gui_data_t *)self->gui_data;
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(g->filepath));
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
