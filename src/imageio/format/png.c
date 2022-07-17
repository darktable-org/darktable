/*
    This file is part of darktable,
    Copyright (C) 2010-2021 darktable developers.

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

#include <inttypes.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>

#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
#include "control/conf.h"
#include "imageio/format/imageio_format_api.h"

DT_MODULE(3)

typedef struct dt_imageio_png_t
{
  dt_imageio_module_data_t global;
  int bpp;
  int compression;
  FILE *f;
  png_structp png_ptr;
  png_infop info_ptr;
} dt_imageio_png_t;

typedef struct dt_imageio_png_gui_t
{
  GtkWidget *bit_depth;
  GtkWidget *compression;
} dt_imageio_png_gui_t;

/* Write EXIF data to PNG file.
 * Code copied from DigiKam's libs/dimg/loaders/pngloader.cpp.
 * The EXIF embedding is defined by ImageMagicK.
 * It is documented in the ExifTool page:
 * http://www.sno.phy.queensu.ca/~phil/exiftool/TagNames/PNG.html
 *
 * ..and in turn copied from ufraw. thanks to udi and colleagues
 * for making useful code much more readable and discoverable ;)
 */

static void PNGwriteRawProfile(png_struct *ping, png_info *ping_info, char *profile_type, guint8 *profile_data,
                               png_uint_32 length)
{
  png_textp text;
  long i;
  guint8 *sp;
  png_charp dp;
  png_uint_32 allocated_length, description_length;

  const guint8 hex[16] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };
  text = png_malloc(ping, sizeof(png_text));
  description_length = strlen(profile_type);
  allocated_length = length * 2 + (length >> 5) + 20 + description_length;

  text[0].text = png_malloc(ping, allocated_length);
  text[0].key = png_malloc(ping, 80);
  text[0].key[0] = '\0';

  g_strlcat(text[0].key, "Raw profile type ", 80);
  g_strlcat(text[0].key, profile_type, 80);

  sp = profile_data;
  dp = text[0].text;
  *dp++ = '\n';

  g_strlcpy(dp, profile_type, allocated_length);

  dp += description_length;
  *dp++ = '\n';
  *dp = '\0';

  g_snprintf(dp, allocated_length - strlen(text[0].text), "%8lu ", (unsigned long int)length);

  dp += 8;

  for(i = 0; i < (long)length; i++)
  {
    if(i % 36 == 0) *dp++ = '\n';

    *(dp++) = hex[((*sp >> 4) & 0x0f)];
    *(dp++) = hex[((*sp++) & 0x0f)];
  }

  *dp++ = '\n';
  *dp = '\0';
  text[0].text_length = (dp - text[0].text);
  text[0].compression = -1;

  if(text[0].text_length <= allocated_length) png_set_text(ping, ping_info, text, 1);

  png_free(ping, text[0].text);
  png_free(ping, text[0].key);
  png_free(ping, text);
}

