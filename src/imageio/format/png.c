/*
    This file is part of darktable,
    Copyright (C) 2010-2024 darktable developers.

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
#include "control/conf.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_module.h"
#include "imageio/format/imageio_format_api.h"

#include <exiv2/exv_conf.h>

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

#if !defined(PNG_eXIf_SUPPORTED) || (EXIV2_MAJOR_VERSION < 1 && EXIV2_MINOR_VERSION <= 27)
static void PNGwriteRawProfile(png_struct *ping,
                               png_info *ping_info,
                               char *profile_type,
                               guint8 *profile_data,
                               png_uint_32 length)
{
  png_textp text;
  long i;
  guint8 *sp;
  png_charp dp;
  png_uint_32 allocated_length, description_length;

  const guint8 hex[16] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
                           'a', 'b', 'c', 'd', 'e', 'f' };
  text = png_malloc(ping, sizeof(png_text));
  if(!text)
  {
    dt_print(DT_DEBUG_ALWAYS,"[png] out of memory adding profile to image");
    return;
  }
  description_length = strlen(profile_type);
  allocated_length = length * 2 + (length >> 5) + 20 + description_length;

  text[0].text = png_malloc(ping, allocated_length);
  text[0].key = png_malloc(ping, 80);
  if(!text[0].text || !text[0].key)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[png] out of memory adding profile to image");
    goto cleanup;
  }
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

  g_snprintf(dp, allocated_length - strlen(text[0].text), "%8lu ",
             (unsigned long int)length);

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

cleanup:
  png_free(ping, text[0].text);
  png_free(ping, text[0].key);
  png_free(ping, text);
}
#endif

int write_image(dt_imageio_module_data_t *p_tmp,
                const char *filename,
                const void *ivoid,
                dt_colorspaces_color_profile_type_t over_type,
                const char *over_filename,
                void *exif,
                int exif_len,
                dt_imgid_t imgid,
                int num,
                int total,
                struct dt_dev_pixelpipe_t *pipe,
                const gboolean export_masks)
{
  dt_imageio_png_t *p = (dt_imageio_png_t *)p_tmp;
  const int width = p->global.width;
  const int height = p->global.height;
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

  // determine the actual (export vs colorout) color profile used
  const dt_colorspaces_color_profile_t *cp =
    dt_colorspaces_get_output_profile(imgid, over_type, over_filename);
  cmsHPROFILE out_profile = cp->profile;

#ifdef PNG_iCCP_SUPPORTED
  // embed ICC profile regardless of cICP later (compliant readers
  // shall check cICP first)
  uint32_t len = 0;
  cmsSaveProfileToMem(out_profile, NULL, &len);
  if(len > 0)
  {
    png_bytep buf = malloc(sizeof(png_byte) * len);
    if(buf)
    {
      cmsSaveProfileToMem(out_profile, buf, &len);
      char name[512] = { 0 };
      dt_colorspaces_get_profile_name(out_profile, "en", "US", name, sizeof(name));
      png_set_iCCP(png_ptr, info_ptr, *name ? name : "icc", 0, buf, len);
      free(buf);
    }
  }
#endif

  // write exif data
  if(exif && exif_len > 0)
  {
#if defined(PNG_eXIf_SUPPORTED) && (EXIV2_MAJOR_VERSION >= 1 || EXIV2_MINOR_VERSION > 27)
    png_set_eXIf_1(png_ptr, info_ptr, (uint32_t)exif_len, (png_bytep)exif);
#else
    /* The legacy tEXt chunk storage scheme implies the "Exif\0\0" APP1 prefix */
    uint8_t *buf = malloc(exif_len + 6);
    if(buf)
    {
      memcpy(buf, "Exif\0\0", 6);
      memcpy(buf + 6, exif, exif_len);
      PNGwriteRawProfile(png_ptr, info_ptr, "exif", buf, exif_len + 6);
      free(buf);
    }
