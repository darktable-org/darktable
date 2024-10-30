/*
    This file is part of darktable,
    Copyright (C) 2010-2023 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/exif.h"
#include "control/conf.h"
#include "develop/pixelpipe_hb.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_module.h"
#include "imageio/format/imageio_format_api.h"

#include "imageio/imageio_exr.hh"

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfOutputFile.h>
#include <OpenEXR/ImfStandardAttributes.h>
#include <OpenEXR/ImfThreading.h>

#include <cstdio>
#include <cstdlib>
#include <forward_list>
#include <memory>
#include <utility>

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
                void *exif, int exif_len, dt_imgid_t imgid, int num, int total, struct dt_dev_pixelpipe_t *pipe,
                const gboolean export_masks)
{
  const dt_imageio_exr_t *exr = (dt_imageio_exr_t *)tmp;

  Imf::setGlobalThreadCount(dt_get_num_threads());

  Imf::Header header(exr->global.width, exr->global.height, 1, Imath::V2f(0, 0), 1, Imf::INCREASING_Y,
                     (Imf::Compression)exr->compression);

  char comment[1024];
  snprintf(comment, sizeof(comment), "Created with %s", darktable_package_string);

  header.insert("comment", Imf::StringAttribute(comment));

  // TODO: workaround; remove when exiv2 implements EXR write support and use dt_exif_write_blob() at the end
  if(exif && exif_len > 0)
  {
    Imf::Blob exif_blob(exif_len, (uint8_t *)exif);
    header.insert("exif", Imf::BlobAttribute(exif_blob));
  }

  // TODO: workaround; remove when exiv2 implements EXR write support and update flags()
  // TODO: workaround; uses valid exif as a way to indicate ALL metadata was requested
  if(exif && exif_len > 0)
  {
    char *xmp_string = dt_exif_xmp_read_string(imgid);
    if(xmp_string && strlen(xmp_string) > 0)
    {
      header.insert("xmp", Imf::StringAttribute(xmp_string));
      g_free(xmp_string);
    }
  }

  // try to add the chromaticities
  cmsToneCurve *red_curve = NULL, *green_curve = NULL, *blue_curve = NULL;
  cmsCIEXYZ *red_color = NULL, *green_color = NULL, *blue_color = NULL;
  Imf::Chromaticities chromaticities; // initialized w/ Rec709 primaries and D65 white

  // determine the actual (export vs colorout) color profile used
  const dt_colorspaces_color_profile_t *cp = dt_colorspaces_get_output_profile(imgid, over_type, over_filename);

  if(!cmsIsMatrixShaper(cp->profile)) goto icc_error;

  red_curve = (cmsToneCurve *)cmsReadTag(cp->profile, cmsSigRedTRCTag);
  green_curve = (cmsToneCurve *)cmsReadTag(cp->profile, cmsSigGreenTRCTag);
  blue_curve = (cmsToneCurve *)cmsReadTag(cp->profile, cmsSigBlueTRCTag);

  red_color = (cmsCIEXYZ *)cmsReadTag(cp->profile, cmsSigRedColorantTag);
  green_color = (cmsCIEXYZ *)cmsReadTag(cp->profile, cmsSigGreenColorantTag);
  blue_color = (cmsCIEXYZ *)cmsReadTag(cp->profile, cmsSigBlueColorantTag);

  if(!red_curve || !green_curve || !blue_curve || !red_color || !green_color || !blue_color) goto icc_error;

  if(!cmsIsToneCurveLinear(red_curve) || !cmsIsToneCurveLinear(green_curve) || !cmsIsToneCurveLinear(blue_curve))
    goto icc_error;

  // calculate primaries only if white point is not D65
  if(cp->type != DT_COLORSPACE_LIN_REC709 && cp->type != DT_COLORSPACE_LIN_REC2020)
  {
    float r[2], g[2], b[2];
    double sum;

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
    chromaticities.white = Imath::V2f(cmsD50_xyY()->x, cmsD50_xyY()->y);

    chromaticities.red = Imath::V2f(r[0], r[1]);
    chromaticities.green = Imath::V2f(g[0], g[1]);
    chromaticities.blue = Imath::V2f(b[0], b[1]);
  }
  else if(cp->type == DT_COLORSPACE_LIN_REC2020)
  {
    chromaticities.red = Imath::V2f(0.7080f, 0.2920f);
    chromaticities.green = Imath::V2f(0.1700f, 0.7970f);
    chromaticities.blue = Imath::V2f(0.1310f, 0.0460f);
  }
  // else use Rec709 default

  Imf::addChromaticities(header, chromaticities);
  Imf::addWhiteLuminance(header, 1.0); // just assume 1 here

  goto icc_end;

icc_error:
  dt_control_log("%s", _("the selected output profile doesn't work well with EXR"));
  dt_print(DT_DEBUG_ALWAYS, "[exr export] warning: exporting with anything but linear matrix profiles might lead to wrong "
                  "results when opening the image\n");
icc_end:

  Imf::PixelType pixel_type = (Imf::PixelType)exr->pixel_type;

  header.channels().insert("R", Imf::Channel(pixel_type, 1, 1, true));
  header.channels().insert("G", Imf::Channel(pixel_type, 1, 1, true));
  header.channels().insert("B", Imf::Channel(pixel_type, 1, 1, true));

  Imf::FrameBuffer data;
  size_t stride;
  void *out_image = NULL;

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
  }
  else
  {
    const size_t width = exr->global.width;
    const size_t height = exr->global.height;
    stride = 3 * sizeof(unsigned short);
    out_image = dt_alloc_aligned(stride * width * height);
    if(out_image == NULL)
    {
      dt_print(DT_DEBUG_ALWAYS, "[exr export] error allocating image conversion buffer");
      return 1;
    }

    DT_OMP_FOR(collapse(2))
    for(size_t y = 0; y < height; y++)
    {
      for(size_t x = 0; x < width; x++)
      {
        const float *in_pixel = (const float *)in_tmp + 4 * ((y * width) + x);
        unsigned short *out_pixel = (unsigned short *)out_image + 3 * ((y * width) + x);

        out_pixel[0] = half(in_pixel[0]).bits();
        out_pixel[1] = half(in_pixel[1]).bits();
        out_pixel[2] = half(in_pixel[2]).bits();
      }
    }

    data.insert("R", Imf::Slice(pixel_type, (char *)out_image, stride,
                                stride * exr->global.width));

    data.insert("G", Imf::Slice(pixel_type, (char *)((unsigned short *)out_image + 1), stride,
                                stride * exr->global.width));

    data.insert("B", Imf::Slice(pixel_type, (char *)((unsigned short *)out_image + 2), stride,
                                stride * exr->global.width));
  }

  // add masks as additional channels
  // NB: GIMP does not support multi-part EXR files as layers yet
  //     (https://gitlab.gnome.org/GNOME/gimp/-/issues/4379)
  std::forward_list<std::pair<gboolean, void *> > mask_bufs;
  if(export_masks && pipe)
  {
    for(GList *iter = pipe->nodes; iter; iter = g_list_next(iter))
    {
      dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)iter->data;

      GHashTableIter rm_iter;
      gpointer key, value;

      g_hash_table_iter_init(&rm_iter, piece->raster_masks);
      while(g_hash_table_iter_next(&rm_iter, &key, &value))
      {
        const char *pagename = (char *)g_hash_table_lookup(piece->module->raster_mask.source.masks, key);
        std::string layername;
        if(pagename)
          layername = std::string(pagename);
        else
          layername = std::string(piece->module->name());
        layername += ".Y";

        header.channels().insert(layername, Imf::Channel(pixel_type, 1, 1, true));

        gboolean free_mask;
        float *raster_mask = dt_dev_get_raster_mask(piece, piece->module, GPOINTER_TO_INT(key), NULL, &free_mask);

        if(!raster_mask)
          return 1;

        if(pixel_type == Imf::PixelType::FLOAT)
        {
          stride = sizeof(float);

          data.insert(layername, Imf::Slice(pixel_type, (char *)raster_mask, stride, stride * exr->global.width));

          mask_bufs.emplace_front(free_mask, raster_mask);
        }
        else
        {
          const size_t width = exr->global.width;
          const size_t height = exr->global.height;
          stride = sizeof(unsigned short);
          void *out_mask = dt_alloc_aligned(stride * width * height);
          if(out_mask == NULL)
          {
            dt_print(DT_DEBUG_ALWAYS, "[exr export] error allocating mask conversion buffer");
            return 1;
          }

          DT_OMP_FOR(collapse(2))
          for(size_t y = 0; y < height; y++)
          {
            for(size_t x = 0; x < width; x++)
            {
              const float *in_pixel = (const float *)raster_mask + ((y * width) + x);
              unsigned short *out_pixel = (unsigned short *)out_mask + ((y * width) + x);

              out_pixel[0] = half(in_pixel[0]).bits();
            }
          }

          data.insert(layername, Imf::Slice(pixel_type, (char *)out_mask, stride, stride * exr->global.width));

          mask_bufs.emplace_front(TRUE, out_mask);

          if(free_mask) dt_free_align(raster_mask);
        }
      } // for all raster masks
    } // for all pipe nodes
  }

  // write out to file
  Imf::OutputFile file(filename, header);

  file.setFrameBuffer(data);
  file.writePixels(exr->global.height);

  // clean up
  dt_free_align(out_image);
  for(auto &mb : mask_bufs)
  {
    if(mb.first) dt_free_align(mb.second);
  }

  return 0;
}

size_t params_size(dt_imageio_module_format_t *self)
{
  return sizeof(dt_imageio_exr_t);
}

void *legacy_params(dt_imageio_module_format_t *self,
                    const void *const old_params,
                    const size_t old_params_size,
                    const int old_version,
                    int *new_version,
                    size_t *new_size)
{
  typedef struct _imageio_exr_v5_t
  {
    dt_imageio_module_data_t global;
    dt_imageio_exr_compression_t compression;
    dt_imageio_exr_pixeltype_t pixel_type;
  } dt_imageio_exr_v5_t;

  if(old_version == 1)
  {
    typedef struct _imageio_exr_v1_t
    {
      int max_width, max_height;
      int width, height;
      char style[128];
    } dt_imageio_exr_v1_t;

    const dt_imageio_exr_v1_t *o = (dt_imageio_exr_v1_t *)old_params;
    dt_imageio_exr_v5_t *n = (dt_imageio_exr_v5_t *)malloc(sizeof(dt_imageio_exr_v5_t));

    n->global.max_width = o->max_width;
    n->global.max_height = o->max_height;
    n->global.width = o->width;
    n->global.height = o->height;
    g_strlcpy(n->global.style, o->style, sizeof(o->style));
    n->global.style_append = FALSE;
    n->compression = PIZ_COMPRESSION;
    n->pixel_type = EXR_PT_FLOAT;

    *new_version = 5;
    *new_size = sizeof(dt_imageio_exr_v5_t);
    return n;
  }
  if(old_version == 2)
  {
    typedef struct _imageio_exr_v2_t
    {
      int max_width, max_height;
      int width, height;
      char style[128];
      dt_imageio_exr_compression_t compression;
      dt_imageio_exr_pixeltype_t pixel_type;
    } dt_imageio_exr_v2_t;

    const dt_imageio_exr_v2_t *o = (dt_imageio_exr_v2_t *)old_params;
    dt_imageio_exr_v5_t *n = (dt_imageio_exr_v5_t *)malloc(sizeof(dt_imageio_exr_v5_t));

    // last param was dropped (pixel type)
    n->global.max_width = o->max_width;
    n->global.max_height = o->max_height;
    n->global.width = o->width;
    n->global.height = o->height;
    g_strlcpy(n->global.style, o->style, sizeof(o->style));
    n->global.style_append = FALSE;
    n->compression = o->compression;
    n->pixel_type = o->pixel_type >= EXR_PT_HALF ? o->pixel_type : EXR_PT_FLOAT;

    *new_version = 5;
    *new_size = sizeof(dt_imageio_exr_v5_t);
    return n;
  }
  if(old_version == 3)
  {
    typedef struct _imageio_exr_v3_t
    {
      int max_width, max_height;
      int width, height;
      char style[128];
      dt_imageio_exr_compression_t compression;
    } dt_imageio_exr_v3_t;

    const dt_imageio_exr_v3_t *o = (dt_imageio_exr_v3_t *)old_params;
    dt_imageio_exr_v5_t *n = (dt_imageio_exr_v5_t *)malloc(sizeof(dt_imageio_exr_v5_t));

    n->global.max_width = o->max_width;
    n->global.max_height = o->max_height;
    n->global.width = o->width;
    n->global.height = o->height;
    g_strlcpy(n->global.style, o->style, sizeof(o->style));
    n->global.style_append = FALSE;
    n->compression = o->compression;
    n->pixel_type = EXR_PT_FLOAT;

    *new_version = 5;
    *new_size = sizeof(dt_imageio_exr_v5_t);
    return n;
  }
  if(old_version == 4)
  {
    typedef struct _imageio_exr_v4_t
    {
      dt_imageio_module_data_t global;
      dt_imageio_exr_compression_t compression;
    } dt_imageio_exr_v4_t;

    const dt_imageio_exr_v4_t *o = (dt_imageio_exr_v4_t *)old_params;
    dt_imageio_exr_v5_t *n = (dt_imageio_exr_v5_t *)malloc(sizeof(dt_imageio_exr_v5_t));

    n->global.max_width = o->global.max_width;
    n->global.max_height = o->global.max_height;
    n->global.width = o->global.width;
    n->global.height = o->global.height;
    g_strlcpy(n->global.style, o->global.style, sizeof(o->global.style));
    n->global.style_append = o->global.style_append;
    n->compression = o->compression;
    n->pixel_type = EXR_PT_FLOAT;

    *new_version = 5;
    *new_size = sizeof(dt_imageio_exr_v5_t);
    return n;
  }

  // incremental update supported:
  /*
  typedef struct dt_imageio_exr_v6_t
  {
    ...
  } dt_imageio_exr_v6_t;

  if(old_version == 5)
  {
    // let's update from 5 to 6

    ...
    *new_size = sizeof(dt_imageio_exr_v6_t);
    *new_version = 6;
    return n;
  }
  */
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