int write_image(dt_imageio_module_data_t *p_tmp, const char *filename, const void *ivoid,
                dt_colorspaces_color_profile_type_t over_type, const char *over_filename,
                void *exif, int exif_len, int imgid, int num, int total, struct dt_dev_pixelpipe_t *pipe,
                const gboolean export_masks)
{
  dt_imageio_png_t *p = (dt_imageio_png_t *)p_tmp;
  const int width = p->global.width, height = p->global.height;
  FILE *f = g_fopen(filename, "wb");
  if(!f) return 1;

  png_structp png_ptr;
  png_infop info_ptr;

  png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if(!png_ptr)
  {
    fclose(f);
    return 1;
  }

  info_ptr = png_create_info_struct(png_ptr);
  if(!info_ptr)
  {
    fclose(f);
    png_destroy_write_struct(&png_ptr, NULL);
    return 1;
  }

  if(setjmp(png_jmpbuf(png_ptr)))
  {
    fclose(f);
    png_destroy_write_struct(&png_ptr, &info_ptr);
    return 1;
  }

  png_init_io(png_ptr, f);

  png_set_compression_level(png_ptr, p->compression);
  png_set_compression_mem_level(png_ptr, 8);
  png_set_compression_strategy(png_ptr, Z_DEFAULT_STRATEGY);
  png_set_compression_window_bits(png_ptr, 15);
  png_set_compression_method(png_ptr, 8);
  png_set_compression_buffer_size(png_ptr, 8192);

  png_set_IHDR(png_ptr, info_ptr, width, height, p->bpp, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

  // metadata has to be written before the pixels

  // embed icc profile
  cmsHPROFILE out_profile = dt_colorspaces_get_output_profile(imgid, over_type, over_filename)->profile;
  uint32_t len = 0;
  cmsSaveProfileToMem(out_profile, NULL, &len);
  if(len > 0)
  {
    char *buf = malloc(sizeof(char) * len);
    if(buf)
    {
      cmsSaveProfileToMem(out_profile, buf, &len);
      char name[512] = { 0 };
      dt_colorspaces_get_profile_name(out_profile, "en", "US", name, sizeof(name));

      png_set_iCCP(png_ptr, info_ptr, *name ? name : "icc", 0,
#if(PNG_LIBPNG_VER < 10500)
                    (png_charp)buf,
#else
                    (png_const_bytep)buf,
#endif
                    len);
      free(buf);
    }
  }

  // write exif data
  if(exif && exif_len > 0)
  {
    /* The legacy tEXt chunk storage scheme implies the "Exif\0\0" APP1 prefix */
    uint8_t *buf = malloc(exif_len + 6);
    if(buf)
    {
      memcpy(buf, "Exif\0\0", 6);
      memcpy(buf + 6, exif, exif_len);
      PNGwriteRawProfile(png_ptr, info_ptr, "exif", buf, exif_len + 6);
      free(buf);
    }
  }

  png_write_info(png_ptr, info_ptr);

  /*
   * Get rid of filler (OR ALPHA) bytes, pack XRGB/RGBX/ARGB/RGBA into
   * RGB (4 channels -> 3 channels). The second parameter is not used.
   */
  png_set_filler(png_ptr, 0, PNG_FILLER_AFTER);

  png_bytep *row_pointers = dt_alloc_align(64, sizeof(png_bytep) * height);

  if(p->bpp > 8)
  {
    /* swap bytes of 16 bit files to most significant bit first */
    png_set_swap(png_ptr);

    for(unsigned i = 0; i < height; i++) row_pointers[i] = (png_bytep)((uint16_t *)ivoid + (size_t)4 * i * width);
  }
  else
  {
    for(unsigned i = 0; i < height; i++) row_pointers[i] = (uint8_t *)ivoid + (size_t)4 * i * width;
  }

  png_write_image(png_ptr, row_pointers);

  dt_free_align(row_pointers);

  png_write_end(png_ptr, info_ptr);
  png_destroy_write_struct(&png_ptr, &info_ptr);
  fclose(f);
  return 0;
}

static int __attribute__((__unused__)) read_header(const char *filename, dt_imageio_module_data_t *p_tmp)
{
  dt_imageio_png_t *png = (dt_imageio_png_t *)p_tmp;
  png->f = g_fopen(filename, "rb");

  if(!png->f) return 1;

#define NUM_BYTES_CHECK (8)

  png_byte dat[NUM_BYTES_CHECK];

  size_t cnt = fread(dat, 1, NUM_BYTES_CHECK, png->f);

  if(cnt != NUM_BYTES_CHECK || png_sig_cmp(dat, (png_size_t)0, NUM_BYTES_CHECK))
  {
    fclose(png->f);
    return 1;
  }

  png->png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);

  if(!png->png_ptr)
  {
    fclose(png->f);
    return 1;
  }

  png->info_ptr = png_create_info_struct(png->png_ptr);
  if(!png->info_ptr)
  {
    fclose(png->f);
    png_destroy_read_struct(&png->png_ptr, NULL, NULL);
    return 1;
  }

  if(setjmp(png_jmpbuf(png->png_ptr)))
  {
    fclose(png->f);
    png_destroy_read_struct(&png->png_ptr, &png->info_ptr, NULL);
    return 1;
  }

  png_init_io(png->png_ptr, png->f);

  // we checked some bytes
  png_set_sig_bytes(png->png_ptr, NUM_BYTES_CHECK);

  // image info
  png_read_info(png->png_ptr, png->info_ptr);

  uint32_t bit_depth = png_get_bit_depth(png->png_ptr, png->info_ptr);
  uint32_t color_type = png_get_color_type(png->png_ptr, png->info_ptr);

  // image input transformations

  // palette => rgb
  if(color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png->png_ptr);

  // 1, 2, 4 bit => 8 bit
  if(color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png->png_ptr);

  // strip alpha channel
  if(color_type & PNG_COLOR_MASK_ALPHA) png_set_strip_alpha(png->png_ptr);

  // grayscale => rgb
  if(color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_gray_to_rgb(png->png_ptr);

  // png->bytespp = 3*bit_depth/8;
  png->global.width = png_get_image_width(png->png_ptr, png->info_ptr);
  png->global.height = png_get_image_height(png->png_ptr, png->info_ptr);

  return 0;

#undef NUM_BYTES_CHECK
}

#if 0
int dt_imageio_png_read_assure_8(dt_imageio_png_t *png)
{
  if (setjmp(png_jmpbuf(png->png_ptr)))
  {
    fclose(png->f);
    png_destroy_read_struct(&png->png_ptr, NULL, NULL);
    return 1;
  }
  uint32_t bit_depth = png_get_bit_depth(png->png_ptr, png->info_ptr);
  // strip down to 8 bit channels
  if (bit_depth == 16)
    png_set_strip_16(png->png_ptr);

  return 0;
}
#endif

int read_image(dt_imageio_module_data_t *p_tmp, uint8_t *out)
{
  dt_imageio_png_t *png = (dt_imageio_png_t *)p_tmp;
  if(setjmp(png_jmpbuf(png->png_ptr)))
  {
    fclose(png->f);
    png_destroy_read_struct(&png->png_ptr, &png->info_ptr, NULL);
    return 1;
  }

  png_bytep row_pointer = (png_bytep)out;
  unsigned long rowbytes = png_get_rowbytes(png->png_ptr, png->info_ptr);

  for(int y = 0; y < png->global.height; y++)
  {
    png_read_row(png->png_ptr, row_pointer, NULL);
    row_pointer += rowbytes;
  }

  png_read_end(png->png_ptr, png->info_ptr);
  png_destroy_read_struct(&png->png_ptr, &png->info_ptr, NULL);

  fclose(png->f);
  return 0;
}

size_t params_size(dt_imageio_module_format_t *self)
{
  return sizeof(dt_imageio_module_data_t) + 2 * sizeof(int);
}

void *legacy_params(dt_imageio_module_format_t *self, const void *const old_params, const size_t old_params_size,
                    const int old_version, const int new_version, size_t *new_size)
{
  if(old_version == 1 && new_version == 3)
  {
    typedef struct dt_imageio_png_v1_t
    {
      int max_width, max_height;
      int width, height;
      char style[128];
      int bpp;
      FILE *f;
      png_structp png_ptr;
      png_infop info_ptr;
    } dt_imageio_png_v1_t;

    dt_imageio_png_v1_t *o = (dt_imageio_png_v1_t *)old_params;
    dt_imageio_png_t *n = (dt_imageio_png_t *)malloc(sizeof(dt_imageio_png_t));

    n->global.max_width = o->max_width;
    n->global.max_height = o->max_height;
    n->global.width = o->width;
    n->global.height = o->height;
    g_strlcpy(n->global.style, o->style, sizeof(o->style));
    n->global.style_append = FALSE;
    n->bpp = o->bpp;
    n->compression = Z_BEST_COMPRESSION;
    n->f = o->f;
    n->png_ptr = o->png_ptr;
    n->info_ptr = o->info_ptr;
    *new_size = self->params_size(self);
    return n;
  }
  else if(old_version == 2 && new_version == 3)
  {
    typedef struct dt_imageio_png_v2_t
    {
      int max_width, max_height;
      int width, height;
      char style[128];
      gboolean style_append;
      int bpp;
      FILE *f;
      png_structp png_ptr;
      png_infop info_ptr;
    } dt_imageio_png_v2_t;

    dt_imageio_png_v2_t *o = (dt_imageio_png_v2_t *)old_params;
    dt_imageio_png_t *n = (dt_imageio_png_t *)malloc(sizeof(dt_imageio_png_t));

    n->global.max_width = o->max_width;
    n->global.max_height = o->max_height;
    n->global.width = o->width;
    n->global.height = o->height;
    g_strlcpy(n->global.style, o->style, sizeof(o->style));
    n->global.style_append = o->style_append;
    n->bpp = o->bpp;
    n->compression = Z_BEST_COMPRESSION;
    n->f = o->f;
    n->png_ptr = o->png_ptr;
    n->info_ptr = o->info_ptr;
    *new_size = self->params_size(self);
    return n;
  }
  return NULL;
}

void *get_params(dt_imageio_module_format_t *self)
{
  dt_imageio_png_t *d = (dt_imageio_png_t *)calloc(1, sizeof(dt_imageio_png_t));
  const char *bpp = dt_conf_get_string_const("plugins/imageio/format/png/bpp");
  d->bpp = atoi(bpp);
  if(d->bpp != 8 && d->bpp != 16)
    d->bpp = 8;

  // PNG compression level might actually be zero!
  if(!dt_conf_key_exists("plugins/imageio/format/png/compression"))
    d->compression = 5;
  else
  {
    d->compression = dt_conf_get_int("plugins/imageio/format/png/compression");
    if(d->compression < 0 || d->compression > 9) d->compression = 5;
  }

  return d;
}

void free_params(dt_imageio_module_format_t *self, dt_imageio_module_data_t *params)
{
  free(params);
}

int set_params(dt_imageio_module_format_t *self, const void *params, const int size)
{
  if(size != self->params_size(self)) return 1;
  const dt_imageio_png_t *d = (dt_imageio_png_t *)params;
  const dt_imageio_png_gui_t *g = (dt_imageio_png_gui_t *)self->gui_data;
  if(d->bpp < 12)
    dt_bauhaus_combobox_set(g->bit_depth, 0);
  else
    dt_bauhaus_combobox_set(g->bit_depth, 1);
  dt_conf_set_int("plugins/imageio/format/png/bpp", d->bpp);
  dt_bauhaus_slider_set(g->compression, d->compression);
  dt_conf_set_int("plugins/imageio/format/png/compression", d->compression);
  return 0;
}

int dimension(struct dt_imageio_module_format_t *self, struct dt_imageio_module_data_t *data, uint32_t *width,
              uint32_t *height)
{
  /* maximum dimensions supported by PNG images */
  *width = 2147483647U;
  *height = 2147483647U;
  return 1;
}

int bpp(dt_imageio_module_data_t *p)
{
  return ((dt_imageio_png_t *)p)->bpp;
}

int levels(dt_imageio_module_data_t *p)
{
  return IMAGEIO_RGB | (((dt_imageio_png_t *)p)->bpp == 8 ? IMAGEIO_INT8 : IMAGEIO_INT16);
}

const char *mime(dt_imageio_module_data_t *data)
{
  return "image/png";
}

const char *extension(dt_imageio_module_data_t *data)
{
  return "png";
}

const char *name()
{
  return _("PNG (8/16-bit)");
}

static void bit_depth_changed(GtkWidget *widget, gpointer user_data)
{
  const int bpp = (dt_bauhaus_combobox_get(widget) == 0 ? 8 : 16);
  dt_conf_set_int("plugins/imageio/format/png/bpp", bpp);
}

static void compression_level_changed(GtkWidget *slider, gpointer user_data)
{
  const int compression = (int)dt_bauhaus_slider_get(slider);
  dt_conf_set_int("plugins/imageio/format/png/compression", compression);
}

void init(dt_imageio_module_format_t *self)
{
#ifdef USE_LUA
  luaA_struct(darktable.lua_state.state, dt_imageio_png_t);
  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_png_t, bpp, int);
#endif
}
void cleanup(dt_imageio_module_format_t *self)
{
}

