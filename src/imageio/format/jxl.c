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

#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/exif.h"
#include "common/imageio.h"
#include "control/conf.h"
#include "imageio/format/imageio_format_api.h"

#include "jxl/encode.h"
#include "jxl/resizable_parallel_runner.h"
#include "jxl/thread_parallel_runner.h"

DT_MODULE(1)



#define CONF_PREFIX "plugins/imageio/format/jxl/"

// Should the decoding effort option be exposed to user?
#define SHOW_DECODING_EFFORT



typedef struct dt_imageio_jxl_params_t
{
  dt_imageio_module_data_t global;
  gboolean lossless;
  float quality;
  int encoding_effort;
#ifdef SHOW_DECODING_EFFORT
  int decoding_effort;
#endif
} dt_imageio_jxl_params_t;

typedef struct dt_imageio_jxl_gui_data_t
{
  // Boolean: is output lossless
  GtkWidget *lossless;
  // Float (0.0-15.0): if lossy, the quality of the image. 0 = extremely lossy, 14 = visually lossless, 15 =
  // lossless.
  GtkWidget *quality;
  // Int (1-9): effort with which to encode output. 1 = low quality, 9 = high quality.
  GtkWidget *encoding_effort;
#ifdef SHOW_DECODING_EFFORT
  // Int (0-4): effort required to decode output. 0 = fast, 4 = slow.
  GtkWidget *decoding_effort;
#endif
} dt_imageio_jxl_gui_data_t;



void init(dt_imageio_module_format_t *self)
{
#ifdef USE_LUA
  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_jxl_params_t, lossless, gboolean);

  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_jxl_params_t, quality, float);

  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_jxl_params_t, encoding_effort, int);

#ifdef SHOW_DECODING_EFFORT
  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_jxl_params_t, decoding_effort, int);
#endif
#endif
}

void cleanup(dt_imageio_module_format_t *self)
{
}



const char *mime(dt_imageio_module_data_t *data)
{
  return "image/jxl";
}

const char *extension(dt_imageio_module_data_t *data)
{
  return "jxl";
}

int dimension(struct dt_imageio_module_format_t *self, struct dt_imageio_module_data_t *data, uint32_t *width,
              uint32_t *height)
{
  *width = 1073741823;
  *height = 1073741823;
  return 1;
}

int bpp(dt_imageio_module_data_t *data)
{
  return 32;
}