#endif
  }

  png_write_info(png_ptr, info_ptr);

  /*
   * If possible, we want libpng to save the color encoding in a new
   * cICP chunk as well (see https://www.w3.org/TR/png-3/#cICP-chunk).
   * If we are unable to find the required color encoding data we have
   * anyway provided an iCCP chunk (and hope we could at least do that!).
   *
   * Must come after png_write_info() for the time being.
   * TODO: use known cICP chunk write support API once added to libpng
   */
  png_byte data[4] = {
    DT_CICP_COLOR_PRIMARIES_UNSPECIFIED, DT_CICP_TRANSFER_CHARACTERISTICS_UNSPECIFIED,
    DT_CICP_MATRIX_COEFFICIENTS_IDENTITY,
    1, // full range
  };

  switch(cp->type)
  {
    case DT_COLORSPACE_SRGB:
      data[0] = DT_CICP_COLOR_PRIMARIES_REC709;
      data[1] = DT_CICP_TRANSFER_CHARACTERISTICS_SRGB;
      break;
    case DT_COLORSPACE_REC709:
      data[0] = DT_CICP_COLOR_PRIMARIES_REC709;
      data[1] = DT_CICP_TRANSFER_CHARACTERISTICS_REC709;
      break;
    case DT_COLORSPACE_LIN_REC709:
      data[0] = DT_CICP_COLOR_PRIMARIES_REC709;
      data[1] = DT_CICP_TRANSFER_CHARACTERISTICS_LINEAR;
      break;
    case DT_COLORSPACE_LIN_REC2020:
      data[0] = DT_CICP_COLOR_PRIMARIES_REC2020;
      data[1] = DT_CICP_TRANSFER_CHARACTERISTICS_LINEAR;
      break;
    case DT_COLORSPACE_PQ_REC2020:
      data[0] = DT_CICP_COLOR_PRIMARIES_REC2020;
      data[1] = DT_CICP_TRANSFER_CHARACTERISTICS_PQ;
      break;
    case DT_COLORSPACE_HLG_REC2020:
      data[0] = DT_CICP_COLOR_PRIMARIES_REC2020;
      data[1] = DT_CICP_TRANSFER_CHARACTERISTICS_HLG;
      break;
    case DT_COLORSPACE_PQ_P3:
      data[0] = DT_CICP_COLOR_PRIMARIES_P3;
      data[1] = DT_CICP_TRANSFER_CHARACTERISTICS_PQ;
      break;
    case DT_COLORSPACE_HLG_P3:
      data[0] = DT_CICP_COLOR_PRIMARIES_P3;
      data[1] = DT_CICP_TRANSFER_CHARACTERISTICS_HLG;
      break;
    case DT_COLORSPACE_DISPLAY_P3:
      data[0] = DT_CICP_COLOR_PRIMARIES_P3;
      data[1] = DT_CICP_TRANSFER_CHARACTERISTICS_SRGB;
      break;
    default:
      break;
  }

  if(data[0] != DT_CICP_COLOR_PRIMARIES_UNSPECIFIED
     && data[1] != DT_CICP_TRANSFER_CHARACTERISTICS_UNSPECIFIED)
  {
    const png_byte chunk_name[5] = "cICP";
    png_write_chunk(png_ptr, chunk_name, data, 4);
  }

  /*
   * Get rid of filler (OR ALPHA) bytes, pack XRGB/RGBX/ARGB/RGBA into
   * RGB (4 channels -> 3 channels). The second parameter is not used.
   */
  png_set_filler(png_ptr, 0, PNG_FILLER_AFTER);

  png_bytep *row_pointers = dt_alloc_align_type(png_bytep, height);
  if(row_pointers)
  {
    if(p->bpp > 8)
    {
      /* swap bytes of 16 bit files to most significant bit first */
      png_set_swap(png_ptr);

      for(unsigned i = 0; i < height; i++)
        row_pointers[i] = (png_bytep)((uint16_t *)ivoid + (size_t)4 * i * width);
    }
    else
    {
      for(unsigned i = 0; i < height; i++)
        row_pointers[i] = (uint8_t *)ivoid + (size_t)4 * i * width;
    }

    png_write_image(png_ptr, row_pointers);

    dt_free_align(row_pointers);
  }
  else
  {
    dt_print(DT_DEBUG_ALWAYS, "[png] out of memory writing %s", filename);
  }
  png_write_end(png_ptr, info_ptr);
  png_destroy_write_struct(&png_ptr, &info_ptr);
  fclose(f);
  return 0;
}

