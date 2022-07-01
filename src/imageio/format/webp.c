/*
    This file is part of darktable,
    Copyright (C) 2013-2021 darktable developers.

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
#include "common/darktable.h"
#include "common/exif.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
#include "control/conf.h"
#include "imageio/format/imageio_format_api.h"
#include <stdio.h>
#include <stdlib.h>

#include <webp/encode.h>
#include <webp/mux.h>

DT_MODULE(2)

typedef enum
{
  webp_lossy = 0,
  webp_lossless = 1
} comp_type_t;

typedef enum
{
  hint_default,
  hint_picture,
  hint_photo,
  hint_graphic
} hint_t;

typedef struct dt_imageio_webp_t
{
  dt_imageio_module_data_t global;
  int comp_type;
  int quality;
  int hint;
} dt_imageio_webp_t;

typedef struct dt_imageio_webp_gui_data_t
{
  GtkWidget *compression;
  GtkWidget *quality;
  GtkWidget *hint;
} dt_imageio_webp_gui_data_t;

#define _stringify(a) #a
#define stringify(a) _stringify(a)

static const char *const EncoderError[] = {
  "ok",
  "out_of_memory: out of memory allocating objects",
  "bitstream_out_of_memory: out of memory re-allocating byte buffer",
  "null_parameter: null parameter passed to function",
  "invalid_configuration: configuration is invalid",
  "bad_dimension: bad picture dimension. maximum width and height "
  "allowed is " stringify(WEBP_MAX_DIMENSION) " pixels.",
  "partition0_overflow: partition #0 is too big to fit 512k.\n"
  "to reduce the size of this partition, try using less segments "
  "with the -segments option, and eventually reduce the number of "
  "header bits using -partition_limit. more details are available "
  "in the manual (`man cwebp`)",
  "partition_overflow: partition is too big to fit 16M",
  "bad_write: picture writer returned an i/o error",
  "file_too_big: file would be too big to fit in 4G",
  "user_abort: encoding abort requested by user"
};

const char *get_error_str(int err)
{
  if (err < 0 || err >= sizeof(EncoderError)/sizeof(EncoderError[0]))
  {
    return "unknown error (consider filing a darktable issue to update the WebP error list)";
  }
  return EncoderError[err];
}

void init(dt_imageio_module_format_t *self)
{
#ifdef USE_LUA
  luaA_enum(darktable.lua_state.state, comp_type_t);
  luaA_enum_value(darktable.lua_state.state, comp_type_t, webp_lossy);
  luaA_enum_value(darktable.lua_state.state, comp_type_t, webp_lossless);
  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_webp_t, comp_type, comp_type_t);
  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_webp_t, quality, int);
  luaA_enum(darktable.lua_state.state, hint_t);
  luaA_enum_value(darktable.lua_state.state, hint_t, hint_default);
  luaA_enum_value(darktable.lua_state.state, hint_t, hint_picture);
  luaA_enum_value(darktable.lua_state.state, hint_t, hint_photo);
  luaA_enum_value(darktable.lua_state.state, hint_t, hint_graphic);
  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_webp_t, hint, hint_t);
#endif
}

void cleanup(dt_imageio_module_format_t *self)
{
}

int write_image(dt_imageio_module_data_t *webp, const char *filename, const void *in_tmp,
                dt_colorspaces_color_profile_type_t over_type, const char *over_filename,
                void *exif, int exif_len, int imgid, int num, int total, struct dt_dev_pixelpipe_t *pipe,
                const gboolean export_masks)
{
  int res = 1;
  FILE *out = NULL;
  uint8_t *buf = NULL;
  WebPPicture pic;
  WebPMemoryWriter writer;
  WebPMemoryWriterInit(&writer);
  WebPData icc_profile;
  WebPDataInit(&icc_profile);
  WebPData bitstream;
  WebPDataInit(&bitstream);
  WebPData assembled_data;
  WebPDataInit(&assembled_data);
  WebPMux *mux = WebPMuxNew();
  WebPMuxError err;

  dt_imageio_webp_t *webp_data = (dt_imageio_webp_t *)webp;

  // Create, configure and validate a WebPConfig instance
  WebPConfig config;
  if(!WebPConfigPreset(&config, webp_data->hint, (float)webp_data->quality)) goto error;

  // TODO(jinxos): expose more config options in the UI
  config.lossless = webp_data->comp_type;
  config.image_hint = webp_data->hint;
  config.method = 6;

  // these are to allow for large image export.
  // TODO(jinxos): these values should be adjusted as needed and ideally determined at runtime.
  config.segments = 4;
  config.partition_limit = 70;
  if(!WebPValidateConfig(&config))
  {
    fprintf(stderr, "[webp export] error validating encoder configuration\n");
    goto error;
  }

  // embed ICC profile
  cmsHPROFILE out_profile = dt_colorspaces_get_output_profile(imgid, over_type, over_filename)->profile;
  uint32_t len = 0;
  cmsSaveProfileToMem(out_profile, NULL, &len);
  if(len > 0)
  {
    buf = (uint8_t *)g_malloc(len);
    if(buf)
    {
      cmsSaveProfileToMem(out_profile, buf, &len);
      icc_profile.bytes = buf;
      icc_profile.size = len;
      err = WebPMuxSetChunk(mux, "ICCP", &icc_profile, 0);
      if(err != WEBP_MUX_OK)
      {
        fprintf(stderr, "[webp export] error adding ICC profile to WebP stream\n");
        goto error;
      }
    }
    else
    {
      fprintf(stderr, "[webp export] error allocating ICC profile buffer\n");
      goto error;
    }
  }

  // encode image data to memory and add to mux
  if(!WebPPictureInit(&pic)) goto error;
  pic.width = webp_data->global.width;
  pic.height = webp_data->global.height;
  pic.use_argb = !!(config.lossless);
  pic.writer = WebPMemoryWrite;
  pic.custom_ptr = &writer;

  WebPPictureImportRGBX(&pic, (const uint8_t *)in_tmp, webp_data->global.width * 4);
  if(!config.lossless)
  {
    // webp is more efficient at coding YUV images, as we go lossy
    // let the encoder where best to spend its bits instead of forcing it
    // to spend bits equally on RGB data that doesn't weight the same when
    // considering the human visual system.
    // use the slower, but better and sharper YUV conversion.
    WebPPictureSharpARGBToYUVA(&pic);
  }

  if(!WebPEncode(&config, &pic))
  {
    fprintf(stderr, "[webp export] error (%d) during encoding: %s\n", pic.error_code,
            get_error_str(pic.error_code));
    goto error;
  }

  bitstream.bytes = writer.mem;
  bitstream.size = writer.size;
  err = WebPMuxSetImage(mux, &bitstream, 0);
  if(err != WEBP_MUX_OK)
  {
    fprintf(stderr, "[webp export] error adding image to WebP stream\n");
    goto error;
  }

  // finally write out assembled data to file
  err = WebPMuxAssemble(mux, &assembled_data);
  if(err != WEBP_MUX_OK)
  {
    fprintf(stderr, "[webp export] error assembling the WebP file\n");
    goto error;
  }

  out = g_fopen(filename, "w+b");
  if(!out)
  {
    fprintf(stderr, "[webp export] error creating file %s\n", filename);
    goto error;
  }
  if(fwrite(assembled_data.bytes, assembled_data.size, 1, out) != 1)
  {
    fprintf(stderr, "[webp export] error writing %zu bytes to file %s\n", assembled_data.size, filename);
    goto error;
  }

  res = 0;

error:
  WebPPictureFree(&pic);
  WebPMemoryWriterClear(&writer); // no need to WebPDataClear(&bitstream) as well
  g_free(buf); // instead of WebPDataClear(&icc_profile)
  WebPDataClear(&assembled_data);
  WebPMuxDelete(mux);
  fclose(out);
  if(!res) dt_exif_write_blob(exif, exif_len, filename, 1);
  return res;
}

size_t params_size(dt_imageio_module_format_t *self)
{
  return sizeof(dt_imageio_webp_t);
}

void *legacy_params(dt_imageio_module_format_t *self, const void *const old_params,
                    const size_t old_params_size, const int old_version, const int new_version,
                    size_t *new_size)
{
  if(old_version == 1 && new_version == 2)
  {
    typedef struct dt_imageio_webp_v1_t
    {
      int max_width, max_height;
      int width, height;
      char style[128];
      int comp_type;
      int quality;
      int hint;
    } dt_imageio_webp_v1_t;

    dt_imageio_webp_v1_t *o = (dt_imageio_webp_v1_t *)old_params;
    dt_imageio_webp_t *n = (dt_imageio_webp_t *)malloc(sizeof(dt_imageio_webp_t));

    n->global.max_width = o->max_width;
    n->global.max_height = o->max_height;
    n->global.width = o->width;
    n->global.height = o->height;
    g_strlcpy(n->global.style, o->style, sizeof(o->style));
    n->global.style_append = FALSE;
    n->comp_type = o->comp_type;
    n->quality = o->quality;
    n->hint = o->hint;
    *new_size = self->params_size(self);
    return n;
  }
  return NULL;
}

void *get_params(dt_imageio_module_format_t *self)
{
  dt_imageio_webp_t *d = (dt_imageio_webp_t *)calloc(1, sizeof(dt_imageio_webp_t));
  d->comp_type = dt_conf_get_int("plugins/imageio/format/webp/comp_type");
  d->quality = dt_conf_get_int("plugins/imageio/format/webp/quality");
  d->hint = dt_conf_get_int("plugins/imageio/format/webp/hint");
  return d;
}

int set_params(dt_imageio_module_format_t *self, const void *params, const int size)
{
  if(size != self->params_size(self)) return 1;
  const dt_imageio_webp_t *d = (dt_imageio_webp_t *)params;
  dt_imageio_webp_gui_data_t *g = (dt_imageio_webp_gui_data_t *)self->gui_data;
  dt_bauhaus_combobox_set(g->compression, d->comp_type);
  dt_bauhaus_slider_set(g->quality, d->quality);
  dt_bauhaus_combobox_set(g->hint, d->hint);
  return 0;
}

void free_params(dt_imageio_module_format_t *self, dt_imageio_module_data_t *params)
{
  free(params);
}

int dimension(struct dt_imageio_module_format_t *self, struct dt_imageio_module_data_t *data, uint32_t *width,
              uint32_t *height)
{
  /* maximum dimensions supported by WebP images */
  *width = WEBP_MAX_DIMENSION;
  *height = WEBP_MAX_DIMENSION;
  return 1;
}

