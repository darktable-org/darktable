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

// needs to be defined before any system header includes for control/conf.h to work in C++ code
#define __STDC_FORMAT_MACROS

#include <cstdio>
#include <cstdlib>
#include <memory>

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfStandardAttributes.h>
#include <OpenEXR/ImfThreading.h>
#include <OpenEXR/ImfOutputFile.h>

extern "C" {
#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/exif.h"
#include "common/imageio.h"
#include "common/imageio_exr.h"
#include "common/imageio_module.h"
#include "control/conf.h"
#include "imageio/format/imageio_format_api.h"
}
#include "common/imageio_exr.hh"

#ifdef __cplusplus
extern "C" {
#endif

DT_MODULE(5)

enum dt_imageio_exr_compression_t
{
  NO_COMPRESSION = 0,     // no compression
  RLE_COMPRESSION = 1,    // run length encoding
  ZIPS_COMPRESSION = 2,   // zlib compression, one scan line at a time
  ZIP_COMPRESSION = 3,    // zlib compression, in blocks of 16 scan lines
  PIZ_COMPRESSION = 4,    // piz-based wavelet compression
  PXR24_COMPRESSION = 5,  // lossy 24-bit float compression
  B44_COMPRESSION = 6,    // lossy 4-by-4 pixel block compression,
                          // fixed compression rate
  B44A_COMPRESSION = 7,   // lossy 4-by-4 pixel block compression,
                          // flat fields are compressed more
  DWAA_COMPRESSION = 8,   // lossy DCT based compression, in blocks
                          // of 32 scanlines
  DWAB_COMPRESSION = 9,   // lossy DCT based compression, in blocks
                          // of 256 scanlines
  NUM_COMPRESSION_METHODS // number of different compression methods
};                        // copy of Imf::Compression

enum dt_imageio_exr_pixeltype_t
{
  EXR_PT_UINT = 0,  // unsigned int (32 bit)
  EXR_PT_HALF = 1,  // half (16 bit floating point)
  EXR_PT_FLOAT = 2, // float (32 bit floating point)
  NUM_PIXELTYPES    // number of different pixel types
};                  // copy of Imf::PixelType

typedef struct dt_imageio_exr_t
{
  dt_imageio_module_data_t global;
  dt_imageio_exr_compression_t compression;
  dt_imageio_exr_pixeltype_t pixel_type;
} dt_imageio_exr_t;

typedef struct dt_imageio_exr_gui_t
{
  GtkWidget *bpp;
  GtkWidget *compression;
} dt_imageio_exr_gui_t;

void init(dt_imageio_module_format_t *self)
{
#ifdef USE_LUA
  luaA_enum(darktable.lua_state.state, dt_imageio_exr_compression_t);
  luaA_enum_value_name(darktable.lua_state.state, dt_imageio_exr_compression_t, NO_COMPRESSION, "off");
  luaA_enum_value_name(darktable.lua_state.state, dt_imageio_exr_compression_t, RLE_COMPRESSION, "rle");
  luaA_enum_value_name(darktable.lua_state.state, dt_imageio_exr_compression_t, ZIPS_COMPRESSION, "zips");
  luaA_enum_value_name(darktable.lua_state.state, dt_imageio_exr_compression_t, ZIP_COMPRESSION, "zip");
  luaA_enum_value_name(darktable.lua_state.state, dt_imageio_exr_compression_t, PIZ_COMPRESSION, "piz");
  luaA_enum_value_name(darktable.lua_state.state, dt_imageio_exr_compression_t, PXR24_COMPRESSION, "pxr24");
  luaA_enum_value_name(darktable.lua_state.state, dt_imageio_exr_compression_t, B44_COMPRESSION, "b44");
  luaA_enum_value_name(darktable.lua_state.state, dt_imageio_exr_compression_t, B44A_COMPRESSION, "b44a");
  luaA_enum_value_name(darktable.lua_state.state, dt_imageio_exr_compression_t, DWAA_COMPRESSION, "dwaa");
  luaA_enum_value_name(darktable.lua_state.state, dt_imageio_exr_compression_t, DWAB_COMPRESSION, "dwab");

  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_exr_t, compression,
                                dt_imageio_exr_compression_t);

  luaA_enum(darktable.lua_state.state, dt_imageio_exr_pixeltype_t);
  luaA_enum_value_name(darktable.lua_state.state, dt_imageio_exr_pixeltype_t, EXR_PT_HALF, "half");
  luaA_enum_value_name(darktable.lua_state.state, dt_imageio_exr_pixeltype_t, EXR_PT_FLOAT, "float");

  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_exr_t, pixel_type,
                                dt_imageio_exr_pixeltype_t);