static int __attribute__((__unused__)) read_header(const char *filename,
                                                   dt_imageio_module_data_t *p_tmp)
{
  dt_imageio_png_t *png = (dt_imageio_png_t *)p_tmp;
  png->f = g_fopen(filename, "rb");

  if(!png->f) return 1;

#define NUM_BYTES_CHECK (8)

  png_byte dat[NUM_BYTES_CHECK];

  size_t cnt = fread(dat, 1, NUM_BYTES_CHECK, png->f);

  if(cnt != NUM_BYTES_CHECK
     || png_sig_cmp(dat, (png_size_t)0, NUM_BYTES_CHECK))
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

  const uint32_t bit_depth = png_get_bit_depth(png->png_ptr, png->info_ptr);
  const uint32_t color_type = png_get_color_type(png->png_ptr, png->info_ptr);

  // image input transformations

  // palette => rgb
  if(color_type == PNG_COLOR_TYPE_PALETTE)
    png_set_palette_to_rgb(png->png_ptr);

  // 1, 2, 4 bit => 8 bit
  if(color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
    png_set_expand_gray_1_2_4_to_8(png->png_ptr);

  // strip alpha channel
  if(color_type & PNG_COLOR_MASK_ALPHA)
    png_set_strip_alpha(png->png_ptr);

  // grayscale => rgb
  if(color_type == PNG_COLOR_TYPE_GRAY
     || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
  {
    png_set_gray_to_rgb(png->png_ptr);
  }

  // png->bytespp = 3*bit_depth/8;
  png->global.width = png_get_image_width(png->png_ptr, png->info_ptr);
  png->global.height = png_get_image_height(png->png_ptr, png->info_ptr);

  return 0;

#undef NUM_BYTES_CHECK
}

int read_image(dt_imageio_module_data_t *p_tmp,
               uint8_t *out)
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

void *legacy_params(dt_imageio_module_format_t *self,
                    const void *const old_params,
                    const size_t old_params_size,
                    const int old_version,
                    int *new_version,
                    size_t *new_size)
{
  typedef struct dt_imageio_png_v3_t
  {
    dt_imageio_module_data_t global;
    int bpp;
    int compression;
    FILE *f;
    png_structp png_ptr;
    png_infop info_ptr;
  } dt_imageio_png_v3_t;

  if(old_version == 1)
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

    const dt_imageio_png_v1_t *o = (dt_imageio_png_v1_t *)old_params;
    dt_imageio_png_v3_t *n = malloc(sizeof(dt_imageio_png_v3_t));

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

    *new_version = 3;
    *new_size = sizeof(dt_imageio_module_data_t) + 2 * sizeof(int);
    return n;
  }
  else if(old_version == 2)
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

    const dt_imageio_png_v2_t *o = (dt_imageio_png_v2_t *)old_params;
    dt_imageio_png_v3_t *n = malloc(sizeof(dt_imageio_png_v3_t));

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

    *new_version = 3;
    *new_size = sizeof(dt_imageio_module_data_t) + 2 * sizeof(int);
    return n;
  }


  // incremental update supported:
  /*
  typedef struct dt_imageio_png_v4_t
  {
    ...
  } dt_imageio_png_v4_t;

  if(old_version == 3)
  {
    // let's update from 3 to 4

    ...
    *new_size = sizeof(dt_imageio_module_data_t) + 2 * sizeof(int);
    *new_version = 4;
    return n;
  }
  */
  return NULL;
}

