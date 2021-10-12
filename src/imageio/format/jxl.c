/*
    This file is part of darktable,
    Copyright (C) 2021 darktable developers.
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
#include "common/colorspaces.c"
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



/*
-------- MODULE INFO --------
*/


typedef struct dt_imageio_jxl_params_t
{
  dt_imageio_module_data_t global;
  gboolean lossless;
  float quality;
  int encoding_effort;
  int decoding_effort;
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
  // Int (0-4): effort required to decode output. 0 = fast, 4 = slow.
  GtkWidget *decoding_effort;
} dt_imageio_jxl_gui_data_t;



void init(dt_imageio_module_format_t *self)
{
#ifdef USE_LUA
  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_jxl_params_t, lossless, gboolean);

  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_jxl_params_t, quality, float);

  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_jxl_params_t, encoding_effort, int);

  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_jxl_params_t, decoding_effort, int);
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
  // The maximum dimensions supported by jxl images
  *width = 1073741823;
  *height = 1073741823;
  return 1;
}

int bpp(dt_imageio_module_data_t *data)
{
  // 32 bytes per pixel
  return 32;
}



/*
-------- IMAGE WRITING --------
*/

// Note: We include our own code to write the jxl codestream within a BMFF container.
// JXL images can either be naked (just the codestream) or wrapped in a BMFF container.
// libjxl can write the codestream in a container for you, but it doesn't have options for
// including other "boxes" such as exif data, so we will manage the BMFF container ourselves.

int write_32(FILE *stream, const uint32_t num)
{
  uint32_t be;

  // Need to ensure that the number is in big endian format
  uint8_t *buf = (uint8_t *)&be;
  buf[0] = (num >> 24) & 0xFF;
  buf[1] = (num >> 16) & 0xFF;
  buf[2] = (num >> 8) & 0xFF;
  buf[3] = (num >> 0) & 0xFF;

  if(fwrite(buf, 4, 1, stream) != 1) return 1; // failure writing to file
  return 0;
}

int write_64(FILE *stream, const uint64_t num)
{
  uint64_t be;

  // Need to ensure that the number is in big endian format
  uint8_t *buf = (uint8_t *)&be;
  buf[0] = (num >> 56) & 0xFF;
  buf[1] = (num >> 48) & 0xFF;
  buf[2] = (num >> 40) & 0xFF;
  buf[3] = (num >> 32) & 0xFF;
  buf[4] = (num >> 24) & 0xFF;
  buf[5] = (num >> 16) & 0xFF;
  buf[6] = (num >> 8) & 0xFF;
  buf[7] = (num >> 0) & 0xFF;

  if(fwrite(buf, 8, 1, stream) != 1) return 1; // failure writing to file
  return 0;
}

int write_box(FILE *stream, const uint8_t *type, const uint8_t *data, const uint64_t data_len)
{
  uint64_t box_len = 0;
  box_len += 4; // The size of the "size" field - 4 bytes
  box_len += 4; // The size of the "type" field - 4 bytes
  box_len += data_len;

  // can we fit the data length into a 32 bit unsigned integer?
  const gboolean large = box_len > 0xFFFFFFFF;
  if(large) box_len += 8; // The size of the "largesize" field - 8 bytes

  if(write_32(stream, large ? 1 : box_len)) return 1;

  fwrite(type, 1, 4, stream);

  if(large)
  {
    if(write_64(stream, box_len)) return 1;
  }

  // If data is null then the caller wants to write the data itself
  if(data)
  {
    if(fwrite(data, data_len, 1, stream) != 1) return 1; // failure writing to file
  }
  return 0;
}

// Write a BMFF header for a JXL file
int write_container_header(FILE *stream)
{
  const uint8_t *signature_type = (uint8_t *)"JXL ";
  const uint8_t signature[4] = { 0xd, 0xa, 0x87, 0xa };
  if(write_box(stream, signature_type, (uint8_t *)&signature, 4)) return 1;

  const uint8_t *ftyp_type = (uint8_t *)"ftyp";
  // first 4 bytes: major brand (jxl )
  // second 4 bytes: minor version (0)
  // third 4 bytes: compatible brands (jxl )
  const uint8_t *ftyp = (uint8_t *)"jxl \0\0\0\0jxl ";
  if(write_box(stream, ftyp_type, ftyp, 12)) return 1;
  return 0;
}