#endif
  Imf::BlobAttribute::registerAttributeType();
}

void cleanup(dt_imageio_module_format_t *self)
{
}

int write_image(dt_imageio_module_data_t *tmp, const char *filename, const void *in_tmp,
                dt_colorspaces_color_profile_type_t over_type, const char *over_filename,
                void *exif, int exif_len, int imgid, int num, int total, struct dt_dev_pixelpipe_t *pipe,
                const gboolean export_masks)
{
  const dt_imageio_exr_t *exr = (dt_imageio_exr_t *)tmp;

  Imf::setGlobalThreadCount(dt_get_num_threads());

  Imf::Header header(exr->global.width, exr->global.height, 1, Imath::V2f(0, 0), 1, Imf::INCREASING_Y,
                     (Imf::Compression)exr->compression);

  char comment[1024];
  snprintf(comment, sizeof(comment), "Developed using %s", darktable_package_string);

  header.insert("comment", Imf::StringAttribute(comment));

  if(exif && exif_len > 0)
  {
    Imf::Blob exif_blob(exif_len, (uint8_t *)exif);
    header.insert("exif", Imf::BlobAttribute(exif_blob));
  }

  char *xmp_string = dt_exif_xmp_read_string(imgid);
  if(xmp_string && strlen(xmp_string) > 0)
  {
    header.insert("xmp", Imf::StringAttribute(xmp_string));
    g_free(xmp_string);
  }

  // try to add the chromaticities
  if(imgid > 0)
  {
    cmsToneCurve *red_curve = NULL,
                 *green_curve = NULL,
                 *blue_curve = NULL;
    cmsCIEXYZ *red_color = NULL,
              *green_color = NULL,
              *blue_color = NULL;
    cmsHPROFILE out_profile = dt_colorspaces_get_output_profile(imgid, over_type, over_filename)->profile;
    float r[2], g[2], b[2], w[2];
    float sum;
    Imf::Chromaticities chromaticities;

    if(!cmsIsMatrixShaper(out_profile)) goto icc_error;

    red_curve = (cmsToneCurve *)cmsReadTag(out_profile, cmsSigRedTRCTag);
    green_curve = (cmsToneCurve *)cmsReadTag(out_profile, cmsSigGreenTRCTag);
    blue_curve = (cmsToneCurve *)cmsReadTag(out_profile, cmsSigBlueTRCTag);

    red_color = (cmsCIEXYZ *)cmsReadTag(out_profile, cmsSigRedColorantTag);
    green_color = (cmsCIEXYZ *)cmsReadTag(out_profile, cmsSigGreenColorantTag);
    blue_color = (cmsCIEXYZ *)cmsReadTag(out_profile, cmsSigBlueColorantTag);

    if(!red_curve || !green_curve || !blue_curve || !red_color || !green_color || !blue_color)
      goto icc_error;

    if(!cmsIsToneCurveLinear(red_curve) || !cmsIsToneCurveLinear(green_curve) || !cmsIsToneCurveLinear(blue_curve))
      goto icc_error;

//     printf("r: %f %f %f\n", red_color->X, red_color->Y, red_color->Z);
//     printf("g: %f %f %f\n", green_color->X, green_color->Y, green_color->Z);
//     printf("b: %f %f %f\n", blue_color->X, blue_color->Y, blue_color->Z);
//     printf("w: %f %f %f\n", white_point->X, white_point->Y, white_point->Z);

    sum = red_color->X + red_color->Y + red_color->Z;
    r[0] = red_color->X / sum;
    r[1] = red_color->Y / sum;
    sum = green_color->X + green_color->Y + green_color->Z;
    g[0] = green_color->X / sum;
    g[1] = green_color->Y / sum;
    sum = blue_color->X + blue_color->Y + blue_color->Z;
    b[0] = blue_color->X / sum;
    b[1] = blue_color->Y / sum;

    // hard code the white point to D50 as the primaries from the ICC should be adapted to that
    // calculated from D50 illuminant XYZ values in ICC specs
    w[0] = 0.345702915;
    w[1] = 0.358538597;

    chromaticities.red = Imath::V2f(r[0], r[1]);
    chromaticities.green = Imath::V2f(g[0], g[1]);
    chromaticities.blue = Imath::V2f(b[0], b[1]);
    chromaticities.white = Imath::V2f(w[0], w[1]);

    Imf::addChromaticities(header, chromaticities);
    Imf::addWhiteLuminance(header, 1.0); // just assume 1 here

    goto icc_end;

icc_error:
    dt_control_log("%s", _("The selected output profile doesn't work well with EXR"));
    fprintf(stderr, "[exr export] warning: exporting with anything but linear matrix profiles might lead to wrong results when opening the image\n");
  }
icc_end:

  Imf::PixelType pixel_type = (Imf::PixelType)exr->pixel_type;

  header.channels().insert("R", Imf::Channel(pixel_type, 1, 1, true));
  header.channels().insert("G", Imf::Channel(pixel_type, 1, 1, true));
  header.channels().insert("B", Imf::Channel(pixel_type, 1, 1, true));

  Imf::OutputFile file(filename, header);

  Imf::FrameBuffer data;
  size_t stride;

  if(pixel_type == Imf::PixelType::FLOAT)
  {
    stride = 4 * sizeof(float);
    const float *in = (const float *)in_tmp;

    data.insert("R", Imf::Slice(pixel_type, (char *)(in + 0), stride,
                                stride * exr->global.width));

    data.insert("G", Imf::Slice(pixel_type, (char *)(in + 1), stride,
                                stride * exr->global.width));

    data.insert("B", Imf::Slice(pixel_type, (char *)(in + 2), stride,
                                stride * exr->global.width));

    file.setFrameBuffer(data);
    file.writePixels(exr->global.height);
  }
  else
  {
    const size_t width = exr->global.width;
    const size_t height = exr->global.height;
    stride = 3 * sizeof(unsigned short);
    unsigned short *out = (unsigned short *)malloc(stride * width * height);
    if(out == NULL)
      return 1;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(in_tmp, out, width, height) \
  schedule(simd:static) \
  collapse(2)
#endif
    for(size_t y = 0; y < height; y++)
    {
      for(size_t x = 0; x < width; x++)
      {
        const float *in_pixel = (const float *)in_tmp + 4 * ((y * width) + x);
        unsigned short *out_pixel = out + 3 * ((y * width) + x);

        out_pixel[0] = half(in_pixel[0]).bits();
        out_pixel[1] = half(in_pixel[1]).bits();
        out_pixel[2] = half(in_pixel[2]).bits();
      }
    }

    data.insert("R", Imf::Slice(pixel_type, (char *)(out + 0), stride,
                                stride * exr->global.width));

    data.insert("G", Imf::Slice(pixel_type, (char *)(out + 1), stride,
                                stride * exr->global.width));

    data.insert("B", Imf::Slice(pixel_type, (char *)(out + 2), stride,
                                stride * exr->global.width));

    file.setFrameBuffer(data);
    file.writePixels(exr->global.height);

    free(out);
  }

  return 0;
}

