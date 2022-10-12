/*
 *    This file is part of darktable,
 *    Copyright (C) 2016-2021 darktable developers.
 *
 *    darktable is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    darktable is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <lcms2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma GCC diagnostic ignored "-Wshadow"

#include "colorchart.h"

#define MAX_LINE_LENGTH 512

typedef enum parser_state_t {
  BLOCK_NONE = 0,
  BLOCK_BOXES,
  BLOCK_BOX_SHRINK,
  BLOCK_REF_ROTATION,
  BLOCK_XLIST,
  BLOCK_YLIST,
  BLOCK_EXPECTED
} parser_state_t;

void free_chart(chart_t *chart)
{
  if(!chart) return;
  g_list_free_full(chart->f_list, free);
  if(chart->d_table) g_hash_table_unref(chart->d_table);
  if(chart->box_table) g_hash_table_unref(chart->box_table);
  if(chart->patch_sets) g_hash_table_unref(chart->patch_sets);
  free(chart);
}

static char *parse_string(char **c)
{
  while(**c == ' ' || **c == '\t') (*c)++;
  char *result = *c;
  while(**c != ' ' && **c != '\t' && **c != '\0' && **c != '\n') (*c)++;
  *(*c)++ = '\0';
  return result;
}

static double parse_double(char **c)
{
  while(**c == ' ' || **c == '\t') (*c)++;
  double result = g_ascii_strtod(*c, c);
  *((*c) - 1) = '\0';
  return result;
}

// this is not the code from argyll but a rewrite!
static int strinc(char *label, size_t buffer_size)
{
  size_t label_len = strlen(label);
  char *c = label + label_len - 1;
  while(c >= label)
  {
    char carry_over = 0;
    switch(*c)
    {
      case 'z':
      case 'Z':
        *c -= 25;
        carry_over = *c;
        break;
      case '9':
        *c = '0';
        carry_over = '1';
        break;
      default:
        (*c)++;
    }
    if(!carry_over)
      break;
    else if(c == label)
    {
      if(label_len + 1 >= buffer_size) return 0;
      memmove(c + 1, c, label_len + 1);
      *c = carry_over;
    }
    c--;
  }
  return 1;
}

void checker_set_color(box_t *box, dt_colorspaces_color_profile_type_t color_space, float c0, float c1, float c2)
{
  box->color_space = color_space;
  box->color[0] = c0;
  box->color[1] = c1;
  box->color[2] = c2;

  dt_aligned_pixel_t Lab = { c0, c1, c2 };
  dt_aligned_pixel_t XYZ = { c0 * 0.01, c1 * 0.01, c2 * 0.01 };

  switch(color_space)
  {
    default:
    case DT_COLORSPACE_NONE:
      for(int c = 0; c < 3; c++) box->rgb[c] = 0.0;
      break;
    case DT_COLORSPACE_LAB:
      dt_Lab_to_XYZ(Lab, XYZ);
    case DT_COLORSPACE_XYZ:
      dt_XYZ_to_sRGB_clipped(XYZ, box->rgb);
      break;
  }
}

static void free_labels_list(gpointer data)
{
  g_list_free_full((GList *)data, g_free);
}

// In some environments ERROR is already defined, ie: WIN32
#if defined(ERROR)
#undef ERROR
#endif // defined (ERROR)

#define ERROR                                                                                                     \
  {                                                                                                               \
    lineno = __LINE__;                                                                                            \
    goto error;                                                                                                   \
  }
// according to cht_format.html from argyll:
// "The keywords and associated data must be used in the following order: BOXES, BOX_SHRINK, REF_ROTATION,
// XLIST, YLIST and EXPECTED."
chart_t *parse_cht(const char *filename)
{
  chart_t *result = (chart_t *)calloc(1, sizeof(chart_t));
  int lineno = 0;

  FILE *fp = g_fopen(filename, "rb");
  if(!fp)
  {
    fprintf(stderr, "error opening `%s'\n", filename);
    ERROR;
  }

  // parser control
  char line[MAX_LINE_LENGTH] = { 0 };
  parser_state_t last_block = BLOCK_NONE;
  int skip_block = 0;

  // data gathered from the CHT file
  unsigned int n_boxes;
  result->d_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free);
  result->box_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free);
  result->patch_sets = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_labels_list);

  float x_min = FLT_MAX, x_max = FLT_MIN, y_min = FLT_MAX, y_max = FLT_MIN;

  // main loop over the input file
  while(fgets(line, MAX_LINE_LENGTH, fp))
  {
    if(line[0] == '\0' || line[0] == '\n')
    {
      skip_block = 0;
      continue;
    }
    if(skip_block) continue;

    // we should be at the start of a block now
    char *c = line;
    ssize_t len = strlen(line);
    char *keyword = parse_string(&c);

    if(!g_strcmp0(keyword, "BOXES") && last_block < BLOCK_BOXES)
    {
      last_block = BLOCK_BOXES;
      if(c - line >= len) ERROR;
      n_boxes = parse_double(&c);

      // let's have another loop reading from the file.
      while(fgets(line, MAX_LINE_LENGTH, fp))
      {
        if(line[0] == '\0' || line[0] == '\n') break;

        char *c = line;
        ssize_t len = strlen(line);
        while(*c == ' ') c++;
        if(*c == 'F')
        {
          float x0, y0, x1, y1, x2, y2, x3, y3;
          // using sscanf would be nice, but parsing floats does only work with LANG=C
          // if(sscanf(line, " F _ _ %f %f %f %f %f %f %f %f", &x0, &y0, &x1, &y1, &x2, &y2, &x3, &y3) != 8)
          // ERROR;
          c++;
          while(*c == ' ') c++;
          if(*c++ != '_') ERROR;
          while(*c == ' ') c++;
          if(*c++ != '_') ERROR;
          while(*c == ' ') c++;
          if(c - line >= len) ERROR;
          x0 = parse_double(&c);
          if(c - line >= len) ERROR;
          y0 = parse_double(&c);
          if(c - line >= len) ERROR;
          x1 = parse_double(&c);
          if(c - line >= len) ERROR;
          y1 = parse_double(&c);
          if(c - line >= len) ERROR;
          x2 = parse_double(&c);
          if(c - line >= len) ERROR;
          y2 = parse_double(&c);
          if(c - line >= len) ERROR;
          x3 = parse_double(&c);
          if(c - line >= len) ERROR;
          y3 = parse_double(&c);

          x_min = MIN(x_min, x0);
          x_min = MIN(x_min, x1);
          x_min = MIN(x_min, x2);
          x_min = MIN(x_min, x3);

          y_min = MIN(y_min, y0);
          y_min = MIN(y_min, y1);
          y_min = MIN(y_min, y2);
          y_min = MIN(y_min, y3);

          x_max = MAX(x_max, x0);
          x_max = MAX(x_max, x1);
          x_max = MAX(x_max, x2);
          x_max = MAX(x_max, x3);

          y_max = MAX(y_max, y0);
          y_max = MAX(y_max, y1);
          y_max = MAX(y_max, y2);
          y_max = MAX(y_max, y3);

          f_line_t *l = (f_line_t *)malloc(sizeof(f_line_t));

          l->p[0].x = x0;
          l->p[0].y = y0;
          l->p[1].x = x1;
          l->p[1].y = y1;
          l->p[2].x = x2;
          l->p[2].y = y2;
          l->p[3].x = x3;
          l->p[3].y = y3;

          result->f_list = g_list_append(result->f_list, l);
        }
        // these get parsed the same way
        else if((*c == 'D') || (*c == 'X') || (*c == 'Y'))
        {
          char kl, *lxs, *lxe, *lys, *lye;
          float w, h, xo, yo, xi, yi;
          kl = *c;
          *c++ = '\0';

          if(c - line >= len) ERROR;
          lxs = parse_string(&c);
          if(c - line >= len) ERROR;
          lxe = parse_string(&c);
          if(c - line >= len) ERROR;
          lys = parse_string(&c);
          if(c - line >= len) ERROR;
          lye = parse_string(&c);

          if(c - line >= len) ERROR;
          w = parse_double(&c);
          if(c - line >= len) ERROR;
          h = parse_double(&c);
          if(c - line >= len) ERROR;
          xo = parse_double(&c);
          if(c - line >= len) ERROR;
          yo = parse_double(&c);
          if(c - line >= len) ERROR;
          xi = parse_double(&c);
          if(c - line >= len) ERROR;
          yi = parse_double(&c);

          x_min = MIN(x_min, xo);
          y_min = MIN(y_min, yo);

          size_t lxs_len = strlen(lxs), lxe_len = strlen(lxe), lys_len = strlen(lys), lye_len = strlen(lye);
          if(lxs_len > lxe_len || lys_len > lye_len) ERROR;

          // make sure there is enough room to add another char in the beginning
          const size_t x_label_size = lxe_len + 1;
          const size_t y_label_size = lye_len + 1;

          char *x_label = malloc(x_label_size);
          char *y_label = malloc(y_label_size);

          char *first_label = NULL, *last_label = NULL;
          GList *labels = NULL;

          float y = yo;
          memcpy(y_label, lys, lys_len + 1);
          while(1)
          {
            float x = xo;
            memcpy(x_label, lxs, lxs_len + 1);
            while(1)
            {
              // build the label of the box
              char *label;
              if(!g_strcmp0(x_label, "_"))
                label = g_strdup(y_label);
              else if(!g_strcmp0(y_label, "_"))
                label = g_strdup(x_label);
              else
              {
                if(kl == 'Y')
                  label = g_strconcat(y_label, x_label, NULL);
                else
                  label = g_strconcat(x_label, y_label, NULL);
              }

              if(!first_label) first_label = label;
              g_free(last_label);
              last_label = label;

              // store it
              box_t *box = calloc(1, sizeof(box_t));
              box->p.x = x;
              box->p.y = y;
              box->w = w;
              box->h = h;
              box->color_space = DT_COLORSPACE_NONE; // no color for this box yet
              if(kl == 'D')
                g_hash_table_insert(result->d_table, label, box);
              else
                g_hash_table_insert(result->box_table, label, box);
              if(kl == 'X' || kl == 'Y') labels = g_list_append(labels, g_strdup(label));

              // increment in x direction
              if(!g_strcmp0(x_label, lxe)) break;
              x += xi;
              if(!strinc(x_label, x_label_size))
              {
                free(y_label);
                free(x_label);
                ERROR;
              }
            }
            x_max = MAX(x_max, x + w);
            // increment in y direction
            if(!g_strcmp0(y_label, lye)) break;
            y += yi;
            if(!strinc(y_label, y_label_size))
            {
              free(y_label);
              free(x_label);
              ERROR;
            }
          }
          y_max = MAX(y_max, y + h);
          if(kl == 'X' || kl == 'Y')
            g_hash_table_insert(result->patch_sets, g_strdup_printf("%s .. %s", first_label, last_label), labels);

          g_free(last_label);
          free(y_label);
          free(x_label);
        }
        else
          ERROR;
      }

      if(n_boxes != g_hash_table_size(result->d_table) + g_hash_table_size(result->box_table)) ERROR;

      // all the box lines are read and we know the bounding box,
      // so let's scale all the values to have a bounding box with the longer side having length 1 and start
      // at (0, 0)

      result->bb_w = x_max - x_min;
      result->bb_h = y_max - y_min;

#define SCALE_X(x) x = (x - x_min) / result->bb_w
#define SCALE_Y(y) y = (y - y_min) / result->bb_h

      for(GList *iter = result->f_list; iter; iter = g_list_next(iter))
      {
        f_line_t *f = iter->data;
        for(int i = 0; i < 4; i++)
        {
          SCALE_X(f->p[i].x);
          SCALE_Y(f->p[i].y);
        }
      }

      GHashTableIter table_iter;
      gpointer key, value;

      g_hash_table_iter_init(&table_iter, result->d_table);
      while(g_hash_table_iter_next(&table_iter, &key, &value))
      {
        box_t *box = (box_t *)value;
        SCALE_X(box->p.x);
        SCALE_Y(box->p.y);
        box->w /= result->bb_w;
        box->h /= result->bb_h;
      }

      g_hash_table_iter_init(&table_iter, result->box_table);
      while(g_hash_table_iter_next(&table_iter, &key, &value))
      {
        box_t *box = (box_t *)value;
        SCALE_X(box->p.x);
        SCALE_Y(box->p.y);
        box->w /= result->bb_w;
        box->h /= result->bb_h;
      }

#undef SCALE_X
#undef SCALE_Y
    }
    else if(!g_strcmp0(keyword, "BOX_SHRINK") && last_block < BLOCK_BOX_SHRINK)
    {
      last_block = BLOCK_BOX_SHRINK;
      if(c - line >= len) ERROR;
      result->box_shrink = parse_double(&c);
    }
    else if(!g_strcmp0(keyword, "REF_ROTATION") && last_block < BLOCK_REF_ROTATION)
    {
      last_block = BLOCK_REF_ROTATION;
      if(c - line >= len) ERROR;
      result->ref_rotation = parse_double(&c);
    }
    else if(!g_strcmp0(keyword, "XLIST") && last_block < BLOCK_XLIST)
    {
      last_block = BLOCK_XLIST;
      // skip until empty line, we don't care about these
      skip_block = 1;
    }
    else if(!g_strcmp0(keyword, "YLIST") && last_block < BLOCK_YLIST)
    {
      last_block = BLOCK_YLIST;
      // skip until empty line, we don't care about these
      skip_block = 1;
    }
    else if(!g_strcmp0(keyword, "EXPECTED") && last_block < BLOCK_EXPECTED)
    {
      last_block = BLOCK_EXPECTED;
      dt_colorspaces_color_profile_type_t color_space = DT_COLORSPACE_NONE;
      if(c - line >= len) ERROR;
      char *cs = parse_string(&c);
      if(c - line >= len) ERROR;
      unsigned int n_colors = parse_double(&c);

      if(!g_strcmp0(cs, "XYZ"))
        color_space = DT_COLORSPACE_XYZ;
      else if(!g_strcmp0(cs, "LAB"))
        color_space = DT_COLORSPACE_LAB;
      else
        ERROR;

      // read and store the numbers.
      // we use them 1) to draw visual hints on the grid and 2) as a fallback reference set

      // let's have another loop reading from the file.
      while(fgets(line, MAX_LINE_LENGTH, fp))
      {
        if(line[0] == '\0' || line[0] == '\n') break;
        n_colors--;
        ssize_t len = strlen(line);
        char *c = line;

        char *label = parse_string(&c);
        box_t *box = (box_t *)g_hash_table_lookup(result->box_table, label);
        if(!box) ERROR;

        if(c - line >= len) ERROR;
        float c0 = parse_double(&c);
        if(c - line >= len) ERROR;
        float c1 = parse_double(&c);
        if(c - line >= len) ERROR;
        float c2 = parse_double(&c);
        checker_set_color(box, color_space, c0, c1, c2);
      }
      if(n_colors != 0) ERROR;
    }
    else
    {
      fprintf(stderr, "unknown keyword `%s'\n", keyword);
      ERROR;
    }
  }

  fprintf(stderr, "cht `%s' done\n", filename);
  goto end;

error:
  fprintf(stderr, "error parsing CHT file, (%s:%d)\n", __FUNCTION__, lineno);
  // clean up
  free_chart(result);
  result = NULL;

end:
  if(fp) fclose(fp);
  return result;
}

int parse_it8(const char *filename, chart_t *chart)
{
  int result = 1;
  cmsHANDLE hIT8 = cmsIT8LoadFromFile(NULL, filename);
  if(!hIT8)
  {
    fprintf(stderr, "error loading IT8 file `%s'\n", filename);
    goto error;
  }

  if(cmsIT8TableCount(hIT8) != 1)
  {
    fprintf(stderr, "error with the IT8 file, we only support files with one table at the moment\n");
    goto error;
  }

  dt_colorspaces_color_profile_type_t color_space = DT_COLORSPACE_NONE;
  int column_SAMPLE_ID = -1, column_X = -1, column_Y = -1, column_Z = -1, column_L = -1, column_a = -1,
      column_b = -1;
  char **sample_names = NULL;
  int n_columns = cmsIT8EnumDataFormat(hIT8, &sample_names);

  if(n_columns == -1)
  {
    fprintf(stderr, "error with the IT8 file, can't get column types\n");
    goto error;
  }

  for(int i = 0; i < n_columns; i++)
  {
    if(!g_strcmp0(sample_names[i], "SAMPLE_ID"))
      column_SAMPLE_ID = i;
    else if(!g_strcmp0(sample_names[i], "XYZ_X"))
      column_X = i;
    else if(!g_strcmp0(sample_names[i], "XYZ_Y"))
      column_Y = i;
    else if(!g_strcmp0(sample_names[i], "XYZ_Z"))
      column_Z = i;
    else if(!g_strcmp0(sample_names[i], "LAB_L"))
      column_L = i;
    else if(!g_strcmp0(sample_names[i], "LAB_A"))
      column_a = i;
    else if(!g_strcmp0(sample_names[i], "LAB_B"))
      column_b = i;
  }

  if(column_SAMPLE_ID == -1)
  {
    fprintf(stderr, "error with the IT8 file, can't find the SAMPLE_ID column\n");
    goto error;
  }

  char *columns[3] = { 0 };
  if(column_X != -1 && column_Y != -1 && column_Z != -1)
  {
    color_space = DT_COLORSPACE_XYZ;
    columns[0] = "XYZ_X";
    columns[1] = "XYZ_Y";
    columns[2] = "XYZ_Z";
  }
  else if(column_L != -1 && column_a != -1 && column_b != -1)
  {
    color_space = DT_COLORSPACE_LAB;
    columns[0] = "LAB_L";
    columns[1] = "LAB_A";
    columns[2] = "LAB_B";
  }
  else
  {
    fprintf(stderr, "error with the IT8 file, can't find XYZ or Lab columns\n");
    goto error;
  }

  GHashTableIter table_iter;
  gpointer key, value;

  g_hash_table_iter_init(&table_iter, chart->box_table);
  while(g_hash_table_iter_next(&table_iter, &key, &value))
  {
    box_t *box = (box_t *)value;

    if(cmsIT8GetData(hIT8, key, "SAMPLE_ID") == NULL)
    {
      fprintf(stderr, "error with the IT8 file, can't find sample `%s'\n", (char *)key);
      goto error;
    }

    checker_set_color(box, color_space, cmsIT8GetDataDbl(hIT8, key, columns[0]), cmsIT8GetDataDbl(hIT8, key, columns[1]),
              cmsIT8GetDataDbl(hIT8, key, columns[2]));
  }

  fprintf(stderr, "it8 `%s' done\n", filename);
  goto end;

error:
  result = 0;
end:
  if(hIT8) cmsIT8Free(hIT8);
  return result;
}

#undef MAX_LINE_LENGTH

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