int write_container_codestream(FILE *stream, const uint8_t *codestream, const uint64_t codestream_len)
{
  const uint8_t *codestream_type = (uint8_t *)"jxlc";
  if(write_box(stream, codestream_type, codestream, codestream_len)) return 1;
  return 0;
}

int write_container_exif(FILE *stream, const uint8_t *exif, const uint64_t exif_len)
{
  const uint8_t *exif_type = (uint8_t *)"Exif";
  const uint64_t data_len = 4 + exif_len; // add space for offset
  if(write_box(stream, exif_type, NULL, data_len)) return 1;

  write_32(stream, 0); // offset
  if(fwrite(exif, exif_len, 1, stream) != 1) return 1;

  return 0;
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
    const JxlEncoderStatus res = code;                                                                            \
    if(res != JXL_ENC_SUCCESS)                                                                                    \
    {                                                                                                             \
      fprintf(stderr, "JXL call failed with err %d (src/imageio/format/jxl.c#L%d)\n", res, __LINE__);             \
      goto end;                                                                                                   \
    }                                                                                                             \
  }

  const dt_imageio_jxl_params_t *params = (dt_imageio_jxl_params_t *)data;
  const int width = params->global.width;
  const int height = params->global.height;

  JxlEncoder *encoder = JxlEncoderCreate(NULL);

  // JxlThreadParallelRunnerDefaultNumWorkerThreads()
  const uint32_t num_threads = JxlResizableParallelRunnerSuggestThreads(width, height);
  void *runner = JxlThreadParallelRunnerCreate(NULL, num_threads);
  JXL_ASSERT(JxlEncoderSetParallelRunner(encoder, JxlThreadParallelRunner, runner));

  const dt_colorspaces_color_profile_t *output_profile
      = dt_colorspaces_get_output_profile(imgid, over_type, over_filename);
  const cmsHPROFILE out_profile = output_profile->profile;
  // Previous call will give us a more accurate color profile type
  // (not what the user requested in the export menu but what the image is actually using)
  over_type = output_profile->type;

  // If possible we want libjxl to save the color encoding in it's own format, rather
  // than as an ICC binary blob which is possible. ICC blobs are slightly larger and
  // are also less compatible with various image viewers.
  // If we are unable to find the required color encoding data for libjxl we will
  // just fallback to providing an ICC blob (and hope we can at least do that!).
  gboolean write_icc = true;
  JxlColorEncoding color_encoding;

  if(cmsGetColorSpace(out_profile) == cmsSigRgbData)
  {
    color_encoding.color_space = JXL_COLOR_SPACE_RGB;

    const cmsCIEXYZ *wtpt = cmsReadTag(out_profile, cmsSigMediaWhitePointTag);
    if(wtpt)
    {
      color_encoding.white_point = JXL_WHITE_POINT_CUSTOM;
      color_encoding.white_point_xy[0] = wtpt->X;
      color_encoding.white_point_xy[1] = wtpt->Y;

      cmsCIExyYTRIPLE primaries;
      write_icc = false;
      if(over_type == DT_COLORSPACE_SRGB)
      {
        primaries = sRGB_Primaries;
        color_encoding.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
      }
      else if(over_type == DT_COLORSPACE_BRG)
      {
        primaries = sRGB_Primaries;
        color_encoding.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
      }
      else if(over_type == DT_COLORSPACE_INFRARED)
      {
        primaries = sRGB_Primaries;
        color_encoding.transfer_function = JXL_TRANSFER_FUNCTION_LINEAR;
      }
      else if(over_type == DT_COLORSPACE_DISPLAY)
      {
        primaries = sRGB_Primaries;
        color_encoding.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
      }
      else if(over_type == DT_COLORSPACE_DISPLAY2)
      {
        primaries = sRGB_Primaries;
        color_encoding.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
      }
      else if(over_type == DT_COLORSPACE_LIN_REC2020)
      {
        primaries = Rec2020_Primaries;
        color_encoding.transfer_function = JXL_TRANSFER_FUNCTION_LINEAR;
      }
      else if(over_type == DT_COLORSPACE_PQ_REC2020)
      {
        primaries = Rec2020_Primaries;
        color_encoding.transfer_function = JXL_TRANSFER_FUNCTION_PQ;
      }
      else if(over_type == DT_COLORSPACE_HLG_REC2020)
      {
        primaries = Rec2020_Primaries;
        color_encoding.transfer_function = JXL_TRANSFER_FUNCTION_HLG;
      }
      else if(over_type == DT_COLORSPACE_REC709)
      {
        primaries = Rec709_Primaries;
        color_encoding.transfer_function = JXL_TRANSFER_FUNCTION_709;
      }
      else if(over_type == DT_COLORSPACE_LIN_REC709)
      {
        primaries = Rec709_Primaries;
        color_encoding.transfer_function = JXL_TRANSFER_FUNCTION_LINEAR;
      }
      else if(over_type == DT_COLORSPACE_ADOBERGB)
      {
        primaries = Adobe_Primaries;
        color_encoding.transfer_function = JXL_TRANSFER_FUNCTION_GAMMA;
        color_encoding.gamma = 2.19921875;
      }
      else if(over_type == DT_COLORSPACE_PQ_P3)
      {
        primaries = P3_Primaries;
        color_encoding.transfer_function = JXL_TRANSFER_FUNCTION_PQ;
      }
      else if(over_type == DT_COLORSPACE_HLG_P3)
      {
        primaries = P3_Primaries;
        color_encoding.transfer_function = JXL_TRANSFER_FUNCTION_HLG;
      }
      else if(over_type == DT_COLORSPACE_PROPHOTO_RGB)
      {
        primaries = ProPhoto_Primaries;
        color_encoding.transfer_function = JXL_TRANSFER_FUNCTION_LINEAR;
      }
      else
      {
        write_icc = true;
      }

      if(!write_icc)
      {
        color_encoding.primaries = JXL_PRIMARIES_CUSTOM;
        color_encoding.primaries_red_xy[0] = primaries.Red.x;
        color_encoding.primaries_red_xy[1] = primaries.Red.y;
        color_encoding.primaries_green_xy[0] = primaries.Green.x;
        color_encoding.primaries_green_xy[1] = primaries.Green.y;
        color_encoding.primaries_blue_xy[0] = primaries.Blue.x;
        color_encoding.primaries_blue_xy[1] = primaries.Blue.y;
        // TODO: safe to simply put pipe->icc_intent here?
        color_encoding.rendering_intent = JXL_RENDERING_INTENT_PERCEPTUAL;

        JxlEncoderSetColorEncoding(encoder, &color_encoding);
      }
    }
  }

  if(write_icc)
  {
    fprintf(stderr, "JXL: Could not generate color encoding data, falling back to ICC binary\n");

    cmsUInt32Number size = 0;
    if(cmsSaveProfileToMem(out_profile, NULL, &size))
    {
      unsigned char *buf = malloc(size);
      if(!buf || !cmsSaveProfileToMem(out_profile, buf, &size)) goto end;

      JXL_ASSERT(JxlEncoderSetICCProfile(encoder, buf, size));

      free(buf);
    }
    else
    {
      fprintf(stderr, "JXL: Error writing ICC data\n");
      goto end;
    }
  }

  JxlBasicInfo basic_info;
  JxlEncoderInitBasicInfo(&basic_info);
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
  // HOWEVER - We do not want libjxl to write the image in a container, we will
  // do that ourselves. This gives us the control to be able to add our own
  // sections (e.g. exif data).
  JXL_ASSERT(JxlEncoderUseContainer(encoder, JXL_FALSE));

  // automatically freed when we destroy the encoder
  JxlEncoderOptions *options = JxlEncoderOptionsCreate(encoder, NULL);
  JXL_ASSERT(JxlEncoderOptionsSetLossless(options, params->lossless ? JXL_TRUE : JXL_FALSE));

  int decode_effort = params->decoding_effort;
  // We store decoding effort but jxl wants encoding speed, we must reverse it
  decode_effort = (decode_effort - 4) * -1;
  JXL_ASSERT(JxlEncoderOptionsSetDecodingSpeed(options, decode_effort));

  const int encode_effort = params->encoding_effort;
  JXL_ASSERT(JxlEncoderOptionsSetEffort(options, encode_effort));

  int quality = params->quality;
  // We store quality but jxl wants distance, so we reverse the scale
  quality = (quality - 15) * -1;
  JXL_ASSERT(JxlEncoderOptionsSetDistance(options, quality));

  const int channels = 3;
  JxlPixelFormat pixel_format = { channels, JXL_TYPE_FLOAT, JXL_NATIVE_ENDIAN, 1 };

  // Fix pixel stride
  const size_t in_buf_size = width * height * channels * sizeof(float);
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


  // Write the image codestream to a buffer. We write the image in chunks of 50,000 bytes.
  // fixme: Can we better estimate what the optimal size of chunks is for this image?
  const size_t chunkSize = 50000;
  out_buf = malloc(chunkSize);
  uint8_t *out_cur = out_buf;
  size_t out_len = chunkSize;
  if(!out_buf)
  {
    fprintf(stderr, "JXL failure: out of memory\n");
    goto end;
  }

  JxlEncoderStatus out_status = JXL_ENC_NEED_MORE_OUTPUT;

  while(out_status == JXL_ENC_NEED_MORE_OUTPUT)
  {
    size_t out_avail = out_buf + out_len - out_cur;

    out_status = JxlEncoderProcessOutput(encoder, &out_cur, &out_avail);

    // out_cur is incremented by the amount written and out_avail is decremented by the same amount

    if(out_status == JXL_ENC_NEED_MORE_OUTPUT)
    {
      // Make sure there is still enough available space
      if(out_avail < chunkSize / 2)
      {
        const size_t offset = out_cur - out_buf;
        out_len += chunkSize - out_avail;
        out_buf = realloc(out_buf, out_len);
        if(!out_buf)
        {
          fprintf(stderr, "JXL failure: out of memory\n");
          goto end;
        }
        out_cur = out_buf + offset;
      }
    }
    else if(out_status != JXL_ENC_SUCCESS)
    {
      // out_status should be an error: force it to be processed
      JXL_ASSERT(out_status);
      // just to let the compiler know that we will goto end (even though it
      // should happen anyway in the assertion)
      goto end;
    }
  }

  out_file = g_fopen(filename, "wb");
  if(!out_file) goto end;

  // Now we need to write the bmff container, starting with the header
  if(write_container_header(out_file)) goto end;

  // Write the image codestream (the output from libjxl)
  if(write_container_codestream(out_file, out_buf, out_len)) goto end;

  // Write the exif data if it exists
  if(exif)
  {
    if(write_container_exif(out_file, exif, exif_len)) goto end;
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
  dt_imageio_jxl_params_t *params = calloc(1, sizeof(dt_imageio_jxl_params_t));

  if(params == NULL)
  {
    return NULL;
  }

  params->lossless = dt_conf_get_bool(CONF_PREFIX "lossless");

  params->quality = dt_conf_get_float(CONF_PREFIX "quality");

  params->encoding_effort = dt_conf_get_int(CONF_PREFIX "encoding_effort");

  params->decoding_effort = dt_conf_get_int(CONF_PREFIX "decoding_effort");

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

  int decoding_effort = params->decoding_effort;
  if(decoding_effort < 0)
    decoding_effort = 0;
  else if(decoding_effort > 4)
    decoding_effort = 4;
  dt_bauhaus_slider_set(gui->decoding_effort, decoding_effort);

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
  const gboolean is_lossless = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(lossless));

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

static void decoding_effort_changed(GtkWidget *decoding_effort, dt_imageio_module_format_t *self)
{
  dt_conf_set_float(CONF_PREFIX "decoding_effort", dt_bauhaus_slider_get(decoding_effort));
}

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
}

void gui_cleanup(dt_imageio_module_format_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

void gui_reset(dt_imageio_module_format_t *self)
{
  dt_imageio_jxl_gui_data_t *gui = (dt_imageio_jxl_gui_data_t *)self->gui_data;

  const gboolean lossless = dt_confgen_get_bool(CONF_PREFIX "lossless", DT_DEFAULT);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gui->lossless), lossless);

  const float quality = dt_confgen_get_float(CONF_PREFIX "quality", DT_DEFAULT);
  dt_bauhaus_slider_set(gui->quality, quality);

  const int encoding_effort = dt_confgen_get_int(CONF_PREFIX "encoding_effort", DT_DEFAULT);
  dt_bauhaus_slider_set(gui->encoding_effort, encoding_effort);

  const int decoding_effort = dt_confgen_get_int(CONF_PREFIX "decoding_effort", DT_DEFAULT);
  dt_bauhaus_slider_set(gui->decoding_effort, decoding_effort);
}



#undef CONF_PREFIX