size_t params_size(dt_imageio_module_format_t *self)
{
  return sizeof(dt_imageio_exr_t);
}

void *legacy_params(dt_imageio_module_format_t *self, const void *const old_params,
                    const size_t old_params_size, const int old_version, const int new_version,
                    size_t *new_size)
{
  if(old_version == 1 && new_version == 5)
  {
    struct dt_imageio_exr_v1_t
    {
      int max_width, max_height;
      int width, height;
      char style[128];
    };

    const dt_imageio_exr_v1_t *o = (dt_imageio_exr_v1_t *)old_params;
    dt_imageio_exr_t *n = (dt_imageio_exr_t *)malloc(sizeof(dt_imageio_exr_t));

    n->global.max_width = o->max_width;
    n->global.max_height = o->max_height;
    n->global.width = o->width;
    n->global.height = o->height;
    g_strlcpy(n->global.style, o->style, sizeof(o->style));
    n->global.style_append = FALSE;
    n->compression = PIZ_COMPRESSION;
    n->pixel_type = EXR_PT_FLOAT;
    *new_size = self->params_size(self);
    return n;
  }
  if(old_version == 2 && new_version == 5)
  {
    struct dt_imageio_exr_v2_t
    {
      int max_width, max_height;
      int width, height;
      char style[128];
      dt_imageio_exr_compression_t compression;
      dt_imageio_exr_pixeltype_t pixel_type;
    };

    const dt_imageio_exr_v2_t *o = (dt_imageio_exr_v2_t *)old_params;
    dt_imageio_exr_t *n = (dt_imageio_exr_t *)malloc(sizeof(dt_imageio_exr_t));

    // last param was dropped (pixel type)
    n->global.max_width = o->max_width;
    n->global.max_height = o->max_height;
    n->global.width = o->width;
    n->global.height = o->height;
    g_strlcpy(n->global.style, o->style, sizeof(o->style));
    n->global.style_append = FALSE;
    n->compression = o->compression;
    n->pixel_type = o->pixel_type >= EXR_PT_HALF ? o->pixel_type : EXR_PT_FLOAT;
    *new_size = self->params_size(self);
    return n;
  }
  if(old_version == 3 && new_version == 5)
  {
    struct dt_imageio_exr_v3_t
    {
      int max_width, max_height;
      int width, height;
      char style[128];
      dt_imageio_exr_compression_t compression;
    };

    const dt_imageio_exr_v3_t *o = (dt_imageio_exr_v3_t *)old_params;
    dt_imageio_exr_t *n = (dt_imageio_exr_t *)malloc(sizeof(dt_imageio_exr_t));

    n->global.max_width = o->max_width;
    n->global.max_height = o->max_height;
    n->global.width = o->width;
    n->global.height = o->height;
    g_strlcpy(n->global.style, o->style, sizeof(o->style));
    n->global.style_append = FALSE;
    n->compression = o->compression;
    n->pixel_type = EXR_PT_FLOAT;
    *new_size = self->params_size(self);
    return n;
  }
  if(old_version == 4 && new_version == 5)
  {
    struct dt_imageio_exr_v4_t
    {
      dt_imageio_module_data_t global;
      dt_imageio_exr_compression_t compression;
    };

    const dt_imageio_exr_v4_t *o = (dt_imageio_exr_v4_t *)old_params;
    dt_imageio_exr_t *n = (dt_imageio_exr_t *)malloc(sizeof(dt_imageio_exr_t));

    n->global.max_width = o->global.max_width;
    n->global.max_height = o->global.max_height;
    n->global.width = o->global.width;
    n->global.height = o->global.height;
    g_strlcpy(n->global.style, o->global.style, sizeof(o->global.style));
    n->global.style_append = o->global.style_append;
    n->compression = o->compression;
    n->pixel_type = EXR_PT_FLOAT;
    *new_size = self->params_size(self);
    return n;
  }
  return NULL;
}