int bpp(dt_imageio_module_data_t *p)
{
  return 8;
}

int levels(dt_imageio_module_data_t *p)
{
  return IMAGEIO_RGB | IMAGEIO_INT8;
}

const char *mime(dt_imageio_module_data_t *data)
{
  // TODO: revisit this when IANA makes it official.
  return "image/webp";
}

const char *extension(dt_imageio_module_data_t *data)
{
  return "webp";
}

const char *name()
{
  return _("WebP (8-bit)");
}

static void compression_changed(GtkWidget *widget, gpointer user_data)
{
  const int comp_type = dt_bauhaus_combobox_get(widget);
  dt_conf_set_int("plugins/imageio/format/webp/comp_type", comp_type);
}

static void quality_changed(GtkWidget *slider, gpointer user_data)
{
  const int quality = (int)dt_bauhaus_slider_get(slider);
  dt_conf_set_int("plugins/imageio/format/webp/quality", quality);
}

static void hint_combobox_changed(GtkWidget *widget, gpointer user_data)
{
  const int hint = dt_bauhaus_combobox_get(widget);
  dt_conf_set_int("plugins/imageio/format/webp/hint", hint);
}

void gui_init(dt_imageio_module_format_t *self)
{
  dt_imageio_webp_gui_data_t *gui = (dt_imageio_webp_gui_data_t *)malloc(sizeof(dt_imageio_webp_gui_data_t));
  self->gui_data = (void *)gui;
  const int comp_type = dt_conf_get_int("plugins/imageio/format/webp/comp_type");
  const int quality = dt_conf_get_int("plugins/imageio/format/webp/quality");
  const int hint = dt_conf_get_int("plugins/imageio/format/webp/hint");

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  gui->compression = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(gui->compression, NULL, N_("compression type"));
  dt_bauhaus_combobox_add(gui->compression, _("lossy"));
  dt_bauhaus_combobox_add(gui->compression, _("lossless"));
  dt_bauhaus_combobox_set(gui->compression, comp_type);
  gtk_box_pack_start(GTK_BOX(self->widget), gui->compression, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(gui->compression), "value-changed", G_CALLBACK(compression_changed), NULL);

  gui->quality = dt_bauhaus_slider_new_with_range(NULL,
                                                  dt_confgen_get_int("plugins/imageio/format/webp/quality", DT_MIN),
                                                  dt_confgen_get_int("plugins/imageio/format/webp/quality", DT_MAX),
                                                  1,
                                                  dt_confgen_get_int("plugins/imageio/format/webp/quality", DT_DEFAULT),
                                                  0);
  dt_bauhaus_widget_set_label(gui->quality, NULL, N_("quality"));
  dt_bauhaus_slider_set_default(gui->quality, dt_confgen_get_int("plugins/imageio/format/webp/quality", DT_DEFAULT));
  dt_bauhaus_slider_set_format(gui->quality, "%");
  gtk_widget_set_tooltip_text(gui->quality, _("for lossy, 0 gives the smallest size and 100 the best quality.\n"
                                              "for lossless, 0 is the fastest but gives larger files compared\n"
                                              "to the slowest 100."));
  if(quality >= 0 && quality <= 100) dt_bauhaus_slider_set(gui->quality, quality);
  gtk_box_pack_start(GTK_BOX(self->widget), gui->quality, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(gui->quality), "value-changed", G_CALLBACK(quality_changed), NULL);

  gui->hint = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(gui->hint, NULL, N_("image hint"));
  gtk_widget_set_tooltip_text(gui->hint,
               _("image characteristics hint for the underlying encoder.\n"
               "picture : digital picture, like portrait, inner shot\n"
               "photo   : outdoor photograph, with natural lighting\n"
               "graphic : discrete tone image (graph, map-tile etc)"));
  dt_bauhaus_combobox_add(gui->hint, _("default"));
  dt_bauhaus_combobox_add(gui->hint, _("picture"));
  dt_bauhaus_combobox_add(gui->hint, _("photo"));
  dt_bauhaus_combobox_add(gui->hint, _("graphic"));
  dt_bauhaus_combobox_set(gui->hint, hint);
  gtk_box_pack_start(GTK_BOX(self->widget), gui->hint, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(gui->hint), "value-changed", G_CALLBACK(hint_combobox_changed), NULL);
}

void gui_cleanup(dt_imageio_module_format_t *self)
{
  free(self->gui_data);
}

void gui_reset(dt_imageio_module_format_t *self)
{
  dt_imageio_webp_gui_data_t *gui = (dt_imageio_webp_gui_data_t *)self->gui_data;
  const int comp_type = dt_confgen_get_int("plugins/imageio/format/webp/comp_type", DT_DEFAULT);
  const int quality = dt_confgen_get_int("plugins/imageio/format/webp/quality", DT_DEFAULT);
  const int hint = dt_confgen_get_int("plugins/imageio/format/webp/hint", DT_DEFAULT);
  dt_bauhaus_combobox_set(gui->compression, comp_type);
  dt_bauhaus_slider_set(gui->quality, quality);
  dt_bauhaus_combobox_set(gui->hint, hint);
}

int flags(dt_imageio_module_data_t *data)
{
  // TODO(jinxos): support embedded ICC
  return FORMAT_FLAGS_SUPPORT_XMP;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