int flags(dt_imageio_module_data_t *data)
{
  return FORMAT_FLAGS_SUPPORT_LAYERS;
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
  return _("OpenEXR");
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

  DT_BAUHAUS_COMBOBOX_NEW_FULL(gui->bpp,self, NULL, N_("bit depth"), NULL,
                               (bpp_last >> 4) - EXR_PT_HALF, bpp_combobox_changed, self,
                               N_("16 bit (float)"), N_("32 bit (float)"));
  const int bpp_default = dt_confgen_get_int("plugins/imageio/format/exr/bpp", DT_DEFAULT);
  dt_bauhaus_combobox_set_default(gui->bpp, (bpp_default >> 4) - EXR_PT_HALF);
  gtk_box_pack_start(GTK_BOX(self->widget), gui->bpp, TRUE, TRUE, 0);

  // Compression combo box
  const int compression_last = dt_conf_get_int("plugins/imageio/format/exr/compression");

  DT_BAUHAUS_COMBOBOX_NEW_FULL(gui->compression, self, NULL, N_("compression"), NULL,
                               compression_last, compression_combobox_changed, self,
                               N_("uncompressed"),
                               N_("RLE"),
                               N_("ZIPS"),
                               N_("ZIP"),
                               N_("PIZ"),
                               N_("PXR24"),
                               N_("B44"),
                               N_("B44A"),
                               N_("DWAA"),
                               N_("DWAB"));
  dt_bauhaus_combobox_set_default(gui->compression,
                                  dt_confgen_get_int("plugins/imageio/format/exr/compression", DT_DEFAULT));
  gtk_box_pack_start(GTK_BOX(self->widget), gui->compression, TRUE, TRUE, 0);
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
} // extern "C"
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
