/*
    This file is part of darktable,
    copyright (c) 2013 Google Inc.

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
  int max_width, max_height;
  int width, height;
  char style[128];
  gboolean style_append;
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

static const char *const EncoderError[] = {
  "ok", "out_of_memory: out of memory allocating objects",
  "bitstream_out_of_memory: out of memory re-allocating byte buffer",
  "null_parameter: null parameter passed to function", "invalid_configuration: configuration is invalid",
  "bad_dimension: bad picture dimension. maximum width and height "
  "allowed is 16383 pixels.",
  "partition0_overflow: partition #0 is too big to fit 512k.\n"
  "to reduce the size of this partition, try using less segments "
  "with the -segments option, and eventually reduce the number of "
  "header bits using -partition_limit. more details are available "
  "in the manual (`man cwebp`)",
  "partition_overflow: partition is too big to fit 16M", "bad_write: picture writer returned an i/o error",
  "file_too_big: file would be too big to fit in 4G", "user_abort: encoding abort requested by user"
};

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

static int FileWriter(const uint8_t *data, size_t data_size, const WebPPicture *const pic)
{
  FILE *const out = (FILE *)pic->custom_ptr;
  return data_size ? (fwrite(data, data_size, 1, out) == 1) : 1;
}

int write_image(dt_imageio_module_data_t *webp, const char *filename, const void *in_tmp,
                dt_colorspaces_color_profile_type_t over_type, const char *over_filename,
                void *exif, int exif_len, int imgid, int num, int total)
{
  dt_imageio_webp_t *webp_data = (dt_imageio_webp_t *)webp;
  FILE *out = g_fopen(filename, "wb");

  // Create, configure and validate a WebPConfig instance
  WebPConfig config;
  if(!WebPConfigPreset(&config, webp_data->hint, (float)webp_data->quality)) goto Error;
  // TODO(jinxos): expose more config options in the UI
  config.lossless = webp_data->comp_type;
  config.image_hint = webp_data->hint;

  // these are to allow for large image export.
  // TODO(jinxos): these values should be adjusted as needed and ideally determined at runtime.
  config.segments = 4;
  config.partition_limit = 70;

  WebPPicture pic;
  if(!WebPPictureInit(&pic)) goto Error;
  pic.width = webp_data->width;
  pic.height = webp_data->height;
  if(!out)
  {
    fprintf(stderr, "[webp export] error saving to %s\n", filename);
    goto Error;
  }
  else
  {
    pic.writer = FileWriter;
    pic.custom_ptr = out;
  }

  WebPPictureImportRGBX(&pic, (const uint8_t *)in_tmp, webp_data->width * 4);
  if(!config.lossless)
  {
    WebPPictureARGBToYUVA(&pic, WEBP_YUV420A);
  }
  else
  {
    WebPCleanupTransparentArea(&pic);
    WebPPictureYUVAToARGB(&pic);
  }
  if(!WebPValidateConfig(&config))
  {
    fprintf(stderr, "error validating encoder config\n");
    goto Error;
  }
  if(!WebPEncode(&config, &pic))
  {
    fprintf(stderr, "[webp export] error during encoding!\n");
    fprintf(stderr, "[webp export] error code: %d (%s)\n", pic.error_code, EncoderError[pic.error_code]);
    goto Error;
  }
  WebPPictureFree(&pic);
  fclose(out);
  return 0;

Error:
  WebPPictureFree(&pic);
  if(out != NULL)
  {
    fclose(out);
  }
  return 1;
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

    n->max_width = o->max_width;
    n->max_height = o->max_height;
    n->width = o->width;
    n->height = o->height;
    g_strlcpy(n->style, o->style, sizeof(o->style));
    n->style_append = 0;
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
  if(d->comp_type == webp_lossy)
    d->quality = dt_conf_get_int("plugins/imageio/format/webp/quality");
  else
    d->quality = 100;
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

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_PIXEL_APPLY_DPI(5));

  gui->compression = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(gui->compression, NULL, _("compression type"));
  dt_bauhaus_combobox_add(gui->compression, _("lossy"));
  dt_bauhaus_combobox_add(gui->compression, _("lossless"));
  dt_bauhaus_combobox_set(gui->compression, comp_type);
  g_signal_connect(G_OBJECT(gui->compression), "value-changed", G_CALLBACK(compression_changed), NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), gui->compression, TRUE, TRUE, 0);

  gui->quality = dt_bauhaus_slider_new_with_range(NULL, 5, 100, 1, 95, 0);
  dt_bauhaus_widget_set_label(gui->quality, NULL, _("quality"));
  dt_bauhaus_slider_set_default(gui->quality, 95);
  dt_bauhaus_slider_set_format(gui->quality, "%.2f%%");
  gtk_widget_set_tooltip_text(gui->quality, _("applies only to lossy setting"));
  if(quality > 0 && quality <= 100) dt_bauhaus_slider_set(gui->quality, quality);
  gtk_box_pack_start(GTK_BOX(self->widget), gui->quality, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(gui->quality), "value-changed", G_CALLBACK(quality_changed), (gpointer)0);


  gui->hint = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(gui->hint, NULL, _("image hint"));
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
}

int flags(dt_imageio_module_data_t *data)
{
  // TODO(jinxos): support embedded XMP/ICC
  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