void *get_params(dt_imageio_module_format_t *self)
{
  dt_imageio_png_t *d = calloc(1, sizeof(dt_imageio_png_t));
  d->bpp = dt_conf_get_int("plugins/imageio/format/png/bpp");
  if(d->bpp != 8 && d->bpp != 16)
    d->bpp = 8;

  // PNG compression level might actually be zero!
  if(!dt_conf_key_exists("plugins/imageio/format/png/compression"))
  {
    d->compression = 5;
  }
  else
  {
    d->compression = dt_conf_get_int("plugins/imageio/format/png/compression");
    if(d->compression < 0
       || d->compression > 9)
    {
      d->compression = 5;
    }
  }

  return d;
}

void free_params(dt_imageio_module_format_t *self,
                 dt_imageio_module_data_t *params)
{
  free(params);
}

int set_params(dt_imageio_module_format_t *self,
               const void *params,
               const int size)
{
  if(size != self->params_size(self)) return 1;
  const dt_imageio_png_t *d = (dt_imageio_png_t *)params;
  const dt_imageio_png_gui_t *g = self->gui_data;
  if(d->bpp < 12)
    dt_bauhaus_combobox_set(g->bit_depth, 0);
  else
    dt_bauhaus_combobox_set(g->bit_depth, 1);
  dt_conf_set_int("plugins/imageio/format/png/bpp", d->bpp);
  dt_bauhaus_slider_set(g->compression, d->compression);
  dt_conf_set_int("plugins/imageio/format/png/compression", d->compression);
  return 0;
}

int dimension(struct dt_imageio_module_format_t *self,
              struct dt_imageio_module_data_t *data,
              uint32_t *width,
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
  return _("PNG");
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
  dt_imageio_png_gui_t *gui = malloc(sizeof(dt_imageio_png_gui_t));
  self->gui_data = (void *)gui;
  const int bpp = dt_conf_get_int("plugins/imageio/format/png/bpp");

  // PNG compression level might actually be zero!
  int compression = 5;
  if(dt_conf_key_exists("plugins/imageio/format/png/compression"))
    compression = dt_conf_get_int("plugins/imageio/format/png/compression");

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  // Bit depth combo box
  DT_BAUHAUS_COMBOBOX_NEW_FULL(gui->bit_depth, self, NULL, N_("bit depth"), NULL,
                               0, bit_depth_changed, self,
                               N_("8 bit"), N_("16 bit"));
  if(bpp == 16)
    dt_bauhaus_combobox_set(gui->bit_depth, 1);
  gtk_box_pack_start(GTK_BOX(self->widget), gui->bit_depth, TRUE, TRUE, 0);

  // Compression level slider
  gui->compression = dt_bauhaus_slider_new_with_range
    ((dt_iop_module_t*)self,
     dt_confgen_get_int("plugins/imageio/format/png/compression", DT_MIN),
     dt_confgen_get_int("plugins/imageio/format/png/compression", DT_MAX),
     1,
     dt_confgen_get_int("plugins/imageio/format/png/compression", DT_DEFAULT),
     0);
  dt_bauhaus_widget_set_label(gui->compression, NULL, N_("compression"));
  dt_bauhaus_slider_set(gui->compression, compression);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(gui->compression), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(gui->compression), "value-changed",
                   G_CALLBACK(compression_level_changed), NULL);
}

void gui_cleanup(dt_imageio_module_format_t *self)
{
  free(self->gui_data);
}

void gui_reset(dt_imageio_module_format_t *self)
{
  dt_imageio_png_gui_t *gui = self->gui_data;
  dt_bauhaus_combobox_set(gui->bit_depth, 0); // 8bpp
  dt_bauhaus_slider_set(gui->compression,
                        dt_confgen_get_int("plugins/imageio/format/png/compression",
                                           DT_DEFAULT));
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
