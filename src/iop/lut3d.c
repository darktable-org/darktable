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

typedef struct dt_iop_lut3d_params_t
{
  char filepath[512];
  uint8_t colorspace;
} dt_iop_lut3d_params_t;

typedef struct dt_iop_lut3d_gui_data_t
{
  GtkWidget *filepath;
  GtkWidget *colorspace;
} dt_iop_lut3d_gui_data_t;

typedef struct dt_iop_lut3d_global_data_t
{
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

// From `HaldCLUT_correct.c' by Eskil Steenberg (http://www.quelsolaar.com) (BSD licensed)
void correct_pixel(float *input, float *output, float *clut, unsigned int level)
{
  for(int c = 0; c < 3; ++c) input[c] = input[c] < 0.0f ? 0.0f : (input[c] > 1.0f ? 1.0f : input[c]);
  int color, red, green, blue, i, j;
  float tmp[6], r, g, b;
//  level *= level;

  red = input[0] * (float)(level - 1);
  if(red > level - 2) red = (float)level - 2;
  if(red < 0) red = 0;

  green = input[1] * (float)(level - 1);
  if(green > level - 2) green = (float)level - 2;
  if(green < 0) green = 0;

  blue = input[2] * (float)(level - 1);
  if(blue > level - 2) blue = (float)level - 2;
  if(blue < 0) blue = 0;

  r = input[0] * (float)(level - 1) - red;
  g = input[1] * (float)(level - 1) - green;
  b = input[2] * (float)(level - 1) - blue;

  color = red + green * level + blue * level * level;

  i = color * 3;
  j = (color + 1) * 3;

  tmp[0] = clut[i++] * (1 - r) + clut[j++] * r;
  tmp[1] = clut[i++] * (1 - r) + clut[j++] * r;
  tmp[2] = clut[i] * (1 - r) + clut[j] * r;

  i = (color + level) * 3;
  j = (color + level + 1) * 3;

  tmp[3] = clut[i++] * (1 - r) + clut[j++] * r;
  tmp[4] = clut[i++] * (1 - r) + clut[j++] * r;
  tmp[5] = clut[i] * (1 - r) + clut[j] * r;

  output[0] = tmp[0] * (1 - g) + tmp[3] * g;
  output[1] = tmp[1] * (1 - g) + tmp[4] * g;
  output[2] = tmp[2] * (1 - g) + tmp[5] * g;

  i = (color + level * level) * 3;
  j = (color + level * level + 1) * 3;

  tmp[0] = clut[i++] * (1 - r) + clut[j++] * r;
  tmp[1] = clut[i++] * (1 - r) + clut[j++] * r;
  tmp[2] = clut[i] * (1 - r) + clut[j] * r;

  i = (color + level + level * level) * 3;
  j = (color + level + level * level + 1) * 3;

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

uint8_t process_haldclut(dt_dev_pixelpipe_iop_t *piece, float **clut)
{
  dt_iop_lut3d_params_t *params = (dt_iop_lut3d_params_t *)piece->data;
  dt_imageio_png_t png;
  if(read_header(params->filepath, &png))
  {
    fprintf(stderr, "[lut3d] invalid png header from `%s'\n", params->filepath);
    return 0;
  }
  dt_print(DT_DEBUG_DEV, "[lut3d] png: width=%d, height=%d, color_type=%d, bit_depth=%d\n", png.width,
           png.height, png.color_type, png.bit_depth);
  uint8_t level = 2;
  while(level * level * level < png.width) ++level;
  if(level * level * level != png.width)
  {
    fprintf(stderr, "[lut3d] invalid level in png file %d %d\n", level, png.width);
    fclose(png.f);
    png_destroy_read_struct(&png.png_ptr, &png.info_ptr, NULL);
    return 0;
  }
  const size_t buf_size = (size_t)png.height * png_get_rowbytes(png.png_ptr, png.info_ptr);
  dt_print(DT_DEBUG_DEV, "[lut3d] allocating %zu bytes for png lut\n", buf_size);
  uint8_t *buf = dt_alloc_align(16, buf_size);
  if(!buf)
  {
    fclose(png.f);
    png_destroy_read_struct(&png.png_ptr, &png.info_ptr, NULL);
    fprintf(stderr, "[lut3d] error allocating buffer for png lut\n");
    return 0;
  }
  if(read_image(&png, (void *)buf))
  {
    dt_free_align(buf);
    fprintf(stderr, "[lut3d] could not read png image `%s'\n", params->filepath);
    return 0;
  }
  float *lclut = dt_alloc_align(16, buf_size * sizeof(float));
  if(!lclut)
  {
    dt_free_align(buf);
    fprintf(stderr, "[lut3d] error allocating buffer for png lut\n");
    return 0;
  }
  for(size_t i = 0; i < buf_size; ++i)
  {
    lclut[i] = buf[i] / 256.0f;
  }
  dt_free_align(buf);
  *clut = lclut;
//printf("haldclut ok - level %d, size %d; clut %p\n", level, buf_size, lclut);
  return level;
}

uint8_t parse_line(char *line, char *token)
{
  uint8_t i, c;
  i = c = 0;
  char *t = token;
  char *l = line;

//printf("input line %s size %d last %02hhx\n", line, strlen(line), line[strlen(line)-1]);
  while (*l != 0 && c < 3 && i < 50)
  {
    if (*l == '#' || *l == '\n' || *l == '\r')
    {
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
    if (*l == ' ')
    {
      if (i > 0)
      {
        *t = 0;
        c++;
        i = 0;
        t = token + (50 * c);
      }
    }
    else
    {
      *t = *l;
      t++;
      i++;
    }
    l++;
  }
  return c;
}
uint8_t process_cube(dt_dev_pixelpipe_iop_t *piece, float **clut)
{
  dt_iop_lut3d_params_t *params = (dt_iop_lut3d_params_t *)piece->data;
  FILE *cube_file;
  char *line = NULL;
  size_t len = 0;
  ssize_t read;
  char token[3][50];
  uint8_t level = 0;
  float *lclut = NULL;
  uint32_t i = 0;
  uint32_t buf_size = 0;

  if(!(cube_file = g_fopen(params->filepath, "r")))
  {
    fprintf(stderr, "[lut3d] invalid cube file from `%s'\n", params->filepath);
    return 0;
  }
//printf("cube file opened\n");
  while ((read = getline(&line, &len, cube_file)) != -1)
  {
    uint8_t nb_token = parse_line(line, &token[0][0]);
    if (nb_token)
    {
//printf("line parsed: %d tokens: %s, %s, %s\n", nb_token, (nb_token > 0) ? token[0] : "", (nb_token > 1) ? token[1] : "", (nb_token > 2) ? token[2] : "" );
      if (token[0][0] == 'T') continue;
      if (strcmp("DOMAIN_MIN", token[0]) == 0)
      {
//        printf("->Domain min %d\n", atoll(token[1]));
      }
      else if (strcmp("DOMAIN_MAX", token[0]) == 0)
      {
//        printf("->Domain max %d\n", atoll(token[1]));
      }
      else if (strcmp("LUT_1D_SIZE", token[0]) == 0)
      {
        fprintf(stderr, "[lut3d] 1D cube lut not supported\n");
        free(line);
        fclose(cube_file);
        return 0;
//        printf("->Lut 1D %d\n", atoll(token[1]));
      }
      else if (strcmp("LUT_3D_SIZE", token[0]) == 0)
      {
        level = atoll(token[1]);
        buf_size = level * level * level * 3;
        lclut = dt_alloc_align(16, buf_size * sizeof(float));
//printf("->Lut 3D %d buf_size %d\n", level, buf_size);
        if(!lclut)
        {
          fprintf(stderr, "[lut3d] error allocating buffer for cube lut\n");
          free(line);
          fclose(cube_file);
          return 0;
        }
      }
      else
      {
        if (!level)
        {
          fprintf(stderr, "[lut3d] error cube lut size not defined\n");
          dt_free_align(lclut);
          free(line);
          fclose(cube_file);
          return 0;
        }
        for (int j=0; j < 3; j++) lclut[i+j] = strtod(token[j], NULL);
        i += 3;
//printf("->Values %f %f %f\n", strtod(token[0], NULL), strtod(token[1], NULL), strtod(token[2], NULL));
      }
    }
  }
  if (i != buf_size || i == 0)
  {
    fprintf(stderr, "[lut3d] error cube lut lines number not correct\n");
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

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ibuf, void *const obuf,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)piece->data;
  const int ch = piece->colors;
  const int colorspace = p->colorspace;
  float *clut = NULL;
  uint8_t level = 0;

  if (!g_str_has_suffix (p->filepath, ".cube"))
  {
    level = process_haldclut(piece, &clut);
    level *= level;
  }
  else
  {
    level = process_cube(piece, &clut);
  }
  if (clut)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) shared(clut, level)
#endif
    for(int j = 0; j < roi_out->height; j++)
    {
      float *in = ((float *)ibuf) + (size_t)ch * roi_in->width * j;
      float *out = ((float *)obuf) + (size_t)ch * roi_out->width * j;
      for(int i = 0; i < roi_out->width; i++)
      {
        float lin[3] = {0, 0, 0};
        float lout[3] = {0, 0, 0};
        if (colorspace == 0)
        { // linear RGB
          dt_Lab_to_prophotorgb(in, lin);
          correct_pixel(lin, lout, clut, level);
          dt_prophotorgb_to_Lab(lout, out);
        }
        else if (colorspace == 1) // gamma sRGB
        { // linear RGB
          dt_Lab_to_sRGB(in, lin);
          correct_pixel(lin, lout, clut, level);
          dt_sRGB_to_Lab(lout, out);
        }
        else if (colorspace == 2) // lab
        {
          lin[0] = in[0] / 100.0f;
          lin[1] = in[1] / 255.0f;
          lin[2] = in[2] / 255.0f;
          correct_pixel(lin, lout, clut, level);
          out[0] = lout[0] * 100.0f;
          out[1] = lout[1] * 255.0f;
          out[2] = lout[2] * 255.0f;
        }
        else if (colorspace == 3)
        { // XYZ
          dt_Lab_to_XYZ(in, lin);
          correct_pixel(lin, lout, clut, level);
          dt_XYZ_to_Lab(lout, out);
        }
        else for(int c = 0; c < 3; ++c) out[c] = in[c];
        in += ch;
        out += ch;
      }
    }
    dt_free_align(clut);
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
void reload_defaults(dt_iop_module_t *module)
{
}

void init(dt_iop_module_t *module)
{
//printf("init\n");
  module->data = NULL;
  module->params = calloc(1, sizeof(dt_iop_lut3d_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_lut3d_params_t));
  module->default_enabled = 0;
  module->priority = 680; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_lut3d_params_t);
  module->gui_data = NULL;
  dt_iop_lut3d_params_t tmp = (dt_iop_lut3d_params_t){ { "" }, 1 };

  memcpy(module->params, &tmp, sizeof(dt_iop_lut3d_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_lut3d_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  module->data = malloc(sizeof(dt_iop_lut3d_global_data_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  free(module->data);
  module->data = NULL;
}

static void filepath_callback(GtkWidget *w, dt_iop_module_t *self)
{
//printf("filepath_callback\n");
  if(darktable.gui->reset) return;
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;
  snprintf(p->filepath, sizeof(p->filepath), "%s", gtk_entry_get_text(GTK_ENTRY(w)));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void colorspace_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;
//  dt_iop_lut3d_gui_data_t *g = (dt_iop_lut3d_gui_data_t *)self->gui_data;
  p->colorspace = dt_bauhaus_combobox_get(widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void button_clicked(GtkWidget *widget, dt_iop_module_t *self)
{
  dt_iop_lut3d_gui_data_t *g = (dt_iop_lut3d_gui_data_t *)self->gui_data;
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new(
      _("select lut file"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_OPEN, _("_cancel"), GTK_RESPONSE_CANCEL,
      _("_select"), GTK_RESPONSE_ACCEPT, (char *)NULL);
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);

  if (strlen(p->filepath) == 0 || access(p->filepath, F_OK) == -1)
  {
    // not sure about this. How to init def_path ?
    gchar* def_path = dt_conf_get_string("plugins/darkroom/lut3d/def_path");
//printf("filepath = null; default path %s\n", def_path);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), def_path);
    g_free(def_path);
  }
  else
  {
//printf("filepath <> null\n");
    gtk_file_chooser_select_filename(GTK_FILE_CHOOSER(filechooser), p->filepath);
  }

  GtkFileFilter* filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_mime_type(filter, "image/png");
  gtk_file_filter_set_name(filter, _("hald cluts (png)"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*.cube");
  gtk_file_filter_set_name(filter, _("3Dlut (cube)"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*");
  gtk_file_filter_set_name(filter, _("all files"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *filepath = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
    gtk_entry_set_text(GTK_ENTRY(g->filepath), filepath);
    snprintf(p->filepath, sizeof(p->filepath), "%s", filepath);
    g_free(filepath);
  }
  gtk_widget_destroy(filechooser);
}

void gui_update(dt_iop_module_t *self)
{
//printf("gui_update\n");
  dt_iop_lut3d_gui_data_t *g = (dt_iop_lut3d_gui_data_t *)self->gui_data;
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;
  gtk_entry_set_text(GTK_ENTRY(g->filepath), p->filepath);
  dt_bauhaus_combobox_set(g->colorspace, p->colorspace);
}

void gui_init(dt_iop_module_t *self)
{
setvbuf(stdout, NULL, _IONBF, 0);
//printf("gui_init\n");
  self->gui_data = malloc(sizeof(dt_iop_lut3d_gui_data_t));
  dt_iop_lut3d_gui_data_t *g = (dt_iop_lut3d_gui_data_t *)self->gui_data;
//  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(8));
  g->filepath = gtk_entry_new();
  gtk_box_pack_start(GTK_BOX(hbox), g->filepath, TRUE, TRUE, 0);
  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(g->filepath));
  g_signal_connect(G_OBJECT(g->filepath), "changed", G_CALLBACK(filepath_callback), self);

  GtkWidget *button = dtgtk_button_new(dtgtk_cairo_paint_directory, CPF_DO_NOT_USE_BORDER, NULL);
  gtk_widget_set_size_request(button, DT_PIXEL_APPLY_DPI(18), DT_PIXEL_APPLY_DPI(18));
  gtk_widget_set_tooltip_text(button, _("select haldcult file"));
  gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), self);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);

  g->colorspace = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->colorspace, NULL, _("application color space"));
  dt_bauhaus_combobox_add(g->colorspace, _("linear RGB"));
  dt_bauhaus_combobox_add(g->colorspace, _("gamma sRGB"));
  dt_bauhaus_combobox_add(g->colorspace, _("LAB"));
  dt_bauhaus_combobox_add(g->colorspace, _("XYZ"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->colorspace) , TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->colorspace, _("select the color space in which the LUT has to be applied\n"
                                                 "if log RGB is desired, use unbreak module first then select linear RGB here\n"));
  g_signal_connect(G_OBJECT(g->colorspace), "value-changed", G_CALLBACK(colorspace_callback), self);

//  self->widget = hbox;
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