int write_image(struct dt_imageio_module_data_t *data, const char *filename, const void *in_tmp,
                dt_colorspaces_color_profile_type_t over_type, const char *over_filename, void *exif, int exif_len,
                int imgid, int num, int total, struct dt_dev_pixelpipe_t *pipe, const gboolean export_masks)
{
  // By default error code
  int out = 1;

  const float *in = (float *)in_tmp;
  float *pixels = NULL;
  uint8_t *out_buf = NULL;
  FILE *out_file = NULL;

#define JXL_ASSERT(code)                                                                                          \
  {                                                                                                               \
    JxlEncoderStatus res = code;                                                                                  \
    if(res != JXL_ENC_SUCCESS)                                                                                    \
    {                                                                                                             \
      fprintf(stderr, "JXL call failed with err %d (src/imageio/format/jxl.c#L%d)\n", res, __LINE__);             \
      goto end;                                                                                                   \
    }                                                                                                             \
  }

  dt_imageio_jxl_params_t *params = (dt_imageio_jxl_params_t *)data;
  int width = params->global.width;
  int height = params->global.height;

  JxlEncoder *encoder = JxlEncoderCreate(NULL);

  // JxlThreadParallelRunnerDefaultNumWorkerThreads()
  uint32_t num_threads = JxlResizableParallelRunnerSuggestThreads(width, height);
  void *runner = JxlThreadParallelRunnerCreate(NULL, num_threads);
  JXL_ASSERT(JxlEncoderSetParallelRunner(encoder, JxlThreadParallelRunner, runner));

  cmsHPROFILE out_profile = dt_colorspaces_get_output_profile(imgid, over_type, over_filename)->profile;

  cmsUInt32Number size = 0;
  if(cmsSaveProfileToMem(out_profile, NULL, &size))
  {
    unsigned char *buf = malloc(size);
    if(!buf || !cmsSaveProfileToMem(out_profile, buf, &size)) goto end;

    JXL_ASSERT(JxlEncoderSetICCProfile(encoder, buf, size));

    free(buf);
  }

  JxlBasicInfo basic_info = {};
  // fixme: add this in once libjxl 0.6 is released and forced as a dependency
  // This method is only added in 0.6 but it is required to be called
  // JXL_ASSERT(JxlEncoderInitBasicInfo(&basic_info));
  basic_info.have_container = JXL_FALSE;
  basic_info.xsize = width;
  basic_info.ysize = height;
  basic_info.bits_per_sample = 32;
  basic_info.exponent_bits_per_sample = 8;
  basic_info.intensity_target = 255.f;
  basic_info.min_nits = 0.f;
  basic_info.relative_to_max_display = JXL_FALSE;
  basic_info.linear_below = 0.f;
  basic_info.uses_original_profile = JXL_TRUE;
  basic_info.have_preview = JXL_FALSE;
  basic_info.have_animation = JXL_FALSE;
  basic_info.orientation = JXL_ORIENT_IDENTITY;
  basic_info.num_color_channels = 3;
  basic_info.num_extra_channels = 0;
  basic_info.alpha_bits = 0;
  basic_info.alpha_exponent_bits = 0;
  basic_info.alpha_premultiplied = JXL_FALSE;
  JXL_ASSERT(JxlEncoderSetBasicInfo(encoder, &basic_info));

  // We assume that the user wants the JXL image in a bmff container.
  // JXL images can be stored without any container so they are smaller, but this
  // removes the possibility of storing extra data like exif.
  // It is not likely darktable users exporting images care so much about this
  // small size improvement, and more likely they want to store exif data.
  JXL_ASSERT(JxlEncoderUseContainer(encoder, JXL_TRUE));

  // automatically freed when we destroy the encoder
  JxlEncoderOptions *options = JxlEncoderOptionsCreate(encoder, NULL);
  JXL_ASSERT(JxlEncoderOptionsSetLossless(options, params->lossless ? JXL_TRUE : JXL_FALSE));
#ifdef SHOW_DECODING_EFFORT
  int decode_effort = params->decoding_effort;
  // We store decoding effort but jxl wants encoding speed, we must reverse it
  decode_effort = (decode_effort - 4) * -1;
  JXL_ASSERT(JxlEncoderOptionsSetDecodingSpeed(options, decode_effort));
#endif
  int encode_effort = params->encoding_effort;
  // In libjxl 0.5 encode efforts over 4 cause a crash and encode efforts below 3 are unsupported
  if(JxlEncoderVersion() <= 0 * 1000000 + 5 * 1000 + 0) // if version <= 0.5
  {
    fprintf(stderr, "Warning: Encoding effort is limited to 3-4 on libjxl 0.5 and lower\n");
    if(encode_effort > 4)
      encode_effort = 4;
    else if(encode_effort < 3)
      encode_effort = 3;
  }
  JXL_ASSERT(JxlEncoderOptionsSetEffort(options, encode_effort));
  int quality = params->quality;
  // We store quality but jxl wants distance, so we reverse the scale
  quality = (quality - 15) * -1;
  JXL_ASSERT(JxlEncoderOptionsSetDistance(options, quality));

  int channels = 3;
  JxlPixelFormat pixel_format = { channels, JXL_TYPE_FLOAT, JXL_NATIVE_ENDIAN, 1 };
  size_t in_buf_size = width * height * channels * sizeof(float);

  pixels = malloc(in_buf_size);
  for(int y = 0; y < height; y++)
  {
    for(int x = 0; x < width; x++)
    {
      const float *buf = &in[((y * width) + x) * 4];

      for(int k = 0; k < channels; k++) pixels[(y * width * channels) + (x * channels) + k] = buf[k];
    }
  }

  JXL_ASSERT(JxlEncoderAddImageFrame(options, &pixel_format, pixels, in_buf_size));
  JxlEncoderCloseInput(encoder);

  // Write the image to disk in chunks of 50,000 bytes
  // fixme: Can we better estimate what the optimal size of chunks is for this image?
  size_t chunkSize = 50000;

  out_buf = malloc(chunkSize);
  if(!out_buf) goto end;

  out_file = g_fopen(filename, "wb");
  if(!out_file) goto end;

  JxlEncoderStatus out_status = JXL_ENC_NEED_MORE_OUTPUT;

  while(out_status == JXL_ENC_NEED_MORE_OUTPUT)
  {
    uint8_t *out_tmp = out_buf;
    size_t out_avail = chunkSize;

    out_status = JxlEncoderProcessOutput(encoder, &out_tmp, &out_avail);
    if(out_status != JXL_ENC_NEED_MORE_OUTPUT && out_status != JXL_ENC_SUCCESS)
    {
      // out_status should be an error: force it to be processed
      JXL_ASSERT(out_status);
      // just to let the compiler know that we will goto end (even though it
      // should happen anyway in the assertion)
      goto end;
    }

    // out_tmp is incremented by the amount written
    size_t consumed = out_tmp - out_buf;
    // write however much we got to the file
    if(fwrite(out_buf, 1, consumed, out_file) != consumed) goto end;
  }

  // Successful write: set to success code
  out = 0;

end:
  if(runner) JxlThreadParallelRunnerDestroy(runner);
  if(encoder) JxlEncoderDestroy(encoder);
  if(out_file) fclose(out_file);
  if(out_buf) free(out_buf);
  if(pixels) free(pixels);

  return out;
}

