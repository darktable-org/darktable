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
#include <stdlib.h>
#include <stdio.h>
#include "common/darktable.h"
#include "common/exif.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
#include "common/imageio_format.h"
#include "control/conf.h"
#include "dtgtk/slider.h"
#include "dtgtk/togglebutton.h"

#include <webp/encode.h>

DT_MODULE(1)

typedef enum {
	webp_lossy = 0,
	webp_lossless = 1
} comp_type_t;


typedef enum {
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
  int comp_type;
  int quality;
  int hint;
}
dt_imageio_webp_t;

typedef struct dt_imageio_webp_gui_data_t
{
  GtkToggleButton *lossy, *lossless;
  GtkComboBox *preset;
  GtkDarktableSlider *quality;
  GtkComboBox *hint_combo;
}
dt_imageio_webp_gui_data_t;

static const char* const EncoderError[] = {
  "ok",
  "out_of_memory: out of memory allocating objects",
  "bitstream_out_of_memory: out of memory re-allocating byte buffer",
  "null_parameter: null parameter passed to function",
  "invalid_configuration: configuration is invalid",
  "bad_dimension: bad picture dimension. maximum width and height "
  "allowed is 16383 pixels.",
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

void init(dt_imageio_module_format_t *self) {
#ifdef USE_LUA
  luaA_enum(darktable.lua_state,comp_type_t);
  luaA_enum_value(darktable.lua_state,comp_type_t,webp_lossy,false);
  luaA_enum_value(darktable.lua_state,comp_type_t,webp_lossless,false);
  dt_lua_register_module_member(darktable.lua_state,self,dt_imageio_webp_t,comp_type,comp_type_t);
  dt_lua_register_module_member(darktable.lua_state,self,dt_imageio_webp_t,quality,int);
  luaA_enum(darktable.lua_state,hint_t);
  luaA_enum_value(darktable.lua_state,hint_t,hint_default,false);
  luaA_enum_value(darktable.lua_state,hint_t,hint_picture,false);
  luaA_enum_value(darktable.lua_state,hint_t,hint_photo,false);
  luaA_enum_value(darktable.lua_state,hint_t,hint_graphic,false);
  dt_lua_register_module_member(darktable.lua_state,self,dt_imageio_webp_t,hint,hint_t);
#endif
}
void cleanup(dt_imageio_module_format_t *self) {}

static int
FileWriter(const uint8_t* data, size_t data_size, const WebPPicture* const pic)
{
 FILE* const out = (FILE*)pic->custom_ptr;
 return data_size ? (fwrite(data, data_size, 1, out) == 1) : 1;
}

int
write_image (dt_imageio_module_data_t *webp, const char *filename, const void *in_tmp, void *exif, int exif_len, int imgid)
{
  dt_imageio_webp_t *webp_data = (dt_imageio_webp_t*) webp;
  FILE *out = fopen(filename, "wb");

  // Create, configure and validate a WebPConfig instance
  WebPConfig config;
  if (!WebPConfigPreset(&config, webp_data->hint, (float) webp_data->quality)) goto Error;
  //TODO(jinxos): expose more config options in the UI
  config.lossless = webp_data->comp_type;
  config.image_hint = webp_data->hint;

  //these are to allow for large image export.
  //TODO(jinxos): these values should be adjusted as needed and ideally determined at runtime.
  config.segments = 4;
  config.partition_limit = 70;
  
  WebPPicture pic;
  if (!WebPPictureInit(&pic)) goto Error;
  pic.width = webp_data->width;
  pic.height = webp_data->height;
  if (!out) {
    fprintf(stderr, _("[webp export] error saving to %s\n"), filename);
    goto Error;
  } else {
    pic.writer = FileWriter;
    pic.custom_ptr = out;  
  }
  
  WebPPictureImportRGBX(&pic, (const uint8_t*) in_tmp, webp_data->width*4);
  if (!config.lossless) {
    WebPPictureARGBToYUVA(&pic, WEBP_YUV420A);
  } else {
    WebPCleanupTransparentArea(&pic);
    WebPPictureYUVAToARGB(&pic);
  }
  if (!WebPValidateConfig(&config)) {
    fprintf(stderr, "error validating encoder config\n");
    goto Error;
  }
  if (!WebPEncode(&config, &pic)) {
    fprintf(stderr, _("[webp export] error during encoding!\n"));
    fprintf(stderr, _("[webp export] error code: %d (%s)\n"),
            pic.error_code, EncoderError[pic.error_code]);
    goto Error;
  }
  WebPPictureFree(&pic);
  fclose(out);
  return 0;

  Error:
    WebPPictureFree(&pic);
    if (out != NULL) {
      fclose(out);
    }
    return 1;
}

size_t
params_size(dt_imageio_module_format_t *self)
{
  return sizeof(dt_imageio_webp_t);
}

void*
get_params(dt_imageio_module_format_t *self)
{
  dt_imageio_webp_t *d = (dt_imageio_webp_t *)malloc(sizeof(dt_imageio_webp_t));
  memset(d, 0, sizeof(dt_imageio_webp_t));
  d->comp_type = dt_conf_get_int("plugins/imageio/format/webp/comp_type");
  if(d->comp_type == webp_lossy) d->quality = dt_conf_get_int("plugins/imageio/format/webp/quality");
  else                           d->quality = 100;
  d->hint = dt_conf_get_int("plugins/imageio/format/webp/hint");
  return d;
}

int
set_params(dt_imageio_module_format_t *self, const void *params, const int size)
{
  if(size != self->params_size(self)) return 1;
  dt_imageio_webp_t *d = (dt_imageio_webp_t *)params;
  dt_imageio_webp_gui_data_t *g = (dt_imageio_webp_gui_data_t *)self->gui_data;
  if(d->comp_type == webp_lossy) gtk_toggle_button_set_active(g->lossy, TRUE);
  else                           gtk_toggle_button_set_active(g->lossless, TRUE);
  dtgtk_slider_set_value(g->quality, d->quality);
  gtk_combo_box_set_active(g->hint_combo, d->hint);
  return 0;
}

void
free_params(dt_imageio_module_format_t *self, dt_imageio_module_data_t *params)
{
  free(params);
}

int
bpp(dt_imageio_module_data_t *p)
{
  return 8;
}

int
levels(dt_imageio_module_data_t *p)
{
  return IMAGEIO_RGB|IMAGEIO_INT8;
}

const char*
mime(dt_imageio_module_data_t *data)
{
  // TODO: revisit this when IANA makes it official.
  return "image/webp";
}

const char*
extension(dt_imageio_module_data_t *data)
{
  return "webp";
}

int
dimension (dt_imageio_module_format_t *self, uint32_t *width, uint32_t *height)
{
  return 0;
}

const char*
name ()
{
  return _("WebP");
}

static void
radiobutton_changed (GtkRadioButton *radiobutton, gpointer user_data)
{
  long int comp_type = (long int)user_data;
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(radiobutton)))
    dt_conf_set_int("plugins/imageio/format/webp/comp_type", comp_type);
}