void *get_params(dt_imageio_module_format_t *self)
{
  dt_imageio_exr_t *d = (dt_imageio_exr_t *)calloc(1, sizeof(dt_imageio_exr_t));
  d->compression = (dt_imageio_exr_compression_t)dt_conf_get_int("plugins/imageio/format/exr/compression");
  const int bpp = dt_conf_get_int("plugins/imageio/format/exr/bpp");
  d->pixel_type = (dt_imageio_exr_pixeltype_t)(bpp >> 4);
  return d;
}

void free_params(dt_imageio_module_format_t *self, dt_imageio_module_data_t *params)
{
  free(params);
}

int set_params(dt_imageio_module_format_t *self, const void *params, const int size)
{
  if(size != (int)self->params_size(self)) return 1;
  dt_imageio_exr_t *d = (dt_imageio_exr_t *)params;
  dt_imageio_exr_gui_t *g = (dt_imageio_exr_gui_t *)self->gui_data;
  dt_bauhaus_combobox_set(g->bpp, d->pixel_type - EXR_PT_HALF);
  dt_bauhaus_combobox_set(g->compression, d->compression);
  return 0;
}

int bpp(dt_imageio_module_data_t *p)
{
  return 32;  // always request float, any conversion is done internally
}

int levels(dt_imageio_module_data_t *p)
{
  return IMAGEIO_RGB | IMAGEIO_FLOAT;
}

