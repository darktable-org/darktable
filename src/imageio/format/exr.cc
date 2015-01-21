/*
   This file is part of darktable,
   copyright (c) 2010-2011 Henrik Andersson.
   copyright (c) 2014 LebedevRI.

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
#include "version.h"

// needs to be defined before any system header includes for control/conf.h to work in C++ code
#define __STDC_FORMAT_MACROS

#include <cstdlib>
#include <cstdio>
#include <memory>

#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfTiledOutputFile.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfStandardAttributes.h>
#include <OpenEXR/ImfThreading.h>

extern "C" {
#include "common/darktable.h"
#include "common/exif.h"
#include "common/colorspaces.h"
#include "common/imageio.h"
#include "control/conf.h"
#include "common/imageio_module.h"
#include "common/imageio_exr.h"
#include "common/imageio_format.h"
#include "bauhaus/bauhaus.h"
}
#include "common/imageio_exr.hh"

#ifdef __cplusplus
extern "C" {
#endif

DT_MODULE(4)

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
  NUM_COMPRESSION_METHODS // number of different compression methods
};                        // copy of Imf::Compression

typedef struct dt_imageio_exr_t
{
  int max_width, max_height;
  int width, height;
  char style[128];
  gboolean style_append;
  dt_imageio_exr_compression_t compression;
} dt_imageio_exr_t;

typedef struct dt_imageio_exr_gui_t
{
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

  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_exr_t, compression,
                                dt_imageio_exr_compression_t);
#endif
  Imf::BlobAttribute::registerAttributeType();
}

void cleanup(dt_imageio_module_format_t *self)
{
}

int write_image(dt_imageio_module_data_t *tmp, const char *filename, const void *in_tmp, void *exif,
                int exif_len, int imgid, int num, int total)
{
  const dt_imageio_exr_t *exr = (dt_imageio_exr_t *)tmp;

  Imf::setGlobalThreadCount(dt_get_num_threads());

  Imf::Blob exif_blob(exif_len, (uint8_t *)exif);

  Imf::Header header(exr->width, exr->height, 1, Imath::V2f(0, 0), 1, Imf::INCREASING_Y,
                     (Imf::Compression)exr->compression);

  header.insert("comment", Imf::StringAttribute("Developed using Darktable " PACKAGE_VERSION));

  header.insert("exif", Imf::BlobAttribute(exif_blob));

  header.channels().insert("R", Imf::Channel(Imf::PixelType::FLOAT));
  header.channels().insert("G", Imf::Channel(Imf::PixelType::FLOAT));
  header.channels().insert("B", Imf::Channel(Imf::PixelType::FLOAT));

  header.setTileDescription(Imf::TileDescription(100, 100, Imf::ONE_LEVEL));

  Imf::TiledOutputFile file(filename, header);

  Imf::FrameBuffer data;

  const float *in = (const float *)in_tmp;

  data.insert("R", Imf::Slice(Imf::PixelType::FLOAT, (char *)(in + 0), 4 * sizeof(float),
                              4 * sizeof(float) * exr->width));

  data.insert("G", Imf::Slice(Imf::PixelType::FLOAT, (char *)(in + 1), 4 * sizeof(float),
                              4 * sizeof(float) * exr->width));

  data.insert("B", Imf::Slice(Imf::PixelType::FLOAT, (char *)(in + 2), 4 * sizeof(float),
                              4 * sizeof(float) * exr->width));

  file.setFrameBuffer(data);
  file.writeTiles(0, file.numXTiles() - 1, 0, file.numYTiles() - 1);

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
  if(old_version == 1 && new_version == 4)
  {
    dt_imageio_exr_t *new_params = (dt_imageio_exr_t *)malloc(sizeof(dt_imageio_exr_t));
    memcpy(new_params, old_params, old_params_size);
    new_params->compression = (dt_imageio_exr_compression_t)PIZ_COMPRESSION;
    new_params->style_append = 0;
    *new_size = self->params_size(self);
    return new_params;
  }
  if(old_version == 2 && new_version == 4)
  {
    enum dt_imageio_exr_pixeltype_t
    {
      UINT = 0,      // unsigned int (32 bit)
      HALF = 1,      // half (16 bit floating point)
      FLOAT = 2,     // float (32 bit floating point)
      NUM_PIXELTYPES // number of different pixel types
    };               // copy of Imf::PixelType

    typedef struct dt_imageio_exr_v2_t
    {
      int max_width, max_height;
      int width, height;
      char style[128];
      dt_imageio_exr_compression_t compression;
      dt_imageio_exr_pixeltype_t pixel_type;
    } dt_imageio_exr_v2_t;

    const dt_imageio_exr_v2_t *o = (dt_imageio_exr_v2_t *)old_params;
    dt_imageio_exr_t *new_params = (dt_imageio_exr_t *)malloc(sizeof(dt_imageio_exr_t));

    // last param was dropped (pixel type)
    memcpy(new_params, old_params, sizeof(old_params_size));
    new_params->style_append = 0;
    new_params->compression = o->compression;

    *new_size = self->params_size(self);
    return new_params;
  }
  if(old_version == 3 && new_version == 4)
  {
    typedef struct dt_imageio_exr_v3_t
    {
      int max_width, max_height;
      int width, height;
      char style[128];
      dt_imageio_exr_compression_t compression;
    } dt_imageio_exr_v3_t;

    const dt_imageio_exr_v3_t *o = (dt_imageio_exr_v3_t *)old_params;
    dt_imageio_exr_t *new_params = (dt_imageio_exr_t *)malloc(sizeof(dt_imageio_exr_t));

    memcpy(new_params, old_params, sizeof(dt_imageio_exr_t));
    new_params->style_append = 0;
    new_params->compression = o->compression;

    *new_size = self->params_size(self);
    return new_params;
  }
  return NULL;
}

void *get_params(dt_imageio_module_format_t *self)
{
  dt_imageio_exr_t *d = (dt_imageio_exr_t *)calloc(1, sizeof(dt_imageio_exr_t));
  d->compression = (dt_imageio_exr_compression_t)dt_conf_get_int("plugins/imageio/format/exr/compression");
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
  dt_bauhaus_combobox_set(g->compression, d->compression);
  return 0;
}

int bpp(dt_imageio_module_data_t *p)
{
  return 32;
}

int levels(dt_imageio_module_data_t *p)
{
  return IMAGEIO_RGB | IMAGEIO_FLOAT;
}

const char *mime(dt_imageio_module_data_t *data)
{
  return "image/openexr";
}

const char *extension(dt_imageio_module_data_t *data)
{
  return "exr";
}

const char *name()
{
  return _("OpenEXR (float)");
}

static void combobox_changed(GtkWidget *widget, gpointer user_data)
{
  const int compression = dt_bauhaus_combobox_get(widget);
  dt_conf_set_int("plugins/imageio/format/exr/compression", compression);
}

void gui_init(dt_imageio_module_format_t *self)
{
  self->gui_data = malloc(sizeof(dt_imageio_exr_gui_t));
  dt_imageio_exr_gui_t *gui = (dt_imageio_exr_gui_t *)self->gui_data;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);

  const int compression_last = dt_conf_get_int("plugins/imageio/format/exr/compression");

  gui->compression = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(gui->compression, NULL, _("compression mode"));

  dt_bauhaus_combobox_add(gui->compression, _("off"));
  dt_bauhaus_combobox_add(gui->compression, _("RLE"));
  dt_bauhaus_combobox_add(gui->compression, _("ZIPS"));
  dt_bauhaus_combobox_add(gui->compression, _("ZIP"));
  dt_bauhaus_combobox_add(gui->compression, _("PIZ (default)"));
  dt_bauhaus_combobox_add(gui->compression, _("PXR24 (lossy)"));
  dt_bauhaus_combobox_add(gui->compression, _("B44 (lossy)"));
  dt_bauhaus_combobox_add(gui->compression, _("B44A (lossy)"));
  dt_bauhaus_combobox_set(gui->compression, compression_last);
  gtk_box_pack_start(GTK_BOX(self->widget), gui->compression, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(gui->compression), "value-changed", G_CALLBACK(combobox_changed), NULL);
}

void gui_cleanup(dt_imageio_module_format_t *self)
{
  free(self->gui_data);
}

void gui_reset(dt_imageio_module_format_t *self)
{
}



#ifdef __cplusplus
}
#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