int levels(dt_imageio_module_data_t *data)
{
  return IMAGEIO_INT32 | IMAGEIO_FLOAT | IMAGEIO_RGB;
}

int flags(dt_imageio_module_data_t *data)
{
  // fixme: For now exiv2 does not support writing to bmff containers.
  // Uncomment when that is no longer the case.
  // return FORMAT_FLAGS_SUPPORT_XMP;
  return 0;
}



/*
-------- PARAMS --------
*/


size_t params_size(dt_imageio_module_format_t *self)
{
  return sizeof(dt_imageio_jxl_params_t);
}

void *get_params(dt_imageio_module_format_t *self)
{
  dt_imageio_jxl_params_t *params = malloc(sizeof(dt_imageio_jxl_params_t));

  if(params == NULL)
  {
    return NULL;
  }

  params->lossless = dt_conf_get_bool(CONF_PREFIX "lossless");

  params->quality = dt_conf_get_float(CONF_PREFIX "quality");

  params->encoding_effort = dt_conf_get_int(CONF_PREFIX "encoding_effort");

#ifdef SHOW_DECODING_EFFORT
  params->decoding_effort = dt_conf_get_int(CONF_PREFIX "decoding_effort");
#endif

  return params;
}

void free_params(dt_imageio_module_format_t *self, dt_imageio_module_data_t *params)
{
  free(params);
}

int set_params(dt_imageio_module_format_t *self, const void *params_v, const int size)
{
  if(size != self->params_size(self)) return 1;

  const dt_imageio_jxl_params_t *params = (dt_imageio_jxl_params_t *)params_v;
  dt_imageio_jxl_gui_data_t *gui = (dt_imageio_jxl_gui_data_t *)self->gui_data;

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gui->lossless), params->lossless);

  float quality = params->quality;
  if(quality < 0.0)
    quality = 0.0;
  else if(quality > 5.0)
    quality = 5.0;
  dt_bauhaus_slider_set(gui->quality, quality);

  int encoding_effort = params->encoding_effort;
  if(encoding_effort < 1)
    encoding_effort = 1;
  else if(encoding_effort > 9)
    encoding_effort = 9;
  dt_bauhaus_slider_set(gui->encoding_effort, encoding_effort);

#ifdef SHOW_DECODING_EFFORT
  int decoding_effort = params->decoding_effort;
  if(decoding_effort < 0)
    decoding_effort = 0;
  else if(decoding_effort > 4)
    decoding_effort = 4;
  dt_bauhaus_slider_set(gui->decoding_effort, decoding_effort);
#endif

  return 0;
}



/*
-------- GUI --------
*/

const char *name()
{
  return _("JPEG XL");
}

static void lossless_changed(GtkWidget *lossless, dt_imageio_module_format_t *self)
{
  gboolean is_lossless = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(lossless));

  dt_conf_set_bool(CONF_PREFIX "lossless", is_lossless);
  dt_imageio_jxl_gui_data_t *gui = (dt_imageio_jxl_gui_data_t *)self->gui_data;
  if(is_lossless == TRUE)
  {
    gtk_widget_set_sensitive(gui->quality, FALSE);
  }
  else
  {
    gtk_widget_set_sensitive(gui->quality, TRUE);
  }
}

static void quality_changed(GtkWidget *quality, dt_imageio_module_format_t *self)
{
  dt_conf_set_int(CONF_PREFIX "quality", (int)dt_bauhaus_slider_get(quality));
}

static void encoding_effort_changed(GtkWidget *encoding_effort, dt_imageio_module_format_t *self)
{
  dt_conf_set_int(CONF_PREFIX "encoding_effort", (int)dt_bauhaus_slider_get(encoding_effort));
}

#ifdef SHOW_DECODING_EFFORT
static void decoding_effort_changed(GtkWidget *decoding_effort, dt_imageio_module_format_t *self)
{
  dt_conf_set_float(CONF_PREFIX "decoding_effort", dt_bauhaus_slider_get(decoding_effort));
}
#endif