static void
quality_changed (GtkDarktableSlider *slider, gpointer user_data)
{
  int quality = (int)dtgtk_slider_get_value(slider);
  dt_conf_set_int("plugins/imageio/format/webp/quality", quality);
}

static void hint_combobox_changed (GtkComboBox *widget, gpointer user_data)
{
  int hint = gtk_combo_box_get_active(widget);
  dt_conf_set_int("plugins/imageio/format/webp/hint", hint);
}

void gui_init (dt_imageio_module_format_t *self)
{
  dt_imageio_webp_gui_data_t *gui = (dt_imageio_webp_gui_data_t *)malloc(sizeof(dt_imageio_webp_gui_data_t));
  self->gui_data = (void *)gui;
  int comp_type = dt_conf_get_int("plugins/imageio/format/webp/comp_type");
  int quality = dt_conf_get_int("plugins/imageio/format/webp/quality");
  int hint = dt_conf_get_int("plugins/imageio/format/webp/hint");
  
  self->widget = gtk_vbox_new(TRUE, 5);
  GtkWidget *comp_type_label = gtk_label_new(_("compression type"));
  gtk_misc_set_alignment(GTK_MISC(comp_type_label), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(self->widget), comp_type_label, TRUE, TRUE, 0);
  GtkWidget *hbox = gtk_hbox_new(TRUE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);
  GtkWidget *radiobutton = gtk_radio_button_new_with_label(NULL, _("lossy"));
  gui->lossy = GTK_TOGGLE_BUTTON(radiobutton);
  gtk_box_pack_start(GTK_BOX(hbox), radiobutton, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(radiobutton), "toggled", G_CALLBACK(radiobutton_changed), (gpointer)webp_lossy);
  if(comp_type == webp_lossy) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radiobutton), TRUE);
  radiobutton = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(radiobutton), _("lossless"));
  gui->lossless = GTK_TOGGLE_BUTTON(radiobutton);
  gtk_box_pack_start(GTK_BOX(hbox), radiobutton, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(radiobutton), "toggled", G_CALLBACK(radiobutton_changed), (gpointer)webp_lossless);
  if(comp_type == webp_lossless) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radiobutton), TRUE);

  gui->quality = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR, 1, 100, 1, 97, 0));
  dtgtk_slider_set_label(gui->quality,_("quality"));
  dtgtk_slider_set_default_value(gui->quality, 97);
  dtgtk_slider_set_format_type(gui->quality, DARKTABLE_SLIDER_FORMAT_PERCENT);
  g_object_set(G_OBJECT(gui->quality), "tooltip-text", _("applies only to lossy setting"), (char *)NULL);
  if(quality > 0 && quality <= 100)
    dtgtk_slider_set_value(gui->quality, quality);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(gui->quality), TRUE, TRUE, 0);
  g_signal_connect (G_OBJECT (gui->quality), "value-changed", G_CALLBACK (quality_changed), (gpointer)0);

  GtkWidget *hint_hbox = gtk_hbox_new(FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), hint_hbox, TRUE, TRUE, 0);
  GtkWidget *hint_label = gtk_label_new(_("Image Hint"));
  g_object_set(G_OBJECT(hint_label), "tooltip-text",_("image characteristics hint for the underlying encoder.\n"
                                                      "picture : digital picture, like portrait, inner shot\n"
                                                      "photo   : outdoor photograph, with natural lighting\n"
                                                      "graphic : discrete tone image (graph, map-tile etc)"), (char *)NULL);
  gtk_misc_set_alignment(GTK_MISC(hint_label), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(hint_hbox), hint_label, TRUE, TRUE, 0);
  GtkComboBoxText *hint_combo = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
  gui->hint_combo = GTK_COMBO_BOX(hint_combo);
  gtk_combo_box_text_append_text(hint_combo, _("default"));
  gtk_combo_box_text_append_text(hint_combo, _("picture"));
  gtk_combo_box_text_append_text(hint_combo, _("photo"));
  gtk_combo_box_text_append_text(hint_combo, _("graphic"));
  gtk_combo_box_set_active(GTK_COMBO_BOX(hint_combo), hint);
  gtk_box_pack_start(GTK_BOX(hint_hbox), GTK_WIDGET(hint_combo), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(hint_combo), "changed", G_CALLBACK(hint_combobox_changed), NULL);
}

void gui_cleanup (dt_imageio_module_format_t *self)
{
  free(self->gui_data);
}

void gui_reset (dt_imageio_module_format_t *self) {}

int flags (dt_imageio_module_data_t *data)
{
  //TODO(jinxos): support embedded XMP/ICC
  return 0;
}