const char *mime(dt_imageio_module_data_t *data)
{
  return "image/x-exr";
}

const char *extension(dt_imageio_module_data_t *data)
{
  return "exr";
}

const char *name()
{
  return _("OpenEXR (16/32-bit float)");
}

static void bpp_combobox_changed(GtkWidget *widget, gpointer user_data)
{
  const int pixel_type = dt_bauhaus_combobox_get(widget) + EXR_PT_HALF;
  dt_conf_set_int("plugins/imageio/format/exr/bpp", pixel_type << 4);
}

static void compression_combobox_changed(GtkWidget *widget, gpointer user_data)
{
  const int compression = dt_bauhaus_combobox_get(widget);
  dt_conf_set_int("plugins/imageio/format/exr/compression", compression);
}

void gui_init(dt_imageio_module_format_t *self)
{
  self->gui_data = malloc(sizeof(dt_imageio_exr_gui_t));
  dt_imageio_exr_gui_t *gui = (dt_imageio_exr_gui_t *)self->gui_data;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  // Bit depth combo box
  const int bpp_last = dt_conf_get_int("plugins/imageio/format/exr/bpp");

  gui->bpp = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(gui->bpp, NULL, N_("Bit depth"));

  dt_bauhaus_combobox_add(gui->bpp, _("16 bit"));
  dt_bauhaus_combobox_add(gui->bpp, _("32 bit"));
  dt_bauhaus_combobox_set(gui->bpp, (bpp_last >> 4) - EXR_PT_HALF);
  gtk_box_pack_start(GTK_BOX(self->widget), gui->bpp, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(gui->bpp), "value-changed", G_CALLBACK(bpp_combobox_changed), NULL);

  // Compression combo box
  const int compression_last = dt_conf_get_int("plugins/imageio/format/exr/compression");

  gui->compression = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(gui->compression, NULL, N_("Compression"));

  dt_bauhaus_combobox_add(gui->compression, _("Uncompressed"));
  dt_bauhaus_combobox_add(gui->compression, _("RLE"));
  dt_bauhaus_combobox_add(gui->compression, _("ZIPS"));
  dt_bauhaus_combobox_add(gui->compression, _("ZIP"));
  dt_bauhaus_combobox_add(gui->compression, _("PIZ"));
  dt_bauhaus_combobox_add(gui->compression, _("PXR24"));
  dt_bauhaus_combobox_add(gui->compression, _("B44"));
  dt_bauhaus_combobox_add(gui->compression, _("B44A"));
  dt_bauhaus_combobox_add(gui->compression, _("DWAA"));
  dt_bauhaus_combobox_add(gui->compression, _("DWAB"));
  dt_bauhaus_combobox_set(gui->compression, compression_last);
  gtk_box_pack_start(GTK_BOX(self->widget), gui->compression, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(gui->compression), "value-changed", G_CALLBACK(compression_combobox_changed), NULL);
}

void gui_cleanup(dt_imageio_module_format_t *self)
{
  free(self->gui_data);
}

void gui_reset(dt_imageio_module_format_t *self)
{
  dt_imageio_exr_gui_t *gui = (dt_imageio_exr_gui_t *)self->gui_data;
  const int bpp = dt_confgen_get_int("plugins/imageio/format/exr/bpp", DT_DEFAULT);
  dt_bauhaus_combobox_set(gui->bpp, (bpp >> 4) - EXR_PT_HALF);
  dt_bauhaus_combobox_set(gui->compression, dt_confgen_get_int("plugins/imageio/format/exr/compression", DT_DEFAULT));
}



#ifdef __cplusplus
}
#endif
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