void gui_init(dt_imageio_module_format_t *self)
{
  dt_imageio_jxl_gui_data_t *gui = malloc(sizeof(dt_imageio_jxl_gui_data_t));
  self->gui_data = gui;

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  self->widget = box;

  // lossless checkbox
  GtkWidget *lossless = gtk_check_button_new_with_label(_("lossless"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lossless), dt_conf_get_bool(CONF_PREFIX "lossless"));
  gtk_widget_set_tooltip_text(lossless, _("preserve all quality"));
  g_signal_connect(G_OBJECT(lossless), "clicked", G_CALLBACK(lossless_changed), self);
  gtk_box_pack_start(GTK_BOX(box), lossless, FALSE, TRUE, 0);
  gui->lossless = lossless;

  // lossy distance/quality slider
  GtkWidget *quality
      = dt_bauhaus_slider_new_with_range(NULL, dt_confgen_get_float(CONF_PREFIX "quality", DT_MIN),
                                         dt_confgen_get_float(CONF_PREFIX "quality", DT_MAX), 0.1,
                                         dt_confgen_get_float(CONF_PREFIX "quality", DT_DEFAULT), 1.0);
  dt_bauhaus_slider_set(quality, dt_conf_get_float(CONF_PREFIX "quality"));
  dt_bauhaus_widget_set_label(quality, NULL, _("quality"));
  gtk_widget_set_tooltip_text(
      quality,
      _("the quality of the output image\n0 = very lossy\n14 = visually lossless\n15 = mathematically lossless"));
  g_signal_connect(G_OBJECT(quality), "value-changed", G_CALLBACK(quality_changed), self);
  gtk_box_pack_start(GTK_BOX(box), quality, TRUE, TRUE, 0);
  // quality should only be available if losslessness is disabled
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(lossless)) == TRUE)
    gtk_widget_set_sensitive(quality, FALSE);
  else
    gtk_widget_set_sensitive(quality, TRUE);
  gui->quality = quality;

  // encode effort slider
  GtkWidget *encoding_effort
      = dt_bauhaus_slider_new_with_range(NULL, dt_confgen_get_int(CONF_PREFIX "encoding_effort", DT_MIN),
                                         dt_confgen_get_int(CONF_PREFIX "encoding_effort", DT_MAX), 1,
                                         dt_confgen_get_int(CONF_PREFIX "encoding_effort", DT_DEFAULT), 0);
  dt_bauhaus_slider_set(encoding_effort, dt_conf_get_int(CONF_PREFIX "encoding_effort"));
  dt_bauhaus_widget_set_label(encoding_effort, NULL, _("encoding effort"));
  gtk_widget_set_tooltip_text(encoding_effort, _("the effort used to encode the image, higher efforts will have "
                                                 "better results at the expense of longer encode times"));
  g_signal_connect(G_OBJECT(encoding_effort), "value-changed", G_CALLBACK(encoding_effort_changed), self);
  gtk_box_pack_start(GTK_BOX(box), encoding_effort, TRUE, TRUE, 0);
  gui->encoding_effort = encoding_effort;

#ifdef SHOW_DECODING_EFFORT
  // decode effort slider
  GtkWidget *decoding_effort
      = dt_bauhaus_slider_new_with_range(NULL, dt_confgen_get_int(CONF_PREFIX "decoding_effort", DT_MIN),
                                         dt_confgen_get_int(CONF_PREFIX "decoding_effort", DT_MAX), 1,
                                         dt_confgen_get_int(CONF_PREFIX "decoding_effort", DT_DEFAULT), 0);
  dt_bauhaus_slider_set(decoding_effort, dt_conf_get_float(CONF_PREFIX "decoding_effort"));
  dt_bauhaus_widget_set_label(decoding_effort, NULL, _("decoding effort"));
  gtk_widget_set_tooltip_text(decoding_effort, _("the effort required to decode the image"));
  g_signal_connect(G_OBJECT(decoding_effort), "value-changed", G_CALLBACK(decoding_effort_changed), self);
  gtk_box_pack_start(GTK_BOX(box), decoding_effort, TRUE, TRUE, 0);
  gui->decoding_effort = decoding_effort;
#endif
}

void gui_cleanup(dt_imageio_module_format_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

void gui_reset(dt_imageio_module_format_t *self)
{
  dt_imageio_jxl_gui_data_t *gui = (dt_imageio_jxl_gui_data_t *)self->gui_data;

  gboolean lossless = dt_confgen_get_bool(CONF_PREFIX "lossless", DT_DEFAULT);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gui->lossless), lossless);

  float quality = dt_confgen_get_float(CONF_PREFIX "quality", DT_DEFAULT);
  dt_bauhaus_slider_set(gui->quality, quality);

  int encoding_effort = dt_confgen_get_int(CONF_PREFIX "encoding_effort", DT_DEFAULT);
  dt_bauhaus_slider_set(gui->encoding_effort, encoding_effort);

#ifdef SHOW_DECODING_EFFORT
  int decoding_effort = dt_confgen_get_int(CONF_PREFIX "decoding_effort", DT_DEFAULT);
  dt_bauhaus_slider_set(gui->decoding_effort, decoding_effort);
#endif
}



#undef SHOW_DECODING_EFFORT
#undef CONF_PREFIX