void gui_init(dt_imageio_module_format_t *self)
{
  dt_imageio_png_gui_t *gui = (dt_imageio_png_gui_t *)malloc(sizeof(dt_imageio_png_gui_t));
  self->gui_data = (void *)gui;
  const char *conf_bpp = dt_conf_get_string_const("plugins/imageio/format/png/bpp");
  int bpp = atoi(conf_bpp);

  // PNG compression level might actually be zero!
  int compression = 5;
  if(dt_conf_key_exists("plugins/imageio/format/png/compression"))
    compression = dt_conf_get_int("plugins/imageio/format/png/compression");

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  // Bit depth combo box
  gui->bit_depth = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(gui->bit_depth, NULL, N_("bit depth"));
  dt_bauhaus_combobox_add(gui->bit_depth, _("8 bit"));
  dt_bauhaus_combobox_add(gui->bit_depth, _("16 bit"));
  if(bpp == 16)
    dt_bauhaus_combobox_set(gui->bit_depth, 1);
  else {
    bpp = 8; // We know only about 8 or 16 bits, at least for now
    dt_bauhaus_combobox_set(gui->bit_depth, 0);
  }
  gtk_box_pack_start(GTK_BOX(self->widget), gui->bit_depth, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(gui->bit_depth), "value-changed", G_CALLBACK(bit_depth_changed), NULL);

  // Compression level slider
  gui->compression = dt_bauhaus_slider_new_with_range(NULL,
                                                      dt_confgen_get_int("plugins/imageio/format/png/compression", DT_MIN),
                                                      dt_confgen_get_int("plugins/imageio/format/png/compression", DT_MAX),
                                                      1,
                                                      dt_confgen_get_int("plugins/imageio/format/png/compression", DT_DEFAULT),
                                                      0);
  dt_bauhaus_widget_set_label(gui->compression, NULL, N_("compression"));
  dt_bauhaus_slider_set(gui->compression, compression);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(gui->compression), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(gui->compression), "value-changed", G_CALLBACK(compression_level_changed), NULL);
}

void gui_cleanup(dt_imageio_module_format_t *self)
{
  free(self->gui_data);
}

void gui_reset(dt_imageio_module_format_t *self)
{
  dt_imageio_png_gui_t *gui = (dt_imageio_png_gui_t *)self->gui_data;
  dt_bauhaus_combobox_set(gui->bit_depth, 0); // 8bpp
  dt_bauhaus_slider_set(gui->compression, dt_confgen_get_int("plugins/imageio/format/png/compression", DT_DEFAULT));
}

int flags(dt_imageio_module_data_t *data)
{
  return FORMAT_FLAGS_SUPPORT_XMP;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
